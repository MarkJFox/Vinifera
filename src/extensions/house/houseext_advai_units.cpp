/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  AF-0009U: DTA-style Advanced AI unit production and tactics.
 *
 *          Ported from the Dawn of the Tiberium Age Vinifera build
 *          (Rampastring, branch dta-advaiv2-spawner, commit f5ae74ae) and
 *          adapted for Tiberian Sun: Aftermath:
 *
 *          - Active only with [AI] UseAdvancedAI=yes, AdvancedAIUnitProduction=yes,
 *            for computer houses outside of campaign missions.
 *          - Only teams created by Advanced AI tactics are managed; scripted
 *            and trigger created teams keep their native behaviour.
 *          - The Aftermath base planner (AF-0009B/C) and the unit start
 *            budget (AF-0009 unit production stage A) stay in charge of
 *            buildings and funding.
 *          - No local-player state (radar, shroud) is read, so all clients
 *            of a multiplayer game make identical decisions.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "houseext.h"

#include "aircraft.h"
#include "aircrafttype.h"
#include "advaitactictype.h"
#include "building.h"
#include "buildingext.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "debughandler.h"
#include "extension.h"
#include "factory.h"
#include "foot.h"
#include "house.h"
#include "housetype.h"
#include "infantry.h"
#include "infantrytype.h"
#include "infantrytypeext.h"
#include "aircrafttypeext.h"
#include "iomap.h"
#include "rules.h"
#include "rulesext.h"
#include "session.h"
#include "taskforce.h"
#include "team.h"
#include "teamext.h"
#include "teamtype.h"
#include "techno.h"
#include "technotype.h"
#include "technotypeext.h"
#include "technoext.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "unit.h"
#include "unittype.h"
#include "unittypeext.h"
#include "vinifera_globals.h"
#include "weapontype.h"

#include <algorithm>
#include <climits>
#include <vector>


void EnemyStrengthStruct::Add_Techno_Type(const TechnoTypeClassExtension* technotypeext)
{
    None += technotypeext->AntiNoneArmorValue();
    Light += technotypeext->AntiLightArmorValue();
    Heavy += technotypeext->AntiHeavyArmorValue();
}


void EnemyStrengthStruct::Add_Techno_Type_Half_Weight(const TechnoTypeClassExtension* technotypeext)
{
    None += technotypeext->AntiNoneArmorValue() / 2;
    Light += technotypeext->AntiLightArmorValue() / 2;
    Heavy += technotypeext->AntiHeavyArmorValue() / 2;
}


void EnemyStrengthStruct::Add_Techno_Type_AntiAir(const TechnoTypeClassExtension* technotypeext)
{
    None += technotypeext->AntiNoneArmorValueAA();
    Light += technotypeext->AntiLightArmorValueAA();
    Heavy += technotypeext->AntiHeavyArmorValueAA();
}


namespace
{
    /**
     *  Developer log throttle: print the evaluated values at most every 10000 frames.
     */
    bool Debug_Print_Due(int& lastframe)
    {
        if (!Vinifera_DeveloperMode) return false;
        if (Frame <= lastframe + 10000) return false;
        lastframe = Frame;
        return true;
    }


    bool Is_Usable(const TechnoClass* techno)
    {
        return techno != nullptr && techno->IsActive && !techno->IsInLimbo && techno->IsDown;
    }
}


/**
 *  Is the DTA-style tactic based unit production in charge of this house?
 */
bool HouseClassExtension::Is_Advanced_AI_Unit_Production() const
{
    if (!RuleExtension || !RuleExtension->IsAdvancedAIUnitProduction) return false;
    if (Session.Type == GAME_NORMAL) return false;
    if (IsObserver || !Is_Advanced_AI_Enabled()) return false;

    const HouseClass* house = This_Const();
    return house->Class != nullptr && !house->Class->IsMultiplayPassive && !house->IsDefeated;
}


/**
 *  Iterates the Advanced AI teams of this house that are still gathering units.
 *  Pass id = 0 to start; returns nullptr when no further team matches.
 *
 *  @author: Rampastring
 */
TeamClass* HouseClassExtension::Get_Team_In_Production(int& id, RTTIType for_type, ProductionFlags prodflags) const
{
    if (id < 0) {
        return nullptr;
    }

    for (int i = id; i < Teams.Count(); i++) {
        TeamClass* team = Teams[i];

        if (team == nullptr || team->House != This_Const() || team->IsHasBeen) {
            continue;
        }

        const TeamClassExtension* teamext = Extension::Fetch(team);

        /**
         *  Aftermath: never touch scripted or trigger created teams.
         */
        if (!teamext->Is_AdvAI_Managed() || teamext->ProdFlags != prodflags) {
            continue;
        }

        if (for_type == RTTI_INFANTRY) {
            if (teamext->NoInfantry) continue;
            if (teamext->MaxInfantry > -1 && teamext->Infantry_Count() >= teamext->MaxInfantry) continue;
        }

        if (for_type == RTTI_UNIT) {
            if (teamext->NoVehicles) continue;
            if (teamext->IsTransportTeam && teamext->Contains_Transport()) continue;
        }

        if (for_type == RTTI_AIRCRAFT && teamext->NoAircraft) {
            continue;
        }

        id = i + 1;
        return team;
    }

    return nullptr;
}


void HouseClassExtension::Fill_Owned_Buildings_List(DynamicVectorClass<BuildingTypeClass*>& owned) const
{
    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* building = Buildings[i];
        if (building->House == This_Const() && !building->IsInLimbo && building->IsDown) {
            owned.Add(building->Class);
        }
    }
}


/**
 *  Can this house start producing the given type right now?
 */
bool HouseClassExtension::AdvAI_Can_Produce(const TechnoTypeClass* type, DynamicVectorClass<BuildingTypeClass*>& owned) const
{
    HouseClass* house = This();

    if (type == nullptr || house->Control.TechLevel < 0 || type->Level > (unsigned)house->Control.TechLevel) {
        return false;
    }

    if (house->ActLike == HOUSE_NONE || (type->Ownable & (1 << house->ActLike)) == 0) {
        return false;
    }

    if (!house->Can_Build(type, false, true)) {
        return false;
    }

    return house->Has_Prerequisites(type, owned, owned.Count());
}


/**
 *  AF-0009U: Is this a gathering team of a fixed TeamType started by a tactic?
 */
static bool Is_Gathering_Scripted_Team(const TeamClass* team, const HouseClass* house)
{
    return team != nullptr && team->House == house && !team->IsHasBeen && !team->IsMoving &&
        team->Class != nullptr && team->Class->TaskForce != nullptr && Extension::Fetch(team)->IsAdvAIScriptedTeam;
}


/**
 *  AF-0009U: Returns a member type of the given kind (RTTI_UNITTYPE,
 *  RTTI_INFANTRYTYPE or RTTI_AIRCRAFTTYPE) that a gathering scripted team
 *  still misses and that is neither free on the map nor in production.
 *  Free units without a team (objects in a factory are such units) cover the
 *  demand, because the team recruits them.
 */
const TechnoTypeClass* HouseClassExtension::AdvAI_Scripted_Team_Need(RTTIType typertti, bool naval,
    DynamicVectorClass<BuildingTypeClass*>& owned) const
{
    const HouseClass* house = This_Const();

    struct Demand { const TechnoTypeClass* Type; int Count; };
    std::vector<Demand> demand;

    for (int i = 0; i < Teams.Count(); i++) {
        const TeamClass* team = Teams[i];
        if (!Is_Gathering_Scripted_Team(team, house)) continue;

        const TaskForceClass* taskforce = team->Class->TaskForce;
        for (int j = 0; j < taskforce->ClassCount && j < TaskForceClass::MAX_TEAM_CLASSCOUNT; j++) {
            const TechnoTypeClass* type = taskforce->Members[j].Class;
            if (type == nullptr || type->RTTI != typertti) continue;
            if (typertti == RTTI_UNITTYPE && Extension::Fetch(type)->IsNaval != naval) continue;

            const int missing = taskforce->Members[j].Quantity - team->Quantity[j];
            if (missing <= 0) continue;

            auto found = std::find_if(demand.begin(), demand.end(), [&](const Demand& d) { return d.Type == type; });
            if (found != demand.end()) {
                found->Count += missing;
            } else {
                demand.push_back({ type, missing });
            }
        }
    }

    for (const Demand& d : demand) {
        int supply = 0;
        for (int i = 0; i < Foots.Count(); i++) {
            const FootClass* foot = Foots[i];
            if (foot->IsActive && foot->House == house && foot->Team == nullptr && foot->TClass == d.Type) supply++;
        }
        if (supply >= d.Count) continue;

        if (!AdvAI_Can_Produce(d.Type, owned)) continue;

        if (d.Type->BuildLimit > 0) {
            int quantity = 0;
            if (typertti == RTTI_UNITTYPE) quantity = house->UQuantity.Value(static_cast<const UnitTypeClass*>(d.Type)->HeapID);
            else if (typertti == RTTI_INFANTRYTYPE) quantity = house->IQuantity.Value(static_cast<const InfantryTypeClass*>(d.Type)->HeapID);
            else if (typertti == RTTI_AIRCRAFTTYPE) quantity = house->AQuantity.Value(static_cast<const AircraftTypeClass*>(d.Type)->HeapID);
            if (quantity >= d.Type->BuildLimit) continue;
        }

        if (typertti == RTTI_AIRCRAFTTYPE) {
            const AircraftTypeClass* airtype = static_cast<const AircraftTypeClass*>(d.Type);
            if (Advanced_AI_Aircraft_Needs_Dock(airtype)) {
                int aircraft = 0;
                int docks = 0;
                Advanced_AI_Aircraft_Dock_Balance(airtype, aircraft, docks);
                if (aircraft >= docks) continue;
            }
        }

        return d.Type;
    }

    return nullptr;
}


/**
 *  AF-0009U: Fills gathering scripted teams with free units before the
 *  dynamic teams recruit, and starts teams whose wait time ran out.
 */
void HouseClassExtension::AdvAI_Scripted_Team_AI()
{
    HouseClass* house = This();

    for (int i = 0; i < Teams.Count(); i++) {
        TeamClass* team = Teams[i];
        if (!Is_Gathering_Scripted_Team(team, house)) continue;

        for (int j = 0; j < Foots.Count(); j++) {
            FootClass* foot = Foots[j];
            if (!foot->IsActive || foot->IsInLimbo || foot->House != house || foot->Team != nullptr) continue;

            int typeindex = -1;
            if (team->Can_Add(foot, typeindex)) {
                foot->Assign_Target(nullptr);
                team->Add(foot);
            }
        }

        TeamClassExtension* teamext = Extension::Fetch(team);
        if (teamext->AdvAIScriptedDeadline > 0 && Frame >= teamext->AdvAIScriptedDeadline && team->Member != nullptr &&
            !team->IsForcedActive) {
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} scripted-team={} wait time over, starting with {} members\n",
                (int)house->HeapID, Frame, team->Class->Name(), team->Total);
            team->IsForcedActive = true;
        }
    }
}


bool HouseClassExtension::Has_One_Of(const TypeList<BuildingTypeClass*>& buildingtypes) const
{
    for (int i = 0; i < buildingtypes.Count(); i++) {
        if (buildingtypes[i] != nullptr && This_Const()->ActiveBQuantity.Value(buildingtypes[i]->HeapID) > 0) {
            return true;
        }
    }
    return false;
}


