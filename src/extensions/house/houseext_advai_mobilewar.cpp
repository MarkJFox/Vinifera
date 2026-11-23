/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  AF-0011: Advanced AI use of mobile war factories.
 *
 *          A mobile war factory is a vehicle whose DeploysInto building has
 *          IsMobileWar=yes. TS makes such a building the primary factory when
 *          it is deployed, so new vehicles leave it near the front. The AI
 *          never built or deployed these vehicles on its own; this file:
 *
 *          - orders one when the house has a fixed war factory, spare money
 *            and an enemy to face,
 *          - drives it to a site part of the way towards the enemy base,
 *            out of range of enemy buildings, and deploys it there,
 *          - packs it up and moves it when the site falls behind or becomes
 *            unsafe,
 *          - deploys it where it stands when the fixed war factories are gone.
 *
 *          All decisions read only simulation state (no shroud or radar of
 *          the local player), so multiplayer clients stay in sync.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "houseext.h"

#include "building.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "cell.h"
#include "debughandler.h"
#include "extension.h"
#include "factory.h"
#include "house.h"
#include "housetype.h"
#include "iomap.h"
#include "rules.h"
#include "rulesext.h"
#include "techno.h"
#include "technotype.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "unit.h"
#include "unittype.h"
#include "unittypeext.h"
#include "weapontype.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>


namespace {

/**
 *  How far around the wanted point a deploy site is searched, in cells.
 */
constexpr int MOBILE_WAR_SITE_RADIUS = 12;

/**
 *  The site stays at least this many cells away from the own base center.
 */
constexpr int MOBILE_WAR_MIN_BASE_DISTANCE = 8;

/**
 *  Failed trips or deploy attempts before a new site is searched.
 */
constexpr int MOBILE_WAR_MAX_ATTEMPTS = 3;

/**
 *  A move must bring the factory at least this many cells closer to the
 *  wanted point to be worth packing up for.
 */
constexpr int MOBILE_WAR_RELOCATE_GAIN = 5;


/**
 *  Approximate distance between two cells, in cells. Integer only, so the
 *  result is identical on every machine.
 */
int Cell_Span(const Cell& a, const Cell& b)
{
    const int dx = std::abs(a.X - b.X);
    const int dy = std::abs(a.Y - b.Y);
    const int hi = dx > dy ? dx : dy;
    const int lo = dx > dy ? dy : dx;
    return hi + lo / 2;
}


bool Is_Live_Building(const BuildingClass* building)
{
    return building != nullptr && building->IsActive && !building->IsInLimbo && building->Strength > 0;
}


/**
 *  Deploys the vehicle where it stands. The placement is checked first, so a
 *  failed attempt neither scatters into MISSION_HUNT (UnitClass::Mission_Unload
 *  does that for the computer) nor makes EVA speak.
 */
bool Deploy_Mobile_War_Factory(UnitClass* unit)
{
    BuildingTypeClass* buildingtype = unit->Class->DeploysInto;
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
 *  Are the cells around the footprint free of buildings? The factory needs
 *  room to let its vehicles out and must not wall in other buildings.
 */
bool Is_Footprint_Surrounding_Clear(const BuildingTypeClass* buildingtype, const Cell& topleft)
{
    const int width = buildingtype->Width();
    const int height = buildingtype->Height();

    for (int y = -1; y <= height; ++y) {
        for (int x = -1; x <= width; ++x) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                continue;
            }
            const Cell cell(topleft.X + x, topleft.Y + y);
            if (!Map.In_Local_Radar(cell)) {
                continue;
            }
            if (Map[cell].Cell_Building() != nullptr) {
                return false;
            }
        }
    }

    return true;
}

} // namespace


/**
 *  Is this vehicle a mobile war factory?
 */
bool HouseClassExtension::Is_Mobile_War_Factory_Type(const UnitTypeClass* type)
{
    if (type == nullptr || type->DeploysInto == nullptr) {
        return false;
    }

    const BuildingTypeClass* buildingtype = type->DeploysInto;
    return buildingtype->IsMobileWar && (buildingtype->IsWeaponsFactory || buildingtype->ToBuild == RTTI_UNITTYPE);
}


