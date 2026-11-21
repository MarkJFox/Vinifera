/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *
 *  @project       Vinifera
 *
 *  @file          SLAVEMANAGER.CPP
 *
 *  @author        Aftermath
 *
 *  @brief         YR-style Slave Miner logic (mobile refinery that seeks
 *                 Tiberium, deploys next to it, harvests and relocates).
 *
 *                 The miner state machine follows SlaveManagerClass in Yuri's
 *                 Revenge (gamemd.exe 0x006AFD60). This first stage implements
 *                 the miner itself (seek, deploy, relocate); slaves come later.
 *
 *  @license       Vinifera is free software: you can redistribute it and/or
 *                 modify it under the terms of the GNU General Public License
 *                 as published by the Free Software Foundation, either version
 *                 3 of the License, or (at your option) any later version.
 *
 *                 Vinifera is distributed in the hope that it will be
 *                 useful, but WITHOUT ANY WARRANTY; without even the implied
 *                 warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *                 PURPOSE. See the GNU General Public License for more details.
 *
 *                 You should have received a copy of the GNU General Public
 *                 License along with this program.
 *                 If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#include "always.h"

#include "slavemanager.h"

#include "building.h"
#include "buildingext.h"
#include "buildingtype.h"
#include "cell.h"
#include "extension.h"
#include "house.h"
#include "mouse.h"
#include "rules.h"
#include "rulesext.h"
#include "technoext.h"
#include "technotypeext.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "unit.h"
#include "unittype.h"
#include "unittypeext.h"
#include "vinifera_globals.h"
#include "vinifera_saveload.h"
#include "wwcrc.h"

#include <climits>
#include <cstdlib>


/**
 *  The manager runs its logic every LOGIC_RATE frames (YR uses 10).
 */
static constexpr int LOGIC_RATE = 10;

/**
 *  Frames to wait before a failed deploy is retried (YR uses 30).
 */
static constexpr int DEPLOY_RETRY_DELAY = 30;

/**
 *  Give up on a site after this many failed attempts and scan again.
 */
static constexpr int MAX_DEPLOY_ATTEMPTS = 3;

/**
 *  Minimum frames between two relocation scans of a deployed miner.
 */
static constexpr int RELOCATION_SCAN_DELAY = 90;

/**
 *  How far (in cells) around the Tiberium the miner looks for a deploy site at
 *  most. The nearest legal site wins, so a larger value only matters for
 *  fields surrounded by slopes or cliffs.
 */
static constexpr int MAX_DEPLOY_SITE_RADIUS = 12;


/**
 *  Approximate distance between two cells, in leptons. Integer only, so the
 *  result is identical on every machine.
 */
static int Cell_Distance(const Cell &a, const Cell &b)
{
    const int dx = std::abs(a.X - b.X);
    const int dy = std::abs(a.Y - b.Y);
    const int hi = dx > dy ? dx : dy;
    const int lo = dx > dy ? dy : dx;
    return (hi + lo / 2) * CELL_LEPTON_W;
}


/**
 *  Finds the nearest Tiberium cell around a center cell. Within the nearest
 *  ring that has Tiberium, the most valuable cell wins. Shroud is ignored on
 *  purpose (it differs between players and would desync multiplayer games).
 */
static Cell Find_Tiberium(const Cell &center, int radius_leptons)
{
    const int radius = radius_leptons / CELL_LEPTON_W;

    for (int ring = 0; ring <= radius; ++ring) {

        Cell best = CELL_NONE;
        int best_value = -1;

        for (int y = -ring; y <= ring; ++y) {
            for (int x = -ring; x <= ring; ++x) {

                /**
                 *  Only the outline of the ring.
                 */
                if (std::abs(x) != ring && std::abs(y) != ring) {
                    continue;
                }

                Cell cell(center.X + x, center.Y + y);
                if (!Map.In_Local_Radar(cell)) {
                    continue;
                }

                CellClass &cellptr = Map[cell];
                if (cellptr.Land_Type() != LAND_TIBERIUM) {
                    continue;
                }

                const int value = cellptr.Get_Tiberium_Value();
                if (value > best_value) {
                    best_value = value;
                    best = cell;
                }
            }
        }

        if (best_value >= 0) {
            return best;
        }
    }

    return CELL_NONE;
}


