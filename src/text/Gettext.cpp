#include "text/Gettext.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>

namespace Text::Gettext
{
    namespace
    {
        constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
        constexpr std::string_view kSpace = " \t";
        constexpr std::size_t kMaximumMessageForms = 100;
        constexpr std::size_t kMaximumExpressionLength = 256;
        constexpr std::size_t kMaximumNesting = 32;
        // Plural-Forms rules are checked over this range, as the build does.
        constexpr std::uint64_t kCheckedCounts = 10000;

        std::string_view Trim(std::string_view a_text)
        {
            const auto first = a_text.find_first_not_of(kSpace);
            if (first == std::string_view::npos)
            {
                return {};
            }
            return a_text.substr(first, a_text.find_last_not_of(kSpace) - first + 1);
        }

        std::optional<std::string> Unquote(std::string_view a_text)
        {
            if (a_text.size() < 2 || a_text.front() != '"' || a_text.back() != '"')
            {
                return std::nullopt;
            }
            a_text = a_text.substr(1, a_text.size() - 2);
            std::string result;
            result.reserve(a_text.size());
            for (std::size_t i = 0; i < a_text.size(); ++i)
            {
                const char character = a_text[i];
                if (character == '"')
                {
                    return std::nullopt;
                }
                if (character != '\\')
                {
                    result += character;
                    continue;
                }
                if (++i == a_text.size())
                {
                    return std::nullopt;
                }
                switch (a_text[i])
                {
                    case 'n':
                        result += '\n';
                        break;
                    case 't':
                        result += '\t';
                        break;
                    case 'r':
                        result += '\r';
                        break;
                    case '\\':
                    case '"':
                        result += a_text[i];
                        break;
                    default:
                        return std::nullopt;
                }
            }
            return result;
        }

        class Reader
        {
        public:
            std::expected<File, std::string> Read(std::string_view a_text)
            {
                if (a_text.starts_with(kByteOrderMark))
                {
                    a_text.remove_prefix(kByteOrderMark.size());
                }
                while (true)
                {
                    const auto end = a_text.find('\n');
                    auto line = a_text.substr(0, end);
                    if (line.ends_with('\r'))
                    {
                        line.remove_suffix(1);
                    }
                    ++lineNumber;
                    if (!ReadLine(Trim(line)))
                    {
                        return std::unexpected(std::format("line {}: {}", lineNumber, error));
                    }
                    if (end == std::string_view::npos)
                    {
                        break;
                    }
                    a_text.remove_prefix(end + 1);
                }
                if (!Finish())
                {
                    return std::unexpected(std::format("line {}: {}", lineNumber, error));
                }
                ReadHeader();
                return std::move(file);
            }

        private:
            enum class Target
            {
                kNone,
                kContext,
                kId,
                kPlural,
                kForm
            };

            bool Fail(std::string_view a_error)
            {
                error = a_error;
                return false;
            }

            bool Finish()
            {
                if (current && current->hasId)
                {
                    file.messages.push_back(std::move(current->message));
                }
                else if (current && (current->hasContext || !current->message.forms.empty()))
                {
                    return Fail("entry has no msgid");
                }
                current.reset();
                target = Target::kNone;
                return true;
            }

            bool Start()
            {
                if (current && current->hasId && !Finish())
                {
                    return false;
                }
                if (!current)
                {
                    current.emplace();
                    current->message.line = lineNumber;
                }
                return true;
            }

