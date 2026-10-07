#include "ui/trade/TradeWindow.h"
#include "sync/trade/TradeSync.h"
#include "text/AsciiCase.h"
#include "ui/Fonts.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/preview/ItemPreview.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        using Phase = TradeSession::Phase;

        constexpr ImVec2 kSizeFonts{ 56.0F, 40.0F };
        constexpr ImVec2 kMinimumSizeFonts{ 44.0F, 28.0F };
        constexpr float kBackgroundAlpha = 0.96F;
        constexpr float kCountColumnFonts = 4.0F;
        constexpr float kCountFieldFonts = 7.0F;
        constexpr float kTooltipWidthFonts = 22.0F;
        constexpr float kFooterButtonWidthFonts = 6.5F;
        constexpr float kTitleScale = 1.25F;
        // A window kept from focus this long by a game menu gives the trade up.
        constexpr float kFocusWaitSeconds = 2.0F;
        constexpr float kMaximumFrameSeconds = 0.1F;

        constexpr int kGridColumns = 5;
        constexpr int kGridRows = 3;
        constexpr float kMinimumCellFonts = 2.5F;
        constexpr float kMaximumCellFonts = 7.0F;
        constexpr float kMinimumInventoryWidthFonts = 20.0F;
        constexpr float kCellPaddingFonts = 0.2F;
        constexpr float kFallbackNameScale = 0.8F;

        // Item backgrounds are opaque: the thumbnail of each item is captured on its own.
        constexpr ImU32 kWellColor = IM_COL32(12, 12, 14, 255);
        constexpr ImU32 kWellLineColor = IM_COL32(44, 44, 50, 255);
        constexpr ImU32 kItemColor = IM_COL32(30, 30, 34, 255);
        constexpr ImU32 kEnchantedColor = IM_COL32(26, 34, 78, 255);
        constexpr ImU32 kUninspectedShade = IM_COL32(0, 0, 0, 150);
        constexpr ImU32 kCountShadow = IM_COL32(0, 0, 0, 220);
        constexpr float kAcceptedTintAlpha = 0.14F;

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
        constexpr const char* kOfferPopup = "##OfferLine";

        constexpr std::array<const char*, 11> kCategoryNames{ "All", "Weapons", "Apparel", "Ammunition", "Potions",
            "Ingredients", "Books", "Scrolls", "Soul gems", "Keys", "Miscellaneous" };

        std::string Describe(bool a_equipped, bool a_favorited)
        {
            std::string text;
            const auto add = [&text](std::string_view a_part) {
                text += text.empty() ? "" : " \xC2\xB7 ";
                text += a_part;
            };
            if (a_equipped)
            {
                add("Equipped");
            }
            if (a_favorited)
            {
                add("Favorite");
            }
            return text;
        }

        std::string FormatWeight(float a_weight)
        {
            const auto tenths = std::round(a_weight * 10.0F);
            return std::fmod(tenths, 10.0F) == 0.0F ? std::format("{:.0f}", tenths / 10.0F)
                                                    : std::format("{:.1f}", tenths / 10.0F);
        }

        // A label in the disabled color and its value, after a separating dot unless first.
        void DrawStat(const char* a_label, const std::string& a_value, bool& a_first)
        {
            if (!std::exchange(a_first, false))
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7");
                ImGui::SameLine();
            }
            ImGui::TextDisabled("%s", a_label);
            ImGui::SameLine(0.0F, ImGui::GetFontSize() * 0.3F);
            ImGui::TextUnformatted(a_value.c_str());
        }

        struct ItemTooltip
        {
            std::string_view name;
            std::uint32_t count{ 0 };
            const Forms::ItemDetails* details{ nullptr };
            std::string_view notes;
            // Why the item cannot be offered, or empty.
            std::string_view blocked;
        };

        // Name and count; the item's own damage or armor, weight, and value, with a stack's
        // totals; its effects as the game words them, and charge; then the notes.
        void DrawItemTooltip(const ItemTooltip& a_tooltip)
        {
            if (!ImGui::BeginTooltip())
            {
                return;
            }
            const auto& details = *a_tooltip.details;
            ImGui::PushTextWrapPos(kTooltipWidthFonts * ImGui::GetFontSize());
            ImGui::TextUnformatted(a_tooltip.name.data(), a_tooltip.name.data() + a_tooltip.name.size());
            if (a_tooltip.count > 1)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC3\x97%u", a_tooltip.count);
            }
            bool first = true;
            if (details.rating != Forms::ItemDetails::Rating::kNone)
            {
                DrawStat(details.rating == Forms::ItemDetails::Rating::kDamage ? "Damage" : "Armor",
                    std::format("{}", details.ratingValue), first);
            }
            DrawStat("Weight", FormatWeight(details.weight), first);
            DrawStat("Value", std::format("{}", details.value), first);
            if (a_tooltip.count > 1)
            {
                ImGui::TextDisabled("Total: weight %s \xC2\xB7 value %lld",
                    FormatWeight(details.weight * static_cast<float>(a_tooltip.count)).c_str(),
                    static_cast<long long>(details.value) * a_tooltip.count);
            }
            if (!details.effects.empty() || details.maximumCharge > 0)
            {
                ImGui::Separator();
                for (const auto& effect : details.effects)
                {
                    if (!effect.known)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    }
                    ImGui::TextUnformatted(effect.text.c_str());
                    if (!effect.known)
                    {
                        ImGui::PopStyleColor();
                    }
                }
                if (details.maximumCharge > 0)
                {
                    ImGui::TextDisabled("Charge %u / %u", details.charge, details.maximumCharge);
                }
            }
            if (!a_tooltip.notes.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(a_tooltip.notes.data(), a_tooltip.notes.data() + a_tooltip.notes.size());
                ImGui::PopStyleColor();
            }
            if (!a_tooltip.blocked.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::kWarning);
                ImGui::TextUnformatted(a_tooltip.blocked.data(), a_tooltip.blocked.data() + a_tooltip.blocked.size());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        struct CellItem
        {
            std::string_view name;
            std::uint32_t count{ 0 };
            std::uint32_t formID{ 0 };
            bool enchanted{ false };
        };

        // An offered line's cell: its thumbnail on a background by kind, or its name while the
        // thumbnail is unavailable, and its count in the corner as in Path of Exile.
        void DrawItemCell(ImDrawList& a_drawList, const CellItem& a_item, const ImVec2& a_min, const ImVec2& a_max)
        {
            const auto fontSize = ImGui::GetFontSize();
            const auto background = a_item.enchanted ? kEnchantedColor : kItemColor;
            if (ItemPreview::DrawThumbnail(a_drawList, a_item.formID, background, a_min, a_max)
                == ItemPreview::Thumbnail::kUnavailable)
            {
                const auto padding = kCellPaddingFonts * fontSize;
                const ImVec4 clip{ a_min.x + padding, a_min.y + padding, a_max.x - padding, a_max.y - padding };
                a_drawList.AddText(nullptr, fontSize * kFallbackNameScale,
                    ImVec2{ clip.x, a_item.count > 1 ? a_min.y + padding + fontSize : clip.y },
                    ImGui::GetColorU32(ImGuiCol_Text), a_item.name.data(), a_item.name.data() + a_item.name.size(),
                    clip.z - clip.x, &clip);
            }
            if (a_item.count > 1)
            {
                char text[16];
                const auto end = std::format_to_n(text, sizeof(text) - 1, "{}", a_item.count).out;
                *end = '\0';
                const ImVec2 position{ a_min.x + kCellPaddingFonts * fontSize,
                    a_min.y + kCellPaddingFonts * fontSize * 0.5F };
                a_drawList.AddText(ImVec2{ position.x + 1.0F, position.y + 1.0F }, kCountShadow, text, end);
                a_drawList.AddText(position, ImGui::GetColorU32(ImGuiCol_Text), text, end);
            }
        }

        class TradeWindow final : public Window
        {
        public:
            [[nodiscard]] bool UsesMouse() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                if (TradeSync::CopyView(revision, view))
                {
                    OnViewChanged();
                }
                if (view.phase != Phase::kOpen)
                {
                    if (a_input.focused)
                    {
                        WindowManager::ReleaseFocus(*this);
                    }
                    Reset();
                    return;
                }
                if (!a_input.focused)
                {
                    TakeFocus();
                    return;
                }
                hadFocus = true;
                bool open = true;
                fieldActive = ImGui::IsAnyItemActive();
                const bool escapeClaimed = fieldActive || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
                DrawWindow(open);
                if (!open || (!escapeClaimed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                {
                    Close(Multiplayer::TradeCloseReason::kCancelled);
                }
            }

        private:
            void Reset()
            {
                hadFocus = false;
                closing = false;
                waited = 0.0F;
                focusWindow = true;
                query.clear();
                category = 0;
                inspected.clear();
            }

            void OnViewChanged()
            {
                if (view.inventoryRevision != matchedRevision)
                {
                    Match();
                }
                // A line the peer added or changed has a new id, so it needs inspecting again.
                std::erase_if(inspected, [this](std::uint64_t a_id) {
                    return std::ranges::find(view.peerOffer, a_id, &TradeSync::PeerLine::id) == view.peerOffer.end();
                });
            }

            void TakeFocus()
            {
                if (closing)
                {
                    return;
                }
                if (hadFocus)
                {
                    // Something else took input: a game menu, the console, or a loading screen.
                    Close(Multiplayer::TradeCloseReason::kMenu);
                    return;
                }
                waited += (std::min)(ImGui::GetIO().DeltaTime, kMaximumFrameSeconds);
                if (waited > kFocusWaitSeconds)
                {
                    Close(Multiplayer::TradeCloseReason::kFailed);
                    return;
                }
                // A game menu releases it again at the next input dispatch.
                WindowManager::TakeFocus(*this);
            }

            void Close(Multiplayer::TradeCloseReason a_reason)
            {
                closing = true;
                TradeSync::Cancel(a_reason);
                WindowManager::ReleaseFocus(*this);
            }

            void Match()
            {
                matchedRevision = view.inventoryRevision;
                matches.clear();
                for (std::size_t row = 0; row < view.inventory.size(); ++row)
                {
                    const auto& line = view.inventory[row];
                    if ((category == 0 || static_cast<int>(line.category) == category - 1)
                        && AsciiCase::Contains(line.name, query))
                    {
                        matches.push_back(row);
                    }
                }
            }

            void DrawWindow(bool& a_open)
            {
                const Theme::Scope theme;
                const MenuFrame::WindowLayout layout{ .sizeFonts = kSizeFonts,
                    .minimumSizeFonts = kMinimumSizeFonts,
                    .paddingFonts = 1.0F,
                    .backgroundAlpha = kBackgroundAlpha };
                if (MenuFrame::BeginWindow("Trade###Trade", layout, kWindowFlags, std::exchange(focusWindow, false)))
                {
                    DrawTitle(a_open);
                    const auto& style = ImGui::GetStyle();
                    const auto available = ImGui::GetContentRegionAvail();
                    const auto height = available.y - ImGui::GetFrameHeight() - style.ItemSpacing.y;
                    const auto cell = GetCellSize(available.x, height);
                    const auto gridWidth = cell * static_cast<float>(kGridColumns) + style.ScrollbarSize;
                    DrawInventory(ImVec2{ available.x - gridWidth - style.ItemSpacing.x, height });
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    DrawPeerOffer(cell);
                    DrawOwnOffer(cell);
                    ImGui::EndGroup();
                    DrawOfferPopup();
                    DrawFooter();
                }
                ImGui::End();
            }

            // The largest cell up to kMaximumCellFonts that fits both grids' visible rows in the
            // height and leaves the inventory list its width.
            [[nodiscard]] static float GetCellSize(float a_width, float a_height)
            {
                const auto& style = ImGui::GetStyle();
                const auto fontSize = ImGui::GetFontSize();
                const auto byHeight = (a_height - 2.0F * (ImGui::GetFrameHeight() + style.ItemSpacing.y + 1.0F)
                                          - 2.0F * style.ItemSpacing.y)
                    / static_cast<float>(kGridRows * 2);
                const auto byWidth
                    = (a_width - kMinimumInventoryWidthFonts * fontSize - style.ItemSpacing.x - style.ScrollbarSize)
                    / static_cast<float>(kGridColumns);
                return std::floor(std::clamp(
                    (std::min)(byHeight, byWidth), kMinimumCellFonts * fontSize, kMaximumCellFonts * fontSize));
            }

            void DrawTitle(bool& a_open) const
            {
                const auto& style = ImGui::GetStyle();
                ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kTitleScale);
                ImGui::TextUnformatted("Trade with");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::GetAccent());
                ImGui::TextUnformatted(view.peer.name.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
                if (Controls::IconButton("##close", Controls::Icon::kClose))
                {
                    a_open = false;
                }
                ImGui::Separator();
            }

            static void DrawGridHeading(const char* a_label, bool a_accepted)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(a_label);
                if (a_accepted)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(Theme::kPositive, "Accepted");
                }
            }

            // Draws the grid's wells, then each line's cell through a_drawCell(index, min, max,
            // hovered) over an invisible button the callback may query. The grid shows
            // kGridRows and gains rows to scroll through as the lines outgrow it.
            template <class DrawCell>
            static void DrawGrid(
                const char* a_id, std::size_t a_lines, float a_cell, bool a_accepted, DrawCell&& a_drawCell)
            {
                const auto& style = ImGui::GetStyle();
                const auto columns = static_cast<std::size_t>(kGridColumns);
                const auto rows = (std::max)(static_cast<std::size_t>(kGridRows), (a_lines + columns - 1) / columns);
                // One more pixel for the wells' closing line.
                const ImVec2 size{ a_cell * static_cast<float>(kGridColumns) + style.ScrollbarSize,
                    a_cell * static_cast<float>(kGridRows) + 1.0F };
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0.0F, 0.0F });
                const bool visible = ImGui::BeginChild(a_id, size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                ImGui::PopStyleVar();
                if (visible)
                {
                    auto& drawList = *ImGui::GetWindowDrawList();
                    const auto origin = ImGui::GetCursorScreenPos();
                    const auto cellAt = [&](std::size_t a_index) {
                        const ImVec2 min{ origin.x + static_cast<float>(a_index % columns) * a_cell,
                            origin.y + static_cast<float>(a_index / columns) * a_cell };
                        return std::pair{ min, ImVec2{ min.x + a_cell, min.y + a_cell } };
                    };
                    const ImVec2 gridEnd{ origin.x + a_cell * static_cast<float>(kGridColumns),
                        origin.y + a_cell * static_cast<float>(rows) };
                    drawList.AddRectFilled(origin, gridEnd, kWellColor);
                    for (std::size_t index = 0; index < rows * columns; ++index)
                    {
                        const auto [min, max] = cellAt(index);
                        drawList.AddRect(min, ImVec2{ max.x + 1.0F, max.y + 1.0F }, kWellLineColor);
                    }
                    for (std::size_t index = 0; index < a_lines; ++index)
                    {
                        const auto [min, max] = cellAt(index);
                        ImGui::SetCursorScreenPos(min);
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::InvisibleButton("##cell", ImVec2{ a_cell, a_cell },
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        const bool hovered = ImGui::IsItemHovered();
                        // Inside the well lines, which stay visible between neighbouring items.
                        const ImVec2 inner{ min.x + 1.0F, min.y + 1.0F };
                        a_drawCell(index, inner, max, hovered);
                        if (hovered)
                        {
                            drawList.AddRect(inner, max, ImGui::GetColorU32(Theme::GetAccent()), 0.0F, 0, 2.0F);
                        }
                        ImGui::PopID();
                    }
                    ImGui::SetCursorScreenPos(origin);
                    ImGui::Dummy(
                        ImVec2{ a_cell * static_cast<float>(kGridColumns), a_cell * static_cast<float>(rows) + 1.0F });
                    if (a_accepted)
                    {
                        // Path of Exile tints the side of a player who accepted.
                        const auto windowMin = ImGui::GetWindowPos();
                        const ImVec2 windowMax{ windowMin.x + a_cell * static_cast<float>(kGridColumns),
                            windowMin.y + ImGui::GetWindowHeight() };
                        auto tint = Theme::kPositive;
                        tint.w = kAcceptedTintAlpha;
                        drawList.AddRectFilled(windowMin, windowMax, ImGui::GetColorU32(tint));
                        drawList.AddRect(windowMin, windowMax, ImGui::GetColorU32(Theme::kPositive), 0.0F, 0, 2.0F);
                    }
                }
                ImGui::EndChild();
            }

            void DrawPeerOffer(float a_cell)
            {
                char heading[96];
                const auto end = std::format_to_n(heading, sizeof(heading) - 1, "{}'s offer", view.peer.name).out;
                *end = '\0';
                DrawGridHeading(heading, view.peerAccepted);
                DrawGrid("##peer", view.peerOffer.size(), a_cell, view.peerAccepted,
                    [this](std::size_t a_index, const ImVec2& a_min, const ImVec2& a_max, bool a_hovered) {
                        const auto& line = view.peerOffer[a_index];
                        auto& drawList = *ImGui::GetWindowDrawList();
                        DrawItemCell(drawList,
                            CellItem{ .name = line.name,
                                .count = line.count,
                                .formID = line.formID,
                                .enchanted = line.enchanted },
                            a_min, a_max);
                        bool seen = std::ranges::find(inspected, line.id) != inspected.end();
                        if (a_hovered)
                        {
                            DrawItemTooltip(
                                ItemTooltip{ .name = line.name, .count = line.count, .details = &line.details });
                            if (!seen)
                            {
                                inspected.push_back(line.id);
                                seen = true;
                            }
                        }
                        if (!seen)
                        {
                            drawList.AddRectFilled(a_min, a_max, kUninspectedShade);
                        }
                    });
                uninspected = static_cast<std::size_t>(
                    std::ranges::count_if(view.peerOffer, [this](const TradeSync::PeerLine& a_line) {
                        return std::ranges::find(inspected, a_line.id) == inspected.end();
                    }));
            }

            void DrawOwnOffer(float a_cell)
            {
                DrawGridHeading("Your offer", view.ownAccepted);
                DrawGrid("##own", view.offer.size(), a_cell, view.ownAccepted,
                    [this](std::size_t a_index, const ImVec2& a_min, const ImVec2& a_max, bool a_hovered) {
                        const auto& line = view.offer[a_index];
                        if (line.row >= view.inventory.size())
                        {
                            return;
                        }
                        const auto& item = view.inventory[line.row];
                        DrawItemCell(*ImGui::GetWindowDrawList(),
                            CellItem{ .name = item.name,
                                .count = line.count,
                                .formID = item.formID,
                                .enchanted = item.enchanted },
                            a_min, a_max);
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        {
                            popupRow = line.row;
                            openPopup = true;
                        }
                        else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                        {
                            TradeSync::OfferOneLess(view.inventoryRevision, line.row);
                        }
                        if (a_hovered)
                        {
                            const auto notes = Describe(item.equipped, item.favorited);
                            DrawItemTooltip(ItemTooltip{
                                .name = item.name, .count = line.count, .details = &item.details, .notes = notes });
                        }
                    });
            }

            // A clicked line of the own offer: its count for a stack, and Remove.
            void DrawOfferPopup()
            {
                if (std::exchange(openPopup, false))
                {
                    ImGui::OpenPopup(kOfferPopup);
                }
                if (!ImGui::BeginPopup(kOfferPopup))
                {
                    return;
                }
                const auto line = std::ranges::find(view.offer, popupRow, &TradeSync::OfferLine::row);
                if (line == view.offer.end() || popupRow >= view.inventory.size()
                    || (!fieldActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return;
                }
                const auto& item = view.inventory[popupRow];
                ImGui::TextUnformatted(item.name.c_str());
                if (item.count > 1)
                {
                    std::uint32_t count = line->count;
                    const std::uint32_t step = 1;
                    ImGui::SetNextItemWidth(kCountFieldFonts * ImGui::GetFontSize());
                    if (ImGui::InputScalar("##count", ImGuiDataType_U32, &count, &step))
                    {
                        TradeSync::SetOfferCount(view.inventoryRevision, popupRow, std::clamp(count, 1U, item.count));
                    }
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("of %u", item.count);
                }
                if (Controls::DangerButton("Remove", ImVec2{ -FLT_MIN, 0.0F }))
                {
                    TradeSync::SetOfferCount(view.inventoryRevision, popupRow, 0);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            void DrawInventory(const ImVec2& a_size)
            {
                ImGui::BeginGroup();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Your items");
                const auto& style = ImGui::GetStyle();
                const auto comboWidth = std::floor(a_size.x * 0.4F);
                ImGui::SetNextItemWidth(a_size.x - comboWidth - style.ItemSpacing.x);
                bool changed = ImGui::InputTextWithHint("##search", "Search", &query);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(comboWidth);
                changed = ImGui::Combo("##category", &category, kCategoryNames.data(),
                              static_cast<int>(kCategoryNames.size()))
                    || changed;
                if (changed)
                {
                    Match();
                }
                const ImVec2 listSize{ a_size.x, (std::max)(ImGui::GetContentRegionAvail().y
                                                         - ImGui::GetFrameHeight() - style.ItemSpacing.y,
                                                     ImGui::GetFrameHeight()) };
                if (ImGui::BeginTable("##inventory", 2, kListFlags, listSize))
                {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, kCountColumnFonts * ImGui::GetFontSize());
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(matches.size()));
                    while (clipper.Step())
                    {
                        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                        {
                            DrawInventoryRow(matches[static_cast<std::size_t>(index)]);
                        }
                    }
                    if (matches.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled(view.inventory.empty() ? "You carry nothing to trade." : "No item matches.");
                    }
                    ImGui::EndTable();
                }
                ImGui::EndGroup();
            }

            [[nodiscard]] std::uint32_t OfferedCount(std::size_t a_row) const
            {
                const auto line = std::ranges::find(view.offer, a_row, &TradeSync::OfferLine::row);
                return line != view.offer.end() ? line->count : 0;
            }

            void DrawInventoryRow(std::size_t a_row)
            {
                const auto& line = view.inventory[a_row];
                const auto offered = OfferedCount(a_row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(a_row));
                const bool blocked = !line.blocked.empty();
                ImGui::BeginDisabled(blocked);
                if (ImGui::Selectable(line.name.c_str(), offered > 0,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    TradeSync::SetOfferCount(view.inventoryRevision, a_row, line.count);
                }
                if (!blocked && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    TradeSync::OfferOneMore(view.inventoryRevision, a_row);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    const auto notes = Describe(line.equipped, line.favorited);
                    DrawItemTooltip(ItemTooltip{ .name = line.name,
                        .count = line.count,
                        .details = &line.details,
                        .notes = notes,
                        .blocked = line.blocked });
                }
                ImGui::TableNextColumn();
                if (line.count > 1)
                {
                    ImGui::TextDisabled("%u", line.count);
                }
                ImGui::PopID();
            }

            void DrawFooter()
            {
                const auto& style = ImGui::GetStyle();
                const auto fontSize = ImGui::GetFontSize();
                const auto buttonWidth = kFooterButtonWidthFonts * fontSize;
                // Acceptance shows on the grids; the line is left for the one rule they cannot show.
                if (uninspected > 0)
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled(
                        "Hover over %s's items to inspect them (%zu left).", view.peer.name.c_str(), uninspected);
                }
                else
                {
                    ImGui::Dummy(ImVec2{ 0.0F, ImGui::GetFrameHeight() });
                }
                ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2.0F - style.ItemSpacing.x);
                const bool blocked = !view.acceptBlocker.empty();
                ImGui::BeginDisabled(view.ownAccepted || blocked || uninspected > 0);
                if (Controls::PrimaryButton("Accept", ImVec2{ buttonWidth, 0.0F }))
                {
                    TradeSync::Accept();
                }
                ImGui::EndDisabled();
                // TradeSync's blockers mostly last a round trip, too briefly for a line of their own.
                if (blocked && !view.ownAccepted
                    && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s", view.acceptBlocker.c_str());
                }
                ImGui::SameLine();
                if (Controls::DangerButton("Cancel", ImVec2{ buttonWidth, 0.0F }))
                {
                    Close(Multiplayer::TradeCloseReason::kCancelled);
                }
            }

            TradeSync::View view;
            std::uint64_t revision{ 0 };
            std::uint64_t matchedRevision{ 0 };
            std::vector<std::size_t> matches;
            std::string query;
            int category{ 0 };
            // Ids of the peer's lines the player hovered since the peer last changed them.
            std::vector<std::uint64_t> inspected;
            std::size_t uninspected{ 0 };
            std::size_t popupRow{ 0 };
            bool openPopup{ false };
            bool fieldActive{ false };
            float waited{ 0.0F };
            bool hadFocus{ false };
            bool closing{ false };
            bool focusWindow{ true };
        };
    }

    std::unique_ptr<Window> CreateTradeWindow()
    {
        return std::make_unique<TradeWindow>();
    }
}
