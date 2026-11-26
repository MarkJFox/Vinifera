/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Spectator info tiles shown in the sidebar in place of the cameo
 *          strips while the local player is an observer.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "sidebar_observer_view.h"

#include "bsurface.h"
#include "colorscheme.h"
#include "drawshape.h"
#include "dsurface.h"
#include "extension.h"
#include "house.h"
#include "houseext.h"
#include "housetype.h"
#include "ccfile.h"
#include "mixfile.h"
#include "rgb.h"
#include "shapeset.h"
#include "side.h"
#include "sideext.h"
#include "spritecollection.h"
#include "textprint.h"
#include "tibsun_globals.h"
#include "vinifera_util.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>


namespace
{
/**
 *  Text layout inside a tile, relative to its top left corner.
 */
constexpr int TILE_TEXT_X = 5;
constexpr int TILE_TEXT_Y = 5;
constexpr int TILE_LINE_HEIGHT = 14;
constexpr int TILE_ICON_MARGIN = 4;


/**
 *  Loads an image surface once per name. The surfaces live for the whole
 *  session, like the cameo image surfaces.
 *
 *  @author: Aftermath
 */
BSurface* Fetch_Tile_Image(const char* name)
{
    static std::map<std::string, BSurface*> _cache;

    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    const auto found = _cache.find(name);
    if (found != _cache.end()) {
        return found->second;
    }

    BSurface* surface = Vinifera_Get_Image_Surface(name);
    _cache.emplace(name, surface);
    return surface;
}


/**
 *  Fetches a SHP of the given name (without extension) from the loaded mixes.
 *
 *  @author: Aftermath
 */
const ShapeSet* Fetch_Tile_Shape(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    std::string filename = name;
    filename += ".SHP";
    return MFCD::RetrieveT<ShapeSet>(filename.c_str());
}


/**
 *  Returns the named color scheme, or the fallback if the scheme is missing.
 *
 *  @author: Aftermath
 */
ColorScheme* Fetch_Tile_Scheme(const char* name, ColorScheme* fallback)
{
    ColorScheme* scheme = Fetch_Scheme_By_Name(name, 1);
    return scheme != nullptr ? scheme : fallback;
}


/**
 *  Returns the total number of enemy units this house has killed.
 *
 *  @author: Aftermath
 */
unsigned Count_Units_Killed(const HouseClass& house)
{
    unsigned total = 0;
    for (unsigned kills : house.UnitsKilled) {
        total += kills;
    }
    return total;
}


/**
 *  Draws an image or a shape with its top left corner at the given point.
 *
 *  @author: Aftermath
 */
void Draw_Tile_Art(Surface& surface, const Rect& rect, BSurface* image, const ShapeSet* shape, const Point2D& point)
{
    if (image != nullptr) {
        Rect image_rect(rect.X + point.X, rect.Y + point.Y, image->Get_Width(), image->Get_Height());
        SpriteCollection.Draw_Trans(image_rect, surface, *image);
    } else if (shape != nullptr) {
        Draw_Shape(surface, *SidebarDrawer, shape, 0, point, rect, SHAPE_WIN_REL);
    }
}
}


/**
 *  Returns whether the local player is a spectator.
 *
 *  @author: Aftermath
 */
bool SidebarObserverView::Is_Observer()
{
    return PlayerPtr != nullptr && !Debug_Map && Extension::Fetch(PlayerPtr)->IsObserver;
}


