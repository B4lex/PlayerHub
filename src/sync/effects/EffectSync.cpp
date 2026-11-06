#include "sync/effects/EffectSync.h"
#include "actor/EffectPresentation.h"
#include "actor/GameplayPolicy.h"
#include "actor/RemoteActor.h"
#include "actor/SelfEffects.h"
#include "forms/FormLookup.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace EffectSync
{
    namespace
    {
        constexpr float kCapturePollIntervalSeconds = 0.1F;
        constexpr float kCheckIntervalSeconds = 1.0F;
        constexpr float kMaximumHoldSeconds = 1.0F;
        constexpr std::size_t kMaximumHeld = 8;
        constexpr std::uint32_t kMaximumReapplies = 3;

        std::string FormatEffects(const Multiplayer::PlayerEffects& a_effects)
        {
            std::string text;
            for (const auto& entry : a_effects.effects)
            {
                std::format_to(std::back_inserter(text), "{}{}|{:06X}:{:.1f}", text.empty() ? "" : ",",
                    entry.effect.plugin, entry.effect.localFormID, entry.magnitude);
            }
            return text.empty() ? "<none>" : text;
        }

        // At most one entry per effect, ordered by reference; a stronger copy wins.
        std::vector<Multiplayer::ActiveEffect> CaptureLocalEffects(RE::PlayerCharacter& a_player, std::uint32_t& a_skipped)
        {
            std::vector<Multiplayer::ActiveEffect> captured;
            auto* magicTarget = a_player.AsMagicTarget();
            auto* activeEffects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
            if (!activeEffects)
            {
                return captured;
            }
            for (const auto* activeEffect : *activeEffects)
            {
                if (!activeEffect
                    || activeEffect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
                {
                    continue;
                }
                const auto* effect = activeEffect->GetBaseObject();
                if (!effect || !SelfEffects::IsPresented(*effect))
                {
                    continue;
                }
                auto reference = FormLookup::MakeFormReference(effect);
                if (!reference)
                {
                    ++a_skipped;
                    continue;
                }
                const float magnitude = std::isfinite(activeEffect->magnitude)
                    ? std::clamp(activeEffect->magnitude, -Multiplayer::PlayerEffects::kMaximumMagnitude,
                          Multiplayer::PlayerEffects::kMaximumMagnitude)
                    : 0.0F;
                auto& entry = captured.emplace_back();
                entry.effect = std::move(*reference);
                entry.magnitude = magnitude;
            }
            std::ranges::sort(captured, [](const auto& a_left, const auto& a_right) {
                return a_left.effect != a_right.effect ? a_left.effect < a_right.effect
                                                       : a_left.magnitude > a_right.magnitude;
            });
            const auto duplicates = std::ranges::unique(captured, {}, &Multiplayer::ActiveEffect::effect);
            captured.erase(duplicates.begin(), duplicates.end());
            if (captured.size() > Multiplayer::PlayerEffects::kMaximumEffects)
            {
                a_skipped += static_cast<std::uint32_t>(captured.size() - Multiplayer::PlayerEffects::kMaximumEffects);
                captured.resize(Multiplayer::PlayerEffects::kMaximumEffects);
            }
            return captured;
        }

        void Resolve(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, bool a_showing)
        {
            const auto revision = a_remote.latest ? a_remote.latest->revision : 0;
            if (a_remote.resolved && a_remote.resolvedRevision == revision && a_remote.resolvedShowing == a_showing)
            {
                return;
            }
            a_remote.resolved = true;
            a_remote.resolvedRevision = revision;
            a_remote.resolvedShowing = a_showing;
            a_remote.desired.clear();
            a_remote.dirty = true;
            if (!a_remote.latest || !a_showing)
            {
                return;
            }
            for (const auto& entry : a_remote.latest->effects)
            {
                auto* effect = FormLookup::ResolveForm<RE::EffectSetting>(entry.effect);
                if (!effect || !SelfEffects::IsPresented(*effect))
                {
                    logs::debug("Effect sync skipped remote effect: player={} revision={} effect={}|{:06X} reason={}",
                        a_playerID, static_cast<unsigned long long>(revision), entry.effect.plugin,
                        entry.effect.localFormID, effect ? "not-presented" : "missing");
                    continue;
                }
                a_remote.desired.push_back({ .effect = effect, .magnitude = entry.magnitude });
            }
        }

        const ResolvedEffect* Find(const std::vector<ResolvedEffect>& a_effects, const RE::EffectSetting* a_effect)
        {
            const auto found = std::ranges::find(a_effects, a_effect, &ResolvedEffect::effect);
            return found != a_effects.end() ? std::addressof(*found) : nullptr;
        }

        void Reconcile(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RE::Actor& a_actor, bool a_verify)
        {
            for (auto it = a_remote.applied.begin(); it != a_remote.applied.end();)
            {
                const auto* wanted = Find(a_remote.desired, it->effect);
                if (wanted && wanted->magnitude == it->magnitude)
                {
                    ++it;
                    continue;
                }
                EffectPresentation::Remove(a_actor, *it->effect);
                logs::debug("Effect sync removed remote effect: player={} actor={:08X} effect={:08X}", a_playerID,
                    a_actor.GetFormID(), it->effect->GetFormID());
                it = a_remote.applied.erase(it);
            }

            for (const auto& wanted : a_remote.desired)
            {
                const auto applied = std::ranges::find(a_remote.applied, wanted.effect, &ResolvedEffect::effect);
                if (applied != a_remote.applied.end())
                {
                    if (!a_verify || applied->reapplies >= kMaximumReapplies
                        || EffectPresentation::IsApplied(a_actor, *applied->effect))
                    {
                        continue;
                    }
                    // The engine ended it while the source kept it, as when the actor's
                    // own replayed action breaks invisibility.
                    ++applied->reapplies;
                    const bool reapplied = EffectPresentation::Apply(a_actor, *applied->effect, applied->magnitude);
                    logs::debug("Effect sync reapplied remote effect: player={} actor={:08X} effect={:08X} attempt={}/{} "
                                "applied={}",
                        a_playerID, a_actor.GetFormID(), applied->effect->GetFormID(), applied->reapplies,
                        kMaximumReapplies, reapplied);
                    continue;
                }
                const bool added = EffectPresentation::Apply(a_actor, *wanted.effect, wanted.magnitude);
                logs::debug("Effect sync applied remote effect: player={} actor={:08X} effect={:08X} archetype={} "
                            "magnitude={:.1f} applied={}",
                    a_playerID, a_actor.GetFormID(), wanted.effect->GetFormID(),
                    static_cast<std::int32_t>(wanted.effect->GetArchetype()), wanted.magnitude, added);
                // A failed application spends a reapplication, so the budget bounds it too.
                a_remote.applied.push_back(
                    { .effect = wanted.effect, .magnitude = wanted.magnitude, .reapplies = added ? 0U : 1U });
            }
        }
    }

    void SyncLocalEffects(const LocalFrame& a_local, float a_delta)
    {
        PROFILE_FUNCTION();
        static float pollElapsed = kCapturePollIntervalSeconds;
        static std::optional<Multiplayer::PlayerEffects> published;
        static std::uint64_t revision = 0;
        static std::uint64_t publishedConnectionGeneration = 0;

        pollElapsed += FrameTime::Clamp(a_delta);
        if (!Network::IsConnected())
        {
            return;
        }
        if (const auto generation = Network::GetConnectionGeneration(); generation != publishedConnectionGeneration)
        {
            publishedConnectionGeneration = generation;
            if (published)
            {
                Network::PublishLocalEffects(*published);
            }
        }
        if (pollElapsed < kCapturePollIntervalSeconds)
        {
            return;
        }
        pollElapsed = 0.0F;

        std::uint32_t skipped = 0;
        auto captured = CaptureLocalEffects(a_local.player, skipped);
        if (published ? published->effects == captured : captured.empty())
        {
            return;
        }
        if (revision >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            logs::critical("Effect sync cannot advance revision beyond the protocol limit");
            return;
        }
        Multiplayer::PlayerEffects pending{ .revision = ++revision, .effects = std::move(captured) };
        if (!Multiplayer::IsValidEffects(pending))
        {
            logs::warn("Effect sync skipped invalid local effects: revision={}", static_cast<unsigned long long>(revision));
            return;
        }
        logs::debug("Effect sync publishing local effects: revision={} effects={} skipped={}",
            static_cast<unsigned long long>(pending.revision), FormatEffects(pending), skipped);
        Network::PublishLocalEffects(pending);
        published = std::move(pending);
    }

    void AcceptRemoteEffects(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerEffects&& a_effects)
    {
        const auto newest = !a_remote.held.empty() ? a_remote.held.back().effects.revision
            : a_remote.latest                     ? a_remote.latest->revision
                                                  : 0;
        if (a_effects.revision <= newest)
        {
            return;
        }
        logs::debug("Effect sync accepted remote effects: player={} revision={} effects={}", a_playerID,
            static_cast<unsigned long long>(a_effects.revision), FormatEffects(a_effects));
        // The first effects of a peer are in force at once, as its actor appears
        // without a playout delay.
        if (!a_remote.latest)
        {
            a_remote.latest = std::move(a_effects);
            return;
        }
        if (a_remote.held.size() >= kMaximumHeld)
        {
            a_remote.latest = std::move(a_remote.held.front().effects);
            a_remote.held.erase(a_remote.held.begin());
        }
        a_remote.held.push_back({ .effects = std::move(a_effects) });
    }

    void AgeRemoteEffects(RemoteState& a_remote, double a_holdSeconds, float a_delta)
    {
        if (a_remote.held.empty())
        {
            return;
        }
        // The source captures an effect after its cast fires, and the peer plays
        // that cast its playout delay after the checkpoint arrives.
        const float hold = std::clamp(static_cast<float>(a_holdSeconds), 0.0F, kMaximumHoldSeconds);
        const float delta = FrameTime::Clamp(a_delta);
        for (auto& entry : a_remote.held)
        {
            entry.elapsed += delta;
        }
        std::size_t due = 0;
        while (due < a_remote.held.size() && a_remote.held[due].elapsed >= hold)
        {
            ++due;
        }
        if (due == 0)
        {
            return;
        }
        a_remote.latest = std::move(a_remote.held[due - 1].effects);
        a_remote.held.erase(a_remote.held.begin(), a_remote.held.begin() + static_cast<std::ptrdiff_t>(due));
    }

    void OnActorSpawned(RemoteState& a_remote)
    {
        a_remote.applied.clear();
        a_remote.dirty = true;
        a_remote.checkElapsed = 0.0F;
    }

    void SyncEffects(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RemoteActor& a_remoteActor, float a_delta)
    {
        Resolve(a_remote, a_playerID, GameplayPolicy::ShowsSpellEffects());
        a_remote.checkElapsed += FrameTime::Clamp(a_delta);
        const bool verify = a_remote.checkElapsed >= kCheckIntervalSeconds;
        if (!a_remote.dirty && !verify)
        {
            return;
        }
        auto actorPtr = a_remoteActor.handle.get();
        auto* actor = actorPtr.get();
        // Shaders and art attach to loaded 3D.
        if (!actor || !a_remoteActor.ready || !actor->Is3DLoaded())
        {
            return;
        }
        PROFILE_FUNCTION();
        Reconcile(a_remote, a_playerID, *actor, verify);
        a_remote.dirty = false;
        if (verify)
        {
            a_remote.checkElapsed = 0.0F;
        }
    }
}
