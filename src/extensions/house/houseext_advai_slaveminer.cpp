/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  AF-0012: Advanced AI use of Slave Miners.
 *
 *          A Slave Miner is a vehicle that deploys into a building whose
 *          slaves harvest the Tiberium around it (SlaveManagerClass). A miner
 *          of the computer already finds Tiberium, deploys and relocates on
 *          its own; this file adds what the AI was missing:
 *
 *          - it orders one when a rich, reachable field lies far from all of
 *            its refineries and away from enemy bases,
 *          - a miner that wakes up is sent to such a field instead of the
 *            Tiberium next to it (which the refineries already serve),
 *          - losses start a rebuild delay.
 *
 *          All decisions read only simulation state (the resource analysis,
 *          no shroud or radar of the local player), so multiplayer clients
 *          stay in sync.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "houseext.h"

#include "advancedai_resources.h"
#include "building.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "debughandler.h"
#include "extension.h"
#include "factory.h"
#include "house.h"
#include "rules.h"
#include "rulesext.h"
#include "slavemanager.h"
#include "techno.h"
#include "technotype.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "unit.h"
#include "unittype.h"
#include "unittypeext.h"
#include "vinifera_globals.h"

#include "cell.h"
#include "iomap.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>


namespace {

/**
 *  A field within this many cells of another miner of the house (where it
 *  stands, deploys or was sent) is taken.
 */
constexpr int SLAVE_MINER_CLAIM_RADIUS = 12;

/**
 *  A waking miner this close to the chosen field scans on its own.
 */
constexpr int SLAVE_MINER_NEAR_FIELD = 10;


int Grid_Span(const Cell& a, const Cell& b)
{
    const int dx = std::abs(a.X - b.X);
    const int dy = std::abs(a.Y - b.Y);
    return dx > dy ? dx : dy;
}


bool Near(const Cell& cell, const Cell& target)
{
    return cell != CELL_NONE && Grid_Span(cell, target) <= SLAVE_MINER_CLAIM_RADIUS;
}


/**
 *  A harvester works for a mobile refinery when it stands, drives to or last
 *  harvested within this many cells of it.
 */
constexpr int MOBILE_REFINERY_RADIUS = 10;

/**
 *  A harvester that can only unload at mobile refineries and works farther
 *  than this from all of them (its refinery moved away) is sent again.
 */
constexpr int MOBILE_REFINERY_STRAY_RADIUS = MOBILE_REFINERY_RADIUS * 2;


bool Is_Tiberium_Harvester(const UnitClass* unit)
{
    return unit->Class->IsToHarvest && !unit->Class->IsToVeinHarvest;
}


bool Works_Near(const UnitClass* unit, const Cell& center)
{
    if (Grid_Span(unit->Get_Cell(), center) <= MOBILE_REFINERY_RADIUS) return true;
    if (unit->NavCom != nullptr && Grid_Span(Cell(unit->NavCom->Center_Coord()), center) <= MOBILE_REFINERY_RADIUS) return true;
    if (unit->ArchiveTarget != nullptr && Grid_Span(Cell(unit->ArchiveTarget->Center_Coord()), center) <= MOBILE_REFINERY_RADIUS) return true;
    return false;
}


/**
 *  The Tiberium cell nearest to "center" (within MOBILE_REFINERY_RADIUS) that
 *  the harvester can drive to, or CELL_NONE.
 */
Cell Nearest_Ore(const Cell& center, const UnitClass* harvester)
{
    const MZoneType mzone = harvester->Class->MZone;
    const int zone = Map.Get_Cell_Zone(harvester->Get_Cell(), mzone, false);

    for (int ring = 1; ring <= MOBILE_REFINERY_RADIUS; ++ring) {
        Cell best = CELL_NONE;
        int best_value = -1;
        for (int y = -ring; y <= ring; ++y) {
            for (int x = -ring; x <= ring; ++x) {
                if (std::abs(x) != ring && std::abs(y) != ring) continue;
                const Cell cell(center.X + x, center.Y + y);
                if (!Map.In_Local_Radar(cell)) continue;
                CellClass& cellptr = Map[cell];
                if (cellptr.Land_Type() != LAND_TIBERIUM) continue;
                if (Map.Get_Cell_Zone(cell, mzone, false) != zone) continue;
                const int value = cellptr.Get_Tiberium_Value();
                if (value > best_value) {
                    best_value = value;
                    best = cell;
                }
            }
        }
        if (best != CELL_NONE) return best;
    }

    return CELL_NONE;
}

} // namespace


