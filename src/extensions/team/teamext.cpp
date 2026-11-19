/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended TeamClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "teamext.h"

#include "buildingtype.h"
#include "ccini.h"
#include "debughandler.h"
#include "unittype.h"
#include "extension.h"
#include "foot.h"
#include "house.h"
#include "houseext.h"
#include "infantrytype.h"
#include "rulesext.h"
#include "team.h"
#include "teamtype.h"
#include "technotype.h"
#include "technotypeext.h"
#include "tibsun_globals.h"
#include "tibsun_inline.h"
#include "vinifera_saveload.h"
#include "wwcrc.h"

#include <algorithm>
#include <climits>
#include <cmath>


/**
 *  Class constructor.
 *
 *  @author: Rampastring
 */
TeamClassExtension::TeamClassExtension(const TeamClass *this_ptr) :
    AbstractClassExtension(this_ptr),
    AntiNoneStrength(0),
    AntiLightStrength(0),
    AntiHeavyStrength(0),
    ArtilleryStrength(0),
    DesiredAntiNoneRatio(0),
    DesiredAntiLightRatio(0),
    DesiredAntiHeavyRatio(0),
    DesiredArtilleryRatio(0),
    NoneArmorStrength(0),
    LightArmorStrength(0),
    HeavyArmorStrength(0),
    DesiredNoneStrengthRatio(0),
    DesiredLightStrengthRatio(0),
    DesiredHeavyStrengthRatio(0),
    IsAdvAITeam(false),
    OriginalTeamMemberCount(0),
    SpeedValueMultiplier(1.0),
    StrengthValueMultiplier(1.0),
    CloakValueMultiplier(0.0),
    CostWeightMultiplier(1.0),
    NoVehicles(false),
    NoInfantry(false),
    NoAircraft(false),
    MinimumReadyFrame(0),
    DesiredInfantryRatio(-1.0),
    DesiredVehicleRatio(-1.0),
    InfantryCost(0),
    VehicleCost(0),
    MaxCost(0),
    CurrentCost(0),
    IsTransportTeam(false),
    MaxInfantry(-1),
    AdvAIGroundTacticType(TACTIC_NONE),
    AdvAITactic(nullptr),
    IsAircraftTeam(false),
    IsBiasedForEnemyStrength(false),
    PenalizeSameTypeUnits(false),
    ProdFlags(PRODFLAG_NONE),
    IsAdvAIScriptedTeam(false),
    AdvAIScriptedDeadline(0)
{
    TeamExtensions.Add(this);
}


/**
 *  Class no-init constructor.
 *
 *  @author: Rampastring
 */
TeamClassExtension::TeamClassExtension(const NoInitClass &noinit) :
    AbstractClassExtension(noinit)
{
}


/**
 *  Class destructor.
 *
 *  @author: Rampastring
 */
TeamClassExtension::~TeamClassExtension()
{
    TeamExtensions.Delete(this);
}


/**
 *  Retrieves the class identifier (CLSID) of the object.
 *
 *  @author: Rampastring
 */
HRESULT TeamClassExtension::GetClassID(CLSID *lpClassID)
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
 *  @author: Rampastring
 */
HRESULT TeamClassExtension::Load(IStream *pStm)
{
    HRESULT hr = AbstractClassExtension::Internal_Load(pStm);
    if (FAILED(hr)) {
        return E_FAIL;
    }

    new (this) TeamClassExtension(NoInitClass());

    VINIFERA_SWIZZLE_REQUEST_POINTER_REMAP(AdvAITactic, "AdvAITactic");

    return hr;
}


/**
 *  Saves an object to the specified stream.
 *
 *  @author: Rampastring
 */
HRESULT TeamClassExtension::Save(IStream *pStm, BOOL fClearDirty)
{
    HRESULT hr = AbstractClassExtension::Internal_Save(pStm, fClearDirty);
    if (FAILED(hr)) {
        return hr;
    }

    return hr;
}


/**
 *  Return the raw size of class data for save/load purposes.
 *
 *  @author: Rampastring
 */
int TeamClassExtension::Get_Object_Size() const
{
    return sizeof(*this);
}




/**
 *  Compute a unique crc value for this instance.
 *
 *  @author: Rampastring
 */
