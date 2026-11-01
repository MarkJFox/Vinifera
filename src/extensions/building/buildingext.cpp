/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended BuildingClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "buildingext.h"

#include "aircraft.h"
#include "aircrafttype.h"
#include "aircrafttypeext.h"
#include "asserthandler.h"
#include "building.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "extension.h"
#include "house.h"
#include "housetype.h"
#include "cell.h"
#include "debughandler.h"
#include "map.h"
#include "mouse.h"
#include "tibsun_globals.h"
#include "vinifera_globals.h"
#include "vinifera_saveload.h"
#include "wwcrc.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{
    void Trace_Aircraft_Dock_Service(BuildingClass* building, AircraftClass* aircraft,
        int slot, int timer, const Coord& dock, const char* event, const char* reason,
        int ammo_before, RadioMessageType reply)
    {
        if (!Vinifera_DeveloperMode) return;
        const Coord position = aircraft->Get_Coord();
        DEBUG_INFO("[MDOCK] building={} event={} slot={} aircraft={} ammo={}/{} ammoBefore={} "
            "mission={} queued={} status={} buildingMission={} buildingQueued={} buildingStatus={} "
            "bstate={} timer={} radioMatch={} tether={} height={} cellMatch={} slotCellMatch={} "
            "navMatch={} navNone={} reason={} reply={}\n",
            building->ID, event, slot, aircraft->ID, aircraft->Ammo, aircraft->Class->MaxAmmo, ammo_before,
            static_cast<int>(aircraft->CurrentMission), static_cast<int>(aircraft->MissionQueue), aircraft->Status,
            static_cast<int>(building->CurrentMission), static_cast<int>(building->MissionQueue), building->Status,
            static_cast<int>(building->BState), timer, aircraft->Radio == building, aircraft->IsTethered,
            aircraft->HeightAGL, Map[position].Cell_Building() == building, position.As_Cell() == dock.As_Cell(),
            aircraft->NavCom == building, aircraft->NavCom == nullptr, reason, static_cast<int>(reply));
    }
}


/**
 *  Class constructor.
 *
 *  @author: CCHyper
 */
BuildingClassExtension::BuildingClassExtension(const BuildingClass *this_ptr) :
    TechnoClassExtension(this_ptr),
    ProduceCashTimer(),
    CurrentProduceCashBudget(-1),
    IsCaptureOneTimeCashGiven(false),
    IsBudgetDepleted(false),
    LastFlameSpawnFrame(0),
    AircraftDockFullReported(false)
{
    std::fill(std::begin(AircraftDockVisitors), std::end(AircraftDockVisitors), nullptr);
    std::fill(std::begin(AircraftDockReloadTimers), std::end(AircraftDockReloadTimers), -1);
    BuildingExtensions.Add(this);
}


/**
 *  Class no-init constructor.
 *  
 *  @author: CCHyper
 */
BuildingClassExtension::BuildingClassExtension(const NoInitClass &noinit) :
    TechnoClassExtension(noinit)
{
}


/**
 *  Class destructor.
 *  
 *  @author: CCHyper
 */
BuildingClassExtension::~BuildingClassExtension()
{
    if (!Vinifera_PerformingLoad && Buildings.Is_Present(This())) Release_All_Aircraft_Docks("BUILDING_DESTROY");
    BuildingExtensions.Delete(this);
}


/**
 *  Retrieves the class identifier (CLSID) of the object.
 *  
 *  @author: CCHyper
 */
HRESULT BuildingClassExtension::GetClassID(CLSID *lpClassID)
{
    if (lpClassID == nullptr) {
        return E_POINTER;
    }

    *lpClassID = __uuidof(this);

    return S_OK;
}


/**
 *  Initializes an object from the stream where it was saved previously.
 *  
 *  @author: CCHyper
 */
