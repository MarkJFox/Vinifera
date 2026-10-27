/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended BuildingTypeClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "buildingtypeext_hooks.h"

#include "asserthandler.h"
#include "building.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "buildingtypeext_init.h"
#include "bullettype.h"
#include "debughandler.h"
#include "extension.h"
#include "hooker.h"
#include "rules.h"
#include "syringe.h"
#include "tibsun_globals.h"
#include "verses.h"
#include "vinifera_globals.h"
#include "weapontype.h"

/**
 *  AF-0005B: preserve __thiscall and the original native fallback. The first
 *  ten bytes are push ecx / push esi / mov esi,ecx / mov cl,[native flag].
 *  The custom path has not executed that prologue, so jump straight to ret 4.
 */
DEFINE_HOOK(0x0043FED0, _BuildingTypeClass_Occupy_List_Custom_Foundation, 0xA)
{
    // Native objects load before extension pointers are remapped. Cell occupation
    // is restored from the save; do not dereference a serialized extension pointer.
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingTypeClass*, type, ECX);
    const auto ext = Extension::Fetch(type);
    if (ext != nullptr && ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid) {
        R->EAX(ext->CustomFoundationCells);
        return 0x0043FF20;
    }
    return 0;
}

/**
 *  Reject invalid absolute cells before native Legal_Placement indexes Map.
 *  No placement algorithm is replaced; native types execute the original code.
 */
DEFINE_HOOK(0x00442EA0, _BuildingTypeClass_Legal_Placement_Custom_Foundation_Bounds, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingTypeClass*, type, ECX);
    GET_STACK(const Cell*, position, 0x4);
    const auto ext = Extension::Fetch(type);
    if (ext != nullptr && ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid &&
        (position == nullptr || !ext->Custom_Foundation_In_Radar(*position))) {
        R->EAX(0);
        return 0x00442EBE; // ret 8; no native prologue/arguments have been changed.
    }
    return 0;
}

/**
 *  MARK_DOWN_FORCED bypasses Can_Enter_Cell/Legal_Placement. Guard it and
 *  ordinary Mark-down before the parent changes occupation or IsDown.
 *  The overwritten instruction is only sub esp,90h (six bytes).
 */
DEFINE_HOOK(0x00429370, _BuildingClass_Mark_Custom_Foundation_Bounds, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ECX);
    GET_STACK(MarkType, mark, 0x4);
    if (mark == MARK_DOWN || mark == MARK_DOWN_FORCED) {
        const auto ext = Extension::Fetch(building->Class);
        if (ext != nullptr && ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid &&
            !ext->Custom_Foundation_In_Radar(building->Get_Cell())) {
            R->EAX(0);
            return 0x0042978D; // Only ret 4, not the native frame-unwinding epilogue.
        }
    }
    return 0;
}

/**
 *  A fake class for implementing new member functions which allow
 *  access to the "this" pointer of the intended class.
 *
 *  @note: This must not contain a constructor or destructor!
 *  @note: All functions must be prefixed with "_" to prevent accidental virtualization.
 */
DECLARE_EXTENDING_CLASS_AND_PAIR(BuildingTypeClass)
{
public:
    void _Free_Buildup_Image();
    void _Set_Base_Defense_Values();
    int _Raw_Cost();
    int _Cost_Of(HouseClass * house);
    static void _Fetch_Z_Data();
};


/**
 *  Reimplementation of BuildingClass::Free_Buildup_Image.
 *
 *  @author: CCHyper
 */
void BuildingTypeClassExt::_Free_Buildup_Image()
{
    if (IsFreeBuildup && BuildupData) {
        DEV_DEBUG_WARNING("Freeing loaded buildup image for {}\n", Name());

        /**
         *  The original function would incorrectly try to free memory
         *  that the game does not actually allocate, and as a result of
         *  this, Vinifera's new memory management triggers an assertion
         *  because this is no longer allowed. The original game silently
         *  failed when doing this.
         *
         *  We now remove this and just correctly nullify the pointer.
         */
        //delete BuildupData;

        BuildupData = nullptr;
    }
}


/**
 *  Reimplementation of BuildingClass::Set_Base_Defense_Values.
 *
 *  @author: ZivDero
 */