/**
 *  Is this vehicle a Slave Miner that can deploy?
 */
bool HouseClassExtension::Is_AdvAI_Slave_Miner_Unit_Type(const UnitTypeClass* type)
{
    return type != nullptr && type->DeploysInto != nullptr && SlaveManagerClass::Is_Slave_Miner_Type(type);
}


/**
 *  Slave Miners of the house: vehicles, deployed buildings and, if asked,
 *  the ones in production.
 */
int HouseClassExtension::AdvAI_Slave_Miner_Count(bool production) const
{
    const HouseClass* house = This_Const();
    int count = 0;

    for (int i = 0; i < Units.Count(); i++) {
        const UnitClass* unit = Units[i];
        if (unit->House == house && unit->Strength > 0 && !unit->IsInLimbo && Is_AdvAI_Slave_Miner_Unit_Type(unit->Class)) ++count;
    }
    for (int i = 0; i < Buildings.Count(); i++) {
        const BuildingClass* building = Buildings[i];
        if (building->House == house && building->Strength > 0 && !building->IsInLimbo &&
            building->Class->UndeploysInto != nullptr && SlaveManagerClass::Is_Slave_Miner_Type(building->Class)) ++count;
    }
    if (production) {
        for (int i = 0; i < Factories.Count(); i++) {
            const FactoryClass* factory = Factories[i];
            if (factory == nullptr || factory->House != house) continue;
            const TechnoClass* product = factory->Get_Object();
            if (product != nullptr && product->RTTI == RTTI_UNIT &&
                Is_AdvAI_Slave_Miner_Unit_Type(static_cast<const UnitClass*>(product)->Class)) ++count;
        }
    }

    return count;
}


/**
 *  The Tiberium cell a Slave Miner should work, or CELL_NONE. Fields must be
 *  reachable from the base, rich enough, far from the own refineries and
 *  from enemy bases, not taken by another miner of the house ("miner"
 *  itself is ignored) and have no enemy refinery (fixed or mobile) nearby.
 *  For a new miner (no "miner", or "deployed" false: fresh from the factory,
 *  built for the field of its order) the richest field per distance from the
 *  base wins; a miner that already worked somewhere rates by its own distance
 *  and drives at most AdvancedAISlaveMinerMaxTravel cells.
 */
