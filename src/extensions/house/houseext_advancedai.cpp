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

#include "building.h"
#include "buildingtypeext.h"
#include "cell.h"
#include "ccini.h"
#include "debughandler.h"
#include "factory.h"
#include "mouse.h"
#include "prerequisitegroup.h"
#include "rules.h"
#include "rulesext.h"
#include "side.h"
#include "sideext.h"
#include "technotypeext.h"
#include "tibsun_globals.h"
#include "vinifera_globals.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace
{
    // Log deduplication only; never saved or used for AI decisions.
    std::set<std::tuple<int, int, std::string, int>> AdvancedAIErrors;
    std::set<std::tuple<int, int, std::string>> AdvancedAIPowerSelections;
    // Per candidate: -2 = affordable, -1 = no plant, otherwise the selected type.
    std::map<std::pair<int, int>, int> AdvancedAIPowerDiagnostics;

    bool Valid_Building_Type(const BuildingTypeClass* type)
    {
        if (!type || type->RTTI != RTTI_BUILDINGTYPE) return false;
        const int id = type->HeapID;
        return id >= STRUCT_FIRST && id < BuildingTypes.Count() && BuildingTypes[id] == type;
    }

    bool Usable_Owned_Building(const BuildingClass* building, const HouseClass* house)
    {
        return building && building->IsActive && building->IsDown && !building->IsInLimbo &&
            building->Strength > 0 && building->House == house &&
            building->Mission != MISSION_DECONSTRUCTION && building->MissionQueue != MISSION_DECONSTRUCTION;
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

    void Trace_Power_Choice(const HouseClass* house, const BuildingTypeClass* desired,
        const BuildingTypeClass* plant, bool deficit, long long additional, long long gain)
    {
#ifdef NDEBUG
        if (!Vinifera_DeveloperMode) return;
#endif
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

void HouseClassExtension::Clear_Advanced_AI_Log_State()
{
    AdvancedAIErrors.clear();
    AdvancedAIPowerSelections.clear();
    AdvancedAIPowerDiagnostics.clear();
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

bool HouseClassExtension::Can_Advanced_AI_Build_Building(const BuildingTypeClass* type, const BuildingClass* factory,
    bool check_ai_build_this) const
{
    const auto invalid = [this, type](const char* reason, int detail = 0) {
        Log_Advanced_AI_Error(reason, type, detail);
        return false;
    };
    if (!Valid_Building_Type(type)) return invalid("invalid-type");
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
            if (prerequisite >= BuildingTypes.Count()) return invalid("invalid-prerequisite", prerequisite);
        } else {
            const int group = -1 - prerequisite;
            if (group < PREREQ_GROUP_FIRST || group >= PrerequisiteGroups.Count() || !PrerequisiteGroups[group]) return invalid("invalid-prerequisite-group", prerequisite);
            for (int member : PrerequisiteGroups[group]->Prerequisites) {
                if (member < STRUCT_FIRST || member >= BuildingTypes.Count()) return invalid("invalid-group-member", member);
            }
        }
        if (!self->Has_Prerequisite(prerequisite)) return false;
    }

    // Can_Build(false, ...) enforces native buildability/tech restrictions, but
    // AI has an early success path. The forced query reaches native BuildLimit
    // handling (including nonpositive lifetime limits); never treat -1 as true.
    const int buildable = house->Can_Build(type, false, false);
    if (buildable <= 0) return false;
    const int limit = house->Can_Build(type, true, false);
    if (limit <= 0) return false;

    // Existing factory search enforces BuiltAt, ExclusiveFactory and yard
    // ActLike/MultiMCV. It performs no placement search and allocates no objects.
    if (!type->Who_Can_Build_Me(true, true, false, house)) {
        return false;
    }
    if (factory) {
        if (!Usable_Owned_Building(factory, house)) return false;
        if (!factory->IsOn) return false;
        if (factory->Mission == MISSION_CONSTRUCTION || factory->MissionQueue == MISSION_CONSTRUCTION) return false;
        if (factory->Class->ToBuild != RTTI_BUILDINGTYPE) return false;
        if (!(factory->Class->Get_Ownable() & type->Get_Ownable())) return false;
        if (Rule->BuildConst.Is_Present(factory->Class) && !RuleExtension->IsMultiMCV) {
            const unsigned factory_actlike = static_cast<unsigned>(factory->ActLike);
            if (factory_actlike >= std::numeric_limits<unsigned long>::digits ||
                !(static_cast<unsigned long>(type->Get_Ownable()) & (1UL << factory_actlike))) return false;
        }
        const auto typeext = Extension::Fetch(type);
        if (!typeext->BuiltAt.Is_Present(factory->Class) &&
            (typeext->BuiltAt.Count() || Extension::Fetch(factory->Class)->IsExclusiveFactory)) return false;
    }

    // Use the same native target/slot decisions as upgrade placement.
    if (!type->PowersUpBuilding.empty()) {
        if (!Find_Advanced_AI_Upgrade_Target(type)) return false;
    }
    return true;
}

bool HouseClassExtension::Has_Advanced_AI_Building_Production() const
{
    for (const FactoryClass* factory : Factories) {
        const TechnoClass* product = factory ? factory->Get_Object() : nullptr;
        if (factory && factory->House == This_Const() && product && product->RTTI == RTTI_BUILDING) {
            return true;
        }
    }
    return false;
}

const BuildingClass* HouseClassExtension::Find_Advanced_AI_Upgrade_Target(const BuildingTypeClass* upgrade) const
{
    if (!Valid_Building_Type(upgrade) || upgrade->PowersUpBuilding.empty()) return nullptr;
    const StructType target_type = BuildingTypeClass::From_Name(upgrade->PowersUpBuilding.c_str());
    if (target_type < STRUCT_FIRST || target_type >= BuildingTypes.Count()) return nullptr;

    // Exit_Object honors a positioned base node before the generic upgrade search.
    const BaseNodeClass* node = This()->Base.Next_Buildable(upgrade->HeapID);
    Cell cell = node && node->CellID != Cell(0, 0) ? node->CellID : This_Const()->Find_Upgrade_Location(upgrade);
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
        if (!Can_Advanced_AI_Build_Building(type, factory, false)) {
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
    return nullptr;
}

const BuildingTypeClass* HouseClassExtension::Prioritize_Advanced_AI_Power(const BuildingTypeClass* desired,
    const BuildingClass* factory) const
{
    if (!Valid_Building_Type(desired)) return nullptr;
    long long desired_gain;
    if (!Get_Advanced_AI_Power_Gain(desired, desired_gain)) return nullptr;
    // A planned power turbine can lose its useful output before production starts.
    const int side = static_cast<int>(This_Const()->Class->Side);
    if (!desired->PowersUpBuilding.empty() && desired->Power > desired->Drain && desired_gain <= 0 &&
        side >= 0 && side < Sides.Count() && Sides[side] &&
        desired == Extension::Fetch(Sides[side])->PowerTurbine) {
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

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Building() const
{
    const HouseClass* house = This_Const();
    const auto first_buildable = [this](const TypeList<BuildingTypeClass*>& list, InfrastructureRole role) {
        for (const BuildingTypeClass* type : list) {
            if (!Matches_Role(type, role)) {
                continue;
            }
            if (Can_Advanced_AI_Build_Building(type)) {
                if (const BuildingTypeClass* choice = Prioritize_Advanced_AI_Power(type)) return choice;
            }
        }
        return static_cast<const BuildingTypeClass*>(nullptr);
    };
    const auto has_role = [house](const TypeList<BuildingTypeClass*>& list, InfrastructureRole role) {
        for (const BuildingClass* building : Buildings) {
            if (Usable_Owned_Building(building, house) && list.Is_Present(building->Class) && Matches_Role(building->Class, role)) return true;
        }
        return false;
    };

    const BuildingTypeClass* choice = nullptr;
    const char* role = "power";
    if (house->Power <= 0 || static_cast<long long>(house->Power) < static_cast<long long>(house->Drain) + house->PowerSurplus) {
        choice = Select_Advanced_AI_Power_Plant();
    }
    if (!choice && !has_role(Rule->BuildRefinery, InfrastructureRole::Refinery)) {
        choice = first_buildable(Rule->BuildRefinery, InfrastructureRole::Refinery);
        role = "refinery";
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
