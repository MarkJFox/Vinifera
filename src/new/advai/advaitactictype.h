/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Tactic type for the Advanced AI unit production (AF-0009U).
 *
 *          Ported from the Dawn of the Tiberium Age Vinifera build
 *          (Rampastring, branch dta-advaiv2-spawner) and adapted for
 *          Tiberian Sun: Aftermath.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#pragma once

#include "always.h"
#include "tibsun_defines.h"
#include "vinifera_defines.h"
#include "vector.h"

#include <objidl.h>
#include <cstring>


class CCINIClass;
class HouseClass;


enum AdvAIAirTacticType
{
    AIRTACTIC_NONE = -1,
    AIRTACTIC_ATTACK_VEHICLES = 0,
    AIRTACTIC_ATTACK_INFANTRY = 1,
    AIRTACTIC_ATTACK_HARVESTERS = 2,
    AIRTACTIC_ATTACK_REFINERIES = 3,
    AIRTACTIC_ATTACK_FACTORIES = 4
};
DEFINE_ENUMERATION_OPERATORS(AdvAIAirTacticType);


enum AdvAINavalTacticType
{
    NAVALTACTIC_NONE = -1,
    NAVALTACTIC_DIRECT_ATTACK = 0
};
DEFINE_ENUMERATION_OPERATORS(AdvAINavalTacticType);


enum AdvAITacticType
{
    TACTIC_NONE = -1,
    TACTIC_SCOUT = 0,
    TACTIC_DIRECT_ATTACK_REGULAR,
    TACTIC_DIRECT_ATTACK_FAST,
    TACTIC_SIEGE,
    TACTIC_RUSH_ATTACK,
    TACTIC_ATTACK_HARVESTERS,
    TACTIC_ATTACK_REFINERIES,
    TACTIC_APC_ATTACK,
    TACTIC_CHINOOK_ATTACK,
    TACTIC_DEFEND,
    TACTIC_COUNT
};
DEFINE_ENUMERATION_OPERATORS(AdvAITacticType);


const char* AdvAITacticType_To_Name(AdvAITacticType tactic);


enum class AdvAITacticConditionType
{
    TACTICCOND_NONE = -1,
    TACTICCOND_FRAME_LESS_THAN = 0,
    TACTICCOND_FRAME_MORE_THAN = 1,
    TACTICCOND_ENEMY_IS_HOUSETYPE = 2,
    TACTICCOND_ENEMY_IS_NOT_HOUSETYPE = 3,
    TACTICCOND_PERCENT_CHANCE = 4,
    TACTICCOND_HAS_WAR_FACTORY = 5,
    TACTICCOND_HAS_NO_WAR_FACTORY = 6,
    TACTICCOND_IS_OUTNUMBERED = 7,
    TACTICCOND_IS_NOT_OUTNUMBERED = 8,
    TACTICCOND_IS_DISADVANTAGED = 9,
    TACTICCOND_IS_NOT_DISADVANTAGED = 10,
    TACTICCOND_ENEMY_LIGHTLY_DEFENDED_REFINERIES = 11,
    TACTICCOND_ENEMY_VULNERABLE_REFINERIES = 12,
    TACTICCOND_ENEMY_LIGHTLY_DEFENDED_CONYARDS = 13,
    TACTICCOND_ENEMY_VULNERABLE_CONYARDS = 14,
    TACTICCOND_ENEMY_HAS_SENSORS = 15,
    TACTICCOND_ENEMY_HAS_NO_SENSORS = 16,
    TACTICCOND_ENEMY_HARVESTER_COUNT_EXCEEDS = 17,
    TACTICCOND_NOT_REPEATED_FOR_FRAMES = 18,
    TACTICCOND_HOUSE_TACTICAL_VALUE_ABOVE = 19,
    TACTICCOND_HOUSE_TACTICAL_VALUE_BELOW = 20,
    TACTICCOND_ENEMY_ARTILLERY_STRENGTH_ABOVE = 21,
    TACTICCOND_ENEMY_ANTI_AIR_STRENGTH_BELOW = 22,
    TACTICCOND_COUNT
};


enum class AdvAITeamType
{
    ADVAITT_NONE = -1,
    ADVAITT_SCOUT = 0,
    ADVAITT_DIRECT_ATTACK = 1,
    ADVAITT_RUSH = 2,
    ADVAITT_ATTACK_REFINERIES = 3,
    ADVAITT_ATTACK_FACTORIES = 4,
    ADVAITT_APC_VS_REFINERIES = 5,
    ADVAITT_APC_VS_FACTORIES = 6,
    ADVAITT_ATTACK_HARVESTERS = 7,
    ADVAITT_DEFEND = 8,
    ADVAITT_COUNT
};


/**
 *  The number of house specific tactical values a tactic can set and test.
 */
#define ADVAI_TACTICAL_VALUE_COUNT 10


