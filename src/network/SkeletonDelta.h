#pragma once

#include "protocol/SkeletonPose.h"

#include <cstdint>
#include <optional>
#include <span>

namespace Network
{
    // a_pose as layout 3 against a_reference when that is smaller, else as layout 2.
    [[nodiscard]] EncodedSkeletonPose EncodeSkeletonPoseChange(
        const QuantizedSkeletonPose& a_pose, const QuantizedSkeletonPose* a_reference) noexcept;

    // Reads layout 2, or layout 3 against a_reference; empty when a_bytes is empty or invalid.
    [[nodiscard]] std::optional<QuantizedSkeletonPose> ResolveSkeletonPose(
        std::span<const std::uint8_t> a_bytes, const QuantizedSkeletonPose* a_reference) noexcept;
}
