/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Tactic type for the Advanced AI unit production (AF-0009U).
 *
 *          Ported from the Dawn of the Tiberium Age Vinifera build
 *          (Rampastring, branch dta-advaiv2-spawner, commit f5ae74ae) and
 *          adapted for Tiberian Sun: Aftermath:
 *
 *          - House=-1 (default) applies a tactic to every house.
 *          - Built-in default tactics are created when the rules define none.
 *          - Unknown INI values are logged and skipped instead of exiting.
 *          - Section lists are replaced (not appended) when an INI that is
 *            processed again redefines them.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"
#include "advaitactictype.h"
#include "asserthandler.h"
#include "ccini.h"
#include "debughandler.h"
#include "extension.h"
#include "house.h"
#include "housetype.h"
#include "houseext.h"
#include "rules.h"
#include "rulesext.h"
#include "scripttype.h"
#include "taskforce.h"
#include "team.h"
#include "teamext.h"
#include "teamtype.h"
#include "tibsun_functions.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "buildingtype.h"
#include "unittype.h"
#include "vinifera_globals.h"
#include "vinifera_saveload.h"

#include <cstdio>
#include <cstring>


const char* AdvAITacticType_To_Name(AdvAITacticType tactic)
{
    static const char* _names[] = {
        "Scout",
        "Direct Attack (Regular)",
        "Direct Attack (Fast)",
        "Siege",
        "Rush",
        "Attack Harvesters",
        "Attack Refineries",
        "APC Attack",
        "Chinook Attack",
        "Defend"
    };

    if (tactic < TACTIC_SCOUT || tactic >= TACTIC_COUNT) {
        return "None";
    }

    return _names[(int)tactic];
}


AdvancedAITacticTypeClass::AdvancedAITacticTypeClass()
{
    AdvancedAITacticTypes.Add(this);
}


AdvancedAITacticTypeClass::AdvancedAITacticTypeClass(const char* name) :
    IniName(""),
    GroundTacticType(TACTIC_NONE),
    AirTacticType(AIRTACTIC_NONE),
    NavalTacticType(NAVALTACTIC_NONE),
    Conditions(),
    AdvAITeamTypes(),
    ScriptedTeamTypes(),
    MinimumExpectedDuration(-1),
    MaximumExpectedDuration(-1),
    ScaleDurationByBuildTime(false),
    House(HOUSE_NONE),
    SetTacticalValueIndex(-1),
    SetTacticalValueMin(-1),
    SetTacticalValueMax(-1),
    SetTacticalValueChance(100),
    FrameBasedDurationIncrease(0),
    FrameBasedDurationTimeStep(0),
    FrameBasedDurationIncreaseMax(0),
    IsAir(false),
    IsNaval(false),
    MaxInstances(-1),
    IsBuiltIn(false)
{
    ASSERT_FATAL_PRINT(name != nullptr, "Invalid name for AdvancedAITacticType!");

    std::strncpy(IniName, name, sizeof(IniName));
    IniName[sizeof(IniName) - 1] = '\0';

    AdvancedAITacticTypes.Add(this);
}


AdvancedAITacticTypeClass::~AdvancedAITacticTypeClass()
{
    Conditions.Clear();
    AdvAITeamTypes.Clear();
    ScriptedTeamTypes.Clear();
    AdvancedAITacticTypes.Delete(this);
}


LONG AdvancedAITacticTypeClass::QueryInterface(REFIID riid, LPVOID* ppv)
{
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown)) {
        *ppv = reinterpret_cast<IUnknown*>(this);
    }

    if (riid == __uuidof(IStream)) {
        *ppv = reinterpret_cast<IStream*>(this);
    }

    if (riid == __uuidof(IPersistStream)) {
        *ppv = static_cast<IPersistStream*>(this);
    }

    if (*ppv == nullptr) {
        return E_NOINTERFACE;
    }

    reinterpret_cast<IUnknown*>(*ppv)->AddRef();

    return S_OK;
}


ULONG AdvancedAITacticTypeClass::AddRef()
{
    return 1;
}


ULONG AdvancedAITacticTypeClass::Release()
{
    return 1;
}


HRESULT AdvancedAITacticTypeClass::GetClassID(CLSID* lpClassID)
{
    if (lpClassID == nullptr) {
        return E_POINTER;
    }

    *lpClassID = __uuidof(this);

    return S_OK;
}


HRESULT AdvancedAITacticTypeClass::IsDirty()
{
    return S_OK;
}


