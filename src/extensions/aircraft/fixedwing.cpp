/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "always.h"
#include "fixedwing.h"

#include "aircraft.h"
#include "aircraftext.h"
#include "aircrafttype.h"
#include "aircrafttypeext.h"
#include "aircrafttracker.h"
#include "anim.h"
#include "building.h"
#include "buildingtype.h"
#include "cell.h"
#include "coord.h"
#include "debughandler.h"
#include "extension.h"
#include "event.h"
#include "flylocomotion.h"
#include "footext.h"
#include "house.h"
#include "infantry.h"
#include "map.h"
#include "mouse.h"
#include "rules.h"
#include "syringe.h"
#include "target.h"
#include "terrain.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "vinifera_globals.h"
#include "unit.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace FixedWing
{
namespace
{
    enum EnterStatus { INITIAL, TAKEOFF, ALTITUDE, STACK, DOWNWIND, CROSSWIND, TRAVEL, LANDING };
    enum CAPStatus { CAP_INITIAL, CAP_CORNER_0, CAP_CORNER_1, CAP_CORNER_2, CAP_CORNER_3 };
    constexpr int FINAL_LENGTH = 8 * CELL_LEPTON;
    constexpr int ALIGN_LENGTH = 6 * CELL_LEPTON;
    constexpr int SIDE_LENGTH = 6 * CELL_LEPTON;
    constexpr int HOLDING_HALF_WIDTH = 12 * CELL_LEPTON;
    constexpr int HOLDING_HALF_HEIGHT = 8 * CELL_LEPTON;
    constexpr int ARRIVAL = CELL_LEPTON / 2;
    constexpr int TOUCHDOWN = CELL_LEPTON / 8;
    constexpr int HEADING_ERROR = 1024; // 1/64 turn, 5.625 degrees.
    constexpr int CLIMB = 20;
    constexpr int DOCK_RETRY = TICKS_PER_SECOND;
    constexpr int NO_DOCK_GRACE = 3 * TICKS_PER_MINUTE; // 2700 simulation frames.

    struct Approach {
        Coord D, P, G, S;
        DirType Direction;
        bool PrimaryValid; // Transient diagnostic result, never stored on an object.
    };

    // These are native nonvirtual __thiscall methods on the full Fly object,
    // not stdcall ILocomotion methods (whose this pointer is four bytes later).
    void Take_Off(FlyLocomotionClass* fly)
    {
        reinterpret_cast<void (__thiscall*)(FlyLocomotionClass*)>(0x0049CB00)(fly);
    }
    bool Process_Takeoff(FlyLocomotionClass* fly)
    {
        return reinterpret_cast<bool (__thiscall*)(FlyLocomotionClass*)>(0x0049B1A0)(fly);
    }
    bool Process_Landing(FlyLocomotionClass* fly)
    {
        return reinterpret_cast<bool (__thiscall*)(FlyLocomotionClass*)>(0x0049B360)(fly);
    }

    int64_t Distance_Squared(const Coord& a, const Coord& b)
    {
        const int64_t x = a.X - b.X, y = a.Y - b.Y;
        return x * x + y * y;
    }

    // Arrival uses the complete movement segment, including high speed fly-bys.
    bool Passed(const Coord& from, const Coord& to, const Coord& goal, int radius)
    {
        const int64_t dx = to.X - from.X, dy = to.Y - from.Y;
        const int64_t gx = goal.X - from.X, gy = goal.Y - from.Y;
        const int64_t length = dx * dx + dy * dy;
        const int64_t dot = gx * dx + gy * dy;
        const int64_t r2 = int64_t(radius) * radius;
        if (length == 0 || dot <= 0) return Distance_Squared(from, goal) <= r2;
        if (dot >= length) return Distance_Squared(to, goal) <= r2;
        // Divide before squaring, avoiding a fourth-power coordinate product.
        const Coord closest(from.X + int(dx * dot / length), from.Y + int(dy * dot / length));
        return Distance_Squared(closest, goal) <= r2;
    }

    bool Heading_OK(const DirType& a, const DirType& b)
    {
        const int delta = static_cast<short>(a.Get_Raw() - b.Get_Raw());
        return delta >= -HEADING_ERROR && delta <= HEADING_ERROR;
    }

    bool Valid_Point(const Coord& coord)
    {
        return coord.X >= 0 && coord.Y >= 0 && Map.In_Radar(coord) && Map.In_Local_Radar(coord);
    }

    bool Inside_Array(const Coord& coord)
    {
        // Off-local reinforcement coordinates are legal; array bounds are not.
        // Keep a one-cell guard for Mark/adjacency lookups.
        const Cell cell = coord.As_Cell();
        return coord.X >= CELL_LEPTON && coord.Y >= CELL_LEPTON
            && cell.X < Map.XSize - 1 && cell.Y < Map.YSize - 1;
    }

    bool Valid_Segment(const Coord& a, const Coord& b)
    {
        const int steps = std::max(abs(b.X - a.X), abs(b.Y - a.Y)) / (CELL_LEPTON / 2) + 1;
        for (int i = 0; i <= steps; ++i) {
            if (!Valid_Point(Coord(a.X + int(int64_t(b.X - a.X) * i / steps),
                    a.Y + int(int64_t(b.Y - a.Y) * i / steps)))) return false;
        }
        return true;
    }

    bool Native_Flight_Point(const Coord& coord)
    {
        // Fly::Movement_AI commits positions inside the outer In_Radar diamond,
        // including its non-playable margin outside In_Local_Radar.
        return coord.X >= 0 && coord.Y >= 0
            && coord.X / CELL_LEPTON < Map.XSize && coord.Y / CELL_LEPTON < Map.YSize
            && Map.In_Radar(coord);
    }

    Coord Takeoff_Movement_Coord(const Coord& from, Coord to)
    {
        // Native Fly::Movement_AI, 0x0049A5A3..0x0049A6D2: correct only the
        // outer world edge, then retain the old position if correction fails.
        if (!Native_Flight_Point(to)) {
            const Cell cell = to.As_Cell();
            const int column = Map.PlayRect.Width / 2
                + ((int(cell.X) - cell.Y + (Map.PlayRect.Width & 1)) >> 1) - Map.LocalRect.X;
            to.X += column < Map.MapRect.Width / 2 ? CELL_LEPTON / 2 : -CELL_LEPTON / 2;
            if (!Native_Flight_Point(to)) to = Coord_Scatter(to, CELL_LEPTON / 4, false);
        }
        return Inside_Array(to) && Native_Flight_Point(to) ? to : from;
    }

    bool Build_Approach(const Coord& dock, const DirType& direction, Approach& out)
    {
        out.D = dock;
        out.Direction = direction;
        DirType back = out.Direction + DirType(DIR_S);
        DirType side = out.Direction + DirType(DIR_E);
        out.P = Coord_Move(out.D, back, FINAL_LENGTH);
        out.G = Coord_Move(out.P, back, ALIGN_LENGTH);
        out.S = Coord_Move(out.G, side, SIDE_LENGTH);
        return Valid_Point(out.D) && Valid_Point(out.S) && Valid_Point(out.G) && Valid_Point(out.P)
            && Valid_Segment(out.S, out.G) && Valid_Segment(out.G, out.P)
            && Valid_Segment(out.P, out.D) && !Map[out.D].IsUnderBridge;
    }

    bool Geometry(AircraftClass* a, BuildingClass* dock, Approach& out)
    {
        const Coord position = dock->Docking_Coord();
        // Landing_Direction is an eight-way FacingType, NOT a Dir256 value.
        // In mutual contact it reads the same building facing used before HELLO.
        const auto facing = a->Radio == dock && dock->Radio == a
            ? static_cast<FacingType>(a->Landing_Direction())
            : static_cast<FacingType>(dock->PrimaryFacing.Current().Get_Facing<8>());
        const DirType primary(facing);
        out.PrimaryValid = Build_Approach(position, primary, out);
        if (out.PrimaryValid) return true;
        // DirType's 16-bit addition wraps; DIR_S is exactly 128 in Dir256.
        // Rebuild the entire chain with unchanged lengths, never clamp points.
        return Build_Approach(position, primary + DirType(DIR_S), out);
    }

    bool Dock_Exists(AircraftClass* a, BuildingClass* dock)
    {
        if (!dock || !dock->IsActive || dock->IsInLimbo || dock->Strength <= 0
            || dock->House != a->House || !a->Class->Dock.Is_Present(dock->Class)
            || dock->CurrentMission == MISSION_CONSTRUCTION
            || dock->CurrentMission == MISSION_DECONSTRUCTION || dock->BState == BSTATE_CONSTRUCTION
            || !(dock->Class->IsCanUnitReload || dock->Class->IsCanUnitRepair)) return false;
        return Valid_Point(dock->Docking_Coord());
    }

    bool Dock_Status(AircraftClass* a, BuildingClass* dock, bool reserved)
    {
        if (!Dock_Exists(a, dock) || !dock->IsOn || (dock->Radio && dock->Radio != a)) return false;
        const Coord position = dock->Docking_Coord();
        for (auto object = Map[position].Cell_Occupier(); object; object = object->Next) {
            if (object == a || object == dock || !object->IsActive || object->IsInLimbo) continue;
            const auto rtti = object->Fetch_RTTI();
            if (rtti == RTTI_AIRCRAFT || rtti == RTTI_UNIT || rtti == RTTI_INFANTRY) return false;
        }
        return !reserved || (a->Radio == dock && dock->Radio == a);
    }

    BuildingClass* Reserved(AircraftClass* a)
    {
        // RadioClass::Detach clears destroyed contacts before their memory is freed.
        if (!a->Radio || a->Radio->Fetch_RTTI() != RTTI_BUILDING) return nullptr;
        auto dock = static_cast<BuildingClass*>(a->Radio);
        return Dock_Status(a, dock, true) ? dock : nullptr;
    }

    // AF-0004G1: observational state only; never saved, CRC'd or read by gameplay.
    namespace DockDiagnostics
    {
        struct Watch {
            long LastObservation = -TICKS_PER_SECOND;
            long LastSearch = -1;
            long LastLoggedSearch = -TICKS_PER_SECOND;
            long StackSince = -1;
            bool Tracking = false;
            bool ReportedStall = false;
        };
        std::map<int, Watch> Watches;
        std::map<std::pair<int, int>, std::string> Snapshots;
        AircraftClass* Searching = nullptr;
        const BuildingTypeClass* SearchingType = nullptr;
        struct NativeProgress {
            bool FilterPassed = false;
            bool CanLoadStarted = false;
            bool CanLoadPassed = false;
            bool Best = false;
        };
        NativeProgress Progress;

        bool Enabled()
        {
            return Vinifera_DeveloperMode;
        }

        void Reset_If_Needed()
        {
            static long last_frame = -1;
            if (Frame < last_frame || Vinifera_PerformingLoad) {
                Watches.clear();
                Snapshots.clear();
            }
            last_frame = Frame;
        }

        const AbstractClass* Live_Object(const AbstractClass* pointer)
        {
            // Compare pointers before dereferencing suspected stale contacts.
            const auto find = [pointer](const auto& list) -> const AbstractClass* {
                for (int i = 0; i < list.Count(); ++i) if (list[i] == pointer) return list[i];
                return nullptr;
            };
            if (auto object = find(Buildings)) return object;
        if (auto object = find(Aircrafts)) return object;
            if (auto object = find(Units)) return object;
            if (auto object = find(Infantry)) return object;
            if (auto object = find(Anims)) return object;
            return find(Terrains);
        }

        std::string Ref(const AbstractClass* pointer, bool may_be_cell = false)
        {
            if (!pointer) return "null";
            if (auto object = Live_Object(pointer)) {
                return std::format("ptr={},rtti={},id={},heap={}", static_cast<const void*>(pointer),
                    int(object->Fetch_RTTI()), object->ID, object->Fetch_Heap_ID());
            }
            if (may_be_cell) {
                const int index = Map.ID(reinterpret_cast<CellClass*>(const_cast<AbstractClass*>(pointer)));
                if (index >= 0) {
                    const auto cell = Map.Array[index]->CellID;
                    return std::format("cell=({},{}),ptr={}", cell.X, cell.Y, static_cast<const void*>(pointer));
                }
            }
            return std::format("UNRESOLVED_ptr={}", static_cast<const void*>(pointer));
        }

        void Aircraft_State(AircraftClass* a, const char* event)
        {
            auto fly = Locomotor(a);
            const Coord destination = fly ? fly->DestinationCoord : COORD_NONE;
            DEV_DEBUG_INFO("[FWDOCK] frame={} event={} a(id={},heap={},type={},house={},dockTypes={}) mission={} status={} timer={} queue={} suspended={} noDock={} NavCom=[{}] Radio=[{}] Destination=({},{},{}) moving={} takeoff={} powered={} tumble={} field370={} controlled={}\n",
                Frame, event, a->ID, a->Fetch_Heap_ID(), a->Class->Name(), a->House->Fetch_Heap_ID(), a->Class->Dock.Count(), int(a->CurrentMission), a->Status,
                a->MissionClass::Timer.Value(), int(a->MissionQueue), int(a->SuspendedMission),
                Extension::Fetch(a)->FixedWingNoDockTimer, Ref(a->NavCom, true), Ref(a->Radio),
                destination.X, destination.Y, destination.Z, fly && fly->IsMoving, fly && fly->IsTakingOff,
                fly && fly->IsPowered, fly && fly->field_4B, a->field_370, fly && Controlled(fly) == a);
        }

        void Candidate(AircraftClass* a, BuildingClass* dock, const char* phase)
        {
            if (!dock) return;
            const Coord d = dock->Docking_Coord();
            const bool valid = Valid_Point(d);
            bool clear = true;
            bool occupancy_known = valid;
            std::string occupants;
            if (valid) {
                std::vector<ObjectClass*> seen;
                auto object = Map[d].Cell_Occupier();
                for (; object && seen.size() < 16; object = object->Next) {
                    if (std::find(seen.begin(), seen.end(), object) != seen.end()
                        || !Live_Object(object)) {
                        occupants += std::format(" [UNRESOLVED_OR_CYCLE {}]", static_cast<void*>(object));
                        occupancy_known = false;
                        break; // No diagnostic dereference of an unregistered pointer.
                    }
                    seen.push_back(object);
                    const auto rtti = object->Fetch_RTTI();
                    const bool blocks = object != a && object != dock && object->IsActive && !object->IsInLimbo
                        && (rtti == RTTI_AIRCRAFT || rtti == RTTI_UNIT || rtti == RTTI_INFANTRY);
                    if (blocks) clear = false;
                    occupants += std::format(" [id={},heap={},rtti={},active={},limbo={},blocks={},ptr={}]",
                        object->ID, object->Fetch_Heap_ID(), int(rtti), object->IsActive, object->IsInLimbo,
                        blocks, static_cast<void*>(object));
                }
                if (object) {
                    occupancy_known = false;
                    occupants += " [walk stopped; actual filter result is logged separately]";
                }
            }
            Approach approach;
            const bool geometry = valid && Geometry(a, dock, approach);
            std::string checks;
            const char* reason = "PASS";
            const auto check = [&](const char* name, bool pass) {
                checks += std::format(" {}={}", name, pass ? "PASS" : "FAIL");
                if (!pass && std::string_view(reason) == "PASS") reason = name;
            };
            check("REJECT_INACTIVE", dock->IsActive);
            check("REJECT_LIMBO", !dock->IsInLimbo);
            check("REJECT_DEAD", dock->Strength > 0);
            check("REJECT_WRONG_HOUSE", dock->House == a->House);
            check("REJECT_NOT_IN_DOCK_LIST", a->Class->Dock.Is_Present(dock->Class));
            check("REJECT_UNFINISHED", dock->CurrentMission != MISSION_CONSTRUCTION && dock->BState != BSTATE_CONSTRUCTION);
            check("REJECT_DECONSTRUCTION", dock->CurrentMission != MISSION_DECONSTRUCTION);
            check("REJECT_NO_SERVICE", dock->Class->IsCanUnitReload || dock->Class->IsCanUnitRepair);
            check("REJECT_INVALID_DOCK_COORD", valid);
            check("REJECT_POWERED_OFF", dock->IsOn);
            check("REJECT_RADIO_OCCUPIED", !dock->Radio || dock->Radio == a);
            if (occupancy_known) check("REJECT_CELL_OCCUPIED", clear);
            else checks += " REJECT_CELL_OCCUPIED=SKIP_UNRESOLVED_OR_INVALID";
            check("REJECT_APPROACH_GEOMETRY", geometry);
            // These are probes, not extra radio messages or new candidate decisions.
            checks += std::format(" REJECT_FOREIGN_RESERVATION={} nativeUnoccupied={} reservedProbe={}",
                a->Radio == dock && dock->Radio == a ? "PASS" : "FAIL", dock->Radio == nullptr,
                occupancy_known ? int(Dock_Status(a, dock, true)) : -1);
            const std::string state = std::format("type={} house={} active={} limbo={} strength={} IsOn={} mission={} status={} BState={} QueueBState={} ready={} opened={} reload={} repair={} helipad={} BQuantity={} D=({},{},{}) Radio=[{}] aircraftRadio=[{}] cellUnderBridge={} altOccupant=[{}] firstProbe={} checks:{}",
                dock->Class->Name(), dock->House ? dock->House->Fetch_Heap_ID() : -1,
                dock->IsActive, dock->IsInLimbo, dock->Strength, dock->IsOn, int(dock->CurrentMission), dock->Status,
                int(dock->BState), int(dock->QueueBState), dock->IsReadyToCommence, dock->HasOpened,
                dock->Class->IsCanUnitReload, dock->Class->IsCanUnitRepair, dock->Class->IsHelipad,
                a->House->BQuantity.Value(static_cast<StructType>(dock->Class->Fetch_Heap_ID())),
                d.X, d.Y, d.Z, Ref(dock->Radio), Ref(a->Radio), valid && bool(Map[d].IsUnderBridge),
                valid ? Ref(Map[d].AltOccupierPtr) : "SKIP_INVALID_COORD", reason, checks);
            std::string geometry_state = "SKIP_INVALID_DOCK_COORD";
            if (valid) {
                const char* selection = approach.PrimaryValid ? "APPROACH_PRIMARY_VALID"
                    : geometry ? "APPROACH_PRIMARY_INVALID_FALLBACK_OPPOSITE APPROACH_OPPOSITE_VALID"
                    : "APPROACH_PRIMARY_INVALID_FALLBACK_OPPOSITE APPROACH_OPPOSITE_INVALID";
                geometry_state = std::format("{} primaryLandingFacing={} selectedLandingFacing={} headingRaw={} Dvalid={} S=({},{},{}) valid={} G=({},{},{}) valid={} P=({},{},{}) valid={} segments(SG,GP,PD)=({},{},{})",
                    selection,
                    a->Radio == dock && dock->Radio == a ? int(a->Landing_Direction()) : int(dock->PrimaryFacing.Current().Get_Facing<8>()),
                    int(approach.Direction.Get_Facing<8>()),
                    int(approach.Direction.Get_Raw()), Valid_Point(approach.D),
                    approach.S.X, approach.S.Y, approach.S.Z, Valid_Point(approach.S),
                    approach.G.X, approach.G.Y, approach.G.Z, Valid_Point(approach.G),
                    approach.P.X, approach.P.Y, approach.P.Z, Valid_Point(approach.P),
                    Valid_Segment(approach.S, approach.G), Valid_Segment(approach.G, approach.P), Valid_Segment(approach.P, approach.D));
            }
            auto& previous = Snapshots[{a->ID, dock->ID}];
            const auto signature = state + occupants + geometry_state;
            if (signature == previous) return;
            previous = signature;
            Aircraft_State(a, phase);
            DEV_DEBUG_INFO("[FWDOCK] frame={} a={} dock(id={},heap={},ptr={}) {}\n", Frame,
                a->ID, dock->ID, dock->Fetch_Heap_ID(), static_cast<void*>(dock), state);
            DEV_DEBUG_INFO("[FWDOCK] frame={} a={} dock={} cellOccupants:{} geometry:{} presence={} candidateProbe={}\n",
                Frame, a->ID, dock->ID, occupants, geometry_state, Dock_Exists(a, dock),
                occupancy_known ? int(Dock_Status(a, dock, false) && geometry) : -1);
        }

        void Inventory(AircraftClass* a, const char* phase)
        {
            for (int i = 0; i < Buildings.Count(); ++i) {
                auto dock = Buildings[i];
                if (dock && a->Class->Dock.Is_Present(dock->Class)) Candidate(a, dock, phase);
            }
        }

        void Observe(AircraftClass* a)
        {
            if (!Enabled() || !a || !a->IsActive || a->IsInLimbo || a->Strength <= 0 || !FixedWing::Enabled(a)) return;
            Reset_If_Needed();
            const bool stack = a->CurrentMission == MISSION_ENTER && a->Status == STACK;
            auto& watch = Watches[a->ID];
            watch.Tracking |= stack || Extension::Fetch(a)->FixedWingNoDockTimer > 0;
            if (!watch.Tracking || Frame - watch.LastObservation < TICKS_PER_SECOND) return;
            watch.LastObservation = Frame;
            Inventory(a, "INVENTORY_CHANGE");
            if (!stack) { watch.StackSince = -1; watch.ReportedStall = false; return; }
            if (watch.StackSince < 0) watch.StackSince = Frame;
            if (!watch.ReportedStall && Frame - std::max(watch.StackSince, watch.LastSearch) > 2 * DOCK_RETRY) {
                Aircraft_State(a, "STACK_SEARCH_OVERDUE");
                watch.ReportedStall = true;
            }
        }

        struct Search {
            AircraftClass* Aircraft;
            AircraftClass* Previous;
            const BuildingTypeClass* PreviousType;
            NativeProgress PreviousProgress;
            bool Active;
            explicit Search(AircraftClass* a) : Aircraft(a), Previous(Searching), PreviousType(SearchingType),
                PreviousProgress(Progress), Active(Enabled())
            {
                if (!Active) return;
                Reset_If_Needed();
                auto& watch = Watches[a->ID];
                watch.LastSearch = Frame;
                watch.ReportedStall = false;
                // Also bound output if a bug calls Find_Dock every frame.
                Active = Frame - watch.LastLoggedSearch >= DOCK_RETRY;
                if (!Active) return;
                watch.LastLoggedSearch = Frame;
                Searching = a;
                SearchingType = nullptr;
                Progress = {};
                Aircraft_State(a, "SEARCH_BEGIN");
                Inventory(a, "SEARCH_CANDIDATE_CHANGE");
            }
            ~Search() { if (Active) { Searching = Previous; SearchingType = PreviousType; Progress = PreviousProgress; } }
            void Type(const BuildingTypeClass* type)
            {
                if (!Active) return;
                SearchingType = type;
                Progress = {};
                const int quantity = Aircraft->House->BQuantity.Value(static_cast<StructType>(type->Fetch_Heap_ID()));
                DEV_DEBUG_INFO("[FWDOCK] frame={} a={} NATIVE_TYPE_BEGIN type={} BQuantity={} inventoryProbe={}\n",
                    Frame, Aircraft->ID, type->Name(), quantity, quantity ? "PASS" : "REJECT_NATIVE_HOUSE_INVENTORY");
            }
            void Result(BuildingClass* dock)
            {
                if (!Active) return;
                DEV_DEBUG_INFO("[FWDOCK] frame={} a={} nativeResult type={} dock=[{}]\n", Frame,
                    Aircraft->ID, SearchingType->Name(), Ref(dock));
            }
            void Success(BuildingClass* dock)
            {
                if (!Enabled()) return;
                Aircraft_State(Aircraft, "RESERVED_SUCCESS");
                Candidate(Aircraft, dock, "RESERVED_STATE");
                Watches[Aircraft->ID].Tracking = false;
            }
        };

        void Radio_Result(AircraftClass* a, const AbstractClass* contact, const char* message, RadioMessageType reply)
        {
            if (Searching != a) return;
            std::string peer_radio = "UNRESOLVED";
            if (auto live = Live_Object(contact)) {
                const auto rtti = live->Fetch_RTTI();
                if (rtti == RTTI_BUILDING || rtti == RTTI_AIRCRAFT || rtti == RTTI_UNIT || rtti == RTTI_INFANTRY)
                    peer_radio = Ref(static_cast<const RadioClass*>(live)->Radio);
            }
            DEV_DEBUG_INFO("[FWDOCK] frame={} a={} {} contactPtr={} reply={} {} aircraftRadio=[{}] peerRadio=[{}]\n", Frame,
                a->ID, message, static_cast<const void*>(contact), int(reply),
                reply == RADIO_ROGER ? "PASS" : "FAIL", Ref(a->Radio), peer_radio);
        }

        void Native(AircraftClass* a, BuildingClass* dock, const char* stage, int value)
        {
            if (Searching != a || !SearchingType) return;
            if (dock && dock->Class != SearchingType) return;
            DEV_DEBUG_INFO("[FWDOCK] frame={} a={} nativeStage={} type={} dock=[{}] value={}\n",
                Frame, a->ID, stage, SearchingType->Name(), Ref(dock), value);
            if (dock) Candidate(a, dock, stage);
        }

        bool Native_Candidate(AircraftClass* a, BuildingClass* dock)
        {
            return Searching == a && SearchingType && dock && dock->Class == SearchingType;
        }

        void Native_Enumerated(AircraftClass* a, BuildingClass* dock)
        {
            if (!Native_Candidate(a, dock)) return;
            Progress = {};
            Native(a, dock, "NATIVE_ENUMERATED", 0);
        }

        void Native_Filter_Passed(AircraftClass* a, BuildingClass* dock)
        {
            if (!Native_Candidate(a, dock)) return;
            Progress.FilterPassed = true;
            Native(a, dock, dock->Radio ? "REJECT_NATIVE_RADIO_CONTACT" : "FIXED_FILTER_PASS", 0);
        }

        void Native_CanLoad(AircraftClass* a, BuildingClass* dock, bool passed)
        {
            if (!Native_Candidate(a, dock)) return;
            if (passed) Progress.CanLoadPassed = true;
            else Progress.CanLoadStarted = true;
            Native(a, dock, passed ? "RADIO_CAN_LOAD_PASS" : "RADIO_CAN_LOAD_BEGIN", passed ? int(RADIO_ROGER) : 0);
        }

        void Native_Finished(AircraftClass* a, BuildingClass* dock, int value)
        {
            if (!Native_Candidate(a, dock)) return;
            const char* stage = "NATIVE_SKIP_BEFORE_CAN_LOAD";
            // Best can only be reached after ROGER, regardless of the optional
            // distance-entry marker. Vinifera patches that call to its own
            // _Cell_Distance_Squared, bypassing the original entry hook. EAX at
            // this join is then a distance, not necessarily a radio reply.
            if (Progress.Best) stage = "NATIVE_ACCEPTED_SO_FAR";
            else if (Progress.CanLoadPassed) stage = "NATIVE_NOT_BEST_DISTANCE_PRIORITY";
            else if (Progress.CanLoadStarted) stage = "NATIVE_CAN_LOAD_OUTCOME_UNCONFIRMED";
            else if (Progress.FilterPassed && dock->Radio) stage = "REJECT_NATIVE_RADIO_CONTACT";
            Native(a, dock, stage, value);
        }
    }

    void Release(AircraftClass* a)
    {
        if (a->In_Radio_Contact()) {
            const auto contact = a->Radio;
            const auto reply = a->Transmit_Message(RADIO_OVER_OUT);
            DockDiagnostics::Radio_Result(a, contact, "RADIO_OVER_OUT", reply);
        }
    }

    int Full_Speed(AircraftClass* a)
    {
        // Match Vinifera's Apparent_Speed at CurrentSpeed=1, including its
        // integer truncation before and after the FASTER modifier.
        int speed = a->Get_Max_Speed() * a->House->AirspeedBias * a->SpeedBias;
        if (a->Has_Ability(ABILITY_FASTER)) speed *= Rule->VeteranSpeed + 1.0;
        return speed;
    }

    double Get_Approach_Speed(AircraftClass* a, int mission_status)
    {
        // Return a dimensionless locomotor factor, not a Rules Speed value.
        if (a->CurrentMission != MISSION_ENTER) return 1.0;
        int multiplier;
        switch (mission_status) {
        case DOWNWIND: multiplier = 4; break;
        case CROSSWIND: multiplier = 3; break;
        case TRAVEL: case LANDING: multiplier = 2; break;
        default: return 1.0;
        }

        // Read the actual Facing rate (Dir256 units/frame); never change ROT.
        const int rot = static_cast<int>(a->PrimaryFacing.ROT.Get_Dir());
        const int rules_limit = std::clamp(rot * multiplier, 1, 100);
        // CCINIClass::Get_MPHType conversion: clamp to 0..100, scale by
        // 256/100 with integer truncation, then cap at 255 leptons/tick.
        const int step_limit = std::min(rules_limit * 256 / 100, 255);
        const int normal_speed = std::max(1, Full_Speed(a));
        return std::min(1.0, double(step_limit) / normal_speed);
    }

    void Cruise(AircraftClass* a, FlyLocomotionClass* fly)
    {
        fly->IsLanding = false;
        fly->WasLanding = false;
        fly->FlightLevel = a->Class->Flight_Level();
        fly->IsElevating = false;
        fly->IsMoving = true;
    }

    void Goal(FlyLocomotionClass* fly, const Coord& coord)
    {
        fly->DestinationCoord = coord;
        fly->HeadToCoord = coord;
    }

    bool CAP_Valid(AircraftClass* a);

    // Transient CAP/escort/native-attack observations only, never gameplay state.
    namespace CAPDiagnostics
    {
        struct Watch {
            const AircraftClass* Object = nullptr;
            std::string CommandTarget, CommandDestination, Identity, EscortRef, LastTarget;
            bool FirstAI = false, FirstFlight = false, HaveCenter = false;
            std::string NativeOrigin = "NATIVE";
            bool NativeObserved = false, NativeHadTarget = false;
            bool NativeMissionSeen = false, NativeFlySeen = false;
            int NativeAmmo = 0, Corner = -1;
            Coord Center = COORD_NONE;
        };
        std::map<int, Watch> Watches;

        bool Enabled()
        {
            if (Vinifera_PerformingLoad) { Watches.clear(); return false; }
            if (!Vinifera_DeveloperMode) Watches.clear();
            return Vinifera_DeveloperMode;
        }

        Watch& Get(AircraftClass* a)
        {
            static long last_frame = -1;
            if (Frame < last_frame) Watches.clear();
            last_frame = Frame;
            auto& watch = Watches[a->ID];
            if (watch.Object != a) { watch = Watch{}; watch.Object = a; }
            return watch;
        }

        std::string XYZ(const Coord& c) { return std::format("({},{},{})", c.X, c.Y, c.Z); }
        std::string Point(const Coord& c)
        {
            const auto cell = c.As_Cell();
            const bool inside = Inside_Array(c);
            const bool allocated = c.X >= 0 && c.Y >= 0
                && c.X / CELL_LEPTON < Map.XSize && c.Y / CELL_LEPTON < Map.YSize;
            return std::format("{} cell=({},{}) radar={} local={} array={} arrayGuard={} valid={} nativeFlightValid={}", XYZ(c), cell.X, cell.Y,
                c.X >= 0 && c.Y >= 0 && Map.In_Radar(c),
                allocated && Map.In_Local_Radar(c), allocated, inside, inside && Valid_Point(c), Native_Flight_Point(c));
        }
        std::string Ref(const AbstractClass* p)
        {
            if (!p) return "NULL";
            // Native target fields have engine detach handling; CAPEscort also
            // has H's swizzle/listener. No enemy/object search for diagnostics.
            const auto techno = dynamic_cast<const TechnoClass*>(p);
            return std::format("ptr={},kind={},rtti={},uid={},heap={},owner={},coord={}",
                static_cast<const void*>(p), p->Fetch_RTTI() == RTTI_CELL ? "CELL" : techno ? "TECHNO_OBJECT" : "OTHER",
                int(p->Fetch_RTTI()), p->ID, p->Fetch_Heap_ID(),
                techno && techno->House ? techno->House->Fetch_Heap_ID() : -1, XYZ(p->Center_Coord()));
        }
        Coord Center(AircraftClass* a)
        {
            const auto ext = Extension::Fetch(a);
            return ext->CAPMode == FixedWingCAPMode::Escort && ext->CAPEscort
                ? ext->CAPEscort->Center_Coord() : ext->CAPAnchor;
        }
        std::string State(AircraftClass* a)
        {
            const auto ext = Extension::Fetch(a);
            const auto fly = Locomotor(a);
            return std::format("mission={} queue={} status={} Ammo={} pos={} facingRaw={} CAPMode={} CAPMission={} CAPAnchor={} CAPEscort=[{}] rearming={} CAPOwner={} NavCom=[{}] ArchiveTarget=[{}] TarCom=[{}] FlyDestination={} FlyHeadTo={}",
                int(a->CurrentMission), int(a->MissionQueue), a->Status, a->Ammo, XYZ(a->PositionCoord),
                int(a->PrimaryFacing.Current().Get_Raw()), int(ext->CAPMode), int(ext->CAPMission), XYZ(ext->CAPAnchor),
                Ref(ext->CAPEscort), ext->CAPRearming, ext->CAPOwner, Ref(a->NavCom), Ref(a->ArchiveTarget), Ref(a->TarCom),
                fly ? XYZ(fly->DestinationCoord) : "NO_FLY", fly ? XYZ(fly->HeadToCoord) : "NO_FLY");
        }
        void Log(AircraftClass* a, const char* event, const std::string& details)
        {
            DEV_DEBUG_INFO("[FWCAP] frame={} aircraft(uid={},heap={}) event={} {} state: {}\n",
                Frame, a->ID, a->Fetch_Heap_ID(), event, details, State(a));
        }
        void Escort_State(AircraftClass* a, const char* source, bool force = false)
        {
            if (!Enabled()) return;
            const auto ext = Extension::Fetch(a);
            auto& w = Get(a);
            if (ext->CAPMode != FixedWingCAPMode::Escort && w.EscortRef.empty()) return;
            const auto t = ext->CAPEscort;
            // Deliberately exclude moving coordinates from the signature.
            const auto identity = std::format("mode={} mission={} anchor={} escort={} owner={} active={} limbo={} strength={} current={} queued={} status={} rearm={} Nav={} Archive={} Tar={}",
                int(ext->CAPMode), int(ext->CAPMission), XYZ(ext->CAPAnchor), static_cast<const void*>(t),
                t && t->House ? t->House->Fetch_Heap_ID() : -1, t && t->IsActive, t && t->IsInLimbo,
                t ? t->Strength : -1, int(a->CurrentMission), int(a->MissionQueue), a->Status, ext->CAPRearming,
                static_cast<const void*>(a->NavCom), static_cast<const void*>(a->ArchiveTarget), static_cast<const void*>(a->TarCom));
            if (!force && identity == w.Identity) return;
            Log(a, "ESCORT_STATE", std::format("source={} escortValid={} effectiveCAPCenter={} previousEscort=[{}]",
                source, ext->CAPMode == FixedWingCAPMode::Escort && CAP_Valid(a),
                w.HaveCenter ? XYZ(w.Center) : "UNSELECTED", w.EscortRef));
            w.Identity = identity;
            w.EscortRef = ext->CAPMode == FixedWingCAPMode::Escort ? Ref(t) : "";
            if (t && t->Fetch_RTTI() != RTTI_AIRCRAFT && t->Fetch_RTTI() != RTTI_UNIT
                && t->Fetch_RTTI() != RTTI_INFANTRY && t->Fetch_RTTI() != RTTI_BUILDING) {
                Log(a, "ESCORT_INVALID", "reason=UNSUPPORTED_RTTI_OBSERVED diagnosticOnly=true noNewTypeRejection=true");
            }
        }
        void First(AircraftClass* a, bool flight)
        {
            if (!Enabled()) return;
            auto& w = Get(a);
            bool& seen = flight ? w.FirstFlight : w.FirstAI;
            Escort_State(a, flight ? "CAP_FLIGHT" : "CAP_AI", !seen);
            seen = true;
        }
        void Set(AircraftClass* a, AbstractClass* target)
        {
            if (!Enabled()) return;
            auto& w = Get(a);
            w.FirstAI = w.FirstFlight = w.HaveCenter = false;
            w.Identity.clear(); w.EscortRef.clear();
            const auto ext = Extension::Fetch(a);
            Log(a, ext->CAPMode == FixedWingCAPMode::Escort ? "CAP_SET_ESCORT" : "CAP_SET_STATIC",
                std::format("commandTarget=[{}] commandDestination=[{}] actualBeginTarget=[{}]",
                    w.CommandTarget, w.CommandDestination, Ref(target)));
        }
        void Queued(AircraftClass* a, const AbstractClass* target, const char* source)
        {
            if (!Enabled()) return;
            Log(a, "COMMAND_QUEUED", std::format("source={} mission={} actualTarget=[{}] destination=NULL localUIObservationOnly=true",
                source, int(MISSION_GUARD_AREA), Ref(target)));
        }
        void Invalid(AircraftClass* a, const char* source, bool returning)
        {
            if (!Enabled()) return;
            const auto ext = Extension::Fetch(a);
            if (ext->CAPMode != FixedWingCAPMode::Escort) return;
            const auto t = ext->CAPEscort;
            const char* reason = "OTHER_EXISTING_REJECTION";
            if (!a->House || ext->CAPOwner != a->House->Fetch_Heap_ID()) reason = "AIRCRAFT_OWNER_CHANGED";
            else if (!t) reason = "NULL";
            else if (t == a) reason = "SELF";
            else if (!t->IsActive) reason = "INACTIVE";
            else if (t->IsInLimbo) reason = "LIMBO";
            else if (t->Strength <= 0) reason = "DEAD";
            else if (t->House != a->House) reason = "OWNER_CHANGED";
            else if (t->Fetch_RTTI() != RTTI_AIRCRAFT && t->Fetch_RTTI() != RTTI_UNIT
                && t->Fetch_RTTI() != RTTI_INFANTRY && t->Fetch_RTTI() != RTTI_BUILDING) reason = "UNSUPPORTED_RTTI_OBSERVED";
            Log(a, "ESCORT_INVALID", std::format("source={} reason={} previousEscort=[{}]", source, reason, Get(a).EscortRef));
            if (returning) Log(a, "ESCORT_ABORT_RTB", std::format("source={} reason={} existingReturnPath=true", source, reason));
        }
        void Leg(AircraftClass* a, const Coord& requested, const Cell& anchor, const Coord (&points)[4], int corner, int requested_corner)
        {
            if (!Enabled()) return;
            auto& w = Get(a);
            const auto ext = Extension::Fetch(a);
            const Coord used = anchor.As_Coord();
            if (ext->CAPMode == FixedWingCAPMode::Escort && (!w.HaveCenter || w.Center != used)) {
                Log(a, "ESCORT_CENTER_UPDATE", std::format("escort=[{}] previousCenter={} requestedCenter={} newCenter={} corner={} nextGoal={}",
                    Ref(ext->CAPEscort), w.HaveCenter ? XYZ(w.Center) : "UNOBSERVED", XYZ(requested), XYZ(used), corner, XYZ(points[corner])));
            }
            Log(a, "CAP_GEOMETRY", std::format("requestedCenter={} usedCenter={} corner0=[{}] corner1=[{}] corner2=[{}] corner3=[{}] requestedCorner={} chosenCorner={} nextGoal={}",
                XYZ(requested), XYZ(used), Point(points[0]), Point(points[1]), Point(points[2]), Point(points[3]), requested_corner, corner, XYZ(points[corner])));
            w.Center = used; w.HaveCenter = true; w.Corner = corner;
            Escort_State(a, "CAP_CORNER_SELECTED", true);
        }
        void Acquired(AircraftClass* a, FlyLocomotionClass* fly)
        {
            if (!Enabled()) return;
            auto& w = Get(a);
            w.LastTarget = Ref(a->TarCom);
            Log(a, "AUTO_TARGET_ACQUIRED", std::format("CAPCenter={} corner={} CAPMovementTarget={} target=[{}] nativeAttack=true",
                XYZ(Center(a)), w.Corner, XYZ(fly->DestinationCoord), w.LastTarget));
        }
        void Clear(AircraftClass* a)
        {
            if (Enabled()) Escort_State(a, "CAP_CLEARED", true);
        }
    }

    bool Guard_Mission(MissionType mission)
    {
        return mission == MISSION_GUARD || mission == MISSION_GUARD_AREA;
    }

    bool CAP_Active(AircraftClass* a)
    {
        return Extension::Fetch(a)->CAPMode != FixedWingCAPMode::None;
    }

    bool CAP_Valid(AircraftClass* a)
    {
        const auto ext = Extension::Fetch(a);
        if (!CAP_Active(a) || !a->House || ext->CAPOwner != a->House->Fetch_Heap_ID()) return false;
        if (ext->CAPMode == FixedWingCAPMode::Escort) {
            const auto target = ext->CAPEscort;
            // On_Detach clears this swizzled reference before destruction.
            return target && target != a && target->IsActive && !target->IsInLimbo
                && target->Strength > 0 && target->House == a->House;
        }
        return Valid_Point(ext->CAPAnchor);
    }

    bool Begin_CAP(AircraftClass* a, MissionType mission, AbstractClass* target)
    {
        Cancel_CAP(a);
        auto ext = Extension::Fetch(a);
        const auto escort = dynamic_cast<TechnoClass*>(target);
        if (escort && escort != a) {
            if (!escort->IsActive || escort->IsInLimbo || escort->Strength <= 0 || escort->House != a->House) return false;
            ext->CAPMode = FixedWingCAPMode::Escort;
            ext->CAPEscort = escort;
        } else {
            ext->CAPMode = FixedWingCAPMode::Static;
            ext->CAPAnchor = target && target->Fetch_RTTI() == RTTI_CELL && target != a->Get_Target_Cell_Ptr()
                ? target->Center_Coord() : a->PositionCoord;
        }
        ext->CAPMission = mission;
        ext->CAPOwner = a->House->Fetch_Heap_ID();
        CAPDiagnostics::Set(a, target);
        return true;
    }

    bool CAP_Needs_Reload(AircraftClass* a)
    {
        return a->Ammo == 0 && a->Class->MaxAmmo > 0 && a->Is_Weapon_Equipped();
    }

    void Resume_CAP(AircraftClass* a);

    void Return_From_CAP(AircraftClass* a)
    {
        Request_Return(a);
        // Request_Return deliberately leaves grounded aircraft alone. A new
        // CAP with empty magazines still needs the normal Enter/service path.
        if (a->HeightAGL <= 0) {
            a->Assign_Mission(MISSION_ENTER);
            a->field_370 = true;
        }
        a->Commence();
        a->MissionClass::Timer = 0;
        CAPDiagnostics::Escort_State(a, "RETURN_COMMENCED", true);
    }

    bool CAP_Target_In_Area(AircraftClass* a)
    {
        const auto ext = Extension::Fetch(a);
        if (!CAP_Valid(a) || !a->TarCom) return false;
        const auto target = dynamic_cast<TechnoClass*>(a->TarCom);
        if (!target || target == a || target->Is_Inactive() || target->IsInLimbo
            || target->Strength <= 0 || target->In_Air() || !Valid_Point(target->Center_Coord())
            || !target->House || a->House->Is_Ally(target->House)) return false;
        const Coord center = ext->CAPMode == FixedWingCAPMode::Escort ? ext->CAPEscort->Center_Coord() : ext->CAPAnchor;
        int range = a->Threat_Range(ext->CAPMission == MISSION_GUARD ? 0 : 1);
        if (range <= 0) range = std::max(a->Weapon_Range(WEAPON_SLOT_PRIMARY), a->Weapon_Range(WEAPON_SLOT_SECONDARY));
        return range > 0 && Distance_Squared(center, target->Center_Coord()) <= int64_t(range) * range;
    }

    // Navigation cleanup at an explicit replacement or completed native attack.
    // Ammunition remains exclusively owned by native Aircraft mission/AI code.
    void Clear_Attack(AircraftClass* a, FlyLocomotionClass* fly)
    {
        a->IsLocked = false;
        a->Assign_Target(nullptr);
        a->NavCom = nullptr;
        Goal(fly, COORD_NONE);
    }
    void Go_Around(AircraftClass* a, FlyLocomotionClass* fly, bool lost)
    {
        Cruise(a, fly);
        a->field_370 = true;
        a->Status = lost ? ALTITUDE : DOWNWIND;
        fly->TargetSpeed = Get_Approach_Speed(a, a->Status);
        a->MissionClass::Timer = 0;
        if (lost) {
            Release(a);
            a->NavCom = nullptr;
            Goal(fly, COORD_NONE);
        } else {
            Approach approach;
            auto dock = Reserved(a);
            if (dock && Geometry(a, dock, approach)) Goal(fly, approach.S);
        }
    }

    bool Holding_Points(const Coord& anchor, Coord (&points)[4])
    {
        // The size, too, is reconstructed solely from the saved anchor/map.
        // All alternatives remain nondegenerate rectangles with turning room.
        constexpr int sizes[][2] = {
            {HOLDING_HALF_WIDTH, HOLDING_HALF_HEIGHT},
            {4 * CELL_LEPTON, 3 * CELL_LEPTON},
            {3 * CELL_LEPTON, 2 * CELL_LEPTON},
            {2 * CELL_LEPTON, 2 * CELL_LEPTON}
        };
        for (const auto& size : sizes) {
            const int w = size[0], h = size[1];
            points[0] = anchor + Coord(-w, -h);
            points[1] = anchor + Coord(w, -h);
            points[2] = anchor + Coord(w, h);
            points[3] = anchor + Coord(-w, h);
            bool valid = true;
            for (int i = 0; i < 4 && valid; ++i) {
                valid = Valid_Segment(points[i], points[(i + 1) % 4]);
                // Leave two cells of turning clearance around every corner.
                for (int x : {-2, 2}) for (int y : {-2, 2}) {
                    valid = valid && Valid_Point(points[i] + Coord(x * CELL_LEPTON, y * CELL_LEPTON));
                }
            }
            if (valid) return true;
        }
        return false;
    }

    bool Orbit_Points(const Coord& center, Cell& anchor, Coord (&points)[4])
    {
        anchor = center.As_Cell();
        bool found = Holding_Points(anchor.As_Coord(), points);
        if (!found) {
            // Expanding cell rings, then row-major tie breaking within a ring.
            // Relocate the complete rectangle; never clamp individual corners.
            const Cell origin = anchor;
            for (int radius = 1; radius < std::max(Map.XSize, Map.YSize) && !found; ++radius) {
                for (int y = std::max(0, origin.Y - radius); y <= std::min(Map.YSize - 1, origin.Y + radius) && !found; ++y) {
                    for (int x = std::max(0, origin.X - radius); x <= std::min(Map.XSize - 1, origin.X + radius); ++x) {
                        if (abs(x - origin.X) != radius && abs(y - origin.Y) != radius) continue;
                        const Cell candidate {short(x), short(y)};
                        if (Holding_Points(candidate.As_Coord(), points)) {
                            anchor = candidate;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!found) return false; // A map too small for this flight model is unsupported.
        Holding_Points(anchor.As_Coord(), points);
        return true;
    }

    bool Start_Holding(AircraftClass* a, FlyLocomotionClass* fly)
    {
        const Coord current = a->PositionCoord;
        Coord points[4];
        Cell anchor;
        if (!Orbit_Points(current, anchor, points)) return false;
        a->NavCom = &Map[anchor];
        int first = 0;
        for (int i = 1; i < 4; ++i) {
            if (Distance_Squared(current, points[i]) < Distance_Squared(current, points[first])) first = i;
        }
        Goal(fly, points[first]);
        a->Status = STACK;
        Cruise(a, fly);
        return true;
    }

    bool CAP_Leg(AircraftClass* a, FlyLocomotionClass* fly, int corner)
    {
        if (!CAP_Valid(a)) { CAPDiagnostics::Invalid(a, "CAP_LEG", true); return false; }
        const int diagnostic_requested_corner = corner;
        const auto ext = Extension::Fetch(a);
        const Coord center = ext->CAPMode == FixedWingCAPMode::Escort
            ? ext->CAPEscort->Center_Coord() : ext->CAPAnchor;
        Coord points[4];
        Cell anchor;
        if (!Orbit_Points(center, anchor, points)) {
            if (CAPDiagnostics::Enabled()) CAPDiagnostics::Log(a, "CAP_GEOMETRY",
                std::format("accepted=false requestedCenter={} noValidHoldingRectangle=true corners=NOT_COMMITTED", CAPDiagnostics::XYZ(center)));
            return false;
        }
        if (corner < 0) {
            corner = 0;
            for (int i = 1; i < 4; ++i) {
                if (Distance_Squared(a->PositionCoord, points[i]) < Distance_Squared(a->PositionCoord, points[corner])) corner = i;
            }
        }
        // NavCom is a geometry anchor only while the mission is Guard/GuardArea.
        // The protected Escort reference remains separate across Attack/Enter.
        a->NavCom = &Map[anchor];
        Goal(fly, points[corner]);
        a->Status = CAP_CORNER_0 + corner;
        CAPDiagnostics::Leg(a, center, anchor, points, corner, diagnostic_requested_corner);
        return true;
    }

    void Resume_CAP(AircraftClass* a)
    {
        auto ext = Extension::Fetch(a);
        if (!CAP_Valid(a)) { CAPDiagnostics::Invalid(a, "RESUME_CAP", true); Cancel_CAP(a); Return_From_CAP(a); return; }
        auto fly = Locomotor(a);
        if (!fly) { Cancel_CAP(a); return; }
        Clear_Attack(a, fly);
        ext->CAPRearming = false;
        a->Assign_Mission(ext->CAPMission);
        a->Commence();
        // Repeated Guard commands may not queue a different native mission.
        a->Status = CAP_INITIAL;
        a->field_370 = true;
        a->MissionClass::Timer = 0;
        CAPDiagnostics::Escort_State(a, "CAP_RESUME_COMMENCED", true);
    }

    bool CAP_Flight(AircraftClass* a, FlyLocomotionClass* fly)
    {
        // Guard is also the native production/idle mission. Only an accepted
        // explicit command may create the parent CAP context.
        if (!CAP_Active(a)) return false;
        CAPDiagnostics::First(a, true);
        if (!CAP_Valid(a)) { CAPDiagnostics::Invalid(a, "CAP_FLIGHT", true); Cancel_CAP(a); Return_From_CAP(a); return false; }
        auto ext = Extension::Fetch(a);
        if (ext->CAPRearming || CAP_Needs_Reload(a)) {
            ext->CAPRearming = true;
            Return_From_CAP(a);
            return false;
        }
        if (a->Status < CAP_CORNER_0 || a->Status > CAP_CORNER_3) {
            a->Assign_Target(nullptr);
            Release(a);
            if (!CAP_Leg(a, fly, -1)) { Cancel_CAP(a); Return_From_CAP(a); return false; }
        }
        a->field_370 = true;
        Cruise(a, fly);
        if (a->HeightAGL <= 0 && !fly->IsTakingOff) Take_Off(fly);
        return true;
    }

    bool Find_Dock(AircraftClass* a, FlyLocomotionClass* fly)
    {
        DockDiagnostics::Search trace(a);
        Approach approach;
        auto dock = Reserved(a);
        if (dock && Geometry(a, dock, approach)) {
            a->NavCom = dock;
            Goal(fly, approach.S);
            a->Status = DOWNWIND;
            trace.Success(dock);
            return true;
        }
        Release(a);
        // Preserve both the type-list order and the native per-type selection.
        for (int i = 0; i < a->Class->Dock.Count(); ++i) {
            trace.Type(a->Class->Dock[i]);
            dock = a->Find_Docking_Bay(a->Class->Dock[i], false, true);
            trace.Result(dock);
            if (!Dock_Candidate(a, dock)) continue;
            const auto reply = a->Transmit_Message(RADIO_HELLO, dock);
            DockDiagnostics::Radio_Result(a, dock, "RADIO_HELLO", reply);
            if (reply != RADIO_ROGER) continue;
            if (!Dock_Status(a, dock, true) || !Geometry(a, dock, approach)) {
                if (trace.Active) {
                    DockDiagnostics::Aircraft_State(a, "REJECT_POST_HELLO");
                    DockDiagnostics::Candidate(a, dock, "REJECT_POST_HELLO");
                }
                Release(a);
                continue;
            }
            a->NavCom = dock;
            Goal(fly, approach.S);
            a->Status = DOWNWIND;
            trace.Success(dock);
            return true;
        }
        return false;
    }

    void Finish_Move(AircraftClass* a)
    {
        // Do not call Assign_Destination(nullptr): the Foot implementation calls Stop_Moving.
        a->NavCom = nullptr;
        a->field_370 = true;
        if (a->MissionQueue != MISSION_NONE) {
            a->Commence();
            return;
        }
        if (a->House->Is_Human_Player() && a->NavQueue.Count() > 0) {
            a->Handle_Navigation_List();
            if (a->NavCom) {
                a->Assign_Mission(MISSION_MOVE);
                a->field_370 = true;
                a->Commence();
                return;
            }
        }
        a->Enter_Idle_Mode();
    }

    void Arrived(AircraftClass* a, FlyLocomotionClass* fly, const Coord& from, const Coord& to)
    {
        if (!Passed(from, to, fly->DestinationCoord, ARRIVAL)) return;
        if (a->CurrentMission == MISSION_MOVE) {
            if (a->HeightAGL > 0) Finish_Move(a);
            return;
        }
        if (Guard_Mission(a->CurrentMission) && CAP_Active(a)) {
            // Keep the committed leg unchanged in between waypoints. Only here
            // sample a moving escort's new position and select the next corner.
            const int corner = (a->Status - CAP_CORNER_0 + 1) % 4;
            if (!CAP_Leg(a, fly, corner)) { Cancel_CAP(a); Return_From_CAP(a); }
            return;
        }
        if (a->CurrentMission != MISSION_ENTER) return;
        if (a->Status == STACK && a->NavCom && a->NavCom->Fetch_RTTI() == RTTI_CELL) {
            Coord points[4];
            if (!Holding_Points(a->NavCom->Center_Coord(), points)) {
                Start_Holding(a, fly);
                return;
            }
            for (int i = 0; i < 4; ++i) {
                if (Distance_Squared(points[i], fly->DestinationCoord) == 0) {
                    Goal(fly, points[(i + 1) % 4]);
                    return;
                }
            }
            Goal(fly, points[0]);
            return;
        }
        auto dock = Reserved(a);
        Approach approach;
        if (!dock || !Geometry(a, dock, approach)) return;
        switch (a->Status) {
        case DOWNWIND:
            a->Status = CROSSWIND;
            Goal(fly, approach.G);
            break;
        case CROSSWIND:
            a->Status = TRAVEL;
            Goal(fly, approach.P);
            break;
        case TRAVEL:
            if (Heading_OK(a->PrimaryFacing.Current(), approach.Direction)
                && a->PositionCoord.Z >= approach.D.Z + a->Class->Flight_Level() && !fly->IsTakingOff) {
                a->Status = LANDING;
                // SecondaryFacing.Desired stores the committed eight-way heading.
                a->SecondaryFacing.Set_Desired(approach.Direction);
                fly->IsLanding = true;
                fly->WasLanding = false;
                fly->FlightLevel = 0;
                a->field_370 = false;
                Goal(fly, approach.D);
            } else Go_Around(a, fly, false);
            break;
        }
        a->MissionClass::Timer = 0;
    }

    void Tracker(AircraftClass* a)
    {
        auto ext = Extension::Fetch(static_cast<FootClass*>(a));
        const Cell old = ext->Get_Last_Flight_Cell();
        if (a->HeightAGL > 0) {
            if (old == CELL_NONE) AircraftTracker->Track(a);
            else if (old != a->Get_Cell()) AircraftTracker->Update_Position(a, old, a->Get_Cell());
        } else if (old != CELL_NONE) AircraftTracker->Untrack(a);
    }

    const char* Native_Attack_Origin(AircraftClass* a)
    {
        const auto mode = Extension::Fetch(a)->CAPMode;
        if (mode == FixedWingCAPMode::Escort) return "ESCORT";
        if (mode == FixedWingCAPMode::Static) return "CAP";
        return a->House && !a->House->Is_Human_Player() ? "AI" : "NATIVE";
    }

    bool Attack_Order(AircraftClass* a)
    {
        return a->MissionQueue == MISSION_ATTACK
            || (a->CurrentMission == MISSION_ATTACK && a->MissionQueue == MISSION_NONE);
    }

    void Begin_Native_Attack(AircraftClass* a, AbstractClass* target, const char* origin, int prior_cap = -1)
    {
        Extension::Fetch(a)->AttackControl = FixedWingAttackControl::NativePending;
        if (!CAPDiagnostics::Enabled()) return;
        auto& w = CAPDiagnostics::Get(a);
        w.NativeOrigin = origin;
        w.LastTarget = CAPDiagnostics::Ref(target);
        w.NativeObserved = true;
        w.NativeAmmo = a->Ammo;
        w.NativeHadTarget = target != nullptr;
        w.NativeMissionSeen = w.NativeFlySeen = false;
        const TargetClass reference(target);
        CAPDiagnostics::Log(a, "NATIVE_ATTACK_HANDOFF", std::format(
            "origin={} mission={} target=[{}] targetKind={} targetUID={} Ammo={} position={} facingRaw={} wasDocked={} airborne={} priorCAPMode={} stage=PENDING",
            origin, int(MISSION_ATTACK), w.LastTarget, int(reference.Kind()), reference.Value(), a->Ammo,
            CAPDiagnostics::XYZ(a->PositionCoord), int(a->PrimaryFacing.Current().Get_Raw()),
            Reserved(a) != nullptr && a->HeightAGL <= 0, a->HeightAGL > 0, prior_cap));
    }

    // All Attack origins share the runtime-confirmed H6A startup/ownership adapter.
    // Only native code owns attack approach, movement, firing and ammo.
    void Update_Native_Attack(AircraftClass* a, FlyLocomotionClass* fly)
    {
        if (!Enabled(a) || !fly || !a->IsActive || a->IsInLimbo || a->Strength <= 0) return;
        auto ext = Extension::Fetch(a);
        // A queued replacement wins over the still-current Attack. Never enroll
        // it again between Command clearing ownership and the new Commence.
        if (!Native_Attack(a) && Attack_Order(a)) {
            Begin_Native_Attack(a, a->TarCom, Native_Attack_Origin(a));
        }
        if (!Native_Attack(a)) return;
        if (ext->AttackControl == FixedWingAttackControl::NativePending) {
            if (a->MissionQueue == MISSION_ATTACK) {
                a->field_370 = true;
                a->Commence();
            }
            if (a->CurrentMission != MISSION_ATTACK) return;
            ext->AttackControl = a->TarCom && a->Ammo != 0
                && (fly->IsTakingOff || a->HeightAGL < a->Class->Flight_Level())
                ? FixedWingAttackControl::NativeTakeoff : FixedWingAttackControl::Native;
            a->IsLocked = false;
            a->Status = 0; // Native VALIDATE_AZ.
            a->MissionClass::Timer = 0;
            a->field_370 = true;
            a->NavCom = nullptr;
            Goal(fly, COORD_NONE);
            if (a->TarCom && a->Ammo != 0) {
                Cruise(a, fly);
                // Re-enter the ORIGINAL Aircraft destination method: it wakes
                // dock locomotion, releases service radio and calls Fly::Move_To.
                a->Assign_Destination(a->TarCom);
            }
            if (ext->AttackControl == FixedWingAttackControl::Native && CAPDiagnostics::Enabled()) {
                const bool airborne = !fly->IsTakingOff && a->HeightAGL >= a->Class->Flight_Level();
                CAPDiagnostics::Log(a, airborne ? "NATIVE_ATTACK_AIRBORNE" : "NATIVE_ATTACK_RELEASE",
                    std::format("alreadyAirborne={} nativeMissionAndFly=true", airborne));
            }
        }
        if (ext->AttackControl == FixedWingAttackControl::NativeEnding
            && a->CurrentMission == MISSION_ATTACK && a->MissionQueue == MISSION_NONE) {
            // Native Idle retained Attack rather than selecting another mission.
            ext->AttackControl = FixedWingAttackControl::Native;
        }
        if (a->CurrentMission != MISSION_ATTACK && a->MissionQueue != MISSION_ATTACK) {
            const bool completed = ext->AttackControl == FixedWingAttackControl::NativeEnding
                || (Guard_Mission(a->CurrentMission) && a->CurrentMission == ext->CAPMission
                    && a->MissionQueue == MISSION_NONE);
            const bool parent = CAP_Active(a);
            ext->AttackControl = FixedWingAttackControl::Custom;
            if (parent && completed) {
                if (!CAP_Valid(a)) {
                    CAPDiagnostics::Invalid(a, "NATIVE_ATTACK_END", true);
                    Cancel_CAP(a);
                    Return_From_CAP(a);
                    return;
                }
                if (a->Ammo != 0) {
                    Resume_CAP(a);
                    // Existing CAP scan cadence, set after the native mission
                    // returned; flight/escort-center progression is not delayed.
                    a->MissionClass::Timer = DOCK_RETRY;
                    if (CAPDiagnostics::Enabled()) {
                        const auto& w = CAPDiagnostics::Get(a);
                        CAPDiagnostics::Log(a, "NATIVE_ATTACK_RESUME_CAP", std::format(
                            "origin={} Ammo={} currentCenter={} priorTarget=[{}]",
                            Native_Attack_Origin(a), a->Ammo, CAPDiagnostics::XYZ(CAPDiagnostics::Center(a)), w.LastTarget));
                    }
                    return;
                }
                ext->CAPRearming = true;
                if (a->CurrentMission != MISSION_ENTER && a->CurrentMission != MISSION_SLEEP) {
                    // Native no-dock Idle may choose Move; CAP's established
                    // rearm path instead keeps its parent while returning/holding.
                    Return_From_CAP(a);
                }
            } else if (parent) {
                // AI/script mission replacement did not finish through native
                // Idle. Do not resurrect its old CAP over the new order.
                Cancel_CAP(a);
            }
            if (CAPDiagnostics::Enabled()) {
                CAPDiagnostics::Log(a, a->CurrentMission == MISSION_ENTER || a->MissionQueue == MISSION_ENTER
                    ? "NATIVE_ATTACK_RTB" : "NATIVE_ATTACK_END", std::format(
                        "origin={} nativeIdleCompleted={} customControlRestored=true",
                        CAPDiagnostics::Get(a).NativeOrigin, completed));
            }
            if (a->CurrentMission == MISSION_ENTER) {
                // Native Idle chose Return. Its Attack Status must not become
                // a FixedWing approach phase, nor may a native landing continue.
                a->Status = INITIAL;
                a->MissionClass::Timer = 0;
                a->field_370 = true;
                Goal(fly, COORD_NONE);
                Cruise(a, fly);
            }
            return;
        }
        if (ext->AttackControl == FixedWingAttackControl::NativeTakeoff
            && (!a->TarCom || a->Ammo == 0
                || (!fly->IsTakingOff && a->HeightAGL >= a->Class->Flight_Level()))) {
            ext->AttackControl = FixedWingAttackControl::Native;
            a->Status = 0;
            a->MissionClass::Timer = 0;
            fly->HeadToCoord = COORD_NONE;
            if (CAPDiagnostics::Enabled()) {
                CAPDiagnostics::Log(a, !fly->IsTakingOff && a->HeightAGL >= a->Class->Flight_Level()
                    ? "NATIVE_ATTACK_AIRBORNE" : "NATIVE_ATTACK_RELEASE", std::format(
                    "alreadyAirborne=false takeoffComplete={} nativeMissionAndFly=true",
                    !fly->IsTakingOff && a->HeightAGL >= a->Class->Flight_Level()));
            }
        }
    }

    void Observe_Native_Attack(AircraftClass* a)
    {
        if (!Native_Attack(a) || !CAPDiagnostics::Enabled()) return;
        auto& w = CAPDiagnostics::Get(a);
        if (!w.NativeObserved) {
            // Load/developer-mode changes establish a baseline, not fake ammo use.
            w.NativeObserved = true;
            w.NativeAmmo = a->Ammo;
            w.NativeHadTarget = a->TarCom != nullptr;
            w.LastTarget = CAPDiagnostics::Ref(a->TarCom);
            w.NativeOrigin = Native_Attack_Origin(a);
        }
        if (w.NativeAmmo != a->Ammo) {
            CAPDiagnostics::Log(a, "NATIVE_ATTACK_AMMO_CHANGE", std::format(
                "previousAmmo={} Ammo={} observationalOnly=true", w.NativeAmmo, a->Ammo));
            w.NativeAmmo = a->Ammo;
        }
        if (w.NativeHadTarget && !a->TarCom) {
            CAPDiagnostics::Log(a, "NATIVE_ATTACK_TARGET_GONE", "nativeTargetCleared=true noCustomAbort=true");
            w.NativeHadTarget = false;
        }
    }

    void Trace_Native_Entry(AircraftClass* a, bool locomotor)
    {
        if (!CAPDiagnostics::Enabled()) return;
        auto& w = CAPDiagnostics::Get(a);
        bool& seen = locomotor ? w.NativeFlySeen : w.NativeMissionSeen;
        if (seen) return;
        seen = true;
        CAPDiagnostics::Log(a, locomotor ? "NATIVE_ATTACK_FLY_ENTER" : "NATIVE_ATTACK_MISSION_ENTER",
            "originalEntryTrampoline=true fullNativeBody=true");
    }
}

void Trace_CAP_Command(AircraftClass* a, MissionType mission, const EventClass* event)
{
    if (!CAPDiagnostics::Enabled() || !Enabled(a) || !event) return;
    // MEGAMISSION_F has the same Whom/Mission/Target/Destination prefix.
    // Decode only those real payload references, never search for a nearby unit.
    const auto& data = event->Data.MegaMission;
    auto& w = CAPDiagnostics::Get(a);
    w.CommandTarget = CAPDiagnostics::Ref(data.Target.As_Abstract());
    w.CommandDestination = CAPDiagnostics::Ref(data.Destination.As_Abstract());
    CAPDiagnostics::Log(a, "COMMAND_RECEIVED", std::format("source=ACCEPTED_SYNCHRONIZED_EVENT eventType={} eventFrame={} eventOwner={} newMission={} payloadMission={} whom(kind={},value={}) target(kind={},value={},resolved=[{}]) destination(kind={},value={},resolved=[{}]) targetPayload={} priorCAPMode={}",
        int(event->Type), event->Frame, event->ID, int(mission), int(data.Mission), int(data.Whom.Kind()), data.Whom.Value(),
        int(data.Target.Kind()), data.Target.Value(), w.CommandTarget, int(data.Destination.Kind()), data.Destination.Value(), w.CommandDestination,
        data.Target.Kind() == RTTI_CELL ? "CELL_ONLY_NO_OBJECT_RECONSTRUCTION" : data.Target.Is_Valid() ? "OBJECT_OR_OTHER_REFERENCE" : "NONE",
        int(Extension::Fetch(a)->CAPMode)));
}

void Trace_CAP_State(AircraftClass* a, const char* source)
{
    if (!CAPDiagnostics::Enabled() || !a) return;
    if (Extension::Fetch(a)->CAPMode == FixedWingCAPMode::Escort && !Extension::Fetch(a)->CAPEscort) {
        CAPDiagnostics::Invalid(a, source, false);
    }
    CAPDiagnostics::Escort_State(a, source, true);
}

void Reset_CAP_Trace()
{
    CAPDiagnostics::Watches.clear();
}

bool Supported_Type(const AircraftTypeClass* type)
{
    if (!type) return false;
    const auto ext = Extension::Fetch(type);
    return !type->IsCarryall && !type->IsDropship && !type->IsHunterSeeker
        && type->Max_Passengers() == 0 && !ext->IsSpawned && !ext->IsMissileSpawn
        && type->Locomotor == __uuidof(FlyLocomotionClass) && type->Flight_Level() > 0
        && type->MaxSpeed > 0 && type->ROT > 0;
}

bool Enabled(const AircraftClass* a)
{
    return a && Supported_Type(a->Class) && Extension::Fetch(a->Class)->IsFixedWing
        && !a->Passenger && !a->Cargo.Is_Something_Attached() && !a->IsKamikaze
        && !Extension::Fetch(a)->SpawnOwner;
}

bool Native_Attack(const AircraftClass* a)
{
    return a && Extension::Fetch(a)->AttackControl != FixedWingAttackControl::Custom;
}

void Native_Attack_Idle(AircraftClass* a)
{
    if (!Enabled(a) || !Native_Attack(a)) return;
    if (Extension::Fetch(a)->AttackControl == FixedWingAttackControl::NativePending) return;
    // The original Idle body still chooses the next mission. Record only its
    // completion provenance so a later script Move cannot be mistaken for it.
    Extension::Fetch(a)->AttackControl = FixedWingAttackControl::NativeEnding;
}

FlyLocomotionClass* Locomotor(AircraftClass* a)
{
    if (!a || !a->Locomotion) return nullptr;
    IPersistStream* persist = nullptr;
    if (FAILED(a->Locomotion->QueryInterface(__uuidof(IPersistStream), reinterpret_cast<void**>(&persist))) || !persist) return nullptr;
    CLSID id;
    const HRESULT result = persist->GetClassID(&id);
    persist->Release();
    if (FAILED(result) || id != __uuidof(FlyLocomotionClass)) return nullptr;
    return static_cast<FlyLocomotionClass*>(static_cast<ILocomotion*>(a->Locomotion));
}

AircraftClass* Controlled(FlyLocomotionClass* fly)
{
    if (!fly || !fly->LinkedTo || fly->LinkedTo->Fetch_RTTI() != RTTI_AIRCRAFT) return nullptr;
    auto a = static_cast<AircraftClass*>(fly->LinkedTo);
    // Release the FULL native Fly::Process (including rotation/world edges),
    // not just Mission_Attack. Pending/startup still uses the FixedWing takeoff.
    if (Extension::Fetch(a)->AttackControl == FixedWingAttackControl::Native
        || Extension::Fetch(a)->AttackControl == FixedWingAttackControl::NativeEnding) return nullptr;
    // field_4B is the native tumbling flag. Keep crashes and failures native.
    if (!Enabled(a) || !a->IsActive || a->IsInLimbo || a->Strength <= 0
        || a->EMPFramesRemaining > 0 || !fly->IsPowered || fly->field_4B
        || a->CurrentMission == MISSION_RETREAT) return nullptr;
    // Preflight before changing any native state. Invalid array trajectories use
    // the original failure/edge handling, just like a crashed or tumbling flyer.
    // This duplicates Apparent_Speed's full-speed upper bound, not its movement.
    const int full_speed = Full_Speed(a);
    DirType direction = a->PrimaryFacing.Current();
    const Coord next = Coord_Move(a->PositionCoord, direction, std::max({1, full_speed, fly->Apparent_Speed()}));
    return Inside_Array(a->PositionCoord) && Inside_Array(next) ? a : nullptr;
}

bool Dock_Candidate(AircraftClass* a, BuildingClass* dock)
{
    Approach approach;
    return Dock_Status(a, dock, false) && Geometry(a, dock, approach);
}

bool Has_Compatible_Dock(AircraftClass* a)
{
    if (!a) return false;
    // Presence deliberately ignores Radio, occupancy, power and approach reservation.
    // A completed compatible base remains a base while another aircraft uses it.
    for (int i = 0; i < Buildings.Count(); ++i) {
        if (Dock_Exists(a, Buildings[i])) return true;
    }
    return false;
}

bool No_Dock_AI(AircraftClass* a)
{
    // Run from Aircraft::AI, including parked/EMP-disabled aircraft, not from
    // a mission or Controlled(): neither orders nor loss of flight power pause it.
    if (!a || !a->IsActive || a->IsInLimbo || a->Strength <= 0 || !Enabled(a)) return false;
    auto ext = Extension::Fetch(a);
    if (Has_Compatible_Dock(a)) {
        if (ext->FixedWingNoDockTimer != 0) {
            if (DockDiagnostics::Enabled()) {
                DockDiagnostics::Aircraft_State(a, "NO_DOCK_PRESENCE_RESTORED");
                DockDiagnostics::Inventory(a, "NO_DOCK_PRESENCE_RESTORED");
            }
            ext->FixedWingNoDockTimer = 0;
            if (Vinifera_DeveloperMode) {
                DEV_DEBUG_INFO("FixedWing {}: compatible dock restored; no-dock timer reset.\n", a->Fetch_Heap_ID());
            }
        }
        return false;
    }
    if (ext->FixedWingNoDockTimer == 0) {
        ext->FixedWingNoDockTimer = NO_DOCK_GRACE;
        if (Vinifera_DeveloperMode) {
            DEV_DEBUG_INFO("FixedWing {}: no-dock timer started ({} ticks).\n", a->Fetch_Heap_ID(), NO_DOCK_GRACE);
        }
        return false;
    }
    if (ext->FixedWingNoDockTimer > 1) {
        --ext->FixedWingNoDockTimer;
        return false;
    }

    const int id = a->Fetch_Heap_ID();
    // On_Death is the native Aircraft crash path. It handles kill recording,
    // triggers, radio release, Stun and tumbling; do not touch a after it returns.
    if (a->HeightAGL > 0) {
        a->On_Death(nullptr);
        if (Vinifera_DeveloperMode) {
            DEV_DEBUG_INFO("FixedWing {}: no-dock timer expired; native aircraft crash.\n", id);
        }
        return true;
    }
    // On_Death cannot crash a grounded aircraft. Use its normal damage/death
    // sequence instead, including explosion and the native removal fallback.
    int damage = a->Strength;
    const auto result = a->Take_Damage(damage, 0, Rule->C4Warhead, nullptr, true, false);
    if (result == RESULT_DESTROYED || result == RESULT_ALREADY_DESTROYED) {
        if (Vinifera_DeveloperMode) {
            DEV_DEBUG_INFO("FixedWing {}: no-dock timer expired; native ground death.\n", id);
        }
        return true;
    }
    // Native damage protection may refuse ground damage. Keep expiry pending,
    // without restarting the grace period or emitting per-frame logs. Always
    // leave this AI call after damage, even if a trigger affected its lifetime.
    return true;
}

void Initialize_Spawn(AircraftClass* a)
{
    auto fly = Locomotor(a);
    if (!fly) return;
    fly->IsLanding = fly->WasLanding = fly->IsTakingOff = false;
    fly->FlightLevel = a->Class->Flight_Level();
    fly->IsMoving = a->HeightAGL > 0;
    fly->CurrentSpeed = fly->TargetSpeed = fly->IsMoving ? 1.0 : 0.0;
    a->Set_Speed(fly->CurrentSpeed);
    if (fly->IsMoving) Tracker(a);
}

void Cancel_CAP(AircraftClass* a)
{
    auto ext = Extension::Fetch(a);
    if (ext->CAPMode != FixedWingCAPMode::None) {
        a->ArchiveTarget = nullptr;
        // Native retaliation may have saved Guard in the one-deep override
        // slot. It must not resurrect a canceled parent order after service.
        if (Guard_Mission(a->SuspendedMission)) {
            a->SuspendedMission = MISSION_NONE;
            a->SuspendedTarCom = nullptr;
            a->SuspendedNavCom = nullptr;
        }
    }
    ext->CAPMode = FixedWingCAPMode::None;
    ext->CAPMission = MISSION_NONE;
    ext->CAPAnchor = COORD_NONE;
    ext->CAPEscort = nullptr;
    ext->CAPRearming = false;
    ext->CAPOwner = -1;
    CAPDiagnostics::Clear(a);
}

void Command(AircraftClass* a, MissionType mission, AbstractClass* target)
{
    if (!Enabled(a)) return;
    const auto ext = Extension::Fetch(a);
    const auto prior_cap = ext->CAPMode;
    ext->AttackControl = FixedWingAttackControl::Custom;
    // The accepted MEGAMISSION, not a local UI callback, distinguishes an
    // explicit Attack from a Target_Something_Nearby/retaliation attack.
    Cancel_CAP(a);
    if (mission == MISSION_ATTACK) {
        Begin_Native_Attack(a, target, a->House && a->House->Is_Human_Player() ? "MANUAL" : "AI", int(prior_cap));
        return;
    }
    if (!Guard_Mission(mission)) return;
    a->Clear_Navigation_List();
    a->CurrentPath = -1;
    a->NextWaypoint = 0;
    Begin_CAP(a, mission, target);
    a->Status = CAP_INITIAL;
    a->field_370 = true;
    a->MissionClass::Timer = 0;
    CAPDiagnostics::Escort_State(a, "AFTER_COMMAND", true);
}

void CAP_AI(AircraftClass* a)
{
    if (!a || !a->IsActive || a->IsInLimbo || a->Strength <= 0) return;
    if (!CAP_Active(a)) return;
    CAPDiagnostics::First(a, false);
    if (!Enabled(a)) { Cancel_CAP(a); return; }
    auto fly = Locomotor(a);
    if (!fly) { Cancel_CAP(a); return; }
    // Native target/flight ownership continues even if an Escort is destroyed.
    // Parent validity and resume/RTB are evaluated after native Attack ends.
    if (Native_Attack(a) || a->CurrentMission == MISSION_ATTACK || a->MissionQueue == MISSION_ATTACK) return;
    if (!CAP_Valid(a)) {
        CAPDiagnostics::Invalid(a, "CAP_AI", true); Cancel_CAP(a); Return_From_CAP(a); return;
    }
    const auto ext = Extension::Fetch(a);
    // Direct AI/script movement orders also replace CAP. Native automatic
    // Attack, Enter/Sleep for rearming and the service's Guard handoff retain it.
    const auto queued = a->MissionQueue;
    if (queued == MISSION_NONE && !Guard_Mission(a->CurrentMission) && a->CurrentMission != MISSION_ATTACK
        && !(ext->CAPRearming && (a->CurrentMission == MISSION_ENTER || a->CurrentMission == MISSION_SLEEP))) {
        Cancel_CAP(a);
        return;
    }
    if (queued != MISSION_NONE && !Guard_Mission(queued) && queued != MISSION_ATTACK
        && !(ext->CAPRearming && (queued == MISSION_ENTER || queued == MISSION_SLEEP))) {
        Cancel_CAP(a);
        return;
    }
    const auto dock = ext->CAPRearming ? Reserved(a) : nullptr;
    if (dock && (Guard_Mission(a->CurrentMission)
            || (a->CurrentMission == MISSION_SLEEP && !dock->Class->IsCanUnitRepair))) Resume_After_Service(a);
}

bool Resume_After_Service(AircraftClass* a)
{
    if (!Enabled(a) || !CAP_Active(a) || !Extension::Fetch(a)->CAPRearming
        || !CAP_Valid(a) || a->Ammo < a->Class->MaxAmmo || a->HeightAGL > 0) return false;
    auto fly = Locomotor(a);
    if (!fly || fly->IsMoving || fly->IsTakingOff || fly->IsLanding || !Reserved(a)
        || Controlled(fly) != a) return false;
    Resume_CAP(a);
    return true;
}

int Mission_CAP(AircraftClass* a)
{
    auto fly = Locomotor(a);
    if (!fly || !CAP_Flight(a, fly)) return 1;
    if (!fly->IsTakingOff && a->HeightAGL >= a->Class->Flight_Level()
        && a->Ammo != 0 && a->Is_Weapon_Equipped()) {
        const auto ext = Extension::Fetch(a);
        Coord center = ext->CAPMode == FixedWingCAPMode::Escort ? ext->CAPEscort->Center_Coord() : ext->CAPAnchor;
        // The same native acquisition used by Foot Guard / GuardArea, including
        // Vinifera's replacement, GuardRange, NoAutoFire and legality rules.
        a->Target_Something_Nearby(center, ext->CAPMission == MISSION_GUARD ? THREAT_RANGE : THREAT_AREA);
        if (CAP_Target_In_Area(a)) {
            CAPDiagnostics::Acquired(a, fly);
            a->Assign_Mission(MISSION_ATTACK);
            a->field_370 = true;
            a->Commence();
            Begin_Native_Attack(a, a->TarCom, Native_Attack_Origin(a));
            Update_Native_Attack(a, fly);
            CAPDiagnostics::Escort_State(a, "NATIVE_ATTACK_COMMENCED", true);
        } else a->Assign_Target(nullptr);
    }
    // Deterministic scan cadence. Flight/waypoint progression runs every frame.
    return DOCK_RETRY;
}

void Request_Return(AircraftClass* a)
{
    auto fly = Locomotor(a);
    if (!fly) return;
    Extension::Fetch(a)->AttackControl = FixedWingAttackControl::Custom;
    if (a->CurrentMission == MISSION_ATTACK) Clear_Attack(a, fly);
    if (a->HeightAGL <= 0) return;
    Cruise(a, fly);
    a->NavCom = nullptr;
    Goal(fly, COORD_NONE);
    a->field_370 = true;
    if (a->CurrentMission == MISSION_ENTER) a->Status = ALTITUDE;
    else a->Assign_Mission(MISSION_ENTER);
    a->MissionClass::Timer = 0;
}

bool Idle(AircraftClass* a)
{
    auto fly = Locomotor(a);
    if (!fly) return false;
    if (CAP_Active(a) && Extension::Fetch(a)->CAPRearming && CAP_Valid(a)) {
        if (Resume_After_Service(a)) return true;
        if (a->CurrentMission == MISSION_ATTACK) {
            Return_From_CAP(a);
            return true;
        }
    }
    if (!CAP_Active(a) && a->HeightAGL <= 0 && Reserved(a) && Guard_Mission(a->MissionQueue)) {
        // The reload facility's internal Guard -> Idle handoff is not a new
        // Guard order. A manual flight must remain parked after normal service.
        a->Commence();
        // Continue through ordinary grounded Idle/navigation handling below.
    }
    a->field_370 = true;
    if (a->MissionQueue != MISSION_NONE) {
        a->Commence();
        return true;
    }
    if (a->CurrentMission == MISSION_ATTACK) Clear_Attack(a, fly);
    if (!CAP_Active(a) && a->House->Is_Human_Player() && a->NavQueue.Count() > 0) {
        a->NavCom = nullptr;
        a->Handle_Navigation_List();
        if (a->NavCom) {
            a->Assign_Mission(MISSION_MOVE);
            a->Commence();
            return true;
        }
    }
    if (a->HeightAGL <= 0 && !fly->IsTakingOff) {
        fly->CurrentSpeed = fly->TargetSpeed = 0.0;
        fly->IsMoving = false;
        a->NavCom = nullptr;
        Goal(fly, COORD_NONE);
        a->Set_Speed(0.0);
        a->Assign_Mission(Reserved(a) ? MISSION_SLEEP : MISSION_GUARD);
        a->Commence();
    } else {
        Request_Return(a);
        a->Commence();
    }
    return true;
}

void Assign_Destination(AircraftClass* a, const AbstractClass* target, bool update)
{
    auto fly = Locomotor(a);
    if (!fly) return;
    const auto guard = Guard_Mission(a->MissionQueue) ? a->MissionQueue : a->CurrentMission;
    if (CAP_Active(a) && Guard_Mission(guard) && (Guard_Mission(a->MissionQueue)
            || a->MissionQueue == MISSION_NONE)
        && !Extension::Fetch(a)->CAPRearming) {
        // Command already captured the accepted event's exact cell/object.
        // Native destination writes alone carry no CAP order provenance.
        a->NavCom = nullptr;
        a->IsLocked = false;
        Goal(fly, COORD_NONE);
        a->field_370 = true;
        if (Guard_Mission(a->MissionQueue)) a->Commence();
        a->Status = CAP_INITIAL;
        a->MissionClass::Timer = 0;
        CAPDiagnostics::Escort_State(a, "GUARD_DESTINATION_AFTER_COMMENCE", true);
        return;
    }
    auto dock = Reserved(a);
    if (target && target == dock && a->HeightAGL <= 0 && !fly->IsTakingOff
        && Distance_Squared(a->PositionCoord, dock->Docking_Coord()) <= int64_t(TOUCHDOWN) * TOUCHDOWN) {
        a->NavCom = dock;
        a->Status = INITIAL;
        a->MissionClass::Timer = 0;
        return;
    }
    // Native Process_Landing clears NavCom after completing all ground bookkeeping.
    if (!target && a->HeightAGL <= 0 && !fly->IsMoving && !fly->IsTakingOff) {
        a->NavCom = nullptr;
        Goal(fly, COORD_NONE);
        return;
    }
    if (target != a->Radio) Release(a);
    Cruise(a, fly);
    a->field_370 = true;
    a->Status = INITIAL;
    a->MissionClass::Timer = 0;
    if (target) a->FootClass::Assign_Destination(target, update);
    else {
        a->NavCom = nullptr;
        if (a->HeightAGL > 0) Request_Return(a);
        else {
            fly->IsMoving = false;
            fly->CurrentSpeed = fly->TargetSpeed = 0.0;
            Goal(fly, COORD_NONE);
        }
    }
}

int Mission_Move(AircraftClass* a)
{
    auto fly = Locomotor(a);
    if (!fly) return 1;
    a->field_370 = true;
    if (!a->NavCom) { Finish_Move(a); return 1; }
    // TSpp's name for the native virtual Destination_Coord slot.
    Goal(fly, a->NavCom->entry_50());
    Cruise(a, fly);
    if (a->HeightAGL <= 0 && !fly->IsTakingOff) Take_Off(fly);
    if (a->HeightAGL > 0 && Passed(a->PositionCoord, a->PositionCoord, fly->DestinationCoord, ARRIVAL)) Finish_Move(a);
    return 1;
}

int Mission_Enter(AircraftClass* a)
{
    auto fly = Locomotor(a);
    if (!fly) return 1;
    auto dock = Reserved(a);
    if (a->HeightAGL <= 0 && !fly->IsTakingOff && !fly->IsMoving) {
        Approach approach;
        if (dock && Geometry(a, dock, approach) && !fly->IsLanding && fly->CurrentSpeed == 0.0 && fly->TargetSpeed == 0.0
            && Heading_OK(a->PrimaryFacing.Current(), approach.Direction)
            && Distance_Squared(a->PositionCoord, dock->Docking_Coord()) <= int64_t(TOUCHDOWN) * TOUCHDOWN) {
            // IM_IN queues the existing aircraft SLEEP and building REPAIR missions.
            // Repair rejects a nonempty NavCom, including an already parked
            // INITIAL aircraft that did not pass through Process_Landing.
            a->NavCom = nullptr;
            Goal(fly, COORD_NONE);
            if (a->Transmit_Message(RADIO_IM_IN, dock) == RADIO_ROGER) {
                a->field_370 = true;
                a->Commence();
                return DOCK_RETRY;
            }
        }
        // Losing the service permission between touchdown and IM_IN must not
        // leave the aircraft asleep on an invalid dock. Re-enter search/climb.
        if (a->Status == LANDING) {
            Go_Around(a, fly, true);
            Take_Off(fly);
            return 1;
        }
    }
    switch (a->Status) {
    case INITIAL:
        if (a->HeightAGL <= 0 || fly->IsTakingOff) {
            if (!fly->IsTakingOff) Take_Off(fly);
            a->Status = TAKEOFF;
        } else a->Status = ALTITUDE;
        break;
    case TAKEOFF:
        if (!fly->IsTakingOff && a->HeightAGL >= a->Class->Flight_Level()) a->Status = ALTITUDE;
        break;
    case ALTITUDE:
        Cruise(a, fly);
        if (!Find_Dock(a, fly)) Start_Holding(a, fly);
        break;
    case STACK:
        if (Find_Dock(a, fly)) return 1;
        return DOCK_RETRY;
    case DOWNWIND: case CROSSWIND: case TRAVEL: case LANDING:
        if (!dock) Go_Around(a, fly, true);
        else if (a->Transmit_Message(RADIO_DOCKING, dock) != RADIO_ROGER) Go_Around(a, fly, true);
        break;
    default:
        Go_Around(a, fly, true);
        break;
    }
    return 1;
}

bool Process(FlyLocomotionClass* fly, AircraftClass* a)
{
    if (a->field_370 && a->MissionQueue != MISSION_NONE
        && (!fly->IsTakingOff || a->CurrentMission == MISSION_ATTACK)) {
        a->Commence();
        CAPDiagnostics::Escort_State(a, "PROCESS_AFTER_COMMENCE", true);
    }
    Update_Native_Attack(a, fly);
    if (Extension::Fetch(a)->AttackControl == FixedWingAttackControl::Native
        || Extension::Fetch(a)->AttackControl == FixedWingAttackControl::NativeEnding) {
        // A mission may have changed above. Re-entry reaches the original Fly
        // body because Controlled() now declines it; no custom move has run.
        return fly->Process();
    }
    if (!a->IsActive) return false;
    if (a->HeightAGL <= 0 && !fly->IsTakingOff) {
        if (!fly->IsMoving) return false;
        Take_Off(fly);
    }
    if (a->CurrentMission == MISSION_ENTER && a->Status >= DOWNWIND) {
        Approach valid;
        auto reserved = Reserved(a);
        if (!reserved || (a->Status != LANDING && !Geometry(a, reserved, valid))) Go_Around(a, fly, true);
    }
    if (Guard_Mission(a->CurrentMission) && CAP_Active(a)) CAP_Flight(a, fly);
    // Without an explicit CAP, retain the pre-H airborne Guard fallback.
    if (!CAP_Active(a) && a->HeightAGL > 0 && a->CurrentMission == MISSION_GUARD) {
        if (a->TarCom && a->Ammo != 0 && a->Is_Weapon_Equipped()) {
            a->Assign_Mission(MISSION_ATTACK);
            a->Commence();
            Update_Native_Attack(a, fly);
            if (Extension::Fetch(a)->AttackControl == FixedWingAttackControl::Native) return fly->Process();
        } else if (!a->NavCom) {
            Request_Return(a);
            a->Commence();
        } else {
            a->Assign_Mission(MISSION_MOVE);
            a->Commence();
        }
    }
    if (a->HeightAGL > 0 && a->CurrentMission == MISSION_SLEEP) {
        Request_Return(a);
        a->Commence();
    }
    if (a->CurrentMission == MISSION_ENTER && a->Status == ALTITUDE) Mission_Enter(a);

    Approach approach;
    auto dock = Reserved(a);
    bool final = a->CurrentMission == MISSION_ENTER && a->Status == LANDING;
    if (final) {
        // Compare the saved committed facing with the centrally selected approach,
        // including its opposite fallback. Maintenance never overwrites it.
        if (!dock || !Geometry(a, dock, approach)) {
            Go_Around(a, fly, true);
            final = false;
        } else if (a->SecondaryFacing.Desired() != approach.Direction) {
            Go_Around(a, fly, false);
            final = false;
        }
    }
    if (!final) Cruise(a, fly);
    fly->TargetSpeed = Get_Approach_Speed(a, a->Status);
    // Preserve the native Fly acceleration/deceleration step. The positive
    // bootstrap is only for a stationary airborne state, not a per-frame floor.
    if (fly->CurrentSpeed <= 0.0) fly->CurrentSpeed = std::min(0.25, fly->TargetSpeed);
    if (fly->CurrentSpeed < fly->TargetSpeed) {
        fly->CurrentSpeed = std::min(fly->CurrentSpeed + 0.1, fly->TargetSpeed);
    } else if (fly->CurrentSpeed > fly->TargetSpeed) {
        fly->CurrentSpeed = std::max(fly->CurrentSpeed - 0.1, fly->TargetSpeed);
    }
    fly->IsMoving = true;

    const Coord from = a->PositionCoord;
    if (fly->DestinationCoord == COORD_NONE) {
        DirType heading = a->PrimaryFacing.Current();
        Goal(fly, Coord_Move(from, heading, 2 * CELL_LEPTON));
    }
    const DirType heading = Direction(from, fly->DestinationCoord);
    if (!fly->IsTakingOff || a->HeightAGL > fly->FlightLevel / 2) a->PrimaryFacing.Set_Desired(heading);
    if (!final) a->SecondaryFacing.Set_Desired(a->PrimaryFacing.Desired());

    const int speed = std::max(1, fly->Apparent_Speed());
    DirType direction = a->PrimaryFacing.Current();
    Coord to = Coord_Move(from, direction, speed);
    if (Extension::Fetch(a)->AttackControl == FixedWingAttackControl::NativeTakeoff) {
        // Only startup still skips native Movement_AI. Full native Attack uses
        // the original world-edge handling, with no parallel FixedWing wrapper.
        to = Takeoff_Movement_Coord(from, to);
    }
    const int ground = Map.Get_Height_GL(to);
    bool touchdown = false;
    if (final) {
        const DirType landing = approach.Direction;
        DirType back = landing + DirType(DIR_S);
        const Coord unit = Coord_Move(approach.D, back, FINAL_LENGTH) - approach.D;
        const int64_t remaining = int64_t(to.X - approach.D.X) * unit.X + int64_t(to.Y - approach.D.Y) * unit.Y;
        const int64_t along = remaining / FINAL_LENGTH;
        const int desired_z = approach.D.Z + int(std::max<int64_t>(0, along) * a->Class->Flight_Level() / FINAL_LENGTH);
        to.Z = from.Z + std::clamp(desired_z - from.Z, -CLIMB, CLIMB);
        if (Passed(from, to, approach.D, TOUCHDOWN)
            && Distance_Squared(from, approach.D) <= int64_t(speed + TOUCHDOWN) * (speed + TOUCHDOWN)
            && Heading_OK(a->PrimaryFacing.Current(), landing) && to.Z <= approach.D.Z + CLIMB
            && a->PitchAngle <= 0 && Dock_Status(a, dock, true)
            && a->Can_Enter_Cell(&Map[approach.D]) == MOVE_OK && a->Is_LZ_Clear(dock)) {
            // Only the final movement segment may terminate at the dock coordinate.
            to = approach.D;
            touchdown = true;
        } else if (to.Z <= ground || along <= 0
            || (!Heading_OK(a->PrimaryFacing.Current(), landing) && along <= TOUCHDOWN)) {
            // A rising terrain intersection or an unsuccessful crossing is a
            // missed approach. Test the full touchdown segment BEFORE overshoot.
            Go_Around(a, fly, false);
            final = false;
        }
    }
    if (!final) {
        int desired = ground + a->Class->Flight_Level();
        if (a->CurrentMission == MISSION_ENTER && a->Status >= DOWNWIND && dock) {
            desired = std::max(desired, dock->Docking_Coord().Z + a->Class->Flight_Level());
        }
        to.Z = from.Z + std::clamp(desired - from.Z, -CLIMB, CLIMB);
    }
    if (!touchdown) to.Z = std::max(to.Z, ground + 1);

    // A single transaction brackets XY, height, native touchdown and layer changes.
    const LayerType old_layer = a->In_Which_Layer();
    const bool diagnostic_was_taking_off = fly->IsTakingOff;
    a->Mark(MARK_UP);
    Map.Remove(a);
    a->Set_Coord(to);
    a->IsOnBridge = false;
    if (touchdown) {
        // All native failure/random-LZ branches have already been ruled out above.
        Process_Landing(fly);
    } else if (fly->IsTakingOff) Process_Takeoff(fly);
    if (old_layer != a->In_Which_Layer()) {
        if (a->In_Which_Layer() == LAYER_GROUND) {
            a->Transmit_Message(RADIO_TETHER);
            a->Look();
        } else {
            a->Transmit_Message(RADIO_UNTETHER);
            if (a->Radio && a->NavCom != a->Radio) Release(a);
        }
    }
    Map.Submit(a);
    a->Mark(MARK_DOWN);
    Tracker(a);
    if (diagnostic_was_taking_off && !fly->IsTakingOff) CAPDiagnostics::Escort_State(a, "TAKEOFF_COMPLETE", true);
    if (!touchdown) {
        Update_Native_Attack(a, fly);
        if (a->CurrentMission != MISSION_ATTACK) Arrived(a, fly, from, to);
    } else a->MissionClass::Timer = 0;

    // Preserve the native wall-crossing interaction skipped with Movement_AI.
    const bool moving = fly->IsMoving;
    auto building = Map[a->PositionCoord].Cell_Building();
    if (building && building->Class->IsFirestormWall && building->House->IsFirestormActive
        && !a->Class->IsIgnoresFirestorm) building->Crossing_Firestorm(a, true);
    return moving;
}
}

// Entry hooks run before ANY stolen instruction. A nonzero return bypasses
// Syringe's displaced-instruction trampoline, so use a matching leaf return.
static __declspec(naked) void FixedWing_Return() { __asm { ret } }
static __declspec(naked) void FixedWing_Return4() { __asm { ret 4 } }
static __declspec(naked) void FixedWing_Return8() { __asm { ret 8 } }
static __declspec(naked) void FixedWing_Return12() { __asm { ret 12 } }

// Only the existing GuardArea action bypasses the native weapon/ammo gate.
// Queue the ordinary synchronized mission; no local CAP state is changed here.
// The binary's Object/Cell entries are reversed in TSpp's address labels.
DEFINE_HOOK(0x0040B690, _Aircraft_GuardClickObject_FixedWing, 5)
{
    GET(AircraftClass*, a, ECX);
    GET_STACK(ActionType, action, 0x4);
    GET_STACK(ObjectClass*, target, 0x8);
    if (action != ACTION_GUARD_AREA || !FixedWing::Enabled(a) || !a->Can_Player_Move()) return 0;
    const auto techno = dynamic_cast<TechnoClass*>(target);
    if (!techno || techno->House != a->House) return 0;
    FixedWing::CAPDiagnostics::Queued(a, target, "UI_OBJECT_GUARD_AREA");
    a->Player_Assign_Mission(MISSION_GUARD_AREA, target);
    R->EAX(true);
    return reinterpret_cast<DWORD>(&FixedWing_Return12);
}

DEFINE_HOOK(0x0040B740, _Aircraft_GuardClickCell_FixedWing, 8)
{
    GET(AircraftClass*, a, ECX);
    GET_STACK(ActionType, action, 0x4);
    GET_STACK(Cell*, cell, 0x8);
    if (action != ACTION_GUARD_AREA || !FixedWing::Enabled(a) || !a->Can_Player_Move()) return 0;
    if (FixedWing::CAPDiagnostics::Enabled()) FixedWing::CAPDiagnostics::Queued(a, &Map[*cell], "UI_CELL_GUARD_AREA");
    a->Player_Assign_Mission(MISSION_GUARD_AREA, &Map[*cell]);
    R->EAX(true);
    return reinterpret_cast<DWORD>(&FixedWing_Return12);
}

DEFINE_HOOK(0x004090C0, _Aircraft_AI_FixedWing_NoDock, 5)
{
    GET(AircraftClass*, a, ECX);
    FixedWing::DockDiagnostics::Observe(a);
    if (FixedWing::No_Dock_AI(a)) return reinterpret_cast<DWORD>(&FixedWing_Return);
    if (FixedWing::Enabled(a)) {
        FixedWing::Observe_Native_Attack(a);
        FixedWing::Update_Native_Attack(a, FixedWing::Locomotor(a));
    }
    FixedWing::CAP_AI(a);
    return 0;
}

// Branch-free displaced instructions only: no relative CALL/Jcc relocation or
// incoming flags needed. All callbacks return 0 and leave registers untouched.
DEFINE_HOOK(0x00637E93, _FixedWing_DockTrace_Native_Inventory, 5)
{
    GET(AircraftClass*, a, ESI);
    GET(int, result, EAX);
    FixedWing::DockDiagnostics::Native(a, nullptr, "HOUSE_INVENTORY_PASS", result);
    return 0;
}

DEFINE_HOOK(0x00637EAB, _FixedWing_DockTrace_Native_Enumerate, 5)
{
    GET(AircraftClass*, a, ESI);
    if (FixedWing::DockDiagnostics::Searching == a) {
        GET(BuildingClass**, buildings, EDX);
        GET(int, index, EBX);
        FixedWing::DockDiagnostics::Native_Enumerated(a, buildings[index]);
    }
    return 0;
}

DEFINE_HOOK(0x00637F13, _FixedWing_DockTrace_Native_FilterPassed, 5)
{
    GET(AircraftClass*, a, ESI);
    GET(BuildingClass*, dock, EDI);
    // Reached only after owner/limbo/type and the existing FixedWing hook passed.
    FixedWing::DockDiagnostics::Native_Filter_Passed(a, dock);
    return 0;
}

DEFINE_HOOK(0x00637FE0, _FixedWing_DockTrace_Native_CanLoad, 5)
{
    GET(AircraftClass*, a, ESI);
    GET(BuildingClass*, dock, EDI);
    FixedWing::DockDiagnostics::Native_CanLoad(a, dock, false);
    return 0;
}

DEFINE_HOOK(0x00586CC0, _FixedWing_DockTrace_Native_CanLoadPassed, 8)
{
    GET_STACK(DWORD, caller, 0x0);
    // This exact distance call is reached only after RADIO_CAN_LOAD == ROGER.
    if (caller == 0x00637FFA) {
        GET(AircraftClass*, a, ECX);
        GET_STACK(BuildingClass*, dock, 0x4);
        FixedWing::DockDiagnostics::Native_CanLoad(a, dock, true);
    }
    return 0;
}

DEFINE_HOOK(0x0063800D, _FixedWing_DockTrace_Native_Best, 6)
{
    GET(AircraftClass*, a, ESI);
    GET(BuildingClass*, dock, EDI);
    GET(int, distance, EAX);
    if (FixedWing::DockDiagnostics::Native_Candidate(a, dock)) FixedWing::DockDiagnostics::Progress.Best = true;
    FixedWing::DockDiagnostics::Native(a, dock, "NATIVE_BEST_SO_FAR", distance);
    return 0;
}

DEFINE_HOOK(0x00638013, _FixedWing_DockTrace_Native_CandidateFinished, 5)
{
    GET(AircraftClass*, a, ESI);
    GET(BuildingClass*, dock, EDI);
    GET(int, value, EAX);
    FixedWing::DockDiagnostics::Native_Finished(a, dock, value);
    return 0;
}

DEFINE_HOOK(0x00499600, _Fly_Process_FixedWing, 6)
{
    GET_STACK(ILocomotion*, iface, 0x4);
    auto fly = static_cast<FlyLocomotionClass*>(iface);
    if (fly->LinkedTo && fly->LinkedTo->Fetch_RTTI() == RTTI_AIRCRAFT) {
        auto aircraft = static_cast<AircraftClass*>(fly->LinkedTo);
        FixedWing::Update_Native_Attack(aircraft, fly);
        if (Extension::Fetch(aircraft)->AttackControl == FixedWingAttackControl::Native
            || Extension::Fetch(aircraft)->AttackControl == FixedWingAttackControl::NativeEnding) {
            FixedWing::Trace_Native_Entry(aircraft, true);
        }
    }
    auto a = FixedWing::Controlled(fly);
    if (!a) return 0;
    R->EAX(FixedWing::Process(fly, a));
    return reinterpret_cast<DWORD>(&FixedWing_Return4);
}

DEFINE_HOOK(0x00499AB0, _Fly_Stop_FixedWing, 8)
{
    GET_STACK(ILocomotion*, iface, 0x4);
    auto fly = static_cast<FlyLocomotionClass*>(iface);
    if (fly->LinkedTo && fly->LinkedTo->Fetch_RTTI() == RTTI_AIRCRAFT) {
        auto aircraft = static_cast<AircraftClass*>(fly->LinkedTo);
        if (FixedWing::Attack_Order(aircraft) && !FixedWing::Native_Attack(aircraft)) {
            FixedWing::Update_Native_Attack(aircraft, fly);
        }
    }
    if (fly->LinkedTo && fly->LinkedTo->Fetch_RTTI() == RTTI_AIRCRAFT
        && FixedWing::Native_Attack(static_cast<AircraftClass*>(fly->LinkedTo))) return 0;
    auto a = FixedWing::Controlled(fly);
    if (!a) return 0;
    FixedWing::Request_Return(a);
    return reinterpret_cast<DWORD>(&FixedWing_Return4);
}

DEFINE_HOOK(0x0040A350, _Aircraft_MissionMove_FixedWing, 6)
{
    GET(AircraftClass*, a, ECX);
    if (!FixedWing::Enabled(a) || !FixedWing::Controlled(FixedWing::Locomotor(a))) return 0;
    R->EAX(FixedWing::Mission_Move(a));
    return reinterpret_cast<DWORD>(&FixedWing_Return);
}

DEFINE_HOOK(0x0040BA40, _Aircraft_MissionAttack_FixedWing, 6)
{
    GET(AircraftClass*, a, ECX);
    if (!FixedWing::Enabled(a)) return 0;
    auto fly = FixedWing::Locomotor(a);
    if (!fly) return 0;
    FixedWing::Update_Native_Attack(a, fly);
    if (Extension::Fetch(a)->AttackControl == FixedWingAttackControl::Native
        || Extension::Fetch(a)->AttackControl == FixedWingAttackControl::NativeEnding) {
        FixedWing::Trace_Native_Entry(a, false);
        return 0;
    }
    // Only the FixedWing takeoff adapter delays native mission processing.
    R->EAX(1);
    return reinterpret_cast<DWORD>(&FixedWing_Return);
}

DEFINE_HOOK(0x0040D360, _Aircraft_MissionEnter_FixedWing, 5)
{
    GET(AircraftClass*, a, ECX);
    if (FixedWing::Native_Attack(a)) FixedWing::Update_Native_Attack(a, FixedWing::Locomotor(a));
    if (a->CurrentMission != MISSION_ENTER && FixedWing::Enabled(a)) {
        // Native completion resumed CAP; do not run an Enter body afterward.
        R->EAX(FixedWing::DOCK_RETRY);
        return reinterpret_cast<DWORD>(&FixedWing_Return);
    }
    if (!FixedWing::Enabled(a) || !FixedWing::Controlled(FixedWing::Locomotor(a))) return 0;
    R->EAX(FixedWing::Mission_Enter(a));
    return reinterpret_cast<DWORD>(&FixedWing_Return);
}

DEFINE_HOOK(0x0040DD10, _Aircraft_MissionGuard_FixedWing, 5)
{
    GET(AircraftClass*, a, ECX);
    if (!FixedWing::Enabled(a) || Extension::Fetch(a)->CAPMode == FixedWingCAPMode::None
        || !FixedWing::Controlled(FixedWing::Locomotor(a))) return 0;
    R->EAX(FixedWing::Mission_CAP(a));
    return reinterpret_cast<DWORD>(&FixedWing_Return);
}

DEFINE_HOOK(0x0040E0A0, _Aircraft_MissionGuardArea_FixedWing, 10)
{
    GET(AircraftClass*, a, ECX);
    if (!FixedWing::Enabled(a) || Extension::Fetch(a)->CAPMode == FixedWingCAPMode::None
        || !FixedWing::Controlled(FixedWing::Locomotor(a))) return 0;
    R->EAX(FixedWing::Mission_CAP(a));
    return reinterpret_cast<DWORD>(&FixedWing_Return);
}

DEFINE_HOOK(0x0040E1B0, _Aircraft_AssignDestination_FixedWing, 7)
{
    GET(AircraftClass*, a, ECX);
    if (!FixedWing::Enabled(a)) return 0;
    GET_STACK(const AbstractClass*, target, 0x4);
    GET_STACK(bool, update, 0x8);
    const bool pending = Extension::Fetch(a)->AttackControl == FixedWingAttackControl::NativePending
        || (!FixedWing::Native_Attack(a) && FixedWing::Attack_Order(a));
    if (pending && !target && FixedWing::Attack_Order(a)) {
        // Accepted event/script has installed TarCom. Consume its empty initial
        // destination after priming native navigation, as in the H6A handoff.
        FixedWing::Update_Native_Attack(a, FixedWing::Locomotor(a));
        return reinterpret_cast<DWORD>(&FixedWing_Return8);
    }
    if (FixedWing::Native_Attack(a)) {
        return 0; // Native attack's own destination writes preserve native Status.
    }
    if (FixedWing::Attack_Order(a)) {
        FixedWing::Update_Native_Attack(a, FixedWing::Locomotor(a));
        if (FixedWing::Native_Attack(a)) return 0;
    }
    if (!FixedWing::Controlled(FixedWing::Locomotor(a))) return 0;
    FixedWing::Assign_Destination(a, target, update);
    return reinterpret_cast<DWORD>(&FixedWing_Return8);
}
