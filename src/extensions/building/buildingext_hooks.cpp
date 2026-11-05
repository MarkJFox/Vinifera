/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended BuildingClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "buildingext_hooks.h"

#include "aircraft.h"
#include "aircraftext.h"
#include "aircrafttype.h"
#include "aircrafttypeext.h"
#include "anim.h"
#include "animext.h"
#include "asserthandler.h"
#include "audio_vox.h"
#include "bsurface.h"
#include "building.h"
#include "buildingext.h"
#include "buildingext_init.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "bullettype.h"
#include "cell.h"
#include "convert.h"
#include "debughandler.h"
#include "drawshape.h"
#include "event.h"
#include "extension.h"
#include "factory.h"
#include "fixedwing.h"
#include "hooker.h"
#include "house.h"
#include "houseext.h"
#include "housetype.h"
#include "infantry.h"
#include "infantrytype.h"
#include "jumpjetlocomotion.h"
#include "newjumpjetlocomotion.h"
#include "map.h"
#include "mouse.h"
#include "rules.h"
#include "rulesext.h"
#include "session.h"
#include "sideext.h"
#include "spritecollection.h"
#include "super.h"
#include "supertypeext.h"
#include "syringe.h"
#include "tactical.h"
#include "technotype.h"
#include "technotypeext.h"
#include "tibsun_globals.h"
#include "unit.h"
#include "unitext.h"
#include "unittype.h"
#include "unittypeext.h"
#include "vinifera_globals.h"
#include "vinifera_saveload.h"
#include "voc.h"
#include "vox.h"
#include "weapontype.h"
#include "wave.h"

#include <algorithm>

// Occupation and native Unlimbo have succeeded before the gate-update block.
DEFINE_HOOK(0x0042A8F7, _Building_Unlimbo_Refresh_Wall_Connections, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ESI);
    BuildingTypeClassExtension::Refresh_Wall_Connections(building, building->Get_Cell());
    return 0;
}

// EDI and the saved origin survive Techno::Limbo; replay mov bl,al / mov al,[esp+13h].
DEFINE_HOOK(0x0042DF54, _Building_Limbo_Refresh_Wall_Connections, 6)
{
    if (Vinifera_PerformingLoad || !(R->EAX() & 0xFF)) return 0;
    GET(BuildingClass*, building, EDI);
    GET_STACK(Cell, origin, 0x24);
    BuildingTypeClassExtension::Refresh_Wall_Connections(building, origin);
    return 0;
}

// D5 geometry applies only to explicitly configured land WeaponsFactory types.
// Clear_Bib also fixes released Hover blockers at native exits. Inactive hooks
// replay their complete original instructions, including native facing.
DEFINE_HOOK(0x00434580, _Building_Exit_Coord_Unit_Factory, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ECX);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    Cell exit;
    Coord door;
    Coord spawn;
    if (!Extension::Fetch(building)->Unit_Factory_Exit_Geometry(exit, door, spawn)) return 0;
    GET_STACK(Coord*, result, 0x4);
    *result = spawn;
    R->EAX(result);
    return 0x004345CC; // Bare ret 4; the entry prologue has not executed.
}

DEFINE_HOOK(0x0042CB67, _Building_Exit_Object_Unit_Factory_Preflight, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ESI);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    GET(TechnoClass*, product, EDI);
    if (!BuildingClassExtension::Is_Supported_Unit_Factory_Product(product) ||
        !Extension::Fetch(building)->Unit_Factory_Spawn_Clear(static_cast<const UnitClass*>(product))) {
        DEBUG_WARNING("[UNITFACTORY] type={} cannot output this product at the configured spawn; native failure returned.\n",
            building->Class->Name());
        // Before ++ScenarioInit. The native zero-result epilogue pops the same
        // saved registers and locals without decrementing ScenarioInit.
        return 0x0042D966;
    }
    return 0;
}

DEFINE_HOOK(0x0042FF70, _Building_Clear_Weapons_Factory_Bib_Custom_Exit, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ECX);
    const auto ext = Extension::Fetch(building);
    if (!ext->Has_Unit_Factory_Exit_Override() && !ext->Has_Unit_Factory_Hover_Exit_Blocker()) return 0;
    R->EAX(ext->Clear_Unit_Factory_Exit() ? 1 : 0);
    return 0x004300C3; // Bare ret; no Clear_Bib prologue or saved registers.
}

DEFINE_HOOK(0x00433568, _Building_Mission_Unload_Unit_Factory_Exit, 7)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, EBP);
    const auto type = Extension::Fetch(building->Class);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    const Cell cell = type->CustomUnitFactoryExitCell;
    R->ESI(static_cast<DWORD>(static_cast<unsigned short>(cell.X)) |
        (static_cast<DWORD>(static_cast<unsigned short>(cell.Y)) << 16));
    R->EAX(R->ESP() + 0x24);
    return 0x0043356F; // Replaces MOV ESI,[EAX+20] / LEA EAX,[ESP+24].
}

DEFINE_HOOK(0x00433670, _Building_Mission_Unload_Unit_Factory_Dispatch, 5)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, EBP);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    const auto peer = building->Radio;
    if (peer && peer->RTTI == RTTI_UNIT &&
        !Extension::Fetch(building)->Unit_Factory_Corridor_Clear(static_cast<const UnitClass*>(peer))) {
        building->Status = 1; // Existing CLEAR_BIB; recheck before acquiring COM references.
        return 0x004339B3; // Native delay/epilogue, no extra timer or ownership.
    }
    return 0; // MOV EBX,[EBP+78] / TEST EBX,EBX and the whole native dispatch.
}

DEFINE_HOOK(0x0043392D, _Building_Mission_Unload_Unit_Factory_Track_Result, 7)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, EBP);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    building->Status = Extension::Fetch(building)->Unit_Factory_Track_Started() ? 3 : 1;
    // Keep EAX (the native IPersistStream reference) and its Release path intact.
    // A failed void Force_Track must not strand the tether in LEAVE.
    return 0x00433934;
}

DEFINE_HOOK(0x004338AD, _Building_Mission_Unload_Unit_Factory_Native_Destination, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, EBP);
    const auto type = Extension::Fetch(building->Class);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    R->CX(static_cast<WORD>(R->CX() + type->CustomUnitFactoryExitCell.X - 3));
    R->DX(static_cast<WORD>(R->DX() + type->CustomUnitFactoryExitCell.Y - 1));
    return 0; // Replay ADD CX,3 / INC DX before the native Assign_Destination call.
}

DEFINE_HOOK(0x006581C4, _Unit_Assign_Destination_Unit_Factory_Exit, 8)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, EDI); // Native RTTI/WeaponsFactory tests already passed.
    const auto type = Extension::Fetch(building->Class);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    R->CX(static_cast<WORD>(R->CX() + type->CustomUnitFactoryExitCell.X - 3));
    R->DX(static_cast<WORD>(R->DX() + type->CustomUnitFactoryExitCell.Y - 1));
    return 0; // Replay ADD ECX,3 / MOV [ESP+2C],CX, then native INC EDX.
}

DEFINE_HOOK(0x00659526, _Unit_Ready_To_Commence_Unit_Factory_Spawn, 8)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(UnitClass*, unit, ESI); // Still this, before native ESI is reused for anchor Y.
    const auto building = Map[unit->Get_Cell()].Cell_Building();
    if (!building) return 0;
    const auto type = Extension::Fetch(building->Class);
    if (!Extension::Fetch(building)->Has_Unit_Factory_Exit_Override()) return 0;
    GET_STACK(DWORD, x, 0x10);
    GET_STACK(DWORD, y, 0x12);
    R->EDI((x & 0xFFFF0000u) | static_cast<WORD>(x + type->CustomUnitFactoryExitCell.X - 3));
    R->ESI((y & 0xFFFF0000u) | static_cast<WORD>(y + type->CustomUnitFactoryExitCell.Y - 1));
    return 0x0065952E; // Keep ECX (Unit Coord*) and the native (0,1) comparison.
}

namespace
{
    bool Custom_Aircraft_Dock_Coord(const BuildingClass* building, Coord& result)
    {
        const auto ext = Extension::Fetch(building->Class);
        if (ext == nullptr || ext->Is_Aircraft_MultiDock() || !ext->Is_Custom_Foundation_Aircraft_Helipad_Eligible() ||
            !ext->CustomAircraftDockCellSet) return false;
        const Cell cell = building->Get_Cell() + ext->CustomAircraftDockCell;
        result = cell.As_Coord(building->Get_Coord().Z);
        return true;
    }

    Cell Find_Custom_Foundation_Infantry_Exit(const BuildingClass* building,
        const TechnoClass* product, const BuildingTypeClassExtension& type_ext)
    {
        const Cell origin = building->Get_Cell();
        Cell exit = CELL_NONE;

        const auto try_exit = [&](int x, int y, const char* preference, bool custom_exit = false) {
            const Cell candidate(origin.X + x, origin.Y + y);
            // TSpp declares Can_Enter_Cell non-const, unlike the product
            // parameter of native Find_Exit_Cell. Keep the exact native call.
            const bool usable = Map.In_Radar(candidate) &&
                const_cast<TechnoClass*>(product)->Can_Enter_Cell(
                    &Map[candidate], FACING_NONE, -1, nullptr, true) == MOVE_OK;

            // Only the explicit/preferred attempts and the selected perimeter cell:
            // no per-frame hooks and no log for every rejected perimeter cell.
            if (Vinifera_DeveloperMode && custom_exit) {
                DEBUG_INFO("[CFINF] type={} event=CUSTOM_EXIT relative=({}, {}) absolute=({}, {}) usable={}\n",
                    building->Class->Name(), x, y, candidate.X, candidate.Y, usable);
            } else if (Vinifera_DeveloperMode && (preference != nullptr || usable)) {
                DEBUG_INFO("[CFINF] type={} event={} priority={} exit=({}, {}) usable={}\n",
                    building->Class->Name(), preference != nullptr ? "PREFERRED" : "SELECTED",
                    preference != nullptr ? preference : "PERIMETER", candidate.X, candidate.Y, usable);
            }
            if (usable) exit = candidate;
            return usable;
        };

        if (type_ext.CustomInfantryExitCellSet &&
            try_exit(type_ext.CustomInfantryExitCell.X, type_ext.CustomInfantryExitCell.Y, nullptr, true)) return exit;

        if (building->Class->IsGDIBarracks && try_exit(1, 2, "GDI")) return exit;
        if (building->Class->IsNODBarracks && try_exit(2, 2, "NOD")) return exit;

        // Preserve the native rectangle-fallback order, including repeated
        // corners. Generate candidates directly: no exit buffer or RNG.
        for (int x = -1; x <= type_ext.FoundationWidth; ++x) {
            if (try_exit(x, -1, nullptr) || try_exit(x, type_ext.FoundationHeight, nullptr)) return exit;
        }
        for (int y = -1; y <= type_ext.FoundationHeight; ++y) {
            if (try_exit(-1, y, nullptr) || try_exit(type_ext.FoundationWidth, y, nullptr)) return exit;
        }

        if (Vinifera_DeveloperMode) {
            DEBUG_INFO("[CFINF] type={} event=NO_EXIT anchor=({}, {}) width={} height={}\n",
                building->Class->Name(), origin.X, origin.Y, type_ext.FoundationWidth, type_ext.FoundationHeight);
        }
        return CELL_NONE;
    }
}

/**
 *  AF-0005D1: native __thiscall with hidden Cell* at ESP+4 and
 *  const TechnoClass* at ESP+8. The six displaced bytes are
 *  sub esp,18h / push ebx / mov ebx,ecx. No prologue ran on the custom path.
 */
DEFINE_HOOK(0x00434140, _BuildingClass_Find_Exit_Cell_Custom_Foundation_Infantry, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ECX);
    const auto ext = Extension::Fetch(building->Class);
    if (ext == nullptr) return 0;
    if (ext->Is_Custom_Foundation_Unit_Repair_Eligible()) {
        GET_STACK(Cell*, result, 0x4);
        GET_STACK(const TechnoClass*, visitor, 0x8);
        *result = Extension::Fetch(building)->Custom_Unit_Repair_Exit(visitor);
        R->EAX(result);
        return 0x004342EB; // bare ret 8; no native prologue has run.
    }
    if (!ext->Is_Custom_Foundation_Infantry_Factory_Eligible()) return 0;

    GET_STACK(Cell*, result, 0x4);
    GET_STACK(const TechnoClass*, product, 0x8);
    *result = Find_Custom_Foundation_Infantry_Exit(building, product, *ext);
    R->EAX(result);
    return 0x004342EB; // Only ret 8; do not execute the native frame-unwinding epilogue.
}

/**
 *  AF-0005D2: before either native Barracks ExitCoord branch. ESI=building,
 *  [ESP+10h]=selected Cell, [ESP+14h]=anchor Cell, [ESP+38h]=spawn Coord.
 *  The displaced seven-byte instruction is cmp byte ptr [edx+82Bh],0.
 */
DEFINE_HOOK(0x0042D0A4, _BuildingClass_Exit_Object_Custom_Infantry_ExitCoord, 7)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ESI);
    const auto ext = Extension::Fetch(building->Class);
    if (ext == nullptr || !ext->Is_Custom_Foundation_Infantry_Factory_Eligible() ||
        !ext->CustomInfantryExitCellSet) return 0;

    GET_STACK(Cell, exit, 0x10);
    GET_STACK(Cell, origin, 0x14);
    if (exit != origin + ext->CustomInfantryExitCell) return 0;

    static_assert(sizeof(Coord) == 12, "TS infantry spawn Coord ABI changed");
    REF_STACK(Coord, spawn, 0x38);
    const Coord base = spawn;
    const Coord& offset = building->Class->ExitCoordinate;
    spawn += offset;
    R->EBP(spawn.X); // Keep the native spawn-X register consistent; EBX retains facing.
    if (Vinifera_DeveloperMode) {
        DEBUG_INFO("[CFINF] type={} event=CUSTOM_EXIT_OFFSET exitCell=({}, {}) base=({}, {}, {}) ExitCoord=({}, {}, {}) result=({}, {}, {})\n",
            building->Class->Name(), exit.X, exit.Y, base.X, base.Y, base.Z,
            offset.X, offset.Y, offset.Z, spawn.X, spawn.Y, spawn.Z);
    }
    // Skip BOTH native GDI/Nod offset branches, also when the custom cell
    // equals their preference. Continue with the unchanged Unlimbo path.
    return 0x0042D14F;
}

/**
 *  D3 __thiscall Coord return: ECX=this, hidden result at original ESP+4.
 *  Six bytes: sub esp,10h / push esi / mov esi,ecx. No prologue on custom path.
 */
DEFINE_HOOK(0x0042F2C0, _BuildingClass_Docking_Coord_Custom_Aircraft_Pad, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ECX);
    Coord coord;
    if (!Custom_Aircraft_Dock_Coord(building, coord)) return 0;
    GET_STACK(Coord*, result, 0x4);
    *result = coord;
    R->EAX(result);
    // Docking_Coord is a hot query. Trace only the proven free-pad production call.
    GET_STACK(uintptr_t, caller, 0x0);
    if (Vinifera_DeveloperMode && caller == 0x0042C97D) {
        const Cell anchor = building->Get_Cell();
        const Cell absolute = coord.As_Cell();
        DEBUG_INFO("[CFPAD] type={} event=DOCK_COORD source=PRODUCTION_DOCK anchor=({}, {}) relative=({}, {}) absolute=({}, {}) coord=({}, {}, {}) native=FREE_PAD_UNLIMBO_ATTEMPT\n",
            building->Class->Name(), anchor.X, anchor.Y, absolute.X - anchor.X, absolute.Y - anchor.Y,
            absolute.X, absolute.Y, coord.X, coord.Y, coord.Z);
    }
    return 0x0042F3A3; // ret 4 only; the native frame was never created.
}

/**
 *  Native Mission_Enter's landing correction bypasses Destination_Coord.
 *  After its Center_Coord call, replace only the returned local for D3 pads.
 *  ESI=aircraft, EAX=Coord* [native ESP+18h]. Replay six bytes at 0x0040D515:
 *  mov edi,[eax] / mov edx,[esi] / mov ecx,esi. Facing/clamp/Z logic is native.
 */
