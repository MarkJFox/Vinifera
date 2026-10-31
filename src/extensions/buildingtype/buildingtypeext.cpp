/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended BuildingTypeClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "buildingtypeext.h"

#include "aircrafttype.h"
#include "buildingtype.h"
#include "ccini.h"
#include "debughandler.h"
#include "extension.h"
#include "infantrytype.h"
#include "map.h"
#include "mouse.h"
#include "radar.h"
#include "rules.h"
#include "scenario.h"
#include "tibsun_defines.h"
#include "tibsun_globals.h"
#include "unittype.h"
#include "vinifera_globals.h"
#include "wwcrc.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>

namespace
{
    bool FoundationSyncCRCValid = false;
    bool FoundationSyncCRCPresent = false;
    unsigned FoundationSyncCRC = 0;

    // Derived UI state only. Never part of the raw-saved type extension or its CRC.
    const RadarClass* FoundationRadarOwner = nullptr;
    float FoundationRadarZoom = 0.0f;
    std::map<std::pair<int, int>, DynamicVectorClass<Point2D>> FoundationRadarShapes;

    void Clear_Foundation_Radar()
    {
        FoundationRadarShapes.clear();
        FoundationRadarOwner = nullptr;
        FoundationRadarZoom = 0.0f;
    }

    void Invalidate_Foundation_Sync_CRC()
    {
        FoundationSyncCRCValid = false;
    }

    void Skip_Foundation_Whitespace(const char*& cursor)
    {
        // ASCII only: parsing must not depend on a client's C locale.
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
               *cursor == '\n' || *cursor == '\f' || *cursor == '\v') ++cursor;
    }

    bool Read_Foundation_Integer(const char*& cursor, int& value)
    {
        Skip_Foundation_Whitespace(cursor);
        const auto result = std::from_chars(cursor, cursor + std::strlen(cursor), value);
        if (result.ec != std::errc()) return false;
        cursor = result.ptr;
        Skip_Foundation_Whitespace(cursor);
        return true;
    }

    template<class T>
    bool Has_Foundation_Service_Reference(const DynamicVectorClass<T*>& types, const BuildingTypeClass* target,
        bool allow_dock = false)
    {
        for (int i = 0; i < types.Count(); ++i) {
            const auto type = types[i];
            if (type->DeploysInto == target) return true;
            for (int j = 0; !allow_dock && j < type->Dock.Count(); ++j) {
                if (type->Dock[j] == target) return true;
            }
        }
        return false;
    }
}

static_assert(sizeof(Cell) == 4 && alignof(Cell) == alignof(short), "TS foundation Cell ABI changed");

/**
 *  Class constructor.
 *
 *  @author: CCHyper
 */
BuildingTypeClassExtension::BuildingTypeClassExtension(const BuildingTypeClass *this_ptr) :
    TechnoTypeClassExtension(this_ptr),
    GateUpSound(VOC_NONE),
    GateDownSound(VOC_NONE),
    ProduceCashStartup(0),
    ProduceCashAmount(0),
    ProduceCashDelay(0),
    ProduceCashBudget(0),
    IsStartupCashOneTime(false),
    IsResetBudgetOnCapture(false),
    IsEligibleForAllyBuilding(false),
    EngineerChance(0),
    IsHideDuringSpecialAnim(false),
    RoofDeployingAnim(nullptr),
    RoofDoorAnim(nullptr),
    UnderRoofDoorAnim(nullptr),
    IsExclusiveFactory(false),
    IsWallOwner(true),
    IsBarGate(false),
    IsCustomFoundationEnabled(false),
    IsCustomFoundationValid(false),
    FoundationWidth(0),
    FoundationHeight(0),
    FoundationCellCount(0),
    CustomInfantryExitCellSet(false),
    CustomInfantryExitCell(0, 0),
    CustomAircraftDockCellSet(false),
    CustomAircraftDockCell(0, 0)
{
    Reset_Custom_Foundation();
    BuildingTypeExtensions.Add(this);
}


/**
 *  Class no-init constructor.
 *  
 *  @author: CCHyper
 */
BuildingTypeClassExtension::BuildingTypeClassExtension(const NoInitClass &noinit) :
    TechnoTypeClassExtension(noinit)
{
}


/**
 *  Class destructor.
 *  
 *  @author: CCHyper
 */
BuildingTypeClassExtension::~BuildingTypeClassExtension()
{
    Clear_Foundation_Radar();
    Invalidate_Foundation_Sync_CRC();
    BuildingTypeExtensions.Delete(this);
}


/**
 *  Retrieves the class identifier (CLSID) of the object.
 *  
 *  @author: CCHyper
 */
HRESULT BuildingTypeClassExtension::GetClassID(CLSID *lpClassID)
{
    if (lpClassID == nullptr) {
        return E_POINTER;
    }

    *lpClassID = __uuidof(this);

    return S_OK;
}


