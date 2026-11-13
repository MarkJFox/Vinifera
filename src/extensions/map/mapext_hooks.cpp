/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended MapClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "mapext_hooks.h"

#include "battleui.h"
#include "buildingtypeext.h"
#include "cell.h"
#include "extension.h"
#include "hooker.h"
#include "map.h"
#include "mouse.h"
#include "object.h"
#include "objecttype.h"
#include "overlaytype.h"
#include "rules.h"
#include "syringe.h"
#include "tactical.h"
#include "techno.h"
#include "tibsun_functions.h"
#include "tibsun_globals.h"
#include "vinifera_globals.h"

#include <algorithm>
#include <cstdlib>


/**
 *  A fake class for implementing new member functions which allow
 *  access to the "this" pointer of the intended class.
 *
 *  @note: This must not contain a constructor or destructor!
 *  @note: All functions must be prefixed with "_" to prevent accidental virtualization.
 */
static class MapClassExt final : public MapClass
{
public:
    void _Place_Down(Cell& cell, ObjectClass* object);
    void _Pick_Up(Cell& cell, ObjectClass* object);
    void _Detach(AbstractClass* target, bool all);

    // Cumulative cell counts for each radius
    static std::vector<int> RadiusCountTable;
    // Ordered list of all relative cell coordinates
    static std::vector<Cell> RadiusOffsets;
    // Vectors pointing toward the parent cell for occlusion logic
    static std::vector<Cell> OcclusionOffsets;
    static void _Calculate_Sight_Radius_If_Needed(int target_sight_range);
};

// Initialize static members with the values for Radius 0
std::vector<int> MapClassExt::RadiusCountTable = {1};
std::vector<Cell> MapClassExt::RadiusOffsets = {Cell(0, 0)};
std::vector<Cell> MapClassExt::OcclusionOffsets = {Cell(0, 0)};

static Cell Sight_Occlusion_Offset(int x, int y)
{
    const auto sign = [](int value) { return (value > 0) - (value < 0); };
    const int abs_x = std::abs(x);
    const int abs_y = std::abs(y);
    return Cell(static_cast<short>(abs_x >= abs_y ? -sign(x) : 0), static_cast<short>(abs_y >= abs_x ? -sign(y) : 0));
}

MapObserverSight Map_Observer_Sight(const Coord& origin, int range)
{
    const int shift = Tactical::Z_Lepton_To_Pixel(origin.Z) / -CELL_PIXEL_W * CELL_LEPTON_W;
    const Cell center = Coord(origin.X + shift, origin.Y + shift, origin.Z).As_Cell();
    return {center, center - origin.As_Cell() - Cell(2, 2), origin.Z / LEVEL_LEPTON_H, Map.In_Radar(center) ? range : 0};
}

bool MapObserverSight::Contains(const Cell& target) const
{
    // The caller has already checked that the target cell lies inside the playable map.
    if (Range <= 0) return false;
    const int x = target.X - Center.X;
    const int y = target.Y - Center.Y;
    const int abs_x = std::abs(x);
    const int abs_y = std::abs(y);
    if (std::max(abs_x, abs_y) + std::min(abs_x, abs_y) / 2 > Range) return false;
    if (static_cast<long long>(x) * x + static_cast<long long>(y) * y > static_cast<long long>(Range) * Range) return false;
    if (!Rule->IsRevealByHeight) return true;
    // The same projected center and parent cell as Sight_From's height test.
    const Cell occlusion = target - HeightOffset + Sight_Occlusion_Offset(x, y);
    return Map.In_Radar(occlusion) && Map[occlusion].Height <= Level + 3;
}

bool Map_Defense_Line_Clear(const Cell& from, const Cell& to)
{
    if (!Map.In_Radar(from) || !Map.In_Radar(to)) return false;
    const int x = to.X - from.X;
    const int y = to.Y - from.Y;
    const int steps = std::max(std::abs(x), std::abs(y));
    const auto* source = Map[from].Cell_Building();
    const int source_height = Map[from].Height;
    const int target_height = Map[to].Height;
    for (int step = 1; step < steps; ++step) {
        const Cell position(static_cast<short>(from.X + x * step / steps), static_cast<short>(from.Y + y * step / steps));
        if (!Map.In_Radar(position)) return false;
        const auto& cell = Map[position];
        const auto* building = cell.Cell_Building();
        if (building && building != source) return false;
        if (cell.Overlay >= OVERLAY_FIRST && cell.Overlay < OverlayTypes.Count()) {
            const auto* overlay = OverlayTypes[cell.Overlay];
            if (overlay && overlay->IsWall) return false;
        }
        if (cell.Height * steps > source_height * (steps - step) + target_height * step + steps) return false;
    }
    return true;
}


/**
 *  Fixes a buffer overflow crash in the original MapClass::Place_Down
 *  where the game accessed the cell array before checking that the cell
 *  number is within bounds.
 *
 *  @author: Rampastring
 */
void MapClassExt::_Place_Down(Cell& cell, ObjectClass* object)
{
    if (!object) return;

    if (object->Class_Of()->IsFootprint && object->In_Which_Layer() == LAYER_GROUND) {
        Cell xlist[BuildingTypeClassExtension::CustomFoundationStorageSize];
        List_Copy(object->Occupy_List(), std::size(xlist), xlist);
        Cell const* list = xlist;
        while (*list != REFRESH_EOL) {
            Cell newcell = cell + *list++;
            int cellnum = newcell.As_Cell_Number();

            if (cellnum >= 0 && cellnum < MAP_CELL_TOTAL && Array[cellnum] != nullptr) {
                (*this)[newcell].Occupy_Down(object, object->IsOnBridge);
                (*this)[newcell].Recalc_Attributes();
            }
        }
    }
}


