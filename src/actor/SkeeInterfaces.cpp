#include "actor/SkeeInterfaces.h"

#include <algorithm>
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
        // FaceMorph kept version 2 through the 2023 interface update that dropped Load
        // and Save from its vtable. BodyMorph version 5 came after that update, so it
        // tells the current layout apart.
        constexpr std::uint32_t kMinimumFaceMorphBodyMorphVersion = 5;

        IOverlayInterface* s_overlays{ nullptr };
        IOverrideInterface* s_overrides{ nullptr };
        IBodyMorphInterface* s_bodyMorphs{ nullptr };
        IFaceMorphInterface* s_faceMorphs{ nullptr };

        // SKEE's utils::hash_lower: FNV-1a over the lower-cased bytes.
        std::size_t HashLower(std::string_view a_text) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto character : a_text)
            {
                const auto byte = static_cast<unsigned char>(character);
                hash ^= byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
                hash *= 1099511628211ULL;
            }
            return static_cast<std::size_t>(hash);
        }

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

    SkeeString::SkeeString(std::string_view a_text)
    {
        layout.size = a_text.size();
        layout.hash = HashLower(a_text);
        if (a_text.size() <= kInPlaceCapacity)
        {
            std::ranges::copy(a_text, layout.storage.buffer);
            layout.storage.buffer[a_text.size()] = '\0';
            layout.capacity = kInPlaceCapacity;
            return;
        }
        heap = std::make_unique<char[]>(a_text.size() + 1);
        std::ranges::copy(a_text, heap.get());
        heap[a_text.size()] = '\0';
        layout.storage.pointer = heap.get();
        layout.capacity = a_text.size();
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
            logs::info("RaceMenu (SKEE) did not answer; overlays, body morphs, and face sliders are not synchronized");
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

        const auto bodyMorphVersion = s_bodyMorphs ? s_bodyMorphs->GetVersion() : 0;
        auto* faceMorphs = static_cast<IFaceMorphInterface*>(map.QueryInterface("FaceMorph"));
        if (!faceMorphs || bodyMorphVersion < kMinimumFaceMorphBodyMorphVersion)
        {
            logs::warn(
                "RaceMenu (SKEE) FaceMorph interface is missing or from an older build; face sliders are not "
                "synchronized: found={} bodyMorphVersion={} minimumBodyMorphVersion={}",
                faceMorphs != nullptr, bodyMorphVersion, kMinimumFaceMorphBodyMorphVersion);
            return;
        }
        s_faceMorphs = faceMorphs;
        logs::info("RaceMenu (SKEE) FaceMorph interface found: version={}", faceMorphs->GetVersion());
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

    IFaceMorphInterface* GetFaceMorphs()
    {
        return s_faceMorphs;
    }
}
