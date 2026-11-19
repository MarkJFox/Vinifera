/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Contains the hooks for the extended AStarClass.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/
#include "astarext_hooks.h"
#include "fatal.h"
#include "debughandler.h"
#include "asserthandler.h"

#include "hooker.h"
#include "hooker_macros.h"
#include "house.h"
#include "rules.h"
#include "rulesext.h"
#include "cell.h"
#include "foot.h"
#include "iomap.h"
#include "technotype.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "tspp.h"

#include <algorithm>
#include <cstdlib>


/**
 *  Aftermath: Vein avoidance for computer controlled units.
 *
 *  AStarClass::Get_Movement_Cost (0x0041A620, symbol from GAME.PDB) only prices
 *  blockages, so paths ran straight through veins and killed units without
 *  ImmuneToVeins. The single call in AStarClass::Find_Path_Regular (0x0041AD79)
 *  is routed through this wrapper, which adds a cost to solid flat vein cells
 *  (the cells CellClass::Trigger_Veins attacks). Veins stay passable, so a unit
 *  still crosses them when no reasonable detour exists.
 */
class AStarClassFake
{
public:
    double _Get_Movement_Cost(CellClass** from, CellClass** to, bool bridge, MoveType move, FootClass* foot);
    double Original_Get_Movement_Cost(CellClass** from, CellClass** to, bool bridge, MoveType move, FootClass* foot);

    PathStruct* _Find_Path(Cell* from, Cell* to, FootClass* foot, FacingType* moves, int max_loops, MZoneType mzone, int avoidance);
    PathStruct* Original_Find_Path(Cell* from, Cell* to, FootClass* foot, FacingType* moves, int max_loops, MZoneType mzone, int avoidance);
    PathStruct* Find_Path_Regular(Cell* from, Cell* to, FootClass* foot, FacingType* moves, int max_loops, bool with_hs);
    void Cleanup();
};

DEFINE_IMPLEMENTATION(double AStarClassFake::Original_Get_Movement_Cost(CellClass**, CellClass**, bool, MoveType, FootClass*), 0x0041A620);
DEFINE_IMPLEMENTATION(PathStruct* AStarClassFake::Original_Find_Path(Cell*, Cell*, FootClass*, FacingType*, int, MZoneType, int), 0x0041D6E0);
DEFINE_IMPLEMENTATION(PathStruct* AStarClassFake::Find_Path_Regular(Cell*, Cell*, FootClass*, FacingType*, int, bool), 0x0041A880);
DEFINE_IMPLEMENTATION(void AStarClassFake::Cleanup(), 0x0041B3A0);


static bool Is_Harmful_Vein_Cell(const CellClass* cell)
{
    static constexpr unsigned char FirstSolidVeinData = 48;

    return cell != nullptr && cell->Overlay == OVERLAY_VEINS && cell->OverlayData >= FirstSolidVeinData && cell->Ramp == RAMP_NONE;
}


static bool Should_Avoid_Veins(const FootClass* foot)
{
    if (foot == nullptr || foot->House == nullptr || foot->House->Is_Human_Player()) return false;
    if (foot->RTTI == RTTI_AIRCRAFT) return false;
    if (foot->TClass->IsImmuneToVeins || foot->Has_Ability(ABILITY_VEIN_PROOF)) return false;
    return true;
}


double AStarClassFake::_Get_Movement_Cost(CellClass** from, CellClass** to, bool bridge, MoveType move, FootClass* foot)
{
    double cost = Original_Get_Movement_Cost(from, to, bridge, move, foot);

    const double penalty = RuleExtension ? RuleExtension->AIVeinPathCost : 0.0;
    if (penalty > 0.0 && !bridge && to != nullptr && Is_Harmful_Vein_Cell(to[0]) && Should_Avoid_Veins(foot)) {
        cost += penalty;
    }

    return cost;
}


/**
 *  Aftermath: How many harmful vein cells does the path cross between its start
 *  and its destination? Both end cells are ignored: the unit stands on the
 *  first and was sent to the last.
 */
static int Path_Vein_Cells(const Cell& from, const PathStruct* path)
{
    if (path == nullptr || path->Command == nullptr) return 0;

    int count = 0;
    Cell cell = from;
    for (int i = 0; i < path->Length && path->Command[i] != FACING_NONE; i++) {
        cell = Adjacent_Cell(cell, path->Command[i]);
        const bool last = i + 1 >= path->Length || path->Command[i + 1] == FACING_NONE;
        if (!last && Is_Harmful_Vein_Cell(&Map[cell])) count++;
    }
    return count;
}


/**
 *  Aftermath: Find_Path first plans a coarse corridor through the map's subzones
 *  (Find_Path_Hierarchical) and then confines the cell search to it. The corridor
 *  knows nothing about veins, so the vein cost alone could only shift a path
 *  inside a corridor that usually runs straight through a vein field.
 *
 *  When the path of a vein avoiding computer unit crosses veins, the cell search
 *  is repeated once without the corridor, with a bounded effort and only for
 *  nearby destinations. Long trips keep the native result; units path again on
 *  their way and get the detour once they are close. If the retry fails, the
 *  native path is used unchanged (it was not touched).
 */