/**
 *  Fixes a buffer overflow crash in the original MapClass::Pick_Up
 *  where the game accessed the cell array before checking that the cell
 *  number is within bounds.
 *
 *  @author: Rampastring
 */
void MapClassExt::_Pick_Up(Cell& cell, ObjectClass* object)
{
    if (!object) return;

    if (object->Class_Of()->IsFootprint && object->In_Which_Layer() == LAYER_GROUND) {
        Cell xlist[BuildingTypeClassExtension::CustomFoundationStorageSize];
        List_Copy(object->Occupy_List(), std::size(xlist), xlist);
        Cell const* list = xlist;
        while (*list != REFRESH_EOL) {
            Cell newcell = cell + *list++;
            int cellnum = newcell.As_Cell_Number();

            if (cellnum >= 0 && cellnum < MAP_CELL_TOTAL && Array[cellnum] != nullptr) {
                (*this)[newcell].Occupy_Up(object, object->IsOnBridge);
                (*this)[newcell].Recalc_Attributes();
            }
        }
    }
}


/**
 *  Proxy for MapClass::Detach
 *
 *  @author: ZivDero
 */
void MapClassExt::_Detach(AbstractClass* target, bool all)
{
    MapClass::Detach(target, all);

    if (target->RTTI == RTTI_FACTORY) {
        BattleUI.Get_Sidebar().Detach(target);
    }
}

/**
 *  Dynamically populates the radius and occlusion tables if they are not populated for a given radius (sight range).
 *  This allows any desired radius of cells to be revealed when called through MapClass::From_Sight.
 *  Results are cached up to the highest calculated sight range, in which case, we'll simply use the calculated results.
 *
 *  @author: JoyfulShush
 */
void MapClassExt::_Calculate_Sight_Radius_If_Needed(int sight_range)
{
    int current_max_radius = static_cast<int>(RadiusCountTable.size());

    if (sight_range <= current_max_radius - 1) {
        return;
    }

    for (int current_radius = current_max_radius; current_radius <= sight_range; ++current_radius) {
        int search_bounds = current_radius + 2;

        for (int cell_y = -search_bounds; cell_y <= search_bounds; ++cell_y) {
            for (int cell_x = -search_bounds; cell_x <= search_bounds; ++cell_x) {

                int absolute_x = std::abs(cell_x);
                int absolute_y = std::abs(cell_y);

                int octagonal_distance = std::max(absolute_x, absolute_y) + (std::min(absolute_x, absolute_y) / 2);

                if (octagonal_distance == current_radius) {
                    RadiusOffsets.push_back(Cell(cell_x, cell_y));

                    OcclusionOffsets.push_back(Sight_Occlusion_Offset(cell_x, cell_y));
                }
            }
        }

        RadiusCountTable.push_back(static_cast<int>(RadiusOffsets.size()));
    }
}


/*
 *  Similar implementation to Tactical::Select_These, however instead of being specific to the tactical view,
 *  applies the selection callback on all technos in the map.
 *  Used to conditionally select technos based on the callback logic.
 * 
 *  @author: JoyfulShush
 */
void Map_Select_These(void (*select_callback)(ObjectClass*))
{
    if (select_callback == nullptr) {
        return;
    }

    for (int index = 0; index < Technos.Count(); ++index) {
        TechnoClass* techno = Technos[index];

        if (techno == nullptr || !techno->IsActive || !techno->IsDown) {
            continue;
        }

        if (!Map.In_Radar(techno->Center_Coord())) {
            continue;
        }

        select_callback(techno);
    }
}


/*
 *  Patches MapClass::From_Sight to no longer clamp the sight range to 10.
 *  Implements a dynamic algorithm handling that can compute the cells and counts needed for a given Sight Range.
 *  Used by units when they call Look, and by Reveal by Waypoints, allowing the game to reveal any desired radius.
 *  Additionally, removes the logic of 'incremental' which was intended to be performance saving,
 *  but only worked when height reveals was turned off and sight ranges were small enough.
 *
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x00510C9A, _From_Sight_Dynamic_Sight_Range_Patch, 6)
{
    int sight_range = R->EAX();

    MapClassExt::_Calculate_Sight_Radius_If_Needed(sight_range);

    int cell_count = MapClassExt::RadiusCountTable[sight_range];
    Cell* radius_offsets_ptr = MapClassExt::RadiusOffsets.data();
    Cell* occlusion_offsets_ptr = MapClassExt::OcclusionOffsets.data();

    R->ESI(cell_count);

    R->Stack(0x10, radius_offsets_ptr);
    R->Stack(0x4C, occlusion_offsets_ptr);

    return 0x510CFB;
}


/**
 *  Main function for patching the hooks.
 */
void MapClassExtension_Hooks()
{
    Patch_Jump(0x00511070, &MapClassExt::_Place_Down);
    Patch_Jump(0x005111B0, &MapClassExt::_Pick_Up);
    Patch_Call(0x00648BCF, &MapClassExt::_Detach);
    Patch_Call(0x00648B67, &MapClassExt::_Detach);
}