HRESULT BuildingClassExtension::Load(IStream *pStm)
{
    HRESULT hr = TechnoClassExtension::Load(pStm);
    if (FAILED(hr)) {
        return E_FAIL;
    }

    new (this) BuildingClassExtension(NoInitClass());

    for (int i = 0; i < BuildingTypeClassExtension::MAX_AIRCRAFT_DOCKS; ++i) {
        if (AircraftDockReloadTimers[i] < -2 ||
            (AircraftDockVisitors[i] == nullptr && AircraftDockReloadTimers[i] != -1)) return E_FAIL;
        if (AircraftDockVisitors[i] != nullptr) {
            VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(AircraftDockVisitors[i], "AircraftDockVisitors");
        }
    }
    
    return hr;
}


/**
 *  Saves an object to the specified stream.
 *  
 *  @author: CCHyper
 */
HRESULT BuildingClassExtension::Save(IStream *pStm, BOOL fClearDirty)
{
    HRESULT hr = TechnoClassExtension::Save(pStm, fClearDirty);
    if (FAILED(hr)) {
        return hr;
    }

    return hr;
}


/**
 *  Return the raw size of class data for save/load purposes.
 *  
 *  @author: CCHyper
 */
int BuildingClassExtension::Get_Object_Size() const
{
    return sizeof(*this);
}




/**
 *  Compute a unique crc value for this instance.
 *  
 *  @author: CCHyper
 */
void BuildingClassExtension::Object_CRC(CRCEngine &crc) const
{
    /**
     *  #issue-26
     * 
     *  Members for the Produce Cash logic.
     */
    crc(ProduceCashTimer());
    if (Is_Aircraft_MultiDock()) {
        const int count = Extension::Fetch(This_Const()->Class)->AircraftDockCount;
        crc(0x4D444F43u);
        crc(count);
        for (int i = 0; i < count; ++i) {
            const auto visitor = AircraftDockVisitors[i];
            crc(i);
            crc(visitor != nullptr ? 1 : 0);
            if (visitor != nullptr) crc(visitor->Fetch_Heap_ID());
            crc(AircraftDockReloadTimers[i]);
        }
    }
}

bool BuildingClassExtension::Is_Aircraft_MultiDock() const
{
    if (Vinifera_PerformingLoad) return false; // Native ThisPtr/Class may still be raw swizzle IDs.
    const auto building = This_Const();
    const auto type = building != nullptr && building->Class != nullptr ? Extension::Fetch(building->Class) : nullptr;
    return type != nullptr && type->Is_Aircraft_MultiDock();
}

int BuildingClassExtension::Find_Aircraft_Dock_Slot(const AircraftClass* aircraft) const
{
    if (aircraft == nullptr || !Is_Aircraft_MultiDock()) return -1;
    const int count = Extension::Fetch(This_Const()->Class)->AircraftDockCount;
    for (int i = 0; i < count; ++i) if (AircraftDockVisitors[i] == aircraft) return i;
    return -1;
}

int BuildingClassExtension::Find_First_Free_Aircraft_Dock_Slot() const
{
    if (!Is_Aircraft_MultiDock()) return -1;
    const int count = Extension::Fetch(This_Const()->Class)->AircraftDockCount;
    for (int i = 0; i < count; ++i) if (AircraftDockVisitors[i] == nullptr) return i;
    return -1;
}

bool BuildingClassExtension::Can_Accept_Aircraft_Dock(const AircraftClass* aircraft, bool production) const
{
    const auto building = This_Const();
    if (!Is_Aircraft_MultiDock() || aircraft == nullptr || aircraft->RTTI != RTTI_AIRCRAFT ||
        aircraft->Class == nullptr || Extension::Fetch(aircraft->Class)->IsFixedWing ||
        aircraft->Class->IsCarryall || !aircraft->Class->Dock.Is_Present(building->Class) ||
        !building->House->Is_Ally(aircraft) || !building->IsActive || building->IsInLimbo ||
        building->Strength <= 0 || !building->IsOn || !building->Is_Powered_On() ||
        building->EMPFramesRemaining > 0 || building->Mission == MISSION_CONSTRUCTION ||
        building->Mission == MISSION_DECONSTRUCTION || building->BState == BSTATE_CONSTRUCTION ||
        (building->Radio != nullptr && building->Radio->RTTI != RTTI_AIRCRAFT) ||
        aircraft->Strength <= 0 || (!production && (!aircraft->IsActive || aircraft->IsInLimbo))) return false;
    return Find_Aircraft_Dock_Slot(aircraft) >= 0 || Find_First_Free_Aircraft_Dock_Slot() >= 0;
}

