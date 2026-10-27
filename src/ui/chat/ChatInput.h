#pragma once

#include "protocol/ChatMessage.h"
#include "sync/chat/ChatComposer.h"
#include "text/Text.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
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

        [[nodiscard]] float GetFieldHeight() const;
        [[nodiscard]] ChatSync::Composer& GetComposer() { return composer; }
        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }

    private:
        static int OnFieldEvent(ImGuiInputTextCallbackData* a_data);
        int FilterCharacter(ImGuiInputTextCallbackData& a_data);
        void Measure(std::string_view a_text, std::size_t a_caret);
        void DrawChip(bool a_closeMenu, float a_rowTop);
        void DrawNotes() const;

        ChatSync::Composer composer;
        std::string draft;
        std::string draftWhenOpened;
        std::array<char, Multiplayer::kMaximumChatTextBytes + 1> field{};
        bool fieldActive{ false };
        bool sendByLineBreak{ false };
        std::size_t room{ 0 };
        int roomFrame{ -1 };
        int caretPosition{ 0 };
        int selectionStart{ 0 };
        int selectionEnd{ 0 };
        float fieldWrapWidth{ 0.0F };
        int lineCount{ 1 };
        int caretLine{ 0 };

        ChatSync::ChatTarget chipTarget;
        std::string chipLabel;
        std::optional<Text::Language> chipLanguage;
        bool menuOpen{ false };
        std::vector<Multiplayer::PlayerIdentity> partners;
    };
}