            bool ReadLine(std::string_view a_line)
            {
                if (a_line.empty() || a_line.starts_with("#~"))
                {
                    // A blank line ends an entry; #~ marks an obsolete one msgmerge keeps for reference.
                    return Finish();
                }
                if (a_line.starts_with('#'))
                {
                    if (!Start())
                    {
                        return false;
                    }
                    if (a_line.starts_with("#,"))
                    {
                        auto flags = a_line.substr(2);
                        while (!flags.empty())
                        {
                            const auto end = flags.find(',');
                            if (Trim(flags.substr(0, end)) == "fuzzy")
                            {
                                current->message.fuzzy = true;
                            }
                            flags.remove_prefix(end == std::string_view::npos ? flags.size() : end + 1);
                        }
                    }
                    return true;
                }
                if (a_line.starts_with('"'))
                {
                    if (target == Target::kNone)
                    {
                        return Fail("string outside an entry");
                    }
                    const auto value = Unquote(a_line);
                    if (!value)
                    {
                        return Fail("expected a quoted string");
                    }
                    GetTarget() += *value;
                    return true;
                }

                const auto space = a_line.find_first_of(kSpace);
                if (space == std::string_view::npos)
                {
                    return Fail("cannot read the line");
                }
                const auto keyword = a_line.substr(0, space);
                const auto value = Unquote(Trim(a_line.substr(space)));
                if (!value)
                {
                    return Fail("expected a quoted string");
                }
                if (keyword == "msgctxt")
                {
                    if (!Start())
                    {
                        return false;
                    }
                    current->message.context = *value;
                    current->hasContext = true;
                    target = Target::kContext;
                    return true;
                }
                if (!current)
                {
                    current.emplace();
                    current->message.line = lineNumber;
                }
                auto& message = current->message;
                if (keyword == "msgid")
                {
                    if (current->hasId)
                    {
                        return Fail("second msgid in one entry");
                    }
                    message.id = *value;
                    current->hasId = true;
                    target = Target::kId;
                    return true;
                }
                if (keyword == "msgid_plural")
                {
                    message.plural = *value;
                    message.hasPlural = true;
                    target = Target::kPlural;
                    return true;
                }
                std::size_t index = 0;
                if (keyword != "msgstr")
                {
                    if (!keyword.starts_with("msgstr[") || !keyword.ends_with(']'))
                    {
                        return Fail("unknown keyword");
                    }
                    const auto digits = keyword.substr(7, keyword.size() - 8);
                    const auto [end, result] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
                    if (digits.empty() || result != std::errc{} || end != digits.data() + digits.size()
                        || index >= kMaximumMessageForms)
                    {
                        return Fail("unknown keyword");
                    }
                }
                if (index < message.forms.size() && current->given[index])
                {
                    return Fail("msgstr given twice");
                }
                if (index >= message.forms.size())
                {
                    message.forms.resize(index + 1);
                    current->given.resize(index + 1);
                }
                message.forms[index] = *value;
                current->given[index] = true;
                formIndex = index;
                target = Target::kForm;
                return true;
            }

            std::string& GetTarget()
            {
                auto& message = current->message;
                switch (target)
                {
                    case Target::kContext:
                        return message.context;
                    case Target::kId:
                        return message.id;
                    case Target::kPlural:
                        return message.plural;
                    default:
                        return message.forms[formIndex];
                }
            }

            void ReadHeader()
            {
                auto& messages = file.messages;
                if (messages.empty() || !messages.front().id.empty() || !messages.front().context.empty())
                {
                    return;
                }
                std::string_view text = messages.front().forms.empty() ? std::string_view{}
                                                                        : messages.front().forms.front();
                while (!text.empty())
                {
                    const auto end = text.find('\n');
                    const auto line = text.substr(0, end);
                    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
                    const auto colon = line.find(':');
                    if (colon != std::string_view::npos)
                    {
                        file.headers.emplace_back(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
                    }
                }
                messages.erase(messages.begin());
            }

            struct Entry
            {
                Message message;
                std::vector<bool> given;
                bool hasContext{ false };
                bool hasId{ false };
            };

            File file;
            std::optional<Entry> current;
            Target target{ Target::kNone };
            std::size_t formIndex{ 0 };
            std::size_t lineNumber{ 0 };
            std::string error;
        };
    }

    std::string_view File::GetHeader(std::string_view a_name) const
    {
        const auto header = std::ranges::find(headers, a_name, [](const auto& a_header) -> std::string_view {
            return a_header.first;
        });
        return header == headers.end() ? std::string_view{} : std::string_view{ header->second };
    }

    std::expected<File, std::string> Parse(std::string_view a_text)
    {
        return Reader{}.Read(a_text);
    }

    class PluralRule::Parser
    {
    public:
        Parser(PluralRule& a_rule, std::string_view a_expression)
            : rule(a_rule), text(a_expression)
        {
        }

        std::expected<std::uint16_t, std::string> Run()
        {
            const auto node = ParseConditional();
            if (!node)
            {
                return node;
            }
            SkipSpace();
            if (position != text.size())
            {
                return Fail("unexpected text");
            }
            return node;
        }

    private:
        struct Operator
        {
            std::string_view symbol;
            int precedence;
            Op op;
        };

