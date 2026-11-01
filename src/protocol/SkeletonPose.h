#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Network
{
    // Bones of the vanilla human skeleton (meshes\actors\character\character
    // assets\skeleton.hkx), by name. Only the first one's translation travels.
    inline constexpr std::array<std::string_view, 63> kSkeletonBoneNames{ "NPC COM [COM ]", "NPC Pelvis [Pelv]",
        "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]", "NPC L Toe0 [LToe]", "NPC R Thigh [RThg]",
        "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC R Toe0 [RToe]", "NPC Spine [Spn0]", "NPC Spine1 [Spn1]",
        "NPC Spine2 [Spn2]", "NPC Neck [Neck]", "NPC Head [Head]", "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]",
        "NPC L Forearm [LLar]", "NPC L Hand [LHnd]", "NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]",
        "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", "NPC L ForearmTwist1 [LLt1]", "NPC L ForearmTwist2 [LLt2]",
        "NPC L UpperarmTwist1 [LUt1]", "NPC L UpperarmTwist2 [LUt2]", "NPC R ForearmTwist1 [RLt1]",
        "NPC R ForearmTwist2 [RLt2]", "NPC R UpperarmTwist1 [RUt1]", "NPC R UpperarmTwist2 [RUt2]",
        "NPC L Finger00 [LF00]", "NPC L Finger01 [LF01]", "NPC L Finger02 [LF02]", "NPC L Finger10 [LF10]",
        "NPC L Finger11 [LF11]", "NPC L Finger12 [LF12]", "NPC L Finger20 [LF20]", "NPC L Finger21 [LF21]",
        "NPC L Finger22 [LF22]", "NPC L Finger30 [LF30]", "NPC L Finger31 [LF31]", "NPC L Finger32 [LF32]",
        "NPC L Finger40 [LF40]", "NPC L Finger41 [LF41]", "NPC L Finger42 [LF42]", "NPC R Finger00 [RF00]",
        "NPC R Finger01 [RF01]", "NPC R Finger02 [RF02]", "NPC R Finger10 [RF10]", "NPC R Finger11 [RF11]",
        "NPC R Finger12 [RF12]", "NPC R Finger20 [RF20]", "NPC R Finger21 [RF21]", "NPC R Finger22 [RF22]",
        "NPC R Finger30 [RF30]", "NPC R Finger31 [RF31]", "NPC R Finger32 [RF32]", "NPC R Finger40 [RF40]",
        "NPC R Finger41 [RF41]", "NPC R Finger42 [RF42]", "Weapon", "Shield" };
    inline constexpr std::size_t kSkeletonBoneCount = kSkeletonBoneNames.size();
    static_assert(kSkeletonBoneCount < 64);
    inline constexpr std::size_t kSkeletonRootBone = 0;

    inline constexpr std::uint8_t kSkeletonLayoutHuman = 1;
    // Layout, presence mask, root translation.
    inline constexpr std::size_t kSkeletonHeaderBytes = 1 + 8 + 3 * 2;
    inline constexpr std::size_t kSkeletonRotationBytes = 4;
    inline constexpr std::size_t kMaximumSkeletonPoseBytes
        = kSkeletonHeaderBytes + kSkeletonRotationBytes * kSkeletonBoneCount;
    inline constexpr float kSkeletonTranslationScale = 16.0F;

    // A unit quaternion in Havok's component order.
    struct SkeletonRotation
    {
        float x{ 0.0F };
        float y{ 0.0F };
        float z{ 0.0F };
        float w{ 1.0F };
    };

    struct SkeletonPose
    {
        std::uint64_t present{ 0 };
        std::array<SkeletonRotation, kSkeletonBoneCount> rotations{};
        std::array<float, 3> rootTranslation{};
    };

    struct EncodedSkeletonPose
    {
        std::array<std::uint8_t, kMaximumSkeletonPoseBytes> bytes{};
        std::uint16_t size{ 0 };

        [[nodiscard]] bool empty() const noexcept { return size == 0; }
        [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return { bytes.data(), size }; }
    };

    namespace Detail
    {
        inline constexpr float kSqrtHalf = 0.70710678118654752440F;
        inline constexpr std::uint32_t kRotationComponentMaximum = 1023;

        inline std::uint32_t PackRotationComponent(float a_value) noexcept
        {
            const float unit = std::clamp((a_value / kSqrtHalf + 1.0F) * 0.5F, 0.0F, 1.0F);
            return static_cast<std::uint32_t>(std::lround(unit * kRotationComponentMaximum));
        }

        inline float UnpackRotationComponent(std::uint32_t a_value) noexcept
        {
            return (static_cast<float>(a_value) / kRotationComponentMaximum * 2.0F - 1.0F) * kSqrtHalf;
        }

        // Smallest three: the largest component's index in the top two bits, then the
        // other three in 10 bits each. The largest is made positive, since q and -q
        // are the same rotation, and restored from the unit length.
        inline std::uint32_t PackRotation(const SkeletonRotation& a_rotation) noexcept
        {
            std::array components{ a_rotation.x, a_rotation.y, a_rotation.z, a_rotation.w };
            float length = 0.0F;
            for (const float component : components)
            {
                length += component * component;
            }
            length = std::sqrt(length);
            if (!std::isfinite(length) || length < 1e-6F)
            {
                return 3U << 30 | PackRotationComponent(0.0F) << 20 | PackRotationComponent(0.0F) << 10
                    | PackRotationComponent(0.0F);
            }
            std::size_t largest = 0;
            for (std::size_t i = 1; i < components.size(); ++i)
            {
                if (std::abs(components[i]) > std::abs(components[largest]))
                {
                    largest = i;
                }
            }
            const float sign = components[largest] < 0.0F ? -1.0F : 1.0F;
            std::uint32_t bits = static_cast<std::uint32_t>(largest) << 30;
            std::uint32_t shift = 20;
            for (std::size_t i = 0; i < components.size(); ++i)
            {
                if (i != largest)
                {
                    bits |= PackRotationComponent(components[i] * sign / length) << shift;
                    shift -= 10;
                }
            }
            return bits;
        }

        inline SkeletonRotation UnpackRotation(std::uint32_t a_bits) noexcept
        {
            const auto largest = static_cast<std::size_t>(a_bits >> 30);
            std::array<float, 4> components{};
            float sum = 0.0F;
            std::uint32_t shift = 20;
            for (std::size_t i = 0; i < components.size(); ++i)
            {
                if (i != largest)
                {
                    components[i] = UnpackRotationComponent((a_bits >> shift) & kRotationComponentMaximum);
                    sum += components[i] * components[i];
                    shift -= 10;
                }
            }
            components[largest] = std::sqrt((std::max)(0.0F, 1.0F - sum));
            return SkeletonRotation{ components[0], components[1], components[2], components[3] };
        }

        template <class T>
        T ReadLittleEndian(std::span<const std::uint8_t> a_bytes, std::size_t a_offset) noexcept
        {
            std::make_unsigned_t<T> value = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                value |= static_cast<std::make_unsigned_t<T>>(a_bytes[a_offset + i]) << (8 * i);
            }
            return static_cast<T>(value);
        }

        template <class T>
        void WriteLittleEndian(EncodedSkeletonPose& a_pose, T a_value) noexcept
        {
            const auto value = static_cast<std::make_unsigned_t<T>>(a_value);
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                a_pose.bytes[a_pose.size++] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        }
    }

    [[nodiscard]] inline bool IsValidSkeletonPose(std::span<const std::uint8_t> a_bytes) noexcept
    {
        if (a_bytes.empty())
        {
            return true;
        }
        if (a_bytes.size() < kSkeletonHeaderBytes || a_bytes[0] != kSkeletonLayoutHuman)
        {
            return false;
        }
        const auto present = Detail::ReadLittleEndian<std::uint64_t>(a_bytes, 1);
        return (present >> kSkeletonBoneCount) == 0
            && a_bytes.size() == kSkeletonHeaderBytes + kSkeletonRotationBytes * std::popcount(present);
    }

    [[nodiscard]] inline EncodedSkeletonPose EncodeSkeletonPose(const SkeletonPose& a_pose) noexcept
    {
        EncodedSkeletonPose result;
        const auto present = a_pose.present & ((std::uint64_t{ 1 } << kSkeletonBoneCount) - 1);
        result.bytes[result.size++] = kSkeletonLayoutHuman;
        Detail::WriteLittleEndian(result, present);
        for (const float translation : a_pose.rootTranslation)
        {
            const float scaled = std::isfinite(translation) ? std::round(translation * kSkeletonTranslationScale) : 0.0F;
            Detail::WriteLittleEndian(result, static_cast<std::int16_t>(std::clamp(scaled, -32768.0F, 32767.0F)));
        }
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if ((present >> bone) & 1)
            {
                Detail::WriteLittleEndian(result, Detail::PackRotation(a_pose.rotations[bone]));
            }
        }
        return result;
    }

    // An empty or invalid encoding leaves no bone present.
    [[nodiscard]] inline SkeletonPose DecodeSkeletonPose(const EncodedSkeletonPose& a_encoded) noexcept
    {
        SkeletonPose result;
        const auto bytes = a_encoded.view();
        if (bytes.empty() || !IsValidSkeletonPose(bytes))
        {
            return result;
        }
        result.present = Detail::ReadLittleEndian<std::uint64_t>(bytes, 1);
        for (std::size_t axis = 0; axis < result.rootTranslation.size(); ++axis)
        {
            result.rootTranslation[axis] = static_cast<float>(Detail::ReadLittleEndian<std::int16_t>(bytes, 9 + 2 * axis))
                / kSkeletonTranslationScale;
        }
        std::size_t offset = kSkeletonHeaderBytes;
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if ((result.present >> bone) & 1)
            {
                result.rotations[bone] = Detail::UnpackRotation(Detail::ReadLittleEndian<std::uint32_t>(bytes, offset));
                offset += kSkeletonRotationBytes;
            }
        }
        return result;
    }

    // Along the shorter arc; a degenerate result keeps a_from.
    [[nodiscard]] inline SkeletonRotation NlerpRotation(
        const SkeletonRotation& a_from, SkeletonRotation a_to, float a_fraction) noexcept
    {
        if (a_from.x * a_to.x + a_from.y * a_to.y + a_from.z * a_to.z + a_from.w * a_to.w < 0.0F)
        {
            a_to = { -a_to.x, -a_to.y, -a_to.z, -a_to.w };
        }
        const SkeletonRotation result{ a_from.x + (a_to.x - a_from.x) * a_fraction,
            a_from.y + (a_to.y - a_from.y) * a_fraction, a_from.z + (a_to.z - a_from.z) * a_fraction,
            a_from.w + (a_to.w - a_from.w) * a_fraction };
        const float length
            = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
        if (!(length >= 1e-6F))
        {
            return a_from;
        }
        return { result.x / length, result.y / length, result.z / length, result.w / length };
    }
}