bool BuildingClassExtension::Has_Aircraft_Dock_Visitors() const
{
    return std::any_of(std::begin(AircraftDockVisitors), std::end(AircraftDockVisitors),
        [](const AircraftClass* visitor) { return visitor != nullptr; });
}

bool BuildingClassExtension::Aircraft_Dock_Coord(int slot, Coord& result) const
{
    if (!Is_Aircraft_MultiDock()) return false;
    const auto building = This_Const();
    const auto type = Extension::Fetch(building->Class);
    if (slot < 0 || slot >= type->AircraftDockCount) return false;
    result = (building->Get_Cell() + type->AircraftDockCells[slot]).As_Coord(building->Get_Coord().Z);
    return true;
}

bool BuildingClassExtension::Aircraft_Dock_Coord(const AircraftClass* aircraft, Coord& result) const
{
    return aircraft != nullptr && aircraft->Radio == This_Const() &&
        Aircraft_Dock_Coord(Find_Aircraft_Dock_Slot(aircraft), result);
}

int BuildingClassExtension::Reserve_Aircraft_Dock(AircraftClass* aircraft, bool production)
{
    const int existing = Find_Aircraft_Dock_Slot(aircraft);
    if (existing >= 0 && aircraft->Radio == This()) return existing;
    if (!Can_Accept_Aircraft_Dock(aircraft, production)) return -1;
    const int slot = existing >= 0 ? existing : Find_First_Free_Aircraft_Dock_Slot();
    if (slot < 0) return -1;
    if (aircraft->Radio != nullptr && aircraft->Radio != This()) aircraft->Transmit_Message(RADIO_OVER_OUT);
    AircraftDockVisitors[slot] = aircraft;
    AircraftDockReloadTimers[slot] = -1;
    long param = 0;
    // Keep the native Aircraft-side HELLO acceptance and single Radio field.
    // Building-side HELLO never enters RadioClass's single-partner setter.
    if (aircraft->RadioClass::Receive_Message(This(), RADIO_HELLO, param) != RADIO_ROGER) {
        AircraftDockVisitors[slot] = nullptr;
        return -1;
    }
    if (Vinifera_DeveloperMode && existing < 0) {
        DEBUG_INFO("[MDOCK] building={} event=SLOT_RESERVE slot={} aircraft={}\n", This()->ID, slot, aircraft->ID);
    }
    return slot;
}

void BuildingClassExtension::Release_Aircraft_Dock(AircraftClass* aircraft, const char* reason, bool notify, bool wake)
{
    const int slot = Find_Aircraft_Dock_Slot(aircraft);
    if (slot < 0) return;
    // Clear first: native OVER_OUT/UNTETHER echo through our Receive_Message.
    AircraftDockVisitors[slot] = nullptr;
    AircraftDockReloadTimers[slot] = -1;
    AircraftDockFullReported = false;
    if (Vinifera_DeveloperMode) {
        DEBUG_INFO("[MDOCK] building={} event=SLOT_RELEASE slot={} reason={}\n", This()->ID, slot, reason);
    }
    if (notify && Aircrafts.Is_Present(aircraft) &&
        (aircraft->Radio == This() || aircraft->Radio == nullptr)) {
        if (!aircraft->IsActive || aircraft->IsInLimbo || aircraft->Strength <= 0) {
            if (aircraft->Radio == This()) aircraft->Radio = nullptr;
            aircraft->IsTethered = false;
            return; // No mission/locomotor callbacks into a dying or limbo object.
        }
        long param = 0;
        // Native Aircraft TX clears its Radio BEFORE delivering OVER_OUT.
        // Untether this visitor even in that case; never disturb a newer peer.
        aircraft->Receive_Message(This(), RADIO_UNTETHER, param);
        if (aircraft->Radio == This()) aircraft->Receive_Message(This(), RADIO_OVER_OUT, param);
        if (aircraft->Radio == This()) aircraft->Radio = nullptr;
        aircraft->IsTethered = false;
        // A normal visitor-initiated OVER_OUT must preserve its queued command.
        // Forced cleanup alone wakes a visitor stranded in Sleep/Enter.
        if (wake && (aircraft->Mission == MISSION_SLEEP ||
            (aircraft->NavCom == This() && aircraft->Mission == MISSION_ENTER))) {
            if (aircraft->NavCom == This()) aircraft->Assign_Destination(nullptr);
            aircraft->Assign_Mission(MISSION_GUARD);
        }
    }
}

