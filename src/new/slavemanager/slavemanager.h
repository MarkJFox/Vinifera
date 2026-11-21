/*******************************************************************************
/*                 O P E N  S O U R C E  --  V I N I F E R A                  **
/*******************************************************************************
 *
 *  @project       Vinifera
 *
 *  @file          SLAVEMANAGER.H
 *
 *  @author        Aftermath
 *
 *  @brief         YR-style Slave Miner logic (mobile refinery that seeks
 *                 Tiberium, deploys next to it, harvests and relocates).
 *
 *  @license       Vinifera is free software: you can redistribute it and/or
 *                 modify it under the terms of the GNU General Public License
 *                 as published by the Free Software Foundation, either version
 *                 3 of the License, or (at your option) any later version.
 *
 *                 Vinifera is distributed in the hope that it will be
 *                 useful, but WITHOUT ANY WARRANTY; without even the implied
 *                 warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *                 PURPOSE. See the GNU General Public License for more details.
 *
 *                 You should have received a copy of the GNU General Public
 *                 License along with this program.
 *                 If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#pragma once

#include "abstract.h"
#include "detach_listener.h"
#include "ttimer.h"
#include "ftimer.h"
#include "tibsun_defines.h"
#include "vinifera_defines.h"


class TechnoClass;
class TechnoTypeClass;
class TechnoTypeClassExtension;
class UnitClass;
class BuildingClass;
class CRCEngine;


/**
 *  States of the miner itself. The numbering follows the state machine of
 *  SlaveManagerClass::UpdateMiner in Yuri's Revenge (gamemd.exe 0x006AFD60).
 */
enum class SlaveManagerStatus {
    Idle = 0,           // Vehicle waits to be woken up; a building goes straight to Working.
    Scanning = 1,       // Vehicle looks for Tiberium (long scan) and a site to deploy at.
    Travelling = 2,     // Vehicle drives to the deploy site.
    DeployRetry = 3,    // Deploying failed once, wait and try again.
    Deploying = 4,      // Vehicle is deploying (rotating / unpacking).
    Working = 5,        // Deployed and harvesting; relocates when the field is depleted.
    PackingUp = 6       // Building undeploys to move to a better site.
};


class DECLSPEC_UUID(UUID_SLAVE_MANAGER)
    SlaveManagerClass : public AbstractClass,
                        public Vinifera::Detach::Listener<TechnoClass>
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
    SlaveManagerClass();
    SlaveManagerClass(TechnoClass *owner);
    SlaveManagerClass(const NoInitClass &noinit);
    virtual ~SlaveManagerClass() override;

    /**
     *  AbstractClass
     */
    virtual RTTIType Fetch_RTTI() const override;
    virtual int Get_Object_Size(bool firestorm = false) const override;
    virtual void Object_CRC(CRCEngine &crc) const override;
    virtual void AI() override;

    /**
     *  Detach listener: the owner went away.
     */
    void On_Detach(TechnoClass *target, bool all) override;

    void Set_Owner(TechnoClass *owner);
    void Stop();

    SlaveManagerStatus Get_Status() const { return Status; }

    static bool Is_Slave_Miner_Type(const TechnoTypeClass *type);
    static void Transfer(TechnoClass *from, TechnoClass *to);
    static bool Should_Undeploy(const BuildingClass *building);
    static bool Accepts_Harvest_Order(const TechnoClass *techno);
    static bool Is_Harvest_Order_Cell(const Cell &cell);

    SlaveManagerClass(const SlaveManagerClass &) = delete;
    SlaveManagerClass &operator=(const SlaveManagerClass &) = delete;

private:
    void Set_Status(SlaveManagerStatus status);

    void AI_Idle();
    void AI_Scanning();
    void AI_Travelling();
    void AI_Deploy_Retry();
    void AI_Deploying();
    void AI_Working();
    void AI_Packing_Up();

    void Handle_Harvest_Order();
    bool Relocate(BuildingClass *building, const Cell &ore, bool check_correction);

    bool Should_Wake_Up() const;
    bool Start_Deploy();
    bool Was_Given_Other_Order(const UnitClass *unit) const;
    void Check_Relocation(BuildingClass *building);

    UnitClass *Owner_Unit() const;
    BuildingClass *Owner_Building() const;

    const TechnoTypeClassExtension *Config() const;
    int Short_Scan() const;
    int Long_Scan() const;
    int Scan_Correction() const;
    int Kick_Frame_Delay() const;
    int Harvest_Radius() const;

public:
    /**
     *  The object this manager belongs to: the vehicle or the deployed building.
     */
    TechnoClass *Owner;

    /**
     *  Current state of the miner.
     */
    SlaveManagerStatus Status;

    /**
     *  The manager only runs its logic every few frames.
     */
    CDTimerClass<FrameTimerClass> LogicTimer;

    /**
     *  Wait timer for the DeployRetry state.
     */
    CDTimerClass<FrameTimerClass> StateTimer;

    /**
     *  Frame of the last scan; INT_MAX while busy (like YR's LastScanFrame).
     */
    int LastScanFrame;

    /**
     *  Frame of the last relocation scan of a deployed miner.
     */
    int LastRelocationScanFrame;

    /**
     *  The cell the vehicle has to stand on to deploy at the chosen site.
     */
    Cell DeploySite;

    /**
     *  The Tiberium cell the player sent the miner to (CELL_NONE: scan on our own).
     */
    Cell OrderedOre;

    /**
     *  How many deploy attempts failed in a row.
     */
    int DeployAttempts;

    /**
     *  Does the miner have a harvest order? Only then it relocates on its own
     *  once the field is depleted (YR behaviour for human players). Cleared by
     *  any other order such as a move order or a manual undeploy.
     */
    bool HasHarvestOrder;

    /**
     *  Did the manager itself start the current undeploy (to relocate)?
     */
    bool IsRelocating;
};
