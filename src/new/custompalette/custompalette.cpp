/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Per-type custom palettes for SHP objects (ART.INI "Palette=").
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "custompalette.h"

#include "animtype.h"
#include "animtypeext.h"
#include "ccfile.h"
#include "convert.h"
#include "colorscheme.h"
#include "debughandler.h"
#include "extension.h"
#include "ionstorm.h"
#include "lightconvert.h"
#include "mixfile.h"
#include "palette.h"
#include "scenario.h"
#include "technotype.h"
#include "technotypeext.h"
#include "terraintype.h"
#include "terraintypeext.h"
#include "theatertype.h"
#include "tibsun_globals.h"

#include <cstring>
#include <string>
#include <vector>


namespace
{

/**
 *  The game's table of palette entries that take the lighting tint. Color schemes
 *  build their converters with it (ColorScheme::ColorScheme at 0x005E24F0).
 */
bool *const SchemeTintMask = reinterpret_cast<bool *>(0x0070CEAC);

/**
 *  The tint level that leaves colors unchanged.
 */
constexpr int NORMAL_TINT = 1000;

/**
 *  The remap ramp in a palette: the 16 entries that take the house color.
 */
constexpr int REMAP_FIRST = 16;
constexpr int REMAP_COUNT = 16;


/**
 *  One converter of a custom palette for one color scheme.
 *
 *  LightConvertClass keeps a pointer to the art palette and rebuilds its tables
 *  from it whenever the tint changes, so the remapped palette must live as long
 *  as the converter does.
 */
struct SchemeDrawer
{
    PaletteClass Remapped;
    LightConvertClass *Drawer = nullptr;
    bool IsIonTinted = false;
};


/**
 *  A converter of a custom palette for one cell light tint.
 */
struct TintDrawer
{
    int RedTint = 0;
    int GreenTint = 0;
    int BlueTint = 0;
    LightConvertClass *Drawer = nullptr;
};


/**
 *  A loaded custom palette and its converters, one slot per color scheme.
 */
struct PaletteEntry
{
    std::string Name;
    bool IsValid = false;
    PaletteClass Palette;
    std::vector<SchemeDrawer *> Drawers;

    /**
     *  Converter without a house color ramp, built like the game's AnimDrawer
     *  for ANIM.PAL. Used by animations.
     */
    ConvertClass *PlainDrawer = nullptr;

    /**
     *  Converters for terrain objects, one per cell light tint, built like
     *  the game's iso tile drawers (IsometricTileTypeClass::Find_Or_Make_Drawer).
     */
    std::vector<TintDrawer *> TintDrawers;
};


/**
 *  The tint a LightConvertClass was built with and its ion storm state. The
 *  members are private in TS++, so they are read at the offsets the game
 *  uses (LightConvertClass::Apply_Tint at 0x00502B90).
 */
struct LightConvertTint
{
    int NormalRed;
    int NormalGreen;
    int NormalBlue;
    int IonRed;
    int IonGreen;
    int IonBlue;
    bool UseIonLighting;
};

LightConvertTint Read_Tint(const LightConvertClass &drawer)
{
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&drawer);

    LightConvertTint tint;
    std::memcpy(&tint.NormalRed, bytes + 0x1A0, sizeof(int));
    std::memcpy(&tint.NormalGreen, bytes + 0x1A4, sizeof(int));
    std::memcpy(&tint.NormalBlue, bytes + 0x1A8, sizeof(int));
    std::memcpy(&tint.IonRed, bytes + 0x1AC, sizeof(int));
    std::memcpy(&tint.IonGreen, bytes + 0x1B0, sizeof(int));
    std::memcpy(&tint.IonBlue, bytes + 0x1B4, sizeof(int));
    tint.UseIonLighting = bytes[0x1B8] != 0;
    return tint;
}


/**
 *  The number of intensity levels the game gives AnimDrawer (Init_Theater).
 */
constexpr int ANIM_INTENSITY_LEVELS = 63;


std::vector<PaletteEntry *> Entries;


/**
 *  Loads a .PAL file from the mix files and converts it from the 6-bit file
 *  format to 8-bit color values, as the game does for its own palettes.
 */
bool Load_Palette_File(const char *filename, PaletteClass &palette)
{
    const void *data = MFCD::Retrieve(filename);
    if (data == nullptr) {
        return false;
    }

    std::memcpy(&palette, data, sizeof(palette));

    unsigned char *bytes = palette;
    for (int i = 0; i < PaletteClass::COLOR_COUNT * 3; ++i) {
        bytes[i] = static_cast<unsigned char>(bytes[i] << 2);
    }

    return true;
}


/**
 *  Resolves the palette name to a file and loads it. A name without an extension
 *  gets the theater suffix and ".PAL" (CUSTOM -> CUSTOMTEM.PAL); when that file
 *  does not exist, CUSTOM.PAL is tried. A name with an extension is used as is.
 */