void BuildingClassExtension::Release_All_Aircraft_Docks(const char* reason)
{
    if (!Is_Aircraft_MultiDock()) return;
    for (int i = 0; i < BuildingTypeClassExtension::MAX_AIRCRAFT_DOCKS; ++i) {
        if (AircraftDockVisitors[i] != nullptr) Release_Aircraft_Dock(AircraftDockVisitors[i], reason);
    }
}

void BuildingClassExtension::On_Detach(TechnoClass* target, bool all)
{
    TechnoClassExtension::On_Detach(target, all);
    // Pointer identity only. Never call back into the object being destroyed.
    for (int i = 0; i < BuildingTypeClassExtension::MAX_AIRCRAFT_DOCKS; ++i) {
        if (AircraftDockVisitors[i] == target) {
            AircraftDockVisitors[i] = nullptr;
            AircraftDockReloadTimers[i] = -1;
            AircraftDockFullReported = false;
        }
    }
}

void BuildingClassExtension::Maintain_Aircraft_Docks()
{
    if (Vinifera_PerformingLoad || !Is_Aircraft_MultiDock()) return;
    const int count = Extension::Fetch(This()->Class)->AircraftDockCount;
    for (int i = 0; i < count; ++i) {
        const auto aircraft = AircraftDockVisitors[i];
        if (aircraft == nullptr) continue;
        if (!Aircrafts.Is_Present(aircraft)) {
            AircraftDockVisitors[i] = nullptr;
            AircraftDockReloadTimers[i] = -1;
            AircraftDockFullReported = false;
        } else if (!aircraft->IsActive || aircraft->IsInLimbo || aircraft->Strength <= 0 ||
                   aircraft->Radio != This() || !This()->House->Is_Ally(aircraft)) {
            Release_Aircraft_Dock(aircraft, "STALE_CONTACT");
        } else if (aircraft->Mission == MISSION_SLEEP && AircraftDockReloadTimers[i] != -2 &&
                   This()->Mission == MISSION_GUARD && This()->BState != BSTATE_CONSTRUCTION) {
            // Native Mission_Guard's service wake-up reads the scalar Radio.
            // Give an arrived MultiDock visitor the same wake-up via membership.
            This()->IsReadyToCommence = true;
            This()->Assign_Mission(MISSION_REPAIR);
        }
    }
    // Power/EMP never clear reservations; these states only gate acceptance/service.
}

