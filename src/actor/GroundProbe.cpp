#include "actor/GroundProbe.h"
#include "runtime/GameThread.h"

#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace GroundProbe
{
    namespace
    {
        // In Skyrim.esm's COLL records only L_CHARCONTROLLER collides with L_STAIRHELPER, the
        // ramps characters walk on over stairs; the pick layers pass through them.
        constexpr auto kRayLayer = RE::COL_LAYER::kCharController;
        constexpr float kSupportTolerance = 8.0F;
        constexpr float kSupportRadius = 24.0F;
        constexpr float kDiagonal = 0.70710678118654752440F;
        constexpr std::array<std::pair<float, float>, 8> kSupportDirections{ { { 1.0F, 0.0F }, { kDiagonal, kDiagonal },
            { 0.0F, 1.0F }, { -kDiagonal, kDiagonal }, { -1.0F, 0.0F }, { -kDiagonal, -kDiagonal }, { 0.0F, -1.0F },
            { kDiagonal, -kDiagonal } } };

        bool IsWalkable(RE::COL_LAYER a_layer)
        {
            switch (a_layer)
            {
                case RE::COL_LAYER::kStatic:
                case RE::COL_LAYER::kAnimStatic:
                case RE::COL_LAYER::kTransparent:
                case RE::COL_LAYER::kClutter:
                case RE::COL_LAYER::kTrees:
                case RE::COL_LAYER::kProps:
                case RE::COL_LAYER::kTerrain:
                case RE::COL_LAYER::kTrap:
                case RE::COL_LAYER::kGround:
                case RE::COL_LAYER::kDebrisLarge:
                case RE::COL_LAYER::kTransparentSmall:
                case RE::COL_LAYER::kInvisibleWall:
                case RE::COL_LAYER::kTransparentSmallAnim:
                case RE::COL_LAYER::kStairHelper:
                    return true;
                default:
                    return false;
            }
        }

        // bhkWorld::PickObject (vfunc 0x33, ID 77860) resets this collector's hit, casts
        // through hkpWorld::castRay with a collector (ID 61400), and copies rayHit into the
        // pick's output once rootCollidable is set. A rejected hit leaves
        // earlyOutHitFraction alone, so the ray continues through water and characters.
        class WalkableHitCollector final : public RE::hkpClosestRayHitCollector
        {
        public:
            void AddRayHit(const RE::hkpCdBody& a_body, const RE::hkpShapeRayCastCollectorOutput& a_hitInfo) override
            {
                if (a_hitInfo.hitFraction >= earlyOutHitFraction)
                {
                    return;
                }
                const RE::hkpCdBody* root = &a_body;
                while (root->parent)
                {
                    root = root->parent;
                }
                const auto* collidable = static_cast<const RE::hkpCollidable*>(root);
                const auto layer = collidable->GetCollisionLayer();
                if (!IsWalkable(layer))
                {
                    return;
                }
                rayHit.normal = a_hitInfo.normal;
                rayHit.hitFraction = a_hitInfo.hitFraction;
                rayHit.extraInfo = a_hitInfo.extraInfo;
                rayHit.shapeKey = a_hitInfo.shapeKey;
                rayHit.rootCollidable = collidable;
                earlyOutHitFraction = a_hitInfo.hitFraction;
                hitLayer = layer;
            }

            RE::COL_LAYER hitLayer{ RE::COL_LAYER::kUnidentified };
        };

        std::optional<Hit> CastRay(RE::bhkWorld& a_world, float a_x, float a_y, float a_top, float a_bottom)
        {
            const float scale = RE::bhkWorld::GetWorldScale();
            RE::bhkPickData pick{};
            pick.rayInput.from = RE::hkVector4(a_x * scale, a_y * scale, a_top * scale, 0.0F);
            pick.rayInput.to = RE::hkVector4(a_x * scale, a_y * scale, a_bottom * scale, 0.0F);
            pick.rayInput.filterInfo.SetCollisionLayer(kRayLayer);
            WalkableHitCollector collector;
            pick.closestRayHitCollector = &collector;
            a_world.PickObject(pick);
            if (!collector.rayHit.HasHit())
            {
                return std::nullopt;
            }
            return Hit{ .z = a_top + (a_bottom - a_top) * collector.rayHit.hitFraction, .layer = collector.hitLayer };
        }
    }

    Result FindSupport(RE::TESObjectCELL& a_cell, const RE::NiPoint3& a_position, float a_above, float a_below)
    {
        Result result;
        if (!GameThread::Require("FindSupport"))
        {
            return result;
        }
        auto* world = a_cell.GetbhkWorld();
        if (!world)
        {
            return result;
        }

        const float top = a_position.z + a_above;
        const float bottom = a_position.z - a_below;
        result.ground = CastRay(*world, a_position.x, a_position.y, top, bottom);
        ++result.rays;
        if (result.ground && result.ground->z >= a_position.z - kSupportTolerance)
        {
            return result;
        }

        // A character controller's capsule rests on an edge its center has passed, so a
        // ray through the center falls past a deck it stands on.
        float bestDistance
            = result.ground ? std::abs(result.ground->z - a_position.z) : std::numeric_limits<float>::infinity();
        for (const auto& [directionX, directionY] : kSupportDirections)
        {
            const auto hit = CastRay(*world, a_position.x + directionX * kSupportRadius,
                a_position.y + directionY * kSupportRadius, top, bottom);
            ++result.rays;
            if (hit && std::abs(hit->z - a_position.z) < bestDistance)
            {
                bestDistance = std::abs(hit->z - a_position.z);
                result.ground = hit;
                result.edge = true;
            }
        }
        return result;
    }
}