bool HouseClassExtension::Has_Barracks() const { return Has_One_Of(Rule->BuildBarracks); }
bool HouseClassExtension::Has_War_Factory() const { return Has_One_Of(Rule->BuildWeapons); }
bool HouseClassExtension::Has_Helipad() const { return Has_One_Of(Rule->BuildHelipad); }
bool HouseClassExtension::Has_Construction_Yard() const { return Has_One_Of(Rule->BuildConst); }


/**
 *  Aftermath has no BuildNavalYard list; a naval yard is any owned factory
 *  that produces naval units.
 */
bool HouseClassExtension::Has_Naval_Yard() const
{
    for (int i = 0; i < Buildings.Count(); i++) {
        const BuildingClass* building = Buildings[i];
        if (building->House == This_Const() && Is_Usable(building) && building->Class->ToBuild == RTTI_UNITTYPE &&
            Extension::Fetch(building->Class)->IsNaval) {
            return true;
        }
    }
    return false;
}


bool HouseClassExtension::AdvAI_Is_Recently_Attacked() const
{
    return This_Const()->LATime > 0 && This_Const()->LATime + TICKS_PER_MINUTE > Frame;
}


bool HouseClassExtension::AdvAI_Is_Outnumbered() const
{
    const int enemytotalstrength = EnemyNoneStrength + EnemyLightStrength + EnemyHeavyStrength;
    int ourstrength = 0;

    for (int i = 0; i < Teams.Count(); i++) {
        TeamClass* team = Teams[i];
        if (team->House != This_Const()) continue;

        const TeamClassExtension* teamext = Extension::Fetch(team);
        if (!teamext->Is_AdvAI_Managed() || teamext->IsAircraftTeam) continue;

        ourstrength += teamext->CurrentCost;
    }

    return enemytotalstrength > ourstrength * 2;
}


bool HouseClassExtension::AdvAI_Is_Disadvantaged() const
{
    const int enemytotalstrength = EnemyNoneStrength + EnemyLightStrength + EnemyHeavyStrength;
    int ourstrength = 0;

    for (int i = 0; i < Teams.Count(); i++) {
        TeamClass* team = Teams[i];
        if (team->House != This_Const()) continue;

        const TeamClassExtension* teamext = Extension::Fetch(team);
        if (!teamext->Is_AdvAI_Managed()) continue;

        ourstrength += teamext->CurrentCost;
    }

    return enemytotalstrength > ourstrength;
}


bool HouseClassExtension::Enemy_Building_Scan(const TypeList<BuildingTypeClass*>& list, int threatvalue) const
{
    HouseClass* house = This();

    if (house->Enemy == HOUSE_NONE) {
        return false;
    }

    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* building = Buildings[i];

        if (!Is_Usable(building) || building->House->HeapID != house->Enemy) continue;
        if (!list.Is_Present(building->Class)) continue;

        Cell cell = building->Center_Coord().As_Cell();
        if (Map.Cell_Threat(cell, house) <= threatvalue) {
            return true;
        }
    }

    return false;
}


bool HouseClassExtension::AdvAI_Enemy_Has_Vulnerable_Buildings(const TypeList<BuildingTypeClass*>& list) const
{
    return Enemy_Building_Scan(list, RuleExtension->AdvancedAIVulnerableBuildingMaxThreat);
}


bool HouseClassExtension::AdvAI_Enemy_Has_Lightly_Defended_Buildings(const TypeList<BuildingTypeClass*>& list) const
{
    return Enemy_Building_Scan(list, RuleExtension->AdvancedAILightlyDefendedBuildingMaxThreat);
}


/**
 *  Biases the desired composition of a team towards countering the enemy.
 *
 *  @author: Rampastring
 */
void HouseClassExtension::AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(TeamClass* team) const
{
    TeamClassExtension* teamext = Extension::Fetch(team);

    teamext->IsBiasedForEnemyStrength = true;

    const int enemytotal = EnemyBaseDefenseStrength + EnemyHeavyStrength + EnemyLightStrength + EnemyNoneStrength + EnemyArtilleryStrength;
    if (enemytotal <= 0) {
        return;
    }

    const double enemyBaseDefenses = EnemyBaseDefenseStrength / (double)enemytotal;
    const double enemyHeavy = EnemyHeavyStrength / (double)enemytotal;
    const double enemyLight = EnemyLightStrength / (double)enemytotal;
    const double enemyNone = EnemyNoneStrength / (double)enemytotal;
    const double enemyArtillery = EnemyArtilleryStrength / (double)enemytotal;

    if (teamext->DesiredAntiNoneRatio > 0.0) teamext->DesiredAntiNoneRatio = (teamext->DesiredAntiNoneRatio + enemyNone * 2) / 3.0;
    if (teamext->DesiredAntiLightRatio > 0.0) teamext->DesiredAntiLightRatio = (teamext->DesiredAntiLightRatio + enemyLight * 2) / 3.0;
    if (teamext->DesiredAntiHeavyRatio > 0.0) teamext->DesiredAntiHeavyRatio = (teamext->DesiredAntiHeavyRatio + enemyHeavy * 2) / 3.0;

    if (enemyBaseDefenses > 0.0 && teamext->DesiredArtilleryRatio > 0.0) {
        teamext->DesiredArtilleryRatio = (teamext->DesiredArtilleryRatio + enemyBaseDefenses) / 2.0;
    }

    if (enemyArtillery > teamext->DesiredArtilleryRatio) {
        teamext->DesiredArtilleryRatio = (teamext->DesiredArtilleryRatio + enemyArtillery) / 2.0;
    }

    const EnemyStrengthStruct* strengthstruct = teamext->ProdFlags == PRODFLAG_NAVAL ? &EnemyAntiNavalStrength : &EnemyAntiGroundStrength;

    if (teamext->IsAircraftTeam || teamext->IsTransportTeam) {
        return;
    }

    const int enemyCounterTotal = strengthstruct->Total();
    if (enemyCounterTotal <= 0) {
        return;
    }

    if (teamext->DesiredNoneStrengthRatio > 0.0) {
        const double enemyAntiNoneRatio = (strengthstruct->None * 2.0) / (double)enemyCounterTotal;
        const double ratio = enemyAntiNoneRatio / teamext->DesiredNoneStrengthRatio;
        teamext->DesiredNoneStrengthRatio = teamext->DesiredNoneStrengthRatio / std::max(ratio, 0.1);

        if (enemyArtillery > 0) {
            const double multi = EnemyArtilleryStrength > 2500 ? 2.0 : 1.0;
            teamext->DesiredNoneStrengthRatio = std::max(teamext->DesiredNoneStrengthRatio * (1.0 - (enemyArtillery * multi)), 0.0);
        }
    }

    if (teamext->DesiredLightStrengthRatio > 0.0) {
        const double enemyAntiLightRatio = strengthstruct->Light / (double)enemyCounterTotal;
        const double ratio = enemyAntiLightRatio / teamext->DesiredLightStrengthRatio;
        teamext->DesiredLightStrengthRatio = teamext->DesiredLightStrengthRatio / std::max(ratio, 0.1);
    }

    if (teamext->DesiredHeavyStrengthRatio > 0.0) {
        const double enemyAntiHeavyRatio = strengthstruct->Heavy / (double)enemyCounterTotal;
        const double ratio = enemyAntiHeavyRatio / teamext->DesiredHeavyStrengthRatio;
        teamext->DesiredHeavyStrengthRatio = teamext->DesiredHeavyStrengthRatio / std::max(ratio, 0.1);
    }
}


void HouseClassExtension::AdvAI_Set_Aircraft_Team_Desired_Ratios(TeamClass* team) const
{
    TeamClassExtension* teamext = Extension::Fetch(team);
    const UnitTypeClass* harvester = Rule->HarvesterUnit.Count() > 0 ? Rule->HarvesterUnit[0] : nullptr;
    const bool steelharvester = harvester != nullptr && harvester->Armor == ARMOR_STEEL;

    teamext->DesiredAntiNoneRatio = 0.0;
    teamext->DesiredAntiLightRatio = 0.0;
    teamext->DesiredAntiHeavyRatio = 0.0;

    switch (AdvAIAirTactic.Tactic) {
    case AIRTACTIC_ATTACK_INFANTRY:
        teamext->DesiredAntiNoneRatio = 0.8;
        teamext->DesiredAntiLightRatio = 0.1;
        teamext->DesiredAntiHeavyRatio = 0.1;
        break;
    case AIRTACTIC_ATTACK_HARVESTERS:
        if (steelharvester) {
            teamext->DesiredAntiHeavyRatio = 1.0;
        } else {
            teamext->DesiredAntiLightRatio = 1.0;
        }
        break;
    case AIRTACTIC_ATTACK_REFINERIES:
    case AIRTACTIC_ATTACK_FACTORIES:
        teamext->DesiredAntiLightRatio = 1.0;
        break;
    case AIRTACTIC_ATTACK_VEHICLES:
        if (EnemyArtilleryStrength > 2000 || EnemyLightStrength > EnemyHeavyStrength || !steelharvester) {
            teamext->DesiredAntiLightRatio = 0.5;
            teamext->DesiredAntiHeavyRatio = 0.5;
        } else {
            teamext->DesiredAntiLightRatio = 0.25;
            teamext->DesiredAntiHeavyRatio = 0.75;
        }
        break;
    default:
        teamext->DesiredAntiLightRatio = 0.5;
        teamext->DesiredAntiHeavyRatio = 0.5;
        break;
    }
}


void HouseClassExtension::AdvAI_Set_Naval_Team_Desired_Ratios(TeamClass* team) const
{
    TeamClassExtension* teamext = Extension::Fetch(team);

    teamext->DesiredAntiNoneRatio = 0.0;
    teamext->DesiredAntiLightRatio = 0.5;
    teamext->DesiredAntiHeavyRatio = 0.5;
    teamext->DesiredArtilleryRatio = 0.3;
    teamext->DesiredLightStrengthRatio = 0.5;
    teamext->DesiredHeavyStrengthRatio = 0.5;
    teamext->PenalizeSameTypeUnits = true;
    AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(team);
}


/**
 *  Sets the desired composition of a ground team for its tactic.
 *
 *  @author: Rampastring
 */