/**
 *  Finds the cell a vehicle has to stand on to deploy into "buildingtype"
 *  as close as possible to the given Tiberium cell. The vehicle must be able
 *  to reach the cell (same movement zone as "zone_cell").
 *
 *  Slopes, cliffs and other obstacles often block the cells right next to a
 *  field, so the search grows ring by ring up to MAX_DEPLOY_SITE_RADIUS and
 *  always returns the nearest legal site, never just the first one found.
 *  Equally near sites are decided by the distance to "zone_cell" (shorter trip).
 */
static Cell Find_Deploy_Site(BuildingTypeClass *buildingtype, const Cell &ore, const Cell &zone_cell, MZoneType mzone)
{
    if (buildingtype == nullptr || ore == CELL_NONE) {
        return CELL_NONE;
    }

    const bool mobile = buildingtype->Is_Deployable();
    const int zone = Map.Get_Cell_Zone(zone_cell, mzone, false);

    Cell best = CELL_NONE;
    int best_distance = INT_MAX;
    int best_trip = INT_MAX;

    for (int ring = 0; ring <= MAX_DEPLOY_SITE_RADIUS; ++ring) {

        /**
         *  Every cell of this ring is at least "ring" cells away from the
         *  Tiberium; once that is no better than what we have, stop.
         */
        if (ring * CELL_LEPTON_W > best_distance) {
            break;
        }

        for (int y = -ring; y <= ring; ++y) {
            for (int x = -ring; x <= ring; ++x) {

                if (std::abs(x) != ring && std::abs(y) != ring) {
                    continue;
                }

                Cell stand(ore.X + x, ore.Y + y);
                if (!Map.In_Local_Radar(stand)) {
                    continue;
                }

                const int distance = Cell_Distance(stand, ore);
                const int trip = Cell_Distance(stand, zone_cell);
                if (distance > best_distance || (distance == best_distance && trip >= best_trip)) {
                    continue;
                }

                if (Map.Get_Cell_Zone(stand, mzone, false) != zone) {
                    continue;
                }

                /**
                 *  UnitClass::Try_To_Deploy places the building at the north-west
                 *  neighbour of the vehicle, unless it is a mobile deployer.
                 *  Legal_Placement rejects slopes and uneven ground.
                 */
                Cell topleft = mobile ? stand : Adjacent_Cell(stand, FACING_NW);
                if (!buildingtype->Legal_Placement(topleft, nullptr)) {
                    continue;
                }

                best = stand;
                best_distance = distance;
                best_trip = trip;
            }
        }
    }

    return best;
}


/**
 *  Retrieves the class identifier (CLSID) of the object.
 */
IFACEMETHODIMP SlaveManagerClass::GetClassID(CLSID *pClassID)
{
    if (pClassID == nullptr) {
        return E_POINTER;
    }

    *pClassID = __uuidof(this);

    return S_OK;
}


/**
 *  Initializes an object from the stream where it was saved previously.
 */
IFACEMETHODIMP SlaveManagerClass::Load(IStream *pStm)
{
    HRESULT hr = Abstract_Load(pStm);
    if (FAILED(hr)) {
        return hr;
    }

    new (this) SlaveManagerClass(NoInitClass());

    VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(Owner, "Owner");

    return hr;
}


/**
 *  Saves an object to the specified stream.
 */
IFACEMETHODIMP SlaveManagerClass::Save(IStream *pStm, BOOL fClearDirty)
{
    return Abstract_Save(pStm, fClearDirty);
}


/**
 *  Basic constructor, used by the class factory when loading.
 */
SlaveManagerClass::SlaveManagerClass() :
    AbstractClass(),
    Vinifera::Detach::Listener<TechnoClass>(),
    Owner(nullptr),
    Status(SlaveManagerStatus::Idle),
    LogicTimer(0),
    StateTimer(0),
    LastScanFrame(0),
    LastRelocationScanFrame(0),
    DeploySite(CELL_NONE),
    OrderedOre(CELL_NONE),
    DeployAttempts(0),
    HasHarvestOrder(false),
    IsRelocating(false)
{
    SlaveManagers.Add(this);
}