DEFINE_HOOK(0x0040D515, _AircraftClass_Mission_Enter_Custom_Pad_Landing_Point, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(AircraftClass*, aircraft, ESI);
    const auto target = aircraft->NavCom;
    if (target != nullptr && target->Fetch_RTTI() == RTTI_BUILDING) {
        Coord coord;
        const auto building = static_cast<BuildingClass*>(target);
        const auto ext = Extension::Fetch(building);
        if ((ext->Is_Aircraft_MultiDock() && ext->Aircraft_Dock_Coord(aircraft, coord)) ||
            (!ext->Is_Aircraft_MultiDock() && Custom_Aircraft_Dock_Coord(building, coord))) {
            GET(Coord*, result, EAX);
            *result = coord;
        }
    }
    return 0; // Original register loads and +/-5-lepton adjustment remain unchanged.
}

DEFINE_HOOK(0x00672B68, _WaveClass_Custom_Foundation_Distance, 8)
{
    GET(WaveClass*, wave, ESI);
    const int raw_distance = R->EAX<int>();
    if (wave->Type == WAVE_SONIC && wave->xTarget != nullptr && wave->xTarget->RTTI == RTTI_BUILDING) {
        const auto target = static_cast<BuildingClass*>(wave->xTarget);
        const auto ext = target->Class != nullptr ? Extension::Fetch(target->Class) : nullptr;
        if (ext != nullptr && ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid &&
            target->IsActive && target->IsDown && !target->IsInLimbo && target->Strength > 0) {
            // Match the native building extent used by TechnoTypeClass::In_Range.
            const int building_allowance = (ext->FoundationWidth + ext->FoundationHeight) * (CELL_LEPTON_W / 4);
            const int adjusted_distance = std::max(0, raw_distance - building_allowance);
            R->EAX(adjusted_distance);
        }
    }
    return 0; // Replay mov [esp+10h],eax and fild using the selected distance; keep the native comparison.
}

// D3c1: every local-coordinate adapter has an explicit Aircraft requester.
namespace
{
    bool MultiDock_Destination(FootClass* foot, const AbstractClass* target, Coord& coord)
    {
        if (Vinifera_PerformingLoad || foot == nullptr || foot->RTTI != RTTI_AIRCRAFT ||
            target == nullptr || target->Fetch_RTTI() != RTTI_BUILDING) return false;
        const auto ext = Extension::Fetch(static_cast<const BuildingClass*>(target));
        return ext->Is_Aircraft_MultiDock() &&
            ext->Aircraft_Dock_Coord(static_cast<AircraftClass*>(foot), coord);
    }
}

// Original 8B 46 78 / 3B C3 (5). ESI=pad, EDI=product, EBX=0.
DEFINE_HOOK(0x0042C6A1, _Building_Exit_Aircraft_MultiDock_Capacity, 5)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ESI);
    const auto ext = Extension::Fetch(building);
    if (!ext->Is_Aircraft_MultiDock()) return 0;
    GET(AircraftClass*, aircraft, EDI);
    // Only supported FixedWing may share the normal MultiDock production path.
    if ((Extension::Fetch(aircraft->Class)->IsFixedWing && !FixedWing::Enabled(aircraft)) ||
        aircraft->Class->IsCarryall) return 0x0042C6B9;
    return ext->Can_Accept_Aircraft_Dock(aircraft, true) ? 0x0042C8EA : 0x0042C6AC;
}

// Original FF 92 A8 00 00 00 (6). Stack[0]=hidden Coord*, [4]=Pose_Dir
// for the NEXT Unlimbo call. Emulate Docking_Coord's ret 4, never consume Facing.
DEFINE_HOOK(0x0042C977, _Building_Exit_Aircraft_MultiDock_Coord, 6)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ESI);
    const auto ext = Extension::Fetch(building);
    if (!ext->Is_Aircraft_MultiDock()) return 0;
    GET(AircraftClass*, aircraft, EDI);
    GET_STACK(Coord*, result, 0x0);
    const int slot = ext->Reserve_Aircraft_Dock(aircraft, true);
    if (slot < 0 || !ext->Aircraft_Dock_Coord(aircraft, *result)) {
        ext->Release_Aircraft_Dock(aircraft, "PRODUCTION_RESERVE_FAILED");
        R->ESP(R->ESP() + 8); // Neither nested call ran; discard buffer AND Facing.
        return 0x0042D35D;
    }
    if (Vinifera_DeveloperMode) {
        DEBUG_INFO("[MDOCK] building={} event=PRODUCTION_SLOT slot={} aircraft={}\n", building->ID, slot, aircraft->ID);
    }
    R->EAX(result);
    R->ESP(R->ESP() + 4);
    return 0x0042C97D;
}

// Original 50 / 8B CF / FF 95 C8 00 00 00 (9). EAX=Coord*, EDI=product,
// ESI=pad; Stack[0]=Facing. Single-Dock replays the native virtual call.
DEFINE_HOOK(0x0042C97D, _Building_Exit_Aircraft_MultiDock_Unlimbo, 9)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ESI);
    const auto ext = Extension::Fetch(building);
    if (!ext->Is_Aircraft_MultiDock()) return 0;
    GET(AircraftClass*, aircraft, EDI);
    GET(Coord*, coord, EAX);
    GET_STACK(Dir256, facing, 0x0);
    bool success = aircraft->Unlimbo(*coord, facing);
    if (success && ext->Reserve_Aircraft_Dock(aircraft, true) < 0) {
        aircraft->Limbo();
        success = false;
    }
    if (!success) ext->Release_Aircraft_Dock(aircraft, "UNLIMBO_FAILED");
    R->EAX(success ? 1 : 0);
    R->ESP(R->ESP() + 4); // Native Unlimbo would consume Coord* plus saved Facing.
    return 0x0042C986; // Keep the original test AL / JZ and ScenarioInit unwind.
}

// Original 8B 30 / 8B 78 04 (5). EBP=Foot, EAX=returned Coord*.
DEFINE_HOOK(0x004A4AD9, _Foot_Destination_Aircraft_MultiDock, 5)
{
    GET(FootClass*, foot, EBP);
    GET(Coord*, result, EAX);
    MultiDock_Destination(foot, foot->NavCom, *result);
    Coord point;
    if (BuildingClassExtension::Custom_Unit_Repair_Nav_Coord(foot, point)) *result = point;
    return 0;
}

// D4b: only local Ground Repair consumers. No Center_Coord/Docking_Coord patch.
// Each custom path skips the complete native hidden-result call, including its
// argument push; fallbacks return 0 and replay the original instructions.

// 8D 44 24 2C / 50 (5). EBP=Unit, EBX=contact; native Cell equality,
// NavCom identity, IM_IN and Power_Off are still executed after the coordinate.
DEFINE_HOOK(0x00651356, _Unit_PCP_Custom_Repair_Point, 5)
{
    GET(UnitClass*, unit, EBP);
    GET(RadioClass*, peer, EBX);
    if (Vinifera_PerformingLoad || !peer || peer->RTTI != RTTI_BUILDING || peer->Radio != unit || unit->Radio != peer) return 0;
    Coord point;
    if (!Extension::Fetch(static_cast<BuildingClass*>(peer))->Custom_Unit_Repair_Coord(point, unit)) return 0;
    REF_STACK(Coord, result, 0x2C);
    result = point;
    R->EAX(&result);
    return 0x00651360;
}

// 8B 06 / 51 / 8B CE (5). ESI=Building, ECX=Coord* (ESP+28h).
DEFINE_HOOK(0x004302DF, _Building_Guard_Custom_Repair_Point, 5)
{
    GET(BuildingClass*, building, ESI);
    Coord point;
    if (!Extension::Fetch(building)->Custom_Unit_Repair_Contact_Coord(point)) return 0;
    GET(Coord*, result, ECX);
    *result = point;
    R->EAX(result);
    return 0x004302E7;
}

// 51 / 8B CD / FF 50 4C (6). EBP=Building, ECX=Coord* (ESP+3Ch).
DEFINE_HOOK(0x00432446, _Building_Repair_Initial_Custom_Point, 6)
{
    GET(BuildingClass*, building, EBP);
    Coord point;
    if (!Extension::Fetch(building)->Custom_Unit_Repair_Contact_Coord(point)) return 0;
    GET(Coord*, result, ECX);
    *result = point;
    R->EAX(result);
    return 0x0043244C;
}

// 8D 44 24 3C / 8B CD (6). EBP=Building. Keep native <150 and power policy.
DEFINE_HOOK(0x00431F26, _Building_Repair_Idle_Custom_Point, 6)
{
    GET(BuildingClass*, building, EBP);
    Coord point;
    if (!Extension::Fetch(building)->Custom_Unit_Repair_Contact_Coord(point)) return 0;
    REF_STACK(Coord, result, 0x3C);
    result = point;
    R->EAX(&result);
    return 0x00431F30;
}

// FF 52 4C / 8B 16 (5). Unlike the other sites, the hidden-result
// argument is ALREADY on the stack. Emulate the call's ret 4 AND mov edx,[esi].
DEFINE_HOOK(0x004A5280, _Foot_Can_Demolish_Custom_Repair_Point, 5)
{
    GET(BuildingClass*, building, ECX);
    GET(TechnoClass*, visitor, ESI);
    Coord point;
    if (building->Radio != visitor || !Extension::Fetch(building)->Custom_Unit_Repair_Contact_Coord(point)) return 0;
    GET_STACK(Coord*, result, 0x0);
    *result = point;
    R->EAX(result);
    R->ESP(R->ESP() + 4);
    R->EDX(*R->ESI<DWORD*>()); // Exact displaced native vtable load, not a visitor cast.
    return 0x004A5285;
}

// 8B 06 / 51 / 8B CE (5). ESI=Building, ECX=Coord* (ESP+90h).
DEFINE_HOOK(0x00431025, _Building_Sell_Custom_Repair_Point, 5)
{
    GET(BuildingClass*, building, ESI);
    Coord point;
    if (!Extension::Fetch(building)->Custom_Unit_Repair_Contact_Coord(point)) return 0;
    GET(Coord*, result, ECX);
    *result = point;
    R->EAX(result);
    return 0x0043102D;
}

// 8D 44 24 38 / 50 (5). ESI=Building, EBX=customer; only the dockpoint side
// of the native capture distance is changed, not ownership/run-away policy.
DEFINE_HOOK(0x0042F820, _Building_Capture_Custom_Repair_Point, 5)
{
    GET(BuildingClass*, building, ESI);
    Coord point;
    if (!Extension::Fetch(building)->Custom_Unit_Repair_Contact_Coord(point)) return 0;
    REF_STACK(Coord, result, 0x38);
    result = point;
    R->EAX(&result);
    return 0x0042F82D;
}

// 8D 44 24 24 / 50 (5). ESI=searcher, EDI=candidate, no reservation yet.
// Adapt the zone endpoint only; ranking at 637FF2 and its policies stay native.
DEFINE_HOOK(0x00637F30, _Techno_Find_Dock_Custom_Repair_Zone, 5)
{
    GET(TechnoClass*, visitor, ESI);
    GET(BuildingClass*, building, EDI);
    Coord point;
    if (!Extension::Fetch(building)->Custom_Unit_Repair_Coord(point, visitor)) return 0;
    REF_STACK(Coord, result, 0x24);
    result = point;
    R->EAX(&result);
    return 0x00637F3A;
}

// Original 8B 38 / 8B 48 04 (5). ESI=&Fly owner, Stack[20h]=Building*,
// EAX=returned Coord*. Do not replace Fly::Process or any FixedWing entry.
DEFINE_HOOK(0x0049BC8D, _Fly_Destination_Aircraft_MultiDock, 5)
{
    GET(FootClass**, owner, ESI);
    GET_STACK(BuildingClass*, building, 0x20);
    GET(Coord*, result, EAX);
    if ((*owner)->Radio == building && ((*owner)->NavCom == building || (*owner)->NavCom == nullptr)) {
        MultiDock_Destination(*owner, building, *result);
    }
    return 0;
}

// Original 8B 48 78 / 85 C9 (5). EAX=target Techno, ESI=Aircraft.
// A registered visitor is already mutually linked through the extension.
DEFINE_HOOK(0x0040E3AC, _Aircraft_AssignDestination_MultiDock_Contact, 5)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(TechnoClass*, target, EAX);
    GET(AircraftClass*, aircraft, ESI);
    if (target != nullptr && target->RTTI == RTTI_BUILDING) {
        const auto ext = Extension::Fetch(static_cast<BuildingClass*>(target));
        if (ext->Is_Aircraft_MultiDock() && aircraft->Radio == target &&
            ext->Find_Aircraft_Dock_Slot(aircraft) >= 0) return 0x0040E427;
    }
    return 0;
}

// Original 83 EC 54 / 53 / 55 (5). ECX=Building, Stack[0]=return address.
// The multi branch creates no native frame; EAX=ticks, jump to a bare ret.
DEFINE_HOOK(0x00431880, _Building_MissionRepair_Aircraft_MultiDock, 5)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, ECX);
    const auto ext = Extension::Fetch(building);
    if (!ext->Is_Aircraft_MultiDock()) return 0;
    R->EAX(ext->Aircraft_Dock_Service());
    return 0x004318DA;
}


/**
 *  A fake class for implementing new member functions which allow
 *  access to the "this" pointer of the intended class.
 *
 *  @note: This must not contain a constructor or destructor!
 *  @note: All functions must be prefixed with "_" to prevent accidental virtualization.
 */
static DECLARE_EXTENDING_CLASS_AND_PAIR(BuildingClass)
{
public:
    bool _Can_Have_Rally_Point();
    void _Update_Buildables();
    const InfantryTypeClass* _Crew_Type() const;
    int _How_Many_Survivors() const;
    int _Shape_Number() const;
    void _Detach_Anim(AnimClass* anim);
    void _Draw_It(Point2D const& xdrawpoint, Rect const& xcliprect);
    void _Detach_All(bool all);
    bool _Toggle_Primary();
    void _Assign_Rally_Point(const Cell& cell);
    ActionType _What_Action(ObjectClass const* object, bool disallow_force);
    ActionType _What_Action(const Cell& cell, bool check_fog, bool disallow_force) const;
    void _Factory_AI();
    SuperWeaponType _Fetch_Super_Weapon() const;
    SuperWeaponType _Fetch_Super_Weapon2() const;
    void _Swizzle_Light_Source();
    RadioMessageType _Receive_Message(RadioClass * from, RadioMessageType message, long& param);
    RadioMessageType _Aircraft_Dock_Transmit(RadioMessageType message, RadioClass* to);
    RadioMessageType _Aircraft_Dock_Transmit_Data(RadioMessageType message, long& param, RadioClass* to);
    MoveType _Can_Enter_Cell(CellClass const* cell, FacingType dir, int cell_height, CellClass const*, bool) const;
    void _Draw_Overlays(const Point2D & coord, const Rect & rect);
};


bool BuildingClassExt::_Can_Have_Rally_Point()
{
    RTTIType tobuild = this->Class->ToBuild;
    if (tobuild == RTTI_UNITTYPE || tobuild == RTTI_INFANTRYTYPE || tobuild == RTTI_AIRCRAFTTYPE)
        return true;

    /**
     *  #issue-966
     *
     *  Makes it possible to give rally points to Service Depots.
     *
     *  @author: Rampastring
     */
    if (this->Class->IsCanUnitRepair)
        return true;

    /**     
     *  Makes it possible to give rally points to Hospitals and Armories
     *
     *  @author: JoyfulShush
     */
    if (this->Class->IsArmory || this->Class->IsHospital) 
        return true;

    return false;
}


