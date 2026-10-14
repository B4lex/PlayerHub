#include "ui/chat/ChatInput.h"
#include "config/PluginConfig.h"
#include "sync/chat/ChatRoster.h"
#include "ui/chat/ChatStyle.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>

namespace UI
{
    namespace
    {
        constexpr std::size_t kMaximumMenuPartners = 5;
        constexpr float kMaximumChipTextFonts = 12.0F;

        struct RowOutcome
        {
            bool clicked{ false };
            bool hinted{ false };
        };

        // Dear ImGui would parse "##" in a player's name.
        RowOutcome DrawRow(std::string_view a_text, const ImVec4& a_color, bool a_selected)
        {
            const auto start = ImGui::GetCursorPosX();
            RowOutcome outcome;
            outcome.clicked = ImGui::Selectable("##ChatRow", a_selected, ImGuiSelectableFlags_AllowOverlap);
            outcome.hinted = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            ImGui::SameLine(start);
            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            ImGui::TextUnformatted(a_text.data(), a_text.data() + a_text.size());
            ImGui::PopStyleColor();
            return outcome;
        }

        std::string DescribeTarget(const ChatSync::ChatTarget& a_target)
        {
            switch (a_target.channel)
            {
                case Multiplayer::ChatChannel::kNearby:
                    return "Nearby";
                case Multiplayer::ChatChannel::kGlobal:
                    return "Global";
                case Multiplayer::ChatChannel::kWhisper:
                    break;
            }
            return std::format("To {}", a_target.player.name);
        }
    }

    void ChatInput::OnOpened()
    {
        draftWhenOpened = draft;
        composer.StopRecalling();
        composer.ClearNotes();
    }

    int ChatInput::OnFieldEvent(ImGuiInputTextCallbackData* a_data)
    {
        // Escape puts back the text the field had when it took the keyboard, before this runs.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            return 0;
        }
        auto& self = *static_cast<ChatInput*>(a_data->UserData);
        auto& composer = self.composer;
        const std::string_view typed{ a_data->Buf, static_cast<std::size_t>(a_data->BufTextLen) };
        bool rewritten = false;
        if (typed != self.draft)
        {
            self.draft.assign(typed);
            rewritten = composer.OnEdited(self.draft);
        }