HRESULT AdvancedAITacticTypeClass::Load(IStream* pStm)
{
    if (!pStm) {
        return E_POINTER;
    }

    Conditions.Clear();
    AdvAITeamTypes.Clear();
    ScriptedTeamTypes.Clear();

    LONG id = 0;
    HRESULT hr = pStm->Read(&id, sizeof(LONG), nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    VINIFERA_SWIZZLE_REGISTER_POINTER(id, this, IniName);

    hr = pStm->Read(this, sizeof(*this), nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    new (this) AdvancedAITacticTypeClass(NoInitClass());

    hr = Load_Primitive_Vector(pStm, Conditions);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Load_Primitive_Vector(pStm, AdvAITeamTypes);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Load_Primitive_Vector(pStm, ScriptedTeamTypes);
    if (FAILED(hr)) {
        return hr;
    }

    return hr;
}


HRESULT AdvancedAITacticTypeClass::Save(IStream* pStm, BOOL fClearDirty)
{
    if (!pStm) {
        return E_POINTER;
    }

    const LONG id = reinterpret_cast<LONG>(this);

    HRESULT hr = pStm->Write(&id, sizeof(id), nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    hr = pStm->Write(this, sizeof(*this), nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Save_Primitive_Vector(pStm, Conditions);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Save_Primitive_Vector(pStm, AdvAITeamTypes);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Save_Primitive_Vector(pStm, ScriptedTeamTypes);
    if (FAILED(hr)) {
        return hr;
    }

    return hr;
}


LONG AdvancedAITacticTypeClass::GetSizeMax(ULARGE_INTEGER* pcbSize)
{
    if (!pcbSize) {
        return E_POINTER;
    }

    pcbSize->LowPart = sizeof(*this) + sizeof(uint32_t);
    pcbSize->HighPart = 0;

    return S_OK;
}


int AdvancedAITacticTypeClass::From_Name(const char* name)
{
    if (name != nullptr) {
        for (int i = 0; i < AdvancedAITacticTypes.Count(); i++) {
            if (std::strncmp(AdvancedAITacticTypes[i]->IniName, name, sizeof(IniName)) == 0) {
                return i;
            }
        }
    }

    return -1;
}


AdvancedAITacticTypeClass* AdvancedAITacticTypeClass::Find_Or_Make(const char* name, bool air, bool naval)
{
    ASSERT(name != nullptr);

    for (int i = 0; i < AdvancedAITacticTypes.Count(); i++) {
        if (std::strncmp(AdvancedAITacticTypes[i]->IniName, name, sizeof(IniName)) == 0) {
            return AdvancedAITacticTypes[i];
        }
    }

    AdvancedAITacticTypeClass* ptr = new AdvancedAITacticTypeClass(name);
    if (ptr != nullptr) {
        ptr->IsAir = air;
        ptr->IsNaval = naval && !air;
    }

    return ptr;
}


/**
 *  Script and team type factories. These create the dynamic team data the
 *  tactics use. All objects carry fixed names, so repeated calls reuse them
 *  and every client of a multiplayer game creates identical objects.
 */
static TaskForceClass* Find_Or_Make_Dummy_TaskForce()
{
    const char* taskforcename = "AFAI_TASKFORCE_DUMMY";

    TaskForceType taskforceid = TaskForceClass::From_Name(taskforcename);
    if (taskforceid != TASKFORCE_NONE) {
        return TaskForces[taskforceid];
    }

    TaskForceClass* newtaskforce = new TaskForceClass(taskforcename);
    if (newtaskforce != nullptr) {
        newtaskforce->ClassCount = 0;
    }
    return newtaskforce;
}


static ScriptTypeClass* Make_Script(const char* scriptname)
{
    ScriptTypeClass* newscripttype = new ScriptTypeClass(scriptname);
    if (newscripttype != nullptr) {
        newscripttype->MissionCount = 0;
    }
    return newscripttype;
}


static ScriptTypeClass* Find_Script(const char* scriptname)
{
    ScriptType scriptid = ScriptTypeClass::From_Name(scriptname);
    if (scriptid != SCRIPT_NONE) {
        return ScriptTypes[scriptid];
    }
    return nullptr;
}


static void Add_Script_Mission(ScriptTypeClass* script, ScriptMissionType mission, int value)
{
    if (script->MissionCount >= ScriptTypeClass::MAX_SCRIPT_MISSIONS) {
        return;
    }
    script->MissionList[script->MissionCount].Mission = mission;
    script->MissionList[script->MissionCount].Data.Value = value;
    script->MissionCount++;
}


static ScriptTypeClass* Find_Or_Make_Hunt_Script()
{
    const char* scriptname = "AFAI_SCRIPT_HUNT";
    if (ScriptTypeClass* script = Find_Script(scriptname)) return script;

    ScriptTypeClass* script = Make_Script(scriptname);
    if (script == nullptr) return nullptr;
    Add_Script_Mission(script, SMISSION_DO, MISSION_HUNT);
    return script;
}


static ScriptTypeClass* Find_Or_Make_Attack_Harvesters_Script()
{
    const char* scriptname = "AFAI_SCRIPT_ATTACK_HARVESTERS";
    if (ScriptTypeClass* script = Find_Script(scriptname)) return script;

    ScriptTypeClass* script = Make_Script(scriptname);
    if (script == nullptr) return nullptr;
    Add_Script_Mission(script, SMISSION_ATTACK, EXT_QUARRY_HARVESTERS);
    Add_Script_Mission(script, SMISSION_ATTACK, QUARRY_VEHICLES);
    Add_Script_Mission(script, SMISSION_ATTACK, QUARRY_ANYTHING);
    return script;
}


/**
 *  Creates a script that attacks enemy buildings of the given kind and then
 *  continues with a final mission.
 */
static ScriptTypeClass* Find_Or_Make_Building_Script(const char* scriptname, bool (*matches)(const BuildingTypeClass*),
    bool transport, ScriptMissionType final_mission, int final_value)
{
    if (ScriptTypeClass* script = Find_Script(scriptname)) return script;

    ScriptTypeClass* script = Make_Script(scriptname);
    if (script == nullptr) return nullptr;

    if (transport) {
        Add_Script_Mission(script, SMISSION_LOAD, 0);
        Add_Script_Mission(script, SMISSION_WAIT_TILL_FULLY_LOADED, 0);
    }

    /**
     *  Reserve room for the closing missions.
     */
    const int reserved = transport ? 2 : 1;
    for (StructType i = STRUCT_FIRST; i < BuildingTypes.Count(); i++) {
        if (script->MissionCount >= ScriptTypeClass::MAX_SCRIPT_MISSIONS - reserved) break;
        if (matches(BuildingTypes[i])) {
            Add_Script_Mission(script, transport ? SMISSION_MOVETO_ENEMY_BUILDING : SMISSION_ATTACK_ENEMY_BUILDING, i);
        }
    }

    if (transport) {
        Add_Script_Mission(script, SMISSION_UNLOAD, 0); // Keep transports, keep units.
    }
    Add_Script_Mission(script, final_mission, final_value);
    return script;
}


static bool Is_Construction_Yard_Type(const BuildingTypeClass* type) { return type->IsConstructionYard; }
static bool Is_Refinery_Type(const BuildingTypeClass* type) { return type->IsRefinery; }

static bool Is_Factory_Type(const BuildingTypeClass* type)
{
    return Rule->BuildWeapons.Is_Present(const_cast<BuildingTypeClass*>(type)) ||
        Rule->BuildConst.Is_Present(const_cast<BuildingTypeClass*>(type)) ||
        Rule->BuildBarracks.Is_Present(const_cast<BuildingTypeClass*>(type)) ||
        Rule->BuildHelipad.Is_Present(const_cast<BuildingTypeClass*>(type));
}


static ScriptTypeClass* Find_Or_Make_Attack_ConYard_Script()
{
    return Find_Or_Make_Building_Script("AFAI_SCRIPT_ATTACK_CONYARDS", Is_Construction_Yard_Type, false, SMISSION_DO, MISSION_HUNT);
}


static ScriptTypeClass* Find_Or_Make_Attack_Refineries_Script()
{
    return Find_Or_Make_Building_Script("AFAI_SCRIPT_ATTACK_REFINERIES", Is_Refinery_Type, false, SMISSION_ATTACK, QUARRY_ANYTHING);
}


static ScriptTypeClass* Find_Or_Make_Attack_Factories_Script()
{
    return Find_Or_Make_Building_Script("AFAI_SCRIPT_ATTACK_FACTORIES", Is_Factory_Type, false, SMISSION_ATTACK, QUARRY_ANYTHING);
}


static ScriptTypeClass* Find_Or_Make_APC_Vs_Refineries_Script()
{
    return Find_Or_Make_Building_Script("AFAI_SCRIPT_APC_VS_REFINERIES", Is_Refinery_Type, true, SMISSION_DO, MISSION_HUNT);
}


static ScriptTypeClass* Find_Or_Make_APC_Vs_ConYards_Script()
{
    return Find_Or_Make_Building_Script("AFAI_SCRIPT_APC_VS_CONYARDS", Is_Construction_Yard_Type, true, SMISSION_ATTACK, QUARRY_ANYTHING);
}


static ScriptTypeClass* Find_Or_Make_Attack_Vehicles_Script()
{
    const char* scriptname = "AFAI_SCRIPT_ATTACK_VEHICLES";
    if (ScriptTypeClass* script = Find_Script(scriptname)) return script;

    ScriptTypeClass* script = Make_Script(scriptname);
    if (script == nullptr) return nullptr;
    Add_Script_Mission(script, SMISSION_ATTACK, QUARRY_VEHICLES);
    Add_Script_Mission(script, SMISSION_ATTACK, QUARRY_ANYTHING);
    return script;
}


static ScriptTypeClass* Find_Or_Make_Attack_Infantry_Script()
{
    const char* scriptname = "AFAI_SCRIPT_ATTACK_INFANTRY";
    if (ScriptTypeClass* script = Find_Script(scriptname)) return script;

    ScriptTypeClass* script = Make_Script(scriptname);
    if (script == nullptr) return nullptr;
    Add_Script_Mission(script, SMISSION_ATTACK, QUARRY_INFANTRY);
    Add_Script_Mission(script, SMISSION_ATTACK, QUARRY_ANYTHING);
    return script;
}


static ScriptTypeClass* Find_Or_Make_Empty_Script()
{
    const char* scriptname = "AFAI_SCRIPT_EMPTY";
    if (ScriptTypeClass* script = Find_Script(scriptname)) return script;

    ScriptTypeClass* script = Make_Script(scriptname);
    if (script == nullptr) return nullptr;
    Add_Script_Mission(script, SMISSION_SCATTER, 0);
    return script;
}


/**
 *  Fetches or creates a dynamic team type of the given name for a house.
 */
static TeamTypeClass* Find_Or_Make_House_TeamType(HouseClass* house, const char* suffix, ScriptTypeClass* script,
    bool suicide, bool avoidthreats, bool onlyenemy)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "AFAI_H%d_%s", (int)house->HeapID, suffix);

    TeamType teamtypeid = TeamTypeClass::From_Name(buffer);
    if (teamtypeid != TEAM_NONE) {
        return TeamTypes[teamtypeid];
    }

    if (script == nullptr) {
        return nullptr;
    }

    TaskForceClass* taskforce = Find_Or_Make_Dummy_TaskForce();
    if (taskforce == nullptr) {
        return nullptr;
    }

    TeamTypeClass* teamtype = TeamTypeClass::Find_Or_Make(buffer);
    if (teamtype == nullptr) {
        return nullptr;
    }

    teamtype->MaxAllowed = -1; // Allow an unlimited number of these.
    teamtype->House = house;
    teamtype->IsSuicide = suicide;
    teamtype->AvoidThreats = avoidthreats;
    teamtype->OnlyTargetHouseEnemy = onlyenemy;
    teamtype->Script = script;
    teamtype->TaskForce = taskforce;
    return teamtype;
}


static TeamTypeClass* Find_Or_Make_Ground_TeamType(HouseClass* house, AdvAITeamType type, bool suicide)
{
    switch (type) {
    case AdvAITeamType::ADVAITT_SCOUT:
        // For scouting, either use hunt teams, or anti-ConYard teams that avoid threats.
        if (Percent_Chance(50)) {
            return Find_Or_Make_House_TeamType(house, "SCOUT_1", Find_Or_Make_Hunt_Script(), false, false, false);
        }
        return Find_Or_Make_House_TeamType(house, "SCOUT_0", Find_Or_Make_Attack_ConYard_Script(), false, true, false);

    case AdvAITeamType::ADVAITT_DIRECT_ATTACK:
        return Find_Or_Make_House_TeamType(house, "DA", Find_Or_Make_Hunt_Script(), false, false, false);

    case AdvAITeamType::ADVAITT_RUSH:
        if (Percent_Chance(50)) {
            return Find_Or_Make_House_TeamType(house, "RUSH_1", Find_Or_Make_Attack_ConYard_Script(), false, true, false);
        }
        return Find_Or_Make_House_TeamType(house, "RUSH_0", Find_Or_Make_Attack_Refineries_Script(), false, true, false);

    case AdvAITeamType::ADVAITT_ATTACK_REFINERIES:
        return Find_Or_Make_House_TeamType(house, suicide ? "ANTI_REF_1" : "ANTI_REF_0",
            Find_Or_Make_Attack_Refineries_Script(), suicide, true, true);

    case AdvAITeamType::ADVAITT_ATTACK_FACTORIES:
        return Find_Or_Make_House_TeamType(house, suicide ? "ANTI_FAC_1" : "ANTI_FAC_0",
            Find_Or_Make_Attack_Factories_Script(), suicide, true, true);

    case AdvAITeamType::ADVAITT_APC_VS_REFINERIES:
        return Find_Or_Make_House_TeamType(house, "APC_VS_REFS", Find_Or_Make_APC_Vs_Refineries_Script(), true, true, true);

    case AdvAITeamType::ADVAITT_APC_VS_FACTORIES:
        return Find_Or_Make_House_TeamType(house, "APC_VS_CYS", Find_Or_Make_APC_Vs_ConYards_Script(), true, true, true);

    case AdvAITeamType::ADVAITT_ATTACK_HARVESTERS:
        return Find_Or_Make_House_TeamType(house, "AH", Find_Or_Make_Attack_Harvesters_Script(), true, true, false);

    case AdvAITeamType::ADVAITT_DEFEND:
        return Find_Or_Make_House_TeamType(house, "DEFEND", Find_Or_Make_Empty_Script(), false, false, false);

    default:
        return nullptr;
    }
}


static TeamTypeClass* Find_Or_Make_Aircraft_TeamType(HouseClass* house, AdvAIAirTacticType tactic, bool suicide)
{
    char suffix[32];
    switch (tactic) {
    case AIRTACTIC_ATTACK_VEHICLES:
        std::snprintf(suffix, sizeof(suffix), "AIR_VS_VEH_%d", suicide);
        return Find_Or_Make_House_TeamType(house, suffix, Find_Or_Make_Attack_Vehicles_Script(), suicide, true, true);
    case AIRTACTIC_ATTACK_INFANTRY:
        std::snprintf(suffix, sizeof(suffix), "AIR_VS_INF_%d", suicide);
        return Find_Or_Make_House_TeamType(house, suffix, Find_Or_Make_Attack_Infantry_Script(), suicide, true, true);
    case AIRTACTIC_ATTACK_HARVESTERS:
        std::snprintf(suffix, sizeof(suffix), "AIR_VS_HARV_%d", suicide);
        return Find_Or_Make_House_TeamType(house, suffix, Find_Or_Make_Attack_Harvesters_Script(), suicide, true, true);
    case AIRTACTIC_ATTACK_REFINERIES:
        std::snprintf(suffix, sizeof(suffix), "AIR_VS_REF_%d", suicide);
        return Find_Or_Make_House_TeamType(house, suffix, Find_Or_Make_Attack_Refineries_Script(), suicide, true, true);
    case AIRTACTIC_ATTACK_FACTORIES:
        std::snprintf(suffix, sizeof(suffix), "AIR_VS_FAC_%d", suicide);
        return Find_Or_Make_House_TeamType(house, suffix, Find_Or_Make_Attack_Factories_Script(), suicide, true, true);
    default:
        return nullptr;
    }
}


static TeamClass* AdvAI_Create_Team(HouseClass* house, AdvancedAITacticTypeClass* tactic, TeamTypeClass* teamtype, RTTIType fortype,
    int maxcost, bool transport, AdvAITacticType forcedtactictype)
{
    if (teamtype == nullptr) {
        DEBUG_WARNING("AF-0009U: AdvAI_Create_Team called with a null team type.\n");
        return nullptr;
    }

    TeamClass* team = teamtype->Create_One_Of(house);
    if (team == nullptr) {
        DEBUG_WARNING("AF-0009U: Failed to create an instance of team type {}.\n", teamtype->IniName);
        return nullptr;
    }

    HouseClassExtension* houseext = Extension::Fetch(house);
    TeamClassExtension* teamext = Extension::Fetch(team);
    teamext->IsAdvAITeam = true;
    teamext->AdvAITactic = tactic;
    teamext->AdvAIGroundTacticType = forcedtactictype != TACTIC_NONE ? forcedtactictype : houseext->AdvAIGroundTactic.Tactic;
    houseext->AdvAI_Set_Ground_Team_Desired_Ratios(team, forcedtactictype);

    /**
     *  Aftermath: A ground team without its own MaxCost stops gathering at a
     *  limit that grows with the game time (3000 + Frame/10, at most 15000).
     *  Otherwise a long direct attack tactic kept buying units without end.
     */
    if (maxcost > 0) {
        teamext->MaxCost = maxcost;
    } else {
        teamext->MaxCost = static_cast<int>(std::min<long>(15000, 3000 + Frame / 10));
    }

    if (transport) {
        teamext->IsTransportTeam = true;
        teamext->MaxInfantry = 5;
    }

    teamext->NoAircraft = true;

    if (fortype == RTTI_INFANTRY || fortype == RTTI_INFANTRYTYPE) {
        teamext->NoVehicles = true;
    } else if (fortype == RTTI_UNIT || fortype == RTTI_UNITTYPE) {
        teamext->NoInfantry = true;
    }

    return team;
}


static TeamClass* AdvAI_Create_Aircraft_Team(HouseClass* house, AdvancedAITacticTypeClass* tactic, TeamTypeClass* teamtype, int maxcost)
{
    if (teamtype == nullptr) {
        DEBUG_WARNING("AF-0009U: AdvAI_Create_Aircraft_Team called with a null team type.\n");
        return nullptr;
    }

    TeamClass* team = teamtype->Create_One_Of(house);
    if (team == nullptr) {
        DEBUG_WARNING("AF-0009U: Failed to create an instance of team type {}.\n", teamtype->IniName);
        return nullptr;
    }

    HouseClassExtension* houseext = Extension::Fetch(house);
    TeamClassExtension* teamext = Extension::Fetch(team);
    teamext->IsAdvAITeam = true;
    teamext->AdvAITactic = tactic;
    teamext->IsAircraftTeam = true;
    teamext->NoInfantry = true;
    teamext->NoVehicles = true;
    teamext->AdvAIGroundTacticType = houseext->AdvAIGroundTactic.Tactic;
    houseext->AdvAI_Set_Aircraft_Team_Desired_Ratios(team);

    if (maxcost > 0) {
        teamext->MaxCost = maxcost;
    }

    return team;
}


static TeamClass* AdvAI_Create_Naval_Team(HouseClass* house, AdvancedAITacticTypeClass* tactic, TeamTypeClass* teamtype, int maxcost)
{
    if (teamtype == nullptr) {
        DEBUG_WARNING("AF-0009U: AdvAI_Create_Naval_Team called with a null team type.\n");
        return nullptr;
    }

    TeamClass* team = teamtype->Create_One_Of(house);
    if (team == nullptr) {
        DEBUG_WARNING("AF-0009U: Failed to create an instance of team type {}.\n", teamtype->IniName);
        return nullptr;
    }

    HouseClassExtension* houseext = Extension::Fetch(house);
    TeamClassExtension* teamext = Extension::Fetch(team);
    teamext->ProdFlags = PRODFLAG_NAVAL;
    teamext->IsAdvAITeam = true;
    teamext->AdvAITactic = tactic;
    teamext->NoAircraft = true;
    teamext->NoInfantry = true;
    teamext->AdvAIGroundTacticType = houseext->AdvAIGroundTactic.Tactic;
    houseext->AdvAI_Set_Naval_Team_Desired_Ratios(team);

    if (maxcost > 0) {
        teamext->MaxCost = maxcost;
    }

    return team;
}


/**
 *  Aftermath: Can the house start a team of the named fixed TeamType now?
 */
static bool Can_Start_Scripted_Team(HouseClass* house, const char* name)
{
    const TeamType id = TeamTypeClass::From_Name(name);
    if (id == TEAM_NONE) {
        DEV_DEBUG_WARNING("AF-0009U: Scripted team type {} does not exist.\n", name);
        return false;
    }

    const TeamTypeClass* teamtype = TeamTypes[id];
    const TaskForceClass* taskforce = teamtype->TaskForce;
    if (taskforce == nullptr || taskforce->ClassCount <= 0) {
        DEV_DEBUG_WARNING("AF-0009U: Scripted team type {} has no TaskForce.\n", name);
        return false;
    }

    if (teamtype->MaxAllowed >= 0) {
        int count = 0;
        for (int i = 0; i < Teams.Count(); i++) {
            if (Teams[i]->Class == teamtype && Teams[i]->House == house) count++;
        }
        if (count >= teamtype->MaxAllowed) return false;
    }

    HouseClassExtension* houseext = Extension::Fetch(house);
    DynamicVectorClass<BuildingTypeClass*> owned;
    houseext->Fill_Owned_Buildings_List(owned);

    for (int i = 0; i < taskforce->ClassCount; i++) {
        const TechnoTypeClass* type = taskforce->Members[i].Class;
        if (type == nullptr || taskforce->Members[i].Quantity <= 0) continue;
        if (!houseext->AdvAI_Can_Produce(type, owned)) {
            return false;
        }
    }

    return true;
}


bool AdvancedAITacticTypeClass::Process(HouseClass* house)
{
    if (House != HOUSE_NONE && house->Class->HeapID != House) {
        return false;
    }

    HouseClassExtension* houseext = Extension::Fetch(house);

    if (MaxInstances > -1) {
        int currentinstances = 0;
        for (int i = 0; i < Teams.Count(); i++) {
            TeamClass* team = Teams[i];
            if (team->House == house && Extension::Fetch(team)->AdvAITactic == this) {
                currentinstances++;
            }
        }

        if (currentinstances >= MaxInstances) {
            return false;
        }
    }

    for (int i = 0; i < Conditions.Count(); i++) {
        const AdvancedAITacticCondition& condition = Conditions[i];

        bool pass = false;

        switch (condition.ConditionType) {
        case AdvAITacticConditionType::TACTICCOND_NONE:
            pass = true;
            break;
        case AdvAITacticConditionType::TACTICCOND_FRAME_LESS_THAN:
            pass = Frame < condition.Parameter1;
            break;
        case AdvAITacticConditionType::TACTICCOND_FRAME_MORE_THAN:
            pass = Frame >= condition.Parameter1;
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_IS_HOUSETYPE:
            pass = house->Enemy != HOUSE_NONE && Houses[house->Enemy]->Class->HeapID == (HousesType)condition.Parameter1;
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_IS_NOT_HOUSETYPE:
            pass = house->Enemy != HOUSE_NONE && Houses[house->Enemy]->Class->HeapID != (HousesType)condition.Parameter1;
            break;
        case AdvAITacticConditionType::TACTICCOND_PERCENT_CHANCE:
            pass = Percent_Chance(condition.Parameter1);
            break;
        case AdvAITacticConditionType::TACTICCOND_HAS_WAR_FACTORY:
            pass = houseext->Has_War_Factory();
            break;
        case AdvAITacticConditionType::TACTICCOND_HAS_NO_WAR_FACTORY:
            pass = !houseext->Has_War_Factory();
            break;
        case AdvAITacticConditionType::TACTICCOND_IS_OUTNUMBERED:
            pass = houseext->AdvAI_Is_Outnumbered();
            break;
        case AdvAITacticConditionType::TACTICCOND_IS_NOT_OUTNUMBERED:
            pass = !houseext->AdvAI_Is_Outnumbered();
            break;
        case AdvAITacticConditionType::TACTICCOND_IS_DISADVANTAGED:
            pass = houseext->AdvAI_Is_Disadvantaged();
            break;
        case AdvAITacticConditionType::TACTICCOND_IS_NOT_DISADVANTAGED:
            pass = !houseext->AdvAI_Is_Disadvantaged();
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_LIGHTLY_DEFENDED_REFINERIES:
            pass = houseext->AdvAI_Enemy_Has_Lightly_Defended_Buildings(Rule->BuildRefinery);
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_VULNERABLE_REFINERIES:
            pass = houseext->AdvAI_Enemy_Has_Vulnerable_Buildings(Rule->BuildRefinery);
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_LIGHTLY_DEFENDED_CONYARDS:
            pass = houseext->AdvAI_Enemy_Has_Lightly_Defended_Buildings(Rule->BuildConst);
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_VULNERABLE_CONYARDS:
            pass = houseext->AdvAI_Enemy_Has_Vulnerable_Buildings(Rule->BuildConst);
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_HAS_SENSORS:
            pass = houseext->EnemyHasSensors;
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_HAS_NO_SENSORS:
            pass = !houseext->EnemyHasSensors;
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_HARVESTER_COUNT_EXCEEDS:
            pass = houseext->EnemyHarvesterCount > condition.Parameter1;
            break;
        case AdvAITacticConditionType::TACTICCOND_NOT_REPEATED_FOR_FRAMES:
            if (IsAir || IsNaval || GroundTacticType < TACTIC_SCOUT || GroundTacticType >= TACTIC_COUNT) {
                pass = true;
            } else {
                pass = Frame > houseext->AdvAILastExecutionFrameForTactic[GroundTacticType] + condition.Parameter1;
            }
            break;
        case AdvAITacticConditionType::TACTICCOND_HOUSE_TACTICAL_VALUE_ABOVE:
            pass = condition.Parameter1 >= 0 && condition.Parameter1 < ADVAI_TACTICAL_VALUE_COUNT &&
                houseext->FactionSpecificTacticalValues[condition.Parameter1] > condition.Parameter2;
            break;
        case AdvAITacticConditionType::TACTICCOND_HOUSE_TACTICAL_VALUE_BELOW:
            pass = condition.Parameter1 >= 0 && condition.Parameter1 < ADVAI_TACTICAL_VALUE_COUNT &&
                houseext->FactionSpecificTacticalValues[condition.Parameter1] < condition.Parameter2;
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_ARTILLERY_STRENGTH_ABOVE:
            pass = houseext->EnemyArtilleryStrength > condition.Parameter1;
            break;
        case AdvAITacticConditionType::TACTICCOND_ENEMY_ANTI_AIR_STRENGTH_BELOW:
            pass = houseext->EnemyAntiAirStrength.Total() < condition.Parameter1;
            break;
        default:
            pass = false;
            break;
        }

        if (!pass) {
            return false;
        }
    }

    /**
     *  Aftermath: Every fixed TeamType must exist, be below its MaxAllowed and
     *  consist of members this house can build. Otherwise the tactic is
     *  skipped like one whose conditions failed.
     */
    for (int i = 0; i < ScriptedTeamTypes.Count(); i++) {
        if (!Can_Start_Scripted_Team(house, ScriptedTeamTypes[i].TeamTypeName)) {
            return false;
        }
    }

    /**
     *  Either there are no conditions or they all passed. Assign the tactic and create its teams.
     */
    int duration = std::max(0, MinimumExpectedDuration);
    if (MaximumExpectedDuration > duration) {
        duration = Random_Pick(duration, MaximumExpectedDuration);
    }

    if (FrameBasedDurationIncrease > 0 && FrameBasedDurationTimeStep > 0) {
        int framebasedduration = FrameBasedDurationIncrease * (Frame / FrameBasedDurationTimeStep);
        if (FrameBasedDurationIncreaseMax > 0) {
            framebasedduration = std::min(FrameBasedDurationIncreaseMax, framebasedduration);
        }
        duration += framebasedduration;
    }

    if (ScaleDurationByBuildTime) {
        duration = (int)(duration * house->BuildSpeedBias);
    }

    DEV_DEBUG_INFO("AF-0009U: house={} frame={} tactic={} duration={}\n", (int)house->HeapID, Frame, IniName, duration);

    if (IsAir) {
        houseext->Assign_AdvAI_Air_Tactic(AirTacticType, duration);
    } else if (IsNaval) {
        houseext->Assign_AdvAI_Naval_Tactic(NavalTacticType, duration);
    } else {
        houseext->Assign_AdvAI_Tactic(GroundTacticType, duration);
    }

    for (int i = 0; i < AdvAITeamTypes.Count(); i++) {
        const AdvancedAITacticTeam& advaiteamtype = AdvAITeamTypes[i];

        if (IsAir) {
            TeamTypeClass* teamtype = Find_Or_Make_Aircraft_TeamType(house, AirTacticType, advaiteamtype.IsSuicide);
            if (teamtype == nullptr) {
                DEBUG_WARNING("AF-0009U: Tactic {} has no valid air tactic type.\n", IniName);
                continue;
            }
            AdvAI_Create_Aircraft_Team(house, this, teamtype, advaiteamtype.MaxCost);

        } else if (IsNaval) {
            if (advaiteamtype.TeamType != AdvAITeamType::ADVAITT_DIRECT_ATTACK) {
                DEBUG_WARNING("AF-0009U: Tactic {} uses an unsupported naval team type.\n", IniName);
                continue;
            }
            TeamTypeClass* teamtype = Find_Or_Make_House_TeamType(house, "NAVAL_DA", Find_Or_Make_Hunt_Script(), false, false, false);
            AdvAI_Create_Naval_Team(house, this, teamtype, advaiteamtype.MaxCost);

        } else {
            TeamTypeClass* teamtype = Find_Or_Make_Ground_TeamType(house, advaiteamtype.TeamType, advaiteamtype.IsSuicide);
            if (teamtype == nullptr) {
                DEBUG_WARNING("AF-0009U: Tactic {} uses an unknown team type.\n", IniName);
                continue;
            }
            AdvAI_Create_Team(house, this, teamtype, advaiteamtype.LimitToRTTI, advaiteamtype.MaxCost,
                advaiteamtype.IsTransportTeam, advaiteamtype.ForcedTacticType);
        }
    }

    for (int i = 0; i < ScriptedTeamTypes.Count(); i++) {
        const AdvancedAITacticScriptedTeam& scripted = ScriptedTeamTypes[i];
        const TeamType id = TeamTypeClass::From_Name(scripted.TeamTypeName);
        TeamClass* team = id != TEAM_NONE ? TeamTypes[id]->Create_One_Of(house) : nullptr;
        if (team == nullptr) {
            DEBUG_WARNING("AF-0009U: Tactic {} failed to create scripted team {}.\n", IniName, scripted.TeamTypeName);
            continue;
        }

        TeamClassExtension* teamext = Extension::Fetch(team);
        teamext->IsAdvAIScriptedTeam = true;
        teamext->AdvAITactic = this;
        teamext->AdvAIScriptedDeadline = scripted.MaxWait > 0 ? Frame + scripted.MaxWait : 0;

        DEV_DEBUG_INFO("AF-0009U: house={} frame={} tactic={} scripted-team={} max-wait={}\n", (int)house->HeapID, Frame,
            IniName, scripted.TeamTypeName, scripted.MaxWait);
    }

    if (SetTacticalValueIndex > -1 && SetTacticalValueIndex < ADVAI_TACTICAL_VALUE_COUNT &&
        (SetTacticalValueChance >= 100 || Percent_Chance(SetTacticalValueChance))) {

        int value = SetTacticalValueMin;
        if (SetTacticalValueMax > value) {
            value = Random_Pick(value, SetTacticalValueMax);
        }

        houseext->FactionSpecificTacticalValues[SetTacticalValueIndex] = value;
    }

    return true;
}


AdvAITacticType AdvancedAITacticTypeClass::Get_AdvAI_Ground_Tactic_Type(const char* buffer)
{
    if (buffer == nullptr || !strcasecmp(buffer, "<none>") || !strcasecmp(buffer, "none")) {
        return TACTIC_NONE;
    }

    static const char* _names[] = {
        "TACTIC_SCOUT", "TACTIC_DIRECT_ATTACK_REGULAR", "TACTIC_DIRECT_ATTACK_FAST", "TACTIC_SIEGE",
        "TACTIC_RUSH_ATTACK", "TACTIC_ATTACK_HARVESTERS", "TACTIC_ATTACK_REFINERIES", "TACTIC_APC_ATTACK",
        "TACTIC_CHINOOK_ATTACK", "TACTIC_DEFEND"
    };

    for (int i = 0; i < (int)std::size(_names); i++) {
        if (!strcasecmp(_names[i], buffer)) {
            return (AdvAITacticType)i;
        }
    }

    DEBUG_WARNING("AF-0009U: Unknown ground tactic type {}.\n", buffer);
    return TACTIC_NONE;
}


AdvAIAirTacticType AdvancedAITacticTypeClass::Get_AdvAI_Air_Tactic_Type(const char* buffer)
{
    if (buffer == nullptr || !strcasecmp(buffer, "<none>") || !strcasecmp(buffer, "none")) {
        return AIRTACTIC_NONE;
    }

    static const char* _names[] = {
        "AIRTACTIC_ATTACK_VEHICLES", "AIRTACTIC_ATTACK_INFANTRY", "AIRTACTIC_ATTACK_HARVESTERS",
        "AIRTACTIC_ATTACK_REFINERIES", "AIRTACTIC_ATTACK_FACTORIES"
    };

    for (int i = 0; i < (int)std::size(_names); i++) {
        if (!strcasecmp(_names[i], buffer)) {
            return (AdvAIAirTacticType)i;
        }
    }

    DEBUG_WARNING("AF-0009U: Unknown air tactic type {}.\n", buffer);
    return AIRTACTIC_NONE;
}


AdvAINavalTacticType AdvancedAITacticTypeClass::Get_AdvAI_Naval_Tactic_Type(const char* buffer)
{
    if (buffer == nullptr || !strcasecmp(buffer, "<none>") || !strcasecmp(buffer, "none")) {
        return NAVALTACTIC_NONE;
    }

    if (!strcasecmp("NAVALTACTIC_DIRECT_ATTACK", buffer)) {
        return NAVALTACTIC_DIRECT_ATTACK;
    }

    DEBUG_WARNING("AF-0009U: Unknown naval tactic type {}.\n", buffer);
    return NAVALTACTIC_NONE;
}


static AdvAITacticConditionType Get_AdvAI_Tactic_Condition_Type(const char* buffer)
{
    if (buffer == nullptr || !strcasecmp(buffer, "<none>") || !strcasecmp(buffer, "none") || !strcasecmp(buffer, "TACTICCOND_NONE")) {
        return AdvAITacticConditionType::TACTICCOND_NONE;
    }

    static const char* _names[] = {
        "TACTICCOND_FRAME_LESS_THAN", "TACTICCOND_FRAME_MORE_THAN", "TACTICCOND_ENEMY_IS_HOUSETYPE",
        "TACTICCOND_ENEMY_IS_NOT_HOUSETYPE", "TACTICCOND_PERCENT_CHANCE", "TACTICCOND_HAS_WAR_FACTORY",
        "TACTICCOND_HAS_NO_WAR_FACTORY", "TACTICCOND_IS_OUTNUMBERED", "TACTICCOND_IS_NOT_OUTNUMBERED",
        "TACTICCOND_IS_DISADVANTAGED", "TACTICCOND_IS_NOT_DISADVANTAGED", "TACTICCOND_ENEMY_LIGHTLY_DEFENDED_REFINERIES",
        "TACTICCOND_ENEMY_VULNERABLE_REFINERIES", "TACTICCOND_ENEMY_LIGHTLY_DEFENDED_CONYARDS",
        "TACTICCOND_ENEMY_VULNERABLE_CONYARDS", "TACTICCOND_ENEMY_HAS_SENSORS", "TACTICCOND_ENEMY_HAS_NO_SENSORS",
        "TACTICCOND_ENEMY_HARVESTER_COUNT_EXCEEDS", "TACTICCOND_NOT_REPEATED_FOR_FRAMES",
        "TACTICCOND_HOUSE_TACTICAL_VALUE_ABOVE", "TACTICCOND_HOUSE_TACTICAL_VALUE_BELOW",
        "TACTICCOND_ENEMY_ARTILLERY_STRENGTH_ABOVE", "TACTICCOND_ENEMY_ANTI_AIR_STRENGTH_BELOW"
    };

    for (int i = 0; i < (int)std::size(_names); i++) {
        if (!strcasecmp(_names[i], buffer)) {
            return (AdvAITacticConditionType)i;
        }
    }

    DEBUG_WARNING("AF-0009U: Unknown tactic condition type {}.\n", buffer);
    return AdvAITacticConditionType::TACTICCOND_COUNT;
}


static AdvAITeamType Get_AdvAI_Team_Type(const char* buffer)
{
    if (buffer == nullptr || !strcasecmp(buffer, "<none>") || !strcasecmp(buffer, "none")) {
        return AdvAITeamType::ADVAITT_NONE;
    }

    static const char* _names[] = {
        "ADVAITT_SCOUT", "ADVAITT_DIRECT_ATTACK", "ADVAITT_RUSH", "ADVAITT_ATTACK_REFINERIES",
        "ADVAITT_ATTACK_FACTORIES", "ADVAITT_APC_VS_REFINERIES", "ADVAITT_APC_VS_FACTORIES",
        "ADVAITT_ATTACK_HARVESTERS", "ADVAITT_DEFEND"
    };

    for (int i = 0; i < (int)std::size(_names); i++) {
        if (!strcasecmp(_names[i], buffer)) {
            return (AdvAITeamType)i;
        }
    }

    DEBUG_WARNING("AF-0009U: Unknown team type {}.\n", buffer);
    return AdvAITeamType::ADVAITT_NONE;
}


static int Next_Token_Int(const char* delimiter)
{
    const char* token = std::strtok(nullptr, delimiter);
    return token ? std::atoi(token) : 0;
}


bool AdvancedAITacticTypeClass::Read_INI(CCINIClass& ini)
{
    if (!ini.Is_Present(IniName)) {
        return false;
    }

    char keybuffer[128];
    char outputbuffer[1024];
    const char* delimiter = ",";

    /**
     *  Conditions.N=ConditionType,Parameter1[,Parameter2]
     *  A redefinition replaces the previous list.
     */
    if (ini.Is_Present(IniName, "Conditions.0")) {
        Conditions.Clear();
    }

    for (int i = 0; ; i++) {
        std::snprintf(keybuffer, sizeof(keybuffer), "Conditions.%d", i);
        if (ini.Get_String(IniName, keybuffer, "", outputbuffer, sizeof(outputbuffer)) <= 0) break;

        AdvancedAITacticCondition condition{};
        condition.ConditionType = Get_AdvAI_Tactic_Condition_Type(std::strtok(outputbuffer, delimiter));
        condition.Parameter1 = Next_Token_Int(delimiter);
        condition.Parameter2 = 0;
        if (condition.ConditionType == AdvAITacticConditionType::TACTICCOND_HOUSE_TACTICAL_VALUE_ABOVE ||
            condition.ConditionType == AdvAITacticConditionType::TACTICCOND_HOUSE_TACTICAL_VALUE_BELOW) {
            condition.Parameter2 = Next_Token_Int(delimiter);
        }

        /**
         *  An unknown condition never passes, matching DTA where it was fatal.
         */
        Conditions.Add(condition);
    }

    /**
     *  AdvAITeamTypes.N=TeamType,IsSuicide,IsTransportTeam,AvoidThreats,MaxCost,LimitToRTTI,TacticTypeOverride
     */
    if (ini.Is_Present(IniName, "AdvAITeamTypes.0") || ini.Is_Present(IniName, "AdvAIAircraftTeamType")) {
        AdvAITeamTypes.Clear();
    }

    for (int i = 0; ; i++) {
        std::snprintf(keybuffer, sizeof(keybuffer), "AdvAITeamTypes.%d", i);
        if (ini.Get_String(IniName, keybuffer, "", outputbuffer, sizeof(outputbuffer)) <= 0) break;

        AdvancedAITacticTeam advaiteamtype{};
        advaiteamtype.TeamType = Get_AdvAI_Team_Type(std::strtok(outputbuffer, delimiter));
        advaiteamtype.IsSuicide = Next_Token_Int(delimiter) > 0;
        advaiteamtype.IsTransportTeam = Next_Token_Int(delimiter) > 0;
        advaiteamtype.AvoidThreats = Next_Token_Int(delimiter) > 0;
        advaiteamtype.MaxCost = Next_Token_Int(delimiter);
        const char* rtti = std::strtok(nullptr, delimiter);
        advaiteamtype.LimitToRTTI = rtti ? RTTI_From_Name(rtti) : RTTI_NONE;
        advaiteamtype.ForcedTacticType = Get_AdvAI_Ground_Tactic_Type(std::strtok(nullptr, delimiter));

        AdvAITeamTypes.Add(advaiteamtype);
    }

    /**
     *  Only suicide and max cost are relevant for aircraft team types.
     */
    if (IsAir && ini.Get_String(IniName, "AdvAIAircraftTeamType", "", outputbuffer, sizeof(outputbuffer)) > 0) {
        AdvancedAITacticTeam advaiteamtype{};
        const char* token = std::strtok(outputbuffer, delimiter);
        advaiteamtype.TeamType = AdvAITeamType::ADVAITT_NONE;
        advaiteamtype.IsSuicide = token ? std::atoi(token) > 0 : false;
        advaiteamtype.IsTransportTeam = false;
        advaiteamtype.AvoidThreats = true;
        advaiteamtype.MaxCost = Next_Token_Int(delimiter);
        advaiteamtype.LimitToRTTI = RTTI_AIRCRAFT;
        advaiteamtype.ForcedTacticType = TACTIC_NONE;

        AdvAITeamTypes.Add(advaiteamtype);
    }

    /**
     *  Aftermath: ScriptedTeamTypes.N=TeamTypeName[,MaxWait]
     *  Fixed TeamTypes (TaskForce + Script) the tactic starts as native teams.
     *  MaxWait defaults to 6000 frames; 0 waits until the team is complete.
     */
    if (ini.Is_Present(IniName, "ScriptedTeamTypes.0")) {
        ScriptedTeamTypes.Clear();
    }

    for (int i = 0; ; i++) {
        std::snprintf(keybuffer, sizeof(keybuffer), "ScriptedTeamTypes.%d", i);
        if (ini.Get_String(IniName, keybuffer, "", outputbuffer, sizeof(outputbuffer)) <= 0) break;

        AdvancedAITacticScriptedTeam scripted{};
        const char* name = std::strtok(outputbuffer, delimiter);
        if (name == nullptr) continue;
        while (*name == ' ' || *name == '\t') name++;
        std::strncpy(scripted.TeamTypeName, name, sizeof(scripted.TeamTypeName));
        scripted.TeamTypeName[sizeof(scripted.TeamTypeName) - 1] = '\0';
        for (int end = (int)std::strlen(scripted.TeamTypeName) - 1; end >= 0 &&
            (scripted.TeamTypeName[end] == ' ' || scripted.TeamTypeName[end] == '\t'); end--) {
            scripted.TeamTypeName[end] = '\0';
        }
        const char* wait = std::strtok(nullptr, delimiter);
        scripted.MaxWait = wait ? std::max(0, std::atoi(wait)) : 6000;

        ScriptedTeamTypes.Add(scripted);
    }

    if (ini.Get_String(IniName, "PrimaryTacticType", "", outputbuffer, sizeof(outputbuffer)) > 0) {
        if (IsAir) {
            AirTacticType = Get_AdvAI_Air_Tactic_Type(outputbuffer);
        } else if (IsNaval) {
            NavalTacticType = Get_AdvAI_Naval_Tactic_Type(outputbuffer);
        } else {
            GroundTacticType = Get_AdvAI_Ground_Tactic_Type(outputbuffer);
        }
    }

    MinimumExpectedDuration = ini.Get_Int(IniName, "MinimumExpectedDuration", MinimumExpectedDuration);
    MaximumExpectedDuration = ini.Get_Int(IniName, "MaximumExpectedDuration", std::max(MaximumExpectedDuration, MinimumExpectedDuration));
    ScaleDurationByBuildTime = ini.Get_Bool(IniName, "ScaleDurationByBuildTime", ScaleDurationByBuildTime);
    House = (HousesType)ini.Get_Int(IniName, "House", (int)House);
    SetTacticalValueIndex = ini.Get_Int(IniName, "SetTacticalValueIndex", SetTacticalValueIndex);
    SetTacticalValueMin = ini.Get_Int(IniName, "SetTacticalValueMin", SetTacticalValueMin);
    SetTacticalValueMax = ini.Get_Int(IniName, "SetTacticalValueMax", SetTacticalValueMax);
    SetTacticalValueChance = ini.Get_Int(IniName, "SetTacticalValueChance", SetTacticalValueChance);
    FrameBasedDurationIncrease = ini.Get_Int(IniName, "FrameBasedDurationIncrease", FrameBasedDurationIncrease);
    FrameBasedDurationTimeStep = ini.Get_Int(IniName, "FrameBasedDurationTimeStep", FrameBasedDurationTimeStep);
    FrameBasedDurationIncreaseMax = ini.Get_Int(IniName, "FrameBasedDurationIncreaseMax", FrameBasedDurationIncreaseMax);
    MaxInstances = ini.Get_Int(IniName, "MaxInstances", MaxInstances);

    IsBuiltIn = false;

    return true;
}


/**
 *  Helpers for building the default tactic set.
 */
static AdvancedAITacticTypeClass* Make_Default(const char* name, bool air, bool naval, int mindur, int maxdur)
{
    if (AdvancedAITacticTypeClass::From_Name(name) != -1) {
        return nullptr;
    }

    AdvancedAITacticTypeClass* tactic = AdvancedAITacticTypeClass::Find_Or_Make(name, air, naval);
    if (tactic != nullptr) {
        tactic->MinimumExpectedDuration = mindur;
        tactic->MaximumExpectedDuration = maxdur;
        tactic->IsBuiltIn = true;
    }
    return tactic;
}


static void Add_Condition(AdvancedAITacticTypeClass* tactic, AdvAITacticConditionType type, int p1 = 0, int p2 = 0)
{
    AdvancedAITacticTypeClass::AdvancedAITacticCondition condition{};
    condition.ConditionType = type;
    condition.Parameter1 = p1;
    condition.Parameter2 = p2;
    tactic->Conditions.Add(condition);
}


static void Add_Team(AdvancedAITacticTypeClass* tactic, AdvAITeamType type, bool suicide, int maxcost, RTTIType limit = RTTI_NONE,
    bool transport = false)
{
    AdvancedAITacticTypeClass::AdvancedAITacticTeam team{};
    team.ForcedTacticType = TACTIC_NONE;
    team.TeamType = type;
    team.IsSuicide = suicide;
    team.IsTransportTeam = transport;
    team.AvoidThreats = suicide;
    team.MaxCost = maxcost;
    team.LimitToRTTI = limit;
    tactic->AdvAITeamTypes.Add(team);
}


/**
 *  Creates the built-in Aftermath tactic set for every category that the
 *  rules leave empty, or after the custom tactics with
 *  AdvancedAIKeepDefaultTactics=yes. Evaluation order is the creation order; the first
 *  tactic whose conditions pass is adopted. All tactics apply to every house.
 */
void AdvancedAITacticTypeClass::Create_Defaults()
{
    bool hasground = false;
    bool hasair = false;
    bool hasnaval = false;

    for (int i = 0; i < AdvancedAITacticTypes.Count(); i++) {
        const AdvancedAITacticTypeClass* tactic = AdvancedAITacticTypes[i];
        hasground |= tactic->Is_Ground();
        hasair |= tactic->IsAir;
        hasnaval |= tactic->IsNaval;
    }

    using C = AdvAITacticConditionType;

    // Aftermath: AdvancedAIKeepDefaultTactics=yes appends the defaults after custom tactics.
    const bool keep = RuleExtension->IsAdvancedAIKeepDefaultTactics;

    if (!hasground || keep) {
        AdvancedAITacticTypeClass* t;

        if ((t = Make_Default("AFAI_Defend", false, false, 900, 1500)) != nullptr) {
            t->GroundTacticType = TACTIC_DEFEND;
            t->MaxInstances = 1;
            Add_Condition(t, C::TACTICCOND_FRAME_MORE_THAN, 3000);
            Add_Condition(t, C::TACTICCOND_IS_OUTNUMBERED);
            Add_Condition(t, C::TACTICCOND_NOT_REPEATED_FOR_FRAMES, 4000);
            Add_Team(t, AdvAITeamType::ADVAITT_DEFEND, false, 4000);
        }

        if ((t = Make_Default("AFAI_Rush", false, false, 1200, 2000)) != nullptr) {
            t->GroundTacticType = TACTIC_RUSH_ATTACK;
            t->ScaleDurationByBuildTime = true;
            Add_Condition(t, C::TACTICCOND_FRAME_LESS_THAN, 9000);
            Add_Condition(t, C::TACTICCOND_NOT_REPEATED_FOR_FRAMES, 9000);
            Add_Condition(t, C::TACTICCOND_PERCENT_CHANCE, 20);
            Add_Team(t, AdvAITeamType::ADVAITT_RUSH, false, 2500);
        }

        if ((t = Make_Default("AFAI_Scout", false, false, 300, 600)) != nullptr) {
            t->GroundTacticType = TACTIC_SCOUT;
            t->MaxInstances = 1;
            Add_Condition(t, C::TACTICCOND_FRAME_LESS_THAN, 6000);
            Add_Condition(t, C::TACTICCOND_NOT_REPEATED_FOR_FRAMES, 6000);
            Add_Condition(t, C::TACTICCOND_PERCENT_CHANCE, 50);
            Add_Team(t, AdvAITeamType::ADVAITT_SCOUT, false, 800);
        }

        if ((t = Make_Default("AFAI_AttackHarvesters", false, false, 1000, 1800)) != nullptr) {
            t->GroundTacticType = TACTIC_ATTACK_HARVESTERS;
            Add_Condition(t, C::TACTICCOND_FRAME_MORE_THAN, 6000);
            Add_Condition(t, C::TACTICCOND_HAS_WAR_FACTORY);
            Add_Condition(t, C::TACTICCOND_ENEMY_HARVESTER_COUNT_EXCEEDS, 1);
            Add_Condition(t, C::TACTICCOND_NOT_REPEATED_FOR_FRAMES, 6000);
            Add_Condition(t, C::TACTICCOND_PERCENT_CHANCE, 25);
            Add_Team(t, AdvAITeamType::ADVAITT_ATTACK_HARVESTERS, true, 3000, RTTI_UNIT);
        }

        if ((t = Make_Default("AFAI_AttackRefineries", false, false, 1500, 2500)) != nullptr) {
            t->GroundTacticType = TACTIC_ATTACK_REFINERIES;
            Add_Condition(t, C::TACTICCOND_ENEMY_LIGHTLY_DEFENDED_REFINERIES);
            Add_Condition(t, C::TACTICCOND_NOT_REPEATED_FOR_FRAMES, 7000);
            Add_Condition(t, C::TACTICCOND_PERCENT_CHANCE, 35);
            Add_Team(t, AdvAITeamType::ADVAITT_ATTACK_REFINERIES, false, 4000);
        }

        if ((t = Make_Default("AFAI_DirectAttackFast", false, false, 1800, 3000)) != nullptr) {
            t->GroundTacticType = TACTIC_DIRECT_ATTACK_FAST;
            t->FrameBasedDurationIncrease = 250;
            t->FrameBasedDurationTimeStep = 5000;
            t->FrameBasedDurationIncreaseMax = 2500;
            Add_Condition(t, C::TACTICCOND_HAS_WAR_FACTORY);
            Add_Condition(t, C::TACTICCOND_NOT_REPEATED_FOR_FRAMES, 5000);
            Add_Condition(t, C::TACTICCOND_PERCENT_CHANCE, 35);
            Add_Team(t, AdvAITeamType::ADVAITT_DIRECT_ATTACK, false, 0);
        }

        if ((t = Make_Default("AFAI_DirectAttack", false, false, 2500, 4000)) != nullptr) {
            t->GroundTacticType = TACTIC_DIRECT_ATTACK_REGULAR;
            t->ScaleDurationByBuildTime = true;
            t->FrameBasedDurationIncrease = 300;
            t->FrameBasedDurationTimeStep = 5000;
            t->FrameBasedDurationIncreaseMax = 3000;
            Add_Team(t, AdvAITeamType::ADVAITT_DIRECT_ATTACK, false, 0);
        }

        DEBUG_INFO("AF-0009U: Ground tactics: built-in defaults added.\n");
    }

    if (!hasair || keep) {
        AdvancedAITacticTypeClass* t;

        if ((t = Make_Default("AFAI_AirHarvesters", true, false, 1500, 2500)) != nullptr) {
            t->AirTacticType = AIRTACTIC_ATTACK_HARVESTERS;
            Add_Condition(t, C::TACTICCOND_ENEMY_HARVESTER_COUNT_EXCEEDS, 1);
            Add_Condition(t, C::TACTICCOND_PERCENT_CHANCE, 30);
            Add_Team(t, AdvAITeamType::ADVAITT_NONE, false, 3000, RTTI_AIRCRAFT);
        }

        if ((t = Make_Default("AFAI_AirVehicles", true, false, 2000, 3000)) != nullptr) {
            t->AirTacticType = AIRTACTIC_ATTACK_VEHICLES;
            Add_Team(t, AdvAITeamType::ADVAITT_NONE, false, 4000, RTTI_AIRCRAFT);
        }

        DEBUG_INFO("AF-0009U: Air tactics: built-in defaults added.\n");
    }

    if (!hasnaval || keep) {
        AdvancedAITacticTypeClass* t;

        if ((t = Make_Default("AFAI_NavalAttack", false, true, 2500, 4000)) != nullptr) {
            t->NavalTacticType = NAVALTACTIC_DIRECT_ATTACK;
            Add_Team(t, AdvAITeamType::ADVAITT_DIRECT_ATTACK, false, 6000);
        }

        DEBUG_INFO("AF-0009U: Naval tactics: built-in defaults added.\n");
    }
}
