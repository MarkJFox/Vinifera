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
 *                 Revenge (gamemd.exe 0x006AFD60), the slaves follow
 *                 UpdateSlaves (0x006AF6C0): they harvest nearby Tiberium and
 *                 enter the deployed miner to unload it.
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
#include "debughandler.h"
#include "extension.h"
#include "house.h"
#include "infantry.h"
#include "infantrytype.h"
#include "infantrytypeext.h"
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
#include <cstdio>
#include <cstdarg>
#include <cstring>


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
 *  Frames after deploying before the miner checks for a move, so its slaves
 *  work at least one round first.
 */
static constexpr int RELOCATION_SETTLE_DELAY = 600;

/**
 *  How far (in cells) around the Tiberium the miner looks for a deploy site at
 *  most. The nearest legal site wins, so a larger value only matters for
 *  fields surrounded by slopes or cliffs.
 */
static constexpr int MAX_DEPLOY_SITE_RADIUS = 12;

/**
 *  Slaves waiting for the undeployed miner keep these gaps (in cells) to the
 *  area its building will cover: closer than KEEP_OUT they step away, within
 *  WAIT they stop, beyond FOLLOW they walk towards it again.
 */
static constexpr int SLAVE_KEEP_OUT_GAP = 1;
static constexpr int SLAVE_WAIT_GAP = 3;
static constexpr int SLAVE_FOLLOW_GAP = 5;

/**
 *  Frames a returning slave may go without getting closer to the door before
 *  it searches a new path, or enters from where it stands at the building.
 */
static constexpr int SLAVE_STUCK_DELAY = 75;

/**
 *  Frames a returning slave right next to the door may walk without stepping
 *  in before it enters from where it stands.
 */
static constexpr int SLAVE_DOOR_DELAY = 30;

/**
 *  Frames a walking slave may go without getting closer to its Tiberium
 *  target before it gives up (blocked by other infantry).
 */
static constexpr int SLAVE_MOVE_STUCK_DELAY = 150;

/**
 *  After this many stuck delays in a row, a returning slave within
 *  SLAVE_STUCK_ENTER_GAP cells of the deployed miner enters it from where it
 *  stands.
 */
static constexpr int SLAVE_STUCK_ENTER_COUNT = 3;
static constexpr int SLAVE_STUCK_ENTER_GAP = 3;

/**
 *  Walks to the door that end without a path before a slave enters from
 *  where it is (within its search radius).
 */
static constexpr int SLAVE_PATH_FAIL_COUNT = 3;

/**
 *  Frames between two wide searches for Tiberium once the remembered field
 *  is used up.
 */
static constexpr int LAST_ORE_SCAN_DELAY = 450;

/**
 *  Slave search radius when neither the type nor [General] sets one (cells, like YR).
 */
static constexpr int DEFAULT_SLAVE_SCAN = 14;


/**
 *  Frames between two overview lines per miner in the test log.
 */
static constexpr int LOG_SNAPSHOT_DELAY = 150;


/**
 *  Test log in the normal debug log, written only in developer mode. It only
 *  reads game state, so it cannot change the game or desync it.
 */
static bool SlaveLogMuted = false;

static bool Slave_Log_Enabled()
{
    return Vinifera_DeveloperMode && !SlaveLogMuted;
}

static void Slave_Log(const char *fmt, ...)
{
    if (!Slave_Log_Enabled()) {
        return;
    }

    char buffer[1024];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    DEBUG_INFO("[SlaveMiner] {:7d} | {}\n", static_cast<int>(Frame), buffer);
}

/**
 *  One cell for the test log: the five infantry spots (C NW NE SW SE, 1 = taken),
 *  the vehicle, immovable and building flags, what stands there, and the height.
 */
static void Describe_Cell(const Cell &cell, char *buffer, int size)
{
    if (!Map.In_Radar(cell)) {
        std::snprintf(buffer, size, "%d,%d outside the map", cell.X, cell.Y);
        return;
    }

    CellClass &cellptr = Map[cell];
    const unsigned bits = cellptr.Flag.Composite;

    int infantry = 0;
    int other = 0;
    for (ObjectClass *object = cellptr.Cell_Occupier(); object != nullptr; object = object->Next) {
        if (object->RTTI == RTTI_INFANTRY) {
            ++infantry;
        } else {
            ++other;
        }
    }

    BuildingClass *building = cellptr.Cell_Building();

    std::snprintf(buffer, size, "%d,%d spots=%d%d%d%d%d vehicle=%d monolith=%d building=%d(%s) infantry=%d other=%d height=%d ramp=%d",
        cell.X, cell.Y,
        (bits >> 0) & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1, (bits >> 4) & 1,
        (bits >> 5) & 1, (bits >> 6) & 1, (bits >> 7) & 1,
        building != nullptr ? building->Class->Name() : "-",
        infantry, other, cellptr.Height, cellptr.Ramp);
}


static const char *Slave_Status_Name(SlaveStatus status)
{
    switch (status) {
        case SlaveStatus::InMiner:    return "InMiner";
        case SlaveStatus::Seeking:    return "Seeking";
        case SlaveStatus::Moving:     return "Moving";
        case SlaveStatus::Harvesting: return "Harvesting";
        case SlaveStatus::Returning:  return "Returning";
        case SlaveStatus::Reloading:  return "Reloading";
        case SlaveStatus::Dead:       return "Dead";
        case SlaveStatus::Ordered:    return "Ordered";
        default:                      return "?";
    }
}

static const char *Manager_Status_Name(SlaveManagerStatus status)
{
    switch (status) {
        case SlaveManagerStatus::Idle:        return "Idle";
        case SlaveManagerStatus::Scanning:    return "Scanning";
        case SlaveManagerStatus::Travelling:  return "Travelling";
        case SlaveManagerStatus::DeployRetry: return "DeployRetry";
        case SlaveManagerStatus::Deploying:   return "Deploying";
        case SlaveManagerStatus::Working:     return "Working";
        case SlaveManagerStatus::PackingUp:   return "PackingUp";
        default:                              return "?";
    }
}


/**
 *  Cells between a cell and a rectangle of cells (0 inside it).
 */
static int Area_Gap(const Cell &topleft, const Cell &bottomright, const Cell &cell)
{
    int dx = 0;
    if (cell.X < topleft.X) dx = topleft.X - cell.X;
    else if (cell.X > bottomright.X) dx = cell.X - bottomright.X;

    int dy = 0;
    if (cell.Y < topleft.Y) dy = topleft.Y - cell.Y;
    else if (cell.Y > bottomright.Y) dy = cell.Y - bottomright.Y;

    return dx > dy ? dx : dy;
}


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
 *  With a zone (>= 0) only cells in that movement zone count, and cells next
 *  to "avoid" are skipped.
 */
