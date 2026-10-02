#pragma once

#include "sync/chat/ChatSync.h"

#include <cstddef>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace ChatSync
{
    // What the local player is writing and where it goes: the send target and
    // how prefixes, commands, and keys change it, the list offered while a
    // name or a command is typed, and the lines sent before. It holds no text
    // itself; the input's draft is passed in and rewritten.
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
            // A command's description; empty for a player.
            std::string detail;
            // No slot for a command.
            Multiplayer::RosterEntry player;
            // A command's name; empty for a player.
            std::string_view command;
        };

        enum class SubmitResult
        {
            kClose,
            kStayOpen
        };

        [[nodiscard]] const ChatTarget& GetTarget() const { return target; }
        void SetTarget(const ChatTarget& a_target);
        // Local, Global, then the latest whisper partners; a_direction is 1 or -1.
        void CycleTarget(int a_direction);
        // Returns a whisper target to the channel it replaced. False on a channel.
        bool LeaveWhisper();

        // After the draft's text changed: applies a complete prefix or command,
        // removing it from the draft, and refreshes the list. Returns whether
        // it rewrote the draft.
        bool OnEdited(std::string& a_draft);

        // Enter: accepts the selected suggestion, runs a command, or sends the
        // draft and clears it. The draft is kept when nothing was sent.
        SubmitResult Submit(std::string& a_draft);

        // What is listed while the draft names a player or a command. A
        // players listing may be empty: nobody matches.
        [[nodiscard]] Listing GetListing() const { return dismissed ? Listing::kNone : listing; }
        [[nodiscard]] std::span<const Suggestion> GetSuggestions() const;
        [[nodiscard]] std::size_t GetSelection() const { return selection; }
        void MoveSelection(int a_direction);
        void AcceptSuggestion(std::size_t a_index, std::string& a_draft);
        // Hides the list until the draft changes.
        void DismissListing() { dismissed = true; }

        // Steps through the lines sent before, restoring each one's target.
        // Stepping past the newest returns the draft that was being written.
        // Each returns whether it changed the draft.
        bool RecallPrevious(std::string& a_draft);
        bool RecallNext(std::string& a_draft);
        // Forgets the place among the sent lines, when the draft is abandoned.
        void StopRecalling() { recalled = sent.size(); }

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

        ChatTarget target;
        // What a whisper target replaced.
        Multiplayer::ChatChannel previousChannel{ Multiplayer::ChatChannel::kLocal };

        Listing listing{ Listing::kNone };
        std::vector<Suggestion> suggestions;
        std::size_t selection{ 0 };
        bool dismissed{ false };
        // Reused by ListPlayers and CycleTarget.
        std::vector<Multiplayer::RosterEntry> players;

        std::deque<Sent> sent;
        // The recalled line; sent.size() while writing a new one.
        std::size_t recalled{ 0 };
        Sent unsent;
    };
}