RadioMessageType BuildingClassExtension::Aircraft_Dock_Transmit(RadioMessageType message, long& param, RadioClass* to)
{
    if (to == nullptr) {
        // Native RadioClass::Limbo sends OVER_OUT without an explicit recipient.
        if (message == RADIO_OVER_OUT) {
            Release_All_Aircraft_Docks("BUILDING_UNLINK");
            return RADIO_ROGER;
        }
        return RADIO_STATIC; // No guessed implicit visitor for multi-contact messages.
    }
    if (to->RTTI != RTTI_AIRCRAFT) return RADIO_NEGATIVE;
    auto aircraft = static_cast<AircraftClass*>(to);
    if (message == RADIO_HELLO) {
        // Actual pad production reserved before Unlimbo. Nearby/edge production
        // deliberately has no reservation and must not acquire a phantom dock.
        return Find_Aircraft_Dock_Slot(aircraft) >= 0 && Reserve_Aircraft_Dock(aircraft, true) >= 0 ?
            RADIO_ROGER : RADIO_NEGATIVE;
    }
    if (message == RADIO_OVER_OUT) {
        Release_Aircraft_Dock(aircraft, "OVER_OUT");
        return RADIO_ROGER;
    }
    if (Find_Aircraft_Dock_Slot(aircraft) < 0 || aircraft->Radio != This()) return RADIO_NEGATIVE;
    return aircraft->Receive_Message(This(), message, param);
}

RadioMessageType BuildingClassExtension::Aircraft_Dock_Receive(RadioClass* from, RadioMessageType message, long& param)
{
    if (from == nullptr || from->RTTI != RTTI_AIRCRAFT) return RADIO_NEGATIVE;
    auto aircraft = static_cast<AircraftClass*>(from);
    auto building = This();
    if (building->Old[0] != message) {
        building->Old[2] = building->Old[1];
        building->Old[1] = building->Old[0];
        building->Old[0] = message;
    }
    switch (message) {
    case RADIO_CAN_LOAD:
        return Can_Accept_Aircraft_Dock(aircraft) ? RADIO_ROGER : RADIO_NEGATIVE;
    case RADIO_HELLO:
    case RADIO_DOCKING: {
        const int slot = Reserve_Aircraft_Dock(aircraft);
        if (slot < 0) {
            if (Vinifera_DeveloperMode && !AircraftDockFullReported && Find_First_Free_Aircraft_Dock_Slot() < 0) {
                DEBUG_INFO("[MDOCK] building={} event=SLOT_FULL aircraft={}\n", building->ID, aircraft->ID);
                AircraftDockFullReported = true;
            }
            return RADIO_NEGATIVE;
        }
        if (message == RADIO_DOCKING && aircraft->NavCom != building) {
            // Native MOVE_HERE carries an AbstractClass*, not a Coord*. The
            // caller-specific destination adapters below resolve this visitor.
            if (Aircraft_Dock_Transmit(RADIO_NEED_TO_MOVE, param, aircraft) == RADIO_ROGER) {
                param = reinterpret_cast<long>(building);
                if (Aircraft_Dock_Transmit(RADIO_MOVE_HERE, param, aircraft) != RADIO_NEGATIVE) {
                    Aircraft_Dock_Transmit(RADIO_TETHER, param, aircraft);
                }
            }
        }
        return RADIO_ROGER;
    }
    case RADIO_OVER_OUT:
        Release_Aircraft_Dock(aircraft, "OVER_OUT", true, false);
        return RADIO_ROGER;
    case RADIO_TETHER:
        if (Find_Aircraft_Dock_Slot(aircraft) < 0) return RADIO_NEGATIVE;
        // Preserve the native reciprocal echo, with per-Aircraft tether state.
        // The echo ends when Aircraft's native handler has set its own flag.
        return aircraft->IsTethered ? RADIO_ROGER : Aircraft_Dock_Transmit(message, param, aircraft);
    case RADIO_UNTETHER: {
        const int slot = Find_Aircraft_Dock_Slot(aircraft);
        if (slot >= 0) AircraftDockReloadTimers[slot] = -1;
        if (slot >= 0 && aircraft->IsTethered) return Aircraft_Dock_Transmit(message, param, aircraft);
        return RADIO_ROGER; // OVER_OUT is the actual release; never clear other visitors.
    }
    case RADIO_ON_DEPOT:
        return Find_Aircraft_Dock_Slot(aircraft) >= 0 &&
            Map[aircraft->Center_Coord()].Cell_Building() == building ? RADIO_ROGER : RADIO_NEGATIVE;
    case RADIO_IM_IN: {
        Coord dock;
        if (!Aircraft_Dock_Coord(aircraft, dock) || !building->IsActive || building->IsInLimbo ||
            building->Strength <= 0 || !building->House->Is_Ally(aircraft) ||
            building->Mission == MISSION_CONSTRUCTION || building->Mission == MISSION_DECONSTRUCTION) return RADIO_NEGATIVE;
        // Keep native IM_IN/Sleep timing. The service loop separately requires
        // actual touchdown on THIS slot before starting its first reload timer.
        aircraft->Assign_Mission(MISSION_SLEEP);
        Aircraft_Dock_Transmit(RADIO_TETHER, param, aircraft);
        building->IsReadyToCommence = true;
        if (building->Mission != MISSION_REPAIR) building->Assign_Mission(MISSION_REPAIR);
        if (Vinifera_DeveloperMode) {
            const int slot = Find_Aircraft_Dock_Slot(aircraft);
            if (slot >= 0) {
                Trace_Aircraft_Dock_Service(building, aircraft, slot, AircraftDockReloadTimers[slot], dock,
                    "SERVICE_IM_IN", "ACCEPTED", aircraft->Ammo, RADIO_ROGER);
            }
        }
        return RADIO_ROGER;
    }
    default:
        return RADIO_STATIC;
    }
}

