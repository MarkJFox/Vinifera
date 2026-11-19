/*******************************************************************************
 *  AF-0009B2: resource survey and native expansion placement.
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 *
 *  DTA comparison: Rampastring/Vinifera, commit
 *  2438beeab932a6224268a253ead07a1dadff6d37,
 *  AdvAI_House_Search_For_Next_Expansion_Point (Rampastring/CCHyper).
 *  The cell-component survey replaces its tree-only search. No DTA expansion
 *  production, placement, saved assignments or random selection is imported.
 ******************************************************************************/

#include "always.h"
#include "houseext.h"
#include "advancedai_resources.h"

#include "building.h"
#include "buildingtype.h"
#include "buildingtypeext.h"
#include "cell.h"
#include "debughandler.h"
#include "factory.h"
#include "infantry.h"
#include "mapext_hooks.h"
#include "mouse.h"
#include "overlaytype.h"
#include "rules.h"
#include "rulesext.h"
#include "tiberium.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "unit.h"
#include "unittype.h"
#include "vinifera_globals.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    /**
     *  AF-0009U: Credits required before a resource expansion refinery starts.
     *  Like DTA, the refinery starts as soon as its cost is covered (previously
     *  twice the cost); the unit start reserve then protects it until it is paid.
     */
    long long Expansion_Refinery_Minimum(long long cost)
    {
        return cost;
    }

    constexpr int AnalysisInterval = 30 * TICKS_PER_SECOND;
    constexpr int TechnologyInterval = 2 * TICKS_PER_SECOND;
    constexpr int RefineryServiceRadius = 12;
    constexpr int EnemyInfluenceRadius = 20;
    constexpr int LoggedFields = 6;

    struct ResourceSample
    {
        int Index;
        long long Value;
    };

    struct SurveyField
    {
        AdvancedAIResourceField Summary;
        std::vector<ResourceSample> Cells;
    };

    // Derived snapshots only. Neither these caches nor their timing drive B1.
    int SurveyFrame = -1;
    int ZoneFrame = -1;
    std::vector<SurveyField> SurveyFields;
    std::vector<int> FieldForCell;
    std::vector<int> ResourceZones;
    std::map<int, AdvancedAIResourceAnalysis> HouseAnalyses;

    constexpr int ExpansionRetryInterval = 2 * AnalysisInterval;
    constexpr int TargetSearchRadius = 4;
    constexpr int LocalStockRadius = 8;
    constexpr int MaximumPlacementAnchors = 1024;
    constexpr int PlacementProbes = 4;

    /**
     *  AF-0009U: Spreading of defenses that are placed without a threat.
     *  A building counts as covered by own defenses within SpreadCoverRadius
     *  cells; defenses within SpreadCrowdRadius cells of a site crowd it.
     */
    constexpr int SpreadCoverRadius = 6;
    constexpr int SpreadCrowdRadius = 4;
    constexpr int SpreadAssetAims = 4;

    int Spread_Asset_Importance(const BuildingTypeClass* type)
    {
        // Not IsBase (BaseNormal): it is set for almost every building.
        if (type->IsConstructionYard || type->IsRefinery) return 2;
        if (type->ToBuild != RTTI_NONE || type->Power > type->Drain || type->IsRadar ||
            Rule->BuildTech.Is_Present(BuildingTypes[type->HeapID])) return 1;
        return 0;
    }
    constexpr int PlacementTargets = 5;
    constexpr int RefineryPlacementOptions = 2;
    constexpr size_t CorridorSeedLimit = 16;
    constexpr size_t CorridorCellLimit = 1536;
    constexpr int CorridorReachLimit = 96;
    constexpr size_t CorridorSiteLimit = 24;
    constexpr size_t FrontierSiteLimit = 12;
    constexpr int FrontierStepLimit = 3;
    constexpr int FrontierWaitInterval = 4 * ExpansionRetryInterval;
    const Cell NoExpansionCell(0, 0);

    struct RefineryFrontier
    {
        Cell Target = NoExpansionCell;
        Cell Site = NoExpansionCell;
        StructType Type = STRUCT_NONE;
        long LastProgress = -1;
        int Steps = 0;
        bool LongRoute = false;
        bool SharedStock = false;
    };

    struct ExpansionPlan
    {
        Cell Target = NoExpansionCell;
        Cell Site = NoExpansionCell;
        Cell FollowupTarget = NoExpansionCell;
        Cell FollowupSite = NoExpansionCell;
        StructType FollowupRefinery = STRUCT_NONE;
        StructType FollowupPower = STRUCT_NONE;
        bool FollowupLongRoute = false;
        bool FollowupCorridor = false;
        RefineryFrontier Frontier;
        Cell DeferredTarget = NoExpansionCell;
        std::map<int, long> DeferredFields;
        long LastProbe = -1;
        long LastStepProbe = -1;
        long LastDevelopment = -1;
        long LastTechnology = -1;
        long DeferredAt = -1;
        long OrderFrame = -1;
        long FollowupProgress = -1;
        long long MinimumStock = 0;
        long long FollowupMinimumFunds = 0;
        long long DevelopmentMinimum = 0;
        StructType Type = STRUCT_NONE;
        StructType NextNativePlacement = STRUCT_NONE;
        StructType DevelopmentOrder = STRUCT_NONE;
        int ProductID = -1;
        int LastPlacementProduct = -1;
        int FailedSearches = 0;
        int DevelopmentProduct = -1;
        int LastDevelopmentExit = -1;
        bool ExtraRefinery = false;
        bool RelaxedStock = false;
        bool SharedStock = false;
        bool AllowLongRoute = false;
        bool CorridorSite = false;
        bool AwaitingService = false;
        bool FollowupSiteVerified = false;
        bool FollowupAdvanced = false;
        bool DevelopmentIsTechnology = false;
        const char* DevelopmentWaitReason = "no-development-candidate";
    };

    std::map<int, ExpansionPlan> ExpansionPlans;

    struct PlacementRequest
    {
        int House = -1;
        StructType Type = STRUCT_NONE;
        Cell Target = NoExpansionCell;
        Cell Site = NoExpansionCell;
        bool Tracked = false;
        bool Searched = false;
        bool Advances = false;
        bool AdvancesRefineryFrontier = false;
        int FailedCorridorField = -1;
        const char* FailedCorridorReason = nullptr;
        bool Creep = false;
    };

    // The existing factory exit calls the placement adapter synchronously.
    PlacementRequest ActivePlacement;

    /**
     *  AF-0009U: DTA-like base creep towards a resource field whose refinery
     *  site cannot be verified (outside the build range, no corridor step).
     *  Ordinary steps (power plants) are placed as close to the field as the
     *  build range allows; once the base cannot get closer, or is close
     *  enough, the refinery is placed as close to the field as possible.
     */
    struct ExpansionCreep
    {
        Cell Target = NoExpansionCell;
        StructType Type = STRUCT_NONE;
        Cell Site = NoExpansionCell;
        bool Refinery = false;
        int Steps = 0;
        long LastProbe = -1;
        bool FullProbe = false;
        const char* Reason = nullptr;
    };
    std::map<int, ExpansionCreep> ExpansionCreeps;

    /**
     *  AF-0009U: Creep steps whose loss is watched. A target where creep
     *  steps were destroyed repeatedly is blocked for a while, so the house
     *  does not keep feeding power plants into an enemy kill zone.
     */
    struct CreepStepRecord
    {
        int BuildingID = -1;
        Cell Target = NoExpansionCell;
        long Frame = 0;
    };
    struct CreepTargetBlock
    {
        Cell Target = NoExpansionCell;
        int Losses = 0;
        long Since = 0;
        long Until = -1;
    };
    constexpr int CreepLossLimit = 2;
    constexpr int CreepBlockRadius = 6;
    constexpr long CreepBlockDuration = 5 * 60 * TICKS_PER_SECOND;
    // No creep to a field with enemy buildings this close, no step this close to one.
    constexpr int CreepEnemyTargetRadius = 12;
    constexpr int CreepEnemySiteRadius = 8;
    std::map<int, std::vector<CreepStepRecord>> CreepSteps;
    std::map<int, std::vector<CreepTargetBlock>> CreepBlocks;
    std::map<int, Cell> CreepRejectedTargets;

    bool Due(long previous, int interval = AnalysisInterval)
    {
        return previous < 0 || Frame < previous ||
            static_cast<long long>(Frame) - previous >= interval;
    }

    CellClass* Playable_Cell(const Cell& cell)
    {
        if (cell.X < 0 || cell.Y < 0 || cell.X >= MAP_CELL_W || cell.Y >= MAP_CELL_H) return nullptr;
        const int index = cell.As_Cell_Number();
        if (index >= Map.Array.Length() || !Map.In_Radar(cell)) return nullptr;
        CellClass* result = Map.Array[index];
        return result && result->CellID == cell ? result : nullptr;
    }

    Cell Cell_From_Index(int index)
    {
        return Cell(static_cast<short>(index % MAP_CELL_W), static_cast<short>(index / MAP_CELL_W));
    }

    int Grid_Distance(const Cell& a, const Cell& b)
    {
        return std::max(std::abs(static_cast<int>(a.X) - b.X), std::abs(static_cast<int>(a.Y) - b.Y));
    }

    bool Resource_At(CellClass* cell, TiberiumType& type)
    {
        // Tiberium_Type_Here also recognizes bare spawners. Count actual overlay only.
        if (!cell || cell->Overlay < 0 || cell->Overlay >= OverlayTypes.Count() ||
            !OverlayTypes[cell->Overlay] || !OverlayTypes[cell->Overlay]->IsTiberium) return false;
        type = cell->Tiberium_Type_Here();
        return type >= TIBERIUM_FIRST && type < Tiberiums.Count() && Tiberiums[type];
    }

    long long Resource_Value(const CellClass* cell, TiberiumType type)
    {
        const long long expected = (static_cast<long long>(cell->OverlayData) + 1) * Tiberiums[type]->CreditValue;
        // Preserve the native valuation without overflowing its int return for extreme INIs.
        if (expected >= 0 && expected <= std::numeric_limits<int>::max()) return cell->Get_Tiberium_Value();
        return std::max(0LL, expected);
    }

    int Ground_Zone(CellClass* cell)
    {
        if (!cell || !Map.CellZones || !Map.Zones[MZONE_NORMAL] ||
            !cell->Is_Clear_To_Move(SPEED_TRACK, true, true, -1, MZONE_NORMAL, cell->Height, true)) return -1;
        const int zone = Map.Get_Cell_Zone(cell->CellID, MZONE_NORMAL, false);
        // Native movement components start at 2; 1 is blocked, 0xFFFF outside.
        return zone > 1 && zone != 0xFFFF ? zone : -1;
    }

    void Rebuild_Survey()
    {
        SurveyFields.clear();
        const int size = std::max(0, std::min(Map.Array.Length(), MAP_CELL_TOTAL));
        FieldForCell.assign(size, -2); // -2 unseen, -1 nonresource, otherwise field offset.
        ResourceZones.assign(size, -1);
        ZoneFrame = -1;
        for (int seed = 0; seed < size; ++seed) {
            if (FieldForCell[seed] != -2) continue;
            TiberiumType seed_type;
            FieldForCell[seed] = -1;
            if (!Resource_At(Playable_Cell(Cell_From_Index(seed)), seed_type)) continue;

            const int field_index = static_cast<int>(SurveyFields.size());
            SurveyField field;
            field.Summary.ID = seed;
            field.Cells.push_back({seed, 0});
            FieldForCell[seed] = field_index;
            std::map<int, AdvancedAIResourceTypeAmount> types;
            long long weighted_x = 0;
            long long weighted_y = 0;

            // Each cell is marked before enqueueing: iterative flood fill, no duplicates.
            for (size_t cursor = 0; cursor < field.Cells.size(); ++cursor) {
                const int index = field.Cells[cursor].Index;
                CellClass* cell = Playable_Cell(Cell_From_Index(index));
                TiberiumType type;
                if (!Resource_At(cell, type)) continue;
                const long long amount = static_cast<int>(cell->OverlayData) + 1;
                const long long value = Resource_Value(cell, type);
                field.Cells[cursor].Value = value;
                auto& subtotal = types[static_cast<int>(type)];
                subtotal.Type = type;
                ++subtotal.CellCount;
                subtotal.Amount += amount;
                subtotal.Value += value;
                ++field.Summary.CellCount;
                field.Summary.Amount += amount;
                field.Summary.Value += value;
                weighted_x += cell->CellID.X * amount;
                weighted_y += cell->CellID.Y * amount;

                for (int facing = FACING_FIRST; facing < FACING_COUNT; ++facing) {
                    const Cell adjacent = ::Adjacent_Cell(cell->CellID, static_cast<FacingType>(facing));
                    CellClass* next = Playable_Cell(adjacent);
                    if (!next) continue;
                    const int next_index = adjacent.As_Cell_Number();
                    if (next_index >= size || FieldForCell[next_index] != -2) continue;
                    FieldForCell[next_index] = -1;
                    TiberiumType next_type;
                    if (Resource_At(next, next_type)) {
                        FieldForCell[next_index] = field_index;
                        field.Cells.push_back({next_index, 0});
                    }
                }
            }

            const Cell center(static_cast<short>((weighted_x + field.Summary.Amount / 2) / field.Summary.Amount),
                static_cast<short>((weighted_y + field.Summary.Amount / 2) / field.Summary.Amount));
            int nearest = std::numeric_limits<int>::max();
            int center_index = seed;
            for (const auto& sample : field.Cells) {
                const int index = sample.Index;
                const int distance = center.Relative_Distance_To(Cell_From_Index(index));
                if (distance < nearest || (distance == nearest && index < center_index)) {
                    nearest = distance;
                    center_index = index;
                }
            }
            field.Summary.Center = Cell_From_Index(center_index);
            for (const auto& entry : types) field.Summary.Types.push_back(entry.second);
            SurveyFields.push_back(std::move(field));
        }
        SurveyFrame = Frame;
    }

    void Refresh_Resource_Zones()
    {
        if (ZoneFrame == Frame) return;
        // Native zone numbers may change after terrain/resource changes. Compare
        // field cells and building access using the same simulation frame's table.
        for (const auto& field : SurveyFields) {
            for (const auto& sample : field.Cells) {
                CellClass* cell = Playable_Cell(Cell_From_Index(sample.Index));
                TiberiumType type;
                ResourceZones[sample.Index] = Resource_At(cell, type) ? Ground_Zone(cell) : -1;
            }
        }
        ZoneFrame = Frame;
    }

    bool Existing_Building(const BuildingClass* building)
    {
        return building && building->Class && building->House && building->IsActive && building->IsDown &&
            !building->IsInLimbo && building->Strength > 0 &&
            building->Mission != MISSION_DECONSTRUCTION && building->MissionQueue != MISSION_DECONSTRUCTION;
    }

    std::set<int> Footprint_Access_Zones(const BuildingTypeClass* type, const Cell& origin)
    {
        std::set<int> result;
        const int width = std::clamp(type->Width(), 1, MAP_CELL_W);
        const int height = std::clamp(type->Height(), 1, MAP_CELL_H);
        // The building's own cell is occupied. Query the native footprint perimeter.
        const auto add = [&result, origin](int x, int y) {
            const Cell cell(static_cast<short>(origin.X + x), static_cast<short>(origin.Y + y));
            const int zone = Ground_Zone(Playable_Cell(cell));
            if (zone > 0) result.insert(zone);
        };
        for (int x = -1; x <= width; ++x) {
            add(x, -1);
            add(x, height);
        }
        for (int y = 0; y < height; ++y) {
            add(-1, y);
            add(width, y);
        }
        return result;
    }

    std::set<int> Building_Access_Zones(const BuildingClass* building)
    {
        return Footprint_Access_Zones(building->Class, building->Get_Cell());
    }

    bool Better_Field(const AdvancedAIResourceField& a, const AdvancedAIResourceField& b)
    {
        if (a.Score != b.Score) return a.Score > b.Score;
        if (a.BaseDistance != b.BaseDistance) return a.BaseDistance < b.BaseDistance;
        return a.ID < b.ID;
    }

    long long Apply_Ratio(long long value, long long numerator, long long denominator)
    {
        // Divide before scaling so extreme INI credit values cannot overflow.
        return (value / denominator) * numerator + (value % denominator) * numerator / denominator;
    }

    bool Operational_Refinery(const BuildingClass* building)
    {
        return Existing_Building(building) && building->Class->IsRefinery && building->IsOn &&
            building->Mission != MISSION_CONSTRUCTION && building->MissionQueue != MISSION_CONSTRUCTION;
    }

    struct ExpansionStock
    {
        struct Sample
        {
            Cell Position;
            long long Value;
            bool Covered;
        };

        Cell Seed = NoExpansionCell;
        int Zone = -1;
        long long Value = 0;
        long long Uncovered = 0;
        int ServingRefineries = 0;
        bool HasRefinery = false;
        bool HasResource = false;
        bool HasLongRouteRefinery = false;
        std::vector<Sample> Cells;
    };

    constexpr int MaximumHarvesterRoute = 2 * RefineryServiceRadius;
    constexpr int HarvesterSearchWidth = 2 * MaximumHarvesterRoute + 1;

    struct RefineryServiceTrace
    {
        const char* Result = "not-run";
        int OutsideRadius = 0;
        int NotReached = 0;
        int OverDetour = 0;
        int FoundSteps = -1;
        long long FoundValue = 0;
        int UsableSteps = -1;
        long long UsableValue = 0;

        auto Status() const
        {
            return std::make_tuple(std::string_view(Result), OutsideRadius > 0, NotReached > 0,
                OverDetour > 0, FoundSteps, UsableSteps);
        }
    };

    struct RefinerySiteTrace
    {
        const char* Native = "not-run";
        const char* Result = "not-run";
        const char* ApproachResult = "not-run";
        const char* FootprintBlocker = "none";
        const char* LongPolicy = "not-run";
        Cell Approach = NoExpansionCell;
        Cell BlockedCell = NoExpansionCell;
        int Field = -1;
        int Zone = -1;
        int Access = -1;
        int Cost = -1;
        bool StepLimitReached = false;
        bool FirstHitOnly = false;
        RefineryServiceTrace Short;
        RefineryServiceTrace Long;

        auto Status() const
        {
            // Stock can fluctuate while the same route and rejection repeat.
            return std::make_tuple(std::string_view(Native), std::string_view(Result),
                std::string_view(ApproachResult), std::string_view(FootprintBlocker), std::string_view(LongPolicy),
                Approach, BlockedCell, Zone, Access, Cost, StepLimitReached, FirstHitOnly, Short.Status(), Long.Status());
        }
    };

    struct RefineryTraceContext
    {
        Cell Target = NoExpansionCell;
        const char* Phase = nullptr;
        const char* Search = "native";
    };

    using RefineryTraceKey = std::tuple<int, std::string_view, std::string_view, int, short, short, int, short, short>;
    // Diagnostic state only. The planner and the route search do not read it.
    std::map<RefineryTraceKey, std::pair<decltype(RefinerySiteTrace{}.Status()), unsigned>> RefineryTraces;

    bool Refinery_Trace_Enabled()
    {
#ifdef NDEBUG
        return Vinifera_DeveloperMode;
#else
        return true;
#endif
    }

    int Survey_Field_ID(const Cell& cell)
    {
        if (cell == NoExpansionCell || cell.X < 0 || cell.X >= MAP_CELL_W || cell.Y < 0 || cell.Y >= MAP_CELL_H) return -1;
        const int index = cell.As_Cell_Number();
        if (index < 0 || index >= static_cast<int>(FieldForCell.size())) return -1;
        const int field = FieldForCell[index];
        return field >= 0 && field < static_cast<int>(SurveyFields.size()) ? SurveyFields[field].Summary.ID : -1;
    }

    void Log_Refinery_Site(const HouseClass* house, const BuildingTypeClass* type, const RefineryTraceContext& context,
        const Cell& site, const RefinerySiteTrace& trace)
    {
        const auto key = std::make_tuple(static_cast<int>(house->HeapID), std::string_view(context.Phase), std::string_view(context.Search),
            trace.Field, context.Target.X, context.Target.Y, static_cast<int>(type->HeapID), site.X, site.Y);
        const auto status = trace.Status();
        auto [entry, inserted] = RefineryTraces.try_emplace(key, status, 0u);
        if (!inserted && entry->second.first == status) {
            ++entry->second.second;
            return;
        }
        const unsigned repeated = entry->second.second;
        entry->second = {status, 0u};
        DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} phase={} search={} field={} target={},{} type={} site={},{} "
            "native={} result={} zone={} clear-perimeter-access={} blocked-footprint={},{} footprint-blocker={} "
            "dock=not-evaluated dock-approach={},{} approach-result={} short-radius={} step-limit={} step-limit-reached={} cost={} long-policy={} "
            "short-result={} short-steps={} short-value={} short-found-steps={} short-found-value={} "
            "short-outside-radius={} short-not-reached={} short-over-detour={} "
            "long-result={} long-steps={} long-value={} long-found-steps={} long-found-value={} "
            "long-outside-radius={} long-not-reached={} value-scope={} repeated-previous={}\n",
            static_cast<int>(house->HeapID), Frame, context.Phase, context.Search, trace.Field < 0 ? "unknown" : std::to_string(trace.Field),
            context.Target.X, context.Target.Y, type->Name(), site.X, site.Y, trace.Native, trace.Result, trace.Zone,
            trace.Access, trace.BlockedCell.X, trace.BlockedCell.Y, trace.FootprintBlocker,
            trace.Approach.X, trace.Approach.Y, trace.ApproachResult, RefineryServiceRadius, MaximumHarvesterRoute,
            trace.StepLimitReached, trace.Cost, trace.LongPolicy,
            trace.Short.Result, trace.Short.UsableSteps, trace.Short.UsableValue, trace.Short.FoundSteps, trace.Short.FoundValue,
            trace.Short.OutsideRadius, trace.Short.NotReached, trace.Short.OverDetour,
            trace.Long.Result, trace.Long.UsableSteps, trace.Long.UsableValue, trace.Long.FoundSteps, trace.Long.FoundValue,
            trace.Long.OutsideRadius, trace.Long.NotReached, trace.FirstHitOnly ? "first-reachable-cell" : "field-sample",
            repeated);
    }

    struct HarvesterRoutes
    {
        Cell Approach = NoExpansionCell;
        std::array<signed char, HarvesterSearchWidth * HarvesterSearchWidth> Steps;

        int Distance_To(const Cell& cell, bool allow_long_route = false) const
        {
            const int dx = std::abs(cell.X - Approach.X);
            const int dy = std::abs(cell.Y - Approach.Y);
            if (Approach == NoExpansionCell ||
                std::max(dx, dy) > (allow_long_route ? MaximumHarvesterRoute : RefineryServiceRadius)) return -1;
            const int steps = Steps[Index(cell)];
            return steps >= 0 && (allow_long_route || steps <= dx + dy + 4) ? steps : -1;
        }

        int Index(const Cell& cell) const
        {
            const int x = cell.X - Approach.X + MaximumHarvesterRoute;
            const int y = cell.Y - Approach.Y + MaximumHarvesterRoute;
            return y * HarvesterSearchWidth + x;
        }
    };

    HarvesterRoutes Find_Harvester_Routes(const HouseClass* house, const BuildingTypeClass* refinery, const Cell& site,
        const std::set<int>* extra_occupied = nullptr, RefinerySiteTrace* trace = nullptr);

    Rect Base_Footprint_Rect(const HouseClass* house)
    {
        if (house->Base.InnerCells.Count() <= 0 || house->Base.InnerCells.Count() > MaximumPlacementAnchors) {
            return house->Base.BaseAreaRect;
        }
        int left = MAP_CELL_W, top = MAP_CELL_H, right = 0, bottom = 0;
        for (const Cell& anchor : house->Base.InnerCells) {
            left = std::min<int>(left, anchor.X);
            top = std::min<int>(top, anchor.Y);
            right = std::max<int>(right, anchor.X);
            bottom = std::max<int>(bottom, anchor.Y);
        }
        return Rect(left, top, right - left + 1, bottom - top + 1);
    }

    int Distance_To_Rect(const Cell& cell, const Rect& area)
    {
        const int dx = std::max({area.X - cell.X, cell.X - (area.X + area.Width - 1), 0});
        const int dy = std::max({area.Y - cell.Y, cell.Y - (area.Y + area.Height - 1), 0});
        return std::max(dx, dy);
    }

    ExpansionStock Read_Expansion_Stock(const HouseClass* house, const Cell& target, bool check_long_routes = false)
    {
        ExpansionStock stock;
        std::set<int> access;
        std::vector<const BuildingClass*> refineries;
        for (const BuildingClass* building : Buildings) {
            if (!Existing_Building(building) || building->House != house || !Playable_Cell(building->Get_Cell())) continue;
            auto zones = Building_Access_Zones(building);
            access.insert(zones.begin(), zones.end());
            if (Operational_Refinery(building)) refineries.push_back(building);
        }
        stock.HasRefinery = !refineries.empty();

        int nearest = std::numeric_limits<int>::max();
        for (int y = -TargetSearchRadius; y <= TargetSearchRadius; ++y) {
            for (int x = -TargetSearchRadius; x <= TargetSearchRadius; ++x) {
                const int distance = x * x + y * y;
                if (distance >= nearest) continue;
                const Cell cell(static_cast<short>(target.X + x), static_cast<short>(target.Y + y));
                CellClass* map_cell = Playable_Cell(cell);
                TiberiumType type;
                if (!Resource_At(map_cell, type)) continue;
                stock.HasResource = true;
                const int zone = Ground_Zone(map_cell);
                if (access.count(zone)) {
                    nearest = distance;
                    stock.Seed = cell;
                    stock.Zone = zone;
                }
            }
        }
        if (stock.Zone < 0) return stock;

        // Sample the near side of this connected field when the survey target
        // is deep inside it. A nearby disconnected patch is a different target.
        const Rect base_area = Base_Footprint_Rect(house);
        const Cell original_seed = stock.Seed;
        const int seed_index = stock.Seed.As_Cell_Number();
        if (base_area.Is_Valid() && Distance_To_Rect(stock.Seed, base_area) > RefineryServiceRadius &&
            seed_index < static_cast<int>(FieldForCell.size()) && FieldForCell[seed_index] >= 0) {
            constexpr int frontage_search = LocalStockRadius + RefineryServiceRadius;
            const int field = FieldForCell[seed_index];
            auto best = std::make_tuple(Distance_To_Rect(stock.Seed, base_area), Grid_Distance(stock.Seed, target), seed_index);
            for (int y = -frontage_search; y <= frontage_search; ++y) {
                for (int x = -frontage_search; x <= frontage_search; ++x) {
                    const Cell cell(static_cast<short>(target.X + x), static_cast<short>(target.Y + y));
                    auto* map_cell = Playable_Cell(cell);
                    if (!map_cell) continue;
                    const int index = cell.As_Cell_Number();
                    if (index >= static_cast<int>(FieldForCell.size()) || FieldForCell[index] != field) continue;
                    const auto rank = std::make_tuple(Distance_To_Rect(cell, base_area), Grid_Distance(cell, target), index);
                    TiberiumType type;
                    if (rank >= best || !Resource_At(map_cell, type) || Ground_Zone(map_cell) != stock.Zone) continue;
                    stock.Seed = cell;
                    best = rank;
                }
            }
        }

        // Re-read one bounded connected part of the field, independent of survey IDs.
        const int stock_radius = stock.Seed == original_seed ? LocalStockRadius : RefineryServiceRadius;
        std::vector<Cell> cells{stock.Seed};
        std::set<int> visited{stock.Seed.As_Cell_Number()};
        for (size_t i = 0; i < cells.size(); ++i) {
            const Cell cell = cells[i];
            TiberiumType type;
            CellClass* map_cell = Playable_Cell(cell);
            if (!Resource_At(map_cell, type)) continue;
            const long long value = Resource_Value(map_cell, type);
            stock.Value += value;
            stock.Cells.push_back({cell, value, false});

            for (int facing = FACING_FIRST; facing < FACING_COUNT; ++facing) {
                const Cell adjacent = ::Adjacent_Cell(cell, static_cast<FacingType>(facing));
                if (Grid_Distance(adjacent, stock.Seed) > stock_radius) continue;
                CellClass* next = Playable_Cell(adjacent);
                if (!next || !visited.insert(adjacent.As_Cell_Number()).second) continue;
                TiberiumType next_type;
                if (Resource_At(next, next_type) && Ground_Zone(next) == stock.Zone) cells.push_back(adjacent);
            }
        }
        stock.Uncovered = stock.Value;
        for (const BuildingClass* refinery : refineries) {
            const int distance = Grid_Distance(refinery->Get_Cell(), stock.Seed);
            const bool within_short_range = distance <= RefineryServiceRadius + stock_radius + std::max(refinery->Class->Height(), 2);
            if (!within_short_range && (!check_long_routes ||
                distance > MaximumHarvesterRoute + stock_radius + std::max(refinery->Class->Height(), 2))) continue;
            const auto routes = Find_Harvester_Routes(house, refinery->Class, refinery->Get_Cell());
            bool serves_stock = false;
            for (auto& sample : stock.Cells) {
                if (check_long_routes && !stock.HasLongRouteRefinery && sample.Value > 0 &&
                    routes.Distance_To(sample.Position, true) >= 0) stock.HasLongRouteRefinery = true;
                if (!within_short_range || routes.Distance_To(sample.Position) < 0) continue;
                serves_stock = true;
                if (sample.Covered) continue;
                sample.Covered = true;
                stock.Uncovered -= sample.Value;
            }
            stock.ServingRefineries += serves_stock;
        }
        return stock;
    }

    long long Refinery_Service_Value(const ExpansionStock& stock, const HarvesterRoutes& routes, bool uncovered_only,
        int& nearest, bool allow_long_route = false, RefineryServiceTrace* trace = nullptr)
    {
        long long result = 0;
        for (const auto& sample : stock.Cells) {
            if ((uncovered_only && sample.Covered) || sample.Value <= 0) continue;
            const int steps = routes.Distance_To(sample.Position, allow_long_route);
            if (trace && routes.Approach != NoExpansionCell) {
                const int radius = Grid_Distance(routes.Approach, sample.Position);
                if (radius > (allow_long_route ? MaximumHarvesterRoute : RefineryServiceRadius)) ++trace->OutsideRadius;
                if (radius <= MaximumHarvesterRoute) {
                    const int found = routes.Steps[routes.Index(sample.Position)];
                    if (found >= 0) {
                        if (trace->FoundSteps < 0 || found < trace->FoundSteps) trace->FoundSteps = found;
                        trace->FoundValue += sample.Value;
                        if (steps < 0 && radius <= RefineryServiceRadius && !allow_long_route) ++trace->OverDetour;
                    } else if (radius <= (allow_long_route ? MaximumHarvesterRoute : RefineryServiceRadius)) {
                        ++trace->NotReached;
                    }
                }
                if (steps >= 0 && (trace->UsableSteps < 0 || steps < trace->UsableSteps)) trace->UsableSteps = steps;
            }
            if (steps < 0) continue;
            nearest = std::min(nearest, steps);
            result += sample.Value;
        }
        if (trace) trace->UsableValue = result;
        return result;
    }

    const char* Refinery_Route_Result(const RefinerySiteTrace& site, const RefineryServiceTrace& service)
    {
        if (site.Approach == NoExpansionCell || std::string_view(site.ApproachResult) != "clear") return site.ApproachResult;
        if (service.UsableSteps >= 0) return "reachable";
        if (service.NotReached) return "not-determined-within-bounded-search";
        if (service.OverDetour && !service.OutsideRadius) return "exceeds-short-detour-limit";
        if (service.OutsideRadius && !service.OverDetour) return "outside-service-radius";
        if (service.OverDetour) return "outside-service-radius-or-exceeds-short-detour-limit";
        return "no-positive-eligible-field-samples";
    }

    const char* Refinery_Stock_Rejection(long long served, int cost, bool shared)
    {
        if (shared) return "reachable-share-below-refinery-cost";
        return served < cost ? "reachable-stock-below-refinery-cost" : "reachable-stock-below-required-field-coverage";
    }

    int Distance_To_Footprint(const Cell& target, const Cell& site, const BuildingTypeClass* type)
    {
        const int x = std::clamp<int>(target.X, site.X, site.X + std::clamp(type->Width(), 1, MAP_CELL_W) - 1);
        const int y = std::clamp<int>(target.Y, site.Y, site.Y + std::clamp(type->Height(), 1, MAP_CELL_H) - 1);
        return Grid_Distance(target, Cell(static_cast<short>(x), static_cast<short>(y)));
    }

    enum class PlacementRole { None, Refinery, Power, Infantry, Vehicles, Aircraft, Infrastructure };

    bool Defensive_Building(const BuildingTypeClass* type)
    {
        return type->IsBaseDefense || type->IsWall || type->IsGate || type == Rule->WallTower ||
            std::find(Rule->BuildDefense.begin(), Rule->BuildDefense.end(), type) != Rule->BuildDefense.end() ||
            std::find(Rule->BuildPDefense.begin(), Rule->BuildPDefense.end(), type) != Rule->BuildPDefense.end() ||
            std::find(Rule->BuildAA.begin(), Rule->BuildAA.end(), type) != Rule->BuildAA.end();
    }

    PlacementRole Resource_Placement_Role(const BuildingTypeClass* type)
    {
        if (!type || Defensive_Building(type) || !type->PowersUpBuilding.empty() || type->Speed == SPEED_FLOAT ||
            Extension::Fetch(type)->IsNaval) return PlacementRole::None;
        if (type->IsRefinery) return PlacementRole::Refinery;
        if (!type->IsBase) return PlacementRole::None;
        if (type->ToBuild == RTTI_INFANTRYTYPE) return PlacementRole::Infantry;
        if (type->ToBuild == RTTI_UNITTYPE) return PlacementRole::Vehicles;
        if (type->ToBuild == RTTI_AIRCRAFTTYPE || type->IsHelipad) return PlacementRole::Aircraft;
        if (type->Power > type->Drain) return PlacementRole::Power;
        return type->IsConstructionYard || type->IsCloakGenerator ? PlacementRole::None : PlacementRole::Infrastructure;
    }

    const char* Stock_Blocker(const ExpansionStock& stock, bool relaxed = false)
    {
        if (!stock.HasResource) return "field-depleted";
        if (stock.Zone < 0) return "field-unreachable";
        if (stock.Value <= 0) return "insufficient-resource-value";
        if (!relaxed && stock.Uncovered * 2 <= stock.Value) return "field-already-served";
        return nullptr;
    }

    long long Expansion_Stock_Value(const ExpansionStock& stock, bool shared)
    {
        return shared ? stock.Value / (1 + stock.ServingRefineries) : stock.Uncovered;
    }

    bool Serves_Expansion_Stock(const ExpansionStock& stock, long long served, int cost, bool relaxed, bool shared)
    {
        if (shared) return served / (1 + stock.ServingRefineries) >= cost;
        return served >= cost && (relaxed || (served * 2 >= stock.Uncovered &&
            (stock.Value - stock.Uncovered + served) * 2 >= stock.Value));
    }

    Cell Expansion_Field_Target(const AdvancedAIResourceField& field, bool relaxed)
    {
        return relaxed && field.UncoveredTarget == NoExpansionCell ? field.Target : field.UncoveredTarget;
    }

    using RankedTargets = std::array<const AdvancedAIResourceField*, PlacementTargets>;

    bool Expansion_Field_Deferred(const ExpansionPlan& plan, int field)
    {
        const auto deferred = plan.DeferredFields.find(field);
        return deferred != plan.DeferredFields.end() && !Due(deferred->second, ExpansionRetryInterval);
    }

    RankedTargets Rank_Expansion_Targets(const HouseClass* house, const AdvancedAIResourceAnalysis& analysis,
        const ExpansionPlan& plan, long long minimum_stock, bool relaxed, bool infrastructure, const char** outcome = nullptr)
    {
        RankedTargets targets{};
        const Rect area = Base_Footprint_Rect(house);
        const auto distance = [&](const AdvancedAIResourceField& field) {
            return area.Is_Valid() ? Distance_To_Rect(Expansion_Field_Target(field, relaxed), area) : std::max(0, field.BaseDistance);
        };
        const auto better = [&](const AdvancedAIResourceField& a, const AdvancedAIResourceField& b) {
            const int da = distance(a);
            const int db = distance(b);
            // The base rectangle is a ranking hint. The native finder proves legal reach.
            const bool near_a = da <= RefineryServiceRadius;
            const bool near_b = db <= RefineryServiceRadius;
            if (near_a != near_b) return near_a;
            if ((a.NearbyRefineries == 0) != (b.NearbyRefineries == 0)) return a.NearbyRefineries == 0;
            const long long sa = a.Score / (1 + da / RefineryServiceRadius);
            const long long sb = b.Score / (1 + db / RefineryServiceRadius);
            if (sa != sb) return sa > sb;
            if (da != db) return da < db;
            return Better_Field(a, b);
        };
        bool reachable = false;
        bool uncovered = false;
        bool enough_stock = false;
        const AdvancedAIResourceField* shared_fallback = nullptr;
        for (const auto& field : analysis.Fields) {
            reachable |= field.ReachableCells > 0;
            const Cell target = Expansion_Field_Target(field, relaxed);
            if (field.Score <= 0 || target == NoExpansionCell || (!relaxed && field.UncoveredValue <= 0)) continue;
            uncovered = true;
            // Keep small remnants from filling the shortlist ahead of the live site checks.
            if (field.ReachableValue < minimum_stock) continue;
            enough_stock = true;
            if (!infrastructure && Expansion_Field_Deferred(plan, field.ID)) continue;
            if (!Due(plan.DeferredAt, ExpansionRetryInterval)) {
                const Cell& deferred = plan.DeferredTarget;
                const int index = deferred.As_Cell_Number();
                const int field_index = index >= 0 && index < static_cast<int>(FieldForCell.size()) ? FieldForCell[index] : -1;
                if (target == deferred ||
                    (field_index >= 0 && field_index < static_cast<int>(SurveyFields.size()) &&
                        SurveyFields[field_index].Summary.ID == field.ID &&
                        Grid_Distance(deferred, target) <= LocalStockRadius)) continue;
            }
            if (relaxed && field.NearbyRefineries > 0 && (!shared_fallback || better(field, *shared_fallback))) {
                shared_fallback = &field;
            }
            const auto* candidate = &field;
            for (auto& slot : targets) {
                if (!slot || better(*candidate, *slot)) std::swap(candidate, slot);
                if (!candidate) break;
            }
        }
        if (shared_fallback && std::find(targets.begin(), targets.end(), shared_fallback) == targets.end()) {
            targets.back() = shared_fallback;
        }
        if (outcome && !targets.front()) {
            *outcome = analysis.Fields.empty() ? "no-resource-fields" : !reachable ? "fields-unreachable" :
                !uncovered ? "no-uncovered-field-value" : !enough_stock ? "insufficient-resource-value" : "targets-on-cooldown";
        }
        return targets;
    }

    int __fastcall Weight_Expansion_Anchor(int, Cell& cell, int index, int target_index)
    {
        // Native ranks ascending. The anchor limit keeps both the weight and tie break in int range.
        const int distance = cell.Relative_Distance_To(Cell_From_Index(target_index));
        return distance * (MaximumPlacementAnchors + 1) + index;
    }

    bool Unit_Factory_Exit_Offset(const BuildingTypeClass* type, Cell& offset)
    {
        const auto* ext = Extension::Fetch(type);
        if (!type->IsWeaponsFactory || type->ToBuild != RTTI_UNITTYPE || ext->IsNaval || type->Speed == SPEED_FLOAT) return false;
        if (ext->Has_Custom_Unit_Factory_Exit()) {
            offset = ext->CustomUnitFactoryExitCell;
            return true;
        }
        if (!type->ExitList || (ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid)) return false;
        // The native weapons-factory unload and the existing Hover fix use slot 8.
        offset = type->ExitList[8];
        return offset != REFRESH_EOL;
    }

    CellClass* Clear_Access_Cell(int x, int y, bool ignore_units = false)
    {
        if (x < 0 || y < 0 || x >= MAP_CELL_W || y >= MAP_CELL_H) return nullptr;
        CellClass* cell = Playable_Cell(Cell(static_cast<short>(x), static_cast<short>(y)));
        if (!cell || cell->IsUnderBridge || cell->Cell_Building() ||
            !cell->Is_Clear_To_Move(SPEED_TRACK, ignore_units, ignore_units, -1, MZONE_NORMAL, cell->Height, true)) return nullptr;
        return cell;
    }

    int Clear_Access_Count(const Cell& site, int width, int height, int zone)
    {
        int count = 0;
        const auto check = [&](int x, int y) {
            if (Ground_Zone(Clear_Access_Cell(site.X + x, site.Y + y)) == zone) ++count;
        };
        for (int x = -1; x <= width; ++x) {
            check(x, -1);
            check(x, height);
        }
        for (int y = 0; y < height; ++y) {
            check(-1, y);
            check(width, y);
        }
        return count;
    }

    std::span<const Cell> Occupied_Foundation(const BuildingTypeClass* type)
    {
        const auto* ext = Extension::Fetch(type);
        const bool custom_mask = !Vinifera_PerformingLoad && ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid;
        const int capacity = custom_mask ? BuildingTypeClassExtension::CustomFoundationStorageSize
                                        : static_cast<int>(std::size(BuildingOccupyLists[0]));
        // Occupy_List(false) supplies the actual occupied cells for both foundation kinds.
        const Cell* mask = type->Occupy_List(false);
        if (!mask) return {};
        int count = 0;
        while (count < capacity && mask[count] != REFRESH_EOL) ++count;
        if (count == capacity) return {};
        return {mask, static_cast<size_t>(count)};
    }

    const UnitTypeClass* Expansion_Harvester(const HouseClass* house, const BuildingTypeClass* refinery)
    {
        const auto accepts = [refinery](const UnitTypeClass* type) {
            return type && type->IsToHarvest &&
                std::find(type->Dock.begin(), type->Dock.end(), refinery) != type->Dock.end();
        };
        const UnitTypeClass* harvester = nullptr;
        for (const UnitClass* unit : Units) {
            if (unit && unit->House == house && unit->IsActive && !unit->IsInLimbo && unit->Strength > 0 && accepts(unit->Class) &&
                (!harvester || unit->Class->HeapID < harvester->HeapID)) {
                harvester = unit->Class;
            }
        }
        if (!harvester && accepts(refinery->FreeUnit)) harvester = refinery->FreeUnit;
        return harvester;
    }

    Cell Refinery_Approach_Offset(int width, int height)
    {
        // Begin outside the foundation, aligned with the native unload column.
        return Cell(static_cast<short>(std::min(2, width - 1)), static_cast<short>(height));
    }

    const char* Harvester_Ground_Blocker(CellClass* cell)
    {
        if (!cell) return "outside-playable-map";
        if (cell->IsUnderBridge) return "bridge-cell-excluded";
        if (cell->Cell_Building()) return "building-blocks-approach";
        return nullptr;
    }

    const char* Harvester_Passability_Blocker(const UnitTypeClass* harvester, CellClass* cell)
    {
        if (cell->Overlay >= OVERLAY_FIRST && cell->Overlay < OverlayTypes.Count()) {
            const auto* overlay = OverlayTypes[cell->Overlay];
            if (overlay && overlay->IsWall) return "wall-blocks-approach";
        }
        return cell->Is_Clear_To_Move(harvester->Speed, true, true, -1, harvester->MZone, cell->Height, true)
            ? nullptr : "native-passability-rejected";
    }

    bool Harvester_Step_Allowed(const CellClass* from, const CellClass* to)
    {
        const int rise = to->Height - from->Height;
        return std::abs(rise) <= 1 && (rise <= 0 || from->Ramp) && (rise >= 0 || to->Ramp);
    }

    HarvesterRoutes Find_Harvester_Routes(const HouseClass* house, const BuildingTypeClass* refinery, const Cell& site,
        const std::set<int>* extra_occupied, RefinerySiteTrace* trace)
    {
        HarvesterRoutes routes;
        routes.Steps.fill(-1);
        const auto* harvester = Expansion_Harvester(house, refinery);
        const int width = refinery->Width();
        const int height = refinery->Height();
        const auto foundation = Occupied_Foundation(refinery);
        if (!harvester || width <= 0 || height <= 0 || foundation.empty()) {
            if (trace) trace->ApproachResult = !harvester ? "no-compatible-harvester" : "invalid-refinery-foundation";
            return routes;
        }

        const Cell approach = site + Refinery_Approach_Offset(width, height);
        const auto clear = [&](const Cell& position, CellClass* cell, const char** reason = nullptr) {
            const auto blocked = [reason](const char* cause) {
                if (reason) *reason = cause;
                return false;
            };
            if (const char* cause = Harvester_Ground_Blocker(cell)) return blocked(cause);
            if (extra_occupied && extra_occupied->count(position.As_Cell_Number())) return blocked("planned-footprint-blocks-approach");
            const Cell offset(static_cast<short>(position.X - site.X), static_cast<short>(position.Y - site.Y));
            if (offset.X >= 0 && offset.X < width && offset.Y >= 0 && offset.Y < height &&
                std::find(foundation.begin(), foundation.end(), offset) != foundation.end()) return blocked("refinery-footprint");
            if (const char* cause = Harvester_Passability_Blocker(harvester, cell)) return blocked(cause);
            return true;
        };
        CellClass* entrance = Playable_Cell(approach);
        if (trace) trace->Approach = approach;
        if (!clear(approach, entrance, trace ? &trace->ApproachResult : nullptr)) return routes;
        if (trace) trace->ApproachResult = "clear";
        routes.Approach = approach;
        routes.Steps[routes.Index(approach)] = 0;

        std::vector<Cell> pending{approach};
        constexpr FacingType facings[]{FACING_N, FACING_E, FACING_S, FACING_W};
        for (size_t i = 0; i < pending.size(); ++i) {
            const Cell position = pending[i];
            const int steps = routes.Steps[routes.Index(position)];
            if (steps == MaximumHarvesterRoute) {
                if (trace) trace->StepLimitReached = true;
                continue;
            }
            const CellClass* from = Playable_Cell(position);
            for (const auto facing : facings) {
                const Cell next = ::Adjacent_Cell(position, facing);
                if (Grid_Distance(next, approach) > MaximumHarvesterRoute) continue;
                auto& distance = routes.Steps[routes.Index(next)];
                if (distance >= 0) continue;
                CellClass* cell = Playable_Cell(next);
                if (!clear(next, cell)) continue;
                if (!Harvester_Step_Allowed(from, cell)) continue;
                distance = static_cast<signed char>(steps + 1);
                pending.push_back(next);
            }
        }
        return routes;
    }

    bool Vehicle_Exit_Clear(const BuildingTypeClass* type, const Cell& site, int zone, std::span<const Cell> mask)
    {
        if (type->ToBuild != RTTI_UNITTYPE || !type->IsWeaponsFactory) return true;
        Cell offset;
        if (!Unit_Factory_Exit_Offset(type, offset)) return false;
        const bool custom_exit = Extension::Fetch(type)->Has_Custom_Unit_Factory_Exit();
        const int outside = custom_exit ? offset.X + 1 : std::max(offset.X + 1, type->Width());
        const int level = Playable_Cell(site)->Height;
        bool reaches_outside = false;
        for (int x = offset.X - 3; x <= outside; ++x) {
            const bool inside = std::any_of(mask.begin(), mask.end(), [x, &offset](const Cell& cell) {
                return cell.X == x && cell.Y == offset.Y;
            });
            // The native 4x3 exit list can end its track inside its own mask.
            // Check the way out of the building, including the first exterior cell.
            if (inside && (x < offset.X || !custom_exit)) continue;
            CellClass* cell = Clear_Access_Cell(site.X + x, site.Y + offset.Y, true);
            if (inside || !cell || cell->Height != level || Ground_Zone(cell) != zone) return false;
            reaches_outside |= x >= offset.X;
        }
        return reaches_outside;
    }

    enum class RefinerySiteSearch { Native, Corridor, CorridorPlan, Exact, Frontier };

    struct SiteSearch
    {
        Cell Site = NoExpansionCell;
        Cell FrontierSite = NoExpansionCell;
        int Probes = 0;
        int Checked = 0;
        int Legal = 0;
        int Spacing = 0;
        long long CoveredAfter = 0;
        int RouteSteps = -1;
        long long RouteValue = 0;
        int FrontierRouteSteps = -1;
        long long FrontierRouteValue = 0;
        int CorridorCells = 0;
        int CorridorCandidates = 0;
        int CorridorValid = 0;
        int CorridorOutOfRange = 0;
        bool Advances = false;
        bool LongRoute = false;
        bool FrontierLongRoute = false;
        bool Corridor = false;
        const char* Reason = "no-legal-native-site";
    };

    bool Matches_Base_Height(const HouseClass* house, const Cell& site)
    {
        const auto* cell = Playable_Cell(site);
        const auto* center = Playable_Cell(house->Base.PlacementCenter);
        return cell && center && std::abs(cell->Height - center->Height) < 3;
    }

    bool In_Native_AI_Build_Range(const HouseClass* house, const BuildingTypeClass* type, const Cell& site)
    {
        if (!Matches_Base_Height(house, site)) return false;
        // HouseClass::Can_Build_Here, called by the native AI finder at 0x004C6A10.
        // The player cursor's proximity check returns true for other houses.
        return reinterpret_cast<bool (__thiscall*)(const HouseClass*, const BuildingTypeClass*, const Cell&)>(
            0x004CB9C0)(house, type, site);
    }

    std::vector<Cell> Refinery_Corridor_Sites(const HouseClass* house, const BuildingTypeClass* type,
        const ExpansionStock& stock, bool shared, const Rect& base, int width, int height, std::span<const Cell> foundation,
        const std::set<int>& already_checked, const Cell& preferred, SiteSearch& search, std::vector<Cell>* frontier_sites)
    {
        const auto* ext = Extension::Fetch(type);
        if (width < 3 || foundation.size() != static_cast<size_t>(width * height) ||
            (ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid)) {
            search.Reason = "unsupported-corridor-dock-geometry";
            return {};
        }
        const auto* harvester = Expansion_Harvester(house, type);
        if (!harvester || harvester->MZone < MZONE_FIRST || harvester->MZone >= MZONE_COUNT ||
            !Map.CellZones || !Map.Zones[harvester->MZone]) {
            search.Reason = "no-compatible-corridor-movement-zone";
            return {};
        }
        const Cell offset = Refinery_Approach_Offset(width, height);
        // A coarse distance bound keeps the reverse search near possible
        // building sites. Native range validation still decides legality.
        const long long reach = 2LL * std::abs(static_cast<long long>(Rule->AIBaseSpacing)) + std::max(width, height) + 4LL;
        std::vector<const ExpansionStock::Sample*> seeds;
        for (const auto& sample : stock.Cells) {
            if (sample.Value > 0 && (shared || !sample.Covered) &&
                Distance_To_Rect(sample.Position - offset, base) <= reach + MaximumHarvesterRoute) seeds.push_back(&sample);
        }
        std::sort(seeds.begin(), seeds.end(), [&](const auto* a, const auto* b) {
            const auto rank = [&](const auto* sample) {
                return std::make_tuple(Distance_To_Rect(sample->Position - offset, base), -sample->Value,
                    sample->Position.As_Cell_Number());
            };
            return rank(a) < rank(b);
        });
        const auto passable_zone = [&](CellClass* cell) {
            if (Harvester_Ground_Blocker(cell) || Harvester_Passability_Blocker(harvester, cell)) return -1;
            const int zone = Map.Get_Cell_Zone(cell->CellID, harvester->MZone, false);
            return zone > 1 && zone != 0xFFFF ? zone : -1;
        };
        struct CorridorCell
        {
            Cell Position;
            CellClass* MapCell;
            int Zone;
            int Steps;
        };
        std::vector<CorridorCell> pending;
        pending.reserve(CorridorCellLimit);
        std::set<int> visited;
        for (const auto* sample : seeds) {
            if (std::any_of(pending.begin(), pending.end(), [&](const auto& entry) {
                return Grid_Distance(sample->Position, entry.Position) < 2;
            })) continue;
            auto* cell = Playable_Cell(sample->Position);
            const int zone = passable_zone(cell);
            if (zone < 0) continue;
            visited.insert(sample->Position.As_Cell_Number());
            pending.push_back({sample->Position, cell, zone, 0});
            if (pending.size() == CorridorSeedLimit) break;
        }
        std::vector<Cell> sites;
        sites.reserve(CorridorSiteLimit);
        const auto consider = [&](const Cell& site) {
            ++search.CorridorCandidates;
            if (In_Native_AI_Build_Range(house, type, site)) sites.push_back(site);
            else {
                ++search.CorridorOutOfRange;
                if (frontier_sites && Matches_Base_Height(house, site)) {
                    frontier_sites->push_back(site);
                }
            }
        };
        if (preferred != NoExpansionCell && !already_checked.count(preferred.As_Cell_Number())) consider(preferred);
        if (pending.empty() && sites.empty()) {
            search.Reason = "no-passable-corridor-seeds";
            return sites;
        }
        constexpr FacingType facings[]{FACING_N, FACING_E, FACING_S, FACING_W};
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto current = pending[i];
            ++search.CorridorCells;
            const Cell site = current.Position - offset;
            if (Distance_To_Rect(site, base) <= reach && Playable_Cell(site) && site != NoExpansionCell && site != preferred &&
                !already_checked.count(site.As_Cell_Number())) {
                consider(site);
                if (sites.size() == CorridorSiteLimit || search.CorridorCandidates == CorridorReachLimit) break;
            }
            if (current.Steps == MaximumHarvesterRoute || pending.size() == CorridorCellLimit) continue;
            for (const auto facing : facings) {
                const Cell previous = ::Adjacent_Cell(current.Position, facing);
                if (Distance_To_Rect(previous - offset, base) > reach + MaximumHarvesterRoute - current.Steps - 1) continue;
                auto* cell = Playable_Cell(previous);
                if (!cell || visited.count(previous.As_Cell_Number()) || passable_zone(cell) != current.Zone ||
                    !Harvester_Step_Allowed(cell, current.MapCell)) continue;
                visited.insert(previous.As_Cell_Number());
                pending.push_back({previous, cell, current.Zone, current.Steps + 1});
                if (pending.size() == CorridorCellLimit) break;
            }
        }
        if (sites.empty()) search.Reason = "no-new-corridor-sites-within-budget";
        return sites;
    }

    bool Preserves_Refinery_Site(HouseClass* house, const BuildingTypeClass* refinery, const Cell& refinery_site,
        bool long_route, const ExpansionStock& stock, const BuildingTypeClass* type, const Cell& site,
        std::span<const Cell> foundation, const RefineryFrontier* frontier = nullptr, bool relaxed = false)
    {
        std::set<int> occupied;
        for (const Cell& offset : foundation) occupied.insert((site + offset).As_Cell_Number());
        const auto intersects = [&](const Cell& offset) { return occupied.count((refinery_site + offset).As_Cell_Number()); };
        const auto refinery_foundation = Occupied_Foundation(refinery);
        const auto in_refinery_footprint = [&](const Cell& cell) {
            const Cell offset = cell - refinery_site;
            return (offset.Y >= refinery->Height() && offset.Y < refinery->Height(true) &&
                offset.X >= 0 && offset.X < refinery->Width()) ||
                std::find(refinery_foundation.begin(), refinery_foundation.end(), offset) != refinery_foundation.end();
        };
        if (std::any_of(refinery_foundation.begin(), refinery_foundation.end(), intersects)) return false;
        for (int y = refinery->Height(); y < refinery->Height(true); ++y) {
            for (int x = 0; x < refinery->Width(); ++x) {
                if (intersects(Cell(static_cast<short>(x), static_cast<short>(y)))) return false;
            }
        }
        // The intervening building's bib must also leave the refinery footprint free.
        for (int y = type->Height(); y < type->Height(true); ++y) {
            for (int x = 0; x < type->Width(); ++x) {
                const Cell cell = site + Cell(static_cast<short>(x), static_cast<short>(y));
                if (in_refinery_footprint(cell)) return false;
            }
        }
        Cell exit;
        if (frontier && Unit_Factory_Exit_Offset(type, exit)) {
            const int outside = std::max<int>(exit.X + 1, type->Width());
            for (int x = exit.X; x <= outside; ++x) {
                if (in_refinery_footprint(site + Cell(static_cast<short>(x), exit.Y))) return false;
            }
        }
        const auto routes = Find_Harvester_Routes(house, refinery, refinery_site, &occupied);
        int distance = std::numeric_limits<int>::max();
        const auto served = Refinery_Service_Value(stock, routes, frontier && !frontier->SharedStock, distance, long_route);
        const int cost = refinery->Cost_Of(house);
        return served > 0 && (frontier && !long_route ? Serves_Expansion_Stock(stock, served, cost, relaxed, frontier->SharedStock) :
            served >= cost);
    }

    SiteSearch Find_Expansion_Site(HouseClass* house, BuildingTypeClass* type, const ExpansionStock& stock,
        PlacementRole role, bool strict, bool relaxed = false, bool shared = false, bool allow_long_route = false,
        const Cell& preferred_site = NoExpansionCell, const ExpansionPlan* protect_refinery = nullptr,
        const RefineryTraceContext& context = {}, std::set<int>* examined = nullptr,
        RefinerySiteSearch mode = RefinerySiteSearch::Native, const RefineryFrontier* frontier = nullptr)
    {
        SiteSearch search;
        const bool corridor = mode == RefinerySiteSearch::Corridor || mode == RefinerySiteSearch::CorridorPlan;
        const bool exact = mode == RefinerySiteSearch::Exact || mode == RefinerySiteSearch::Frontier;
        const bool provisional_only = mode == RefinerySiteSearch::Frontier;
        search.Corridor = corridor || mode == RefinerySiteSearch::Exact;
        if (corridor) search.Reason = "no-valid-corridor-site";
        if (role == PlacementRole::None) {
            search.Reason = "unsupported-placement-role";
            return search;
        }
        if (const char* reason = Stock_Blocker(stock, relaxed)) {
            search.Reason = reason;
            return search;
        }
        if (stock.Seed == NoExpansionCell || type->IsCloakGenerator ||
            !Playable_Cell(house->Base.PlacementCenter) || house->Base.PlacementCenter == NoExpansionCell ||
            house->Base.InnerCells.Count() <= 0 || house->Base.InnerCells.Count() > MaximumPlacementAnchors) {
            search.Reason = "native-search-unavailable";
            return search;
        }
        const int width = type->Width();
        const int foundation_height = type->Height();
        const int height = type->Height(true);
        if (width <= 0 || foundation_height <= 0 || height < foundation_height || width > 64 || height > 65) return search;
        const auto foundation = Occupied_Foundation(type);
        if (foundation.empty()) return search;
        std::vector<Cell> anchors;
        Rect area(stock.Seed.X, stock.Seed.Y, 1, 1);
        int min_x = MAP_CELL_W;
        int min_y = MAP_CELL_H;
        int max_x = 0;
        int max_y = 0;
        for (const Cell& anchor : house->Base.InnerCells) {
            if (!Playable_Cell(anchor)) return search;
            anchors.push_back(anchor);
            min_x = std::min<int>(min_x, anchor.X);
            min_y = std::min<int>(min_y, anchor.Y);
            max_x = std::max<int>(max_x, anchor.X);
            max_y = std::max<int>(max_y, anchor.Y);
        }
        area.Set(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
        const bool refinery = role == PlacementRole::Refinery;
        const bool vehicles = role == PlacementRole::Vehicles;
        const bool finish_expansion = refinery && allow_long_route && !shared &&
            !stock.ServingRefineries && !stock.HasLongRouteRefinery;
        // Native offsets include the footprint, AIBaseSpacing, one facing offset,
        // an opposite-facing retry and up to two lateral steps.
        const long long reach = (finish_expansion ? MaximumHarvesterRoute : RefineryServiceRadius) + std::max(width, height) + 4LL +
            std::abs(static_cast<long long>(Rule->AIBaseSpacing));
        if (refinery && mode == RefinerySiteSearch::Native && std::none_of(stock.Cells.begin(), stock.Cells.end(), [&](const auto& sample) {
            return (shared || !sample.Covered) && Distance_To_Rect(sample.Position, area) <= reach;
        })) {
            search.Reason = "target-outside-native-search-radius";
            return search;
        }
        const Cell aim_cell = frontier ? frontier->Site : stock.Seed;
        const auto edge_distance = [&](const Cell& site, const BuildingTypeClass* building, std::span<const Cell> offsets) {
            if (!frontier) return Distance_To_Footprint(aim_cell, site, building);
            int distance = std::numeric_limits<int>::max();
            for (const Cell& offset : offsets) distance = std::min(distance, Grid_Distance(aim_cell, site + offset));
            return distance;
        };
        int current_edge = std::numeric_limits<int>::max();
        std::set<int> factory_exits;
        std::vector<const BuildingClass*> vehicle_factories;
        for (const BuildingClass* building : Buildings) {
            if (!Existing_Building(building) || building->House != house || !Playable_Cell(building->Get_Cell())) continue;
            if (!refinery && building->Class->IsBase && !Defensive_Building(building->Class) && Building_Access_Zones(building).count(stock.Zone)) {
                current_edge = std::min(current_edge, edge_distance(building->Get_Cell(), building->Class,
                    frontier ? Occupied_Foundation(building->Class) : std::span<const Cell>{}));
            }
            Cell offset;
            if (Unit_Factory_Exit_Offset(building->Class, offset)) {
                if (vehicles) vehicle_factories.push_back(building);
                const Cell origin = building->Get_Cell();
                const int y = origin.Y + offset.Y;
                const int outside = std::max<int>(offset.X + 1, building->Class->Width());
                for (int x = origin.X + offset.X; x <= origin.X + outside; ++x) {
                    if (x >= 0 && y >= 0 && x < MAP_CELL_W && y < MAP_CELL_H) {
                        factory_exits.insert(y * MAP_CELL_W + x);
                    }
                }
            }
        }
        const auto native_aims = [&] {
            std::sort(anchors.begin(), anchors.end(), [&](const Cell& a, const Cell& b) {
                const int da = a.Relative_Distance_To(aim_cell);
                const int db = b.Relative_Distance_To(aim_cell);
                return da != db ? da < db : a.As_Cell_Number() < b.As_Cell_Number();
            });
            std::vector<Cell> aims;
            if (refinery && preferred_site != NoExpansionCell) aims.push_back(preferred_site);
            aims.push_back(aim_cell);
            if (protect_refinery) aims.push_back(house->Base.PlacementCenter);
            if (refinery || vehicles) {
                const ExpansionStock::Sample* front = nullptr;
                for (const auto& sample : stock.Cells) {
                    if (!shared && sample.Covered) continue;
                    if (!front || std::make_tuple(Distance_To_Rect(sample.Position, area), -sample.Value, sample.Position.As_Cell_Number()) <
                        std::make_tuple(Distance_To_Rect(front->Position, area), -front->Value, front->Position.As_Cell_Number())) front = &sample;
                }
                if (front && front->Position != stock.Seed) aims.push_back(front->Position);
                if (refinery && front) {
                    const int center_x = area.X + area.Width / 2;
                    const int center_y = area.Y + area.Height / 2;
                    const bool across_y = std::abs(stock.Seed.X - center_x) >= std::abs(stock.Seed.Y - center_y);
                    const int front_distance = Distance_To_Rect(front->Position, area);
                    for (const int side : {-1, 1}) {
                        const ExpansionStock::Sample* flank = nullptr;
                        int farthest = 1;
                        for (const auto& sample : stock.Cells) {
                            if ((!shared && sample.Covered) || Distance_To_Rect(sample.Position, area) > front_distance + 3) continue;
                            const int lateral = across_y ? sample.Position.Y - front->Position.Y : sample.Position.X - front->Position.X;
                            if (side * lateral <= farthest) continue;
                            farthest = side * lateral;
                            flank = &sample;
                        }
                        if (flank && std::find(aims.begin(), aims.end(), flank->Position) == aims.end()) aims.push_back(flank->Position);
                    }
                }
            }
            const auto distinct_aim = [&](const Cell& anchor) {
                return std::none_of(aims.begin(), aims.end(), [&](const Cell& aim) { return Grid_Distance(anchor, aim) < 2; });
            };
            const size_t probe_limit = refinery ? 4 * PlacementProbes : vehicles ? 2 * PlacementProbes : PlacementProbes;
            const size_t spacing_probes = vehicles && !strict && !vehicle_factories.empty() ? 2 : 0;
            for (const Cell& anchor : anchors) {
                if (distinct_aim(anchor)) {
                    aims.push_back(anchor);
                    if (aims.size() == probe_limit - spacing_probes) break;
                }
            }
            for (size_t probe = 0; probe < spacing_probes; ++probe) {
                const Cell* best = nullptr;
                int farthest = -1;
                for (const Cell& anchor : anchors) {
                    if (!distinct_aim(anchor)) continue;
                    int nearest = std::numeric_limits<int>::max();
                    for (const auto* factory : vehicle_factories) {
                        nearest = std::min(nearest, Distance_To_Footprint(anchor, factory->Get_Cell(), factory->Class));
                    }
                    if (nearest > farthest) {
                        farthest = nearest;
                        best = &anchor;
                    }
                }
                if (!best) break;
                aims.push_back(*best);
            }
            return aims;
        };

        std::set<int> checked;
        const auto protected_stock = protect_refinery ? Read_Expansion_Stock(house, protect_refinery->FollowupTarget) : ExpansionStock{};
        const bool trace_sites = refinery && context.Phase && Refinery_Trace_Enabled();
        const int field = trace_sites ? Survey_Field_ID(stock.Seed) : -1;
        std::tuple<bool, bool, bool, int, long long, int, int, int> best_rating{};
        std::tuple<int, bool, int, long long, int> best_frontier{};
        const auto consider_site = [&](Cell site, bool provisional = false) {
            if (site == NoExpansionCell) return;
            RefinerySiteTrace trace;
            trace.Field = field;
            trace.Zone = stock.Zone;
            const auto report = [&](const char* result) {
                if (!trace_sites) return;
                trace.Result = result;
                Log_Refinery_Site(house, type, context, site, trace);
            };
            if (!Playable_Cell(site)) {
                trace.Native = "returned-site-outside-playable-map";
                report("site-outside-playable-map");
                return;
            }
            if (!checked.insert(site.As_Cell_Number()).second) return;
            if (!provisional) ++search.Checked;
            const Cell derived_site = site;
            if (!type->Legal_Placement(site, house) || ((corridor || exact) && site != derived_site)) {
                if (exact) search.Reason = "native-legal-placement-rejected";
                trace.Native = "legal-placement-rejected";
                report("native-legal-placement-rejected");
                return;
            }
            if (!provisional) ++search.Legal;
            trace.Native = provisional ? "legal-placement-passed-outside-build-range" : "legal-placement-passed";

            const auto protected_cell = [&](const Cell& offset) {
                const Cell cell(static_cast<short>(site.X + offset.X), static_cast<short>(site.Y + offset.Y));
                CellClass* map_cell = Playable_Cell(cell);
                TiberiumType resource;
                const char* blocker = !map_cell ? "outside-playable-map" :
                    map_cell->IsUnderBridge ? "bridge-cell" : map_cell->Cell_Occupier() ? "occupied-cell" :
                    map_cell->Land_Type() == LAND_ROAD ? "road" : factory_exits.count(cell.As_Cell_Number()) ? "factory-exit" :
                    Resource_At(map_cell, resource) ? "tiberium" : nullptr;
                if (blocker && trace_sites) {
                    trace.BlockedCell = cell;
                    trace.FootprintBlocker = blocker;
                }
                return blocker != nullptr;
            };
            bool protected_cells = std::any_of(foundation.begin(), foundation.end(), protected_cell);
            for (int y = foundation_height; y < height && !protected_cells; ++y) {
                for (int x = 0; x < width; ++x) {
                    if (protected_cell(Cell(static_cast<short>(x), static_cast<short>(y)))) {
                        protected_cells = true;
                        break;
                    }
                }
            }
            if (protected_cells) {
                search.Reason = "footprint-crosses-occupied-or-protected-cells";
                report(search.Reason);
                return;
            }
            const int access = Clear_Access_Count(site, width, foundation_height, stock.Zone);
            trace.Access = access;
            if (access < (refinery || vehicles ? 1 : 2)) {
                search.Reason = "no-clear-ground-access";
                report("no-clear-perimeter-access-in-field-zone");
                return;
            }
            if (vehicles && !Vehicle_Exit_Clear(type, site, stock.Zone, foundation)) {
                search.Reason = "blocked-vehicle-exit";
                return;
            }
            if (protect_refinery && !Preserves_Refinery_Site(house, BuildingTypes[protect_refinery->FollowupRefinery],
                protect_refinery->FollowupSite, protect_refinery->FollowupLongRoute, protected_stock, type, site, foundation)) {
                search.Reason = "power-site-conflicts-with-refinery";
                return;
            }
            int distance = refinery ? std::numeric_limits<int>::max() : edge_distance(site, type, foundation);
            long long served = 0;
            bool long_route = false;
            if (refinery) {
                const auto routes = Find_Harvester_Routes(house, type, site, nullptr, trace_sites ? &trace : nullptr);
                const int cost = type->Cost_Of(house);
                trace.Cost = cost;
                served = Refinery_Service_Value(stock, routes, !shared, distance, false, trace_sites ? &trace.Short : nullptr);
                if (trace_sites) trace.Short.Result = Refinery_Route_Result(trace, trace.Short);
                if (served <= 0 || (strict && !Serves_Expansion_Stock(stock, served, cost, relaxed, shared))) {
                    search.Reason = served <= 0 ? "no-short-harvester-route" : "target-outside-service-of-sampled-sites";
                    if (served > 0) trace.Short.Result = Refinery_Stock_Rejection(served, cost, shared);
                    if (!finish_expansion) {
                        trace.LongPolicy = !allow_long_route ? "not-enabled-for-target" : shared ? "shared-stock" :
                            stock.ServingRefineries ? "field-already-served" : "existing-long-route-refinery";
                        report(trace.Short.Result);
                        return;
                    }
                    trace.LongPolicy = "checked";
                    distance = std::numeric_limits<int>::max();
                    served = Refinery_Service_Value(stock, routes, true, distance, true, trace_sites ? &trace.Long : nullptr);
                    if (trace_sites) trace.Long.Result = Refinery_Route_Result(trace, trace.Long);
                    if (served <= 0 || served < cost) {
                        if (served > 0) trace.Long.Result = "reachable-stock-below-refinery-cost";
                        report(trace.Long.Result);
                        return;
                    }
                    long_route = true;
                }
                if (!long_route) trace.LongPolicy = "not-needed";
                if (provisional) {
                    const auto rank = std::make_tuple(Distance_To_Rect(site, area), long_route, distance, -served, site.As_Cell_Number());
                    if (search.FrontierSite == NoExpansionCell || rank < best_frontier) {
                        search.FrontierSite = site;
                        search.FrontierLongRoute = long_route;
                        search.FrontierRouteSteps = distance;
                        search.FrontierRouteValue = served;
                        best_frontier = rank;
                    }
                    report("eligible-refinery-frontier");
                    return;
                }
                if (corridor) ++search.CorridorValid;
                report(long_route ? "eligible-long-route" : "eligible-short-route");
            }
            const bool advances = !refinery && current_edge != std::numeric_limits<int>::max() && distance < current_edge &&
                (frontier || !strict || vehicles || distance + 2 <= current_edge);
            const bool spread_only = !frontier && vehicles && !strict && !vehicle_factories.empty() && !advances;
            if (!refinery && !advances && !spread_only && !protect_refinery) {
                search.Reason = "no-forward-progress";
                return;
            }
            if (frontier && !Preserves_Refinery_Site(house, BuildingTypes[frontier->Type], frontier->Site,
                frontier->LongRoute, stock, type, site, foundation, frontier, relaxed)) {
                search.Reason = "infrastructure-conflicts-with-refinery-frontier";
                return;
            }
            // Require sufficient value, then prefer the shortest usable route.
            int spacing = 0;
            if (!vehicle_factories.empty()) {
                spacing = std::numeric_limits<int>::max();
                for (const auto* factory : vehicle_factories) {
                    const Cell other = factory->Get_Cell();
                    const int dx = std::max({site.X - other.X - factory->Class->Width() + 1, other.X - site.X - width + 1, 0});
                    const int dy = std::max({site.Y - other.Y - factory->Class->Height(true) + 1, other.Y - site.Y - height + 1, 0});
                    spacing = std::min(spacing, std::max(dx, dy));
                }
            }
            const auto rating = std::make_tuple(!corridor && long_route,
                !corridor && preferred_site != NoExpansionCell && site != preferred_site,
                spread_only, spread_only ? -spacing : distance,
                -served, spread_only ? distance : -spacing, -access, site.As_Cell_Number());
            if (search.Site == NoExpansionCell || rating < best_rating) {
                search.Site = site;
                search.Spacing = spacing;
                search.CoveredAfter = !refinery ? 0 : shared ? std::max(served, stock.Value - stock.Uncovered) :
                    stock.Value - stock.Uncovered + served;
                search.Advances = advances;
                search.LongRoute = long_route;
                if (refinery) {
                    search.RouteSteps = distance;
                    search.RouteValue = served;
                }
                best_rating = rating;
            }
        };
        if (corridor) {
            const auto& already_checked = examined ? *examined : checked;
            std::vector<Cell> frontier_sites;
            for (const Cell& site : Refinery_Corridor_Sites(house, type, stock, shared, area, width, foundation_height,
                foundation, already_checked, preferred_site, search,
                mode == RefinerySiteSearch::CorridorPlan ? &frontier_sites : nullptr)) consider_site(site);
            if (search.Site == NoExpansionCell && !frontier_sites.empty()) {
                const char* failure = search.Reason;
                std::sort(frontier_sites.begin(), frontier_sites.end(), [&](const Cell& a, const Cell& b) {
                    return std::make_pair(Distance_To_Rect(a, area), a.As_Cell_Number()) <
                        std::make_pair(Distance_To_Rect(b, area), b.As_Cell_Number());
                });
                if (frontier_sites.size() > FrontierSiteLimit) frontier_sites.resize(FrontierSiteLimit);
                for (const Cell& site : frontier_sites) consider_site(site, true);
                search.Reason = failure;
            }
        } else if (exact) {
            if (provisional_only ? Matches_Base_Height(house, preferred_site) : In_Native_AI_Build_Range(house, type, preferred_site)) {
                consider_site(preferred_site, provisional_only);
            } else search.Reason = "refinery-frontier-outside-build-range";
        } else {
            for (const Cell& aim : native_aims()) {
                ++search.Probes;
                consider_site(house->Find_Build_Location(type, &Weight_Expansion_Anchor, aim.As_Cell_Number()));
            }
            if (examined) *examined = std::move(checked);
        }
        if (search.Site != NoExpansionCell) search.Reason = corridor ? "refinery-corridor-site-selected" :
            exact ? "refinery-exact-site-verified" :
            search.LongRoute ? "refinery-followup-long-route" : refinery || search.Advances ? "selected" : "factory-spacing";
        return search;
    }

    const char* Refinery_Frontier_Blocker(HouseClass* house, const RefineryFrontier& frontier,
        const ExpansionStock& stock, bool relaxed)
    {
        if (!stock.HasRefinery) return "no-operational-refinery";
        const int field = Survey_Field_ID(frontier.Target);
        if (field >= 0 && Survey_Field_ID(stock.Seed) != field) return "refinery-frontier-field-changed";
        const auto check = Find_Expansion_Site(house, BuildingTypes[frontier.Type], stock, PlacementRole::Refinery, true,
            relaxed, frontier.SharedStock, frontier.LongRoute, frontier.Site, nullptr, {}, nullptr, RefinerySiteSearch::Frontier);
        return check.FrontierSite == NoExpansionCell ? check.Reason : nullptr;
    }

    SiteSearch Find_Refinery_Corridor(HouseClass* house, BuildingTypeClass* type, const ExpansionStock& stock,
        bool relaxed, bool shared, bool allow_long_route, const Cell& preferred, const Cell& target,
        const char* phase, const char* native_result, std::set<int>& examined, bool survey_frontier = false)
    {
        const int field = Survey_Field_ID(stock.Seed);
        DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} phase={} search=corridor state=started "
            "native-result={} field={} target={},{} type={}\n",
            static_cast<int>(house->HeapID), Frame, phase, native_result, field, target.X, target.Y, type->Name());
        const auto search = Find_Expansion_Site(house, type, stock, PlacementRole::Refinery, true, relaxed, shared,
            allow_long_route, preferred, nullptr, {target, phase, "corridor"}, &examined,
            survey_frontier ? RefinerySiteSearch::CorridorPlan : RefinerySiteSearch::Corridor);
        DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} phase={} search=corridor field={} target={},{} type={} "
            "cells={} derived={} out-of-build-range={} checked={} valid={} result={} site={},{} steps={} reachable-value={}\n",
            static_cast<int>(house->HeapID), Frame, phase, field, target.X, target.Y, type->Name(), search.CorridorCells,
            search.CorridorCandidates, search.CorridorOutOfRange, search.Checked, search.CorridorValid, search.Reason,
            search.Site.X, search.Site.Y, search.RouteSteps, search.RouteValue);
        return search;
    }

    void Log_Placement_Search(const HouseClass* house, const BuildingTypeClass* type,
        const Cell& target, const SiteSearch& search, const char* phase)
    {
        DEV_DEBUG_INFO("AF-0009B2b1: house={} type={} phase={} target={},{} site={},{} probes={} checked={} legal={} reason={} covered-after={}\n",
            static_cast<int>(house->HeapID), type->Name(), phase, target.X, target.Y, search.Site.X, search.Site.Y,
            search.Probes, search.Checked, search.Legal, search.Reason, search.CoveredAfter);
        if (type->IsRefinery && !search.Corridor && !search.Checked && Refinery_Trace_Enabled()) {
            RefinerySiteTrace trace;
            trace.Field = Survey_Field_ID(target);
            trace.Native = search.Probes ? "no-playable-site-returned" : "not-run";
            trace.Result = search.Probes ? "no-candidate-coordinate-to-check" : search.Reason;
            Log_Refinery_Site(house, type, {target, phase}, NoExpansionCell, trace);
        }
    }

    bool Can_Supply_Refinery(const HouseClass* house, const BuildingTypeClass* refinery)
    {
        const auto compatible = [refinery](const UnitTypeClass* type) {
            return type && type->IsToHarvest &&
                std::find(type->Dock.begin(), type->Dock.end(), refinery) != type->Dock.end();
        };
        if (compatible(refinery->FreeUnit)) return true;
        return std::any_of(Units.begin(), Units.end(), [&](const UnitClass* unit) {
            return unit && unit->House == house && unit->IsActive && !unit->IsInLimbo && unit->Strength > 0 &&
                compatible(unit->Class);
        });
    }

    bool Has_Expansion_Yard(const HouseClassExtension& extension, const BuildingTypeClass* type)
    {
        for (const BuildingClass* yard : extension.This_Const()->ConstructionYards) {
            if (yard && !yard->Factory && extension.Can_Advanced_AI_Build_Building(type, yard)) return true;
        }
        return false;
    }

    void Clear_Expansion_Order(ExpansionPlan& plan)
    {
        plan.Type = STRUCT_NONE;
        plan.ProductID = -1;
        plan.Site = NoExpansionCell;
        plan.MinimumStock = 0;
        plan.ExtraRefinery = false;
        plan.RelaxedStock = false;
        plan.SharedStock = false;
        plan.AllowLongRoute = false;
        plan.CorridorSite = false;
        plan.AwaitingService = false;
    }

    void Keep_Expansion_Followup(ExpansionPlan& plan, const Cell& target, bool verified)
    {
        if (plan.FollowupTarget != NoExpansionCell && plan.FollowupTarget != target) return;
        plan.FollowupTarget = target;
        plan.FollowupProgress = Frame;
        plan.FollowupMinimumFunds = 0;
        plan.FollowupSiteVerified |= verified;
        plan.FollowupAdvanced |= !verified;
    }

    void Clear_Expansion_Followup(ExpansionPlan& plan)
    {
        plan.FollowupTarget = NoExpansionCell;
        plan.FollowupSite = NoExpansionCell;
        plan.FollowupRefinery = STRUCT_NONE;
        plan.FollowupPower = STRUCT_NONE;
        plan.FollowupLongRoute = false;
        plan.FollowupCorridor = false;
        plan.FollowupProgress = -1;
        plan.FollowupMinimumFunds = 0;
        plan.FollowupSiteVerified = false;
        plan.FollowupAdvanced = false;
    }

    void Defer_Expansion(const HouseClass* house, ExpansionPlan& plan, const char* reason)
    {
        DEV_DEBUG_INFO("AF-0009B2b: house={} deferred target={},{} reason={}\n",
            static_cast<int>(house->HeapID), plan.Target.X, plan.Target.Y, reason);
        plan.DeferredTarget = plan.Target;
        plan.DeferredAt = Frame;
        plan.LastProbe = Frame;
        if (plan.ExtraRefinery && plan.FollowupTarget == plan.Target) plan.FollowupSiteVerified = false;
        plan.Target = NoExpansionCell;
        plan.FailedSearches = 0;
        Clear_Expansion_Order(plan);
    }

    void Defer_Corridor_Field(const HouseClass* house, ExpansionPlan& plan, int field, Cell target, const char* reason)
    {
        plan.DeferredFields.insert_or_assign(field, Frame);
        if (plan.FollowupTarget == target || Survey_Field_ID(plan.FollowupTarget) == field) Clear_Expansion_Followup(plan);
        if (plan.Target == target || Survey_Field_ID(plan.Target) == field) plan.Target = NoExpansionCell;
        plan.FailedSearches = 0;
        DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} search=corridor field={} target={},{} "
            "state=deferred reason={} retry-frame={}\n",
            static_cast<int>(house->HeapID), Frame, field, target.X, target.Y, reason, Frame + ExpansionRetryInterval);
    }

    void Abandon_Refinery_Frontier(const HouseClass* house, ExpansionPlan& plan, const char* reason)
    {
        const auto frontier = plan.Frontier;
        plan.Frontier = {};
        const int field = Survey_Field_ID(frontier.Target);
        DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} target={},{} site={},{} "
            "state=frontier-abandoned reason={} steps={}\n", static_cast<int>(house->HeapID), Frame,
            frontier.Target.X, frontier.Target.Y, frontier.Site.X, frontier.Site.Y, reason, frontier.Steps);
        if (field >= 0) Defer_Corridor_Field(house, plan, field, frontier.Target, reason);
        else {
            plan.DeferredTarget = frontier.Target;
            plan.DeferredAt = Frame;
        }
    }

    void Evaluate(HouseClass* house, AdvancedAIResourceAnalysis& result)
    {
        result = AdvancedAIResourceAnalysis();
        result.Frame = Frame;
        result.SurveyFrame = SurveyFrame;
        Refresh_Resource_Zones();
        std::set<int> access_zones;
        std::vector<const BuildingClass*> refineries;
        std::map<int, Cell> enemy_bases;
        bool has_yard = false;
        for (const BuildingClass* building : Buildings) {
            if (!Existing_Building(building) || !Playable_Cell(building->Get_Cell())) continue;
            if (building->House == house) {
                const bool yard = building->Class->ToBuild == RTTI_BUILDINGTYPE;
                if (!result.HasBase || (yard && !has_yard)) result.Base = building->Get_Cell();
                result.HasBase = true;
                has_yard |= yard;
                const auto zones = Building_Access_Zones(building);
                access_zones.insert(zones.begin(), zones.end());
                if (building->Class->IsRefinery && building->IsOn &&
                    building->Mission != MISSION_CONSTRUCTION && building->MissionQueue != MISSION_CONSTRUCTION) {
                    refineries.push_back(building);
                }
            } else if (!building->House->IsDefeated && building->House->Class && !building->House->Class->IsMultiplayPassive &&
                !house->Is_Ally(building->House)) {
                const Cell center = building->House->Center.As_Cell();
                enemy_bases.emplace(static_cast<int>(building->House->HeapID),
                    Playable_Cell(center) ? center : building->Get_Cell());
            }
        }
        if (result.HasBase && Playable_Cell(house->Center.As_Cell())) result.Base = house->Center.As_Cell();

        std::vector<unsigned char> covered(FieldForCell.size(), 0);
        std::vector<int> nearby(SurveyFields.size(), 0);
        for (const BuildingClass* refinery : refineries) {
            const auto routes = Find_Harvester_Routes(house, refinery->Class, refinery->Get_Cell());
            if (routes.Approach == NoExpansionCell) continue;
            std::set<int> served_fields;
            for (int y = -RefineryServiceRadius; y <= RefineryServiceRadius; ++y) {
                for (int x = -RefineryServiceRadius; x <= RefineryServiceRadius; ++x) {
                    const Cell cell(static_cast<short>(routes.Approach.X + x), static_cast<short>(routes.Approach.Y + y));
                    if (routes.Distance_To(cell) < 0) continue;
                    const int index = cell.As_Cell_Number();
                    if (index >= static_cast<int>(FieldForCell.size())) continue;
                    const int field = FieldForCell[index];
                    if (field >= 0 && ResourceZones[index] >= 0) {
                        covered[index] = 1;
                        served_fields.insert(field);
                    }
                }
            }
            for (int field : served_fields) ++nearby[field];
        }

        for (size_t i = 0; i < SurveyFields.size(); ++i) {
            const auto& source = SurveyFields[i];
            auto field = source.Summary;
            field.NearbyRefineries = nearby[i];
            for (const BuildingClass* refinery : refineries) {
                const int distance = Grid_Distance(field.Center, refinery->Get_Cell());
                if (field.RefineryDistance < 0 || distance < field.RefineryDistance) field.RefineryDistance = distance;
            }
            long long uncovered_value = 0;
            int target_distance = std::numeric_limits<int>::max();
            int target_index = std::numeric_limits<int>::max();
            auto best_uncovered = std::make_pair(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
            for (const auto& sample : source.Cells) {
                const int index = sample.Index;
                if (covered[index]) ++field.CoveredCells;
                if (!result.HasBase || !access_zones.count(ResourceZones[index])) continue;
                ++field.ReachableCells;
                const long long value = sample.Value;
                field.ReachableValue += value;
                const Cell cell = Cell_From_Index(index);
                const int distance = field.Center.Relative_Distance_To(cell);
                if (!covered[index] && value > 0) {
                    uncovered_value += value;
                    const auto rating = std::make_pair(distance, index);
                    if (rating < best_uncovered) {
                        best_uncovered = rating;
                        field.UncoveredTarget = cell;
                    }
                }
                if (distance < target_distance || (distance == target_distance && index < target_index)) {
                    target_distance = distance;
                    target_index = index;
                    field.Target = cell;
                }
            }
            if (result.HasBase) field.BaseDistance = Grid_Distance(result.Base,
                field.ReachableCells ? field.Target : field.Center);
            const Cell target = field.ReachableCells ? field.Target : field.Center;
            for (const auto& enemy : enemy_bases) {
                const int distance = Grid_Distance(target, enemy.second);
                if (field.EnemyBaseDistance < 0 || distance < field.EnemyBaseDistance) field.EnemyBaseDistance = distance;
            }
            if (field.ReachableCells) {
                field.UncoveredValue = uncovered_value;
                // Retain 1/4 of already served value; reward uncovered, reachable stock.
                const long long useful_value = (field.ReachableValue + 3 * uncovered_value) / 4;
                const long long distance = 8LL + field.BaseDistance;
                const long long divisor = distance * distance * (1LL + field.NearbyRefineries);
                field.Score = Apply_Ratio(useful_value, 1024, divisor);
                if (field.EnemyBaseDistance >= 0 && field.EnemyBaseDistance < EnemyInfluenceRadius) {
                    field.Score = Apply_Ratio(field.Score, field.EnemyBaseDistance + 1, EnemyInfluenceRadius + 1);
                }
                if (field.EnemyBaseDistance >= 0 && field.EnemyBaseDistance < field.BaseDistance) field.Score /= 2;
                // Integer rounding must not erase the only otherwise useful target.
                if (field.ReachableValue > 0) field.Score = std::max(1LL, field.Score);
            }
            result.Fields.push_back(std::move(field));
        }
        const AdvancedAIResourceField* best = nullptr;
        for (const auto& field : result.Fields) {
            if (field.Score > 0 && (!best || Better_Field(field, *best))) best = &field;
        }
        if (best) result.SelectedField = best->ID;
    }

    void Log_Analysis(const HouseClass* house, const AdvancedAIResourceAnalysis& result)
    {
#ifdef NDEBUG
        if (!Vinifera_DeveloperMode) return;
#endif
        // No diagnostic ordering or state participates in the selection above.
        const size_t count = std::min(result.Fields.size(), static_cast<size_t>(LoggedFields));
        DEV_DEBUG_INFO("AF-0009B2: house={} fields={} logged={} base={} selected-field={} frame={} survey-frame={}\n",
            static_cast<int>(house->HeapID), result.Fields.size(), count, result.HasBase,
            result.SelectedField, result.Frame, result.SurveyFrame);
        std::vector<const AdvancedAIResourceField*> order;
        for (const auto& field : result.Fields) order.push_back(&field);
        std::partial_sort(order.begin(), order.begin() + count, order.end(),
            [](const auto* a, const auto* b) { return Better_Field(*a, *b); });
        for (size_t i = 0; i < count; ++i) {
            const auto& field = *order[i];
            DEV_DEBUG_INFO("AF-0009B2: house={} field={} cells={} amount={} value={} types={} distance={} refinery-distance={} refineries={} covered={} reachable={} reachable-value={} enemy-distance={} score={} target={},{}\n",
                static_cast<int>(house->HeapID), field.ID, field.CellCount, field.Amount, field.Value, field.Types.size(),
                field.BaseDistance, field.RefineryDistance, field.NearbyRefineries, field.CoveredCells, field.ReachableCells,
                field.ReachableValue, field.EnemyBaseDistance, field.Score, field.Target.X, field.Target.Y);
        }
    }
}