Cell HouseClassExtension::AdvAI_Slave_Miner_Field(const TechnoClass* miner, bool deployed, char* reason, int reason_size) const
{
    const HouseClass* house = This_Const();
    const AdvancedAIResourceAnalysis* analysis = Get_Advanced_AI_Resource_Analysis();
    if (analysis == nullptr || !analysis->HasBase) {
        std::snprintf(reason, reason_size, "no-resource-analysis");
        return CELL_NONE;
    }

    int poor = 0, refinery = 0, enemy = 0, claimed = 0, enemy_refinery = 0, too_far = 0;
    const Cell from = miner != nullptr && deployed ? miner->Get_Cell() : CELL_NONE;
    const int max_travel = RuleExtension->AdvancedAISlaveMinerMaxTravel;
    const AdvancedAIResourceField* best = nullptr;
    long long best_rating = -1;

    for (const AdvancedAIResourceField& field : analysis->Fields) {
        if (field.ReachableCells <= 0) continue;
        if (field.ReachableValue < RuleExtension->AdvancedAISlaveMinerFieldValue) { ++poor; continue; }
        if (field.RefineryDistance >= 0 && field.RefineryDistance < RuleExtension->AdvancedAISlaveMinerRefineryDistance) { ++refinery; continue; }
        if (field.EnemyBaseDistance >= 0 && field.EnemyBaseDistance < RuleExtension->AdvancedAISlaveMinerEnemyDistance) { ++enemy; continue; }

        const int travel = from != CELL_NONE ? Grid_Span(from, field.Target) : -1;
        if (travel >= 0 && max_travel > 0 && travel > max_travel) { ++too_far; continue; }

        // Enemy outposts: a refinery of an enemy house (also a deployed mobile one) at the field.
        bool hostile = false;
        for (int i = 0; i < Buildings.Count() && !hostile; i++) {
            const BuildingClass* building = Buildings[i];
            if (building->House == nullptr || house->Is_Ally(building->House)) continue;
            if (building->IsInLimbo || building->Strength <= 0 || !building->Class->IsRefinery || building->Class->IsWeeder) continue;
            hostile = Near(building->Get_Cell(), field.Target);
        }
        if (hostile) { ++enemy_refinery; continue; }

        bool taken = false;
        for (int i = 0; i < SlaveManagers.Count() && !taken; i++) {
            const SlaveManagerClass* manager = SlaveManagers[i];
            const TechnoClass* owner = manager->Owner;
            if (owner == nullptr || owner == miner || owner->House != house) continue;
            taken = Near(owner->Get_Cell(), field.Target) || Near(manager->DeploySite, field.Target) || Near(manager->OrderedOre, field.Target);
        }
        if (taken) { ++claimed; continue; }

        const int distance = travel >= 0 ? travel : field.BaseDistance;
        const long long rating = field.ReachableValue * 16 / (16 + (distance > 0 ? distance : 0));
        if (best == nullptr || rating > best_rating || (rating == best_rating && field.ID < best->ID)) {
            best = &field;
            best_rating = rating;
        }
    }

    if (best == nullptr) {
        std::snprintf(reason, reason_size, "no-field (fields=%d poor=%d near-refinery=%d near-enemy=%d taken=%d enemy-refinery=%d too-far=%d)",
            (int)analysis->Fields.size(), poor, refinery, enemy, claimed, enemy_refinery, too_far);
        return CELL_NONE;
    }

    if (from != CELL_NONE) {
        std::snprintf(reason, reason_size, "field %d value=%lld refinery-distance=%d base-distance=%d travel=%d",
            best->ID, best->ReachableValue, best->RefineryDistance, best->BaseDistance, Grid_Span(from, best->Target));
    } else {
        std::snprintf(reason, reason_size, "field %d value=%lld refinery-distance=%d base-distance=%d",
            best->ID, best->ReachableValue, best->RefineryDistance, best->BaseDistance);
    }
    return best->Target;
}


/**
 *  Called by the Slave Miner of a computer house when it wakes up and is
 *  about to look for Tiberium. Returns the field it should go to, or
 *  CELL_NONE to let it scan around itself.
 */
Cell HouseClassExtension::AdvAI_Slave_Miner_Wake_Up_Field(const UnitClass* unit, bool deployed)
{
    if (unit == nullptr || unit->House == nullptr || unit->House->Is_Human_Player()) return CELL_NONE;
    if (!RuleExtension->IsAdvancedAISlaveMiner) return CELL_NONE;

    const HouseClassExtension* ext = Extension::Fetch(unit->House);
    char reason[160] = "";
    const Cell target = ext->AdvAI_Slave_Miner_Field(unit, deployed, reason, sizeof(reason));
    if (target == CELL_NONE) {
        const Cell here = unit->Get_Cell();
        DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner {} at {},{} scans nearby ({})\n",
            (int)unit->House->HeapID, Frame, unit->Class->Name(), here.X, here.Y, reason);
        return CELL_NONE;
    }

    /**
     *  The analysis target lies near the middle of the field, where nothing can
     *  be built. Aim at the edge of the field facing the miner instead.
     */
    const Cell here = unit->Get_Cell();
    const Cell field = SlaveManagerClass::Field_Edge(target, here);
    if (Grid_Span(here, field) <= SLAVE_MINER_NEAR_FIELD) return CELL_NONE;

    DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner {} at {},{} sent to {},{} ({})\n",
        (int)unit->House->HeapID, Frame, unit->Class->Name(), here.X, here.Y, field.X, field.Y, reason);
    return field;
}


/**
 *  Called by the Slave Manager of a deployed computer miner whose field is
 *  used up. Returns the edge of the next field (same rules as for a
 *  relocating miner: at most AdvancedAISlaveMinerMaxTravel cells away), or
 *  CELL_NONE to let the miner look around itself as before.
 */