void HouseClassExtension::AdvAI_Set_Ground_Team_Desired_Ratios(TeamClass* team, AdvAITacticType tacticoverride) const
{
    TeamClassExtension* teamext = Extension::Fetch(team);

    AdvAITacticType tactic = AdvAIGroundTactic.Tactic;
    if (tacticoverride != TACTIC_NONE) {
        tactic = tacticoverride;
    }

    /**
     *  Reset the values so that repeated maintenance passes don't accumulate.
     */
    teamext->DesiredAntiNoneRatio = 0.0;
    teamext->DesiredAntiLightRatio = 0.0;
    teamext->DesiredAntiHeavyRatio = 0.0;
    teamext->DesiredArtilleryRatio = 0.0;
    teamext->DesiredNoneStrengthRatio = 0.0;
    teamext->DesiredLightStrengthRatio = 0.0;
    teamext->DesiredHeavyStrengthRatio = 0.0;
    teamext->SpeedValueMultiplier = 1.0;
    teamext->StrengthValueMultiplier = 1.0;
    teamext->CloakValueMultiplier = 0.0;
    teamext->CostWeightMultiplier = 1.0;
    teamext->PenalizeSameTypeUnits = false;

    /**
     *  DTA keyed some values to its two primary factions. Aftermath uses
     *  the side instead: side 0 (GDI-like) and side 1 (Nod-like).
     */
    const int side = (int)This_Const()->Class->Side;

    switch (tactic) {
    case TACTIC_SCOUT:
        teamext->DesiredAntiNoneRatio = 0.75;
        teamext->DesiredAntiLightRatio = 0.25;
        teamext->SpeedValueMultiplier = 2.5;
        teamext->CostWeightMultiplier = 1.0;
        teamext->DesiredNoneStrengthRatio = 1.0;
        break;
    case TACTIC_RUSH_ATTACK:
        teamext->DesiredAntiLightRatio = 1.0;
        teamext->SpeedValueMultiplier = side == 1 ? 3.0 : 4.0;
        if (teamext->NoInfantry) {
            teamext->DesiredLightStrengthRatio = 1.0;
        } else if (teamext->NoVehicles) {
            teamext->DesiredNoneStrengthRatio = 1.0;
        } else {
            teamext->DesiredLightStrengthRatio = 0.67;
            teamext->DesiredNoneStrengthRatio = 0.33;
        }
        break;
    case TACTIC_DIRECT_ATTACK_REGULAR:
        teamext->DesiredAntiNoneRatio = 0.33;
        teamext->DesiredAntiLightRatio = 0.33;
        teamext->DesiredAntiHeavyRatio = 0.33;
        teamext->DesiredArtilleryRatio = 0.5;
        teamext->SpeedValueMultiplier = side > 0 ? -0.2 : 0.0;
        teamext->StrengthValueMultiplier = 1.2;
        teamext->CostWeightMultiplier = 0.85;
        teamext->DesiredNoneStrengthRatio = 0.2;
        teamext->DesiredLightStrengthRatio = 0.35;
        teamext->DesiredHeavyStrengthRatio = 0.45;
        teamext->PenalizeSameTypeUnits = true;
        AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(team);
        break;
    case TACTIC_DIRECT_ATTACK_FAST:
        teamext->DesiredAntiNoneRatio = 0.2;
        teamext->DesiredAntiLightRatio = 0.5;
        teamext->DesiredAntiHeavyRatio = 0.3;
        teamext->SpeedValueMultiplier = 2.0;
        teamext->CostWeightMultiplier = 0.9;
        teamext->DesiredNoneStrengthRatio = 0.2;
        teamext->DesiredLightStrengthRatio = 0.3;
        teamext->DesiredHeavyStrengthRatio = 0.5;
        teamext->PenalizeSameTypeUnits = true;
        AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(team);
        break;
    case TACTIC_ATTACK_HARVESTERS:
        teamext->DesiredAntiHeavyRatio = 0.7;
        teamext->DesiredAntiLightRatio = 0.3;
        teamext->SpeedValueMultiplier = 2.0;
        teamext->CloakValueMultiplier = EnemyHasSensors ? 0.3 : 2.0;
        AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(team);
        break;
    case TACTIC_ATTACK_REFINERIES:
        teamext->DesiredAntiLightRatio = 0.7;
        teamext->DesiredAntiHeavyRatio = 0.3;
        teamext->SpeedValueMultiplier = 1.2;
        teamext->StrengthValueMultiplier = 1.2;
        break;
    case TACTIC_APC_ATTACK:
        teamext->SpeedValueMultiplier = 0.0; // Speed doesn't matter for infantry carried by APCs.
        teamext->DesiredAntiNoneRatio = 0.1;
        teamext->DesiredAntiLightRatio = 0.8;
        teamext->DesiredAntiHeavyRatio = 0.1;
        teamext->StrengthValueMultiplier = 2.0;
        teamext->CostWeightMultiplier = 0.75;
        teamext->CloakValueMultiplier = 3.0;
        break;
    default:
        teamext->DesiredAntiNoneRatio = 0.33;
        teamext->DesiredAntiLightRatio = 0.33;
        teamext->DesiredAntiHeavyRatio = 0.33;
        teamext->DesiredNoneStrengthRatio = 0.33;
        teamext->DesiredLightStrengthRatio = 0.33;
        teamext->DesiredHeavyStrengthRatio = 0.33;
        AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(team);
        break;
    }
}


void HouseClassExtension::Assign_AdvAI_Tactic(AdvAITacticType tactic, int expected_duration)
{
    DEV_DEBUG_INFO("AF-0009U: house={} frame={} ground-tactic={} duration={}\n", (int)This_Const()->HeapID, Frame,
        AdvAITacticType_To_Name(tactic), expected_duration);

    AdvAIGroundTactic = AdvAITacticInfo<AdvAITacticType>(tactic, Frame, expected_duration);
    AdvAIGroundTacticPostponements = 0;
    LastInfantryValueDebugPrintFrame = INT_MIN / 2;
    LastUnitValueDebugPrintFrame = INT_MIN / 2;
}


void HouseClassExtension::Assign_AdvAI_Air_Tactic(AdvAIAirTacticType airtactic, int expected_duration)
{
    DEV_DEBUG_INFO("AF-0009U: house={} frame={} air-tactic={} duration={}\n", (int)This_Const()->HeapID, Frame,
        (int)airtactic, expected_duration);

    AdvAIAirTactic = AdvAITacticInfo<AdvAIAirTacticType>(airtactic, Frame, expected_duration);
    LastAircraftValueDebugPrintFrame = INT_MIN / 2;
}


void HouseClassExtension::Assign_AdvAI_Naval_Tactic(AdvAINavalTacticType navaltactic, int expected_duration)
{
    DEV_DEBUG_INFO("AF-0009U: house={} frame={} naval-tactic={} duration={}\n", (int)This_Const()->HeapID, Frame,
        (int)navaltactic, expected_duration);

    AdvAINavalTactic = AdvAITacticInfo<AdvAINavalTacticType>(navaltactic, Frame, expected_duration);
    LastNavalValueDebugPrintFrame = INT_MIN / 2;
}


/**
 *  Building capture bookkeeping for engineers (oil derricks and neutral
 *  superweapon buildings).
 */
int HouseClassExtension::Get_Building_Capture_Attempt_Index_For(const BuildingClass* building) const
{
    for (int j = 0; j < AttemptedBuildingCaptureCount; j++) {
        if (AttemptedBuildingCaptures[j].BuildingLocation == building->PositionCell) {
            return j;
        }
    }
    return -1;
}


static bool Should_Try_To_Capture(const BuildingClass* building)
{
    const BuildingTypeClassExtension* btypeext = Extension::Fetch(building->Class);

    /**
     *  AF-0009U: AIEngineerCapture=yes marks further targets (tech buildings),
     *  with the same owner rule as oil derricks.
     */
    if ((btypeext->ProduceCashAmount > 0 || btypeext->IsAIEngineerCapture) &&
        (building->House->Class->IsMultiplayPassive || !Session.Options.CrapEngineers)) {
        return true;
    }

    return building->Class->SuperWeapon != SUPER_NONE && building->House->Class->IsMultiplayPassive;
}


bool HouseClassExtension::Is_Valid_Building_For_Capturing(BuildingClass* building, const Cell& zonecell, bool& okintheory) const
{
    okintheory = false;

    if (!Is_Usable(building) || !Should_Try_To_Capture(building) || !building->Class->IsCaptureable ||
        This_Const()->Is_Ally(building->House) || !Map.Is_Same_Zone(zonecell, building->PositionCell)) {
        return false;
    }

    okintheory = true;

    Cell cell = building->Center_Coord().As_Cell();
    if (Map.Cell_Threat(cell, This()) > 0) {
        return false;
    }

    /**
     *  Don't retry a recent attempt: the engineer is on its way or died there.
     */
    const int attemptindex = Get_Building_Capture_Attempt_Index_For(building);
    if (attemptindex > -1 && AttemptedBuildingCaptures[attemptindex].Frame > Frame - 20000) {
        return false;
    }

    return true;
}


void HouseClassExtension::Add_Building_Capture_Attempt(const BuildingClass* building)
{
    AttemptedBuildingCaptureStruct attempt{ building->PositionCell, Frame };

    if (AttemptedBuildingCaptureCount >= (int)std::size(AttemptedBuildingCaptures)) {
        AttemptedBuildingCaptures[0] = attempt;
    } else {
        AttemptedBuildingCaptures[AttemptedBuildingCaptureCount++] = attempt;
    }
}


/**
 *  Checks if the AI is under threat of being rushed early in the game.
 *
 *  @author: Rampastring
 */
bool HouseClassExtension::AdvAI_Is_Under_Start_Rush_Threat() const
{
    // If the game has progressed for long enough, it is no longer considered a start rush.
    if (Frame > 10000) {
        return false;
    }

    if (AdvAI_Is_Recently_Attacked()) {
        return true;
    }

    const HouseClass* house = This_Const();
    const int count = std::min(Houses.Count(), 32);
    int strength[32] = {};

    for (int i = 0; i < Infantry.Count(); i++) {
        const InfantryClass* inf = Infantry[i];

        if (inf->IsInLimbo || inf->House == nullptr) continue;

        const int id = inf->House->HeapID;
        if (id < 0 || id >= count) continue;

        if (inf->House != house && (inf->House->Class->IsMultiplayPassive || inf->House->Is_Ally(house))) continue;

        strength[id] += inf->Class->Points;
    }

    const int ourstrength = strength[house->HeapID];
    for (int i = 0; i < count; i++) {
        if (i == house->HeapID) continue;

        // Humans can typically micromanage better than the AI.
        const int theirmultiplier = Houses[i]->Is_Human_Player() ? 5 : 2;
        if (strength[i] * theirmultiplier > ourstrength * 3) {
            return true;
        }
    }

    return false;
}


/**
 *  AF-0009U: Flying enemies (for example jumpjets) attack our buildings and too
 *  few of our units near the base can hit them. Build the anti-aircraft unit of
 *  the given kind (infantry or vehicle) with the best value per cost, before
 *  team production. The self defense sends it against the attackers.
 */
bool HouseClassExtension::AdvAI_Select_Air_Defense_Unit(RTTIType rtti)
{
    HouseClass* house = This();

    const int attackers = Advanced_AI_Air_Attackers();
    if (attackers <= 0) return false;

    // Count our units near the base that can already hit air targets, including production.
    static constexpr int BaseRadius = 30 * CELL_LEPTON_W;
    int defenders = 0;
    for (int i = 0; i < Foots.Count(); i++) {
        const FootClass* foot = Foots[i];
        if (foot->House != house || !foot->IsActive || foot->Strength <= 0 || foot->RTTI == RTTI_AIRCRAFT) continue;
        if (!Advanced_AI_Can_Hit_Air(foot->TClass)) continue;
        if (!foot->IsInLimbo && house->Center != COORD_NONE && foot->Center_Coord().Distance_To(house->Center) > BaseRadius) continue;
        defenders++;
    }
    if (defenders >= 2 * attackers + 1) return false;

    DynamicVectorClass<BuildingTypeClass*> owned;
    Fill_Owned_Buildings_List(owned);

    const auto score = [](const TechnoTypeClass* type) -> long long {
        const TechnoTypeClassExtension* ext = Extension::Fetch(type);
        const long long value = static_cast<long long>(ext->AntiNoneArmorValueAA()) + ext->AntiLightArmorValueAA() +
            ext->AntiHeavyArmorValueAA();
        return value * 1000 / std::max(1, type->Cost);
    };

    long long best = 0;
    int choice = -1;
    if (rtti == RTTI_INFANTRY) {
        for (int i = 0; i < InfantryTypes.Count(); i++) {
            const InfantryTypeClass* type = InfantryTypes[i];
            if (!Advanced_AI_Can_Hit_Air(type) || !AdvAI_Can_Produce(type, owned)) continue;
            if (type->BuildLimit > 0 && house->ActiveIQuantity.Value(type->HeapID) >= type->BuildLimit) continue;
            const long long value = score(type);
            if (value > best) { best = value; choice = type->HeapID; }
        }
        if (choice < 0) return false;
        house->BuildInfantry = static_cast<InfantryType>(choice);
    } else {
        for (int i = 0; i < UnitTypes.Count(); i++) {
            const UnitTypeClass* type = UnitTypes[i];
            if (Extension::Fetch(type)->IsNaval || type->IsToHarvest || type->IsToVeinHarvest) continue;
            if (!Advanced_AI_Can_Hit_Air(type) || !AdvAI_Can_Produce(type, owned)) continue;
            if (type->BuildLimit > 0 && house->ActiveUQuantity.Value(type->HeapID) >= type->BuildLimit) continue;
            const long long value = score(type);
            if (value > best) { best = value; choice = type->HeapID; }
        }
        if (choice < 0) return false;
        house->BuildUnit = static_cast<UnitType>(choice);
    }

    DEV_DEBUG_INFO("AF-0009U: house={} frame={} air-defense-unit={} attackers={} defenders={}\n", (int)house->HeapID, Frame,
        rtti == RTTI_INFANTRY ? InfantryTypes[choice]->Name() : UnitTypes[choice]->Name(), attackers, defenders);
    return true;
}


