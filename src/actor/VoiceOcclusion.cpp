#include "actor/VoiceOcclusion.h"
#include "runtime/GameThread.h"

namespace VoiceOcclusion
{
    namespace
    {
        // GroundProbe's ray layer: it passes through the stair ramps that only character controllers hit.
        constexpr auto kRayLayer = RE::COL_LAYER::kCharController;

        bool Blocks(RE::COL_LAYER a_layer)
        {
            switch (a_layer)
            {
                case RE::COL_LAYER::kStatic:
                case RE::COL_LAYER::kAnimStatic:
                case RE::COL_LAYER::kTerrain:
                case RE::COL_LAYER::kGround:
                case RE::COL_LAYER::kProps:
                    return true;
                default:
                    return false;
            }
        }

        // As GroundProbe's collector: bhkWorld::PickObject (ID 77860) keeps the closest accepted hit,
        // and a rejected one leaves the ray going.
        class BlockingHitCollector final : public RE::hkpClosestRayHitCollector
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
                if (!Blocks(collidable->GetCollisionLayer()))
                {
                    return;
                }
                rayHit.normal = a_hitInfo.normal;
                rayHit.hitFraction = a_hitInfo.hitFraction;
                rayHit.extraInfo = a_hitInfo.extraInfo;
                rayHit.shapeKey = a_hitInfo.shapeKey;
                rayHit.rootCollidable = collidable;
                earlyOutHitFraction = a_hitInfo.hitFraction;
            }
        };
    }

    bool IsBlocked(RE::TESObjectCELL& a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
    {
        if (!GameThread::Require("VoiceOcclusion::IsBlocked"))
        {
            return false;
        }
        auto* world = a_cell.GetbhkWorld();
        if (!world)
        {
            return false;
        }
        const float scale = RE::bhkWorld::GetWorldScale();
        RE::bhkPickData pick{};
        pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0F);
        pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0F);
        pick.rayInput.filterInfo.SetCollisionLayer(kRayLayer);
        BlockingHitCollector collector;
        pick.closestRayHitCollector = &collector;
        world->PickObject(pick);
        return collector.rayHit.HasHit();
    }
}
