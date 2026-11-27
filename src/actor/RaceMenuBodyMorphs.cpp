#include "actor/RaceMenuBodyMorphs.h"
#include "actor/SKEEInterfaces.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RaceMenuBodyMorphs
{
    namespace
    {
        using Multiplayer::AppearanceBodyMorph;
        using Multiplayer::PlayerAppearance;
        using SKEEInterfaces::IBodyMorphInterface;

        constexpr const char* kMorphKey = "PlayerHub";
        // OBody NG assigns its preset from a Character::Load3D hook, which may apply
        // it later than the load itself, so a rebuilt actor is checked once more.
        constexpr float kForeignCheckSeconds = 2.0F;
        constexpr std::size_t kMaximumReportedNames = 32;

        struct Entry
        {
            std::vector<AppearanceBodyMorph> morphs;
            // Holds the 3D the morphs were last written for, so a rebuilt one is told
            // apart even at the same address.
            RE::NiPointer<RE::NiAVObject> root;
            float checkRemaining{ 0.0F };
            bool foreignReported{ false };
        };

        std::unordered_map<std::uint32_t, Entry> s_entries;

        class MorphSums final : public IBodyMorphInterface::MorphValueVisitor
        {
        public:
            void Visit(RE::TESObjectREFR*, const char* a_name, const char*, float a_value) override
            {
                if (a_name && std::isfinite(a_value))
                {
                    sums[a_name] += a_value;
                }
            }

            // std::string orders by unsigned byte value, as the wire requires.
            std::map<std::string, double, std::less<>> sums;
        };

        class ForeignKeys final : public IBodyMorphInterface::MorphValueVisitor
        {
        public:
            void Visit(RE::TESObjectREFR*, const char*, const char* a_key, float) override
            {
                const std::string_view key{ a_key ? a_key : "" };
                if (key != kMorphKey && std::ranges::find(keys, key) == keys.end())
                {
                    keys.emplace_back(key);
                }
            }

            std::vector<std::string> keys;
        };

        void ReportSkippedName(std::string_view a_name, std::string_view a_reason, double a_sum)
        {
            static std::vector<std::string> reported;
            if (reported.size() >= kMaximumReportedNames || std::ranges::find(reported, a_name) != reported.end())
            {
                return;
            }
            reported.emplace_back(a_name);
            logs::warn("Body morph capture skipped a morph peers would reject: name=\"{}\" reason={} value={}", a_name,
                a_reason, a_sum);
        }

        std::string JoinKeys(const std::vector<std::string>& a_keys)
        {
            std::string result;
            for (const auto& key : a_keys)
            {
                result += result.empty() ? "" : ",";
                result += key;
            }
            return result;
        }

        // ClearMorphs drops every key of the reference, so morphs other mods wrote go
        // with the previous ones.
        void Replace(IBodyMorphInterface& a_interface, RE::Actor& a_actor, Entry& a_entry, std::string_view a_reason)
        {
            PROFILE_FUNCTION();
            auto* reference = std::addressof(a_actor);
            const bool hadMorphs = a_interface.HasMorphs(reference);
            if (!hadMorphs && a_entry.morphs.empty())
            {
                return;
            }

            ForeignKeys foreign;
            if (hadMorphs)
            {
                a_interface.VisitMorphValues(reference, foreign);
                a_interface.ClearMorphs(reference);
            }
            for (const auto& morph : a_entry.morphs)
            {
                a_interface.SetMorph(reference, morph.name.c_str(), kMorphKey,
                    static_cast<float>(morph.value) / AppearanceBodyMorph::kScale);
            }
            a_interface.ApplyBodyMorphs(reference, true);

            if (!foreign.keys.empty() && !a_entry.foreignReported)
            {
                a_entry.foreignReported = true;
                logs::debug("Body morphs from other mods found on a remote actor: actor={:08X} keys={}",
                    a_actor.GetHandle().native_handle(), JoinKeys(foreign.keys));
            }
            logs::debug("Applied body morphs: actor={:08X} reason={} applied={} clearedForeign={}",
                a_actor.GetHandle().native_handle(), a_reason, a_entry.morphs.size(), !foreign.keys.empty());
        }

        bool HasForeignMorphs(IBodyMorphInterface& a_interface, RE::Actor& a_actor)
        {
            auto* reference = std::addressof(a_actor);
            if (!a_interface.HasMorphs(reference))
            {
                return false;
            }
            ForeignKeys foreign;
            a_interface.VisitMorphValues(reference, foreign);
            return !foreign.keys.empty();
        }
    }

    std::vector<AppearanceBodyMorph> Capture(RE::Actor& a_player)
    {
        std::vector<AppearanceBodyMorph> result;
        auto* bodyMorphs = SKEEInterfaces::GetBodyMorphs();
        if (!bodyMorphs || !bodyMorphs->HasMorphs(std::addressof(a_player)))
        {
            return result;
        }

        // SKEE renders the sum of a name's values over its keys.
        MorphSums visitor;
        bodyMorphs->VisitMorphValues(std::addressof(a_player), visitor);
        for (const auto& [name, sum] : visitor.sums)
        {
            const auto value = std::llround(sum * AppearanceBodyMorph::kScale);
            if (value == 0)
            {
                continue;
            }
            if (!Multiplayer::IsValidBodyMorphName(name))
            {
                ReportSkippedName(name, "name", sum);
                continue;
            }
            if (std::llabs(value) > AppearanceBodyMorph::kMaximumValue)
            {
                ReportSkippedName(name, "range", sum);
                continue;
            }
            result.push_back(AppearanceBodyMorph{ .name = name, .value = static_cast<std::int32_t>(value) });
        }

        if (result.size() > PlayerAppearance::kMaximumBodyMorphs)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                logs::warn("Body morph capture kept the largest morphs over the limit: morphs={} limit={}",
                    result.size(), PlayerAppearance::kMaximumBodyMorphs);
            }
            std::ranges::stable_sort(
                result, std::greater{}, [](const AppearanceBodyMorph& a_morph) { return std::abs(a_morph.value); });
            result.resize(PlayerAppearance::kMaximumBodyMorphs);
            std::ranges::sort(result, {}, &AppearanceBodyMorph::name);
        }
        return result;
    }

    void Apply(RE::Actor& a_actor, std::span<const AppearanceBodyMorph> a_morphs)
    {
        auto* bodyMorphs = SKEEInterfaces::GetBodyMorphs();
        if (!bodyMorphs || !GameThread::Require("RaceMenuBodyMorphs::Apply"))
        {
            return;
        }
        auto& entry = s_entries[a_actor.GetHandle().native_handle()];
        entry.morphs.assign(a_morphs.begin(), a_morphs.end());
        entry.root.reset(a_actor.Get3D(false));
        entry.checkRemaining = kForeignCheckSeconds;
        Replace(*bodyMorphs, a_actor, entry, "revision");
    }

    void Maintain(RE::Actor& a_actor, float a_delta)
    {
        auto* bodyMorphs = SKEEInterfaces::GetBodyMorphs();
        const auto found = bodyMorphs ? s_entries.find(a_actor.GetHandle().native_handle()) : s_entries.end();
        if (found == s_entries.end())
        {
            return;
        }
        auto& entry = found->second;
        if (auto* root = a_actor.Get3D(false); root != entry.root.get())
        {
            entry.root.reset(root);
            if (root)
            {
                entry.checkRemaining = kForeignCheckSeconds;
                Replace(*bodyMorphs, a_actor, entry, "3D rebuilt");
            }
            return;
        }
        if (entry.checkRemaining <= 0.0F)
        {
            return;
        }
        entry.checkRemaining -= FrameTime::Clamp(a_delta);
        if (entry.checkRemaining <= 0.0F && HasForeignMorphs(*bodyMorphs, a_actor))
        {
            Replace(*bodyMorphs, a_actor, entry, "foreign morphs");
        }
    }

    void Release(RE::ActorHandle a_handle, RE::Actor* a_actor)
    {
        s_entries.erase(a_handle.native_handle());
        auto* bodyMorphs = SKEEInterfaces::GetBodyMorphs();
        // SKEE's co-save writes every reference that has morphs.
        if (bodyMorphs && a_actor && bodyMorphs->HasMorphs(a_actor))
        {
            bodyMorphs->ClearMorphs(a_actor);
        }
    }
}