/**
 *  Makes the game check whether you can actually build the object before adding it to the sidebar,
 *  preventing grayed out cameos (except for build limited types)
 *
 *  This reimplements the entire BuildingClass::Update_Buildables() function
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Update_Buildables()
{
    if (House == PlayerPtr && !IsInLimbo && IsDiscoveredByPlayer && IsOn) {
        switch (Class->ToBuild) {
        case RTTI_AIRCRAFTTYPE:
            for (int i = 0; i < AircraftTypes.Count(); i++) {
                int can_build = PlayerPtr->Can_Build(AircraftTypes[i], false, true); // 0 = not allowed, 1 = allowed, -1 = build limit
                if (can_build && (can_build == -1 || AircraftTypes[i]->Who_Can_Build_Me(true, false, RuleExtension->IsRecheckPrerequisites, PlayerPtr) != nullptr)) {
                    Map.Add(RTTI_AIRCRAFTTYPE, i);
                }
            }
            break;

        case RTTI_BUILDINGTYPE:
            for (int i = 0; i < BuildingTypes.Count(); i++) {
                int can_build = PlayerPtr->Can_Build(BuildingTypes[i], false, true);
                if (can_build && (can_build == -1 || BuildingTypes[i]->Who_Can_Build_Me(true, false, RuleExtension->IsRecheckPrerequisites, PlayerPtr) != nullptr)) {
                    Map.Add(RTTI_BUILDINGTYPE, i);
                }
            }
            break;

        case RTTI_INFANTRYTYPE:
            for (int i = 0; i < InfantryTypes.Count(); i++) {
                int can_build = PlayerPtr->Can_Build(InfantryTypes[i], false, true);
                if (can_build && (can_build == -1 || InfantryTypes[i]->Who_Can_Build_Me(true, false, RuleExtension->IsRecheckPrerequisites, PlayerPtr) != nullptr)) {
                    Map.Add(RTTI_INFANTRYTYPE, i);
                }
            }
            break;

        case RTTI_UNITTYPE:
            for (int i = 0; i < UnitTypes.Count(); i++) {
                int can_build = PlayerPtr->Can_Build(UnitTypes[i], false, true);
                if (can_build && (can_build == -1 || UnitTypes[i]->Who_Can_Build_Me(true, false, RuleExtension->IsRecheckPrerequisites, PlayerPtr) != nullptr)) {
                    Map.Add(RTTI_UNITTYPE, i);
                }
            }
            break;

        default:
            break;
        }
    }
}


/**
 *  Fetches the kind of crew this object contains.
 *
 *  @author: ZivDero
 */
const InfantryTypeClass* BuildingClassExt::_Crew_Type() const
{
    /**
     *  Construction yards can sometimes have an engineer exit them.
     */
    const int engineer_chance = Extension::Fetch(Class)->EngineerChance;
    if (!IsCaptured && Percent_Chance(engineer_chance))
        return SideClassExtension::Get_Engineer(House);

    return TechnoClass::Crew_Type();
}


/**
 *  This determines the maximum number of survivors.
 *
 *  @author: 08/04/1996 JLB - Created
 *           ZivDero - Adjustments for Tiberian Sun
 */
int BuildingClassExt::_How_Many_Survivors() const
{
    if (IsSurvivorless || !Class->IsCrew)
        return 0;

    int divisor = SideClassExtension::Get_Survivor_Divisor(House);
    if (divisor == 0)
        return 0;

    if (IsCaptured)
        divisor *= 2;

    const int count = Class->Cost_Of(House) * Rule->SurvivorFraction / divisor;
    return std::clamp(count, 1, 5);
}


/**
 *  Fetch the shape number for this building.
 *
 *  @author: 07/29/1996 JLB - Created
 *           ZivDero - Adjustments for Tiberian Sun
 */
int BuildingClassExt::_Shape_Number() const
{
    int shapenum = Fetch_Stage();

    if (Class->IsLaserFence) {
        return LaserFenceFrame;
    }

    if (Class->IsFirestormWall) {
        return FirestormWallFrame;
    }

    /**
     *  The shape file to use for rendering depends on whether the building
     *  is undergoing construction or not.
     */
    if (BState == BSTATE_CONSTRUCTION) {

        if (Class->IsGate) {
            shapenum = Class->Anims[BSTATE_CONSTRUCTION].Start + Class->Anims[BSTATE_CONSTRUCTION].Count - 1 - shapenum;
        }

        /**
         *  If the building is deconstructing, then the display frame progresses
         *  from the end to the beginning. Reverse the shape number accordingly.
         */
        if (Mission == MISSION_DECONSTRUCTION) {
            shapenum = Class->Anims[BState].Start + Class->Anims[BState].Count - 1 - shapenum;
        }

    } else if (Class->IsGate) {

        if (HealthRatio <= Rule->ConditionYellow) {
            return Class->GateStages + 1;
        } else {
            return 0;
        }

    } else {

        /**
         *  If below half strenth, then show the damage frames of the
         *  building.
         */
        if (HealthRatio <= Rule->ConditionYellow) {
            if (BState == BSTATE_IDLE) {
                shapenum++;
            } else {
                int last1 = Class->Anims[BSTATE_IDLE].Start + Class->Anims[BSTATE_IDLE].Count;
                int last2 = Class->Anims[BSTATE_ACTIVE].Start + Class->Anims[BSTATE_ACTIVE].Count;
                int largest = std::max(last1, last2);
                last2 = Class->Anims[BSTATE_AUX1].Start + Class->Anims[BSTATE_AUX1].Count;
                largest = std::max(largest, last2);
                last2 = Class->Anims[BSTATE_AUX2].Start + Class->Anims[BSTATE_AUX2].Count;
                largest = std::max(largest, last2);
                shapenum += largest;
            }
        }
    }
    return shapenum;
}


/**
 *  Detaches the animation from the building, and also
 *  creates a "sequel" animation in some cases.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Detach_Anim(AnimClass* anim)
{
    if (IsActive) {
        for (int i = 0; i < BANIM_COUNT; i++) {
            if (Anims[i] == anim) {
                Anims[i] = nullptr;
                switch (i) {
                case BANIM_SPECIAL_ONE:
                    if (Class->IsCanUnitRepair) {
                        if (In_Radio_Contact() && Get_Mission() == MISSION_REPAIR) {
                            Begin_Anim(BANIM_SPECIAL_TWO, Get_Health_Ratio() <= Rule->ConditionYellow, 0);
                        }
                    }
                    else if (Class->IsNukeSilo) {
                        if (Get_Mission() == MISSION_MISSILE) {
                            Begin_Anim(BANIM_SPECIAL_TWO, Get_Health_Ratio() <= Rule->ConditionYellow, 0);
                        }
                    }
                    IsToDisplay = true;
                    break;

                case BANIM_SPECIAL_TWO:
                    if (Class->IsNukeSilo) {
                        if (Get_Mission() == MISSION_MISSILE) {
                            Begin_Anim(BANIM_SPECIAL_THREE, Get_Health_Ratio() <= Rule->ConditionYellow, 0);
                        }
                    }
                    IsToDisplay = true;
                    break;

                case BANIM_SPECIAL_THREE:
                    IsToDisplay = true;
                    break;

                default:
                    break;
                }
            }
        }
    }
}


/**
 *  Reimplementation of BuildingClass::Draw_It.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Draw_It(Point2D const& xdrawpoint, Rect const& xcliprect)
{
    Cell cell = Get_Cell();

    /*
    **  The shape file to use for rendering depends on whether the building
    **  is undergoing construction or not.
    */
    ShapeSet const* shapefile = Get_Image_Data();
    if (shapefile == nullptr) return;

    if (Class->IsInvisibleInGame) return;

    const auto type_ext = Extension::Fetch(Class);
    if (type_ext->IsHideDuringSpecialAnim && (Anims[BANIM_SPECIAL_ONE] || Anims[BANIM_SPECIAL_TWO] || Anims[BANIM_SPECIAL_THREE])) return;

    bool open_roof = false;
    if (Get_Mission() == MISSION_UNLOAD) {
        TechnoClass* radio = Contact_With_Whom();
        if (radio != nullptr && radio->TClass->Locomotor == __uuidof(JumpjetLocomotionClass)) {
            open_roof = true;
        }
    }

    Point2D zdrawpoint(144, 172);
    int zadjust = Class->NormalZAdjust;

    if (Get_Mission() == MISSION_OPEN && !Door.Func1()) {

        int shapenum = static_cast<int>(Door.Get_Percent_Complete() * Class->GateStages);
        if (Door.Is_Door_Closing()) {
            shapenum = Class->GateStages - shapenum;
        }
        if (Door.Is_Door_Closed()) {
            shapenum = 0;
        }
        if (Door.Is_Door_Open()) {
            shapenum = Class->GateStages - 1;
        }
        shapenum = std::min(shapenum, Class->GateStages - 1);
        shapenum = std::max(shapenum, 0);

        shapefile = Get_Image_Data();

        ZGradientType zgrad = ZGRAD_GROUND;
        if (shapenum < Class->GateStages / 2 || type_ext->IsBarGate) {
            zgrad = ZGRAD_90DEG;
        }

        shapenum += HealthRatio <= Rule->ConditionYellow ? Class->GateStages + 1 : 0;
        Techno_Draw_Object(shapefile, shapenum, xdrawpoint, xcliprect, DIR_N, 256, zadjust - TacticalMap->Z_Lepton_To_Pixel(Height), zgrad, true, Map[cell].Brightness);

        return;
    }

    if (Get_Mission() == MISSION_UNLOAD) {
        if (open_roof) {
            if (type_ext->RoofDeployingAnim != nullptr) {
                shapefile = type_ext->RoofDeployingAnim;
                zadjust = 0;
            }
        }
        else {
            if (Class->DeployingAnim != nullptr) {
                shapefile = Class->DeployingAnim;
                zadjust = 0;
            }
        }
    }

    Point2D drawpoint = xdrawpoint;
    int height = drawpoint.Y + shapefile->Get_Height() / 2;

    Rect cliprect = xcliprect;
    cliprect.Height = std::min(cliprect.Height, height);

    zdrawpoint += Class->ZShapePointMove;
    Point2D zsizeoffset(Class->Width() * CELL_LEPTON_W - CELL_LEPTON_W, Class->Height() * CELL_LEPTON_H - CELL_LEPTON_H);
    zdrawpoint -= TacticalMap->func_60F270(zsizeoffset);

    ShapeSet const* zshapefile = BuildingTypeClass::BuildingZShape;
    if (Class->Width() >= 6) {
        zshapefile = nullptr;
    }

    if (cliprect.Height > 0) {

        /*
        **  Actually draw the building shape.
        */
        if ((Class->IsLaserFence && (LaserFenceFrame == 12 || LaserFenceFrame == 8)) || Class->IsFirestormWall) {
            Techno_Draw_Object(shapefile, Shape_Number(), drawpoint, cliprect, DIR_N, 256, -1 - TacticalMap->Z_Lepton_To_Pixel(Height), ZGRAD_GROUND, true, Map[cell].Brightness + Class->ExtraLight);
        }
        else {
            Techno_Draw_Object(shapefile, Shape_Number() < shapefile->Get_Count() / 2 ? Shape_Number() : shapefile->Get_Count() / 2, drawpoint, cliprect, DIR_N, 256, zadjust - TacticalMap->Z_Lepton_To_Pixel(Height), ZGRAD_90DEG, true, Map[cell].Brightness + Class->ExtraLight, zshapefile, 0, zdrawpoint);
        }
    }

    /*
    **  Patch for adding overlay onto weapon factory.  Only add the overlay if
    **  the building has more than 1 hp.  Also, if the building's in radio
    **  contact, he must be unloading a constructed vehicle, so draw that
    **  vehicle before drawing the overlay.
    */
    if (Class->BibShape && BState != BSTATE_CONSTRUCTION) {
        Techno_Draw_Object(Class->BibShape, Shape_Number(), xdrawpoint, xcliprect, DIR_N, 256, -1 - TacticalMap->Z_Lepton_To_Pixel(Height), ZGRAD_GROUND, true, Map[cell].Brightness + Class->ExtraLight);
    }

    /*
    **  Draw the weapon factory custom overlay graphic.
    */
    if (Get_Mission() == MISSION_UNLOAD) {
        ShapeSet const* under_door_anim;
        if (open_roof) {
            under_door_anim = type_ext->UnderRoofDoorAnim;
        } else {
            under_door_anim = Class->UnderDoorAnim;
        }
        if (under_door_anim != nullptr) {
            Techno_Draw_Object(under_door_anim, HealthRatio <= Rule->ConditionYellow ? 1 : 0, xdrawpoint, xcliprect, DIR_N, 256, -TacticalMap->Z_Lepton_To_Pixel(Height), ZGRAD_GROUND, true, Map[cell].Brightness + Class->ExtraLight);
        }
    }
}


/**
 *  Reimplementation of BuildingClass::Detach_All.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Detach_All(bool all)
{
    if (all) {
        /*
        **  If it is producing something, then it must be abandoned.
        */
        if (Factory) {
            Factory->Abandon();
            delete Factory;
            Factory = nullptr;
        }

        /*
        **  If the owner HouseClass is building something, and this building can
        **  build that thing, we may be the last building for that house that can
        **  build that thing; if so, abandon production of it.
        */
        if (House) {
            auto type_ext = Extension::Fetch(Class);
            ProductionFlags prodflags = PRODFLAG_NONE;
            if (type_ext->IsNaval) {
                prodflags = PRODFLAG_NAVAL;
            }

            FactoryClass* factory = Extension::Fetch(House)->Fetch_Factory(Class->ToBuild, prodflags);

            /*
            **  If a factory was found, then temporarily disable this building and then
            **  determine if any object that is being produced can still be produced. If
            **  not, then the object being produced must be abandoned.
            */
            if (factory) {
                TechnoClass* object = factory->Get_Object();
                bool limbo = IsInLimbo;
                IsInLimbo = true;
                if (object && !object->TClass->Who_Can_Build_Me(true, false, false, House)) {
                    Extension::Fetch(House)->Abandon_Production(Class->ToBuild, -1, prodflags);
                }
                IsInLimbo = limbo;
            }
        }
    }

    if (!all) {
        if (In_Radio_Contact() && !House->Is_Ally(Contact_With_Whom())) {
            Transmit_Message(RADIO_OVER_OUT);
        }
    } else {
        Transmit_Message(RADIO_OVER_OUT);
    }

    TechnoClass::Detach_All(all);
}


/**
 *  Reimplementation of BuildingClass::Toggle_Primary.
 *
 *  @author: ZivDero
 */
bool BuildingClassExt::_Toggle_Primary()
{
    if (Class->ToBuild == RTTI_NONE) {
        return IsLeader;
    }

    if (IsLeader) {
        IsLeader = false;
    } else {
        for (int index = 0; index < Buildings.Count(); index++) {
            BuildingClass* building = Buildings[index];

            if (!building->IsInLimbo && building->House == House && building->Class->ToBuild == Class->ToBuild &&
                Extension::Fetch(building->Class)->IsNaval == Extension::Fetch(Class)->IsNaval) {
                building->IsLeader = false;
            }
        }
        IsLeader = true;
        if (House->Is_Player_Control()) {
            Speak(VOX_PRIMARY_SELECTED);
        }
    }
    Mark(MARK_CHANGE);
    return IsLeader;
}


/**
 *  Reimplementation of BuildingClass::Assign_Rally_Point.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Assign_Rally_Point(Cell const& cell)
{
    SpeedType speed = SPEED_FOOT;
    MZoneType mzone = MZONE_NORMAL;

    bool underbridge = Map[cell].IsUnderBridge;

    if (Class->ToBuild == RTTI_AIRCRAFTTYPE) {
        speed = SPEED_WINGED;
        mzone = MZONE_FLYER;
    } else {

        /**
         *  If this is a factory that produces units, and is flagged as a shipyard (Naval=yes), then
         *  change the zone flags to scan for water regions only.
         *
         *  @author: CCHyper, modified by Rampastring
         */
        if (Class->ToBuild == RTTI_UNITTYPE && Extension::Fetch(Class)->IsNaval) {
            speed = SPEED_AMPHIBIOUS;
            mzone = MZONE_AMPHIBIOUS_CRUSHER;
        }
    }

    int zone = Map.Get_Cell_Zone(Get_Coord().As_Cell(), mzone, underbridge);

    Cell nearbyloc = Map.Nearby_Location(cell, speed, zone, mzone, underbridge);

    if (nearbyloc != CELL_NONE) {
        OutList.Add(EventClass(Owner(), EVENT_ARCHIVE, TargetClass(this), TargetClass(&Map[nearbyloc])));
    } else {
        if (Class->IsConstructionYard && House->Is_Human_Player() && Session.Type != GAME_NORMAL && Session.Options.RedeployMCV) {
            OutList.Add(EventClass(Owner(), EVENT_ARCHIVE, TargetClass(this), TargetClass(&Map[Center_Coord()])));
        }
    }
}


