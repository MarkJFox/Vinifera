/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  AF-0009B1 opt-in infrastructure selection.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 *
 *  Infrastructure priorities adapted from Rampastring's Advanced AI:
 *  Rampastring/Vinifera, houseext_hooks.cpp, AdvAI_Evaluate_Get_Best_Building
 *  and AdvAI_Get_Building_To_Build. Historical commit 0edbf6bb098747636f7548f6085b389764c735a8;
 *  reference commit 2438beeab932a6224268a253ead07a1dadff6d37.
 *  Original authors/notices include CCHyper and Rampastring.
 *
 *  Vinifera is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version. Distributed WITHOUT ANY WARRANTY; without even the
 *  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See LICENSE.md for the GNU General Public License.
 ******************************************************************************/

#include "always.h"

#include "houseext.h"

#include "aircraft.h"
#include "aircrafttype.h"
#include "building.h"
#include "buildingext.h"
#include "buildingtypeext.h"
#include "bullettype.h"
#include "cell.h"
#include "ccini.h"
#include "debughandler.h"
#include "factory.h"
#include "infantry.h"
#include "infantrytype.h"
#include "mapext_hooks.h"
#include "mouse.h"
#include "overlaytype.h"
#include "prerequisitegroup.h"
#include "rules.h"
#include "rulesext.h"
#include "side.h"
#include "sideext.h"
#include "team.h"
#include "teamext.h"
#include "teamtype.h"
#include "technoext.h"
#include "technotypeext.h"
#include "tibsun_globals.h"
#include "unit.h"
#include "unittype.h"
#include "verses.h"
#include "vinifera_globals.h"
#include "warheadtypeext.h"
#include "weapontype.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    // Log deduplication only; never saved or used for AI decisions.
    std::set<std::tuple<int, int, std::string, int>> AdvancedAIErrors;
    std::set<std::tuple<int, int, std::string>> AdvancedAIPowerSelections;
    // Per candidate: -2 = affordable, -1 = no plant, otherwise the selected type.
    std::map<std::pair<int, int>, int> AdvancedAIPowerDiagnostics;
    // AF-0009U: Above zero while defense candidates are only rated, not ordered.
    int PowerTraceMute = 0;
    using ProductionTraceKey = std::tuple<int, std::string_view, int>;
    struct ProductionTraceState
    {
        std::string_view Reason;
        int RequestedType;
        int Order;
        int ProductID;
        std::string_view Production;
        int Refineries;
        int Harvesters;
        int UnitOrder;
        int HouseState;
        bool TiberiumShort;
        long long MinimumFunds;
        bool Affordable;
        bool PowerReserveMet;

        bool operator==(const ProductionTraceState&) const = default;
    };
    std::map<ProductionTraceKey, ProductionTraceState> ProductionTraces;
    std::map<std::tuple<int, RTTIType, int>, std::tuple<std::string_view, std::string_view, std::string_view>> UnitStartTraces;

    struct BuildingStartState
    {
        // A dependency of the current C3 plan. BuildStructure remains the only order.
        AdvancedAIPowerGoal Goal{};
        long GoalFrame = -1;
        int GoalCount = 0;
        StructType Supply = STRUCT_NONE;
        int SupplyProduct = -1;
        bool SupplyCompleted = false;
        bool GoalOptional = false;
        long AttemptFrame = -1;
        StructType RejectedType = STRUCT_NONE;
        StructType SelectedType = STRUCT_NONE;
        bool Optional = false;
        bool ThreatDefenseOrder = false;
        bool EmergencyDefenseOrder = false;
    };
    std::map<int, BuildingStartState> BuildingStarts;
    struct BuildingLossState
    {
        std::map<int, StructType> Owned;
        std::map<StructType, int> ReplaceTo;
        // AF-0009U: Failed placements of a pending replacement (type -> count).
        std::map<StructType, int> PlacementFailures;
    };
    std::map<int, BuildingLossState> BuildingLosses;
    /**
     *  AF-0009U: A replacement that could not be placed this often is dropped.
     *  Nod built, rejected and abandoned the same laser and SAM replacements
     *  160 times (log 19.11.2026 19-57-29), every 30 s after the site deferral.
     */
    constexpr int ReplacementPlacementLimit = 2;
    // AF-0009U: Frames at which defense structures of a type were lost (house, type).
    std::map<std::pair<int, int>, std::vector<long>> DefenseLosses;
    constexpr long DefenseLossWindow = 120 * TICKS_PER_SECOND;
    constexpr int DefenseLossLimit = 2;

    int Recent_Defense_Losses(int house_id, int type)
    {
        const auto found = DefenseLosses.find({house_id, type});
        if (found == DefenseLosses.end()) return 0;
        return static_cast<int>(std::count_if(found->second.begin(), found->second.end(), [](long frame) {
            return Frame >= frame && Frame - frame <= DefenseLossWindow;
        }));
    }
    struct BuildingAttack
    {
        Cell Target;
        long LastHit;
        int Damage;
        // AF-0009U: Last hit on a real base building near the base center.
        long LastBaseHit = -1;
    };
    std::map<int, std::map<int, BuildingAttack>> BuildingAttacks;
    std::map<int, std::tuple<int, StructType, int, bool, bool>> DefenseResponseTraces;
    bool Attack_Expired(const BuildingAttack& hit)
    {
        return Frame < hit.LastHit || Frame - hit.LastHit > TICKS_PER_MINUTE;
    }
    constexpr int Failed_Site_Retry_Interval = 30 * TICKS_PER_SECOND;
    std::map<std::pair<int, StructType>, long> FailedBuildingSites;

    struct BuildingOrderTrace
    {
        std::string_view Reason;
        std::string_view Source;
        std::string_view Production;
        StructType Type;
        StructType Order;
        int ProductID;
        Cell Target;

        bool operator==(const BuildingOrderTrace&) const = default;
    };
    std::map<std::tuple<int, std::string_view, StructType>, BuildingOrderTrace> BuildingOrderTraces;
    using TechnologyTrace = std::tuple<std::string_view, int, int, int, int, int, int>;
    std::map<int, TechnologyTrace> TechnologyTraces;

    FactoryClass* Find_Building_Factory(const HouseClass* house)
    {
        for (FactoryClass* factory : Factories) {
            if (!factory || factory->House != house) continue;
            const TechnoClass* product = factory->Get_Object();
            if (product && product->RTTI == RTTI_BUILDING) return factory;
        }
        return nullptr;
    }

    bool Valid_Building_Type(const BuildingTypeClass* type)
    {
        if (!type || type->RTTI != RTTI_BUILDINGTYPE) return false;
        const int id = type->HeapID;
        return id >= STRUCT_FIRST && id < BuildingTypes.Count() && BuildingTypes[id] == type;
    }

    const BuildingTypeClass* Building_Order_Type(StructType order)
    {
        return order >= STRUCT_FIRST && order < BuildingTypes.Count() ? BuildingTypes[order] : nullptr;
    }

    const char* Building_Order_Role(const BuildingTypeClass* type, std::string_view source)
    {
        if (!type) return "none";
        if (type->Power > type->Drain) return type->PowersUpBuilding.empty() ? "power" : "power-upgrade";
        if (type->IsBaseDefense ||
            std::find(Rule->BuildAA.begin(), Rule->BuildAA.end(), type) != Rule->BuildAA.end() ||
            std::find(Rule->BuildDefense.begin(), Rule->BuildDefense.end(), type) != Rule->BuildDefense.end() ||
            std::find(Rule->BuildPDefense.begin(), Rule->BuildPDefense.end(), type) != Rule->BuildPDefense.end()) return "defense";
        if (type->SuperWeapon != SUPER_NONE || type->SuperWeapon2 != SUPER_NONE) return "superweapon";
        if (type->IsRefinery) return source == "B2" ? "resource-expansion" : "critical-supply";
        if (source == "B1") return "production-infrastructure";
        return source == "B2" ? "optional-infrastructure" : "unknown";
    }

    bool Usable_Owned_Building(const BuildingClass* building, const HouseClass* house)
    {
        return building && building->IsActive && building->IsDown && !building->IsInLimbo &&
            building->Strength > 0 && building->House == house &&
            building->Mission != MISSION_DECONSTRUCTION && building->MissionQueue != MISSION_DECONSTRUCTION;
    }

    bool Operational_Building(const BuildingClass* building)
    {
        return building->IsOn && building->Is_Powered_On() && building->EMPFramesRemaining <= 0 &&
            building->Mission != MISSION_CONSTRUCTION && building->MissionQueue != MISSION_CONSTRUCTION;
    }

    int Placed_Building_Count(const HouseClass* house, const BuildingTypeClass* type)
    {
        int count = 0;
        for (const BuildingClass* building : Buildings) {
            if (!Usable_Owned_Building(building, house)) continue;
            if (type->PowersUpBuilding.empty()) count += building->Class == type;
            else count += static_cast<int>(std::count(std::begin(building->Upgrades), std::end(building->Upgrades), type));
        }
        return count;
    }

    int Operating_Refineries_With_Harvesters(const HouseClass* house)
    {
        std::vector<const BuildingTypeClass*> refineries;
        for (const BuildingClass* building : Buildings) {
            if (Usable_Owned_Building(building, house) && building->Class->IsRefinery && Operational_Building(building)) {
                refineries.push_back(building->Class);
            }
        }

        int harvesters = 0;
        for (const UnitClass* unit : Units) {
            if (!unit || unit->House != house || !unit->IsActive || unit->IsInLimbo || unit->Strength <= 0 ||
                !unit->Class->IsToHarvest) continue;
            const auto& docks = unit->Class->Dock;
            if (std::any_of(refineries.begin(), refineries.end(), [&](const BuildingTypeClass* refinery) {
                return std::find(docks.begin(), docks.end(), refinery) != docks.end();
            })) ++harvesters;
        }
        return std::min(static_cast<int>(refineries.size()), harvesters);
    }

    const BuildingTypeClass* Fund_Optional_Building(const HouseClassExtension& extension,
        const BuildingTypeClass* type, long long funds, long long reserve, long long& required, const char*& reason,
        bool check_buildability = true, bool require_operating_power = false)
    {
        if (extension.Is_Advanced_AI_Building_Type_Deferred(type)) {
            reason = "placement-retry-cooldown";
            return nullptr;
        }
        if (check_buildability && !extension.Can_Advanced_AI_Build_Building(type)) {
            reason = "infrastructure-not-buildable";
            return nullptr;
        }
        HouseClass* house = extension.This();
        const auto* choice = require_operating_power && type->IsPowered && house->Power < house->Drain
            ? extension.Select_Advanced_AI_Power_Plant() : extension.Prioritize_Advanced_AI_Power(type);
        const long long cost = type->Cost_Of(house);
        if (!choice || cost < 0) {
            reason = "infrastructure-power-or-cost-unavailable";
            return nullptr;
        }
        const long long power_cost = choice == type ? 0 : choice->Cost_Of(house);
        required = reserve + cost + power_cost;
        if (power_cost < 0 || funds < required) {
            reason = "infrastructure-budget-insufficient";
            return nullptr;
        }
        return choice;
    }

    enum class InfrastructureRole { Power, Refinery, Barracks, Weapons };

    bool Matches_Role(const BuildingTypeClass* type, InfrastructureRole role)
    {
        // B1 only selects standalone infrastructure, never upgrades or naval yards.
        if (!Valid_Building_Type(type) || !type->PowersUpBuilding.empty()) return false;
        switch (role) {
        case InfrastructureRole::Power: return type->Power > type->Drain;
        case InfrastructureRole::Refinery: return type->IsRefinery;
        case InfrastructureRole::Barracks: return type->ToBuild == RTTI_INFANTRYTYPE;
        case InfrastructureRole::Weapons:
            return type->ToBuild == RTTI_UNITTYPE && !Extension::Fetch(type)->IsNaval;
        }
        return false;
    }

    bool Team_Needs_Members(const TeamClass* team, const HouseClass* house)
    {
        return team && team->House == house && team->Class && team->Class->TaskForce &&
            ((team->Class->IsReinforcable && !team->IsFullStrength) || (!team->IsForcedActive && !team->IsHasBeen));
    }

    bool Has_Unfilled_Team_Demand(HouseClass* house, const TechnoTypeClass* type, int pending)
    {
        int missing = 0;
        DynamicVectorClass<const TechnoTypeClass*> members;
        for (TeamClass* team : Teams) {
            if (!Team_Needs_Members(team, house)) continue;
            members.Reset_Active();
            team->Team_Members(members);
            for (const auto* member : members) missing += member == type;
        }
        if (missing <= pending) return false;

        const auto needs_more = [&](const auto& units) {
            for (const auto* unit : units) {
                if (unit && unit->TClass == type && unit->Is_Recruitable(house) && --missing <= pending) return false;
            }
            return true;
        };
        switch (type->RTTI) {
        case RTTI_INFANTRYTYPE: return needs_more(Infantry);
        case RTTI_UNITTYPE: return needs_more(Units);
        case RTTI_AIRCRAFTTYPE: return needs_more(Aircrafts);
        default: return false;
        }
    }

    enum class DefenseRole { Air, Infantry, Armor };
    constexpr std::array DefenseRoles{DefenseRole::Air, DefenseRole::Infantry, DefenseRole::Armor};

    int Defense_Range(const BuildingTypeClass* type, DefenseRole role)
    {
        int range = 0;
        for (const auto slot : {WEAPON_SLOT_PRIMARY, WEAPON_SLOT_SECONDARY}) {
            const auto* weapon = type->Fetch_Weapon_Info(slot).Weapon;
            if (!weapon || !weapon->Bullet || weapon->Attack <= 0) continue;
            if (role == DefenseRole::Air ? weapon->Bullet->IsAntiAircraft : weapon->Bullet->IsAntiGround) {
                range = std::max(range, static_cast<int>(weapon->Range));
            }
        }
        return range;
    }

    int Defense_Rating(const BuildingTypeClass* type, DefenseRole role)
    {
        switch (role) {
        case DefenseRole::Air: return type->AntiAirValue;
        case DefenseRole::Infantry: return type->AntiInfantryValue;
        case DefenseRole::Armor: return type->AntiArmorValue;
        }
        return 0;
    }

    struct DefenseThreat
    {
        Cell Target{0, 0};
        int Attackers = 0;
        long long Damage = 0;
        int AttackerID = -1;
        long LastHit = -1;
    };

    constexpr int Active_Defense_Hit_Window = 12 * TICKS_PER_SECOND;

    bool Hostile_To(const HouseClass* house, const TechnoClass* unit)
    {
        return unit && unit->IsActive && !unit->IsInLimbo && unit->Strength > 0 && unit->House &&
            unit->House != house && !unit->House->IsDefeated && unit->House->Class &&
            !unit->House->Class->IsMultiplayPassive && !house->Is_Ally(unit->House);
    }

    auto Active_Building_Threats(const HouseClass* house)
    {
        std::array<DefenseThreat, DefenseRoles.size()> threats{};
        const auto history = BuildingAttacks.find(static_cast<int>(house->HeapID));
        if (history == BuildingAttacks.end()) return threats;
        std::erase_if(history->second, [](const auto& entry) { return Attack_Expired(entry.second); });
        if (history->second.empty()) return threats;
        std::array<std::optional<std::pair<long, int>>, DefenseRoles.size()> best_focuses{};
        const auto count_attackers = [&](const auto& objects, DefenseRole list_role) {
            for (const TechnoClass* attacker : objects) {
                if (!Hostile_To(house, attacker)) continue;
                /**
                 *  AF-0009U: A flying attacker (for example a jumpjet infantry or vehicle)
                 *  is an air threat; only anti-aircraft defenses can respond to it.
                 */
                const DefenseRole role = attacker->In_Air() ? DefenseRole::Air : list_role;
                auto& threat = threats[static_cast<size_t>(role)];
                auto& best_focus = best_focuses[static_cast<size_t>(role)];
                const auto found = history->second.find(attacker->ID);
                if (found == history->second.end()) continue;
                const Cell position = attacker->Get_Cell();
                const auto& hit = found->second;
                if (Frame - hit.LastHit > Active_Defense_Hit_Window || !Map.In_Radar(position) || !Map.In_Radar(hit.Target) ||
                    position.Relative_Distance_To(hit.Target) > 20 * 20 ||
                    !Usable_Owned_Building(Map[hit.Target].Cell_Building(), house)) continue;
                ++threat.Attackers;
                threat.Damage += hit.Damage;
                const auto rank = std::make_pair(-hit.LastHit, hit.Target.As_Cell_Number());
                if (!best_focus || rank < *best_focus) {
                    best_focus = rank;
                    threat.Target = hit.Target;
                    threat.AttackerID = attacker->ID;
                    threat.LastHit = hit.LastHit;
                }
            }
        };
        count_attackers(Aircrafts, DefenseRole::Air);
        count_attackers(Infantry, DefenseRole::Infantry);
        count_attackers(Units, DefenseRole::Armor);
        return threats;
    }

    /**
     *  AF-0009U: Building damage the attacker dealt to this house within the last minute.
     */
    long long Recent_Attacker_Damage(const HouseClass* house, int attacker_id)
    {
        const auto history = BuildingAttacks.find(static_cast<int>(house->HeapID));
        if (history == BuildingAttacks.end()) return 0;
        const auto found = history->second.find(attacker_id);
        if (found == history->second.end() || Attack_Expired(found->second)) return 0;
        return found->second.Damage;
    }

    /**
     *  AF-0009U: A single strong attacker (for example a hero unit) is weighed by
     *  the damage it already dealt, not only by the size of its group.
     */
    constexpr long long Significant_Attacker_Damage = 400;
    constexpr long long Urgent_Attacker_Damage = 1000;

    constexpr int Preventive_Scan_Interval = TICKS_PER_SECOND;
    constexpr int Preventive_Maximum_Distance = 16;
    constexpr int Preventive_Cooldown = 5 * TICKS_PER_SECOND;

    struct PreventiveContact
    {
        int ID;
        Cell Position;
        ArmorType Armor;
        RTTIType Category;
        DefenseRole Role;
        int Strength;
        bool Closing;
        bool InRange;
        bool ObservedBefore;
    };

    struct PreventiveGroup
    {
        Cell Asset;
        StructType AssetType;
        int Importance;
        std::vector<PreventiveContact> Contacts;
    };

    struct PreventiveDefenseState
    {
        long ScanFrame = -1;
        long EvaluatedScan = -1;
        long UrgentScan = -1;
        long LastEmergencyEvaluation = -1;
        long LastOrder = -1;
        Cell LastOrderAsset{0, 0};
        std::map<int, Cell> VisiblePositions;
        std::vector<PreventiveGroup> Groups;
        const BuildingTypeClass* Planned = nullptr;
        AdvancedAIDefenseFocus Focus;
    };

    std::map<int, PreventiveDefenseState> PreventiveDefenses;

    /**
     *  AF-0009U: Savings goal published by the building planner when it waits
     *  for credits. Unit starts leave this amount untouched so that the base
     *  can grow. Derived from the simulation and refreshed by the planner.
     */
    struct SavingsGoalState
    {
        long long Amount = 0;
        int Frame = 0;
        const char* Reason = "none";
        bool Economy = false;
        int Since = 0;      // First frame of the current continuous savings period.
    };
    std::map<int, SavingsGoalState> SavingsGoals;
    // The planner refreshes goals at least every ~30 s; two minutes bridge slow probes.
    constexpr int SavingsGoalLifetime = 2 * TICKS_PER_MINUTE;
    std::map<std::pair<int, int>, std::string_view> SavingsTraces;
    std::map<int, std::tuple<std::string_view, int, int, int, int>> PreventiveDefenseTraces;
    std::map<std::pair<int, StructType>, long> ThreatDefensePlacementFailures;

    int Weapon_Strength(const WeaponTypeClass* weapon, ArmorType armor, RTTIType category, bool air, bool passive)
    {
        if (!weapon || !weapon->Bullet || !weapon->WarheadPtr || weapon->Attack <= 0 || weapon->Range <= 0 ||
            (air ? !weapon->Bullet->IsAntiAircraft : !weapon->Bullet->IsAntiGround)) return 0;
        const auto* warhead = weapon->WarheadPtr;
        if (!Verses::Get_ForceFire(armor, warhead)) return 0;
        if (passive && !Verses::Get_PassiveAcquire(armor, warhead)) return 0;
        const double modifier = Verses::Get_Modifier(armor, warhead) * Extension::Fetch(warhead)->Fetch_Type_Modifier(category);
        if (!std::isfinite(modifier) || modifier <= 0) return 0;
        const double damage = static_cast<double>(weapon->Attack) * std::clamp<int>(weapon->Burst, 1, 4) *
            TICKS_PER_SECOND * modifier / std::max(TICKS_PER_SECOND, weapon->ROF);
        return static_cast<int>(std::clamp(damage, 1.0, 240.0));
    }

    Cell Building_Center_Cell(const BuildingClass* building)
    {
        const Cell origin = building->Get_Cell();
        return Cell(static_cast<short>(origin.X + building->Class->Width() / 2),
            static_cast<short>(origin.Y + building->Class->Height() / 2));
    }

    int Preventive_Asset_Importance(const BuildingTypeClass* type)
    {
        if (type->IsConstructionYard || type->IsRefinery) return 2;
        if (type->ToBuild != RTTI_NONE || type->Power > type->Drain || type->IsBase || type->IsRadar ||
            Rule->BuildTech.Is_Present(BuildingTypes[type->HeapID])) return 1;
        return 0;
    }

    bool Within_Defense_Range(const Cell& from, const Cell& to, int range)
    {
        return static_cast<long long>(from.Relative_Distance_To(to)) * CELL_LEPTON_W * CELL_LEPTON_W <=
            static_cast<long long>(range) * range;
    }

    void Scan_Preventive_Defense(const HouseClass* house, PreventiveDefenseState& state, bool refresh = false)
    {
        if (state.ScanFrame == Frame || (!refresh && state.ScanFrame >= 0 && Frame > state.ScanFrame &&
            Frame - state.ScanFrame < Preventive_Scan_Interval)) return;
        const bool recent = state.ScanFrame >= 0 && Frame >= state.ScanFrame && Frame - state.ScanFrame <= 2 * Preventive_Scan_Interval;
        state.ScanFrame = Frame;
        auto previous = std::move(state.VisiblePositions);
        state.VisiblePositions.clear();
        state.Groups.clear();

        std::vector<MapObserverSight> observers;
        for (const TechnoClass* unit : Technos) {
            if (!unit || unit->House != house || !unit->TClass || !unit->IsActive || unit->IsInLimbo || !unit->IsLocked ||
                unit->Strength <= 0) continue;
            int range = Extension::Fetch(unit)->Get_Sight_Range();
            if (unit->RTTI == RTTI_AIRCRAFT && unit->HeightAGL == 0) range = std::min(range, 1);
            if (range > 0) observers.push_back(Map_Observer_Sight(unit->PositionCoord, range));
        }
        if (observers.empty()) return;

        struct Asset { const BuildingClass* Building; Cell Position; int Importance; };
        std::vector<Asset> assets;
        const auto* extension = Extension::Fetch(house);
        for (const BuildingClass* building : Buildings) {
            if (!Usable_Owned_Building(building, house) || building->Class->IsWall || building->Class->IsGate ||
                building->Class == Rule->WallTower || extension->Is_Advanced_AI_Defense_Type(building->Class)) continue;
            const int importance = Preventive_Asset_Importance(building->Class);
            const Cell position = Building_Center_Cell(building);
            if (importance && Map.In_Radar(position)) assets.push_back({building, position, importance});
        }
        if (assets.empty()) return;

        std::map<std::pair<int, int>, PreventiveGroup> groups;
        const auto distance_to_asset = [](const Cell& position, const Asset& asset) {
            const Cell origin = asset.Building->Get_Cell();
            const short east = static_cast<short>(origin.X + std::max(1, asset.Building->Class->Width()) - 1);
            const short south = static_cast<short>(origin.Y + std::max(1, asset.Building->Class->Height()) - 1);
            return position.Relative_Distance_To(Cell(std::clamp(position.X, origin.X, east), std::clamp(position.Y, origin.Y, south)));
        };
        for (const TechnoClass* enemy : Technos) {
            if (!Hostile_To(house, enemy) || !enemy->IsLocked || !enemy->TClass ||
                (enemy->RTTI != RTTI_AIRCRAFT && enemy->RTTI != RTTI_INFANTRY && enemy->RTTI != RTTI_UNIT)) continue;
            const Cell position = enemy->Center_Coord().As_Cell();
            if (!Map.In_Radar(position) || std::none_of(assets.begin(), assets.end(), [&](const Asset& asset) {
                return distance_to_asset(position, asset) <= Preventive_Maximum_Distance * Preventive_Maximum_Distance;
            })) continue;
            if (std::none_of(observers.begin(), observers.end(), [&](const auto& observer) { return observer.Contains(position); })) continue;
            if (enemy->TClass->IsInvisible || (enemy->Cloak != UNCLOAKED && !Map[position].Sensed_By(house->HeapID))) continue;

            const bool air = enemy->In_Air();
            if (enemy->RTTI == RTTI_AIRCRAFT && !air) continue;
            const DefenseRole role = air ? DefenseRole::Air : enemy->RTTI == RTTI_INFANTRY ? DefenseRole::Infantry : DefenseRole::Armor;
            const int zone = !air && Map.CellZones && Map.Zones[MZONE_NORMAL] ? Map.Get_Cell_Zone(position, MZONE_NORMAL, false) : -1;
            const Asset* threatened = nullptr;
            std::tuple<int, int, int> best;
            int strength = 0;
            int range = 0;
            for (const auto& asset : assets) {
                const int distance = distance_to_asset(position, asset);
                if (distance > Preventive_Maximum_Distance * Preventive_Maximum_Distance) continue;
                const auto* building = asset.Building;
                int power = 0;
                int reach = 0;
                for (const auto* weapon : {enemy->PrimaryWeapon, enemy->SecondaryWeapon}) {
                    const int value = Weapon_Strength(weapon, building->Class->Armor, RTTI_BUILDING, false, false);
                    if (!value) continue;
                    power = std::max(power, value);
                    reach = std::max(reach, static_cast<int>(weapon->Range));
                }
                if (!reach) continue;
                const int warning = std::clamp(reach / CELL_LEPTON_W + 5, 6, Preventive_Maximum_Distance);
                if (distance > warning * warning) continue;
                if (!air && static_cast<long long>(distance) * CELL_LEPTON_W * CELL_LEPTON_W > static_cast<long long>(reach) * reach) {
                    if (zone <= 1 || zone == 0xFFFF) continue;
                    const Cell origin = building->Get_Cell();
                    const short east = static_cast<short>(origin.X + building->Class->Width());
                    const short south = static_cast<short>(origin.Y + building->Class->Height());
                    const short x = std::clamp<short>(position.X, origin.X, static_cast<short>(east - 1));
                    const short y = std::clamp<short>(position.Y, origin.Y, static_cast<short>(south - 1));
                    const std::array perimeter{Cell(x, static_cast<short>(origin.Y - 1)), Cell(x, south),
                        Cell(static_cast<short>(origin.X - 1), y), Cell(east, y)};
                    if (std::none_of(perimeter.begin(), perimeter.end(), [&](const Cell& cell) {
                        return Map.In_Radar(cell) && Map.Get_Cell_Zone(cell, MZONE_NORMAL, false) == zone;
                    })) continue;
                }
                const auto rank = std::make_tuple(distance - 4 * asset.Importance, -asset.Importance, building->ID);
                if (!threatened || rank < best) {
                    threatened = &asset;
                    best = rank;
                    strength = power;
                    range = reach;
                }
            }
            if (!threatened) continue;
            const auto old = previous.find(enemy->ID);
            const bool seen = recent && old != previous.end();
            const Cell asset = threatened->Position;
            const int distance = distance_to_asset(position, *threatened);
            const bool closing = seen && distance < distance_to_asset(old->second, *threatened);
            state.VisiblePositions.emplace(enemy->ID, position);
            const int x = position.X - asset.X;
            const int y = position.Y - asset.Y;
            const int side = std::abs(x) >= std::abs(y) ? (x >= 0 ? 0 : 1) : (y >= 0 ? 2 : 3);
            auto& group = groups.try_emplace({threatened->Building->ID, side},
                PreventiveGroup{asset, threatened->Building->Class->HeapID, threatened->Importance, {}}).first->second;
            group.Contacts.push_back({enemy->ID, position, enemy->TClass->Armor, enemy->RTTI, role,
                6 + strength / 8 + std::clamp(enemy->Strength / 80, 0, 10), closing,
                static_cast<long long>(distance) * CELL_LEPTON_W * CELL_LEPTON_W <= static_cast<long long>(range) * range, seen});
        }
        for (auto& entry : groups) state.Groups.push_back(std::move(entry.second));
    }

    struct DefenseWeapon
    {
        int Strength = 0;
        int Range = 0;
        int MinimumRange = 0;
        bool Direct = true;
    };

    DefenseWeapon Defense_Weapon_Against(const BuildingTypeClass* type, const PreventiveContact& enemy,
        long long distance = -1, bool clear_line = true)
    {
        DefenseWeapon best;
        for (const auto slot : {WEAPON_SLOT_PRIMARY, WEAPON_SLOT_SECONDARY}) {
            const auto* weapon = type->Fetch_Weapon_Info(slot).Weapon;
            const int power = Weapon_Strength(weapon, enemy.Armor, enemy.Category, enemy.Role == DefenseRole::Air, true);
            if (!power) continue;
            const int range = weapon->Range;
            const int minimum = std::max<int>(0, weapon->MinimumRange);
            const bool direct = !weapon->Bullet->IsHigh && !weapon->Bullet->IsArcing;
            if (!clear_line && direct) continue;
            if (distance >= 0 && (distance < static_cast<long long>(minimum) * minimum ||
                distance > static_cast<long long>(range) * range)) continue;
            if (std::make_tuple(power, range, -minimum) > std::make_tuple(best.Strength, best.Range, -best.MinimumRange)) {
                best = {power, range, minimum, direct};
            }
        }
        return best;
    }

    const PreventiveContact* Find_Visible_Attacker(const PreventiveDefenseState& state, int attacker, Cell& defended)
    {
        for (const auto& group : state.Groups) {
            for (const auto& contact : group.Contacts) {
                if (contact.ID != attacker) continue;
                if (contact.InRange) defended = group.Asset;
                return &contact;
            }
        }
        return nullptr;
    }

    bool Is_Defense_Response(std::string_view reason)
    {
        return reason == "preventive-base-defense" || reason == "air-raid-defense" ||
            reason == "infantry-raid-defense" || reason == "armor-raid-defense" || reason == "resume-threat-defense";
    }

    bool Owns_Defense_Target(const HouseClass* house, const Cell& cell)
    {
        if (!Map.In_Radar(cell)) return false;
        const auto* building = Map[cell].Cell_Building();
        return Usable_Owned_Building(building, house);
    }

    long long Threat_Defense_Reserve(long long funds, long long refinery_reserve, bool urgent)
    {
        return urgent ? std::min(refinery_reserve, std::max(0LL, funds / 3)) : refinery_reserve;
    }

    void Log_Preventive_Defense(const HouseClass* house, const char* decision, const char* reason,
        const PreventiveGroup* group = nullptr, const PreventiveContact* contact = nullptr, int strength = 0,
        int coverage = 0, int desired = 0, const BuildingTypeClass* chosen = nullptr, Cell site = Cell(0, 0),
        const BuildingTypeClass* order = nullptr)
    {
#ifdef NDEBUG
        if (!Vinifera_DeveloperMode) return;
#endif
        const int house_id = static_cast<int>(house->HeapID);
        const int count = group ? static_cast<int>(group->Contacts.size()) : 0;
        const Cell asset = group ? group->Asset : Cell(0, 0);
        const Cell enemy = contact ? contact->Position : Cell(0, 0);
        const int role = contact ? static_cast<int>(contact->Role) : -1;
        const auto state = std::make_tuple(std::string_view(reason), asset.As_Cell_Number(), role, count,
            chosen ? static_cast<int>(chosen->HeapID) : -1);
        const auto previous = PreventiveDefenseTraces.find(house_id);
        if (previous != PreventiveDefenseTraces.end() && previous->second == state && !chosen) return;
        PreventiveDefenseTraces.insert_or_assign(house_id, state);
        constexpr const char* roles[]{"air", "infantry", "armor"};
        const bool mixed = group && contact && std::any_of(group->Contacts.begin(), group->Contacts.end(), [&](const auto& entry) {
            return entry.Role != contact->Role;
        });
        DEV_DEBUG_INFO("AF-0009-DEFENSE-PREVENTIVE: house={} frame={} decision={} reason={} asset={} at={},{} "
            "enemy={},{} count={} threat={} response-role={} strength={} coverage={}/{} defense={} site={},{} order={}\n",
            house_id, Frame, decision, reason, group ? BuildingTypes[group->AssetType]->Name() : "none", asset.X, asset.Y,
            enemy.X, enemy.Y, count, mixed ? "mixed" : role >= 0 ? roles[role] : "none", role >= 0 ? roles[role] : "none",
            strength, coverage, desired, chosen ? chosen->Name() : "none", site.X, site.Y, order ? order->Name() : "none");
    }

    void Log_Power_Selection(const HouseClass* house, const BuildingTypeClass* type, const char* kind)
    {
#ifdef NDEBUG
        if (!Vinifera_DeveloperMode) return;
#endif
        const int house_id = static_cast<int>(house->HeapID);
        if (!AdvancedAIPowerSelections.emplace(house_id, static_cast<int>(type->HeapID), kind).second) return;
        DEV_DEBUG_INFO("AF-0009: house={} power={} building={}\n", house_id, kind, type->Name());
    }

    bool Sufficient_Power(const HouseClass* house, long long gain)
    {
        return gain >= 0 || static_cast<long long>(house->Power) - house->Drain - house->PowerSurplus >= -gain;
    }

    bool Is_Unproductive_Power_Turbine(const HouseClass* house, const BuildingTypeClass* type, long long gain)
    {
        if (type->PowersUpBuilding.empty() || type->Power <= type->Drain || gain > 0) return false;
        const int side = static_cast<int>(house->Class->Side);
        return side >= 0 && side < Sides.Count() && Sides[side] &&
            type == Extension::Fetch(Sides[side])->PowerTurbine;
    }

    void Trace_Power_Choice(const HouseClass* house, const BuildingTypeClass* desired,
        const BuildingTypeClass* plant, bool deficit, long long additional, long long gain)
    {
#ifdef NDEBUG
        if (!Vinifera_DeveloperMode) return;
#endif
        if (PowerTraceMute > 0) return;
        const auto key = std::make_pair(static_cast<int>(house->HeapID), static_cast<int>(desired->HeapID));
        const int state = !deficit ? -2 : plant ? static_cast<int>(plant->HeapID) : -1;
        const auto previous = AdvancedAIPowerDiagnostics.find(key);
        if (previous != AdvancedAIPowerDiagnostics.end() && previous->second == state) return;
        AdvancedAIPowerDiagnostics.insert_or_assign(key, state);
        if (!deficit) return;
        DEV_DEBUG_INFO("AF-0009: Power deficit for {} (house={} available={} additional={} PowerSurplus={})\n",
            desired->Name(), static_cast<int>(house->HeapID),
            static_cast<long long>(house->Power) - house->Drain - house->PowerSurplus,
            additional, house->PowerSurplus);
        if (plant) {
            DEV_DEBUG_INFO("AF-0009: Prioritizing power plant {} (house={} for={} net={})\n",
                plant->Name(), static_cast<int>(house->HeapID), desired->Name(),
                gain);
        } else {
            DEV_DEBUG_INFO("AF-0009: No buildable power plant for {} (house={}); checking alternatives or waiting\n",
                desired->Name(), static_cast<int>(house->HeapID));
        }
    }
}