static Cell Find_Tiberium(const Cell &center, int radius_leptons, int zone = -1, MZoneType mzone = MZONE_NORMAL, Cell avoid = CELL_NONE)
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

                if (avoid != CELL_NONE && std::abs(cell.X - avoid.X) <= 1 && std::abs(cell.Y - avoid.Y) <= 1) {
                    continue;
                }

                if (zone >= 0 && Map.Get_Cell_Zone(cell, mzone, false) != zone) {
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
 *  Test log only: why sites closer to the Tiberium than the chosen one were
 *  rejected. Filled by Find_Deploy_Site, no influence on the result.
 */
struct DeploySiteStats
{
    int Zone = 0;       // Vehicle cannot drive there.
    int Tiberium = 0;   // Footprint on Tiberium.
    int Slope = 0;      // Footprint on a slope.
    int Uneven = 0;     // Footprint cells of different height.
    int Occupied = 0;   // Units, infantry, buildings or terrain on the footprint.
    int Placement = 0;  // Legal_Placement failed for another reason.
    int DoorBuilding = 0;
    int DoorBlocked = 0;
    int DoorZone = 0;
    int OtherDoor = 0;
};


/**
 *  Best guess why Legal_Placement rejected the footprint at "topleft".
 */
static void Count_Placement_Reason(const BuildingTypeClass *buildingtype, const Cell &topleft, DeploySiteStats &stats)
{
    const int width = buildingtype->Width();
    const int height = buildingtype->Height();
    bool tiberium = false, slope = false, uneven = false, occupied = false;
    int first_height = -1;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Cell cell(topleft.X + x, topleft.Y + y);
            if (!Map.In_Local_Radar(cell)) {
                continue;
            }
            CellClass &cellptr = Map[cell];
            if (cellptr.Land_Type() == LAND_TIBERIUM) tiberium = true;
            if (cellptr.Ramp != 0) slope = true;
            if (first_height < 0) first_height = cellptr.Height;
            else if (cellptr.Height != first_height) uneven = true;
            if (cellptr.Cell_Occupier() != nullptr) occupied = true;
        }
    }

    if (tiberium) ++stats.Tiberium;
    else if (slope) ++stats.Slope;
    else if (uneven) ++stats.Uneven;
    else if (occupied) ++stats.Occupied;
    else ++stats.Placement;
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
static Cell Find_Deploy_Site(const SlaveManagerClass *manager, BuildingTypeClass *buildingtype, const Cell &ore, const Cell &zone_cell, MZoneType mzone, DeploySiteStats *stats = nullptr)
{
    DeploySiteStats dummy;
    DeploySiteStats &st = stats != nullptr ? *stats : dummy;

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
                    ++st.Zone;
                    continue;
                }

                /**
                 *  UnitClass::Try_To_Deploy places the building at the north-west
                 *  neighbour of the vehicle, unless it is a mobile deployer.
                 *  Legal_Placement rejects slopes and uneven ground.
                 */
                Cell topleft = mobile ? stand : Adjacent_Cell(stand, FACING_NW);
                if (!buildingtype->Legal_Placement(topleft, nullptr)) {
                    if (stats != nullptr) {
                        Count_Placement_Reason(buildingtype, topleft, st);
                    }
                    continue;
                }

                const char *door_reason = nullptr;
                if (manager != nullptr && !manager->Is_Door_Site_Clear(buildingtype, topleft, ore, &door_reason)) {
                    if (door_reason == nullptr) {}
                    else if (std::strcmp(door_reason, "door building") == 0) ++st.DoorBuilding;
                    else if (std::strcmp(door_reason, "door blocked") == 0) ++st.DoorBlocked;
                    else if (std::strcmp(door_reason, "door zone") == 0) ++st.DoorZone;
                    else ++st.OtherDoor;
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

    int count = 0;
    hr = pStm->Read(&count, sizeof(count), nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    new (&Slaves) DynamicVectorClass<SlaveNode *>();

    for (int index = 0; index < count; ++index) {
        SlaveNode *node = new SlaveNode;
        hr = pStm->Read(node, sizeof(SlaveNode), nullptr);
        if (FAILED(hr)) {
            delete node;
            return hr;
        }
        Slaves.Add(node);
    }

    for (int index = 0; index < Slaves.Count(); ++index) {
        VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(Slaves[index]->Slave, "Slave");
    }

    VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(Owner, "Owner");

    IsLimboingSlave = false;
    LastReleaseBlocker = nullptr;

    return hr;
}


/**
 *  Saves an object to the specified stream.
 */
IFACEMETHODIMP SlaveManagerClass::Save(IStream *pStm, BOOL fClearDirty)
{
    HRESULT hr = Abstract_Save(pStm, fClearDirty);
    if (FAILED(hr)) {
        return hr;
    }

    const int count = Slaves.Count();
    hr = pStm->Write(&count, sizeof(count), nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    for (int index = 0; index < count; ++index) {
        hr = pStm->Write(Slaves[index], sizeof(SlaveNode), nullptr);
        if (FAILED(hr)) {
            return hr;
        }
    }

    return hr;
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
    IsRelocating(false),
    Slaves(),
    IsSlavesCreated(false),
    IsLimboingSlave(false),
    IsAbsorbPending(false),
    LastOre(CELL_NONE),
    LastOreScanFrame(-1),
    LastReleaseBlocker(nullptr),
    LastSnapshotFrame(-LOG_SNAPSHOT_DELAY),
    LastCellLogFrame(-LOG_SNAPSHOT_DELAY)
{
    SlaveManagers.Add(this);
}


/**
 *  Creates a manager for the given owner.
 */
SlaveManagerClass::SlaveManagerClass(TechnoClass *owner) :
    SlaveManagerClass()
{
    /**
     *  Not logged: a deployed or undeployed miner gets this manager only for a
     *  moment, Transfer() replaces it with the existing one.
     */
    SlaveLogMuted = true;
    Set_Owner(owner);
    SlaveLogMuted = false;
}


/**
 *  No-init constructor, restores the vtables after loading.
 */
SlaveManagerClass::SlaveManagerClass(const NoInitClass &noinit) :
    AbstractClass(noinit),
    Vinifera::Detach::Listener<TechnoClass>(noinit),
    LogicTimer(noinit),
    StateTimer(noinit),
    Slaves(noinit)
{
}


/**
 *  Class destructor.
 */
SlaveManagerClass::~SlaveManagerClass()
{
    if (GameActive) {
        Free_Slaves();
    }

    for (int index = Slaves.Count() - 1; index >= 0; --index) {
        delete Slaves[index];
    }
    Slaves.Clear();

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
    crc(IsSlavesCreated);
    crc(IsAbsorbPending);
    crc(LastOre.X);
    crc(LastOre.Y);
    crc(Slaves.Count());

    for (int index = 0; index < Slaves.Count(); ++index) {
        const SlaveNode *node = Slaves[index];
        crc(static_cast<int>(node->Status));
        crc(node->Target.As_Cell_Number());
        if (node->Slave != nullptr) {
            crc(node->Slave->Fetch_Heap_ID());
        }
    }

    if (Owner != nullptr) {
        crc(Owner->Fetch_Heap_ID());
    }
}


/**
 *  The owner went away; the manager itself is deleted by the owner's extension.
 */
void SlaveManagerClass::On_Detach(TechnoClass *target, bool all)
{
    if (target == nullptr) {
        return;
    }

    if (target == Owner) {
        Owner = nullptr;
    }

    /**
     *  A slave is gone (killed or removed). The manager's own Limbo() call
     *  when a slave enters the miner is not a loss.
     */
    if (!IsLimboingSlave && target->RTTI == RTTI_INFANTRY && (all || target->Strength <= 0)) {
        Slave_Lost(static_cast<InfantryClass *>(target));
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
 *  May this deployed Slave Miner be repaired although it undeploys into a
 *  vehicle? Read from the building type, or from the vehicle if only that
 *  one is flagged as a Slave Miner.
 */
bool SlaveManagerClass::Is_Repairable(const BuildingClass *building)
{
    if (building == nullptr || !Is_Slave_Miner_Type(building->Class)) {
        return false;
    }

    const TechnoTypeClassExtension *ext = Extension::Fetch(building->Class);
    if (!ext->IsSlaveMiner && building->Class->UndeploysInto != nullptr) {
        ext = Extension::Fetch(building->Class->UndeploysInto);
    }

    return ext->IsSlaveMinerRepairable;
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
    if (owner != nullptr) {
        const Cell cell = owner->Get_Cell();
        Slave_Log("M%-4d |       | owner | %s %s at %d,%d", Manager_Index(),
            owner->RTTI == RTTI_BUILDING ? "building" : "vehicle", owner->TClass->Name(), cell.X, cell.Y);
    }
    Owner = owner;
    LogicTimer = 0;
    DeployAttempts = 0;

    /**
     *  The door moved with the miner: progress towards the old one no longer counts.
     */
    for (int index = 0; index < Slaves.Count(); ++index) {
        SlaveNode *node = Slaves[index];
        node->BestDistance = INT_MAX;
        node->StuckCount = 0;
        node->BadOre = CELL_NONE;
        node->PathFails = 0;
        if (node->Status == SlaveStatus::Returning) {
            node->Timer = SLAVE_STUCK_DELAY;
        }
    }

    if (Owner == nullptr) {
        return;
    }

    if (Owner->RTTI == RTTI_BUILDING) {

        /**
         *  Deployed (or built directly): start working.
         */
        IsRelocating = false;
        OrderedOre = CELL_NONE;
        LastRelocationScanFrame = Frame + RELOCATION_SETTLE_DELAY - RELOCATION_SCAN_DELAY;
        IsAbsorbPending = true;
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
    if (status != Status) {
        Slave_Log("M%-4d |       | miner status | %s -> %s", Manager_Index(), Manager_Status_Name(Status), Manager_Status_Name(status));
    }
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

    if (!IsSlavesCreated) {
        Create_Slaves();
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

    Slaves_AI();

    if (Owner == nullptr) {
        return;
    }

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
        if (ore != CELL_NONE) {
            LastOre = ore;
        }

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
    if (ore != CELL_NONE) {
        LastOre = ore;
    }

    unit->Mark(MARK_UP);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_UP);
    const Cell site = Find_Deploy_Site(this, unit->Class->DeploysInto, ore, here, unit->Class->MZone);
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

            /**
             *  The game moves the destination to a free cell nearby when the
             *  site got blocked (another miner deployed there). That is no
             *  order of the player: look for a new site instead of giving up.
             */
            const Cell navcell(unit->NavCom->Center_Coord());
            if (std::abs(navcell.X - DeploySite.X) <= 2 && std::abs(navcell.Y - DeploySite.Y) <= 2) {
                Slave_Log("M%-4d |       | travel | site %d,%d blocked, destination moved to %d,%d, scanning again",
                    Manager_Index(), DeploySite.X, DeploySite.Y, navcell.X, navcell.Y);
                unit->Assign_Destination(nullptr);
                if (++DeployAttempts >= MAX_DEPLOY_ATTEMPTS) {
                    Stop();
                } else {
                    Set_Status(SlaveManagerStatus::Scanning);
                }
            } else {
                Stop();
            }
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
    const bool has_slaves = Slave_Type() != nullptr;

    /**
     *  The miner's own test: Tiberium in the short scan around its center keeps
     *  it here. With slaves only Tiberium they can walk to from the door counts.
     */
    const Cell outside = has_slaves ? Door_Outside() : CELL_NONE;
    const bool walled_in = has_slaves && outside == CELL_NONE;
    const int zone = (has_slaves && !walled_in) ? Slave_Zone(outside) : -1;
    const MZoneType mzone = has_slaves ? Slave_Type()->MZone : MZONE_NORMAL;

    if (!walled_in && Find_Tiberium(center, Short_Scan(), zone, mzone) != CELL_NONE) {
        return;
    }

    Cell ore = CELL_NONE;
    if (zone >= 0) {
        ore = Find_Tiberium(center, Long_Scan(), zone, mzone);
    }

    if (ore == CELL_NONE) {
        ore = Find_Tiberium(center, Long_Scan());
    }

    if (ore == CELL_NONE) {
        return;
    }


    /**
     *  Only a walled-in door moves the miner to a site that is not closer.
     */
    const bool check_correction = !walled_in;

    if (Slave_Log_Enabled()) {
        char details[96];
        std::snprintf(details, sizeof(details), "%s, ore %d,%d", walled_in ? "door walled in" : "no reachable Tiberium in the short scan", ore.X, ore.Y);
        Slave_Log("M%-4d |       | relocate check | %s", Manager_Index(), details);
    }

    Relocate(building, ore, check_correction);
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
    DeploySiteStats stats;
    const Cell site = Find_Deploy_Site(this, unittype->DeploysInto, ore, exit_cell, unittype->MZone, &stats);

    /**
     *  Test log: what kept the sites closer to the ore free.
     */
    if (Slave_Log_Enabled()) {
        Slave_Log("M%-4d |       | relocate | closer sites rejected: zone %d, tiberium %d, slope %d, uneven %d, occupied %d, placement %d, door building %d, door blocked %d, door zone %d, other door %d",
            Manager_Index(), stats.Zone, stats.Tiberium, stats.Slope, stats.Uneven, stats.Occupied, stats.Placement,
            stats.DoorBuilding, stats.DoorBlocked, stats.DoorZone, stats.OtherDoor);
    }

    if (site == CELL_NONE) {
        Slave_Log("M%-4d |       | relocate | no deploy site near ore %d,%d, staying", Manager_Index(), ore.X, ore.Y);
        return false;
    }

    if (check_correction && Cell_Distance(site, ore) + Scan_Correction() >= Cell_Distance(center, ore)) {
        Slave_Log("M%-4d |       | relocate | site %d,%d not closer to ore %d,%d (%d cells, now %d, correction %d), staying",
            Manager_Index(), site.X, site.Y, ore.X, ore.Y,
            Cell_Distance(site, ore) / CELL_LEPTON_W, Cell_Distance(center, ore) / CELL_LEPTON_W, Scan_Correction() / CELL_LEPTON_W);
        return false;
    }

    Slave_Log("M%-4d |       | relocate | moving to %d,%d for ore %d,%d", Manager_Index(), site.X, site.Y, ore.X, ore.Y);

    LastOre = ore;
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


/**
 *  The type extension holding the slave settings: the owner's type, or the
 *  vehicle/building counterpart if only that one names a slave type.
 */
const TechnoTypeClassExtension *SlaveManagerClass::Slave_Config() const
{
    if (Owner == nullptr) {
        return nullptr;
    }

    const TechnoTypeClass *type = Owner->TClass;
    const TechnoTypeClassExtension *ext = Extension::Fetch(type);
    if (ext->SlaveMinerSlave != nullptr) {
        return ext;
    }

    const TechnoTypeClass *other = nullptr;
    if (Owner->RTTI == RTTI_BUILDING) {
        other = type->UndeploysInto;
    } else {
        other = type->DeploysInto;
    }

    if (other != nullptr && Extension::Fetch(other)->SlaveMinerSlave != nullptr) {
        return Extension::Fetch(other);
    }

    return nullptr;
}


const InfantryTypeClass *SlaveManagerClass::Slave_Type() const
{
    const TechnoTypeClassExtension *config = Slave_Config();
    return config != nullptr ? config->SlaveMinerSlave : nullptr;
}


int SlaveManagerClass::Slave_Scan() const
{
    const TechnoTypeClassExtension *config = Slave_Config();
    if (config != nullptr && config->SlaveMinerSlaveScan >= 0) return config->SlaveMinerSlaveScan;
    if (RuleExtension->SlaveMinerSlaveScan >= 0) return RuleExtension->SlaveMinerSlaveScan;
    return DEFAULT_SLAVE_SCAN * CELL_LEPTON_W;
}


/**
 *  Why slaves cannot leave the miner right now, or nullptr if they can.
 */
const char *SlaveManagerClass::Release_Blocker()
{
    BuildingClass *building = Owner_Building();
    if (building == nullptr) return "miner is a vehicle";
    if (building->IsInLimbo) return "building in limbo";
    if (!building->IsDown) return "building not finished";
    if (Status != SlaveManagerStatus::Working) return "miner not working";

    const MissionType mission = building->Get_Mission();
    if (mission == MISSION_CONSTRUCTION) return "building constructing";
    if (mission == MISSION_DECONSTRUCTION) return "building packing up";

    const Cell outside = Door_Outside();
    if (outside == CELL_NONE) return "door walled in";

    const int zone = Slave_Zone(outside);
    const MZoneType mzone = Slave_Type() != nullptr ? Slave_Type()->MZone : MZONE_NORMAL;
    if (Find_Tiberium(Door_Cell(), Slave_Reach(), zone, mzone) == CELL_NONE
        && (Can_Relocate() || Remembered_Ore(zone) == CELL_NONE)) {
        return Can_Relocate() ? "no Tiberium in reach, miner will move" : "no Tiberium in reach and none remembered";
    }
    return nullptr;
}


int SlaveManagerClass::Manager_Index() const
{
    return SlaveManagers.ID(const_cast<SlaveManagerClass *>(this));
}


void SlaveManagerClass::Log_Slave(const SlaveNode &node, const char *event, const char *details) const
{
    if (!Slave_Log_Enabled()) {
        return;
    }

    int index = -1;
    for (int i = 0; i < Slaves.Count(); ++i) {
        if (Slaves[i] == &node) {
            index = i;
            break;
        }
    }

    const InfantryClass *slave = node.Slave;
    if (slave == nullptr) {
        Slave_Log("M%-4d | S%-4d | %s | %s | (no slave)", Manager_Index(), index, event, details != nullptr ? details : "");
        return;
    }

    const Cell here = slave->Get_Cell();
    const Cell door = Door_Cell();
    Slave_Log("M%-4d | S%-4d | %s | %s | cell %d,%d target %d,%d door %d,%d nav=%d moving=%d limbo=%d mission=%d load=%d hp=%d stuck=%d",
        Manager_Index(), index, event, details != nullptr ? details : "",
        here.X, here.Y, node.Target.X, node.Target.Y, door.X, door.Y,
        slave->NavCom != nullptr, slave->Locomotion->Is_Moving() ? 1 : 0, slave->IsInLimbo ? 1 : 0,
        slave->Get_Mission(), slave->Storage.Get_Total_Amount(), slave->Strength, node.StuckCount);
}


/**
 *  Logs the door and the cells outside it, at most every 30 frames per miner.
 */
void SlaveManagerClass::Log_Door_Area(const SlaveNode &node, const char *event)
{
    if (!Slave_Log_Enabled() || Owner_Building() == nullptr || Frame < LastCellLogFrame + 30) {
        return;
    }
    LastCellLogFrame = Frame;

    int index = -1;
    for (int i = 0; i < Slaves.Count(); ++i) {
        if (Slaves[i] == &node) {
            index = i;
            break;
        }
    }

    const Cell door = Door_Cell();
    static const int _offsets[][2] = { {0,0}, {1,0}, {1,1}, {1,-1}, {0,1}, {0,-1} };

    for (int i = 0; i < int(sizeof(_offsets) / sizeof(_offsets[0])); ++i) {
        char cell_text[256];
        Describe_Cell(Cell(door.X + _offsets[i][0], door.Y + _offsets[i][1]), cell_text, sizeof(cell_text));
        Slave_Log("M%-4d | S%-4d | cell | %s, %s: %s", Manager_Index(), index, event, i == 0 ? "door" : "outside", cell_text);
    }
}


void SlaveManagerClass::Log_Snapshot() const
{
    if (Owner == nullptr) {
        return;
    }

    const Cell cell = Owner->Get_Cell();
    Slave_Log("M%-4d |       | overview | %s %s at %d,%d, miner %s, last ore %d,%d", Manager_Index(),
        Owner->RTTI == RTTI_BUILDING ? "building" : "vehicle", Owner->TClass->Name(), cell.X, cell.Y,
        Manager_Status_Name(Status), LastOre.X, LastOre.Y);

    for (int index = 0; index < Slaves.Count(); ++index) {
        Log_Slave(*Slaves[index], "overview", Slave_Status_Name(Slaves[index]->Status));
    }
}


/**
 *  How far from the door slaves work: their own scan, or the miner's working
 *  radius if that is larger.
 */
int SlaveManagerClass::Slave_Reach() const
{
    const int scan = Slave_Scan();
    const int radius = Owner != nullptr ? Harvest_Radius() : 0;
    return scan > radius ? scan : radius;
}


/**
 *  The remembered Tiberium field, or CELL_NONE. A used-up field is replaced
 *  by Tiberium near it, or by a wide search around the miner (LongScan, at
 *  most every LAST_ORE_SCAN_DELAY frames).
 */
Cell SlaveManagerClass::Remembered_Ore(int zone)
{
    const MZoneType mzone = Slave_Type() != nullptr ? Slave_Type()->MZone : MZONE_NORMAL;

    if (LastOre != CELL_NONE && Map[LastOre].Land_Type() == LAND_TIBERIUM
        && (zone < 0 || Map.Get_Cell_Zone(LastOre, mzone, false) == zone)) {
        return LastOre;
    }

    if (LastOre != CELL_NONE) {
        const Cell ore = Find_Tiberium(LastOre, Slave_Scan(), zone, mzone);
        if (ore != CELL_NONE) {
            LastOre = ore;
            return LastOre;
        }
    }

    if (Owner == nullptr || (LastOreScanFrame >= 0 && Frame < LastOreScanFrame + LAST_ORE_SCAN_DELAY)) {
        return CELL_NONE;
    }

    LastOreScanFrame = Frame;
    const Cell ore = Find_Tiberium(Owner->Get_Cell(), Long_Scan(), zone, mzone);
    if (ore != CELL_NONE) {
        LastOre = ore;
    }
    return ore;
}


/**
 *  Movement zone of a cell for the slave type, or -1 if there is no slave type.
 */
int SlaveManagerClass::Slave_Zone(const Cell &cell) const
{
    const InfantryTypeClass *type = Slave_Type();
    if (type == nullptr || cell == CELL_NONE) {
        return -1;
    }
    return Map.Get_Cell_Zone(cell, type->MZone, false);
}


/**
 *  The first cell outside the deployed miner in front of its door; slaves
 *  coming out stand in its movement zone.
 */
Cell SlaveManagerClass::Door_Outside() const
{
    const Cell door = Door_Cell();
    static const int _offsets[][2] = { {1,0}, {1,1}, {1,-1} };

    for (int i = 0; i < int(sizeof(_offsets) / sizeof(_offsets[0])); ++i) {
        const Cell cell(door.X + _offsets[i][0], door.Y + _offsets[i][1]);
        if (Map.In_Local_Radar(cell) && Map[cell].Cell_Building() == nullptr) {
            return cell;
        }
    }
    return CELL_NONE;
}


/**
 *  Does the miner move on by itself once its field is used up? Like
 *  AI_Working: with a harvest order, or always for the computer.
 */
bool SlaveManagerClass::Can_Relocate() const
{
    BuildingClass *building = Owner_Building();
    if (building == nullptr || building->Class->UndeploysInto == nullptr || building->Class->UndeploysInto->DeploysInto == nullptr) {
        return false;
    }
    return HasHarvestOrder || !building->House->Is_Human_Player();
}


bool SlaveManagerClass::Is_Door_Site_Clear(const BuildingTypeClass *buildingtype, const Cell &topleft, const Cell &ore, const char **reason) const
{
    const char *dummy = nullptr;
    if (reason == nullptr) {
        reason = &dummy;
    }

    const int width = buildingtype->Width();
    const int height = buildingtype->Height();
    const Cell door(topleft.X + width - 1, topleft.Y + height / 2);

    /**
     *  The three cells in front of the door: no building, the middle one walkable.
     */
    for (int dy = -1; dy <= 1; ++dy) {
        const Cell cell(door.X + 1, door.Y + dy);
        if (!Map.In_Local_Radar(cell) || Map[cell].Cell_Building() != nullptr) {
            *reason = "door building";
            return false;
        }
    }

    const Cell front(door.X + 1, door.Y);
    CellClass &frontcell = Map[front];
    if (!frontcell.Is_Clear_To_Move(SPEED_FOOT, true, true, -1, MZONE_NORMAL, frontcell.Height, true)) {
        *reason = "door blocked";
        return false;
    }

    const InfantryTypeClass *type = Slave_Type();
    if (type != nullptr && ore != CELL_NONE
        && Map.Get_Cell_Zone(front, type->MZone, false) != Map.Get_Cell_Zone(ore, type->MZone, false)) {
        *reason = "door zone";
        return false;
    }

    /**
     *  Do not build over the door, or the cells in front of it, of another Slave Miner.
     */
    const Cell bottomright(topleft.X + width - 1, topleft.Y + height - 1);
    for (int index = 0; index < SlaveManagers.Count(); ++index) {
        const SlaveManagerClass *other = SlaveManagers[index];
        if (other == this || other->Owner_Building() == nullptr) {
            continue;
        }
        const Cell other_door = other->Door_Cell();
        for (int dx = 0; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (Area_Gap(topleft, bottomright, Cell(other_door.X + dx, other_door.Y + dy)) == 0) {
                    *reason = "other door";
                    return false;
                }
            }
        }
    }

    return true;
}


int SlaveManagerClass::Slave_Harvest_Rate(const InfantryClass *slave) const
{
    const int value = Extension::Fetch(slave->Class)->SlaveMinerHarvestRate;
    return value >= 0 ? value : RuleExtension->SlaveMinerHarvestRate;
}


/**
 *  Number of living slaves.
 */
int SlaveManagerClass::Slave_Count() const
{
    int count = 0;
    for (int index = 0; index < Slaves.Count(); ++index) {
        if (Slaves[index]->Slave != nullptr) {
            ++count;
        }
    }
    return count;
}


SlaveManagerClass::SlaveNode *SlaveManagerClass::Find_Node(const InfantryClass *slave) const
{
    if (slave == nullptr) {
        return nullptr;
    }

    for (int index = 0; index < Slaves.Count(); ++index) {
        if (Slaves[index]->Slave == slave) {
            return Slaves[index];
        }
    }
    return nullptr;
}


/**
 *  The cell the slaves leave and enter the deployed miner through: the middle
 *  of its right edge (like YR). For the vehicle, the cell it stands on.
 */
Cell SlaveManagerClass::Door_Cell() const
{
    BuildingClass *building = Owner_Building();
    if (building != nullptr) {
        const Cell origin = building->Get_Cell();
        return Cell(origin.X + building->Class->Width() - 1, origin.Y + building->Class->Height() / 2);
    }

    return Owner != nullptr ? Owner->Get_Cell() : CELL_NONE;
}


/**
 *  Slaves only come out of a deployed miner that is working.
 */
bool SlaveManagerClass::Can_Release_Slaves() const
{
    BuildingClass *building = Owner_Building();
    if (building == nullptr || building->IsInLimbo || !building->IsDown) {
        return false;
    }

    if (Status != SlaveManagerStatus::Working) {
        return false;
    }

    const MissionType mission = building->Get_Mission();
    return mission != MISSION_CONSTRUCTION && mission != MISSION_DECONSTRUCTION;
}


/**
 *  Can this infantry enter this cell because it is one of our slaves walking
 *  into the miner's door? Called from InfantryClass::Can_Enter_Cell, so the
 *  path finder and the walk locomotor both let the slave in.
 */
bool SlaveManagerClass::Can_Slave_Enter_Cell(const InfantryClass *infantry, const CellClass *cell)
{
    if (SlaveManagers.Count() == 0 || infantry == nullptr || cell == nullptr) {
        return false;
    }

    BuildingClass *building = cell->Cell_Building();
    if (building == nullptr) {
        return false;
    }

    SlaveManagerClass *manager = Extension::Fetch(building)->SlaveManager;
    if (manager == nullptr || manager->Owner != building) {
        return false;
    }

    if (cell->Fetch_CellID() != manager->Door_Cell()) {
        return false;
    }

    return manager->Find_Node(infantry) != nullptr;
}


/**
 *  The player gave one of our slaves an order: it obeys and goes back to work
 *  once it is idle again.
 */
void SlaveManagerClass::Player_Order(TechnoClass *techno)
{
    if (techno == nullptr || techno->RTTI != RTTI_INFANTRY) {
        return;
    }

    const InfantryClass *infantry = static_cast<const InfantryClass *>(techno);

    for (int index = 0; index < SlaveManagers.Count(); ++index) {
        SlaveNode *node = SlaveManagers[index]->Find_Node(infantry);
        if (node == nullptr) {
            continue;
        }

        if (node->Status != SlaveStatus::InMiner && node->Status != SlaveStatus::Reloading && node->Status != SlaveStatus::Dead) {
            SlaveManagers[index]->Log_Slave(*node, "player order", Slave_Status_Name(node->Status));
            node->Status = SlaveStatus::Ordered;
            node->Target = CELL_NONE;
        }
        return;
    }
}


/**
 *  Makes one slave, inside the miner (limbo).
 */
static InfantryClass *Make_Slave(const InfantryTypeClass *type, HouseClass *house, bool &limbo_guard)
{
    if (type == nullptr || house == nullptr) {
        return nullptr;
    }

    InfantryClass *slave = static_cast<InfantryClass *>(type->Create_One_Of(house));
    if (slave != nullptr && !slave->IsInLimbo) {
        limbo_guard = true;
        slave->Limbo();
        limbo_guard = false;
    }
    return slave;
}


/**
 *  Creates the slaves (inside the miner) on the first logic tick.
 */
void SlaveManagerClass::Create_Slaves()
{
    IsSlavesCreated = true;

    const TechnoTypeClassExtension *config = Slave_Config();
    if (config == nullptr || config->SlaveMinerSlave == nullptr || Owner == nullptr) {
        return;
    }

    for (int index = 0; index < config->SlaveMinerSlaveCount; ++index) {

        SlaveNode *node = new SlaveNode;
        node->Slave = Make_Slave(config->SlaveMinerSlave, Owner->House, IsLimboingSlave);
        node->Status = node->Slave != nullptr ? SlaveStatus::InMiner : SlaveStatus::Dead;
        node->Timer = node->Slave != nullptr ? 0 : config->SlaveMinerRegenRate;
        node->Target = CELL_NONE;
        node->BestDistance = INT_MAX;
        node->StuckCount = 0;
        node->BadOre = CELL_NONE;
        node->PathFails = 0;

        Slaves.Add(node);
    }
}


/**
 *  The miner is gone: slaves inside it die with it, slaves outside stay with
 *  their house as ordinary infantry.
 */
void SlaveManagerClass::Free_Slaves()
{
    for (int index = 0; index < Slaves.Count(); ++index) {

        SlaveNode *node = Slaves[index];
        InfantryClass *slave = node->Slave;

        node->Slave = nullptr;
        node->Status = SlaveStatus::Dead;

        if (slave == nullptr) {
            continue;
        }

        if (slave->IsInLimbo) {
            slave->Delete_Me();
        } else if (slave->IsActive && slave->Strength > 0) {
            slave->Assign_Destination(nullptr);
            slave->Assign_Mission(MISSION_GUARD);
            slave->Commence();
        }
    }
}


/**
 *  A slave died or was removed; replace it after the regen time.
 */
void SlaveManagerClass::Slave_Lost(InfantryClass *slave)
{
    SlaveNode *node = Find_Node(slave);
    if (node == nullptr) {
        return;
    }

    const TechnoTypeClassExtension *config = Slave_Config();

    Log_Slave(*node, "lost", slave->Strength <= 0 ? "killed" : "removed (deleted, or the game ended)");

    node->Slave = nullptr;
    node->Status = SlaveStatus::Dead;
    node->Target = CELL_NONE;
    node->Timer = config != nullptr ? config->SlaveMinerRegenRate : 0;
}


bool SlaveManagerClass::Is_Slave_Full(const InfantryClass *slave) const
{
    const int capacity = slave->Class->Storage > 0 ? slave->Class->Storage : 1;
    return slave->Storage.Get_Total_Amount() >= capacity;
}


void SlaveManagerClass::Slave_Move(SlaveNode &node, const Cell &cell)
{
    InfantryClass *slave = node.Slave;

    node.Target = cell;
    slave->Assign_Target(nullptr);
    slave->Assign_Destination(&Map[cell]);
    slave->Assign_Mission(MISSION_MOVE);
    slave->Commence();
}


void SlaveManagerClass::Slaves_AI()
{
    if (IsAbsorbPending) {
        Absorb_Slaves();
    }

    if (Slave_Log_Enabled() && Frame >= LastSnapshotFrame + LOG_SNAPSHOT_DELAY) {
        LastSnapshotFrame = Frame;
        Log_Snapshot();
    }

    for (int index = 0; index < Slaves.Count(); ++index) {
        SlaveNode &node = *Slaves[index];
        const SlaveStatus old_status = node.Status;
        const InfantryClass *old_slave = node.Slave;

        Slave_AI(node);

        if (Slave_Log_Enabled() && (node.Status != old_status || node.Slave != old_slave)) {
            char details[64];
            std::snprintf(details, sizeof(details), "%s -> %s", Slave_Status_Name(old_status), Slave_Status_Name(node.Status));
            Log_Slave(node, "status", details);
        }

        if (Owner == nullptr) {
            return;
        }
    }
}


/**
 *  One slave (UpdateSlaves in YR).
 */
void SlaveManagerClass::Slave_AI(SlaveNode &node)
{
    InfantryClass *slave = node.Slave;

    /**
     *  Dead: a replacement appears inside the miner after the regen time.
     */
    if (slave == nullptr) {
        node.Status = SlaveStatus::Dead;
        if (node.Timer.Expired()) {
            node.Slave = Make_Slave(Slave_Type(), Owner->House, IsLimboingSlave);
            if (node.Slave != nullptr) {
                node.Status = SlaveStatus::InMiner;
                node.Target = CELL_NONE;
            }
        }
        return;
    }

    if (!slave->IsActive || slave->Strength <= 0) {
        return;
    }

    const bool busy = slave->NavCom != nullptr || slave->Locomotion->Is_Moving();

    switch (node.Status) {

        case SlaveStatus::InMiner:
            if (!slave->IsInLimbo) {
                node.Status = SlaveStatus::Seeking;
                break;
            }
        {
            const char *blocker = Release_Blocker();
            if (blocker != LastReleaseBlocker) {
                LastReleaseBlocker = blocker;
                Slave_Log("M%-4d |       | release | %s", Manager_Index(), blocker != nullptr ? blocker : "allowed");
            }
            if (blocker == nullptr && Slave_Exit(node)) {
                Slave_Seek(node);
            }
            break;
        }

        case SlaveStatus::Reloading:
            if (node.Timer.Expired()) {
                slave->Strength = slave->Class->MaxStrength;
                node.Status = SlaveStatus::InMiner;
            }
            break;

        case SlaveStatus::Seeking:
            if (!slave->IsInLimbo) {
                Slave_Seek(node);
            }
            break;

        case SlaveStatus::Moving:
            if (slave->IsInLimbo) {
                node.Status = SlaveStatus::Ordered;
                break;
            }
            if (busy) {

                /**
                 *  Still walking, but no closer for a while: blocked by other
                 *  infantry (the locomotor waits forever). Skip this target.
                 */
                if (node.Timer.Expired() && node.Target != CELL_NONE) {
                    const int distance = Cell_Distance(slave->Get_Cell(), node.Target);
                    if (distance < node.BestDistance) {
                        node.BestDistance = distance;
                        node.Timer = SLAVE_MOVE_STUCK_DELAY;
                    } else {
                        node.BadOre = node.Target;
                        Log_Slave(node, "stuck", "no progress towards the Tiberium target, skipped");
                        slave->Assign_Destination(nullptr);
                        slave->Assign_Mission(MISSION_GUARD);
                        Slave_Seek(node);
                    }
                }
                break;
            }
            if (Map[slave->Get_Cell()].Land_Type() == LAND_TIBERIUM) {
                node.Status = SlaveStatus::Harvesting;
                node.Timer = Slave_Harvest_Rate(slave);
                node.BadOre = CELL_NONE;
            } else {

                /**
                 *  Stopped short of the target: no path there. Skip it from now on.
                 */
                if (node.Target != CELL_NONE && Cell_Distance(slave->Get_Cell(), node.Target) > CELL_LEPTON_W) {
                    node.BadOre = node.Target;
                    Log_Slave(node, "unreachable", "Tiberium target could not be reached, skipped");
                }
                Slave_Seek(node);
            }
            break;

        case SlaveStatus::Harvesting:
        {
            if (slave->IsInLimbo) {
                node.Status = SlaveStatus::Ordered;
                break;
            }
            if (busy) {
                break;
            }
            if (Is_Slave_Full(slave)) {
                Slave_Return(node);
                break;
            }

            CellClass &cell = Map[slave->Get_Cell()];
            if (cell.Land_Type() != LAND_TIBERIUM) {
                Slave_Seek(node);
                break;
            }

            if (!node.Timer.Expired()) {
                break;
            }

            const TiberiumType tiberium = cell.Tiberium_Type_Here();
            const int amount = cell.Reduce_Tiberium(1);
            if (amount <= 0) {
                Slave_Seek(node);
                break;
            }

            slave->Storage.Increase_Amount(amount, tiberium);
            node.Timer = Slave_Harvest_Rate(slave);
            LastOre = slave->Get_Cell();

            if (Is_Slave_Full(slave)) {
                Slave_Return(node);
            }
            break;
        }

        case SlaveStatus::Returning:
            if (slave->IsInLimbo) {
                node.Status = SlaveStatus::Ordered;
                break;
            }
            Slave_Return(node);
            break;

        case SlaveStatus::Ordered:
            if (slave->IsInLimbo || busy || slave->TarCom != nullptr) {
                break;
            }
            if (slave->Get_Mission() == MISSION_GUARD || slave->Get_Mission() == MISSION_GUARD_AREA) {
                Slave_Seek(node);
            }
            break;

        default:
            node.Status = SlaveStatus::Seeking;
            break;
    }
}


/**
 *  Looks for Tiberium around the slave. Full, or nothing found: back to the miner.
 */
void SlaveManagerClass::Slave_Seek(SlaveNode &node)
{
    InfantryClass *slave = node.Slave;

    if (Is_Slave_Full(slave)) {
        Slave_Return(node);
        return;
    }

    const Cell here = slave->Get_Cell();
    const int zone = Slave_Zone(here);
    const MZoneType mzone = slave->Class->MZone;
    Cell ore = Find_Tiberium(here, Slave_Scan(), zone, mzone, node.BadOre);
    const char *source = "near slave";

    /**
     *  The miner may stand farther from the field than the slave's own scan
     *  reaches; then search from the door as far as the miner works.
     */
    if (ore == CELL_NONE && Owner_Building() != nullptr) {
        ore = Find_Tiberium(Door_Cell(), Slave_Reach(), zone, mzone, node.BadOre);
        source = "near door";
    }

    /**
     *  Nothing within reach: walk to the remembered field and search there,
     *  unless the miner moves on by itself (it then comes closer instead).
     */
    if (ore == CELL_NONE && !Can_Relocate()) {
        ore = Remembered_Ore(zone);
        source = "remembered field";
    }

    if (Slave_Log_Enabled()) {
        char details[80];
        if (ore == CELL_NONE) {
            std::snprintf(details, sizeof(details), "no Tiberium found, going back");
        } else {
            std::snprintf(details, sizeof(details), "ore %d,%d (%s)", ore.X, ore.Y, source);
        }
        Log_Slave(node, "seek", details);
    }

    if (ore == CELL_NONE) {
        Slave_Return(node);
        return;
    }

    if (ore == here) {
        slave->Assign_Destination(nullptr);
        slave->Assign_Mission(MISSION_GUARD);
        node.Target = here;
        node.Status = SlaveStatus::Harvesting;
        node.Timer = Slave_Harvest_Rate(slave);
        return;
    }

    Slave_Move(node, ore);
    node.Status = SlaveStatus::Moving;
    node.BestDistance = Cell_Distance(here, ore);
    node.Timer = SLAVE_MOVE_STUCK_DELAY;
}


/**
 *  Walks the slave back to the miner. The deployed miner is entered through
 *  its door; at the vehicle the slave waits outside until it has deployed.
 */
void SlaveManagerClass::Slave_Return(SlaveNode &node)
{
    if (node.Status != SlaveStatus::Returning) {
        node.Status = SlaveStatus::Returning;
        node.BestDistance = INT_MAX;
        node.StuckCount = 0;
        node.PathFails = 0;
        node.Timer = SLAVE_STUCK_DELAY;
    }

    if (Owner == nullptr || Owner->IsInLimbo) {
        return;
    }

    if (Owner_Building() != nullptr) {
        Slave_Return_To_Building(node);
    } else {
        Slave_Wait_For_Vehicle(node);
    }
}


void SlaveManagerClass::Slave_Return_To_Building(SlaveNode &node)
{
    InfantryClass *slave = node.Slave;

    if (Is_On_Miner(slave)) {
        Slave_Enter(node, "stepped onto the miner");
        return;
    }

    const Cell here = slave->Get_Cell();
    const Cell door = Door_Cell();
    const bool moving = slave->Locomotion->Is_Moving();
    const bool busy = slave->NavCom != nullptr || moving;

    const int distance = Cell_Distance(here, door);
    if (distance < node.BestDistance) {
        node.BestDistance = distance;
        node.StuckCount = 0;
        node.PathFails = 0;
        node.Timer = SLAVE_STUCK_DELAY;
    }

    /**
     *  Standing at the building but not getting in (door blocked or not
     *  reachable): enter right away next to the door, elsewhere along the
     *  building after the stuck delay.
     */
    /**
     *  Right next to the door: enter when standing, or when still walking
     *  without stepping in for SLAVE_DOOR_DELAY frames (door blocked).
     */
    const bool at_door = distance <= CELL_LEPTON_W && Is_Next_To_Miner(here);
    if (at_door && !moving) {
        Slave_Enter(node, "standing next to the door");
        return;
    }
    if (at_door && node.Timer.Value() <= SLAVE_STUCK_DELAY - SLAVE_DOOR_DELAY) {
        Log_Door_Area(node, "door blocked");
        Slave_Enter(node, "blocked next to the door");
        return;
    }

    if (!moving && Is_Next_To_Miner(here) && node.Timer.Expired()) {
        Slave_Enter(node, "stuck at the building");
        return;
    }

    if (node.Timer.Expired()) {

        /**
         *  Repeatedly stuck close to the miner: enter from here.
         */
        BuildingClass *building = Owner_Building();
        const Cell topleft = building->Get_Cell();
        const Cell bottomright(topleft.X + building->Class->Width() - 1, topleft.Y + building->Class->Height() - 1);
        const int gap = Area_Gap(topleft, bottomright, here);
        ++node.StuckCount;

        if (Slave_Log_Enabled()) {
            char details[80];
            std::snprintf(details, sizeof(details), "no progress, distance to door %d cells, gap to building %d",
                distance / CELL_LEPTON_W, gap);
            Log_Slave(node, "stuck", details);
            if (distance <= 2 * CELL_LEPTON_W) {
                Log_Door_Area(node, "stuck");
            }
        }

        if (node.StuckCount >= SLAVE_STUCK_ENTER_COUNT && gap <= SLAVE_STUCK_ENTER_GAP) {
            Slave_Enter(node, "stuck repeatedly near the building");
            return;
        }

        node.Timer = SLAVE_STUCK_DELAY;
        Slave_Move(node, door);
        return;
    }

    /**
     *  The last walk to the door ended without getting there: no path (door
     *  walled in, e.g. by another building). After a few tries the slave
     *  enters from where it is, if it is within its search radius.
     */
    if (!busy && node.Target == door) {
        ++node.PathFails;

        BuildingClass *building = Owner_Building();
        const Cell topleft = building->Get_Cell();
        const Cell bottomright(topleft.X + building->Class->Width() - 1, topleft.Y + building->Class->Height() - 1);
        const int gap = Area_Gap(topleft, bottomright, here);

        if (Slave_Log_Enabled()) {
            char details[80];
            std::snprintf(details, sizeof(details), "no path to the door (%d), gap to building %d", node.PathFails, gap);
            Log_Slave(node, "unreachable", details);
        }

        if (node.PathFails >= SLAVE_PATH_FAIL_COUNT && gap * CELL_LEPTON_W <= Slave_Scan()) {
            Slave_Enter(node, "door unreachable");
            return;
        }
    }

    if (!busy || node.Target != door) {
        Slave_Move(node, door);
    }
}


/**
 *  The vehicle cannot be entered. The slave keeps clear of the area the
 *  building will cover (at the deploy site while travelling there), so it
 *  never blocks deploying, and follows when the vehicle moves away.
 */
void SlaveManagerClass::Slave_Wait_For_Vehicle(SlaveNode &node)
{
    InfantryClass *slave = node.Slave;
    const Cell here = slave->Get_Cell();
    const bool busy = slave->NavCom != nullptr || slave->Locomotion->Is_Moving();

    Cell topleft;
    Cell bottomright;
    Planned_Footprint(topleft, bottomright);

    const int gap = Area_Gap(topleft, bottomright, here);
    const bool heading_in = node.Target != CELL_NONE && Area_Gap(topleft, bottomright, node.Target) <= SLAVE_KEEP_OUT_GAP;

    if (gap <= SLAVE_KEEP_OUT_GAP) {
        if (busy && !heading_in) {
            return;
        }

        /**
         *  Step away from the middle of the area, a few cells past its edge.
         */
        const int cx = (topleft.X + bottomright.X) / 2;
        const int cy = (topleft.Y + bottomright.Y) / 2;
        int sx = here.X > cx ? 1 : (here.X < cx ? -1 : 0);
        const int sy = here.Y > cy ? 1 : (here.Y < cy ? -1 : 0);
        if (sx == 0 && sy == 0) {
            sx = 1;
        }

        const int margin = SLAVE_KEEP_OUT_GAP + 2;
        Cell away(
            sx > 0 ? bottomright.X + margin : (sx < 0 ? topleft.X - margin : here.X),
            sy > 0 ? bottomright.Y + margin : (sy < 0 ? topleft.Y - margin : here.Y));

        away = Map.Nearby_Location(away, slave->Class->Speed);
        const bool ok = away != CELL_NONE && Area_Gap(topleft, bottomright, away) > SLAVE_KEEP_OUT_GAP;

        if (Slave_Log_Enabled()) {
            char details[96];
            std::snprintf(details, sizeof(details), "too close (gap %d) to area %d,%d-%d,%d, step away to %d,%d%s",
                gap, topleft.X, topleft.Y, bottomright.X, bottomright.Y, away.X, away.Y, ok ? "" : " (no free cell)");
            Log_Slave(node, "wait", details);
        }

        if (ok) {
            Slave_Move(node, away);
        }
        return;
    }

    if (gap <= SLAVE_WAIT_GAP) {
        if (slave->NavCom != nullptr && heading_in) {
            Log_Slave(node, "wait", "close enough to the vehicle, stop");
            node.Target = CELL_NONE;
            slave->Assign_Destination(nullptr);
            slave->Assign_Mission(MISSION_GUARD);
            slave->Commence();
        }
        return;
    }

    if (gap > SLAVE_FOLLOW_GAP && (!busy || !heading_in)) {
        Log_Slave(node, "wait", "vehicle too far, follow");
        Slave_Move(node, Cell((topleft.X + bottomright.X) / 2, (topleft.Y + bottomright.Y) / 2));
    }
}


/**
 *  Cells the undeployed miner's building will cover: at the deploy site while
 *  travelling there, otherwise where the vehicle stands now.
 */
void SlaveManagerClass::Planned_Footprint(Cell &topleft, Cell &bottomright) const
{
    UnitClass *unit = Owner_Unit();
    Cell stand = Owner != nullptr ? Owner->Get_Cell() : CELL_NONE;

    const bool to_site = Status == SlaveManagerStatus::Travelling
        || Status == SlaveManagerStatus::DeployRetry
        || Status == SlaveManagerStatus::Deploying;
    if (to_site && DeploySite != CELL_NONE) {
        stand = DeploySite;
    }

    BuildingTypeClass *buildingtype = unit != nullptr ? unit->Class->DeploysInto : nullptr;
    if (buildingtype == nullptr) {
        topleft = stand;
        bottomright = stand;
        return;
    }

    topleft = buildingtype->Is_Deployable() ? stand : Adjacent_Cell(stand, FACING_NW);
    bottomright = Cell(topleft.X + buildingtype->Width() - 1, topleft.Y + buildingtype->Height() - 1);
}


/**
 *  Does the cell touch the deployed miner (one of its eight neighbours is a
 *  cell of the building)?
 */
bool SlaveManagerClass::Is_Next_To_Miner(const Cell &cell) const
{
    BuildingClass *building = Owner_Building();
    if (building == nullptr) {
        return false;
    }

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const Cell next(cell.X + x, cell.Y + y);
            if (Map.In_Radar(next) && Map[next].Cell_Building() == building) {
                return true;
            }
        }
    }
    return false;
}


/**
 *  Is the slave standing on one of the deployed miner's cells?
 */
bool SlaveManagerClass::Is_On_Miner(const InfantryClass *slave) const
{
    BuildingClass *building = Owner_Building();
    return building != nullptr && !slave->IsInLimbo && Map[slave->Get_Cell()].Cell_Building() == building;
}


/**
 *  The miner just deployed: slaves the building was placed on go inside.
 */
void SlaveManagerClass::Absorb_Slaves()
{
    if (Owner_Building() == nullptr || Owner->IsInLimbo) {
        return;
    }

    IsAbsorbPending = false;

    for (int index = 0; index < Slaves.Count(); ++index) {
        SlaveNode &node = *Slaves[index];
        InfantryClass *slave = node.Slave;
        if (slave != nullptr && slave->IsActive && slave->Strength > 0 && Is_On_Miner(slave)) {
            Slave_Enter(node, "building deployed on it");
        }
    }
}


/**
 *  The slave enters the miner: its load is credited to the house like a
 *  harvester unloading at a refinery, then it waits inside and is healed.
 */
void SlaveManagerClass::Slave_Enter(SlaveNode &node, const char *reason)
{
    InfantryClass *slave = node.Slave;

    Log_Slave(node, "enter", reason);
    HouseClass *house = Owner->House;

    for (int index = 0; index < Tiberiums.Count(); ++index) {
        const TiberiumType tiberium = static_cast<TiberiumType>(index);
        const int amount = slave->Storage.Get_Amount(tiberium);
        if (amount > 0) {
            house->Tiberium_Harvested(amount, tiberium);
            slave->Storage.Decrease_Amount(amount, tiberium);
        }
    }

    /**
     *  Limbo() does not release an infantry's spot in its cell (TS only does
     *  that for other objects), so free it first, like ObjectClass::Fall_From_Height;
     *  otherwise the door fills up with spots nobody stands on.
     */
    if (slave->Locomotion->Is_Moving()) {
        slave->Locomotion->Mark_All_Occupation_Bits(MARK_UP);

        /**
         *  Limbo() keeps a step that is under way; after Unlimbo() the slave
         *  would finish it instead of starting a new one, and the spot it
         *  appeared on would stay taken. Drop the step and the destination.
         */
        slave->Locomotion->Stop_Moving();
        slave->Locomotion->Force_Immediate_Destination(COORD_NONE);
    } else {
        Coord coord = slave->Get_Coord();
        slave->Clear_Occupy_Bit(coord);
    }

    slave->Assign_Target(nullptr);
    slave->Assign_Destination(nullptr);
    slave->Assign_Mission(MISSION_GUARD);

    IsLimboingSlave = true;
    slave->Limbo();
    IsLimboingSlave = false;

    const TechnoTypeClassExtension *config = Slave_Config();

    node.Target = CELL_NONE;
    node.Status = SlaveStatus::Reloading;
    node.Timer = config != nullptr ? config->SlaveMinerReloadRate : 0;
}


/**
 *  The slave leaves the deployed miner through its door cell.
 */
bool SlaveManagerClass::Slave_Exit(SlaveNode &node)
{
    InfantryClass *slave = node.Slave;
    const Cell door = Door_Cell();

    /**
     *  The door first; if it is full (three infantry per cell) or blocked,
     *  the cells right outside it.
     */
    static const int _offsets[][2] = { {0,0}, {1,0}, {1,1}, {1,-1}, {0,1}, {0,-1} };
    static const int _count = sizeof(_offsets) / sizeof(_offsets[0]);

    bool placed = false;
    for (int index = 0; index < _count && !placed; ++index) {
        const Cell cell(door.X + _offsets[index][0], door.Y + _offsets[index][1]);
        if (!Map.In_Local_Radar(cell)) {
            continue;
        }
        if (index > 0 && Map[cell].Cell_Building() != nullptr) {
            continue;
        }

        Coord coord = Map[cell].Center_Coord();
        coord.Z = Map.Get_Height_GL(coord);
        placed = slave->Unlimbo(coord, DIR_E);
    }

    if (!placed) {
        Log_Slave(node, "exit", "failed: door and the cells outside it are full or blocked");
        Log_Door_Area(node, "exit failed");
        return false;
    }

    Log_Slave(node, "exit", "left the miner");

    node.Target = CELL_NONE;
    node.Status = SlaveStatus::Seeking;
    return true;
}
