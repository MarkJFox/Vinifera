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
#include "scenario.h"
#include "tibsun_defines.h"
#include "tibsun_globals.h"
#include "unittype.h"
#include "vinifera_globals.h"
#include "wwcrc.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <system_error>

namespace
{
    bool FoundationSyncCRCValid = false;
    bool FoundationSyncCRCPresent = false;
    unsigned FoundationSyncCRC = 0;

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
    bool Has_Foundation_Service_Reference(const DynamicVectorClass<T*>& types, const BuildingTypeClass* target)
    {
        for (int i = 0; i < types.Count(); ++i) {
            const auto type = types[i];
            if (type->DeploysInto == target) return true;
            for (int j = 0; j < type->Dock.Count(); ++j) {
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
    FoundationCellCount(0)
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
    std::fill(CustomFoundationCells + FoundationCellCount, std::end(CustomFoundationCells), REFRESH_EOL);
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
    std::fill(std::begin(CustomFoundationCells), std::end(CustomFoundationCells), REFRESH_EOL);
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

    if (This()->Size != BSIZE_33) {
        Reject_Custom_Foundation("native Foundation must be 3x3");
        return;
    }

    // Parse into ints before any short conversion. Reject truncated strings,
    // trailing garbage, overflow and malformed pairs instead of accepting a prefix.
    char buffer[128];
    int width = 0;
    int height = 0;
    const char* dimensions[] = {"Foundation.X", "Foundation.Y"};
    int* values[] = {&width, &height};
    for (int i = 0; i < 2; ++i) {
        const int length = ArtINI.Get_String(section, dimensions[i], "", buffer, sizeof(buffer));
        const char* cursor = buffer;
        if (length <= 0 || length >= static_cast<int>(sizeof(buffer)) - 1 ||
            !Read_Foundation_Integer(cursor, *values[i]) || *cursor != '\0' || *values[i] != 3) {
            Reject_Custom_Foundation("Foundation.X and Foundation.Y must both be 3");
            return;
        }
    }

    Cell cells[CustomFoundationMaxCells];
    bool seen[3][3] = {};
    int count = 0;
    for (int i = 0; i <= CustomFoundationMaxCells; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "Foundation.%d", i);
        if (!ArtINI.Is_Present(section, key)) break;
        if (i == CustomFoundationMaxCells) {
            Reject_Custom_Foundation("more than 31 occupied cells");
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
        if (x < 0 || x >= 3 || y < 0 || y >= 3) {
            Reject_Custom_Foundation("Foundation.N coordinate outside 0..2");
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
    if (FoundationWidth != 3 || FoundationHeight != 3 ||
        FoundationCellCount <= 0 || FoundationCellCount > CustomFoundationMaxCells ||
        FoundationCellCount > 9 || CustomFoundationCells[0] != Cell(0, 0)) {
        return false;
    }
    int previous = -1;
    for (int i = 0; i < FoundationCellCount; ++i) {
        const Cell& cell = CustomFoundationCells[i];
        if (cell.X < 0 || cell.X >= 3 || cell.Y < 0 || cell.Y >= 3) return false;
        const int index = cell.Y * 3 + cell.X;
        if (index <= previous) return false;
        previous = index;
    }
    return CustomFoundationCells[FoundationCellCount] == REFRESH_EOL;
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
    const char* reason = nullptr;

    if (!Validate_Custom_Foundation_Cells() || type->Size != BSIZE_33) {
        reason = "invalid native 3x3 mask";
    } else if (type->ToBuild != RTTI_NONE || type->IsWeaponsFactory || type->IsConstructionYard ||
               type->IsHelipad || type->IsGDIBarracks || type->IsNODBarracks) {
        reason = "factory/construction/service building";
    } else if (type->IsRefinery || type->IsWeeder || type->FreeUnit != nullptr ||
               type->IsCanUnitRepair || type->IsCanUnitReload || type->IsDockUnload ||
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
    } else if (type->IsBibbed || type->BibShape != nullptr ||
               !ArtINI.Get_String(type->GraphicName.c_str(), "BibShape", std::string()).empty()) {
        reason = "bib contract";
    } else if (!type->IsFootprint || type->IsCanPlaceAnywhere || type->IsBridgeRepairHut ||
               type->IsInvisibleInGame || IsNaval) {
        reason = "special placement contract";
    } else if (Has_Foundation_Service_Reference(BuildingTypes, type) ||
               Has_Foundation_Service_Reference(UnitTypes, type) ||
               Has_Foundation_Service_Reference(InfantryTypes, type) ||
               Has_Foundation_Service_Reference(AircraftTypes, type)) {
        reason = "referenced as Dock or DeploysInto";
    }

    if (reason != nullptr) {
        Reject_Custom_Foundation(reason);
        return;
    }
    const bool was_valid = IsCustomFoundationValid;
    IsCustomFoundationValid = true;
    Invalidate_Foundation_Sync_CRC();
    if (!was_valid) {
        DEV_DEBUG_INFO("[CUSTOMFOUNDATION] type={} event=LOADED width=3 height=3 count={}\n",
            Name(), FoundationCellCount);
    }
}

bool BuildingTypeClassExtension::Custom_Foundation_In_Radar(const Cell& origin) const
{
    // Keep native 3x3 bounding helpers inside the engine's physical cell array,
    // even if an outer row/column is absent from the logical occupied mask.
    const int origin_x = origin.X;
    const int origin_y = origin.Y;
    if (origin_x < 0 || origin_y < 0 ||
        origin_x > MAP_CELL_W - 3 || origin_y > MAP_CELL_H - 3) return false;

    for (int i = 0; i < FoundationCellCount; ++i) {
        const int x = origin_x + CustomFoundationCells[i].X;
        const int y = origin_y + CustomFoundationCells[i].Y;
        if (x < 0 || x >= MAP_CELL_W || y < 0 || y >= MAP_CELL_H) return false;
        const Cell cell(static_cast<short>(x), static_cast<short>(y));
        if (!Map.In_Radar(cell) || Map.Array[y * MAP_CELL_W + x] == nullptr) return false;
    }
    return true;
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
