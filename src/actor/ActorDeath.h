#pragma once

struct RemoteActor;

namespace ActorDeath
{
    // Kills a ready remote actor into an immediate ragdoll after its player died.
    // The kill has no attacker and sends no death event, so no local quest,
    // crime, or statistic observes it. The actor is released from its survival
    // policy and animation firewall and must not be driven again. Returns whether
    // Skyrim accepted the kill. Main-loop thread only.
    bool Kill(RemoteActor& a_remote);
}