/**
 *  Initializes an object from the stream where it was saved previously.
 *  
 *  @author: CCHyper
 */
HRESULT BuildingTypeClassExtension::Load(IStream *pStm)
{
    HRESULT hr = TechnoTypeClassExtension::Load(pStm);
    if (FAILED(hr)) {
        return E_FAIL;
    }

    new (this) BuildingTypeClassExtension(NoInitClass());

    // ThisPtr is still awaiting swizzling here; validate only owned scalar/Cell data.
    if (IsCustomFoundationValid) {
        if (!IsCustomFoundationEnabled || !Validate_Custom_Foundation_Cells()) {
            DEV_DEBUG_WARNING("[CUSTOMFOUNDATION] event=REJECTED reason=invalid saved mask\n");
            return E_FAIL;
        }
    } else if (FoundationWidth != 0 || FoundationHeight != 0 || FoundationCellCount != 0) {
        return E_FAIL;
    }
    if (CustomInfantryExitCellSet) {
        const char* reason = !IsCustomFoundationValid ? "INVALID_SAVED_FOUNDATION" :
            Custom_Infantry_Exit_Cell_Error(CustomInfantryExitCell.X, CustomInfantryExitCell.Y);
        if (reason != nullptr) {
            // ThisPtr is not swizzled yet. Do not read type flags or Name().
            if (Vinifera_DeveloperMode) {
                DEBUG_WARNING("[CFINF] event=CUSTOM_EXIT_REJECT source=SAVE relative=({}, {}) reason={}\n",
                    CustomInfantryExitCell.X, CustomInfantryExitCell.Y, reason);
            }
            CustomInfantryExitCellSet = false;
        }
    }
    if (!CustomInfantryExitCellSet) CustomInfantryExitCell = Cell(0, 0);
    if (CustomAircraftDockCellSet) {
        const char* reason = !IsCustomFoundationValid ? "INVALID_SAVED_FOUNDATION" :
            Custom_Aircraft_Dock_Cell_Error(CustomAircraftDockCell.X, CustomAircraftDockCell.Y);
        if (reason != nullptr) {
            // Only owned data is safe before ThisPtr is swizzled.
            if (Vinifera_DeveloperMode) {
                DEBUG_WARNING("[CFPAD] event=DOCK_REJECT source=SAVE relative=({}, {}) reason={}\n",
                    CustomAircraftDockCell.X, CustomAircraftDockCell.Y, reason);
            }
            if (IsCustomFoundationValid &&
                !Has_Custom_Foundation_Cell(FoundationWidth / 2, FoundationHeight / 2)) return E_FAIL;
            CustomAircraftDockCellSet = false;
        }
    }
    if (!CustomAircraftDockCellSet) CustomAircraftDockCell = Cell(0, 0);
    std::fill(CustomFoundationCells + FoundationCellCount, std::end(CustomFoundationCells), REFRESH_EOL);
    Clear_Foundation_Radar();
    Invalidate_Foundation_Sync_CRC();
    
    return hr;
}


/**
 *  Saves an object to the specified stream.
 *  
 *  @author: CCHyper
 */
HRESULT BuildingTypeClassExtension::Save(IStream *pStm, BOOL fClearDirty)
{
    HRESULT hr = TechnoTypeClassExtension::Save(pStm, fClearDirty);
    if (FAILED(hr)) {
        return hr;
    }

    return hr;
}


/**
 *  Return the raw size of class data for save/load purposes.
 *  
 *  @author: CCHyper
 */
int BuildingTypeClassExtension::Get_Object_Size() const
{
    return sizeof(*this);
}




/**
 *  Compute a unique crc value for this instance.
 *  
 *  @author: CCHyper
 */
void BuildingTypeClassExtension::Object_CRC(CRCEngine &crc) const
{
    crc(static_cast<int>(IsEligibleForAllyBuilding));
    crc(static_cast<int>(IsExclusiveFactory));
    Custom_Foundation_CRC(crc);
}


/**
 *  Fetches the extension data from the INI database.  
 *  
 *  @author: CCHyper
 */