/**
 *  Creates a manager for the given owner.
 */
SlaveManagerClass::SlaveManagerClass(TechnoClass *owner) :
    SlaveManagerClass()
{
    Set_Owner(owner);
}


/**
 *  No-init constructor, restores the vtables after loading.
 */
SlaveManagerClass::SlaveManagerClass(const NoInitClass &noinit) :
    AbstractClass(noinit),
    Vinifera::Detach::Listener<TechnoClass>(noinit),
    LogicTimer(noinit),
    StateTimer(noinit)
{
}


/**
 *  Class destructor.
 */
SlaveManagerClass::~SlaveManagerClass()
{
    SlaveManagers.Delete(this);
}


/**
 *  Returns the RTTI type of the object.
 */
RTTIType SlaveManagerClass::Fetch_RTTI() const
{
    return static_cast<RTTIType>(EXT_RTTI_SLAVE_MANAGER);
}


/**
 *  Returns the size of the object.
 */
int SlaveManagerClass::Get_Object_Size(bool firestorm) const
{
    return sizeof(*this);
}


/**
 *  Computes the CRC of the object.
 */
void SlaveManagerClass::Object_CRC(CRCEngine &crc) const
{
    AbstractClass::Object_CRC(crc);

    crc(static_cast<int>(Status));
    crc(LastScanFrame);
    crc(LastRelocationScanFrame);
    crc(DeploySite.As_Cell_Number());
    crc(OrderedOre.As_Cell_Number());
    crc(DeployAttempts);
    crc(HasHarvestOrder);
    crc(IsRelocating);

    if (Owner != nullptr) {
        crc(Owner->Fetch_Heap_ID());
    }
}


/**
 *  The owner went away; the manager itself is deleted by the owner's extension.
 */
void SlaveManagerClass::On_Detach(TechnoClass *target, bool all)
{
    if (target == Owner) {
        Owner = nullptr;
    }
}


/**
 *  Is this type a Slave Miner, or the deployed form of one?
 */
bool SlaveManagerClass::Is_Slave_Miner_Type(const TechnoTypeClass *type)
{
    if (type == nullptr) {
        return false;
    }

    if (Extension::Fetch(type)->IsSlaveMiner) {
        return true;
    }

    if (type->UndeploysInto != nullptr && Extension::Fetch(type->UndeploysInto)->IsSlaveMiner) {
        return true;
    }

    return false;
}


/**
 *  Hands the manager over when a miner deploys or undeploys. The new object
 *  gets a fresh manager from its extension constructor; that one is replaced
 *  so the state (and later the slaves) survive the conversion.
 */
void SlaveManagerClass::Transfer(TechnoClass *from, TechnoClass *to)
{
    if (from == nullptr || to == nullptr) {
        return;
    }

    TechnoClassExtension *from_ext = Extension::Fetch(from);
    if (from_ext->SlaveManager == nullptr) {
        return;
    }

    TechnoClassExtension *to_ext = Extension::Fetch(to);
    if (to_ext->SlaveManager != nullptr) {
        delete to_ext->SlaveManager;
    }

    to_ext->SlaveManager = from_ext->SlaveManager;
    from_ext->SlaveManager = nullptr;

    to_ext->SlaveManager->Set_Owner(to);
}


/**
 *  Should this building undeploy instead of being sold? TS only undeploys
 *  mobile deployers (or MCVs with MCVRedeploy); a Slave Miner with a target
 *  to move to always undeploys.
 */
bool SlaveManagerClass::Should_Undeploy(const BuildingClass *building)
{
    if (building == nullptr || building->Class->UndeploysInto == nullptr) {
        return false;
    }

    return Extension::Fetch(building)->SlaveManager != nullptr && building->ArchiveTarget != nullptr;
}


/**
 *  Can the player send this object to a Tiberium field (harvest cursor)?
 *  Vehicles must be able to deploy, buildings to undeploy.
 */