void HouseClassExtension::Clear_Advanced_AI_Resource_State()
{
    Clear_Advanced_AI_Building_Order_State();
    SurveyFrame = -1;
    ZoneFrame = -1;
    SurveyFields.clear();
    FieldForCell.clear();
    ResourceZones.clear();
    HouseAnalyses.clear();
    ExpansionPlans.clear();
    ExpansionCreeps.clear();
    CreepSteps.clear();
    CreepBlocks.clear();
    CreepRejectedTargets.clear();
    RefineryTraces.clear();
    ActivePlacement = PlacementRequest{};
}

void HouseClassExtension::Update_Advanced_AI_Resource_Analysis() const
{
    if (!Is_Advanced_AI_Enabled()) return;
    auto& result = HouseAnalyses[static_cast<int>(This_Const()->HeapID)];
    if (!Due(result.Frame)) return;
    if (Due(SurveyFrame)) Rebuild_Survey();
    Evaluate(This(), result);
    Log_Analysis(This_Const(), result);
}

const AdvancedAIResourceAnalysis* HouseClassExtension::Get_Advanced_AI_Resource_Analysis() const
{
    if (!Is_Advanced_AI_Enabled()) return nullptr;
    const auto found = HouseAnalyses.find(static_cast<int>(This_Const()->HeapID));
    return found != HouseAnalyses.end() ? &found->second : nullptr;
}