void TeamClassExtension::Object_CRC(CRCEngine &crc) const
{
    crc(IsAdvAITeam);
    crc(CurrentCost);
    crc(MaxCost);
    crc(OriginalTeamMemberCount);
    crc(ProdFlags);
    crc(IsAdvAIScriptedTeam);
    crc(AdvAIScriptedDeadline);
}

/**
 *  Returns the name of this object type.
 *
 *  @author: Rampastring
 */
const char* TeamClassExtension::Name() const
{
    return This()->Class->Name();
}


/**
 *  Returns the full name of this object type.
 *
 *  @author: CCHyper
 */
const char* TeamClassExtension::Full_Name() const
{
    return This()->Class->Full_Name();
}


/**
 *  AF-0009U: Is this team managed by the dynamic Advanced AI team logic?
 *  Only teams created by Advanced AI tactics are; all other teams keep the
 *  native TaskForce behaviour.
 */
bool TeamClassExtension::Is_AdvAI_Managed() const
{
    return IsAdvAITeam;
}


int TeamClassExtension::AdvAI_Effective_Max_Cost() const
{
    if (MaxCost <= 0 || RuleExtension->AdvancedAIRichFunds <= 0) return MaxCost;
    HouseClass* house = This_Const()->House;
    if (house == nullptr) return MaxCost;
    const long long surplus = static_cast<long long>(house->Available_Money()) - RuleExtension->AdvancedAIRichFunds;
    if (surplus <= 0) return MaxCost;
    return static_cast<int>(std::min<long long>(INT_MAX / 2, MaxCost + surplus / 2));
}


/**
 *  Copies team execution related state from another instance.
 *  This function does NOT copy state that is only used for producing the team.
 *
 *  @author: Rampastring
 */
void TeamClassExtension::Copy_Executive_State_From(const TeamClassExtension* other)
{
    IsAdvAITeam = other->IsAdvAITeam;
    MinimumReadyFrame = other->MinimumReadyFrame;
    IsTransportTeam = other->IsTransportTeam;
    AdvAIGroundTacticType = other->AdvAIGroundTacticType;
    AdvAITactic = other->AdvAITactic;
    IsAircraftTeam = other->IsAircraftTeam;
    ProdFlags = other->ProdFlags;
    NoInfantry = other->NoInfantry;
    NoVehicles = other->NoVehicles;
    NoAircraft = other->NoAircraft;
}


/**
 *  Adds (sign = 1) or removes (sign = -1) the combat strength of a member.
 *
 *  @author: Rampastring
 */
void TeamClassExtension::AdvAI_Add_Member_Strength(const FootClass* obj, int sign)
{
    const TechnoTypeClass* technotype = obj->TClass;
    const TechnoTypeClassExtension* technotypeext = Extension::Fetch(technotype);

    AntiNoneStrength += sign * technotypeext->AntiNoneArmorValue();
    AntiLightStrength += sign * technotypeext->AntiLightArmorValue();
    AntiHeavyStrength += sign * technotypeext->AntiHeavyArmorValue();
    ArtilleryStrength += sign * technotypeext->ArtilleryValue();
    CurrentCost += sign * technotype->Cost;

    if (obj->RTTI == RTTI_INFANTRY) {
        InfantryCost += sign * technotype->Cost;
    } else if (obj->RTTI == RTTI_UNIT) {
        VehicleCost += sign * technotype->Cost;
    }

    if (technotype->Armor == ARMOR_NONE) {
        NoneArmorStrength += sign * (int)technotype->MaxStrength;
    } else if (technotypeext->CategorizedAsLightlyArmored()) {
        LightArmorStrength += sign * (int)technotype->MaxStrength;
    } else if (technotype->Armor == ARMOR_STEEL) {
        HeavyArmorStrength += sign * (int)technotype->MaxStrength;
    }
}


/**
 *  Recruits suitable idle units of the owning house into this team.
 *
 *  @author: Rampastring
 */
