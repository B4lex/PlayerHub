#include "actor/ActorSkeleton.h"
#include "actor/ActorConstants.h"
#include "actor/BehaviorGraphLayout.h"
#include "actor/RemoteActor.h"
#include "runtime/CodeDetour.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

namespace ActorSkeleton
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using CodeDetour::kCallDetourTrampolineSize;
        using CodeDetour::WriteCallDetour;
        using Network::kSkeletonBoneCount;

        static_assert(2 * kCallDetourTrampolineSize == kTrampolineSize);

        // BShkbAnimationGraph's per-graph step (ID 63589, SE 62644) that copies the
        // generated pose into boneNodes' local transforms (ID 63856, SE 62933). The
        // graph manager's update (ID 63358, SE 62416) calls it for every graph while
        // holding the manager's updateLock, and so does ID 63359 (SE 62417). Skyrim
        // derives world transforms from the locals later in the frame.
        struct PoseCallSite
        {
            REL::ID function;
            std::ptrdiff_t offset;
        };

        using BoneIndices = std::array<std::int16_t, kSkeletonBoneCount>;

        struct Entry
        {
            Network::SkeletonPose pose;
            float weight{ 1.0F };
            // Bone indices into the graph that was last mapped; skeleton and bone count
            // tell a new graph at a freed graph's address apart.
            const RE::BShkbAnimationGraph* graph{ nullptr };
            const RE::hkaSkeleton* skeleton{ nullptr };
            std::uint32_t boneCount{ 0 };
            BoneIndices bones{};
        };

        std::shared_mutex s_mutex;
        // Keyed like ActorRegistry; the pointer is only compared.
        std::unordered_map<const RE::IAnimationGraphManagerHolder*, Entry> s_entries;
        // Lets the hook, which runs for every actor's graph, skip the lock while no pose is set.
        std::atomic<std::size_t> s_entryCount{ 0 };
        std::atomic<std::uint32_t> s_mappingCount{ 0 };

        const RE::hkaSkeleton* GetAnimationSkeleton(const RE::BShkbAnimationGraph& a_graph)
        {
            const auto* setup = a_graph.characterInstance.setup.get();
            return setup ? setup->animationSkeleton.get() : nullptr;
        }

        // Matches the vanilla bone names against the graph's Havok skeleton, whose
        // indices are boneNodes' indices.
        std::uint32_t MapBones(const RE::BShkbAnimationGraph& a_graph, BoneIndices& a_bones)
        {
            a_bones.fill(-1);
            const auto* skeleton = GetAnimationSkeleton(a_graph);
            if (!skeleton)
            {
                return 0;
            }
            const auto count = std::min<std::size_t>(
                { static_cast<std::size_t>(skeleton->bones.size()), a_graph.boneNodes.size(), 0x7FFF });
            std::uint32_t mapped = 0;
            for (std::size_t index = 0; index < count; ++index)
            {
                const std::string_view name = BehaviorGraphLayout::GetName(skeleton->bones[static_cast<std::int32_t>(index)].name);
                const auto bone = std::ranges::find(Network::kSkeletonBoneNames, name);
                if (bone != Network::kSkeletonBoneNames.end())
                {
                    auto& slot = a_bones[static_cast<std::size_t>(bone - Network::kSkeletonBoneNames.begin())];
                    if (slot < 0)
                    {
                        slot = static_cast<std::int16_t>(index);
                        ++mapped;
                    }
                }
            }
            return mapped;
        }

        // A nonnegative index at +0x08 of a boneNodes entry selects a 0x80-byte slot
        // in the array at node + 0x130: the object at the slot's +0x70 when set, or
        // the slot itself, holds the local transform (ID 63856, SE 62933).
        RE::NiTransform* GetBoneLocal(const RE::BShkbAnimationGraph::BoneNodeEntry& a_entry)
        {
            constexpr std::size_t kSlotArrayOffset = 0x130;
            constexpr std::size_t kSlotSize = 0x80;
            constexpr std::size_t kSlotObjectOffset = 0x70;
            auto* node = a_entry.node;
            if (!node)
            {
                return nullptr;
            }
            const auto index = static_cast<std::int32_t>(a_entry.unk08);
            if (index < 0)
            {
                return &node->local;
            }
            auto* slots = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(node) + kSlotArrayOffset);
            if (!slots)
            {
                return nullptr;
            }
            auto* slot = slots + static_cast<std::size_t>(index) * kSlotSize;
            auto* object = *reinterpret_cast<RE::NiAVObject**>(slot + kSlotObjectOffset);
            return object ? &object->local : reinterpret_cast<RE::NiTransform*>(slot);
        }

        void WriteRotation(RE::NiMatrix3& a_matrix, const Network::SkeletonRotation& a_rotation)
        {
            const float x2 = a_rotation.x + a_rotation.x;
            const float y2 = a_rotation.y + a_rotation.y;
            const float z2 = a_rotation.z + a_rotation.z;
            const float xx = a_rotation.x * x2;
            const float yy = a_rotation.y * y2;
            const float zz = a_rotation.z * z2;
            const float xy = a_rotation.x * y2;
            const float xz = a_rotation.x * z2;
            const float yz = a_rotation.y * z2;
            const float wx = a_rotation.w * x2;
            const float wy = a_rotation.w * y2;
            const float wz = a_rotation.w * z2;
            a_matrix.entry[0][0] = 1.0F - (yy + zz);
            a_matrix.entry[0][1] = xy - wz;
            a_matrix.entry[0][2] = xz + wy;
            a_matrix.entry[1][0] = xy + wz;
            a_matrix.entry[1][1] = 1.0F - (xx + zz);
            a_matrix.entry[1][2] = yz - wx;
            a_matrix.entry[2][0] = xz - wy;
            a_matrix.entry[2][1] = yz + wx;
            a_matrix.entry[2][2] = 1.0F - (xx + yy);
        }

        Network::SkeletonRotation ReadRotation(const RE::NiMatrix3& a_matrix)
        {
            const auto& m = a_matrix.entry;
            const float trace = m[0][0] + m[1][1] + m[2][2];
            Network::SkeletonRotation result;
            if (trace > 0.0F)
            {
                const float s = std::sqrt(trace + 1.0F) * 2.0F;
                result = { (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25F * s };
            }
            else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
            {
                const float s = std::sqrt(1.0F + m[0][0] - m[1][1] - m[2][2]) * 2.0F;
                result = { 0.25F * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s };
            }
            else if (m[1][1] > m[2][2])
            {
                const float s = std::sqrt(1.0F + m[1][1] - m[0][0] - m[2][2]) * 2.0F;
                result = { (m[0][1] + m[1][0]) / s, 0.25F * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s };
            }
            else
            {
                const float s = std::sqrt(1.0F + m[2][2] - m[0][0] - m[1][1]) * 2.0F;
                result = { (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25F * s, (m[1][0] - m[0][1]) / s };
            }
            return result;
        }

        // The locals hold the graph's pose for this update, which a partial weight blends from.
        void WritePose(RE::BShkbAnimationGraph& a_graph, const Entry& a_entry)
        {
            const bool blend = a_entry.weight < 1.0F;
            for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
            {
                const auto index = a_entry.bones[bone];
                if (index < 0 || ((a_entry.pose.present >> bone) & 1) == 0
                    || static_cast<std::uint32_t>(index) >= a_graph.boneNodes.size())
                {
                    continue;
                }
                auto* local = GetBoneLocal(a_graph.boneNodes[static_cast<std::uint32_t>(index)]);
                if (!local)
                {
                    continue;
                }
                const auto& rotation = a_entry.pose.rotations[bone];
                WriteRotation(local->rotate,
                    blend ? Network::NlerpRotation(ReadRotation(local->rotate), rotation, a_entry.weight) : rotation);
                if (bone == Network::kSkeletonRootBone)
                {
                    const RE::NiPoint3 translation{ a_entry.pose.rootTranslation[0], a_entry.pose.rootTranslation[1],
                        a_entry.pose.rootTranslation[2] };
                    local->translate = blend ? local->translate + (translation - local->translate) * a_entry.weight
                                             : translation;
                }
            }
        }

        bool IsMapped(const Entry& a_entry, const RE::BShkbAnimationGraph& a_graph)
        {
            return a_entry.graph == &a_graph && a_entry.skeleton == GetAnimationSkeleton(a_graph)
                && a_entry.boneCount == a_graph.boneNodes.size();
        }

        void ApplyNetworkPose(RE::BShkbAnimationGraph& a_graph)
        {
            if (s_entryCount.load(std::memory_order_relaxed) == 0 || !a_graph.holder)
            {
                return;
            }
            const auto* holder = static_cast<const RE::IAnimationGraphManagerHolder*>(a_graph.holder);
            {
                std::shared_lock lock{ s_mutex };
                const auto entry = s_entries.find(holder);
                if (entry == s_entries.end())
                {
                    return;
                }
                if (IsMapped(entry->second, a_graph))
                {
                    WritePose(a_graph, entry->second);
                    return;
                }
            }

            BoneIndices bones;
            const auto mapped = MapBones(a_graph, bones);
            std::unique_lock lock{ s_mutex };
            const auto entry = s_entries.find(holder);
            if (entry == s_entries.end())
            {
                return;
            }
            entry->second.graph = &a_graph;
            entry->second.skeleton = GetAnimationSkeleton(a_graph);
            entry->second.boneCount = a_graph.boneNodes.size();
            entry->second.bones = bones;
            const auto count = s_mappingCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ShouldLogOccurrence(count))
            {
                logs::debug("Remote skeleton pose mapped a graph: actor={:08X} graphBones={} mapped={}/{} count={}",
                    a_graph.holder->GetFormID(), a_graph.boneNodes.size(), mapped, kSkeletonBoneCount, count);
            }
            WritePose(a_graph, entry->second);
        }

        template <int Site>
        struct PoseCopyHook
        {
            static void Thunk(RE::BShkbAnimationGraph* a_graph, const std::uint8_t* a_flag)
            {
                _original(a_graph, a_flag);
                if (a_graph)
                {
                    ApplyNetworkPose(*a_graph);
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The local player's third-person graph and its mapping, read on the main-loop thread.
        struct LocalMapping
        {
            const RE::BShkbAnimationGraph* graph{ nullptr };
            const RE::hkaSkeleton* skeleton{ nullptr };
            std::uint32_t boneCount{ 0 };
            BoneIndices bones{};
            std::uint32_t mapped{ 0 };
        };

        LocalMapping s_localMapping;
        std::uint32_t s_localMissingGraphCount = 0;
    }

    void InstallHooks()
    {
        PoseCopyHook<0>::_original = WriteCallDetour(REL::ID(RELOCATION_ID(62416, 63358)), REL::Relocate(0x267, 0x319),
            reinterpret_cast<std::uintptr_t>(&PoseCopyHook<0>::Thunk), "skeleton pose copy");
        PoseCopyHook<1>::_original = WriteCallDetour(REL::ID(RELOCATION_ID(62417, 63359)), REL::Relocate(0x66, 0x77),
            reinterpret_cast<std::uintptr_t>(&PoseCopyHook<1>::Thunk), "deferred skeleton pose copy");
        logs::debug("Remote skeleton pose hooks: update={} deferred={}", PoseCopyHook<0>::_original.address() != 0,
            PoseCopyHook<1>::_original.address() != 0);
    }

    std::optional<Network::SkeletonPose> ReadThirdPersonPose(RE::Actor& a_actor)
    {
        PROFILE_FUNCTION();
        const auto* root = a_actor.Get3D(false);
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!root || !a_actor.GetAnimationGraphManager(manager) || !manager)
        {
            return std::nullopt;
        }
        RE::BSSpinLockGuard lock{ manager->GetRuntimeData().updateLock };
        const RE::BShkbAnimationGraph* graph = nullptr;
        for (const auto& candidate : manager->graphs)
        {
            if (candidate && candidate->rootNode == root)
            {
                graph = candidate.get();
                break;
            }
        }
        if (!graph)
        {
            if (ShouldLogOccurrence(++s_localMissingGraphCount))
            {
                logs::debug("Local skeleton pose found no third-person graph: graphs={} count={}",
                    manager->graphs.size(), s_localMissingGraphCount);
            }
            return std::nullopt;
        }

        auto& mapping = s_localMapping;
        if (mapping.graph != graph || mapping.skeleton != GetAnimationSkeleton(*graph)
            || mapping.boneCount != graph->boneNodes.size())
        {
            mapping.graph = graph;
            mapping.skeleton = GetAnimationSkeleton(*graph);
            mapping.boneCount = graph->boneNodes.size();
            mapping.mapped = MapBones(*graph, mapping.bones);
            logs::debug("Local skeleton pose mapped the third-person graph: graphBones={} mapped={}/{}",
                graph->boneNodes.size(), mapping.mapped, kSkeletonBoneCount);
        }
        if (mapping.mapped == 0)
        {
            return std::nullopt;
        }

        Network::SkeletonPose pose;
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            const auto index = mapping.bones[bone];
            if (index < 0)
            {
                continue;
            }
            const auto* local = GetBoneLocal(graph->boneNodes[static_cast<std::uint32_t>(index)]);
            if (!local)
            {
                continue;
            }
            pose.rotations[bone] = ReadRotation(local->rotate);
            pose.present |= std::uint64_t{ 1 } << bone;
            if (bone == Network::kSkeletonRootBone)
            {
                pose.rootTranslation = { local->translate.x, local->translate.y, local->translate.z };
            }
        }
        return pose;
    }

    void SetPose(const RemoteActor& a_remote, const Network::SkeletonPose& a_pose, float a_weight)
    {
        if (!GameThread::Require("ActorSkeleton::SetPose") || !a_remote.animationGraphHolder)
        {
            return;
        }
        std::unique_lock lock{ s_mutex };
        const auto [entry, inserted] = s_entries.try_emplace(a_remote.animationGraphHolder);
        entry->second.pose = a_pose;
        entry->second.weight = std::clamp(a_weight, 0.0F, 1.0F);
        if (inserted)
        {
            s_entryCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void ClearPose(const RemoteActor& a_remote)
    {
        if (s_entryCount.load(std::memory_order_relaxed) == 0 || !a_remote.animationGraphHolder)
        {
            return;
        }
        std::unique_lock lock{ s_mutex };
        if (s_entries.erase(a_remote.animationGraphHolder) != 0)
        {
            s_entryCount.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}