/**
 *  AF-0009U: Has the expansion planner found a placeable site for its next
 *  expansion step (an order with a site, or a verified site waiting for
 *  credits)? A target outside the build range has none; saving for it would
 *  only hoard credits.
 */
/**
 *  AF-0009U: Placement rule for the base creep.
 *
 *  The native AI only builds within 2 height levels of its base center
 *  (HouseClass::Find_Build_Location); human players are not limited that way.
 *  A creep step may use the player rules instead (legal footprint, next to own
 *  buildings via HouseClass::Can_Build_Here), so the base can grow over a cliff,
 *  as long as the site stays reachable on the ground:
 *
 *  - a refinery's dock approach cell must be in the field's ground zone
 *    (also on the base level; harvesters must reach the dock),
 *  - another building on a different level needs a neighbouring cell in the
 *    base's ground zone.
 */
static bool Creep_Site_Allowed(HouseClass* house, BuildingTypeClass* type, const Cell& site, bool refinery,
    int base_zone, int field_zone)
{
    Cell placement = site; // Legal_Placement takes a non-const reference.
    if (!type->Legal_Placement(placement, house)) return false;

    if (refinery) {
        const int zone = Ground_Zone(Playable_Cell(site + Refinery_Approach_Offset(type->Width(), type->Height())));
        const int wanted = field_zone >= 0 ? field_zone : base_zone;
        if (zone < 0 || (wanted >= 0 && zone != wanted)) return false;
    }

    if (In_Native_AI_Build_Range(house, type, site)) return true;

    // Another level: the player proximity rule without the native AI height limit.
    using CanBuildHere = bool(__thiscall*)(const HouseClass*, const BuildingTypeClass*, const Cell&);
    if (!reinterpret_cast<CanBuildHere>(0x004CB9C0)(house, type, site)) return false;
    if (refinery) return true;

    if (base_zone < 0) return false;
    const int width = type->Width();
    const int height = type->Height();
    for (int y = site.Y - 1; y <= site.Y + height; ++y) {
        for (int x = site.X - 1; x <= site.X + width; ++x) {
            if (x >= site.X && x < site.X + width && y >= site.Y && y < site.Y + height) continue;
            if (Ground_Zone(Playable_Cell(Cell(static_cast<short>(x), static_cast<short>(y)))) == base_zone) return true;
        }
    }
    return false;
}


