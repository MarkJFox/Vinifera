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
#include "detach_listener.h"
#include "house.h"
#include "housetype.h"


class FactoryClass;
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
    void Record_Advanced_AI_Building_Hit(int attacker_id, const Cell& target, int damage);
    Cell Advanced_AI_Defense_Focus(const BuildingTypeClass* type) const;
    Cell Find_Advanced_AI_Defense_Site(BuildingTypeClass* type, const Cell* required_focus = nullptr, int range = 0,
        const AdvancedAIDefenseFocus* threat = nullptr) const;
    bool Advanced_AI_Ordered_Defense_Focus(const BuildingTypeClass* type, AdvancedAIDefenseFocus& focus) const;
    void Retry_Advanced_AI_Defense_Placement(const BuildingTypeClass* type) const;
    bool Has_Advanced_AI_Refinery_Route(const BuildingTypeClass* type, const Cell& site) const;
    long long Advanced_AI_Refinery_Reserve() const;
    long long Advanced_AI_Uncommitted_Funds() const;
    const char* Validate_Advanced_AI_Expansion_Start(const BuildingTypeClass* type);
    bool Report_Advanced_AI_Expansion_Start(const BuildingTypeClass* type, FactoryClass* factory);
    Cell Advanced_AI_Expansion_Target() const;
    Cell Find_Advanced_AI_Expansion_Placement(BuildingTypeClass* type);
    ExitType Exit_Advanced_AI_Expansion_Product(BuildingClass* yard, const TechnoClass* product);

private:
    const BuildingTypeClass* Select_Advanced_AI_Preventive_Defense(AdvancedAIPowerGoal& goal,
        const BuildingTypeClass* required = nullptr, bool urgent_only = false);
    const BuildingTypeClass* Select_Advanced_AI_Technology(const char*& reason, long long& minimum, AdvancedAIPowerGoal& goal);
    const BuildingTypeClass* Select_Advanced_AI_Capacity(const char*& reason, long long& minimum, AdvancedAIPowerGoal& goal) const;
    const BuildingTypeClass* Select_Advanced_AI_Yard_Wall() const;
    const BuildingTypeClass* Select_Advanced_AI_Wall_Prerequisite();
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
};
