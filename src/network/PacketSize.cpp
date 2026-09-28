#include "network/PacketSize.h"

#include <charconv>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace Network
{
    namespace
    {
        // The Engine.IO message type "4", then the Socket.IO packet type.
        constexpr std::size_t kPacketTypeBytes = 2;
        constexpr std::size_t kNullBytes = 4;

        std::size_t CountDigits(std::uint64_t a_value)
        {
            std::size_t digits = 1;
            while (a_value >= 10)
            {
                a_value /= 10;
                ++digits;
            }
            return digits;
        }

        std::size_t EstimateIntegerSize(std::int64_t a_value)
        {
            const auto magnitude
                = a_value < 0 ? 0 - static_cast<std::uint64_t>(a_value) : static_cast<std::uint64_t>(a_value);
            return (a_value < 0 ? 1 : 0) + CountDigits(magnitude);
        }

        // Both writers print the shortest round-trip digits in fixed notation and
        // mark an integral value with ".0". They switch to exponent notation only
        // for magnitudes synced values do not reach (Python below 1e-4 or from
        // 1e16), where this estimate stays in fixed notation.
        std::size_t EstimateDoubleSize(double a_value)
        {
            char buffer[64];
            auto result = std::to_chars(std::begin(buffer), std::end(buffer), a_value, std::chars_format::fixed);
            if (result.ec != std::errc{})
            {
                result = std::to_chars(std::begin(buffer), std::end(buffer), a_value);
            }
            const std::string_view text{ buffer, result.ec == std::errc{} ? result.ptr : buffer };
            return text.size() + (text.find_first_of(".e") == std::string_view::npos ? 2 : 0);
        }

        std::size_t EstimateStringSize(std::string_view a_text, JsonWriter a_writer)
        {
            std::size_t size = 2;
            for (std::size_t index = 0; index < a_text.size();)
            {
                const auto byte = static_cast<unsigned char>(a_text[index]);
                if (byte >= 0x80)
                {
                    const std::size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;
                    if (a_writer == JsonWriter::kServer)
                    {
                        // \uXXXX, or a surrogate pair of them outside the BMP.
                        size += length == 4 ? 12 : 6;
                    }
                    else
                    {
                        size += length;
                    }
                    index += length;
                    continue;
                }

                switch (byte)
                {
                    case '"':
                    case '\\':
                    case '\b':
                    case '\f':
                    case '\n':
                    case '\r':
                    case '\t':
                        size += 2;
                        break;
                    default:
                        size += byte < 0x20 ? 6 : 1;
                        break;
                }
                ++index;
            }
            return size;
        }

        std::size_t EstimateJsonSize(const sio::message::ptr& a_message, JsonWriter a_writer)
        {
            if (!a_message)
            {
                return kNullBytes;
            }

            switch (a_message->get_flag())
            {
                case sio::message::flag_integer:
                    return EstimateIntegerSize(a_message->get_int());
                case sio::message::flag_double:
                    return EstimateDoubleSize(a_message->get_double());
                case sio::message::flag_string:
                    return EstimateStringSize(a_message->get_string(), a_writer);
                case sio::message::flag_binary:
                {
                    const auto& binary = a_message->get_binary();
                    return binary ? binary->size() : 0;
                }
                case sio::message::flag_array:
                {
                    const auto& values = a_message->get_vector();
                    std::size_t size = 2 + (values.empty() ? 0 : values.size() - 1);
                    for (const auto& value : values)
                    {
                        size += EstimateJsonSize(value, a_writer);
                    }
                    return size;
                }
                case sio::message::flag_object:
                {
                    const auto& values = a_message->get_map();
                    std::size_t size = 2 + (values.empty() ? 0 : values.size() - 1);
                    for (const auto& [key, value] : values)
                    {
                        size += EstimateStringSize(key, a_writer) + 1 + EstimateJsonSize(value, a_writer);
                    }
                    return size;
                }
                case sio::message::flag_boolean:
                    return a_message->get_bool() ? 4 : 5;
                case sio::message::flag_null:
                default:
                    return kNullBytes;
            }
        }

        // The JSON array of the arguments, with a leading comma per argument.
        std::size_t EstimateTrailingArgumentsSize(const sio::message::list& a_arguments, JsonWriter a_writer)
        {
            std::size_t size = a_arguments.size();
            for (std::size_t index = 0; index < a_arguments.size(); ++index)
            {
                size += EstimateJsonSize(a_arguments[index], a_writer);
            }
            return size;
        }
    }

    std::size_t EstimateEventPacketSize(
        std::string_view a_event, const sio::message::list& a_arguments, JsonWriter a_writer)
    {
        return kPacketTypeBytes + 2 + EstimateStringSize(a_event, a_writer)
            + EstimateTrailingArgumentsSize(a_arguments, a_writer);
    }

    std::size_t EstimateAcknowledgementPacketSize(const sio::message::list& a_arguments, JsonWriter a_writer)
    {
        // The arguments follow "[" directly, so there is one comma fewer.
        const auto arguments = EstimateTrailingArgumentsSize(a_arguments, a_writer);
        return kPacketTypeBytes + 2 + (a_arguments.size() > 0 ? arguments - 1 : 0);
    }
}
