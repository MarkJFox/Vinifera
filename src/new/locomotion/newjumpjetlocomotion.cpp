/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Jumpjet locomotion re-implementation.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "newjumpjetlocomotion.h"

#include "aircrafttracker.h"
#include "building.h"
#include "buildingtype.h"
#include "cell.h"
#include "coord.h"
#include "debughandler.h"
#include "extension.h"
#include "foot.h"
#include "footext.h"
#include "house.h"
#include "ionstorm.h"
#include "mouse.h"
#include "rules.h"
#include "technotypeext.h"
#include "unit.h"
#include "vinifera_globals.h"
#include "vinifera_saveload.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace
{
    bool Balloon_Flight_Coord(const Coord& coord)
    {
        return coord.X >= 0 && coord.Y >= 0 &&
            coord.X / CELL_LEPTON <= std::numeric_limits<short>::max() &&
            coord.Y / CELL_LEPTON <= std::numeric_limits<short>::max() &&
            Map.In_Radar(coord.As_Cell());
    }

    std::vector<Coord> Balloon_Factory_Flight_Points(FootClass* product)
    {
        std::vector<Coord> occupied;
        occupied.push_back(product->PositionCoord);
        for (int i = 0; i < Units.Count(); ++i) {
            const auto other = Units[i];
            if (!other || other == product || !other->IsActive || other->IsInLimbo || !other->IsDown || other->IsOnCarryall ||
                other->Strength <= 0 || other->IsToExplode) continue;
            const auto loco = NewJumpjetLocomotionClass::Balloon_Hover_Locomotor(other);
            if (!loco) continue;

            if (Balloon_Flight_Coord(other->PositionCoord)) occupied.push_back(other->PositionCoord);
            const Coord destination = loco->Destination();
            if (destination != COORD_NONE && Balloon_Flight_Coord(destination)) occupied.push_back(destination);
            // Pending factory rally cells reserve their airspace before the clearing leg ends.
            for (int q = 0; q < other->NavQueue.Count(); ++q) {
                const auto target = other->NavQueue[q];
                if (target && target->RTTI == RTTI_CELL) {
                    const Coord point = target->Center_Coord();
                    if (Balloon_Flight_Coord(point)) occupied.push_back(point);
                }
            }
        }
        return occupied;
    }

    Coord Balloon_Factory_Flight_Point(const Coord& origin, const std::vector<Coord>& occupied, int radius)
    {
        if (!Balloon_Flight_Coord(origin)) return COORD_NONE;
        const Cell center = origin.As_Cell();
        // Expanding square rings, then Y/X order: no RNG or ground landing search.
        for (int ring = 0; ring <= radius; ++ring) {
            for (int dy = -ring; dy <= ring; ++dy) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (std::abs(dx) != ring && std::abs(dy) != ring) continue;
                    const int x = center.X + dx;
                    const int y = center.Y + dy;
                    if (x < 0 || y < 0 || x > std::numeric_limits<short>::max() ||
                        y > std::numeric_limits<short>::max()) continue;
                    const Cell cell(x, y);
                    if (!Map.In_Radar(cell) || Map[cell].Cell_Building()) continue;
                    const Coord point = cell.As_Coord();
                    const bool blocked = std::any_of(occupied.begin(), occupied.end(), [&](const Coord& other) {
                        return std::abs(point.X - other.X) < CELL_LEPTON &&
                            std::abs(point.Y - other.Y) < CELL_LEPTON;
                    });
                    if (!blocked) return point;
                }
            }
        }
        return COORD_NONE;
    }
}


bool NewJumpjetLocomotionClass::Is_Balloon_Hover() const
{
    return LinkedTo && LinkedTo->RTTI == RTTI_UNIT &&
        static_cast<ILocomotion*>(LinkedTo->Locomotion) == static_cast<const ILocomotion*>(this) &&
        Extension::Fetch(LinkedTo->TClass)->Supports_Balloon_Hover();
}


NewJumpjetLocomotionClass* NewJumpjetLocomotionClass::Balloon_Hover_Locomotor(FootClass* foot)
{
    if (!foot || foot->RTTI != RTTI_UNIT || !foot->Locomotion ||
        !Extension::Fetch(foot->TClass)->Supports_Balloon_Hover()) return nullptr;
    IPersistStream* persist = nullptr;
    if (FAILED(foot->Locomotion->QueryInterface(__uuidof(IPersistStream),
        reinterpret_cast<void**>(&persist))) || !persist) return nullptr;
    CLSID id;
    const HRESULT result = persist->GetClassID(&id);
    persist->Release();
    if (FAILED(result) || id != __uuidof(NewJumpjetLocomotionClass)) return nullptr;
    return static_cast<NewJumpjetLocomotionClass*>(static_cast<ILocomotion*>(foot->Locomotion));
}