void TeamClassExtension::AdvAI_Team_Recruit_AI()
{
    TeamClass* team = This();

    if (IsTransportTeam && Contains_Transport() && Infantry_Count() >= MaxInfantry) {
        return;
    }

    for (int i = 0; i < Foots.Count(); i++) {
        FootClass* foot = Foots[i];

        if (!foot->IsActive || !foot->IsDown || foot->IsInLimbo || foot->House != team->House) {
            continue;
        }

        if (NoAircraft && foot->RTTI == RTTI_AIRCRAFT) continue;
        if (NoInfantry && foot->RTTI == RTTI_INFANTRY) continue;
        if (NoVehicles && foot->RTTI == RTTI_UNIT) continue;

        const bool naval = Extension::Fetch(foot->TClass)->IsNaval;
        if (ProdFlags != PRODFLAG_NAVAL && naval) continue;
        if (ProdFlags == PRODFLAG_NAVAL && !naval) continue;

        /**
         *  Unarmed units only join as the transport of a transport team, which
         *  the selector may order (Aftermath: DTA rejected them before the
         *  transport check, so an ordered transport never joined its team).
         */
        const bool transport = IsTransportTeam && foot->RTTI != RTTI_INFANTRY && foot->TClass->MaxPassengers > 0;
        if (!foot->Is_Weapon_Equipped() && !transport) {
            continue;
        }

        /**
         *  Harvesters and units that rebuild bases never join attack teams.
         */
        if (foot->TClass->RTTI == RTTI_UNITTYPE) {
            const UnitTypeClass* unittype = static_cast<const UnitTypeClass*>(foot->TClass);
            if (unittype->IsToHarvest || unittype->IsToVeinHarvest ||
                (unittype->DeploysInto != nullptr && unittype->DeploysInto->IsConstructionYard)) {
                continue;
            }
        }

        // Do not recruit units that don't have full ammo.
        if (foot->TClass->MaxAmmo > 0 && foot->Ammo != foot->TClass->MaxAmmo) {
            continue;
        }

        if (IsTransportTeam) {
            if (foot->RTTI != RTTI_INFANTRY) {
                if (foot->TClass->MaxPassengers <= 0 || Contains_Transport()) {
                    continue;
                }
            } else if (Infantry_Count() >= MaxInfantry) {
                continue;
            }
        }

        /**
         *  Aftermath: Members of scripted or trigger created teams stay there;
         *  only free units and other Advanced AI teams' members are recruited.
         */
        if (foot->Team != nullptr && !Extension::Fetch(foot->Team)->Is_AdvAI_Managed()) {
            continue;
        }

        if (foot->Team != nullptr && foot->Team->Class->RecruitPriority >= team->Class->RecruitPriority) {
            continue;
        }

        if (team->Class->Group == -2 || (int)foot->Group == team->Class->Group || team->Class->IsRecruiter) {
            foot->Assign_Target(nullptr);
            team->Add(foot);
        }
    }
}


/**
 *  Keeps passengers of transport teams from acting on their own.
 *
 *  @author: Rampastring
 */
void TeamClassExtension::AdvAI_Team_Maintenance_AI()
{
    if (!IsTransportTeam) {
        return;
    }

    for (FootClass* member = This()->Member; member != nullptr; member = member->Member) {
        if (member->IsInLimbo) {
            if (member->NavCom != nullptr) member->Assign_Destination(nullptr);
            if (member->TarCom != nullptr) member->Assign_Target(nullptr);
            if (member->ArchiveTarget != nullptr) member->Assign_Archive_Target(nullptr);
        }
    }
}


int TeamClassExtension::Object_Type_Count(const TechnoTypeClass* technotype) const
{
    int count = 0;
    for (const FootClass* member = This_Const()->Member; member != nullptr; member = member->Member) {
        if (member->TClass == technotype) {
            count++;
        }
    }
    return count;
}


/**
 *  Evaluates how valuable a techno type would be for this team.
 *
 *  @author: Rampastring
 */
