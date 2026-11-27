#include "actor/RaceMenuFaceSliders.h"
#include "actor/RaceMenuSliderCatalog.h"
#include "actor/SKEEInterfaces.h"
#include "forms/DynamicForm.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace RaceMenuFaceSliders
{
    namespace
    {
        using Multiplayer::AppearanceFaceSlider;
        using Multiplayer::PlayerAppearance;

        constexpr std::size_t kMaximumReportedNames = 32;

        void ReportOutOfRange(std::string_view a_name, float a_value)
        {
            static std::vector<std::string> reported;
            if (reported.size() >= kMaximumReportedNames || std::ranges::find(reported, a_name) != reported.end())
            {
                return;
            }
            reported.emplace_back(a_name);
            logs::warn("Face slider capture skipped a value peers would reject: name=\"{}\" value={}", a_name, a_value);
        }

        // SKEE's StringTable frees a name once nothing holds it, so a lookup of a name
        // that no value uses allocates the entry and its removal scans the whole table.
        // Zero values on a base that no actor uses hold every catalog name. SKEE
        // reverts every base's values when a save loads, which clears the marker too.
        void HoldCatalogNames(SKEEInterfaces::IFaceMorphInterface& a_faceMorphs)
        {
            static RE::TESNPC* holder = nullptr;
            static bool unavailable = false;
            static const SKEEInterfaces::SKEEString marker{ "PlayerHub" };
            if (!holder && !unavailable)
            {
                holder = DynamicForm::Create<RE::TESNPC>();
                unavailable = holder == nullptr;
                if (unavailable)
                {
                    logs::warn("Face slider capture could not create the base that holds slider names");
                }
            }
            if (!holder || a_faceMorphs.GetMorphValueByName(holder, marker.Get()) != 0.0F)
            {
                return;
            }
            a_faceMorphs.SetMorphValue(holder, marker.Get(), 1.0F);
            const auto& sliders = RaceMenuSliderCatalog::GetAll();
            for (const auto& slider : sliders)
            {
                a_faceMorphs.SetMorphValue(holder, slider.key.Get(), 0.0F);
            }
            logs::debug("Face slider names held in SKEE's string table: base={:08X} names={}", holder->GetFormID(),
                sliders.size());
        }
    }

    std::vector<AppearanceFaceSlider> Capture(RE::TESNPC& a_playerBase, const RE::TESRace& a_race, bool a_female)
    {
        PROFILE_FUNCTION();
        std::vector<AppearanceFaceSlider> result;
        auto* faceMorphs = SKEEInterfaces::GetFaceMorphs();
        if (!faceMorphs || !GameThread::Require("RaceMenuFaceSliders::Capture"))
        {
            return result;
        }

        HoldCatalogNames(*faceMorphs);
        for (const auto& slider : RaceMenuSliderCatalog::Get(a_race, a_female))
        {
            const float value = faceMorphs->GetMorphValueByName(std::addressof(a_playerBase), slider.key.Get());
            if (!std::isfinite(value))
            {
                ReportOutOfRange(slider.name, value);
                continue;
            }
            const auto scaled = std::llround(static_cast<double>(value) * AppearanceFaceSlider::kScale);
            if (scaled == 0)
            {
                continue;
            }
            if (std::llabs(scaled) > AppearanceFaceSlider::kMaximumValue)
            {
                ReportOutOfRange(slider.name, value);
                continue;
            }
            result.push_back(AppearanceFaceSlider{ .name = slider.name, .value = static_cast<std::int32_t>(scaled) });
        }

        if (result.size() > PlayerAppearance::kMaximumFaceSliders)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                logs::warn("Face slider capture kept the largest values over the limit: sliders={} limit={}",
                    result.size(), PlayerAppearance::kMaximumFaceSliders);
            }
            std::ranges::stable_sort(
                result, std::greater{}, [](const AppearanceFaceSlider& a_slider) { return std::abs(a_slider.value); });
            result.resize(PlayerAppearance::kMaximumFaceSliders);
            std::ranges::sort(result, {}, &AppearanceFaceSlider::name);
        }
        return result;
    }

    void Apply(RE::TESNPC& a_base, std::span<const AppearanceFaceSlider> a_sliders)
    {
        auto* faceMorphs = SKEEInterfaces::GetFaceMorphs();
        if (!faceMorphs || a_sliders.empty() || !GameThread::Require("RaceMenuFaceSliders::Apply"))
        {
            return;
        }
        PROFILE_FUNCTION();
        for (const auto& slider : a_sliders)
        {
            const SKEEInterfaces::SKEEString name{ slider.name };
            faceMorphs->SetMorphValue(
                std::addressof(a_base), name.Get(), static_cast<float>(slider.value) / AppearanceFaceSlider::kScale);
        }
        logs::debug("Applied face sliders: base={:08X} sliders={}", a_base.GetFormID(), a_sliders.size());
    }
}
