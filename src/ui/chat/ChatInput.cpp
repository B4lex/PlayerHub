#include "ui/chat/ChatInput.h"
#include "config/PluginConfig.h"
#include "sync/chat/ChatRoster.h"
#include "text/Text.h"
#include "ui/chat/ChatStyle.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string_view>

namespace UI
{
    namespace
    {
        constexpr std::size_t kMaximumMenuPartners = 5;
        constexpr float kMaximumChipTextFonts = 12.0F;
        constexpr int kMaximumFieldLines = 7;

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
                    return std::string{ Text::Get(Text::Id::kChatNearby) };
                case Multiplayer::ChatChannel::kGlobal:
                    return std::string{ Text::Get(Text::Id::kChatGlobal) };
                case Multiplayer::ChatChannel::kWhisper:
                    break;
            }
            return Text::Format(Text::Id::kChatTo, a_target.player.name);
        }
    }

    void ChatInput::OnOpened()
    {
        draftWhenOpened = draft;
        composer.StopRecalling();
        composer.ClearNotes();
    }

    float ChatInput::GetFieldHeight() const
    {
        const auto lines = std::clamp(lineCount, 1, kMaximumFieldLines);
        return static_cast<float>(lines) * ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0F;
    }

    int ChatInput::FilterCharacter(ImGuiInputTextCallbackData& a_data)
    {
        if (a_data.EventChar == '\n')
        {
            // Shift+Enter and Ctrl+Enter, which a multi-line field turns into a line break.
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            {
                sendByLineBreak = true;
                return 1;
            }
            a_data.EventChar = ' ';
        }

        // Pasted text passes here character by character before any of it is
        // inserted, so the room is taken once per frame and counted down.
        const auto frame = ImGui::GetFrameCount();
        if (frame != roomFrame)
        {
            roomFrame = frame;
            const auto* state = ImGui::GetInputTextState(a_data.ID);
            const std::string_view text{ state->TextA.Data, static_cast<std::size_t>(state->TextLen) };
            const auto selectionFirst
                = static_cast<std::size_t>((std::min)(a_data.SelectionStart, a_data.SelectionEnd));
            const auto selectionLast = static_cast<std::size_t>((std::max)(a_data.SelectionStart, a_data.SelectionEnd));
            const auto kept = Multiplayer::CountChatCharacters(text)
                - Multiplayer::CountChatCharacters(text.substr(selectionFirst, selectionLast - selectionFirst));
            room = Multiplayer::kMaximumChatTextCharacters - (std::min)(kept, Multiplayer::kMaximumChatTextCharacters);
        }
        if (room == 0)
        {
            return 1;
        }
        --room;
        return 0;
    }

    void ChatInput::Measure(std::string_view a_text, std::size_t a_caret)
    {
        lineCount = 1;
        caretLine = 0;
        if (fieldWrapWidth <= 0.0F || a_text.empty())
        {
            return;
        }
        // As Dear ImGui breaks the lines of a word-wrapped field.
        auto* font = ImGui::GetFont();
        const auto fontSize = ImGui::GetFontSize();
        const auto* end = a_text.data() + a_text.size();
        int lines = 0;
        for (const auto* line = a_text.data(); line < end;)
        {
            if (static_cast<std::size_t>(line - a_text.data()) <= a_caret)
            {
                caretLine = lines;
            }
            ++lines;
            line = ImFontCalcWordWrapPositionEx(
                font, fontSize, line, end, fieldWrapWidth, ImDrawTextFlags_WrapKeepBlanks);
        }
        lineCount = lines;
    }

    int ChatInput::OnFieldEvent(ImGuiInputTextCallbackData* a_data)
    {
        auto& self = *static_cast<ChatInput*>(a_data->UserData);
        if (a_data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
        {
            return self.FilterCharacter(*a_data);
        }
        // Escape puts back the text the field had when it took the keyboard, before this runs.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            return 0;
        }
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
        else
        {
            // A multi-line field has moved its caret for these keys already;
            // the caret line is still the one from before. Ctrl scrolls the
            // field and Shift selects.
            const auto& io = ImGui::GetIO();
            const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
            const bool down = !up && ImGui::IsKeyPressed(ImGuiKey_DownArrow);
            if ((up || down) && !io.KeyCtrl && !io.KeyShift)
            {
                if (listed)
                {
                    composer.MoveSelection(up ? -1 : 1);
                    a_data->CursorPos = self.caretPosition;
                    a_data->SelectionStart = self.selectionStart;
                    a_data->SelectionEnd = self.selectionEnd;
                }
                else if (up ? self.caretLine == 0 && composer.RecallPrevious(self.draft)
                            : self.caretLine + 1 >= self.lineCount && composer.RecallNext(self.draft))
                {
                    rewritten = true;
                }
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
        self.caretPosition = a_data->CursorPos;
        self.selectionStart = a_data->SelectionStart;
        self.selectionEnd = a_data->SelectionEnd;
        if (const auto* state = ImGui::GetInputTextState(a_data->ID))
        {
            self.fieldWrapWidth = state->WrapWidth;
        }
        self.Measure(self.draft, static_cast<std::size_t>(self.caretPosition));
        return 0;
    }

    void ChatInput::DrawChip(bool a_closeMenu, float a_rowTop)
    {
        const auto& target = composer.GetTarget();
        if (!(target == chipTarget) || target.player.name != chipTarget.player.name
            || chipLanguage != Text::GetLanguage())
        {
            chipTarget = target;
            chipLanguage = Text::GetLanguage();
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

        ImGui::SetNextWindowPos(
            ImVec2{ min.x, a_rowTop - style.ItemSpacing.y }, ImGuiCond_Always, ImVec2{ 0.0F, 1.0F });
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
                        ImGui::SetTooltip("%s", Text::GetCString(Text::Id::kChatGlobalTargetHint));
                        break;
                    case Multiplayer::ChatChannel::kNearby:
                        ImGui::SetTooltip("%s", Text::GetCString(Text::Id::kChatNearbyTargetHint));
                        break;
                    case Multiplayer::ChatChannel::kWhisper:
                        ImGui::SetTooltip(
                            "%s", Text::Format(Text::Id::kChatWhisperTargetHint, a_target.player.name).c_str());
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
        pick(ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kGlobal }, Text::Get(Text::Id::kChatGlobal), 0);
        pick(ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kNearby }, Text::Get(Text::Id::kChatNearby), 1);
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
                DrawRow(Text::Get(Text::Id::kChatNoPlayerNamed), ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), false);
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
            Measure(draft, draft.size());
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
        // The field grows upward from the chip, which stays by its last line.
        const auto fieldHeight = GetFieldHeight();
        const auto row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2{ row.x, row.y + fieldHeight - ImGui::GetFrameHeight() });
        DrawChip(a_frame.closeMenu, row.y);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2{ ImGui::GetCursorScreenPos().x, row.y });

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
        // Past its last line the field scrolls without a scrollbar, as a
        // single-line field does sideways. The wrap keeps the scrollbar's
        // width free on the right either way.
        constexpr ImVec4 kHidden{ 0.0F, 0.0F, 0.0F, 0.0F };
        for (const auto color : { ImGuiCol_ScrollbarBg, ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered,
                 ImGuiCol_ScrollbarGrabActive })
        {
            ImGui::PushStyleColor(color, kHidden);
        }
        sendByLineBreak = false;
        bool entered = ImGui::InputTextEx("##ChatInput", Text::GetCString(Text::Id::kChatInputHint), field.data(),
            static_cast<int>(field.size()), ImVec2{ -FLT_MIN, fieldHeight },
            static_cast<ImGuiInputTextFlags>(ImGuiInputTextFlags_Multiline) | ImGuiInputTextFlags_WordWrap
                | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue
                | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackCompletion
                | ImGuiInputTextFlags_CallbackCharFilter,
            &OnFieldEvent, this);
        ImGui::PopStyleColor(4);
        if (sendByLineBreak && ImGui::IsItemActive())
        {
            // Leaves the field as Enter does, so it takes the emptied draft back.
            ImGui::ClearActiveID();
            entered = true;
        }
        fieldActive = ImGui::IsItemActive();

        if (entered)
        {
            const auto result = composer.Submit(draft);
            Measure(draft, draft.size());
            if (result != ChatSync::Composer::SubmitResult::kClose)
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
            Measure(draft, draft.size());
            return Outcome::kClose;
        }
        return Outcome::kNone;
    }
}