bool BuildingTypeClassExtension::Read_INI(CCINIClass &ini)
{
    if (!IsInitialized) {
        IsEligibleForAllyBuilding = IsEligibleForAllyBuilding || This()->IsConstructionYard;
        EngineerChance = This()->ToBuild == RTTI_BUILDINGTYPE ? 25 : 0;
    }

    if (!TechnoTypeClassExtension::Read_INI(ini)) {
        return false;
    }

    const char *ini_name = Name();

    GateUpSound = ini.Get_VocType(ini_name, "GateUpSound", GateUpSound);
    GateDownSound = ini.Get_VocType(ini_name, "GateDownSound", GateDownSound);

    ProduceCashStartup = ini.Get_Int(ini_name, "ProduceCashStartup", ProduceCashStartup);
    ProduceCashAmount = ini.Get_Int(ini_name, "ProduceCashAmount", ProduceCashAmount);
    ProduceCashDelay = ini.Get_Int(ini_name, "ProduceCashDelay", ProduceCashDelay);
    ProduceCashBudget = ini.Get_Int(ini_name, "ProduceCashBudget", ProduceCashBudget);
    IsStartupCashOneTime = ini.Get_Int(ini_name, "ProduceCashStartupOneTime", IsStartupCashOneTime);
    IsResetBudgetOnCapture = ini.Get_Bool(ini_name, "ProduceCashResetOnCapture", IsResetBudgetOnCapture);

    IsEligibleForAllyBuilding = ini.Get_Bool(ini_name, "EligibleForAllyBuilding", IsEligibleForAllyBuilding);
    IsHideDuringSpecialAnim = ArtINI.Get_Bool(ini_name, "HideDuringSpecialAnim", IsHideDuringSpecialAnim);

    IsExclusiveFactory = ini.Get_Bool(ini_name, "ExclusiveFactory", IsExclusiveFactory);
    IsWallOwner = ini.Get_Bool(ini_name, "WallOwner", IsWallOwner);
    IsBarGate = ini.Get_Bool(ini_name, "BarGate", IsBarGate);

    Fetch_Building_Normal_Image(Scen->Theater);

    /**
     *  ObjectTypeClass::Read_INI attempts to preload the image from a MIX file.
     *  If you mark an object type DemandLoad=yes, and then place its image in a cached MIX,
     *  the game will incorrectly attempt to delete that image later. To avoid this,
     *  null the MIX-fetched image out now.
     */
    if (This()->IsDemandLoad) {
        This()->Image = nullptr;
    }

    /**
     *  Don't try to free buildups if we don't demand load them.
     */
    if (!This()->IsDemandLoadBuildup) {
        This()->IsFreeBuildup = false;
    }

    IsInitialized = true;

    return true;
}



/**
 *  AF-0005B uses inline storage so native preview reads and raw save/load
 *  never depend on a heap pointer. Cell's default constructor does not write
 *  its components, so the NoInit constructor preserves this array on load.
 */
void BuildingTypeClassExtension::Reset_Custom_Foundation()
{
    IsCustomFoundationValid = false;
    FoundationWidth = 0;
    FoundationHeight = 0;
    FoundationCellCount = 0;
    CustomInfantryExitCellSet = false;
    CustomInfantryExitCell = Cell(0, 0);
    CustomAircraftDockCellSet = false;
    CustomAircraftDockCell = Cell(0, 0);
    std::fill(std::begin(CustomFoundationCells), std::end(CustomFoundationCells), REFRESH_EOL);
    Clear_Foundation_Radar();
    Invalidate_Foundation_Sync_CRC();
}

void BuildingTypeClassExtension::Reject_Custom_Foundation(const char* reason)
{
    Reset_Custom_Foundation();
    DEV_DEBUG_WARNING("[CUSTOMFOUNDATION] type={} event=REJECTED reason={}\n", Name(), reason);
}