Cell AdvancedAIDefenseFocus::Intercept() const
{
    if (!HasContact) return Asset;
    if (RequireContactRange) return Enemy;
    const int x = Enemy.X - Asset.X;
    const int y = Enemy.Y - Asset.Y;
    const int distance = std::max(std::abs(x), std::abs(y));
    if (distance <= 4) return Enemy;
    return Cell(static_cast<short>(Asset.X + 4 * x / distance), static_cast<short>(Asset.Y + 4 * y / distance));
}

void HouseClassExtension::Clear_Advanced_AI_Log_State()
{
    AdvancedAIErrors.clear();
    AdvancedAIPowerSelections.clear();
    AdvancedAIPowerDiagnostics.clear();
    ProductionTraces.clear();
    DefenseResponseTraces.clear();
    PreventiveDefenseTraces.clear();
}

void HouseClassExtension::Clear_Advanced_AI_Building_Order_State()
{
    BuildingStarts.clear();
    BuildingLosses.clear();
    DefenseLosses.clear();
    BuildingAttacks.clear();
    PreventiveDefenses.clear();
    SavingsGoals.clear();
    SavingsTraces.clear();
    PreventiveDefenseTraces.clear();
    ThreatDefensePlacementFailures.clear();
    FailedBuildingSites.clear();
    BuildingOrderTraces.clear();
    TechnologyTraces.clear();
    UnitStartTraces.clear();
}

void HouseClassExtension::Record_Advanced_AI_Building_Hit(int attacker_id, const Cell& target, int damage, bool base_hit)
{
    auto& history = BuildingAttacks[static_cast<int>(This_Const()->HeapID)];
    auto& hit = history.try_emplace(attacker_id, BuildingAttack{target, Frame, 0}).first->second;
    if (Attack_Expired(hit)) hit = {target, Frame, 0};
    hit.Target = target;
    hit.LastHit = Frame;
    if (base_hit) hit.LastBaseHit = Frame;
    hit.Damage = static_cast<int>(std::min(1000000LL, static_cast<long long>(hit.Damage) + damage));
}

Cell HouseClassExtension::Advanced_AI_Defense_Focus(const BuildingTypeClass* type) const
{
    const auto threats = Active_Building_Threats(This_Const());
    std::pair<long long, int> highest{};
    Cell target(0, 0);
    for (const auto role : DefenseRoles) {
        const auto& threat = threats[static_cast<size_t>(role)];
        const auto rank = std::make_pair(threat.Damage, threat.Attackers);
        if (rank > highest && Defense_Range(type, role) > 0) {
            highest = rank;
            target = threat.Target;
        }
    }
    return target;
}

/**
 *  AF-0009U: The hostile unit that dealt the most building damage to this house
 *  recently and is still active near its target. Used to send units against it.
 */
TechnoClass* HouseClassExtension::Advanced_AI_Primary_Base_Attacker(long long& damage) const
{
    damage = 0;
    const HouseClass* house = This_Const();
    const auto history = BuildingAttacks.find(static_cast<int>(house->HeapID));
    if (history == BuildingAttacks.end() || history->second.empty()) return nullptr;

    TechnoClass* best = nullptr;
    for (TechnoClass* techno : Technos) {
        if (techno->RTTI == RTTI_BUILDING || !Hostile_To(house, techno)) continue;
        const auto found = history->second.find(techno->ID);
        if (found == history->second.end()) continue;
        const auto& hit = found->second;
        if (Attack_Expired(hit) || Frame - hit.LastHit > Active_Defense_Hit_Window) continue;
        if (techno->Get_Cell().Relative_Distance_To(hit.Target) > 20 * 20) continue;
        if (!best || hit.Damage > damage) {
            best = techno;
            damage = hit.Damage;
        }
    }
    return best;
}


/**
 *  Credits that must remain after starting the given defense. Threat responses
 *  use the same urgency rule as the planner, so an emergency defense is not
 *  selected and then rejected again at the factory.
 */
long long HouseClassExtension::Advanced_AI_Defense_Start_Reserve(const BuildingTypeClass* type) const
{
    const int id = static_cast<int>(This_Const()->HeapID);
    bool urgent = false;

    const auto start = BuildingStarts.find(id);
    if (start != BuildingStarts.end() && start->second.ThreatDefenseOrder && start->second.SelectedType == type->HeapID) {
        urgent = start->second.EmergencyDefenseOrder;
        const auto defense = PreventiveDefenses.find(id);
        if (defense != PreventiveDefenses.end() && defense->second.Planned == type) {
            urgent |= defense->second.Focus.Urgent;
        }
    }

    return Threat_Defense_Reserve(Advanced_AI_Uncommitted_Funds(), Advanced_AI_Refinery_Reserve(), urgent);
}


bool HouseClassExtension::Advanced_AI_Ordered_Defense_Focus(const BuildingTypeClass* type,
    AdvancedAIDefenseFocus& focus) const
{
    const auto* house = This_Const();
    const int id = static_cast<int>(house->HeapID);
    const auto start = BuildingStarts.find(id);
    const auto defense = PreventiveDefenses.find(id);
    if (start == BuildingStarts.end() || !start->second.ThreatDefenseOrder ||
        start->second.SelectedType != type->HeapID || defense == PreventiveDefenses.end() || defense->second.Planned != type) return false;
    auto& state = defense->second;
    focus = state.Focus;

    Scan_Preventive_Defense(house, state, true);
    const PreventiveGroup* chosen_group = nullptr;
    const PreventiveContact* chosen_contact = nullptr;
    DefenseWeapon chosen_weapon;
    std::tuple<bool, bool, bool, int, int, int, int> best{};
    for (const auto& group : state.Groups) {
        for (const auto& contact : group.Contacts) {
            if (contact.ID != focus.ContactID && group.Asset.Relative_Distance_To(focus.Asset) > 20 * 20) continue;
            if (contact.ID != focus.ContactID && !contact.InRange && group.Contacts.size() < 2) continue;
            if ((contact.Role == DefenseRole::Air) != focus.Air) continue;
            const auto weapon = Defense_Weapon_Against(type, contact);
            if (!weapon.Range) continue;
            const auto rank = std::make_tuple(contact.InRange, contact.Role == static_cast<DefenseRole>(focus.Role),
                contact.ID == focus.ContactID, static_cast<int>(group.Contacts.size()), contact.Strength,
                -contact.Position.Relative_Distance_To(group.Asset), -contact.ID);
            if (!chosen_contact || rank > best) {
                chosen_group = &group;
                chosen_contact = &contact;
                chosen_weapon = weapon;
                best = rank;
            }
        }
    }
    if (chosen_contact) {
        focus = {chosen_group->Asset, chosen_contact->Position, chosen_weapon.Range, chosen_weapon.MinimumRange,
            focus.Air, chosen_weapon.Direct, true, start->second.EmergencyDefenseOrder || chosen_contact->InRange,
            chosen_contact->ID, static_cast<int>(chosen_contact->Role), focus.Urgent};
        state.Focus = focus;
    } else {
        const auto threats = Active_Building_Threats(house);
        const DefenseThreat* latest = nullptr;
        int range = 0;
        for (const auto role : DefenseRoles) {
            const auto& threat = threats[static_cast<size_t>(role)];
            const int reach = Defense_Range(type, role);
            if (static_cast<int>(role) != focus.Role || !reach || !threat.Attackers ||
                threat.Target.Relative_Distance_To(focus.Asset) > 20 * 20 || !Owns_Defense_Target(house, threat.Target)) continue;
            if (!latest || std::make_pair(threat.LastHit, threat.Damage) > std::make_pair(latest->LastHit, latest->Damage)) {
                latest = &threat;
                range = reach;
            }
        }
        if (latest) {
            focus = {latest->Target, latest->Target, range, 0, focus.Air, false, false, false,
                latest->AttackerID, focus.Role, true};
            state.Focus = focus;
        } else focus.Range = 0;
    }
    return true;
}