        // Longer symbols first, so <= is not read as <.
        static constexpr Operator kOperators[]{
            { "||", 1, Op::kOr },
            { "&&", 2, Op::kAnd },
            { "==", 3, Op::kEqual },
            { "!=", 3, Op::kNotEqual },
            { "<=", 4, Op::kLessEqual },
            { ">=", 4, Op::kGreaterEqual },
            { "<", 4, Op::kLess },
            { ">", 4, Op::kGreater },
            { "+", 5, Op::kAdd },
            { "-", 5, Op::kSubtract },
            { "*", 6, Op::kMultiply },
            { "/", 6, Op::kDivide },
            { "%", 6, Op::kModulo },
        };

        std::unexpected<std::string> Fail(std::string_view a_error) const
        {
            return std::unexpected(std::format("{} at '{}'", a_error, text.substr(position)));
        }

        void SkipSpace()
        {
            while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
            {
                ++position;
            }
        }

        bool Take(std::string_view a_symbol)
        {
            SkipSpace();
            if (!text.substr(position).starts_with(a_symbol))
            {
                return false;
            }
            position += a_symbol.size();
            return true;
        }

        std::uint16_t Add(Op a_op, std::uint64_t a_value = 0, std::uint16_t a_first = 0, std::uint16_t a_second = 0,
            std::uint16_t a_third = 0)
        {
            rule.nodes.push_back(Node{ .op = a_op, .value = a_value, .operands = { a_first, a_second, a_third } });
            return static_cast<std::uint16_t>(rule.nodes.size() - 1);
        }

        std::expected<std::uint16_t, std::string> ParsePrimary()
        {
            SkipSpace();
            if (Take("("))
            {
                if (++nesting > kMaximumNesting)
                {
                    return Fail("too deeply nested");
                }
                const auto inner = ParseConditional();
                --nesting;
                if (!inner)
                {
                    return inner;
                }
                if (!Take(")"))
                {
                    return Fail("expected ')'");
                }
                return inner;
            }
            if (Take("!"))
            {
                if (++nesting > kMaximumNesting)
                {
                    return Fail("too deeply nested");
                }
                const auto operand = ParsePrimary();
                --nesting;
                if (!operand)
                {
                    return operand;
                }
                return Add(Op::kNot, 0, *operand);
            }
            if (Take("n"))
            {
                return Add(Op::kCount);
            }
            std::uint32_t value = 0;
            const auto* begin = text.data() + position;
            const auto [end, result] = std::from_chars(begin, text.data() + text.size(), value);
            if (result != std::errc{} || end == begin)
            {
                return Fail("unexpected token");
            }
            position += static_cast<std::size_t>(end - begin);
            return Add(Op::kNumber, value);
        }

        std::expected<std::uint16_t, std::string> ParseBinary(int a_minimum)
        {
            auto left = ParsePrimary();
            while (left)
            {
                SkipSpace();
                const auto* found = std::ranges::find_if(
                    kOperators, [&](const Operator& a_operator) { return text.substr(position).starts_with(a_operator.symbol); });
                if (found == std::end(kOperators) || found->precedence < a_minimum)
                {
                    break;
                }
                position += found->symbol.size();
                const auto right = ParseBinary(found->precedence + 1);
                if (!right)
                {
                    return right;
                }
                left = Add(found->op, 0, *left, *right);
            }
            return left;
        }

        std::expected<std::uint16_t, std::string> ParseConditional()
        {
            const auto condition = ParseBinary(1);
            if (!condition || !Take("?"))
            {
                return condition;
            }
            if (++nesting > kMaximumNesting)
            {
                return Fail("too deeply nested");
            }
            const auto yes = ParseConditional();
            if (!yes)
            {
                return yes;
            }
            if (!Take(":"))
            {
                return Fail("expected ':'");
            }
            const auto no = ParseConditional();
            --nesting;
            if (!no)
            {
                return no;
            }
            return Add(Op::kConditional, 0, *condition, *yes, *no);
        }

        PluralRule& rule;
        std::string_view text;
        std::size_t position{ 0 };
        std::size_t nesting{ 0 };
    };