void BuildingTypeClassExtension::Read_Custom_Foundation()
{
    // A loaded game's canonical mask comes from its extension snapshot, not today's ArtINI.
    if (Vinifera_PerformingLoad) return;
    Reset_Custom_Foundation();
    const char* section = This()->GraphicName.c_str();
    IsCustomFoundationEnabled = ArtINI.Get_Bool(section, "CustomFoundation", false);
    if (!IsCustomFoundationEnabled) return;

    if (This()->Size < BSIZE_FIRST || This()->Size >= BSIZE_COUNT) {
        Reject_Custom_Foundation("valid native Foundation fallback required");
        return;
    }

    // Parse into ints before any short conversion. Reject truncated strings,
    // trailing garbage, overflow and malformed pairs instead of accepting a prefix.
    char buffer[128];
    int width = 0;
    int height = 0;
    const char* dimensions[] = {"Foundation.X", "Foundation.Y"};
    int* values[] = {&width, &height};
    const int limits[] = {CustomFoundationMaxWidth, CustomFoundationMaxHeight};
    for (int i = 0; i < 2; ++i) {
        const int length = ArtINI.Get_String(section, dimensions[i], "", buffer, sizeof(buffer));
        const char* cursor = buffer;
        if (length <= 0 || length >= static_cast<int>(sizeof(buffer)) - 1 ||
            !Read_Foundation_Integer(cursor, *values[i]) || *cursor != '\0' ||
            *values[i] <= 0 || *values[i] > limits[i]) {
            Reject_Custom_Foundation("Foundation.X/Y must be in 1..64");
            return;
        }
    }

    Cell cells[CustomFoundationMaxCells];
    bool seen[CustomFoundationMaxHeight][CustomFoundationMaxWidth] = {};
    int count = 0;
    for (int i = 0; i <= CustomFoundationMaxCells; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "Foundation.%d", i);
        if (!ArtINI.Is_Present(section, key)) break;
        if (i == CustomFoundationMaxCells) {
            Reject_Custom_Foundation("more than 119 occupied cells");
            return;
        }

        const int length = ArtINI.Get_String(section, key, "", buffer, sizeof(buffer));
        const char* cursor = buffer;
        int x = 0;
        int y = 0;
        if (length <= 0 || length >= static_cast<int>(sizeof(buffer)) - 1 ||
            !Read_Foundation_Integer(cursor, x) || *cursor != ',') {
            Reject_Custom_Foundation("malformed Foundation.N coordinate");
            return;
        }
        ++cursor;
        if (!Read_Foundation_Integer(cursor, y) || *cursor != '\0') {
            Reject_Custom_Foundation("malformed Foundation.N coordinate");
            return;
        }
        if (x < 0 || x >= width || y < 0 || y >= height) {
            Reject_Custom_Foundation("Foundation.N coordinate outside bounding dimensions");
            return;
        }
        if (seen[y][x]) {
            Reject_Custom_Foundation("duplicate Foundation.N coordinate");
            return;
        }
        seen[y][x] = true;
        cells[count++] = Cell(static_cast<short>(x), static_cast<short>(y));
    }

    if (count == 0 || !seen[0][0]) {
        Reject_Custom_Foundation("nonempty mask including (0,0) required");
        return;
    }
    std::sort(cells, cells + count, [](const Cell& lhs, const Cell& rhs) {
        return lhs.Y != rhs.Y ? lhs.Y < rhs.Y : lhs.X < rhs.X;
    });
    FoundationWidth = width;
    FoundationHeight = height;
    FoundationCellCount = count;
    std::copy(cells, cells + count, CustomFoundationCells);
    // Publish only after the following complete-type scope validation.
}

bool BuildingTypeClassExtension::Validate_Custom_Foundation_Cells() const
{
    if (FoundationWidth <= 0 || FoundationWidth > CustomFoundationMaxWidth ||
        FoundationHeight <= 0 || FoundationHeight > CustomFoundationMaxHeight ||
        FoundationCellCount <= 0 || FoundationCellCount > CustomFoundationMaxCells ||
        FoundationCellCount > FoundationWidth * FoundationHeight ||
        CustomFoundationCells[0] != Cell(0, 0)) {
        return false;
    }
    int previous = -1;
    for (int i = 0; i < FoundationCellCount; ++i) {
        const Cell& cell = CustomFoundationCells[i];
        if (cell.X < 0 || cell.X >= FoundationWidth ||
            cell.Y < 0 || cell.Y >= FoundationHeight) return false;
        const int index = cell.Y * FoundationWidth + cell.X;
        if (index <= previous) return false;
        previous = index;
    }
    return CustomFoundationCells[FoundationCellCount] == REFRESH_EOL;
}

const char* BuildingTypeClassExtension::Custom_Infantry_Exit_Cell_Error(int x, int y) const
{
    if (x < -1 || x > FoundationWidth || y < -1 || y > FoundationHeight) return "OUT_OF_RANGE";
    if (x >= 0 && x < FoundationWidth && y >= 0 && y < FoundationHeight) {
        for (int i = 0; i < FoundationCellCount; ++i) {
            if (CustomFoundationCells[i].X == x && CustomFoundationCells[i].Y == y) return "OCCUPIED";
        }
    }
    return nullptr;
}

void BuildingTypeClassExtension::Read_Custom_Infantry_Exit_Cell()
{
    CustomInfantryExitCellSet = false;
    CustomInfantryExitCell = Cell(0, 0);
    if (!Is_Custom_Foundation_Infantry_Factory_Eligible()) return;
    const char* section = This_Const()->GraphicName.c_str();
    if (!ArtINI.Is_Present(section, "CustomExitCell")) return;

    char buffer[128] = {};
    const int length = ArtINI.Get_String(section, "CustomExitCell", "", buffer, sizeof(buffer));
    const char* cursor = buffer;
    int x = 0;
    int y = 0;
    bool parsed = length > 0 && length < static_cast<int>(sizeof(buffer)) - 1 &&
        Read_Foundation_Integer(cursor, x) && *cursor == ',';
    if (parsed) {
        ++cursor;
        parsed = Read_Foundation_Integer(cursor, y) && *cursor == '\0';
    }
    if (!parsed) {
        if (Vinifera_DeveloperMode) {
            DEBUG_WARNING("[CFINF] type={} event=CUSTOM_EXIT_REJECT value={} reason=MALFORMED\n", Name(), buffer);
        }
        return;
    }
    const char* reason = Custom_Infantry_Exit_Cell_Error(x, y);
    if (reason != nullptr) {
        if (Vinifera_DeveloperMode) {
            DEBUG_WARNING("[CFINF] type={} event=CUSTOM_EXIT_REJECT relative=({}, {}) reason={}\n", Name(), x, y, reason);
        }
        return;
    }
    CustomInfantryExitCell = Cell(static_cast<short>(x), static_cast<short>(y));
    CustomInfantryExitCellSet = true;
    if (Vinifera_DeveloperMode) {
        const Coord& offset = This_Const()->ExitCoordinate;
        DEBUG_INFO("[CFINF] type={} event=CUSTOM_EXIT_CONFIG relative=({}, {}) ExitCoord=({}, {}, {}) bounds=({}, {})\n",
            Name(), x, y, offset.X, offset.Y, offset.Z, FoundationWidth, FoundationHeight);
    }
}

