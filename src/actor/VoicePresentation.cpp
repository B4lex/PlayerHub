#include "actor/VoicePresentation.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/AttackDispatch.h"
#include "actor/IdlePresentation.h"
#include "actor/RemoteActor.h"
#include "actor/WeaponTransition.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"

#include <algorithm>

namespace VoicePresentation
{
    namespace
    {
        using VoiceState = RE::VOICE_STATE;

        constexpr float kStartRetrySeconds = 0.25F;
        // The source decides within fShoutTime2 of its start; a charge still undecided
        // after this lost its release.
        constexpr float kChargeHoldFallbackSeconds = 2.0F;
        // Beyond the engine's own fShoutTimeout for a request or a release.
        constexpr float kShoutFallbackSeconds = 12.0F;

        float GetFloatSetting(const char* a_name, float a_default)
        {
            auto* settings = RE::GameSettingCollection::GetSingleton();
            const auto* setting = settings ? settings->GetSetting(a_name) : nullptr;
            return setting && setting->GetType() == RE::Setting::Type::kFloat ? setting->GetFloat() : a_default;
        }

        // The voice state update (ID 38805, SE 37851) leaves kStart once voiceTimeElapsed
        // reaches fShoutTime1 and kContinue once it reaches fShoutTime2, each earlier only
        // for the player when the shout key is up. It then reads the actor's shout level,
        // which for an NPC is the high process's currentShoutVariation (ID 38810): level 0
        // releases from kStart, any other enters kContinue, and kContinue releases that level.
        float GetDecisionSeconds(VoiceState a_state)
        {
            static const float startSeconds = GetFloatSetting("fShoutTime1", 0.2F);
            static const float continueSeconds = GetFloatSetting("fShoutTime2", 0.9F);
            return a_state == VoiceState::kStart ? startSeconds : continueSeconds;
        }

        constexpr bool IsCharging(VoiceState a_state)
        {
            return a_state == VoiceState::kRequest || a_state == VoiceState::kStart || a_state == VoiceState::kContinue;
        }

        // The actor's shout start (ID 38804, SE 37850), which the player's shout key and
        // NPC combat call: fails while staggered or while the voice state is not idle,
        // stores the shout and variation in the high process and enters kRequest, then
        // requests the variation's spell from the voice caster.
        bool StartShout(RE::Actor& a_actor, RE::TESShout* a_shout, std::int32_t a_variation)
        {
            using func_t = bool(RE::Actor*, RE::TESShout*, std::int32_t, RE::TESObjectREFR*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(37850, 38804) };
            return func(std::addressof(a_actor), a_shout, a_variation, nullptr);
        }

        // The highest variation up to the word count that has a spell, else the lowest above
        // it; -1 when none has.
        std::int32_t SelectVariation(const RE::TESShout& a_shout, std::uint8_t a_words)
        {
            constexpr auto kVariations = static_cast<std::int32_t>(RE::TESShout::VariationIDs::kTotal);
            const std::int32_t wanted = std::clamp<std::int32_t>(a_words, 1, kVariations) - 1;
            for (std::int32_t variation = wanted; variation >= 0; --variation)
            {
                if (a_shout.variations[variation].spell)
                {
                    return variation;
                }
            }
            for (std::int32_t variation = wanted + 1; variation < kVariations; ++variation)
            {
                if (a_shout.variations[variation].spell)
                {
                    return variation;
                }
            }
            return -1;
        }

        void SetOwnership(RemoteActor& a_remote, bool a_owned)
        {
            auto& voice = a_remote.animation.voice;
            if (voice.owned != a_owned)
            {
                voice.owned = a_owned;
                ActorRegistry::SetVoiceAnimationOwned(a_remote, a_owned);
            }
        }

        // The voice caster's interrupt performs the voice interrupt action (shoutStop).
        // The voice state is then cleared as its reset (ID 39524) does for a shout that
        // has not released: nothing has played that the reset would stop.
        void InterruptShout(RE::Actor& a_actor, RE::HighProcessData& a_high)
        {
            if (auto* caster = a_actor.GetMagicCaster(RE::MagicSystem::CastingSource::kOther))
            {
                caster->InterruptCast(false);
            }
            a_high.voiceState = VoiceState::kNone;
            a_high.currentShout = nullptr;
            a_high.currentShoutVariation = RE::TESShout::VariationIDs::kNone;
            a_high.voiceTimeElapsed = 0.0F;
        }

        bool CanStart(const RemoteActor& a_remote, const RE::Actor& a_actor)
        {
            const auto& state = a_remote.animation;
            if (!a_remote.ready || !a_remote.animationGraphHolder || !ActorAnimation::IsAnimationStateSettled(a_remote)
                || state.recovery.inProgress || state.dispatch.reassertReason || AttackDispatch::IsPlaying(a_remote)
                || IdlePresentation::IsPlaying(a_remote) || WeaponTransition::IsUnsettled(a_remote))
            {
                return false;
            }
            const auto* actorState = a_actor.AsActorState();
            if (!actorState || actorState->IsSwimming())
            {
                return false;
            }
            const auto weaponState = actorState->GetWeaponState();
            return weaponState == RE::WEAPON_STATE::kDrawn || weaponState == RE::WEAPON_STATE::kSheathed;
        }

        void EndShout(RemoteActor& a_remote, const char* a_reason, VoiceState a_voiceState)
        {
            auto& voice = a_remote.animation.voice;
            logs::debug("Voice sync ended shout: actor={:08X} shout={:08X} reason={} voiceState={} heldSeconds={:.3f}",
                a_remote.handle.native_handle(), voice.shout ? voice.shout->GetFormID() : 0, a_reason,
                static_cast<std::uint32_t>(a_voiceState), voice.heldElapsed);
            voice.held = false;
            voice.shout = nullptr;
            voice.heldElapsed = 0.0F;
            SetOwnership(a_remote, false);
        }
    }

