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
    bool Can_Advanced_AI_Build_Building(const BuildingTypeClass* type, const BuildingClass* factory = nullptr,
        bool check_ai_build_this = true) const;
    bool Has_Advanced_AI_Building_Production() const;
    const BuildingClass* Find_Advanced_AI_Upgrade_Target(const BuildingTypeClass* upgrade) const;
    bool Get_Advanced_AI_Power_Gain(const BuildingTypeClass* type, long long& gain) const;
    bool Has_Advanced_AI_Power_For(const BuildingTypeClass* type) const;
    const BuildingTypeClass* Select_Advanced_AI_Power_Plant(const BuildingClass* factory = nullptr) const;
    // Resolve power for an already validated order: desired type, a power plant, or nullptr.
    const BuildingTypeClass* Prioritize_Advanced_AI_Power(const BuildingTypeClass* desired, const BuildingClass* factory = nullptr) const;
    const BuildingTypeClass* Select_Advanced_AI_Building() const;
    void Log_Advanced_AI_Error(const char* reason, const BuildingTypeClass* type = nullptr, int detail = 0) const;
    static void Clear_Advanced_AI_Log_State();

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
