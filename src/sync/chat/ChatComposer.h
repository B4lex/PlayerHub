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
        enum class Listing
        {
            kNone,
            kPlayers,
            kCommands
        };

        struct Suggestion
        {
            std::string label;
            std::string detail;
            Multiplayer::RosterEntry player;
            std::string_view command;
        };

        enum class SubmitResult
        {
            kClose,
            kStayOpen
        };

        struct Note
        {
            std::string text;
            std::string detail;
            bool failure{ false };
        };

        [[nodiscard]] const ChatTarget& GetTarget() const { return target; }
        void SetTarget(const ChatTarget& a_target);
        void CycleTarget(int a_direction);

        bool OnEdited(std::string& a_draft);

        SubmitResult Submit(std::string& a_draft);

        [[nodiscard]] Listing GetListing() const { return dismissed ? Listing::kNone : listing; }
        [[nodiscard]] std::span<const Suggestion> GetSuggestions() const;
        [[nodiscard]] std::size_t GetSelection() const { return selection; }
        void MoveSelection(int a_direction);
        void AcceptSuggestion(std::size_t a_index, std::string& a_draft);
        void DismissListing() { dismissed = true; }

        bool RecallPrevious(std::string& a_draft);
        bool RecallNext(std::string& a_draft);
        void StopRecalling() { recalled = sent.size(); }

        [[nodiscard]] std::span<const Note> GetNotes() const { return notes; }
        void Fail(std::string a_text);
        void ClearNotes() { notes.clear(); }

    private:
        struct Sent
        {
            ChatTarget target;
            std::string text;
        };

        void ListPlayers(std::string_view a_prefix);
        void ListCommands(std::string_view a_prefix);
        void ClearListing();
        void Reply();
        void Remember(std::string_view a_text);
        void Inform(std::string a_text, std::string a_detail = {});
        void ShowHelp();
        void ShowPlayerCount();

        ChatTarget target;

        Listing listing{ Listing::kNone };
        std::vector<Suggestion> suggestions;
        std::size_t selection{ 0 };
        bool dismissed{ false };
        std::vector<Multiplayer::RosterEntry> players;

        std::vector<Note> notes;

        std::deque<Sent> sent;
        std::size_t recalled{ 0 };
        Sent unsent;
    };
}
