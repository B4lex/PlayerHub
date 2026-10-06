#include "ui/trade/TradeWindow.h"
#include "config/PluginConfig.h"
#include "sync/trade/TradeSync.h"
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
#include <cctype>
#include <cfloat>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        using Phase = TradeSession::Phase;

        constexpr ImVec2 kSizeFonts{ 62.0F, 32.0F };
        constexpr ImVec2 kMinimumSizeFonts{ 48.0F, 22.0F };
        constexpr float kBackgroundAlpha = 0.96F;
        constexpr float kCountColumnFonts = 4.0F;
        constexpr float kCountFieldFonts = 6.5F;
        constexpr float kPreviewFonts = 12.0F;
        constexpr float kTooltipWidthFonts = 22.0F;
        constexpr float kFooterButtonWidthFonts = 6.5F;
        constexpr float kTitleScale = 1.25F;
        // A window kept from focus this long by a game menu gives the trade up.
        constexpr float kFocusWaitSeconds = 2.0F;
        constexpr float kMaximumFrameSeconds = 0.1F;

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;

        constexpr std::array<const char*, 11> kCategoryNames{ "All", "Weapons", "Apparel", "Ammunition", "Potions",
            "Ingredients", "Books", "Scrolls", "Soul gems", "Keys", "Miscellaneous" };

        bool ContainsIgnoringCase(std::string_view a_text, std::string_view a_query)
        {
            return a_query.empty()
                || std::ranges::search(a_text, a_query, [](char a_left, char a_right) {
                       return std::tolower(static_cast<unsigned char>(a_left))
                           == std::tolower(static_cast<unsigned char>(a_right));
                   }).begin() != a_text.end();
        }

        class TradeWindow final : public Window
        {
        public:
            [[nodiscard]] bool UsesMouse() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                if (TradeSync::CopyView(revision, view) && view.inventoryRevision != matchedRevision)
                {
                    Match();
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
                const bool escapeClaimed
                    = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
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
                        && ContainsIgnoringCase(line.name, query))
                    {
                        matches.push_back(row);
                    }
                }
            }

            void DrawWindow(bool& a_open)
            {
                const Theme::Scope theme;
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                ImGui::SetNextWindowPos(
                    ImVec2{ display.x * 0.5F, display.y * 0.5F }, ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
                ImGui::SetNextWindowSize(
                    ImVec2{ kSizeFonts.x * fontSize, kSizeFonts.y * fontSize }, ImGuiCond_Appearing);
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2{ kMinimumSizeFonts.x * fontSize, kMinimumSizeFonts.y * fontSize }, display);
                if (std::exchange(focusWindow, false))
                {
                    ImGui::SetNextWindowFocus();
                }
                const auto frame = MenuFrame::GetMetrics();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ frame.inset + fontSize, frame.inset + fontSize });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, frame.rounding);
                ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, IM_COL32_BLACK_TRANS);
                ImGui::PushStyleColor(ImGuiCol_SeparatorActive, IM_COL32_BLACK_TRANS);
                const bool visible = ImGui::Begin("Trade###Trade", nullptr, kWindowFlags);
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
                if (visible)
                {
                    MenuFrame::Draw(ImGui::GetColorU32(Theme::GetFrameColor()),
                        ImGui::GetColorU32(ImGuiCol_WindowBg, kBackgroundAlpha));
                    hovered = false;
                    DrawTitle(a_open);
                    const auto& style = ImGui::GetStyle();
                    const auto footerHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y;
                    const auto height = ImGui::GetContentRegionAvail().y - footerHeight;
                    const auto width = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 2.0F) / 3.0F);
                    DrawInventory(ImVec2{ width, height });
                    ImGui::SameLine();
                    DrawOwnOffer(ImVec2{ width, height });
                    ImGui::SameLine();
                    DrawPeerOffer(ImVec2{ width, height });
                    DrawFooter();
                    DrawTooltip();
                }
                ImGui::End();
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

            void DrawInventory(const ImVec2& a_size)
            {
                ImGui::BeginGroup();
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
                    TradeSync::SetOfferCount(view.inventoryRevision, a_row, (std::min)(offered + 1, line.count));
                }
                ImGui::EndDisabled();
                if (Hovered(line.formID))
                {
                    hoveredText = blocked ? line.blocked : Describe(line);
                }
                ImGui::TableNextColumn();
                if (line.count > 1)
                {
                    ImGui::TextDisabled("%u", line.count);
                }
                ImGui::PopID();
            }

            static std::string Describe(const TradeSync::InventoryLine& a_line)
            {
                std::string text;
                const auto add = [&text](std::string_view a_part) {
                    text += text.empty() ? "" : " \xC2\xB7 ";
                    text += a_part;
                };
                if (a_line.enchanted)
                {
                    add("Enchanted");
                }
                if (a_line.equipped)
                {
                    add("Equipped; giving it takes it off");
                }
                if (a_line.favorited)
                {
                    add("Favorite");
                }
                return text;
            }

            void DrawOwnOffer(const ImVec2& a_size)
            {
                ImGui::BeginGroup();
                ImGui::TextUnformatted("Your offer");
                const ImVec2 listSize{ a_size.x, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight()
                        - ImGui::GetStyle().ItemSpacing.y };
                if (ImGui::BeginTable("##own", 3, kListFlags, listSize))
                {
                    const auto fontSize = ImGui::GetFontSize();
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, kCountFieldFonts * fontSize);
                    ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                    for (const auto& line : view.offer)
                    {
                        DrawOwnLine(line);
                    }
                    if (view.offer.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushTextWrapPos(0.0F);
                        ImGui::TextDisabled("Nothing yet. Click one of your items to offer all of it, or right-click "
                                            "it to offer one more.");
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndGroup();
            }

            void DrawOwnLine(const TradeSync::OfferLine& a_line)
            {
                if (a_line.row >= view.inventory.size())
                {
                    return;
                }
                const auto& item = view.inventory[a_line.row];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(a_line.row));
                const bool skipped = !a_line.skipped.empty();
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, skipped ? Theme::kWarning : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ImGui::TextUnformatted(item.name.c_str());
                ImGui::PopStyleColor();
                if (Hovered(item.formID))
                {
                    hoveredText = skipped ? a_line.skipped + " It stays with you." : Describe(item);
                }
                ImGui::TableNextColumn();
                if (item.count > 1)
                {
                    std::uint32_t count = a_line.count;
                    const std::uint32_t step = 1;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputScalar("##count", ImGuiDataType_U32, &count, &step))
                    {
                        TradeSync::SetOfferCount(view.inventoryRevision, a_line.row, std::clamp(count, 1U, item.count));
                    }
                }
                ImGui::TableNextColumn();
                if (Controls::IconButton("##remove", Controls::Icon::kClose))
                {
                    TradeSync::SetOfferCount(view.inventoryRevision, a_line.row, 0);
                }
                ImGui::PopID();
            }

            void DrawPeerOffer(const ImVec2& a_size)
            {
                ImGui::BeginGroup();
                ImGui::Text("%s's offer", view.peer.name.c_str());
                const ImVec2 listSize{ a_size.x, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight()
                        - ImGui::GetStyle().ItemSpacing.y };
                if (ImGui::BeginTable("##peer", 2, kListFlags, listSize))
                {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, kCountColumnFonts * ImGui::GetFontSize());
                    for (std::size_t i = 0; i < view.peerOffer.size(); ++i)
                    {
                        const auto& line = view.peerOffer[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(i));
                        const bool skipped = !line.skipped.empty();
                        ImGui::PushStyleColor(
                            ImGuiCol_Text, skipped ? Theme::kWarning : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                        ImGui::TextUnformatted(line.name.c_str());
                        ImGui::PopStyleColor();
                        if (Hovered(line.formID))
                        {
                            hoveredText = skipped ? line.skipped + " It stays with " + view.peer.name + "."
                                                  : (line.enchanted ? std::string{ "Enchanted" } : std::string{});
                        }
                        ImGui::TableNextColumn();
                        if (line.count > 1)
                        {
                            ImGui::TextDisabled("%u", line.count);
                        }
                        ImGui::PopID();
                    }
                    if (view.peerOffer.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Nothing yet.");
                    }
                    ImGui::EndTable();
                }
                ImGui::EndGroup();
            }

            void DrawFooter()
            {
                const auto& style = ImGui::GetStyle();
                const auto fontSize = ImGui::GetFontSize();
                const auto buttonWidth = kFooterButtonWidthFonts * fontSize;
                ImGui::AlignTextToFramePadding();
                if (view.peerAccepted)
                {
                    ImGui::TextColored(Theme::kPositive, "%s accepted.", view.peer.name.c_str());
                    ImGui::SameLine();
                }
                if (view.ownAccepted)
                {
                    ImGui::TextColored(Theme::kPositive, "You accepted; waiting for %s.", view.peer.name.c_str());
                }
                else if (!view.acceptBlocker.empty())
                {
                    ImGui::TextDisabled("%s", view.acceptBlocker.c_str());
                }
                else
                {
                    ImGui::TextDisabled("Changing either offer takes both acceptances back.");
                }
                ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2.0F - style.ItemSpacing.x);
                ImGui::BeginDisabled(view.ownAccepted || !view.acceptBlocker.empty());
                if (Controls::PrimaryButton("Accept", ImVec2{ buttonWidth, 0.0F }))
                {
                    TradeSync::Accept();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (Controls::DangerButton("Cancel", ImVec2{ buttonWidth, 0.0F }))
                {
                    Close(Multiplayer::TradeCloseReason::kCancelled);
                }
            }

            // Notes the item under the cursor, whose text the caller sets; DrawTooltip shows
            // both after the lists.
            bool Hovered(std::uint32_t a_formID)
            {
                if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    return false;
                }
                hoveredForm = a_formID;
                hovered = true;
                return true;
            }

            void DrawTooltip() const
            {
                const bool preview = hoveredForm != 0 && Config::Get()->ui.itemPreview;
                if (!hovered || (!preview && hoveredText.empty()) || !ImGui::BeginTooltip())
                {
                    return;
                }
                const auto fontSize = ImGui::GetFontSize();
                if (preview)
                {
                    ItemPreview::Draw(hoveredForm, ImVec2{ kPreviewFonts * fontSize, kPreviewFonts * fontSize });
                }
                if (!hoveredText.empty())
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (preview ? kPreviewFonts : kTooltipWidthFonts) * fontSize);
                    ImGui::TextUnformatted(hoveredText.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndTooltip();
            }

            TradeSync::View view;
            std::uint64_t revision{ 0 };
            std::uint64_t matchedRevision{ 0 };
            std::vector<std::size_t> matches;
            std::string query;
            int category{ 0 };
            std::uint32_t hoveredForm{ 0 };
            std::string hoveredText;
            bool hovered{ false };
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