class DECLSPEC_UUID(UUID_ADVANCED_AI_TACTIC)
AdvancedAITacticTypeClass final : IPersistStream
{
public:
    struct AdvancedAITacticCondition
    {
        AdvAITacticConditionType ConditionType;
        int Parameter1;
        int Parameter2;

        bool operator==(const AdvancedAITacticCondition& other) const
        {
            return ConditionType == other.ConditionType && Parameter1 == other.Parameter1 && Parameter2 == other.Parameter2;
        }

        bool operator!=(const AdvancedAITacticCondition& other) const { return !operator==(other); }
    };

    struct AdvancedAITacticTeam
    {
        AdvAITacticType ForcedTacticType;
        AdvAITeamType TeamType;
        bool IsSuicide;
        bool IsTransportTeam;
        bool AvoidThreats;
        int MaxCost;
        RTTIType LimitToRTTI;

        bool operator==(const AdvancedAITacticTeam& other) const
        {
            return ForcedTacticType == other.ForcedTacticType && TeamType == other.TeamType && IsSuicide == other.IsSuicide &&
                IsTransportTeam == other.IsTransportTeam && AvoidThreats == other.AvoidThreats && MaxCost == other.MaxCost &&
                LimitToRTTI == other.LimitToRTTI;
        }

        bool operator!=(const AdvancedAITacticTeam& other) const { return !operator==(other); }
    };

    /**
     *  Aftermath: A fixed TeamType (TaskForce + Script, usually from AI.ini)
     *  that the tactic starts as a native team. MaxWait is the number of
     *  frames the team may gather before it starts with what it has
     *  (0 = until it is complete).
     */
    struct AdvancedAITacticScriptedTeam
    {
        char TeamTypeName[64];
        int MaxWait;

        bool operator==(const AdvancedAITacticScriptedTeam& other) const
        {
            return MaxWait == other.MaxWait && std::strncmp(TeamTypeName, other.TeamTypeName, sizeof(TeamTypeName)) == 0;
        }

        bool operator!=(const AdvancedAITacticScriptedTeam& other) const { return !operator==(other); }
    };

public:
    /**
     *  IUnknown
     */
    IFACEMETHOD(QueryInterface)(REFIID riid, LPVOID* ppvObj);
    IFACEMETHOD_(ULONG, AddRef)();
    IFACEMETHOD_(ULONG, Release)();

    /**
     *  IPersist
     */
    IFACEMETHOD(GetClassID)(CLSID* pClassID);

    /**
     *  IPersistStream
     */
    IFACEMETHOD(IsDirty)();
    IFACEMETHOD(Load)(IStream* pStm);
    IFACEMETHOD(Save)(IStream* pStm, BOOL fClearDirty);
    IFACEMETHOD_(LONG, GetSizeMax)(ULARGE_INTEGER* pcbSize);

public:
    AdvancedAITacticTypeClass();
    AdvancedAITacticTypeClass(const NoInitClass& noinit) {}
    AdvancedAITacticTypeClass(const char* name);
    virtual ~AdvancedAITacticTypeClass();

    char const* Name() const { return IniName; }
    bool Read_INI(CCINIClass& ini);
    bool Process(HouseClass* house);
    bool Is_Ground() const { return !IsAir && !IsNaval; }

    static int From_Name(const char* name);
    static AdvancedAITacticTypeClass* Find_Or_Make(const char* name, bool air = false, bool naval = false);
    static void Create_Defaults();

    static AdvAITacticType Get_AdvAI_Ground_Tactic_Type(const char* buffer);
    static AdvAIAirTacticType Get_AdvAI_Air_Tactic_Type(const char* buffer);
    static AdvAINavalTacticType Get_AdvAI_Naval_Tactic_Type(const char* buffer);

private:
    char IniName[128];

public:
    AdvAITacticType GroundTacticType;
    AdvAIAirTacticType AirTacticType;
    AdvAINavalTacticType NavalTacticType;
    DynamicVectorClass<AdvancedAITacticCondition> Conditions;
    DynamicVectorClass<AdvancedAITacticTeam> AdvAITeamTypes;
    DynamicVectorClass<AdvancedAITacticScriptedTeam> ScriptedTeamTypes;
    int MinimumExpectedDuration;
    int MaximumExpectedDuration;
    bool ScaleDurationByBuildTime;

    /**
     *  The house type (HouseTypes index) this tactic applies to.
     *  Aftermath: -1 (the default) makes the tactic available to every house.
     *  DTA required an explicit house; such tactics keep working unchanged.
     */
    HousesType House;

    int SetTacticalValueIndex;
    int SetTacticalValueMin;
    int SetTacticalValueMax;
    int SetTacticalValueChance;
    int FrameBasedDurationIncrease;
    int FrameBasedDurationTimeStep;
    int FrameBasedDurationIncreaseMax;
    bool IsAir;
    bool IsNaval;
    int MaxInstances;

    /**
     *  Was this tactic created from the built-in Aftermath defaults?
     */
    bool IsBuiltIn;
};