bool BuildingTypeClassExtension::Has_Custom_Foundation_Cell(int x, int y) const
{
    for (int i = 0; i < FoundationCellCount; ++i) {
        if (CustomFoundationCells[i].X == x && CustomFoundationCells[i].Y == y) return true;
    }
    return false;
}

const char* BuildingTypeClassExtension::Custom_Aircraft_Dock_Cell_Error(int x, int y) const
{
    if (x < 0 || x >= FoundationWidth || y < 0 || y >= FoundationHeight) return "OUT_OF_RANGE";
    return Has_Custom_Foundation_Cell(x, y) ? nullptr : "NOT_OCCUPIED";
}

bool BuildingTypeClassExtension::Read_Custom_Aircraft_Dock_Cell()
{
    // Called only after the complete pure-helipad role/reverse-reference checks.
    CustomAircraftDockCellSet = false;
    CustomAircraftDockCell = Cell(0, 0);
    const char* section = This_Const()->GraphicName.c_str();
    if (ArtINI.Is_Present(section, "CustomDockCell")) {
        char buffer[128] = {};
        const int length = ArtINI.Get_String(section, "CustomDockCell", "", buffer, sizeof(buffer));
        const char* cursor = buffer;
        int x = 0;
        int y = 0;
        bool parsed = length > 0 && length < static_cast<int>(sizeof(buffer)) - 1 &&
            Read_Foundation_Integer(cursor, x) && *cursor == ',';
        if (parsed) {
            ++cursor;
            parsed = Read_Foundation_Integer(cursor, y) && *cursor == '\0';
        }
        const char* reason = parsed ? Custom_Aircraft_Dock_Cell_Error(x, y) : "MALFORMED";
        if (reason != nullptr) {
            if (Vinifera_DeveloperMode) {
                DEBUG_WARNING("[CFPAD] type={} event=DOCK_REJECT value={} relative=({}, {}) reason={}\n",
                    Name(), buffer, x, y, reason);
            }
        } else {
            CustomAircraftDockCell = Cell(static_cast<short>(x), static_cast<short>(y));
            CustomAircraftDockCellSet = true;
        }
    }
    // Native Center_Coord = anchor center + (W*128-128,H*128-128,0).
    // Its containing Cell is A+(floor(W/2),floor(H/2)), including even bounds.
    if (!CustomAircraftDockCellSet &&
        !Has_Custom_Foundation_Cell(FoundationWidth / 2, FoundationHeight / 2)) {
        if (Vinifera_DeveloperMode) {
            DEBUG_WARNING("[CFPAD] type={} event=DOCK_REJECT reason=CENTER_NOT_OCCUPIED relative=({}, {})\n",
                Name(), FoundationWidth / 2, FoundationHeight / 2);
        }
        return false;
    }
    if (Vinifera_DeveloperMode) {
        DEBUG_INFO("[CFPAD] type={} event=DOCK_CONFIG source={} relative=({}, {}) bounds=({}, {})\n",
            Name(), CustomAircraftDockCellSet ? "CUSTOM" : "NATIVE_CENTER",
            CustomAircraftDockCellSet ? CustomAircraftDockCell.X : FoundationWidth / 2,
            CustomAircraftDockCellSet ? CustomAircraftDockCell.Y : FoundationHeight / 2,
            FoundationWidth, FoundationHeight);
    }
    return true;
}

/**
 *  Called after every complete Rules::Objects pass. Reverse Dock/DeploysInto
 *  references cannot be validated while BuildingTypes are read before Units.
 */
