#pragma once

#include "sync/chat/ChatSync.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

        void OnEdited();
        void Update(std::string_view a_draft, std::size_t a_caret);

        SubmitResult Submit(std::string& a_draft);

        [[nodiscard]] bool IsListing() const { return !suggestions.empty() && !dismissed; }
        [[nodiscard]] std::span<const Multiplayer::PlayerIdentity> GetSuggestions() const;
        [[nodiscard]] std::size_t GetSelection() const { return selection; }
        void MoveSelection(int a_direction);
        bool AcceptSuggestion(std::size_t a_index, std::string& a_draft);
        void DismissListing() { dismissed = true; }
        // Where the caret goes after an accepted name, once.
        [[nodiscard]] std::optional<std::size_t> TakePlacedCaret();

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

        // The mention the caret is in: its `@` and the end of its name.
        struct TypedMention
        {
            std::size_t offset{ 0 };
            std::size_t end{ 0 };
            std::string prefix;
        };

        void ClearListing();
        void Recalled(std::string_view a_draft);
        void Remember(std::string_view a_text);

        ChatTarget target;

        std::optional<TypedMention> mention;
        std::vector<Multiplayer::PlayerIdentity> suggestions;
        std::size_t selection{ 0 };
        bool dismissed{ false };
        bool stale{ true };
        std::size_t updatedCaret{ 0 };
        std::optional<std::size_t> placedCaret;
        std::vector<Multiplayer::PlayerIdentity> players;

        std::vector<std::string> notes;

        std::deque<Sent> sent;
        std::size_t recalled{ 0 };
        Sent unsent;
    };
}
