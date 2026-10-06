#pragma once

#include "forms/ItemFilter.h"
#include "protocol/FormReference.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Forms
{
    struct CatalogItem
    {
        Multiplayer::FormReference reference;
        std::uint32_t formID{ 0 };
        std::string name;
        ItemTraits traits;
    };

    class ItemCatalog
    {
    public:
        ItemCatalog(std::vector<CatalogItem> a_items, std::span<const std::string_view> a_loadedPlugins);

        [[nodiscard]] std::span<const CatalogItem> GetItems() const noexcept { return items; }

        // Plugin names compare without case, as the game's lookup does.
        [[nodiscard]] const CatalogItem* Find(const Multiplayer::FormReference& a_reference) const;
        [[nodiscard]] bool IsPluginLoaded(std::string_view a_plugin) const;

        void Search(const ItemFilter& a_filter, std::string_view a_query, std::vector<std::uint32_t>& a_result) const;

    private:
        std::vector<CatalogItem> items;
        std::vector<std::string> searchText;
        std::vector<std::uint32_t> byReference;
        std::vector<std::string> loadedPlugins;
    };

    [[nodiscard]] std::shared_ptr<const ItemCatalog> GetItemCatalog();
    void PublishItemCatalog(std::shared_ptr<const ItemCatalog> a_catalog);
}