Cell HouseClassExtension::AdvAI_Slave_Miner_Relocate_Field(const BuildingClass* building)
{
    if (building == nullptr || building->House == nullptr || building->House->Is_Human_Player()) return CELL_NONE;
    if (!RuleExtension->IsAdvancedAISlaveMiner) return CELL_NONE;

    const HouseClassExtension* ext = Extension::Fetch(building->House);
    char reason[160] = "";
    const Cell target = ext->AdvAI_Slave_Miner_Field(building, true, reason, sizeof(reason));
    const Cell here(building->Center_Coord());
    if (target == CELL_NONE) {
        DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner {} at {},{} relocates nearby ({})\n",
            (int)building->House->HeapID, Frame, building->Class->Name(), here.X, here.Y, reason);
        return CELL_NONE;
    }

    const Cell field = SlaveManagerClass::Field_Edge(target, here);
    DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner {} at {},{} relocates to {},{} ({})\n",
        (int)building->House->HeapID, Frame, building->Class->Name(), here.X, here.Y, field.X, field.Y, reason);
    return field;
}


/**
 *  Picks a Slave Miner to order, or nullptr. Called by AdvAI_AI_Unit when no
 *  other order is pending.
 */
const UnitTypeClass* HouseClassExtension::AdvAI_Slave_Miner_To_Build(DynamicVectorClass<BuildingTypeClass*>& owned) const
{
    if (!RuleExtension->IsAdvancedAISlaveMiner || RuleExtension->AdvancedAISlaveMinerMax <= 0) return nullptr;

    const HouseClass* house = This_Const();

    /**
     *  Diagnosis: why no order is given. Written when the reason changes, at
     *  most once a minute per house. Only for the log, so not saved.
     */
    char reason[200] = "";
    auto skip = [&]() -> const UnitTypeClass* {
        struct Trace { std::string Reason; int Frame = INT_MIN / 2; };
        static std::map<int, Trace> traces;
        Trace& trace = traces[(int)house->HeapID];
        if (Frame < trace.Frame) trace = Trace(); // New game.
        if (trace.Reason != reason && Frame >= trace.Frame + TICKS_PER_MINUTE) {
            DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner skip reason={}\n", (int)house->HeapID, Frame, reason);
            trace.Reason = reason;
            trace.Frame = Frame;
        }
        return nullptr;
    };

    if (Frame < RuleExtension->AdvancedAISlaveMinerDelay) {
        std::snprintf(reason, sizeof(reason), "delay (until frame %d)", RuleExtension->AdvancedAISlaveMinerDelay);
        return skip();
    }
    if (Frame < SlaveMinerLostFrame + RuleExtension->AdvancedAISlaveMinerRebuildDelay) {
        std::snprintf(reason, sizeof(reason), "rebuild-delay (until frame %d)", SlaveMinerLostFrame + RuleExtension->AdvancedAISlaveMinerRebuildDelay);
        return skip();
    }
    if (IsUnderStartRushThreat) {
        std::snprintf(reason, sizeof(reason), "start-rush-threat");
        return skip();
    }
    if (AdvAI_Is_Recently_Attacked()) {
        std::snprintf(reason, sizeof(reason), "recently-attacked");
        return skip();
    }

    const int count = AdvAI_Slave_Miner_Count(true);
    if (count >= RuleExtension->AdvancedAISlaveMinerMax) {
        std::snprintf(reason, sizeof(reason), "limit (%d/%d)", count, RuleExtension->AdvancedAISlaveMinerMax);
        return skip();
    }

    /**
     *  Without a war factory the order could not start and would be given
     *  again on every pass.
     */
    bool has_factory = false;
    for (int i = 0; i < Buildings.Count() && !has_factory; i++) {
        const BuildingClass* building = Buildings[i];
        has_factory = building->House == house && building->IsActive && !building->IsInLimbo && building->Strength > 0 &&
            building->Class->ToBuild == RTTI_UNITTYPE && !Extension::Fetch(building->Class)->IsNaval &&
            building->Get_Mission() != MISSION_CONSTRUCTION && building->Get_Mission() != MISSION_DECONSTRUCTION;
    }
    if (!has_factory) {
        std::snprintf(reason, sizeof(reason), "no-war-factory");
        return skip();
    }

    char field_reason[160] = "";
    if (AdvAI_Slave_Miner_Field(nullptr, false, field_reason, sizeof(field_reason)) == CELL_NONE) {
        std::snprintf(reason, sizeof(reason), "%s", field_reason);
        return skip();
    }

    /**
     *  The reason names the first type that could be built in this game at all.
     *  Types above the tech level (TIBRIG2: TechLevel=-1) are reported only when
     *  there is no other.
     */
    bool any_type = false;
    bool have_reason = false;
    auto note = [&](bool real, const char* format, auto... args) {
        if (have_reason) return;
        std::snprintf(reason, sizeof(reason), format, args...);
        have_reason = real;
    };

    for (int i = 0; i < UnitTypes.Count(); i++) {
        const UnitTypeClass* unittype = UnitTypes[i];
        if (!Is_AdvAI_Slave_Miner_Unit_Type(unittype)) continue;
        if (Extension::Fetch(unittype)->IsNaval) continue;
        any_type = true;

        const int level = (int)unittype->Level;
        if (level < 0 || level > house->Control.TechLevel) {
            note(false, "techlevel %s (%d/%d)", unittype->Name(), level, house->Control.TechLevel);
            continue;
        }

        if (!AdvAI_Can_Produce(unittype, owned)) {
            note(true, "cannot-produce %s (owner or prerequisites)", unittype->Name());
            continue;
        }

        const AdvancedAIUnitStartBudget budget = Check_Advanced_AI_Unit_Start(unittype);
        if (budget.Blocked != nullptr) {
            note(true, "budget %s %s (uncommitted=%lld cost=%lld reserve=%lld)",
                unittype->Name(), budget.Blocked, budget.Uncommitted, budget.Cost, budget.Reserve);
            continue;
        }
        const long long left = budget.Uncommitted - budget.Cost - budget.Reserve;
        if (left < RuleExtension->AdvancedAISlaveMinerReserve) {
            note(true, "money %s (left=%lld wanted=%d)",
                unittype->Name(), left, RuleExtension->AdvancedAISlaveMinerReserve);
            continue;
        }

        DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner ordered {} ({}, {}/{})\n",
            (int)house->HeapID, Frame, unittype->Name(), field_reason, count + 1, RuleExtension->AdvancedAISlaveMinerMax);
        return unittype;
    }

    if (!any_type) {
        std::snprintf(reason, sizeof(reason), "no-slave-miner-type");
    }
    return skip();
}