BuildingClass* NewJumpjetLocomotionClass::Balloon_Factory() const
{
    const auto contact = LinkedTo->Contact_With_Whom();
    if (contact && contact->RTTI == RTTI_BUILDING) {
        const auto factory = static_cast<BuildingClass*>(contact);
        if (factory->Class->IsWeaponsFactory && factory->Contact_With_Whom() == LinkedTo) return factory;
    }
    return nullptr;
}


void NewJumpjetLocomotionClass::Complete_Balloon_Move()
{
    CurrentSpeed = 0;
    TargetSpeed = 0;
    IsMoving = false;
    HeadToCoord = COORD_NONE;
    CurrentState = HOVERING;
    FlightLevel = JumpjetCruiseHeight;
    // The ordinary Move mission observes NavCom=null and completes on its next AI.
    LinkedTo->Assign_Destination(nullptr);
    const Cell oldcell = Extension::Fetch(LinkedTo)->Get_Last_Flight_Cell();
    const Cell newcell = LinkedTo->Get_Cell();
    if (oldcell != CELL_NONE && oldcell != newcell) AircraftTracker->Update_Position(LinkedTo, oldcell, newcell);
    if (Vinifera_DeveloperMode) DEBUG_INFO("[BALLOON] type={} destination reached; hovering\n", LinkedTo->TClass->Name());
}


void NewJumpjetLocomotionClass::Release_Balloon_Factory(BuildingClass* factory)
{
    const int floor = Map.Get_Height_GL(LinkedTo->PositionCoord);
    const int clearance = std::max(floor + JumpjetCruiseHeight,
        Map[LinkedTo->PositionCoord].Occupier_Height() + LEVEL_LEPTON_H);
    if (LinkedTo->PositionCoord.Z < clearance) return;
    if (LinkedTo->IsTethered) {
        // Keep radio contact through CLOSE so the existing renderer retains the roof image.
        LinkedTo->Transmit_Message(RADIO_UNTETHER, factory);
        if (Vinifera_DeveloperMode) DEBUG_INFO("[BALLOON] type={} roof clear; native factory closing\n", LinkedTo->TClass->Name());
        return;
    }
    if (!factory->Door.Is_Door_Closed()) return;

    // Unit PCP_END also breaks ice and validates ground landing; use the radio protocol directly.
    const auto response = LinkedTo->Transmit_Message(RADIO_UNLOADED, factory);
    if (LinkedTo->IsTethered || LinkedTo->Radio == factory) return;

    CurrentSpeed = TargetSpeed = 0;
    IsMoving = false;
    HeadToCoord = COORD_NONE;
    CurrentState = HOVERING;
    FlightLevel = JumpjetCruiseHeight;
    LinkedTo->Assign_Destination(nullptr);

    auto occupied = Balloon_Factory_Flight_Points(LinkedTo);
    const Coord clearing = Balloon_Factory_Flight_Point(LinkedTo->PositionCoord, occupied, 8);
    if (clearing != COORD_NONE) {
        AbstractClass* rally = nullptr;
        if (LinkedTo->NavQueue.Count() == 0 && response == RADIO_RUN_AWAY) {
            rally = LinkedTo->ArchiveTarget;
            if (rally && rally->RTTI == RTTI_CELL) {
                occupied.push_back(clearing);
                const Coord point = Balloon_Factory_Flight_Point(rally->Center_Coord(), occupied, 4);
                if (point != COORD_NONE) rally = &Map[point];
                else if (Vinifera_DeveloperMode) DEBUG_WARNING("[BALLOON] type={} no free rally point; keeping ArchiveTarget\n", LinkedTo->TClass->Name());
            }
        }
        // The existing Move/Enter_Idle_Mode path consumes this queue after clearing.
        if (!rally || LinkedTo->NavQueue.Add(rally)) {
            if (rally) LinkedTo->IsNavQueueLoop = false;
            LinkedTo->Assign_Destination(&Map[clearing]);
            LinkedTo->Assign_Mission(MISSION_MOVE);
            if (Vinifera_DeveloperMode) DEBUG_INFO("[BALLOON] type={} factory released; clearing to ({},{}) before {} queued targets\n",
                LinkedTo->TClass->Name(), clearing.X, clearing.Y, LinkedTo->NavQueue.Count());
            return;
        }
    }

    if (Vinifera_DeveloperMode) DEBUG_WARNING("[BALLOON] type={} no factory clearing route; keeping existing navigation\n", LinkedTo->TClass->Name());
    LinkedTo->Handle_Navigation_List();
    if (!LinkedTo->NavCom && response == RADIO_RUN_AWAY && LinkedTo->ArchiveTarget) {
        LinkedTo->Assign_Destination(LinkedTo->ArchiveTarget);
    }
    if (Vinifera_DeveloperMode) DEBUG_INFO("[BALLOON] type={} factory released after vertical ascent\n", LinkedTo->TClass->Name());
}