int BuildingClassExtension::Aircraft_Dock_Service()
{
    Maintain_Aircraft_Docks();
    auto building = This();
    if (!building->IsOn || !building->Is_Powered_On() || building->EMPFramesRemaining > 0) return 1;
    const int count = Extension::Fetch(building->Class)->AircraftDockCount;
    bool pending = false;
    for (int i = 0; i < count; ++i) {
        auto aircraft = AircraftDockVisitors[i];
        if (aircraft == nullptr) continue;
        Coord dock;
        if (!Aircraft_Dock_Coord(aircraft, dock)) continue;
        if (aircraft->Mission == MISSION_ENTER || aircraft->HeightAGL != 0 || !aircraft->IsTethered ||
            Map[aircraft->Center_Coord()].Cell_Building() != building ||
            std::abs(aircraft->Get_Coord().X - dock.X) > 5 ||
            std::abs(aircraft->Get_Coord().Y - dock.Y) > 5) {
            pending = true;
            continue;
        }
        if (AircraftDockReloadTimers[i] == -2) continue;
        auto& deadline = AircraftDockReloadTimers[i];
        long param = 0;
        // Match native UnitReload initialization (432620): ask NEED_TO_MOVE,
        // then PREPARED. Sleep is assigned by service, not its prerequisite.
        if (deadline == -1 && Aircraft_Dock_Transmit(RADIO_NEED_TO_MOVE, param, aircraft) != RADIO_ROGER) {
            pending = true;
            continue;
        }
        const auto finish = [&](const char* reason, RadioMessageType reply) {
            AircraftDockReloadTimers[i] = -2;
            Trace_Aircraft_Dock_Service(building, aircraft, i, deadline, dock,
                "SERVICE_COMPLETE", reason, aircraft->Ammo, reply);
            aircraft->Assign_Mission(MISSION_GUARD);
            aircraft->Enter_Idle_Mode(false, true);
            aircraft->entry_2C8();
        };
        if (Aircraft_Dock_Transmit(RADIO_PREPARED, param, aircraft) == RADIO_ROGER) {
            finish("PREPARED", RADIO_ROGER);
            continue;
        }
        pending = true;
        const double rate = Extension::Fetch(aircraft->Class)->Get_ReloadRate() * TICKS_PER_MINUTE;
        const int delay = std::isfinite(rate) ? static_cast<int>(std::clamp(rate, 1.0,
            static_cast<double>(std::numeric_limits<int>::max() - Frame))) : 1;
        if (deadline == -1) {
            // Native Mission_Enter queues Guard after successful IM_IN (40D6D3),
            // replacing IM_IN's queued Sleep. Native service queues Sleep here
            // (432659), before returning the initial ReloadRate delay.
            aircraft->Assign_Mission(MISSION_SLEEP);
            deadline = Frame + delay;
            if (building->BState != BSTATE_ACTIVE) building->Begin_Mode(BSTATE_ACTIVE);
            Trace_Aircraft_Dock_Service(building, aircraft, i, deadline, dock,
                "SERVICE_START", "SLEEP_QUEUED", aircraft->Ammo, RADIO_ROGER);
        } else if (aircraft->Mission == MISSION_SLEEP && Frame >= deadline) {
            const int ammo_before = aircraft->Ammo;
            const auto reply = Aircraft_Dock_Transmit(RADIO_RELOAD, param, aircraft);
            Trace_Aircraft_Dock_Service(building, aircraft, i, deadline, dock,
                "SERVICE_RELOAD", "DEADLINE", ammo_before, reply);
            if (reply == RADIO_ROGER) {
                deadline = Frame + delay;
            } else {
                finish("RELOAD_DONE", reply);
            }
        }
    }
    if (!pending) {
        building->Begin_Mode(BSTATE_IDLE);
        building->Assign_Mission(MISSION_GUARD);
    }
    return 1; // Deadlines are Frame-based; later arrivals cannot reset earlier visitors.
}