/**
 *  Builds the best anti-infantry unit when being rushed without a team to fill.
 */
int HouseClassExtension::AdvAI_Start_Rush_Counter(RTTIType rtti)
{
    HouseClass* house = This();

    DynamicVectorClass<BuildingTypeClass*> owned;
    Fill_Owned_Buildings_List(owned);

    int highestvalue = -1;

    if (rtti == RTTI_INFANTRY) {
        InfantryType mostvaluable = INFANTRY_NONE;
        for (int i = 0; i < InfantryTypes.Count(); i++) {
            const InfantryTypeClass* type = InfantryTypes[i];
            if (!AdvAI_Can_Produce(type, owned)) continue;
            if (type->BuildLimit > 0 && house->ActiveIQuantity.Value(type->HeapID) >= type->BuildLimit) continue;

            const int value = Extension::Fetch(type)->AntiNoneArmorValue();
            if (value > highestvalue) {
                mostvaluable = type->HeapID;
                highestvalue = value;
            }
        }
        if (mostvaluable != INFANTRY_NONE) house->BuildInfantry = mostvaluable;

    } else {
        UnitType mostvaluable = UNIT_NONE;
        for (int i = 0; i < UnitTypes.Count(); i++) {
            const UnitTypeClass* type = UnitTypes[i];
            const UnitTypeClassExtension* typeext = Extension::Fetch(type);
            if (typeext->IsNaval || type->IsToHarvest || type->IsToVeinHarvest) continue;
            if (!AdvAI_Can_Produce(type, owned)) continue;
            if (type->BuildLimit > 0 && house->ActiveUQuantity.Value(type->HeapID) >= type->BuildLimit) continue;

            const int value = typeext->AntiNoneArmorValue() + typeext->AntiLightArmorValue();
            if (value > highestvalue) {
                mostvaluable = type->HeapID;
                highestvalue = value;
            }
        }
        if (mostvaluable != UNIT_NONE) house->BuildUnit = mostvaluable;
    }

    return TICKS_PER_SECOND;
}


/**
 *  Aftermath: How many harvesters are missing to reach the target per refinery?
 *  Below one harvester per refinery the replacement always has priority. Above
 *  that it waits while the army is outnumbered or the house is being rushed.
 *  Funding is checked when the factory starts (unit budget, stage A); harvesters
 *  are exempt from the planner's savings goals.
 */
int HouseClassExtension::AdvAI_Harvester_Shortfall(const UnitTypeClass* harvester) const
{
    const HouseClass* house = This_Const();

    if (harvester == nullptr || house->IQ < Rule->IQHarvester || house->Control.TechLevel < 0 ||
        harvester->Level > (unsigned)house->Control.TechLevel) {
        return 0;
    }

    int refineries = 0;
    for (int i = 0; i < Buildings.Count(); i++) {
        const BuildingClass* building = Buildings[i];
        if (building->House == house && Is_Usable(building) && building->Class->IsRefinery && !building->Class->IsWeeder) {
            refineries++;
        }
    }
    if (refineries == 0) {
        return 0;
    }

    /**
     *  Units already contains harvesters still in production (active, in limbo),
     *  so no separate factory pass is needed. Weeders don't serve Tiberium
     *  refineries and don't count towards their quota.
     */
    int harvesters = 0;
    for (int i = 0; i < Units.Count(); i++) {
        const UnitClass* unit = Units[i];
        if (unit->House == house && unit->IsActive && unit->Strength > 0 && unit->Class->IsToHarvest && !unit->Class->IsToVeinHarvest) {
            harvesters++;
        }
    }

    const int perrefinery = std::max(1, RuleExtension->AdvancedAIHarvestersPerRefinery);
    const int shortfall = refineries * perrefinery - harvesters;
    if (shortfall <= 0) {
        return 0;
    }

    // At least one harvester per refinery is always restored.
    if (harvesters < refineries) {
        return shortfall;
    }

    // The second harvester waits while the army is needed more urgently.
    if (IsUnderStartRushThreat || AdvAI_Is_Outnumbered()) {
        return 0;
    }

    return shortfall;
}


/**
 *  Advanced AI vehicle production.
 *
 *  @author: Rampastring (DTA), Aftermath adaptation
 */
int HouseClassExtension::AdvAI_AI_Unit()
{
    HouseClass* house = This();
    UnitTypeClass* harvester = Rule->HarvesterUnit.Count() > 0 ? house->Get_First_ActLike(Rule->HarvesterUnit) : nullptr;

    DynamicVectorClass<BuildingTypeClass*> owned;
    Fill_Owned_Buildings_List(owned);

    /**
     *  Without a construction yard, rebuild the base with an MCV.
     *
     *  Aftermath: This runs before the pending-order checks below. They return
     *  while an order waits, so a military order waiting for credits blocked the
     *  MCV (log 19.11.2026 19-30-45: MMCH pending when the yard was lost). An
     *  order waiting here has not started yet (the factory clears BuildUnit when
     *  it starts), so the MCV may replace it. A pending recovery unit (the MCV
     *  itself, or the last harvester) keeps its order.
     */
    const UnitTypeClass* pending_unit = house->BuildUnit >= UNIT_FIRST && house->BuildUnit < UnitTypes.Count() ? UnitTypes[house->BuildUnit] : nullptr;
    if (house->ConstructionYards.Count() < 1 && !(pending_unit && Needs_Advanced_AI_Recovery_Unit(pending_unit))) {
        /**
         *  Aftermath: Trace why no MCV is ordered (once per type and reason).
         *  A house without a construction yard built no MCV for 35000 frames
         *  (log 19.11.2026 10-46-00) and the log did not tell why.
         */
        struct McvTrace { int Key; const char* Reason; long Frame; };
        static std::vector<McvTrace> mcv_traces;
        auto trace_mcv = [&](const UnitTypeClass* mcv, const char* reason) {
            const int key = static_cast<int>(house->HeapID) * 1024 + (mcv ? static_cast<int>(mcv->HeapID) : 1023);
            auto found = std::find_if(mcv_traces.begin(), mcv_traces.end(), [key](const McvTrace& entry) { return entry.Key == key; });
            // Frame < entry.Frame: a new game started; trace again.
            if (found != mcv_traces.end() && found->Reason == reason && Frame >= found->Frame) {
                found->Frame = Frame;
                return;
            }
            if (found != mcv_traces.end()) *found = {key, reason, static_cast<long>(Frame)};
            else mcv_traces.push_back({key, reason, static_cast<long>(Frame)});
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} mcv-recovery type={} result={} owned-base-units={} funds={}\n",
                (int)house->HeapID, Frame, mcv ? mcv->Name() : "none", reason,
                house->Count_Owned(RuleExtension->BaseUnit), house->Available_Money());
        };
        auto try_mcv = [&](UnitTypeClass* mcv) {
            if (mcv == nullptr) return false;
            const char* reason = !Needs_Advanced_AI_Recovery_Unit(mcv) ? "not-needed" :
                house->ActiveUQuantity.Value(mcv->HeapID) >= 1 ? "already-active" :
                !house->Can_Build(mcv, true, true) ? "cannot-build" :
                !house->Has_Prerequisites(mcv, owned, owned.Count()) ? "missing-prerequisites" : nullptr;
            const bool order = reason == nullptr;
            trace_mcv(mcv, order ? "ordered" : reason);
            if (order) {
                house->BuildUnit = mcv->HeapID;
                return true;
            }
            return false;
        };

        for (int i = 0; i < RuleExtension->BaseUnit.Count(); i++) {
            if (try_mcv(RuleExtension->BaseUnit[i])) return TICKS_PER_SECOND;
        }
        if (try_mcv(Rule->BaseUnit)) return TICKS_PER_SECOND;
    }

    /**
     *  An order is still pending. Keep the Aftermath rule that the first
     *  replacement harvester may replace a deferred military request.
     */
    /**
     *  AF-0009U: While flying enemies hit the base, a pending order that cannot
     *  hit air targets (it may wait for credits for a long time) gives way to an
     *  anti-aircraft unit. Harvesters and recovery units keep their order.
     */
    if (house->BuildUnit != UNIT_NONE && Advanced_AI_Air_Attackers() > 0) {
        const UnitTypeClass* pending = house->BuildUnit >= UNIT_FIRST && house->BuildUnit < UnitTypes.Count() ? UnitTypes[house->BuildUnit] : nullptr;
        if (pending && !Advanced_AI_Can_Hit_Air(pending) && !pending->IsToHarvest && !pending->IsToVeinHarvest &&
            !Needs_Advanced_AI_Recovery_Unit(pending) && AdvAI_Select_Air_Defense_Unit(RTTI_UNIT)) {
            return TICKS_PER_SECOND;
        }
    }

    if (house->BuildUnit != UNIT_NONE) {
        const UnitTypeClass* current = house->BuildUnit >= UNIT_FIRST && house->BuildUnit < UnitTypes.Count() ? UnitTypes[house->BuildUnit] : nullptr;
        if (harvester != nullptr && current != harvester && !(current && Needs_Advanced_AI_Recovery_Unit(current)) &&
            (Needs_Advanced_AI_Recovery_Unit(harvester) || AdvAI_Harvester_Shortfall(harvester) > 0) &&
            harvester->Who_Can_Build_Me(true, true, true, house)) {
            // A missing harvester replaces a not yet started military order.
            house->BuildUnit = harvester->HeapID;
        }
        return TICKS_PER_SECOND;
    }

    /**
     *  Harvesters. Aftermath: keep AdvancedAIHarvestersPerRefinery (default 2)
     *  harvesters per refinery and replace losses at once. DTA aimed for 1-2
     *  per refinery depending on difficulty and built at most one every 3000
     *  frames, which left refineries idle.
     */
    if (const int shortfall = AdvAI_Harvester_Shortfall(harvester); shortfall > 0) {
        LastHarvesterBuildFrame = Frame;
        house->BuildUnit = harvester->HeapID;
        DEV_DEBUG_INFO("AF-0009U: house={} frame={} harvester-replacement={} missing={}\n",
            (int)house->HeapID, Frame, harvester->Name(), shortfall);
        return TICKS_PER_SECOND;
    }

    // AF-0009U: Anti-aircraft vehicles against flying attackers come first.
    if (AdvAI_Select_Air_Defense_Unit(RTTI_UNIT)) return TICKS_PER_SECOND;

    // AF-0009U: Missing members of scripted (fixed TeamType) tactic teams come before dynamic teams.
    if (const TechnoTypeClass* need = AdvAI_Scripted_Team_Need(RTTI_UNITTYPE, false, owned)) {
        house->BuildUnit = static_cast<const UnitTypeClass*>(need)->HeapID;
        return TICKS_PER_SECOND;
    }

    int id = 0;
    TeamClass* team = Get_Team_In_Production(id, RTTI_UNIT, PRODFLAG_NONE);

    if (team == nullptr) {
        // Produce something when being rushed, even without a team.
        if (IsUnderStartRushThreat) {
            return AdvAI_Start_Rush_Counter(RTTI_UNIT);
        }
        return TICKS_PER_SECOND;
    }

    const TeamClassExtension* teamext = Extension::Fetch(team);

    UnitType mostvaluable = UNIT_NONE;
    int highestvalue = 0;

    const bool debugprint = Debug_Print_Due(LastUnitValueDebugPrintFrame);
    if (debugprint) {
        DEV_DEBUG_INFO("AF-0009U: house={} frame={} unit values, tactic={}\n", (int)house->HeapID, Frame,
            AdvAITacticType_To_Name(AdvAIGroundTactic.Tactic));
    }

    for (int i = 0; i < UnitTypes.Count(); i++) {
        UnitTypeClass* unittype = UnitTypes[i];
        const UnitTypeClassExtension* unittypeext = Extension::Fetch(unittype);

        if (unittypeext->IsNaval || unittype->IsToHarvest || unittype->IsToVeinHarvest) continue;
        if (!AdvAI_Can_Produce(unittype, owned)) continue;

        if (unittype->BuildLimit > 0) {
            int quantity = house->UQuantity.Value(unittype->HeapID);
            if (unittype->DeploysInto != nullptr && unittype->DeploysInto->UndeploysInto == unittype) {
                quantity += house->ActiveBQuantity.Value(unittype->DeploysInto->HeapID);
            }
            if (quantity >= unittype->BuildLimit) continue;
        }

        // Don't waste epic units on risky suicide missions.
        if (team->Class->IsSuicide && unittype->BuildLimit > 0 && unittype->BuildLimit < 5) continue;

        const int value = teamext->AdvAI_Get_Object_Value_For_Team(unittype, debugprint);
        if (value > highestvalue) {
            mostvaluable = unittype->HeapID;
            highestvalue = value;
        }
    }

    if (mostvaluable != UNIT_NONE) {
        if (debugprint) {
            DEV_DEBUG_INFO("AF-0009U: house={} selected unit {}\n", (int)house->HeapID, UnitTypes[mostvaluable]->Name());
        }

        house->BuildUnit = mostvaluable;

        if (AdvAILastBuiltUnit == mostvaluable) {
            AdvAILastBuiltUnitCount++;
        } else {
            AdvAILastBuiltUnit = mostvaluable;
            AdvAILastBuiltUnitCount = 1;
        }
    }

    return TICKS_PER_SECOND;
}