void NewJumpjetLocomotionClass::Finish_Balloon_Factory_Exit(BuildingClass* factory)
{
    if (factory && Is_Balloon_Hover() && CurrentState == ASCENDING && Balloon_Factory() == factory &&
        LinkedTo->IsActive && !LinkedTo->IsInLimbo && LinkedTo->Strength > 0 &&
        !LinkedTo->IsTethered && !factory->IsTethered && Balloon_Flight_Coord(LinkedTo->PositionCoord)) {
        Release_Balloon_Factory(factory);
    }
}


NewJumpjetLocomotionClass::NewJumpjetLocomotionClass() :
    BASECLASS(),
    JumpjetTurnRate(Rule->JumpjetTurnRate),
    JumpjetSpeed(Rule->JumpjetSpeed),
    JumpjetClimb(Rule->JumpjetClimb),
    JumpjetCruiseHeight(Rule->JumpjetCruiseHeight),
    JumpjetAcceleration(Rule->JumpjetAcceleration),
    JumpjetWobblesPerSecond(Rule->JumpjetWobblesPerSecond),
    JumpjetWobbleDeviation(Rule->JumpjetWobbleDeviation),
    JumpjetNoWobbles(false),
    JumpjetCloakDetectionRadius(Rule->JumpjetCloakDetectionRadius),
    HeadToCoord(COORD_NONE),
    IsMoving(false),
    CurrentState(GROUNDED),
    Facing(Rule->JumpjetTurnRate),
    CurrentSpeed(0.0),
    TargetSpeed(0.0),
    FlightLevel(0),
    CurrentWobble(0.0),
    IsLanding(false)
{

}


IFACEMETHODIMP NewJumpjetLocomotionClass::Link_To_Object(void* object)
{
    TechnoTypeClassExtension const* type_ext = Extension::Fetch(static_cast<TechnoClass*>(object)->TClass);
    JumpjetTurnRate = type_ext->JumpjetTurnRate;
    JumpjetSpeed = type_ext->JumpjetSpeed;
    JumpjetClimb = type_ext->JumpjetClimb;
    int height = type_ext->JumpjetCruiseHeight;
    height = std::max(height, 2 * LEVEL_LEPTON_H);
    JumpjetCruiseHeight = height;
    JumpjetAcceleration = type_ext->JumpjetAcceleration;
    JumpjetWobblesPerSecond = type_ext->JumpjetWobblesPerSecond;
    JumpjetWobbleDeviation = type_ext->JumpjetWobbleDeviation;
    Facing = JumpjetTurnRate;
    Facing.Set_Desired(DirType(0x4000));
    Facing.Set(DirType(0x4000));
    JumpjetCloakDetectionRadius = type_ext->JumpjetCloakDetectionRadius;
    JumpjetNoWobbles = type_ext->JumpjetNoWobbles;
    return BASECLASS::Link_To_Object(object);
}


IFACEMETHODIMP_(bool) NewJumpjetLocomotionClass::Is_Moving()
{
    return IsMoving;
}


IFACEMETHODIMP_(Coord) NewJumpjetLocomotionClass::Destination()
{
    if (Is_Moving()) {
        return HeadToCoord;
    } else {
        return COORD_NONE;
    }
}


