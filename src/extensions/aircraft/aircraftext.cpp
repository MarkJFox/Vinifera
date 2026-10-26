/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended AircraftClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "aircraftext.h"

#include "aircraft.h"
#include "extension.h"
#include "fixedwing.h"
#include "vinifera_saveload.h"
#include "wwcrc.h"


/**
 *  Class constructor.
 *
 *  @author: CCHyper
 */
AircraftClassExtension::AircraftClassExtension(const AircraftClass *this_ptr) :
    FootClassExtension(this_ptr),
    FixedWingNoDockTimer(0),
    CAPMode(FixedWingCAPMode::None),
    CAPMission(MISSION_NONE),
    CAPAnchor(COORD_NONE),
    CAPEscort(nullptr),
    CAPRearming(false),
    CAPOwner(-1),
    AttackControl(FixedWingAttackControl::Custom)
{
    AircraftExtensions.Add(this);
}


/**
 *  Class no-init constructor.
 *  
 *  @author: CCHyper
 */
AircraftClassExtension::AircraftClassExtension(const NoInitClass &noinit) :
    FootClassExtension(noinit)
{
    // Internal_Load restored timer, CAP and attack ownership; do not initialize.
}


/**
 *  Class destructor.
 *  
 *  @author: CCHyper
 */
AircraftClassExtension::~AircraftClassExtension()
{
    AircraftExtensions.Delete(this);
}


/**
 *  Retrieves the class identifier (CLSID) of the object.
 *  
 *  @author: CCHyper
 */
HRESULT AircraftClassExtension::GetClassID(CLSID *lpClassID)
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
HRESULT AircraftClassExtension::Load(IStream *pStm)
{
    FixedWing::Reset_CAP_Trace(); // Transient observations cannot cross a load.
    // The inherited raw-blob load uses our Get_Object_Size(), including the timer.
    HRESULT hr = FootClassExtension::Load(pStm);
    if (FAILED(hr)) {
        return E_FAIL;
    }

    new (this) AircraftClassExtension(NoInitClass());
    VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(CAPEscort, "CAPEscort");
    
    return hr;
}


/**
 *  Saves an object to the specified stream.
 *  
 *  @author: CCHyper
 */
HRESULT AircraftClassExtension::Save(IStream *pStm, BOOL fClearDirty)
{
    // The inherited raw blob includes survival, CAP and attack ownership.
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
int AircraftClassExtension::Get_Object_Size() const
{
    return sizeof(*this);
}




/**
 *  Compute a unique crc value for this instance.
 *  
 *  @author: CCHyper
 */
void AircraftClassExtension::Object_CRC(CRCEngine &crc) const
{
    FootClassExtension::Object_CRC(crc);
    crc(FixedWingNoDockTimer);
    crc(static_cast<int>(CAPMode));
    crc(static_cast<int>(CAPMission));
    crc(CAPAnchor.X);
    crc(CAPAnchor.Y);
    crc(CAPAnchor.Z);
    crc(CAPEscort ? CAPEscort->Fetch_ID() : 0);
    crc(CAPRearming);
    crc(CAPOwner);
    crc(static_cast<int>(AttackControl));
}

void AircraftClassExtension::On_Detach(TechnoClass *target, bool all)
{
    FootClassExtension::On_Detach(target, all);
    // Keep Escort mode until the AI notices the loss and requests Return.
    // Never dereference the destroyed target or start missions in Detach.
    if (CAPEscort == target) {
        CAPEscort = nullptr;
        FixedWing::Trace_CAP_State(This(), "DETACH_POINTER_CLEARED");
    }
}