void BuildingTypeClassExt::_Set_Base_Defense_Values()
{
    if (IsBaseDefense) {
        const WeaponTypeClass* weapon = Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon;

        if (weapon != nullptr) {
            int damage = weapon->Attack / (weapon->ROF * 0.025);

            if (weapon->Bullet->IsAntiAircraft) {
                AntiAirValue = std::min(static_cast<double>(Rule->MaximumBaseDefenseValue), damage * Verses::Get_Modifier(ARMOR_STEEL, weapon->WarheadPtr));
            }

            if (weapon->Bullet->IsAntiGround) {
                AntiArmorValue = std::min(static_cast<double>(Rule->MaximumBaseDefenseValue), damage * Verses::Get_Modifier(ARMOR_STEEL, weapon->WarheadPtr));
                AntiInfantryValue = std::min(static_cast<double>(Rule->MaximumBaseDefenseValue), damage * Verses::Get_Modifier(ARMOR_NONE, weapon->WarheadPtr));
            }
        }
    }
}


/**
 *  #issue-433
 *
 *  Disables unintuitive Westwood logic that links a BuildingType's cost to the cost
 *  of its FreeUnit or pad aircraft.
 *
 *  Author: Rampastring
 */
int BuildingTypeClassExt::_Raw_Cost()
{
    return TechnoTypeClass::Raw_Cost();
}


/**
 *  #issue-433
 *
 *  Disables unintuitive Westwood logic that links a BuildingType's cost to the cost
 *  of its FreeUnit or pad aircraft.
 *
 *  Author: Rampastring
 */
int BuildingTypeClassExt::_Cost_Of(HouseClass* house)
{
    return TechnoTypeClass::Cost_Of(house);
}


/**
 *  Pads the building Z shape data with a bunch of empty space to prevent out of bounds reads.
 *
 *  Author: ZivDero, tomsons26
 */
static char* BuildingZShapeBuffer = nullptr;
void BuildingTypeClassExt::_Fetch_Z_Data()
{
    if (BuildingZShapeBuffer != nullptr) {
        delete[] BuildingZShapeBuffer;
        BuildingZShapeBuffer = nullptr;
        BuildingZShape = nullptr;
    }

    int size = CCFileClass("BUILDNGZ.SHP").Size();
    BuildingZShapeBuffer = new char[size * 3];
    memset(BuildingZShapeBuffer, 0, size * 3);
    BuildingZShape = reinterpret_cast<ShapeSet*>(&BuildingZShapeBuffer[size]);
    memcpy(const_cast<ShapeSet*>(BuildingZShape), MFCD::Retrieve("BUILDNGZ.SHP"), size);

    char* data = static_cast<char*>(BuildingZShape->Get_Data(0));
    int width = BuildingZShape->Get_Width();
    int height = BuildingZShape->Get_Height();

    for (int h = 0; h < height; h++) {
        for (int w = 0; w < width; w++) {
            if (data[h * width + w] != 0) {
                data[h * width + w] -= 39;
            }
        }
    }
}


/**
 *  BuldingZ is freed at the end, need to patch it to delete the correct pointer.
 *
 *  Author: ZivDero, tomsons26
 */
DEFINE_HOOK(0x00601ECB, _Prog_End_Delete_BuildingZ_Data_Patch, 0)
{
    if (BuildingZShapeBuffer != nullptr) {
        delete[] BuildingZShapeBuffer;
        BuildingZShapeBuffer = nullptr;
        BuildingTypeClass::BuildingZShape = nullptr;
    }

    return 0x00601EE3;
}


/**
 *  Patch to fetch the new building images.
 *
 *  Author: ZivDero
 */
DEFINE_HOOK(0x0043FDBF, _BuildingTypeClass_Init_Fetch_Image_Patch, 0)
{
    GET(BuildingTypeClass*, btype, ESI);
    GET(TheaterType, theater, EDI);

    btype->Fetch_Building_Normal_Image(theater);

    Extension::Fetch(btype)->Fetch_Building_Normal_Image(theater);

    return 0x0043FDC7;
}


/**
 *  Main function for patching the hooks.
 */
void BuildingTypeClassExtension_Hooks()
{
    /**
     *  Initialises the extended class.
     */
    BuildingTypeClassExtension_Init();

    Patch_Jump(0x00443CF0, &BuildingTypeClassExt::_Free_Buildup_Image);
    Patch_Jump(0x00443D20, &BuildingTypeClassExt::_Set_Base_Defense_Values);
    Patch_Jump(0x00440000, &BuildingTypeClassExt::_Raw_Cost);
    Patch_Jump(0x00440080, &BuildingTypeClassExt::_Cost_Of);
    Patch_Jump(0x0043FB50, &BuildingTypeClassExt::_Fetch_Z_Data);
}
