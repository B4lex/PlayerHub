#include "actor/RemoteLights.h"
#include "actor/ActorRegistry.h"
#include "config/PluginConfig.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace RemoteLights
{
    namespace
    {
        // ShadowSceneNode's light updates (IDs 106342 and 106392) detach a light from every
        // geometry (ID 108285) while its NiLight is app-culled or its fade is below this.
        constexpr float kMinimumLitFade = 0.05F;
        // Of squared distances: a lit light keeps its slot until an unlit one is a fifth
        // closer, so players at about the same distance do not trade the light back and forth.
        constexpr float kLitRankScale = 0.64F;

        struct CulledLight
        {
            // Never the NiLight: ID 106392 drops a light from the scene once its BSLight holds
            // the last reference to the NiLight, so holding one would leave the light behind.
            RE::NiPointer<RE::BSLight> light;
            std::uint64_t seenPass{ 0 };
        };

        struct Candidate
        {
            RE::BSLight* light{ nullptr };
            float rank{ 0.0F };
        };

        std::unordered_map<const RE::BSLight*, CulledLight> s_culled;
        std::vector<Candidate> s_candidates;
        std::uint64_t s_pass{ 0 };
        std::optional<std::uint64_t> s_configRevision;
        std::uint32_t s_limit{ 0 };

        struct LogState
        {
            std::uint32_t limit{ 0 };
            std::size_t remote{ 0 };
            std::size_t culled{ 0 };

            bool operator==(const LogState&) const = default;
        };

        LogState s_logged;

        void FollowConfig()
        {
            const auto revision = Config::GetRevision();
            if (s_configRevision == revision)
            {
                return;
            }
            s_configRevision = revision;
            s_limit = Config::Get()->advanced.remoteLights;
        }

        bool IsOnRemoteActor(const RE::NiLight& a_light)
        {
            auto* owner = a_light.GetUserData();
            return owner && ActorRegistry::IsRegistered(owner->As<RE::Actor>());
        }

        template <class Light>
        void Collect(const RE::BSTArray<RE::NiPointer<Light>>& a_lights, const RE::NiPoint3& a_cameraPosition)
        {
            for (const auto& entry : a_lights)
            {
                RE::BSLight* light = entry.get();
                const auto* niLight = light ? light->light.get() : nullptr;
                if (!niLight)
                {
                    continue;
                }
                const auto culled = s_culled.find(light);
                const bool culledHere = culled != s_culled.end();
                // A light the engine keeps dark already frees its slot.
                if (!culledHere && (niLight->GetAppCulled() || niLight->GetLightRuntimeData().fade < kMinimumLitFade))
                {
                    continue;
                }
                if (!IsOnRemoteActor(*niLight))
                {
                    continue;
                }
                if (culledHere)
                {
                    culled->second.seenPass = s_pass;
                }
                const float distanceSquared = niLight->world.translate.GetSquaredDistance(a_cameraPosition);
                s_candidates.push_back(
                    { .light = light, .rank = culledHere ? distanceSquared : distanceSquared * kLitRankScale });
            }
        }

        void Show(RE::BSLight& a_light)
        {
            const auto culled = s_culled.find(&a_light);
            if (culled == s_culled.end())
            {
                return;
            }
            a_light.light->SetAppCulled(false);
            s_culled.erase(culled);
        }

        // Set again in every pass, in case the engine clears it.
        void Cull(RE::BSLight& a_light)
        {
            a_light.light->SetAppCulled(true);
            s_culled.try_emplace(
                &a_light, CulledLight{ .light = RE::NiPointer<RE::BSLight>{ &a_light }, .seenPass = s_pass });
        }

        // Lights that left the scene or their remote actor get their flag back, so none
        // stays dark without being tracked.
        void RestoreUnseen()
        {
            std::erase_if(s_culled, [](const auto& a_entry) {
                const auto& culled = a_entry.second;
                if (culled.seenPass == s_pass)
                {
                    return false;
                }
                if (culled.light->light)
                {
                    culled.light->light->SetAppCulled(false);
                }
                return true;
            });
        }
    }

    void Update(const RE::NiPoint3& a_cameraPosition, bool a_hasRemotePlayers)
    {
        if (!a_hasRemotePlayers && s_culled.empty())
        {
            return;
        }
        if (!GameThread::Require("RemoteLights::Update"))
        {
            return;
        }
        auto* sceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
        if (!sceneNode)
        {
            return;
        }
        PROFILE_FUNCTION();
        FollowConfig();
        ++s_pass;
        s_candidates.clear();
        {
            auto& scene = sceneNode->GetRuntimeData();
            // Queued lights join the active arrays under this lock (ID 106335).
            const RE::BSSpinLockGuard lock{ scene.lightQueueLock };
            Collect(scene.activeLights, a_cameraPosition);
            Collect(scene.activeShadowLights, a_cameraPosition);

            const auto lit = (std::min)(static_cast<std::size_t>(s_limit), s_candidates.size());
            if (lit < s_candidates.size())
            {
                std::nth_element(s_candidates.begin(), s_candidates.begin() + static_cast<std::ptrdiff_t>(lit),
                    s_candidates.end(), [](const Candidate& a_left, const Candidate& a_right) {
                        return a_left.rank < a_right.rank;
                    });
            }
            for (std::size_t index = 0; index < s_candidates.size(); ++index)
            {
                auto& light = *s_candidates[index].light;
                if (index < lit)
                {
                    Show(light);
                }
                else
                {
                    Cull(light);
                }
            }
            RestoreUnseen();
        }

        const LogState state{ .limit = s_limit, .remote = s_candidates.size(), .culled = s_culled.size() };
        if (state != s_logged)
        {
            s_logged = state;
            logs::debug("Remote light budget: limit={} remoteLights={} culled={}", state.limit, state.remote,
                state.culled);
        }
    }
}