/**
 *  Does the house own a war factory that is not a mobile one?
 */
bool HouseClassExtension::AdvAI_Has_Fixed_Weapons_Factory() const
{
    const HouseClass* house = This_Const();

    for (int i = 0; i < Buildings.Count(); i++) {
        const BuildingClass* building = Buildings[i];
        if (building->House != house || !Is_Live_Building(building)) continue;

        const BuildingTypeClass* type = building->Class;
        if (type->IsMobileWar || type->ToBuild != RTTI_UNITTYPE) continue;
        if (Extension::Fetch(type)->IsNaval) continue;

        return true;
    }

    return false;
}


/**
 *  The enemy house the factory moves towards: the current enemy, otherwise
 *  the nearest active enemy house with a base.
 */
const HouseClass* HouseClassExtension::AdvAI_Mobile_War_Front_Enemy() const
{
    const HouseClass* house = This_Const();

    auto valid = [house](const HouseClass* other) {
        return other != nullptr && other != house && !other->IsDefeated && other->Class != nullptr &&
            !other->Class->IsMultiplayPassive && !house->Is_Ally(other) &&
            other->Center != COORD_NONE && other->CurBuildings > 0;
    };

    if (house->Enemy != HOUSE_NONE && valid(Houses[house->Enemy])) {
        return Houses[house->Enemy];
    }

    if (house->Center == COORD_NONE) {
        return nullptr;
    }

    const Cell own = house->Center.As_Cell();
    const HouseClass* nearest = nullptr;
    int nearest_distance = INT_MAX;

    for (int i = 0; i < Houses.Count(); i++) {
        const HouseClass* other = Houses[i];
        if (!valid(other)) continue;

        const int distance = Cell_Span(own, other->Center.As_Cell());
        if (distance < nearest_distance) {
            nearest = other;
            nearest_distance = distance;
        }
    }

    return nearest;
}


/**
 *  The point the factory should deploy at: AdvancedAIMobileWarFactoryFrontRatio
 *  percent of the way from the own base center to the enemy base center, then
 *  pulled back towards the own base until no enemy building is too close.
 *  Returns CELL_NONE if no such point lies far enough from the own base.
 */
Cell HouseClassExtension::AdvAI_Mobile_War_Front_Point() const
{
    const HouseClass* house = This_Const();
    const HouseClass* enemy = AdvAI_Mobile_War_Front_Enemy();

    if (enemy == nullptr || house->Center == COORD_NONE) {
        return CELL_NONE;
    }

    const Cell own = house->Center.As_Cell();
    const Cell target = enemy->Center.As_Cell();
    const int dx = target.X - own.X;
    const int dy = target.Y - own.Y;

    for (int percent = RuleExtension->AdvancedAIMobileWarFactoryFrontRatio; percent > 0; percent -= 5) {
        const Cell cell(own.X + dx * percent / 100, own.Y + dy * percent / 100);

        if (Cell_Span(cell, own) < MOBILE_WAR_MIN_BASE_DISTANCE) {
            break;
        }

        if (Map.In_Local_Radar(cell) && AdvAI_Is_Mobile_War_Site_Safe(cell)) {
            return cell;
        }
    }

    return CELL_NONE;
}


/**
 *  Is the cell far enough from every enemy building? Armed buildings also
 *  need their weapon range plus three cells.
 */
bool HouseClassExtension::AdvAI_Is_Mobile_War_Site_Safe(const Cell& cell) const
{
    const HouseClass* house = This_Const();
    const Coord center = Map[cell].Center_Coord();
    const int safe = RuleExtension->AdvancedAIMobileWarFactorySafeDistance * CELL_LEPTON_W;

    for (int i = 0; i < Buildings.Count(); i++) {
        const BuildingClass* building = Buildings[i];
        if (!Is_Live_Building(building) || building->House == nullptr || building->House == house) continue;
        if (building->House->Class == nullptr || building->House->Class->IsMultiplayPassive) continue;
        if (house->Is_Ally(building->House)) continue;

        int limit = safe;
        if (building->Is_Weapon_Equipped()) {
            const WeaponInfoStruct* info = building->Get_Weapon(WEAPON_SLOT_PRIMARY);
            if (info != nullptr && info->Weapon != nullptr) {
                limit = std::max(limit, (int)info->Weapon->Range + 3 * CELL_LEPTON_W);
            }
        }

        if (limit > 0 && center.Distance_To(building->Center_Coord()) < limit) {
            return false;
        }
    }

    return true;
}


