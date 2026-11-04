#include "actor/VoicePresentation.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/AttackDispatch.h"
#include "actor/GameplayPolicy.h"
#include "actor/IdlePresentation.h"
#include "actor/RemoteActor.h"
#include "actor/SpellIsolation.h"
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
        // The source ends a released shout about when the actor does, both seen through
        // the playout delay; a release still held this long after the source's ended or
        // was followed by another lingers. A release in kPrecast waits as long for its fire.
        constexpr float kOutlastSeconds = 1.0F;

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

        void SetFireSuppressed(RemoteActor& a_remote, bool a_suppressed)
        {
            auto& voice = a_remote.animation.voice;
            if (voice.fireSuppressed == a_suppressed)
            {
                return;
            }
            voice.fireSuppressed = a_suppressed;
            ActorRegistry::SetVoiceFireSuppressed(a_remote, a_suppressed);
            if (a_suppressed)
            {
                logs::debug("Voice sync suppresses the fire of a shout that failed on its source: actor={:08X} "
                            "shout={:08X}",
                    a_remote.handle.native_handle(), voice.shout ? voice.shout->GetFormID() : 0);
            }
        }

        // The voice reset (ID 39524, SE 38514) on the actor's process: with a_clear, it
        // returns to kNone, clears the shout, variation, and elapsed time, and releases
        // the word's sound, as kPostcast's end does.
        void ResetVoice(RE::Actor& a_actor)
        {
            auto* process = a_actor.GetActorRuntimeData().currentProcess;
            if (!process)
            {
                return;
            }
            using func_t = void(RE::AIProcess*, bool, const char*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(38514, 39524) };
            func(process, true, nullptr);
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
            voice.outlastedElapsed = 0.0F;
            SetFireSuppressed(a_remote, false);
            SetOwnership(a_remote, false);
        }

        // The voice update (ID 38805) ends kPostcast once the actor is not speaking
        // (HighProcessData::unk467, else no queueofGreetings) and kSoundFileDone, the
        // voice caster holds no spell, and the graph's IsShouting is false; kRequest and
        // kPrecast, and kPostcast otherwise, end at fShoutTimeout. Logs which condition
        // held the voice.
        void EndLingeringShout(RemoteActor& a_remote, RE::Actor& a_actor, const RE::HighProcessData& a_high,
            VoiceState a_voiceState, const char* a_reason)
        {
            auto* caster = a_actor.GetMagicCaster(RE::MagicSystem::CastingSource::kOther);
            bool isShouting = false;
            const bool hasIsShouting = a_actor.GetGraphVariableBool("IsShouting", isShouting);
            logs::debug("Voice sync ending lingering shout: actor={:08X} reason={} voiceState={} unk467={} "
                        "queuedGreetings={} soundFileDone={} casterSpell={:08X} casterState={} isShouting={}",
                a_remote.handle.native_handle(), a_reason, static_cast<std::uint32_t>(a_voiceState), a_high.unk467,
                a_high.queueofGreetings.size(),
                a_actor.GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kSoundFileDone),
                caster && caster->currentSpell ? caster->currentSpell->GetFormID() : 0,
                caster ? static_cast<std::uint32_t>(caster->state.get()) : 0,
                hasIsShouting ? (isShouting ? "true" : "false") : "missing");
            // As the voice update's own timeout: the caster finishes, then the voice resets.
            if (caster)
            {
                caster->FinishCast();
            }
            ResetVoice(a_actor);
            EndShout(a_remote, a_reason, a_voiceState);
        }
    }

    void ApplyShout(RemoteActor& a_remote, const ShoutRequest& a_request, bool a_mayStart, float a_deltaTime)
    {
        if (!GameThread::Require("ApplyShout"))
        {
            return;
        }
        auto& voice = a_remote.animation.voice;
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        voice.startCooldown = (std::max)(voice.startCooldown - deltaTime, 0.0F);
        if (!voice.held && (!a_request.shouting || a_request.shoutID == voice.shoutID))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }
        auto& actor = *actorPtr;

        auto* high = actor.GetHighProcess();
        const auto voiceState = high ? high->voiceState.get() : VoiceState::kNone;
        if (voice.held)
        {
            voice.heldElapsed += deltaTime;
            if (!high || voiceState == VoiceState::kNone)
            {
                EndShout(a_remote, "finished", voiceState);
                return;
            }
            if (voice.heldElapsed >= kShoutFallbackSeconds)
            {
                InterruptShout(actor, *high);
                EndShout(a_remote, "timed-out", voiceState);
                return;
            }
            const bool current = a_request.shouting && a_request.shoutID == voice.shoutID;
            // The release comes with its failure, before the actor's own fire.
            if (current && a_request.failed)
            {
                SetFireSuppressed(a_remote, true);
            }
            // From its release on, the shout plays out natively, until it outlasts the
            // source's or another follows it, which waits on the voice state.
            if (!IsCharging(voiceState))
            {
                voice.outlastedElapsed = current ? 0.0F : voice.outlastedElapsed + deltaTime;
                const bool superseded = a_request.shouting && !current;
                // A shout started while the graph still plays the last one gets no release
                // clip and never fires (observed in game); the engine starts none then.
                bool graphShouting = false;
                if ((voice.outlastedElapsed >= kOutlastSeconds || (superseded && voiceState != VoiceState::kPrecast))
                    && !(actor.GetGraphVariableBool("IsShouting", graphShouting) && graphShouting))
                {
                    EndLingeringShout(a_remote, actor, *high, voiceState, superseded ? "superseded" : "outlasted");
                }
                return;
            }
            if (!current)
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

        if (!a_request.shouting || !a_request.shout || a_request.shoutID == voice.shoutID || !a_mayStart
            || !SpellIsolation::IsolatesNativeCasts() || voice.startCooldown > 0.0F || !high
            || voiceState != VoiceState::kNone || !CanStart(a_remote, actor))
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
        voice.shoutID = a_request.shoutID;
        voice.shout = a_request.shout;
        voice.heldElapsed = 0.0F;
        voice.outlastedElapsed = 0.0F;
        logs::debug("Voice sync started shout: actor={:08X} shout={:08X} shoutID={} variation={} released={} "
                    "failed={}",
            a_remote.handle.native_handle(), a_request.shout->GetFormID(), a_request.shoutID, variation,
            a_request.released, a_request.failed);
        if (a_request.failed)
        {
            SetFireSuppressed(a_remote, true);
        }
    }

    void Release(RemoteActor& a_remote)
    {
        if (!GameThread::Require("ReleaseShout"))
        {
            return;
        }
        auto& voice = a_remote.animation.voice;
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
        if (!GameThread::Require("CastPower") || !SpellIsolation::IsolatesNativeCasts()
            || !GameplayPolicy::ShowsShoutEffects())
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
