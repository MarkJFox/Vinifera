/*******************************************************************************
 *  AF-0009B2a resource analysis results. No engine objects or saved state.
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/
#pragma once

#include "tibsun_defines.h"

#include <vector>

struct AdvancedAIResourceTypeAmount
{
    TiberiumType Type = TIBERIUM_NONE;
    int CellCount = 0;
    long long Amount = 0;
    long long Value = 0;
};

struct AdvancedAIResourceField
{
    int ID = -1; // Lowest native cell index; valid only for this survey.
    int CellCount = 0;
    long long Amount = 0; // Estimated harvest units: sum of OverlayData + 1.
    long long Value = 0;
    std::vector<AdvancedAIResourceTypeAmount> Types;
    Cell Center {0, 0}; // Density-weighted center snapped to a resource cell.
    Cell Target {0, 0}; // Valid if ReachableCells > 0; not a building placement.
    Cell UncoveredTarget {0, 0}; // Reachable unserved cell used for expansion probes.
    int BaseDistance = -1; // Eight-neighbor steps, ignoring obstacles.
    int RefineryDistance = -1; // Center to nearest completed own refinery.
    int NearbyRefineries = 0;
    int CoveredCells = 0;
    int ReachableCells = 0;
    long long ReachableValue = 0;
    long long UncoveredValue = 0;
    int EnemyBaseDistance = -1;
    long long Score = 0;
};

struct AdvancedAIResourceAnalysis
{
    int Frame = -1;
    int SurveyFrame = -1;
    bool HasBase = false;
    Cell Base {0, 0};
    std::vector<AdvancedAIResourceField> Fields;
    int SelectedField = -1; // Field ID, not a vector offset; -1 means no target.
};