/**
 *  Finds the nearest cell to "point" where the vehicle can stand and deploy:
 *  reachable from "zone_cell", a legal footprint with free cells around it,
 *  and safe from enemy buildings. Equally near cells are decided by the
 *  shorter trip from "zone_cell". Returns CELL_NONE if there is none.
 *
 *  Lift the vehicle off the map before calling this, so its own cells count
 *  as free.
 */
Cell HouseClassExtension::AdvAI_Find_Mobile_War_Site(const UnitTypeClass* type, const Cell& point, const Cell& zone_cell) const
{
    if (type == nullptr || type->DeploysInto == nullptr || point == CELL_NONE) {
        return CELL_NONE;
    }

    BuildingTypeClass* buildingtype = type->DeploysInto;
    const bool mobile = buildingtype->Is_Deployable();
    const int zone = Map.Get_Cell_Zone(zone_cell, type->MZone, false);

    Cell best = CELL_NONE;
    int best_distance = INT_MAX;
    int best_trip = INT_MAX;

    for (int ring = 0; ring <= MOBILE_WAR_SITE_RADIUS; ++ring) {

        /**
         *  Every cell of this ring is at least "ring" cells from the point.
         */
        if (ring > best_distance) {
            break;
        }

        for (int y = -ring; y <= ring; ++y) {
            for (int x = -ring; x <= ring; ++x) {

                if (std::abs(x) != ring && std::abs(y) != ring) {
                    continue;
                }

                const Cell stand(point.X + x, point.Y + y);
                if (!Map.In_Local_Radar(stand)) {
                    continue;
                }

                const int distance = Cell_Span(stand, point);
                const int trip = Cell_Span(stand, zone_cell);
                if (distance > best_distance || (distance == best_distance && trip >= best_trip)) {
                    continue;
                }

                if (Map.Get_Cell_Zone(stand, type->MZone, false) != zone) {
                    continue;
                }

                /**
                 *  UnitClass::Try_To_Deploy places the building at the north-west
                 *  neighbour of the vehicle, unless it is a mobile deployer.
                 */
                Cell topleft = mobile ? stand : Adjacent_Cell(stand, FACING_NW);
                if (!buildingtype->Legal_Placement(topleft, nullptr)) {
                    continue;
                }

                if (!Is_Footprint_Surrounding_Clear(buildingtype, topleft)) {
                    continue;
                }

                if (!AdvAI_Is_Mobile_War_Site_Safe(stand)) {
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
 *  Picks a mobile war factory to order, or nullptr. Called by AdvAI_AI_Unit
 *  when no other order is pending.
 */
const UnitTypeClass* HouseClassExtension::AdvAI_Mobile_War_Factory_To_Build(DynamicVectorClass<BuildingTypeClass*>& owned) const
{
    if (!RuleExtension->IsAdvancedAIMobileWarFactory) return nullptr;

    const HouseClass* house = This_Const();

    /**
     *  Diagnosis: why no order is given. Written when the reason changes, at
     *  most once a minute per house. Only for the log, so not saved.
     */
    char reason[160] = "";
    auto skip = [&]() -> const UnitTypeClass* {
        struct Trace { std::string Reason; int Frame = INT_MIN / 2; };
        static std::map<int, Trace> traces;
        Trace& trace = traces[(int)house->HeapID];
        if (Frame < trace.Frame) trace = Trace(); // New game.
        if (trace.Reason != reason && Frame >= trace.Frame + TICKS_PER_MINUTE) {
            DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war skip reason={}\n", (int)house->HeapID, Frame, reason);
            trace.Reason = reason;
            trace.Frame = Frame;
        }
        return nullptr;
    };

    if (Frame < RuleExtension->AdvancedAIMobileWarFactoryDelay) {
        std::snprintf(reason, sizeof(reason), "delay (until frame %d)", RuleExtension->AdvancedAIMobileWarFactoryDelay);
        return skip();
    }
    if (Frame < MobileWarLostFrame + RuleExtension->AdvancedAIMobileWarFactoryRebuildDelay) {
        std::snprintf(reason, sizeof(reason), "rebuild-delay (until frame %d)", MobileWarLostFrame + RuleExtension->AdvancedAIMobileWarFactoryRebuildDelay);
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

    /**
     *  One at a time: none on the map, none in production. Not a reason worth
     *  logging; the factory AI reports its own state.
     */
    for (int i = 0; i < Units.Count(); i++) {
        const UnitClass* unit = Units[i];
        if (unit->House == house && unit->Strength > 0 && Is_Mobile_War_Factory_Type(unit->Class)) return nullptr;
    }
    for (int i = 0; i < Buildings.Count(); i++) {
        const BuildingClass* building = Buildings[i];
        if (building->House == house && building->Strength > 0 && building->Class->IsMobileWar) return nullptr;
    }
    for (int i = 0; i < Factories.Count(); i++) {
        const FactoryClass* factory = Factories[i];
        if (factory == nullptr || factory->House != house) continue;
        const TechnoClass* product = factory->Get_Object();
        if (product != nullptr && product->RTTI == RTTI_UNIT &&
            Is_Mobile_War_Factory_Type(static_cast<const UnitClass*>(product)->Class)) return nullptr;
    }

    if (!AdvAI_Has_Fixed_Weapons_Factory()) {
        std::snprintf(reason, sizeof(reason), "no-fixed-war-factory");
        return skip();
    }
    if (AdvAI_Mobile_War_Front_Point() == CELL_NONE) {
        const HouseClass* enemy = AdvAI_Mobile_War_Front_Enemy();
        std::snprintf(reason, sizeof(reason), "no-front-point (enemy=%d)", enemy != nullptr ? (int)enemy->HeapID : -1);
        return skip();
    }

    bool any_type = false;
    for (int i = 0; i < UnitTypes.Count(); i++) {
        const UnitTypeClass* unittype = UnitTypes[i];
        if (!Is_Mobile_War_Factory_Type(unittype)) continue;
        if (Extension::Fetch(unittype)->IsNaval) continue;
        any_type = true;

        if (!AdvAI_Can_Produce(unittype, owned)) {
            std::snprintf(reason, sizeof(reason), "cannot-produce %s (techlevel %d/%d, owner or prerequisites)",
                unittype->Name(), (int)unittype->Level, house->Control.TechLevel);
            continue;
        }

        const AdvancedAIUnitStartBudget budget = Check_Advanced_AI_Unit_Start(unittype);
        if (budget.Blocked != nullptr) {
            std::snprintf(reason, sizeof(reason), "budget %s %s (uncommitted=%lld cost=%lld reserve=%lld)",
                unittype->Name(), budget.Blocked, budget.Uncommitted, budget.Cost, budget.Reserve);
            continue;
        }
        const long long left = budget.Uncommitted - budget.Cost - budget.Reserve;
        if (left < RuleExtension->AdvancedAIMobileWarFactoryReserve) {
            std::snprintf(reason, sizeof(reason), "money %s (left=%lld wanted=%d)",
                unittype->Name(), left, RuleExtension->AdvancedAIMobileWarFactoryReserve);
            continue;
        }

        return unittype;
    }

    if (!any_type) {
        std::snprintf(reason, sizeof(reason), "no-mobile-war-factory-type");
    }
    return skip();
}


/**
 *  Drives the vehicle to its site and deploys it there.
 */
void HouseClassExtension::AdvAI_Mobile_War_Factory_Vehicle_AI(UnitClass* unit, bool emergency)
{
    /**
     *  Members of scripted or trigger created teams keep their orders.
     */
    if (unit->Team != nullptr || unit->IsDeploying) {
        return;
    }

    const MissionType mission = unit->Get_Mission();

    /**
     *  UnitClass::Mission_Unload sends a computer vehicle into MISSION_HUNT when
     *  its deploy fails. An unarmed factory would drive at the enemy; stop it.
     */
    if (mission == MISSION_HUNT) {
        unit->Assign_Destination(nullptr);
        unit->Assign_Target(nullptr);
        unit->Assign_Mission(MISSION_GUARD);
        unit->Commence();
        return;
    }

    if (mission != MISSION_GUARD && mission != MISSION_GUARD_AREA && mission != MISSION_MOVE) {
        return;
    }

    if (unit->NavCom != nullptr || unit->Locomotion->Is_Moving()) {
        return;
    }

    const HouseClass* house = This_Const();
    const Cell here = unit->Get_Cell();

    if (MobileWarSite != CELL_NONE) {

        if (here == MobileWarSite) {
            if (Deploy_Mobile_War_Factory(unit)) {
                DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war deploy {} at {},{}\n",
                    (int)house->HeapID, Frame, unit->Class->Name(), here.X, here.Y);
                MobileWarDeployedFrame = Frame;
                MobileWarSite = CELL_NONE;
                MobileWarAttempts = 0;
                return;
            }
            if (++MobileWarAttempts < MOBILE_WAR_MAX_ATTEMPTS) {
                return;
            }
        }
        else if (++MobileWarAttempts < MOBILE_WAR_MAX_ATTEMPTS) {

            /**
             *  Stopped short of the site (blocked or ordered elsewhere by the
             *  game). Try again.
             */
            unit->Assign_Destination(&Map[MobileWarSite]);
            unit->Assign_Mission(MISSION_MOVE);
            unit->Commence();
            return;
        }

        DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war site {},{} failed, searching again\n",
            (int)house->HeapID, Frame, MobileWarSite.X, MobileWarSite.Y);
        MobileWarSite = CELL_NONE;
        MobileWarAttempts = 0;
    }

    /**
     *  Without a fixed war factory the house needs production now: deploy
     *  where it stands. Otherwise head for the front.
     */
    Cell point = emergency ? here : AdvAI_Mobile_War_Front_Point();
    if (point == CELL_NONE) {
        point = house->Center != COORD_NONE ? house->Center.As_Cell() : here;
    }

    unit->Mark(MARK_UP);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_UP);
    const Cell site = AdvAI_Find_Mobile_War_Site(unit->Class, point, here);
    unit->Locomotion->Mark_All_Occupation_Bits(MARK_DOWN);
    unit->Mark(MARK_DOWN);

    if (site == CELL_NONE) {
        DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war no site near {},{}{}\n",
            (int)house->HeapID, Frame, point.X, point.Y, emergency ? " (emergency)" : "");
        MobileWarNextFrame = Frame + TICKS_PER_SECOND * 10;
        return;
    }

    MobileWarSite = site;
    MobileWarAttempts = 0;

    DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war {} site {},{} for point {},{}{}\n",
        (int)house->HeapID, Frame, unit->Class->Name(), site.X, site.Y, point.X, point.Y, emergency ? " (emergency)" : "");

    if (site == here) {
        if (Deploy_Mobile_War_Factory(unit)) {
            MobileWarDeployedFrame = Frame;
            MobileWarSite = CELL_NONE;
        }
        return;
    }

    unit->Assign_Destination(&Map[site]);
    unit->Assign_Mission(MISSION_MOVE);
    unit->Commence();
}


/**
 *  Packs the deployed factory up and moves it when its site fell behind the
 *  front or enemy buildings came too close.
 */
void HouseClassExtension::AdvAI_Mobile_War_Factory_Building_AI(BuildingClass* building)
{
    if (MobileWarDeployedFrame == INT_MIN / 2) {
        MobileWarDeployedFrame = Frame; // Placed by the map or loaded from an older state.
    }

    if (RuleExtension->AdvancedAIMobileWarFactoryRelocateDistance <= 0) return;
    if (Frame < MobileWarDeployedFrame + RuleExtension->AdvancedAIMobileWarFactoryRelocateDelay) return;

    /**
     *  The only war factory stays. So does one that is busy: producing,
     *  letting a vehicle out, being built, sold or captured.
     */
    if (!AdvAI_Has_Fixed_Weapons_Factory()) return;
    if (building->Get_Mission() != MISSION_GUARD || building->Factory != nullptr) return;
    if (AdvAI_Is_Recently_Attacked()) return;

    const UnitTypeClass* unittype = building->Class->UndeploysInto;
    if (unittype == nullptr) return;

    const Cell point = AdvAI_Mobile_War_Front_Point();
    if (point == CELL_NONE) return;

    const Cell center = building->Center_Coord().As_Cell();
    const bool safe = AdvAI_Is_Mobile_War_Site_Safe(center);
    const int offset = Cell_Span(center, point);
    if (safe && offset <= RuleExtension->AdvancedAIMobileWarFactoryRelocateDistance) return;

    /**
     *  The vehicle appears south-east of the building (BuildingClass::
     *  Mission_Deconstruction), so the trip starts there.
     */
    const Cell exit = Adjacent_Cell(building->Get_Cell(), FACING_SE);
    const Cell site = AdvAI_Find_Mobile_War_Site(unittype, point, exit);
    if (site == CELL_NONE) return;
    if (safe && Cell_Span(site, point) + MOBILE_WAR_RELOCATE_GAIN > offset) return;

    DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war relocate from {},{} to {},{} ({})\n",
        (int)This()->HeapID, Frame, center.X, center.Y, site.X, site.Y, safe ? "front moved" : "unsafe");

    MobileWarSite = site;
    MobileWarAttempts = 0;
    MobileWarNextFrame = Frame + TICKS_PER_SECOND * 5;

    /**
     *  IsMobileWar makes the deconstruction undeploy instead of sell. The
     *  archive target becomes the vehicle's destination, and without one
     *  the deconstruction would also release survivors.
     */
    building->Assign_Archive_Target(&Map[site]);
    building->Assign_Mission(MISSION_DECONSTRUCTION);
    building->Commence();
}


