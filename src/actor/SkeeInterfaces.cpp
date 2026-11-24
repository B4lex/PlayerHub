#include "actor/SkeeInterfaces.h"

#include <cstdint>
#include <memory>

namespace SkeeInterfaces
{
    namespace
    {
        class IInterfaceMap
        {
        public:
            virtual IPluginInterface* QueryInterface(const char* a_name) = 0;
            virtual bool AddInterface(const char* a_name, IPluginInterface* a_interface) = 0;
            virtual IPluginInterface* RemoveInterface(const char* a_name) = 0;
        };

        struct InterfaceExchangeMessage
        {
            static constexpr std::uint32_t kExchangeInterface = 0x9E3779B9;

            IInterfaceMap* interfaceMap{ nullptr };
        };

        // The version that added GetOverlayCount, and the one that introduced the
        // variant wrappers.
        constexpr std::uint32_t kMinimumOverlayVersion = 2;
        constexpr std::uint32_t kMinimumOverrideVersion = 2;
        // The layout was checked against version 5 only. Version 4 is the build the
        // SDK header shipped with before version 5 appended AddMorphShapeCallback;
        // older builds are not trusted.
        constexpr std::uint32_t kMinimumBodyMorphVersion = 4;

        IOverlayInterface* s_overlays{ nullptr };
        IOverrideInterface* s_overrides{ nullptr };
        IBodyMorphInterface* s_bodyMorphs{ nullptr };

        template <class T>
        T* Query(IInterfaceMap& a_map, const char* a_name, std::uint32_t a_minimumVersion, const char* a_feature)
        {
            auto* result = static_cast<T*>(a_map.QueryInterface(a_name));
            const auto version = result ? result->GetVersion() : 0;
            if (version < a_minimumVersion)
            {
                logs::warn(
                    "RaceMenu (SKEE) {} interface is missing or too old; {} are not synchronized: version={} "
                    "minimum={}",
                    a_name, a_feature, version, a_minimumVersion);
                return nullptr;
            }
            logs::info("RaceMenu (SKEE) {} interface found: version={}", a_name, version);
            return result;
        }
    }

    void Initialize()
    {
        InterfaceExchangeMessage message;
        const auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging
            || !messaging->Dispatch(InterfaceExchangeMessage::kExchangeInterface, std::addressof(message),
                sizeof(InterfaceExchangeMessage*), "skee")
            || !message.interfaceMap)
        {
            logs::info("RaceMenu (SKEE) did not answer; overlays and body morphs are not synchronized");
            return;
        }

        auto& map = *message.interfaceMap;
        auto* overlays = Query<IOverlayInterface>(map, "Overlay", kMinimumOverlayVersion, "overlays");
        auto* overrides = Query<IOverrideInterface>(map, "Override", kMinimumOverrideVersion, "overlays");
        if (overlays && overrides)
        {
            s_overlays = overlays;
            s_overrides = overrides;
        }
        s_bodyMorphs = Query<IBodyMorphInterface>(map, "BodyMorph", kMinimumBodyMorphVersion, "body morphs");
    }

    IOverlayInterface* GetOverlays()
    {
        return s_overlays;
    }

    IOverrideInterface* GetOverrides()
    {
        return s_overrides;
    }

    IBodyMorphInterface* GetBodyMorphs()
    {
        return s_bodyMorphs;
    }
}