    std::expected<PluralRule, std::string> PluralRule::Parse(std::string_view a_pluralForms, std::size_t a_maximumForms)
    {
        constexpr std::string_view kFormat = "expected 'nplurals=N; plural=EXPRESSION;'";
        auto text = Trim(a_pluralForms);
        const auto readName = [&](std::string_view a_name) {
            text = Trim(text);
            if (!text.starts_with(a_name))
            {
                return false;
            }
            text = Trim(text.substr(a_name.size()));
            if (!text.starts_with('='))
            {
                return false;
            }
            text = Trim(text.substr(1));
            return true;
        };
        if (!readName("nplurals"))
        {
            return std::unexpected(std::string{ kFormat });
        }
        std::size_t count = 0;
        const auto [end, result] = std::from_chars(text.data(), text.data() + text.size(), count);
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        text = Trim(text);
        if (result != std::errc{} || !text.starts_with(';'))
        {
            return std::unexpected(std::string{ kFormat });
        }
        text.remove_prefix(1);
        if (!readName("plural"))
        {
            return std::unexpected(std::string{ kFormat });
        }
        if (text.ends_with(';'))
        {
            text.remove_suffix(1);
        }
        if (count < 1 || count > a_maximumForms)
        {
            return std::unexpected(std::format("nplurals must be 1 to {}", a_maximumForms));
        }
        if (text.size() > kMaximumExpressionLength)
        {
            return std::unexpected(std::format("the expression is longer than {} characters", kMaximumExpressionLength));
        }

        PluralRule rule;
        rule.formCount = count;
        const auto root = Parser{ rule, text }.Run();
        if (!root)
        {
            return std::unexpected(root.error());
        }
        rule.root = *root;
        for (std::uint64_t n = 0; n < kCheckedCounts; ++n)
        {
            std::uint64_t form = 0;
            if (!rule.Evaluate(rule.root, n, form))
            {
                return std::unexpected(std::format("divides by zero for n = {}", n));
            }
            if (form >= count)
            {
                return std::unexpected(std::format("gives form {} for n = {}, but nplurals is {}", form, n, count));
            }
        }
        return rule;
    }

    std::size_t PluralRule::GetForm(std::uint64_t a_count) const
    {
        std::uint64_t form = 0;
        if (nodes.empty() || !Evaluate(root, a_count, form))
        {
            return 0;
        }
        return static_cast<std::size_t>((std::min)(form, static_cast<std::uint64_t>(formCount - 1)));
    }

    bool PluralRule::Evaluate(std::uint16_t a_node, std::uint64_t a_count, std::uint64_t& a_result) const
    {
        const auto& node = nodes[a_node];
        switch (node.op)
        {
            case Op::kNumber:
                a_result = node.value;
                return true;
            case Op::kCount:
                a_result = a_count;
                return true;
            case Op::kConditional:
            {
                std::uint64_t condition = 0;
                return Evaluate(node.operands[0], a_count, condition)
                    && Evaluate(node.operands[condition != 0 ? 1 : 2], a_count, a_result);
            }
            default:
                break;
        }

        std::uint64_t a = 0;
        if (!Evaluate(node.operands[0], a_count, a))
        {
            return false;
        }
        if (node.op == Op::kNot)
        {
            a_result = a == 0 ? 1 : 0;
            return true;
        }
        std::uint64_t b = 0;
        if (!Evaluate(node.operands[1], a_count, b))
        {
            return false;
        }
        switch (node.op)
        {
            case Op::kOr:
                a_result = a != 0 || b != 0;
                break;
            case Op::kAnd:
                a_result = a != 0 && b != 0;
                break;
            case Op::kEqual:
                a_result = a == b;
                break;
            case Op::kNotEqual:
                a_result = a != b;
                break;
            case Op::kLess:
                a_result = a < b;
                break;
            case Op::kGreater:
                a_result = a > b;
                break;
            case Op::kLessEqual:
                a_result = a <= b;
                break;
            case Op::kGreaterEqual:
                a_result = a >= b;
                break;
            case Op::kAdd:
                a_result = a + b;
                break;
            case Op::kSubtract:
                a_result = a - b;
                break;
            case Op::kMultiply:
                a_result = a * b;
                break;
            case Op::kDivide:
            case Op::kModulo:
                if (b == 0)
                {
                    return false;
                }
                a_result = node.op == Op::kDivide ? a / b : a % b;
                break;
            default:
                return false;
        }
        return true;
    }
}