bool Load_Palette(const char *name, PaletteClass &palette)
{
    char filename[64];

    if (std::strchr(name, '.') != nullptr) {
        std::snprintf(filename, sizeof(filename), "%s", name);
        if (Load_Palette_File(filename, palette)) {
            DEBUG_INFO("CustomPalette: Loaded \"{}\".\n", filename);
            return true;
        }

    } else {
        const char *suffix = "";
        if (Scen != nullptr && Scen->Theater != THEATER_NONE) {
            suffix = TheaterTypeClass::Suffix_From(Scen->Theater);
        }

        std::snprintf(filename, sizeof(filename), "%s%s.PAL", name, suffix);
        if (Load_Palette_File(filename, palette)) {
            DEBUG_INFO("CustomPalette: Loaded \"{}\".\n", filename);
            return true;
        }

        std::snprintf(filename, sizeof(filename), "%s.PAL", name);
        if (Load_Palette_File(filename, palette)) {
            DEBUG_INFO("CustomPalette: Loaded \"{}\" (no theater variant found).\n", filename);
            return true;
        }
    }

    DEBUG_WARNING("CustomPalette: Palette \"{}\" not found, the default palette is used.\n", name);
    return false;
}


/**
 *  Finds the entry for the palette name, loading the palette on first use.
 *  Failed loads are remembered too, so a missing file is only looked for once.
 */
PaletteEntry *Fetch_Entry(const char *name)
{
    for (PaletteEntry *entry : Entries) {
        if (_stricmp(entry->Name.c_str(), name) == 0) {
            return entry;
        }
    }

    PaletteEntry *entry = new PaletteEntry;
    entry->Name = name;
    entry->IsValid = Load_Palette(name, entry->Palette);
    Entries.push_back(entry);

    return entry;
}


/**
 *  Builds the converter for one color scheme: the custom palette with the
 *  scheme's remap ramp, lit the same way as the scheme's own converter.
 */
SchemeDrawer *Build_Drawer(const PaletteEntry &entry, const ColorScheme &scheme)
{
    SchemeDrawer *drawer = new SchemeDrawer;

    /**
     *  The scheme's palette is the UNIT palette with the house color ramp shaded
     *  into entries 16-31 (see Build_Light_Converter). Take that ramp over so the
     *  house colors stay the same with any custom palette.
     */
    drawer->Remapped = entry.Palette;
    for (int i = REMAP_FIRST; i < REMAP_FIRST + REMAP_COUNT; ++i) {
        drawer->Remapped[i] = scheme.Palette[i];
    }

    /**
     *  #NOTE: field_310 is the scheme's number of intensity levels
     *         (ColorScheme::ColorScheme stores it at offset 0x310).
     */
    const int intensity_levels = static_cast<int>(scheme.field_310);

    /**
     *  The constructor already applies the ion storm tint when a storm is active.
     */
    drawer->Drawer = new LightConvertClass(&drawer->Remapped, &GamePalette, VisibleSurface,
        NORMAL_TINT, NORMAL_TINT, NORMAL_TINT, false, SchemeTintMask, intensity_levels);
    drawer->IsIonTinted = IonStorm_Is_Active();

    return drawer;
}


/**
 *  Brings the converter in line with the ion storm. The game tints the scheme
 *  converters when a storm starts and restores them when it ends; doing the same
 *  here on first use after the change avoids hooking every place that does it.
 */
void Update_Ion_Tint(SchemeDrawer &drawer, int intensity_levels)
{
    const bool is_storm = IonStorm_Is_Active();
    if (drawer.IsIonTinted == is_storm) {
        return;
    }

    if (is_storm) {

        /**
         *  The game leaves single-level schemes untinted during a storm.
         */
        if (intensity_levels > 1) {
            drawer.Drawer->Apply_Tint(NORMAL_TINT * Scen->IonRedTint / 100,
                                      NORMAL_TINT * Scen->IonGreenTint / 100,
                                      NORMAL_TINT * Scen->IonBlueTint / 100, true);
        }

    } else {
        drawer.Drawer->Apply_Tint(-1, -1, -1, false);
    }

    drawer.IsIonTinted = is_storm;
}


/**
 *  Fetches the converter of the named palette for the given color scheme,
 *  building it on first use.
 */
ConvertClass *Scheme_Converter(const char *name, int scheme)
{
    if (scheme < 0 || scheme >= ColorSchemes.Count()) {
        return nullptr;
    }

    PaletteEntry *entry = Fetch_Entry(name);
    if (!entry->IsValid) {
        return nullptr;
    }

    if (static_cast<int>(entry->Drawers.size()) < ColorSchemes.Count()) {
        entry->Drawers.resize(ColorSchemes.Count(), nullptr);
    }

    const ColorScheme *colorscheme = ColorSchemes[scheme];

    SchemeDrawer *&drawer = entry->Drawers[scheme];
    if (drawer == nullptr) {
        drawer = Build_Drawer(*entry, *colorscheme);
    }

    Update_Ion_Tint(*drawer, static_cast<int>(colorscheme->field_310));

    return drawer->Drawer;
}

}


/**
 *  Fetches the converter to draw the given type with for the given color scheme.
 *
 *  @author: Aftermath
 */
