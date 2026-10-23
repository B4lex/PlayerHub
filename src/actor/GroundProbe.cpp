#include "actor/GroundProbe.h"
#include "runtime/GameThread.h"

namespace GroundProbe
{
    namespace
    {
        // In Skyrim.esm's COLL records this layer collides with every walkable layer below;
        // L_LOS misses L_TRANSPARENT, and L_PATHINGPICK misses clutter and props.
        constexpr auto kRayLayer = RE::COL_LAYER::kCustomPick1;

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
                case RE::COL_LAYER::kGround:
                case RE::COL_LAYER::kTransparentSmall:
                case RE::COL_LAYER::kTransparentSmallAnim:
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
    }

    std::optional<Hit> FindGround(
        RE::TESObjectCELL& a_cell, const RE::NiPoint3& a_position, float a_above, float a_below)
    {
        if (!GameThread::Require("FindGround"))
        {
            return std::nullopt;
        }
        auto* world = a_cell.GetbhkWorld();
        if (!world)
        {
            return std::nullopt;
        }

        const float scale = RE::bhkWorld::GetWorldScale();
        const float top = a_position.z + a_above;
        const float bottom = a_position.z - a_below;
        RE::bhkPickData pick{};
        pick.rayInput.from = RE::hkVector4(a_position.x * scale, a_position.y * scale, top * scale, 0.0F);
        pick.rayInput.to = RE::hkVector4(a_position.x * scale, a_position.y * scale, bottom * scale, 0.0F);
        pick.rayInput.filterInfo.SetCollisionLayer(kRayLayer);
        WalkableHitCollector collector;
        pick.closestRayHitCollector = &collector;
        world->PickObject(pick);
        if (!collector.rayHit.HasHit())
        {
            return std::nullopt;
        }
        return Hit{ .z = top + (bottom - top) * collector.rayHit.hitFraction, .layer = collector.hitLayer };
    }
}