bool HouseClassExtension::Has_Advanced_AI_Expansion_Site() const
{
    const int id = static_cast<int>(This_Const()->HeapID);
    if (const auto creep = ExpansionCreeps.find(id); creep != ExpansionCreeps.end() &&
        creep->second.Target != NoExpansionCell && creep->second.Site != NoExpansionCell) return true;
    const auto found = ExpansionPlans.find(id);
    if (found == ExpansionPlans.end()) return false;
    const auto& plan = found->second;
    return (plan.Type != STRUCT_NONE && plan.Site != NoExpansionCell) ||
        (plan.FollowupTarget != NoExpansionCell && plan.FollowupSiteVerified);
}


/**
 *  AF-0009U: Best of both worlds. The AF-0009 planner first looks for a verified
 *  refinery site (service radius, harvester route, corridor). When it finds none
 *  for the field it wants, the base grows towards that field like DTA does:
 *
 *  - Power plants are placed as close to the field as the build range allows.
 *  - The refinery follows when its closest legal site is near the field
 *    (CloseEnough), when no power plant site gets the base any closer, or
 *    after MaxSteps steps. It is then placed as close to the field as possible,
 *    without the AF-0009 route checks.
 *
 *  refinery_only: only return a refinery step (checked before the B2
 *  development); otherwise power steps are allowed too (after B2).
 */
const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Expansion_Creep(bool refinery_only, const char*& reason)
{
    static constexpr int ProbeInterval = 15 * TICKS_PER_SECOND;
    static constexpr int CloseEnough = 6;
    static constexpr int MaxSteps = 10;
    static constexpr int SearchMargin = 10;

    HouseClass* house = This();
    const int id = static_cast<int>(house->HeapID);
    auto& creep = ExpansionCreeps[id];
    const auto stop = [&](const char* why) -> const BuildingTypeClass* {
        if (creep.Target != NoExpansionCell) {
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} expansion-creep stopped target={},{} steps={} reason={}\n",
                id, Frame, creep.Target.X, creep.Target.Y, creep.Steps, why);
        }
        creep = {};
        return nullptr;
    };

    /**
     *  AF-0009U: Count destroyed creep steps per target. LossLimit losses near
     *  one target block it for BlockDuration frames. Steps that survived
     *  BlockDuration are no longer watched.
     */
    const auto near_cell = [](const Cell& a, const Cell& b, int radius) {
        return std::abs(a.X - b.X) <= radius && std::abs(a.Y - b.Y) <= radius;
    };
    auto& blocks = CreepBlocks[id];
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const CreepTargetBlock& block) {
        return block.Until >= 0 ? Frame >= block.Until : Frame - block.Since >= CreepBlockDuration;
    }), blocks.end());
    auto& steps = CreepSteps[id];
    for (auto it = steps.begin(); it != steps.end();) {
        bool alive = false;
        for (const BuildingClass* building : Buildings) {
            if (building && building->ID == it->BuildingID) {
                alive = Existing_Building(building) && building->House == house;
                break;
            }
        }
        if (alive) {
            it = Frame - it->Frame >= CreepBlockDuration ? steps.erase(it) : it + 1;
            continue;
        }
        auto block = std::find_if(blocks.begin(), blocks.end(), [&](const CreepTargetBlock& entry) {
            return near_cell(entry.Target, it->Target, CreepBlockRadius);
        });
        if (block == blocks.end()) {
            blocks.push_back({it->Target, 0, static_cast<long>(Frame), -1});
            block = blocks.end() - 1;
        }
        ++block->Losses;
        if (block->Losses >= CreepLossLimit && block->Until < 0) {
            block->Until = Frame + CreepBlockDuration;
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} expansion-creep target-blocked target={},{} losses={} until={}\n",
                id, Frame, block->Target.X, block->Target.Y, block->Losses, block->Until);
        }
        it = steps.erase(it);
    }

    if (IsObserver || house->ConstructionYards.Count() <= 0) return stop("no-construction-yard");
    /**
     *  AF-0009U: Besides the factory-based shortfall (two refineries per war
     *  factory), creep while the house has fewer refineries than three or than
     *  its enemy. A house with one war factory stopped at two refineries even
     *  when every field it could reach directly was rejected.
     */
    if (Advanced_AI_Refinery_Shortfall(true) <= 0) {
        int refineries = 0;
        for (const BuildingClass* building : Buildings) {
            if (Existing_Building(building) && building->House == house && building->Class->IsRefinery) ++refineries;
        }
        if (refineries >= std::max(3, EnemyRefineryCount)) return stop("no-refinery-shortfall");
    }

    const auto found = ExpansionPlans.find(id);
    if (found == ExpansionPlans.end()) return stop("no-expansion-plan");
    auto& plan = found->second;
    if (plan.Type != STRUCT_NONE) return stop("expansion-order-in-progress");
    if (plan.FollowupTarget != NoExpansionCell && plan.FollowupSiteVerified) return stop("verified-site");

    const Cell target = plan.Target != NoExpansionCell ? plan.Target :
        plan.DeferredTarget != NoExpansionCell ? plan.DeferredTarget : plan.FollowupTarget;
    if (target == NoExpansionCell || !Playable_Cell(target)) return stop("no-target");

    /**
     *  AF-0009U: No creep towards a field next to an enemy base or towards a
     *  target blocked after lost steps. Logged once per rejected target.
     *  The field is deferred like a failed corridor field, so the planner
     *  turns to another field instead of holding the refinery reserve for one
     *  it will never reach.
     */
    std::vector<const BuildingClass*> enemy_buildings;
    for (const BuildingClass* building : Buildings) {
        if (!Existing_Building(building) || building->House == nullptr || building->Class->IsWall ||
            house->Is_Ally(building->House) || building->House->Class->IsMultiplayPassive) continue;
        enemy_buildings.push_back(building);
    }
    const auto near_enemy = [&](const Cell& cell, int radius) {
        for (const BuildingClass* building : enemy_buildings) {
            if (Distance_To_Footprint(cell, building->Get_Cell(), building->Class) <= radius) return true;
        }
        return false;
    };
    const auto reject = [&](const char* why) -> const BuildingTypeClass* {
        auto& rejected = CreepRejectedTargets[id];
        if (rejected != target) {
            rejected = target;
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} expansion-creep rejected target={},{} reason={}\n",
                id, Frame, target.X, target.Y, why);
        }
        if (const int field = Survey_Field_ID(target); field >= 0) Defer_Corridor_Field(house, plan, field, target, why);
        // Defer_Corridor_Field clears Target and FollowupTarget only. A target taken from
        // DeferredTarget was rejected again on every pass (log 19.11.2026 10-05-25).
        if (plan.DeferredTarget == target) plan.DeferredTarget = NoExpansionCell;
        return stop(why);
    };
    for (const auto& block : blocks) {
        if (block.Until >= 0 && near_cell(block.Target, target, CreepBlockRadius)) return reject("target-blocked-after-losses");
    }
    if (near_enemy(target, CreepEnemyTargetRadius)) return reject("enemy-base-near-target");

    // Harvesters must be able to drive from the base to the field. Checked before
    // the creep starts, so an unreachable field neither starts nor logs a creep.
    const int base_zone = Ground_Zone(Playable_Cell(house->Base.PlacementCenter));
    const int field_zone = Ground_Zone(Playable_Cell(target));
    if (base_zone >= 0 && field_zone >= 0 && base_zone != field_zone) return stop("field-in-other-zone");

    BuildingTypeClass* refinery = nullptr;
    for (BuildingTypeClass* candidate : Rule->BuildRefinery) {
        if (candidate && Resource_Placement_Role(candidate) == PlacementRole::Refinery && Can_Advanced_AI_Build_Building(candidate) &&
            Can_Supply_Refinery(house, candidate)) {
            refinery = candidate;
            break;
        }
    }
    if (!refinery) return stop("no-buildable-refinery");

    if (creep.Target != target) {
        if (creep.Target != NoExpansionCell) stop("target-changed");
        creep.Target = target;
        DEV_DEBUG_INFO("AF-0009U: house={} frame={} expansion-creep started target={},{}\n", id, Frame, target.X, target.Y);
    }
    auto* power = const_cast<BuildingTypeClass*>(Select_Advanced_AI_Power_Plant());

    /**
     *  AF-0009U: With more spare power than the plant would add, a creep step
     *  only has to extend the base, so the cheapest standalone power plant is used.
     */
    if (power) {
        const long long available = static_cast<long long>(house->Power) - house->Drain - house->PowerSurplus;
        if (available >= static_cast<long long>(power->Power) - power->Drain) {
            for (BuildingTypeClass* candidate : BuildingTypes) {
                if (!candidate || !candidate->PowersUpBuilding.empty() || candidate->Power <= candidate->Drain ||
                    !candidate->IsBase || Extension::Fetch(candidate)->IsNaval || !Can_Advanced_AI_Build_Building(candidate)) continue;
                if (candidate->Cost_Of(house) < power->Cost_Of(house)) power = candidate;
            }
        }
    }

    // Re-evaluate the sites at most every ProbeInterval frames.
    if (creep.LastProbe < 0 || Frame - creep.LastProbe >= ProbeInterval || (!refinery_only && !creep.FullProbe)) {
        creep.LastProbe = Frame;
        creep.FullProbe = !refinery_only;
        creep.Type = STRUCT_NONE;
        creep.Site = NoExpansionCell;
        creep.Refinery = false;

        // How close does the base already get?
        int edge = std::numeric_limits<int>::max();
        for (const BuildingClass* building : Buildings) {
            if (!Existing_Building(building) || building->House != house || !building->Class->IsBase ||
                Defensive_Building(building->Class) || building->Class->IsWall) continue;
            edge = std::min(edge, Distance_To_Footprint(target, building->Get_Cell(), building->Class));
        }

        Rect area = Base_Footprint_Rect(house);
        area.X -= SearchMargin;
        area.Y -= SearchMargin;
        area.Width += 2 * SearchMargin;
        area.Height += 2 * SearchMargin;
        const auto closest_site = [&](BuildingTypeClass* type, bool is_refinery, int& distance) {
            Cell best = NoExpansionCell;
            distance = std::numeric_limits<int>::max();
            if (!type) return best;
            for (int y = area.Y; y < area.Y + area.Height; ++y) {
                for (int x = area.X; x < area.X + area.Width; ++x) {
                    const Cell site(static_cast<short>(x), static_cast<short>(y));
                    if (!Playable_Cell(site)) continue;
                    const int d = Distance_To_Footprint(target, site, type);
                    if (d >= distance) continue;
                    if (near_enemy(site, CreepEnemySiteRadius)) continue;
                    if (!Creep_Site_Allowed(house, type, site, is_refinery, base_zone, field_zone)) continue;
                    distance = d;
                    best = site;
                }
            }
            return best;
        };

        int refinery_distance = 0;
        const Cell refinery_site = closest_site(refinery, true, refinery_distance);
        int power_distance = 0;
        const Cell power_site = refinery_only ? NoExpansionCell : closest_site(power, false, power_distance);

        const bool close_enough = refinery_site != NoExpansionCell && refinery_distance <= CloseEnough;
        const bool stalled = !refinery_only && (power_site == NoExpansionCell || power_distance >= edge);
        if (refinery_site != NoExpansionCell && (close_enough || stalled || creep.Steps >= MaxSteps)) {
            creep.Type = refinery->HeapID;
            creep.Site = refinery_site;
            creep.Refinery = true;
            creep.Reason = close_enough ? "expansion-creep-refinery" : creep.Steps >= MaxSteps ? "expansion-creep-refinery-step-limit" :
                "expansion-creep-refinery-closest";
        } else if (!refinery_only && power_site != NoExpansionCell && power_distance < edge) {
            creep.Type = power->HeapID;
            creep.Site = power_site;
            creep.Reason = "expansion-creep-step";
        }
        DEV_DEBUG_INFO("AF-0009U: house={} frame={} expansion-creep probe target={},{} edge={} refinery-site={},{} refinery-distance={} "
            "power-site={},{} power-distance={} steps={} choice={}\n", id, Frame, target.X, target.Y, edge,
            refinery_site.X, refinery_site.Y, refinery_site != NoExpansionCell ? refinery_distance : -1,
            power_site.X, power_site.Y, power_site != NoExpansionCell ? power_distance : -1, creep.Steps,
            creep.Type != STRUCT_NONE ? BuildingTypes[creep.Type]->Name() : "none");
    }

    if (creep.Type == STRUCT_NONE || creep.Site == NoExpansionCell) return nullptr;
    if (refinery_only && !creep.Refinery) return nullptr;

    BuildingTypeClass* type = BuildingTypes[creep.Type];
    const long long cost = type->Cost_Of(house);
    if (cost < 0) return nullptr;
    if (Advanced_AI_Uncommitted_Funds() < cost) {
        Request_Advanced_AI_Savings(cost, "expansion-creep", true);
        return nullptr;
    }
    if (creep.Refinery) Request_Advanced_AI_Savings(cost, "expansion-creep", true);
    reason = creep.Reason;
    return type;
}