IFACEMETHODIMP_(bool) NewJumpjetLocomotionClass::Process()
{
    const bool balloon = Is_Balloon_Hover();
    if (balloon) {
        if (Vinifera_PerformingLoad || !LinkedTo->IsActive || LinkedTo->IsInLimbo || !LinkedTo->IsDown ||
            LinkedTo->Strength <= 0 || LinkedTo->IsToExplode || LinkedTo->IsFalling || LinkedTo->IsSinking ||
            LinkedTo->IsOnCarryall || LinkedTo->Is_Immobilized() || LinkedTo->EMPFramesRemaining > 0 ||
            !IsPowered || !Balloon_Flight_Coord(LinkedTo->PositionCoord)) return false;

        const auto factory = Balloon_Factory();
        if (factory) {
            // Native WeaponsFactory OPEN dispatch must finish before we leave the floor.
            const bool closing = CurrentState == ASCENDING && !LinkedTo->IsTethered && !factory->IsTethered;
            if (!closing && (factory->Get_Mission() != MISSION_UNLOAD || factory->Status != 3 ||
                !factory->Door.Is_Door_Open())) return false;
        } else if (LinkedTo->IsTethered) {
            return false;
        }
        if (CurrentState == ASCENDING) CurrentSpeed = TargetSpeed = 0;
    }

    LayerType layer = In_Which_Layer();

    if (balloon && CurrentState == GROUNDED) {
        if (IonStorm_Is_Active()) return false;
        Process_Grounded();
    }

    if (balloon || Is_Moving() || Is_Moving_Now()) {
        Movement_AI();
        if (!LinkedTo->IsActive || (balloon && (LinkedTo->IsInLimbo || LinkedTo->Strength <= 0 ||
            LinkedTo->IsToExplode || LinkedTo->IsFalling || LinkedTo->IsSinking))) {
            return false;
        }

        if (IonStorm_Is_Active()) {
            if (CurrentState != GROUNDED) {
                ResultType result = LinkedTo->Take_Damage(LinkedTo->Strength, 0, Rule->C4Warhead, nullptr, true, true);
                if (result == RESULT_DESTROYED) {
                    return false;
                }
            }
        }

        switch (CurrentState) {
            case GROUNDED:
                Process_Grounded();
                break;

            case ASCENDING:
                Process_Ascent();
                break;

            case HOVERING:
                Process_Hover();
                break;

            case CRUISING:
                Process_Cruise();
                break;

            case DESCENDING:
                Process_Descent();
                break;
        }

        if (LinkedTo->IsSelected && LinkedTo->House != PlayerPtr) {
            if (Map.Is_Shrouded(LinkedTo->PositionCoord) || (Scen->Special.IsFogOfWar && Map.Is_Fogged(LinkedTo->PositionCoord))) {
                LinkedTo->Unselect();
            }
        }

        if (!LinkedTo->IsDiscoveredByPlayer && LinkedTo->House != PlayerPtr) {
            if (!Map.Is_Shrouded(LinkedTo->PositionCoord)) {
                LinkedTo->Revealed(PlayerPtr);
            }
        }
    }

    if (In_Which_Layer() != layer) {
        Map.Submit(LinkedTo);
    }

    return false;
}


IFACEMETHODIMP_(void) NewJumpjetLocomotionClass::Move_To(Coord to)
{
    if (Is_Balloon_Hover()) {
        if (IsLanding && HeadToCoord != COORD_NONE) LinkedTo->Clear_Occupy_Bit(HeadToCoord);
        IsLanding = false;
        if (to == COORD_NONE || !Balloon_Flight_Coord(to)) {
            Stop_Moving();
            LinkedTo->NavCom = nullptr;
            return;
        }
        // A flight endpoint need not contain a free ground landing spot.
        HeadToCoord = to;
        IsMoving = true;
        FlightLevel = JumpjetCruiseHeight;
        if (CurrentState == DESCENDING) CurrentState = ASCENDING;
        return;
    }

    if (HeadToCoord != COORD_NONE && CurrentState != GROUNDED && IsLanding) {
        LinkedTo->Clear_Occupy_Bit(HeadToCoord);
        IsLanding = false;
    }

    HeadToCoord = to;

    if (to != COORD_NONE) {
        Cell cell = Map.Nearby_Location(to.As_Cell(), LinkedTo->TClass->Speed, -1, MZONE_FLYER, Map[to].IsUnderBridge);
        Coord free = Closest_Free_Spot(cell.As_Coord());
        if (free != COORD_NONE) {
            HeadToCoord = free;
            LinkedTo->NavCom = &Map[HeadToCoord];
            IsMoving = true;
            if (CurrentState == DESCENDING) {
                CurrentState = ASCENDING;
                FlightLevel = JumpjetCruiseHeight;
            }
        }
    } else {
        IsMoving = false;
    }
}


IFACEMETHODIMP_(void) NewJumpjetLocomotionClass::Stop_Moving()
{
    if (Is_Balloon_Hover()) {
        const bool was_moving = IsMoving || CurrentSpeed > 0;
        if (IsLanding && HeadToCoord != COORD_NONE) LinkedTo->Clear_Occupy_Bit(HeadToCoord);
        IsLanding = false;
        IsMoving = false;
        LinkedTo->NavCom = nullptr;
        LinkedTo->Path[0] = FACING_NONE;
        HeadToCoord = COORD_NONE;
        TargetSpeed = 0;
        FlightLevel = JumpjetCruiseHeight;
        if (CurrentState != GROUNDED && CurrentState != ASCENDING) CurrentState = HOVERING;
        if (was_moving && Vinifera_DeveloperMode) DEBUG_INFO("[BALLOON] type={} stop without landing\n", LinkedTo->TClass->Name());
        return;
    }

    if (IsMoving) {
        if (HeadToCoord != COORD_NONE && CurrentState != GROUNDED && IsLanding) {
            LinkedTo->Clear_Occupy_Bit(HeadToCoord);
            IsLanding = false;
        }
        Cell cell = LinkedTo->PositionCoord.As_Cell();
        Cell nearby = Map.Nearby_Location(cell, SPEED_TRACK);
        if (nearby != CELL_NONE) {
            Coord nearby_coord = nearby.As_Coord();
            nearby_coord.Z = Map.Get_Height_GL(nearby_coord);
            if (Map[nearby].IsUnderBridge) {
                nearby_coord.Z += BRIDGE_LEPTON_HEIGHT;
            }
            Move_To(nearby_coord);
        } else {
            LinkedTo->Take_Damage(LinkedTo->Strength, 0, Rule->C4Warhead, nullptr, true, true);
            HeadToCoord = COORD_NONE;
        }
    }
}