void HouseClassExtension::Retry_Advanced_AI_Defense_Placement(const BuildingTypeClass* type) const
{
    ThreatDefensePlacementFailures.insert_or_assign({static_cast<int>(This_Const()->HeapID), type->HeapID}, Frame);
}

void HouseClassExtension::Trace_Advanced_AI_Building_Order(const char* phase, const char* reason,
    const BuildingTypeClass* type, FactoryClass* factory, const Cell* target, const char* source) const
{
#ifdef NDEBUG
    if (!Vinifera_DeveloperMode) return;
#endif
    if (!Is_Advanced_AI_Enabled() || IsObserver) return;
    HouseClass* house = This();
    if (!factory) factory = Find_Building_Factory(house);
    const TechnoClass* product = factory ? factory->Get_Object() : nullptr;
    const auto* product_type = product && product->RTTI == RTTI_BUILDING
        ? static_cast<const BuildingClass*>(product)->Class : nullptr;
    if (!type) type = product_type ? product_type : Building_Order_Type(house->BuildStructure);
    const StructType type_id = type ? type->HeapID : STRUCT_NONE;
    const int id = static_cast<int>(house->HeapID);
    const auto selected = BuildingOrderTraces.find({id, "selected", STRUCT_NONE});
    const std::string_view origin = source ? source :
        selected != BuildingOrderTraces.end() && selected->second.Type == type_id ? selected->second.Source : "engine";
    const char* production = !product_type ? "none" : factory->Has_Completed() ? "awaiting-placement" :
        factory->Is_Building() ? "running" : "allocated-or-stopped";
    const Cell goal = target ? *target : Advanced_AI_Expansion_Target();
    const std::string_view lane = phase;
    const auto key = std::make_tuple(id, lane, lane == "candidate" ? type_id : STRUCT_NONE);
    const BuildingOrderTrace state{reason, origin, production, type_id, house->BuildStructure,
        product_type ? product->ID : -1, goal};
    const auto previous = BuildingOrderTraces.find(key);
    if (previous != BuildingOrderTraces.end() && previous->second == state) return;
    BuildingOrderTraces.insert_or_assign(key, state);
    DEV_DEBUG_INFO("AF-0009C3: house={} frame={} phase={} role={} type={} reason={} source={} "
        "build-structure={} product={} product-id={} production={} target={},{} funds={} uncommitted={} "
        "power-surplus={} yards={} refineries={} harvesters={} barracks={} weapons={} helipads={}\n",
        id, Frame, phase, Building_Order_Role(type, origin), type ? type->Name() : "none", reason, origin,
        static_cast<int>(house->BuildStructure), product_type ? product_type->Name() : "none",
        state.ProductID, production, goal.X, goal.Y, house->Available_Money(), Advanced_AI_Uncommitted_Funds(),
        static_cast<long long>(house->Power) - house->Drain - house->PowerSurplus, house->ConstructionYards.Count(),
        house->Count_Owned(Rule->BuildRefinery), house->Count_Owned(Rule->HarvesterUnit),
        house->Count_Owned(Rule->BuildBarracks), house->Count_Owned(Rule->BuildWeapons), house->Count_Owned(Rule->BuildHelipad));
}

void HouseClassExtension::Trace_Advanced_AI_Production(const char* phase, const char* reason,
    const BuildingTypeClass* requested, long long minimum_funds, FactoryClass* observed) const
{
#ifdef NDEBUG
    if (!Vinifera_DeveloperMode) return;
#endif
    if (!Is_Advanced_AI_Enabled() || IsObserver) return;
    HouseClass* house = This();
    FactoryClass* factory = observed ? observed : Find_Building_Factory(house);

    const TechnoClass* product = factory ? factory->Get_Object() : nullptr;
    const bool building_product = product && product->RTTI == RTTI_BUILDING;
    const auto* product_type = building_product ? static_cast<const BuildingClass*>(product)->Class : nullptr;
    const long long funds = house->Available_Money();
    int installment = 0;
    std::string_view production = "none";
    if (building_product) {
        if (factory->Has_Completed()) production = "awaiting-placement";
        else if (!factory->Is_Building()) production = "stopped";
        else {
            installment = std::min(factory->Balance, factory->Cost_Per_Tick());
            production = funds < installment ? "below-current-installment" : "running";
        }
    }

    const int order = house->BuildStructure;
    const auto* queued = order >= STRUCT_FIRST && order < BuildingTypes.Count() ? BuildingTypes[order] : nullptr;
    const auto* desired = Valid_Building_Type(requested) ? requested : (queued ? queued : product_type);
    const int type = desired ? static_cast<int>(desired->HeapID) : STRUCT_NONE;
    const int refineries = house->Count_Owned(Rule->BuildRefinery);
    const int harvesters = house->Count_Owned(Rule->HarvesterUnit);
    const long long surplus = static_cast<long long>(house->Power) - house->Drain;
    const int id = static_cast<int>(house->HeapID);
    const int product_id = building_product ? product->ID : -1;
    const std::string_view lane = phase;
    // Callers pass static phase/reason strings. Amounts and stage are sampled
    // on transitions rather than using every credit or production tick as a key.
    const ProductionTraceKey key{id, lane, lane == "expansion-budget" ? type : -1};
    const ProductionTraceState state{reason, type, order, product_id, production, refineries, harvesters,
        house->BuildUnit, static_cast<int>(house->State), house->IsTiberiumShort, minimum_funds, funds >= minimum_funds,
        surplus >= house->PowerSurplus};
    const auto previous = ProductionTraces.find(key);
    if (previous != ProductionTraces.end() && previous->second == state) return;
    ProductionTraces.insert_or_assign(key, state);

    const int cost = desired ? desired->Cost_Of(house) : 0;
    // Upgrades use a placement-dependent delta already reported by the power planner.
    const long long demand = desired && desired->PowersUpBuilding.empty()
        ? std::max(0LL, static_cast<long long>(desired->Drain) - desired->Power) : -1;
    DEV_DEBUG_INFO("AF-0009B2b1b: house={} frame={} phase={} reason={} desired={} funds={} cost={} minimum={} reserve={} "
        "power={} drain={} surplus={} power-reserve={} demand={} refineries={} harvesters={} harvested={} tiberium-short={} "
        "house-state={} unit-order={} build-structure={} queued={} product={} product-id={} production={} stage={} balance={} installment={}\n",
        id, Frame, phase, reason, desired ? desired->Name() : "none", funds, cost, minimum_funds,
        std::max(0LL, minimum_funds - cost), house->Power, house->Drain, surplus, house->PowerSurplus, demand,
        refineries, harvesters, house->HarvestedCredits, house->IsTiberiumShort, static_cast<int>(house->State),
        static_cast<int>(house->BuildUnit), order,
        queued ? queued->Name() : "none", product_type ? product_type->Name() : "none", product_id, production,
        building_product ? factory->Fetch_Stage() : -1, building_product ? factory->Balance : 0, installment);
}

void HouseClassExtension::Log_Advanced_AI_Error(const char* reason, const BuildingTypeClass* type, int detail) const
{
#ifdef NDEBUG
    if (!Vinifera_DeveloperMode) return;
#endif
    const int house_id = static_cast<int>(This_Const()->HeapID);
    const int type_id = Valid_Building_Type(type) ? static_cast<int>(type->HeapID) : -1;
    if (!AdvancedAIErrors.emplace(house_id, type_id, reason, detail).second) return;
    DEV_DEBUG_WARNING("AF-0009: house={} error={} building={} detail={}\n",
        house_id, reason, type_id >= 0 ? type->Name() : "none", detail);
}

bool HouseClassExtension::Is_Advanced_AI_Enabled() const
{
    if (!RuleExtension || !RuleExtension->IsUseAdvancedAI) return false;
    const HouseClass* house = This_Const();
    return house && !house->Is_Human_Player() && !house->Is_Player_Control() &&
        !house->IsHuman && !house->IsPlayerControl;
}

AdvancedAIBuildingStart HouseClassExtension::Check_Advanced_AI_Building_Start(
    const BuildingTypeClass* type, const BuildingClass* yard) const
{
    using Status = AdvancedAIBuildingStart;
    HouseClass* house = This();
    if (Has_Advanced_AI_Building_Production()) return Status::ProductOwnsSlot;
    if (IsObserver || !Can_Advanced_AI_Build_Type(type, false)) return Status::NotBuildable;
    const auto start = BuildingStarts.find(static_cast<int>(house->HeapID));
    if (start == BuildingStarts.end() || start->second.SelectedType != type->HeapID) return Status::UnmanagedOrder;
    if (start->second.RejectedType == type->HeapID) return Status::AwaitingPlanner;
    if (!Has_Advanced_AI_Ready_Yard(type, yard)) return Status::NoReadyYard;
    if (!Has_Advanced_AI_Refineries_For(type)) return Status::NotBuildable;

    AdvancedAIDefenseFocus focus;
    if (Advanced_AI_Ordered_Defense_Focus(type, focus)) {
        const int cost = type->Cost_Of(house);
        const long long funds = Advanced_AI_Uncommitted_Funds();
        if (cost < 0 || funds < cost + Threat_Defense_Reserve(funds, Advanced_AI_Refinery_Reserve(),
            start->second.EmergencyDefenseOrder || focus.Urgent)) {
            Trace_Advanced_AI_Building_Order("factory-start", "insufficient-threat-defense-funds", type, nullptr, &focus.Asset);
            return Status::NotBuildable;
        }
        if (!focus.Range || Find_Advanced_AI_Defense_Site(BuildingTypes[type->HeapID], &focus.Asset, focus.Range, &focus) == Cell(0, 0)) {
            Retry_Advanced_AI_Defense_Placement(type);
            Trace_Advanced_AI_Building_Order("factory-start", "no-legal-defense-site", type, nullptr, &focus.Asset);
            return Status::NotBuildable;
        }
    }

    long long gain;
    if (!Get_Advanced_AI_Power_Gain(type, gain)) return Status::NotBuildable;
    if (Is_Unproductive_Power_Turbine(house, type, gain) || !Sufficient_Power(house, gain)) return Status::NeedsPower;
    Trace_Power_Choice(house, type, nullptr, false, -gain, 0);
    return house->Available_Money() > 10 ? Status::Ready : Status::InsufficientFunds;
}

const char* HouseClassExtension::Advanced_AI_Building_Start_Reason(AdvancedAIBuildingStart status)
{
    switch (status) {
    case AdvancedAIBuildingStart::Ready: return "startable";
    case AdvancedAIBuildingStart::NeedsPower: return "needs-power";
    case AdvancedAIBuildingStart::InsufficientFunds: return "below-start-minimum";
    case AdvancedAIBuildingStart::NoReadyYard: return "no-ready-yard-for-order";
    case AdvancedAIBuildingStart::NotBuildable: return "order-not-buildable";
    case AdvancedAIBuildingStart::ProductOwnsSlot: return "building-product-owns-slot";
    case AdvancedAIBuildingStart::AwaitingPlanner: return "awaiting-planner";
    case AdvancedAIBuildingStart::UnmanagedOrder: return "order-without-current-plan";
    }
    return "unknown";
}

bool HouseClassExtension::Begin_Advanced_AI_Building_Attempt()
{
    auto& start = BuildingStarts[static_cast<int>(This_Const()->HeapID)];
    if (start.AttemptFrame == Frame) return false;
    start.AttemptFrame = Frame;
    return true;
}

void HouseClassExtension::Reject_Advanced_AI_Building_Start(const BuildingTypeClass* type)
{
    if (This_Const()->BuildStructure == type->HeapID && !Has_Advanced_AI_Building_Production()) {
        BuildingStarts[static_cast<int>(This_Const()->HeapID)].RejectedType = type->HeapID;
    }
}

void HouseClassExtension::Defer_Advanced_AI_Building_Type(const BuildingTypeClass* type)
{
    const auto key = std::make_pair(static_cast<int>(This_Const()->HeapID), type->HeapID);
    const auto failed = ThreatDefensePlacementFailures.find(key);
    if (failed != ThreatDefensePlacementFailures.end() && failed->second == Frame) return;
    FailedBuildingSites.insert_or_assign(key, Frame);

    // AF-0009U: Drop a replacement that keeps failing; a later loss adds it again.
    auto& state = BuildingLosses[key.first];
    if (!state.ReplaceTo.count(type->HeapID)) return;
    const int failures = ++state.PlacementFailures[type->HeapID];
    if (failures < ReplacementPlacementLimit) return;
    state.ReplaceTo.erase(type->HeapID);
    state.PlacementFailures.erase(type->HeapID);
    DEV_DEBUG_INFO("AF-0009U: house={} frame={} replacement-dropped type={} placement-failures={}\n",
        key.first, Frame, type->Name(), failures);
}

bool HouseClassExtension::Is_Advanced_AI_Building_Type_Deferred(const BuildingTypeClass* type) const
{
    const auto found = FailedBuildingSites.find({static_cast<int>(This_Const()->HeapID), type->HeapID});
    return found != FailedBuildingSites.end() && Frame >= found->second &&
        Frame - found->second < Failed_Site_Retry_Interval;
}