Cell HouseClassExtension::Advanced_AI_Expansion_Target() const
{
    const auto found = ExpansionPlans.find(static_cast<int>(This_Const()->HeapID));
    if (found == ExpansionPlans.end()) return NoExpansionCell;
    const auto& plan = found->second;
    return plan.Type == STRUCT_NONE && plan.FollowupTarget != NoExpansionCell ? plan.FollowupTarget : plan.Target;
}

const char* HouseClassExtension::Update_Advanced_AI_Expansion()
{
    HouseClass* house = This();
    const auto found = ExpansionPlans.find(static_cast<int>(house->HeapID));
    if (found == ExpansionPlans.end()) return nullptr;
    auto& plan = found->second;
    if (plan.DevelopmentOrder != house->BuildStructure) plan.DevelopmentOrder = STRUCT_NONE;
    if (plan.Type == STRUCT_NONE) return nullptr;
    if (plan.AwaitingService) {
        for (const BuildingClass* building : Buildings) {
            if (!building || building->ID != plan.ProductID || building->House != house ||
                !Existing_Building(building)) continue;
            if (!Operational_Refinery(building)) {
                if (!Due(plan.OrderFrame, ExpansionRetryInterval)) return nullptr;
                break;
            }
            const auto stock = Read_Expansion_Stock(house, plan.Target);
            const Cell site = building->Get_Cell();
            const bool same_zone = stock.Zone >= 0 && Building_Access_Zones(building).count(stock.Zone);
            int route = std::numeric_limits<int>::max();
            const long long in_range = Refinery_Service_Value(stock, Find_Harvester_Routes(house, building->Class, site),
                false, route, plan.AllowLongRoute);
            if (stock.Value > 0 && (plan.RelaxedStock || plan.AllowLongRoute || stock.Uncovered * 2 <= stock.Value) &&
                same_zone && in_range > 0) {
                DEV_DEBUG_INFO("AF-0009B2b: house={} served target={},{} refinery={},{} value={}\n",
                    static_cast<int>(house->HeapID), plan.Target.X, plan.Target.Y,
                    plan.Site.X, plan.Site.Y, stock.Value - stock.Uncovered);
                DEV_DEBUG_INFO("AF-0009B2b2: house={} served target={},{} site={},{} covered={} value={}\n",
                    static_cast<int>(house->HeapID), plan.Target.X, plan.Target.Y, site.X, site.Y,
                    stock.Value - stock.Uncovered, stock.Value);
                if (plan.AllowLongRoute) {
                    DEV_DEBUG_INFO("AF-0009: house={} refinery-followup-long-route site={},{} target={},{} steps={} reachable-value={}\n",
                        static_cast<int>(house->HeapID), site.X, site.Y, plan.Target.X, plan.Target.Y, route, in_range);
                }
                if (plan.FollowupTarget == plan.Target) Clear_Expansion_Followup(plan);
                Clear_Expansion_Order(plan);
                plan.Target = NoExpansionCell;
                plan.LastProbe = Frame;
                return nullptr;
            }
            const char* reason = Stock_Blocker(stock);
            if (stock.Value > 0) {
                reason = !same_zone ? "refinery-zone-mismatch" :
                    in_range <= 0 ? "no-stock-in-service-radius" : "coverage-below-half";
            }
            DEV_DEBUG_INFO("AF-0009B2b1b: house={} frame={} phase=service reason={} type={} product-id={} "
                "target={},{} seed={},{} site={},{} seed-distance={} zone={} same-zone={} radius={} "
                "value={} uncovered={} in-range={}\n",
                static_cast<int>(house->HeapID), Frame, reason, building->Class->Name(), building->ID,
                plan.Target.X, plan.Target.Y, stock.Seed.X, stock.Seed.Y, site.X, site.Y,
                stock.Zone >= 0 ? Grid_Distance(stock.Seed, site) : -1, stock.Zone, same_zone,
                RefineryServiceRadius, stock.Value, stock.Uncovered, in_range);
            Defer_Expansion(house, plan, "completed-refinery-does-not-serve-target");
            return nullptr;
        }
        Defer_Expansion(house, plan, "refinery-lost-or-buildup-timeout");
        return nullptr;
    }

    if (plan.ProductID >= 0) {
        for (const FactoryClass* factory : Factories) {
            if (!factory || factory->House != house) continue;
            const TechnoClass* product = factory->Get_Object();
            if (product && product->RTTI == RTTI_BUILDING && product->ID == plan.ProductID) return nullptr;
        }
        const bool release = plan.ExtraRefinery && house->BuildStructure == plan.Type && !Has_Advanced_AI_Building_Production();
        Defer_Expansion(house, plan, "yard-or-product-lost");
        return release ? "yard-or-product-lost" : nullptr;
    }

    if (house->BuildStructure != plan.Type) {
        // B1 may replace an unstarted order with a needed power plant.
        DEV_DEBUG_INFO("AF-0009B2b: house={} order-released reason=native-order-changed\n", static_cast<int>(house->HeapID));
        Clear_Expansion_Order(plan);
    } else if (house->ConstructionYards.Count() == 0 || Due(plan.OrderFrame) ||
        (plan.ExtraRefinery && !Has_Expansion_Yard(*this, BuildingTypes[plan.Type]))) {
        const bool release = plan.ExtraRefinery && !Has_Advanced_AI_Building_Production();
        Defer_Expansion(house, plan, "yard-unavailable-or-start-timeout");
        if (release) return "yard-unavailable-or-start-timeout";
    }
    return nullptr;
}

namespace
{
    struct ExpansionAttempt
    {
        const BuildingTypeClass* Choice = nullptr;
        const char* Reason = "no-buildable-refinery";
        bool HoldTarget = false;
        bool RetrySite = false;
        int FailedCorridorField = -1;
        bool PendingFrontier = false;
    };

    ExpansionAttempt Probe_Expansion_Target(HouseClassExtension& extension, ExpansionPlan& plan,
        const Cell& target, const BuildingTypeClass* infrastructure, bool relaxed, AdvancedAIPowerGoal* goal,
        const BuildingTypeClass* required_refinery = nullptr)
    {
        HouseClass* house = extension.This();
        const int id = static_cast<int>(house->HeapID);
        const auto* frontier = plan.Frontier.Target == target ? &plan.Frontier : nullptr;
        const bool finish_expansion = !infrastructure && ((plan.FollowupTarget == target && plan.FollowupAdvanced) ||
            (frontier && frontier->LongRoute));
        const auto stock = Read_Expansion_Stock(house, target, finish_expansion);
        if (const char* reason = Stock_Blocker(stock, relaxed)) return {nullptr, reason};
        if (!stock.HasRefinery) return {nullptr, "no-operational-refinery", true};
        const int field = Survey_Field_ID(stock.Seed);
        const int selected_field = Survey_Field_ID(target);
        if (frontier && selected_field >= 0 && field != selected_field) {
            if (!infrastructure) Abandon_Refinery_Frontier(house, plan, "refinery-frontier-field-changed");
            return {nullptr, "refinery-frontier-field-changed"};
        }
        if (!infrastructure && Expansion_Field_Deferred(plan, field)) return {nullptr, "field-on-corridor-cooldown"};
        const bool frontier_in_range = frontier && In_Native_AI_Build_Range(house, BuildingTypes[frontier->Type], frontier->Site);
        const auto* frontier_step = infrastructure && frontier && !frontier_in_range ? frontier : nullptr;
        const bool corridor_allowed = !infrastructure && !frontier && (plan.FollowupTarget == target || plan.Target == target) &&
            field >= 0 && (selected_field < 0 || field == selected_field);

        const long long funds = house->Available_Money();
        const long long uncommitted = extension.Advanced_AI_Uncommitted_Funds();
        const long long step_cost = infrastructure ? infrastructure->Cost_Of(house) : 0;
        if (infrastructure && step_cost < 0) {
            return {nullptr, "infrastructure-step-unavailable"};
        }

        const char* blocked = nullptr;
        const char* waiting = nullptr;
        const char* site_failure = nullptr;
        int sites = 0;
        int logged_rejections = 0;
        struct RefineryOption
        {
            BuildingTypeClass* Type;
            BuildingTypeClass* Refinery;
            const BuildingTypeClass* Power;
            int Cost;
            long long MinimumFunds;
            const char* WaitReason;
            bool VerifyWaiting;
            bool Shared;
            const char* NativeResult;
            std::set<int> Examined;
        };
        std::vector<RefineryOption> corridor_options;
        const auto accept_site = [&](const RefineryOption& option, const SiteSearch& search) -> ExpansionAttempt {
            if (plan.Target != target) {
                DEV_DEBUG_INFO("AF-0009B2b: house={} target={},{} value={}\n", id, target.X, target.Y, stock.Uncovered);
            }
            plan.Target = target;
            plan.FailedSearches = 0;
            if (!infrastructure && frontier) {
                if (search.Site == frontier->Site) {
                    DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} target={},{} site={},{} "
                        "state=frontier-ready type={} steps={}\n", id, Frame, target.X, target.Y, search.Site.X,
                        search.Site.Y, option.Refinery->Name(), frontier->Steps);
                }
                plan.Frontier = {};
                if (plan.FollowupTarget != target && !plan.FollowupSiteVerified) Clear_Expansion_Followup(plan);
            }
            if (!infrastructure) Keep_Expansion_Followup(plan, target, true);
            if (option.VerifyWaiting) {
                plan.FollowupSite = search.Site;
                plan.FollowupRefinery = option.Refinery->HeapID;
                plan.FollowupCorridor = search.Corridor;
                plan.FollowupLongRoute = search.LongRoute;
                plan.FollowupMinimumFunds = option.MinimumFunds;
                return {nullptr, option.WaitReason, true};
            }
            if (option.Power) {
                // A power order is justified only after proving a usable refinery site.
                plan.FollowupSite = search.Site;
                plan.FollowupRefinery = option.Refinery->HeapID;
                plan.FollowupPower = option.Power->HeapID;
                plan.FollowupLongRoute = search.LongRoute;
                plan.FollowupCorridor = search.Corridor;
                plan.DevelopmentMinimum = option.MinimumFunds;
                if (goal) *goal = {option.Refinery, false, true};
                DEV_DEBUG_INFO("AF-0009B2b2: house={} need=power-for-refinery type={} refinery={} target={},{} "
                    "refinery-site={},{} minimum={} uncommitted={}\n", id, option.Power->Name(), option.Refinery->Name(),
                    target.X, target.Y, search.Site.X, search.Site.Y, option.MinimumFunds, uncommitted);
                return {option.Power, "power-for-verified-refinery"};
            }

            plan.Site = search.Site;
            plan.Type = option.Type->HeapID;
            plan.OrderFrame = Frame;
            plan.MinimumStock = option.Cost;
            plan.ExtraRefinery = !infrastructure;
            plan.RelaxedStock = relaxed;
            plan.SharedStock = option.Shared;
            plan.AllowLongRoute = search.LongRoute;
            plan.CorridorSite = search.Corridor;
            DEV_DEBUG_INFO("AF-0009B2b: house={} queued={} role={} target={},{} site={},{} uncovered={}\n",
                id, option.Type->Name(), plan.ExtraRefinery ? "refinery" :
                    (Resource_Placement_Role(option.Type) == PlacementRole::Power ? "needed-power-step" : "needed-infrastructure-step"),
                target.X, target.Y, plan.Site.X, plan.Site.Y, stock.Uncovered);
            DEV_DEBUG_INFO("AF-0009B2b2: house={} type={} target={},{} site={},{} covered-after={} value={} "
                "uncovered={} uncommitted={} cost={} refinery-reserve={}\n",
                id, option.Type->Name(), target.X, target.Y, plan.Site.X, plan.Site.Y, search.CoveredAfter, stock.Value,
                stock.Uncovered, uncommitted, option.Type->Cost_Of(house), option.Cost);
            return {option.Type, infrastructure ? "needed-infrastructure-step" : search.Corridor ? search.Reason :
                search.LongRoute ? "refinery-followup-long-route" : "refinery-site-verified"};
        };
        std::vector<BuildingTypeClass*> refineries;
        if (Rule->BuildRefinery.Count() > 0) refineries.assign(Rule->BuildRefinery.begin(), Rule->BuildRefinery.end());
        if (!infrastructure && frontier_in_range) {
            const auto preferred = std::find(refineries.begin(), refineries.end(), BuildingTypes[frontier->Type]);
            if (preferred != refineries.end()) std::rotate(refineries.begin(), preferred, preferred + 1);
        }
        for (BuildingTypeClass* refinery : refineries) {
            if ((required_refinery && refinery != required_refinery) || Resource_Placement_Role(refinery) != PlacementRole::Refinery) continue;
            const int cost = refinery->Cost_Of(house);
            // A directional placement reuses an infrastructure order already
            // chosen by the building planner. Only the refinery needs to earn back its cost.
            const long long minimum_stock = cost;
            bool shared = relaxed && stock.ServingRefineries > 0 && stock.Uncovered < minimum_stock;
            // AF-0009U: Start once the refinery cost is covered (DTA-like), see Expansion_Refinery_Minimum.
            const long long reserve_with_cost = infrastructure ? cost + step_cost : Expansion_Refinery_Minimum(cost);
            long long minimum_funds = std::max(11LL, reserve_with_cost);
            const BuildingTypeClass* power = nullptr;
            const char* reason = nullptr;
            bool budget_wait = false;
            if (!refinery->Who_Can_Build_Me(true, true, false, house)) reason = "no-eligible-yard";
            else if (!extension.Can_Advanced_AI_Build_Building(refinery)) reason = "not-buildable";
            else if (!Has_Expansion_Yard(extension, refinery)) reason = "no-eligible-yard";
            else if (extension.Is_Advanced_AI_Building_Type_Deferred(refinery)) reason = "placement-retry-cooldown";
            else if (cost <= 0 || Expansion_Stock_Value(stock, shared) < minimum_stock) blocked = reason = "insufficient-resource-value";
            else if (!Can_Supply_Refinery(house, refinery)) waiting = reason = "no-compatible-harvester";
            else if (funds < minimum_funds || uncommitted < minimum_funds) {
                budget_wait = true;
                waiting = reason = funds < minimum_funds ? "insufficient-credits" : "production-commitments";
                // AF-0009U: Unit starts keep the expansion budget free.
                extension.Request_Advanced_AI_Savings(minimum_funds, "expansion-refinery", true);
            } else if (!infrastructure && !extension.Has_Advanced_AI_Power_For(refinery)) {
                power = extension.Prioritize_Advanced_AI_Power(refinery);
                if (!power || power == refinery) waiting = reason = "insufficient-power";
                else {
                    const long long power_cost = power->Cost_Of(house);
                    minimum_funds = std::max(11LL, reserve_with_cost + power_cost);
                    if (power_cost < 0 || Expansion_Stock_Value(stock, shared) < minimum_stock + power_cost) reason = "insufficient-value-for-power-and-refinery";
                    else if (uncommitted < minimum_funds) {
                        budget_wait = true;
                        waiting = reason = "insufficient-funds-for-power-and-refinery";
                        extension.Request_Advanced_AI_Savings(minimum_funds, "expansion-power-and-refinery", true);
                    }
                }
            }
            const bool matching_frontier = frontier_in_range && frontier->Type == refinery->HeapID;
            const bool budget_followup = budget_wait && !infrastructure &&
                (plan.FollowupTarget == target || (matching_frontier && !plan.FollowupSiteVerified));
            const bool verify_waiting_site = budget_followup && !plan.FollowupSiteVerified;
            if (budget_followup && plan.FollowupSiteVerified &&
                (plan.FollowupMinimumFunds == 0 || minimum_funds < plan.FollowupMinimumFunds)) {
                plan.FollowupMinimumFunds = minimum_funds;
            }
            if (reason && !verify_waiting_site) {
                if (!blocked) blocked = reason;
                if (logged_rejections++ < PlacementProbes) {
                    DEV_DEBUG_INFO("AF-0009B2b1: house={} type={} target={},{} value={} cost={} funds={} reason={}\n",
                        id, refinery->Name(), target.X, target.Y, stock.Uncovered, cost, funds, reason);
                    DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} target={},{} candidate={} rejected={} minimum={} uncommitted={}\n",
                        id, Frame, target.X, target.Y, refinery->Name(), reason, minimum_funds, uncommitted);
                    extension.Trace_Advanced_AI_Production("expansion-budget", reason, refinery, minimum_funds);
                }
                continue;
            }

            extension.Trace_Advanced_AI_Production("expansion-budget", reason ? reason : "budget-passed", refinery, minimum_funds);
            // AF-0009U: Keep the expansion budget reserved until the refinery actually starts.
            if (!infrastructure) extension.Request_Advanced_AI_Savings(minimum_funds, "expansion-refinery", true);
            BuildingTypeClass* type = infrastructure ? BuildingTypes[infrastructure->HeapID] : refinery;
            const auto role = Resource_Placement_Role(type);
            const Cell preferred = !infrastructure && plan.FollowupTarget == target && plan.FollowupRefinery == refinery->HeapID
                ? plan.FollowupSite : NoExpansionCell;
            RefineryOption option{type, refinery, power, cost, minimum_funds, reason, verify_waiting_site, shared, nullptr, {}};
            if (!infrastructure && (matching_frontier || (preferred != NoExpansionCell && plan.FollowupCorridor))) {
                const Cell site = matching_frontier ? frontier->Site : preferred;
                const auto exact = Find_Expansion_Site(house, refinery, stock, role, true, relaxed, shared,
                    finish_expansion || plan.FollowupLongRoute, site, nullptr, {target, "plan-exact", "corridor"},
                    nullptr, RefinerySiteSearch::Exact);
                if (exact.Site != NoExpansionCell) return accept_site(option, exact);
            }
            auto* examined = corridor_allowed ? &option.Examined : nullptr;
            auto search = Find_Expansion_Site(house, type, stock, role, true, relaxed, shared, finish_expansion, preferred,
                nullptr, {target, shared ? "plan-shared" : "plan"}, examined, RefinerySiteSearch::Native, frontier_step);
            if (search.Site == NoExpansionCell && relaxed && !shared && stock.ServingRefineries > 0 &&
                Expansion_Stock_Value(stock, true) >= minimum_stock) {
                shared = true;
                option.Shared = true;
                option.Examined.clear();
                search = Find_Expansion_Site(house, type, stock, role, true, relaxed, shared, false, preferred,
                    nullptr, {target, "plan-shared"}, examined, RefinerySiteSearch::Native, frontier_step);
            }
            Log_Placement_Search(house, type, target, search, "plan");
            if (search.Site != NoExpansionCell) return accept_site(option, search);
            site_failure = search.Reason;
            DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} target={},{} candidate={} rejected={} probes={} legal={}\n",
                id, Frame, target.X, target.Y, type->Name(), search.Reason, search.Probes, search.Legal);
            if (corridor_allowed && std::none_of(corridor_options.begin(), corridor_options.end(), [&](const auto& previous) {
                return previous.Type == option.Type && previous.Shared == option.Shared;
            })) {
                option.NativeResult = search.Reason;
                corridor_options.push_back(std::move(option));
            }
            if (infrastructure || ++sites >= RefineryPlacementOptions) break;
        }
        RefineryOption* best_option = nullptr;
        const RefineryOption* frontier_option = nullptr;
        SiteSearch best;
        SiteSearch provisional;
        const auto rank = [](const SiteSearch& search, const RefineryOption& option) {
            return std::make_tuple(option.VerifyWaiting, search.RouteSteps, -search.RouteValue,
                search.Site.As_Cell_Number(), option.Type->HeapID);
        };
        for (auto& option : corridor_options) {
            const Cell preferred = plan.FollowupTarget == target && plan.FollowupRefinery == option.Refinery->HeapID &&
                plan.FollowupCorridor ? plan.FollowupSite : NoExpansionCell;
            const auto search = Find_Refinery_Corridor(house, option.Type, stock, relaxed, option.Shared, finish_expansion,
                preferred, target, "plan", option.NativeResult, option.Examined,
                !required_refinery && plan.Frontier.Site == NoExpansionCell);
            if (search.Site != NoExpansionCell && (!best_option || rank(search, option) < rank(best, *best_option))) {
                best = search;
                best_option = &option;
            } else if (search.Site == NoExpansionCell) {
                site_failure = search.Reason;
            }
            if (!frontier_option && search.FrontierSite != NoExpansionCell) {
                frontier_option = &option;
                provisional = search;
            }
        }
        if (best_option) return accept_site(*best_option, best);
        if (frontier_option) {
            plan.Frontier = {target, provisional.FrontierSite, frontier_option->Refinery->HeapID, Frame, 0,
                provisional.FrontierLongRoute, frontier_option->Shared};
            if (plan.FollowupTarget == target && !plan.FollowupSiteVerified) Clear_Expansion_Followup(plan);
            DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} field={} target={},{} site={},{} "
                "state=frontier-planned type={} route-steps={} reachable-value={}\n", id, Frame, field, target.X, target.Y,
                provisional.FrontierSite.X, provisional.FrontierSite.Y, frontier_option->Refinery->Name(),
                provisional.FrontierRouteSteps, provisional.FrontierRouteValue);
            return {nullptr, "refinery-frontier-awaiting-infrastructure", false, false, -1, true};
        }
        if (!infrastructure && frontier && !frontier_in_range) {
            return {nullptr, "refinery-frontier-awaiting-infrastructure", false, false, -1, true};
        }
        if (!infrastructure && frontier && site_failure && !waiting) {
            Abandon_Refinery_Frontier(house, plan, site_failure);
            return {nullptr, site_failure};
        }
        if (!corridor_options.empty()) return {nullptr, site_failure, false, false, field};
        if (site_failure) return {nullptr, site_failure, false, true};
        if (waiting) return {nullptr, waiting, true};
        return {nullptr, blocked ? blocked : "no-buildable-refinery"};
    }
}

