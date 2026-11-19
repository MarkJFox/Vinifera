/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *  @brief  AF-0009U: Advanced AI combat value model for TechnoTypes.
 *
 *          Ported from the Dawn of the Tiberium Age Vinifera build
 *          (Rampastring, branch dta-advaiv2-spawner). The values are pure
 *          functions of the rules data; they add no saved state and are
 *          identical on every client of a multiplayer game.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2020-2026 Vinifera contributors
 ******************************************************************************/

#include "always.h"

#include "technotypeext.h"

#include "aircrafttype.h"
#include "aircrafttypeext.h"
#include "bullettype.h"
#include "extension.h"
#include "rockettype.h"
#include "rulesext.h"
#include "technotype.h"
#include "unittype.h"
#include "verses.h"
#include "vinifera_globals.h"
#include "warheadtype.h"
#include "weapontype.h"
#include "weapontypeext.h"

#include <algorithm>
#include <cmath>


namespace
{
    int Scaled_Weapon_Value(double value)
    {
        return (int)(value * RuleExtension->AdvancedAIOffensiveWeaponValueMultiplier);
    }


    int Drone_Spawn_Weapon_Value(const TechnoTypeClass* technotype, const WeaponTypeClass* weapon, ArmorType armortype)
    {
        const TechnoTypeClassExtension* technotypeext = Extension::Fetch(technotype);
        if (technotypeext->Spawns == nullptr) {
            return 0;
        }

        const WeaponTypeClass* spawnerweapon = technotypeext->Spawns->Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon;
        if (spawnerweapon == nullptr || spawnerweapon->WarheadPtr == nullptr) {
            return 0;
        }

        // Factor speed as a slight bonus to range for low-ranged units.
        int rangescore = std::max(3 + (technotype->MaxSpeed / 12), weapon->Range / CELL_LEPTON);
        int damage = spawnerweapon->Attack * spawnerweapon->Burst;

        int rof = std::max(1, technotypeext->SpawnRegenRate + technotypeext->SpawnSpawnRate);

        double spreadf = 1.0 + spawnerweapon->WarheadPtr->SpreadFactor / 128.0;

        // Give extra value to high-ranged high-damage weapons.
        if (weapon->Range > 8 * CELL_LEPTON) {
            damage = damage + (int)(damage * ((weapon->Range / CELL_LEPTON) - 7) * 0.2);
        }

        if (weapon->Bullet->IsInaccurate) {
            damage = (int)(damage * 0.67);
        }

        // Bounciness and elasticity is kind of like inaccuracy.
        if (weapon->Bullet->IsBouncy && weapon->Bullet->Elasticity > 0.0) {
            damage = (int)(damage * (1.0 / (1.0 + weapon->Bullet->Elasticity / (armortype == ARMOR_NONE ? 1.0 : 2.0))));
        }

        int weaponvalue = (int)((damage * Verses::Get_Modifier(armortype, spawnerweapon->WarheadPtr) * rangescore * spreadf) / std::pow(rof, 0.95));

        if (technotype->MaxAmmo > 0) {
            weaponvalue = weaponvalue / 5;
            weaponvalue = weaponvalue * technotype->MaxAmmo;
        }

        return Scaled_Weapon_Value(weaponvalue);
    }


    int Missile_Spawn_Weapon_Value(const TechnoTypeClass* technotype, const WeaponTypeClass* weapon, ArmorType armortype)
    {
        const TechnoTypeClassExtension* technotypeext = Extension::Fetch(technotype);
        const RocketTypeClass* rockettype = RocketTypeClass::From_AircraftType(technotypeext->Spawns);

        // On scenario teardown, rocket types might be null.
        if (rockettype == nullptr || rockettype->Warhead == nullptr) {
            return 0;
        }

        int rangescore = std::max(3 + (technotype->MaxSpeed / 12), weapon->Range / CELL_LEPTON);
        int damage = rockettype->Damage * technotypeext->SpawnsNumber;
        int rof = std::max(1, technotypeext->SpawnRegenRate + technotypeext->SpawnSpawnRate);

        double spreadf = 1.0 + rockettype->Warhead->SpreadFactor / 128.0;

        // Give extra value to high-ranged high-damage weapons.
        if (weapon->Range > 8 * CELL_LEPTON) {
            damage = damage + (int)(damage * ((weapon->Range / CELL_LEPTON) - 7) * 0.2);
        }

        int weaponvalue = (int)((damage * Verses::Get_Modifier(armortype, rockettype->Warhead) * rangescore * spreadf) / std::pow(rof, 0.95));

        return Scaled_Weapon_Value(weaponvalue);
    }


