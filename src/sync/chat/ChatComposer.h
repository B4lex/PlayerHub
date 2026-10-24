#pragma once

#include "sync/chat/ChatSync.h"

#include <cstddef>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace ChatSync
{
    class Composer
    {
    public:
        enum class SubmitResult
        {
            kClose,
            kStayOpen
        };

        [[nodiscard]] const ChatTarget& GetTarget() const { return target; }
        void SetTarget(const ChatTarget& a_target);
        void CycleTarget(int a_direction);

        bool OnEdited(std::string& a_draft);

        SubmitResult Submit(std::string& a_draft);

        [[nodiscard]] bool IsListing() const { return listing && !dismissed; }
        [[nodiscard]] std::span<const Multiplayer::PlayerIdentity> GetSuggestions() const;
        [[nodiscard]] std::size_t GetSelection() const { return selection; }
        void MoveSelection(int a_direction);
        void AcceptSuggestion(std::size_t a_index, std::string& a_draft);
        void DismissListing() { dismissed = true; }

        bool RecallPrevious(std::string& a_draft);
        bool RecallNext(std::string& a_draft);
        void StopRecalling() { recalled = sent.size(); }

        [[nodiscard]] std::span<const std::string> GetNotes() const { return notes; }
        void Fail(std::string a_text);
        void ClearNotes() { notes.clear(); }

    private:
        struct Sent
        {
            ChatTarget target;
            std::string text;
        };

        void ListPlayers(std::string_view a_prefix);
        void ClearListing();
        void Remember(std::string_view a_text);

        ChatTarget target;

        bool listing{ false };
        std::vector<Multiplayer::PlayerIdentity> suggestions;
        std::size_t selection{ 0 };
        bool dismissed{ false };
        std::vector<Multiplayer::PlayerIdentity> players;

        std::vector<std::string> notes;

        std::deque<Sent> sent;
        std::size_t recalled{ 0 };
        Sent unsent;
    };
}
