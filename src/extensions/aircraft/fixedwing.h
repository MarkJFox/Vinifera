/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "tibsun_defines.h"

class AircraftClass;
class AircraftTypeClass;
class BuildingClass;
class FlyLocomotionClass;
class FootClass;
class AbstractClass;
class EventClass;

/** Flight legs use engine fields; AircraftExtension retains survival, CAP and attack ownership. */
namespace FixedWing
{
    bool Supported_Type(const AircraftTypeClass* type);
    bool Enabled(const AircraftClass* aircraft);
    // Ownership persists through target loss until native Attack finishes.
    bool Native_Attack(const AircraftClass* aircraft);
    // Observe the existing native Idle entry, without changing mission/movement.
    void Native_Attack_Idle(AircraftClass* aircraft);
    AircraftClass* Controlled(FlyLocomotionClass* fly);
    FlyLocomotionClass* Locomotor(AircraftClass* aircraft);
    bool Dock_Candidate(AircraftClass* aircraft, BuildingClass* dock);
    bool Has_Compatible_Dock(AircraftClass* aircraft);
    // True means death handling ran; the caller must not touch the aircraft again.
    bool No_Dock_AI(AircraftClass* aircraft);
    void Initialize_Spawn(AircraftClass* aircraft);
    bool Idle(AircraftClass* aircraft);
    void Request_Return(AircraftClass* aircraft);
    // Called only for accepted synchronized commands, never for automatic attacks.
    void Command(AircraftClass* aircraft, MissionType mission, AbstractClass* target);
    // Developer-only observations; never change simulation or persistent state.
    void Trace_CAP_Command(AircraftClass* aircraft, MissionType mission, const EventClass* event);
    void Trace_CAP_State(AircraftClass* aircraft, const char* source);
    void Reset_CAP_Trace();
    void Cancel_CAP(AircraftClass* aircraft);
    void CAP_AI(AircraftClass* aircraft);
    bool Resume_After_Service(AircraftClass* aircraft);
    int Mission_CAP(AircraftClass* aircraft);
    void Assign_Destination(AircraftClass* aircraft, const AbstractClass* target, bool update);
    int Mission_Move(AircraftClass* aircraft);
    int Mission_Enter(AircraftClass* aircraft);
    bool Process(FlyLocomotionClass* fly, AircraftClass* aircraft);
}