/**
 *  Called from AdvAI_Expert_AI.
 */
void HouseClassExtension::AdvAI_Mobile_War_Factory_AI()
{
    if (!RuleExtension->IsAdvancedAIMobileWarFactory) return;
    if (Frame < MobileWarNextFrame) return;
    MobileWarNextFrame = Frame + TICKS_PER_SECOND * 2;

    HouseClass* house = This();
    const bool emergency = !AdvAI_Has_Fixed_Weapons_Factory();
    bool owned = false;

    /**
     *  BuildLimit=1 is the rule for these; with more, only the first of each
     *  kind is managed and the others are left alone.
     */
    for (int i = 0; i < Units.Count(); i++) {
        UnitClass* unit = Units[i];
        if (unit->House != house || !unit->IsActive || unit->IsInLimbo || unit->Strength <= 0) continue;
        if (!Is_Mobile_War_Factory_Type(unit->Class)) continue;

        if (!owned) {
            AdvAI_Mobile_War_Factory_Vehicle_AI(unit, emergency);
        }
        owned = true;
    }

    bool building_seen = false;
    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* building = Buildings[i];
        if (building->House != house || !Is_Live_Building(building) || !building->Class->IsMobileWar) continue;

        if (!building_seen && !owned) {
            AdvAI_Mobile_War_Factory_Building_AI(building);
        }
        building_seen = true;
    }
    owned = owned || building_seen;

    if (IsMobileWarOwned && !owned) {
        DEV_DEBUG_INFO("AF-0011: house={} frame={} mobile-war lost\n", (int)house->HeapID, Frame);
        MobileWarLostFrame = Frame;
        MobileWarSite = CELL_NONE;
        MobileWarAttempts = 0;
        MobileWarDeployedFrame = INT_MIN / 2;
    }

    IsMobileWarOwned = owned;
}