const BuildingTypeClass* HouseClassExtension::Plan_Advanced_AI_Expansion(const BuildingTypeClass* infrastructure,
    const char** outcome, AdvancedAIPowerGoal* goal)
{
    HouseClass* house = This();
    const auto finish = [outcome](const char* reason, const BuildingTypeClass* choice = nullptr) {
        if (outcome) *outcome = reason;
        return choice;
    };
    if (infrastructure) {
        const auto role = Resource_Placement_Role(infrastructure);
        if (role == PlacementRole::None || role == PlacementRole::Refinery) return finish("type-is-not-an-expansion-step");
    }
    const auto* analysis = Get_Advanced_AI_Resource_Analysis();
    if (IsObserver) return finish("observer-house");
    if (!analysis) return finish("no-resource-analysis");
    if (!analysis->HasBase) return finish("no-usable-base");
    auto& plan = ExpansionPlans[static_cast<int>(house->HeapID)];
    if (plan.Type != STRUCT_NONE) return finish("expansion-order-in-progress");
    std::erase_if(plan.DeferredFields, [](const auto& field) { return Due(field.second, ExpansionRetryInterval); });
    if (Due(plan.FollowupProgress, ExpansionRetryInterval) ||
        Expansion_Field_Deferred(plan, Survey_Field_ID(plan.FollowupTarget))) Clear_Expansion_Followup(plan);
    bool following = plan.FollowupTarget != NoExpansionCell;
    long& last_probe = infrastructure ? plan.LastStepProbe : plan.LastProbe;
    const bool funding_recovered = !infrastructure && following && plan.FollowupMinimumFunds > 0 &&
        Advanced_AI_Uncommitted_Funds() >= plan.FollowupMinimumFunds;
    if (!Due(last_probe) && !funding_recovered) return finish("resource-probe-cooldown");
    last_probe = Frame;
    if (!infrastructure && following) plan.FollowupMinimumFunds = 0;

    const bool relaxed = Advanced_AI_Refinery_Shortfall(true) > 0;
    const auto& frontier = plan.Frontier;
    if (frontier.Site != NoExpansionCell) {
        auto* refinery = BuildingTypes[frontier.Type];
        const bool in_range = In_Native_AI_Build_Range(house, refinery, frontier.Site);
        if (Due(frontier.LastProgress, FrontierWaitInterval) || (!in_range && frontier.Steps >= FrontierStepLimit)) {
            Abandon_Refinery_Frontier(house, plan, in_range || frontier.Steps < FrontierStepLimit ? "progress-timeout" : "step-limit");
        } else if (!infrastructure && !in_range) {
            const auto stock = Read_Expansion_Stock(house, frontier.Target, frontier.LongRoute);
            if (const char* reason = Refinery_Frontier_Blocker(house, frontier, stock, relaxed)) {
                Abandon_Refinery_Frontier(house, plan, reason);
            }
        }
        following = plan.FollowupTarget != NoExpansionCell;
    }
    std::array<Cell, PlacementTargets> targets;
    targets.fill(NoExpansionCell);
    const char* no_target = "no-resource-fields";
    size_t count = 0;
    if (following && plan.FollowupSiteVerified) targets[count++] = plan.FollowupTarget;
    if (frontier.Site != NoExpansionCell && frontier.Target != targets.front()) {
        targets[count++] = frontier.Target;
    }
    if (following && !plan.FollowupSiteVerified && plan.FollowupTarget != targets.front()) {
        targets[count++] = plan.FollowupTarget;
    }
    if (!count && !relaxed && plan.Target != NoExpansionCell && !Expansion_Field_Deferred(plan, Survey_Field_ID(plan.Target))) {
        targets[count++] = plan.Target;
    }
    if (!following || !infrastructure) {
        for (const auto* field : Rank_Expansion_Targets(house, *analysis, plan, Advanced_AI_Refinery_Reserve(),
            relaxed, infrastructure != nullptr, &no_target)) {
            if (!field) continue;
            const Cell target = Expansion_Field_Target(*field, relaxed);
            if (std::find(targets.begin(), targets.end(), target) != targets.end() ||
                (following && Survey_Field_ID(plan.FollowupTarget) == field->ID) ||
                (frontier.Site != NoExpansionCell && Survey_Field_ID(frontier.Target) == field->ID)) continue;
            targets[count++] = target;
            if (count == targets.size()) break;
        }
        if (!following && relaxed && count < targets.size() && plan.Target != NoExpansionCell &&
            std::find(targets.begin(), targets.end(), plan.Target) == targets.end() &&
            !Expansion_Field_Deferred(plan, Survey_Field_ID(plan.Target))) {
            targets[count] = plan.Target;
        }
    }
    if (targets.front() == NoExpansionCell) return finish(no_target);

    ExpansionAttempt retained;
    Cell retained_target = NoExpansionCell;
    const char* deferred_reason = nullptr;
    for (const Cell& target : targets) {
        if (target == NoExpansionCell || (!infrastructure && target != frontier.Target &&
            Expansion_Field_Deferred(plan, Survey_Field_ID(target)))) continue;
        const auto attempt = Probe_Expansion_Target(*this, plan, target, infrastructure, relaxed, goal);
        DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} phase={} target={},{} reason={} order={}\n",
            static_cast<int>(house->HeapID), Frame, infrastructure ? "infrastructure-step" : "resource",
            target.X, target.Y, attempt.Reason, attempt.Choice ? attempt.Choice->Name() : "none");
        if (attempt.Choice) {
            if (following && !infrastructure) {
                DEV_DEBUG_INFO("AF-0009B2b: house={} frame={} refinery-followup target={},{} order={} reason={}\n",
                    static_cast<int>(house->HeapID), Frame, target.X, target.Y, attempt.Choice->Name(), attempt.Reason);
            }
            return finish(attempt.Reason, attempt.Choice);
        }
        if (attempt.FailedCorridorField >= 0) {
            Defer_Corridor_Field(house, plan, attempt.FailedCorridorField, target, attempt.Reason);
            if (Survey_Field_ID(retained_target) == attempt.FailedCorridorField) retained_target = NoExpansionCell;
            following = plan.FollowupTarget != NoExpansionCell;
            deferred_reason = attempt.Reason;
            continue;
        }
        if (!infrastructure && Expansion_Field_Deferred(plan, Survey_Field_ID(target))) continue;
        if (retained_target == NoExpansionCell || (!retained.HoldTarget && !retained.RetrySite)) {
            retained = attempt;
            retained_target = target;
        }
        // Budget, first refinery and power are common to the current target.
        // Keep a valid waiting target rather than switch to a speculative site.
        if (attempt.HoldTarget || (following && target == plan.FollowupTarget)) break;
    }

    if (retained_target == NoExpansionCell) return finish(deferred_reason ? deferred_reason : no_target);
    if (retained.PendingFrontier) return finish(retained.Reason);
    if (!infrastructure) {
        if (following) {
            if (retained.HoldTarget) return finish(retained.Reason);
            if (retained.RetrySite) {
                plan.FollowupSiteVerified = false;
                plan.FollowupMinimumFunds = 0;
                return finish(retained.Reason);
            }
            Clear_Expansion_Followup(plan);
        }
        if (plan.Target != retained_target || !retained.RetrySite) plan.FailedSearches = 0;
        plan.Target = retained_target;
        if (retained.RetrySite) {
            ++plan.FailedSearches;
            DEV_DEBUG_INFO("AF-0009B2b: house={} target={},{} retry=no-native-refinery-site\n",
                static_cast<int>(house->HeapID), retained_target.X, retained_target.Y);
        } else if (retained.HoldTarget) {
            DEV_DEBUG_INFO("AF-0009B2b: house={} target={},{} waiting={}\n",
                static_cast<int>(house->HeapID), retained_target.X, retained_target.Y, retained.Reason);
        } else {
            Defer_Expansion(house, plan, retained.Reason);
        }
    }
    return finish(retained.Reason);
}

const BuildingTypeClass* HouseClassExtension::Resume_Advanced_AI_Refinery(const BuildingTypeClass* desired, AdvancedAIPowerGoal& goal)
{
    const auto found = ExpansionPlans.find(static_cast<int>(This_Const()->HeapID));
    if (found == ExpansionPlans.end()) return nullptr;
    auto& plan = found->second;
    if (plan.Type != STRUCT_NONE || plan.FollowupTarget == NoExpansionCell) return nullptr;
    const auto attempt = Probe_Expansion_Target(*this, plan, plan.FollowupTarget, nullptr,
        Advanced_AI_Refinery_Shortfall(true) > 0, &goal, desired);
    if (!attempt.Choice) {
        if (attempt.FailedCorridorField >= 0) {
            Defer_Corridor_Field(This_Const(), plan, attempt.FailedCorridorField, plan.FollowupTarget, attempt.Reason);
        } else if (!attempt.HoldTarget) {
            const Cell target = plan.FollowupTarget;
            Clear_Expansion_Followup(plan);
            plan.Target = target;
            Defer_Expansion(This_Const(), plan, attempt.Reason);
        }
        return nullptr;
    }
    plan.DevelopmentOrder = attempt.Choice->HeapID;
    plan.DevelopmentIsTechnology = false;
    // AF-0009U: The refinery floor of the expansion budget, plus a power step bought first.
    if (!goal.Type) {
        plan.DevelopmentMinimum = Expansion_Refinery_Minimum(desired->Cost_Of(This())) +
            (attempt.Choice != desired ? attempt.Choice->Cost_Of(This()) : 0);
    }
    return attempt.Choice;
}

void HouseClassExtension::Report_Advanced_AI_Building_Completed(const BuildingTypeClass* type, int product_id)
{
    Report_Advanced_AI_Power_Completed(type, product_id);
    auto& plan = ExpansionPlans[static_cast<int>(This_Const()->HeapID)];
    plan.LastTechnology = -1;
    if (type && type->HeapID == plan.FollowupPower) plan.FollowupPower = STRUCT_NONE;
    if (plan.FollowupTarget != NoExpansionCell) plan.LastProbe = -1;
}

void HouseClassExtension::Set_Advanced_AI_Development_Order(const BuildingTypeClass* type, long long minimum, bool technology)
{
    auto& plan = ExpansionPlans[static_cast<int>(This_Const()->HeapID)];
    plan.DevelopmentOrder = type->HeapID;
    plan.DevelopmentMinimum = minimum;
    plan.DevelopmentIsTechnology = technology;
}

void HouseClassExtension::Interrupt_Advanced_AI_Development()
{
    const auto found = ExpansionPlans.find(static_cast<int>(This_Const()->HeapID));
    if (found == ExpansionPlans.end()) return;
    auto& plan = found->second;
    plan.DevelopmentOrder = STRUCT_NONE;
    plan.DevelopmentProduct = -1;
    if (!plan.AwaitingService) Clear_Expansion_Order(plan);
    plan.LastProbe = -1;
    plan.LastDevelopment = -1;
    plan.LastTechnology = -1;
    if (plan.FollowupTarget != NoExpansionCell) plan.FollowupProgress = Frame;
}

Cell HouseClassExtension::Find_Advanced_AI_Yard_Wall_Site(const BuildingTypeClass* type, const Cell* placed,
    int* autofill) const
{
    if (autofill) *autofill = 0;
    if (!type->IsWall || !type->ToOverlay || !type->ToOverlay->IsWall || type->Width() != 1 || type->Height() != 1) {
        return NoExpansionCell;
    }
    HouseClass* house = This();
    std::set<int> protected_exits;
    for (const BuildingClass* building : Buildings) {
        if (!Existing_Building(building) || building->House != house) continue;
        const Cell origin = building->Get_Cell();
        Cell offset;
        if (Unit_Factory_Exit_Offset(building->Class, offset)) {
            for (int x = offset.X - 3; x <= offset.X + 1; ++x) {
                const Cell exit(static_cast<short>(origin.X + x), static_cast<short>(origin.Y + offset.Y));
                if (Playable_Cell(exit)) protected_exits.insert(exit.As_Cell_Number());
            }
        } else if (building->Class->IsRefinery) {
            const int dock = std::min(2, building->Class->Width() - 1);
            for (int y = building->Class->Height(); y <= building->Class->Height(true) + 1; ++y) {
                const Cell exit(static_cast<short>(origin.X + dock), static_cast<short>(origin.Y + y));
                if (Playable_Cell(exit)) protected_exits.insert(exit.As_Cell_Number());
            }
        }
    }

    const int range = type->ThreatRange / CELL_LEPTON_W;
    const auto is_own_wall = [&](const CellClass* cell) {
        return cell && cell->Overlay == type->ToOverlay->HeapID && cell->Owner == house->HeapID;
    };
    for (const BuildingClass* yard : house->ConstructionYards) {
        if (!Existing_Building(yard) || yard->House != house || yard->Mission == MISSION_CONSTRUCTION ||
            yard->MissionQueue == MISSION_CONSTRUCTION) continue;
        const Cell origin = yard->Get_Cell();
        const auto* yard_cell = Playable_Cell(origin);
        if (!yard_cell) continue;
        const int width = yard->Class->Width();
        const int height = yard->Class->Height(true);
        std::vector<Cell> open_ring;
        std::set<int> legal_cells;
        std::set<int> existing_walls;
        for (int y = -1; y <= height; ++y) {
            for (int x = -1; x <= width; ++x) {
                if (y != -1 && y != height && x != -1 && x != width) continue;
                Cell site(static_cast<short>(origin.X + x), static_cast<short>(origin.Y + y));
                const CellClass* cell = Playable_Cell(site);
                if (is_own_wall(cell)) existing_walls.insert(site.As_Cell_Number());
                if (!cell || cell->Height != yard_cell->Height || cell->Ramp || cell->IsUnderBridge ||
                    cell->Overlay != OVERLAY_NONE || cell->Cell_Occupier() || cell->Land_Type() == LAND_ROAD ||
                    protected_exits.count(site.As_Cell_Number()) || !type->Legal_Placement(site, house)) continue;
                open_ring.push_back(site);
                legal_cells.insert(site.As_Cell_Number());
            }
        }
        // Mirror the native cardinal scan before using it. A matching wall off
        // the ring could otherwise make Place_Wall fill an unrelated gap.
        const auto fill_count = [&](const Cell& anchor) {
            if (range <= 1 || range > std::max(MAP_CELL_W, MAP_CELL_H) || existing_walls.empty()) return 0;
            int count = 0;
            for (const auto facing : {FACING_N, FACING_E, FACING_S, FACING_W}) {
                Cell position = anchor;
                bool legal_gap = true;
                for (int distance = 0; distance < range; ++distance) {
                    position = ::Adjacent_Cell(position, facing);
                    auto* cell = Playable_Cell(position);
                    if (!cell) return 0;
                    if (is_own_wall(cell)) {
                        if (distance > 0 && (!legal_gap || !existing_walls.count(position.As_Cell_Number()))) return 0;
                        count += distance;
                        break;
                    }
                    if (!cell->Is_Clear_To_Build(type->Speed, type, house)) break;
                    legal_gap &= legal_cells.count(position.As_Cell_Number()) != 0;
                }
            }
            return count;
        };
        if (placed) {
            if (!existing_walls.count(placed->As_Cell_Number())) continue;
            if (autofill) *autofill = fill_count(*placed);
            return *placed;
        }
        if (open_ring.empty()) continue;
        Cell best = open_ring.front();
        int most_fill = 0;
        for (const Cell& site : open_ring) {
            const int fill = fill_count(site);
            if (fill <= most_fill) continue;
            most_fill = fill;
            best = site;
        }
        if (autofill) *autofill = most_fill;
        return best;
    }
    return NoExpansionCell;
}

const BuildingTypeClass* HouseClassExtension::Select_Advanced_AI_Yard_Wall() const
{
    HouseClass* house = This();
    const long long funds = Advanced_AI_Uncommitted_Funds();
    const long long reserve = Advanced_AI_Refinery_Reserve();
    for (const BuildingTypeClass* wall : Rule->ConcreteWalls) {
        if (!wall || !wall->ToOverlay || !wall->ToOverlay->IsWall || Is_Advanced_AI_Building_Type_Deferred(wall) ||
            !Can_Advanced_AI_Build_Building(wall, nullptr, false) || !Has_Advanced_AI_Power_For(wall)) continue;
        const int cost = wall->Cost_Of(house);
        if (cost < 0 || funds < reserve + cost || Find_Advanced_AI_Yard_Wall_Site(wall) == NoExpansionCell) continue;
        return wall;
    }
    return nullptr;
}

const BuildingTypeClass* HouseClassExtension::Plan_Advanced_AI_Development(const char*& reason, AdvancedAIPowerGoal& goal)
{
    HouseClass* house = This();
    auto& plan = ExpansionPlans[static_cast<int>(house->HeapID)];
    const bool review_development = Due(plan.LastDevelopment);
    const bool review_technology = Due(plan.LastTechnology, TechnologyInterval);
    const bool following = plan.FollowupTarget != NoExpansionCell || plan.Frontier.Site != NoExpansionCell;
    reason = plan.DevelopmentWaitReason;
    if (!review_development && !review_technology && !following) return nullptr;
    plan.DevelopmentOrder = STRUCT_NONE;
    plan.DevelopmentMinimum = 0;
    plan.DevelopmentIsTechnology = false;

    const char* resource = "resource-probe-cooldown";
    const char* need = "no-development-candidate";
    const BuildingTypeClass* choice = nullptr;
    if (following || review_development || (review_technology && Advanced_AI_Refinery_Shortfall(true) > 0)) {
        choice = Plan_Advanced_AI_Expansion(nullptr, &resource, &goal);
    }
    if (review_development) plan.LastDevelopment = Frame;
    if (!choice && plan.FollowupSiteVerified && plan.Type == STRUCT_NONE) {
        reason = "waiting-for-refinery-followup";
        return nullptr;
    }
    if (!choice && !review_development && !review_technology) return nullptr;

    if (choice) {
        need = choice->IsRefinery ? "resource-expansion" : "power-for-refinery";
        DEV_DEBUG_INFO("AF-0009C3a: house={} frame={} decision=resource-expansion-preferred order={} target={},{}\n",
            static_cast<int>(house->HeapID), Frame, choice->Name(), plan.Target.X, plan.Target.Y);
    } else {
        if (review_technology) {
            plan.LastTechnology = Frame;
            choice = Select_Advanced_AI_Defense(need, plan.DevelopmentMinimum, goal, true);
            if (!choice) {
                choice = Select_Advanced_AI_Technology(need, plan.DevelopmentMinimum, goal);
                plan.DevelopmentIsTechnology = choice != nullptr;
            }
            plan.DevelopmentWaitReason = choice ? "technology-recheck-pending" : need;
        }
        if (!choice && review_development) {
            choice = Select_Advanced_AI_Development(need, plan.DevelopmentMinimum, goal);
            plan.DevelopmentIsTechnology = choice && std::string_view(need) == "missing-technology-prerequisite";
            if (goal.Type) goal.Technology = plan.DevelopmentIsTechnology;
        }
        const char* step = "no-needed-infrastructure-step";
        const bool directed = choice && Plan_Advanced_AI_Expansion(choice, &step);
        if (review_development && !directed && plan.Type == STRUCT_NONE && plan.FollowupTarget == NoExpansionCell && plan.FailedSearches >= 2) {
            Defer_Expansion(house, plan, resource);
        }
        if (choice && review_development) {
            DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} phase=step-result type={} reason={}\n",
                static_cast<int>(house->HeapID), Frame, choice->Name(), step);
        }
    }
    if (choice) {
        plan.DevelopmentOrder = choice->HeapID;
        // AF-0009U: The refinery cost instead of twice the cost, see the expansion budget.
        if (choice->IsRefinery) plan.DevelopmentMinimum = Expansion_Refinery_Minimum(choice->Cost_Of(house));
        else plan.DevelopmentMinimum = std::max(plan.DevelopmentMinimum,
            static_cast<long long>(choice->Cost_Of(house)) + Advanced_AI_Refinery_Reserve());
    }
    reason = choice ? need : plan.DevelopmentWaitReason;
    if (!review_development) return choice;
    DEV_DEBUG_INFO("AF-0009B2b2: house={} frame={} need={} result={} target={},{} minimum={} uncommitted={}\n",
        static_cast<int>(house->HeapID), Frame, need, choice ? choice->Name() : "wait",
        plan.Target.X, plan.Target.Y, plan.DevelopmentMinimum, Advanced_AI_Uncommitted_Funds());
#ifdef NDEBUG
    if (!Vinifera_DeveloperMode) return choice;
#endif
    int refineries = 0;
    int infantry = 0;
    int vehicles = 0;
    int aircraft = 0;
    for (const BuildingClass* building : Buildings) {
        if (!Existing_Building(building) || building->House != house) continue;
        refineries += building->Class->IsRefinery;
        infantry += building->Class->ToBuild == RTTI_INFANTRYTYPE;
        vehicles += building->Class->ToBuild == RTTI_UNITTYPE && !Extension::Fetch(building->Class)->IsNaval;
        aircraft += building->Class->ToBuild == RTTI_AIRCRAFTTYPE;
    }
    DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} resource={} need={} result={} target={},{} "
        "yards={} refineries={} infantry-factories={} vehicle-factories={} aircraft-factories={} minimum={} uncommitted={}\n",
        static_cast<int>(house->HeapID), Frame, resource, need, choice ? choice->Name() : "wait",
        plan.Target.X, plan.Target.Y, house->ConstructionYards.Count(), refineries, infantry, vehicles, aircraft,
        plan.DevelopmentMinimum, Advanced_AI_Uncommitted_Funds());
    return choice;
}

bool HouseClassExtension::Retarget_Advanced_AI_Development(const BuildingTypeClass* desired, const BuildingTypeClass* power,
    AdvancedAIPowerGoal& goal)
{
    HouseClass* house = This();
    const int id = static_cast<int>(house->HeapID);
    const auto found = ExpansionPlans.find(id);
    if (found == ExpansionPlans.end() || found->second.DevelopmentOrder != desired->HeapID) return true;
    auto& plan = found->second;
    const long long cost = power->Cost_Of(house);
    const long long uncommitted = Advanced_AI_Uncommitted_Funds();
    const long long replaced_cost = desired->Power > desired->Drain ? std::max(0, desired->Cost_Of(house)) : 0;
    const long long minimum = plan.DevelopmentMinimum - replaced_cost + cost;
    if (cost >= 0 && uncommitted >= minimum) {
        goal.Technology = plan.DevelopmentIsTechnology;
        if (plan.Type == desired->HeapID && plan.ExtraRefinery && plan.ProductID < 0) {
            goal.ResourceExpansion = true;
            Keep_Expansion_Followup(plan, plan.Target, true);
            plan.FollowupSite = plan.Site;
            plan.FollowupRefinery = desired->HeapID;
            plan.FollowupPower = power->HeapID;
            plan.FollowupLongRoute = plan.AllowLongRoute;
            plan.FollowupCorridor = plan.CorridorSite;
        }
        plan.DevelopmentOrder = power->HeapID;
        plan.DevelopmentMinimum = minimum;
        return true;
    }

    DEV_DEBUG_INFO("AF-0009B2b2: house={} frame={} result=start-rejected type={} reason=late-power-budget minimum={} uncommitted={}\n",
        id, Frame, desired->Name(), minimum, uncommitted);
    plan.DevelopmentOrder = STRUCT_NONE;
    if (plan.Type == desired->HeapID && plan.ProductID < 0) {
        Clear_Expansion_Order(plan);
        plan.LastProbe = Frame;
    }
    return false;
}

