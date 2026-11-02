#include "sync/skeleton/SkeletonSync.h"
#include "actor/ActorMovement.h"
#include "actor/ActorSkeleton.h"
#include "actor/RemoteActor.h"
#include "config/PluginConfig.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"
#include "sync/skeleton/PoseWindow.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace SkeletonSync
{
    namespace
    {
        constexpr float kBlendInSeconds = 0.2F;
        constexpr float kBlendOutSeconds = 0.2F;
        // Of squared distances: a shown peer keeps its slot until another is a fifth closer,
        // so peers at about the same distance do not trade it back and forth.
        constexpr float kSlotRankScale = 0.64F;

        struct Settings
        {
            bool share{ false };
            bool show{ false };
            // The nearest posed peers shown, 0 while showing is off; the server sends no
            // more peers' poses.
            std::uint32_t players{ 0 };
        };

        struct Candidate
        {
            RemoteState* state{ nullptr };
            float rank{ 0.0F };
        };

        struct SlotLog
        {
            std::uint32_t limit{ 0 };
            std::size_t candidates{ 0 };

            bool operator==(const SlotLog&) const = default;
        };

        std::optional<std::uint64_t> s_configRevision;
        Settings s_settings;
        // Filled by SyncSkeleton and emptied by AssignSlots in the same pass, while no peer
        // is erased.
        std::vector<Candidate> s_candidates;
        // Starts past a new peer's slotPass.
        std::uint64_t s_slotPass = 1;
        SlotLog s_slotLog;

        bool s_publishing = false;

        const Settings& GetSettings()
        {
            const auto revision = Config::GetRevision();
            if (s_configRevision != revision)
            {
                s_configRevision = revision;
                const auto& advanced = Config::Get()->advanced;
                const auto players = advanced.GetShownCustomAnimationPlayers();
                if (players != s_settings.players)
                {
                    Network::PublishPoseLimit(players);
                }
                s_settings = { .share = advanced.shareCustomAnimations,
                    .show = advanced.showCustomAnimations,
                    .players = players };
            }
            return s_settings;
        }

        // The played checkpoint's pose usually is the previous pass's next one.
        void Decode(const RemoteFrame& a_remote, RemoteState& a_state)
        {
            const auto played = a_remote.checkpoint.sequence;
            if (a_state.fromSequence != played)
            {
                if (a_state.toSequence == played)
                {
                    a_state.from = a_state.to;
                }
                else
                {
                    a_state.from = Network::DecodeSkeletonPose(a_remote.checkpoint.skeleton);
                }
                a_state.fromSequence = played;
            }
            const auto next = a_remote.next ? a_remote.next->sequence : 0;
            if (next != 0 && a_state.toSequence != next)
            {
                a_state.to = Network::DecodeSkeletonPose(a_remote.next->skeleton);
                a_state.toSequence = next;
            }
        }

        void Interpolate(const RemoteFrame& a_remote, RemoteState& a_state)
        {
            auto& pose = a_state.shown;
            pose = a_state.from;
            const float fraction = a_remote.pose.fraction;
            if (!a_remote.next || a_state.toSequence != a_remote.next->sequence || fraction <= 0.0F)
            {
                return;
            }
            const auto shared = a_state.from.present & a_state.to.present;
            for (std::size_t bone = 0; bone < Network::kSkeletonBoneCount; ++bone)
            {
                if ((shared >> bone) & 1)
                {
                    pose.rotations[bone]
                        = Network::NlerpRotation(a_state.from.rotations[bone], a_state.to.rotations[bone], fraction);
                }
            }
            if ((shared >> Network::kSkeletonRootBone) & 1)
            {
                for (std::size_t axis = 0; axis < pose.rootTranslation.size(); ++axis)
                {
                    pose.rootTranslation[axis] += (a_state.to.rootTranslation[axis] - pose.rootTranslation[axis]) * fraction;
                }
            }
        }
    }

    Network::EncodedSkeletonPose SampleLocalSkeleton(const LocalFrame& a_local)
    {
        PROFILE_FUNCTION();
        if (!GetSettings().share || !PoseWindow::IsOpen())
        {
            s_publishing = false;
            return {};
        }
        const auto rootSpace = a_local.player.IsInRagdollState() ? ActorSkeleton::RootSpace::kReference
                                                                 : ActorSkeleton::RootSpace::kParent;
        const auto pose = ActorSkeleton::ReadThirdPersonPose(a_local.player, rootSpace);
        if (!pose)
        {
            return {};
        }
        auto encoded = Network::EncodeSkeletonPose(*pose);
        if (!s_publishing)
        {
            s_publishing = true;
            logs::debug("Publishing local skeleton poses: bytes={} bones={}", encoded.size, std::popcount(pose->present));
        }
        return encoded;
    }

    void SyncSkeleton(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_owned,
        const RE::NiPoint3& a_viewer, float a_delta)
    {
        PROFILE_FUNCTION();
        const auto* holder = a_actor.animationGraphHolder;
        if (!a_owned || !holder)
        {
            ReleaseSkeleton(a_state, a_actor);
            return;
        }
        if (a_state.holder && a_state.holder != holder)
        {
            // A respawn: the registry cleared the previous actor's pose.
            a_state.holder = nullptr;
            a_state.weight = 0.0F;
            ActorMovement::SetPoseWeight(a_actor, 0.0F);
        }

        const auto& checkpoint = a_remote.checkpoint;
        const bool posed = GetSettings().show && !checkpoint.skeleton.empty()
            && checkpoint.shapeshift == Multiplayer::Shapeshift::kNone;
        const bool slot = a_state.slotPass == s_slotPass;
        if (posed)
        {
            const float distanceSquared = a_remote.pose.position.GetSquaredDistance(a_viewer);
            s_candidates.push_back({ .state = &a_state, .rank = slot ? distanceSquared * kSlotRankScale : distanceSquared });
        }

        const float delta = FrameTime::Clamp(a_delta);
        if (posed && slot)
        {
            Decode(a_remote, a_state);
            Interpolate(a_remote, a_state);
            if (!a_state.holder)
            {
                logs::debug("Remote skeleton pose blending in: player={} actor={:08X} bones={} sequence={}",
                    a_remote.playerID, a_actor.handle.native_handle(), std::popcount(a_state.shown.present),
                    static_cast<unsigned long long>(checkpoint.sequence));
            }
            a_state.weight = (std::min)(a_state.weight + delta / kBlendInSeconds, 1.0F);
        }
        else
        {
            if (!a_state.holder)
            {
                return;
            }
            a_state.weight -= delta / kBlendOutSeconds;
            if (a_state.weight <= 0.0F)
            {
                ReleaseSkeleton(a_state, a_actor);
                return;
            }
        }
        ActorSkeleton::SetPose(a_actor, a_state.shown, a_state.weight);
        ActorMovement::SetPoseWeight(a_actor, a_state.weight);
        a_state.holder = holder;
    }

    void ReleaseSkeleton(RemoteState& a_state, RemoteActor& a_actor)
    {
        if (!a_state.holder)
        {
            return;
        }
        if (a_state.holder == a_actor.animationGraphHolder)
        {
            ActorSkeleton::ClearPose(a_actor);
        }
        ActorMovement::SetPoseWeight(a_actor, 0.0F);
        logs::debug("Remote skeleton pose released to the graph: actor={:08X} weight={:.2f}",
            a_actor.handle.native_handle(), (std::max)(a_state.weight, 0.0F));
        a_state.holder = nullptr;
        a_state.weight = 0.0F;
    }

    void AssignSlots()
    {
        PROFILE_FUNCTION();
        const auto& settings = GetSettings();
        const auto limit = settings.players;
        const auto shown = (std::min)(static_cast<std::size_t>(limit), s_candidates.size());
        if (shown < s_candidates.size())
        {
            std::nth_element(s_candidates.begin(), s_candidates.begin() + static_cast<std::ptrdiff_t>(shown),
                s_candidates.end(),
                [](const Candidate& a_left, const Candidate& a_right) { return a_left.rank < a_right.rank; });
        }
        ++s_slotPass;
        for (std::size_t index = 0; index < shown; ++index)
        {
            s_candidates[index].state->slotPass = s_slotPass;
        }
        const SlotLog log{ .limit = limit, .candidates = s_candidates.size() };
        if (log != s_slotLog)
        {
            s_slotLog = log;
            logs::debug("Remote skeleton pose slots: limit={} posedPeers={} shown={}", log.limit, log.candidates, shown);
        }
        s_candidates.clear();
    }
}