/**
 *  Resets the tile list for a new scenario.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Init_Clear()
{
    TopIndex = 0;
}


/**
 *  Drops the tile art. It is loaded again on the next draw.
 *
 *  This runs during a savegame load (Prep_For_Side), before the swizzle
 *  manager has restored the side extension pointers, so the side extensions
 *  must not be read here.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Init_For_House()
{
    Art.clear();
    IsArtLoaded = false;
}


/**
 *  Loads the tile art for every side.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Load_Art()
{
    IsArtLoaded = true;
    Art.clear();
    Art.resize(Sides.Count());

    for (int i = 0; i < Sides.Count(); i++) {
        const SideClassExtension* sideext = Extension::Fetch(Sides[i]);
        TileArt& art = Art[i];

        art.BackgroundImage = Fetch_Tile_Image(sideext->ObserverTileBackground);
        if (art.BackgroundImage == nullptr) {
            art.BackgroundShape = Fetch_Tile_Shape(sideext->ObserverTileBackground);
        }

        art.IconImage = Fetch_Tile_Image(sideext->ObserverTileIcon);
        if (art.IconImage == nullptr) {
            art.IconShape = Fetch_Tile_Shape(sideext->ObserverTileIcon);
        }
    }
}


/**
 *  Places the tiles over the cameo area of the active sidebar view. A tile
 *  covers two cameo rows and both cameo columns.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Set_Layout(SidebarViewType view_type, int rows_per_column)
{
    int row_pitch;

    if (view_type == SIDEBAR_TABBED) {
        const SidebarTabbedLayout& layout = UIControls->TabbedSidebarLayoutConfig;
        Origin = layout.StripPosition;
        TileSize.X = 2 * layout.CameoSize.X + layout.ColumnSpacing;
        TileSize.Y = 2 * layout.CameoSize.Y + layout.RowSpacing;
        row_pitch = layout.CameoSize.Y + layout.RowSpacing;
    } else {
        const SidebarClassicLayout& layout = UIControls->ClassicSidebarLayoutConfig;
        Origin = layout.LeftStripPosition;
        TileSize.X = layout.RightStripPosition.X + layout.CameoSize.X - layout.LeftStripPosition.X;
        TileSize.Y = 2 * layout.CameoSize.Y + layout.RowSpacing;
        row_pitch = layout.CameoSize.Y + layout.RowSpacing;
    }

    TilePitch = std::max(1, 2 * row_pitch);
    VisibleTiles = std::max(1, rows_per_column / 2);
}


/**
 *  Draws the visible tiles.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Draw(Surface& surface, const Rect& rect)
{
    if (!IsArtLoaded) {
        Load_Art();
    }

    std::vector<HouseClass*> players;
    Collect_Players(players);

    TopIndex = std::clamp(TopIndex, 0, Max_Top_Index(static_cast<int>(players.size())));

    for (int i = 0; i < VisibleTiles; i++) {
        const int index = TopIndex + i;
        if (index >= static_cast<int>(players.size())) {
            break;
        }

        const Point2D point(Origin.X, SidebarRect.Y + Origin.Y + i * TilePitch);
        Draw_Tile(surface, rect, *players[index], point);
    }
}


/**
 *  Scrolls the tile list by one tile.
 *
 *  @author: Aftermath
 */
bool SidebarObserverView::Scroll(bool up)
{
    std::vector<HouseClass*> players;
    Collect_Players(players);

    const int top = std::clamp(TopIndex + (up ? -1 : 1), 0, Max_Top_Index(static_cast<int>(players.size())));
    if (top == TopIndex) {
        return false;
    }

    TopIndex = top;
    return true;
}


/**
 *  Scrolls the tile list by one page.
 *
 *  @author: Aftermath
 */
bool SidebarObserverView::Scroll_Page(bool up)
{
    std::vector<HouseClass*> players;
    Collect_Players(players);

    const int top = std::clamp(TopIndex + (up ? -VisibleTiles : VisibleTiles), 0, Max_Top_Index(static_cast<int>(players.size())));
    if (top == TopIndex) {
        return false;
    }

    TopIndex = top;
    return true;
}


/**
 *  Collects the houses that get a tile: every playing house, but not passive
 *  houses and not observers.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Collect_Players(std::vector<HouseClass*>& players) const
{
    players.clear();

    for (int i = 0; i < Houses.Count(); i++) {
        HouseClass* house = Houses[i];
        if (house == nullptr || house->Class->IsMultiplayPassive || Extension::Fetch(house)->IsObserver) {
            continue;
        }
        players.push_back(house);
    }
}


/**
 *  Draws one player tile.
 *
 *  @author: Aftermath
 */