    void ApplyShout(RemoteActor& a_remote, const ShoutRequest& a_request, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplyShout"))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }
        auto& actor = *actorPtr;
        auto& voice = a_remote.animation.voice;
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        voice.startCooldown = (std::max)(voice.startCooldown - deltaTime, 0.0F);
        if (!a_request.shouting || !a_request.released)
        {
            voice.awaitingNewShout = false;
        }

        auto* high = actor.GetHighProcess();
        const auto voiceState = high ? high->voiceState.get() : VoiceState::kNone;
        if (voice.held)
        {
            voice.heldElapsed += deltaTime;
            if (!high || voiceState == VoiceState::kNone)
            {
                voice.awaitingNewShout = a_request.shouting && a_request.released;
                EndShout(a_remote, "finished", voiceState);
                return;
            }
            if (voice.heldElapsed >= kShoutFallbackSeconds)
            {
                InterruptShout(actor, *high);
                EndShout(a_remote, "timed-out", voiceState);
                return;
            }
            // From its release on, the shout plays out natively.
            if (!IsCharging(voiceState))
            {
                return;
            }
            if (!a_request.shouting)
            {
                InterruptShout(actor, *high);
                EndShout(a_remote, "interrupted", voiceState);
                return;
            }
            if (const auto variation = SelectVariation(*voice.shout, a_request.words); variation >= 0)
            {
                high->currentShoutVariation = static_cast<RE::TESShout::VariationID>(variation);
            }
            if (voiceState == VoiceState::kRequest)
            {
                return;
            }
            // The charge waits for the source's decision: a release, or a second word
            // while in kStart. Once decided, the next voice update moves on.
            const bool decided = a_request.released || (voiceState == VoiceState::kStart && a_request.words >= 2)
                || voice.heldElapsed >= kChargeHoldFallbackSeconds;
            high->voiceTimeElapsed = decided ? (std::max)(high->voiceTimeElapsed, GetDecisionSeconds(voiceState)) : 0.0F;
            return;
        }

        if (!a_request.shouting || !a_request.shout || voice.awaitingNewShout || !a_mayStart
            || voice.startCooldown > 0.0F || !high || voiceState != VoiceState::kNone || !CanStart(a_remote, actor))
        {
            return;
        }
        const auto variation = SelectVariation(*a_request.shout, a_request.words);
        if (variation < 0)
        {
            return;
        }
        voice.startCooldown = kStartRetrySeconds;
        auto& runtimeData = actor.GetActorRuntimeData();
        // The shouted words' dialogue is conditioned on GetEquippedShout, which reads
        // selectedPower (ID 38771, SE 37822), as the voice state update says them.
        runtimeData.selectedPower = a_request.shout;
        // The voice caster's graph events reach the firewall synchronously and on later
        // updates, and its request checks the casting-disabled flag.
        SetOwnership(a_remote, true);
        runtimeData.boolFlags.reset(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        if (!StartShout(actor, a_request.shout, variation))
        {
            SetOwnership(a_remote, false);
            logs::debug("Voice sync shout start rejected: actor={:08X} shout={:08X} variation={}",
                a_remote.handle.native_handle(), a_request.shout->GetFormID(), variation);
            return;
        }
        voice.held = true;
        voice.shout = a_request.shout;
        voice.heldElapsed = 0.0F;
        logs::debug("Voice sync started shout: actor={:08X} shout={:08X} variation={} released={}",
            a_remote.handle.native_handle(), a_request.shout->GetFormID(), variation, a_request.released);
    }

    void Release(RemoteActor& a_remote)
    {
        if (!GameThread::Require("ReleaseShout"))
        {
            return;
        }
        auto& voice = a_remote.animation.voice;
        voice.awaitingNewShout = false;
        if (!voice.held)
        {
            SetOwnership(a_remote, false);
            return;
        }
        auto actorPtr = a_remote.handle.get();
        auto* high = actorPtr ? actorPtr->GetHighProcess() : nullptr;
        const auto voiceState = high ? high->voiceState.get() : VoiceState::kNone;
        if (high && IsCharging(voiceState))
        {
            InterruptShout(*actorPtr, *high);
        }
        EndShout(a_remote, "released", voiceState);
    }

    bool CastPower(RemoteActor& a_remote, RE::SpellItem& a_power)
    {
        if (!GameThread::Require("CastPower"))
        {
            return false;
        }
        auto actorPtr = a_remote.handle.get();
        auto* caster = actorPtr ? actorPtr->GetMagicCaster(RE::MagicSystem::CastingSource::kOther) : nullptr;
        if (!caster)
        {
            return false;
        }
        // The magic caster's spell cast without an animation (ID 34409, SE 33631), which
        // the player's shout key calls on its release for a power: selects the spell and,
        // for a power, fires it at once through spell fire (ID 34407).
        using func_t = bool(RE::MagicCaster*, RE::MagicItem*, RE::TESObjectREFR*, RE::TESObjectREFR*, bool);
        static REL::Relocation<func_t> func{ RELOCATION_ID(33631, 34409) };
        auto& actorFlags = actorPtr->GetActorRuntimeData().boolFlags;
        actorFlags.reset(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        const bool cast = func(caster, std::addressof(a_power), nullptr, nullptr, false);
        if (!a_remote.animation.voice.owned && !ActorRegistry::IsCastAnimationOwned(actorPtr.get()))
        {
            actorFlags.set(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        }
        return cast;
    }

    bool IsShouting(const RemoteActor& a_remote)
    {
        return a_remote.animation.voice.held;
    }
}
