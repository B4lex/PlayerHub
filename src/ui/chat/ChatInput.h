#pragma once

#include "protocol/ChatMessage.h"
#include "sync/chat/ChatComposer.h"

#include <imgui.h>

#include <array>
#include <string>
#include <vector>

namespace UI
{
    class ChatInput
    {
    public:
        enum class Outcome
        {
            kNone,
            kSubmitted,
            kClose
        };

        struct Frame
        {
            bool focus{ false };
            bool escape{ false };
            bool closeMenu{ false };
            bool menuOpen{ false };
        };

        void OnOpened();

        void DrawListing();
        Outcome Draw(const Frame& a_frame);

        [[nodiscard]] ChatSync::Composer& GetComposer() { return composer; }
        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }

    private:
        static int OnFieldEvent(ImGuiInputTextCallbackData* a_data);
        void DrawChip(bool a_closeMenu);

        ChatSync::Composer composer;
        std::string draft;
        std::string draftWhenOpened;
        std::array<char, Multiplayer::kMaximumChatTextBytes + 1> field{};
        bool fieldActive{ false };

        ChatSync::ChatTarget chipTarget;
        std::string chipLabel{ "Local" };
        bool menuOpen{ false };
        std::vector<Multiplayer::RosterEntry> partners;
    };
}