/**
 *  Notices lost Slave Miners (rebuild delay). Runs every two seconds.
 */
void HouseClassExtension::AdvAI_Slave_Miner_AI()
{
    if (!RuleExtension->IsAdvancedAISlaveMiner) return;
    if (Frame < SlaveMinerNextFrame) return;
    SlaveMinerNextFrame = Frame + TICKS_PER_SECOND * 2;

    const int count = AdvAI_Slave_Miner_Count(false);
    if (count < SlaveMinerOwnedCount) {
        SlaveMinerLostFrame = Frame;
        DEV_DEBUG_INFO("AF-0012: house={} frame={} slave-miner lost ({} left)\n", (int)This()->HeapID, Frame, count);
    }
    SlaveMinerOwnedCount = count;

    AdvAI_Mobile_Refinery_AI();
}


/**
 *  Keeps harvesters at each deployed mobile refinery (Slave Miner without
 *  slaves, Refinery=yes). TS harvesters harvest near where they are and unload
 *  at the nearest refinery, so the ones from the war factory never go to a
 *  refinery far from the base. Missing ones are taken from the free harvesters
 *  that can dock there (Dock=), one per refinery per pass, and sent to the
 *  Tiberium next to it; each fixed refinery keeps at least one harvester that
 *  can dock at it. The harvester quota (AdvAI_Harvester_Shortfall) builds the
 *  replacements of a fitting type.
 *
 *  No saved state: which harvester works where is read from the map.
 */