/**
 *  Advanced AI naval production.
 *
 *  @author: Rampastring (DTA)
 */
int HouseClassExtension::AdvAI_AI_Naval_Unit()
{
    if (BuildNavalUnit != UNIT_NONE) return TICKS_PER_SECOND;

    HouseClass* house = This();

    // AF-0009U: Missing naval members of scripted tactic teams first.
    {
        DynamicVectorClass<BuildingTypeClass*> scriptowned;
        Fill_Owned_Buildings_List(scriptowned);
        if (const TechnoTypeClass* need = AdvAI_Scripted_Team_Need(RTTI_UNITTYPE, true, scriptowned)) {
            BuildNavalUnit = static_cast<const UnitTypeClass*>(need)->HeapID;
            return TICKS_PER_SECOND;
        }
    }

    int id = 0;
    TeamClass* team = Get_Team_In_Production(id, RTTI_UNIT, PRODFLAG_NAVAL);
    if (team == nullptr) {
        return TICKS_PER_SECOND;
    }

    DynamicVectorClass<BuildingTypeClass*> owned;
    Fill_Owned_Buildings_List(owned);

    const TeamClassExtension* teamext = Extension::Fetch(team);

    UnitType mostvaluable = UNIT_NONE;
    int highestvalue = 0;

    const bool debugprint = Debug_Print_Due(LastNavalValueDebugPrintFrame);

    for (int i = 0; i < UnitTypes.Count(); i++) {
        UnitTypeClass* unittype = UnitTypes[i];
        if (!Extension::Fetch(unittype)->IsNaval) continue;
        if (!AdvAI_Can_Produce(unittype, owned)) continue;
        if (unittype->BuildLimit > 0 && house->UQuantity.Value(unittype->HeapID) >= unittype->BuildLimit) continue;

        const int value = teamext->AdvAI_Get_Object_Value_For_Team(unittype, debugprint);
        if (value > highestvalue) {
            mostvaluable = unittype->HeapID;
            highestvalue = value;
        }
    }

    if (mostvaluable != UNIT_NONE) {
        if (debugprint) {
            DEV_DEBUG_INFO("AF-0009U: house={} selected naval unit {}\n", (int)house->HeapID, UnitTypes[mostvaluable]->Name());
        }
        BuildNavalUnit = mostvaluable;
    }

    return TICKS_PER_SECOND;
}


static InfantryType Find_Engineer(HouseClass* house)
{
    for (int i = 0; i < InfantryTypes.Count(); i++) {
        InfantryTypeClass* infantrytype = InfantryTypes[i];
        if (infantrytype->IsEngineer && house->Can_Build(infantrytype, false, true)) {
            return infantrytype->HeapID;
        }
    }
    return INFANTRY_NONE;
}


/**
 *  Advanced AI infantry production.
 *
 *  @author: Rampastring (DTA), Aftermath adaptation
 */
int HouseClassExtension::AdvAI_AI_Infantry()
{
    HouseClass* house = This();

    if (house->BuildInfantry != INFANTRY_NONE) {
        // AF-0009U: A pending infantry order without anti-aircraft weapon gives way, see AdvAI_AI_Unit.
        const InfantryTypeClass* pending = house->BuildInfantry >= INFANTRY_FIRST && house->BuildInfantry < InfantryTypes.Count()
            ? InfantryTypes[house->BuildInfantry] : nullptr;
        if (pending && !Advanced_AI_Can_Hit_Air(pending) && !pending->IsEngineer && Advanced_AI_Air_Attackers() > 0) {
            AdvAI_Select_Air_Defense_Unit(RTTI_INFANTRY);
        }
        return TICKS_PER_SECOND;
    }

    /**
     *  Engineers for capturing neutral oil derricks and similar buildings.
     */
    if (RuleExtension->IsAdvancedAIEngineerCapture && NextOilRefineryCaptureCheckFrame < Frame && house->ConstructionYards.Count() > 0) {
        bool capturablefound = false;

        for (int i = 0; i < Buildings.Count(); i++) {
            BuildingClass* building = Buildings[i];

            bool okintheory = false;
            const bool valid = Is_Valid_Building_For_Capturing(building, house->ConstructionYards[0]->PositionCell, okintheory);

            if ((valid || okintheory) && !capturablefound) {
                // There is a building on the map that is theoretically capturable by us.
                NextOilRefineryCaptureCheckFrame = Frame + 5000 + Random_Pick(0, 500);
                capturablefound = true;
            }

            if (valid) {
                const InfantryType engineer = Find_Engineer(house);
                if (engineer != INFANTRY_NONE && house->ActiveIQuantity.Value(engineer) == 0) {
                    house->BuildInfantry = engineer;
                    return TICKS_PER_SECOND;
                }
                break;
            }
        }

        // If no capturable building exists even in theory, assume they are gone for the rest of the game.
        if (!capturablefound) {
            NextOilRefineryCaptureCheckFrame = INT_MAX;
        }
    }

    // AF-0009U: Anti-aircraft infantry against flying attackers comes first.
    if (AdvAI_Select_Air_Defense_Unit(RTTI_INFANTRY)) return TICKS_PER_SECOND;

    // AF-0009U: Missing members of scripted (fixed TeamType) tactic teams come before dynamic teams.
    {
        DynamicVectorClass<BuildingTypeClass*> scriptowned;
        Fill_Owned_Buildings_List(scriptowned);
        if (const TechnoTypeClass* need = AdvAI_Scripted_Team_Need(RTTI_INFANTRYTYPE, false, scriptowned)) {
            house->BuildInfantry = static_cast<const InfantryTypeClass*>(need)->HeapID;
            return TICKS_PER_SECOND;
        }
    }

    int id = 0;
    TeamClass* team = Get_Team_In_Production(id, RTTI_INFANTRY, PRODFLAG_NONE);

    if (team == nullptr) {
        // If we are being rushed, just build the best anti-infantry infantry type we can.
        if (IsUnderStartRushThreat) {
            return AdvAI_Start_Rush_Counter(RTTI_INFANTRY);
        }
        return TICKS_PER_SECOND;
    }

    const TeamClassExtension* teamext = Extension::Fetch(team);

    if (teamext->AdvAIGroundTacticType == TACTIC_DEFEND && !AdvAI_Is_Disadvantaged()) {
        return TICKS_PER_SECOND;
    }

    const bool haswarfactory = Has_War_Factory();

    DynamicVectorClass<BuildingTypeClass*> owned;
    Fill_Owned_Buildings_List(owned);

    const bool debugprint = Debug_Print_Due(LastInfantryValueDebugPrintFrame);
    if (debugprint) {
        DEV_DEBUG_INFO("AF-0009U: house={} frame={} infantry values, tactic={} war-factory={}\n", (int)house->HeapID, Frame,
            AdvAITacticType_To_Name(AdvAIGroundTactic.Tactic), haswarfactory);
    }

    InfantryType mostvaluable = INFANTRY_NONE;
    int highestvalue = 0;

    for (int i = 0; i < InfantryTypes.Count(); i++) {
        InfantryTypeClass* inftype = InfantryTypes[i];

        if (!AdvAI_Can_Produce(inftype, owned)) continue;
        if (inftype->BuildLimit > 0 && house->IQuantity.Value(inftype->HeapID) >= inftype->BuildLimit) continue;

        const int value = teamext->AdvAI_Get_Object_Value_For_Team(inftype, debugprint);
        if (value > highestvalue) {
            mostvaluable = inftype->HeapID;
            highestvalue = value;
        }
    }

    if (mostvaluable != INFANTRY_NONE) {

        if (debugprint) {
            DEV_DEBUG_INFO("AF-0009U: house={} selected infantry {}\n", (int)house->HeapID, InfantryTypes[mostvaluable]->Name());
        }

        if (haswarfactory) {
            const bool infantry_tactic = AdvAIGroundTactic.Tactic == TACTIC_RUSH_ATTACK ||
                AdvAIGroundTactic.Tactic == TACTIC_APC_ATTACK || AdvAIGroundTactic.Tactic == TACTIC_CHINOOK_ATTACK;

            if (highestvalue < RuleExtension->AdvancedAISkipInfantryProductionValueThreshold) {
                if (house->Available_Money() < Rule->AIAlternateProductionCreditCutoff && !infantry_tactic) {
                    return TICKS_PER_SECOND * 10;
                }
            } else if (highestvalue < RuleExtension->AdvancedAIConditionalSkipInfantryProductionValueThreshold) {
                if (house->Available_Money() < Rule->AIAlternateProductionCreditCutoff * 2 && !infantry_tactic && Percent_Chance(50)) {
                    return TICKS_PER_SECOND * 30;
                }
            }
        }

        if (AdvAILastBuiltInfantry == mostvaluable) {
            AdvAILastBuiltInfantryCount++;
        } else {
            AdvAILastBuiltInfantry = mostvaluable;
            AdvAILastBuiltInfantryCount = 1;
        }

        house->BuildInfantry = mostvaluable;

    } else if (IsUnderStartRushThreat) {
        // If we are under start rush threat, we should still build something.
        return AdvAI_Start_Rush_Counter(RTTI_INFANTRY);
    }

    return TICKS_PER_SECOND;
}


