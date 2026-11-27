/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended BulletClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "bulletext_hooks.h"

#include "anim.h"
#include "asserthandler.h"
#include "building.h"
#include "bullet.h"
#include "bullettype.h"
#include "bullettypeext.h"
#include "colorscheme.h"
#include "custompalette.h"
#include "extension.h"
#include "hooker.h"
#include "house.h"
#include "infantry.h"
#include "iomap.h"
#include "overlaytype.h"
#include "syringe.h"
#include "techno.h"
#include "warheadtype.h"
#include "warheadtypeext.h"


/**
 *  A fake class for implementing new member functions which allow
 *  access to the "this" pointer of the intended class.
 *
 *  @note: This must not contain a constructor or destructor!
 *  @note: All functions must be prefixed with "_" to prevent accidental virtualization.
 */
static DECLARE_EXTENDING_CLASS_AND_PAIR(BulletClass)
{
public:
    bool _Is_Forced_To_Explode(Coord& coord);
};


/**
 *  #issue-444
 *
 *  Full replacement of BulletClass::Is_Forced_To_Explode.
 *
 *  @author: 10/10/1996 JLB : Created.
 *           22/10/2024 Rampastring : Adjustments for Tiberian Sun.
 */
bool BulletClassExt::_Is_Forced_To_Explode(Coord& coord)
{
    coord = Position;
    CellClass* cellptr = &Map[PositionCoord];
    int height = HeightAGL;

    /*
    **  Check for impact on a wall or other high obstacle.
    */
    if (!Class->IsHigh && cellptr->Overlay != OVERLAY_NONE && OverlayTypes[cellptr->Overlay]->IsHigh && height < 100) {
        return true;
    }

    /*
    **  Check for impact on the ground.
    */
    if (height < 0) {
        return true;
    }

    /*
    **  Check to make sure that underwater projectiles (torpedoes) will not
    **  travel in anything but water.
    */
    const auto bullettypeext = Extension::Fetch(Class);
    if (bullettypeext->IsTorpedo) {
        int distance = ::Distance(Coord_Fraction(coord), Coord(CELL_LEPTON_W / 2, CELL_LEPTON_W / 2));

        if (cellptr->Land_Type() != LAND_WATER ||
            (distance < CELL_LEPTON_W / 3 && cellptr->Cell_Techno() != nullptr &&
            (Payback == nullptr || !Payback->House->Is_Ally(cellptr->Cell_Techno())))) {

            /*
            **  If the torpedo was blocked by a bridge, then force the
            **  torpedo to explode on top of that bridge cell.
            */
            if (cellptr->Is_Bridge_Here()) {
                coord = Coord_Snap(coord);
            }

            return true;
        }

        /*
        **  Torpedoes can be blocked by enemy objects on their path.
        */
        TechnoClass* celltechno = cellptr->Cell_Techno();

        if (celltechno != nullptr)
        {
            int snapdistance = CELL_LEPTON_W * 2;

            if (celltechno == TarCom || 
                (Distance(celltechno) < snapdistance && (Payback == nullptr || !Payback->House->Is_Ally(celltechno))))
            {
                /*
                **  If the techno is not a building, force
                **  explosion to be at center of techno object.
                **  Otherwise, explode in the center of the cell.
                */
                if (celltechno->Fetch_RTTI() != RTTI_BUILDING) {
                    coord = cellptr->Cell_Techno()->Target_Coord();
                }
                else {
                    coord = cellptr->Center_Coord();
                }

                return true;
            }
        }
    }

    /*
    **  Bullets are generally more effective when they are fired at flying objects.
    */
    if (Class->IsAntiAircraft && TarCom != nullptr && TarCom->In_Air() && Distance(TarCom) < 0x0080) {
        return true;
    }

    return false;
}


/**
 *  #issue-563
 * 
 *  Implements SpawnDelay for BulletTypes.
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x004447BF, _BulletClass_AI_SpawnDelay_Patch, 0)
{
    GET(BulletClass *, this_ptr, EBP);

    /**
     *  Fetch the extension instance.
     */
    BulletTypeClassExtension* bullettypeext = Extension::Fetch(this_ptr->Class);

    /**
     *  If this bullet has a custom spawn delay (defaults to the original delay of 3), perform that check first.
     */
    if (Frame % bullettypeext->SpawnDelay == 0) {
        goto create_trailer_anim;
    }

skip_anim:
    return 0x00444801;

create_trailer_anim:
    return 0x004447D0;
}


/**
 *  #issue-415
 * 
 *  Implements screen shake values for WarheadTypes.
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x00446652, _BulletClass_Logic_ShakeScreen_Patch, 0)
{
    GET(WarheadTypeClass *, warhead, EAX);
    GET_STACK(Coord*, coord, 0x0A8);

    R->EDI(coord);

    /**
     *  Fetch the extension instance.
     */
    auto warheadext = Extension::Fetch(warhead);

    /**
     *  If this warhead has screen shake values defined, then set the blitter
     *  offset values. GScreenClass::Blit will handle the rest for us.
     */
    if (warheadext->ShakePixelXLo > 0 || warheadext->ShakePixelXHi > 0) {
        Map.ScreenX = Sim_Random_Pick(warheadext->ShakePixelXLo, warheadext->ShakePixelXHi);
    }
    if (warheadext->ShakePixelYLo > 0 || warheadext->ShakePixelYHi > 0) {
        Map.ScreenY = Sim_Random_Pick(warheadext->ShakePixelYLo, warheadext->ShakePixelYHi);
    }

    /**
     *  Jumps back to IsEMEffect check.
     */
    return 0x00446659;
}


