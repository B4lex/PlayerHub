#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Text::Gettext
{
    struct Message
    {
        std::size_t line{ 0 };
        std::string context;
        std::string id;
        std::string plural;
        bool hasPlural{ false };
        bool fuzzy{ false };
        // By msgstr index; a gap is an empty string.
        std::vector<std::string> forms;
    };

    struct File
    {
        // Without the header entry.
        std::vector<Message> messages;
        std::vector<std::pair<std::string, std::string>> headers;

        // Empty when the header is missing.
        [[nodiscard]] std::string_view GetHeader(std::string_view a_name) const;
    };

    // Reads the subset of the PO format that msgmerge and PO editors write: msgctxt, msgid,
    // msgid_plural, msgstr[N], continuation lines, comments, and the fuzzy flag.
    [[nodiscard]] std::expected<File, std::string> Parse(std::string_view a_text);

    // A Plural-Forms header's C expression of n.
    class PluralRule
    {
    public:
        [[nodiscard]] static std::expected<PluralRule, std::string> Parse(
            std::string_view a_pluralForms, std::size_t a_maximumForms);

        [[nodiscard]] std::size_t GetFormCount() const { return formCount; }

        // Below GetFormCount for any count.
        [[nodiscard]] std::size_t GetForm(std::uint64_t a_count) const;

    private:
        enum class Op : std::uint8_t
        {
            kNumber,
            kCount,
            kNot,
            kOr,
            kAnd,
            kEqual,
            kNotEqual,
            kLess,
            kGreater,
            kLessEqual,
            kGreaterEqual,
            kAdd,
            kSubtract,
            kMultiply,
            kDivide,
            kModulo,
            kConditional
        };

        struct Node
        {
            Op op{ Op::kNumber };
            std::uint64_t value{ 0 };
            std::uint16_t operands[3]{};
        };

        class Parser;

        // False on a division by zero.
        [[nodiscard]] bool Evaluate(std::uint16_t a_node, std::uint64_t a_count, std::uint64_t& a_result) const;

        std::vector<Node> nodes;
        std::uint16_t root{ 0 };
        std::size_t formCount{ 1 };
    };
}