/**
 *  Advanced AI aircraft production.
 *
 *  @author: Rampastring (DTA), Aftermath adaptation
 */
int HouseClassExtension::AdvAI_AI_Aircraft()
{
    HouseClass* house = This();

    if (house->BuildAircraft != AIRCRAFT_NONE) return TICKS_PER_SECOND;

    // AF-0009U: Missing aircraft of scripted (fixed TeamType) tactic teams first.
    {
        DynamicVectorClass<BuildingTypeClass*> scriptowned;
        Fill_Owned_Buildings_List(scriptowned);
        if (const TechnoTypeClass* need = AdvAI_Scripted_Team_Need(RTTI_AIRCRAFTTYPE, false, scriptowned)) {
            house->BuildAircraft = static_cast<const AircraftTypeClass*>(need)->HeapID;
            return TICKS_PER_SECOND;
        }
    }

    int id = 0;
    TeamClass* team = Get_Team_In_Production(id, RTTI_AIRCRAFT, PRODFLAG_NONE);
    if (team == nullptr) {
        return TICKS_PER_SECOND;
    }

    const TeamClassExtension* teamext = Extension::Fetch(team);

    DynamicVectorClass<BuildingTypeClass*> owned;
    Fill_Owned_Buildings_List(owned);

    const bool debugprint = Debug_Print_Due(LastAircraftValueDebugPrintFrame);

    AircraftType mostvaluable = AIRCRAFT_NONE;
    int highestvalue = 0;

    for (int i = 0; i < AircraftTypes.Count(); i++) {
        AircraftTypeClass* airtype = AircraftTypes[i];

        if (!AdvAI_Can_Produce(airtype, owned)) continue;
        if (airtype->BuildLimit > 0 && house->AQuantity.Value(airtype->HeapID) >= airtype->BuildLimit) continue;

        /**
         *  Aftermath: Every armed aircraft needs a fixed dock. It is only built while
         *  the owned armed aircraft (flying, landed or in production) are fewer than
         *  the docks they can use. Transports and other unarmed aircraft are free.
         */
        if (Advanced_AI_Aircraft_Needs_Dock(airtype)) {
            int aircraft = 0;
            int docks = 0;
            Advanced_AI_Aircraft_Dock_Balance(airtype, aircraft, docks);
            if (aircraft >= docks) continue;
        }

        const int value = teamext->AdvAI_Get_Object_Value_For_Team(airtype, debugprint);
        if (value > highestvalue) {
            mostvaluable = airtype->HeapID;
            highestvalue = value;
        }
    }

    if (mostvaluable != AIRCRAFT_NONE) {
        if (debugprint) {
            DEV_DEBUG_INFO("AF-0009U: house={} selected aircraft {}\n", (int)house->HeapID, AircraftTypes[mostvaluable]->Name());
        }
        house->BuildAircraft = mostvaluable;
    }

    return TICKS_PER_SECOND;
}


/**
 *  Calculates the composition of the enemy forces.
 *
 *  @author: Rampastring
 */
void HouseClassExtension::AdvAI_Calculate_Enemy_Strength()
{
    HouseClass* house = This();

    EnemyNoneStrength = 0;
    EnemyLightStrength = 0;
    EnemyHeavyStrength = 0;
    EnemyArtilleryStrength = 0;
    EnemyBaseDefenseStrength = 0;
    EnemyNavalStrength = 0;
    EnemyAirStrength = 0;
    EnemyAntiGroundStrength.Clear();
    EnemyAntiAirStrength.Clear();
    EnemyAntiNavalStrength.Clear();
    EnemyHarvesterCount = 0;
    EnemyRefineryCount = 0;
    EnemyHasSensors = false;

    for (int i = 0; i < Technos.Count(); i++) {
        TechnoClass* techno = Technos[i];

        if (!Is_Usable(techno) || techno->House == nullptr) continue;

        if (house->Enemy != HOUSE_NONE) {
            if (techno->House->HeapID != house->Enemy) continue;
        } else if (house->Is_Ally(techno->House) || techno->House->Class->IsMultiplayPassive) {
            continue;
        }

        const TechnoTypeClass* technotype = techno->TClass;
        const TechnoTypeClassExtension* technotypeext = Extension::Fetch(technotype);

        if (techno->RTTI == RTTI_BUILDING) {
            const BuildingClass* building = static_cast<const BuildingClass*>(techno);

            if (building->Class->IsRefinery) EnemyRefineryCount++;
            if (building->Class->IsSensorArray) EnemyHasSensors = true;

            if (techno->Is_Weapon_Equipped()) {
                EnemyBaseDefenseStrength += technotype->Cost;
                EnemyAntiAirStrength.Add_Techno_Type_AntiAir(technotypeext);

                if (!technotypeext->IsNaval) {
                    EnemyAntiGroundStrength.Add_Techno_Type_Half_Weight(technotypeext);
                } else {
                    EnemyAntiNavalStrength.Add_Techno_Type_Half_Weight(technotypeext);
                }
            }

            continue;
        }

        if (techno->RTTI == RTTI_UNIT && static_cast<const UnitClass*>(techno)->Class->IsToHarvest) {
            EnemyHarvesterCount++;
            continue;
        }

        if (!techno->Is_Weapon_Equipped()) continue;

        EnemyAntiAirStrength.Add_Techno_Type_AntiAir(technotypeext);

        if (technotypeext->IsNaval) {
            EnemyNavalStrength += technotype->Cost;
            EnemyAntiNavalStrength.Add_Techno_Type(technotypeext);
            continue;
        }

        /**
         *  Aftermath: Flying enemies (aircraft and airborne jumpjets) can only be
         *  countered by anti-aircraft weapons, so they form their own share.
         */
        if (techno->RTTI == RTTI_AIRCRAFT || techno->In_Air()) {
            EnemyAirStrength += technotype->Cost;
            continue;
        }

        EnemyAntiGroundStrength.Add_Techno_Type(technotypeext);

        if (technotype->Armor == ARMOR_NONE) {
            EnemyNoneStrength += technotype->Cost;
        } else if (technotype->Armor == ARMOR_WOOD || technotype->Armor == ARMOR_ALUMINUM || technotype->Armor == (ArmorType)5) {
            EnemyLightStrength += technotype->Cost;
        } else if (technotype->Armor == ARMOR_STEEL || technotype->Armor == ARMOR_CONCRETE) {
            EnemyHeavyStrength += technotype->Cost;
        }

        const WeaponTypeClass* primaryweapon = techno->Get_Primary_Weapon();
        if (primaryweapon != nullptr && primaryweapon->Range >= CELL_LEPTON * 8) {
            EnemyArtilleryStrength += technotype->Cost;
        }
    }
}


static void AdvAI_Commence_Team(TeamClass* team)
{
    team->IsFullStrength = true;
    team->IsHasBeen = true;
    team->IsForcedActive = true;
    Extension::Fetch(team)->OriginalTeamMemberCount = team->Total;
}


/**
 *  Splits a team up if it's too large, then commences all resulting teams.
 *
 *  @author: Rampastring
 */
static void AdvAI_Check_Split_And_Commence_Team(TeamClass* team)
{
    TeamClassExtension* ext = Extension::Fetch(team);
    const int maxsize = std::max(1, RuleExtension->AdvancedAIMaxTeamSize);

    if (!ext->IsAircraftTeam && ext->ProdFlags == PRODFLAG_NONE && !ext->IsTransportTeam && team->Total > maxsize) {

        int numteams = team->Total / maxsize;
        if (team->Total % maxsize == 0) numteams--;

        DEV_DEBUG_INFO("AF-0009U: house={} splitting a team of {} units into {} teams\n", (int)team->House->HeapID, team->Total, numteams + 1);

        for (int i = 0; i < numteams; i++) {
            TeamClass* other = team->Class->Create_One_Of(team->House);
            if (other == nullptr) break;

            Extension::Fetch(other)->Copy_Executive_State_From(ext);

            for (int unitindex = 0; unitindex < maxsize && team->Member != nullptr; unitindex++) {
                if (!other->Add(team->Member)) {
                    DEBUG_WARNING("AF-0009U: Failed to move a unit into a split team.\n");
                    break;
                }
            }

            AdvAI_Commence_Team(other);
        }
    }

    AdvAI_Commence_Team(team);
}


/**
 *  Executes the current tactic when its gathering time is over.
 *
 *  @author: Rampastring
 */
