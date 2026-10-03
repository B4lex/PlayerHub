#pragma once

#include "protocol/ChatMessage.h"
#include "sync/chat/ChatComposer.h"

#include <imgui.h>

#include <array>
#include <string>
#include <vector>

namespace UI
{
    // The open chat's input row: the send target as a chip with its menu, the
    // text field, and the list offered above it while a name or a command is
    // typed. What the keys and the text mean is ChatSync::Composer's.
    class ChatInput
    {
    public:
        enum class Outcome
        {
            kNone,
            // Enter was handled and the chat stays open.
            kSubmitted,
            // A line was sent or an empty one entered, with [UI] CloseChatOnSend,
            // or the draft was abandoned.
            kClose
        };

        struct Frame
        {
            // Put the keyboard in the field.
            bool focus{ false };
            // Escape was pressed and nothing before the input took it.
            bool escape{ false };
            // Close the target menu.
            bool closeMenu{ false };
            // A menu of the chat is open, so the field leaves it the keyboard.
            bool menuOpen{ false };
        };

        // The chat opened: Escape returns to the draft as it is now.
        void OnOpened();

        // The list, above where the input row is drawn next. Before Draw, so
        // the row stays the window's last item.
        void DrawListing();
        Outcome Draw(const Frame& a_frame);

        [[nodiscard]] ChatSync::Composer& GetComposer() { return composer; }
        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }

    private:
        static int OnFieldEvent(ImGuiInputTextCallbackData* a_data);
        void DrawChip(bool a_closeMenu);

        ChatSync::Composer composer;
        // The text as the composer left it. The field edits a copy and is
        // brought back to this whenever they differ.
        std::string draft;
        std::string draftWhenOpened;
        std::array<char, Multiplayer::kMaximumChatTextBytes + 1> field{};
        bool fieldActive{ false };

        // Rebuilt when the target changes.
        ChatSync::ChatTarget chipTarget;
        std::string chipLabel{ "Local" };
        bool menuOpen{ false };
        // Reused by the target menu.
        std::vector<Multiplayer::RosterEntry> partners;
    };
}