/**
 *  Reimplementation of BuildingClass::What_Action.
 *
 *  @author: ZivDero
 */
ActionType BuildingClassExt::_What_Action(ObjectClass const* object, bool disallow_force)
{
    if (Class->IsInvisibleInGame) {
        return ACTION_NONE;
    }

    if (object->RTTI == RTTI_BUILDING && ((BuildingClass*)object)->Class->IsInvisibleInGame) {
        return ACTION_NONE;
    }

    ActionType action = TechnoClass::What_Action(object, disallow_force);

    if (action == ACTION_SELF) {
        int index; 
        if (EMPFramesRemaining == 0 && Class->ToBuild != RTTI_NONE && PlayerPtr == House &&
            Extension::Fetch(House)->Factory_Count(Class->ToBuild, Extension::Fetch(Class)->IsNaval ? PRODFLAG_NAVAL : PRODFLAG_NONE) > 1) {

            switch (Class->ToBuild) {
            case RTTI_INFANTRYTYPE:
            case RTTI_INFANTRY:
                action = ACTION_NONE;
                for (index = 0; index < Buildings.Count(); index++) {
                    BuildingClass* bldg = Buildings[index];
                    if (bldg != this && bldg->House == House && bldg->Class->ToBuild == RTTI_INFANTRYTYPE) {
                        action = ACTION_SELF;
                        break;
                    }
                }
                break;

            case RTTI_NONE:
                action = ACTION_NONE;
                break;

            default:
                break;
            }

        } else {
            action = ACTION_NONE;
        }
    }

    /*
    **  Don't allow targeting with SAM sites, even if the CTRL key
    **  is held down. Also don't allow targeting if the object is too
    **  far away.
    */
    if (action == ACTION_ATTACK && PrimaryWeapon != nullptr) {
        if (!In_Range_Of(const_cast<ObjectClass*>(object))/* || !PrimaryWeapon->Bullet->IsAntiGround*/) {
            action = ACTION_NONE;
        } else if (Class->IsEMPulseCannon || Class->IsLimpetMine) {
            action = ACTION_NONE;
        }
        if (CurrentMission == MISSION_DECONSTRUCTION) {
            action = ACTION_NONE;
        }
    }

    if (action == ACTION_MOVE || action == ACTION_NOMOVE) {
        if (!Can_Player_Move()) {
            action = ACTION_SELECT;
        } else if (Class->ToBuild == RTTI_INFANTRYTYPE || Class->ToBuild == RTTI_UNITTYPE || Class->ToBuild == RTTI_AIRCRAFTTYPE) {
            bool altdown = Keyboard->Down(Options.KeyForceMove1) || Keyboard->Down(Options.KeyForceMove2);
            if (!altdown) {
                action = ACTION_SELECT;
            } else {
                Cell cell = object->Center_Coord().As_Cell();
                if (Class->ToBuild != RTTI_AIRCRAFTTYPE) {
                    if (!Is_In_Same_Zone(cell.As_Coord())) {
                        action = ACTION_NOMOVE;
                    }
                    if (!Map[cell].IsUnderBridge) {

                        /**
                         *  This patch allows naval yards to display the "place rally point" cursor action
                         *  on water cells.
                         *
                         *  @author: Rampastring
                         */
                        if (Map[cell].Passability != PASSABLE_LAND && !(Extension::Fetch(Class)->IsNaval && Map[cell].Passability == PASSABLE_WATER)) {
                            action = ACTION_NOMOVE;
                        }
                    }
                }
            }
        }
    }

    return action;
}


/**
 *  Reimplementation of BuildingClass::What_Action.
 *
 *  @author: ZivDero
 */
ActionType BuildingClassExt::_What_Action(const Cell& cell, bool check_fog, bool disallow_force) const
{
    if (Class->IsInvisibleInGame) {
        return ACTION_NONE;
    }

    ActionType action;

    if (Class->UndeploysInto != nullptr && check_fog) {
        action = TechnoClass::What_Action(cell, false, disallow_force);
    } else {
        action = TechnoClass::What_Action(cell, check_fog, disallow_force);
    }


    if (action == ACTION_RALLY_TO_POINT) {
        if (Class->ToBuild != RTTI_AIRCRAFTTYPE) {
            if (!Is_In_Same_Zone(cell.As_Coord())) {
                action = ACTION_NOMOVE;
            }
            if (!Map[cell].IsUnderBridge) {

                /**
                 *  This patch allows naval yards to display the "place rally point" cursor action
                 *  on water cells.
                 *
                 *  @author: Rampastring
                 */
                if (Map[cell].Passability != PASSABLE_LAND && !(Extension::Fetch(Class)->IsNaval && Map[cell].Passability == PASSABLE_WATER)) {
                    action = ACTION_NOMOVE;
                }
                
            }
        }
    }

    /*
    **  Don't allow targeting of SAM sites, even if the CTRL key
    **  is held down.
    */
    if (action == ACTION_ATTACK && PrimaryWeapon != nullptr) {
        if (!PrimaryWeapon->Bullet->IsAntiGround) {
            action = ACTION_NONE;
        } else if (Class->IsEMPulseCannon || Class->IsLimpetMine) {
            action = ACTION_NONE;
        } if (CurrentMission == MISSION_DECONSTRUCTION) {
            action = ACTION_NONE;
        }
    }

    return action;
}


/**
 *  Reimplementation of BuildingClass::Factory_AI.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Factory_AI()
{
    /*
    **  Handle any production tied to this building. Only computer controlled buildings have
    **  production attached to the building itself. The player uses the sidebar interface for
    **  all production control.
    */
    if (Factory != nullptr && Factory->Has_Completed() && PlacementDelay == 0) {
        TechnoClass* product = Factory->Get_Object();

        switch (Exit_Object(product)) {

            /*
            **  If the object could not leave the factory, then either request
            **  a transport, place the (what must be a) building using another method, or
            **  abort the production and refund money.
            */
        case 0:
            Factory->Abandon();
            delete Factory;
            Factory = nullptr;
            break;

            /*
            **  Exiting this building is prevented by some temporary blockage. Wait
            **  a bit before trying again.
            */
        case 1:
            PlacementDelay = static_cast<int>(Rule->PlacementDelay * TICKS_PER_MINUTE);
            break;

            /*
            **  The object was successfully sent from this factory. Inform the house
            **  tracking logic that the requested object has been produced.
            */
        case 2:
            House->Just_Built(product);
            Factory->Completed();
            delete Factory;
            Factory = nullptr;
            break;

        default:
            break;
        }
    }

    /*
    **  Pick something to create for this factory.
    */
    if (House->IsStarted && Mission != MISSION_CONSTRUCTION && Mission != MISSION_DECONSTRUCTION) {

        /*
        **  Buildings that produce other objects have special factory logic handled here.
        */
        if (Class->ToBuild != RTTI_NONE) {
            if (Factory != nullptr) {

                /*
                **  If production has halted, then just abort production and make the
                **  funds available for something else.
                */
                if (PlacementDelay == 0 && !Factory->Is_Building()) {
                    Factory->Abandon();
                    delete Factory;
                    Factory = nullptr;
                }

            } else {

                /*
                **  Only look to start production if there is at least a small amount of
                **  money available. In cases where there is no practical money left, then
                **  production can never complete -- don't bother starting it.
                */
                if (House->IsStarted && House->Available_Money() > 10) {
                    auto btype_ext = Extension::Fetch(Class);
                    TechnoTypeClass const* ttype = Extension::Fetch(House)->Suggest_New_Object(Class->ToBuild, btype_ext->IsNaval ? PRODFLAG_NAVAL : PRODFLAG_NONE);

                    /*
                    **  If a suitable object type was selected for production, then start
                    **  producing it now.
                    */
                    if (ttype != nullptr) {

                        /*
                        **  But first, verify if this building is a valid factory for this object.
                        */
                        bool allowed_factory = true;
                        auto ttype_ext = Extension::Fetch(ttype);
                        if (ttype->RTTI == RTTI_UNITTYPE) {
                            if (btype_ext->IsNaval != ttype_ext->IsNaval) {
                                allowed_factory = false;
                            }
                        }

                        /*
                        ** There may be limitations on whether this specific factory can build this object.
                        */
                        if (allowed_factory && !ttype_ext->BuiltAt.Is_Present(Class)) {

                            /*
                            **  This object doesn't allow this factory to produce it.
                            */
                            if (ttype_ext->BuiltAt.Count() != 0) {
                                allowed_factory = false;
                            }

                            /*
                            **  This factory can't produce this kind of object.
                            */
                            else if (btype_ext->IsExclusiveFactory) {
                                allowed_factory = false;
                            }
                        }

                        /*
                        **  If everything is okay, create the factory.
                        */
                        if (allowed_factory) {
                            Factory = new FactoryClass;
                            if (Factory != nullptr) {
                                if (!Factory->Set(*ttype, *House, false)) {
                                    delete Factory;
                                    Factory = nullptr;
                                } else {
                                    House->Production_Begun(Factory->Get_Object());
                                    Factory->Start(false);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}


/**
 *  Reimplementation of BuildingClass::Fetch_Super_Weapon.
 *
 *  @author: ZivDero
 */
SuperWeaponType BuildingClassExt::_Fetch_Super_Weapon() const
{
    if (Class->SuperWeapon != SUPER_NONE) {
        BuildingTypeClass const* aux = Supers[Class->SuperWeapon]->Class->AuxBuilding;

        /**
         *  Fix: use the prerequisite check to allow building upgrades to be AuxBulding.
         */
        if (aux != nullptr && !Extension::Fetch(House)->Has_Prerequisite(aux->HeapID)) {
            return SUPER_NONE;
        }
    }
    return Class->SuperWeapon;
}


/**
 *  Reimplementation of BuildingClass::Fetch_Super_Weapon2.
 *
 *  @author: ZivDero
 */
SuperWeaponType BuildingClassExt::_Fetch_Super_Weapon2() const
{
    if (Class->SuperWeapon2 != SUPER_NONE) {

        /**
         *  Fix: use the prerequisite check to allow building upgrades to be AuxBulding.
         */
        BuildingTypeClass const* aux = Supers[Class->SuperWeapon2]->Class->AuxBuilding;
        if (aux != nullptr && !Extension::Fetch(House)->Has_Prerequisite(aux->HeapID)) {
            return SUPER_NONE;
        }
    }
    return Class->SuperWeapon2;
}


/**
 *  This patch fetches the correct factory when displaying a cameo on a spied factory.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x00428AA4, _BuildingClass_Draw_Overlays_Fetch_Factory_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    BuildingTypeClassExtension const* type_ext = Extension::Fetch(this_ptr->Class);
    FactoryClass* factory = Extension::Fetch(this_ptr->House)->Fetch_Factory(
        this_ptr->Class->ToBuild, type_ext->IsNaval ? PRODFLAG_NAVAL : PRODFLAG_NONE);
    R->EAX(factory);

    return 0x00428AC4;
}


/**
 *  Replaces an inlined call of Detach_Anim with a direct call.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x00433F1D, _BuildingClass_Detach_Detach_Anim_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);
    GET(AnimClass*, anim, ECX);

    this_ptr->Detach_Anim(anim);

    return 0x00433F84;
}


/**
 *  Reimplements the BuildingClass::Draw_Overlays function.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Draw_Overlays(const Point2D& coord, const Rect& rect)
{
    if (BState != BSTATE_CONSTRUCTION) {

        /**
         *  Draw the repair animation.
         */
        if (IsRepairing) {
            if (!Map.Is_Shrouded(Center_Coord()) && !(Scen->Special.IsFogOfWar || IsFogged) && Visual_Character() != VISUAL_HIDDEN) {
                Point2D xy = coord;
                if (!IsOn) xy -= Point2D(5, 5);

                int delay = Options.Normalize_Delay(14) / 4;
                delay = std::max(delay, 2);

                int draw_frame;
                if (Extension::Fetch(House)->IsPauseRepairs && House->Available_Money() < Class->Repair_Step()) {
                    draw_frame = RuleExtension->PausedRepairsFrame;
                } else {
                    draw_frame = 6 * (Frame % delay) / (delay - 1);
                }

                Draw_Shape(*LogicalSurface, *MouseDrawer, WrenchShape, draw_frame, xy, rect, SHAPE_ALPHA | SHAPE_WIN_REL | SHAPE_CENTER);
            }
        }

        /**
         *  Draw the power off animation.
         */
        if (!IsOn && House->Is_Player_Control()) {
            if (!Map.Is_Shrouded(Center_Coord()) && !(Scen->Special.IsFogOfWar || IsFogged)) {
                Point2D xy = coord;
                if (IsRepairing) xy += Point2D(10, 10);

                int delay = Options.Normalize_Delay(14) / 4;
                delay = std::max(delay, 2);

                Draw_Shape(*LogicalSurface, *MouseDrawer, PowerOffShape, 6 * (Frame % delay) / (delay - 1), xy, rect, SHAPE_ALPHA | SHAPE_WIN_REL | SHAPE_CENTER);
            }
        }

        if (IsSelected) {

            /**
             *  Draw the primary factory pip.
             */
            if (House->Is_Ally(PlayerPtr) || SpiedBy & (1 << (PlayerPtr->Class->House)) || Session.ObiWan) {
                Point2D xy(coord.X - 10, coord.Y + 10);
                Draw_Text_Overlay(xy, coord, rect);
            }

            /**
             *  If this is a factory, and the player has spied its owner, draw the cameo of what it's currently producing.
             */
            if (SpiedBy & (1 << (PlayerPtr->Class->House)) || Session.ObiWan) {

                BuildingTypeClassExtension* btypeext = Extension::Fetch(Class);
                FactoryClass* factory = House->Is_Human_Player() ? Extension::Fetch(House)->Fetch_Factory(Class->ToBuild, btypeext->IsNaval ? PRODFLAG_NAVAL : PRODFLAG_NONE) : Factory;
                if (factory != nullptr) {
                    ObjectClass* obj = factory->Get_Object();
                    if (obj != nullptr) {

                        /**
                         *  #issue-487
                         *
                         *  Adds support for PCX/PNG cameo icons.
                         *
                         *  @author: CCHyper
                         */
                        const auto technotypeext = Extension::Fetch(obj->Techno_Type_Class());
                        if (technotypeext->CameoImageSurface) {

                            /**
                             *  Draw the cameo pcx image.
                             */
                            Rect pcxrect;
                            pcxrect.X = rect.X + coord.X;
                            pcxrect.Y = rect.Y + coord.Y;
                            pcxrect.Width = technotypeext->CameoImageSurface->Get_Width();
                            pcxrect.Height = technotypeext->CameoImageSurface->Get_Height();

                            SpriteCollection.Draw(pcxrect, *LogicalSurface, *technotypeext->CameoImageSurface);
                        } else {
                            const ShapeSet* shape = obj->TClass->Get_Cameo_Data();

                            /**
                             *  Draw the cameo shape.
                             *
                             *  Original code used NormalDrawer, which is the old Red Alert shape
                             *  drawer, so we need to use CameoDrawer here for the correct palette.
                             */
                            Draw_Shape(*LogicalSurface, *CameoDrawer, shape, 0, coord, rect, SHAPE_ALPHA | SHAPE_WIN_REL | SHAPE_CENTER);
                        }
                    }
                }
            }
        }
    }
}


/**
 *  Extracted logic for #issue-204 fixes for reuse.
 *  Note that this function assumes the building is in contact with a valid aircraft.
 *
 *  @author: CCHyper, JoyfulShush
 */
int Get_Aircraft_Reload_Time(BuildingClass* building)
{
    AircraftClass* radio = reinterpret_cast<AircraftClass*>(building->Contact_With_Whom());
    AircraftTypeClassExtension* radio_class_ext = Extension::Fetch(radio->Class);
    return radio_class_ext->Get_ReloadRate() * TICKS_PER_MINUTE;
}


/**
 *  #issue-204
 * 
 *  Implements ReloadRate for AircraftTypes, allowing each aircraft to have
 *  its own independent ammo reloading rate when docked with a helipad.
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x0043266C, _BuildingClass_Mission_Repair_ReloadRate_Patch_Initial, 0)
{
    GET(BuildingClass*, this_ptr, EBP);
           
    int time = Get_Aircraft_Reload_Time(this_ptr);
    R->EAX(time);

    return 0x0043260F;
}


/**
 *  #issue-204
 *
 *  Implements ReloadRate for AircraftTypes, allowing each aircraft to have
 *  its own independent ammo reloading rate when docked with a helipad.
 *  Fixes an issue where aircraft only did reload with their own rate when starting to regenerate ammo
 *
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x004325FE, _BuildingClass_Mission_Repair_ReloadRate_Patch_During, 0)
{
    GET(BuildingClass*, this_ptr, EBP);

    int time = Get_Aircraft_Reload_Time(this_ptr);
    R->EAX(time);

    return 0x0043260F;
}


/**
 *  #issue-966
 *
 *  Assigns destination to a unit when it's leaving a service depot.
 *
 *  @author: Rampastring
 */
bool _BuildingClass_Mission_Repair_Assign_Unit_Destination(BuildingClass *building, TechnoClass *techno, bool clear_archive)
{
    // The facility has completed service. A retained CAP replaces only its
    // internal ground exit/rally Move; explicit player Move already canceled CAP.
    if (techno->RTTI == RTTI_AIRCRAFT) {
        auto aircraft = static_cast<AircraftClass*>(techno);
        const auto ext = Extension::Fetch(aircraft);
        if (FixedWing::Enabled(aircraft) && ext->CAPMode != FixedWingCAPMode::None && ext->CAPRearming) {
            // Retain service contact while unpowered/EMP-disabled or not yet
            // fully reloaded; the existing facility retry/Power_On path owns it.
            if (!FixedWing::Resume_After_Service(aircraft)) return false;
            if (clear_archive) techno->ArchiveTarget = nullptr;
            building->Transmit_Message(RADIO_OVER_OUT);
            techno->field_20C = nullptr;
            return true;
        }
    }
    AbstractClass * target = nullptr;

    if (building->ArchiveTarget != nullptr) {
        if (building->ArchiveTarget != nullptr) target = building->ArchiveTarget;
    }

    if (target == nullptr) {

        /**
         *  Stolen bytes/code.
         *  Reimplements original game behaviour.
         */
        Cell exitcell = building->Find_Exit_Cell(reinterpret_cast<TechnoClass*>(building->Radio));

        if (exitcell.X == 0 && exitcell.Y == 0) {

            /**
             *  Failed to find valid exit cell.
             */
            return false;
        }

        CellClass* cellptr = &Map[exitcell];
        target = As_Target(cellptr);
    }

    techno->Assign_Mission(MISSION_MOVE);
    techno->Assign_Destination(target);

    if (clear_archive) {
        /**
         *  Clear the archive target in cases where the original game did so as well.
         */
        techno->ArchiveTarget = nullptr;
    }

    building->Transmit_Message(RADIO_OVER_OUT);
    techno->field_20C = nullptr;
    return true;
}


/**
 *  #issue-966
 *
 *  Makes Service Depots assign their archive target as the destination to units
 *  that didn't need any repair when entering the depot.
 *
 *  This reimplements the whole 0x00432184 - 0x00432202 range of the original game's code.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x00432184, _BuildingClass_Mission_Repair_Assign_Rally_Destination_When_No_Repair_Needed, 0)
{
    GET(BuildingClass*, building, EBP);
    GET(TechnoClass*, techno, ESI);

    _BuildingClass_Mission_Repair_Assign_Unit_Destination(building, techno, true);

    /**
     *  Set mission delay and return, regardless of whether the
     *  destination assignment succeeded.
     */
    return 0x004324DF;
}


/**
 *  #issue-966
 *
 *  Makes Service Depots assign their archive target as the destination to the
 *  unit that they've finished repairing.
 *
 *  This reimplements the whole 0x00431DAB - 0x00431E27 range of the original game's code.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x00431DAB, _BuildingClass_Mission_Repair_Assign_Rally_Destination_After_Repair_Complete, 0)
{
    GET(BuildingClass *, building, EBP);
    GET(TechnoClass *, techno, ESI);

    if (_BuildingClass_Mission_Repair_Assign_Unit_Destination(building, techno, false)) {
        goto success;
    } else {
        goto fail_return;
    }

    /**
     *  The unit destination was applied successfully.
     */
success:
    R->EAX(true);
    return 0x00431E27;

    /**
     *  Return from the function without assigning any location for the unit to move into.
     */
fail_return:
    return 0x00431E90;
}


/**
 *  #issue-26
 * 
 *  Adds functionality for the produce cash per-frame logic.
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x00429A96, _BuildingClass_AI_ProduceCash_Patch, 0)
{
    GET(BuildingClass *, this_ptr, ESI);

    /**
     *  Fetch the extension instance.
     */
    Extension::Fetch(this_ptr)->Produce_Cash_AI();
    Extension::Fetch(this_ptr)->Maintain_Aircraft_Docks();

    /**
     *  Stolen bytes/code here.
     */
original_code:

    /**
     *  Animation per frame update.
     */
    this_ptr->Animation_AI();

    return 0x00429A9D;
}


/**
 *  #issue-26
 * 
 *  Grants cash bonus and starts the cash timer on building capture.
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x0042F67D, _BuildingClass_Captured_ProduceCash_Patch, 0)
{
    GET(BuildingClass *, this_ptr, ESI);
    GET_STACK(HouseClass *, newowner, 0x58);

    /**
     *  Fetch the extension instances.
     */
    BuildingClassExtension* ext_ptr = Extension::Fetch(this_ptr);
    BuildingTypeClassExtension* exttype_ptr = Extension::Fetch(this_ptr->Class);

    /**
     *  Is the owner a passive/neutral house? Only they can provide the capture bonus.
     */
    if (this_ptr->House->Class->IsMultiplayPassive) {

        /**
         *  Should this building produce a cash bonus on capture?
         */
        if (exttype_ptr->ProduceCashStartup > 0) {

            /**
             *  Grant the bonus to the new owner, making sure this
             *  building has not already done so if flagged
             *  as a one time bonus.
             */
            if (!ext_ptr->IsCaptureOneTimeCashGiven) {
                newowner->Refund_Money(exttype_ptr->ProduceCashStartup);
            }

            /**
             *  Is a one time bonus?
             */
            if (exttype_ptr->IsStartupCashOneTime) {
                ext_ptr->IsCaptureOneTimeCashGiven = true;
            }

            /**
             *  Start the cycle timer.
             */
            ext_ptr->ProduceCashTimer = exttype_ptr->ProduceCashDelay;
            ext_ptr->ProduceCashTimer.Start();
        }

        /**
         *  Should we reset the available budget?
         */
        if (exttype_ptr->IsResetBudgetOnCapture) {
            if (exttype_ptr->ProduceCashBudget > 0) {
                ext_ptr->CurrentProduceCashBudget = exttype_ptr->ProduceCashBudget;
            }
        }
    }

    /**
     *  Stolen bytes/code here.
     */
original_code:
    if (this_ptr->Class->IsCloakGenerator) {
        newowner->HasCloakGenerator = true;
    }

    return 0x0042F68E;
}


/**
 *  #issue-26
 * 
 *  Starts the cash timer on building placement complete (the "grand opening").
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x0042E179, _BuildingClass_Grand_Opening_ProduceCash_Patch, 0)
{
    GET_STACK(bool, captured, 0x40);
    GET(BuildingClass *, this_ptr, ESI);
    BuildingClassExtension *ext_ptr;
    BuildingTypeClassExtension *exttype_ptr;

    /**
     *  Stolen bytes/code here.
     */
    if (this_ptr->HasOpened) {
        if (!captured) {
            goto function_return;
        }
        goto has_opened_else;
    }

    /**
     *  Fetch the extension instances.
     */
    ext_ptr = Extension::Fetch(this_ptr);
    exttype_ptr = Extension::Fetch(this_ptr->Class);

    /**
     *  Start the cash delay timer.
     */
    if (exttype_ptr->ProduceCashAmount != 0) {

        ext_ptr->ProduceCashTimer = exttype_ptr->ProduceCashDelay;
        ext_ptr->ProduceCashTimer.Start();

        if (exttype_ptr->ProduceCashBudget > 0) {
            ext_ptr->CurrentProduceCashBudget = exttype_ptr->ProduceCashBudget;
        }
    }

    /**
     *  Continue function flow (HasOpened == false).
     */
continue_function:
    return 0x0042E197;

    /**
     *  Function return.
     */
function_return:
    return 0x0042E9DF;

    /**
     *  Else case from "HasOpened" check.
     */
has_opened_else:
    return 0x0042E4C7;
}