void HouseClassExtension::AdvAI_Mobile_Refinery_AI()
{
    HouseClass* house = This();

    const int wanted = RuleExtension->AdvancedAIMobileRefineryHarvesters >= 0
        ? RuleExtension->AdvancedAIMobileRefineryHarvesters
        : std::max(1, RuleExtension->AdvancedAIHarvestersPerRefinery);
    if (wanted <= 0) return;

    DynamicVectorClass<BuildingClass*> mobiles;
    std::vector<const BuildingTypeClass*> fixed_types; // One entry per fixed refinery.
    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* building = Buildings[i];
        if (building->House != house || !building->IsActive || building->IsInLimbo || building->Strength <= 0) continue;
        if (!building->Class->IsRefinery || building->Class->IsWeeder) continue;

        if (SlaveManagerClass::Is_Mobile_Refinery_Type(building->Class)) {
            const MissionType mission = building->Get_Mission();
            if (mission != MISSION_CONSTRUCTION && mission != MISSION_DECONSTRUCTION) mobiles.Add(building);
        } else {
            fixed_types.push_back(building->Class);
        }
    }
    if (mobiles.Count() == 0) return;

    /**
     *  Can the harvester unload at one of the fixed refineries of the house?
     */
    auto serves_fixed = [&](const UnitClass* unit) {
        for (const BuildingTypeClass* type : fixed_types) {
            if (AdvAI_Can_Dock(unit->Class, type)) return true;
        }
        return false;
    };

    /**
     *  Who works where. A harvester counts for the first mobile refinery it can
     *  dock at (Dock= of its type) and works near. One that can only unload at
     *  mobile refineries (LOCUST) and already harvests or unloads works for the
     *  nearest of them, so it is not sent again and again; a new or idle one
     *  is sent once. If even the nearest is more than
     *  MOBILE_REFINERY_STRAY_RADIUS away (the refinery moved), it strays and
     *  is free again, so it follows. All others are free.
     */
    /**
     *  Diagnosis: a harvester sent to a mobile refinery is watched for two
     *  minutes, and every change of where it counts is logged (state, mobile
     *  refinery, mission). Only for the log, so not saved.
     */
    struct Watch { UnitClass* Unit; Cell Refinery; int Frame; std::string State; bool Seen; };
    static std::map<int, std::vector<Watch>> watches;
    std::vector<Watch>& watched = watches[(int)house->HeapID];
    watched.erase(std::remove_if(watched.begin(), watched.end(), [&](const Watch& w) { return w.Frame > Frame; }), watched.end()); // New game.
    for (Watch& w : watched) w.Seen = false;

    auto watch = [&](UnitClass* unit, int counted, const char* via) {
        for (Watch& w : watched) {
            if (w.Unit != unit) continue;
            w.Seen = true;
            const Cell at = counted >= 0 ? mobiles[counted]->Get_Cell() : CELL_NONE;
            char state[96];
            std::snprintf(state, sizeof(state), "%s %d,%d mission=%d", via, at.X, at.Y, (int)unit->Get_Mission());
            if (w.State == state) continue;
            w.State = state;
            const Cell here = unit->Get_Cell();
            const Cell nav = unit->NavCom != nullptr ? Cell(unit->NavCom->Center_Coord()) : CELL_NONE;
            const Cell archive = unit->ArchiveTarget != nullptr ? Cell(unit->ArchiveTarget->Center_Coord()) : CELL_NONE;
            DEV_DEBUG_INFO("AF-0012: house={} frame={} sent {} for {},{} at {},{} counts {} (navcom {},{} archive {},{} team={} after {} frames)\n",
                (int)house->HeapID, Frame, unit->Class->Name(), w.Refinery.X, w.Refinery.Y, here.X, here.Y, state,
                nav.X, nav.Y, archive.X, archive.Y, unit->Team != nullptr ? 1 : 0, Frame - w.Frame);
        }
    };

    std::vector<int> counts(mobiles.Count(), 0);
    std::vector<UnitClass*> free;
    for (int i = 0; i < Units.Count(); i++) {
        UnitClass* unit = Units[i];
        if (unit->House != house || !unit->IsActive || unit->Strength <= 0 || !Is_Tiberium_Harvester(unit)) continue;

        bool assigned = false;
        int counted = -1;
        const char* via = "free";
        for (int m = 0; m < mobiles.Count() && !assigned; m++) {
            if (AdvAI_Can_Dock(unit->Class, mobiles[m]->Class) && Works_Near(unit, mobiles[m]->Get_Cell())) {
                ++counts[m];
                assigned = true;
                counted = m;
                via = "near";
            }
        }
        const MissionType mission = unit->Get_Mission();
        const bool working = mission == MISSION_HARVEST || mission == MISSION_ENTER || mission == MISSION_UNLOAD;
        if (!assigned && working && !serves_fixed(unit)) {
            int nearest = -1;
            int nearest_distance = INT_MAX;
            for (int m = 0; m < mobiles.Count(); m++) {
                if (!AdvAI_Can_Dock(unit->Class, mobiles[m]->Class)) continue;
                const int distance = Grid_Span(unit->Get_Cell(), mobiles[m]->Get_Cell());
                if (distance < nearest_distance) {
                    nearest = m;
                    nearest_distance = distance;
                }
            }
            if (nearest >= 0 && nearest_distance <= MOBILE_REFINERY_STRAY_RADIUS) {
                ++counts[nearest];
                assigned = true;
                counted = nearest;
                via = "nearest";
            } else if (nearest >= 0) {
                via = "stray";
            }
        }

        // Busy (unloading, in a team, inside a building): not taken away.
        const bool busy = unit->IsInLimbo || unit->Team != nullptr || unit->Get_Mission() == MISSION_ENTER || unit->Get_Mission() == MISSION_UNLOAD;
        if (!assigned && busy) via = unit->IsInLimbo ? "limbo" : unit->Team != nullptr ? "team" : "busy";
        watch(unit, counted, via);

        if (assigned || busy) continue;
        free.push_back(unit);
    }

    for (const Watch& w : watched) {
        if (!w.Seen) {
            DEV_DEBUG_INFO("AF-0012: house={} frame={} sent harvester for {},{} gone (dead or not active) after {} frames\n",
                (int)house->HeapID, Frame, w.Refinery.X, w.Refinery.Y, Frame - w.Frame);
        }
    }
    watched.erase(std::remove_if(watched.begin(), watched.end(), [&](const Watch& w) {
        return !w.Seen || Frame - w.Frame > TICKS_PER_MINUTE * 2;
    }), watched.end());

    /**
     *  Every fixed refinery keeps one free harvester that can dock at it.
     *  Harvesters that cannot dock at any fixed refinery (LOCUST) are not
     *  needed there and may all go.
     */
    int fixed_spare = 0;
    for (const UnitClass* unit : free) {
        if (serves_fixed(unit)) ++fixed_spare;
    }
    const int fixed = static_cast<int>(fixed_types.size());

    for (int m = 0; m < mobiles.Count(); m++) {
        if (counts[m] >= wanted) continue;

        const BuildingClass* refinery = mobiles[m];
        const Cell center = refinery->Get_Cell();

        UnitClass* best = nullptr;
        int best_distance = INT_MAX;
        Cell best_ore = CELL_NONE;
        for (UnitClass* unit : free) {
            if (unit == nullptr) continue;
            if (!AdvAI_Can_Dock(unit->Class, refinery->Class)) continue;
            if (serves_fixed(unit) && fixed_spare <= fixed) continue;
            const int distance = Grid_Span(unit->Get_Cell(), center);
            if (distance >= best_distance) continue;
            const Cell ore = Nearest_Ore(center, unit);
            if (ore == CELL_NONE) continue;
            best = unit;
            best_distance = distance;
            best_ore = ore;
        }
        if (best == nullptr) continue;

        const Cell from = best->Get_Cell();
        best->Assign_Archive_Target(&Map[best_ore]);
        best->Assign_Destination(&Map[best_ore]);
        best->Assign_Mission(MISSION_HARVEST);
        best->Commence();

        DEV_DEBUG_INFO("AF-0012: house={} frame={} mobile-refinery {} at {},{} harvesters {}/{}: sent {} from {},{} to {},{}\n",
            (int)house->HeapID, Frame, refinery->Class->Name(), center.X, center.Y, counts[m], wanted,
            best->Class->Name(), from.X, from.Y, best_ore.X, best_ore.Y);

        watched.push_back(Watch{ best, center, Frame, "sent", true });

        if (serves_fixed(best)) --fixed_spare;
        for (UnitClass*& unit : free) {
            if (unit == best) unit = nullptr;
        }
        ++counts[m];
    }
}


/**
 *  Can harvesters of this type unload at this refinery? TS sends a harvester
 *  only to buildings on its Dock= list; an empty list is taken as any refinery.
 */
bool HouseClassExtension::AdvAI_Can_Dock(const UnitTypeClass* harvester, const BuildingTypeClass* refinery)
{
    if (harvester == nullptr || refinery == nullptr) return false;
    const auto& docks = harvester->Dock;
    if (docks.Count() == 0) return refinery->IsRefinery;
    return std::find(docks.begin(), docks.end(), refinery) != docks.end();
}
