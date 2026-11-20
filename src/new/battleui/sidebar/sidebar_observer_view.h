/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Spectator info tiles shown in the sidebar in place of the cameo
 *          strips while the local player is an observer.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#pragma once

#include "point.h"
#include "rect.h"
#include "uicontrol.h"

#include <vector>

class BSurface;
class HouseClass;
class ShapeSet;
class Surface;


/**
 *  Aftermath: One tile per playing house, two cameo rows high and both cameo
 *  columns wide. A tile shows the player name, available money, power output
 *  and drain, and the number of units killed, with the side icon in the
 *  bottom right corner.
 */
class SidebarObserverView
{
public:
    static bool Is_Observer();

    void Init_Clear();
    void Init_For_House();
    void Set_Layout(SidebarViewType view_type, int rows_per_column);
    void Draw(Surface& surface, const Rect& rect);
    bool Scroll(bool up);
    bool Scroll_Page(bool up);

private:
    /**
     *  Per-side tile art. An image (PNG, BMP or PCX) is preferred over a SHP.
     */
    struct TileArt
    {
        BSurface* BackgroundImage = nullptr;
        const ShapeSet* BackgroundShape = nullptr;
        BSurface* IconImage = nullptr;
        const ShapeSet* IconShape = nullptr;
    };

    void Collect_Players(std::vector<HouseClass*>& players) const;
    void Draw_Tile(Surface& surface, const Rect& rect, const HouseClass& house, const Point2D& point) const;
    int Max_Top_Index(int count) const;

    std::vector<TileArt> Art;
    TPoint2D<int> Origin = { 24, 26 };
    TPoint2D<int> TileSize = { 131, 102 };
    int TilePitch = 102;
    int VisibleTiles = 1;
    int TopIndex = 0;
};