/**
 *  #issue-65
 * 
 *  Gate lowering and rising sound overrides for buildings.
 * 
 *  @author: CCHyper
 */
DEFINE_HOOK(0x00433BB5, _BuildingClass_Mission_Open_Gate_Open_Sound_Patch, 0)
{
    GET(Coord *, coord, EAX);
    GET(BuildingClass *, this_ptr, ESI);

    BuildingTypeClass* buildingtype = this_ptr->Class;

    /**
     *  Fetch the default gate lowering sound.
     */
    VocType voc = Rule->GateDownSound;

    /**
     *  Fetch the extension instance.
     */
    BuildingTypeClassExtension* buildingtypeext = Extension::Fetch(buildingtype);

    /**
     *  Does this building have a custom gate lowering sound? If so, use it.
     */
    if (buildingtypeext->GateDownSound != VOC_NONE) {
        voc = buildingtypeext->GateDownSound;
    }

    /**
     *  Play the sound effect at the buildings location.
     */
    Static_Sound(voc, *coord);

    return 0x00433BC8;
}


DEFINE_HOOK(0x00433C6F, _BuildingClass_Mission_Open_Gate_Close_Sound_Patch, 0)
{
    GET(Coord *, coord, EAX);
    GET(BuildingClass *, this_ptr, ESI);

    BuildingTypeClass* buildingtype = this_ptr->Class;

    /**
     *  Fetch the default gate rising sound.
     */
    VocType voc = Rule->GateUpSound;

    /**
     *  Fetch the extension instance.
     */
    BuildingTypeClassExtension* buildingtypeext = Extension::Fetch(buildingtype);

    /**
     *  Does this building have a custom gate rising sound? If so, use it.
     */
    if (buildingtypeext->GateUpSound != VOC_NONE) {
        voc = buildingtypeext->GateUpSound;
    }

    /**
     *  Play the sound effect at the buildings location.
     */
    Static_Sound(voc, *coord);

    /**
     *  Function return (0).
     */
    return 0x00433C81;
}


/**
 *  #issue-333
 * 
 *  Fixes a division by zero crash when Rule->ShakeScreen is zero
 *  and a building dies/explodes.
 * 
 *  @author: CCHyper
 */
static void BuildingClass_Shake_Screen(BuildingClass *building)
{
    /**
     *  Fetch the extension instance.
     */
    BuildingTypeClassExtension* buildingtypeext = Extension::Fetch(
        static_cast<const BuildingTypeClass*>(building->TClass));

    /**
     *  #issue-414
     * 
     *  Can this unit shake the screen when it is destroyed?
     * 
     *  @author: CCHyper
     */
    if (buildingtypeext->IsShakeScreen) {

        /**
         *  If this building has screen shake values defined, then set the blitter
         *  offset values. GScreenClass::Blit will handle the rest for us.
         */
        if (buildingtypeext->ShakePixelXLo > 0 || buildingtypeext->ShakePixelXHi > 0
         || buildingtypeext->ShakePixelYLo > 0 || buildingtypeext->ShakePixelYHi > 0) {

            if (buildingtypeext->ShakePixelXLo > 0 || buildingtypeext->ShakePixelXHi > 0) {
                Map.ScreenX = Sim_Random_Pick(buildingtypeext->ShakePixelXLo, buildingtypeext->ShakePixelXHi);
            }
            if (buildingtypeext->ShakePixelYLo > 0 || buildingtypeext->ShakePixelYHi > 0) {
                Map.ScreenY = Sim_Random_Pick(buildingtypeext->ShakePixelYLo, buildingtypeext->ShakePixelYHi);
            }

        } else {

            /**
             *  Make sure both the screen shake factor and the buildings cost
             *  are valid before performing the division.
             */
            if (Rule->ShakeScreen > 0 && building->Class->Cost_Of() > 0) {

                int shakes = std::min(building->Class->Cost_Of() / Rule->ShakeScreen, 6);
                //int shakes = building->Class->Cost_Of() / Rule->ShakeScreen;
                if (shakes > 0) {

                    /**
                     *  #issue-414
                     * 
                     *  Restores the vertical screen shake when a strong building is destroyed.
                     * 
                     *  @author: CCHyper
                     */
                    Map.ScreenY = shakes;

                    //Shake_The_Screen(shakes);
                }

            }

        }

    }
}

DEFINE_HOOK(0x0042B250, _BuildingClass_Explode_ShakeScreen_Division_BugFix_Patch, 0)
{
    GET(BuildingClass *, this_ptr, ESI);

    BuildingClass_Shake_Screen(this_ptr);

    /**
     *  Continue execution of function.
     */
continue_function:

    /**
     *  #issue-502
     * 
     *  Fixes the bug where buildings randomly respawn in a "limbo" state
     *  when destroyed. The EDI register was used to set Strength to 0 further
     *  down in the function after we return back.
     * 
     *  @author: CCHyper
     */
    R->EDI(0);

    return 0x0042B27F;
}

/**
 *  #issue-1049
 *
 *  The AI undeploys deployed Tick Tanks, Artillery and Juggernauts that get attacked by something
 *  that is out of their range. This is done by assigning MISSION_DECONSTRUCTION, which is used for both
 *  undeploying and selling.
 *
 *  The AI does not check whether the building actually has UndeploysInto= specified as something
 *  non-null, meaning if the building has UndeploysInto as null, the AI ends up selling the
 *  buildings.
 *
 *  This patch fixes the bug by denying the AI from assigning MISSION_DECONSTRUCTION
 *  when the building has UndeploysInto as null.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x0042C624, _BuildingClass_Assign_Target_No_Deconstruction_With_Null_UndeploysInto, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    if (this_ptr->Class->UndeploysInto == nullptr) {

        /**
         *  This building cannot undeploy. Exit the function.
         */
        return 0x0042C58C;
    }

    /**
     *  Stolen bytes / code.
     *  Assign MISSION_DECONSTRUCTION and exit.
     */
    this_ptr->Assign_Mission(MISSION_DECONSTRUCTION);
    this_ptr->Commence();
    return 0x0042C63A;
}


bool Is_Allowed_Harvester(BuildingClass* building, UnitClass* harvester)
{
    int dockcount = harvester->Class->Dock.Count();

    for (int i = 0; i < dockcount; i++) {
        if (harvester->Class->Dock[i] == building->Class) {
            return true;
        }
    }

    return false;
}


/**
 *  #issue-445
 *
 *  Fixes a bug where crew wouldn't come out of sold/destroyed construction yards
 *  (or buildings that undeploy, to be more specific).
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x00430CC2, _BuildingClass_Mission_Deconstruction_ConYard_Survivors_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    // Unfortunately, it seems like Mission_Deconstruction does not know if the building was sold or is undeploying
    // So we're going to
    // a) check if this building doesn't ever undeploy
    // b) if it does undeploy, check that it has no archive target (the place you order them to move is set as the archive target, although rally points are also set as archive targets, so it may have side-effects)
    // c) ensure that this isn't artillery/icbm/etc.
    if ((this_ptr->ArchiveTarget == nullptr || this_ptr->Class->UndeploysInto == nullptr) && !this_ptr->Class->Is_Deployable())
    {
        // Process crew
        return 0x00430CE4;
    }

    // Don't process crew
    return 0x00430EEA;
}


/**
 *  Fixes a bug where if when undeploying a construction yard an
 *  MCV couldn't be placed, it would stay limboed.
 *
 *  @author: ZivDero
 */