void BuildingTypeClassExtension::Finalize_Custom_Foundation()
{
    if (Vinifera_PerformingLoad) return;
    Read_Custom_Foundation();
    if (!IsCustomFoundationEnabled || FoundationCellCount == 0) return;
    const BuildingTypeClass* type = This_Const();
    const bool infantry_factory = type->ToBuild == RTTI_INFANTRYTYPE;
    const bool aircraft_helipad = type->ToBuild == RTTI_AIRCRAFTTYPE &&
        type->IsHelipad && type->IsCanUnitReload;
    const bool pad_request = type->ToBuild == RTTI_AIRCRAFTTYPE || type->IsHelipad || type->IsCanUnitReload;
    bool reverse_conflict = false;
    const char* reason = nullptr;

    if (!Validate_Custom_Foundation_Cells() || type->Size < BSIZE_FIRST || type->Size >= BSIZE_COUNT) {
        reason = "invalid custom mask or native fallback";
    } else if ((type->ToBuild != RTTI_NONE && !infantry_factory && !aircraft_helipad) ||
               type->IsWeaponsFactory || type->IsConstructionYard || (type->IsHelipad && !aircraft_helipad) ||
               (!infantry_factory && (type->IsGDIBarracks || type->IsNODBarracks))) {
        reason = "factory/construction/service building";
    } else if (type->IsRefinery || type->IsWeeder || type->FreeUnit != nullptr ||
               type->IsCanUnitRepair || (type->IsCanUnitReload && !aircraft_helipad) || type->IsDockUnload ||
               type->IsHoverPad || type->IsHospital || type->IsArmory || type->Dock.Count() != 0) {
        reason = "dock/refinery/repair contract";
    } else if (type->IsWall || type->IsGate || type->IsLaserFencePost || type->IsLaserFence ||
               type->IsFirestormWall || type->IsPlug || !type->PowersUpBuilding.empty() ||
               type->Upgrades != 0 || type->ToTile != nullptr || type->ToOverlay != nullptr) {
        reason = "wall/gate/fence/plug/tile contract";
    } else if (type->DeploysInto != nullptr || type->UndeploysInto != nullptr ||
               type->IsTickTank || type->IsArtillary || type->IsJuggernaut ||
               type->IsICBMLauncher || type->IsMobileWar || type->IsMobileStealth ||
               type->IsCoreDefender || type->IsLimpetMine) {
        reason = "deploy/undeploy/mobile building contract";
    } else if (type->IsBibbed) {
        reason = "IsBibbed contract";
    } else if (!aircraft_helipad &&
               (type->BibShape != nullptr ||
                !ArtINI.Get_String(type->GraphicName.c_str(), "BibShape", std::string()).empty())) {
        reason = "BibShape contract";
    } else if (!type->IsFootprint || type->IsCanPlaceAnywhere || type->IsBridgeRepairHut ||
               type->IsInvisibleInGame || IsNaval) {
        reason = "special placement contract";
    } else if (Has_Foundation_Service_Reference(BuildingTypes, type) ||
               Has_Foundation_Service_Reference(UnitTypes, type) ||
               Has_Foundation_Service_Reference(InfantryTypes, type) ||
               Has_Foundation_Service_Reference(AircraftTypes, type, aircraft_helipad)) {
        reverse_conflict = true;
        reason = "referenced as Dock or DeploysInto";
    }

    if (reason != nullptr) {
        if (pad_request && Vinifera_DeveloperMode) {
            DEBUG_WARNING("[CFPAD] type={} event=DOCK_REJECT reason={} detail={}\n",
                Name(), reverse_conflict ? "REVERSE_DOCK_CONFLICT" : "ROLE_CONFLICT", reason);
        }
        Reject_Custom_Foundation(reason);
        return;
    }
    if (aircraft_helipad && !Read_Custom_Aircraft_Dock_Cell()) {
        Reject_Custom_Foundation("helipad requires occupied custom or native center dock cell");
        return;
    }
    const bool was_valid = IsCustomFoundationValid;
    IsCustomFoundationValid = true;
    Read_Custom_Infantry_Exit_Cell();
    Invalidate_Foundation_Sync_CRC();
    if (!was_valid) {
        DEV_DEBUG_INFO("[CUSTOMFOUNDATION] type={} event=LOADED width={} height={} count={}\n",
            Name(), FoundationWidth, FoundationHeight, FoundationCellCount);
        if (infantry_factory && Vinifera_DeveloperMode) {
            DEBUG_INFO("[CFINF] type={} event=ELIGIBLE width={} height={} count={}\n",
                Name(), FoundationWidth, FoundationHeight, FoundationCellCount);
        }
        if (aircraft_helipad && Vinifera_DeveloperMode) {
            DEBUG_INFO("[CFPAD] type={} event=PAD_ELIGIBLE bounds=({}, {}) count={} Factory={} Helipad={} UnitReload={}\n",
                Name(), FoundationWidth, FoundationHeight, FoundationCellCount,
                static_cast<int>(type->ToBuild), type->IsHelipad, type->IsCanUnitReload);
        }
    }
}

bool BuildingTypeClassExtension::Is_Custom_Foundation_Infantry_Factory_Eligible() const
{
    // Finalize_Custom_Foundation validates every remaining role and reverse
    // reference before setting IsCustomFoundationValid. Reuse that result;
    // do not cache another eligibility flag in the saved extension layout.
    return IsCustomFoundationEnabled && IsCustomFoundationValid &&
        This_Const()->ToBuild == RTTI_INFANTRYTYPE;
}