void SidebarObserverView::Draw_Tile(Surface& surface, const Rect& rect, const HouseClass& house, const Point2D& point) const
{
    const Rect tile_rect(rect.X + point.X, rect.Y + point.Y, TileSize.X, TileSize.Y);
    const SideType side = house.Class->Side;
    const TileArt* art = side >= 0 && side < static_cast<int>(Art.size()) ? &Art[side] : nullptr;

    /**
     *  Background. Without art, a darkened tile.
     */
    if (art != nullptr && (art->BackgroundImage != nullptr || art->BackgroundShape != nullptr)) {
        Draw_Tile_Art(surface, rect, art->BackgroundImage, art->BackgroundShape, point);
    } else {
        surface.Fill_Rect_Trans(tile_rect, RGBClass(0, 0, 0), 60);
    }

    /**
     *  Frame in the player color, drawn over the art so tiles of the same
     *  side can still be told apart.
     */
    surface.Draw_Rect(tile_rect, DSurface::Build_Hicolor_Pixel(house.RemapColorRGB.Get_Red(), house.RemapColorRGB.Get_Green(), house.RemapColorRGB.Get_Blue()));

    /**
     *  Side icon in the bottom right corner.
     */
    if (art != nullptr) {
        int width = 0;
        int height = 0;
        if (art->IconImage != nullptr) {
            width = art->IconImage->Get_Width();
            height = art->IconImage->Get_Height();
        } else if (art->IconShape != nullptr) {
            width = art->IconShape->Get_Width();
            height = art->IconShape->Get_Height();
        }

        if (width > 0 && height > 0) {
            const Point2D icon_point(point.X + TileSize.X - width - TILE_ICON_MARGIN, point.Y + TileSize.Y - height - TILE_ICON_MARGIN);
            Draw_Tile_Art(surface, rect, art->IconImage, art->IconShape, icon_point);
        }
    }

    /**
     *  Text lines: name, money, power, kills.
     */
    ColorScheme* text_scheme = Fetch_Tile_Scheme("LightGrey", ColorSchemes[COLORSCHEME_FIRST]);
    ColorScheme* name_scheme = house.Scheme >= 0 && house.Scheme < ColorSchemes.Count() ? ColorSchemes[house.Scheme] : text_scheme;
    ColorScheme* power_scheme = house.Power >= house.Drain
        ? Fetch_Tile_Scheme("Green", text_scheme)
        : Fetch_Tile_Scheme("Red", text_scheme);

    const TextPrintType flags = TextPrintType(TPF_8POINT | TPF_FULLSHADOW);
    Point2D text_point(point.X + TILE_TEXT_X, point.Y + TILE_TEXT_Y);

    Conquer_Clip_Text_Print(house.IniName.c_str(), surface, rect, text_point, name_scheme, COLOR_TBLACK, flags, TileSize.X - 2 * TILE_TEXT_X);
    text_point.Y += TILE_LINE_HEIGHT;

    char buffer[64];

    std::snprintf(buffer, sizeof(buffer), "$%ld", static_cast<long>(const_cast<HouseClass&>(house).Available_Money()));
    Fancy_Text_Print(buffer, surface, rect, text_point, text_scheme, COLOR_TBLACK, flags);
    text_point.Y += TILE_LINE_HEIGHT;

    std::snprintf(buffer, sizeof(buffer), "Power %d/%d", house.Power, house.Drain);
    Fancy_Text_Print(buffer, surface, rect, text_point, power_scheme, COLOR_TBLACK, flags);
    text_point.Y += TILE_LINE_HEIGHT;

    std::snprintf(buffer, sizeof(buffer), "Kills %u", Count_Units_Killed(house));
    Fancy_Text_Print(buffer, surface, rect, text_point, text_scheme, COLOR_TBLACK, flags);

    /**
     *  Darken the tile of a defeated player.
     */
    if (house.IsDefeated) {
        surface.Fill_Rect_Trans(tile_rect, RGBClass(0, 0, 0), 50);
    }
}


/**
 *  Returns the last valid top index for the given number of tiles.
 *
 *  @author: Aftermath
 */
int SidebarObserverView::Max_Top_Index(int count) const
{
    return std::max(0, count - VisibleTiles);
}