static bool Unlimbo_Helper(UnitClass* unit, Coord const& coord, Dir256 dir)
{
    ScenarioInit++;
    bool result = unit->Unlimbo(coord, dir);
    ScenarioInit--;

    if (!result) {
        delete unit;
    }

    return result;
}

DEFINE_HOOK(0x00430A01, _BuildingClass_Mission_Deconstruction_ConYard_Unlimbo_Patch, 0)
{
    GET(UnitClass*, mcv, EBP);
    GET(Dir256, dir, EAX);
    LEA_STACK(Coord const*, coord, 0x40);

    if (Unlimbo_Helper(mcv, *coord, dir)) {
        return 0x00430A1A;
    } else {
        return 0x00430B37;
    }
}


/**
 *  Fixes a bug where you could receive double the amount of survivors
 *  if a building that was being sold got destroyed,
 *  or free survivors by undeploying a building that was being sold.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x00430F2B, _BuildingClass_Mission_Deconstruction_Double_Survivors_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    // We've already ejected the survivors, don't eject them any more.
    this_ptr->IsSurvivorless = true;

    // Stolen instructions
    this_ptr->Status = 2;
    this_ptr->Begin_Mode(BSTATE_CONSTRUCTION);

    return 0x00430F3B;
}


/**
 *  Patch to not assign archive targets to buildings currently being sold.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0049436A, _EventClass_Execute_Archive_Selling_Patch, 0)
{
    GET(TechnoClass*, techno, EDI);
    GET(AbstractClass *, target, EAX);

    // Don't assign an archive target if currently selling
    if (techno->Mission != MISSION_DECONSTRUCTION) {
        techno->Assign_Archive_Target(target);
    }

    return 0x00494372;
}


/**
 *  Patch in BuildingClass::Captured to not count captured DontScore buildings.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042F799, _BuildingClass_Captured_DontScore_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    auto ext = Extension::Fetch(this_ptr->Class);
    if ((Session.Type == GAME_INTERNET || Session.Type == GAME_IPX) && !ext->IsDontScore) {
        return 0x0042F7A3;
    }

    return 0x0042F7BB;
}

/**
 *  #issue-203
 *
 *  Assigns the last docked building of a spawned free unit on
 *  building placement complete (the "grand opening").
 *  This allows harvesters to know which refinery they spawned from.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x0042E5F5, _BuildingClass_Grand_Opening_Assign_FreeUnit_LastDockedBuilding_Patch, 6)
{
    GET(BuildingClass*, this_ptr, ESI);
    GET(UnitClass*, unit, EDI);

    UnitClassExtension* unitext = Extension::Fetch(unit);
    unitext->LastDockedBuilding = this_ptr;

    /**
     *  Continue the FreeUnit down-placing process.
     */
    return 0;
}


/**
 *  An enum for BuildingClass::Mission_Missile missile states
 */
enum {
    INITIAL,
    DOOR_OPENING,
    LAUNCH_UP,
    LAUNCH_DOWN,
    DONE_LAUNCH
};


/**
 *  Play SpecialAnim(Two, Three) as the MultiMissile/ChemMissile
 *  Nuke open/wait/close animations.
 *
 *  @author: ZivDero
 */
int _BuildingClass_Mission_Missile_INITIAL(BuildingClass * this_ptr)
{
    /**
     *  Play the silo opening animation.
     */
    this_ptr->Begin_Anim(BANIM_SPECIAL_ONE, this_ptr->HealthRatio <= Rule->ConditionYellow, 0);
    this_ptr->Status = DOOR_OPENING;
    return 1;
}


static bool Is_Anim_Present(BuildingClass * building, BAnimType anim)
{
    const char* name = building->HealthRatio <= Rule->ConditionYellow ? building->Class->field_580[anim].AnimDamaged : building->Class->field_580[anim].Anim;
    return std::strlen(name) != 0;
}


int _BuildingClass_Mission_Missile_DOOR_OPENING(BuildingClass* this_ptr)
{
    /**
     *  Check if the silo opening animation has finished.
     */
    if (this_ptr->Anims[BANIM_SPECIAL_TWO] != nullptr || !Is_Anim_Present(this_ptr, BANIM_SPECIAL_TWO)) {

        /**
         *  If so, signal that we're ready to fire and play the "holding open" animation.
         */
        this_ptr->Status = LAUNCH_UP;
    }
    return 1;
}


int _BuildingClass_Mission_Missile_LAUNCH_DOWN(BuildingClass* this_ptr)
{
    /**
     *  Check if the silo open animation has finished.
     */
    if (this_ptr->Anims[BANIM_SPECIAL_THREE] != nullptr || !Is_Anim_Present(this_ptr, BANIM_SPECIAL_THREE)) {

        /**
         *  If so, play the closing animation.
         */
        this_ptr->Status = DONE_LAUNCH;
    }

    return 1;
}


DEFINE_HOOK(0x00432709, _BuildingClass_Mission_Missile_INITIAL_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    int delay = _BuildingClass_Mission_Missile_INITIAL(this_ptr);
    this_ptr->IsToDisplay = true;
    R->EAX(delay);

    return 0x00432721;
}



DEFINE_HOOK(0x00432729, _BuildingClass_Mission_Missile_DOOR_OPENING_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);
    int delay;
    
    delay = _BuildingClass_Mission_Missile_DOOR_OPENING(this_ptr);
    R->EAX(delay);

    return 0x0043274C;
}


DEFINE_HOOK(0x00432957, _BuildingClass_Mission_Missile_LAUNCH_DOWN_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    int delay = _BuildingClass_Mission_Missile_LAUNCH_DOWN(this_ptr);
    R->EAX(delay);

    return 0x0043296C;
}


/**
 *  Implements `MissileLaunchedVoice` for missile SWs.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x00432937, _BuildingClass_Mission_Missile_LAUNCH_DOWN_Voice_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    SuperWeaponTypeClassExtension* super_ext = Extension::Fetch(SuperWeaponTypes[this_ptr->field_298]);
    if (super_ext->VoxMissileLaunched != VOX_NONE) {
        AudioVoxClass::Speak(super_ext->VoxMissileLaunched);
    }

    return 0x00432943;
}


/**
 *  Should the factory open the roof as opposed to the door?
 *
 *  @author: ZivDero
 */
bool Should_Open_Roof(BuildingClass* building)
{
    if (building->Get_Mission() == MISSION_UNLOAD) {
        TechnoClass* radio = building->Contact_With_Whom();
        if (radio != nullptr && radio->TClass->Locomotor == __uuidof(JumpjetLocomotionClass)) {
            return true;
        }
    }
    return false;
}


static const ShapeSet* Balloon_Roof_Door(BuildingClass* building)
{
    const auto shape = Extension::Fetch(building->Class)->RoofDoorAnim;
    return shape ? shape : building->Class->DoorAnim;
}


static bool Uses_Balloon_Roof(BuildingClass* building)
{
    const auto product = building->Contact_With_Whom();
    return building->Get_Mission() == MISSION_UNLOAD && product && product->RTTI == RTTI_UNIT &&
        NewJumpjetLocomotionClass::Balloon_Hover_Locomotor(static_cast<FootClass*>(product));
}


// Native door drawing already uses the factory Door timer and frame count.
// 8B 85 20 02 00 00: EBP=building, no stack mutation; skip the normal DoorAnim test.
DEFINE_HOOK(0x00427CD8, _BuildingClass_Balloon_Roof_Door_Check, 6)
{
    GET(BuildingClass*, building, EBP);
    if (!Uses_Balloon_Roof(building)) return 0;
    return Balloon_Roof_Door(building) ? 0x00427CEC : 0x00427E27;
}


// 8B 91 C8 07 00 00: replace only the shape pointer; native draw arguments remain intact.
DEFINE_HOOK(0x00427DF5, _BuildingClass_Balloon_Roof_Door_Image, 6)
{
    GET(BuildingClass*, building, EBP);
    if (!Uses_Balloon_Roof(building)) return 0;
    R->EDX(Balloon_Roof_Door(building));
    return 0x00427DFB;
}


// CLOSE, after Is_Door_Closed and before native Enter_Idle_Mode can allow another product.
// 8B 55 00 / BB 01 00 00 00: EBP=building; replay all eight position-independent bytes.
DEFINE_HOOK(0x0043399D, _BuildingClass_Balloon_Roof_Closed, 8)
{
    if (Vinifera_PerformingLoad) return 0;
    GET(BuildingClass*, building, EBP);
    const auto product = building->Contact_With_Whom();
    if (product && product->RTTI == RTTI_UNIT) {
        if (const auto loco = NewJumpjetLocomotionClass::Balloon_Hover_Locomotor(static_cast<FootClass*>(product))) {
            loco->Finish_Balloon_Factory_Exit(building);
        }
    }
    return 0;
}


/**
 *  Patches to make the factory show the roof door opening anim for JJs.
 *
 *  @author: ZivDero
 */
EXPORT_FUNC(_BuildingClass_entry_370_RoofDoorAnim_Patch1)
{
    GET(BuildingClass*, building, EBP);

    const auto btypeext = Extension::Fetch(building->Class);
    if (building->Class->DoorAnim != nullptr && !Should_Open_Roof(building) || btypeext->RoofDoorAnim != nullptr && Should_Open_Roof(building)) {
        return 0x00427CEC;
    }

    return 0x00427E27;
}


EXPORT_FUNC(_BuildingClass_entry_370_RoofDoorAnim_Patch2)
{
    GET(BuildingClass*, building, EBP);

    const BuildingTypeClassExtension* btypeext = Extension::Fetch(building->Class);

    const ShapeSet* shapefile;
    if (Should_Open_Roof(building)) {
        shapefile = btypeext->RoofDoorAnim;
    } else {
        shapefile = building->Class->DoorAnim;
    }

    R->EDX(shapefile);
    return 0x00427DFB;
}


/**
 *  Helper function that handles unlimboing a unit the naval yard has produced.
 *
 *  @author: ZivDero
 */
bool Unlimbo_Naval_Helper(BuildingClass* building, TechnoClass* techno)
{
    if (!building->In_Radio_Contact()) {
        building->Assign_Mission(MISSION_UNLOAD);
    }

    Cell unlimbo_cell = building->Center_Coord().As_Cell();

    /**
     *  If the yard has a rally point set, attempt to place the unit in that direction, next to the naval yard.
     */
    if (building->ArchiveTarget != nullptr) {
        Cell rally = building->ArchiveTarget->Center_Coord().As_Cell();
        DirType direction = Desired_Facing(Point2D(unlimbo_cell.X, unlimbo_cell.Y), Point2D(rally.X, rally.Y));
        FacingType facing = static_cast<FacingType>(direction.Get_Facing<8>());

        while (Map[unlimbo_cell].Cell_Building() == building) {
            unlimbo_cell = Adjacent_Cell(unlimbo_cell, facing);
        }
    }

    /**
     *  If we haven't got a rally point, or the cell we've selected is no good, just pick some cell near the yard that is valid.
     */
    if (building->ArchiveTarget == nullptr || Map[unlimbo_cell].Land_Type() != LAND_WATER || Map[unlimbo_cell].Cell_Techno() != nullptr || !Map.In_Radar(unlimbo_cell)) {
        unlimbo_cell = Map.Nearby_Location(building->Center_Coord().As_Cell(), techno->TClass->Speed);
    }

    /**
     *  Calculate the direction the unit should face - away from the naval yard,
     *  snapped to cardinal directions (looks nicer).
     */
    FacingType facing = static_cast<FacingType>(Desired_Facing(building->Center_Coord(), unlimbo_cell.As_Coord()).Get_Facing<8>());
    Dir256 dir = static_cast<Dir256>(facing * ((DIR_MAX + 1) / FACING_COUNT));

    /**
     *  Unlimbo the unit at that cell.
     */
    if (techno->Unlimbo(Map[unlimbo_cell].Center_Coord(), dir)) {

        /**
         *  If there's a rally point, assign the unit to move there.
         */
        if (building->ArchiveTarget != nullptr) {
            techno->Assign_Destination(building->ArchiveTarget);
            techno->Assign_Mission(MISSION_MOVE);
        }

        /**
         *  Reposition the unit. I'm not exactly sure why this is necessary,
         *  it was copied from YR.
         */
        techno->Mark(MARK_UP);
        techno->PositionCoord = Map[unlimbo_cell].Cell_Coord();
        techno->Mark(MARK_DOWN);

        /**
         *  If this is an AI, give the unit a scatter order so that the AI's ships don't clump at the naval yard.
         */
        if (!techno->House->Is_Human_Player()) {
            techno->Scatter(building->Center_Coord());
        }
        return true;
    }

    return false;
}


/**
 *  This patch handles unlimboing naval yards' production
 *  next to them as opposed to having the units "drive out".
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042CA98, _BuildingClass_Exit_Object_Naval_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);
    GET(TechnoClass*, techno, EDI);

    auto type_ext = Extension::Fetch(this_ptr->Class);
    if (type_ext->IsNaval) {
        if (Unlimbo_Naval_Helper(this_ptr, techno)) {
            return 0x0042D7DF; // return 2 - successfully exited
        } else {
            return 0x0042D966; // return 0 - exit failed
        }
    } else {
        // Stolen call
        techno->Assign_Archive_Target(this_ptr->ArchiveTarget);
        return 0x0042CAA6;
    }
}


/**
 *  This patch is part of adding an extra naval queue for the AI.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042CA35, _BuildingClass_Exit_Object_BuildNavalUnit_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);
    GET(TechnoClass*, techno, EDI);

    if (techno->RTTI == RTTI_UNIT) {
        TechnoTypeClassExtension* ttype_ext = Extension::Fetch(techno->TClass);
        if (ttype_ext->IsNaval) {
            HouseClassExtension* house_ext = Extension::Fetch(this_ptr->House);
            house_ext->BuildNavalUnit = UNIT_NONE;
        } else {
            this_ptr->House->BuildUnit = UNIT_NONE;
        }
    }

    R->EBP(0xFFFFFFFF);
    return 0x0042CA50;
}


/**
 *  Fixes the bug where a building detaches its light when loading a save.
 *
 *  @author: ZivDero
 */
void BuildingClassExt::_Swizzle_Light_Source()
{
    VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(LightSource, "LightSource");
}


/**
 *  Handle an incoming message to the building.
 *
 *  @author: 06/09/1994 JLB - Created.
 *           ZivDero - Adjustments for Tiberian Sun.
 */