void HouseClassExtension::AdvAI_Commence_Current_Tactic()
{
    HouseClass* house = This();
    TeamClass* team = nullptr;
    int id = 0;

    if (AdvAIGroundTactic.Tactic != TACTIC_NONE && Frame >= AdvAIGroundTactic.EndFrame()) {

        // Direct attack tactics might not be executed at the first opportunity.
        if (house->Enemy != HOUSE_NONE &&
            (AdvAIGroundTactic.Tactic == TACTIC_DIRECT_ATTACK_FAST || AdvAIGroundTactic.Tactic == TACTIC_DIRECT_ATTACK_REGULAR)) {

            // If our teams are very small relative to the enemy, keep accumulating forces,
            // unless our economy is significantly worse - then we need to go all-in.
            const UnitTypeClass* harvester = Rule->HarvesterUnit.Count() > 0 ? house->Get_First_ActLike(Rule->HarvesterUnit) : nullptr;
            const BuildingTypeClass* refinery = Rule->BuildRefinery.Count() > 0 ? house->Get_First_ActLike(Rule->BuildRefinery) : nullptr;
            const int ourharv = harvester ? house->ActiveUQuantity.Value(harvester->HeapID) : 0;
            const int ourref = refinery ? house->ActiveBQuantity.Value(refinery->HeapID) : 0;
            const int ourtotalecon = std::min(ourref * 2, ourharv);
            const int enemytotalecon = std::min(EnemyRefineryCount * 2, EnemyHarvesterCount);

            if (ourtotalecon >= enemytotalecon) {
                int totalcost = 0;
                int artillerystrength = 0;
                bool canstillgrow = false;

                id = 0;
                while ((team = Get_Team_In_Production(id, RTTI_NONE, PRODFLAG_NONE)) != nullptr) {
                    const TeamClassExtension* teamext = Extension::Fetch(team);
                    totalcost += teamext->CurrentCost;
                    artillerystrength += teamext->ArtilleryStrength;

                    // A team at 90% of its production limit cannot grow meaningfully anymore.
                    if (teamext->MaxCost <= 0 || teamext->CurrentCost * 10 < teamext->MaxCost * 9) {
                        canstillgrow = true;
                    }
                }

                int enemytotalstrength = EnemyNoneStrength + EnemyLightStrength + EnemyHeavyStrength;
                if (artillerystrength <= 0) {
                    enemytotalstrength += EnemyBaseDefenseStrength;
                }

                // Require at least 50% of the enemy's strength, otherwise extend the tactic.
                /**
                 *  Aftermath: Only wait while the teams can still grow, and at most
                 *  three times. Otherwise a full team (MaxCost reached) waited
                 *  forever against an enemy stronger than twice its limit.
                 */
                static constexpr int MaxDirectAttackPostponements = 3;
                if (totalcost * 2 < enemytotalstrength && canstillgrow &&
                    AdvAIGroundTacticPostponements < MaxDirectAttackPostponements) {
                    AdvAIGroundTactic.Duration += 1000;
                    AdvAIGroundTacticPostponements++;
                    DEV_DEBUG_INFO("AF-0009U: house={} frame={} postponing direct attack ({}/{}), cost={} enemy={}\n",
                        (int)house->HeapID, Frame, AdvAIGroundTacticPostponements, MaxDirectAttackPostponements,
                        totalcost, enemytotalstrength);
                    return;
                }
            }
        }

        DEV_DEBUG_INFO("AF-0009U: house={} frame={} commencing tactic {}\n", (int)house->HeapID, Frame,
            AdvAITacticType_To_Name(AdvAIGroundTactic.Tactic));

        id = 0;
        while ((team = Get_Team_In_Production(id, RTTI_NONE, PRODFLAG_NONE)) != nullptr) {
            const TeamClassExtension* teamext = Extension::Fetch(team);
            if (!teamext->IsAircraftTeam && (teamext->MinimumReadyFrame <= 0 || Frame >= teamext->MinimumReadyFrame)) {
                AdvAI_Check_Split_And_Commence_Team(team);
            }
        }

        if (AdvAIGroundTactic.Tactic >= TACTIC_SCOUT && AdvAIGroundTactic.Tactic < TACTIC_COUNT) {
            AdvAILastExecutionFrameForTactic[AdvAIGroundTactic.Tactic] = Frame;
        }
        AdvAILastTacticExecutionFrame = Frame;
        AdvAIGroundTactic.Tactic = TACTIC_NONE;
    }

    if (Frame > AdvAIAirTactic.EndFrame()) {
        id = 0;
        while ((team = Get_Team_In_Production(id, RTTI_AIRCRAFT, PRODFLAG_NONE)) != nullptr) {
            const TeamClassExtension* teamext = Extension::Fetch(team);
            if (teamext->IsAircraftTeam && (teamext->MinimumReadyFrame <= 0 || Frame >= teamext->MinimumReadyFrame)) {
                AdvAI_Check_Split_And_Commence_Team(team);
            }
        }
        AdvAIAirTactic.Tactic = AIRTACTIC_NONE;
    }

    if (Frame > AdvAINavalTactic.EndFrame()) {
        id = 0;
        while ((team = Get_Team_In_Production(id, RTTI_NONE, PRODFLAG_NAVAL)) != nullptr) {
            const TeamClassExtension* teamext = Extension::Fetch(team);
            if (teamext->MinimumReadyFrame <= 0 || Frame >= teamext->MinimumReadyFrame) {
                AdvAI_Check_Split_And_Commence_Team(team);
            }
        }
        AdvAINavalTactic.Tactic = NAVALTACTIC_NONE;
    }
}


void HouseClassExtension::AdvAI_Air_Tactic_AI()
{
    HouseClass* house = This();

    if (AdvAIAirTactic.Tactic != AIRTACTIC_NONE || Frame < AdvAIAirTactic.EndFrame()) {

        // If the existing aircraft teams are (nearly) at max cost, commence them.
        bool hasteam = false;
        bool allatmaxcost = true;
        int id = 0;
        TeamClass* team = nullptr;

        while ((team = Get_Team_In_Production(id, RTTI_AIRCRAFT, PRODFLAG_NONE)) != nullptr) {
            const TeamClassExtension* teamext = Extension::Fetch(team);
            if (!teamext->IsAircraftTeam) continue;

            hasteam = true;
            if (teamext->CurrentCost < teamext->MaxCost - 1500) {
                allatmaxcost = false;
                break;
            }
        }

        if (!hasteam) {
            return;
        }

        if (allatmaxcost || !Has_Helipad()) {
            id = 0;
            while ((team = Get_Team_In_Production(id, RTTI_AIRCRAFT, PRODFLAG_NONE)) != nullptr) {
                if (Extension::Fetch(team)->IsAircraftTeam) {
                    AdvAI_Check_Split_And_Commence_Team(team);
                }
            }
            AdvAIAirTactic.Tactic = AIRTACTIC_NONE;
        }

        return;
    }

    // Without a helipad/airfield there is nothing to do here.
    if (!Has_Helipad()) {
        return;
    }

    int chance = 0;
    if (EnemyAntiAirStrength.Total() <= 0) {
        // If our enemy has absolutely no anti-air capability, we need to exploit that.
        chance = 100;
    } else {
        chance = std::max(5, 100 - EnemyAntiAirStrength.Total() / 100);

        const int cutoff = std::max(1, Rule->AIAlternateProductionCreditCutoff);
        if (house->Credits > cutoff * 2) {
            chance *= house->Credits / cutoff;
        } else if (house->Credits < cutoff / 2) {
            chance = chance / 2;
        }
    }

    // If we are significantly outnumbered, it's usually better to focus on ground.
    if (AdvAI_Is_Outnumbered()) {
        chance = chance / 2;
    }

    // If aircraft is the only thing we can build, then we should build it.
    if (!Has_Barracks() && !Has_War_Factory()) {
        chance = 100;
    }

    if (chance >= 100 || Percent_Chance(chance)) {
        for (int i = 0; i < AdvancedAITacticTypes.Count(); i++) {
            if (AdvancedAITacticTypes[i]->IsAir && AdvancedAITacticTypes[i]->Process(house)) {
                return;
            }
        }
    } else {
        Assign_AdvAI_Air_Tactic(AIRTACTIC_NONE, 2000);
    }
}


void HouseClassExtension::AdvAI_Naval_Tactic_AI()
{
    if (!Has_Naval_Yard() || AdvAINavalTactic.Tactic != NAVALTACTIC_NONE) {
        return;
    }

    for (int i = 0; i < AdvancedAITacticTypes.Count(); i++) {
        if (AdvancedAITacticTypes[i]->IsNaval && AdvancedAITacticTypes[i]->Process(This())) {
            return;
        }
    }
}


void HouseClassExtension::AdvAI_Tactic_Selection_AI()
{
    HouseClass* house = This();

    AdvAI_Calculate_Enemy_Strength();
    AdvAI_Commence_Current_Tactic();
    AdvAI_Air_Tactic_AI();
    AdvAI_Naval_Tactic_AI();

    // Don't be too mean against newbies.
    if (house->Difficulty == DIFF_HARD && Frame < 1500) {
        return;
    }

    // On some difficulty levels, wait between tactics to give the player a moment to breathe,
    // but don't allow us to get rushed too easily.
    const int diff = std::clamp((int)house->Difficulty, 0, (int)DIFF_COUNT - 1);
    const int tacticselectiondelay = RuleExtension->AdvancedAITacticSelectionDelay[diff];
    if (!IsUnderStartRushThreat && tacticselectiondelay > 0 && Frame < AdvAILastTacticExecutionFrame + tacticselectiondelay) {
        return;
    }

    if (AdvAIGroundTactic.Tactic != TACTIC_NONE) {
        return;
    }

    if (FactionSpecificTacticalValues[0] == 0) {
        FactionSpecificTacticalValues[0] = Random_Pick(1, 10);
    }

    for (int i = 0; i < AdvancedAITacticTypes.Count(); i++) {
        AdvancedAITacticTypeClass* tactic = AdvancedAITacticTypes[i];
        if (!tactic->Is_Ground()) continue;

        // On naval-only maps, only build a limited number of ground units for defense.
        if (IsNavalOnly == AdvancedAINavalOnlyState::NAVAL_ONLY && tactic->GroundTacticType != TACTIC_DEFEND) continue;

        if (tactic->Process(house)) {
            return;
        }
    }
}


void HouseClassExtension::AdvAI_Team_Recruitment_AI()
{
    int id = 0;
    TeamClass* team = nullptr;

    while ((team = Get_Team_In_Production(id, RTTI_NONE, PRODFLAG_NONE)) != nullptr) {
        Extension::Fetch(team)->AdvAI_Team_Recruit_AI();
    }

    id = 0;
    while ((team = Get_Team_In_Production(id, RTTI_NONE, PRODFLAG_NAVAL)) != nullptr) {
        Extension::Fetch(team)->AdvAI_Team_Recruit_AI();
    }
}


void HouseClassExtension::AdvAI_Team_Maintenance_AI()
{
    for (int i = 0; i < Teams.Count(); i++) {
        TeamClass* team = Teams[i];
        if (team->House != This_Const()) continue;

        TeamClassExtension* teamext = Extension::Fetch(team);
        if (!teamext->Is_AdvAI_Managed()) continue;

        if (!team->IsForcedActive && teamext->IsBiasedForEnemyStrength && !teamext->IsAircraftTeam && teamext->ProdFlags == PRODFLAG_NONE) {
            AdvAI_Set_Ground_Team_Desired_Ratios(team, teamext->AdvAIGroundTacticType);
        }

        teamext->AdvAI_Team_Maintenance_AI();
    }
}


/**
 *  When recently attacked, idle units that are not on an active mission
 *  guard their area and move towards the attacked building.
 *
 *  @author: Rampastring
 */
