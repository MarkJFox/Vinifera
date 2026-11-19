/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  Extended TeamClass class.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#pragma once

#include "abstracttypeext.h"
#include "advaitactictype.h"
#include "team.h"
#include "vinifera_defines.h"


class AdvancedAITacticTypeClass;
class TechnoTypeClass;


class DECLSPEC_UUID(UUID_TEAM_EXTENSION)
TeamClassExtension final : public AbstractClassExtension
{
    public:
        /**
         *  IPersist
         */
        IFACEMETHOD(GetClassID)(CLSID *pClassID);

        /**
         *  IPersistStream
         */
        IFACEMETHOD(Load)(IStream *pStm);
        IFACEMETHOD(Save)(IStream *pStm, BOOL fClearDirty);

    public:
        TeamClassExtension(const TeamClass *this_ptr = nullptr);
        TeamClassExtension(const NoInitClass &noinit);
        virtual ~TeamClassExtension();

        virtual int Get_Object_Size() const override;
        virtual void Object_CRC(CRCEngine &crc) const override;

        virtual const char* Name() const override;
        virtual const char* Full_Name() const override;

        virtual TeamClass *This() const override { return reinterpret_cast<TeamClass *>(AbstractClassExtension::This()); }
        virtual const TeamClass *This_Const() const override { return reinterpret_cast<const TeamClass *>(AbstractClassExtension::This_Const()); }
        virtual RTTIType Fetch_RTTI() const override { return RTTI_TEAM; }

        /**
         *  AF-0009U: Advanced AI team support (ported from DTA).
         */
        void Copy_Executive_State_From(const TeamClassExtension* other);
        void AdvAI_Team_Recruit_AI();
        void AdvAI_Team_Maintenance_AI();
        int AdvAI_Get_Object_Value_For_Team(const TechnoTypeClass* technotype, bool debug) const;
        bool Contains_Transport() const;
        int Infantry_Count() const;
        int Object_Type_Count(const TechnoTypeClass* technotype) const;
        void AdvAI_Add_Member_Strength(const FootClass* obj, int sign);

        /**
         *  Is the dynamic Advanced AI team logic active for this team?
         */
        bool Is_AdvAI_Managed() const;

        /**
         *  AF-0009U: MaxCost plus half of the owner's credits above
         *  AdvancedAIRichFunds, so a rich house keeps producing instead of
         *  waiting for the next tactic. MaxCost <= 0 stays unlimited.
         */
        int AdvAI_Effective_Max_Cost() const;

    public:
        int AntiNoneStrength;
        int AntiLightStrength;
        int AntiHeavyStrength;
        int ArtilleryStrength;
        double DesiredAntiNoneRatio;
        double DesiredAntiLightRatio;
        double DesiredAntiHeavyRatio;
        double DesiredArtilleryRatio;
        int NoneArmorStrength;
        int LightArmorStrength;
        int HeavyArmorStrength;
        double DesiredNoneStrengthRatio;
        double DesiredLightStrengthRatio;
        double DesiredHeavyStrengthRatio;
        bool IsAdvAITeam;
        int OriginalTeamMemberCount;
        double SpeedValueMultiplier;
        double StrengthValueMultiplier;
        double CloakValueMultiplier;
        double CostWeightMultiplier;
        bool NoVehicles;
        bool NoInfantry;
        bool NoAircraft;
        int MinimumReadyFrame;
        double DesiredInfantryRatio;
        double DesiredVehicleRatio;
        int InfantryCost;
        int VehicleCost;
        int MaxCost;
        int CurrentCost;
        bool IsTransportTeam;
        int MaxInfantry;
        AdvAITacticType AdvAIGroundTacticType;
        AdvancedAITacticTypeClass* AdvAITactic;
        bool IsAircraftTeam;
        bool IsBiasedForEnemyStrength;
        bool PenalizeSameTypeUnits;
        ProductionFlags ProdFlags;

        /**
         *  AF-0009U: Team of a fixed (AI.ini) TeamType started by a tactic.
         *  It keeps the native TaskForce/Script behaviour; the Advanced AI only
         *  builds its missing members and starts it after the deadline frame
         *  (0 = no deadline).
         */
        bool IsAdvAIScriptedTeam;
        int AdvAIScriptedDeadline;
};