IFACEMETHODIMP_(void) NewJumpjetLocomotionClass::Do_Turn(DirType coord)
{
    if (Is_Balloon_Hover()) {
        Facing.Set_Desired(coord);
        LinkedTo->PrimaryFacing.Set_Desired(coord);
        return;
    }
    LinkedTo->PrimaryFacing.Set(coord);
}


IFACEMETHODIMP_(void) NewJumpjetLocomotionClass::Unlimbo()
{
    BASECLASS::Unlimbo();
    if (Is_Balloon_Hover() && !Vinifera_PerformingLoad) {
        // A real placement starts a new ascent; Load restores the saved state instead.
        CurrentState = GROUNDED;
        CurrentSpeed = TargetSpeed = 0;
        FlightLevel = 0;
        CurrentWobble = 0;
        IsLanding = false;
        // TSpp names the native virtual Destination_Coord slot entry_50.
        HeadToCoord = LinkedTo->NavCom ? LinkedTo->NavCom->entry_50() : COORD_NONE;
        IsMoving = HeadToCoord != COORD_NONE && Balloon_Flight_Coord(HeadToCoord);
        if (!IsMoving) {
            HeadToCoord = COORD_NONE;
            LinkedTo->NavCom = nullptr;
        }
    }
}


IFACEMETHODIMP_(HRESULT) NewJumpjetLocomotionClass::GetClassID(CLSID * pClassID)
{
    if (pClassID == nullptr) {
        return E_POINTER;
    }

    *pClassID = __uuidof(this);

    return S_OK;
}


IFACEMETHODIMP_(HRESULT) NewJumpjetLocomotionClass::Load(IStream * stream)
{
    HRESULT result = BASECLASS::Locomotion_Load(stream);
    if (SUCCEEDED(result)) {
        new (this) NewJumpjetLocomotionClass(NoInitClass());
    }
    return result;
}


IFACEMETHODIMP_(LayerType) NewJumpjetLocomotionClass::In_Which_Layer()
{
    int height = LinkedTo->HeightAGL;
    if (!LinkedTo->IsOnBridge) {
        if (Map[LinkedTo->PositionCoord].IsUnderBridge && height >= BRIDGE_LEPTON_HEIGHT && !LinkedTo->IsFalling) {
            height -= BRIDGE_LEPTON_HEIGHT;
        }
    }

    if (height == 0) {
        return LAYER_GROUND;
    } else if (height < JumpjetCruiseHeight) {
        return LAYER_AIR;
    } else {
        return LAYER_TOP;
    }
}


void NewJumpjetLocomotionClass::Process_Grounded()
{
    if (Is_Moving() || Is_Balloon_Hover()) {
        LinkedTo->Set_Speed(1.0);
        Facing.Set(LinkedTo->PrimaryFacing.Current());
        CurrentSpeed = 0;
        TargetSpeed = 0;
        FlightLevel = JumpjetCruiseHeight;
        if (Is_Balloon_Hover()) {
            if (!IsMoving) HeadToCoord = LinkedTo->PositionCoord;
            if (Balloon_Factory()) {
                const int roof = Map[LinkedTo->PositionCoord].Occupier_Height() -
                    Map.Get_Height_GL(LinkedTo->PositionCoord);
                FlightLevel = std::max(FlightLevel, roof + LEVEL_LEPTON_H);
            }
        }
        if (!IonStorm_Is_Active()) {
            const auto extension = Extension::Fetch(LinkedTo);
            if (extension->Get_Last_Flight_Cell() == CELL_NONE) {
                AircraftTracker->Track(LinkedTo);
            }

            CurrentState = ASCENDING;
            if (Is_Balloon_Hover() && Vinifera_DeveloperMode) DEBUG_INFO("[BALLOON] type={} initial ascent\n", LinkedTo->TClass->Name());
        }
    }
}