bool SlaveManagerClass::Accepts_Harvest_Order(const TechnoClass *techno)
{
    if (techno == nullptr || Extension::Fetch(techno)->SlaveManager == nullptr) {
        return false;
    }

    if (techno->RTTI == RTTI_UNIT) {
        return static_cast<const UnitClass *>(techno)->Class->DeploysInto != nullptr;
    }

    if (techno->RTTI == RTTI_BUILDING) {
        const BuildingClass *building = static_cast<const BuildingClass *>(techno);
        const MissionType mission = building->Get_Mission();
        return building->Class->UndeploysInto != nullptr && building->IsDown
            && mission != MISSION_CONSTRUCTION && mission != MISSION_DECONSTRUCTION;
    }

    return false;
}


/**
 *  Is this cell a valid target for a harvest order (Tiberium, not hidden under a bridge)?
 */
bool SlaveManagerClass::Is_Harvest_Order_Cell(const Cell &cell)
{
    if (!Map.In_Local_Radar(cell)) {
        return false;
    }

    CellClass &cellptr = Map[cell];
    return cellptr.Land_Type() == LAND_TIBERIUM && !cellptr.Is_Bridge_Here();
}


/**
 *  Sets the owner and the matching state.
 */
void SlaveManagerClass::Set_Owner(TechnoClass *owner)
{
    Owner = owner;
    LogicTimer = 0;
    DeployAttempts = 0;

    if (Owner == nullptr) {
        return;
    }

    if (Owner->RTTI == RTTI_BUILDING) {

        /**
         *  Deployed (or built directly): start working.
         */
        IsRelocating = false;
        OrderedOre = CELL_NONE;
        LastRelocationScanFrame = Frame;
        Set_Status(SlaveManagerStatus::Working);

    } else if (Owner->RTTI == RTTI_UNIT) {

        UnitClass *unit = static_cast<UnitClass *>(Owner);

        /**
         *  Undeployed. If we packed up to relocate, drive to the new site,
         *  otherwise the player undeployed us: the harvest order ends and we wait.
         */
        if (IsRelocating && DeploySite != CELL_NONE) {
            IsRelocating = false;
            unit->Assign_Destination(&Map[DeploySite]);
            unit->Assign_Mission(MISSION_MOVE);
            unit->Commence();
            Set_Status(SlaveManagerStatus::Travelling);
        } else {
            Stop();
        }

    } else {
        Set_Status(SlaveManagerStatus::Idle);
    }
}


/**
 *  Stops the automatic behaviour until the miner is woken up again.
 */
void SlaveManagerClass::Stop()
{
    IsRelocating = false;
    HasHarvestOrder = false;
    OrderedOre = CELL_NONE;
    DeployAttempts = 0;
    Set_Status(SlaveManagerStatus::Idle);
}


/**
 *  Changes the state and keeps LastScanFrame like YR (now when idle, INT_MAX when busy).
 */
void SlaveManagerClass::Set_Status(SlaveManagerStatus status)
{
    Status = status;
    LastScanFrame = (status == SlaveManagerStatus::Idle) ? Frame : INT_MAX;
}


UnitClass *SlaveManagerClass::Owner_Unit() const
{
    return (Owner != nullptr && Owner->RTTI == RTTI_UNIT) ? static_cast<UnitClass *>(Owner) : nullptr;
}


BuildingClass *SlaveManagerClass::Owner_Building() const
{
    return (Owner != nullptr && Owner->RTTI == RTTI_BUILDING) ? static_cast<BuildingClass *>(Owner) : nullptr;
}


/**
 *  The type extension holding the Slave Miner settings: the owner's type, or
 *  the vehicle/building counterpart if only that one is flagged.
 */
const TechnoTypeClassExtension *SlaveManagerClass::Config() const
{
    const TechnoTypeClass *type = Owner->TClass;
    const TechnoTypeClassExtension *ext = Extension::Fetch(type);

    if (!ext->IsSlaveMiner) {
        const TechnoTypeClass *other = nullptr;
        if (Owner->RTTI == RTTI_BUILDING) {
            other = type->UndeploysInto;
        } else {
            other = type->DeploysInto;
        }
        if (other != nullptr && Extension::Fetch(other)->IsSlaveMiner) {
            return Extension::Fetch(other);
        }
    }

    return ext;
}