    int Weapon_Value(const TechnoTypeClass* technotype, const WeaponTypeClass* weapon, ArmorType armortype, bool oneburst = false)
    {
        if (weapon == nullptr || weapon->Bullet == nullptr || weapon->WarheadPtr == nullptr) {
            return 0;
        }

        /**
         *  Only evaluate armor classes that exist in the rules (Verses asserts otherwise).
         */
        if (armortype < ARMOR_FIRST || armortype >= ArmorTypes.Count()) {
            return 0;
        }

        const WeaponTypeClassExtension* weaponext = Extension::Fetch(weapon);
        if (weaponext->IsSpawner) {
            const TechnoTypeClassExtension* technotypeext = Extension::Fetch(technotype);
            if (technotypeext->Spawns == nullptr) {
                return 0;
            }

            if (Extension::Fetch(technotypeext->Spawns)->IsMissileSpawn) {
                return Missile_Spawn_Weapon_Value(technotype, weapon, armortype);
            }

            return Drone_Spawn_Weapon_Value(technotype, weapon, armortype);
        }

        const WarheadTypeClass* warhead = weapon->WarheadPtr;

        // Factor speed as a slight bonus to range for low-ranged units.
        int rangescore = std::max(3 * CELL_LEPTON + (technotype->MaxSpeed / 3) * (CELL_LEPTON / 4), (int)weapon->Range) / CELL_LEPTON;

        int bursts = oneburst ? 1 : weapon->Burst;
        int damage = weapon->Attack * bursts;

        // Railguns and other ambient-damage weapons get some extra damage because they sometimes hit multiple targets.
        if (weapon->IsRailgun || weapon->IsSonic) {
            damage += (weapon->AmbientDamage * 3) / 2;
        }

        int rof = weapon->ROF;
        for (int b = 0; b < bursts && b < 4; b++) {
            rof += weapon->BurstDelay[b];
        }
        rof = std::max(1, rof);

        double spreadf = 1.0 + warhead->SpreadFactor / 128.0;

        // Give extra value to high-ranged high-damage weapons.
        if (weapon->Range > 8 * CELL_LEPTON) {
            damage = damage + (int)(damage * ((weapon->Range / CELL_LEPTON) - 7) * 0.2);
        }

        if (weapon->Bullet->IsInaccurate) {
            damage = (int)(damage * 0.67);
        }

        // Bounciness and high elasticity is kind of like inaccuracy.
        if (weapon->Bullet->IsBouncy && weapon->Bullet->Elasticity > 1.0) {
            damage = (int)(damage * (1.0 / (1.0 + weapon->Bullet->Elasticity / (armortype == ARMOR_NONE ? 1.0 : 2.0))));
        }

        double mod = Verses::Get_Modifier(armortype, warhead);
        int weaponvalue = (int)((damage * mod * rangescore * spreadf) / std::pow(rof, 0.95));

        if (technotype->MaxAmmo > 0) {
            weaponvalue = weaponvalue / 5;
            weaponvalue = weaponvalue * technotype->MaxAmmo;
        }

        return Scaled_Weapon_Value(weaponvalue);
    }


    bool Has_Firing_Sync(const TechnoTypeClass* technotype)
    {
        return technotype->RTTI == RTTI_UNITTYPE && static_cast<const UnitTypeClass*>(technotype)->FiringSyncFrame[0] > -1;
    }


    int AntiGroundValue(ArmorType armortype, const TechnoTypeClass* technotype)
    {
        const WeaponTypeClass* weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon;
        int value = 0;

        if (weapon != nullptr && weapon->Bullet != nullptr) {
            if (!weapon->Bullet->IsAntiGround) {
                return 0;
            }

            value += Weapon_Value(technotype, weapon, armortype);

            weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_SECONDARY).Weapon;

            if (weapon != nullptr && weapon->Bullet != nullptr && weapon->Bullet->IsAntiGround) {

                // Units with FiringSyncFrame can use both their primary and secondary weapon on the same target.
                if (Has_Firing_Sync(technotype)) {
                    value += Weapon_Value(technotype, weapon, armortype, true);
                } else {
                    value = std::max(value, Weapon_Value(technotype, weapon, armortype));
                }
            }
        }

        return value;
    }
}


int TechnoTypeClassExtension::AntiNoneArmorValue() const
{
    return AntiGroundValue(ARMOR_NONE, This_Const());
}


/**
 *  Averages a value over the light armor classes. DTA additionally used its
 *  own armor index 6; Aftermath only includes it when that ArmorType exists.
 */
template<typename F>
static int Light_Armor_Average(F value)
{
    static const ArmorType _light[] = { ARMOR_ALUMINUM, ARMOR_WOOD, (ArmorType)6 };

    int total = 0;
    int count = 0;
    for (ArmorType armor : _light) {
        if (armor < ArmorTypes.Count()) {
            total += value(armor);
            count++;
        }
    }
    return count > 0 ? total / count : 0;
}