bool BuildingTypeClassExtension::Is_Custom_Foundation_Aircraft_Helipad_Eligible() const
{
    // Finalization owns all mixed-role and reverse-reference exclusions.
    return IsCustomFoundationEnabled && IsCustomFoundationValid &&
        This_Const()->ToBuild == RTTI_AIRCRAFTTYPE && This_Const()->IsHelipad && This_Const()->IsCanUnitReload;
}

bool BuildingTypeClassExtension::Custom_Foundation_In_Radar(const Cell& origin) const
{
    // Protect the native AdjacentObjectCount halo and the wider base-reservation
    // helpers. Release scans up to 3*spacing past the box; InnerCells queries
    // include spacing+2 cells plus one adjacent-cell lookup. No helper is replaced.
    const int spacing = Rule->AIBaseSpacing;
    if (spacing < 0 || spacing > std::min(MAP_CELL_W, MAP_CELL_H)) return false;
    const int margin = std::max(3 * spacing, spacing + 3);
    const int origin_x = origin.X;
    const int origin_y = origin.Y;
    if (origin_x < margin || origin_y < margin ||
        origin_x > MAP_CELL_W - FoundationWidth - margin ||
        origin_y > MAP_CELL_H - FoundationHeight - margin) return false;

    for (int i = 0; i < FoundationCellCount; ++i) {
        const int x = origin_x + CustomFoundationCells[i].X;
        const int y = origin_y + CustomFoundationCells[i].Y;
        if (x < 0 || x >= MAP_CELL_W || y < 0 || y >= MAP_CELL_H) return false;
        const Cell cell(static_cast<short>(x), static_cast<short>(y));
        if (!Map.In_Radar(cell) || Map.Array[y * MAP_CELL_W + x] == nullptr) return false;
    }
    return true;
}

/**
 *  Native Radar::Get_Foundation returns a reference to a DynamicVector<Point2D>.
 *  Cache the same filled isometric bounding shape by dimensions, outside the
 *  raw-saved extension. std::map keeps returned vector addresses stable.
 */
const DynamicVectorClass<Point2D>* BuildingTypeClassExtension::Custom_Foundation_Radar(const RadarClass& radar) const
{
    if (!(radar.ZoomFactor > 0.0f) || !std::isfinite(radar.ZoomFactor)) return nullptr;
    if (FoundationRadarOwner != &radar || FoundationRadarZoom != radar.ZoomFactor) {
        Clear_Foundation_Radar();
        FoundationRadarOwner = &radar;
        FoundationRadarZoom = radar.ZoomFactor;
    }

    auto [entry, inserted] = FoundationRadarShapes.try_emplace(std::make_pair(FoundationWidth, FoundationHeight));
    auto& shape = entry->second;
    if (!inserted) return &shape;

    // Match the native x87 calculation: multiply the stored float zoom in
    // extended precision, add 0.5, apply the 1/2-pixel minimum, then truncate.
    const double zoom = radar.ZoomFactor;
    const double scaled_width = std::max(FoundationWidth == 1 ? 1.0 : 2.0, FoundationWidth * zoom + 0.5);
    const double scaled_height = std::max(FoundationHeight == 1 ? 1.0 : 2.0, FoundationHeight * zoom + 0.5);
    // Defensive UI budget for invalid/extreme radar scales. A placed bounding
    // box fits the radar's 140x108 map image; 512 pixels per axis leaves headroom.
    // Never let bad derived UI state cause an unbounded allocation or int cast.
    if (scaled_width >= MAP_CELL_W + 1.0 || scaled_height >= MAP_CELL_H + 1.0) {
        DEV_DEBUG_WARNING("[CUSTOMFOUNDATION] type={} event=RADAR_REJECTED reason=radar pixel budget\n", Name());
        return &shape;
    }
    const int zw = static_cast<int>(scaled_width);
    const int zh = static_cast<int>(scaled_height);
    const int count = zw * zh + (zw - 1) * (zh - 1);
    if (!shape.Resize(count)) return &shape;
    shape.Set_Growth_Step(0);

    for (int y = 0; y < zw + zh - 1; ++y) {
        const int begin = y < zh ? -y : y - 2 * zh + 2;
        const int end = y < zw ? y : 2 * zw - y - 2;
        for (int x = begin; x <= end; ++x) {
            shape.Add(Point2D(x, y));
        }
    }
    return &shape;
}