int SlaveManagerClass::Short_Scan() const
{
    const int value = Config()->SlaveMinerShortScan;
    if (value >= 0) return value;
    if (RuleExtension->SlaveMinerShortScan >= 0) return RuleExtension->SlaveMinerShortScan;
    return Rule->TiberiumShortScan;
}


int SlaveManagerClass::Long_Scan() const
{
    const int value = Config()->SlaveMinerLongScan;
    if (value >= 0) return value;
    if (RuleExtension->SlaveMinerLongScan >= 0) return RuleExtension->SlaveMinerLongScan;
    return Rule->TiberiumLongScan;
}


int SlaveManagerClass::Scan_Correction() const
{
    const int value = Config()->SlaveMinerScanCorrection;
    return value >= 0 ? value : RuleExtension->SlaveMinerScanCorrection;
}


int SlaveManagerClass::Kick_Frame_Delay() const
{
    const int value = Config()->SlaveMinerKickFrameDelay;
    return value >= 0 ? value : RuleExtension->SlaveMinerKickFrameDelay;
}


int SlaveManagerClass::Harvest_Radius() const
{
    const int value = Config()->SlaveMinerHarvestRadius;
    return value >= 0 ? value : Short_Scan();
}


/**
 *  Main logic, called from the owner's AI.
 */
void SlaveManagerClass::AI()
{
    if (Owner == nullptr || !Owner->IsActive || Owner->IsInLimbo) {
        return;
    }

    /**
     *  The player (or a script) gave a harvest order. Handled right away so the
     *  vehicle does not first drive onto the clicked cell.
     */
    if (Owner->Get_Mission() == MISSION_HARVEST) {
        Handle_Harvest_Order();
        LogicTimer = LOGIC_RATE;
        return;
    }

    if (!LogicTimer.Expired()) {
        return;
    }

    LogicTimer = LOGIC_RATE;

    switch (Status) {
        case SlaveManagerStatus::Idle:        AI_Idle(); break;
        case SlaveManagerStatus::Scanning:    AI_Scanning(); break;
        case SlaveManagerStatus::Travelling:  AI_Travelling(); break;
        case SlaveManagerStatus::DeployRetry: AI_Deploy_Retry(); break;
        case SlaveManagerStatus::Deploying:   AI_Deploying(); break;
        case SlaveManagerStatus::Working:     AI_Working(); break;
        case SlaveManagerStatus::PackingUp:   AI_Packing_Up(); break;
        default: Stop(); break;
    }
}


/**
 *  Idle: a building starts working, a vehicle waits to be woken up.
 */
void SlaveManagerClass::AI_Idle()
{
    BuildingClass *building = Owner_Building();
    if (building != nullptr) {
        const MissionType mission = building->Get_Mission();
        if (mission != MISSION_CONSTRUCTION && mission != MISSION_DECONSTRUCTION) {
            Set_Status(SlaveManagerStatus::Working);
        }
        return;
    }

    /**
     *  Woken up (computer, or KickFrameDelay for humans): works like a harvest order.
     */
    if (Owner_Unit() != nullptr && Should_Wake_Up()) {
        HasHarvestOrder = true;
        Set_Status(SlaveManagerStatus::Scanning);
    }
}


/**
 *  Harvest order (YR: UnitClass/BuildingClass::Mission_Harvest -> 0x006B0DB0).
 *  The event system stores the clicked cell as the destination of a vehicle
 *  and as the archive target of a building (EVENT_ARCHIVE, sent by the
 *  BuildingClass::Active_Click_With hook just before the mission event).
 */