void HouseClassExtension::AdvAI_Self_Defense_AI()
{
    HouseClass* house = This();

    if (house->LATime + TICKS_PER_MINUTE < Frame) {
        return;
    }

    /**
     *  Aftermath: A dangerous attacker inside the base (for example a hero unit
     *  that already destroyed buildings) is engaged directly. Units that can hit
     *  it, are not on a commenced or scripted team, and are within reach are
     *  sent against it, nearest first, until their combined cost is about twice
     *  the attacker's cost.
     */
    static constexpr long long EngageAttackerDamage = 400;
    static constexpr int EngageMaxDistance = 40 * CELL_LEPTON_W;

    long long attackerdamage = 0;
    TechnoClass* attacker = Advanced_AI_Primary_Base_Attacker(attackerdamage);
    if (attacker != nullptr && attackerdamage >= EngageAttackerDamage) {

        struct Candidate { FootClass* Foot; int Distance; };
        std::vector<Candidate> candidates;

        for (int i = 0; i < Foots.Count(); i++) {
            FootClass* foot = Foots[i];

            if (!Is_Usable(foot) || foot->House != house || !foot->Is_Weapon_Equipped()) continue;
            if (foot->RTTI == RTTI_AIRCRAFT) continue;
            if (foot->Team != nullptr && (foot->Team->IsForcedActive || !Extension::Fetch(foot->Team)->Is_AdvAI_Managed())) continue;

            if (foot->RTTI == RTTI_UNIT) {
                const UnitTypeClass* unittype = static_cast<const UnitClass*>(foot)->Class;
                if (Extension::Fetch(unittype)->IsNaval || unittype->IsToHarvest || unittype->IsToVeinHarvest) continue;
                if (unittype->DeploysInto != nullptr && unittype->DeploysInto->IsConstructionYard) continue;
            }

            if (foot->TarCom == attacker) continue;
            if (!Techno_Can_Hit_Target_Height(foot, attacker)) continue;

            const int distance = foot->Distance_To(attacker);
            if (distance > EngageMaxDistance) continue;

            candidates.push_back(Candidate{ foot, distance });
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.Distance != b.Distance ? a.Distance < b.Distance : a.Foot->ID < b.Foot->ID;
        });

        // Cost already engaged counts towards the response.
        long long committed = 0;
        for (int i = 0; i < Foots.Count(); i++) {
            const FootClass* foot = Foots[i];
            if (foot->House == house && foot->TarCom == attacker) committed += foot->TClass->Cost;
        }

        const long long wanted = std::max(1000LL, 2LL * attacker->TClass->Cost);
        for (size_t i = 0; i < candidates.size() && committed < wanted; i++) {
            FootClass* foot = candidates[i].Foot;
            foot->Assign_Mission(MISSION_ATTACK);
            foot->Assign_Target(attacker);
            foot->Commence();
            committed += foot->TClass->Cost;
        }

        if (!candidates.empty()) {
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} engage-base-attacker={} damage={} candidates={} committed-cost={}\n",
                (int)house->HeapID, Frame, attacker->TClass->Name(), attackerdamage, candidates.size(), committed);
        }
    }

    Cell attackedcell = CELL_NONE;

    // Find a damaged building of ours and assume it to be attacked.
    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* building = Buildings[i];
        if (Is_Usable(building) && building->House == house && building->Strength < (int)building->Class->MaxStrength &&
            !building->Class->IsBaseDefense) {
            attackedcell = building->Center_Coord().As_Cell();
            break;
        }
    }

    /**
     *  Aftermath: If only flying enemies (aircraft, airborne jumpjets) are near the
     *  attacked building, moving ground units there is pointless; only units that
     *  can hit air targets respond.
     */
    const TechnoClass* air_attacker = nullptr;
    if (attackedcell != CELL_NONE) {
        bool ground_enemy = false;
        const Coord center = Map[attackedcell].Center_Coord();
        for (int i = 0; i < Foots.Count(); i++) {
            const FootClass* enemy = Foots[i];
            if (!Is_Usable(enemy) || enemy->House == nullptr || enemy->House == house || house->Is_Ally(enemy->House) ||
                enemy->House->Class->IsMultiplayPassive || !enemy->Is_Weapon_Equipped()) continue;
            if (enemy->Center_Coord().Distance_To(center) > 10 * CELL_LEPTON_W) continue;
            if (enemy->In_Air()) {
                if (air_attacker == nullptr) air_attacker = enemy;
            } else {
                ground_enemy = true;
            }
        }
        if (ground_enemy) air_attacker = nullptr;
    }

    for (int i = 0; i < Foots.Count(); i++) {
        FootClass* foot = Foots[i];

        if (!Is_Usable(foot) || foot->House != house || !foot->Is_Weapon_Equipped()) continue;
        if (foot->NavCom != nullptr) continue;
        // Commenced teams keep their mission. Aftermath: scripted teams are never redirected.
        if (foot->Team != nullptr && (foot->Team->IsForcedActive || !Extension::Fetch(foot->Team)->Is_AdvAI_Managed())) continue;

        if (foot->RTTI == RTTI_AIRCRAFT) {
            continue; // Aftermath: aircraft keep their dock and FixedWing missions.
        }

        if (foot->RTTI == RTTI_UNIT) {
            const UnitTypeClass* unittype = static_cast<const UnitClass*>(foot)->Class;
            if (Extension::Fetch(unittype)->IsNaval || unittype->IsToHarvest || unittype->IsToVeinHarvest) continue;
            if (unittype->DeploysInto != nullptr && unittype->DeploysInto->IsConstructionYard) continue;
        }

        if (foot->Mission == MISSION_GUARD) {
            foot->Assign_Mission(MISSION_GUARD_AREA);
            foot->Commence();
        }

        if (attackedcell != CELL_NONE && foot->Mission == MISSION_GUARD_AREA &&
            (air_attacker == nullptr || Techno_Can_Hit_Target_Height(foot, air_attacker))) {
            const Cell nearbyloc = Map.Nearby_Location(attackedcell, foot->TClass->Speed,
                Map.Get_Cell_Zone(foot->PositionCell, foot->TClass->MZone), foot->TClass->MZone);
            if (nearbyloc != CELL_NONE) {
                foot->Assign_Destination(&Map[nearbyloc]);
            }
        }
    }
}


/**
 *  Detects maps where no enemy can be reached over land.
 *  Aftermath: unlike DTA this does not consult the local radar, which would
 *  differ between multiplayer clients.
 */
void HouseClassExtension::AdvAI_Check_Naval_Only()
{
    HouseClass* house = This();
    LastNavalOnlyCheckFrame = Frame;

    /**
     *  Aftermath: DTA took the zone of the first own techno only. An aircraft
     *  or a jumpjet over water or cliffs gave a zone without enemies, and the
     *  house was stuck on Defend tactics (log 19.11.2026 09-35-25). Only
     *  ground objects count now, and every zone they occupy is checked.
     */
    const auto ground_zone = [](const TechnoClass* techno) {
        if (!Is_Usable(techno) || techno->RTTI == RTTI_AIRCRAFT || techno->In_Air() ||
            Extension::Fetch(techno->TClass)->IsNaval) return -1;
        if (techno->RTTI == RTTI_BUILDING && static_cast<const BuildingClass*>(techno)->Class->IsInvisibleInGame) return -1;
        return Map.Get_Cell_Zone(techno->PositionCell, MZONE_DESTROYER);
    };

    std::vector<int> ourzones;
    for (int i = 0; i < Technos.Count(); i++) {
        const TechnoClass* techno = Technos[i];
        if (techno->House != house) continue;
        const int zone = ground_zone(techno);
        if (zone >= 0 && std::find(ourzones.begin(), ourzones.end(), zone) == ourzones.end()) ourzones.push_back(zone);
    }

    if (ourzones.empty()) {
        return;
    }

    const AdvancedAINavalOnlyState previous = IsNavalOnly;
    IsNavalOnly = AdvancedAINavalOnlyState::NAVAL_ONLY;

    for (int i = 0; i < Technos.Count(); i++) {
        const TechnoClass* techno = Technos[i];
        if (techno->House == nullptr || !techno->TClass->IsLegalTarget ||
            techno->House->Class->IsMultiplayPassive || techno->House->Is_Ally(house)) {
            continue;
        }

        const int zone = ground_zone(techno);
        if (zone >= 0 && std::find(ourzones.begin(), ourzones.end(), zone) != ourzones.end()) {
            IsNavalOnly = AdvancedAINavalOnlyState::NORMAL;
            break;
        }
    }

    if (IsNavalOnly != previous) {
        DEV_DEBUG_INFO("AF-0009U: house={} frame={} naval-only={} zones={}\n", (int)house->HeapID, Frame,
            IsNavalOnly == AdvancedAINavalOnlyState::NAVAL_ONLY ? "yes" : "no", (int)ourzones.size());
    }
}


/**
 *  Sends one idle engineer at a time to capture a nearby neutral building.
 *
 *  @author: Rampastring
 */
void HouseClassExtension::AdvAI_Check_For_Engineer_Capturing_Neutral_Building()
{
    HouseClass* house = This();

    for (int i = 0; i < Infantry.Count(); i++) {
        InfantryClass* infantry = Infantry[i];

        if (!Is_Usable(infantry) || infantry->House != house || !infantry->Class->IsEngineer) continue;
        if (infantry->NavCom != nullptr && infantry->NavCom->RTTI == RTTI_BUILDING) continue;

        /**
         *  Aftermath: leave engineers of scripted teams alone.
         */
        if (infantry->Team != nullptr && !Extension::Fetch(infantry->Team)->Is_AdvAI_Managed()) continue;

        BuildingClass* target = nullptr;
        int shortestdistance = INT_MAX;
        for (int j = 0; j < Buildings.Count(); j++) {
            BuildingClass* building = Buildings[j];
            bool okintheory = false;
            if (Is_Valid_Building_For_Capturing(building, infantry->PositionCell, okintheory)) {
                const int distance = infantry->Distance(building);
                if (distance < shortestdistance) {
                    target = building;
                    shortestdistance = distance;
                }
            }
        }

        if (target == nullptr) continue;

        if (infantry->Team != nullptr) {
            infantry->Team->Remove(infantry);
            infantry->field_205 = false;
            infantry->field_206 = false;
        }

        infantry->Assign_Target(target);
        infantry->Assign_Destination(target);
        infantry->Assign_Mission(MISSION_CAPTURE);
        infantry->Commence();

        const int attemptindex = Get_Building_Capture_Attempt_Index_For(target);
        if (attemptindex > -1) {
            AttemptedBuildingCaptures[attemptindex].Frame = Frame;
        } else {
            Add_Building_Capture_Attempt(target);
        }

        // Only one engineer at a time, or teams with engineers break.
        break;
    }
}


/**
 *  Periodic Advanced AI maintenance, called from the expert AI.
 *
 *  @author: Rampastring (DTA), Aftermath adaptation
 */
void HouseClassExtension::AdvAI_Expert_AI()
{
    if (!Is_Advanced_AI_Unit_Production()) {
        return;
    }

    HouseClass* house = This();

    IsUnderStartRushThreat = AdvAI_Is_Under_Start_Rush_Threat();

    if (RuleExtension->IsAdvancedAISelfDefense) {
        AdvAI_Self_Defense_AI();
    }

    // If our current enemy is passive (Neutral), clear our enemy.
    if (house->Enemy != HOUSE_NONE && Houses[house->Enemy]->Class->IsMultiplayPassive) {
        house->Clear_Anger(Houses[house->Enemy]);
        house->Enemy = HOUSE_NONE;
    }

    if (IsNavalOnly == AdvancedAINavalOnlyState::NOT_CHECKED || Frame > LastNavalOnlyCheckFrame + 5000) {
        AdvAI_Check_Naval_Only();
    }

    // Recruit before selecting tactics: team composition decides whether tactics commence.
    // Scripted tactic teams take the free units they need before the dynamic teams recruit.
    AdvAI_Scripted_Team_AI();
    AdvAI_Team_Recruitment_AI();
    AdvAI_Team_Maintenance_AI();
    AdvAI_Tactic_Selection_AI();

    if (RuleExtension->IsAdvancedAIEngineerCapture && NextEngineerCheckFrame < Frame && NextOilRefineryCaptureCheckFrame < INT_MAX) {
        AdvAI_Check_For_Engineer_Capturing_Neutral_Building();
        NextEngineerCheckFrame = Frame + 500 + Random_Pick(10, 50);
    }

    // Without construction yards and war factories we are unlikely to recover.
    // Send all combat vehicles into hunt mode once.
    if (Frame > 5000 && house->ConstructionYards.Count() == 0 && house->UnitFactories == 0 && !HasPerformedVehicleCharge) {
        HasPerformedVehicleCharge = true;

        for (int i = 0; i < Units.Count(); i++) {
            UnitClass* unit = Units[i];

            if (unit->House == house && !unit->IsInLimbo &&
                (unit->Class->DeploysInto == nullptr || !unit->Class->DeploysInto->IsConstructionYard) &&
                !unit->Class->IsToHarvest && !unit->Class->IsToVeinHarvest &&
                (unit->Team == nullptr || Extension::Fetch(unit->Team)->Is_AdvAI_Managed())) {

                if (unit->Team != nullptr) {
                    unit->Team->Remove(unit);
                }

                unit->Assign_Mission(MISSION_HUNT);
            }
        }
    }
}