ConvertClass *CustomPalette::Techno_Converter(const TechnoTypeClass *type, int scheme)
{
    if (type == nullptr) {
        return nullptr;
    }

    const TechnoTypeClassExtension *typeext = Extension::Fetch(type);
    if (typeext == nullptr || typeext->PaletteName.empty()) {
        return nullptr;
    }

    return Scheme_Converter(typeext->PaletteName.c_str(), scheme);
}


/**
 *  Fetches the remapping converter to draw the given animation type with for
 *  the given color scheme.
 *
 *  @author: Aftermath
 */
ConvertClass *CustomPalette::Anim_Remap_Converter(const AnimTypeClass *type, int scheme)
{
    if (type == nullptr) {
        return nullptr;
    }

    const AnimTypeClassExtension *typeext = Extension::Fetch(type);
    if (typeext == nullptr || typeext->PaletteName.empty()) {
        return nullptr;
    }

    return Scheme_Converter(typeext->PaletteName.c_str(), scheme);
}


/**
 *  Fetches the converter to draw the given animation type with.
 *
 *  @author: Aftermath
 */
ConvertClass *CustomPalette::Anim_Converter(const AnimTypeClass *type)
{
    if (type == nullptr) {
        return nullptr;
    }

    const AnimTypeClassExtension *typeext = Extension::Fetch(type);
    if (typeext == nullptr || typeext->PaletteName.empty()) {
        return nullptr;
    }

    PaletteEntry *entry = Fetch_Entry(typeext->PaletteName.c_str());
    if (!entry->IsValid) {
        return nullptr;
    }

    /**
     *  Same setup as the game's AnimDrawer: no tint, lighting comes from the
     *  brightness the draw call passes in.
     */
    if (entry->PlainDrawer == nullptr) {
        entry->PlainDrawer = new ConvertClass(&entry->Palette, &GamePalette, VisibleSurface, ANIM_INTENSITY_LEVELS);
    }

    return entry->PlainDrawer;
}


/**
 *  Fetches the converter to draw the given terrain type with in a cell whose
 *  own drawer is cell_drawer.
 *
 *  @author: Aftermath
 */
ConvertClass *CustomPalette::Terrain_Converter(const TerrainTypeClass *type, const LightConvertClass *cell_drawer)
{
    if (type == nullptr || cell_drawer == nullptr) {
        return nullptr;
    }

    const TerrainTypeClassExtension *typeext = Extension::Fetch(type);
    if (typeext == nullptr || typeext->PaletteName.empty()) {
        return nullptr;
    }

    PaletteEntry *entry = Fetch_Entry(typeext->PaletteName.c_str());
    if (!entry->IsValid) {
        return nullptr;
    }

    const LightConvertTint cell_tint = Read_Tint(*cell_drawer);

    /**
     *  One converter per light tint, as the game does for iso tiles. The tint
     *  values are already clamped by the game when the cell drawer was made.
     */
    TintDrawer *drawer = nullptr;
    for (TintDrawer *candidate : entry->TintDrawers) {
        if (candidate->RedTint == cell_tint.NormalRed
            && candidate->GreenTint == cell_tint.NormalGreen
            && candidate->BlueTint == cell_tint.NormalBlue) {
            drawer = candidate;
            break;
        }
    }

    if (drawer == nullptr) {
        drawer = new TintDrawer;
        drawer->RedTint = cell_tint.NormalRed;
        drawer->GreenTint = cell_tint.NormalGreen;
        drawer->BlueTint = cell_tint.NormalBlue;
        drawer->Drawer = new LightConvertClass(&entry->Palette, &GamePalette, VisibleSurface,
            cell_tint.NormalRed, cell_tint.NormalGreen, cell_tint.NormalBlue, !entry->TintDrawers.empty());
        entry->TintDrawers.push_back(drawer);
    }

    /**
     *  Follow the ion storm state of the cell drawer, which the game switches
     *  for all tile drawers when a storm starts or ends.
     */
    const LightConvertTint tint = Read_Tint(*drawer->Drawer);
    if (cell_tint.UseIonLighting) {
        if (!tint.UseIonLighting
            || tint.IonRed != cell_tint.IonRed
            || tint.IonGreen != cell_tint.IonGreen
            || tint.IonBlue != cell_tint.IonBlue) {
            drawer->Drawer->Apply_Tint(cell_tint.IonRed, cell_tint.IonGreen, cell_tint.IonBlue, true);
        }
    } else if (tint.UseIonLighting) {
        drawer->Drawer->Apply_Tint(-1, -1, -1, false);
    }

    return drawer->Drawer;
}


/**
 *  Releases all palettes and converters.
 *
 *  @author: Aftermath
 */
void CustomPalette::Clear()
{
    for (PaletteEntry *entry : Entries) {
        for (TintDrawer *drawer : entry->TintDrawers) {
            delete drawer->Drawer;
            delete drawer;
        }
        delete entry->PlainDrawer;
        for (SchemeDrawer *drawer : entry->Drawers) {
            if (drawer != nullptr) {
                delete drawer->Drawer;
                delete drawer;
            }
        }
        delete entry;
    }

    Entries.clear();
}