void NewJumpjetLocomotionClass::Process_Ascent()
{
    int height = LinkedTo->HeightAGL;
    if (!LinkedTo->IsOnBridge) {
        if (Map[LinkedTo->PositionCoord].IsUnderBridge && height >= BRIDGE_LEPTON_HEIGHT) {
            height -= BRIDGE_LEPTON_HEIGHT;
        }
    }

    const auto extension = Extension::Fetch(LinkedTo);
    Cell oldcell = extension->Get_Last_Flight_Cell();
    Cell newcell = LinkedTo->Get_Cell();

    if (newcell != oldcell) {
        AircraftTracker->Update_Position(LinkedTo, oldcell, newcell);
    }

    if (height >= FlightLevel) {
        if (Is_Balloon_Hover()) {
            if (const auto factory = Balloon_Factory()) {
                Release_Balloon_Factory(factory);
            } else {
                CurrentState = HOVERING;
                FlightLevel = JumpjetCruiseHeight;
                if (!IsMoving) HeadToCoord = COORD_NONE;
            }
        } else {
            CurrentState = HOVERING;
        }
    } else if (height > FlightLevel / 4) {
        if (!Is_Balloon_Hover()) {
            TargetSpeed = JumpjetSpeed;
            Facing.Set_Desired(Direction(LinkedTo->PositionCoord, HeadToCoord));
        }
    }
}


void NewJumpjetLocomotionClass::Process_Hover()
{
    if (Is_Moving()) {
        Coord headto = HeadToCoord;
        Coord position = LinkedTo->PositionCoord;
        if (Point2D(headto.X, headto.Y) == Point2D(position.X, position.Y)) {
            if (Is_Balloon_Hover()) {
                Complete_Balloon_Move();
            } else if (LinkedTo->TarCom == nullptr) {
                CurrentState = DESCENDING;
            }
        } else {
            if (Is_Balloon_Hover()) Facing.Set(LinkedTo->PrimaryFacing.Current());
            Facing.Set_Desired(Direction(LinkedTo->PositionCoord, HeadToCoord));
            CurrentState = CRUISING;
        }
    }
}


void NewJumpjetLocomotionClass::Process_Cruise()
{
    Coord position = LinkedTo->PositionCoord;

    const auto extension = Extension::Fetch(LinkedTo);
    Cell oldcell = extension->Get_Last_Flight_Cell();
    Cell newcell = LinkedTo->Get_Cell();

    if (newcell != oldcell) {
        AircraftTracker->Update_Position(LinkedTo, oldcell, newcell);
    }

    Facing.Set_Desired(Direction(position, HeadToCoord));

    int distance = Point2D(position.X, position.Y).Distance_To(Point2D(HeadToCoord.X, HeadToCoord.Y));
    if (distance < 20) {
        CurrentSpeed = 0;
        TargetSpeed = 0;
        position.X = HeadToCoord.X;
        position.Y = HeadToCoord.Y;
        bool down = LinkedTo->IsDown;
        LinkedTo->IsDown = false;
        LinkedTo->PositionCoord = position;
        LinkedTo->IsDown = down;
        if (Is_Balloon_Hover()) {
            Complete_Balloon_Move();
        } else if (LinkedTo->TarCom == nullptr) {
            FlightLevel = 0;
            CurrentState = DESCENDING;
        } else {
            CurrentState = HOVERING;
        }
    } else if (distance < CELL_LEPTON) {
        TargetSpeed = JumpjetSpeed * 0.3;
        if (!Is_Balloon_Hover() && LinkedTo->TarCom == nullptr) {
            FlightLevel = static_cast<int>(JumpjetCruiseHeight * 0.75);
        }
    } else if (distance < CELL_LEPTON * 2) {
        TargetSpeed = JumpjetSpeed * 0.5;
    } else {
        TargetSpeed = JumpjetSpeed;
        FlightLevel = JumpjetCruiseHeight;
    }
}


