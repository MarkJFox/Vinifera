/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended HouseClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#pragma once

#include "abstractext.h"
#include "advaitactictype.h"
#include "detach_listener.h"
#include "house.h"
#include "housetype.h"
#include "typelist.h"


class FactoryClass;
class TeamClass;
class TechnoClass;
class UnitClass;
class UnitTypeClass;
class AircraftTypeClass;
class BuildingClass;
class BuildingTypeClass;
class TechnoTypeClass;
struct AdvancedAIResourceAnalysis;

enum class AdvancedAIBuildingStart {
    Ready, NeedsPower, InsufficientFunds, NoReadyYard, NotBuildable, ProductOwnsSlot, AwaitingPlanner, UnmanagedOrder
};

struct AdvancedAIPowerGoal
{
    const BuildingTypeClass* Type = nullptr;
    bool Technology = false;
    bool ResourceExpansion = false;
    bool EmergencyDefense = false;
    bool PreventiveDefense = false;
};

struct AdvancedAIDefenseFocus
{
    Cell Asset{0, 0};
    Cell Enemy{0, 0};
    int Range = 0;
    int MinimumRange = 0;
    bool Air = false;
    bool Direct = true;
    bool HasContact = true;
    bool RequireContactRange = false;
    int ContactID = -1;
    int Role = -1;
    bool Urgent = false;

    Cell Intercept() const;
};

struct AdvancedAIBuildingPlan
{
    enum class Action { Order, Wait, Keep };

    Action Result;
    const char* Reason;
    const BuildingTypeClass* Type = nullptr;
    const char* Source = nullptr;
    AdvancedAIPowerGoal PowerGoal{};
};

/**
 *  AF-0009U: State of an adopted Advanced AI tactic (ported from DTA).
 */
template <typename T> struct AdvAITacticInfo
{
    AdvAITacticInfo() : Tactic((T)-1), SelectionFrame(0), Duration(0) {}
    AdvAITacticInfo(T tactic, int selectionframe, int duration) : Tactic(tactic), SelectionFrame(selectionframe), Duration(duration) {}

    T Tactic;
    int SelectionFrame;
    int Duration;

    int EndFrame() const { return SelectionFrame + Duration; }
};


struct AttemptedBuildingCaptureStruct
{
    Cell BuildingLocation;
    int Frame;
};


class TechnoTypeClassExtension;

struct EnemyStrengthStruct
{
    int None;
    int Light;
    int Heavy;

    int Total() const { return None + Light + Heavy; }
    void Clear() { None = 0; Light = 0; Heavy = 0; }
    void Add_Techno_Type(const TechnoTypeClassExtension* technotypeext);
    void Add_Techno_Type_Half_Weight(const TechnoTypeClassExtension* technotypeext);
    void Add_Techno_Type_AntiAir(const TechnoTypeClassExtension* technotypeext);
};


enum class AdvancedAINavalOnlyState
{
    NOT_CHECKED = 0,
    NORMAL = 1,
    NAVAL_ONLY = 2
};


struct AdvancedAIUnitStartBudget
{
    long long Uncommitted = 0;
    long long Cost = 0;
    long long Reserve = 0;
    const char* ReserveReason = "none";
    const char* Blocked = nullptr;
    // AF-0009U: The house's build request no longer fits (dock capacity or team budget).
    bool ClearRequest = false;
};


