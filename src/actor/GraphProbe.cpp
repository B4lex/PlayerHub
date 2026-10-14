#include "actor/GraphProbe.h"

#ifndef NDEBUG
#include "actor/BehaviorGraphLayout.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <format>
#include <iterator>

namespace GraphProbe
{
    namespace
    {
        using namespace BehaviorGraphLayout;

        // hkbVariableValueSet stores its word-sized values (bool, int, real) in the
        // hkArray that follows hkReferencedObject, indexed like
        // hkbBehaviorGraphData::variableInfos.
        constexpr std::size_t kWordValuesOffset = 0x10;
        static_assert(sizeof(RE::hkReferencedObject) == kWordValuesOffset);

        void AppendActiveNodes(std::string& a_out, const RE::hkbBehaviorGraph& a_behavior)
        {
            std::string clips;
            a_out += " states=[";
            bool firstState = true;
            if (a_behavior.activeNodes)
            {
                for (const auto& nodeInfo : *a_behavior.activeNodes)
                {
                    auto* node = nodeInfo.nodeClone ? nodeInfo.nodeClone : nodeInfo.nodeTemplate;
                    if (!node)
                    {
                        continue;
                    }
                    if (const auto* machine = skyrim_cast<RE::hkbStateMachine*>(node))
                    {
                        std::format_to(std::back_inserter(a_out), "{}{}>{}", firstState ? "" : " ",
                            GetName(machine->name), GetCurrentStateName(*machine));
                        firstState = false;
                    }
                    else if (const auto* clip = skyrim_cast<RE::hkbClipGenerator*>(node))
                    {
                        std::format_to(
                            std::back_inserter(clips), "{}{}", clips.empty() ? "" : " ", GetName(clip->animationName));
                    }
                }
            }
            else
            {
                a_out += "unavailable";
            }
            std::format_to(std::back_inserter(a_out), "] clips=[{}]", clips);
        }

        void AppendVariable(std::string& a_out, const char* a_separator, const char* a_name,
            RE::hkbVariableInfo::VariableType a_type, std::uint32_t a_word)
        {
            using enum RE::hkbVariableInfo::VariableType;
            auto out = std::back_inserter(a_out);
            switch (a_type)
            {
                case VARIABLE_TYPE_BOOL:
                    std::format_to(out, "{}{}={}", a_separator, a_name, a_word != 0 ? 1 : 0);
                    break;
                case VARIABLE_TYPE_INT8:
                    std::format_to(out, "{}{}={}", a_separator, a_name, static_cast<std::int8_t>(a_word));
                    break;
                case VARIABLE_TYPE_INT16:
                    std::format_to(out, "{}{}={}", a_separator, a_name, static_cast<std::int16_t>(a_word));
                    break;
                case VARIABLE_TYPE_INT32:
                    std::format_to(out, "{}{}={}", a_separator, a_name, static_cast<std::int32_t>(a_word));
                    break;
                case VARIABLE_TYPE_REAL:
                    std::format_to(out, "{}{}={:.3g}", a_separator, a_name, std::bit_cast<float>(a_word));
                    break;
                default:
                    break;
            }
        }

        void AppendVariables(std::string& a_out, const RE::hkbBehaviorGraph& a_behavior, Snapshot& a_snapshot)
        {
            const auto* data = a_behavior.data.get();
            const auto* strings = data ? data->stringData.get() : nullptr;
            const auto* valueSet = a_behavior.variableValueSet.get();
            if (!strings || !valueSet)
            {
                a_out += " variables=unavailable";
                return;
            }

            const auto& words = ReadAt<RE::hkArray<std::uint32_t>>(valueSet, kWordValuesOffset);
            const auto count = (std::min)({ words.size(), data->variableInfos.size(), strings->variableNames.size() });
            const bool full
                = a_snapshot.graph != &a_behavior || a_snapshot.words.size() != static_cast<std::size_t>(count);
            std::format_to(std::back_inserter(a_out), " variables{}=[", full ? "(all)" : "(changed)");
            const auto start = a_out.size();
            for (std::int32_t index = 0; index < count; ++index)
            {
                if (full || words[index] != a_snapshot.words[index])
                {
                    AppendVariable(a_out, a_out.size() == start ? "" : " ", GetName(strings->variableNames[index]),
                        data->variableInfos[index].m_type.get(), words[index]);
                }
            }
            a_out += ']';
            a_snapshot.graph = &a_behavior;
            a_snapshot.words.assign(words.begin(), words.begin() + count);
        }
    }

    std::string Describe(RE::Actor& a_actor, Snapshot& a_snapshot)
    {
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!a_actor.GetAnimationGraphManager(manager) || !manager)
        {
            return "graph=unavailable";
        }

        RE::BSSpinLockGuard lock{ manager->GetRuntimeData().updateLock };
        const auto* behavior = GetActiveBehavior(*manager);
        if (!behavior)
        {
            return "graph=unavailable";
        }

        std::string description = std::format("graph={}", manager->GetRuntimeData().activeGraph);
        AppendActiveNodes(description, *behavior);
        AppendVariables(description, *behavior, a_snapshot);
        return description;
    }
}
#endif
