#include "sync/equipment/FakeLight.h"
#include "forms/DynamicForm.h"
#include "runtime/GameThread.h"

#include <memory>
#include <unordered_map>

namespace FakeLight
{
    namespace
    {
        using Flag = RE::TES_LIGHT_FLAGS;

        // TESObjectLIGH::GenDynamic (ID 17610) makes a shadow light when any of these is set.
        bool CastsShadow(const RE::TESObjectLIGH& a_light)
        {
            return a_light.data.flags.any(Flag::kSpotShadow, Flag::kHemiShadow, Flag::kOmniShadow);
        }

        template <class Component>
        void CopyComponent(RE::TESObjectLIGH& a_copy, RE::TESObjectLIGH& a_source)
        {
            static_cast<Component&>(a_copy).CopyComponent(std::addressof(static_cast<Component&>(a_source)));
        }

        // TESObjectLIGH::Copy is TESForm's empty one (ID 14657); each component's
        // CopyComponent copies its own data from the source cast by RTTI.
        RE::TESObjectLIGH* CreateCopy(RE::TESObjectLIGH& a_source)
        {
            auto* copy = DynamicForm::Create<RE::TESObjectLIGH>();
            if (!copy)
            {
                return nullptr;
            }
            copy->boundData = a_source.boundData;
            CopyComponent<RE::TESFullName>(*copy, a_source);
            CopyComponent<RE::TESModelTextureSwap>(*copy, a_source);
            CopyComponent<RE::TESIcon>(*copy, a_source);
            CopyComponent<RE::BGSMessageIcon>(*copy, a_source);
            CopyComponent<RE::TESWeightForm>(*copy, a_source);
            CopyComponent<RE::TESValueForm>(*copy, a_source);
            CopyComponent<RE::BGSDestructibleObjectForm>(*copy, a_source);
            CopyComponent<RE::BGSEquipType>(*copy, a_source);
            copy->data = a_source.data;
            copy->data.flags.reset(Flag::kSpotShadow, Flag::kHemiShadow, Flag::kOmniShadow);
            copy->fade = a_source.fade;
            copy->sound = a_source.sound;
            copy->emittanceColor = a_source.emittanceColor;
            copy->lensFlare = a_source.lensFlare;
            return copy;
        }
    }

    RE::TESObjectLIGH& Get(RE::TESObjectLIGH& a_light)
    {
        if (!CastsShadow(a_light) || !GameThread::Require("FakeLight::Get"))
        {
            return a_light;
        }
        // Game thread only; copies are never deleted.
        static std::unordered_map<RE::FormID, RE::TESObjectLIGH*> copies;
        const auto sourceID = a_light.GetFormID();
        if (const auto found = copies.find(sourceID); found != copies.end())
        {
            return *found->second;
        }
        auto* copy = CreateCopy(a_light);
        if (!copy)
        {
            logs::error("Fake light could not be created; the shadow light is kept: light={:08X}", sourceID);
            return a_light;
        }
        copies.emplace(sourceID, copy);
        logs::info("Fake light created: light={:08X} copy={:08X} flags={:X}", sourceID, copy->GetFormID(),
            copy->data.flags.underlying());
        return *copy;
    }
}