void SlaveManagerClass::Handle_Harvest_Order()
{
    UnitClass *unit = Owner_Unit();
    if (unit != nullptr) {

        Cell ore = CELL_NONE;
        if (unit->NavCom != nullptr) {
            ore = Cell(unit->NavCom->Center_Coord());
            unit->Assign_Destination(nullptr);
        }

        Stop();
        OrderedOre = ore;
        HasHarvestOrder = true;

        /**
         *  Leave the harvest mission in any case; Scanning gives the move order.
         */
        unit->Assign_Mission(MISSION_GUARD);
        unit->Commence();
        Set_Status(SlaveManagerStatus::Scanning);
        AI_Scanning();
        return;
    }

    BuildingClass *building = Owner_Building();
    if (building != nullptr) {

        AbstractClass *target = building->ArchiveTarget;
        building->ArchiveTarget = nullptr;
        HasHarvestOrder = true;

        if (target != nullptr && Status == SlaveManagerStatus::Working) {
            const Cell ore(target->Center_Coord());
            const Cell center(building->Center_Coord());

            /**
             *  Already within reach of the clicked field: stay.
             */
            if (Cell_Distance(center, ore) > Harvest_Radius() && Relocate(building, ore, false)) {
                return;
            }
        }

        building->Assign_Mission(MISSION_GUARD);
        building->Commence();
    }
}


/**
 *  Equivalent of SlaveManagerClass::ShouldWakeUpNow (gamemd.exe 0x006B1020),
 *  called by YR from the guard missions.
 */
bool SlaveManagerClass::Should_Wake_Up() const
{
    UnitClass *unit = Owner_Unit();
    if (unit == nullptr) {
        return false;
    }

    /**
     *  Only a vehicle that sits around doing nothing is woken up.
     */
    const MissionType mission = unit->Get_Mission();
    if (mission != MISSION_GUARD && mission != MISSION_GUARD_AREA) {
        return false;
    }

    if (unit->NavCom != nullptr || unit->IsDeploying || unit->Locomotion->Is_Moving()) {
        return false;
    }

    const int delay = Kick_Frame_Delay();

    /**
     *  The computer always works. YR scans every logic tick; we wait the kick
     *  delay after a failed scan so idle miners do not long-scan constantly.
     */
    if (!unit->House->Is_Human_Player()) {
        return Frame > LastScanFrame + (delay > 0 ? delay : 0);
    }

    if (delay < 0) {
        return false;
    }

    if (Map[unit->Get_Cell()].Land_Type() == LAND_TIBERIUM) {
        return true;
    }

    if (Frame > LastScanFrame + delay) {
        return Find_Tiberium(unit->Get_Cell(), Short_Scan()) != CELL_NONE;
    }

    return false;
}


/**
 *  Scanning: find Tiberium and a deploy site, then drive there.
 */
void SlaveManagerClass::AI_Scanning()
{
    UnitClass *unit = Owner_Unit();
    if (unit == nullptr || unit->Class->DeploysInto == nullptr) {
        Stop();
        return;
    }

    const Cell here = unit->Get_Cell();

    /**
     *  Sent by the player: the clicked field (or what is left of it nearby).
     *  Otherwise, or if that field is gone, look around the vehicle.
     */
    Cell ore = CELL_NONE;
    if (OrderedOre != CELL_NONE) {
        ore = Find_Tiberium(OrderedOre, Short_Scan());
    }
    if (ore == CELL_NONE) {
        ore = Find_Tiberium(here, Long_Scan());
    }

    /**
     *  Lift the vehicle off the map while searching, so the cells it stands on
     *  count as free (it can deploy right where it is).
     */
    unit->Mark(MARK_UP);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_UP);
    const Cell site = Find_Deploy_Site(unit->Class->DeploysInto, ore, here, unit->Class->MZone);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_DOWN);
    unit->Mark(MARK_DOWN);

    if (site == CELL_NONE) {

        /**
         *  No room to deploy at the ordered field: just drive there.
         */
        const Cell ordered = OrderedOre;
        Stop();
        if (ordered != CELL_NONE && ordered != here) {
            unit->Assign_Destination(&Map[ordered]);
            unit->Assign_Mission(MISSION_MOVE);
            unit->Commence();
        }
        return;
    }

    DeploySite = site;

    if (site == here) {
        if (Start_Deploy()) {
            if (Owner_Unit() != nullptr) {
                Set_Status(SlaveManagerStatus::Deploying);
            }
        } else {
            StateTimer = DEPLOY_RETRY_DELAY;
            Set_Status(SlaveManagerStatus::DeployRetry);
        }
        return;
    }

    unit->Assign_Destination(&Map[site]);
    unit->Assign_Mission(MISSION_MOVE);
    unit->Commence();

    Set_Status(SlaveManagerStatus::Travelling);
}