int TechnoTypeClassExtension::AntiLightArmorValue() const
{
    const TechnoTypeClass* technotype = This_Const();
    return Light_Armor_Average([technotype](ArmorType armor) { return AntiGroundValue(armor, technotype); });
}


int TechnoTypeClassExtension::AntiHeavyArmorValue() const
{
    return AntiGroundValue(ARMOR_STEEL, This_Const());
}


int TechnoTypeClassExtension::AntiNoneArmorValueAA() const
{
    return AntiAirValue(ARMOR_NONE);
}


int TechnoTypeClassExtension::AntiLightArmorValueAA() const
{
    return Light_Armor_Average([this](ArmorType armor) { return AntiAirValue(armor); });
}


int TechnoTypeClassExtension::AntiHeavyArmorValueAA() const
{
    return AntiAirValue(ARMOR_STEEL);
}


int TechnoTypeClassExtension::AntiAirValue(ArmorType armor) const
{
    const TechnoTypeClass* technotype = This_Const();
    int value = 0;

    const WeaponTypeClass* weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon;
    if (weapon != nullptr && weapon->Bullet != nullptr && weapon->Bullet->IsAntiAircraft) {
        value = Weapon_Value(technotype, weapon, armor);
    }

    weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_SECONDARY).Weapon;
    if (weapon != nullptr && weapon->Bullet != nullptr && weapon->Bullet->IsAntiAircraft) {
        value = std::max(Weapon_Value(technotype, weapon, armor), value);
    }

    return value;
}


int TechnoTypeClassExtension::ArtilleryValue() const
{
    const TechnoTypeClass* technotype = This_Const();
    const WeaponTypeClass* weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon;
    int value = 0;

    if (weapon != nullptr && weapon->Bullet != nullptr && weapon->Bullet->IsAntiGround && weapon->Range > CELL_LEPTON * 8) {

        value = (Weapon_Value(technotype, weapon, ARMOR_NONE) + Weapon_Value(technotype, weapon, ARMOR_ALUMINUM) +
            Weapon_Value(technotype, weapon, ARMOR_STEEL)) / 3;

        // Units with FiringSyncFrame can use both their primary and secondary weapon on the same target.
        if (Has_Firing_Sync(technotype)) {
            weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_SECONDARY).Weapon;
            if (weapon != nullptr) {
                value += (Weapon_Value(technotype, weapon, ARMOR_NONE) + Weapon_Value(technotype, weapon, ARMOR_ALUMINUM) +
                    Weapon_Value(technotype, weapon, ARMOR_STEEL)) / 3;
            }
        }
    }

    return value;
}


int TechnoTypeClassExtension::DefensiveValue() const
{
    const TechnoTypeClass* technotype = This_Const();
    int value = technotype->MaxStrength;

    if (technotype->IsCrushable) {
        const WeaponTypeClass* weapon = technotype->Fetch_Weapon_Info(WEAPON_SLOT_PRIMARY).Weapon;
        if (weapon != nullptr) {
            double reduction = (5 - (weapon->Range / CELL_LEPTON)) * 0.1;
            if (reduction > 0.0) {
                value = (int)(value * (1.0 - reduction));
            }
        }
    }

    return value;
}


bool TechnoTypeClassExtension::CategorizedAsLightlyArmored() const
{
    const ArmorType armor = This_Const()->Armor;
    return armor == ARMOR_WOOD || armor == ARMOR_ALUMINUM || armor == (ArmorType)5 || armor == (ArmorType)6 || armor == (ArmorType)7;
}


int TechnoTypeClassExtension::Scale_Value_By_Properties(int value, double speedmultiplier, double strengthmultiplier,
    double cloakmultiplier, double costweightmultiplier) const
{
    const TechnoTypeClass* technotype = This_Const();

    // Increase value if the unit is able to fire while moving.
    if (technotype->RTTI == RTTI_UNITTYPE) {
        const UnitTypeClass* unittype = static_cast<const UnitTypeClass*>(technotype);
        if (unittype->IsTurretEquipped && !unittype->IsNoFireWhileMoving) {
            value = (int)(value * 1.1);
        }
    }

    double speed = (double)technotype->MaxSpeed / 6.0;

    if (speedmultiplier < 0.0) {
        // Negative speed multiplier - give bonus to slower units.
        speed = 1.0 - speed;
        speedmultiplier = speedmultiplier * -1.0;
    }

    double strength = 0.0;

    int adjusted = (int)(value * (1.0 + speed * speedmultiplier + strength * strengthmultiplier));

    if (technotype->IsCloakable) {
        adjusted = (int)(adjusted + adjusted * cloakmultiplier);
    }

    if (technotype->Cost <= 0) {
        return adjusted;
    }

    if (costweightmultiplier == 1.0) {
        return adjusted / technotype->Cost;
    }

    return (int)(adjusted / std::pow(technotype->Cost, costweightmultiplier));
}
