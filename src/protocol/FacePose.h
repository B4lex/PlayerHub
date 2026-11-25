#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Network
{
    // Wire order of the face weights: the 17 expressions, the 17 modifiers, then the 16 phonemes.
    inline constexpr std::size_t kFaceExpressionCount = 17;
    inline constexpr std::size_t kFaceModifierCount = 17;
    inline constexpr std::size_t kFacePhonemeCount = 16;
    inline constexpr std::size_t kFaceFirstModifierWeight = kFaceExpressionCount;
    inline constexpr std::size_t kFaceFirstPhonemeWeight = kFaceFirstModifierWeight + kFaceModifierCount;
    inline constexpr std::size_t kFaceWeightCount = kFaceFirstPhonemeWeight + kFacePhonemeCount;
    static_assert(kFaceWeightCount == 50);

    inline constexpr std::uint8_t kFaceLayoutWhole = 1;
    // The change from the face its stream last carried.
    inline constexpr std::uint8_t kFaceLayoutChange = 2;
    inline constexpr std::uint64_t kAllFaceWeights = (std::uint64_t{ 1 } << kFaceWeightCount) - 1;
    // The layout byte, the mask's varint, and a byte per weight.
    inline constexpr std::size_t kMaximumFaceMaskBytes = (kFaceWeightCount + 6) / 7;
    inline constexpr std::size_t kMaximumFacePoseBytes = 1 + kMaximumFaceMaskBytes + kFaceWeightCount;
    static_assert(kMaximumFacePoseBytes == 59);
    inline constexpr float kFaceWeightSteps = 255.0F;

    struct FaceWeights
    {
        std::array<float, kFaceWeightCount> values{};
    };

    struct QuantizedFacePose
    {
        std::array<std::uint8_t, kFaceWeightCount> weights{};

        [[nodiscard]] bool operator==(const QuantizedFacePose&) const = default;
    };

    struct EncodedFacePose
    {
        std::array<std::uint8_t, kMaximumFacePoseBytes> bytes{};
        std::uint8_t size{ 0 };

        [[nodiscard]] bool empty() const noexcept { return size == 0; }
        [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return { bytes.data(), size }; }
    };

    namespace Detail
    {
        // The layout byte and the shortest varint of a weight mask; 0 when either is invalid.
        inline std::size_t ReadFaceHeader(
            std::span<const std::uint8_t> a_bytes, std::uint8_t a_layout, std::uint64_t& a_mask) noexcept
        {
            if (a_bytes.empty() || a_bytes[0] != a_layout)
            {
                return 0;
            }
            a_mask = 0;
            for (std::size_t i = 0; i < kMaximumFaceMaskBytes && 1 + i < a_bytes.size(); ++i)
            {
                const std::uint8_t byte = a_bytes[1 + i];
                a_mask |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * i);
                if ((byte & 0x80) == 0)
                {
                    if ((i > 0 && byte == 0) || (a_mask & ~kAllFaceWeights) != 0)
                    {
                        return 0;
                    }
                    return 2 + i;
                }
            }
            return 0;
        }

        [[nodiscard]] inline std::size_t GetFacePoseSize(std::uint64_t a_mask) noexcept
        {
            const auto maskBytes = a_mask == 0 ? 1 : (std::bit_width(a_mask) + 6) / 7;
            return 1 + static_cast<std::size_t>(maskBytes) + static_cast<std::size_t>(std::popcount(a_mask));
        }

        inline EncodedFacePose EncodeMaskedFace(
            const QuantizedFacePose& a_face, std::uint8_t a_layout, std::uint64_t a_mask) noexcept
        {
            EncodedFacePose result;
            result.bytes[result.size++] = a_layout;
            auto mask = a_mask;
            while (mask >= 0x80)
            {
                result.bytes[result.size++] = static_cast<std::uint8_t>(mask | 0x80);
                mask >>= 7;
            }
            result.bytes[result.size++] = static_cast<std::uint8_t>(mask);
            for (std::size_t weight = 0; weight < kFaceWeightCount; ++weight)
            {
                if ((a_mask >> weight) & 1)
                {
                    result.bytes[result.size++] = a_face.weights[weight];
                }
            }
            return result;
        }

        [[nodiscard]] inline std::uint64_t GetNonzeroFaceWeights(const QuantizedFacePose& a_face) noexcept
        {
            std::uint64_t mask = 0;
            for (std::size_t weight = 0; weight < kFaceWeightCount; ++weight)
            {
                if (a_face.weights[weight] != 0)
                {
                    mask |= std::uint64_t{ 1 } << weight;
                }
            }
            return mask;
        }
    }

    // A weight outside (0, 1] reads as unset, as the engine's own keyframe test does.
    [[nodiscard]] inline QuantizedFacePose QuantizeFace(const FaceWeights& a_weights) noexcept
    {
        QuantizedFacePose result;
        for (std::size_t weight = 0; weight < kFaceWeightCount; ++weight)
        {
            const float value = a_weights.values[weight];
            if (value > 0.0F && value <= 1.0F)
            {
                result.weights[weight] = static_cast<std::uint8_t>(std::lround(value * kFaceWeightSteps));
            }
        }
        return result;
    }

    [[nodiscard]] inline FaceWeights DequantizeFace(const QuantizedFacePose& a_face) noexcept
    {
        FaceWeights result;
        for (std::size_t weight = 0; weight < kFaceWeightCount; ++weight)
        {
            result.values[weight] = static_cast<float>(a_face.weights[weight]) / kFaceWeightSteps;
        }
        return result;
    }

    [[nodiscard]] inline EncodedFacePose EncodeFacePose(const QuantizedFacePose& a_face) noexcept
    {
        return Detail::EncodeMaskedFace(a_face, kFaceLayoutWhole, Detail::GetNonzeroFaceWeights(a_face));
    }

    // a_face as a change from a_reference when that is smaller, else whole.
    [[nodiscard]] inline EncodedFacePose EncodeFacePoseChange(
        const QuantizedFacePose& a_face, const QuantizedFacePose* a_reference) noexcept
    {
        const auto whole = Detail::GetNonzeroFaceWeights(a_face);
        if (!a_reference)
        {
            return Detail::EncodeMaskedFace(a_face, kFaceLayoutWhole, whole);
        }
        std::uint64_t changed = 0;
        for (std::size_t weight = 0; weight < kFaceWeightCount; ++weight)
        {
            if (a_face.weights[weight] != a_reference->weights[weight])
            {
                changed |= std::uint64_t{ 1 } << weight;
            }
        }
        return Detail::GetFacePoseSize(changed) < Detail::GetFacePoseSize(whole)
            ? Detail::EncodeMaskedFace(a_face, kFaceLayoutChange, changed)
            : Detail::EncodeMaskedFace(a_face, kFaceLayoutWhole, whole);
    }

    // The whole layout, or a change from a_reference; empty when a_bytes is empty or invalid.
    [[nodiscard]] inline std::optional<QuantizedFacePose> ResolveFacePose(
        std::span<const std::uint8_t> a_bytes, const QuantizedFacePose* a_reference) noexcept
    {
        if (a_bytes.empty())
        {
            return std::nullopt;
        }
        const bool change = a_bytes[0] == kFaceLayoutChange;
        if (change && !a_reference)
        {
            return std::nullopt;
        }
        std::uint64_t mask = 0;
        auto offset = Detail::ReadFaceHeader(a_bytes, change ? kFaceLayoutChange : kFaceLayoutWhole, mask);
        if (offset == 0 || a_bytes.size() != offset + static_cast<std::size_t>(std::popcount(mask)))
        {
            return std::nullopt;
        }
        QuantizedFacePose result = change ? *a_reference : QuantizedFacePose{};
        for (std::size_t weight = 0; weight < kFaceWeightCount; ++weight)
        {
            if (((mask >> weight) & 1) == 0)
            {
                continue;
            }
            const std::uint8_t value = a_bytes[offset++];
            if (change ? value == a_reference->weights[weight] : value == 0)
            {
                return std::nullopt;
            }
            result.weights[weight] = value;
        }
        return result;
    }

    [[nodiscard]] inline std::optional<QuantizedFacePose> ReadFacePose(std::span<const std::uint8_t> a_bytes) noexcept
    {
        return ResolveFacePose(a_bytes, nullptr);
    }
}