/**
 *  Travelling: wait for arrival, then deploy. Any other order cancels the trip.
 */
void SlaveManagerClass::AI_Travelling()
{
    UnitClass *unit = Owner_Unit();
    if (unit == nullptr) {
        Stop();
        return;
    }

    const MissionType mission = unit->Get_Mission();
    if (mission != MISSION_MOVE && mission != MISSION_GUARD && mission != MISSION_GUARD_AREA) {
        Stop();
        return;
    }

    if (unit->NavCom != nullptr) {
        if (unit->NavCom != &Map[DeploySite]) {
            Stop();
        }
        return;
    }

    if (unit->Locomotion->Is_Moving()) {
        return;
    }

    if (unit->Get_Cell() != DeploySite) {

        /**
         *  Could not reach the site; look again.
         */
        if (++DeployAttempts >= MAX_DEPLOY_ATTEMPTS) {
            Stop();
        } else {
            Set_Status(SlaveManagerStatus::Scanning);
        }
        return;
    }

    if (Start_Deploy()) {
        if (Owner_Unit() != nullptr) {
            Set_Status(SlaveManagerStatus::Deploying);
        }
    } else {
        StateTimer = DEPLOY_RETRY_DELAY;
        Set_Status(SlaveManagerStatus::DeployRetry);
    }
}


/**
 *  DeployRetry: wait a moment, then try once more; otherwise scan again.
 */
void SlaveManagerClass::AI_Deploy_Retry()
{
    UnitClass *unit = Owner_Unit();
    if (unit == nullptr) {
        Stop();
        return;
    }

    if (Was_Given_Other_Order(unit)) {
        Stop();
        return;
    }

    if (!StateTimer.Expired()) {
        return;
    }

    if (Start_Deploy()) {
        if (Owner_Unit() != nullptr) {
            Set_Status(SlaveManagerStatus::Deploying);
        }
        return;
    }

    if (++DeployAttempts >= MAX_DEPLOY_ATTEMPTS) {
        Stop();
    } else {
        Set_Status(SlaveManagerStatus::Scanning);
    }
}


/**
 *  Deploying: the vehicle is rotating into its deploy facing. Once the
 *  building exists, Transfer() moves us over and we are Working.
 */
void SlaveManagerClass::AI_Deploying()
{
    if (Owner_Building() != nullptr) {
        Set_Status(SlaveManagerStatus::Working);
        return;
    }

    UnitClass *unit = Owner_Unit();
    if (unit == nullptr) {
        Stop();
        return;
    }

    if (unit->IsDeploying) {
        return;
    }

    /**
     *  The deploy was aborted. By a player order: stop; otherwise try again.
     */
    if (Was_Given_Other_Order(unit)) {
        Stop();
        return;
    }

    StateTimer = DEPLOY_RETRY_DELAY;
    Set_Status(SlaveManagerStatus::DeployRetry);
}


/**
 *  Working: stay deployed, and relocate once the field is depleted.
 */
void SlaveManagerClass::AI_Working()
{
    BuildingClass *building = Owner_Building();
    if (building == nullptr) {
        Stop();
        return;
    }

    const MissionType mission = building->Get_Mission();

    /**
     *  Sold or undeployed by the player.
     */
    if (mission == MISSION_DECONSTRUCTION) {
        IsRelocating = false;
        Status = SlaveManagerStatus::PackingUp;
        LastScanFrame = INT_MAX;
        return;
    }

    if (mission == MISSION_CONSTRUCTION || !building->IsDown) {
        return;
    }

    /**
     *  The miner itself produces no income; that only comes from collectors
     *  unloading at it, like at a refinery (slaves, later stage).
     *
     *  Without a harvest order (deployed by hand) the miner stays where it is.
     *  The computer always relocates.
     */
    if (HasHarvestOrder || !building->House->Is_Human_Player()) {
        Check_Relocation(building);
    }
}


