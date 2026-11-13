/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended MapClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#pragma once

#include "tibsun_defines.h"

class ObjectClass;

struct MapObserverSight
{
    Cell Center;
    Cell HeightOffset;
    int Level;
    int Range;

    bool Contains(const Cell& target) const;
};

void Map_Select_These(void (*select_callback)(ObjectClass*));
MapObserverSight Map_Observer_Sight(const Coord& origin, int range);
bool Map_Defense_Line_Clear(const Cell& from, const Cell& to);
void MapClassExtension_Hooks();
