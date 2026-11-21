/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended UnitClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "unitext.h"

#include "building.h"
#include "buildingtype.h"
#include "house.h"
#include "tibsun_globals.h"
#include "unittype.h"
#include "debughandler.h"
#include "extension.h"
#include "unit.h"
#include "vinifera_saveload.h"
#include "wwcrc.h"

#include <algorithm>


/**
 *  Class constructor.
 *
 *  @author: CCHyper
 */
UnitClassExtension::UnitClassExtension(const UnitClass *this_ptr) :
    FootClassExtension(this_ptr),
    Vinifera::Detach::Listener<BuildingClass>(),
    LastDockedBuilding(nullptr),
    IsWaitingForDock(false),
    IsPlayerOrdered(false)
{
    UnitExtensions.Add(this);
}


/**
 *  Class no-init constructor.
 *  
 *  @author: CCHyper
 */
UnitClassExtension::UnitClassExtension(const NoInitClass &noinit) :
    FootClassExtension(noinit),
    Vinifera::Detach::Listener<BuildingClass>(noinit)
{
}


/**
 *  Class destructor.
 *  
 *  @author: CCHyper
 */
UnitClassExtension::~UnitClassExtension()
{
    UnitExtensions.Delete(this);
}


/**
 *  Retrieves the class identifier (CLSID) of the object.
 *  
 *  @author: CCHyper
 */
HRESULT UnitClassExtension::GetClassID(CLSID *lpClassID)
{
    if (lpClassID == nullptr) {
        return E_POINTER;
    }

    *lpClassID = __uuidof(this);

    return S_OK;
}


/**
 *  Initializes an object from the stream where it was saved previously.
 *  
 *  @author: CCHyper
 */
HRESULT UnitClassExtension::Load(IStream *pStm)
{
    HRESULT hr = FootClassExtension::Load(pStm);
    if (FAILED(hr)) {
        return E_FAIL;
    }

    new (this) UnitClassExtension(NoInitClass());
    
    VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(LastDockedBuilding, "LastDockedBuilding");

    return hr;
}


/**
 *  Saves an object to the specified stream.
 *  
 *  @author: CCHyper
 */
HRESULT UnitClassExtension::Save(IStream *pStm, BOOL fClearDirty)
{
    HRESULT hr = FootClassExtension::Save(pStm, fClearDirty);
    if (FAILED(hr)) {
        return hr;
    }

    return hr;
}


/**
 *  Return the raw size of class data for save/load purposes.
 *  
 *  @author: CCHyper
 */
int UnitClassExtension::Get_Object_Size() const
{
    return sizeof(*this);
}


/**
 *  Removes the specified building from any targeting and reference trackers.
 */
void UnitClassExtension::On_Detach(BuildingClass *target, bool all)
{
    if (LastDockedBuilding == target) {
        LastDockedBuilding = nullptr;
    }
}


/**
 *  Compute a unique crc value for this instance.
 *  
 *  @author: CCHyper
 */
void UnitClassExtension::Object_CRC(CRCEngine &crc) const
{
    crc(LastDockedBuilding != nullptr ? LastDockedBuilding->Fetch_ID() : 0);
    crc(IsWaitingForDock);
    crc(IsPlayerOrdered);
}


/**
 *  How often (in frames) a waiting harvester looks for a dock again.
 */
static constexpr int DOCK_WAIT_CHECK_RATE = 30;


/**
 *  Does the harvester's house own a finished dock (from its Dock= list) that is
 *  not being built or sold? Same house only, like the check in Mission_Harvest.
 */
bool UnitClassExtension::Has_Ready_Dock(const UnitClass *unit)
{
    const auto &docks = unit->Class->Dock;

    for (int i = 0; i < Buildings.Count(); ++i) {
        const BuildingClass *building = Buildings[i];

        if (building == nullptr || building->House != unit->House || !building->IsActive
            || building->IsInLimbo || building->Strength <= 0) {
            continue;
        }

        if (std::find(docks.begin(), docks.end(), building->Class) == docks.end()) {
            continue;
        }

        const MissionType mission = building->Get_Mission();
        if (mission == MISSION_CONSTRUCTION || mission == MISSION_DECONSTRUCTION || building->BState == BSTATE_CONSTRUCTION) {
            continue;
        }

        return true;
    }

    return false;
}


/**
 *  Aftermath: A player order (MEGAMISSION or IDLE event) reached this object.
 *  Executed on every machine in the same order, so it is sync safe.
 */
void UnitClassExtension::Player_Order(TechnoClass *techno, MissionType mission)
{
    if (techno == nullptr || techno->RTTI != RTTI_UNIT) {
        return;
    }

    UnitClass *unit = static_cast<UnitClass *>(techno);
    if (!unit->Class->IsToHarvest && !unit->Class->IsToVeinHarvest) {
        return;
    }

    UnitClassExtension *ext = Extension::Fetch(unit);
    ext->IsWaitingForDock = false;
    ext->IsPlayerOrdered = (mission != MISSION_HARVEST);
}


/**
 *  Aftermath: Lets a harvester resume harvesting once a dock is available
 *  again, if it stopped on its own (not by a player order) because:
 *  - Mission_Harvest found no dock (IsWaitingForDock), or
 *  - it is full and idle, e.g. its refinery vanished while it was on its way
 *    there; TS then drives it to where the refinery stood and leaves it there.
 *  Called every frame after the mission AI.
 */
void UnitClassExtension::Dock_Wait_AI()
{
    UnitClass *unit = This();

    if (!unit->Class->IsToHarvest && !unit->Class->IsToVeinHarvest) {
        return;
    }

    /**
     *  Harvesting (again): nothing to resume, and a past player order is done with.
     */
    if (unit->CurrentMission == MISSION_HARVEST && unit->MissionQueue == MISSION_NONE) {
        IsPlayerOrdered = false;
        return;
    }

    if (IsPlayerOrdered) {
        IsWaitingForDock = false;
        return;
    }

    if (unit->IsInLimbo || unit->Team != nullptr || unit->MissionQueue != MISSION_NONE) {
        return;
    }

    const bool full = unit->Class->IsToVeinHarvest ? unit->Weed_Load() >= 1.0 : unit->Tiberium_Load() >= 1.0;
    if (!IsWaitingForDock && !full) {
        return;
    }

    /**
     *  Idle, or (full) driving to where the refinery stood. A destination left
     *  over from the harvest mission is driven to first while waiting.
     */
    const MissionType mission = unit->CurrentMission;
    const bool idle = (mission == MISSION_GUARD || mission == MISSION_GUARD_AREA) && unit->NavCom == nullptr;
    const bool moving_home = full && mission == MISSION_MOVE;
    if (!idle && !moving_home) {
        return;
    }

    if ((Frame + unit->Fetch_ID()) % DOCK_WAIT_CHECK_RATE != 0) {
        return;
    }

    if (!Has_Ready_Dock(unit)) {
        return;
    }

    IsWaitingForDock = false;
    unit->Assign_Destination(nullptr);
    unit->Assign_Mission(MISSION_HARVEST);
    unit->Commence();
}
