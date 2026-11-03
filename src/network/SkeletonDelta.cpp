#include "network/SkeletonDelta.h"

#include <array>
#include <bit>
#include <cstddef>
#include <limits>

namespace Network
{
    namespace
    {
        constexpr std::uint32_t kGolombOrderBits = 2;
        constexpr std::uint32_t kMaximumGolombOrder = (1U << kGolombOrderBits) - 1;
        // A translation change takes at most 17; more cannot come from a valid pose.
        constexpr std::uint32_t kMaximumGolombZeros = 20;

        enum class BoneChange : std::uint8_t
        {
            kUnchanged,
            kDelta,
            kAbsolute
        };

        struct BonePlan
        {
            BoneChange change{ BoneChange::kAbsolute };
            std::uint32_t order{ 0 };
        };

        [[nodiscard]] constexpr std::uint32_t ZigZag(std::int32_t a_value) noexcept
        {
            return a_value >= 0 ? 2U * static_cast<std::uint32_t>(a_value)
                                : 2U * static_cast<std::uint32_t>(-static_cast<std::int64_t>(a_value)) - 1;
        }

        [[nodiscard]] constexpr std::int32_t UnZigZag(std::uint32_t a_value) noexcept
        {
            return (a_value & 1) != 0 ? -static_cast<std::int32_t>(a_value / 2) - 1 : static_cast<std::int32_t>(a_value / 2);
        }

        // Signed Exp-Golomb of order k: zeros, a one, then the code's lower bits.
        [[nodiscard]] constexpr std::uint32_t GetGolombBits(std::int32_t a_value, std::uint32_t a_order) noexcept
        {
            const auto width = static_cast<std::uint32_t>(std::bit_width(ZigZag(a_value) + (1U << a_order)));
            return 2 * width - 1 - a_order;
        }

        void WriteGolomb(Detail::BitWriter& a_writer, std::int32_t a_value, std::uint32_t a_order) noexcept
        {
            const auto code = ZigZag(a_value) + (1U << a_order);
            const auto width = static_cast<std::uint32_t>(std::bit_width(code));
            a_writer.Write(0, width - 1 - a_order);
            a_writer.Write(1, 1);
            a_writer.Write(code, width - 1);
        }

        [[nodiscard]] std::optional<std::int32_t> ReadGolomb(Detail::BitReader& a_reader, std::uint32_t a_order) noexcept
        {
            std::uint32_t zeros = 0;
            while (a_reader.Read(1) == 0)
            {
                if (++zeros > kMaximumGolombZeros)
                {
                    return std::nullopt;
                }
            }
            const auto lower = zeros + a_order;
            const auto code = (1U << lower) | a_reader.Read(lower);
            return UnZigZag(code - (1U << a_order));
        }

        template <std::size_t N>
        [[nodiscard]] std::uint32_t GetBestOrder(const std::array<std::int32_t, N>& a_values, std::uint32_t& a_bits) noexcept
        {
            std::uint32_t best = 0;
            a_bits = (std::numeric_limits<std::uint32_t>::max)();
            for (std::uint32_t order = 0; order <= kMaximumGolombOrder; ++order)
            {
                std::uint32_t bits = 0;
                for (const auto value : a_values)
                {
                    bits += GetGolombBits(value, order);
                }
                if (bits < a_bits)
                {
                    a_bits = bits;
                    best = order;
                }
            }
            return best;
        }

        [[nodiscard]] std::array<std::int32_t, 3> GetComponentChanges(
            const QuantizedSkeletonRotation& a_rotation, const QuantizedSkeletonRotation& a_reference) noexcept
        {
            std::array<std::int32_t, 3> result{};
            for (std::size_t i = 0; i < result.size(); ++i)
            {
                result[i] = static_cast<std::int32_t>(a_rotation.components[i])
                    - static_cast<std::int32_t>(a_reference.components[i]);
            }
            return result;
        }
    }

    EncodedSkeletonPose EncodeSkeletonPoseChange(
        const QuantizedSkeletonPose& a_pose, const QuantizedSkeletonPose* a_reference) noexcept
    {
        auto absolute = EncodeSkeletonPose(a_pose);
        if (a_reference == nullptr)
        {
            return absolute;
        }
        const auto present = a_pose.present & kAllSkeletonBones;
        std::array<std::int32_t, 3> translation{};
        for (std::size_t axis = 0; axis < translation.size(); ++axis)
        {
            translation[axis] = static_cast<std::int32_t>(a_pose.rootTranslation[axis])
                - static_cast<std::int32_t>(a_reference->rootTranslation[axis]);
        }
        std::uint32_t translationBits = 0;
        const auto translationOrder = GetBestOrder(translation, translationBits);
        std::size_t bits = kGolombOrderBits + translationBits;

        std::array<BonePlan, kSkeletonBoneCount> plans{};
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if (((present >> bone) & 1) == 0)
            {
                continue;
            }
            auto& plan = plans[bone];
            const auto& rotation = a_pose.rotations[bone];
            const auto& reference = a_reference->rotations[bone];
            // The changed and absolute flags, then the rotation as layout 2 stores it.
            std::uint32_t planBits = 2 + GetSkeletonRotationBits(bone);
            if (((a_reference->present >> bone) & 1) != 0)
            {
                if (rotation == reference)
                {
                    plan.change = BoneChange::kUnchanged;
                    planBits = 1;
                }
                else if (rotation.largest == reference.largest)
                {
                    std::uint32_t changeBits = 0;
                    const auto order = GetBestOrder(GetComponentChanges(rotation, reference), changeBits);
                    if (2 + kGolombOrderBits + changeBits < planBits)
                    {
                        plan = { BoneChange::kDelta, order };
                        planBits = 2 + kGolombOrderBits + changeBits;
                    }
                }
            }
            bits += planBits;
        }