RadioMessageType BuildingClassExt::_Receive_Message(RadioClass* from, RadioMessageType message, long& param)
{
    if (!Vinifera_PerformingLoad && Extension::Fetch(Class)->Is_Custom_Foundation_Unit_Repair_Eligible()) {
        const bool entry = message == RADIO_CAN_LOAD || message == RADIO_HELLO ||
            message == RADIO_DOCKING || message == RADIO_IM_IN;
        // Preserve the native CY's construction HELLO, not a second service
        // visitor. Unsupported Aircraft/locomotors never acquire a repair link.
        const bool construction_hello = message == RADIO_HELLO && from && from->RTTI == RTTI_BUILDING &&
            static_cast<BuildingClass*>(from)->Class->IsConstructionYard && (!Radio || Radio == from);
        Coord point;
        if (entry && !construction_hello &&
            (!from || from->RTTI != RTTI_UNIT ||
             !Extension::Fetch(this)->Custom_Unit_Repair_Coord(point, static_cast<UnitClass*>(from)))) return RADIO_NEGATIVE;
        if (message == RADIO_IM_IN && (Radio != from || from->Radio != this)) return RADIO_NEGATIVE;
    }
    if (!Vinifera_PerformingLoad) {
        auto dock_ext = Extension::Fetch(this);
        if (dock_ext != nullptr && dock_ext->Is_Aircraft_MultiDock()) {
            const bool dock_message = message == RADIO_HELLO || message == RADIO_OVER_OUT ||
                message == RADIO_CAN_LOAD || message == RADIO_DOCKING || message == RADIO_MOVE_HERE ||
                message == RADIO_IM_IN || message == RADIO_TETHER || message == RADIO_UNTETHER || message == RADIO_ON_DEPOT;
            const bool construction_contact = from != nullptr && from->RTTI != RTTI_AIRCRAFT &&
                (Radio == from || (from->RTTI == RTTI_BUILDING &&
                    static_cast<BuildingClass*>(from)->Class->IsConstructionYard && !dock_ext->Has_Aircraft_Dock_Visitors()));
            if (dock_message && !construction_contact &&
                ((from != nullptr && from->RTTI == RTTI_AIRCRAFT) || message == RADIO_CAN_LOAD ||
                 message == RADIO_HELLO || message == RADIO_DOCKING || message == RADIO_IM_IN)) {
                return dock_ext->Aircraft_Dock_Receive(from, message, param);
            }
        }
    }
    switch (message) {

        /*
        **  This message is received as a request to attach/load/dock with this building.
        **  Verify that this is allowed and return the appropriate response.
        */
    case RADIO_CAN_LOAD:
        TechnoClass::Receive_Message(from, message, param);
        if (!House->Is_Ally(from)) return RADIO_STATIC;
        if (Mission == MISSION_CONSTRUCTION || Mission == MISSION_DECONSTRUCTION || BState == BSTATE_CONSTRUCTION) return RADIO_NEGATIVE;
        if (!IsOn) return RADIO_NEGATIVE;

        if (Class->IsCanUnitRepair) {
            if (from->RTTI == RTTI_UNIT || from->RTTI == RTTI_AIRCRAFT) {
                if (Transmit_Message(RADIO_ON_DEPOT, from) != RADIO_ROGER) {
                    return RADIO_ROGER;
                }
            }
            return RADIO_NEGATIVE;
        }        

        if ((Class->IsArmory || Class->IsHospital) && from->RTTI == RTTI_INFANTRY) {
            if (Ammo != 0 && Mission != MISSION_REPAIR) {
                return RADIO_ROGER;
            }
            return RADIO_NEGATIVE;
        }
        if (Class->IsHelipad) {

            /*
            **  #FIX: Don't allow aircraft to dock with helipads that aren't listed as their Dock.
            */
            if (from->RTTI == RTTI_AIRCRAFT && static_cast<AircraftClass*>(from)->Class->Dock.Is_Present(Class)) {
                return RADIO_ROGER;
            }
            return RADIO_NEGATIVE;
        }

        /*
        *  Prevents units from requesting to load into the building if it's already in a radio contact with a unit.
        *  This is typically used in order to only allow one unit to set up loading into it.
        *  Originally, it was at the very top of this function to prevent any building from interacting with units while during contact,
        *  however it was moved here in order to allow Helipads, Armories, Hospitals and Service Depots to communicate with
        *  all units without limitation. Only one unit can still be accepted into being loaded at a time.
        */ 
        if (!ScenarioInit && In_Radio_Contact() && Contact_With_Whom() != from) return RADIO_NEGATIVE;

        /**
         *  #issue-129
         *
         *  Fixes a bug where a harvester is able to dock to a refinery that is not
         *  listed in the value of the harvester's Dock= key.
         *
         *  @author: Rampastring
         */
        if (Class->IsDockUnload && from->RTTI == RTTI_UNIT && static_cast<UnitClass*>(from)->Class->IsToHarvest && static_cast<UnitClass*>(from)->Class->Dock.Is_Present(Class) && (ScenarioInit || !Cargo.Is_Something_Attached())) {
            return RADIO_ROGER;
        }

        /**
         *  Replicate the fix above for weeders.
         *
         *  @author: ZivDero
         */
        if (Class->IsWeeder && from->RTTI == RTTI_UNIT && static_cast<UnitClass*>(from)->Class->IsToVeinHarvest && static_cast<UnitClass*>(from)->Class->Dock.Is_Present(Class) && (ScenarioInit || !Cargo.Is_Something_Attached())) {
            return RADIO_ROGER;
        }

        return RADIO_STATIC;

        /*
        **  This message is received when the object has attached itself to this
        **  building.
        */
    case RADIO_IM_IN:
        if (Mission == MISSION_DECONSTRUCTION) {
            return RADIO_NEGATIVE;
        }
        if (Class->IsCanUnitRepair || Class->IsCanUnitReload || Class->IsHospital || Class->IsArmory) {
            Coord repair_point;
            if (Vinifera_DeveloperMode && !Vinifera_PerformingLoad &&
                Radio == from && Extension::Fetch(this)->Custom_Unit_Repair_Contact_Coord(repair_point)) {
                const auto cell = repair_point.As_Cell();
                DEBUG_INFO("[GREPAIR] building={} unit={} event=IM_IN serviceCell=({}, {})\n", ID, from->ID, cell.X, cell.Y);
            }
            if (!Vinifera_PerformingLoad && Vinifera_DeveloperMode && from->RTTI == RTTI_AIRCRAFT) {
                const auto ext = Extension::Fetch(Class);
                if (ext != nullptr && ext->Is_Custom_Foundation_Aircraft_Helipad_Eligible()) {
                    const Coord coord = Docking_Coord();
                    const Cell cell = coord.As_Cell();
                    DEBUG_INFO("[CFPAD] type={} event=SERVICE_ENTER aircraft={} dockCell=({}, {}) coord=({}, {}, {}) cellBuildingMatch={}\n",
                        Class->Name(), from->ID, cell.X, cell.Y, coord.X, coord.Y, coord.Z,
                        Map[coord].Cell_Building() == this);
                }
            }
            IsReadyToCommence = true;
            Assign_Mission(MISSION_REPAIR);
            from->Assign_Mission(MISSION_SLEEP);
            return RADIO_ROGER;
        }
        if (Class->IsDockUnload || Class->IsWeeder) {
            from->Assign_Mission(MISSION_UNLOAD);
            return RADIO_ROGER;
        }
        break;

        /*
        **  Docking maneuver maintenance message. See if new order should be given to the
        **  unit trying to dock.
        */
    case RADIO_DOCKING: {
        TechnoClass::Receive_Message(from, message, param);

        if (!IsOn) {
            return RADIO_NEGATIVE;
        }

        if (Class->IsCanUnitRepair) {
            RadioClass* radio = Contact_With_Whom();
            if (radio != nullptr && radio == from) {
                if (Transmit_Message(RADIO_NEED_REPAIR) == RADIO_NEGATIVE) {
                    return RADIO_NEGATIVE;
                }
            }
        }

        /*
        **  If this building is already in radio contact, then it might
        **  be able to satisfy the request to load by bumping off any
        **  preoccupying task.
        */
        if (Class->IsCanUnitReload) {
            FootClass* radio = static_cast<FootClass*>(Contact_With_Whom());
            if (radio != nullptr && radio != from) {
                if (Transmit_Message(RADIO_ON_DEPOT) == RADIO_ROGER) {
                    if (Transmit_Message(RADIO_ALL_DONE) == RADIO_ROGER) {
                        radio->Assign_Destination(&Map[radio->Nearby_Location(this)]);
                        radio->Assign_Mission(MISSION_MOVE);
                        return RADIO_ROGER;
                    }
                }
                return RADIO_NEGATIVE;
            }
        }

        if (Class->IsHospital || Class->IsArmory) {
            /**
             *  If a unit is asking to go ("dock") into the armory or hospital and they don't have ammo for it, turn it away.
             *  If the armory or hospital has a rally point, tell the unit to move to it instead.
             *  This makes the all units that were ordered to go into it regroup at the rally position.
             */ 
            if (Ammo <= 0) {
                if (ArchiveTarget != nullptr) {
                    auto techno = from->As_Techno();
                    techno->Assign_Mission(MISSION_MOVE);
                    techno->Assign_Destination(ArchiveTarget);
                }
                return RADIO_NEGATIVE;
            }

            if (Contact_With_Whom() != from) {
                if (Transmit_Message(RADIO_NEED_REPAIR) != RADIO_NEGATIVE) {
                    return RADIO_ROGER;
                }
                Transmit_Message(RADIO_RUN_AWAY);
            }
            else {
                param = reinterpret_cast<long>(&Map[Get_Coord()]);
                Transmit_Message(RADIO_MOVE_HERE, param);
            }
            return RADIO_ROGER;
        }

        /*
        **  Establish contact with the object if this building isn't already in contact
        **  with another.
        */
        if (!In_Radio_Contact()) {
            Transmit_Message(RADIO_HELLO, from);
        }

        bool needs_to_move = false;

        if (Contact_With_Whom() != nullptr) {
            if (Class->IsDockUnload || Class->IsWeeder) {
                CellClass* docking_cell = &Map[Docking_Coord()];
                AbstractClass* navcom = static_cast<FootClass*>(Contact_With_Whom())->NavCom;
                if (navcom != nullptr && docking_cell != navcom) {
                    needs_to_move = true;
                }
            }
        }

        if (Contact_With_Whom() != nullptr) {
            if (Class->IsCanUnitRepair) {
                Coord point;
                const int distance = Extension::Fetch(this)->Custom_Unit_Repair_Contact_Coord(point) ?
                    point.Distance_To(Contact_With_Whom()->Center_Coord()) : Distance_To(Contact_With_Whom());
                if (distance > CELL_LEPTON / 2) {
                    needs_to_move = true;
                }
            }
        }

        if (Transmit_Message(RADIO_NEED_TO_MOVE) == RADIO_ROGER || needs_to_move) {
            param = reinterpret_cast<long>(this);
            if (Class->IsDockUnload || Class->IsWeeder) {
                param = reinterpret_cast<long>(&Map[Get_Cell() + Cell(2, 1)]);

                /*
                **  Tell the harvester to move to the docking pad of the building.
                */
                if (Transmit_Message(RADIO_MOVE_HERE, param) == RADIO_YEA_NOW_WHAT) {

                    /*
                    **  Since the harvester is already there, tell it to begin the backup
                    **  procedure now. If it can't, then tell it to get outta here.
                    */
                    Transmit_Message(RADIO_TETHER);
                    if (Transmit_Message(RADIO_BACKUP_NOW, from) != RADIO_ROGER) {
                        from->Scatter(COORD_NONE, true, true);
                    }
                }
            }
            else if (Class->IsHelipad) {
                param = reinterpret_cast<long>(this);
                if (Transmit_Message(RADIO_MOVE_HERE, param) == RADIO_YEA_NOW_WHAT) {
                    Transmit_Message(RADIO_TETHER);
                }
            }
        }
        return RADIO_ROGER;
    }

        /*
        **  If a transport or harvester is requesting permission to head toward, dock
        **  and load/unload, check to make sure that this is allowed given the current
        **  state of the building.
        */
    case RADIO_ARE_REFINERY:
        if (Cargo.Is_Something_Attached() || In_Radio_Contact() || IsInLimbo || House != from->Owner_HouseClass() || (!Class->IsRefinery && !Class->IsCanUnitRepair && !Class->IsWeeder)) {
            return RADIO_NEGATIVE;
        }
        return RADIO_ROGER;

        /*
        **  Someone is telling us that it is starting construction. This should only
        **  occur if this is a construction yard and a building was just placed on
        **  the map.
        */
    case RADIO_BUILDING:
        Assign_Mission(MISSION_REPAIR);
        TechnoClass::Receive_Message(from, message, param);
        return RADIO_ROGER;

        /*
        **  Someone is telling us that they have finished construction. This should
        **  only occur if this is a construction yard and the building that was being
        **  constructed has finished. In this case, stop the construction yard
        **  animation.
        */
    case RADIO_COMPLETE:
        if (Mission != MISSION_DECONSTRUCTION) {
            Assign_Mission(MISSION_GUARD);
            if (Class->IsConstructionYard) {
                End_Anim(BANIM_PRE_PRODUCTION);
                Begin_Anim(BANIM_PRODUCTION, Get_Health_Ratio() <= Rule->ConditionYellow);
            }
        }
        TechnoClass::Receive_Message(from, message, param);
        return RADIO_ROGER;

        /*
        **  This message may occur unexpectedly if the unit in contact with this
        **  building is suddenly destroyed. Handle any cleanup necessary. For example,
        **  a construction yard should stop its construction animation in this case.
        */
    case RADIO_OVER_OUT:
        Begin_Mode(BSTATE_IDLE);
        TechnoClass::Receive_Message(from, message, param);
        return RADIO_ROGER;

        /*
        **  This message is received when an object has completely left
        **  building. Sometimes special cleanup action is required when
        **  this event occurs.
        */
    case RADIO_UNLOADED:
        if (Class->IsCanUnitRepair) {
            if (Distance_To(from) < 0x0180) {
                return RADIO_ROGER;
            }
        }
        TechnoClass::Receive_Message(from, message, param);
        if (Class->IsWeaponsFactory || Class->IsCanUnitRepair) return RADIO_RUN_AWAY;
        return RADIO_ROGER;

    case RADIO_REDRAW:
        if (Class->IsWeaponsFactory) {
            return RADIO_ROGER;
        }
        break;

    default:
        break;
    }

    /*
    **  Pass along the message to the default message handler in the radio itself.
    */
    return TechnoClass::Receive_Message(from, message, param);
}

RadioMessageType BuildingClassExt::_Aircraft_Dock_Transmit(RadioMessageType message, RadioClass* to)
{
    if (Vinifera_PerformingLoad) return RadioClass::Transmit_Message(message, to);
    auto ext = Extension::Fetch(this);
    if (ext == nullptr || !ext->Is_Aircraft_MultiDock()) return RadioClass::Transmit_Message(message, to);
    const bool cy_peer = to != nullptr && to->RTTI == RTTI_BUILDING &&
        static_cast<BuildingClass*>(to)->Class->IsConstructionYard && !ext->Has_Aircraft_Dock_Visitors();
    if (Mission != MISSION_CONSTRUCTION && BState != BSTATE_CONSTRUCTION &&
        !cy_peer && (Radio == nullptr || Radio->RTTI == RTTI_AIRCRAFT)) return ext->Aircraft_Dock_Transmit(message, LParam, to);
    return RadioClass::Transmit_Message(message, to);
}

RadioMessageType BuildingClassExt::_Aircraft_Dock_Transmit_Data(RadioMessageType message, long& param, RadioClass* to)
{
    if (Vinifera_PerformingLoad) return RadioClass::Transmit_Message(message, param, to);
    auto ext = Extension::Fetch(this);
    if (ext == nullptr || !ext->Is_Aircraft_MultiDock()) return RadioClass::Transmit_Message(message, param, to);
    const bool cy_peer = to != nullptr && to->RTTI == RTTI_BUILDING &&
        static_cast<BuildingClass*>(to)->Class->IsConstructionYard && !ext->Has_Aircraft_Dock_Visitors();
    if (Mission != MISSION_CONSTRUCTION && BState != BSTATE_CONSTRUCTION &&
        !cy_peer && (Radio == nullptr || Radio->RTTI == RTTI_AIRCRAFT)) return ext->Aircraft_Dock_Transmit(message, param, to);
    return RadioClass::Transmit_Message(message, param, to);
}


DEFINE_HOOK(0x004381F8, _BuildingClass_Load_SwizzleLightSource_Patch, 0)
{
    GET(BuildingClassExt*, this_ptr, ESI);

    this_ptr->_Swizzle_Light_Source();

    return 0x00438202;
}


/**
 *  Prevents buildings from catching flames
 *  when rapidly switching between yellow and green
 *  damage states.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x0042B6CC, _BuildingClass_Take_Damage_Prevent_Cumulative_Flame_Spawn_Patch, 0)
{
    GET(Coord *, coord, EAX);
    GET(BuildingClass *, this_ptr, ESI);

    /**
     *  Stolen bytes / code.
     */
    Static_Sound(Rule->BlowupSound, *coord);

    /**
     *  Actual functionality of the hack.
     *  Do not spawn flames on the building if flames were spawned
     *  on it too recently.
     */
    BuildingClassExtension* buildingext = Extension::Fetch(this_ptr);
    if (Frame < buildingext->LastFlameSpawnFrame + RuleExtension->BuildingFlameSpawnBlockFrames) {
        goto past_flame_spawn;
    }

    buildingext->LastFlameSpawnFrame = Frame;

    /**
     *  Continue into applying building flames.
     */
