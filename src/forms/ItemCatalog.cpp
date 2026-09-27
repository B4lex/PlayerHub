#include "forms/ItemCatalog.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <utility>

namespace Forms
{
    namespace
    {
        using Multiplayer::FormReference;

        std::atomic<std::shared_ptr<const ItemCatalog>> s_catalog;

        char ToLower(char a_character)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(a_character)));
        }

        void AppendLower(std::string_view a_text, std::string& a_result)
        {
            for (const auto character : a_text)
            {
                a_result += ToLower(character);
            }
        }

        // Three-way comparison of ASCII letters without case; other bytes compare as they are.
        int CompareIgnoreCase(std::string_view a_left, std::string_view a_right)
        {
            const auto count = (std::min)(a_left.size(), a_right.size());
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto left = static_cast<unsigned char>(ToLower(a_left[index]));
                const auto right = static_cast<unsigned char>(ToLower(a_right[index]));
                if (left != right)
                {
                    return left < right ? -1 : 1;
                }
            }
            return a_left.size() == a_right.size() ? 0 : (a_left.size() < a_right.size() ? -1 : 1);
        }

        bool ReferenceLess(const FormReference& a_left, const FormReference& a_right)
        {
            const auto plugin = CompareIgnoreCase(a_left.plugin, a_right.plugin);
            return plugin != 0 ? plugin < 0 : a_left.localFormID < a_right.localFormID;
        }
    }

    ItemCatalog::ItemCatalog(std::vector<CatalogItem> a_items, std::span<const std::string_view> a_loadedPlugins)
        : items(std::move(a_items))
    {
        std::ranges::sort(items, [](const CatalogItem& a_left, const CatalogItem& a_right) {
            const auto name = CompareIgnoreCase(a_left.name, a_right.name);
            return name != 0 ? name < 0 : ReferenceLess(a_left.reference, a_right.reference);
        });

        searchText.reserve(items.size());
        byReference.reserve(items.size());
        for (std::uint32_t index = 0; index < items.size(); ++index)
        {
            const auto& item = items[index];
            auto& text = searchText.emplace_back();
            text.reserve(item.name.size() + 1 + item.reference.plugin.size());
            AppendLower(item.name, text);
            text += '\n';
            AppendLower(item.reference.plugin, text);
            byReference.push_back(index);
        }
        std::ranges::sort(byReference, [this](std::uint32_t a_left, std::uint32_t a_right) {
            return ReferenceLess(items[a_left].reference, items[a_right].reference);
        });

        loadedPlugins.reserve(a_loadedPlugins.size());
        for (const auto plugin : a_loadedPlugins)
        {
            AppendLower(plugin, loadedPlugins.emplace_back());
        }
        std::ranges::sort(loadedPlugins);
    }

    const CatalogItem* ItemCatalog::Find(const FormReference& a_reference) const
    {
        const auto found = std::ranges::lower_bound(
            byReference, a_reference,
            [](const FormReference& a_left, const FormReference& a_right) { return ReferenceLess(a_left, a_right); },
            [this](std::uint32_t a_index) -> const FormReference& { return items[a_index].reference; });
        if (found == byReference.end() || ReferenceLess(a_reference, items[*found].reference))
        {
            return nullptr;
        }
        return &items[*found];
    }

    bool ItemCatalog::IsPluginLoaded(std::string_view a_plugin) const
    {
        const auto found = std::ranges::lower_bound(loadedPlugins, a_plugin,
            [](std::string_view a_left, std::string_view a_right) { return CompareIgnoreCase(a_left, a_right) < 0; });
        return found != loadedPlugins.end() && CompareIgnoreCase(*found, a_plugin) == 0;
    }

    void ItemCatalog::Search(
        const ItemFilter& a_filter, std::string_view a_query, std::vector<std::uint32_t>& a_result) const
    {
        a_result.clear();
        std::string query;
        AppendLower(a_query, query);
        std::vector<std::string_view> words;
        for (std::size_t start = 0; start < query.size();)
        {
            const auto end = (std::min)(query.find(' ', start), query.size());
            if (end > start)
            {
                words.emplace_back(query.data() + start, end - start);
            }
            start = end + 1;
        }

        for (std::uint32_t index = 0; index < items.size(); ++index)
        {
            if (!a_filter.Matches(items[index].traits))
            {
                continue;
            }
            const std::string_view text = searchText[index];
            if (std::ranges::all_of(words, [text](std::string_view a_word) { return text.contains(a_word); }))
            {
                a_result.push_back(index);
            }
        }
    }

    std::shared_ptr<const ItemCatalog> GetItemCatalog()
    {
        return s_catalog.load(std::memory_order_acquire);
    }

    void PublishItemCatalog(std::shared_ptr<const ItemCatalog> a_catalog)
    {
        s_catalog.store(std::move(a_catalog), std::memory_order_release);
    }
}
