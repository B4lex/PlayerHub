#include "sync/death/DeathSync.h"
#include "actor/ActorDeath.h"
#include "sync/ActorLifecycle.h"
#include "sync/PeerState.h"
#include "sync/combat/CombatSync.h"

#include <type_traits>

namespace DeathSync
{
    bool SampleLocalDeath(const LocalFrame& a_local, std::uint64_t a_sequence)
    {
        static bool previousDead = false;

        // Bleedout and an essential actor's collapse are not death: the player
        // gets up again without a load.
        const auto* actorState = a_local.player.AsActorState();
        const auto lifeState = actorState ? actorState->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
        const bool dead = lifeState == RE::ACTOR_LIFE_STATE::kDying || lifeState == RE::ACTOR_LIFE_STATE::kDead;
        if (dead != previousDead)
        {
            logs::debug("Publishing local player life state: sequence={} dead={} lifeState={}",
                static_cast<unsigned long long>(a_sequence), dead,
                static_cast<std::underlying_type_t<RE::ACTOR_LIFE_STATE>>(lifeState));
            previousDead = dead;
        }
        return dead;
    }

    bool SyncDeath(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        if (a_remote.checkpoint.dead && a_peer.lifecycle.mount)
        {
            // A rider is never taken off its mount (see ActorMount); its replacement is killed
            // once it is ready.
            ActorLifecycle::RetireMountedForDeath(a_remote, a_peer);
            return false;
        }

        auto& actor = *a_peer.lifecycle.actor;
        if (actor.dead)
        {
            if (a_remote.checkpoint.dead)
            {
                return true;
            }
            ActorLifecycle::RetireForRevival(a_remote, a_peer);
            return false;
        }

        if (!a_remote.checkpoint.dead || !actor.ready)
        {
            return false;
        }

        // Attacks queued before the death must not play on the body.
        CombatSync::ClearRemoteActions(a_peer.combat);
        const bool killed = ActorDeath::Kill(actor);
        logs::debug("Remote player {} died: actor={:08X} killed={} sequence={}", a_remote.playerID,
            actor.handle.native_handle(), killed, static_cast<unsigned long long>(a_remote.lastSequence));
        return true;
    }
}