class DECLSPEC_UUID(UUID_HOUSE_EXTENSION)
HouseClassExtension final : public AbstractClassExtension,
                            public Vinifera::Detach::Listener<FactoryClass>
{
public:
    /**
     *  IPersist
     */
    IFACEMETHOD(GetClassID)(CLSID *pClassID);

    /**
     *  IPersistStream
     */
    IFACEMETHOD(Load)(IStream *pStm);
    IFACEMETHOD(Save)(IStream *pStm, BOOL fClearDirty);

public:
    HouseClassExtension(const HouseClass *this_ptr = nullptr);
    HouseClassExtension(const NoInitClass &noinit);
    virtual ~HouseClassExtension();

    virtual int Get_Object_Size() const override;
    virtual void Object_CRC(CRCEngine &crc) const override;

    void On_Detach(FactoryClass *target, bool all) override;

    virtual const char *Name() const override { return reinterpret_cast<const HouseClass *>(This())->Class->Name(); }
    virtual const char *Full_Name() const override { return reinterpret_cast<const HouseClass *>(This())->Class->Full_Name(); }
    
    virtual HouseClass *This() const override { return reinterpret_cast<HouseClass *>(AbstractClassExtension::This()); }
    virtual const HouseClass *This_Const() const override { return reinterpret_cast<const HouseClass *>(AbstractClassExtension::This_Const()); }
    virtual RTTIType Fetch_RTTI() const override { return RTTI_HOUSE; }

    FactoryClass* Fetch_Factory(RTTIType rtti, ProductionFlags flags) const;
    void Set_Factory(RTTIType rtti, FactoryClass* factory, ProductionFlags flags);
    int* Factory_Counter(RTTIType rtti, ProductionFlags flags);
    int Factory_Count(RTTIType rtti, ProductionFlags flags) const;
    ProdFailType Suspend_Production(RTTIType type, ProductionFlags flags);
    ProdFailType Begin_Production(RTTIType type, int id, bool resume, ProductionFlags flags);
    ProdFailType Abandon_Production(RTTIType type, int id, ProductionFlags flags);
    bool Place_Object(RTTIType type, Cell const& cell, ProductionFlags flags);
    void Update_Factories(RTTIType rtti, ProductionFlags flags);
    TechnoTypeClass const* Suggest_New_Object(RTTIType objecttype, ProductionFlags flags) const;

    int AI_Unit();
    int AI_Naval_Unit();

    // Shared AF-0009 entry points; these methods add no per-house saved state.
    bool Is_Advanced_AI_Enabled() const;
    AdvancedAIUnitStartBudget Check_Advanced_AI_Unit_Start(const TechnoTypeClass* type) const;
    void Trace_Advanced_AI_Unit_Start(const TechnoTypeClass* type, const AdvancedAIUnitStartBudget& budget,
        const char* outcome) const;
    AdvancedAIBuildingPlan Plan_Advanced_AI_Building();
    void Apply_Advanced_AI_Building_Plan(const AdvancedAIBuildingPlan& plan);
    AdvancedAIBuildingStart Check_Advanced_AI_Building_Start(const BuildingTypeClass* type, const BuildingClass* yard) const;
    bool Begin_Advanced_AI_Building_Attempt();
    void Reject_Advanced_AI_Building_Start(const BuildingTypeClass* type);
    void Defer_Advanced_AI_Building_Type(const BuildingTypeClass* type);
    bool Is_Advanced_AI_Building_Type_Deferred(const BuildingTypeClass* type) const;
    void Trace_Advanced_AI_Building_Order(const char* phase, const char* reason, const BuildingTypeClass* type = nullptr,
        FactoryClass* factory = nullptr, const Cell* target = nullptr, const char* source = nullptr) const;
    static const char* Advanced_AI_Building_Start_Reason(AdvancedAIBuildingStart status);
    static void Clear_Advanced_AI_Building_Order_State();
    bool Can_Advanced_AI_Build_Building(const BuildingTypeClass* type, const BuildingClass* factory = nullptr,
        bool check_ai_build_this = true) const;
    bool Has_Advanced_AI_Building_Production() const;
    const BuildingClass* Find_Advanced_AI_Upgrade_Target(const BuildingTypeClass* upgrade) const;
    bool Get_Advanced_AI_Power_Gain(const BuildingTypeClass* type, long long& gain) const;
    bool Has_Advanced_AI_Power_For(const BuildingTypeClass* type) const;
    const BuildingTypeClass* Select_Advanced_AI_Power_Plant(const BuildingClass* factory = nullptr) const;
    // Resolve power for an already validated order: desired type, a power plant, or nullptr.
    const BuildingTypeClass* Prioritize_Advanced_AI_Power(const BuildingTypeClass* desired, const BuildingClass* factory = nullptr) const;
    const BuildingTypeClass* Select_Advanced_AI_Building(AdvancedAIPowerGoal& goal) const;
    void Log_Advanced_AI_Error(const char* reason, const BuildingTypeClass* type = nullptr, int detail = 0) const;
    void Trace_Advanced_AI_Production(const char* phase, const char* reason, const BuildingTypeClass* requested = nullptr,
        long long minimum_funds = 0, FactoryClass* observed = nullptr) const;
    static void Clear_Advanced_AI_Log_State();

    // B2a observes resources only; the returned snapshot expires on refresh/reset.
    void Update_Advanced_AI_Resource_Analysis() const;
    const AdvancedAIResourceAnalysis* Get_Advanced_AI_Resource_Analysis() const;
    static void Clear_Advanced_AI_Resource_State();

    const char* Update_Advanced_AI_Expansion();
    const BuildingTypeClass* Plan_Advanced_AI_Expansion(const BuildingTypeClass* infrastructure = nullptr,
        const char** outcome = nullptr, AdvancedAIPowerGoal* goal = nullptr);
    const BuildingTypeClass* Plan_Advanced_AI_Development(const char*& reason, AdvancedAIPowerGoal& goal);
    void Report_Advanced_AI_Building_Completed(const BuildingTypeClass* type, int product_id);
    void Report_Advanced_AI_Power_Start(const BuildingTypeClass* type, const FactoryClass* factory);
    bool Retarget_Advanced_AI_Development(const BuildingTypeClass* desired, const BuildingTypeClass* power, AdvancedAIPowerGoal& goal);
    const BuildingTypeClass* Select_Advanced_AI_Development(const char*& need, long long& minimum, AdvancedAIPowerGoal& goal) const;
    const BuildingTypeClass* Select_Advanced_AI_Defense(const char*& need, long long& minimum, AdvancedAIPowerGoal& goal, bool basic_only,
        bool emergency = false, const FactoryClass* replacing = nullptr, bool preventive = false,
        const BuildingTypeClass* required = nullptr, bool urgent_only = false) const;
    Cell Find_Advanced_AI_Yard_Wall_Site(const BuildingTypeClass* type, const Cell* placed = nullptr,
        int* autofill = nullptr) const;
    bool Is_Advanced_AI_Defense_Type(const BuildingTypeClass* type) const;
    void Record_Advanced_AI_Building_Hit(int attacker_id, const Cell& target, int damage, bool base_hit);
    Cell Advanced_AI_Defense_Focus(const BuildingTypeClass* type) const;
    Cell Find_Advanced_AI_Defense_Site(BuildingTypeClass* type, const Cell* required_focus = nullptr, int range = 0,
        const AdvancedAIDefenseFocus* threat = nullptr) const;
    bool Advanced_AI_Ordered_Defense_Focus(const BuildingTypeClass* type, AdvancedAIDefenseFocus& focus) const;
    long long Advanced_AI_Defense_Start_Reserve(const BuildingTypeClass* type) const;
    TechnoClass* Advanced_AI_Primary_Base_Attacker(long long& damage) const;
    void Retry_Advanced_AI_Defense_Placement(const BuildingTypeClass* type) const;
    bool Has_Advanced_AI_Refinery_Route(const BuildingTypeClass* type, const Cell& site) const;
    long long Advanced_AI_Refinery_Reserve() const;
    long long Advanced_AI_Uncommitted_Funds() const;
    const char* Validate_Advanced_AI_Expansion_Start(const BuildingTypeClass* type);
    bool Report_Advanced_AI_Expansion_Start(const BuildingTypeClass* type, FactoryClass* factory);
    Cell Advanced_AI_Expansion_Target() const;
    bool Has_Advanced_AI_Expansion_Site() const;
    const BuildingTypeClass* Select_Advanced_AI_Expansion_Creep(bool refinery_only, const char*& reason);
    Cell Find_Advanced_AI_Expansion_Placement(BuildingTypeClass* type);
    ExitType Exit_Advanced_AI_Expansion_Product(BuildingClass* yard, const TechnoClass* product);

    // AF-0009U: Savings goal of the building planner, respected by unit starts.
    void Request_Advanced_AI_Savings(long long amount, const char* reason, bool economy) const;
    void Clear_Advanced_AI_Economy_Savings() const;
    bool Is_Advanced_AI_Economy_Phase_Limited(const TechnoTypeClass* type) const;

private:
    void Reserve_Advanced_AI_Infrastructure(const TechnoClass* building_product, AdvancedAIUnitStartBudget& budget) const;
    void Reserve_Advanced_AI_Savings(const TechnoTypeClass* type, AdvancedAIUnitStartBudget& budget) const;
    bool Needs_Advanced_AI_Recovery_Unit(const UnitTypeClass* type) const;
    const BuildingTypeClass* Select_Advanced_AI_Preventive_Defense(AdvancedAIPowerGoal& goal,
        const BuildingTypeClass* required = nullptr, bool urgent_only = false);
    const BuildingTypeClass* Select_Advanced_AI_Technology(const char*& reason, long long& minimum, AdvancedAIPowerGoal& goal);
    const BuildingTypeClass* Select_Advanced_AI_Capacity(const char*& reason, long long& minimum, AdvancedAIPowerGoal& goal) const;
    const BuildingTypeClass* Select_Advanced_AI_Yard_Wall() const;
    const BuildingTypeClass* Select_Advanced_AI_Wall_Prerequisite();
    const BuildingTypeClass* Select_Advanced_AI_Air_Defense_Prerequisite();
    const BuildingTypeClass* Select_Advanced_AI_Aircraft_Dock();

public:
    static bool Advanced_AI_Aircraft_Needs_Dock(const AircraftTypeClass* type);
    void Advanced_AI_Aircraft_Dock_Balance(const AircraftTypeClass* type, int& aircraft, int& docks) const;
    const char* Advanced_AI_Unit_Start_Limit(const TechnoTypeClass* type) const;
    static bool Advanced_AI_Can_Hit_Air(const TechnoTypeClass* type);
    int Advanced_AI_Air_Attackers() const;

private:
    const BuildingTypeClass* Select_Advanced_AI_Replacement(bool slot_free, AdvancedAIPowerGoal& goal);
    AdvancedAIBuildingPlan Resume_Advanced_AI_Power_Goal();
    const BuildingTypeClass* Resume_Advanced_AI_Refinery(const BuildingTypeClass* desired, AdvancedAIPowerGoal& goal);
    void Set_Advanced_AI_Development_Order(const BuildingTypeClass* type, long long minimum, bool technology);
    void Report_Advanced_AI_Power_Completed(const BuildingTypeClass* type, int product_id);
    bool Has_Advanced_AI_Refineries_For(const BuildingTypeClass* type) const;
    int Advanced_AI_Refinery_Shortfall(bool preferred) const;
    bool Has_Advanced_AI_Technology_Economy() const;
    bool Can_Advanced_AI_Build_Type(const BuildingTypeClass* type, bool check_ai_build_this,
        bool check_prerequisites = true) const;
    bool Has_Advanced_AI_Ready_Yard(const BuildingTypeClass* type, const BuildingClass* factory,
        const FactoryClass* replacing = nullptr) const;
    void Release_Advanced_AI_Building_Order(const char* reason);
    void Interrupt_Advanced_AI_Development();

public:
    /**
     *  AF-0009U: DTA-style Advanced AI unit production and tactics.
     */
    bool Is_Advanced_AI_Unit_Production() const;
    int AdvAI_AI_Unit();
    int AdvAI_AI_Naval_Unit();
    int AdvAI_AI_Infantry();
    int AdvAI_AI_Aircraft();
    void AdvAI_Expert_AI();
    TeamClass* Get_Team_In_Production(int& id, RTTIType for_type, ProductionFlags prodflags) const;
    void Fill_Owned_Buildings_List(DynamicVectorClass<BuildingTypeClass*>& owned) const;
    bool AdvAI_Can_Produce(const TechnoTypeClass* type, DynamicVectorClass<BuildingTypeClass*>& owned) const;
    const TechnoTypeClass* AdvAI_Scripted_Team_Need(RTTIType typertti, bool naval, DynamicVectorClass<BuildingTypeClass*>& owned) const;
    void AdvAI_Scripted_Team_AI();
    bool AdvAI_Is_Outnumbered() const;
    bool AdvAI_Is_Disadvantaged() const;
    bool AdvAI_Is_Recently_Attacked() const;
    void Assign_AdvAI_Tactic(AdvAITacticType tactic, int expected_duration);
    void Assign_AdvAI_Air_Tactic(AdvAIAirTacticType airtactic, int expected_duration);
    void Assign_AdvAI_Naval_Tactic(AdvAINavalTacticType navaltactic, int expected_duration);
    void AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength(TeamClass* team) const;
    void AdvAI_Set_Ground_Team_Desired_Ratios(TeamClass* team, AdvAITacticType tacticoverride = TACTIC_NONE) const;
    void AdvAI_Set_Aircraft_Team_Desired_Ratios(TeamClass* team) const;
    void AdvAI_Set_Naval_Team_Desired_Ratios(TeamClass* team) const;
    bool Enemy_Building_Scan(const TypeList<BuildingTypeClass*>& list, int threatvalue) const;
    bool AdvAI_Enemy_Has_Vulnerable_Buildings(const TypeList<BuildingTypeClass*>& list) const;
    bool AdvAI_Enemy_Has_Lightly_Defended_Buildings(const TypeList<BuildingTypeClass*>& list) const;
    bool Has_One_Of(const TypeList<BuildingTypeClass*>& buildingtypes) const;
    bool Has_Barracks() const;
    bool Has_War_Factory() const;
    bool Has_Naval_Yard() const;
    bool Has_Helipad() const;
    bool Has_Construction_Yard() const;
    int Get_Building_Capture_Attempt_Index_For(const BuildingClass* building) const;
    bool Is_Valid_Building_For_Capturing(BuildingClass* building, const Cell& zonecell, bool& okintheory) const;
    void Add_Building_Capture_Attempt(const BuildingClass* building);

private:
    void AdvAI_Calculate_Enemy_Strength();
    void AdvAI_Commence_Current_Tactic();
    void AdvAI_Air_Tactic_AI();
    void AdvAI_Naval_Tactic_AI();
    void AdvAI_Tactic_Selection_AI();
    void AdvAI_Team_Recruitment_AI();
    void AdvAI_Team_Maintenance_AI();
    void AdvAI_Self_Defense_AI();
    void AdvAI_Check_Naval_Only();
    void AdvAI_Check_For_Engineer_Capturing_Neutral_Building();
    bool AdvAI_Is_Under_Start_Rush_Threat() const;
    int AdvAI_Start_Rush_Counter(RTTIType rtti);
    bool AdvAI_Select_Air_Defense_Unit(RTTIType rtti);
    int AdvAI_Harvester_Shortfall(const UnitTypeClass* harvester) const;

    /**
     *  AF-0011: Mobile war factories (houseext_advai_mobilewar.cpp).
     */
    static bool Is_Mobile_War_Factory_Type(const UnitTypeClass* type);
    const UnitTypeClass* AdvAI_Mobile_War_Factory_To_Build(DynamicVectorClass<BuildingTypeClass*>& owned) const;
    void AdvAI_Mobile_War_Factory_AI();
    void AdvAI_Mobile_War_Factory_Vehicle_AI(UnitClass* unit, bool emergency);
    void AdvAI_Mobile_War_Factory_Building_AI(BuildingClass* building);
    bool AdvAI_Has_Fixed_Weapons_Factory() const;
    const HouseClass* AdvAI_Mobile_War_Front_Enemy() const;
    Cell AdvAI_Mobile_War_Front_Point() const;
    bool AdvAI_Is_Mobile_War_Site_Safe(const Cell& cell) const;
    Cell AdvAI_Find_Mobile_War_Site(const UnitTypeClass* type, const Cell& point, const Cell& zone_cell) const;

public:
    bool Has_Prerequisite(int prerequisite);
    bool Has_Prerequisite(PrerequisiteGroupType group);
    bool Has_Prerequisite(StructType building);

    bool Required_Forbidden_Houses_Check(TechnoTypeClass const* ttype);

    void Put_Storage_Pointers();
    static void Load_Unit_Trackers(HouseClass* house, IStream* pStm);
    static void Save_Unit_Trackers(HouseClass* house, IStream* pStm);

    void Set_Spawn_Point(const Cell& cell);

    static HouseClass* House_At_Spawn_Point(WAYPOINT waypoint);
    static HouseClass* House_From_HousesType(HousesType house);

    bool Can_Use_Iron_Curtain() const;
    void Expend_Iron_Curtain();

public:
    /**
     *  Replacement Tiberium storage.
     */
    VectorClass<int> TiberiumStorage;

    /**
     *  Replacement Weed storage.
     */
    VectorClass<int> WeedStorage;

    /**
     *  Record the number of naval factories active.
     */
    int NavalFactories;

    /**
     *  For human controlled houses, only one type of naval unit can be produced
     *  at any one instant. This is the factory object controlling this production.
     */
    FactoryClass* NavalFactory;

    /**
     *  The type of the naval unit the AI is currently scheduled to build.
     */
    UnitType BuildNavalUnit;

    /**
     *  The waypoint at which this house was spawned.
     */
    WAYPOINT SpawnWaypoint;

    /**
     *  Provides a timer for the availability of the Iron Curtain for this house.
     *  Used until we have a proper superweapon based Iron Curtain implementation.
     */
    CDTimerClass<FrameTimerClass> IronCurtainAvailabilityTimer;

    /**
     *  Determines whether repairs are paused instead of stopped when this house has insufficient funds.
     */
    bool IsPauseRepairs;

    /**
     *  Is this house an observer?
     */
    bool IsObserver;

    /**
     *  AF-0009U: Advanced AI unit production and tactic state (ported from DTA).
     *  All fields are plain data, saved with the extension and only changed by
     *  the synchronised simulation.
     */
    bool IsUnderStartRushThreat;
    bool HasPerformedVehicleCharge;
    int NextOilRefineryCaptureCheckFrame;
    int NextEngineerCheckFrame;
    AdvAITacticInfo<AdvAITacticType> AdvAIGroundTactic;
    int AdvAIGroundTacticPostponements;
    AdvAITacticInfo<AdvAIAirTacticType> AdvAIAirTactic;
    AdvAITacticInfo<AdvAINavalTacticType> AdvAINavalTactic;
    int AdvAILastExecutionFrameForTactic[TACTIC_COUNT];
    int AdvAILastTacticExecutionFrame;
    int EnemyNoneStrength;
    int EnemyLightStrength;
    int EnemyHeavyStrength;
    int EnemyArtilleryStrength;
    int EnemyBaseDefenseStrength;
    int EnemyNavalStrength;
    int EnemyAirStrength;   // Aftermath: cost of flying enemies (aircraft and airborne jumpjets).
    EnemyStrengthStruct EnemyAntiGroundStrength;
    EnemyStrengthStruct EnemyAntiAirStrength;
    EnemyStrengthStruct EnemyAntiNavalStrength;
    int EnemyHarvesterCount;
    int EnemyRefineryCount;
    bool EnemyHasSensors;
    int LastHarvesterBuildFrame;
    int LastUnitValueDebugPrintFrame;
    int LastInfantryValueDebugPrintFrame;
    int LastAircraftValueDebugPrintFrame;
    int LastNavalValueDebugPrintFrame;
    AttemptedBuildingCaptureStruct AttemptedBuildingCaptures[50];
    int AttemptedBuildingCaptureCount;
    UnitType AdvAILastBuiltUnit;
    int AdvAILastBuiltUnitCount;
    InfantryType AdvAILastBuiltInfantry;
    int AdvAILastBuiltInfantryCount;
    int FactionSpecificTacticalValues[ADVAI_TACTICAL_VALUE_COUNT];
    AdvancedAINavalOnlyState IsNavalOnly;
    int LastNavalOnlyCheckFrame;

    /**
     *  AF-0011: Mobile war factory state (plain data, saved with the extension).
     */
    int MobileWarNextFrame;         // Next logic pass.
    int MobileWarLostFrame;         // When the last one was lost (rebuild delay).
    bool IsMobileWarOwned;          // Owned one at the last pass.
    Cell MobileWarSite;             // Cell the vehicle drives to and deploys at.
    int MobileWarAttempts;          // Failed trips or deploys at that site.
    int MobileWarDeployedFrame;     // When it last deployed (relocation delay).
};