int TeamClassExtension::AdvAI_Get_Object_Value_For_Team(const TechnoTypeClass* technotype, bool debug) const
{
    if (MaxCost > 0 && CurrentCost + technotype->Cost > AdvAI_Effective_Max_Cost()) {
        return -1;
    }

    if (technotype->RTTI == RTTI_INFANTRYTYPE) {
        const int infcount = Infantry_Count();

        if (MaxInfantry > -1 && infcount >= MaxInfantry) {
            return -1;
        }

        if (infcount >= RuleExtension->AdvancedAITeamCheapInfantryMax &&
            technotype->Cost < RuleExtension->AdvancedAICheapInfantryCostThreshold) {
            return -1;
        }
    }

    if (IsTransportTeam && (technotype->RTTI == RTTI_UNITTYPE || technotype->RTTI == RTTI_AIRCRAFTTYPE)) {
        if (technotype->MaxPassengers <= 0 || Contains_Transport()) {
            return -1;
        }

        return technotype->MaxPassengers * technotype->MaxSpeed * (technotype->IsCloakable ? 2 : 1);
    }

    const TechnoTypeClassExtension* technotypeext = Extension::Fetch(technotype);

    int antinonevalue = technotypeext->AntiNoneArmorValue();
    int antilightvalue = technotypeext->AntiLightArmorValue();
    int antiheavyvalue = technotypeext->AntiHeavyArmorValue();
    int artilleryvalue = technotypeext->ArtilleryValue();

    if (AdvAIGroundTacticType == TACTIC_APC_ATTACK && technotype->RTTI == RTTI_INFANTRYTYPE &&
        static_cast<const InfantryTypeClass*>(technotype)->IsBomber) {
        antilightvalue += 1000;
    }

    const int teamtotalstrength = AntiNoneStrength + AntiLightStrength + AntiHeavyStrength;
    double teamnoneratio = 0;
    double teamlightratio = 0;
    double teamheavyratio = 0;
    double teamartilleryratio = 0;

    if (teamtotalstrength > 0) {
        teamnoneratio = AntiNoneStrength / (double)teamtotalstrength;
        teamlightratio = AntiLightStrength / (double)teamtotalstrength;
        teamheavyratio = AntiHeavyStrength / (double)teamtotalstrength;
        teamartilleryratio = ArtilleryStrength / (double)teamtotalstrength;
    }

    double desirednoneratio = DesiredAntiNoneRatio;
    if (teamnoneratio > 0.0) desirednoneratio = DesiredAntiNoneRatio * (DesiredAntiNoneRatio / teamnoneratio);

    double desiredlightratio = DesiredAntiLightRatio;
    if (teamlightratio > 0.0) desiredlightratio = DesiredAntiLightRatio * (DesiredAntiLightRatio / teamlightratio);

    double desiredheavyratio = DesiredAntiHeavyRatio;
    if (teamheavyratio > 0.0) desiredheavyratio = DesiredAntiHeavyRatio * (DesiredAntiHeavyRatio / teamheavyratio);

    double desiredartilleryratio = DesiredArtilleryRatio;
    if (teamartilleryratio > 0.0) desiredartilleryratio = DesiredArtilleryRatio * (DesiredArtilleryRatio / teamartilleryratio);

    int totaloffensivevalue = (int)(antinonevalue * desirednoneratio + antilightvalue * desiredlightratio +
        antiheavyvalue * desiredheavyratio + artilleryvalue * desiredartilleryratio);

    /**
     *  Aftermath: Weigh anti-aircraft capability by the share of flying enemies
     *  (aircraft and airborne jumpjets), which ground weapons cannot hit. DTA's
     *  ground teams ignored this, so only anti-ground units were built against
     *  jumpjet attacks.
     */
    if (!IsAircraftTeam && ProdFlags != PRODFLAG_NAVAL) {
        const HouseClassExtension* houseext = Extension::Fetch(This_Const()->House);
        const int air = houseext->EnemyAirStrength;
        const int ground = houseext->EnemyNoneStrength + houseext->EnemyLightStrength + houseext->EnemyHeavyStrength;
        if (air > 0) {
            const double airshare = air / (double)(air + ground);
            const int antiairvalue = (technotypeext->AntiNoneArmorValueAA() + technotypeext->AntiLightArmorValueAA() +
                technotypeext->AntiHeavyArmorValueAA()) / 3;
            totaloffensivevalue += (int)(antiairvalue * airshare * 2.0);
        }
    }

    if (totaloffensivevalue <= 0) {
        return INT_MIN;
    }

    const int teamtotaldefensive = NoneArmorStrength + LightArmorStrength + HeavyArmorStrength;

    int defensivevalue = (int)(technotypeext->DefensiveValue() * StrengthValueMultiplier);

    if (teamtotaldefensive > 0 && (DesiredNoneStrengthRatio > 0.0 || DesiredLightStrengthRatio > 0.0 || DesiredHeavyStrengthRatio > 0.0)) {
        auto weigh = [&](int strength, double desired) {
            double diff = desired - strength / (double)teamtotaldefensive;
            diff *= RuleExtension->AdvancedAIOverweightedArmorTypeAntiBias;
            diff = std::max(-0.9, diff);
            defensivevalue = (int)(defensivevalue * (1.0 + diff));
        };

        if (technotype->Armor == ARMOR_NONE) {
            weigh(NoneArmorStrength, DesiredNoneStrengthRatio);
        } else if (technotypeext->CategorizedAsLightlyArmored()) {
            weigh(LightArmorStrength, DesiredLightStrengthRatio);
        } else if (technotype->Armor == ARMOR_STEEL) {
            weigh(HeavyArmorStrength, DesiredHeavyStrengthRatio);
        }
    }

    int totalvalue = (int)std::pow(std::max(0, totaloffensivevalue + defensivevalue), 1.2) / 2;

    if (technotype->RTTI == RTTI_INFANTRYTYPE) {
        if (DesiredInfantryRatio >= 0.0 && CurrentCost > 0 && InfantryCost > 0) {
            const double infratio = InfantryCost / (double)CurrentCost;
            totalvalue = (int)(totalvalue * (DesiredInfantryRatio / infratio));
        }

        if (PenalizeSameTypeUnits) {
            const double reduction = std::min(0.9, Object_Type_Count(technotype) * RuleExtension->AdvancedAISameUnitAntiBias);
            totalvalue = (int)(totalvalue * (1.0 - reduction));
        }

        // Avoid infantry spam.
        if (Extension::Fetch(This_Const()->House)->Has_War_Factory() && InfantryCost > 0) {
            const double ratio = RuleExtension->AdvancedAITeamInfantryCostPunishmentThreshold / (double)InfantryCost;
            if (ratio < 1.0) {
                totalvalue = (int)(totalvalue * ratio);
            }
        }

    } else if (technotype->RTTI == RTTI_UNITTYPE) {
        if (DesiredVehicleRatio >= 0.0 && CurrentCost > 0 && VehicleCost > 0) {
            const double vehicleratio = VehicleCost / (double)CurrentCost;
            totalvalue = (int)(totalvalue * (DesiredVehicleRatio / vehicleratio));
        }

        if (PenalizeSameTypeUnits) {
            const double multi = ProdFlags == PRODFLAG_NAVAL ? 3.0 : 1.0;
            const double reduction = std::min(0.9, Object_Type_Count(technotype) * RuleExtension->AdvancedAISameUnitAntiBias * multi);
            totalvalue = (int)(totalvalue * (1.0 - reduction));
        }
    }

    const int adjusted = technotypeext->Scale_Value_By_Properties(totalvalue, SpeedValueMultiplier, StrengthValueMultiplier,
        CloakValueMultiplier, CostWeightMultiplier);

    if (debug) {
        DEV_DEBUG_INFO("AF-0009U:     {}: none={} light={} heavy={} artillery={} offensive={} defensive={} total={} adjusted={}\n",
            technotype->Name(), antinonevalue, antilightvalue, antiheavyvalue, artilleryvalue, totaloffensivevalue,
            defensivevalue, totalvalue, adjusted);
    }

    if (adjusted <= 0) {
        return adjusted;
    }

    // Add some randomness for unpredictability. Uses the synchronised scenario random.
    return Random_Pick((adjusted * 2) / 3, (adjusted * 3) / 2);
}


bool TeamClassExtension::Contains_Transport() const
{
    for (const FootClass* member = This_Const()->Member; member != nullptr; member = member->Member) {
        if ((member->RTTI == RTTI_UNIT || member->RTTI == RTTI_AIRCRAFT) && member->TClass->Max_Passengers() > 0) {
            return true;
        }
    }
    return false;
}


int TeamClassExtension::Infantry_Count() const
{
    int inf = 0;
    for (const FootClass* member = This_Const()->Member; member != nullptr; member = member->Member) {
        if (member->RTTI == RTTI_INFANTRY) {
            inf++;
        }
    }
    return inf;
}
