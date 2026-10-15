#include "ui/trade/TradeWindow.h"
#include "sync/trade/TradeSync.h"
#include "text/AsciiCase.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/preview/ItemPreview.h"
#include "ui/render/Backdrop.h"
#include "ui/trade/ItemCard.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        using Phase = TradeSession::Phase;

        constexpr ImVec2 kPanelSizeFonts{ 56.0F, 52.8F };
        constexpr float kPreviewWidthFonts = 28.0F;
        // Narrower than this, the preview column is left out.
        constexpr float kMinimumPreviewWidthFonts = 12.0F;
        constexpr float kMinimumCardHeightFonts = 12.0F;
        constexpr float kCardPaddingFonts = 0.6F;
        constexpr float kPreviewGapFonts = 2.0F;
        constexpr float kScreenMarginFonts = 1.0F;
        constexpr float kCountColumnFonts = 4.0F;
        constexpr float kCountFieldFonts = 7.0F;
        constexpr float kFooterButtonWidthFonts = 6.5F;
        constexpr float kTitleScale = 1.25F;
        // A window kept from focus this long by a game menu gives the trade up.
        constexpr float kFocusWaitSeconds = 2.0F;
        constexpr float kMaximumFrameSeconds = 0.1F;

        constexpr int kGridColumns = 5;
        constexpr int kGridRows = 3;
        // Every line of a full offer is in view, so the grids never scroll.
        static_assert(kGridColumns * kGridRows == Multiplayer::kMaximumTradeItems);
        constexpr float kMinimumCellFonts = 5.0F;
        constexpr float kMaximumCellFonts = 7.0F;
        constexpr float kMinimumInventoryWidthFonts = 20.0F;
        constexpr float kCellPaddingFonts = 0.2F;
        constexpr float kCellGapFonts = 0.25F;
        constexpr float kFallbackNameScale = 0.8F;

        // Item backgrounds are opaque: the thumbnail of each item is captured on its own.
        constexpr ImU32 kWellColor = IM_COL32(8, 8, 10, 255);
        constexpr ImU32 kItemColor = kWellColor;
        constexpr ImU32 kEnchantedColor = IM_COL32(130, 146, 204, 255);
        // A fallback name on the light enchanted background; others take the style's text color.
        constexpr ImU32 kEnchantedTextColor = IM_COL32(20, 20, 24, 255);
        constexpr ImU32 kUninspectedShade = IM_COL32(0, 0, 0, 150);
        constexpr ImU32 kCountShadow = IM_COL32(0, 0, 0, 220);
        constexpr float kCheckingPulsesPerSecond = 0.8F;
        constexpr float kAcceptedTintAlpha = 0.14F;

        // The same background as the item's cell, so both draw the same still.
        constexpr ImU32 ItemBackground(bool a_enchanted)
        {
            return a_enchanted ? kEnchantedColor : kItemColor;
        }

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiWindowFlags kCardFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiWindowFlags kModelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
        constexpr ImGuiTableFlags kListFlags = ImGuiTableFlags_ScrollY;
        constexpr const char* kOfferPopup = "##OfferLine";
        constexpr const char* kPickPopup = "##OfferCount";

        constexpr std::array<Text::Id, 11> kCategoryNames{ Text::Id::kTradeCategoryAll, Text::Id::kTradeCategoryWeapons,
            Text::Id::kTradeCategoryApparel, Text::Id::kTradeCategoryAmmunition, Text::Id::kTradeCategoryPotions,
            Text::Id::kTradeCategoryIngredients, Text::Id::kTradeCategoryBooks, Text::Id::kTradeCategoryScrolls,
            Text::Id::kTradeCategorySoulGems, Text::Id::kTradeCategoryKeys, Text::Id::kTradeCategoryMiscellaneous };

        // Where the panel and the preview column go: the two centered together, the column's
        // model above its card, both as tall as the panel.
        struct Arrangement
        {
            ImVec2 panel;
            ImVec2 model;
            ImVec2 card;
            // Zero when the display leaves no room for the column.
            float previewWidth{ 0.0F };
            float modelSize{ 0.0F };
            // The card's window shrinks to its content up to this.
            float maximumCardHeight{ 0.0F };
        };

        Arrangement Arrange()
        {
            const auto fontSize = ImGui::GetFontSize();
            const auto& display = ImGui::GetIO().DisplaySize;
            const ImVec2 panel{ kPanelSizeFonts.x * fontSize, kPanelSizeFonts.y * fontSize };
            const auto gap = kPreviewGapFonts * fontSize;
            auto previewWidth = std::floor((std::min)(
                kPreviewWidthFonts * fontSize, display.x - 2.0F * kScreenMarginFonts * fontSize - gap - panel.x));
            if (previewWidth < kMinimumPreviewWidthFonts * fontSize)
            {
                previewWidth = 0.0F;
            }
            const auto column = previewWidth > 0.0F ? previewWidth + gap : 0.0F;
            const auto left = std::floor((std::max)((display.x - column - panel.x) * 0.5F, 0.0F));
            const auto top = std::floor((std::max)((display.y - panel.y) * 0.5F, 0.0F));
            const auto columnLeft = left + panel.x + gap;
            Arrangement arrangement{ .panel = ImVec2{ left, top },
                .model = ImVec2{ columnLeft, top },
                .previewWidth = previewWidth };
            arrangement.modelSize = std::floor(
                std::clamp(panel.y - gap - kMinimumCardHeightFonts * fontSize, 0.0F, previewWidth));
            arrangement.card = ImVec2{ columnLeft, top + arrangement.modelSize + gap };
            arrangement.maximumCardHeight = panel.y - arrangement.modelSize - gap;
            return arrangement;
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
            if (ItemPreview::DrawThumbnail(a_drawList, a_item.formID, ItemBackground(a_item.enchanted), a_min, a_max)
                == ItemPreview::Thumbnail::kUnavailable)
            {
                const auto padding = kCellPaddingFonts * fontSize;
                const ImVec4 clip{ a_min.x + padding, a_min.y + padding, a_max.x - padding, a_max.y - padding };
                a_drawList.AddText(nullptr, fontSize * kFallbackNameScale,
                    ImVec2{ clip.x, a_item.count > 1 ? a_min.y + padding + fontSize : clip.y },
                    a_item.enchanted ? kEnchantedTextColor : ImGui::GetColorU32(ImGuiCol_Text), a_item.name.data(), a_item.name.data() + a_item.name.size(), clip.z - clip.x, &clip);
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

        // What the preview column shows: the item last hovered in the item list or a grid.
        struct ShownItem
        {
            enum class Source : std::uint8_t
            {
                kNone,
                kInventory,
                kOffer,
                kPeer
            };

            Source source{ Source::kNone };
            // Into View::inventory, for the player's own items.
            std::size_t row{ 0 };
            std::uint64_t peerLine{ 0 };
            std::uint64_t revision{ 0 };
            RE::FormID formID{ 0 };
            std::string name;
        };

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
                fieldActive = ImGui::IsAnyItemActive();
                const bool escapeClaimed = fieldActive || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
                DrawWindow();
                if (!escapeClaimed && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
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
                shown = {};
            }

            void OnViewChanged()
            {
                if (view.inventoryRevision != matchedRevision)
                {
                    Match();
                }
                if (view.inventoryRevision != shown.revision)
                {
                    FindShownRow();
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

            void DrawWindow()
            {
                const Theme::Scope theme;
                Backdrop::Draw();
                const auto arrangement = Arrange();
                const MenuFrame::WindowLayout layout{
                    .sizeFonts = kPanelSizeFonts, .paddingFonts = 1.0F, .fixedPosition = arrangement.panel
                };
                if (MenuFrame::BeginWindow("Trade###Trade", layout, kWindowFlags, std::exchange(focusWindow, false)))
                {
                    DrawTitle();
                    const auto& style = ImGui::GetStyle();
                    const auto available = ImGui::GetContentRegionAvail();
                    const auto height = available.y - ImGui::GetFrameHeight() - style.ItemSpacing.y;
                    const auto cell = GetCellSize(available.x, height);
                    const auto gridWidth = cell * static_cast<float>(kGridColumns) - GetCellGap();
                    DrawInventory(ImVec2{ available.x - gridWidth - style.ItemSpacing.x, height });
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    DrawPeerOffer(cell);
                    DrawOwnOffer(cell);
                    ImGui::EndGroup();
                    DrawOfferPopup();
                    DrawPickPopup();
                    DrawFooter();
                }
                ImGui::End();
                // After the panel, so the item hovered in it this frame is the one shown.
                if (arrangement.previewWidth > 0.0F)
                {
                    DrawPreview(arrangement);
                }
            }

            // What the grids' area holds besides their cells: the inventory list beside the grids,
            // and both grids' headings.
            [[nodiscard]] static ImVec2 GetGridsOverhead()
            {
                const auto& style = ImGui::GetStyle();
                return ImVec2{ kMinimumInventoryWidthFonts * ImGui::GetFontSize() + style.ItemSpacing.x,
                    2.0F * (ImGui::GetFrameHeight() + style.ItemSpacing.y) + 2.0F * style.ItemSpacing.y };
            }

            // The largest cell up to kMaximumCellFonts that fits both grids' rows in the
            // height and leaves the inventory list its width.
            [[nodiscard]] static float GetCellSize(float a_width, float a_height)
            {
                const auto fontSize = ImGui::GetFontSize();
                const auto overhead = GetGridsOverhead();
                const auto byHeight = (a_height - overhead.y) / static_cast<float>(kGridRows * 2);
                const auto byWidth = (a_width - overhead.x) / static_cast<float>(kGridColumns);
                return std::floor(std::clamp(
                    (std::min)(byHeight, byWidth), kMinimumCellFonts * fontSize, kMaximumCellFonts * fontSize));
            }

            // Taken out of each cell's pitch, so the grids keep their size and the cell its spacing.
            [[nodiscard]] static float GetCellGap()
            {
                return (std::max)(std::round(kCellGapFonts * ImGui::GetFontSize()), 1.0F);
            }

            void DrawPreview(const Arrangement& a_arrangement)
            {
                const auto card = GetShownCard();
                const auto fontSize = ImGui::GetFontSize();
                if (a_arrangement.modelSize > 0.0F)
                {
                    ImGui::SetNextWindowPos(a_arrangement.model, ImGuiCond_Always);
                    ImGui::SetNextWindowSize(
                        ImVec2{ a_arrangement.previewWidth, a_arrangement.modelSize }, ImGuiCond_Always);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0.0F, 0.0F });
                    if (ImGui::Begin("###TradeModel", nullptr, kModelFlags) && card)
                    {
                        ItemPreview::DrawModel(
                            card->formID, ImVec2{ a_arrangement.previewWidth, a_arrangement.modelSize });
                    }
                    ImGui::End();
                    ImGui::PopStyleVar();
                }
                if (!card)
                {
                    return;
                }
                // The card's top stays under the model and its bottom follows the content, as the
                // inventory menu's card does, up to the panel's bottom where it scrolls.
                const auto frame = MenuFrame::GetMetrics();
                const auto padding = frame.inset + kCardPaddingFonts * fontSize;
                // The first and last lines clear the corner knots, which the sides need not.
                const auto knotClearance = std::ceil((std::max)(frame.corner - padding, 0.0F));
                const auto inset = padding + knotClearance;
                const auto contentWidth = a_arrangement.previewWidth - 2.0F * padding;
                const auto contentHeight = MeasureItemCard(*card, contentWidth);
                const bool scrolls = contentHeight + 2.0F * inset > a_arrangement.maximumCardHeight;
                const auto cardHeight = scrolls ? a_arrangement.maximumCardHeight : contentHeight + 2.0F * inset;
                const MenuFrame::WindowLayout layout{
                    .sizeFonts = ImVec2{ a_arrangement.previewWidth / fontSize, cardHeight / fontSize },
                    .paddingFonts = kCardPaddingFonts,
                    .fixedPosition = a_arrangement.card };
                if (MenuFrame::BeginWindow("###TradeCard", layout, kCardFlags, false))
                {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + knotClearance);
                    // A window clips its content only half its padding from its edge, so a long
                    // card scrolls in a child clipped to the content area instead.
                    const auto visibleHeight = scrolls ? cardHeight - 2.0F * inset : contentHeight;
                    const auto childFlags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav
                        | (scrolls ? ImGuiWindowFlags_None
                                   : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    if (ImGui::BeginChild("##content", ImVec2{ contentWidth, visibleHeight }, ImGuiChildFlags_None,
                            childFlags))
                    {
                        DrawItemCard(*card, scrolls ? contentWidth - ImGui::GetStyle().ScrollbarSize : contentWidth);
                    }
                    ImGui::EndChild();
                }
                ImGui::End();
            }

            // The hovered item becomes the shown one and stays so until another is hovered.
            void Show(ShownItem::Source a_source, std::size_t a_row, std::uint64_t a_peerLine)
            {
                if (shown.source == a_source && shown.row == a_row && shown.peerLine == a_peerLine
                    && shown.revision == view.inventoryRevision)
                {
                    return;
                }
                shown.source = a_source;
                shown.row = a_row;
                shown.peerLine = a_peerLine;
                shown.revision = view.inventoryRevision;
                if (a_source != ShownItem::Source::kPeer && a_row < view.inventory.size())
                {
                    shown.formID = view.inventory[a_row].formID;
                    shown.name = view.inventory[a_row].name;
                }
            }

            // Rows may move when the inventory changes, so a shown item of the player's is found
            // again by form and name, or no longer shown.
            void FindShownRow()
            {
                shown.revision = view.inventoryRevision;
                if (shown.source != ShownItem::Source::kInventory && shown.source != ShownItem::Source::kOffer)
                {
                    return;
                }
                const auto line = std::ranges::find_if(view.inventory, [this](const TradeSync::InventoryLine& a_line) {
                    return a_line.formID == shown.formID && a_line.name == shown.name;
                });
                if (line == view.inventory.end())
                {
                    shown.source = ShownItem::Source::kNone;
                    return;
                }
                shown.row = static_cast<std::size_t>(line - view.inventory.begin());
            }

            [[nodiscard]] std::optional<ItemCard> GetShownCard() const
            {
                switch (shown.source)
                {
                case ShownItem::Source::kNone:
                    return std::nullopt;
                case ShownItem::Source::kPeer:
                    {
                        const auto line = std::ranges::find(view.peerOffer, shown.peerLine, &TradeSync::PeerLine::id);
                        if (line == view.peerOffer.end())
                        {
                            return std::nullopt;
                        }
                        return ItemCard{
                            .name = line->name, .count = line->count, .details = &line->details, .formID = line->formID
                        };
                    }
                case ShownItem::Source::kInventory:
                case ShownItem::Source::kOffer:
                    break;
                }
                if (shown.row >= view.inventory.size())
                {
                    return std::nullopt;
                }
                const auto& line = view.inventory[shown.row];
                // An offered stack shows its offered count; once removed, what the player holds.
                const auto offered = shown.source == ShownItem::Source::kOffer ? OfferedCount(shown.row) : 0;
                return ItemCard{ .name = line.name,
                    .count = offered > 0 ? offered : line.count,
                    .details = &line.details,
                    .blocked = offered > 0 ? std::string_view{} : std::string_view{ line.blocked },
                    .formID = line.formID };
            }

            void DrawTitle() const
            {
                const auto& style = ImGui::GetStyle();
                ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kTitleScale);
                const std::array<std::string_view, 1> name{ view.peer.name };
                Controls::AccentedText(Text::Get(Text::Id::kTradeTitle), name);
                ImGui::PopFont();
                ImGui::Separator();
            }

            static void DrawGridHeading(const char* a_label, bool a_accepted, bool a_full = false)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(a_label);
                if (a_full)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(Theme::kWarning, "%s", Text::GetCString(Text::Id::kTradeOfferFull));
                }
                if (a_accepted)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(Theme::kPositive, "%s", Text::GetCString(Text::Id::kTradeAccepted));
                }
            }

            // Draws the grid's wells, then each line's cell through a_drawCell(index, min, max,
            // hovered) over an invisible button the callback may query.
            template <class DrawCell>
            static void DrawGrid(
                const char* a_id, std::size_t a_lines, float a_cell, bool a_accepted, DrawCell&& a_drawCell)
            {
                const auto columns = static_cast<std::size_t>(kGridColumns);
                auto& drawList = *ImGui::GetWindowDrawList();
                const auto origin = ImGui::GetCursorScreenPos();
                const auto gap = GetCellGap();
                const auto well = a_cell - gap;
                const auto cellAt = [&](std::size_t a_index) {
                    const ImVec2 min{ origin.x + static_cast<float>(a_index % columns) * a_cell,
                        origin.y + static_cast<float>(a_index / columns) * a_cell };
                    return std::pair{ min, ImVec2{ min.x + well, min.y + well } };
                };
                const ImVec2 size{ a_cell * static_cast<float>(kGridColumns) - gap,
                    a_cell * static_cast<float>(kGridRows) - gap };
                for (std::size_t index = 0; index < Multiplayer::kMaximumTradeItems; ++index)
                {
                    const auto [min, max] = cellAt(index);
                    drawList.AddRectFilled(min, max, kWellColor);
                }
                ImGui::PushID(a_id);
                for (std::size_t index = 0; index < a_lines; ++index)
                {
                    const auto [min, max] = cellAt(index);
                    ImGui::SetCursorScreenPos(min);
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::InvisibleButton("##cell", ImVec2{ well, well },
                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                    const bool hovered = ImGui::IsItemHovered();
                    a_drawCell(index, min, max, hovered);
                    if (hovered)
                    {
                        drawList.AddRect(min, max, ImGui::GetColorU32(Theme::GetAccent()), 0.0F, 0, 2.0F);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
                ImGui::SetCursorScreenPos(origin);
                ImGui::Dummy(size);
                if (a_accepted)
                {
                    // Path of Exile tints the side of a player who accepted.
                    const ImVec2 tintMax{ origin.x + size.x, origin.y + size.y };
                    auto tint = Theme::kPositive;
                    tint.w = kAcceptedTintAlpha;
                    drawList.AddRectFilled(origin, tintMax, ImGui::GetColorU32(tint));
                    drawList.AddRect(origin, tintMax, ImGui::GetColorU32(Theme::kPositive), 0.0F, 0, 2.0F);
                }
            }

            void DrawPeerOffer(float a_cell)
            {
                Text::FormatTo(heading, Text::Id::kTradePeerOffer, view.peer.name);
                DrawGridHeading(heading.c_str(), view.peerAccepted);
                // Counted before the grid as well, so the hint shows on the window's first frame.
                CountUninspected();
                if (uninspected > 0)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "%s", Text::FormatPlural(Text::Id::kTradeUninspected, uninspected).c_str());
                }
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
                            Show(ShownItem::Source::kPeer, 0, line.id);
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
                CountUninspected();
            }

            void CountUninspected()
            {
                uninspected = static_cast<std::size_t>(
                    std::ranges::count_if(view.peerOffer, [this](const TradeSync::PeerLine& a_line) {
                        return std::ranges::find(inspected, a_line.id) == inspected.end();
                    }));
            }

            void DrawOwnOffer(float a_cell)
            {
                DrawGridHeading(Text::GetCString(Text::Id::kTradeOwnOffer), view.ownAccepted,
                    view.offer.size() >= Multiplayer::kMaximumTradeItems);
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
                            ClickOffered(line.row, line.count);
                        }
                        if (a_hovered)
                        {
                            Show(ShownItem::Source::kOffer, line.row, 0);
                        }
                    });
            }

            void ClickOffered(std::size_t a_row, std::uint32_t a_offered)
            {
                if (a_offered > 1)
                {
                    popupRow = a_row;
                    openPopup = true;
                }
                else
                {
                    TradeSync::SetOfferCount(view.inventoryRevision, a_row, 0);
                }
            }

            // A clicked line of the own offer: its count and Remove all. The count field stays conditional because
            // the held count can drop to one while the popup is open.
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
                    // Since ImGui 1.92.9 typed text applies only on Enter or focus loss, so a 0 is never a digit typed
                    // on the way to another count.
                    if (ImGui::InputScalar("##count", ImGuiDataType_U32, &count, &step))
                    {
                        TradeSync::SetOfferCount(view.inventoryRevision, popupRow, (std::min)(count, item.count));
                        if (count == 0)
                        {
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", Text::Format(Text::Id::kTradeCountOf, item.count).c_str());
                }
                if (Controls::DangerButton(Text::GetCString(Text::Id::kTradeRemoveAll), ImVec2{ -FLT_MIN, 0.0F }))
                {
                    TradeSync::SetOfferCount(view.inventoryRevision, popupRow, 0);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // A clicked stack of the item list that is not offered yet: how many to offer, sent only by Offer or Enter.
            void DrawPickPopup()
            {
                if (std::exchange(openPick, false))
                {
                    ImGui::OpenPopup(kPickPopup);
                }
                if (!ImGui::BeginPopup(kPickPopup))
                {
                    return;
                }
                // The field is focused, so Escape cancels at once rather than only leaving the field.
                if (pickRevision != view.inventoryRevision || pickRow >= view.inventory.size()
                    || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return;
                }
                const auto& item = view.inventory[pickRow];
                ImGui::TextUnformatted(item.name.c_str());
                const std::uint32_t step = 1;
                ImGui::SetNextItemWidth(kCountFieldFonts * ImGui::GetFontSize());
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();
                }
                if (ImGui::InputScalar("##count", ImGuiDataType_U32, &pickCount, &step))
                {
                    pickCount = std::clamp(pickCount, 1U, item.count);
                }
                // Enter leaves the field in the frame it applies the typed text, so the count is current here.
                const bool entered =
                    ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", Text::Format(Text::Id::kTradeCountOf, item.count).c_str());
                if (Controls::PrimaryButton(Text::GetCString(Text::Id::kTradeOffer), ImVec2{ -FLT_MIN, 0.0F }) || entered)
                {
                    TradeSync::SetOfferCount(pickRevision, pickRow, pickCount);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            void DrawInventory(const ImVec2& a_size)
            {
                ImGui::BeginGroup();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Text::GetCString(Text::Id::kTradeInventory));
                if (view.checkingPlugins)
                {
                    ImGui::SameLine();
                    const auto angle = static_cast<float>(ImGui::GetTime()) * kCheckingPulsesPerSecond * 2.0F
                        * std::numbers::pi_v<float>;
                    auto color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    color.w *= 0.65F + 0.35F * std::sin(angle);
                    ImGui::TextColored(color, "%s", Text::GetCString(Text::Id::kTradeCheckingMods));
                }
                const auto& style = ImGui::GetStyle();
                const auto comboWidth = std::floor(a_size.x * 0.4F);
                ImGui::SetNextItemWidth(a_size.x - comboWidth - style.ItemSpacing.x);
                bool changed = ImGui::InputTextWithHint("##search", Text::GetCString(Text::Id::kSearchHint), &query);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(comboWidth);
                std::array<const char*, kCategoryNames.size()> categoryNames{};
                std::ranges::transform(kCategoryNames, categoryNames.begin(), &Text::GetCString);
                changed = ImGui::Combo(
                              "##category", &category, categoryNames.data(), static_cast<int>(categoryNames.size()))
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
                        ImGui::TextDisabled("%s",
                            Text::GetCString(view.inventory.empty() ? Text::Id::kTradeNothingToTrade
                                                                    : Text::Id::kItemsNoMatch));
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
                const bool full = offered == 0 && view.offer.size() >= Multiplayer::kMaximumTradeItems;
                ImGui::BeginDisabled(blocked || full || line.checking);
                if (ImGui::Selectable(line.name.c_str(), offered > 0,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    if (offered > 0)
                    {
                        ClickOffered(a_row, offered);
                    }
                    else if (line.count > 1)
                    {
                        pickRow = a_row;
                        pickRevision = view.inventoryRevision;
                        pickCount = 1;
                        openPick = true;
                    }
                    else
                    {
                        TradeSync::SetOfferCount(view.inventoryRevision, a_row, 1);
                    }
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    Show(ShownItem::Source::kInventory, a_row, 0);
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
                const auto* const accept = Text::GetCString(Text::Id::kTradeAccept);
                const auto* const cancel = Text::GetCString(Text::Id::kTradeCancel);
                const auto buttonWidth = (std::max)(kFooterButtonWidthFonts * fontSize,
                    std::ceil((std::max)(ImGui::CalcTextSize(accept).x, ImGui::CalcTextSize(cancel).x)
                        + style.FramePadding.x * 2.0F));
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttonWidth * 2.0F - style.ItemSpacing.x);
                const bool blocked = !view.acceptBlocker.empty();
                ImGui::BeginDisabled(view.ownAccepted || blocked || uninspected > 0);
                if (Controls::PrimaryButton(accept, ImVec2{ buttonWidth, 0.0F }))
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
                if (Controls::DangerButton(cancel, ImVec2{ buttonWidth, 0.0F }))
                {
                    Close(Multiplayer::TradeCloseReason::kCancelled);
                }
            }

            TradeSync::View view;
            std::uint64_t revision{ 0 };
            std::uint64_t matchedRevision{ 0 };
            std::vector<std::size_t> matches;
            std::string query;
            // Reused so the heading is formatted without allocating.
            std::string heading;
            int category{ 0 };
            // Ids of the peer's lines the player hovered since the peer last changed them.
            std::vector<std::uint64_t> inspected;
            std::size_t uninspected{ 0 };
            ShownItem shown;
            std::size_t popupRow{ 0 };
            bool openPopup{ false };
            std::size_t pickRow{ 0 };
            std::uint64_t pickRevision{ 0 };
            std::uint32_t pickCount{ 1 };
            bool openPick{ false };
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