/**
 *  Relocation check of UpdateMiner state 5: if no Tiberium is left within the
 *  short scan, look further and move if the new site is clearly better.
 */
void SlaveManagerClass::Check_Relocation(BuildingClass *building)
{
    UnitTypeClass *unittype = building->Class->UndeploysInto;
    if (unittype == nullptr || unittype->DeploysInto == nullptr) {
        return;
    }

    if (Frame < LastRelocationScanFrame + RELOCATION_SCAN_DELAY) {
        return;
    }

    LastRelocationScanFrame = Frame;

    const Cell center(building->Center_Coord());
    if (Find_Tiberium(center, Short_Scan()) != CELL_NONE) {
        return;
    }

    const Cell ore = Find_Tiberium(center, Long_Scan());
    if (ore == CELL_NONE) {
        return;
    }

    Relocate(building, ore, true);
}


/**
 *  Packs the building up to move next to the given Tiberium cell. With
 *  "check_correction" (automatic relocation) only if the new site is closer
 *  to the Tiberium by more than ScanCorrection.
 */
bool SlaveManagerClass::Relocate(BuildingClass *building, const Cell &ore, bool check_correction)
{
    UnitTypeClass *unittype = building->Class->UndeploysInto;
    if (unittype == nullptr || unittype->DeploysInto == nullptr || ore == CELL_NONE) {
        return false;
    }

    const Cell center(building->Center_Coord());

    /**
     *  The vehicle appears south-east of the building's origin when undeploying.
     */
    const Cell exit_cell = Adjacent_Cell(building->Get_Cell(), FACING_SE);
    const Cell site = Find_Deploy_Site(unittype->DeploysInto, ore, exit_cell, unittype->MZone);
    if (site == CELL_NONE) {
        return false;
    }

    if (check_correction && Cell_Distance(site, ore) + Scan_Correction() >= Cell_Distance(center, ore)) {
        return false;
    }

    DeploySite = site;
    IsRelocating = true;

    building->Assign_Archive_Target(&Map[site]);
    building->Assign_Mission(MISSION_DECONSTRUCTION);
    building->Commence();

    Status = SlaveManagerStatus::PackingUp;
    LastScanFrame = INT_MAX;

    return true;
}


/**
 *  PackingUp: wait until the building has turned back into the vehicle
 *  (Transfer() takes over), or resume if the undeploy was cancelled.
 */
void SlaveManagerClass::AI_Packing_Up()
{
    BuildingClass *building = Owner_Building();
    if (building == nullptr) {
        if (Owner_Unit() == nullptr) {
            Stop();
        }
        return;
    }

    if (building->Get_Mission() != MISSION_DECONSTRUCTION) {
        IsRelocating = false;
        Set_Status(SlaveManagerStatus::Working);
    }
}


/**
 *  Did the vehicle get another order (move, attack, stop ...) while we wait
 *  to deploy? We do not give it a destination in these states.
 */
bool SlaveManagerClass::Was_Given_Other_Order(const UnitClass *unit) const
{
    if (unit->NavCom != nullptr) {
        return true;
    }

    const MissionType mission = unit->Get_Mission();
    return mission != MISSION_GUARD && mission != MISSION_GUARD_AREA && mission != MISSION_UNLOAD && mission != MISSION_MOVE;
}


/**
 *  Deploys the vehicle where it stands. Pre-checks the placement so a failed
 *  attempt does not make EVA complain to the player.
 */
bool SlaveManagerClass::Start_Deploy()
{
    UnitClass *unit = Owner_Unit();
    if (unit == nullptr || unit->Class->DeploysInto == nullptr) {
        return false;
    }

    BuildingTypeClass *buildingtype = unit->Class->DeploysInto;
    Cell cell = buildingtype->Is_Deployable() ? unit->Get_Cell() : Adjacent_Cell(unit->Get_Cell(), FACING_NW);

    unit->Mark(MARK_UP);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_UP);
    const bool legal = buildingtype->Legal_Placement(cell, nullptr);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_DOWN);
    unit->Mark(MARK_DOWN);

    if (!legal) {
        buildingtype->Flush_For_Placement(cell, unit->House);
        return false;
    }

    return unit->Try_To_Deploy();
}