void HouseClassExtension::Release_Advanced_AI_Building_Order(const char* reason)
{
    HouseClass* house = This();
    if (house->BuildStructure == STRUCT_NONE) return;
    const auto* type = Building_Order_Type(house->BuildStructure);
    house->BuildStructure = STRUCT_NONE;
    if (const auto start = BuildingStarts.find(static_cast<int>(house->HeapID)); start != BuildingStarts.end()) {
        start->second.SelectedType = STRUCT_NONE;
        start->second.ThreatDefenseOrder = false;
        start->second.EmergencyDefenseOrder = false;
    }
    if (const auto defense = PreventiveDefenses.find(static_cast<int>(house->HeapID));
        defense != PreventiveDefenses.end() && defense->second.Planned == type) defense->second.Planned = nullptr;
    Trace_Advanced_AI_Building_Order("discarded", reason, type);
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Replacement(bool slot_free, AdvancedAIPowerGoal& goal)
{
    HouseClass* house = This();
    auto& state = BuildingLosses[static_cast<int>(house->HeapID)];
    std::map<int, StructType> owned;
    std::map<StructType, int> counts;
    std::set<int> selling;
    for (const BuildingClass* building : Buildings) {
        if (building && building->House == house && building->Strength > 0 &&
            (building->Mission == MISSION_DECONSTRUCTION || building->MissionQueue == MISSION_DECONSTRUCTION)) {
            selling.insert(building->ID);
        }
        if (!Usable_Owned_Building(building, house) || !Valid_Building_Type(building->Class)) continue;
        const auto* type = building->Class;
        if (type->IsWall || type->IsGate || type->IsInvisibleInGame) continue;
        // AF-0009U: Deployed vehicles (artillery, tick tanks) are units, not losses to rebuild.
        if (type->UndeploysInto != nullptr && !type->IsConstructionYard) continue;
        owned.emplace(building->ID, type->HeapID);
        ++counts[type->HeapID];
    }

    std::map<StructType, int> previous_counts;
    for (const auto& [id, type] : state.Owned) {
        if (!selling.count(id)) ++previous_counts[type];
    }
    for (const auto& [id, type] : state.Owned) {
        if (!selling.count(id) && !owned.count(id) && counts[type] < previous_counts[type]) {
            auto& goal = state.ReplaceTo[type];
            goal = std::max(goal, previous_counts[type]);
            const auto* lost = Building_Order_Type(type);
            if (lost && Is_Advanced_AI_Defense_Type(lost)) {
                auto& frames = DefenseLosses[{static_cast<int>(house->HeapID), static_cast<int>(type)}];
                std::erase_if(frames, [](long frame) { return Frame < frame || Frame - frame > DefenseLossWindow; });
                frames.push_back(Frame);
            }
        }
    }
    state.Owned.swap(owned);
    std::erase_if(state.ReplaceTo, [&](const auto& entry) {
        const auto* type = Building_Order_Type(entry.first);
        return counts[entry.first] >= entry.second || (type && type->UndeploysInto != nullptr && !type->IsConstructionYard);
    });
    // A finished or dropped replacement starts a later loss with a fresh count.
    std::erase_if(state.PlacementFailures, [&](const auto& entry) { return !state.ReplaceTo.count(entry.first); });
    if (!slot_free || state.ReplaceTo.empty()) return nullptr;

    const long long funds = Advanced_AI_Uncommitted_Funds();
    const long long reserve = Advanced_AI_Refinery_Reserve();
    std::vector<const BuildingTypeClass*> candidates;
    for (const auto& entry : state.ReplaceTo) {
        if (const auto* type = Building_Order_Type(entry.first)) candidates.push_back(type);
    }
    const auto rank = [&](const BuildingTypeClass* type) {
        int priority = 5;
        if (Matches_Role(type, InfrastructureRole::Power)) priority = 0;
        else if (type->IsRefinery) priority = 1;
        else if (type->IsConstructionYard || type->ToBuild != RTTI_NONE) priority = 2;
        else if (Is_Advanced_AI_Defense_Type(type)) priority = 3;
        else if (type->IsRadar || Rule->BuildTech.Is_Present(BuildingTypes[type->HeapID])) priority = 4;
        return std::make_tuple(priority, type->HeapID);
    };
    std::sort(candidates.begin(), candidates.end(), [&](const auto* a, const auto* b) { return rank(a) < rank(b); });

    /**
     *  AF-0009U: While the base is still being hit, a rebuilt building is usually
     *  destroyed again by the same attacker. Defer replacements (except a
     *  construction yard) until the base was not hit for a short while, so the
     *  credits go to the threat defense instead. A power deficit is still
     *  handled separately by the "restore-power" path.
     */
    static constexpr int ReplacementQuietFrames = 10 * TICKS_PER_SECOND;
    bool under_fire = false;
    if (const auto attacks = BuildingAttacks.find(static_cast<int>(house->HeapID)); attacks != BuildingAttacks.end()) {
        for (const auto& [attacker, hit] : attacks->second) {
            if (hit.LastBaseHit >= 0 && Frame >= hit.LastBaseHit && Frame - hit.LastBaseHit <= ReplacementQuietFrames) {
                under_fire = true;
                break;
            }
        }
    }

    bool traced_deferral = false;
    bool traced_power_surplus = false;
    for (const auto* type : candidates) {
        if (under_fire && !type->IsConstructionYard) {
            // One trace per pass: alternating types would defeat the trace deduplication.
            if (!traced_deferral) Trace_Advanced_AI_Building_Order("replacement", "deferred-under-fire", type);
            traced_deferral = true;
            continue;
        }
        if (!Has_Advanced_AI_Refineries_For(type) || Is_Advanced_AI_Building_Type_Deferred(type) ||
            !Can_Advanced_AI_Build_Building(type, nullptr, false)) continue;
        // AF-0009U: A defense lost repeatedly is not rebuilt 1:1; the threat defense picks a stronger type.
        if (Is_Advanced_AI_Defense_Type(type) &&
            Recent_Defense_Losses(static_cast<int>(house->HeapID), type->HeapID) >= DefenseLossLimit) continue;
        /**
         *  AF-0009U: A lost power plant is only rebuilt while the remaining
         *  surplus is smaller than its own output. With enough power left the
         *  credits go elsewhere; a later deficit is covered by "restore-power"
         *  or by this goal, which stays open.
         */
        if (Matches_Role(type, InfrastructureRole::Power)) {
            const long long available = static_cast<long long>(house->Power) - house->Drain - house->PowerSurplus;
            const long long output = static_cast<long long>(type->Power) - type->Drain;
            if (available >= output) {
                // One trace per pass, like the deferral above.
                if (!traced_power_surplus) Trace_Advanced_AI_Building_Order("replacement", "power-surplus-sufficient", type);
                traced_power_surplus = true;
                continue;
            }
        }
        const auto* order = Prioritize_Advanced_AI_Power(type);
        if (!order) continue;
        const int cost = type->Cost_Of(house);
        const int power_cost = order == type ? 0 : order->Cost_Of(house);
        const auto protected_funds = std::get<0>(rank(type)) <= 1 ? 0 : reserve;
        if (cost >= 0 && power_cost >= 0 && funds >= protected_funds + cost + power_cost) {
            if (order != type) goal.Type = type;
            return order;
        }
    }
    return nullptr;
}

void HouseClassExtension::Report_Advanced_AI_Power_Start(const BuildingTypeClass* type, const FactoryClass* factory)
{
    const auto found = BuildingStarts.find(static_cast<int>(This_Const()->HeapID));
    if (found == BuildingStarts.end() || !found->second.Goal.Type) return;
    auto& start = found->second;
    const auto* product = factory ? factory->Get_Object() : nullptr;
    if (!product || product->RTTI != RTTI_BUILDING) return;
    if (type == start.Goal.Type) start.Goal = {};
    else if (!start.SupplyCompleted && start.Supply == type->HeapID) start.SupplyProduct = product->ID;
}

void HouseClassExtension::Report_Advanced_AI_Power_Completed(const BuildingTypeClass* type, int product_id)
{
    const int id = static_cast<int>(This_Const()->HeapID);
    const auto found = BuildingStarts.find(id);
    if (found == BuildingStarts.end()) return;
    auto& start = found->second;
    if (start.ThreatDefenseOrder && type && start.SelectedType == type->HeapID) {
        if (const auto defense = PreventiveDefenses.find(id); defense != PreventiveDefenses.end()) defense->second.Planned = nullptr;
        start.ThreatDefenseOrder = false;
        start.EmergencyDefenseOrder = false;
    }
    if (!start.Goal.Type || !type || product_id < 0 || start.Supply != type->HeapID || start.SupplyProduct != product_id) return;
    start.SupplyCompleted = true;
}

AdvancedAIBuildingPlan HouseClassExtension::Resume_Advanced_AI_Power_Goal()
{
    using Action = AdvancedAIBuildingPlan::Action;
    HouseClass* house = This();
    const auto found = BuildingStarts.find(static_cast<int>(house->HeapID));
    if (found == BuildingStarts.end() || !found->second.Goal.Type) return {Action::Wait, "no-power-goal"};
    auto& start = found->second;
    const auto* desired = start.Goal.Type;
    const auto discard = [&](const char* reason) -> AdvancedAIBuildingPlan {
        Trace_Advanced_AI_Building_Order("power-goal-released", reason, desired);
        if (start.Goal.PreventiveDefense || start.Goal.EmergencyDefense) {
            if (const auto defense = PreventiveDefenses.find(static_cast<int>(house->HeapID)); defense != PreventiveDefenses.end()) {
                defense->second.Planned = nullptr;
            }
        }
        start.Goal = {};
        return {Action::Wait, reason};
    };
    if (!start.SupplyCompleted) return discard("power-order-did-not-complete");
    if (Frame < start.GoalFrame || Frame - start.GoalFrame >= 3 * TICKS_PER_MINUTE) return discard("power-goal-expired");
    if (Placed_Building_Count(house, desired) >= start.GoalCount) return discard("power-goal-already-satisfied");
    if (start.Goal.ResourceExpansion) {
        AdvancedAIPowerGoal next;
        if (const auto* order = Resume_Advanced_AI_Refinery(desired, next)) {
            return {Action::Order, next.Type ? "power-before-retained-goal" : "resume-after-power", order, "B2", next};
        }
        return discard("power-goal-refinery-no-longer-viable");
    }
    if (start.Goal.PreventiveDefense) {
        AdvancedAIPowerGoal next;
        if (const auto* order = Select_Advanced_AI_Preventive_Defense(next, desired)) {
            return {Action::Order, "preventive-base-defense", order, "B1", next};
        }
        return discard("power-goal-preventive-defense-no-longer-needed");
    }
    if (start.Goal.EmergencyDefense) {
        const char* reason = nullptr;
        long long minimum = 0;
        AdvancedAIPowerGoal next;
        if (const auto* order = Select_Advanced_AI_Defense(reason, minimum, next, true, true, nullptr, false, desired)) {
            return {Action::Order, "resume-threat-defense", order, "B1", next};
        }
        return discard("power-goal-threat-ended");
    }
    if (Is_Advanced_AI_Building_Type_Deferred(desired) || !Can_Advanced_AI_Build_Building(desired, nullptr, false) ||
        !Has_Advanced_AI_Refineries_For(desired)) return discard("power-goal-no-longer-buildable");
    if (start.Goal.Technology && !Has_Advanced_AI_Technology_Economy()) return discard("power-goal-tech-economy");
    const auto* order = Prioritize_Advanced_AI_Power(desired);
    if (!order) return discard("power-goal-no-buildable-power");
    const long long cost = desired->Cost_Of(house);
    const long long power_cost = order == desired ? 0 : order->Cost_Of(house);
    const long long reserve = start.GoalOptional || Is_Advanced_AI_Defense_Type(desired) ? Advanced_AI_Refinery_Reserve() : 0;
    if (cost < 0 || power_cost < 0 || Advanced_AI_Uncommitted_Funds() < reserve + cost + power_cost) {
        return discard("power-goal-budget");
    }
    const char* source = start.GoalOptional ? "B2" : "B1";
    if (start.GoalOptional) Set_Advanced_AI_Development_Order(order, reserve + cost + power_cost, start.Goal.Technology);
    if (order != desired) return {Action::Order, "power-before-retained-goal", order, source, start.Goal};
    Plan_Advanced_AI_Expansion(desired);
    return {Action::Order, "resume-after-power", desired, source};
}

AdvancedAIBuildingPlan HouseClassExtension::Plan_Advanced_AI_Building()
{
    using Action = AdvancedAIBuildingPlan::Action;
    using Status = AdvancedAIBuildingStart;
    HouseClass* house = This();
    Update_Advanced_AI_Resource_Analysis();
    if (IsObserver) return {Action::Wait, "observer-house"};
    if (const char* release = Update_Advanced_AI_Expansion()) Release_Advanced_AI_Building_Order(release);
    if (!house->IsStarted) return {Action::Wait, "house-not-started"};
    if (house->ConstructionYards.Count() > 0) {
        Scan_Preventive_Defense(house, PreventiveDefenses[static_cast<int>(house->HeapID)]);
    }
    FactoryClass* factory = Find_Building_Factory(house);
    AdvancedAIPowerGoal replacement_goal;
    const auto* replacement = Select_Advanced_AI_Replacement(!factory, replacement_goal);
    const auto start = BuildingStarts.find(static_cast<int>(house->HeapID));
    if (!factory && start != BuildingStarts.end() && start->second.Goal.Type && !start->second.SupplyCompleted &&
        (house->BuildStructure != start->second.Supply || start->second.RejectedType == start->second.Supply)) {
        Trace_Advanced_AI_Building_Order("power-goal-released", "power-order-did-not-complete", start->second.Goal.Type);
        start->second.Goal = {};
    }
    if (!factory && house->BuildStructure == STRUCT_NONE && start != BuildingStarts.end() && start->second.Goal.Type &&
        start->second.SupplyCompleted && (start->second.Goal.EmergencyDefense || start->second.Goal.PreventiveDefense)) {
        const auto resumed = Resume_Advanced_AI_Power_Goal();
        if (resumed.Result == Action::Order) return resumed;
    }
    const auto* queued = Building_Order_Type(house->BuildStructure);
    const auto* current = factory ? static_cast<const BuildingClass*>(factory->Get_Object())->Class : queued;
    long long current_power = 0;
    const bool interruptible = current && start != BuildingStarts.end() && start->second.Optional &&
        start->second.SelectedType == current->HeapID && !current->IsRefinery && !current->IsConstructionYard &&
        !current->IsWall && current->Power <= current->Drain && !Is_Advanced_AI_Defense_Type(current) &&
        (!Get_Advanced_AI_Power_Gain(current, current_power) || current_power <= 0);
    BuildingClass* interrupted_yard = nullptr;
    if (interruptible && factory && !factory->Has_Completed() && factory->Fetch_Stage() < FactoryClass::STEP_COUNT / 2) {
        for (BuildingClass* yard : Buildings) {
            if (Usable_Owned_Building(yard, house) && yard->Factory == factory &&
                yard->Class->ToBuild == RTTI_BUILDINGTYPE) {
                interrupted_yard = yard;
                break;
            }
        }
    }
    if ((!factory && (!queued || interruptible)) || interrupted_yard) {
        const char* reason = nullptr;
        long long minimum = 0;
        AdvancedAIPowerGoal goal;
        const auto* replacing = interrupted_yard ? factory : nullptr;
        const auto* defense = Select_Advanced_AI_Defense(reason, minimum, goal, true, true, replacing);
        if (!defense && !factory && queued && (start == BuildingStarts.end() || !start->second.Goal.Type)) {
            defense = Select_Advanced_AI_Preventive_Defense(goal, nullptr, true);
            if (defense) reason = "preventive-base-defense";
        }
        if (defense) {
            if (interrupted_yard) {
                Trace_Advanced_AI_Building_Order("preempted", "urgent-base-defense", current, factory);
                if (!factory->Abandon()) return {Action::Keep, "building-product-owns-slot"};
                delete factory;
                interrupted_yard->Factory = nullptr;
            } else if (queued) {
                Release_Advanced_AI_Building_Order(reason);
            }
            if (current) Interrupt_Advanced_AI_Development();
            if (start != BuildingStarts.end()) start->second.RejectedType = STRUCT_NONE;
            return {Action::Order, reason, defense, "B1", goal};
        }
    }
    if (factory) return {Action::Keep, "building-product-owns-slot"};

    if (start != BuildingStarts.end()) {
        const StructType rejected = start->second.RejectedType;
        start->second.RejectedType = STRUCT_NONE;
        if (rejected != STRUCT_NONE && rejected == house->BuildStructure) {
            Release_Advanced_AI_Building_Order("factory-rejected-unstarted-order");
        }
    }

    if (house->BuildStructure != STRUCT_NONE) {
        const auto* desired = Building_Order_Type(house->BuildStructure);
        const auto start = BuildingStarts.find(static_cast<int>(house->HeapID));
        const bool optional = start != BuildingStarts.end() && start->second.Optional;
        if (optional && replacement && desired != replacement) {
            Release_Advanced_AI_Building_Order("replacement-before-unstarted-development");
        } else {
            const auto status = Check_Advanced_AI_Building_Start(desired, nullptr);
            if (status == Status::Ready || (status == Status::InsufficientFunds && !optional)) {
                return {Action::Keep, status == Status::Ready ? "queued-order-retained" : Advanced_AI_Building_Start_Reason(status), desired};
            }
            const char* reason = Advanced_AI_Building_Start_Reason(status);
            if (status == Status::NeedsPower) {
                reason = "no-buildable-power-option";
                if (const auto* power = Prioritize_Advanced_AI_Power(desired); power && power != desired) {
                    AdvancedAIPowerGoal goal{desired};
                    if (start != BuildingStarts.end() && start->second.ThreatDefenseOrder) {
                        goal.PreventiveDefense = !start->second.EmergencyDefenseOrder;
                        goal.EmergencyDefense = start->second.EmergencyDefenseOrder;
                    }
                    if (Retarget_Advanced_AI_Development(desired, power, goal)) {
                        return {Action::Order, "power-before-queued-order", power, optional ? "B2" : "B1", goal};
                    }
                    reason = "late-power-budget";
                }
            }
            Release_Advanced_AI_Building_Order(reason);
        }
    }

    if (house->ConstructionYards.Count() == 0) return {Action::Wait, "no-construction-yard"};
    if (house->Available_Money() <= 10) return {Action::Wait, "below-start-minimum"};
    if (house->Power <= 0 || static_cast<long long>(house->Power) < static_cast<long long>(house->Drain) + house->PowerSurplus) {
        if (const auto* power = Select_Advanced_AI_Power_Plant()) {
            Plan_Advanced_AI_Expansion(power);
            return {Action::Order, "restore-power", power, "B1"};
        }
    }
    AdvancedAIPowerGoal goal;
    if (!replacement || (!replacement->IsConstructionYard && !replacement->IsRefinery && replacement->Power <= replacement->Drain)) {
        if (const auto* defense = Select_Advanced_AI_Preventive_Defense(goal, nullptr, true)) {
            return {Action::Order, "preventive-base-defense", defense, "B1", goal};
        }
    }
    if (const auto* prerequisite = Select_Advanced_AI_Air_Defense_Prerequisite()) {
        Plan_Advanced_AI_Expansion(prerequisite);
        return {Action::Order, "air-defense-prerequisite", prerequisite, "B1"};
    }
    if (const auto* dock = Select_Advanced_AI_Aircraft_Dock()) {
        Plan_Advanced_AI_Expansion(dock);
        return {Action::Order, "missing-aircraft-dock", dock, "B1"};
    }
    if (replacement) {
        Plan_Advanced_AI_Expansion(replacement);
        return {Action::Order, "replace-lost-building", replacement, "B1", replacement_goal};
    }
    const auto resumed = Resume_Advanced_AI_Power_Goal();
    if (resumed.Result == Action::Order) return resumed;
    if (const auto* wall = Select_Advanced_AI_Yard_Wall()) return {Action::Order, "construction-yard-wall", wall, "B1"};
    if (const auto* prerequisite = Select_Advanced_AI_Wall_Prerequisite()) {
        Plan_Advanced_AI_Expansion(prerequisite);
        return {Action::Order, "construction-yard-wall-prerequisite", prerequisite, "B1"};
    }
    if (std::any_of(Buildings.begin(), Buildings.end(), [&](const BuildingClass* building) {
        return Usable_Owned_Building(building, house) && building->Class->IsRefinery;
    })) {
        const char* reason = nullptr;
        long long minimum = 0;
        AdvancedAIPowerGoal goal;
        if (const auto* defense = Select_Advanced_AI_Defense(reason, minimum, goal, true)) {
            return {Action::Order, reason, defense, "B1", goal};
        }
    }
    if (const auto* choice = Select_Advanced_AI_Building(goal)) {
        Plan_Advanced_AI_Expansion(choice);
        return {Action::Order, "b1-infrastructure", choice, "B1", goal};
    }
    if (const auto* defense = Select_Advanced_AI_Preventive_Defense(goal)) {
        return {Action::Order, "preventive-base-defense", defense, "B1", goal};
    }
    /**
     *  AF-0009U: Base creep towards a field without a verified refinery site
     *  (see Select_Advanced_AI_Expansion_Creep). The refinery step comes before
     *  the B2 development, the power steps only when B2 has nothing to do.
     */
    const char* creep_reason = nullptr;
    if (const auto* creep = Select_Advanced_AI_Expansion_Creep(true, creep_reason)) {
        return {Action::Order, creep_reason, creep, "B1"};
    }
    const char* development_reason = nullptr;
    if (const auto* choice = Plan_Advanced_AI_Development(development_reason, goal)) {
        return {Action::Order, development_reason, choice, "B2", goal};
    }
    if (const auto* creep = Select_Advanced_AI_Expansion_Creep(false, creep_reason)) {
        return {Action::Order, creep_reason, creep, "B2"};
    }
    return {Action::Wait, development_reason, nullptr, "B2"};
}

void HouseClassExtension::Apply_Advanced_AI_Building_Plan(const AdvancedAIBuildingPlan& plan)
{
    using Action = AdvancedAIBuildingPlan::Action;
    if (plan.Result == Action::Order) {
        HouseClass* house = This();
        auto& start = BuildingStarts[static_cast<int>(house->HeapID)];
        start.SelectedType = plan.Type->HeapID;
        start.Optional = std::string_view(plan.Source) == "B2";
        start.ThreatDefenseOrder = Is_Defense_Response(plan.Reason) && !plan.PowerGoal.Type;
        start.EmergencyDefenseOrder = start.ThreatDefenseOrder && std::string_view(plan.Reason) != "preventive-base-defense";
        if (plan.PowerGoal.Type && plan.PowerGoal.Type != plan.Type &&
            (!start.Goal.Type || start.Goal.Type == plan.PowerGoal.Type)) {
            if (!start.Goal.Type) {
                start.Goal = plan.PowerGoal;
                start.GoalFrame = Frame;
                start.GoalCount = Placed_Building_Count(house, plan.PowerGoal.Type) + 1;
                start.GoalOptional = start.Optional;
                Trace_Advanced_AI_Building_Order("power-goal-held", plan.Reason, plan.PowerGoal.Type, nullptr, nullptr, plan.Source);
            }
            if (plan.PowerGoal.EmergencyDefense) {
                start.Goal.EmergencyDefense = true;
                start.Goal.PreventiveDefense = false;
            } else if (plan.PowerGoal.PreventiveDefense && !start.Goal.EmergencyDefense) {
                start.Goal.PreventiveDefense = true;
            }
            start.Supply = plan.Type->HeapID;
            start.SupplyProduct = -1;
            start.SupplyCompleted = false;
        }
        house->BuildStructure = start.SelectedType;
        Trace_Advanced_AI_Building_Order("selected", plan.Reason, plan.Type, nullptr, nullptr, plan.Source);
        if (start.Optional) {
            if (plan.Type->IsRefinery) Trace_Advanced_AI_Production("planner", "b2-extra-refinery", plan.Type);
            Trace_Advanced_AI_Production("development", "order-selected", plan.Type, 11);
        } else {
            Trace_Advanced_AI_Production("planner", plan.Reason, plan.Type, 11);
        }
        return;
    }

    Trace_Advanced_AI_Building_Order(plan.Result == Action::Wait ? "WAIT" : "keep",
        plan.Reason, plan.Type, nullptr, nullptr, plan.Source);
    const char* phase = plan.Source && std::string_view(plan.Source) == "B2" ? "development" : "planner";
    Trace_Advanced_AI_Production(phase, plan.Reason, plan.Type, plan.Type ? 11 : 0);
}

bool HouseClassExtension::Can_Advanced_AI_Build_Type(const BuildingTypeClass* type, bool check_ai_build_this,
    bool check_prerequisites) const
{
    const auto invalid = [this, type](const char* reason, int detail = 0) {
        Log_Advanced_AI_Error(reason, type, detail);
        return false;
    };
    if (!Valid_Building_Type(type)) return invalid("invalid-type");
    if (type == Rule->WallTower) return false;
    // Native base planning adds BuildPower separately from AIBuildThis candidates.
    const bool listed_power = std::find(Rule->BuildPower.begin(), Rule->BuildPower.end(), type) != Rule->BuildPower.end() &&
        Matches_Role(type, InfrastructureRole::Power);
    // AIBuildThis is an infrastructure-selection policy, not a start condition
    // for orders already chosen by the native planner.
    if (check_ai_build_this && !type->CanAIBuildThis && !listed_power) return false;
    HouseClass* house = This();
    const unsigned actlike = static_cast<unsigned>(house->ActLike);
    if (actlike >= std::numeric_limits<unsigned long>::digits) return false;
    if ((static_cast<unsigned long>(type->Ownable) & (1UL << actlike)) == 0) return false;

    const int level = static_cast<int>(type->Level);
    if (level < 0 || level > house->Control.TechLevel) return false;
    auto self = const_cast<HouseClassExtension*>(this);
    if (!self->Required_Forbidden_Houses_Check(type)) return false;

    for (int prerequisite : type->Prerequisite) {
        // Validate indices before using the existing shared prerequisite helper.
        if (prerequisite >= STRUCT_FIRST) {
            if (prerequisite >= BuildingTypes.Count() || !BuildingTypes[prerequisite]) {
                return invalid("invalid-prerequisite", prerequisite);
            }
        } else {
            const int group = -1 - prerequisite;
            if (group < PREREQ_GROUP_FIRST || group >= PrerequisiteGroups.Count() || !PrerequisiteGroups[group]) return invalid("invalid-prerequisite-group", prerequisite);
            const auto& members = PrerequisiteGroups[group]->Prerequisites;
            if (members.Count() == 0) return false;
            for (int member : members) {
                if (member < STRUCT_FIRST || member >= BuildingTypes.Count() || !BuildingTypes[member]) {
                    return invalid("invalid-group-member", member);
                }
            }
        }
        if (check_prerequisites && !self->Has_Prerequisite(prerequisite)) return false;
    }

    // Can_Build(false, ...) enforces native buildability/tech restrictions, but
    // AI has an early success path. The forced query reaches native BuildLimit
    // handling (including nonpositive lifetime limits); never treat -1 as true.
    const int buildable = house->Can_Build(type, false, false);
    if (buildable <= 0) return false;
    const int limit = house->Can_Build(type, true, false);
    if (limit <= 0) return false;

    return true;
}

bool HouseClassExtension::Has_Advanced_AI_Ready_Yard(const BuildingTypeClass* type, const BuildingClass* factory,
    const FactoryClass* replacing) const
{
    HouseClass* house = This();
    // Existing factory search enforces BuiltAt, ExclusiveFactory and yard
    // ActLike/MultiMCV. It performs no placement search and allocates no objects.
    const BuildingClass* native_yard = type->Who_Can_Build_Me(true, true, false, house);
    if (!native_yard) return false;
    const auto ready_yard = [house, type](const BuildingClass* yard) {
        if (!Usable_Owned_Building(yard, house)) return false;
        if (!yard->IsOn) return false;
        if (yard->Mission == MISSION_CONSTRUCTION || yard->MissionQueue == MISSION_CONSTRUCTION) return false;
        if (yard->Class->ToBuild != RTTI_BUILDINGTYPE) return false;
        if (!(yard->Class->Get_Ownable() & type->Get_Ownable())) return false;
        if (Rule->BuildConst.Is_Present(yard->Class) && !RuleExtension->IsMultiMCV) {
            const unsigned factory_actlike = static_cast<unsigned>(yard->ActLike);
            if (factory_actlike >= std::numeric_limits<unsigned long>::digits ||
                !(static_cast<unsigned long>(type->Get_Ownable()) & (1UL << factory_actlike))) return false;
        }
        const auto typeext = Extension::Fetch(type);
        return typeext->BuiltAt.Is_Present(yard->Class) ||
            (!typeext->BuiltAt.Count() && !Extension::Fetch(yard->Class)->IsExclusiveFactory);
    };
    const auto slot_free = [replacing](const BuildingClass* yard) { return !yard->Factory || yard->Factory == replacing; };
    if (factory) return slot_free(factory) && ready_yard(factory);
    if (slot_free(native_yard) && ready_yard(native_yard)) return true;
    // Who_Can_Build_Me can return a yard still in construction.
    for (const BuildingClass* yard : Buildings) {
        if (yard && slot_free(yard) && ready_yard(yard)) return true;
    }
    return false;
}

bool HouseClassExtension::Can_Advanced_AI_Build_Building(const BuildingTypeClass* type, const BuildingClass* factory,
    bool check_ai_build_this) const
{
    if (!Can_Advanced_AI_Build_Type(type, check_ai_build_this) || !Has_Advanced_AI_Ready_Yard(type, factory)) return false;
    // Use the same native target/slot decisions as upgrade placement.
    return type->PowersUpBuilding.empty() || Find_Advanced_AI_Upgrade_Target(type);
}

bool HouseClassExtension::Has_Advanced_AI_Building_Production() const
{
    return Find_Building_Factory(This_Const()) != nullptr;
}

const BuildingClass* HouseClassExtension::Find_Advanced_AI_Upgrade_Target(const BuildingTypeClass* upgrade) const
{
    if (!Valid_Building_Type(upgrade) || upgrade->PowersUpBuilding.empty()) return nullptr;
    const StructType target_type = BuildingTypeClass::From_Name(upgrade->PowersUpBuilding.c_str());
    if (target_type < STRUCT_FIRST || target_type >= BuildingTypes.Count()) return nullptr;

    const Cell cell = This_Const()->Find_Upgrade_Location(upgrade);
    if (cell == Cell(0, 0) || !Map.In_Radar(cell)) return nullptr;
    const BuildingClass* target = Map[cell].Cell_Building();
    if (!Usable_Owned_Building(target, This_Const()) || !target->IsOn ||
        target->Class != BuildingTypes[target_type] || target->Mission == MISSION_CONSTRUCTION ||
        target->MissionQueue == MISSION_CONSTRUCTION || !target->Can_Upgrade(upgrade, This_Const())) return nullptr;

    // Bound the native destination slot by the actual TS array, without reserving it.
    const int level = target->UpgradeLevel;
    const int next_level = upgrade->PowersUpToLevel == -1 ? level + 1 : upgrade->PowersUpToLevel;
    if (level < 0 || (upgrade->PowersUpToLevel == -1 && level >= target->Class->Upgrades) ||
        next_level <= level || next_level > static_cast<int>(std::size(target->Upgrades)) ||
        target->Upgrades[next_level - 1] != nullptr) return nullptr;
    return target;
}

bool HouseClassExtension::Get_Advanced_AI_Power_Gain(const BuildingTypeClass* type, long long& gain) const
{
    gain = 0;
    if (!Valid_Building_Type(type)) return false;
    if (type->PowersUpBuilding.empty()) {
        gain = static_cast<long long>(type->Power) - type->Drain;
        return true;
    }
    const BuildingClass* target = Find_Advanced_AI_Upgrade_Target(type);
    if (!target) return false;
    long long raw_power = target->Class->Power;
    long long drain = static_cast<long long>(target->Class->Drain) + type->Drain;
    for (const BuildingTypeClass* installed : target->Upgrades) {
        if (installed) {
            raw_power += installed->Power;
            drain += installed->Drain;
        }
    }
    raw_power += type->Power;
    if (raw_power < std::numeric_limits<int>::min() || raw_power > std::numeric_limits<int>::max() ||
        drain < std::numeric_limits<int>::min() || drain > std::numeric_limits<int>::max()) return false;
    const double health = target->HealthRatio;
    const double output = raw_power > 0 ? static_cast<double>(raw_power) * health : 0.0;
    if (!std::isfinite(output) || output < 0.0 || output > std::numeric_limits<int>::max()) return false;
    // Native Power_Output sums installed upgrades, scales by health and truncates.
    gain = static_cast<long long>(static_cast<int>(output)) - target->Power_Output() - (drain - target->Power_Drain());
    return true;
}

bool HouseClassExtension::Has_Advanced_AI_Power_For(const BuildingTypeClass* type) const
{
    long long gain;
    return Get_Advanced_AI_Power_Gain(type, gain) && Sufficient_Power(This_Const(), gain);
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Power_Plant(const BuildingClass* factory) const
{
    // Existing factories (including upgrade products) own the only building start.
    if (Has_Advanced_AI_Building_Production()) return nullptr;
    const int side = static_cast<int>(This_Const()->Class->Side);
    if (side < 0 || side >= Sides.Count() || !Sides[side]) {
        Log_Advanced_AI_Error("invalid-side", nullptr, side);
        return nullptr;
    }
    const auto sideext = Extension::Fetch(Sides[side]);
    const auto buildable = [this, factory](const BuildingTypeClass* type) {
        long long gain;
        if (!type || Is_Advanced_AI_Building_Type_Deferred(type) || !Can_Advanced_AI_Build_Building(type, factory, false)) {
            return false;
        }
        if (!Get_Advanced_AI_Power_Gain(type, gain) || gain <= 0) {
            return false;
        }
        return true;
    };

    const BuildingTypeClass* turbine = sideext->PowerTurbine;
    if (turbine) {
        const BuildingClass* target = Find_Advanced_AI_Upgrade_Target(turbine);
        if (target && target->Class == sideext->RegularPowerPlant && buildable(turbine)) {
            Log_Power_Selection(This_Const(), turbine, "turbine-upgrade");
            return turbine;
        }
    }
    const BuildingTypeClass* advanced = sideext->AdvancedPowerPlant;
    if (advanced && Valid_Building_Type(advanced) && advanced->PowersUpBuilding.empty() && buildable(advanced)) {
        Log_Power_Selection(This_Const(), advanced, "advanced-power-plant");
        return advanced;
    }
    const BuildingTypeClass* regular = sideext->RegularPowerPlant;
    if (regular && Valid_Building_Type(regular) && regular->PowersUpBuilding.empty() && buildable(regular)) {
        Log_Power_Selection(This_Const(), regular, "regular-power-plant");
        return regular;
    }
    // A custom side without power-role assignments can still use an explicit
    // BuildPower entry. Configured side priorities retain their meaning.
    if (!turbine && !advanced && !regular) {
        for (const BuildingTypeClass* type : Rule->BuildPower) {
            if (Matches_Role(type, InfrastructureRole::Power) && buildable(type)) {
                Log_Power_Selection(This_Const(), type, "build-power-list");
                return type;
            }
        }
    }
    return nullptr;
}

const BuildingTypeClass* HouseClassExtension::Prioritize_Advanced_AI_Power(const BuildingTypeClass* desired,
    const BuildingClass* factory) const
{
    if (!Valid_Building_Type(desired)) return nullptr;
    long long desired_gain;
    if (!Get_Advanced_AI_Power_Gain(desired, desired_gain)) return nullptr;
    // A planned power turbine can lose its useful output before production starts.
    if (Is_Unproductive_Power_Turbine(This_Const(), desired, desired_gain)) {
        return Select_Advanced_AI_Power_Plant(factory);
    }
    if (Sufficient_Power(This_Const(), desired_gain)) {
        Trace_Power_Choice(This_Const(), desired, nullptr, false, -desired_gain, 0);
        return desired;
    }
    // Only resolve a type. Callers retain production ownership and pending orders.
    // No recursion: every selected side power candidate has positive actual gain.
    const BuildingTypeClass* plant = Select_Advanced_AI_Power_Plant(factory);
    long long plant_gain = 0;
    if (plant && !Get_Advanced_AI_Power_Gain(plant, plant_gain)) return nullptr;
    Trace_Power_Choice(This_Const(), desired, plant, true, -desired_gain, plant_gain);
    return plant;
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Building(AdvancedAIPowerGoal& goal) const
{
    const HouseClass* house = This_Const();
    const auto first_buildable = [this, &goal](const TypeList<BuildingTypeClass*>& list, InfrastructureRole role) {
        for (const BuildingTypeClass* type : list) {
            if (!Matches_Role(type, role)) {
                continue;
            }
            const char* blocked = "placement-retry-cooldown";
            if (!Is_Advanced_AI_Building_Type_Deferred(type)) {
                blocked = "infrastructure-not-buildable";
                if (Can_Advanced_AI_Build_Building(type) && Has_Advanced_AI_Refineries_For(type)) {
                    if (const auto* choice = Prioritize_Advanced_AI_Power(type)) {
                        if (choice != type) goal.Type = type;
                        return choice;
                    }
                    blocked = "no-buildable-power-option";
                }
            }
            Trace_Advanced_AI_Building_Order("candidate", blocked, type, nullptr, nullptr, "B1");
        }
        return static_cast<const BuildingTypeClass*>(nullptr);
    };
    const auto has_role = [house](const TypeList<BuildingTypeClass*>& list, InfrastructureRole role) {
        for (const BuildingClass* building : Buildings) {
            if (Usable_Owned_Building(building, house) && Matches_Role(building->Class, role) &&
                (role == InfrastructureRole::Refinery || list.Is_Present(building->Class))) return true;
        }
        return false;
    };

    const BuildingTypeClass* choice = nullptr;
    const char* role = "refinery";
    if (!choice && !has_role(Rule->BuildRefinery, InfrastructureRole::Refinery)) {
        choice = first_buildable(Rule->BuildRefinery, InfrastructureRole::Refinery);
    }
    if (!choice && !has_role(Rule->BuildBarracks, InfrastructureRole::Barracks)) {
        choice = first_buildable(Rule->BuildBarracks, InfrastructureRole::Barracks);
        role = "barracks";
    }
    if (!choice && !has_role(Rule->BuildWeapons, InfrastructureRole::Weapons)) {
        choice = first_buildable(Rule->BuildWeapons, InfrastructureRole::Weapons);
        role = "weapons";
    }
    if (choice) {
        if (Matches_Role(choice, InfrastructureRole::Power)) role = "power";
        if (!choice->PowersUpBuilding.empty()) role = "power-upgrade";
        DEV_DEBUG_INFO("AF-0009B1: house={} role={} building={}\n", static_cast<int>(house->HeapID), role, choice->Name());
    }
    return choice;
}

bool HouseClassExtension::Has_Advanced_AI_Refineries_For(const BuildingTypeClass* type) const
{
    return !Matches_Role(type, InfrastructureRole::Weapons) || Advanced_AI_Refinery_Shortfall(false) == 0;
}

int HouseClassExtension::Advanced_AI_Refinery_Shortfall(bool preferred) const
{
    int refineries = 0;
    int weapons = 0;
    for (const BuildingClass* building : Buildings) {
        if (!Usable_Owned_Building(building, This_Const())) continue;
        refineries += building->Class->IsRefinery;
        weapons += Matches_Role(building->Class, InfrastructureRole::Weapons);
    }
    const int target = preferred ? 2 * std::max(1, weapons) : weapons + 1;
    return std::max(0, target - refineries);
}

/**
 *  AF-0009U: When flying enemies threaten the base but no anti-aircraft defense
 *  can be built yet (for example the SAM site needs a radar), order the missing
 *  prerequisite. Without it the base stayed defenseless against jumpjets.
 */
const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Air_Defense_Prerequisite()
{
    static constexpr int AirThreatRadius = 25 * CELL_LEPTON_W;

    HouseClass* house = This();

    /**
     *  Is there a hostile flying unit attacking or near the base?
     */
    const auto threats = Active_Building_Threats(house);
    bool air_threat = threats[static_cast<size_t>(DefenseRole::Air)].Attackers > 0;
    if (!air_threat && house->Center != COORD_NONE) {
        for (const TechnoClass* techno : Technos) {
            if (techno->RTTI == RTTI_BUILDING || !Hostile_To(house, techno) || !techno->In_Air()) continue;
            if (techno->Center_Coord().Distance_To(house->Center) <= AirThreatRadius) {
                air_threat = true;
                break;
            }
        }
    }
    if (!air_threat) return nullptr;

    /**
     *  Nothing to do if an anti-aircraft defense exists or can already be built.
     */
    for (const BuildingClass* building : Buildings) {
        if (Usable_Owned_Building(building, house) && Is_Advanced_AI_Defense_Type(building->Class) &&
            Defense_Range(building->Class, DefenseRole::Air) > 0) return nullptr;
    }
    for (const BuildingTypeClass* type : BuildingTypes) {
        if (Valid_Building_Type(type) && Is_Advanced_AI_Defense_Type(type) && Defense_Range(type, DefenseRole::Air) > 0 &&
            Can_Advanced_AI_Build_Type(type, false)) return nullptr;
    }

    /**
     *  Find a missing prerequisite of an anti-aircraft defense this house could own.
     */
    for (const BuildingTypeClass* defense : BuildingTypes) {
        if (!Valid_Building_Type(defense) || !Is_Advanced_AI_Defense_Type(defense) || Defense_Range(defense, DefenseRole::Air) <= 0 ||
            !Can_Advanced_AI_Build_Type(defense, false, false)) continue;

        for (const int requirement : defense->Prerequisite) {
            if (Has_Prerequisite(requirement)) continue;

            const BuildingTypeClass* candidate = nullptr;
            if (requirement >= STRUCT_FIRST) {
                candidate = requirement < BuildingTypes.Count() ? BuildingTypes[requirement] : nullptr;
            } else {
                const auto group = PrerequisiteGroupClass::Decode(requirement);
                if (group < PREREQ_GROUP_FIRST || group >= PrerequisiteGroups.Count() || !PrerequisiteGroups[group]) continue;
                for (const auto member : PrerequisiteGroups[group]->Prerequisites) {
                    if (member >= STRUCT_FIRST && member < BuildingTypes.Count() &&
                        Can_Advanced_AI_Build_Building(BuildingTypes[member], nullptr, false)) {
                        candidate = BuildingTypes[member];
                        break;
                    }
                }
            }

            if (candidate == nullptr || Is_Advanced_AI_Building_Type_Deferred(candidate) ||
                !Can_Advanced_AI_Build_Building(candidate, nullptr, false)) continue;

            const int cost = candidate->Cost_Of(house);
            if (cost < 0) continue;

            if (Advanced_AI_Uncommitted_Funds() < cost) {
                // Keep the credits for it; units and optional buildings wait.
                Request_Advanced_AI_Savings(cost, "air-defense-prerequisite", true);
                return nullptr;
            }

            DEV_DEBUG_INFO("AF-0009U: house={} frame={} air-defense-prerequisite={} for={}\n",
                static_cast<int>(house->HeapID), Frame, candidate->Name(), defense->Name());
            return Prioritize_Advanced_AI_Power(candidate);
        }
    }

    return nullptr;
}


/**
 *  Aftermath: Does this aircraft type need a fixed dock? Every armed aircraft
 *  that lists docks does (it rearms there); transports and other unarmed
 *  aircraft may exist without one.
 */
bool HouseClassExtension::Advanced_AI_Aircraft_Needs_Dock(const AircraftTypeClass* type)
{
    return type != nullptr && !type->IsCarryall && type->Dock.Count() > 0 &&
        type->Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon != nullptr;
}


/**
 *  Aftermath: Dock balance for aircraft of the given type. 'docks' counts the
 *  owned docks the type can use (an aircraft MultiDock building counts its
 *  AircraftDockCount). 'aircraft' counts the owned armed aircraft (flying,
 *  landed or in production) that use any of these dock types.
 */
void HouseClassExtension::Advanced_AI_Aircraft_Dock_Balance(const AircraftTypeClass* type, int& aircraft, int& docks) const
{
    aircraft = 0;
    docks = 0;
    if (!Advanced_AI_Aircraft_Needs_Dock(type)) return;

    const HouseClass* house = This_Const();

    for (const BuildingClass* building : Buildings) {
        if (!Usable_Owned_Building(building, house) || !type->Dock.Is_Present(building->Class)) continue;
        const auto* typeext = Extension::Fetch(building->Class);
        docks += typeext->Is_Aircraft_MultiDock() ? typeext->AircraftDockCount : 1;
    }

    const auto shares_dock = [type](const AircraftTypeClass* other) {
        if (!Advanced_AI_Aircraft_Needs_Dock(other)) return false;
        for (const BuildingTypeClass* dock : other->Dock) {
            if (type->Dock.Is_Present(const_cast<BuildingTypeClass*>(dock))) return true;
        }
        return false;
    };

    for (const AircraftClass* unit : Aircrafts) {
        if (unit == nullptr || unit->House != house || !unit->IsActive || unit->Strength <= 0 || unit->IsInLimbo) continue;
        if (shares_dock(unit->Class)) aircraft++;
    }

    for (const FactoryClass* factory : Factories) {
        if (factory == nullptr || factory->House != house) continue;
        const TechnoClass* product = factory->Get_Object();
        if (product != nullptr && product->RTTI == RTTI_AIRCRAFT && shares_dock(static_cast<const AircraftClass*>(product)->Class)) {
            aircraft++;
        }
    }
}


/**
 *  AF-0009U: Limits checked again when a factory takes over a house build
 *  request. The selectors only check them when choosing the type, and the
 *  request stays set until delivery, so several free factories could otherwise
 *  start the same order and exceed the dock capacity or the team budget.
 */
const char* HouseClassExtension::Advanced_AI_Unit_Start_Limit(const TechnoTypeClass* type) const
{
    if (type == nullptr || !Is_Advanced_AI_Unit_Production()) return nullptr;

    const HouseClass* house = This_Const();

    // Dock capacity: armed aircraft (including those in production) need a dock each.
    if (type->RTTI == RTTI_AIRCRAFTTYPE) {
        const auto* airtype = static_cast<const AircraftTypeClass*>(type);
        if (Advanced_AI_Aircraft_Needs_Dock(airtype)) {
            int aircraft = 0;
            int docks = 0;
            Advanced_AI_Aircraft_Dock_Balance(airtype, aircraft, docks);
            if (aircraft >= docks) return "dock-capacity";
        }
    }

    // Anti-aircraft units built against an air attack on the base are not team orders.
    if (Advanced_AI_Can_Hit_Air(type) && Advanced_AI_Air_Attackers() > 0) return nullptr;

    // Harvesters, base rebuilders and engineers for capturing are not team orders.
    if (type->RTTI == RTTI_UNITTYPE) {
        const auto* unittype = static_cast<const UnitTypeClass*>(type);
        if (unittype->IsToHarvest || unittype->IsToVeinHarvest || Needs_Advanced_AI_Recovery_Unit(unittype) ||
            (unittype->DeploysInto != nullptr && unittype->DeploysInto->IsConstructionYard)) return nullptr;
    }
    if (type->RTTI == RTTI_INFANTRYTYPE && static_cast<const InfantryTypeClass*>(type)->IsEngineer) return nullptr;

    // The team that the selector fills with this kind of object.
    const auto team_for = [this](const TechnoTypeClass* t) -> TeamClass* {
        const RTTIType rtti = t->RTTI == RTTI_INFANTRYTYPE ? RTTI_INFANTRY :
            t->RTTI == RTTI_AIRCRAFTTYPE ? RTTI_AIRCRAFT : RTTI_UNIT;
        const ProductionFlags flags = Extension::Fetch(t)->IsNaval ? PRODFLAG_NAVAL : PRODFLAG_NONE;
        int id = 0;
        return Get_Team_In_Production(id, rtti, flags);
    };

    TeamClass* team = team_for(type);
    if (team == nullptr) return nullptr; // Rush counter production without a team.

    const TeamClassExtension* teamext = Extension::Fetch(team);
    if (teamext->MaxCost <= 0) return nullptr;

    // Combat units already in production for the same team count against its budget.
    long long pending = 0;
    for (const FactoryClass* factory : Factories) {
        if (!factory || factory->House != house) continue;
        const TechnoClass* product = factory->Get_Object();
        if (!product || !product->IsInLimbo || product->RTTI == RTTI_BUILDING) continue;
        const TechnoTypeClass* ptype = product->TClass;
        if (ptype->RTTI == RTTI_UNITTYPE) {
            const auto* unittype = static_cast<const UnitTypeClass*>(ptype);
            if (unittype->IsToHarvest || unittype->IsToVeinHarvest ||
                (unittype->DeploysInto != nullptr && unittype->DeploysInto->IsConstructionYard)) continue;
        }
        if (ptype->RTTI == RTTI_INFANTRYTYPE && static_cast<const InfantryTypeClass*>(ptype)->IsEngineer) continue;
        if (team_for(ptype) == team) pending += ptype->Cost;
    }

    if (teamext->CurrentCost + pending + type->Cost > teamext->AdvAI_Effective_Max_Cost()) return "team-budget";
    return nullptr;
}


/**
 *  AF-0009U: Can this type hit flying targets (aircraft, airborne jumpjets)?
 */
bool HouseClassExtension::Advanced_AI_Can_Hit_Air(const TechnoTypeClass* type)
{
    if (type == nullptr) return false;
    for (WeaponSlotType slot : { WEAPON_SLOT_PRIMARY, WEAPON_SLOT_SECONDARY }) {
        const WeaponTypeClass* weapon = type->Fetch_Weapon_Info(slot).Weapon;
        if (weapon != nullptr && weapon->Bullet != nullptr && weapon->Bullet->IsAntiAircraft) return true;
    }
    return false;
}


/**
 *  AF-0009U: Flying enemies (aircraft, airborne jumpjets) that hit one of our
 *  buildings within the active defense window.
 */
int HouseClassExtension::Advanced_AI_Air_Attackers() const
{
    const auto threats = Active_Building_Threats(This_Const());
    return threats[static_cast<size_t>(DefenseRole::Air)].Attackers;
}


/**
 *  Aftermath: When armed aircraft outnumber their docks (for example after
 *  helipads were destroyed), order a missing dock if it can be built and paid.
 */
const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Aircraft_Dock()
{
    HouseClass* house = This();
    std::vector<const AircraftTypeClass*> checked;

    for (const AircraftClass* unit : Aircrafts) {
        if (unit == nullptr || unit->House != house || !unit->IsActive || unit->Strength <= 0) continue;
        const AircraftTypeClass* type = unit->Class;
        if (!Advanced_AI_Aircraft_Needs_Dock(type) || std::find(checked.begin(), checked.end(), type) != checked.end()) continue;
        checked.push_back(type);

        int aircraft = 0;
        int docks = 0;
        Advanced_AI_Aircraft_Dock_Balance(type, aircraft, docks);
        if (aircraft <= docks) continue;

        for (const BuildingTypeClass* dock : type->Dock) {
            if (!Valid_Building_Type(dock) || Is_Advanced_AI_Building_Type_Deferred(dock) ||
                !Can_Advanced_AI_Build_Building(dock, nullptr, false)) continue;

            const int cost = dock->Cost_Of(house);
            if (cost < 0) continue;

            if (Advanced_AI_Uncommitted_Funds() < cost) {
                Request_Advanced_AI_Savings(cost, "aircraft-dock", false);
                return nullptr;
            }

            DEV_DEBUG_INFO("AF-0009U: house={} frame={} aircraft-dock={} for={} aircraft={} docks={}\n",
                static_cast<int>(house->HeapID), Frame, dock->Name(), type->Name(), aircraft, docks);
            return Prioritize_Advanced_AI_Power(dock);
        }
    }

    return nullptr;
}


const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Wall_Prerequisite()
{
    // If a faction needs its barracks for walls, bring it forward when it
    // completes the prerequisites for an otherwise legal wall ring.
    const long long funds = Advanced_AI_Uncommitted_Funds();
    const long long reserve = Advanced_AI_Refinery_Reserve();
    HouseClass* house = This();
    for (const BuildingTypeClass* wall : Rule->ConcreteWalls) {
        if (!Valid_Building_Type(wall) || !wall->IsWall || !wall->ToOverlay || !wall->ToOverlay->IsWall ||
            !Can_Advanced_AI_Build_Type(wall, false, false) || Is_Advanced_AI_Building_Type_Deferred(wall) ||
            Find_Advanced_AI_Yard_Wall_Site(wall) == Cell(0, 0)) continue;
        for (const BuildingTypeClass* barracks : Rule->BuildBarracks) {
            if (!Matches_Role(barracks, InfrastructureRole::Barracks) || Is_Advanced_AI_Building_Type_Deferred(barracks) ||
                !Can_Advanced_AI_Build_Building(barracks) || !Has_Advanced_AI_Power_For(barracks)) continue;
            bool unlocks = false;
            bool blocked = false;
            for (const int requirement : wall->Prerequisite) {
                if (Has_Prerequisite(requirement)) continue;
                bool matches = requirement == barracks->HeapID;
                if (requirement < STRUCT_FIRST) {
                    const auto group = PrerequisiteGroupClass::Decode(requirement);
                    const auto& members = PrerequisiteGroups[group]->Prerequisites;
                    matches = std::find(members.begin(), members.end(), barracks->HeapID) != members.end();
                }
                unlocks |= matches;
                blocked |= !matches;
            }
            const int cost = barracks->Cost_Of(house);
            const int wall_cost = wall->Cost_Of(house);
            if (unlocks && !blocked && cost >= 0 && wall_cost >= 0 && funds >= reserve + cost + wall_cost) return barracks;
        }
    }
    return nullptr;
}

long long HouseClassExtension::Advanced_AI_Uncommitted_Funds() const
{
    HouseClass* house = This();
    long long funds = house->Available_Money();
    for (const FactoryClass* factory : Factories) {
        if (factory && factory->House == house && factory->Get_Object()) funds -= std::max(0, factory->Balance);
    }
    return funds;
}

/**
 *  AF-0009U: Savings for a resource expansion ("expansion-refinery",
 *  "expansion-power-and-refinery").
 */
static bool Is_Expansion_Savings_Reason(const char* reason)
{
    return reason != nullptr && std::string_view(reason).starts_with("expansion");
}


/**
 *  AF-0009U: The building planner publishes what it is saving for. The goal
 *  expires unless the planner refreshes it.
 */
void HouseClassExtension::Request_Advanced_AI_Savings(long long amount, const char* reason, bool economy) const
{
    if (amount <= 0) return;

    // Only save for an expansion whose site has been found (not for a target out of build range).
    if (Is_Expansion_Savings_Reason(reason) && !Has_Advanced_AI_Expansion_Site()) return;

    const int id = static_cast<int>(This_Const()->HeapID);
    auto& goal = SavingsGoals[id];

    // Within one planner pass, keep the most important and then the largest request.
    const bool same_pass = goal.Frame == Frame;
    if (same_pass && (goal.Economy && !economy || (goal.Economy == economy && goal.Amount >= amount))) return;

    // Track since when the planner continuously saves for the same kind of goal.
    const bool continuing = goal.Amount > 0 && goal.Economy == economy && Frame >= goal.Frame &&
        Frame - goal.Frame <= SavingsGoalLifetime;
    if (!continuing) goal.Since = Frame;

    goal.Amount = amount;
    goal.Frame = Frame;
    goal.Reason = reason;
    goal.Economy = economy;
}


/**
 *  AF-0009U: Economy phase of the tactic based unit production.
 *
 *  Like DTA, all factories normally run and an expansion refinery is paid in
 *  installments next to the units. As a fallback only: when a resource
 *  expansion has been waiting for credits for more than two minutes, the house
 *  has fewer refineries than the planner aims for (2 per weapons factory) and an
 *  expansion target exists, only one factory at a time may build combat units.
 *
 *  Not limited: harvesters and recovery units, a house whose base was attacked
 *  within the last minute, an outnumbered army, and an early rush threat.
 */
bool HouseClassExtension::Is_Advanced_AI_Economy_Phase_Limited(const TechnoTypeClass* type) const
{
    if (!Is_Advanced_AI_Unit_Production()) return false;

    if (type->RTTI == RTTI_UNITTYPE) {
        const auto* unittype = static_cast<const UnitTypeClass*>(type);
        if (unittype->IsToHarvest || unittype->IsToVeinHarvest || Needs_Advanced_AI_Recovery_Unit(unittype)) return false;
    }

    const HouseClass* house = This_Const();
    if (house->LATime > 0 && house->LATime + TICKS_PER_MINUTE > Frame) return false;
    if (IsUnderStartRushThreat || AdvAI_Is_Outnumbered()) return false;
    if (Advanced_AI_Can_Hit_Air(type) && Advanced_AI_Air_Attackers() > 0) return false;

    if (Advanced_AI_Refinery_Shortfall(true) <= 0) return false;
    if (Advanced_AI_Expansion_Target() == Cell(0, 0) || !Has_Advanced_AI_Expansion_Site()) return false;

    // Fallback only: the expansion has been waiting for credits for over two minutes.
    static constexpr int EconomyPhaseDelay = 2 * TICKS_PER_MINUTE;
    const auto goal = SavingsGoals.find(static_cast<int>(house->HeapID));
    if (goal == SavingsGoals.end() || !goal->second.Economy || goal->second.Amount <= 0 ||
        Frame < goal->second.Frame || Frame - goal->second.Frame > SavingsGoalLifetime ||
        Frame - goal->second.Since < EconomyPhaseDelay) return false;

    // Is another factory already building a combat unit?
    for (const FactoryClass* factory : Factories) {
        if (!factory || factory->House != house) continue;
        const TechnoClass* product = factory->Get_Object();
        if (!product || product->RTTI == RTTI_BUILDING) continue;
        if (product->RTTI == RTTI_UNIT) {
            const auto* unittype = static_cast<const UnitClass*>(product)->Class;
            if (unittype->IsToHarvest || unittype->IsToVeinHarvest) continue;
        }
        return true;
    }

    return false;
}


/**
 *  AF-0009U: Drops an economy savings goal once its building has started.
 */
void HouseClassExtension::Clear_Advanced_AI_Economy_Savings() const
{
    const auto found = SavingsGoals.find(static_cast<int>(This_Const()->HeapID));
    if (found != SavingsGoals.end() && found->second.Economy) {
        SavingsGoals.erase(found);
    }
}


/**
 *  AF-0009U: Applies the planner's savings goal to a unit start.
 *  Harvesters, recovery units, self defense and an outnumbered army keep priority.
 */
void HouseClassExtension::Reserve_Advanced_AI_Savings(const TechnoTypeClass* type, AdvancedAIUnitStartBudget& budget) const
{
    const HouseClass* house = This_Const();
    const auto found = SavingsGoals.find(static_cast<int>(house->HeapID));
    if (found == SavingsGoals.end()) return;

    const auto& goal = found->second;
    if (Frame - goal.Frame > SavingsGoalLifetime || goal.Amount <= budget.Reserve) return;

    // The expansion site was lost (or never found): don't keep the credits back.
    if (Is_Expansion_Savings_Reason(goal.Reason) && !Has_Advanced_AI_Expansion_Site()) return;

    if (type->RTTI == RTTI_UNITTYPE) {
        const auto* unittype = static_cast<const UnitTypeClass*>(type);
        if (unittype->IsToHarvest || unittype->IsToVeinHarvest) return;
    }

    /**
     *  AF-0009U: Under attack, unit starts normally ignore savings so units can
     *  defend. Against an air attack ("air-defense") ground-only units cannot
     *  help: they keep waiting for the anti-aircraft defense, while units that
     *  can hit air targets are never held back by it.
     */
    const bool air_goal = std::string_view(goal.Reason).starts_with("air-defense");
    if (air_goal && Advanced_AI_Can_Hit_Air(type)) return;
    if (!air_goal && house->LATime > 0 && house->LATime + TICKS_PER_MINUTE > Frame) return;

    // Technology waits while the army is weaker than the enemy, the economy does not.
    if (!goal.Economy && (!Is_Advanced_AI_Unit_Production() || AdvAI_Is_Disadvantaged())) return;

    budget.Reserve = goal.Amount;
    budget.ReserveReason = goal.Reason;
}


bool HouseClassExtension::Needs_Advanced_AI_Recovery_Unit(const UnitTypeClass* type) const
{
    HouseClass* house = This();
    if (std::find(Rule->HarvesterUnit.begin(), Rule->HarvesterUnit.end(), type) != Rule->HarvesterUnit.end()) {
        if (house->Count_Owned(Rule->HarvesterUnit) > 0) return false;
        return std::any_of(Buildings.begin(), Buildings.end(), [house, type](const BuildingClass* building) {
            return Usable_Owned_Building(building, house) && building->Class->IsRefinery &&
                std::find(type->Dock.begin(), type->Dock.end(), building->Class) != type->Dock.end();
        });
    }
    if (house->ConstructionYards.Count() != 0 || !type->DeploysInto || !type->DeploysInto->IsConstructionYard) return false;
    const auto& base_units = RuleExtension->BaseUnit;
    return (type == Rule->BaseUnit || std::find(base_units.begin(), base_units.end(), type) != base_units.end()) &&
        house->Count_Owned(RuleExtension->BaseUnit) == 0 &&
        (!Rule->BaseUnit || house->UQuantity.Value(Rule->BaseUnit->HeapID) == 0);
}

void HouseClassExtension::Reserve_Advanced_AI_Infrastructure(const TechnoClass* building_product,
    AdvancedAIUnitStartBudget& budget) const
{
    HouseClass* house = This();
    const auto found = BuildingStarts.find(static_cast<int>(house->HeapID));
    if (found == BuildingStarts.end()) return;
    const auto& start = found->second;
    const auto* queued = Building_Order_Type(house->BuildStructure);
    const bool pending = !building_product && queued && start.SelectedType == house->BuildStructure &&
        start.RejectedType != house->BuildStructure;
    // AF-0009U: A selected refinery is protected even as an optional B2
    // resource expansion; otherwise unit starts took its credits before it began.
    const auto protect = [](const BuildingTypeClass* type, bool optional, bool threat_defense) {
        return threat_defense || type->IsRefinery || (!optional && (type->IsConstructionYard ||
            type->Power > type->Drain || Matches_Role(type, InfrastructureRole::Barracks) ||
            Matches_Role(type, InfrastructureRole::Weapons)));
    };

    const auto* goal = start.Goal.Type;
    const bool pending_supply = pending && start.Supply == house->BuildStructure;
    const bool producing_supply = building_product && start.SupplyProduct == building_product->ID &&
        static_cast<const BuildingClass*>(building_product)->Class->HeapID == start.Supply;
    const bool threat_goal = start.Goal.EmergencyDefense || start.Goal.PreventiveDefense;
    const bool protect_goal = goal && (!pending || goal != queued) &&
        (start.SupplyCompleted || pending_supply || producing_supply) &&
        Frame >= start.GoalFrame && Frame - start.GoalFrame < 3 * TICKS_PER_MINUTE &&
        protect(goal, start.GoalOptional, threat_goal) && Placed_Building_Count(house, goal) < start.GoalCount;

    const bool protect_order = pending && (protect(queued, start.Optional, start.ThreatDefenseOrder) ||
        (pending_supply && protect_goal));
    if (protect_order) {
        budget.Reserve = std::max(0, queued->Cost_Of(house));
        budget.ReserveReason = "infrastructure-order";
    }
    // An allocated power product is already covered by its factory balance.
    if (protect_goal) {
        budget.Reserve += std::max(0, goal->Cost_Of(house));
        budget.ReserveReason = "infrastructure-after-power";
    }
    if ((protect_order && start.ThreatDefenseOrder) || (protect_goal && threat_goal)) {
        const auto defense = PreventiveDefenses.find(static_cast<int>(house->HeapID));
        const bool urgent = (protect_order && start.EmergencyDefenseOrder) || (protect_goal && start.Goal.EmergencyDefense) ||
            (defense != PreventiveDefenses.end() && defense->second.Focus.Urgent);
        const long long remaining = budget.Uncommitted - budget.Cost;
        budget.Reserve += Threat_Defense_Reserve(remaining, Advanced_AI_Refinery_Reserve(), urgent);
    }
}

AdvancedAIUnitStartBudget HouseClassExtension::Check_Advanced_AI_Unit_Start(const TechnoTypeClass* type) const
{
    HouseClass* house = This();
    AdvancedAIUnitStartBudget budget;
    budget.Uncommitted = house->Available_Money();
    budget.Cost = type->Cost_Of(house);
    const TechnoClass* building_product = nullptr;
    int pending = 0;
    for (const FactoryClass* factory : Factories) {
        if (!factory || factory->House != house) continue;
        const TechnoClass* product = factory->Get_Object();
        if (!product) continue;
        budget.Uncommitted -= std::max(0, factory->Balance);
        if (!building_product && product->RTTI == RTTI_BUILDING) building_product = product;
        if (product->TClass == type && product->IsInLimbo) ++pending;
    }

    // Preserve the native recovery path and its installment-based start.
    if (budget.Cost < 0) {
        budget.Blocked = "invalid-cost";
        return budget;
    }
    if (type->RTTI == RTTI_UNITTYPE && Needs_Advanced_AI_Recovery_Unit(static_cast<const UnitTypeClass*>(type))) {
        budget.ReserveReason = "recovery-unit";
        return budget;
    }
    Reserve_Advanced_AI_Infrastructure(building_product, budget);
    Reserve_Advanced_AI_Savings(type, budget);
    if (budget.Uncommitted >= budget.Cost + budget.Reserve &&
        house->BuildUnit >= UNIT_FIRST && house->BuildUnit < UnitTypes.Count()) {
        const auto* recovery = UnitTypes[house->BuildUnit];
        if (recovery && Needs_Advanced_AI_Recovery_Unit(recovery) && recovery->Who_Can_Build_Me(true, true, true, house)) {
            const bool has_infrastructure = budget.Reserve > 0;
            budget.Reserve += std::max(0, recovery->Cost_Of(house));
            budget.ReserveReason = has_infrastructure ? "infrastructure-and-recovery-unit" : "recovery-unit";
        }
    }
    if (const char* limit = Advanced_AI_Unit_Start_Limit(type)) {
        budget.Blocked = limit;
        budget.ClearRequest = true;
    }
    else if (budget.Uncommitted < budget.Cost) budget.Blocked = "committed-funds";
    else if (budget.Uncommitted - budget.Cost < budget.Reserve) budget.Blocked = "reserved-funds";
    else if (Is_Advanced_AI_Economy_Phase_Limited(type)) budget.Blocked = "economy-phase";
    // AF-0009U: Tactic teams have no TaskForce slots; their MaxCost and the
    // tactic duration limit parallel production instead.
    else if (pending && !Is_Advanced_AI_Unit_Production() && !Has_Unfilled_Team_Demand(house, type, pending)) {
        budget.Blocked = "order-already-in-production";
    }
    return budget;
}

void HouseClassExtension::Trace_Advanced_AI_Unit_Start(const TechnoTypeClass* type, const AdvancedAIUnitStartBudget& budget,
    const char* outcome) const
{
#ifdef NDEBUG
    if (!Vinifera_DeveloperMode) return;
#endif
    const int house = static_cast<int>(This_Const()->HeapID);
    const auto key = std::make_tuple(house, type->RTTI, type->Fetch_Heap_ID());
    const char* reason = budget.Blocked ? budget.Blocked : "none";
    const std::string_view result = outcome;
    if (result == "started" || result == "first-payment-pending") {
        UnitStartTraces.erase(key);
    } else {
        const std::tuple<std::string_view, std::string_view, std::string_view> state{result, reason, budget.ReserveReason};
        const auto previous = UnitStartTraces.find(key);
        if (previous != UnitStartTraces.end() && previous->second == state) return;
        UnitStartTraces.insert_or_assign(key, state);
    }
    DEV_DEBUG_INFO("AF-0009-UNIT-A: house={} frame={} unit={} result={} reason={} reserve-reason={} cost={} uncommitted={} reserve={}\n",
        house, Frame, type->Name(), outcome, reason, budget.ReserveReason, budget.Cost, budget.Uncommitted, budget.Reserve);
}

long long HouseClassExtension::Advanced_AI_Refinery_Reserve() const
{
    HouseClass* house = This();
    const unsigned side = static_cast<unsigned>(house->ActLike);
    long long reserve = 0;
    for (const BuildingTypeClass* type : Rule->BuildRefinery) {
        if (!Matches_Role(type, InfrastructureRole::Refinery) || side >= std::numeric_limits<unsigned long>::digits ||
            !(static_cast<unsigned long>(type->Get_Ownable()) & (1UL << side)) ||
            type->Level < 0 || type->Level > house->Control.TechLevel) continue;
        const int cost = type->Cost_Of(house);
        if (cost > 0 && (!reserve || cost < reserve)) reserve = cost;
    }
    return reserve;
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Development(const char*& need, long long& minimum,
    AdvancedAIPowerGoal& goal) const
{
    HouseClass* house = This();
    const long long funds = Advanced_AI_Uncommitted_Funds();
    const long long reserve = Advanced_AI_Refinery_Reserve();
    need = "no-team-or-native-production-demand";
    if (funds <= 10 || funds <= reserve) {
        need = "insufficient-uncommitted-funds";
        return nullptr;
    }

    struct Demand
    {
        const TechnoTypeClass* Type = nullptr;
        int Count = 0;
        bool NativeOrder = false;
        bool InProduction = false;
    };
    std::map<std::pair<int, int>, Demand> requests;
    const auto key = [](const TechnoTypeClass* type) {
        return std::make_pair(static_cast<int>(type->RTTI), type->Fetch_Heap_ID());
    };
    const auto add_native_order = [&](const TechnoTypeClass* type) {
        if (!type || (type->RTTI == RTTI_UNITTYPE && Extension::Fetch(type)->IsNaval)) return;
        auto& demand = requests[key(type)];
        demand.Type = type;
        demand.NativeOrder = true;
    };
    if (house->BuildInfantry >= 0 && house->BuildInfantry < InfantryTypes.Count()) add_native_order(InfantryTypes[house->BuildInfantry]);
    if (house->BuildUnit >= 0 && house->BuildUnit < UnitTypes.Count()) add_native_order(UnitTypes[house->BuildUnit]);
    if (house->BuildAircraft >= 0 && house->BuildAircraft < AircraftTypes.Count()) add_native_order(AircraftTypes[house->BuildAircraft]);
    for (TeamClass* team : Teams) {
        if (!Team_Needs_Members(team, house)) continue;
        DynamicVectorClass<const TechnoTypeClass*> missing;
        team->Team_Members(missing);
        for (const TechnoTypeClass* type : missing) {
            if (!type || (type->RTTI != RTTI_INFANTRYTYPE && type->RTTI != RTTI_AIRCRAFTTYPE &&
                (type->RTTI != RTTI_UNITTYPE || Extension::Fetch(type)->IsNaval))) continue;
            auto& demand = requests[key(type)];
            demand.Type = type;
            ++demand.Count;
        }
    }
    if (requests.empty()) return nullptr;

    const auto account_for = [&](const TechnoTypeClass* type) {
        if (!type) return;
        const auto found = requests.find(key(type));
        if (found != requests.end()) --found->second.Count;
    };
    for (const InfantryClass* unit : Infantry) if (unit && unit->Is_Recruitable(house)) account_for(unit->TClass);
    for (const UnitClass* unit : Units) if (unit && unit->Is_Recruitable(house)) account_for(unit->TClass);
    for (const AircraftClass* unit : Aircrafts) if (unit && unit->Is_Recruitable(house)) account_for(unit->TClass);
    for (const FactoryClass* factory : Factories) {
        if (!factory || factory->House != house) continue;
        const TechnoClass* product = factory->Get_Object();
        if (!product || !product->TClass) continue;
        const auto found = requests.find(key(product->TClass));
        if (found == requests.end()) continue;
        found->second.InProduction = true;
        if (product->IsInLimbo) --found->second.Count;
    }

    const auto safe_infrastructure = [](const BuildingTypeClass* type) {
        return Valid_Building_Type(type) && type->PowersUpBuilding.empty() && !type->IsBaseDefense &&
            !type->IsWall && !type->IsGate && type != Rule->WallTower &&
            std::find(Rule->BuildDefense.begin(), Rule->BuildDefense.end(), type) == Rule->BuildDefense.end() &&
            std::find(Rule->BuildAA.begin(), Rule->BuildAA.end(), type) == Rule->BuildAA.end();
    };
    int rejected = 0;
    const auto reject = [&](const BuildingTypeClass* type, const char* reason, long long required = 0) {
        need = reason;
        if (rejected++ < 4) {
            DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} candidate={} rejected={} minimum={} uncommitted={}\n",
                static_cast<int>(house->HeapID), Frame, type->Name(), reason, required, funds);
        }
    };
    const auto funded_choice = [&](const BuildingTypeClass* type) -> const BuildingTypeClass* {
        if (!Has_Advanced_AI_Refineries_For(type)) {
            reject(type, "weapons-factory-needs-refinery");
            return nullptr;
        }
        const char* reason = nullptr;
        long long required = 0;
        const auto* choice = Fund_Optional_Building(*this, type, funds, reserve, required, reason);
        if (!choice) {
            reject(type, reason, required);
            return nullptr;
        }
        minimum = required;
        if (choice != type) goal.Type = type;
        return choice;
    };

    const unsigned side = static_cast<unsigned>(house->ActLike);
    if (side >= std::numeric_limits<unsigned long>::digits) return nullptr;
    auto* self = const_cast<HouseClassExtension*>(this);
    const auto permitted_type = [&](const TechnoTypeClass* type) {
        return (static_cast<unsigned long>(type->Get_Ownable()) & (1UL << side)) &&
            self->Required_Forbidden_Houses_Check(type) && house->Can_Build(type, false, false) > 0 &&
            house->Can_Build(type, true, true) > 0;
    };
    const auto prerequisites = [&](const TechnoTypeClass* target, const BuildingTypeClass* first = nullptr,
        const BuildingTypeClass* second = nullptr) {
        const auto covered = [&](int member) {
            return (first && member == first->HeapID) || (second && member == second->HeapID) || self->Has_Prerequisite(member);
        };
        for (const int prerequisite : target->Prerequisite) {
            if (prerequisite >= STRUCT_FIRST) {
                if (prerequisite >= BuildingTypes.Count() || !BuildingTypes[prerequisite] || !covered(prerequisite)) return false;
            } else {
                const auto group = PrerequisiteGroupClass::Decode(prerequisite);
                if (group < PREREQ_GROUP_FIRST || group >= PrerequisiteGroups.Count() || !PrerequisiteGroups[group]) return false;
                bool satisfied = false;
                for (const int member : PrerequisiteGroups[group]->Prerequisites) {
                    if (member < STRUCT_FIRST || member >= BuildingTypes.Count() || !BuildingTypes[member]) return false;
                    satisfied |= covered(member);
                }
                if (!satisfied) return false;
            }
        }
        return true;
    };
    const auto permits = [](const BuildingTypeClass* factory, const TechnoTypeClass* product) {
        if (!factory || factory->ToBuild != product->RTTI || !(factory->Get_Ownable() & product->Get_Ownable())) return false;
        const auto* factory_ext = Extension::Fetch(factory);
        if (product->RTTI == RTTI_UNITTYPE && factory_ext->IsNaval) return false;
        const auto* product_ext = Extension::Fetch(product);
        return std::find(product_ext->BuiltAt.begin(), product_ext->BuiltAt.end(), factory) != product_ext->BuiltAt.end() ||
            (!product_ext->BuiltAt.Count() && !factory_ext->IsExclusiveFactory);
    };

    need = "existing-units-or-products-cover-demand";
    for (const auto& entry : requests) {
        const auto& demand = entry.second;
        const bool native_need = demand.NativeOrder && !demand.InProduction;
        if (demand.Count <= 0 && !native_need) continue;
        const TechnoTypeClass* product = demand.Type;
        if (!permitted_type(product)) {
            need = "requested-unit-not-buildable";
            continue;
        }

        const bool infantry = product->RTTI == RTTI_INFANTRYTYPE;
        const bool aircraft = product->RTTI == RTTI_AIRCRAFTTYPE;
        const auto& types = infantry ? Rule->BuildBarracks : (aircraft ? Rule->BuildHelipad : Rule->BuildWeapons);
        const int limit = infantry ? Rule->BarracksLimit : (aircraft ? Rule->HelipadLimit : Rule->WarLimit);
        const bool needs_dock = aircraft && !static_cast<const AircraftTypeClass*>(product)->IsCarryall && product->Dock.Count() > 0;
        const auto accepts_docking = [&](const BuildingTypeClass* type) {
            return (type->IsHelipad || type->IsCanUnitReload || type->IsCanUnitRepair) &&
                std::find(product->Dock.begin(), product->Dock.end(), type) != product->Dock.end();
        };
        int owned = 0;
        bool compatible = false;
        bool has_dock = false;
        bool free_dock = false;
        for (const BuildingClass* building : Buildings) {
            if (!Usable_Owned_Building(building, house)) continue;
            if (needs_dock && accepts_docking(building->Class)) {
                has_dock = true;
                const auto* ext = Extension::Fetch(building);
                // Treat an off-line or unfinished dock as future capacity.
                free_dock |= !building->IsOn || !building->Is_Powered_On() || building->EMPFramesRemaining > 0 ||
                    building->Mission == MISSION_CONSTRUCTION || building->MissionQueue == MISSION_CONSTRUCTION ||
                    (ext->Is_Aircraft_MultiDock() ? ext->Find_First_Free_Aircraft_Dock_Slot() >= 0 :
                        (!building->Radio || building->Radio->RTTI != RTTI_AIRCRAFT));
            }
            if (building->Class->ToBuild != product->RTTI ||
                (!infantry && !aircraft && Extension::Fetch(building->Class)->IsNaval)) continue;
            ++owned;
            // Include off-line and building-up factories to avoid duplicating capacity.
            compatible |= permits(building->Class, product);
        }

        const bool product_ready = prerequisites(product);
        const bool dock_shortage = compatible && needs_dock && !free_dock && product_ready;
        const char* capacity = dock_shortage ? (has_dock ? "aircraft-docks-full" : "aircraft-dock-missing") :
            (infantry ? "missing-infantry-capacity" : (aircraft ? "missing-aircraft-capacity" : "missing-vehicle-capacity"));
        std::vector<const BuildingTypeClass*> blocked_factories;
        need = compatible && !dock_shortage ? "compatible-factory-present" : "capacity-limit-reached";
        if ((!compatible || dock_shortage) && owned < limit) {
            need = "no-configured-compatible-infrastructure";
            const std::array<const TypeList<BuildingTypeClass*>*, 3> lists{&types, &Extension::Fetch(product)->BuiltAt, &product->Dock};
            for (const auto* list : lists) {
                for (const BuildingTypeClass* type : *list) {
                    if (!safe_infrastructure(type) || !permits(type, product)) continue;
                    if (dock_shortage && !accepts_docking(type)) continue;
                    if (!type->CanAIBuildThis || !permitted_type(type)) {
                        reject(type, "factory-type-restricted");
                        continue;
                    }
                    // The proposed factory may itself be a prerequisite of the product.
                    if (!prerequisites(product, type) || !prerequisites(type)) {
                        blocked_factories.push_back(type);
                    } else if (const auto* choice = funded_choice(type)) {
                        DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} need={} unit={} native-order={} candidate={} order={}\n",
                            static_cast<int>(house->HeapID), Frame, capacity, product->Name(), native_need, type->Name(), choice->Name());
                        need = capacity;
                        return choice;
                    }
                }
            }
        }

        if (((!product_ready && compatible) || (!compatible && !blocked_factories.empty())) &&
            Has_Advanced_AI_Technology_Economy()) {
            need = "no-direct-technology-unlock";
            for (const auto* list : {&Rule->BuildRadar, &Rule->BuildTech}) {
                for (const BuildingTypeClass* type : *list) {
                    if (!safe_infrastructure(type) || std::any_of(Buildings.begin(), Buildings.end(), [&](const BuildingClass* building) {
                        return Usable_Owned_Building(building, house) && building->Class == type;
                    })) continue;
                    const bool unlocks = compatible ? prerequisites(product, type) :
                        std::any_of(blocked_factories.begin(), blocked_factories.end(), [&](const BuildingTypeClass* factory) {
                            return prerequisites(factory, type) && prerequisites(product, factory, type);
                        });
                    if (unlocks) {
                        if (const auto* choice = funded_choice(type)) {
                            DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} need=technology unit={} native-order={} candidate={} order={}\n",
                                static_cast<int>(house->HeapID), Frame, product->Name(), native_need, type->Name(), choice->Name());
                            need = "missing-technology-prerequisite";
                            return choice;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

bool HouseClassExtension::Has_Advanced_AI_Technology_Economy() const
{
    return Operating_Refineries_With_Harvesters(This_Const()) > 0;
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Technology(const char*& reason, long long& minimum,
    AdvancedAIPowerGoal& goal)
{
    HouseClass* house = This();
    const long long funds = Advanced_AI_Uncommitted_Funds();
    const long long reserve = Advanced_AI_Refinery_Reserve();
    const long long surplus = static_cast<long long>(house->Power) - house->Drain - house->PowerSurplus;

    struct TechnologyCandidate
    {
        const BuildingTypeClass* Type = nullptr;
        bool Available = false;
        int Owned = 0;
        int InBuildup = 0;
        int Priority = 5;
        int Unlocks = 0;
        int Dependencies = 0;
        const char* Role = "prerequisite";
        std::optional<int> Missing;
    };
    const auto trace = [&](const TechnologyCandidate* candidate, const BuildingTypeClass* order, long long required) {
#ifdef NDEBUG
        if (!Vinifera_DeveloperMode) return;
#endif
        const int id = static_cast<int>(house->HeapID);
        const int type = candidate ? static_cast<int>(candidate->Type->HeapID) : -1;
        const int chosen = order ? static_cast<int>(order->HeapID) : -1;
        const int missing = candidate ? candidate->Missing.value_or(std::numeric_limits<int>::max()) : std::numeric_limits<int>::max();
        const int unlocks = candidate ? candidate->Unlocks : 0;
        const int dependencies = candidate ? candidate->Dependencies : 0;
        const TechnologyTrace state{reason, type, chosen, missing, unlocks, dependencies, candidate ? candidate->InBuildup : 0};
        const auto previous = TechnologyTraces.find(id);
        if (previous != TechnologyTraces.end() && previous->second == state) return;
        TechnologyTraces.insert_or_assign(id, state);
        const char* missing_name = "none";
        if (candidate && candidate->Missing) {
            missing_name = missing >= STRUCT_FIRST ? BuildingTypes[missing]->Name() :
                PrerequisiteGroups[PrerequisiteGroupClass::Decode(missing)]->Name();
        }
        DEV_DEBUG_INFO("AF-0009C3a: house={} frame={} role={} candidate={} priority={} missing={} unlocks={} dependencies={} "
            "owned={} buildup={} uncommitted={} reserve={} minimum={} power-surplus={} decision={} order={}\n",
            id, Frame, candidate ? candidate->Role : "none", candidate ? candidate->Type->Name() : "none",
            candidate ? candidate->Priority : -1, missing_name, unlocks, dependencies, candidate ? candidate->Owned : 0,
            candidate ? candidate->InBuildup : 0, funds, reserve, required, surplus, reason, order ? order->Name() : "none");
    };

    if (funds <= reserve || surplus < 0 || !Has_Advanced_AI_Technology_Economy()) {
        reason = funds <= reserve ? "tech-blocked-credits" : surplus < 0 ? "tech-blocked-power" : "tech-economy-recovery";
        // AF-0009U: Let the credits reach the refinery reserve before units spend them.
        if (funds <= reserve) Request_Advanced_AI_Savings(reserve + 1, "technology-refinery-reserve", false);
        trace(nullptr, nullptr, 0);
        return nullptr;
    }

    std::vector<TechnologyCandidate> candidates(BuildingTypes.Count());
    for (const BuildingTypeClass* type : BuildingTypes) {
        if (!Valid_Building_Type(type)) continue;
        auto& candidate = candidates[type->HeapID];
        candidate.Type = type;
        candidate.Available = !type->IsInvisibleInGame && Can_Advanced_AI_Build_Type(type, true, false);
    }
    for (const BuildingClass* building : Buildings) {
        if (!Usable_Owned_Building(building, house) || !Valid_Building_Type(building->Class)) continue;
        auto& candidate = candidates[building->Class->HeapID];
        ++candidate.Owned;
        candidate.InBuildup += building->Mission == MISSION_CONSTRUCTION || building->MissionQueue == MISSION_CONSTRUCTION;
        for (const BuildingTypeClass* upgrade : building->Upgrades) {
            if (Valid_Building_Type(upgrade)) ++candidates[upgrade->HeapID].Owned;
        }
    }

    // Cache native satisfaction only for types referenced by a prerequisite.
    std::vector<std::optional<bool>> satisfied(candidates.size());
    const auto has_prerequisite = [&](int id) {
        auto& known = satisfied[id];
        if (!known) known = Has_Prerequisite(static_cast<StructType>(id));
        return *known;
    };
    const auto valid_id = [&](int id) {
        return id >= STRUCT_FIRST && id < BuildingTypes.Count() && candidates[id].Type;
    };
    const auto score_target = [&](const TechnoTypeClass* target, TechnologyCandidate* building) {
        std::map<int, int> hits;
        int missing_count = 0;
        const auto add_clause = [&](std::vector<int>& options, int prerequisite) {
            if (options.empty() || std::any_of(options.begin(), options.end(), [&](int id) { return !valid_id(id); })) return false;
            if (std::any_of(options.begin(), options.end(), has_prerequisite)) return true;
            if (building && !building->Missing) building->Missing = prerequisite;
            ++missing_count;
            std::sort(options.begin(), options.end());
            options.erase(std::unique(options.begin(), options.end()), options.end());
            bool achievable = false;
            for (int id : options) {
                const auto& option = candidates[id];
                if (option.Available || option.Owned) {
                    ++hits[id];
                    achievable = true;
                }
            }
            return achievable;
        };

        for (int prerequisite : target->Prerequisite) {
            std::vector<int> options;
            if (prerequisite >= STRUCT_FIRST) {
                options.push_back(prerequisite);
            } else {
                const auto group = PrerequisiteGroupClass::Decode(prerequisite);
                if (group < PREREQ_GROUP_FIRST || group >= PrerequisiteGroups.Count() || !PrerequisiteGroups[group]) return;
                for (int member : PrerequisiteGroups[group]->Prerequisites) options.push_back(member);
            }
            if (!add_clause(options, prerequisite)) return;
        }
        if (building && !building->Type->PowersUpBuilding.empty()) {
            const auto base = BuildingTypeClass::From_Name(building->Type->PowersUpBuilding.c_str());
            if (!valid_id(base)) return;
            std::vector<int> options{base};
            if (!add_clause(options, base)) return;
        }
        if (!building) {
            const auto& built_at = Extension::Fetch(target)->BuiltAt;
            if (built_at.Count()) {
                std::vector<int> options;
                for (const BuildingTypeClass* producer : built_at) {
                    if (Valid_Building_Type(producer) && producer->ToBuild == target->RTTI) options.push_back(producer->HeapID);
                }
                if (options.empty() || !add_clause(options, options.front())) return;
            }
        }
        for (const auto& [id, clauses] : hits) {
            auto& candidate = candidates[id];
            ++candidate.Dependencies;
            if (clauses == missing_count) ++candidate.Unlocks;
        }
    };

    for (auto& candidate : candidates) {
        if (candidate.Available && !candidate.Owned) score_target(candidate.Type, &candidate);
    }
    const unsigned side = static_cast<unsigned>(house->ActLike);
    const auto score_units = [&](const auto& types) {
        for (const TechnoTypeClass* type : types) {
            if (!type) continue;
            const int level = static_cast<int>(type->Level);
            if (level < 0 || level > house->Control.TechLevel ||
                !(static_cast<unsigned long>(type->Get_Ownable()) & (1UL << side)) || !Required_Forbidden_Houses_Check(type) ||
                house->Can_Build(type, false, false) <= 0 || house->Can_Build(type, true, true) <= 0) continue;
            score_target(type, nullptr);
        }
    };
    if (side < std::numeric_limits<unsigned long>::digits) {
        score_units(InfantryTypes);
        score_units(UnitTypes);
        score_units(AircraftTypes);
    }

    const auto listed = [](const auto& list, const BuildingTypeClass* type) {
        return std::find(list.begin(), list.end(), type) != list.end();
    };
    const auto radar = [&](const BuildingTypeClass* type) {
        return type->IsRadar || listed(Rule->BuildRadar, type);
    };
    std::vector<const TechnologyCandidate*> ranked;
    bool has_technology = false;
    for (auto& candidate : candidates) {
        const auto* type = candidate.Type;
        if ((!candidate.Available && !candidate.Owned) || type->IsConstructionYard || type->IsRefinery ||
            type->IsWall || type->IsGate) continue;
        const bool superweapon = type->SuperWeapon != SUPER_NONE || type->SuperWeapon2 != SUPER_NONE;
        if (!type->PowersUpBuilding.empty()) {
            if (type->Power > type->Drain && !superweapon) continue;
            // AF-0009U: Superweapon uplinks (Ion Cannon, Drop Pods) follow right after their upgrade center.
            candidate.Priority = superweapon ? 2 : 4;
            candidate.Role = superweapon ? "superweapon-upgrade" : "technology-upgrade";
        } else if (radar(type)) {
            candidate.Priority = 0;
            candidate.Role = "radar";
        } else if (listed(Rule->BuildTech, type)) {
            candidate.Priority = 1;
            candidate.Role = "tech-center";
        } else if (type->IsPlug || type->IsTemple) {
            candidate.Priority = 2;
            candidate.Role = type->IsPlug ? "upgrade-center" : "temple";
        } else if (candidate.Dependencies > 0 && (type->Power <= type->Drain || type->ToBuild != RTTI_NONE)) {
            candidate.Priority = 3;
            candidate.Role = type->ToBuild == RTTI_NONE ? "prerequisite" : "production-unlock";
        } else if (superweapon) {
            candidate.Priority = 4;
            candidate.Role = "superweapon";
        } else if (!type->IsInvisibleInGame && type->Power <= type->Drain) {
            candidate.Role = "remaining-ai-building";
        } else {
            continue;
        }
        has_technology = true;
        if (candidate.Available && !candidate.Owned) ranked.push_back(&candidate);
    }
    const auto rank = [](const TechnologyCandidate* candidate) {
        return std::make_tuple(candidate->Priority, -candidate->Unlocks, -candidate->Dependencies, candidate->Type->HeapID);
    };
    std::sort(ranked.begin(), ranked.end(), [&](const auto* a, const auto* b) { return rank(a) < rank(b); });

    const char* first_blocker = has_technology ? "tech-progression-complete" : "tech-no-candidate";
    long long first_minimum = 0;
    const auto expand_or_protect_base = [&]() -> const BuildingTypeClass* {
        if (const auto* order = Select_Advanced_AI_Capacity(reason, minimum, goal)) return order;
        return Select_Advanced_AI_Defense(reason, minimum, goal, false);
    };
    bool checked_infrastructure = false;
    for (const TechnologyCandidate* candidate : ranked) {
        if (!checked_infrastructure && candidate->Priority >= 3) {
            checked_infrastructure = true;
            if (const auto* order = expand_or_protect_base()) return order;
        }
        const auto* type = candidate->Type;
        const char* blocked = "infrastructure-not-buildable";
        long long required = 0;
        if (candidate->Missing || !Can_Advanced_AI_Build_Type(type, true)) {
            blocked = "tech-blocked-prerequisite";
        } else if (!Has_Advanced_AI_Refineries_For(type)) {
            blocked = "weapons-factory-needs-refinery";
        } else if (!Has_Advanced_AI_Ready_Yard(type, nullptr)) {
            blocked = "tech-blocked-yard";
        } else if (!type->PowersUpBuilding.empty() && !Find_Advanced_AI_Upgrade_Target(type)) {
            blocked = "tech-blocked-upgrade-slot";
        } else if (const auto* order = Fund_Optional_Building(*this, type, funds, reserve, required, blocked, false)) {
            minimum = required;
            reason = order == type ? "technology-progress" : "power-before-technology";
            if (order != type) goal = {type, true};
            trace(candidate, order, required);
            return order;
        }

        /**
         *  AF-0009U: Core technology (radar, tech center, upgrade center, superweapon
         *  uplinks) that only lacks credits is saved for. Cheaper lower-ranked buildings
         *  must not take the credits first, or the expensive steps never happen.
         */
        if (candidate->Priority <= 2 && std::string_view(blocked) == "infrastructure-budget-insufficient") {
            Request_Advanced_AI_Savings(required, "technology-savings", false);
            first_blocker = "tech-blocked-credits";
            first_minimum = required;
            break;
        }

        if (candidate == ranked.front()) {
            if (std::string_view(blocked) == "infrastructure-budget-insufficient") {
                blocked = "tech-blocked-credits";
                Request_Advanced_AI_Savings(required, "technology-savings", false);
            }
            if (std::string_view(blocked) == "infrastructure-power-or-cost-unavailable") blocked = "tech-blocked-power";
            first_blocker = blocked;
            first_minimum = required;
        }
    }
    if (!checked_infrastructure) {
        if (const auto* order = expand_or_protect_base()) return order;
    }
    reason = first_blocker;
    trace(ranked.empty() ? nullptr : ranked.front(), nullptr, first_minimum);
    return nullptr;
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Capacity(const char*& reason, long long& minimum,
    AdvancedAIPowerGoal& goal) const
{
    if (Rule->MultipleFactory <= 0.0 || Rule->MultipleFactory >= 1.0) return nullptr;
    const int refineries = Operating_Refineries_With_Harvesters(This_Const());
    if (refineries <= 0) return nullptr;
    const long long funds = Advanced_AI_Uncommitted_Funds();
    const long long reserve = Advanced_AI_Refinery_Reserve();
    struct Category {
        RTTIType Product;
        const TypeList<BuildingTypeClass*>& Types;
        int Limit;
        int RefineriesPerFactory;
    };
    const Category categories[] = {
        {RTTI_UNITTYPE, Rule->BuildWeapons, Rule->WarLimit, 1},
        {RTTI_INFANTRYTYPE, Rule->BuildBarracks, Rule->BarracksLimit, 4},
        {RTTI_AIRCRAFTTYPE, Rule->BuildHelipad, Rule->HelipadLimit, 2},
    };
    for (const auto& category : categories) {
        const int registered = Factory_Count(category.Product, PRODFLAG_NONE);
        const int supported = 1 + (refineries - 1) / category.RefineriesPerFactory;
        const int target = std::min(category.Limit, category.Product == RTTI_UNITTYPE ? supported : std::max(2, supported));
        if (registered <= 0 || registered >= target ||
            (RuleExtension->MultipleFactoryCap > 0 && registered >= RuleExtension->MultipleFactoryCap)) continue;

        int owned = 0;
        bool pending = false;
        for (const BuildingClass* building : Buildings) {
            if (!Usable_Owned_Building(building, This_Const()) || building->Class->ToBuild != category.Product ||
                Extension::Fetch(building->Class)->IsNaval) continue;
            ++owned;
            pending |= building->Mission == MISSION_CONSTRUCTION || building->MissionQueue == MISSION_CONSTRUCTION;
        }
        if (pending || owned >= target) continue;
        for (const BuildingTypeClass* type : category.Types) {
            if (!Valid_Building_Type(type) || type->ToBuild != category.Product ||
                Extension::Fetch(type)->IsNaval || Extension::Fetch(type)->IsExclusiveFactory ||
                !Has_Advanced_AI_Refineries_For(type)) continue;
            const char* blocked = nullptr;
            if (const auto* choice = Fund_Optional_Building(*this, type, funds, reserve, minimum, blocked)) {
                reason = choice == type ? "factory-speed-up" : "power-before-factory-speed-up";
                if (choice != type) goal.Type = type;
                return choice;
            }
        }
    }
    return nullptr;
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Preventive_Defense(AdvancedAIPowerGoal& goal,
    const BuildingTypeClass* required, bool urgent_only)
{
    const char* reason = nullptr;
    long long minimum = 0;
    return Select_Advanced_AI_Defense(reason, minimum, goal, true, false, nullptr, true, required, urgent_only);
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Defense(const char*& need, long long& minimum,
    AdvancedAIPowerGoal& goal, bool basic_only, bool emergency, const FactoryClass* replacing, bool preventive,
    const BuildingTypeClass* required, bool urgent_only) const
{
    const HouseClass* house = This_Const();
    auto* preventive_state = preventive ? &PreventiveDefenses[static_cast<int>(house->HeapID)] : nullptr;
    if (preventive_state) {
        auto& evaluated = urgent_only ? preventive_state->UrgentScan : preventive_state->EvaluatedScan;
        if (!required && evaluated == preventive_state->ScanFrame) return nullptr;
        // A free build slot may arrive between samples. Use current sight for the decision.
        Scan_Preventive_Defense(house, *preventive_state, true);
        evaluated = preventive_state->ScanFrame;
        if (!urgent_only) preventive_state->UrgentScan = preventive_state->ScanFrame;
        if (preventive_state->Groups.empty()) {
            Log_Preventive_Defense(house, "no-threat", "no-visible-combat-group");
            return nullptr;
        }
    }
    long long funds = Advanced_AI_Uncommitted_Funds();
    if (replacing) {
        // Match the native refund and remove the remaining payment commitment.
        const auto* product = replacing->Get_Object();
        funds += static_cast<long long>(product->TClass->Cost_Of(product->House)) - replacing->Balance +
            std::max(0, replacing->Balance);
    }
    const long long refinery_reserve = Advanced_AI_Refinery_Reserve();
    long long reserve = Threat_Defense_Reserve(funds, refinery_reserve, emergency || urgent_only);
    if (funds <= reserve && !preventive) return nullptr;

    int defenses = 0;
    int ground = 0;
    int anti_air = 0;
    int refineries = 0;
    int power_plants = 0;
    std::vector<const BuildingClass*> owned_defenses;
    for (const BuildingClass* building : Buildings) {
        if (!Usable_Owned_Building(building, house)) continue;
        const auto* type = building->Class;
        if (Is_Advanced_AI_Defense_Type(type)) {
            ++defenses;
            ground += Defense_Range(type, DefenseRole::Armor) > 0;
            anti_air += Defense_Range(type, DefenseRole::Air) > 0;
            owned_defenses.push_back(building);
        }
        if (!basic_only && Operational_Building(building)) {
            refineries += type->IsRefinery;
            power_plants += Matches_Role(type, InfrastructureRole::Power);
        }
    }
    if (defenses >= Rule->DefenseLimit && !preventive) return nullptr;

    const BuildingTypeClass* chosen_type = nullptr;
    Cell chosen_site(0, 0);
    AdvancedAIDefenseFocus chosen_focus;
    const char* rejected = nullptr;
    const auto choose = [&](DefenseRole role, const char* reason, const Cell* focus = nullptr,
        const PreventiveContact* contact = nullptr, bool require_contact_range = false, bool urgent = false) -> const BuildingTypeClass* {
        const BuildingTypeClass* best = nullptr;
        std::tuple<bool, long long, int, long long, int> best_rank{};
        long long best_value = 0;
        bool site_rejected = false;
        bool resources_rejected = false;
        /**
         *  AF-0009U: A defense type that was lost repeatedly within two minutes
         *  ranks behind types that held. A stronger type that only lacks credits
         *  becomes a savings goal instead of the next cheap loss.
         */
        const int house_id = static_cast<int>(house->HeapID);
        const BuildingTypeClass* upgrade = nullptr;
        long long upgrade_value = 0;
        long long upgrade_funds = 0;
        for (const BuildingTypeClass* type : BuildingTypes) {
            if ((required && type != required) || !Valid_Building_Type(type) || !Is_Advanced_AI_Defense_Type(type)) continue;
            const auto weapon = contact ? Defense_Weapon_Against(type, *contact) : DefenseWeapon{0, Defense_Range(type, role)};
            const int range = weapon.Range;
            if (range <= 0 || !Can_Advanced_AI_Build_Type(type, false) ||
                !Has_Advanced_AI_Ready_Yard(type, nullptr, replacing)) continue;
            if (focus) {
                const auto failed = ThreatDefensePlacementFailures.find({static_cast<int>(house->HeapID), type->HeapID});
                if (failed != ThreatDefensePlacementFailures.end() && Frame >= failed->second &&
                    Frame - failed->second < Preventive_Scan_Interval) continue;
            }
            const char* blocked = nullptr;
            long long required_funds = 0;
            // Rating only: the power trace follows for the chosen type below.
            ++PowerTraceMute;
            const auto* order = Fund_Optional_Building(*this, type, funds, reserve, required_funds, blocked, false, emergency || contact);
            --PowerTraceMute;
            const long long defense_value = std::max(1, Defense_Rating(type, role)) + 4LL * weapon.Strength;
            const bool held = Recent_Defense_Losses(house_id, type->HeapID) < DefenseLossLimit;
            if (!order) {
                resources_rejected |= std::string_view(blocked) != "placement-retry-cooldown";
                if (held && std::string_view(blocked) == "infrastructure-budget-insufficient" && defense_value > upgrade_value) {
                    upgrade = type;
                    upgrade_value = defense_value;
                    upgrade_funds = required_funds;
                }
                continue;
            }
            const long long cost = required_funds - reserve;
            const long long price_weight = 1 + cost * std::max(1LL, reserve) / std::max(1LL, funds - reserve);
            const auto rating = std::make_tuple(held, 1024LL * defense_value / price_weight,
                range, -cost, -static_cast<int>(type->HeapID));
            if (best && rating <= best_rank) continue;
            if (focus) {
                const AdvancedAIDefenseFocus at{*focus, contact ? contact->Position : *focus, range,
                    weapon.MinimumRange, role == DefenseRole::Air, contact && weapon.Direct, contact != nullptr,
                    require_contact_range, contact ? contact->ID : -1, static_cast<int>(role), urgent};
                const Cell site = Find_Advanced_AI_Defense_Site(BuildingTypes[type->HeapID], focus, range, &at);
                if (site == Cell(0, 0)) {
                    site_rejected = true;
                    continue;
                }
                chosen_site = site;
                chosen_focus = at;
            }
            best = order;
            chosen_type = type;
            best_rank = rating;
            best_value = defense_value;
            minimum = required_funds;
            goal = order == type ? AdvancedAIPowerGoal{} : AdvancedAIPowerGoal{type, false, false, emergency, preventive};
        }
        if (best && !std::get<0>(best_rank) && upgrade && upgrade_value > best_value) {
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} defense-upgrade-savings={} instead-of={} lost={} funds={}\n",
                house_id, Frame, upgrade->Name(), chosen_type->Name(),
                Recent_Defense_Losses(house_id, chosen_type->HeapID), upgrade_funds);
            Request_Advanced_AI_Savings(upgrade_funds, "defense-upgrade", true);
            best = nullptr;
            chosen_type = nullptr;
            goal = {};
            rejected = "saving-for-stronger-defense";
            return nullptr;
        }
        if (best) {
            need = reason;
            // The power plant is ordered for the chosen type only; log it now.
            if (best != chosen_type) Prioritize_Advanced_AI_Power(chosen_type);
        }
        rejected = site_rejected ? "no-legal-defense-site" : resources_rejected ? "insufficient-resources" : "no-buildable-defense";
        return best;
    };

    if (emergency) {
        auto& state = PreventiveDefenses[static_cast<int>(house->HeapID)];
        if (!required && state.LastEmergencyEvaluation >= 0 && Frame >= state.LastEmergencyEvaluation &&
            Frame - state.LastEmergencyEvaluation < Preventive_Scan_Interval) return nullptr;
        state.LastEmergencyEvaluation = Frame;
        const auto threats = Active_Building_Threats(house);
        const auto urgency = [&](DefenseRole role) {
            const auto& threat = threats[static_cast<size_t>(role)];
            return std::make_pair(threat.Damage, threat.Attackers);
        };
        auto roles = DefenseRoles;
        std::stable_sort(roles.begin(), roles.end(), [&](DefenseRole a, DefenseRole b) { return urgency(a) > urgency(b); });
        constexpr const char* response_reasons[]{"air-raid-defense", "infantry-raid-defense", "armor-raid-defense"};
        for (const auto role : roles) {
            const size_t index = static_cast<size_t>(role);
            const auto& threat = threats[index];
            if (!threat.Attackers || !Owns_Defense_Target(house, threat.Target)) continue;

            /**
             *  AF-0009U: A lone scout scratching a building does not justify new
             *  defense structures; units handle it. Respond to groups or to real damage.
             */
            static constexpr long long MinimumResponseDamage = 100;
            if (threat.Attackers < 3 && threat.Damage < MinimumResponseDamage) continue;

            Scan_Preventive_Defense(house, state, true);
            Cell defended = threat.Target;
            const auto* contact = Find_Visible_Attacker(state, threat.AttackerID, defended);
            const auto response_role = contact ? contact->Role : role;
            const Cell intercept = contact ? contact->Position : threat.Target;
            // AF-0009U: One covering defense per small raid; more for groups and heavy damage.
            const int desired = static_cast<int>(std::min(5LL, 1 + (threat.Attackers - 1) / 2 + threat.Damage / 800));
            int covering = 0;
            for (const auto* building : owned_defenses) {
                if (!Operational_Building(building) && building->Mission != MISSION_CONSTRUCTION &&
                    building->MissionQueue != MISSION_CONSTRUCTION) continue;
                const int range = Defense_Range(building->Class, response_role);
                const Cell site = building->Get_Cell();
                const Cell center(static_cast<short>(site.X + building->Class->Width() / 2),
                    static_cast<short>(site.Y + building->Class->Height() / 2));
                const long long distance = static_cast<long long>(center.Relative_Distance_To(intercept)) *
                    CELL_LEPTON_W * CELL_LEPTON_W;
                if (contact) {
                    const auto weapon = Defense_Weapon_Against(building->Class, *contact, distance,
                        response_role == DefenseRole::Air || Map_Defense_Line_Clear(center, intercept));
                    covering += weapon.Strength > 0;
                } else if (range > 0 && distance <= static_cast<long long>(range) * range) ++covering;
            }
            if (covering >= desired) continue;
            if (const auto* order = choose(response_role, response_reasons[static_cast<size_t>(response_role)], &defended,
                contact, contact != nullptr, true)) {
                state.Planned = chosen_type;
                state.Focus = chosen_focus;
                Trace_Advanced_AI_Building_Order("threat", need, order, nullptr, &defended, "B1");
                const int house_id = static_cast<int>(house->HeapID);
                const auto trace = std::make_tuple(static_cast<int>(role), order->HeapID,
                    defended.As_Cell_Number(), replacing != nullptr, threat.Damage > 0);
                const auto previous = DefenseResponseTraces.find(house_id);
                if (previous == DefenseResponseTraces.end() || previous->second != trace) {
                    DefenseResponseTraces.insert_or_assign(house_id, trace);
                    DEV_DEBUG_INFO("AF-0009: house={} frame={} defense-response={} attackers={} damage={} "
                        "coverage={}/{} target={},{} order={} preempt={}\n",
                        house_id, Frame, need, threat.Attackers, threat.Damage, covering, desired,
                        defended.X, defended.Y, order->Name(), replacing != nullptr);
                }
                return order;
            }
        }
        return nullptr;
    }

    if (preventive_state) {
        auto& state = *preventive_state;
        struct Threat
        {
            const PreventiveGroup* Group;
            const PreventiveContact* Contact;
            int Strength;
            int RoleStrength;
            bool Significant;
            bool Urgent;
        };
        std::vector<Threat> threats;
        for (const auto& group : state.Groups) {
            int strength = 0;
            int closing = 0;
            int within_range = 0;
            int seen = 0;
            long long damage = 0;
            for (const auto& contact : group.Contacts) {
                strength += contact.Strength;
                closing += contact.Closing;
                within_range += contact.InRange;
                seen += contact.ObservedBefore;
                damage += Recent_Attacker_Damage(house, contact.ID);
            }
            const bool urgent = (within_range >= 2 && strength >= 12) ||
                (group.Contacts.size() >= 3 && strength >= 18 && (closing > 0 || within_range > 0)) ||
                damage >= Urgent_Attacker_Damage;
            const bool significant = urgent || damage >= Significant_Attacker_Damage ||
                (group.Contacts.size() >= 2 && strength >= (group.Importance == 2 ? 12 : 15) &&
                (closing > 0 || (group.Contacts.size() >= 3 && seen >= 2)));
            for (const DefenseRole role : DefenseRoles) {
                int role_strength = 0;
                const PreventiveContact* best = nullptr;
                for (const auto& contact : group.Contacts) {
                    if (contact.Role != role) continue;
                    role_strength += contact.Strength;
                    if (!best || std::make_tuple(contact.InRange, contact.Closing, contact.Strength,
                        -contact.Position.Relative_Distance_To(group.Asset), -contact.Position.As_Cell_Number()) >
                        std::make_tuple(best->InRange, best->Closing, best->Strength,
                            -best->Position.Relative_Distance_To(group.Asset), -best->Position.As_Cell_Number())) best = &contact;
                }
                if (best && (!urgent_only || urgent)) threats.push_back({&group, best, strength, role_strength, significant, urgent});
            }
        }
        std::stable_sort(threats.begin(), threats.end(), [](const Threat& a, const Threat& b) {
            return std::make_tuple(a.Urgent, a.Significant, a.Group->Importance, a.RoleStrength, a.Strength) >
                std::make_tuple(b.Urgent, b.Significant, b.Group->Importance, b.RoleStrength, b.Strength);
        });
        const Threat* first = nullptr;
        const char* decision = "threat-below-threshold";
        const char* detail = "group-size-strength-or-approach";
        int first_coverage = 0;
        int first_desired = 0;
        int searches = 0;
        for (const auto& threat : threats) {
            const auto& contact = *threat.Contact;
            const Cell asset = threat.Group->Asset;
            if (required && (asset.Relative_Distance_To(state.Focus.Asset) > 20 * 20 || (contact.Role == DefenseRole::Air) != state.Focus.Air ||
                Defense_Weapon_Against(required, contact).Strength <= 0)) continue;
            if (!threat.Significant) {
                if (!first) first = &threat;
                continue;
            }
            const Cell intercept = contact.InRange ? contact.Position : AdvancedAIDefenseFocus{asset, contact.Position}.Intercept();
            int coverage = 0;
            for (const auto* building : owned_defenses) {
                const bool building_up = building->Mission == MISSION_CONSTRUCTION || building->MissionQueue == MISSION_CONSTRUCTION;
                if (!Operational_Building(building) && !building_up) continue;
                const Cell center = Building_Center_Cell(building);
                const int range = Defense_Range(building->Class, contact.Role);
                if (range <= 0 || !Within_Defense_Range(center, intercept, range)) continue;
                const long long distance = static_cast<long long>(center.Relative_Distance_To(intercept)) * CELL_LEPTON_W * CELL_LEPTON_W;
                const auto weapon = Defense_Weapon_Against(building->Class, contact, distance,
                    contact.Role == DefenseRole::Air || Map_Defense_Line_Clear(center, intercept));
                coverage += std::min(80, weapon.Strength);
            }
            const int desired = std::clamp(12 + threat.RoleStrength / 2 + 3 * threat.Group->Importance, 20, 100);

            /**
             *  AF-0009U: Flying enemies near an asset without any anti-aircraft coverage are
             *  urgent: no unit or ground defense can stop them (for example jumpjets).
             */
            const bool uncovered_air = contact.Role == DefenseRole::Air && coverage == 0;
            const bool urgent = threat.Urgent || uncovered_air;
            reserve = Threat_Defense_Reserve(funds, refinery_reserve, urgent);
            const char* block = nullptr;
            if (coverage >= desired) block = "defense-already-sufficient";
            else if (!required && state.LastOrder >= 0 && Frame >= state.LastOrder && Frame - state.LastOrder < Preventive_Cooldown &&
                (!threat.Urgent || state.LastOrderAsset.Relative_Distance_To(asset) <= 4 * 4)) {
                block = "defense-order-cooldown";
            } else if (defenses >= Rule->DefenseLimit) block = "defense-limit";
            else if (funds <= reserve) block = "insufficient-resources";
            else if (searches++ >= 3) break;
            else if (const auto* order = choose(contact.Role, "preventive-base-defense", &asset, &contact, contact.InRange, urgent)) {
                state.Planned = chosen_type;
                state.Focus = chosen_focus;
                state.LastOrder = Frame;
                state.LastOrderAsset = asset;
                Log_Preventive_Defense(house, "defense-requested", order != chosen_type ? "power-before-defense" :
                    threat.Urgent ? "urgent-visible-group" : "visible-group",
                    threat.Group, &contact, threat.Strength, coverage, desired, chosen_type, chosen_site, order);
                return order;
            } else block = rejected;

            /**
             *  AF-0009U: An uncovered air threat that only lacks credits becomes an
             *  economy savings goal, so unit starts leave the anti-aircraft defense funded.
             */
            if (uncovered_air && std::string_view(block) == "insufficient-resources") {
                long long cheapest = 0;
                for (const BuildingTypeClass* type : BuildingTypes) {
                    if (!Valid_Building_Type(type) || !Is_Advanced_AI_Defense_Type(type) || Defense_Range(type, DefenseRole::Air) <= 0 ||
                        !Can_Advanced_AI_Build_Type(type, false)) continue;
                    const long long cost = type->Cost_Of(const_cast<HouseClass*>(house));
                    if (cost > 0 && (cheapest == 0 || cost < cheapest)) cheapest = cost;
                }
                if (cheapest > 0) {
                    Request_Advanced_AI_Savings(cheapest + Threat_Defense_Reserve(cheapest, refinery_reserve, true), "air-defense", true);
                }
            }

            if (!first) {
                first = &threat;
                decision = std::string_view(block) == "defense-order-cooldown" ? "defense-already-planned" : block;
                detail = block;
                first_coverage = coverage;
                first_desired = desired;
            }
        }
        if (first) Log_Preventive_Defense(house, decision, detail, first->Group, first->Contact, first->Strength, first_coverage, first_desired);
        else if (!urgent_only) Log_Preventive_Defense(house, "no-threat", "planned-sector-no-longer-threatened");
        return nullptr;
    }

    if (!basic_only && ground > 0 && refineries > 0 && Rule->BuildPDefense.Count() > 0) {
        const int heavy_goal = std::clamp(refineries / 2, 1, 3);
        const auto heavy_count = std::count_if(owned_defenses.begin(), owned_defenses.end(), [](const auto* building) {
            return Rule->BuildPDefense.Is_Present(building->Class) && Defense_Range(building->Class, DefenseRole::Armor) > 0;
        });
        if (heavy_count < heavy_goal) {
            for (const BuildingTypeClass* type : Rule->BuildPDefense) {
                if (!Is_Advanced_AI_Defense_Type(type) || Defense_Range(type, DefenseRole::Armor) <= 0 ||
                    !Can_Advanced_AI_Build_Building(type, nullptr, false)) continue;
                const char* blocked = nullptr;
                long long required = 0;
                if (const auto* order = Fund_Optional_Building(*this, type, funds, reserve, required, blocked, false)) {
                    minimum = required;
                    need = order == type ? "heavy-ground-defense" : "power-before-heavy-ground-defense";
                    if (order != type) goal.Type = type;
                    return order;
                }
            }
        }
    }

    const int budget_bonus = basic_only ? 0 : static_cast<int>(std::min(3LL, (funds - reserve) / std::max(1LL, 2 * reserve)));

    /**
     *  AF-0009U: Credits above AdvancedAIRichFunds that unit production cannot
     *  spend (factories at their limit) raise the defense goals: one more ground
     *  defense per 2000 credits (at most 10) and half as many anti-aircraft
     *  defenses. AdvancedAIDefenseSpread places them at uncovered buildings.
     */
    const long long rich_funds = RuleExtension->AdvancedAIRichFunds > 0 ? funds - RuleExtension->AdvancedAIRichFunds : 0;
    const int rich_bonus = basic_only || rich_funds <= 0 ? 0 : static_cast<int>(std::min(10LL, rich_funds / 2000));
    const int ground_goal = 1 + refineries / 2 + power_plants / 4 + budget_bonus + rich_bonus;
    if (ground < ground_goal) {
        if (const auto* order = choose(ground == 0 ? DefenseRole::Infantry : DefenseRole::Armor,
            ground == 0 ? "basic-ground-defense" : rich_bonus > 0 && ground >= ground_goal - rich_bonus ?
            "rich-ground-defense" : "additional-ground-defense")) return order;
    }
    if (anti_air < std::max(1, refineries / 2) + rich_bonus / 2) {
        return choose(DefenseRole::Air, anti_air == 0 ? "basic-air-defense" : "additional-air-defense");
    }
    return nullptr;
}
