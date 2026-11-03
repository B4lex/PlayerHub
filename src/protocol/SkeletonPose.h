#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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

    inline constexpr std::uint8_t kSkeletonLayoutHuman = 2;
    // The change from the pose its stream last carried (network/SkeletonDelta).
    inline constexpr std::uint8_t kSkeletonLayoutHumanChange = 3;

    inline constexpr std::size_t kSkeletonFirstTwistBone = 23;
    inline constexpr std::size_t kSkeletonFirstFingerBone = 31;
    inline constexpr std::size_t kSkeletonFirstHeldBone = 61;
    static_assert(kSkeletonBoneNames[kSkeletonFirstTwistBone] == "NPC L ForearmTwist1 [LLt1]");
    static_assert(kSkeletonBoneNames[kSkeletonFirstFingerBone] == "NPC L Finger00 [LF00]");
    static_assert(kSkeletonBoneNames[kSkeletonFirstHeldBone] == "Weapon");

    // Bits per stored quaternion component, by bone. A rotation error in the trunk and limbs
    // moves everything below them, so they keep the finest steps.
    [[nodiscard]] constexpr std::uint32_t GetSkeletonComponentBits(std::size_t a_bone) noexcept
    {
        if (a_bone < kSkeletonFirstTwistBone)
        {
            return 9;
        }
        if (a_bone >= kSkeletonFirstFingerBone && a_bone < kSkeletonFirstHeldBone)
        {
            return 7;
        }
        return 8;
    }

    [[nodiscard]] constexpr std::uint32_t GetSkeletonRotationBits(std::size_t a_bone) noexcept
    {
        return 2 + 3 * GetSkeletonComponentBits(a_bone);
    }

    inline constexpr std::size_t kSkeletonTranslationBytes = 3 * 2;
    // A varint of the 63-bit mask.
    inline constexpr std::size_t kMaximumSkeletonMaskBytes = 9;
    inline constexpr std::size_t kMaximumSkeletonPoseBytes = [] {
        std::size_t bits = 0;
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            bits += GetSkeletonRotationBits(bone);
        }
        return 1 + kMaximumSkeletonMaskBytes + kSkeletonTranslationBytes + (bits + 7) / 8;
    }();
    static_assert(kMaximumSkeletonPoseBytes == 219);
    inline constexpr float kSkeletonTranslationScale = 16.0F;
    inline constexpr std::uint64_t kAllSkeletonBones = (std::uint64_t{ 1 } << kSkeletonBoneCount) - 1;

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

    // Smallest three: the index of the largest component, made positive since q and -q are the
    // same rotation, and the other three in GetSkeletonComponentBits steps.
    struct QuantizedSkeletonRotation
    {
        std::uint8_t largest{ 0 };
        std::array<std::uint16_t, 3> components{};

        [[nodiscard]] bool operator==(const QuantizedSkeletonRotation&) const = default;
    };

    // A pose as layout 2 stores it; rotations of absent bones are unspecified.
    struct QuantizedSkeletonPose
    {
        std::uint64_t present{ 0 };
        std::array<std::int16_t, 3> rootTranslation{};
        std::array<QuantizedSkeletonRotation, kSkeletonBoneCount> rotations{};
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

        [[nodiscard]] constexpr std::uint32_t GetComponentMaximum(std::uint32_t a_bits) noexcept
        {
            return (1U << a_bits) - 1;
        }

        inline std::uint16_t PackRotationComponent(float a_value, std::uint32_t a_bits) noexcept
        {
            const float unit = std::clamp((a_value / kSqrtHalf + 1.0F) * 0.5F, 0.0F, 1.0F);
            return static_cast<std::uint16_t>(std::lround(unit * static_cast<float>(GetComponentMaximum(a_bits))));
        }

        inline float UnpackRotationComponent(std::uint32_t a_value, std::uint32_t a_bits) noexcept
        {
            return (static_cast<float>(a_value) / static_cast<float>(GetComponentMaximum(a_bits)) * 2.0F - 1.0F)
                * kSqrtHalf;
        }

        // Least significant bit first, within and across bytes. At most 24 bits per Write.
        class BitWriter
        {
        public:
            explicit BitWriter(EncodedSkeletonPose& a_pose) noexcept : pose_(a_pose) {}

            void Write(std::uint32_t a_value, std::uint32_t a_bits) noexcept
            {
                pending_ |= static_cast<std::uint64_t>(a_value & ((1U << a_bits) - 1)) << count_;
                count_ += a_bits;
                while (count_ >= 8)
                {
                    pose_.bytes[pose_.size++] = static_cast<std::uint8_t>(pending_);
                    pending_ >>= 8;
                    count_ -= 8;
                }
            }

            // The unused bits of the last byte stay zero.
            void Flush() noexcept
            {
                if (count_ > 0)
                {
                    pose_.bytes[pose_.size++] = static_cast<std::uint8_t>(pending_);
                    pending_ = 0;
                    count_ = 0;
                }
            }

        private:
            EncodedSkeletonPose& pose_;
            std::uint64_t pending_{ 0 };
            std::uint32_t count_{ 0 };
        };

        // Reads zero bits past the end; Finished tells whether it read exactly the bytes there are.
        class BitReader
        {
        public:
            BitReader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset) noexcept :
                bytes_(a_bytes),
                next_(a_offset)
            {}

            std::uint32_t Read(std::uint32_t a_bits) noexcept
            {
                while (count_ < a_bits)
                {
                    const std::uint64_t byte = next_ < bytes_.size() ? bytes_[next_] : 0;
                    ++next_;
                    pending_ |= byte << count_;
                    count_ += 8;
                }
                const auto value = static_cast<std::uint32_t>(pending_ & ((std::uint64_t{ 1 } << a_bits) - 1));
                pending_ >>= a_bits;
                count_ -= a_bits;
                return value;
            }

            // Every byte read, none past the end, and the padding of the last one zero.
            [[nodiscard]] bool Finished() const noexcept
            {
                return next_ == bytes_.size() && count_ < 8 && pending_ == 0;
            }

        private:
            std::span<const std::uint8_t> bytes_;
            std::size_t next_;
            std::uint64_t pending_{ 0 };
            std::uint32_t count_{ 0 };
        };

        inline QuantizedSkeletonRotation QuantizeRotation(const SkeletonRotation& a_rotation, std::uint32_t a_bits) noexcept
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
                components = { 0.0F, 0.0F, 0.0F, 1.0F };
                length = 1.0F;
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
            QuantizedSkeletonRotation result{ .largest = static_cast<std::uint8_t>(largest) };
            std::size_t stored = 0;
            for (std::size_t i = 0; i < components.size(); ++i)
            {
                if (i != largest)
                {
                    result.components[stored++] = PackRotationComponent(components[i] * sign / length, a_bits);
                }
            }
            return result;
        }

        // The largest component is rebuilt from the unit length.
        inline SkeletonRotation DequantizeRotation(const QuantizedSkeletonRotation& a_rotation, std::uint32_t a_bits) noexcept
        {
            std::array<float, 4> components{};
            float sum = 0.0F;
            std::size_t stored = 0;
            for (std::size_t i = 0; i < components.size(); ++i)
            {
                if (i != a_rotation.largest)
                {
                    components[i] = UnpackRotationComponent(a_rotation.components[stored++], a_bits);
                    sum += components[i] * components[i];
                }
            }
            components[a_rotation.largest] = std::sqrt((std::max)(0.0F, 1.0F - sum));
            return SkeletonRotation{ components[0], components[1], components[2], components[3] };
        }

        inline void WriteRotation(BitWriter& a_writer, const QuantizedSkeletonRotation& a_rotation, std::uint32_t a_bits) noexcept
        {
            a_writer.Write(a_rotation.largest, 2);
            for (const auto component : a_rotation.components)
            {
                a_writer.Write(component, a_bits);
            }
        }

        inline QuantizedSkeletonRotation ReadRotation(BitReader& a_reader, std::uint32_t a_bits) noexcept
        {
            QuantizedSkeletonRotation result{ .largest = static_cast<std::uint8_t>(a_reader.Read(2)) };
            for (auto& component : result.components)
            {
                component = static_cast<std::uint16_t>(a_reader.Read(a_bits));
            }
            return result;
        }

        // Unsigned LEB128 of at most 63 bits, in its shortest form only.
        inline std::size_t ReadVarint(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, std::uint64_t& a_value) noexcept
        {
            a_value = 0;
            for (std::size_t i = 0; i < kMaximumSkeletonMaskBytes && a_offset + i < a_bytes.size(); ++i)
            {
                const std::uint8_t byte = a_bytes[a_offset + i];
                a_value |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * i);
                if ((byte & 0x80) == 0)
                {
                    return i > 0 && byte == 0 ? 0 : i + 1;
                }
            }
            return 0;
        }

        inline void WriteVarint(EncodedSkeletonPose& a_pose, std::uint64_t a_value) noexcept
        {
            while (a_value >= 0x80)
            {
                a_pose.bytes[a_pose.size++] = static_cast<std::uint8_t>(a_value | 0x80);
                a_value >>= 7;
            }
            a_pose.bytes[a_pose.size++] = static_cast<std::uint8_t>(a_value);
        }

        [[nodiscard]] inline std::size_t GetRotationBytes(std::uint64_t a_present) noexcept
        {
            std::size_t bits = 0;
            for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
            {
                if ((a_present >> bone) & 1)
                {
                    bits += GetSkeletonRotationBits(bone);
                }
            }
            return (bits + 7) / 8;
        }

        // The offset after the mask of missing bones, or 0 when the header is invalid.
        inline std::size_t ReadPresentBones(
            std::span<const std::uint8_t> a_bytes, std::uint8_t a_layout, std::uint64_t& a_present) noexcept
        {
            if (a_bytes.empty() || a_bytes[0] != a_layout)
            {
                return 0;
            }
            std::uint64_t missing = 0;
            const auto maskBytes = ReadVarint(a_bytes, 1, missing);
            if (maskBytes == 0 || (missing & ~kAllSkeletonBones) != 0)
            {
                return 0;
            }
            a_present = kAllSkeletonBones & ~missing;
            return 1 + maskBytes;
        }

        inline void WriteHeader(EncodedSkeletonPose& a_pose, std::uint8_t a_layout, std::uint64_t a_present) noexcept
        {
            a_pose.bytes[a_pose.size++] = a_layout;
            WriteVarint(a_pose, kAllSkeletonBones & ~a_present);
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
        std::uint64_t present = 0;
        const auto translation = Detail::ReadPresentBones(a_bytes, kSkeletonLayoutHuman, present);
        return translation != 0
            && a_bytes.size() == translation + kSkeletonTranslationBytes + Detail::GetRotationBytes(present);
    }

    [[nodiscard]] inline QuantizedSkeletonPose QuantizeSkeletonPose(const SkeletonPose& a_pose) noexcept
    {
        QuantizedSkeletonPose result{ .present = a_pose.present & kAllSkeletonBones };
        for (std::size_t axis = 0; axis < result.rootTranslation.size(); ++axis)
        {
            const float translation = a_pose.rootTranslation[axis];
            const float scaled = std::isfinite(translation) ? std::round(translation * kSkeletonTranslationScale) : 0.0F;
            result.rootTranslation[axis] = static_cast<std::int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
        }
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if ((result.present >> bone) & 1)
            {
                result.rotations[bone] = Detail::QuantizeRotation(a_pose.rotations[bone], GetSkeletonComponentBits(bone));
            }
        }
        return result;
    }

    [[nodiscard]] inline SkeletonPose DequantizeSkeletonPose(const QuantizedSkeletonPose& a_pose) noexcept
    {
        SkeletonPose result{ .present = a_pose.present };
        for (std::size_t axis = 0; axis < result.rootTranslation.size(); ++axis)
        {
            result.rootTranslation[axis] = static_cast<float>(a_pose.rootTranslation[axis]) / kSkeletonTranslationScale;
        }
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if ((a_pose.present >> bone) & 1)
            {
                result.rotations[bone] = Detail::DequantizeRotation(a_pose.rotations[bone], GetSkeletonComponentBits(bone));
            }
        }
        return result;
    }

    // Layout 2.
    [[nodiscard]] inline EncodedSkeletonPose EncodeSkeletonPose(const QuantizedSkeletonPose& a_pose) noexcept
    {
        EncodedSkeletonPose result;
        const auto present = a_pose.present & kAllSkeletonBones;
        Detail::WriteHeader(result, kSkeletonLayoutHuman, present);
        for (const auto translation : a_pose.rootTranslation)
        {
            Detail::WriteLittleEndian(result, translation);
        }
        Detail::BitWriter writer{ result };
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if ((present >> bone) & 1)
            {
                Detail::WriteRotation(writer, a_pose.rotations[bone], GetSkeletonComponentBits(bone));
            }
        }
        writer.Flush();
        return result;
    }

    [[nodiscard]] inline EncodedSkeletonPose EncodeSkeletonPose(const SkeletonPose& a_pose) noexcept
    {
        return EncodeSkeletonPose(QuantizeSkeletonPose(a_pose));
    }

    // Layout 2 only; empty when a_bytes is empty or invalid.
    [[nodiscard]] inline std::optional<QuantizedSkeletonPose> ReadSkeletonPose(std::span<const std::uint8_t> a_bytes) noexcept
    {
        if (a_bytes.empty() || !IsValidSkeletonPose(a_bytes))
        {
            return std::nullopt;
        }
        QuantizedSkeletonPose result;
        const auto translation = Detail::ReadPresentBones(a_bytes, kSkeletonLayoutHuman, result.present);
        for (std::size_t axis = 0; axis < result.rootTranslation.size(); ++axis)
        {
            result.rootTranslation[axis] = Detail::ReadLittleEndian<std::int16_t>(a_bytes, translation + 2 * axis);
        }
        Detail::BitReader reader{ a_bytes, translation + kSkeletonTranslationBytes };
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if ((result.present >> bone) & 1)
            {
                result.rotations[bone] = Detail::ReadRotation(reader, GetSkeletonComponentBits(bone));
            }
        }
        return result;
    }

    // An empty or invalid encoding leaves no bone present.
    [[nodiscard]] inline SkeletonPose DecodeSkeletonPose(const EncodedSkeletonPose& a_encoded) noexcept
    {
        const auto pose = ReadSkeletonPose(a_encoded.view());
        return pose ? DequantizeSkeletonPose(*pose) : SkeletonPose{};
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