void NewJumpjetLocomotionClass::Process_Descent()
{
    if (Is_Balloon_Hover()) {
        if (IsLanding && HeadToCoord != COORD_NONE) LinkedTo->Clear_Occupy_Bit(HeadToCoord);
        IsLanding = false;
        FlightLevel = JumpjetCruiseHeight;
        CurrentState = ASCENDING;
        return;
    }
    CellClass * cellptr = &Map[HeadToCoord];
    MoveType move = LinkedTo->Can_Enter_Cell(cellptr);
    int spot = CellClass::Spot_Index(HeadToCoord);
    bool stop = true;

    if ((cellptr->Is_Spot_Free(spot, cellptr->IsUnderBridge) || IsLanding) &&
        (Map[HeadToCoord].IsUnderBridge || move <= MOVE_MOVING_BLOCK && (move != MOVE_MOVING_BLOCK || IsLanding))) {
        
        if (!IsLanding) {
            IsLanding = true;
            LinkedTo->Set_Occupy_Bit(HeadToCoord);
        }

        FlightLevel = 0;

        int height = LinkedTo->HeightAGL;
        if (!LinkedTo->IsOnBridge) {
            if (Map[LinkedTo->PositionCoord].IsUnderBridge && height >= BRIDGE_LEPTON_HEIGHT) {
                height -= BRIDGE_LEPTON_HEIGHT;
            }
        }

        if (height == 0) {
            LinkedTo->Set_Speed(0.0);
            LinkedTo->Mark(MARK_UP);
            LinkedTo->PositionCoord = HeadToCoord;

            if (LinkedTo->PositionCoord.Z > Map.Get_Height_GL(LinkedTo->PositionCoord)) {
                if (Map[LinkedTo->PositionCoord].IsUnderBridge) {
                    LinkedTo->IsOnBridge = true;
                }
            }

            LinkedTo->Mark(MARK_DOWN);
            HeadToCoord = COORD_NONE;
            IsMoving = false;
            LinkedTo->Assign_Destination(nullptr);
            LinkedTo->Per_Cell_Process(PCP_END);

            if (LinkedTo != nullptr && LinkedTo->IsActive && !LinkedTo->IsInLimbo && !LinkedTo->IsFalling) {
                LinkedTo->Look();
                AircraftTracker->Untrack(LinkedTo);
                CurrentState = GROUNDED;
                IsLanding = false;
            }
        }
        stop = false;
    }

    if (stop) Stop_Moving();
}


IFACEMETHODIMP_(bool) NewJumpjetLocomotionClass::Is_Moving_Now()
{
    if (Is_Balloon_Hover() && CurrentState == HOVERING) return CurrentSpeed > 0;
    if (CurrentState != GROUNDED && CurrentState != HOVERING) {
        return true;
    }
    return false;
}


void NewJumpjetLocomotionClass::Movement_AI()
{
    const bool balloon = Is_Balloon_Hover();
    bool need_to_mark = CurrentState != HOVERING && CurrentState != CRUISING;
    bool was_down = LinkedTo->IsDown;

    if (need_to_mark) {
        LinkedTo->Mark(MARK_UP);
    } else {
        LinkedTo->IsDown = false;
    }

    if (TargetSpeed > CurrentSpeed) {
        CurrentSpeed += JumpjetAcceleration;
        CurrentSpeed = std::min(CurrentSpeed, static_cast<double>(JumpjetSpeed));
    }
    if (TargetSpeed < CurrentSpeed) {
        CurrentSpeed -= JumpjetAcceleration * 1.5;
        CurrentSpeed = std::max(CurrentSpeed, 0.0);
    }

    LinkedTo->Set_Speed(CurrentSpeed / JumpjetSpeed);

    bool at_destination = LinkedTo->Get_Cell() == HeadToCoord.As_Cell();

    if ((CurrentState == HOVERING || CurrentState == CRUISING) && !JumpjetNoWobbles) {
        CurrentWobble += DEG_TO_RAD(360) / (15.0 / JumpjetWobblesPerSecond);
    } else {
        CurrentWobble = 0;
    }

    int desired_height = std::sin(CurrentWobble) * JumpjetWobbleDeviation + FlightLevel;
    int height = LinkedTo->Height;
    int ground_height = Map.Get_Height_GL(LinkedTo->PositionCoord);

    if (Map[LinkedTo->PositionCoord].IsUnderBridge && LinkedTo->PositionCoord.Z >= ground_height + 4 * LEVEL_LEPTON_H) {
        ground_height += BRIDGE_LEPTON_HEIGHT;
    }

    int height_diff = 0;
    if (CurrentState != DESCENDING && CurrentState != GROUNDED && (balloon || !at_destination)) {
        height_diff = height - Desired_Flight_Level();
    } else {
        height_diff = height - ground_height;
    }

    bool moved = false;
    if (height_diff < desired_height) {
        int height_agl = LinkedTo->HeightAGL;
        if (Map[LinkedTo->PositionCoord].IsUnderBridge && !LinkedTo->IsOnBridge) {
            if (LinkedTo->PositionCoord.Z >= Map.Get_Height_GL(LinkedTo->PositionCoord) + BRIDGE_LEPTON_HEIGHT) {
                height_agl -= BRIDGE_LEPTON_HEIGHT;
            }
        }
        if (height_agl == 0) {
            LinkedTo->Clear_Occupy_Bit(LinkedTo->PositionCoord);
            LinkedTo->IsOnBridge = false;
        }
        height += JumpjetClimb;
        moved = true;
    }
    if (height_diff > desired_height) {
        height -= JumpjetClimb;
        height = std::max(height, ground_height);
        moved = true;
        height_diff = std::max(height_diff, 0);
    }

    if (LinkedTo->Get_Cell() != HeadToCoord.As_Cell()) {
        if (height_diff < desired_height / 2) {
            CurrentSpeed *= 0.9;
        }
        if (height_diff < desired_height / 4) {
            CurrentSpeed *= 0.9;
        }
    }

    if (moved) {
        LinkedTo->Height = height;
        if (need_to_mark) {
            LinkedTo->Mark(MARK_UP);
        }
    }

    Coord new_coord = Coord_Move(LinkedTo->PositionCoord, Facing.Current(), CurrentSpeed);
    if (balloon && !Balloon_Flight_Coord(new_coord)) {
        new_coord = LinkedTo->PositionCoord;
        Stop_Moving();
        LinkedTo->NavCom = nullptr;
        CurrentSpeed = 0;
    }
    LinkedTo->PositionCoord = new_coord;
    if (balloon) {
        const Cell oldcell = Extension::Fetch(LinkedTo)->Get_Last_Flight_Cell();
        const Cell newcell = LinkedTo->Get_Cell();
        if (oldcell != CELL_NONE && oldcell != newcell) AircraftTracker->Update_Position(LinkedTo, oldcell, newcell);
    }

    if (LinkedTo != nullptr) {
        const int & rad = JumpjetCloakDetectionRadius;
        for (int x = -rad; x <= rad; x++) {
            for (int y = -rad; y <= rad; y++) {
                const Cell scan_cell = Cell(x, y) + new_coord.As_Cell();
                if (balloon && !Map.In_Radar(scan_cell)) continue;
                CellClass * cellptr = &Map[scan_cell];
                if (cellptr != nullptr) {
                    ObjectClass * occupier = cellptr->Cell_Occupier(false);
                    while (occupier != nullptr) {
                        TechnoClass * tech = dynamic_cast<TechnoClass *>(occupier);
                        if (tech != nullptr) {
                            tech->Do_Shimmer();
                        }
                        occupier = occupier->Next;
                    }
                    occupier = cellptr->Cell_Occupier(true);
                    while (occupier != nullptr) {
                        TechnoClass * tech = dynamic_cast<TechnoClass *>(occupier);
                        if (tech != nullptr) {
                            tech->Do_Shimmer();
                        }
                        occupier = occupier->Next;
                    }
                }
            }
        }
    }

    CellClass * cellptr = &Map[new_coord];
    BuildingClass * building = cellptr->Cell_Building();
    if (building && building->Class->IsFirestormWall && building->House->IsFirestormActive) {
        building->Crossing_Firestorm(LinkedTo, true);
    }

    if (!balloon || CurrentState != HOVERING || CurrentSpeed > 0) LinkedTo->PrimaryFacing.Set(Facing.Current());

    if (need_to_mark) {
        LinkedTo->Mark(MARK_DOWN);
    } else {
        LinkedTo->IsDown = was_down;
    }
}