original_code:
    R->EBX(0x7FFF);
    return 0x0042B6E4;

    /**
     *  Skip the game's code block for spawning flames on buildings.
     */
past_flame_spawn:
    return 0x0042B684;
}


BuildingClass* Find_Best_Alternative_Factory(BuildingClass* this_ptr, FootClass* exiting_object)
{
    int closest_distance = INT_MAX;
    BuildingClass* closest_match = nullptr;

    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* bldg = Buildings[i];

        if (bldg->House == this_ptr->House && bldg != this_ptr && bldg->Mission == MISSION_GUARD && !bldg->Factory && bldg->Class->ToBuild == this_ptr->Class->ToBuild && bldg->Is_Powered_On()) {
            // Original TS code, left here for reference. Was part of the above condition
            // if (bldg->Class != this_ptr->Class)
            //     continue;

            bool unit_is_naval = Extension::Fetch(exiting_object->TClass)->IsNaval;
            bool building_is_naval = Extension::Fetch(bldg->TClass)->IsNaval;
            if (unit_is_naval != building_is_naval) {
                continue;
            }

            // Check ownable, so only factories of a faction that owns the object can
            // build the object
            if ((bldg->Class->Get_Ownable() & exiting_object->TClass->Get_Ownable()) == 0) {
                continue;
            }

            // All checks have passed. Check the distance to find the closest factory to exit from.
            int distance = this_ptr->Distance_To(bldg);
            if (distance < closest_distance) {
                closest_distance = distance;
                closest_match = bldg;
            }
        }
    }

    return closest_match;
}


/**
 *  Replaces the loop starting from 0x0042CAB9 to improve the alternative
 *  war factory selection logic when a war factory is busy in 3 ways:
 *
 *  1) The object can now exit from factory buildings of a different type
 *     than what the object was originally produced from.
 *
 *  2) The above takes naval into account when finding building to exit from,
 *     preventing ships from exiting from land-based factories and vice-versa.
 *
 *  3) The logic prefers finding the closest rather than the "first" alternative
 *  factory building.
 *
 *  @author: Rampastring
 */
DEFINE_HOOK(0x0042CAB9, _BuildingClass_Exit_Object_Factory_Busy_Customized_Alternate_Factory_Seeking_Logic, 0)
{
    GET(BuildingClass*, this_ptr, ESI);
    GET(FootClass*, exiting_object, EDI);

    BuildingClass* best_alternative_building = Find_Best_Alternative_Factory(this_ptr, exiting_object);
    if (best_alternative_building != nullptr) {

        /**
         *  Exit from the factory we found.
         */
        R->EBP(best_alternative_building);
        return 0x0042CB28;
    }

    /**
     *  We could not find an alternative factory to exit from,
     *  exit the function and return 1.
     */
    return 0x0042CB16;
}


/**
 *  Allows AI to repair Base Nodes by enabling the IsToRepair flag on those buildings.
 *  Only applies in campaign, and only if the AIRepairBaseNodes under [AI] is set to yes/true. 
 *
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x0042A3D1, _BuildingClass_Unlimbo_AI_Repair_Base_Nodes, 5)
{
    GET(BuildingClass*, this_ptr, ESI);

    // Ignore pre-placed buildings
    if (ScenarioInit) {
        return 0;
    }

    if (Session.Type == GAME_NORMAL && !this_ptr->House->Is_Human_Player() && RuleExtension->IsAIRepairBaseNodes) {
        this_ptr->IsToRepair = true;
    }
    
    return 0;
}


/*
*  Patches a portion of BuildingClass::Captured where laser fence connections are updated (or more correctly - removed)
*  At this point, the House of the captured building is NOT updated yet - and belongs to the original owner of this building.
*  This allows us to add an additional check where if a sensor array is captured, the original owner loses that sensor array's sensing capabilities.
* 
*  @author: JoyfulShush
*/
DEFINE_HOOK(0x0042F749, _BuildingClass_Captured_Disable_Sensors, 6)
{
    GET(BuildingClass*, this_ptr, ESI);

    // Conservative D3c1 policy: unlink all visitors before the owner changes.
    Extension::Fetch(this_ptr)->Release_All_Aircraft_Docks("OWNER_CHANGE");

    auto building_type = this_ptr->Class;    

    if (building_type->IsSensorArray) {
        this_ptr->Disable_Sensor_Array();
    }

    return 0;
}


/*
 *  Patches a portion of BuildingClass::Captured where a building that gets captured lets players reveal shroud around it with Look().
 *  At this point, the House of the captured building is already updated - and belongs to the new owner of this building.
 *  This allows us to add an additional check where if a sensor array is captured, the new owner gains that sensor array's sensing capabilities.
 *
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x0042FB9F, _BuildingClass_Captured_Enable_Sensors, 6)
{
    GET(BuildingClass*, this_ptr, ESI);

    auto building_type = this_ptr->Class;

    if (building_type->IsSensorArray) {
        this_ptr->Enable_Sensor_Array();
    }

    return 0;
}


/*
 *  Patches the part of BuildingClass::Repair_AI where a building can no longer be repaired due to a house having insufficient funds.
 *  Typically, it would stop repairs altoghether. However, if the rule for pausing repairs is enabled, then it skips that.
 *
 *  @author: JoyfulShush, Rampastring
 */
DEFINE_HOOK(0x00435A38, _BuildingClass_Repair_AI_Pause_Repairs_Patch, 7)
{
    GET(BuildingClass*, this_ptr, ESI);

    if (this_ptr->House->Is_Human_Player())
    {
        HouseClassExtension* houseext = Extension::Fetch(this_ptr->House);

        if (houseext->IsPauseRepairs) {
            return 0x00435A3F;
        }
    }

    return 0;
}


/**
 *  #issue-177
 *
 *  Patches the check for if a building is a Construction Yard to check the entire BuildConst list.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042AA76, _BuildingClass_Unlimbo_BuildConst_Patch, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    if (Rule->BuildConst.Is_Present(this_ptr->Class)) {
        return 0x0042AA8B;
    }

    return 0x0042AACF;
}


/**
 *  #issue-177
 *
 *  Patches the check for if a building is a Construction Yard to check the entire BuildConst list.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042F958, _BuildingClass_Captured_BuildConst_Patch1, 0)
{
    GET(BuildingTypeClass*, buildingtype, ECX);

    if (Rule->BuildConst.Is_Present(buildingtype)) {
        return 0x0042F968;
    }

    return 0x0042F9A2;
}


/**
 *  #issue-177
 *
 *  Patches the check for if you have a Construction Yard to check the entire BuildConst list.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042FACC, _BuildingClass_Captured_BuildConst_Patch2, 0)
{
    GET(HouseClass*, house, EBX);

    if (house->Count_Owned(Rule->BuildConst)) {
        return 0x0042FAEF;
    }

    return 0x0042FB10;
}


/**
 *  #issue-177
 *
 *  Patches the check for if a building is a Construction Yard to check the entire BuildConst list.
 *
 *  @author: ZivDero
 */
DEFINE_HOOK(0x0042FCA1, _BuildingClass_Captured_BuildConst_Patch3, 0)
{
    GET(BuildingClass*, this_ptr, ESI);

    if (Rule->BuildConst.Is_Present(this_ptr->Class)) {
        return 0x0042FCB6;
    }

    return 0x0042FCF8;
}


/*
 *  Patches BuildingClass::Limbo, inside the IsWall check that turns a wall into an overlay.
 *  Replacing the value of Sight to take into account all sight range modifications for this techno.
 *  Particularly relevant to veterancy granting sight range bonuses.
 *
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x0042A0B0, Building_Class_Unlimbo_Sight_Range_Patch, 0)
{
    GET(Coord*, coord, EBP);
    GET(BuildingClass*, this_ptr, ESI);

    auto building_class_ext = Extension::Fetch(this_ptr);    

    Map.Sight_From(*coord, building_class_ext->Get_Sight_Range(), this_ptr->House);

    return 0x0042A0D7;
}

/*
 *  Typically, whenever an infantry exits a Barracks, an Armory or a Hospital, they are assigned
 *  to move into the exit cell of the building before moving towards the rally point (the ArchiveTarget).
 *  For jumpjets, this causes issues when the rally point is far away, as they will start flying, then take a few seconds to land
 *  on the exit coords before moving to the rally point.
 * 
 *  This patch makes jumpjets skip the exit coord assignment when they are going to be flying to a rally point when they exit the building,
 *  allowing them to go straight to the designated position.
 * 
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x0042D192, Building_Class_Exit_Object_Jumpjet_Exit_Coords_Assignment_Patch, 6)
{
    GET(FootClass*, this_ptr, EDI);

    // If there is no archive target set for the unit, then there was no rally point set.
    if (this_ptr->ArchiveTarget != nullptr && this_ptr->RTTI == RTTI_INFANTRY) {
        auto infantry = static_cast<InfantryClass*>(this_ptr);
        if (infantry->Class->IsJumpJet) {
            bool should_fly = infantry->Should_JumpJet_Fly(infantry->Get_Coord().As_Cell(), infantry->ArchiveTarget->Center_Coord().As_Cell());

            if (should_fly) {
                return 0x0042D1AC;
            }
        }
    }

    return 0;
}


/*
 *  When a unit goes out of a structure such as a Barracks, an Armory or a Hospital, they are in radio contact.
 *  This prevents other units from coming out from this structure until the unit reports that they are officially out of the building,
 *  which is done by cutting off radio contact.
 * 
 *  When the rally point is far away, jumpjet infantry will decide to fly - and when they do, it can take them some time until they reach
 *  the designated position - during which units cannot be produced and are lost.
 *  Note that this is caused even without the "Building_Class_Exit_Object_Jumpjet_Exit_Coords_Assignment_Patch" patch,
 *  as even landing at the exit coords takes some time.
 * 
 *  Instead, this patch causes jumpjet infantry that decide to fly away to the rally point immediately report that they are out of the structure,
 *  without having to wait first to reach the exit coords or the rally points. This allows further production or units processing to resume with no issues.
 *  
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x0042D26B, Building_Class_Exit_Object_Jumpjet_Radio_Contact_Patch, 0)
{
    GET(FootClass*, this_ptr, EDI);

    if (this_ptr->RTTI == RTTI_INFANTRY && this_ptr->ArchiveTarget != nullptr) {
        auto infantry = static_cast<InfantryClass*>(this_ptr);
        if (infantry->Class->IsJumpJet) {
            bool should_fly = infantry->Should_JumpJet_Fly(infantry->Get_Coord().As_Cell(), infantry->ArchiveTarget->Center_Coord().As_Cell());

            if (should_fly) {
                infantry->Transmit_Message(RADIO_OVER_OUT);
            }
        }
    }

    return 0x0042CF07;
}


/*
 *  Reimplements BuildingClass::Can_Enter_Cell, which is used for undeploy logic as well as building unlimbo logic
 *  Improved the undeploy logic to check for the cell being hovered on with the cursor
 *  rather than using building placement logic.
 *  This fixes the cursor showing "NO MOVE" when hovering over tiberium, bridges, or invisible units or structures
 *  Naval buildings checks water passage while making sure the cursor is not on a bridge,
 *  while regular buildings checks land passage as well as bridges.
 *
 *  @author: JoyfulShush, Rampastring
 */
MoveType BuildingClassExt::_Can_Enter_Cell(CellClass const* cell, FacingType dir, int cell_height, CellClass const*, bool) const
{    
    if (Class->UndeploysInto && IsDown && !IsInLimbo) {

        // If the scenario is being initialized, all cells are considered passable.
        if (ScenarioInit) {
            return MOVE_OK;
        }
        
        auto passability = cell->Passability;        
        
        // Buildings that are WaterBound=yes have the SPEED_FLOAT speed type.
        if (Class->Speed == SPEED_FLOAT) {
            if (passability != PASSABLE_WATER || cell->Is_Bridge_Here()) {
                return MOVE_NO;
            }
        } else {
            if (passability != PASSABLE_LAND && !cell->Is_Bridge_Here()) {
                return MOVE_NO;
            }
        }

        return MOVE_OK;
    }

    Cell cell_id = cell->CellID;
    return Class->Legal_Placement(cell_id, House) ? MOVE_OK : MOVE_NO;
}


/**
 *  Patches BuildingClass::Unlimbo right after a building was identified as a ConYard, before being added to the ConYard list.
 *  AI houses that build base nodes need to know the placement center of their base in order to correctly place structures around it.
 *  In a skirmish setting, it is immediately set as the AI deploys its ConYards (in UnitClass::Try_To_Deploy).
 *  However, in campaign, it is only set when the Auto Base Building trigger action is used, which can cause bases to get stuck
 *  trying to build the next node at the ConYard's position, which is registered as the center. (in HouseClass::Where_To_Place_Building).
 *  This makes sure that the AI has this value correctly set during any reason a ConYard is being unlimboed.
 *  Note that this happens after Recalc_Center was already done as part of Unlimbo, so it is already set correctly.
 *
 *  @author: JoyfulShush
 */
DEFINE_HOOK(0x0042AA8B, _BuildingClass_Unlimbo_ConYard_PlacementCenter_Patch, 6)
{
    GET(BuildingClass*, this_ptr, ESI);

    if (this_ptr->House->Base.PlacementCenter == CELL_NONE && !this_ptr->House->Is_Human_Player()) {
        this_ptr->House->Base.PlacementCenter = this_ptr->House->Center.As_Cell();
    }

    return 0;
}


/**
 *  Main function for patching the hooks.
 */
void BuildingClassExtension_Hooks()
{
    /**
     *  Initialises the extended class.
     */
    BuildingClassExtension_Init();

    Patch_Jump(0x00439D10, &BuildingClassExt::_Can_Have_Rally_Point);
    Patch_Jump(0x0042D9A0, &BuildingClassExt::_Update_Buildables);
    Patch_Jump(0x00433FB0, &BuildingClassExt::_Crew_Type);
    Patch_Jump(0x00435DA0, &BuildingClassExt::_How_Many_Survivors);
    //Patch_Jump(0x00429220, &BuildingClassExt::_Shape_Number);
    Patch_Jump(0x0042E53C, 0x0042E56F); // Jump a check for the PurchasePrice of a building for spawning its FreeUnit in Grand_Opening
    Patch_Jump(0x00436410, &BuildingClassExt::_Detach_Anim);
    Patch_Jump(0x004275B0, &BuildingClassExt::_Draw_It);
    //Patch_Jump(0x00427CD8, &_BuildingClass_entry_370_RoofDoorAnim_Patch1);
    //Patch_Jump(0x00427DF5, &_BuildingClass_entry_370_RoofDoorAnim_Patch2);
    Patch_Jump(0x00434000, &BuildingClassExt::_Detach_All);
    Patch_Jump(0x0042F590, &BuildingClassExt::_Toggle_Primary);
    Patch_Jump(0x0042C340, &BuildingClassExt::_Assign_Rally_Point);
    Patch_Jump(0x00434FE0, &BuildingClassExt::_Factory_AI);
    Patch_Jump(0x0042EBD0, static_cast<ActionType(BuildingClassExt::*)(ObjectClass const*, bool)>(&BuildingClassExt::_What_Action));
    Patch_Jump(0x0042EED0, static_cast<ActionType(BuildingClassExt::*)(const Cell&, bool, bool) const>(&BuildingClassExt::_What_Action));
    Patch_Jump(0x0043AF60, &BuildingClassExt::_Fetch_Super_Weapon);
    Patch_Jump(0x0043AFC0, &BuildingClassExt::_Fetch_Super_Weapon2);
    Patch_Jump(0x004268C0, &BuildingClassExt::_Receive_Message);
    // Only BuildingClass's two TX slots; preserve native RadioClass and every legacy type.
    Change_Virtual_Address(0x006CC510, Get_Func_Address(&BuildingClassExt::_Aircraft_Dock_Transmit));
    Change_Virtual_Address(0x006CC514, Get_Func_Address(&BuildingClassExt::_Aircraft_Dock_Transmit_Data));
    Patch_Jump(0x0042FE70, &BuildingClassExt::_Can_Enter_Cell);
    Patch_Jump(0x00428810, &BuildingClassExt::_Draw_Overlays);
}