/**
 *  Aftermath: Draws projectiles with a custom palette (ART.INI "Palette=") and
 *  with the house color of the firer (rules "FirersPalette=yes").
 *
 *  At this point BulletClass::Draw_It has picked NormalDrawer or, with
 *  AnimPalette=yes, AnimDrawer into EBX for the projectile itself. The shadow
 *  is drawn earlier with NormalDrawer and only darkens, so it is unchanged.
 *
 *  - Palette= alone: the custom palette, no remap.
 *  - Palette= and FirersPalette=yes: the custom palette with the house color
 *    ramp of the firer.
 *  - FirersPalette=yes alone: the UNIT palette with the house color of the
 *    firer, as in Yuri's Revenge.
 *  A projectile whose firer is gone uses the first color scheme.
 *
 *  @author: Aftermath
 */
DEFINE_HOOK(0x00445EAA, _BulletClass_Draw_It_Custom_Palette_Patch, 6)
{
    GET(BulletClass *, this_ptr, ESI);

    const BulletTypeClass *type = this_ptr->Class;
    if (type == nullptr) {
        return 0;
    }

    const BulletTypeClassExtension *typeext = Extension::Fetch(type);
    if (typeext == nullptr) {
        return 0;
    }

    ConvertClass *drawer = nullptr;

    if (typeext->IsFirersPalette) {
        int scheme = 0;
        if (this_ptr->Payback != nullptr && this_ptr->Payback->House != nullptr) {
            scheme = this_ptr->Payback->House->Scheme;
        }
        if (scheme < 0 || scheme >= ColorSchemes.Count()) {
            scheme = 0;
        }

        drawer = CustomPalette::Scheme_Converter(typeext->PaletteName.c_str(), scheme);
        if (drawer == nullptr && ColorSchemes.Count() > 0) {
            drawer = ColorSchemes[scheme]->Converter;
        }

    } else {
        drawer = CustomPalette::Plain_Converter(typeext->PaletteName.c_str());
    }

    if (drawer != nullptr) {
        R->EBX(drawer);
    }

    return 0;
}


/**
 *  Aftermath: The bullet BulletClass::Draw_Voxel is drawing. The function keeps
 *  "this" on the stack only, so it is taken here, where EAX holds it.
 */
static const BulletClass *_DrawnVoxelBullet = nullptr;

DEFINE_HOOK(0x004473C6, _BulletClass_Draw_Voxel_Fetch_This_Patch, 11)
{
    GET(const BulletClass *, this_ptr, EAX);

    _DrawnVoxelBullet = this_ptr;

    return 0;
}


/**
 *  Aftermath: Converter for a voxel projectile. The game blits it with the
 *  converter of the projectile's own color scheme (BulletType Color=).
 *
 *  - Palette=: the custom palette with the house color ramp of that scheme,
 *    or of the firer with FirersPalette=yes.
 *  - FirersPalette=yes alone: the game's palette with the firer's house color.
 *
 *  @author: Aftermath
 */
static ConvertClass *Voxel_Bullet_Converter()
{
    const BulletClass *bullet = _DrawnVoxelBullet;
    if (bullet == nullptr || bullet->Class == nullptr) {
        return nullptr;
    }

    const BulletTypeClass *type = bullet->Class;
    const BulletTypeClassExtension *typeext = Extension::Fetch(type);
    if (typeext == nullptr || (typeext->PaletteName.empty() && !typeext->IsFirersPalette)) {
        return nullptr;
    }

    int scheme = type->Color;
    if (typeext->IsFirersPalette && bullet->Payback != nullptr && bullet->Payback->House != nullptr) {
        scheme = bullet->Payback->House->Scheme;
    }
    if (scheme < 0 || scheme >= ColorSchemes.Count()) {
        scheme = 0;
    }

    ConvertClass *drawer = CustomPalette::Scheme_Converter(typeext->PaletteName.c_str(), scheme);
    if (drawer == nullptr && typeext->IsFirersPalette && ColorSchemes.Count() > 0) {
        drawer = ColorSchemes[scheme]->Converter;
    }

    return drawer;
}


/**
 *  Aftermath: Converter for the blitter lookup in BulletClass::Draw_Voxel (ECX).
 *
 *  @author: Aftermath
 */
DEFINE_HOOK(0x004473EE, _BulletClass_Draw_Voxel_Blitter_Custom_Palette_Patch, 6)
{
    ConvertClass *drawer = Voxel_Bullet_Converter();
    if (drawer == nullptr) {
        return 0;
    }

    R->ECX(drawer);
    return 0x004473F4;
}


/**
 *  Aftermath: Converter for the Blit_Block call in BulletClass::Draw_Voxel (EDX).
 *
 *  @author: Aftermath
 */
DEFINE_HOOK(0x00447416, _BulletClass_Draw_Voxel_Custom_Palette_Patch, 6)
{
    ConvertClass *drawer = Voxel_Bullet_Converter();
    _DrawnVoxelBullet = nullptr;

    if (drawer == nullptr) {
        return 0;
    }

    R->EDX(drawer);
    return 0x0044741C;
}


/**
 *  Main function for patching the hooks.
 */
void BulletClassExtension_Hooks()
{
    Patch_Jump(0x004462C0, &BulletClassExt::_Is_Forced_To_Explode);
}