const char* HouseClassExtension::Validate_Advanced_AI_Expansion_Start(const BuildingTypeClass* type)
{
    HouseClass* house = This();
    if (type->IsWall && type->ToOverlay && type->ToOverlay->IsWall &&
        Find_Advanced_AI_Yard_Wall_Site(type) == NoExpansionCell) return "no-legal-yard-wall-site";
    // Emergency defenses use the reduced threat reserve, matching the planner's selection.
    if (Is_Advanced_AI_Defense_Type(type) &&
        Advanced_AI_Uncommitted_Funds() < Advanced_AI_Defense_Start_Reserve(type) + type->Cost_Of(house)) return "defense-budget";
    const auto found = ExpansionPlans.find(static_cast<int>(house->HeapID));
    if (found == ExpansionPlans.end()) return nullptr;
    auto& plan = found->second;
    const bool development = plan.DevelopmentOrder == type->HeapID;
    if (development) {
        const long long uncommitted = Advanced_AI_Uncommitted_Funds();
        const char* rejection = nullptr;
        if (uncommitted < plan.DevelopmentMinimum) rejection = "production-budget";
        else if (plan.DevelopmentIsTechnology && !Has_Advanced_AI_Technology_Economy()) rejection = "tech-economy-recovery";
        if (rejection) {
            DEV_DEBUG_INFO("AF-0009B2b2: house={} frame={} result=start-rejected type={} reason={} minimum={} uncommitted={}\n",
                static_cast<int>(house->HeapID), Frame, type->Name(), rejection, plan.DevelopmentMinimum, uncommitted);
            plan.DevelopmentOrder = STRUCT_NONE;
            if (plan.Type == type->HeapID && plan.ProductID < 0) {
                if (plan.ExtraRefinery && plan.FollowupTarget == plan.Target && uncommitted < plan.DevelopmentMinimum) {
                    plan.FollowupMinimumFunds = plan.DevelopmentMinimum;
                }
                Clear_Expansion_Order(plan);
                plan.LastProbe = Frame;
            }
            return rejection;
        }
    }
    if (plan.Type != type->HeapID || plan.ProductID >= 0) return nullptr;
    const auto stock = Read_Expansion_Stock(house, plan.Target, plan.AllowLongRoute);
    const int cost = type->Cost_Of(house);
    // AF-0009U: Same floor as the expansion budget (Expansion_Refinery_Minimum).
    // Twice the cost here never started a refinery the budget had released.
    const long long refinery_minimum = Expansion_Refinery_Minimum(cost);
    // A passed reserve check already implies the cash floor when it covers
    // today's cost. Keep the cash check for an untracked or repriced order.
    const bool reserve_covers_cost = development && plan.DevelopmentMinimum >= refinery_minimum;
    const int field = Survey_Field_ID(stock.Seed);
    const int target_field = Survey_Field_ID(plan.Target);
    const bool corridor_allowed = plan.ExtraRefinery && field >= 0 && (target_field < 0 || field == target_field);
    SiteSearch search;
    if (const char* reason = Stock_Blocker(stock, plan.RelaxedStock)) search.Reason = reason;
    else if (!stock.HasRefinery) search.Reason = "no-operational-refinery";
    else if (Expansion_Stock_Value(stock, plan.SharedStock) < plan.MinimumStock) {
        search.Reason = "insufficient-resource-value";
    } else if (plan.ExtraRefinery && !Can_Supply_Refinery(house, type)) {
        search.Reason = "no-compatible-harvester";
    } else if (plan.ExtraRefinery && !reserve_covers_cost && house->Available_Money() < refinery_minimum) {
        search.Reason = "insufficient-credits";
    } else {
        std::set<int> examined;
        if (corridor_allowed && plan.CorridorSite) {
            search = Find_Expansion_Site(house, BuildingTypes[plan.Type], stock, PlacementRole::Refinery,
                true, plan.RelaxedStock, plan.SharedStock, plan.AllowLongRoute, plan.Site, nullptr,
                {plan.Target, "start-exact", "corridor"}, nullptr, RefinerySiteSearch::Exact);
        }
        if (search.Site == NoExpansionCell) {
            search = Find_Expansion_Site(house, BuildingTypes[plan.Type], stock, Resource_Placement_Role(type),
                true, plan.RelaxedStock, plan.SharedStock, plan.AllowLongRoute,
                plan.ExtraRefinery && plan.FollowupRefinery == plan.Type ? plan.Site : NoExpansionCell, nullptr, {plan.Target, "start"},
                corridor_allowed ? &examined : nullptr);
        }
        if (search.Site == NoExpansionCell && corridor_allowed) {
            search = Find_Refinery_Corridor(house, BuildingTypes[plan.Type], stock, plan.RelaxedStock, plan.SharedStock,
                plan.AllowLongRoute, plan.CorridorSite ? plan.Site : NoExpansionCell, plan.Target, "start", search.Reason, examined);
        }
    }
    Log_Placement_Search(house, type, plan.Target, search, "start");
    plan.Site = search.Site;
    if (plan.Site != NoExpansionCell) {
        plan.CorridorSite = search.Corridor;
        return nullptr;
    }

    const bool extra = plan.ExtraRefinery;
    Trace_Advanced_AI_Production("expansion-start", search.Reason, type, extra ? std::max(11LL, refinery_minimum) : 11);
    if (extra && plan.DevelopmentOrder == plan.Type) plan.DevelopmentOrder = STRUCT_NONE;
    if (search.Corridor) {
        Defer_Corridor_Field(house, plan, field, plan.Target, search.Reason);
        Clear_Expansion_Order(plan);
        plan.LastProbe = Frame;
    } else {
        Defer_Expansion(house, plan, "start-revalidation");
    }
    return extra ? search.Reason : nullptr;
}

bool HouseClassExtension::Report_Advanced_AI_Expansion_Start(const BuildingTypeClass* type, FactoryClass* factory)
{
    Report_Advanced_AI_Power_Start(type, factory);
    HouseClass* house = This();

    // AF-0009U: The refinery is paid from here on; release the expansion savings.
    if (type->IsRefinery && factory && factory->Get_Object()) {
        Clear_Advanced_AI_Economy_Savings();
    }

    const auto found = ExpansionPlans.find(static_cast<int>(house->HeapID));
    if (found == ExpansionPlans.end()) return false;
    auto& plan = found->second;
    const TechnoClass* product = factory ? factory->Get_Object() : nullptr;
    const bool has_product = product && product->RTTI == RTTI_BUILDING;
    bool release = false;
    if (plan.DevelopmentOrder == type->HeapID) {
        plan.DevelopmentOrder = STRUCT_NONE;
        if (plan.DevelopmentIsTechnology) {
            DEV_DEBUG_INFO("AF-0009C3a: house={} frame={} phase=factory-start order={} product={} production={}\n",
                static_cast<int>(house->HeapID), Frame, type->Name(), has_product ? product->ID : -1,
                has_product ? (factory->Is_Building() ? "running" : "allocated") : "start-failed");
        }
        if (has_product) {
            plan.DevelopmentProduct = product->ID;
            plan.LastDevelopmentExit = -1;
            DEV_DEBUG_INFO("AF-0009B2b2a: house={} frame={} result={} type={} product={} target={},{} site={},{}\n",
                static_cast<int>(house->HeapID), Frame, factory->Is_Building() ? "production-started" : "product-allocated",
                type->Name(), product->ID,
                plan.Target.X, plan.Target.Y, plan.Site.X, plan.Site.Y);
        } else {
            release = true;
            DEV_DEBUG_INFO("AF-0009B2b2: house={} frame={} type={} result=factory-did-not-start\n",
                static_cast<int>(house->HeapID), Frame, type->Name());
        }
    }
    if (plan.Type != type->HeapID || plan.ProductID >= 0) return release;
    if (!has_product) {
        release |= plan.ExtraRefinery;
        Defer_Expansion(house, plan, "factory-start-failed");
        return release;
    }
    plan.ProductID = product->ID;
    DEV_DEBUG_INFO("AF-0009B2b: house={} {}={} product={} site={},{}\n",
        static_cast<int>(house->HeapID), factory->Is_Building() ? "production-started" : "product-allocated",
        type->Name(), product->ID, plan.Site.X, plan.Site.Y);
    return false;
}

Cell HouseClassExtension::Find_Advanced_AI_Expansion_Placement(BuildingTypeClass* type)
{
    HouseClass* house = This();
    if (ActivePlacement.House != static_cast<int>(house->HeapID) || ActivePlacement.Type != type->HeapID) {
        return NoExpansionCell;
    }
    if (ActivePlacement.Searched) return ActivePlacement.Site;
    ActivePlacement.Searched = true;

    // AF-0009U: A base creep step goes to its precomputed site, closest to the field.
    if (const auto creep = ExpansionCreeps.find(ActivePlacement.House); creep != ExpansionCreeps.end() &&
        creep->second.Type == type->HeapID && creep->second.Site != NoExpansionCell) {
        const Cell site = creep->second.Site;
        const int base_zone = Ground_Zone(Playable_Cell(house->Base.PlacementCenter));
        const int field_zone = Ground_Zone(Playable_Cell(creep->second.Target));
        if (Creep_Site_Allowed(house, type, site, type->IsRefinery, base_zone, field_zone)) {
            ActivePlacement.Target = creep->second.Target;
            ActivePlacement.Site = site;
            ActivePlacement.Creep = true;
            return site;
        }
        // The site was taken meanwhile; probe again on the next planner pass.
        creep->second.Site = NoExpansionCell;
        creep->second.LastProbe = -1;
    }

    const auto found = ExpansionPlans.find(ActivePlacement.House);
    if (found == ExpansionPlans.end()) return NoExpansionCell;
    const auto& plan = found->second;
    const auto role = Resource_Placement_Role(type);
    const bool protect_refinery = role == PlacementRole::Power && type->HeapID == plan.FollowupPower &&
        plan.FollowupRefinery >= STRUCT_FIRST && plan.FollowupRefinery < BuildingTypes.Count() &&
        BuildingTypes[plan.FollowupRefinery] &&
        plan.FollowupTarget != NoExpansionCell && plan.FollowupSite != NoExpansionCell;
    const bool vehicle_search = role == PlacementRole::Vehicles;
    const bool relaxed = ActivePlacement.Tracked ? plan.RelaxedStock : vehicle_search &&
        (plan.FollowupTarget != NoExpansionCell || Advanced_AI_Refinery_Shortfall(true) > 0);
    const bool shared = ActivePlacement.Tracked && plan.SharedStock;
    const long long vehicle_stock = vehicle_search && !ActivePlacement.Tracked ? Advanced_AI_Refinery_Reserve() : 0;
    const auto& frontier = plan.Frontier;
    if (role != PlacementRole::None && role != PlacementRole::Refinery && !protect_refinery &&
        frontier.Site != NoExpansionCell && frontier.Steps < FrontierStepLimit &&
        !Due(frontier.LastProgress, FrontierWaitInterval) && !plan.AwaitingService &&
        !In_Native_AI_Build_Range(house, BuildingTypes[frontier.Type], frontier.Site)) {
        const auto stock = Read_Expansion_Stock(house, frontier.Target, frontier.LongRoute);
        const bool frontier_relaxed = Advanced_AI_Refinery_Shortfall(true) > 0;
        if (!Refinery_Frontier_Blocker(house, frontier, stock, frontier_relaxed)) {
            const auto search = Find_Expansion_Site(house, type, stock, role, true, frontier_relaxed,
                frontier.SharedStock, false, NoExpansionCell, nullptr, {}, nullptr, RefinerySiteSearch::Native, &frontier);
            if (search.Advances) {
                ActivePlacement.Target = frontier.Target;
                ActivePlacement.Site = search.Site;
                ActivePlacement.Advances = true;
                ActivePlacement.AdvancesRefineryFrontier = true;
                return search.Site;
            }
        }
    }
    std::array<Cell, PlacementTargets> targets;
    targets.fill(NoExpansionCell);
    targets.front() = !ActivePlacement.Tracked && plan.FollowupTarget != NoExpansionCell ? plan.FollowupTarget : plan.Target;
    if (!ActivePlacement.Tracked) {
        if (const auto* analysis = Get_Advanced_AI_Resource_Analysis()) {
            size_t index = targets.front() == NoExpansionCell ? 0 : 1;
            for (const auto* field : Rank_Expansion_Targets(house, *analysis, plan, vehicle_stock, relaxed,
                role != PlacementRole::Refinery)) {
                if (!field) continue;
                const Cell target = Expansion_Field_Target(*field, relaxed);
                if (target == targets.front()) continue;
                targets[index++] = target;
                if (index == targets.size()) break;
            }
        }
    }

    Cell spaced_site = NoExpansionCell;
    Cell spaced_target = NoExpansionCell;
    int best_spacing = -1;
    for (const Cell& target : targets) {
        if (target == NoExpansionCell) continue;
        const bool allow_long_route = ActivePlacement.Tracked && plan.AllowLongRoute;
        const auto stock = Read_Expansion_Stock(house, target, allow_long_route);
        const int field = Survey_Field_ID(stock.Seed);
        const int target_field = Survey_Field_ID(target);
        const bool corridor_allowed = ActivePlacement.Tracked && plan.ExtraRefinery && role == PlacementRole::Refinery &&
            field >= 0 && (target_field < 0 || field == target_field);
        SiteSearch search;
        if (const char* reason = Stock_Blocker(stock, relaxed)) search.Reason = reason;
        else if (ActivePlacement.Tracked && !stock.HasRefinery) search.Reason = "no-operational-refinery";
        else if (ActivePlacement.Tracked && Expansion_Stock_Value(stock, shared) < plan.MinimumStock) {
            search.Reason = "insufficient-resource-value";
        } else if (stock.Uncovered < vehicle_stock &&
            (!relaxed || stock.ServingRefineries == 0 || Expansion_Stock_Value(stock, true) < vehicle_stock)) {
            search.Reason = "insufficient-resource-value";
        } else {
            std::set<int> examined;
            if (corridor_allowed && plan.CorridorSite) {
                search = Find_Expansion_Site(house, type, stock, role, true, relaxed, shared, allow_long_route, plan.Site,
                    nullptr, {target, "exit-exact", "corridor"}, nullptr, RefinerySiteSearch::Exact);
            }
            if (search.Site == NoExpansionCell) {
                search = Find_Expansion_Site(house, type, stock, role, ActivePlacement.Tracked, relaxed, shared, allow_long_route,
                    ActivePlacement.Tracked && plan.ExtraRefinery && plan.FollowupRefinery == plan.Type ? plan.Site : NoExpansionCell,
                    protect_refinery ? &plan : nullptr, {target, "exit"}, corridor_allowed ? &examined : nullptr);
            }
            if (search.Site == NoExpansionCell && corridor_allowed) {
                search = Find_Refinery_Corridor(house, type, stock, relaxed, shared, allow_long_route,
                    plan.CorridorSite ? plan.Site : NoExpansionCell, target, "exit", search.Reason, examined);
                if (search.Site == NoExpansionCell) {
                    ActivePlacement.FailedCorridorField = field;
                    ActivePlacement.FailedCorridorReason = search.Reason;
                }
            }
        }
        ActivePlacement.Target = target;
        Log_Placement_Search(house, type, target, search, "exit");
        if (search.Site != NoExpansionCell) {
            if (vehicle_search && !search.Advances) {
                if (search.Spacing > best_spacing) {
                    spaced_site = search.Site;
                    spaced_target = target;
                    best_spacing = search.Spacing;
                }
                continue;
            }
            ActivePlacement.Site = search.Site;
            ActivePlacement.Advances = search.Advances;
            return search.Site;
        }
    }
    if (spaced_site != NoExpansionCell) {
        ActivePlacement.Site = spaced_site;
        ActivePlacement.Target = spaced_target;
        return spaced_site;
    }
    DEV_DEBUG_INFO("AF-0009B2b1: house={} type={} target={},{} reason=native-fallback\n",
        static_cast<int>(house->HeapID), type->Name(), ActivePlacement.Target.X, ActivePlacement.Target.Y);
    if (ActivePlacement.Tracked) {
        DEV_DEBUG_INFO("AF-0009B2b: house={} placement-fallback={} reason=target-or-site-changed\n",
            static_cast<int>(house->HeapID), type->Name());
    }
    return NoExpansionCell;
}

bool HouseClassExtension::Has_Advanced_AI_Refinery_Route(const BuildingTypeClass* type, const Cell& site) const
{
    if (site == NoExpansionCell) return false;
    const HouseClass* house = This_Const();
    const bool trace_site = Refinery_Trace_Enabled();
    RefineryTraceContext context{NoExpansionCell, "native-fallback"};
    RefinerySiteTrace trace;
    trace.Native = "returned-site";
    if (trace_site && ActivePlacement.Tracked && ActivePlacement.House == static_cast<int>(house->HeapID)) {
        context.Target = ActivePlacement.Target;
        trace.Field = Survey_Field_ID(context.Target);
    }
    const auto report = [&](bool accepted, const char* result) {
        if (trace_site) {
            trace.Result = result;
            Log_Refinery_Site(house, type, context, site, trace);
        }
        return accepted;
    };
    if (!Playable_Cell(site)) return report(false, "site-outside-playable-map");
    const auto protects = [&](int x, int y) {
        const Cell position(static_cast<short>(site.X + x), static_cast<short>(site.Y + y));
        CellClass* cell = Playable_Cell(position);
        TiberiumType resource;
        const char* blocker = !cell ? "outside-playable-map" : cell->IsUnderBridge ? "bridge-cell" :
            cell->Land_Type() == LAND_ROAD ? "road" : Resource_At(cell, resource) ? "tiberium" : nullptr;
        if (blocker && trace_site) {
            trace.BlockedCell = position;
            trace.FootprintBlocker = blocker;
        }
        return blocker == nullptr;
    };
    for (const Cell& offset : Occupied_Foundation(type)) {
        if (!protects(offset.X, offset.Y)) return report(false, "footprint-crosses-protected-cells");
    }
    for (int y = type->Height(); y < type->Height(true); ++y) {
        for (int x = 0; x < type->Width(); ++x) {
            if (!protects(x, y)) return report(false, "footprint-crosses-protected-cells");
        }
    }
    const auto routes = Find_Harvester_Routes(house, type, site, nullptr, trace_site ? &trace : nullptr);
    if (routes.Approach == NoExpansionCell) {
        trace.Short.Result = trace.ApproachResult;
        return report(false, trace.ApproachResult);
    }

    if (ActivePlacement.Tracked && ActivePlacement.House == static_cast<int>(house->HeapID)) {
        const auto found = ExpansionPlans.find(ActivePlacement.House);
        if (found != ExpansionPlans.end() && found->second.ExtraRefinery) {
            const auto& plan = found->second;
            const auto stock = Read_Expansion_Stock(house, plan.Target, plan.AllowLongRoute);
            if (trace_site) {
                context.Target = plan.Target;
                trace.Field = Survey_Field_ID(stock.Seed);
                trace.Zone = stock.Zone;
            }
            int nearest = std::numeric_limits<int>::max();
            const long long served = Refinery_Service_Value(stock, routes, !plan.SharedStock, nearest, false,
                trace_site ? &trace.Short : nullptr);
            const int cost = type->Cost_Of(This());
            trace.Cost = cost;
            if (trace_site) trace.Short.Result = Refinery_Route_Result(trace, trace.Short);
            if (served > 0 && Serves_Expansion_Stock(stock, served, cost, plan.RelaxedStock, plan.SharedStock)) {
                trace.LongPolicy = "not-needed";
                return report(true, "eligible-short-route");
            }
            if (served > 0) trace.Short.Result = Refinery_Stock_Rejection(served, cost, plan.SharedStock);
            if (!plan.AllowLongRoute || stock.ServingRefineries || stock.HasLongRouteRefinery) {
                trace.LongPolicy = !plan.AllowLongRoute ? "not-enabled-for-target" :
                    stock.ServingRefineries ? "field-already-served" : "existing-long-route-refinery";
                return report(false, trace.Short.Result);
            }
            trace.LongPolicy = "checked";
            const long long reachable = Refinery_Service_Value(stock, routes, true, nearest, true,
                trace_site ? &trace.Long : nullptr);
            if (trace_site) trace.Long.Result = Refinery_Route_Result(trace, trace.Long);
            const bool accepted = reachable > 0 && reachable >= cost;
            if (reachable > 0 && !accepted) trace.Long.Result = "reachable-stock-below-refinery-cost";
            return report(accepted, accepted ? "eligible-long-route" : trace.Long.Result);
        }
    }

    trace.FirstHitOnly = true;
    for (int y = -RefineryServiceRadius; y <= RefineryServiceRadius; ++y) {
        for (int x = -RefineryServiceRadius; x <= RefineryServiceRadius; ++x) {
            const Cell cell(static_cast<short>(routes.Approach.X + x), static_cast<short>(routes.Approach.Y + y));
            const int steps = routes.Distance_To(cell);
            if (steps < 0) continue;
            CellClass* sample = Playable_Cell(cell);
            TiberiumType resource;
            if (!Resource_At(sample, resource)) continue;
            const long long value = Resource_Value(sample, resource);
            if (value > 0) {
                if (trace_site) {
                    trace.Field = Survey_Field_ID(cell);
                    trace.Short.Result = "first-reachable-positive-resource";
                    trace.Short.UsableSteps = steps;
                    trace.Short.UsableValue = value;
                }
                return report(true, "eligible-short-route");
            }
        }
    }
    trace.Short.Result = "no-positive-resource-found-on-checked-routes";
    return report(false, "not-determined-within-bounded-search");
}

bool HouseClassExtension::Is_Advanced_AI_Defense_Type(const BuildingTypeClass* type) const
{
    return type && !type->IsWall && !type->IsGate && type != Rule->WallTower &&
        type->PowersUpBuilding.empty() && Defensive_Building(type);
}

