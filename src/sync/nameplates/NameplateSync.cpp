#include "sync/nameplates/NameplateSync.h"
#include "config/PluginConfig.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"
#include "sync/PeerState.h"
#include "sync/PlayerLabel.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace NameplateSync
{
    namespace
    {
        constexpr float kSightCheckSeconds = 0.2F;
        constexpr float kOffscreenCheckSeconds = 0.05F;
        // A cost cap per main-loop pass, not a duration.
        constexpr std::uint32_t kMaximumSightChecksPerPass = 8;
        constexpr float kFadeSeconds = 0.2F;

        std::vector<Plate> s_plates;
        bool s_enabled = false;

        RE::NiAVObject* FindHead(const RE::Actor& a_actor, RE::NiAVObject& a_root)
        {
            const auto* race = a_actor.GetRace();
            const auto* bodyParts = race ? race->bodyPartData : nullptr;
            const auto* head = bodyParts ? bodyParts->parts[RE::BGSBodyPartDefs::LIMB_ENUM::kHead] : nullptr;
            if (!head || head->nodeName.empty())
            {
                return nullptr;
            }
            return a_root.GetObjectByName(head->nodeName);
        }

        void Hide(RemoteState& a_state)
        {
            a_state.inSight = false;
            a_state.sightCheckRemaining = 0.0F;
            a_state.alpha = 0.0F;
        }

        bool TrackHead(RemoteState& a_state, Multiplayer::PeerSlot a_playerID, RE::Actor& a_actor)
        {
            auto* root = a_actor.Get3D(false);
            if (!root)
            {
                a_state.root.reset();
                a_state.head.reset();
                return false;
            }
            if (root != a_state.root.get())
            {
                a_state.root.reset(root);
                a_state.head.reset(FindHead(a_actor, *root));
                if (!a_state.head)
                {
                    logs::debug("Nameplate has no head node to follow: player={} actor={:08X}", a_playerID,
                        a_actor.GetFormID());
                }
            }
            return a_state.head != nullptr;
        }

        void RefreshLabel(RemoteState& a_state, Multiplayer::PeerSlot a_playerID,
            const AppearanceSync::RemoteState& a_appearance, const LabelRevision& a_revision)
        {
            if (a_state.labelRevision == a_revision)
            {
                return;
            }
            a_state.labelRevision = a_revision;
            const auto player = ChatRoster::Find(a_playerID);
            std::string_view name;
            if (player)
            {
                name = player->name;
            }
            else if (a_appearance.latest)
            {
                name = a_appearance.latest->name;
            }
            a_state.label = PlayerLabel::Format(name);
            a_state.isFriend = player && PlayerFilters::IsFriend(player->session);
        }

        void ObserveSight(RemoteState& a_state, RE::PlayerCharacter& a_player, RE::Actor& a_actor,
            std::uint32_t& a_raycasts)
        {
            if (a_state.sightCheckRemaining > 0.0F || a_raycasts >= kMaximumSightChecksPerPass)
            {
                return;
            }
            // The player's check tests the camera frustum and casts a ray only for an actor inside it.
            bool inFrustum = false;
            const bool inSight = a_player.HasLineOfSight(&a_actor, inFrustum);
            if (!inFrustum)
            {
                // Off screen the plate is not drawn, and it keeps its last state for when the actor is back in view.
                a_state.sightCheckRemaining = kOffscreenCheckSeconds;
                return;
            }
            ++a_raycasts;
            a_state.inSight = inSight;
            a_state.sightCheckRemaining = kSightCheckSeconds;
        }
    }

    void Clear()
    {
        s_plates.clear();
    }

    void Update(std::unordered_map<Multiplayer::PeerSlot, PeerState>& a_peers, RE::PlayerCharacter& a_player,
        float a_delta)
    {
        PROFILE_FUNCTION();
        s_plates.clear();
        if (!GameThread::Require("NameplateSync::Update"))
        {
            return;
        }
        const auto config = Config::Get();
        const auto& ui = config->ui;
        const bool enabled = ui.nameplates && ui.nameplateDistance > 0;
        if (enabled != s_enabled)
        {
            s_enabled = enabled;
            for (auto& [playerID, peer] : a_peers)
            {
                peer.nameplate = {};
            }
        }
        if (!enabled)
        {
            return;
        }

        const auto delta = FrameTime::Clamp(a_delta);
        const auto range = static_cast<float>(ui.nameplateDistance);
        const auto origin = a_player.GetPosition();
        const LabelRevision revision{ .roster = ChatRoster::GetRevision(), .filters = PlayerFilters::GetRevision() };
        std::uint32_t raycasts = 0;
        for (auto& [playerID, peer] : a_peers)
        {
            auto& state = peer.nameplate;
            const auto& remote = peer.lifecycle.actor;
            const auto actor = remote && remote->ready ? remote->handle.get() : nullptr;
            if (!actor)
            {
                state.root.reset();
                state.head.reset();
                Hide(state);
                continue;
            }
            if (!TrackHead(state, playerID, *actor))
            {
                Hide(state);
                continue;
            }

            const auto distance = origin.GetDistance(actor->GetPosition());
            state.sightCheckRemaining -= delta;
            if (distance < range)
            {
                ObserveSight(state, a_player, *actor, raycasts);
            }
            else
            {
                // Coming back into range waits for a fresh check rather than showing a stale one.
                state.inSight = false;
                state.sightCheckRemaining = 0.0F;
            }

            const auto step = delta / kFadeSeconds;
            state.alpha = state.inSight ? (std::min)(state.alpha + step, 1.0F) : (std::max)(state.alpha - step, 0.0F);
            if (state.alpha <= 0.0F)
            {
                continue;
            }

            RefreshLabel(state, playerID, peer.appearance, revision);
            s_plates.push_back(Plate{ .head = state.head.get(),
                .label = state.label,
                .isFriend = state.isFriend,
                .alpha = state.alpha,
                .distanceFraction = distance / range });
        }
        std::ranges::sort(s_plates, std::greater{}, &Plate::distanceFraction);
        PROFILE_PLOT("Nameplates", s_plates.size());
    }

    std::span<const Plate> GetPlates()
    {
        if (!GameThread::IsAuthoritative())
        {
            return {};
        }
        return s_plates;
    }
}