        const bool listed = composer.IsListing();
        if (a_data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
        {
            if (!listed)
            {
                composer.CycleTarget(1);
            }
            else if (!composer.GetSuggestions().empty())
            {
                composer.AcceptSuggestion(composer.GetSelection(), self.draft);
                rewritten = true;
            }
        }
        else if (a_data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
        {
            const bool up = a_data->EventKey == ImGuiKey_UpArrow;
            if (listed)
            {
                composer.MoveSelection(up ? -1 : 1);
            }
            else if (up ? composer.RecallPrevious(self.draft) : composer.RecallNext(self.draft))
            {
                rewritten = true;
            }
        }

        if (rewritten)
        {
            a_data->DeleteChars(0, a_data->BufTextLen);
            a_data->InsertChars(0, self.draft.c_str(), self.draft.c_str() + self.draft.size());
        }
        // Taking the keyboard selects the whole text, which the next key
        // would then replace.
        if (rewritten || a_data->EventActivated)
        {
            a_data->CursorPos = a_data->BufTextLen;
            a_data->ClearSelection();
        }
        return 0;
    }

    void ChatInput::DrawChip(bool a_closeMenu)
    {
        const auto& target = composer.GetTarget();
        if (!(target == chipTarget) || target.player.name != chipTarget.player.name)
        {
            chipTarget = target;
            chipLabel = DescribeTarget(target);
        }

        const auto& style = ImGui::GetStyle();
        const auto fontSize = ImGui::GetFontSize();
        const auto padding = style.FramePadding.x;
        const auto caret = std::floor(fontSize * 0.3F);
        const auto textWidth = (std::min)(
            ImGui::CalcTextSize(chipLabel.c_str(), chipLabel.c_str() + chipLabel.size()).x, kMaximumChipTextFonts * fontSize);
        const ImVec2 size{ padding + textWidth + padding + caret * 2.0F + padding, ImGui::GetFrameHeight() };
        if (ImGui::InvisibleButton("##ChatTarget", size))
        {
            ImGui::OpenPopup("##ChatTargets");
        }

        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        const auto& color = ChatStyle::GetChannelColor(target.channel);
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max,
            ImGui::GetColorU32(ChatStyle::WithAlpha(
                color, ImGui::IsItemHovered() ? ChatStyle::kHoveredChipFillAlpha : ChatStyle::kChipFillAlpha)),
            style.FrameRounding);
        const ImVec2 textMin{ min.x + padding, min.y + style.FramePadding.y };
        drawList->PushClipRect(textMin, ImVec2{ textMin.x + textWidth, max.y }, true);
        drawList->AddText(textMin, ImGui::GetColorU32(color), chipLabel.c_str(), chipLabel.c_str() + chipLabel.size());
        drawList->PopClipRect();
        const ImVec2 center{ max.x - padding - caret, std::floor((min.y + max.y) * 0.5F) };
        drawList->AddTriangleFilled(ImVec2{ center.x - caret, center.y - caret * 0.5F },
            ImVec2{ center.x + caret, center.y - caret * 0.5F }, ImVec2{ center.x, center.y + caret * 0.5F },
            ImGui::GetColorU32(color));

        ImGui::SetNextWindowPos(ImVec2{ min.x, min.y - style.ItemSpacing.y }, ImGuiCond_Always, ImVec2{ 0.0F, 1.0F });
        menuOpen = ImGui::BeginPopup("##ChatTargets");
        if (!menuOpen)
        {
            return;
        }
        if (a_closeMenu)
        {
            ImGui::CloseCurrentPopup();
        }
        const auto pick = [&](const ChatSync::ChatTarget& a_target, std::string_view a_label, int a_id) {
            ImGui::PushID(a_id);
            const auto row = DrawRow(a_label, ChatStyle::GetChannelColor(a_target.channel), a_target == target);
            if (row.hinted)
            {
                switch (a_target.channel)
                {
                    case Multiplayer::ChatChannel::kGlobal:
                        ImGui::SetTooltip("Send to every player on the server.");
                        break;
                    case Multiplayer::ChatChannel::kNearby:
                        ImGui::SetTooltip("Send to players near you.");
                        break;
                    case Multiplayer::ChatChannel::kWhisper:
                        ImGui::SetTooltip("Send wh to %s.", a_target.player.name.c_str());
                        break;
                }
            }
            if (row.clicked)
            {
                composer.SetTarget(a_target);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        };
        pick(ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kGlobal }, "Global", 0);
        pick(ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kNearby }, "Nearby", 1);
        partners.clear();
        ChatRoster::GetRecentPartners(partners, kMaximumMenuPartners);
        if (!partners.empty())
        {
            ImGui::Separator();
        }
        for (std::size_t index = 0; index < partners.size(); ++index)
        {
            pick(ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kWhisper, .player = partners[index] },
                partners[index].name, static_cast<int>(index) + 2);
        }
        ImGui::EndPopup();
    }

    void ChatInput::DrawListing()
    {
        if (!composer.IsListing())
        {
            DrawNotes();
            return;
        }
        const auto suggestions = composer.GetSuggestions();
        const auto& style = ImGui::GetStyle();
        const auto rows = static_cast<float>((std::max)(suggestions.size(), std::size_t{ 1 }));
        const ImVec2 size{ ImGui::GetContentRegionAvail().x,
            rows * ImGui::GetTextLineHeightWithSpacing() + style.WindowPadding.y * 2.0F - style.ItemSpacing.y };

        const auto row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2{ row.x, row.y - style.ItemSpacing.y - size.y });
        // As a popup of the theme, which is opaque: the lines must not show through.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
        std::size_t accepted = suggestions.size();
        if (ImGui::BeginChild("##ChatListing", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            if (suggestions.empty())
            {
                DrawRow("No player with that name is online", ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), false);
            }
            for (std::size_t index = 0; index < suggestions.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                if (DrawRow(suggestions[index].name, ChatStyle::kWhisperColor, index == composer.GetSelection())
                        .clicked)
                {
                    accepted = index;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(row);

        if (accepted < suggestions.size())
        {
            composer.AcceptSuggestion(accepted, draft);
        }
    }

    void ChatInput::DrawNotes() const
    {
        const auto notes = composer.GetNotes();
        if (notes.empty())
        {
            return;
        }
        const auto& style = ImGui::GetStyle();
        const auto width = ImGui::GetContentRegionAvail().x;
        const auto wrapWidth = width - style.WindowPadding.x * 2.0F;
        float height = style.WindowPadding.y * 2.0F - style.ItemSpacing.y;
        for (const auto& note : notes)
        {
            height += ImGui::CalcTextSize(note.c_str(), note.c_str() + note.size(), false, wrapWidth).y
                + style.ItemSpacing.y;
        }

        const auto row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2{ row.x, row.y - style.ItemSpacing.y - height });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
        if (ImGui::BeginChild("##ChatNotes", ImVec2{ width, height },
                ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ChatStyle::kFailureColor);
            ImGui::PushTextWrapPos(0.0F);
            for (const auto& note : notes)
            {
                ImGui::TextUnformatted(note.c_str(), note.c_str() + note.size());
            }
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(row);
    }

    ChatInput::Outcome ChatInput::Draw(const Frame& a_frame)
    {
        DrawChip(a_frame.closeMenu);
        ImGui::SameLine();

        const bool listed = composer.IsListing();
        if (fieldActive)
        {
            // Keys the field does not report.
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && ImGui::GetIO().KeyShift && !listed)
            {
                composer.CycleTarget(-1);
            }
        }
        else
        {
            // Leaving the field, as a click elsewhere or Escape does, writes its own text back.
            const auto length = (std::min)(draft.size(), field.size() - 1);
            std::memcpy(field.data(), draft.data(), length);
            field[length] = '\0';
        }

        // Keyboard focus applies on a later frame, so it is requested until
        // taken. Not from an item the mouse holds, which would lose its click.
        if ((a_frame.focus || !fieldActive) && !a_frame.menuOpen && !menuOpen && !ImGui::IsAnyItemActive()
            && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool entered = ImGui::InputTextWithHint("##ChatInput", "Press Tab to switch channel",
            field.data(), field.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways
                | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion,
            &OnFieldEvent, this);
        fieldActive = ImGui::IsItemActive();

        if (entered)
        {
            if (composer.Submit(draft) != ChatSync::Composer::SubmitResult::kClose)
            {
                return Outcome::kSubmitted;
            }
            if (Config::Get()->ui.closeChatOnSend)
            {
                return Outcome::kClose;
            }
            // Escape would otherwise bring back a draft that was just sent.
            draftWhenOpened = draft;
            return Outcome::kSubmitted;
        }
        if (a_frame.escape)
        {
            if (listed)
            {
                composer.DismissListing();
                return Outcome::kNone;
            }
            draft = draftWhenOpened;
            composer.OnEdited(draft);
            return Outcome::kClose;
        }
        return Outcome::kNone;
    }
}
