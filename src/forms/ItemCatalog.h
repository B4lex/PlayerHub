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
        // This session's form ID, for engine lookups such as the model preview.
        std::uint32_t formID{ 0 };
        std::string name;
        ItemTraits traits;
    };

    // Named item forms from every loaded plugin, sorted by name. Immutable, so any
    // thread may read it.
    class ItemCatalog
    {
    public:
        ItemCatalog(std::vector<CatalogItem> a_items, std::span<const std::string_view> a_loadedPlugins);

        [[nodiscard]] std::span<const CatalogItem> GetItems() const noexcept { return items; }

        // Plugin names compare without case, as the game's lookup does.
        [[nodiscard]] const CatalogItem* Find(const Multiplayer::FormReference& a_reference) const;
        [[nodiscard]] bool IsPluginLoaded(std::string_view a_plugin) const;

        // Replaces a_result with the indices, in name order, of the items that
        // match a_filter and contain every word of a_query in their name or plugin.
        void Search(const ItemFilter& a_filter, std::string_view a_query, std::vector<std::uint32_t>& a_result) const;

    private:
        std::vector<CatalogItem> items;
        // Lowercase name and plugin of each item, for Search.
        std::vector<std::string> searchText;
        // Item indices ordered by reference.
        std::vector<std::uint32_t> byReference;
        // Lowercase and sorted.
        std::vector<std::string> loadedPlugins;
    };

    // Null until the catalog is published. Safe from any thread.
    [[nodiscard]] std::shared_ptr<const ItemCatalog> GetItemCatalog();
    void PublishItemCatalog(std::shared_ptr<const ItemCatalog> a_catalog);
}
