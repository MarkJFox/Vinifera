/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Per-type custom palettes for SHP objects (ART.INI "Palette=").
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#pragma once


class AnimTypeClass;
class ConvertClass;
class LightConvertClass;
class TechnoTypeClass;
class SuperWeaponTypeClass;
class TerrainTypeClass;


/**
 *  Custom palettes give a TechnoType its own .PAL file in place of the theater's
 *  UNIT palette. The palette name is resolved like Yuri's Revenge does it: the
 *  theater suffix and ".PAL" are appended ("Palette=custom" loads CUSTOMTEM.PAL
 *  in temperate). A name that already carries an extension is loaded as written.
 *
 *  For every palette a converter is built per color scheme, so house colors keep
 *  working: the remap ramp (palette entries 16-31) is taken from the house's color
 *  scheme, everything else from the custom palette. The converters are kept outside
 *  the savegame and are rebuilt on demand.
 */
namespace CustomPalette
{
    /**
     *  Fetches the converter of the named palette with the house color ramp
     *  (entries 16-31) of the given color scheme, lit and ion storm tinted like
     *  the scheme's own converter. Returns nullptr for an empty name or a
     *  palette that could not be loaded.
     */
    ConvertClass *Scheme_Converter(const char *name, int scheme);

    /**
     *  Fetches the plain converter of the named palette, built like the game's
     *  AnimDrawer, NormalDrawer and CameoDrawer (no remap, no tint). Returns
     *  nullptr for an empty name or a palette that could not be loaded.
     */
    ConvertClass *Plain_Converter(const char *name);

    /**
     *  Fetches the converter of the named palette with the light tint and ion
     *  storm state of cell_drawer (a cell's drawer). Returns nullptr for an
     *  empty name or a palette that could not be loaded.
     */
    ConvertClass *Tint_Converter(const char *name, const LightConvertClass *cell_drawer);

    /**
     *  Fetches the converter to draw the given type with for the given color scheme.
     *  Returns nullptr when the type has no custom palette or the palette file could
     *  not be loaded, in which case the caller keeps the game's own converter.
     */
    ConvertClass *Techno_Converter(const TechnoTypeClass *type, int scheme);

    /**
     *  Fetches the converter to draw the given animation type with, in place of
     *  the game's AnimDrawer (ANIM.PAL). There is no house color ramp, as with
     *  ANIM.PAL. Returns nullptr when the type has no custom palette or the file
     *  could not be loaded.
     */
    ConvertClass *Anim_Converter(const AnimTypeClass *type);

    /**
     *  Fetches the converter to draw the given animation type with its own
     *  palette and the house color ramp of the given color scheme (for
     *  AltPalette=yes). Returns nullptr when the type has no custom palette
     *  or the file could not be loaded.
     */
    ConvertClass *Anim_Remap_Converter(const AnimTypeClass *type, int scheme);

    /**
     *  Fetches the converter to draw the given terrain type with, in place of
     *  the cell's drawer (the iso palette with the cell's light). The converter
     *  gets the same light tint and ion storm state as cell_drawer. Returns
     *  nullptr when the type has no custom palette or the file could not be
     *  loaded.
     */
    ConvertClass *Terrain_Converter(const TerrainTypeClass *type, const LightConvertClass *cell_drawer);

    /**
     *  Fetches the converter to draw the cameo of the given type with
     *  (ART.INI "CameoPalette="). Returns CameoDrawer when the type has no
     *  cameo palette or the file could not be loaded.
     */
    ConvertClass *Cameo_Converter(const TechnoTypeClass *type);

    /**
     *  Fetches the converter to draw the sidebar cameo of the given super
     *  weapon with (rules "SidebarPalette="). Returns CameoDrawer when the type
     *  has no sidebar palette or the file could not be loaded.
     */
    ConvertClass *Cameo_Converter(const SuperWeaponTypeClass *type);

    /**
     *  Releases all palettes and converters. Called whenever the theater is set up,
     *  since the palette files depend on it.
     */
    void Clear();
};
