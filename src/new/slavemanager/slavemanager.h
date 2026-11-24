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
#include "vector.h"


class TechnoClass;
class TechnoTypeClass;
class TechnoTypeClassExtension;
class UnitClass;
class BuildingClass;
class BuildingTypeClass;
class InfantryClass;
class InfantryTypeClass;
class CellClass;
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


/**
 *  States of a single slave. The numbering follows SlaveManagerClass::UpdateSlaves
 *  in Yuri's Revenge (gamemd.exe 0x006AF6C0); Ordered is new.
 */
enum class SlaveStatus {
    InMiner = 0,        // Inside the miner (limbo), waits until the miner works.
    Seeking = 1,        // Looks for Tiberium around itself.
    Moving = 2,         // Walks to the Tiberium cell.
    Harvesting = 3,     // Harvests until full or the cell is empty.
    Returning = 4,      // Walks back and enters the miner to unload.
    Reloading = 5,      // Inside the miner after unloading; healed when done.
    Dead = 6,           // Waits for the replacement.
    Ordered = 7         // Follows a player order; resumes work once idle.
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
    struct SlaveNode
    {
        InfantryClass *Slave;
        SlaveStatus Status;
        CDTimerClass<FrameTimerClass> Timer;
        Cell Target;
        int BestDistance;   // Closest the slave got to the door while returning, or to its Tiberium target while moving (leptons).
        int StuckCount;     // Times in a row the returning slave made no progress.
        Cell BadOre;        // Tiberium cell the slave could not walk to; skipped (with its neighbours) when seeking.
        int PathFails;      // Times in a row no path to the door was found.
    };

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

    /**
     *  AF-0012: The two kinds of Slave Miner. Slaves exist only when
     *  SlaveMiner.Slave= is set (on the vehicle or the building). Without
     *  slaves and with Refinery=yes on the building, the miner is a mobile
     *  refinery that normal harvesters unload at.
     */
    static bool Has_Slave_Type(const TechnoTypeClass *type);
    static bool Is_Mobile_Refinery_Type(const TechnoTypeClass *type);
    static void Transfer(TechnoClass *from, TechnoClass *to);
    static bool Should_Undeploy(const BuildingClass *building);
    static bool Accepts_Harvest_Order(const TechnoClass *techno);
    static bool Is_Harvest_Order_Cell(const Cell &cell);
    static bool Can_Slave_Enter_Cell(const InfantryClass *infantry, const CellClass *cell);
    static void Player_Order(TechnoClass *techno);
    static bool Is_Repairable(const BuildingClass *building);
    static Cell Field_Edge(const Cell &ore, const Cell &toward);

    /**
     *  Can the miner's building stand at "topleft" without walling in its own
     *  door or another Slave Miner's door, with "ore" reachable on foot from
     *  in front of the door?
     */
    bool Is_Door_Site_Clear(const BuildingTypeClass *buildingtype, const Cell &topleft, const Cell &ore, const char **reason = nullptr) const;

    int Slave_Count() const;

    SlaveManagerClass(const SlaveManagerClass &) = delete;
    SlaveManagerClass &operator=(const SlaveManagerClass &) = delete;

private:
    void Set_Status(SlaveManagerStatus status);
    void Reset_Computer_Order(UnitClass *unit);
    void Retry_Or_Stop();

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

    void Create_Slaves();
    void Free_Slaves();
    void Slaves_AI();
    void Slave_AI(SlaveNode &node);
    void Slave_Seek(SlaveNode &node);
    void Slave_Return(SlaveNode &node);
    void Slave_Enter(SlaveNode &node, const char *reason);
    void Absorb_Slaves();
    void Slave_Return_To_Building(SlaveNode &node);
    void Slave_Wait_For_Vehicle(SlaveNode &node);
    bool Is_Next_To_Miner(const Cell &cell) const;
    void Planned_Footprint(Cell &topleft, Cell &bottomright) const;
    int Slave_Reach() const;
    Cell Remembered_Ore(int zone);
    int Slave_Zone(const Cell &cell) const;
    Cell Door_Outside() const;
    bool Can_Relocate() const;
    const char *Release_Blocker();
    int Manager_Index() const;
    void Log_Slave(const SlaveNode &node, const char *event, const char *details = nullptr) const;
    void Log_Snapshot() const;
    void Log_Door_Area(const SlaveNode &node, const char *event);
    bool Is_On_Miner(const InfantryClass *slave) const;
    bool Slave_Exit(SlaveNode &node);
    void Slave_Lost(InfantryClass *slave);
    void Slave_Move(SlaveNode &node, const Cell &cell);
    bool Is_Slave_Full(const InfantryClass *slave) const;
    bool Can_Release_Slaves() const;
    Cell Door_Cell() const;
    SlaveNode *Find_Node(const InfantryClass *slave) const;

    const TechnoTypeClassExtension *Slave_Config() const;
    const InfantryTypeClass *Slave_Type() const;
    int Slave_Scan() const;
    int Slave_Harvest_Rate(const InfantryClass *slave) const;

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

    /**
     *  The slaves (also the dead ones, waiting to be replaced).
     */
    DynamicVectorClass<SlaveNode *> Slaves;

    /**
     *  Were the slaves created yet? They are made on the first logic tick,
     *  so a manager that is replaced right away (Transfer) creates none.
     */
    bool IsSlavesCreated;

    /**
     *  Set while the manager itself puts a slave into limbo, so the detach
     *  notification of that call is not taken as the slave's death.
     */
    bool IsLimboingSlave;

    /**
     *  Set when the miner deployed: slaves standing on the new building's
     *  cells are taken in on the next logic tick.
     */
    bool IsAbsorbPending;

    /**
     *  The last Tiberium cell the miner knows of (harvested by a slave, or the
     *  target of a harvest order or relocation). Slaves walk there when no
     *  Tiberium is within their reach.
     */
    Cell LastOre;

    /**
     *  Frame of the last wide search for Tiberium after LastOre ran out.
     */
    int LastOreScanFrame;

    /**
     *  Test log only (SlaveMinerLog): why slaves were last kept inside, and
     *  when the last overview was written. No influence on the game.
     */
    const char *LastReleaseBlocker;
    int LastSnapshotFrame;
    int LastCellLogFrame;
};