Cell HouseClassExtension::Find_Advanced_AI_Defense_Site(BuildingTypeClass* type, const Cell* required_focus, int range,
    const AdvancedAIDefenseFocus* threat) const
{
    const HouseClass* house = This_Const();
    AdvancedAIDefenseFocus ordered_focus;
    const bool threat_placement = !required_focus && Advanced_AI_Ordered_Defense_Focus(type, ordered_focus);
    if (threat_placement) threat = &ordered_focus;
    if (threat) {
        required_focus = &threat->Asset;
        range = threat->Range;
        if (range <= 0) {
            if (threat_placement) DEV_DEBUG_INFO("AF-0009-DEFENSE-PREVENTIVE: house={} frame={} decision=placement-deferred "
                "defense={} reason=no-current-threat asset={},{}\n", static_cast<int>(house->HeapID), Frame,
                type->Name(), ordered_focus.Asset.X, ordered_focus.Asset.Y);
            if (threat_placement) Retry_Advanced_AI_Defense_Placement(type);
            return NoExpansionCell;
        }
    }
    const Cell base = house->Base.PlacementCenter;
    if (!Playable_Cell(base) || base == NoExpansionCell || house->Base.InnerCells.Count() <= 0 ||
        house->Base.InnerCells.Count() > MaximumPlacementAnchors) return NoExpansionCell;

    std::set<int> approaches;
    std::set<int> factory_exits;
    std::set<int> refinery_approaches;
    struct RefineryAccess
    {
        const BuildingClass* Building;
        Cell Dock;
        std::optional<std::vector<Cell>> Reachable;
    };
    std::vector<RefineryAccess> refinery_access;
    for (const BuildingClass* building : Buildings) {
        if (!Existing_Building(building) || building->House != house || Defensive_Building(building->Class)) continue;
        const auto zones = Building_Access_Zones(building);
        approaches.insert(zones.begin(), zones.end());
        Cell exit;
        if (Unit_Factory_Exit_Offset(building->Class, exit)) {
            const int outside = threat ? std::max<int>(exit.X + 1, building->Class->Width()) : exit.X + 1;
            for (int x = exit.X; x <= outside; ++x) {
                const Cell lane = building->Get_Cell() + Cell(static_cast<short>(x), exit.Y);
                if (Playable_Cell(lane)) factory_exits.insert(lane.As_Cell_Number());
            }
        }
        if (threat && building->Class->IsRefinery) {
            const Cell dock = building->Get_Cell() + Refinery_Approach_Offset(building->Class->Width(), building->Class->Height());
            for (short y = 0; y <= 2; ++y) {
                const Cell lane = dock + Cell(0, y);
                if (Playable_Cell(lane)) refinery_approaches.insert(lane.As_Cell_Number());
            }
            if (Operational_Refinery(building)) refinery_access.push_back({building, dock, std::nullopt});
        }
    }
    if (approaches.empty()) return NoExpansionCell;
    const auto hostile = [&](const HouseClass* owner) {
        return owner && owner != house && !owner->IsDefeated && owner->Class &&
            !owner->Class->IsMultiplayPassive && !house->Is_Ally(owner);
    };
    Cell enemy = NoExpansionCell;
    auto enemy_rank = std::make_tuple(2, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    const auto enemy_unit_near = [&]() { return enemy != NoExpansionCell && std::get<0>(enemy_rank) == 0; };
    const auto consider_enemy = [&](const Cell& cell, int priority) {
        const auto rating = std::make_tuple(priority, Grid_Distance(base, cell), cell.As_Cell_Number());
        if (rating >= enemy_rank) return;
        enemy_rank = rating;
        enemy = cell;
    };
    const auto consider_unit = [&](const auto* unit) {
        if (!unit || !unit->IsActive || unit->IsInLimbo || unit->Strength <= 0 || !unit->PrimaryWeapon || !hostile(unit->House)) return;
        const Cell cell = unit->Get_Cell();
        if (Grid_Distance(base, cell) <= 20 && approaches.count(Ground_Zone(Playable_Cell(cell)))) consider_enemy(cell, 0);
    };
    if (!threat) {
        for (const auto* unit : Units) consider_unit(unit);
        for (const auto* infantry : Infantry) consider_unit(infantry);
        if (enemy == NoExpansionCell) {
            for (const BuildingClass* building : Buildings) {
                if (!Existing_Building(building) || !hostile(building->House)) continue;
                const auto zones = Building_Access_Zones(building);
                if (std::any_of(zones.begin(), zones.end(), [&](int zone) { return approaches.count(zone); })) {
                    consider_enemy(building->Get_Cell(), 1);
                }
            }
        }
    }

    const auto offset = [](const Cell& cell, const Cell& direction, int distance) {
        return Cell(static_cast<short>(cell.X + direction.X * distance), static_cast<short>(cell.Y + direction.Y * distance));
    };
    const auto open_depth = [&](const Cell& aim, const Cell& direction) {
        const CellClass* previous = nullptr;
        int depth = 0;
        for (; depth < 8; ++depth) {
            auto* cell = Playable_Cell(offset(aim, direction, depth));
            if (!cell || cell->Cell_Building() || !approaches.count(Ground_Zone(cell))) break;
            if (previous) {
                const int rise = cell->Height - previous->Height;
                if (std::abs(rise) > 1 || (rise > 0 && !previous->Ramp) || (rise < 0 && !cell->Ramp)) break;
            }
            previous = cell;
        }
        return depth;
    };
    struct Approach {
        Cell Aim;
        Cell Direction;
        int OpenDepth;
        bool Frontier = false;
        bool AssetAim = false;
        Cell Origin = Cell(0, 0);
    };
    const bool heavy = Rule->BuildPDefense.Is_Present(type) && !Rule->BuildAA.Is_Present(type);
    const Cell threatened = required_focus ? *required_focus : Advanced_AI_Defense_Focus(type);
    const Cell focus = Playable_Cell(threatened) && threatened != NoExpansionCell ? threatened : enemy;
    const bool focus_threat = focus == threatened && threatened != NoExpansionCell;
    std::vector<Approach> ranked;
    Cell intercept = focus;
    if (threat) {
        const int x = threat->Enemy.X - threatened.X;
        const int y = threat->Enemy.Y - threatened.Y;
        const Cell direction(static_cast<short>((x > 0) - (x < 0)), static_cast<short>((y > 0) - (y < 0)));
        const Cell flank(static_cast<short>(-direction.Y), direction.X);
        intercept = threat->Intercept();
        for (const Cell& aim : {intercept, offset(intercept, flank, 2), offset(intercept, flank, -2),
            offset(intercept, direction, 2), offset(threatened, direction, 1), threatened}) {
            if (Playable_Cell(aim)) ranked.push_back({aim, direction, open_depth(aim, direction)});
        }
    } else {
        for (int facing = FACING_FIRST; facing < FACING_COUNT; ++facing) {
            const Cell direction = ::Adjacent_Cell(Cell(0, 0), static_cast<FacingType>(facing));
            for (const int radius : {6, 3}) {
                const Cell aim = offset(base, direction, radius);
                const int depth = open_depth(aim, direction);
                if (depth >= 3) {
                    ranked.push_back({aim, direction, depth});
                    break;
                }
            }
        }
    }
    if (!threat && heavy && focus != NoExpansionCell) {
        const int base_distance = Grid_Distance(base, focus);
        std::set<int> added;
        for (const Cell& anchor : house->Base.InnerCells) {
            if (Grid_Distance(anchor, focus) >= base_distance || Grid_Distance(anchor, base) < 3) continue;
            const Cell direction(static_cast<short>(std::clamp<int>(focus.X - anchor.X, -1, 1)),
                static_cast<short>(std::clamp<int>(focus.Y - anchor.Y, -1, 1)));
            if (direction == NoExpansionCell) continue;
            for (const int gap : {1, 2, 3}) {
                const Cell aim = offset(anchor, direction, gap);
                if (!Playable_Cell(aim) || Grid_Distance(aim, focus) >= Grid_Distance(anchor, focus)) break;
                const int depth = open_depth(aim, direction);
                if (depth < 3) continue;
                if (added.insert(aim.As_Cell_Number()).second) ranked.push_back({aim, direction, depth, true});
                break;
            }
        }
    }
    /**
     *  AF-0009U: Without a threat, a recent attack or enemy units near the base,
     *  defenses are spread instead of all going to the side of the construction
     *  yard that faces the enemy. Important own buildings (construction yard,
     *  refineries, factories, power, radar, tech) with too few defenses nearby
     *  get extra aims, and sites with many defenses nearby rank lower.
     */
    const bool spread = !threat && !focus_threat && !enemy_unit_near() && RuleExtension->IsAdvancedAIDefenseSpread;
    struct SpreadAsset { Cell Center; int HalfSize; int Importance; int Coverage; };
    std::vector<SpreadAsset> spread_assets;
    std::vector<Cell> own_defenses;
    int asset_aims = 0;
    if (spread) {
        for (const BuildingClass* building : Buildings) {
            if (!Existing_Building(building) || building->House != house) continue;
            const Cell center(static_cast<short>(building->Get_Cell().X + building->Class->Width() / 2),
                static_cast<short>(building->Get_Cell().Y + building->Class->Height() / 2));
            if (Is_Advanced_AI_Defense_Type(building->Class)) {
                own_defenses.push_back(center);
                continue;
            }
            if (building->Class->IsWall || Defensive_Building(building->Class)) continue;
            const int importance = Spread_Asset_Importance(building->Class);
            if (importance > 0) {
                spread_assets.push_back({center, std::max(building->Class->Width(), building->Class->Height()) / 2,
                    importance, 0});
            }
        }
        for (auto& asset : spread_assets) {
            for (const Cell& defense : own_defenses) {
                if (asset.Center.Relative_Distance_To(defense) <= SpreadCoverRadius * SpreadCoverRadius) asset.Coverage++;
            }
        }
        std::vector<const SpreadAsset*> order;
        for (const auto& asset : spread_assets) {
            if (asset.Coverage < asset.Importance) order.push_back(&asset);
        }
        const Cell toward = focus != NoExpansionCell ? focus : base;
        std::sort(order.begin(), order.end(), [&](const SpreadAsset* a, const SpreadAsset* b) {
            return std::make_tuple(a->Coverage - a->Importance, a->Center.Relative_Distance_To(toward), a->Center.As_Cell_Number()) <
                std::make_tuple(b->Coverage - b->Importance, b->Center.Relative_Distance_To(toward), b->Center.As_Cell_Number());
        });
        for (const SpreadAsset* asset : order) {
            if (asset_aims >= SpreadAssetAims) break;
            // Face the enemy; without one, face away from the base center.
            const int dx = focus != NoExpansionCell ? focus.X - asset->Center.X : asset->Center.X - base.X;
            const int dy = focus != NoExpansionCell ? focus.Y - asset->Center.Y : asset->Center.Y - base.Y;
            Cell direction(static_cast<short>((dx > 0) - (dx < 0)), static_cast<short>((dy > 0) - (dy < 0)));
            if (direction == Cell(0, 0)) direction = Cell(1, 0);
            for (const int gap : {asset->HalfSize + 2, asset->HalfSize + 3, asset->HalfSize + 1}) {
                const Cell aim = offset(asset->Center, direction, gap);
                if (!Playable_Cell(aim)) continue;
                const int depth = open_depth(aim, direction);
                if (depth < 2) continue;
                ranked.push_back({aim, direction, depth, false, true, asset->Center});
                asset_aims++;
                break;
            }
        }
    }
    const auto rank = [&](const Approach& approach) {
        const int distance = focus == NoExpansionCell ? 0 : approach.Aim.Relative_Distance_To(intercept);
        return std::make_tuple(spread && !approach.AssetAim, !approach.Frontier, distance, -approach.OpenDepth,
            approach.Aim.As_Cell_Number());
    };
    std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    if (!threat && !heavy && !ranked.empty() && focus_threat &&
        Grid_Distance(threatened, ranked.front().Aim) > 2) {
        auto near_threat = ranked.front();
        near_threat.Aim = threatened;
        ranked.insert(ranked.begin(), near_threat);
    }
    int probes = 0;
    const auto foundation = Occupied_Foundation(type);
    if (foundation.empty()) return NoExpansionCell;
    Cell chosen = NoExpansionCell;
    Cell chosen_aim = NoExpansionCell;
    long long chosen_distance = 0;
    bool chosen_frontier = false;
    std::tuple<int, int, int, int> best_rating{};
    std::tuple<int, int, int, int> spread_rating{};
    std::set<int> tried_aims;
    const auto expansion = threat ? ExpansionPlans.find(static_cast<int>(house->HeapID)) : ExpansionPlans.end();
    std::optional<ExpansionStock> frontier_stock;
    std::optional<ExpansionStock> followup_stock;
    const auto protects_expansion = [&](const Cell& site) {
        if (expansion == ExpansionPlans.end()) return true;
        const auto& plan = expansion->second;
        const auto preserves = [&](const Cell& target, const Cell& refinery_site, StructType refinery, bool long_route,
            const RefineryFrontier* frontier, std::optional<ExpansionStock>& stock) {
            if (refinery_site == NoExpansionCell || refinery < STRUCT_FIRST || refinery >= BuildingTypes.Count() ||
                Grid_Distance(site, refinery_site) > MaximumHarvesterRoute + std::max(type->Width(), type->Height(true))) return true;
            const auto* refinery_type = BuildingTypes[refinery];
            if (!refinery_type) return true;
            if (!stock) stock = Read_Expansion_Stock(house, target, long_route);
            if (!stock->HasResource || stock->Zone < 0) return true;
            return Preserves_Refinery_Site(This(), refinery_type, refinery_site, long_route, *stock, type, site, foundation,
                frontier, true);
        };
        return preserves(plan.Frontier.Target, plan.Frontier.Site, plan.Frontier.Type, plan.Frontier.LongRoute,
            &plan.Frontier, frontier_stock) &&
            (!plan.FollowupSiteVerified || preserves(plan.FollowupTarget, plan.FollowupSite, plan.FollowupRefinery,
                plan.FollowupLongRoute, nullptr, followup_stock));
    };
    const auto preserves_working_routes = [&](const Cell& site) {
        std::set<int> occupied;
        for (auto& access : refinery_access) {
            if (Distance_To_Footprint(access.Dock, site, type) > MaximumHarvesterRoute) continue;
            const auto* refinery = access.Building;
            if (!access.Reachable) {
                access.Reachable.emplace();
                const auto baseline = Find_Harvester_Routes(house, refinery->Class, refinery->Get_Cell());
                if (baseline.Approach != NoExpansionCell) {
                    for (int y = -MaximumHarvesterRoute; y <= MaximumHarvesterRoute; ++y) {
                        for (int x = -MaximumHarvesterRoute; x <= MaximumHarvesterRoute; ++x) {
                            const Cell cell(static_cast<short>(baseline.Approach.X + x), static_cast<short>(baseline.Approach.Y + y));
                            if (baseline.Distance_To(cell, true) < 0) continue;
                            TiberiumType resource;
                            if (Resource_At(Playable_Cell(cell), resource)) access.Reachable->push_back(cell);
                        }
                    }
                }
            }
            if (access.Reachable->empty()) continue;
            if (occupied.empty()) for (const Cell& part : foundation) occupied.insert((site + part).As_Cell_Number());
            const auto remaining = Find_Harvester_Routes(house, refinery->Class, refinery->Get_Cell(), &occupied);
            if (std::any_of(access.Reachable->begin(), access.Reachable->end(), [&](const Cell& cell) {
                return remaining.Distance_To(cell, true) < 0;
            })) return false;
        }
        return true;
    };
    for (const auto& approach : ranked) {
        if (!tried_aims.insert(approach.Aim.As_Cell_Number()).second) continue;
        if (probes++ == (threat ? 6 : (heavy ? 3 * PlacementProbes : PlacementProbes) + asset_aims)) break;
        Cell site = house->Find_Build_Location(type, &Weight_Expansion_Anchor, approach.Aim.As_Cell_Number());
        if (site == NoExpansionCell || !Playable_Cell(site) || !type->Legal_Placement(site, This()) ||
            (threat && !In_Native_AI_Build_Range(house, type, site))) continue;
        const Cell center(static_cast<short>(site.X + type->Width() / 2), static_cast<short>(site.Y + type->Height() / 2));
        const long long distance_squared = static_cast<long long>(center.Relative_Distance_To(intercept)) * CELL_LEPTON_W * CELL_LEPTON_W;
        if (required_focus && (!threat || !threat->RequireContactRange) && static_cast<long long>(center.Relative_Distance_To(*required_focus)) *
            CELL_LEPTON_W * CELL_LEPTON_W > static_cast<long long>(range) * range) continue;
        const Cell side_origin = threat ? threatened : approach.AssetAim ? approach.Origin : base;
        const int dot = (center.X - side_origin.X) * approach.Direction.X + (center.Y - side_origin.Y) * approach.Direction.Y;
        if ((threat ? dot < 0 : dot <= 0) ||
            !Playable_Cell(offset(center, Cell(1, 0), 2)) || !Playable_Cell(offset(center, Cell(-1, 0), 2)) ||
            !Playable_Cell(offset(center, Cell(0, 1), 2)) || !Playable_Cell(offset(center, Cell(0, -1), 2))) continue;
        const auto protected_cell = [&](const Cell& part) {
            const Cell position = site + part;
            CellClass* cell = Playable_Cell(position);
            TiberiumType resource;
            return !cell || cell->IsUnderBridge || cell->Land_Type() == LAND_ROAD || Resource_At(cell, resource) ||
                factory_exits.count(position.As_Cell_Number()) || refinery_approaches.count(position.As_Cell_Number());
        };
        if (std::any_of(foundation.begin(), foundation.end(), protected_cell)) continue;
        if (threat) {
            if (distance_squared < static_cast<long long>(threat->MinimumRange) * threat->MinimumRange ||
                distance_squared > static_cast<long long>(range) * range ||
                (!threat->Air && threat->Direct && !Map_Defense_Line_Clear(center, intercept))) continue;
            bool blocks_access = false;
            for (int y = type->Height(); y < type->Height(true) && !blocks_access; ++y) {
                for (int x = 0; x < type->Width(); ++x) {
                    if (protected_cell(Cell(static_cast<short>(x), static_cast<short>(y)))) { blocks_access = true; break; }
                }
            }
            if (blocks_access || !protects_expansion(site) || !preserves_working_routes(site)) continue;
        }
        if (spread) {
            int depth = approach.OpenDepth;
            if (heavy) {
                const int clearance = std::max(type->Width(), type->Height()) / 2 + 1;
                depth = open_depth(offset(center, approach.Direction, clearance), approach.Direction);
                if (depth < 2) continue;
            }
            int crowd = 0;
            for (const Cell& defense : own_defenses) {
                if (center.Relative_Distance_To(defense) <= SpreadCrowdRadius * SpreadCrowdRadius) crowd++;
            }
            int gain = 0;
            for (const auto& asset : spread_assets) {
                if (asset.Coverage < asset.Importance &&
                    center.Relative_Distance_To(asset.Center) <= SpreadCoverRadius * SpreadCoverRadius) gain += asset.Importance;
            }
            const int distance = focus == NoExpansionCell ? 0 : center.Relative_Distance_To(intercept);
            const auto rating = std::make_tuple(2 * crowd - gain, distance, -depth, site.As_Cell_Number());
            if (chosen != NoExpansionCell && rating >= spread_rating) continue;
            spread_rating = rating;
        } else if (heavy || focus_threat) {
            int depth = approach.OpenDepth;
            if ((!threat && heavy) || (threat && threat->HasContact && !threat->Air && threat->Direct &&
                !threat->RequireContactRange)) {
                const int clearance = std::max(type->Width(), type->Height()) / 2 + 1;
                depth = open_depth(offset(center, approach.Direction, clearance), approach.Direction);
                if (depth < 2) continue;
            }
            const int distance = focus == NoExpansionCell ? 0 : center.Relative_Distance_To(intercept);
            const auto rating = std::make_tuple(distance, -Grid_Distance(center, base), -depth, site.As_Cell_Number());
            if (chosen != NoExpansionCell && rating >= best_rating) continue;
            best_rating = rating;
        }
        chosen = site;
        chosen_aim = approach.Aim;
        chosen_distance = distance_squared;
        chosen_frontier = approach.Frontier;
        if (!spread && !heavy && !focus_threat) break;
    }
    if (chosen != NoExpansionCell && threat_placement) {
        DEV_DEBUG_INFO("AF-0009-DEFENSE-PREVENTIVE: house={} frame={} decision=placement-site defense={} "
            "asset={},{} enemy={},{} checked-target={},{} site={},{} distance-squared={} min-range={} max-range={} current-contact={}\n",
            static_cast<int>(house->HeapID), Frame, type->Name(), threatened.X, threatened.Y, threat->Enemy.X,
            threat->Enemy.Y, intercept.X, intercept.Y, chosen.X, chosen.Y, chosen_distance, threat->MinimumRange, range,
            threat->RequireContactRange);
    } else if (threat_placement) {
        DEV_DEBUG_INFO("AF-0009-DEFENSE-PREVENTIVE: house={} frame={} decision=placement-deferred defense={} "
            "reason=no-legal-defense-site asset={},{} checked-target={},{}\n", static_cast<int>(house->HeapID), Frame,
            type->Name(), threatened.X, threatened.Y, intercept.X, intercept.Y);
        Retry_Advanced_AI_Defense_Placement(type);
    }
    if (chosen != NoExpansionCell && !required_focus) {
        DEV_DEBUG_INFO("AF-0009: house={} frame={} defense={} site={},{} approach={},{} focus={},{} frontier-probe={} "
            "spread={} spread-score={} uncovered-assets={}\n",
            static_cast<int>(house->HeapID), Frame, type->Name(), chosen.X, chosen.Y, chosen_aim.X, chosen_aim.Y,
            focus.X, focus.Y, chosen_frontier, spread, spread ? std::get<0>(spread_rating) : 0,
            std::count_if(spread_assets.begin(), spread_assets.end(), [](const auto& asset) { return asset.Coverage < asset.Importance; }));
    }
    return chosen;
}

ExitType HouseClassExtension::Exit_Advanced_AI_Expansion_Product(BuildingClass* yard, const TechnoClass* product)
{
    HouseClass* house = This();
    if (IsObserver || !product || product->RTTI != RTTI_BUILDING) return yard->Exit_Object(product);
    const auto* building = static_cast<const BuildingClass*>(product);
    const auto* type = building->Class;
    const auto role = Resource_Placement_Role(type);

    const int id = static_cast<int>(house->HeapID);
    const auto found = ExpansionPlans.find(id);
    const bool development = found != ExpansionPlans.end() && found->second.DevelopmentProduct == product->ID;
    const auto report_development_exit = [&](ExitType result, const Cell& site, bool directed) {
        if (!development) return;
        auto& plan = found->second;
        const int state = static_cast<int>(result);
        if (plan.LastDevelopmentExit != state) {
            const char* outcome = result == EXIT_NORMAL ? "placed" : (result == EXIT_TRY_AGAIN ? "placement-retry" : "placement-rejected");
            DEV_DEBUG_INFO("AF-0009B2b2: house={} frame={} result={} type={} product={} site={},{} directed={}\n",
                id, Frame, outcome, type->Name(), plan.DevelopmentProduct, site.X, site.Y, directed);
            plan.LastDevelopmentExit = state;
        }
        if (result != EXIT_TRY_AGAIN) plan.DevelopmentProduct = -1;
    };
    if (role == PlacementRole::None ||
        (role != PlacementRole::Refinery && found != ExpansionPlans.end() && found->second.LastPlacementProduct == product->ID)) {
        const bool wall = type->IsWall && type->ToOverlay && type->ToOverlay->IsWall;
        const Cell wall_site = wall ? Find_Advanced_AI_Yard_Wall_Site(type) : NoExpansionCell;
        const ExitType result = yard->Exit_Object(product);
        Cell placed = NoExpansionCell;
        if (result == EXIT_NORMAL) {
            // The native wall and upgrade conversions retire the product object.
            if (wall) {
                placed = wall_site;
                int autofill = 0;
                if (placed != NoExpansionCell) Find_Advanced_AI_Yard_Wall_Site(type, &placed, &autofill);
                if (autofill > 0) Map.Place_Wall(placed, house, BuildingTypes[type->HeapID]);
                DEV_DEBUG_INFO("AF-0009: house={} frame={} placed-yard-wall={} site={},{}\n",
                    id, Frame, type->Name(), placed.X, placed.Y);
                if (autofill > 0) DEV_DEBUG_INFO("AF-0009: house={} frame={} yard-wall-autofill-candidates={}\n", id, Frame, autofill);
            }
            else if (type->PowersUpBuilding.empty()) placed = building->Get_Cell();
        }
        report_development_exit(result, placed, wall && placed != NoExpansionCell);
        return result;
    }

    auto& plan = ExpansionPlans[id];
    plan.LastPlacementProduct = product->ID;
    const bool tracked = plan.ProductID == product->ID && !plan.AwaitingService;
    const bool native_after_failure = (!tracked || !plan.ExtraRefinery) && plan.NextNativePlacement == type->HeapID;
    if (native_after_failure) {
        plan.NextNativePlacement = STRUCT_NONE;
        DEV_DEBUG_INFO("AF-0009B2b1: house={} type={} reason=native-fallback-after-placement-failure\n",
            id, type->Name());
    }
    const auto previous = ActivePlacement;
    ActivePlacement = native_after_failure ? PlacementRequest{} :
        PlacementRequest{id, type->HeapID, plan.Target, NoExpansionCell, tracked, false};
    const ExitType result = yard->Exit_Object(product);
    const auto placement = ActivePlacement;
    ActivePlacement = previous;
    if (tracked && result != EXIT_NORMAL && placement.FailedCorridorField >= 0) {
        report_development_exit(EXIT_FAILED, NoExpansionCell, false);
        Defer_Corridor_Field(house, plan, placement.FailedCorridorField, plan.Target, placement.FailedCorridorReason);
        Clear_Expansion_Order(plan);
        plan.LastProbe = Frame;
        return EXIT_FAILED;
    }
    const Cell placed = result == EXIT_NORMAL ? building->Get_Cell() : NoExpansionCell;
    const bool directed = placement.Site != NoExpansionCell && placed == placement.Site;
    if (placement.Creep) {
        auto& creep = ExpansionCreeps[id];
        if (result == EXIT_NORMAL) {
            DEV_DEBUG_INFO("AF-0009U: house={} frame={} expansion-creep placed={} site={},{} target={},{} step={}\n",
                id, Frame, type->Name(), placed.X, placed.Y, creep.Target.X, creep.Target.Y, creep.Steps + 1);
            CreepSteps[id].push_back({building->ID, creep.Target, static_cast<long>(Frame)});
            if (role == PlacementRole::Refinery) {
                creep = {};
            } else {
                ++creep.Steps;
                creep.Type = STRUCT_NONE;
                creep.Site = NoExpansionCell;
                creep.LastProbe = -1;
            }
        } else if (result != EXIT_TRY_AGAIN) {
            creep.Site = NoExpansionCell;
            creep.LastProbe = -1;
        }
    }
    if (directed && placement.Advances && !plan.AwaitingService) {
        // The base edge moved closer to this field. Retry the native refinery checks.
        const int field = Survey_Field_ID(placement.Target);
        if (Expansion_Field_Deferred(plan, field)) {
            plan.DeferredFields.erase(field);
            plan.LastProbe = -1;
            DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} search=corridor field={} target={},{} "
                "state=reopened reason=infrastructure-advanced type={} site={},{}\n",
                id, Frame, field, placement.Target.X, placement.Target.Y, type->Name(), placed.X, placed.Y);
        }
        if (placement.AdvancesRefineryFrontier) {
            ++plan.Frontier.Steps;
            plan.Frontier.LastProgress = Frame;
            plan.LastProbe = -1;
            DEV_DEBUG_INFO("AF-0009B2-REFINERY-DIAG: house={} frame={} target={},{} site={},{} "
                "state=frontier-advanced type={} placed={},{} steps={}\n", id, Frame, placement.Target.X, placement.Target.Y,
                plan.Frontier.Site.X, plan.Frontier.Site.Y, type->Name(), placed.X, placed.Y, plan.Frontier.Steps);
        } else {
            Keep_Expansion_Followup(plan, placement.Target, false);
        }
    }
    report_development_exit(result, placed, directed);
    if (!development && tracked) {
        DEV_DEBUG_INFO("AF-0009B2b2: house={} frame={} type={} target={},{} placed={},{} directed={} exit={}\n",
            id, Frame, type->Name(), plan.Target.X, plan.Target.Y, placed.X, placed.Y, directed, static_cast<int>(result));
    }
    if (result != EXIT_NORMAL && placement.Site != NoExpansionCell && (!tracked || !plan.ExtraRefinery)) {
        // A directed failure must not repeatedly consume the first refinery or another B1 order.
        plan.NextNativePlacement = type->HeapID;
        DEV_DEBUG_INFO("AF-0009B2b1: house={} type={} site={},{} result={} next-placement=native\n",
            id, type->Name(), placement.Site.X, placement.Site.Y, static_cast<int>(result));
    }
    if (result == EXIT_NORMAL && placement.Searched) {
        DEV_DEBUG_INFO("AF-0009B2b1: house={} type={} target={},{} placed={},{} directed={} advances={} product-id={}\n",
            id, type->Name(), placement.Target.X, placement.Target.Y, placed.X, placed.Y, directed, directed && placement.Advances, product->ID);
    }
    if (!tracked) {
        if (directed && plan.Type == STRUCT_NONE &&
            (plan.Target == NoExpansionCell || plan.Target == placement.Target)) {
            if (plan.Target == NoExpansionCell) plan.Target = placement.Target;
            if (role == PlacementRole::Refinery) {
                plan.Type = type->HeapID;
                plan.ProductID = product->ID;
                plan.Site = placed;
                plan.AwaitingService = true;
                plan.OrderFrame = Frame;
            }
        }
        return result;
    }

    if (result != EXIT_NORMAL) {
        if (role == PlacementRole::Refinery && result == EXIT_TRY_AGAIN) return result;
        Defer_Expansion(house, plan, result == EXIT_TRY_AGAIN ? "native-placement-retry" : "native-placement-failed");
        return result;
    }

    DEV_DEBUG_INFO("AF-0009B2b: house={} placed={} cell={},{} directed={}\n",
        static_cast<int>(house->HeapID), building->Class->Name(), placed.X, placed.Y, directed);
    if (plan.ExtraRefinery) {
        plan.Site = placed;
        plan.AwaitingService = true;
        plan.OrderFrame = Frame;
    } else {
        Clear_Expansion_Order(plan);
        plan.LastProbe = directed && placement.Advances ? -1 : Frame;
    }
    return result;
}
