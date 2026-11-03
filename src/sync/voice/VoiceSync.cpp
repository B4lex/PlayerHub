#include "sync/voice/VoiceSync.h"
#include "actor/RemoteActor.h"
#include "actor/VoicePresentation.h"
#include "forms/FormLookup.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace VoiceSync
{
    namespace
    {
        using VoiceState = RE::VOICE_STATE;

        constexpr RE::FormID kPlayerReferenceFormID = 0x00000014;
        constexpr std::size_t kMaximumQueuedPowers = 8;
        constexpr float kPowerActionTimeoutSeconds = 3.0F;

        Multiplayer::VoicePresentation s_currentVoice;
        Multiplayer::VoicePresentation s_unpublishedVoice;

        std::mutex s_castLock;
        std::vector<RE::FormID> s_observedPowerCasts;
        std::vector<Multiplayer::FormReference> s_pendingPowers;

        bool IsPower(const RE::SpellItem& a_spell)
        {
            const auto type = a_spell.GetSpellType();
            return type == RE::MagicSystem::SpellType::kPower || type == RE::MagicSystem::SpellType::kLesserPower;
        }

        // The voice state update (ID 38805) stores a released shout's variation before
        // its release; while charging, kContinue already means a second word.
        Multiplayer::VoicePresentation GetVoicePresentation(RE::PlayerCharacter& a_player)
        {
            const auto* high = a_player.GetHighProcess();
            if (!high || !high->currentShout)
            {
                return {};
            }
            const auto state = high->voiceState.get();
            const bool released = state == VoiceState::kPrecast || state == VoiceState::kPostcast;
            if (!released && state != VoiceState::kRequest && state != VoiceState::kStart
                && state != VoiceState::kContinue)
            {
                return {};
            }
            const auto variation = static_cast<std::int32_t>(high->currentShoutVariation);
            std::uint8_t words = state == VoiceState::kContinue ? 2 : 1;
            if (released && variation >= 0)
            {
                words = static_cast<std::uint8_t>(
                    (std::min)(variation + 1, static_cast<std::int32_t>(Multiplayer::VoicePresentation::kMaximumWords)));
            }
            return { .shouting = true, .released = released, .words = words };
        }

        // TESSpellCastEvent is sent for each spell the player's casters fire, including a
        // power the shout key casts, possibly off the main-loop thread.
        class SpellCastEventSink final : public RE::BSTEventSink<RE::TESSpellCastEvent>
        {
        public:
            static SpellCastEventSink* GetSingleton()
            {
                static SpellCastEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
            {
                if (a_event && a_event->object && a_event->object->GetFormID() == kPlayerReferenceFormID)
                {
                    const std::scoped_lock lock{ s_castLock };
                    if (s_observedPowerCasts.size() < kMaximumQueuedPowers)
                    {
                        s_observedPowerCasts.push_back(a_event->spell);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterEventSinks()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSources)
        {
            logs::critical("Voice sync could not register the spell cast event sink");
            return;
        }
        eventSources->AddEventSink<RE::TESSpellCastEvent>(SpellCastEventSink::GetSingleton());
        registered = true;
        logs::debug("Registered local spell cast event sink");
    }

    bool CaptureLocalVoice(const LocalFrame& a_local, bool a_transformed)
    {
        const auto previous = s_currentVoice;
        // A beast form's howls are shouts that its own graph's actions replay.
        const auto current = a_transformed ? Multiplayer::VoicePresentation{} : GetVoicePresentation(a_local.player);
        s_currentVoice = current;
        s_unpublishedVoice.shouting |= current.shouting;
        s_unpublishedVoice.released |= current.released;
        s_unpublishedVoice.words = (std::max)(s_unpublishedVoice.words, current.words);
        if (current == previous)
        {
            return false;
        }
        const auto* high = a_local.player.GetHighProcess();
        logs::debug("Voice sync observed local shout: shouting={} released={} words={} voiceState={} shout={:08X}",
            current.shouting, current.released, current.words,
            high ? static_cast<std::uint32_t>(high->voiceState.get()) : 0,
            high && high->currentShout ? high->currentShout->GetFormID() : 0);
        return current.shouting && !previous.shouting;
    }

    Multiplayer::VoicePresentation SampleLocalVoice()
    {
        // Capture runs earlier in the same pass, so this always includes the current shout.
        return std::exchange(s_unpublishedVoice, {});
    }

    void QueueLocalPowers(bool a_transformed, bool a_graphEpochChanged)
    {
        if (a_graphEpochChanged)
        {
            // A cast from the previous graph cannot be attached to a checkpoint from this new epoch.
            s_pendingPowers.clear();
        }
        std::vector<RE::FormID> observed;
        {
            const std::scoped_lock lock{ s_castLock };
            if (s_observedPowerCasts.empty())
            {
                return;
            }
            observed.swap(s_observedPowerCasts);
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto* selected = player ? player->GetActorRuntimeData().selectedPower : nullptr;
        for (const auto formID : observed)
        {
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(formID);
            // Only the voice slot's power, which the shout key casts; a script may cast others.
            if (a_transformed || !spell || spell != selected || !IsPower(*spell))
            {
                continue;
            }
            auto reference = FormLookup::MakeFormReference(spell);
            logs::debug("Voice sync observed local power cast: power={:08X} referenceable={}", formID,
                reference.has_value());
            if (reference && s_pendingPowers.size() < kMaximumQueuedPowers)
            {
                s_pendingPowers.push_back(std::move(*reference));
            }
        }
    }

    void DiscardLocalVoice()
    {
        s_pendingPowers.clear();
        s_unpublishedVoice = s_currentVoice;
    }

    void PublishLocalPowers(const Network::PlayerTransform& a_checkpoint)
    {
        if (a_checkpoint.furniture.IsEmpty() && a_checkpoint.shapeshift == Multiplayer::Shapeshift::kNone)
        {
            for (const auto& power : s_pendingPowers)
            {
                Network::PublishLocalPowerAction(power, a_checkpoint.graphEpoch, a_checkpoint.sequence);
            }
        }
        s_pendingPowers.clear();
    }

    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action)
    {
        if (!a_state || !a_checkpoint || a_action.power.IsEmpty() || a_action.graphEpoch < a_checkpoint->graphEpoch
            || !a_checkpoint->furniture.IsEmpty())
        {
            logs::debug("Voice sync rejected power action without a compatible checkpoint: player={} "
                        "actionSequence={} graphEpoch={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence),
                static_cast<unsigned long long>(a_action.graphEpoch));
            return;
        }
        if (a_state->pendingPowers.size() >= kMaximumQueuedPowers)
        {
            logs::warn("Voice sync rejected power action because the bounded queue is full: player={} "
                       "actionSequence={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence));
            return;
        }
        a_state->pendingPowers.push_back(PendingPower{ .action = std::move(a_action) });
    }

    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta)
    {
        const float delta = FrameTime::Clamp(a_delta);
        auto& queue = a_state.pendingPowers;
        for (auto& pending : queue)
        {
            pending.age += delta;
        }
        while (!queue.empty() && queue.front().age >= kPowerActionTimeoutSeconds)
        {
            const auto& power = queue.front().action.power;
            logs::warn("Voice sync timed out queued power action: player={} power={}|{:06X} actionSequence={}",
                a_remote.playerID, power.plugin, power.localFormID,
                static_cast<unsigned long long>(queue.front().action.actionSequence));
            queue.pop_front();
        }
    }

    void ClearRemoteActions(RemoteState& a_state)
    {
        a_state.pendingPowers.clear();
    }

    void ClearRemote(RemoteState& a_state, RemoteActor& a_actor)
    {
        ClearRemoteActions(a_state);
        VoicePresentation::Release(a_actor);
    }

    void SyncVoice(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, RE::TESShout* a_shout,
        float a_delta)
    {
        PROFILE_FUNCTION();
        const auto& checkpoint = a_remote.checkpoint;
        const auto& voice = checkpoint.voice;
        VoicePresentation::ApplyShout(a_actor,
            VoicePresentation::ShoutRequest{
                .shout = a_shout, .shouting = voice.shouting, .released = voice.released, .words = voice.words },
            a_actor.ready, a_delta);

        // One power per pass, once the checkpoint it follows has played.
        auto& queue = a_state.pendingPowers;
        if (queue.empty() || !a_actor.ready || queue.front().action.checkpointSequence > checkpoint.sequence)
        {
            return;
        }
        const auto pending = std::move(queue.front());
        queue.pop_front();
        const auto& reference = pending.action.power;
        if (pending.action.graphEpoch != checkpoint.graphEpoch)
        {
            logs::debug("Voice sync dropped power action from another graph epoch: player={} actionSequence={}",
                a_remote.playerID, static_cast<unsigned long long>(pending.action.actionSequence));
            return;
        }
        auto* spell = FormLookup::ResolveForm<RE::SpellItem>(reference);
        if (!spell || !IsPower(*spell))
        {
            logs::debug("Voice sync could not resolve remote power: player={} power={}|{:06X} found={}",
                a_remote.playerID, reference.plugin, reference.localFormID, spell != nullptr);
            return;
        }
        const bool cast = VoicePresentation::CastPower(a_actor, *spell);
        logs::debug("Voice sync cast remote power: player={} actor={:08X} power={:08X} cast={} actionSequence={}",
            a_remote.playerID, a_actor.handle.native_handle(), spell->GetFormID(), cast,
            static_cast<unsigned long long>(pending.action.actionSequence));
    }
}
