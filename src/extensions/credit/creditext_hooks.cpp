/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended CreditClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "colorscheme.h"
#include "credits.h"
#include "dsurface.h"
#include "extension.h"
#include "extension_globals.h"
#include "hooker.h"
#include "house.h"
#include "houseext.h"
#include "housetype.h"
#include "mouse.h"
#include "rgb.h"
#include "scenario.h"
#include "scenarioext.h"
#include "session.h"
#include "sideext.h"
#include "syringe.h"
#include "textprint.h"
#include "tibsun_globals.h"

#include <algorithm>
#include <cstdio>
#include <windows.h>


namespace
{
/**
 *  Aftermath: The longest single step the observer clock advances by in a
 *  singleplayer game. Skirmish can be paused, which stops the game frames;
 *  the first frame after the pause must not add the paused time. Multiplayer
 *  cannot be paused, so every real millisecond (lag included) counts there.
 */
constexpr unsigned long long OBSERVER_CLOCK_MAX_STEP = 1000;


/**
 *  Returns whether the local player is a spectator.
 *
 *  @author: Aftermath
 */
bool Is_Observer_Player()
{
    return PlayerPtr != nullptr && ScenExtension != nullptr && Extension::Fetch(PlayerPtr)->IsObserver;
}


/**
 *  Advances the observer clock by the real time since the last game frame.
 *
 *  @author: Aftermath
 */
void Advance_Observer_Clock()
{
    const unsigned long long now = GetTickCount64();
    const unsigned long long last = ScenExtension->ObserverClockLastTick;

    if (last != 0 && now > last) {
        unsigned long long step = now - last;
        if (Session.Singleplayer_Game()) {
            step = std::min(step, OBSERVER_CLOCK_MAX_STEP);
        }
        ScenExtension->ObserverClockElapsed += step;
    }

    ScenExtension->ObserverClockLastTick = now;
}


/**
 *  Returns the whole seconds shown by the observer clock.
 *
 *  @author: Aftermath
 */
long Observer_Clock_Seconds()
{
    return static_cast<long>(ScenExtension->ObserverClockElapsed / 1000);
}
}


/**
 *  Aftermath: A spectator has no money to count, so the credits counter keeps
 *  the real elapsed match time instead. This runs once per game frame, after
 *  the frame check, and flags the tab for redraw when the shown second changes.
 *
 *  @author: Aftermath
 */
DEFINE_HOOK(0x00471612, _CreditClass_AI_Observer_Clock_Patch, 5)
{
    GET(CreditClass *, this_ptr, ESI);

    if (!Is_Observer_Player()) {
        return 0;
    }

    Advance_Observer_Clock();

    const long seconds = Observer_Clock_Seconds();
    if (this_ptr->Current != seconds) {
        this_ptr->Credits = seconds;
        this_ptr->Current = seconds;
        this_ptr->IsAudible = false;
        this_ptr->IsToRedraw = true;
        Map.Redraw_Tab();
    }

    return 0x0047170C;
}


/**
 *  Aftermath: Draws the observer clock in place of the credits value, as MM:SS
 *  and as HH:MM:SS once the match runs for an hour. Resumes at the mission
 *  timer handling, which expects ECX to hold the scenario.
 *
 *  @author: Aftermath
 */
DEFINE_HOOK(0x004714D1, _CreditClass_Graphic_Logic_Observer_Clock_Patch, 9)
{
    GET(int, center_x, ESI);

    if (!Is_Observer_Player()) {
        return 0;
    }

    const long total = Observer_Clock_Seconds();
    const long hours = total / 3600;
    const long minutes = (total / 60) % 60;
    const long seconds = total % 60;

    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%02ld:%02ld:%02ld", hours, minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02ld:%02ld", minutes, seconds);
    }

    ColorSchemeType colorschemetype = Extension::Fetch(Sides[PlayerPtr->Class->Side])->UIColor;
    ColorScheme* colorscheme = ColorSchemes[colorschemetype];

    Fancy_Text_Print(buffer, *SidebarSurface, SidebarSurface->Get_Rect(), Point2D(center_x, 0), colorscheme, COLOR_TBLACK,
        TextPrintType(TPF_USE_GRAD_PAL | TPF_CENTER | TPF_METAL12));

    R->ECX(Scen);
    return 0x00471526;
}


/**
 *  Modifies the color of the "Options" text based on the player's side.
 *
 *  @author: Rampastring, ZivDero
 */
DEFINE_HOOK(0x0060E5AE, _TabClass_Draw_It_Faction_Specific_Options_Button_Color_Scheme_Patch, 6)
{
    ColorSchemeType colorschemetype = Extension::Fetch(Sides[PlayerPtr->Class->Side])->UIColor;
    ColorScheme* colorscheme = ColorSchemes[colorschemetype];
    R->EDX(colorscheme);

    return 0;
}


/**
 *  Modifies the color of the credits display based on the player's side.
 *
 *  @author: Rampastring, ZivDero
 */
DEFINE_HOOK(0x004714E8, _CreditClass_Graphic_Logic_Faction_Specific_Color_Scheme_Patch, 8)
{
    ColorSchemeType colorschemetype = Extension::Fetch(Sides[PlayerPtr->Class->Side])->UIColor;
    ColorScheme* colorscheme = ColorSchemes[colorschemetype];
    R->EAX(colorscheme);

    return 0;
}


/**
 *  Draws the tooltip rectangle using a color based on the player's side.
 *
 *  @author: Rampastring, ZivDero
 */
void Draw_Tooltip_Rectangle(Surface* surface, Rect& drawrect)
{
    surface->Fill_Rect(drawrect, 0);

    const ColorSchemeType colorschemetype = Extension::Fetch(Sides[PlayerPtr->Class->Side])->ToolTipColor;
    const ColorScheme* colorscheme = ColorSchemes[colorschemetype];

    RGBClass rgb = colorscheme->HSV.operator RGBClass();
    surface->Draw_Rect(drawrect, DSurface::Build_Hicolor_Pixel(rgb));
}


/**
 *  Patch to replace the call to draw the tooptip rectangle.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x0044E682, _CCToolTip_Draw_Faction_Specific_Color_Scheme_Rect_Patch, 0)
{
    GET(Surface*, surface, ESI);
    GET(Rect*, drawrect, EAX);

    Draw_Tooltip_Rectangle(surface, *drawrect);
    
    return 0x0044E6D4;
}


/**
 *  Modifies the color of the tooltip text color based on the player's side.
 *
 *  @author: Rampastring, ZivDero
 */
DEFINE_HOOK(0x0044E6F3, _CCToolTip_Draw_Faction_Specific_Color_Scheme_Text_Patch, 0)
{
    ColorSchemeType colorschemetype = Extension::Fetch(Sides[PlayerPtr->Class->Side])->ToolTipColor;
    ColorScheme* colorscheme = ColorSchemes[colorschemetype];
    R->EAX(colorscheme);

    return 0x0044E6F8;
}


/**
 *  Main function for patching the hooks.
 */
void CreditClassExtension_Hooks()
{
    
}