bool BuildingClassExtension::Validate_Aircraft_Docks_After_Load()
{
    for (int i = 0; i < BuildingTypeExtensions.Count(); ++i) {
        const auto type = BuildingTypeExtensions[i];
        if (!BuildingTypes.Is_Present(type->This()) || type->Aircraft_Docks_Error() != nullptr) return false;
    }
    // First reject structural corruption without dereferencing a saved/foreign pointer.
    for (int b = 0; b < BuildingExtensions.Count(); ++b) {
        const auto ext = BuildingExtensions[b];
        if (!Buildings.Is_Present(ext->This()) || ext->This()->RTTI != RTTI_BUILDING ||
            !BuildingTypes.Is_Present(ext->This()->Class)) return false;
        const int count = Extension::Fetch(ext->This()->Class)->AircraftDockCount;
        if (count > 1 && ext->This()->Radio != nullptr) {
            // A saved under-construction pad may still use its ordinary CY link.
            auto peer = reinterpret_cast<BuildingClass*>(ext->This()->Radio);
            if (!Buildings.Is_Present(peer) || !BuildingTypes.Is_Present(peer->Class) ||
                !peer->Class->IsConstructionYard || peer->Radio != ext->This()) return false;
        }
        for (int i = 0; i < BuildingTypeClassExtension::MAX_AIRCRAFT_DOCKS; ++i) {
            const auto aircraft = ext->AircraftDockVisitors[i];
            if (aircraft == nullptr) continue;
            if (count == 1 || i >= count || ext->This()->Radio != nullptr || !Aircrafts.Is_Present(aircraft) ||
                aircraft->RTTI != RTTI_AIRCRAFT ||
                !AircraftTypes.Is_Present(aircraft->Class) ||
                Extension::Fetch(aircraft->Class)->IsFixedWing || aircraft->Class->IsCarryall) return false;
            for (int p = 0; p <= b; ++p) {
                for (int j = 0; j < (p == b ? i : BuildingTypeClassExtension::MAX_AIRCRAFT_DOCKS); ++j) {
                    if (BuildingExtensions[p]->AircraftDockVisitors[j] == aircraft) return false;
                }
            }
        }
    }
    // Correct stale contacts only after every pointer and cyclic reference was checked.
    for (int b = 0; b < BuildingExtensions.Count(); ++b) {
        const auto ext = BuildingExtensions[b];
        for (int i = 0; i < BuildingTypeClassExtension::MAX_AIRCRAFT_DOCKS; ++i) {
            const auto aircraft = ext->AircraftDockVisitors[i];
            if (aircraft != nullptr && (!aircraft->IsActive || aircraft->IsInLimbo || aircraft->Strength <= 0 ||
                aircraft->Radio != ext->This() || !ext->This()->House->Is_Ally(aircraft))) {
                ext->AircraftDockVisitors[i] = nullptr;
                ext->AircraftDockReloadTimers[i] = -1;
                if (aircraft->Radio == ext->This()) {
                    aircraft->Radio = nullptr;
                    aircraft->IsTethered = false;
                    if (aircraft->NavCom == ext->This()) aircraft->NavCom = nullptr;
                    if (aircraft->IsActive && !aircraft->IsInLimbo &&
                        (aircraft->Mission == MISSION_SLEEP || aircraft->Mission == MISSION_ENTER)) {
                        aircraft->Assign_Mission(MISSION_GUARD);
                    }
                }
            }
        }
    }
    for (int i = 0; i < Aircrafts.Count(); ++i) {
        auto aircraft = Aircrafts[i];
        if (aircraft->Radio != nullptr && Buildings.Is_Present(reinterpret_cast<BuildingClass*>(aircraft->Radio))) {
            const auto ext = Extension::Fetch(static_cast<BuildingClass*>(aircraft->Radio));
            if (ext->Is_Aircraft_MultiDock() && ext->Find_Aircraft_Dock_Slot(aircraft) < 0) return false;
        }
    }
    return true;
}

