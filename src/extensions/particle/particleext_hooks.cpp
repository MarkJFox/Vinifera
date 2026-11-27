/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended ParticleClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "aircraft.h"
#include "custompalette.h"
#include "extension.h"
#include "hooker.h"
#include "house.h"
#include "housetype.h"
#include "particle.h"
#include "particlesys.h"
#include "particletype.h"
#include "particletypeext.h"
#include "rules.h"
#include "scenario.h"
#include "syringe.h"
#include "tibsun_defines.h"
#include "tibsun_globals.h"
#include "unit.h"


UnitClass* Create_Visceroid(ObjectClass* destroyedobject)
{
    if (destroyedobject->RTTI == RTTI_INFANTRY || (destroyedobject->Is_Techno() && destroyedobject->TClass->IsCrew)) {
        return new UnitClass(Rule->SmallVisceroid, House_From_HousesType(HouseTypeClass::From_Name("Neutral")));
    }

    return nullptr;
}


/**
 *  Fixes a bug where gas clouds are able to turn everything into visceroids, including
 *  non-crewed vehicles and even terrain objects.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x005A389C, _ParticleClass_Smoke_And_WeakGas_Behaviour_AI_Tiberium_Death_Patch, 0)
{
    GET(ObjectClass*, destroyedobject, ESI);
    GET(ResultType, result, EAX);
    GET_STACK(ObjectClass*, nextobject, 0x40);

    enum {
        ContinueVisceroidPlacement = 0x005A38FC,
        SkipToNextObjectOnCell = 0x005A3965
    };

    R->ESI(nextobject);

    if (result != RESULT_DESTROYED) {
        // Object was not destroyed, do not create visceroid.
        return SkipToNextObjectOnCell;
    }

    if (!Scen->IsTiberiumDeathToVisceroid) {
        // Visceroids spawning from Tiberium death is disabled, do not create visceroid.
        return SkipToNextObjectOnCell;
    }

    UnitClass* visceroid = Create_Visceroid(destroyedobject);
    if (visceroid == nullptr) {
        // No visceroid was created.
        return SkipToNextObjectOnCell;
    }

    R->EDI(visceroid);
    return ContinueVisceroidPlacement;
}


/**
 *  Aftermath: Draws particles that have their own palette (ART.INI "Palette="
 *  in the image section) with it, in place of AnimDrawer (ANIM.PAL).
 *
 *  @author: Aftermath
 */
DEFINE_HOOK(0x005A46DA, _ParticleClass_Draw_It_Custom_Palette_Patch, 6)
{
    GET(ParticleClass *, this_ptr, ESI);

    if (this_ptr->Class == nullptr) {
        return 0;
    }

    const ParticleTypeClassExtension *typeext = Extension::Fetch(this_ptr->Class);
    ConvertClass *drawer = typeext != nullptr ? CustomPalette::Plain_Converter(typeext->PaletteName.c_str()) : nullptr;
    if (drawer == nullptr) {
        return 0;
    }

    R->EDX(drawer);
    return 0x005A46E0;
}


/**
 *  Main function for patching the hooks.
 */
void ParticleClassExtension_Hooks()
{
    
}