        EncodedSkeletonPose result;
        Detail::WriteHeader(result, kSkeletonLayoutHumanChange, present);
        if (result.size + (bits + 7) / 8 >= absolute.size)
        {
            return absolute;
        }
        Detail::BitWriter writer{ result };
        writer.Write(translationOrder, kGolombOrderBits);
        for (const auto change : translation)
        {
            WriteGolomb(writer, change, translationOrder);
        }
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if (((present >> bone) & 1) == 0)
            {
                continue;
            }
            const auto& plan = plans[bone];
            const auto& rotation = a_pose.rotations[bone];
            switch (plan.change)
            {
                case BoneChange::kUnchanged:
                    writer.Write(0, 1);
                    break;
                case BoneChange::kDelta:
                    writer.Write(1, 1);
                    writer.Write(0, 1);
                    writer.Write(plan.order, kGolombOrderBits);
                    for (const auto change : GetComponentChanges(rotation, a_reference->rotations[bone]))
                    {
                        WriteGolomb(writer, change, plan.order);
                    }
                    break;
                case BoneChange::kAbsolute:
                    writer.Write(1, 1);
                    writer.Write(1, 1);
                    Detail::WriteRotation(writer, rotation, GetSkeletonComponentBits(bone));
                    break;
            }
        }
        writer.Flush();
        return result;
    }

    std::optional<QuantizedSkeletonPose> ResolveSkeletonPose(
        std::span<const std::uint8_t> a_bytes, const QuantizedSkeletonPose* a_reference) noexcept
    {
        if (a_bytes.empty())
        {
            return std::nullopt;
        }
        if (a_bytes[0] == kSkeletonLayoutHuman)
        {
            return ReadSkeletonPose(a_bytes);
        }
        if (a_reference == nullptr || a_bytes.size() > kMaximumSkeletonPoseBytes)
        {
            return std::nullopt;
        }
        QuantizedSkeletonPose result;
        const auto offset = Detail::ReadPresentBones(a_bytes, kSkeletonLayoutHumanChange, result.present);
        if (offset == 0)
        {
            return std::nullopt;
        }
        Detail::BitReader reader{ a_bytes, offset };
        const auto translationOrder = reader.Read(kGolombOrderBits);
        for (std::size_t axis = 0; axis < result.rootTranslation.size(); ++axis)
        {
            const auto change = ReadGolomb(reader, translationOrder);
            if (!change)
            {
                return std::nullopt;
            }
            const auto translation = static_cast<std::int64_t>(a_reference->rootTranslation[axis]) + *change;
            if (translation < (std::numeric_limits<std::int16_t>::min)()
                || translation > (std::numeric_limits<std::int16_t>::max)())
            {
                return std::nullopt;
            }
            result.rootTranslation[axis] = static_cast<std::int16_t>(translation);
        }
        for (std::size_t bone = 0; bone < kSkeletonBoneCount; ++bone)
        {
            if (((result.present >> bone) & 1) == 0)
            {
                continue;
            }
            const auto componentBits = GetSkeletonComponentBits(bone);
            const bool referenced = ((a_reference->present >> bone) & 1) != 0;
            if (reader.Read(1) == 0)
            {
                if (!referenced)
                {
                    return std::nullopt;
                }
                result.rotations[bone] = a_reference->rotations[bone];
                continue;
            }
            if (reader.Read(1) != 0)
            {
                result.rotations[bone] = Detail::ReadRotation(reader, componentBits);
                continue;
            }
            if (!referenced)
            {
                return std::nullopt;
            }
            auto rotation = a_reference->rotations[bone];
            const auto order = reader.Read(kGolombOrderBits);
            for (auto& component : rotation.components)
            {
                const auto change = ReadGolomb(reader, order);
                if (!change)
                {
                    return std::nullopt;
                }
                const auto value = static_cast<std::int64_t>(component) + *change;
                if (value < 0 || value > static_cast<std::int64_t>(Detail::GetComponentMaximum(componentBits)))
                {
                    return std::nullopt;
                }
                component = static_cast<std::uint16_t>(value);
            }
            result.rotations[bone] = rotation;
        }
        if (!reader.Finished())
        {
            return std::nullopt;
        }
        return result;
    }
}