/**
 *  #issue-26
 * 
 *  The produce cash per-frame update function.
 *  
 *  @author: CCHyper
 */
void BuildingClassExtension::Produce_Cash_AI()
{
    const BuildingClass *this_building = reinterpret_cast<const BuildingClass *>(This());
    const BuildingTypeClass *this_buildingtype = reinterpret_cast<const BuildingTypeClass *>(This()->Class_Of());

    /**
     *  Fetch the extension instance.
     */
    BuildingTypeClassExtension *buildingtypeext = Extension::Fetch(this_buildingtype);

#if 0
    /**
     *  Debugging code.
     * 
     *  Only updates player owned buildings.
     */
    if (This()->House != PlayerPtr) {
        return;
    }
#endif

    /**
     *  Is this building able to produce cash?
     */
    if (!IsBudgetDepleted && buildingtypeext->ProduceCashAmount != 0) {

        /**
         *  Check if this building requires power to produce cash.
         */
        if (this_buildingtype->IsPowered) {

            /**
             *  Stop the timer if the building is offline or has low power.
             */
            if (ProduceCashTimer.Is_Active() && !This()->Is_Powered_On()) {
                ProduceCashTimer.Stop();
            }

            /**
             *  Restart the timer is if it previously stopped due to low power or is offline.
             */
            if (!ProduceCashTimer.Is_Active() && This()->Is_Powered_On()) {
                ProduceCashTimer.Start();
            }

        }

        /**
         *  Are we ready to hand out some cash?
         */
        if (ProduceCashTimer.Is_Active() && ProduceCashTimer.Expired()) {

            /**
             *  Is the owner a passive house? If so, they should not be receiving cash.
             */
            if (!This()->House->Class->IsMultiplayPassive) {

                int amount = buildingtypeext->ProduceCashAmount;

                /**
                 *  Check we have not depleted our budget first.
                 */
                if (CurrentProduceCashBudget > 0) {

                    /**
                     *  Adjust the budget tracker (if one as been set).
                     */
                    if (CurrentProduceCashBudget != -1) {
                        CurrentProduceCashBudget -= std::max<unsigned>(0, std::abs(amount));
                    }

                    /**
                     *  Has the budget been spent?
                     */
                    if (CurrentProduceCashBudget <= 0) {
                        IsBudgetDepleted = true;
                        CurrentProduceCashBudget = -1;
                    }

                }

                /**
                 *  Check if we need to drain cash from the house or provide a bonus.
                 */
                if (!IsBudgetDepleted && amount != 0) {
                    if (amount < 0) {
                        This()->House->Spend_Money(std::abs(amount));
                    } else {
                        This()->House->Refund_Money(amount);
                    }
                }

            }

            /**
             *  Reset the delay timer.
             */
            ProduceCashTimer = buildingtypeext->ProduceCashDelay;

        }

    }

}