PathStruct* AStarClassFake::_Find_Path(Cell* from, Cell* to, FootClass* foot, FacingType* moves, int max_loops, MZoneType mzone, int avoidance)
{
    static constexpr int MaxDetourDistance = 64;
    static constexpr int DetourSearchLimit = 4000;
    static constexpr int AvoidanceOffset = 0x3C; // AStarClass::Avoidance, written by Find_Path.

    PathStruct* path = Original_Find_Path(from, to, foot, moves, max_loops, mzone, avoidance);

    const double penalty = RuleExtension ? RuleExtension->AIVeinPathCost : 0.0;
    if (path == nullptr || penalty <= 0.0 || from == nullptr || to == nullptr || !Should_Avoid_Veins(foot)) return path;
    if (foot->TClass->IsTrain) return path;

    const int distance = std::max(std::abs(from->X - to->X), std::abs(from->Y - to->Y));
    if (distance > MaxDetourDistance) return path;

    const int crossed = Path_Vein_Cells(*from, path);
    if (crossed <= 0) return path;

    Cleanup();
    *reinterpret_cast<int*>(reinterpret_cast<char*>(this) + AvoidanceOffset) = avoidance;

    const int limit = max_loops > 0 ? std::min(max_loops, DetourSearchLimit) : DetourSearchLimit;
    PathStruct* detour = Find_Path_Regular(from, to, foot, moves, limit, false);
    if (detour == nullptr) {
        DEV_DEBUG_INFO("AF-0009U: vein-detour {} ({},{})->({},{}) veins={} not-found\n",
            foot->TClass->Name(), from->X, from->Y, to->X, to->Y, crossed);
        return path;
    }

    DEV_DEBUG_INFO("AF-0009U: vein-detour {} ({},{})->({},{}) veins={}->{}\n",
        foot->TClass->Name(), from->X, from->Y, to->X, to->Y, crossed, Path_Vein_Cells(*from, detour));
    return detour;
}


/**
 *  Main function for patching the hooks.
 */
void AStarClassExtension_Hooks()
{
    /**
     *  Aftermath: Computer units avoid harmful veins when pathfinding.
     */
    Patch_Call(0x0041AD79, &AStarClassFake::_Get_Movement_Cost);

    /**
     *  Aftermath: Retry vein crossing paths without the hierarchical corridor.
     *  FootClass::Find_Path (call at 0x00498F7F) is the only caller of AStarClass::Find_Path.
     */
    Patch_Call(0x00498F7F, &AStarClassFake::_Find_Path);

    // Patch some pathfinding related arrays to fix pathfinding crashes on very large maps with complex paths

    // Double the size of RegularQueue
    Patch_Byte(0x0041B53F + 3, 0x08);
    Patch_Byte(0x0041B550 + 5, 0x02);

    // Double the size of HierQueue
    Patch_Byte(0x0041B591 + 1, 0x84);  // Allocation size 0x9C44 -> 0x13884 (40,004 -> 80,004 bytes)
    Patch_Byte(0x0041B591 + 2, 0x38);
    Patch_Byte(0x0041B591 + 3, 0x01);
    Patch_Byte(0x0041B5A2 + 3, 0x20);  // Capacity 0x2710 -> 0x4E20 (10,000 -> 20,000)
    Patch_Byte(0x0041B5A2 + 4, 0x4E);

    // Double size of RegularOpenNodePool
    Patch_Byte(0x0041B5D0 + 3, 0x20);
    Patch_Byte(0x0041B5E7 + 3, 0x02);
    Patch_Byte(0x0041B5FA + 4, 0x20);

    // Double the size of ???
    Patch_Byte(0x0041B604 + 3, 0x30);
    Patch_Byte(0x0041B618 + 4, 0x30);
    Patch_Byte(0x0041B625 + 4, 0x30);
    Patch_Byte(0x0041B630 + 4, 0x20);

    // Double the size of HierQueueHeap
    Patch_Byte(0x0041B6D0 + 3, 0x04);
    Patch_Byte(0x0041B6D0 + 2, 0xE2);

    /**
     *  AStarClass::Clear - update sentinel offsets to match doubled buffer sizes.
     */
    Patch_Byte(0x0041B3A9 + 4, 0x30);  // [eax+0x180000] -> [eax+0x300000]
    Patch_Byte(0x0041B3B3 + 4, 0x20);  // [edx+0x100000] -> [edx+0x200000]

    /**
     *  AStarClass::Create_Node - update count/sentinel offsets.
     */
    Patch_Byte(0x0041B256 + 4, 0x20);  // [eax+0x100000] read  -> [eax+0x200000]
    Patch_Byte(0x0041B269 + 4, 0x20);  // [eax+0x100000] write -> [eax+0x200000]
    Patch_Byte(0x0041B272 + 4, 0x30);  // [eax+0x180000] read  -> [eax+0x300000]
    Patch_Byte(0x0041B27C + 4, 0x30);  // [eax+0x180000] write -> [eax+0x300000]

    /**
     *  AStarClass::Find_Path_Hierarchical - fix sign extension of unsigned
     *  16-bit zone/region indices. Values >= 32768 would become negative
     *  when sign-extended, producing wild array indices and crashes.
     *  Change movsx (0F BF) to movzx (0F B7).
     */
    Patch_Byte(0x0041D133, 0xB7);  // movsx -> movzx word at 0x0041D132
    Patch_Byte(0x0041D153, 0xB7);  // movsx -> movzx word at 0x0041D152
}
