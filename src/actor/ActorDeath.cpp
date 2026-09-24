#include "actor/ActorDeath.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"

#include <type_traits>

namespace ActorDeath
{
    bool Kill(RemoteActor& a_remote)
    {
        if (!GameThread::Require("KillRemoteActor"))
        {
            return false;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return false;
        }

        auto* actor = actorPtr.get();
        a_remote.dead = true;
        ActorFurniture::ReleaseFurniture(*actor, a_remote);
        // A dead actor is never updated again, and its body must land on the world.
        RemoteCollision::ReleaseToEngine(*actor, a_remote, "death", false);
        // Before the kill: the registry's kill and damage guards would refuse it,
        // and the survival policy would resurrect the actor on its next update.
        ActorRegistry::SetDead(a_remote);
        actor->GetActorRuntimeData().boolFlags.reset(
            RE::Actor::BOOL_FLAGS::kEssential, RE::Actor::BOOL_FLAGS::kProtected);
        actor->KillImpl(nullptr, 0.0F, false, true);

        const auto* actorState = actor->AsActorState();
        const auto lifeState = actorState ? actorState->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
        const bool killed = lifeState == RE::ACTOR_LIFE_STATE::kDying || lifeState == RE::ACTOR_LIFE_STATE::kDead;
        if (!killed)
        {
            logs::warn("Skyrim did not kill remote actor: actor={:08X} lifeState={} loaded3D={}",
                a_remote.handle.native_handle(), static_cast<std::underlying_type_t<RE::ACTOR_LIFE_STATE>>(lifeState),
                actor->Is3DLoaded());
        }
        return killed;
    }
}