void BuildingTypeClassExtension::Rebuild_Custom_Foundation_Radar(const RadarClass& radar)
{
    Clear_Foundation_Radar();
    // Post-load radar setup can precede extension swizzling. The first normal
    // getter will build any missing entry after load, without serialized pointers.
    if (Vinifera_PerformingLoad) return;
    for (int i = 0; i < BuildingTypeExtensions.Count(); ++i) {
        const auto ext = BuildingTypeExtensions[i];
        if (ext->IsCustomFoundationEnabled && ext->IsCustomFoundationValid) {
            ext->Custom_Foundation_Radar(radar);
        }
    }
}

void BuildingTypeClassExtension::Custom_Foundation_CRC(CRCEngine& crc) const
{
    // Explicit scalar values only; no array padding, object padding or pointers.
    crc(static_cast<int>(IsCustomFoundationEnabled));
    crc(static_cast<int>(IsCustomFoundationValid));
    crc(FoundationWidth);
    crc(FoundationHeight);
    crc(FoundationCellCount);
    for (int i = 0; i < FoundationCellCount; ++i) {
        crc(static_cast<int>(CustomFoundationCells[i].X));
        crc(static_cast<int>(CustomFoundationCells[i].Y));
    }
    // Preserve the D1 CRC stream when the optional override is absent.
    // Its enabled state is encoded by this tagged trailer, then Set/X/Y.
    if (CustomInfantryExitCellSet) {
        crc(static_cast<unsigned>(0x41463544)); // AF5D optional-exit domain.
        crc(static_cast<int>(CustomInfantryExitCellSet));
        crc(static_cast<int>(CustomInfantryExitCell.X));
        crc(static_cast<int>(CustomInfantryExitCell.Y));
    }
    if (CustomAircraftDockCellSet) {
        crc(static_cast<unsigned>(0x41465044)); // AFPD: aircraft pad domain, distinct from D2.
        crc(static_cast<int>(CustomAircraftDockCellSet));
        crc(static_cast<int>(CustomAircraftDockCell.X));
        crc(static_cast<int>(CustomAircraftDockCell.Y));
    }
}

/**
 *  FRAMESYNC already transmits and compares ScenarioCRC. Mix in the canonical
 *  type configuration on send and receive, without mutating ScenarioCRC or the
 *  per-frame GameCRC algorithm. Rebuild only after type/INI/load changes.
 */
unsigned BuildingTypeClassExtension::Custom_Foundation_Sync_CRC(unsigned native_crc)
{
    if (!FoundationSyncCRCValid) {
        CRCEngine config;
        FoundationSyncCRCPresent = false;
        for (int i = 0; i < BuildingTypes.Count(); ++i) {
            const auto type = BuildingTypes[i];
            const auto ext = Extension::Fetch(type);
            if (ext != nullptr && ext->IsCustomFoundationEnabled) {
                FoundationSyncCRCPresent = true;
                config(static_cast<int>(type->HeapID));
                config(static_cast<unsigned>(CRCEngine::CRC_String(type->IniName.c_str(), 0)));
                ext->Custom_Foundation_CRC(config);
            }
        }
        FoundationSyncCRC = static_cast<unsigned>(config());
        FoundationSyncCRCValid = true;
    }
    if (!FoundationSyncCRCPresent) return native_crc;

    CRCEngine combined;
    combined(native_crc);
    combined(static_cast<unsigned>(0x41463542)); // AF5B domain.
    combined(FoundationSyncCRC);
    return static_cast<unsigned>(combined());
}

/**
 *  Fetches the extra building graphics.
 *
 *  @author: ZivDero
 */
void BuildingTypeClassExtension::Fetch_Building_Normal_Image(TheaterType theater)
{
    char fullname[MAX_PATH];
    char buffer[64];

    ArtINI.Get_String(This()->GraphicName.c_str(), "RoofDeployingAnim", "", buffer, sizeof(buffer));
    if (strlen(buffer) != 0) {
        _makepath(fullname, nullptr, nullptr, buffer, ".SHP");
        This()->Theater_Naming_Convention(fullname, theater);
        RoofDeployingAnim = static_cast<ShapeSet const*>(MixFileClass::Retrieve(fullname));
    }

    ArtINI.Get_String(This()->GraphicName.c_str(), "RoofDoorAnim", "", buffer, sizeof(buffer));
    if (strlen(buffer) != 0) {
        _makepath(fullname, nullptr, nullptr, buffer, ".SHP");
        This()->Theater_Naming_Convention(fullname, theater);
        RoofDoorAnim = static_cast<ShapeSet const*>(MixFileClass::Retrieve(fullname));
    }

    ArtINI.Get_String(This()->GraphicName.c_str(), "UnderRoofDoorAnim", "", buffer, sizeof(buffer));
    if (strlen(buffer) != 0) {
        _makepath(fullname, nullptr, nullptr, buffer, ".SHP");
        This()->Theater_Naming_Convention(fullname, theater);
        UnderRoofDoorAnim = static_cast<ShapeSet const*>(MixFileClass::Retrieve(fullname));
    }
}
