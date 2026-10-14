#include "ui/trade/ItemCard.h"
#include "forms/ItemDetails.h"
#include "text/Text.h"
#include "text/Utf8Case.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        constexpr float kTitleScale = 1.45F;
        constexpr float kValueScale = 1.75F;
        constexpr float kLabelScale = 0.95F;
        constexpr float kNoteScale = 0.9F;
        constexpr float kRuleGapFonts = 0.2F;
        constexpr float kSectionGapFonts = 0.9F;
        constexpr float kStatGapFonts = 1.6F;
        constexpr float kMinimumStatGapFonts = 0.8F;
        constexpr float kLabelGapFonts = 0.4F;
        constexpr float kParagraphGapFonts = 0.3F;
        // Of the line height, between the lines of one paragraph.
        constexpr float kLineGap = 0.15F;
        // How far down the value's spare height its label sits.
        constexpr float kLabelDrop = 0.4F;

        constexpr ImU32 kTitleColor = IM_COL32(238, 238, 238, 255);
        constexpr ImU32 kRuleColor = IM_COL32(140, 140, 140, 255);
        constexpr ImU32 kLabelColor = IM_COL32(150, 150, 150, 255);
        constexpr ImU32 kValueColor = IM_COL32(242, 242, 242, 255);
        constexpr ImU32 kBodyColor = IM_COL32(172, 172, 172, 255);
        constexpr ImU32 kUnknownColor = IM_COL32(110, 110, 110, 255);

        struct Face
        {
            ImFont* font{ nullptr };
            float size{ 0.0F };
            ImU32 color{ 0 };
        };

        struct Faces
        {
            Face title;
            Face label;
            Face value;
            Face body;
            Face highlight;
            Face unknown;
            Face warning;
        };

        Faces GetFaces()
        {
            const auto fontSize = ImGui::GetFontSize();
            auto* const text = Fonts::GetText() ? Fonts::GetText() : ImGui::GetFont();
            auto* const heading = Fonts::GetHeading() ? Fonts::GetHeading() : text;
            return Faces{ .title = { heading, fontSize * kTitleScale, kTitleColor },
                .label = { text, fontSize * kLabelScale, kLabelColor },
                .value = { heading, fontSize * kValueScale, kValueColor },
                .body = { text, fontSize, kBodyColor },
                .highlight = { heading, fontSize, kValueColor },
                .unknown = { text, fontSize, kUnknownColor },
                .warning = { text, fontSize * kNoteScale, ImGui::GetColorU32(Theme::kWarning) } };
        }

        // Lays the card out from the top; it draws only with a draw list, so the same pass
        // measures the card first.
        struct Pen
        {
            ImDrawList* drawList{ nullptr };
            ImVec2 origin;
            float width{ 0.0F };
            float y{ 0.0F };
        };

        float Measure(const Face& a_face, std::string_view a_text)
        {
            return a_face.font->CalcTextSizeA(a_face.size, FLT_MAX, 0.0F, a_text.data(), a_text.data() + a_text.size())
                .x;
        }

        void Put(const Pen& a_pen, const Face& a_face, float a_x, float a_y, std::string_view a_text)
        {
            if (a_pen.drawList)
            {
                a_pen.drawList->AddText(a_face.font, a_face.size, ImVec2{ std::floor(a_x), std::floor(a_y) },
                    a_face.color, a_text.data(), a_text.data() + a_text.size());
            }
        }

        struct Piece
        {
            std::string_view text;
            bool highlighted{ false };
            float width{ 0.0F };
        };

        struct Word
        {
            std::size_t first{ 0 };
            std::size_t end{ 0 };
            float width{ 0.0F };
            // False between CJK characters, which have no spaces between words.
            bool spaced{ true };
        };

        // Kept between frames: the card is laid out twice in every one.
        std::vector<Piece> s_pieces;
        std::vector<Word> s_words;
        std::string s_title;
        std::string s_charge;

        bool IsDigit(char a_char)
        {
            return a_char >= '0' && a_char <= '9';
        }

        // Digits with single separators between them, as in 1.5 or 1,000.
        std::size_t NumberLength(std::string_view a_text)
        {
            std::size_t length = 0;
            while (length < a_text.size())
            {
                if (IsDigit(a_text[length]))
                {
                    ++length;
                }
                else if (length > 0 && (a_text[length] == '.' || a_text[length] == ',') && length + 1 < a_text.size()
                    && IsDigit(a_text[length + 1]))
                {
                    length += 2;
                }
                else
                {
                    break;
                }
            }
            return length;
        }

        char32_t Decode(std::string_view a_text, std::size_t& a_index)
        {
            const auto lead = static_cast<unsigned char>(a_text[a_index++]);
            const auto length = lead < 0xC0 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
            char32_t code = length == 0 ? lead : lead & (0x3FU >> length);
            for (int i = 0; i < length && a_index < a_text.size(); ++i)
            {
                code = (code << 6) | (static_cast<unsigned char>(a_text[a_index++]) & 0x3FU);
            }
            return code;
        }

        bool IsWide(char32_t a_code)
        {
            return (a_code >= 0x2E80 && a_code <= 0x9FFF) || (a_code >= 0xF900 && a_code <= 0xFAFF)
                || (a_code >= 0xFF00 && a_code <= 0xFFEF);
        }

        // Japanese and Chinese punctuation and small kana that may not start a line, and
        // opening brackets that may not end one.
        constexpr std::u32string_view kNoLineStart = U"、。，．・：；？！ー」』）〕］｝〉》】ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ々";
        constexpr std::u32string_view kNoLineEnd = U"「『（〔［｛〈《【";

        bool CanBreak(char32_t a_before, char32_t a_after)
        {
            return (IsWide(a_before) || IsWide(a_after)) && kNoLineStart.find(a_after) == std::u32string_view::npos
                && kNoLineEnd.find(a_before) == std::u32string_view::npos;
        }

        void AddWord(std::string_view a_word, bool a_spaced, const Face& a_body, const Face* a_highlight)
        {
            Word word{ .first = s_pieces.size(), .spaced = a_spaced };
            std::size_t start = 0;
            while (start < a_word.size())
            {
                const auto number = a_highlight ? NumberLength(a_word.substr(start)) : 0;
                auto end = start + number;
                if (number == 0)
                {
                    end = start + 1;
                    while (end < a_word.size() && !(a_highlight && IsDigit(a_word[end])))
                    {
                        ++end;
                    }
                }
                const auto text = a_word.substr(start, end - start);
                const auto width = Measure(number > 0 ? *a_highlight : a_body, text);
                s_pieces.push_back(Piece{ .text = text, .highlighted = number > 0, .width = width });
                word.width += width;
                start = end;
            }
            word.end = s_pieces.size();
            s_words.push_back(word);
        }

        // Word-wrapped lines, each centered; with a_highlight, numbers are drawn in it, as the
        // game's descriptions show their magnitudes.
        void Paragraph(Pen& a_pen, std::string_view a_text, const Face& a_body, const Face* a_highlight = nullptr)
        {
            s_pieces.clear();
            s_words.clear();
            std::size_t start = 0;
            while (start < a_text.size())
            {
                auto end = a_text.find_first_of(" \n", start);
                end = end == std::string_view::npos ? a_text.size() : end;
                const auto chunk = a_text.substr(start, end - start);
                std::size_t wordStart = 0;
                std::size_t index = 0;
                char32_t previous = 0;
                while (index < chunk.size())
                {
                    const auto codeStart = index;
                    const auto code = Decode(chunk, index);
                    if (codeStart > wordStart && CanBreak(previous, code))
                    {
                        AddWord(chunk.substr(wordStart, codeStart - wordStart), wordStart == 0, a_body, a_highlight);
                        wordStart = codeStart;
                    }
                    previous = code;
                }
                if (chunk.size() > wordStart)
                {
                    AddWord(chunk.substr(wordStart), wordStart == 0, a_body, a_highlight);
                }
                start = end + 1;
            }
            const auto space = Measure(a_body, " ");
            const auto lineHeight = (std::max)(a_body.size, a_highlight ? a_highlight->size : 0.0F);
            for (std::size_t first = 0; first < s_words.size();)
            {
                auto width = s_words[first].width;
                auto last = first + 1;
                const auto gap = [&](std::size_t a_word) { return s_words[a_word].spaced ? space : 0.0F; };
                while (last < s_words.size() && width + gap(last) + s_words[last].width <= a_pen.width)
                {
                    width += gap(last) + s_words[last].width;
                    ++last;
                }
                auto x = a_pen.origin.x + (std::max)((a_pen.width - width) * 0.5F, 0.0F);
                for (auto word = first; word < last; ++word)
                {
                    x += word > first ? gap(word) : 0.0F;
                    for (auto piece = s_words[word].first; piece < s_words[word].end; ++piece)
                    {
                        const auto& face = s_pieces[piece].highlighted ? *a_highlight : a_body;
                        Put(a_pen, face, x, a_pen.origin.y + a_pen.y + lineHeight - face.size, s_pieces[piece].text);
                        x += s_pieces[piece].width;
                    }
                }
                a_pen.y += lineHeight;
                first = last;
                a_pen.y += first < s_words.size() ? std::floor(lineHeight * kLineGap) : 0.0F;
            }
        }

        void Gap(Pen& a_pen, float a_fonts)
        {
            a_pen.y += std::floor(a_fonts * ImGui::GetFontSize());
        }

        std::string_view FormatWeight(float a_weight, std::span<char> a_buffer)
        {
            const auto tenths = std::round(a_weight * 10.0F);
            const auto end = std::fmod(tenths, 10.0F) == 0.0F
                ? std::format_to_n(a_buffer.data(), a_buffer.size(), "{:.0f}", tenths / 10.0F).out
                : std::format_to_n(a_buffer.data(), a_buffer.size(), "{:.1f}", tenths / 10.0F).out;
            return std::string_view{ a_buffer.data(), end };
        }

        struct Stat
        {
            std::string_view label;
            std::string_view value;
            float labelWidth{ 0.0F };
            float width{ 0.0F };
        };

        // Each stat's label before its value, all on one centered line, spaced as in the
        // inventory menu while they fit and wrapped onto more lines when they do not.
        void Stats(Pen& a_pen, const Faces& a_faces, std::span<Stat> a_stats)
        {
            const auto fontSize = ImGui::GetFontSize();
            const auto labelGap = kLabelGapFonts * fontSize;
            float total = 0.0F;
            for (auto& stat : a_stats)
            {
                stat.labelWidth = Measure(a_faces.label, stat.label);
                stat.width = stat.labelWidth + labelGap + Measure(a_faces.value, stat.value);
                total += stat.width;
            }
            const auto spaces = static_cast<float>(a_stats.size() - 1);
            const auto gap = spaces > 0.0F
                ? std::clamp((a_pen.width - total) / spaces, kMinimumStatGapFonts * fontSize, kStatGapFonts * fontSize)
                : 0.0F;
            const auto labelTop = (a_faces.value.size - a_faces.label.size) * kLabelDrop;
            for (std::size_t first = 0; first < a_stats.size();)
            {
                auto width = a_stats[first].width;
                auto last = first + 1;
                while (last < a_stats.size() && width + gap + a_stats[last].width <= a_pen.width)
                {
                    width += gap + a_stats[last].width;
                    ++last;
                }
                auto x = a_pen.origin.x + (std::max)((a_pen.width - width) * 0.5F, 0.0F);
                const auto top = a_pen.origin.y + a_pen.y;
                for (auto stat = first; stat < last; ++stat)
                {
                    Put(a_pen, a_faces.label, x, top + labelTop, a_stats[stat].label);
                    Put(a_pen, a_faces.value, x + a_stats[stat].labelWidth + labelGap, top, a_stats[stat].value);
                    x += a_stats[stat].width + gap;
                }
                a_pen.y += a_faces.value.size;
                first = last;
            }
        }

        // The inventory menu's item card: the name in capitals over a rule, the numbers, and the
        // effects with their magnitudes picked out.
        void Layout(Pen& a_pen, const Faces& a_faces, const ItemCard& a_card)
        {
            const auto& details = *a_card.details;
            s_title.clear();
            Utf8Case::AppendUpper(s_title, a_card.name);
            if (a_card.count > 1)
            {
                std::format_to(std::back_inserter(s_title), " ({})", a_card.count);
            }
            Paragraph(a_pen, s_title, a_faces.title);
            Gap(a_pen, kRuleGapFonts);
            if (a_pen.drawList)
            {
                const auto y = std::floor(a_pen.origin.y + a_pen.y) + 0.5F;
                a_pen.drawList->AddLine(
                    ImVec2{ a_pen.origin.x, y }, ImVec2{ a_pen.origin.x + a_pen.width, y }, kRuleColor);
            }
            a_pen.y += 1.0F;
            Gap(a_pen, kSectionGapFonts * 0.5F);
            if (!a_card.blocked.empty())
            {
                Paragraph(a_pen, a_card.blocked, a_faces.warning);
                Gap(a_pen, kSectionGapFonts);
            }

            std::array<char, 24> rating{};
            std::array<char, 24> weight{};
            std::array<char, 24> value{};
            std::array<Stat, 3> stats{};
            std::size_t statCount = 0;
            if (details.rating != Forms::ItemDetails::Rating::kNone)
            {
                const auto end = std::format_to_n(rating.data(), rating.size(), "{}", details.ratingValue).out;
                stats[statCount++] = Stat{ .label = Text::Get(details.rating == Forms::ItemDetails::Rating::kDamage
                                                   ? Text::Id::kItemCardDamage
                                                   : Text::Id::kItemCardArmor),
                    .value = std::string_view{ rating.data(), end } };
            }
            const auto count = (std::max)(a_card.count, 1U);
            stats[statCount++] = Stat{ .label = Text::Get(Text::Id::kItemCardWeight),
                .value = FormatWeight(details.weight * static_cast<float>(count), weight) };
            const auto valueEnd
                = std::format_to_n(value.data(), value.size(), "{}", static_cast<long long>(details.value) * count).out;
            stats[statCount++]
                = Stat{ .label = Text::Get(Text::Id::kItemCardValue), .value = std::string_view{ value.data(), valueEnd } };
            Stats(a_pen, a_faces, std::span{ stats.data(), statCount });

            if (!details.effects.empty() || details.maximumCharge > 0)
            {
                Gap(a_pen, kSectionGapFonts);
                bool first = true;
                for (const auto& effect : details.effects)
                {
                    if (!std::exchange(first, false))
                    {
                        Gap(a_pen, kParagraphGapFonts);
                    }
                    if (effect.known)
                    {
                        Paragraph(a_pen, effect.text, a_faces.body, &a_faces.highlight);
                    }
                    else
                    {
                        Paragraph(a_pen, effect.text, a_faces.unknown);
                    }
                }
                if (details.maximumCharge > 0)
                {
                    if (!first)
                    {
                        Gap(a_pen, kParagraphGapFonts);
                    }
                    Text::FormatTo(s_charge, Text::Id::kItemCardCharge, details.charge, details.maximumCharge);
                    Paragraph(a_pen, s_charge, a_faces.body, &a_faces.highlight);
                }
            }
        }

    }

    float MeasureItemCard(const ItemCard& a_card, float a_width)
    {
        Pen pen{ .width = a_width };
        Layout(pen, GetFaces(), a_card);
        return std::ceil(pen.y);
    }

    void DrawItemCard(const ItemCard& a_card, float a_width)
    {
        Pen pen{ .drawList = ImGui::GetWindowDrawList(), .origin = ImGui::GetCursorScreenPos(), .width = a_width };
        Layout(pen, GetFaces(), a_card);
        ImGui::Dummy(ImVec2{ a_width, std::ceil(pen.y) });
    }
}