Coord NewJumpjetLocomotionClass::Closest_Free_Spot(Coord const & to) const
{
    Coord closest = Map.Closest_Free_Spot(to);
    if (closest != COORD_NONE) {
        closest.Z = Map.Get_Height_GL(closest);
        if (Map[closest].IsUnderBridge) {
            closest.Z += BRIDGE_LEPTON_HEIGHT;
        }
    }
    return closest;
}


int NewJumpjetLocomotionClass::Desired_Flight_Level() const
{
    const bool balloon = Is_Balloon_Hover();
    Coord coord = LinkedTo->PositionCoord;

    int height = Map[coord].Occupier_Height();
    if (Map[coord].IsUnderBridge) {
        height += BRIDGE_LEPTON_HEIGHT;
    }

    if (CurrentSpeed > 0) {
        coord = Adjacent_Cell(coord, static_cast<FacingType>(Facing.Current().Get_Facing<8>()));
        if (balloon && !Balloon_Flight_Coord(coord)) return height;
        int adjancent_height = Map[coord].Occupier_Height();
        if (Map[coord].IsUnderBridge) {
            if (balloon) adjancent_height += BRIDGE_LEPTON_HEIGHT;
            else height += BRIDGE_LEPTON_HEIGHT;
        }
        if (adjancent_height > height) {
            return adjancent_height;
        }
        return(height + adjancent_height) / 2;
    }
    return height;
}


IFACEMETHODIMP_(void) NewJumpjetLocomotionClass::Mark_All_Occupation_Bits(MarkType mark)
{
    if (mark == MARK_UP) {
        Coord headto = Head_To_Coord();
        if (headto != COORD_NONE && (CurrentState == GROUNDED || IsLanding)) {
            LinkedTo->Clear_Occupy_Bit(headto);
            IsLanding = false;
        }
    }
}


IFACEMETHODIMP_(Coord) NewJumpjetLocomotionClass::Head_To_Coord()
{
    if (CurrentState == GROUNDED) {
        return LinkedTo->PositionCoord;
    } else {
        return HeadToCoord;
    }
}
