#include "sync/customanimation/GraphTrace.h"

#ifndef NDEBUG
#include "actor/GraphProbe.h"
#include "actor/GraphVocabulary.h"
#include "runtime/FrameTime.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GraphTrace
{
    namespace
    {
        constexpr std::size_t kMaximumQueuedEvents = 256;
        constexpr float kWindowSeconds = 3.0F;
        constexpr double kLeadSeconds = 1.5;
        constexpr std::size_t kMaximumLeadLines = 128;

        struct KnownMachine
        {
            const RE::hkbStateMachine* machine{ nullptr };
            std::int32_t stateId{ 0 };
            std::string name;
            std::string stateName;
        };

        struct LeadLine
        {
            double time{ 0.0 };
            std::string text;
        };

        std::mutex s_lock;
        std::vector<std::string> s_queued;
        std::size_t s_droppedEvents{ 0 };

        std::vector<std::string> s_drained;
        GraphProbe::TraceSample s_sample;
        const RE::hkbBehaviorGraph* s_graph{ nullptr };
        std::uint32_t s_activeGraph{ 0 };
        std::vector<std::string> s_moddedEvents;
        std::vector<KnownMachine> s_machines;
        std::vector<std::uint32_t> s_words;
        std::string s_runEvent;
        std::uint32_t s_runRepeats{ 0 };
        double s_clock{ 0.0 };
        float s_windowRemaining{ 0.0F };
        std::deque<LeadLine> s_lead;
        std::vector<std::string> s_lines;

        bool IsTracked(RE::hkbVariableInfo::VariableType a_type)
        {
            using enum RE::hkbVariableInfo::VariableType;
            return a_type == VARIABLE_TYPE_BOOL || a_type == VARIABLE_TYPE_INT8 || a_type == VARIABLE_TYPE_INT16
                || a_type == VARIABLE_TYPE_INT32;
        }

        std::int32_t ToInteger(RE::hkbVariableInfo::VariableType a_type, std::uint32_t a_word)
        {
            using enum RE::hkbVariableInfo::VariableType;
            switch (a_type)
            {
                case VARIABLE_TYPE_BOOL:
                    return a_word != 0 ? 1 : 0;
                case VARIABLE_TYPE_INT8:
                    return static_cast<std::int8_t>(a_word);
                case VARIABLE_TYPE_INT16:
                    return static_cast<std::int16_t>(a_word);
                default:
                    return static_cast<std::int32_t>(a_word);
            }
        }

        bool IsModdedEvent(std::string_view a_name)
        {
            return std::ranges::any_of(
                s_moddedEvents, [&](const std::string& a_modded) { return AsciiCase::Equals(a_modded, a_name); });
        }

        void RememberMachines()
        {
            s_machines.clear();
            for (const auto& machine : s_sample.machines)
            {
                s_machines.push_back({ .machine = machine.machine,
                    .stateId = machine.stateId,
                    .name = machine.name,
                    .stateName = machine.stateName });
            }
        }

        void RememberWords()
        {
            s_words.clear();
            for (const auto& variable : s_sample.variables)
            {
                s_words.push_back(variable.word);
            }
        }

        void LogGraphChange()
        {
            std::string moddedEvents;
            for (const auto& name : s_moddedEvents)
            {
                std::format_to(std::back_inserter(moddedEvents), "{}{}", moddedEvents.empty() ? "" : " ", name);
            }
            std::string moddedVariables;
            for (auto index = static_cast<std::size_t>(GraphVocabulary::kVanillaVariableCount);
                index < s_sample.variables.size(); ++index)
            {
                std::format_to(std::back_inserter(moddedVariables), "{}{}", moddedVariables.empty() ? "" : " ",
                    s_sample.variables[index].name);
            }
            logs::debug("Graph trace graph changed: graph={} vanillaPrefix={} moddedEvents=[{}] moddedVariables=[{}]",
                s_activeGraph, s_sample.vanillaPrefix, moddedEvents, moddedVariables);
        }

        void FlushRun()
        {
            if (s_runRepeats > 0)
            {
                s_lines.push_back(std::format("event={} repeated {} more times", s_runEvent, s_runRepeats));
            }
            s_runRepeats = 0;
        }

        void AddEvents(bool& a_trigger)
        {
            for (auto& name : s_drained)
            {
                const bool modded = IsModdedEvent(name);
                a_trigger |= modded;
                if (!s_runEvent.empty() && AsciiCase::Equals(name, s_runEvent))
                {
                    ++s_runRepeats;
                    continue;
                }
                FlushRun();
                s_lines.push_back(std::format("event={}{}", name, modded ? " modded" : ""));
                s_runEvent = std::move(name);
            }
            s_drained.clear();
        }

        void AddMachineChanges()
        {
            bool changed = s_machines.size() != s_sample.machines.size();
            for (const auto& current : s_sample.machines)
            {
                const auto known = std::ranges::find(s_machines, current.machine, &KnownMachine::machine);
                if (known == s_machines.end() || known->name != current.name)
                {
                    s_lines.push_back(std::format("machine {} active in {}", current.name, current.stateName));
                    changed = true;
                }
                else if (known->stateId != current.stateId)
                {
                    s_lines.push_back(
                        std::format("machine {}: {} -> {}", current.name, known->stateName, current.stateName));
                    changed = true;
                }
            }
            for (const auto& known : s_machines)
            {
                const auto current
                    = std::ranges::find(s_sample.machines, known.machine, &GraphProbe::TraceMachine::machine);
                if (current == s_sample.machines.end() || known.name != current->name)
                {
                    s_lines.push_back(std::format("machine {} inactive, was in {}", known.name, known.stateName));
                    changed = true;
                }
            }
            if (changed)
            {
                RememberMachines();
            }
        }

        void AddVariableChanges(bool& a_trigger)
        {
            const auto count = (std::min)(s_words.size(), s_sample.variables.size());
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto& variable = s_sample.variables[index];
                if (variable.word == s_words[index] || !IsTracked(variable.type))
                {
                    continue;
                }
                const bool modded = index >= static_cast<std::size_t>(GraphVocabulary::kVanillaVariableCount);
                a_trigger |= modded;
                s_lines.push_back(std::format("variable {}: {} -> {}{}", variable.name,
                    ToInteger(variable.type, s_words[index]), ToInteger(variable.type, variable.word),
                    modded ? " modded" : ""));
            }
            RememberWords();
        }

        void Emit(bool a_trigger)
        {
            while (!s_lead.empty() && s_clock - s_lead.front().time > kLeadSeconds)
            {
                s_lead.pop_front();
            }
            if (a_trigger)
            {
                if (s_windowRemaining <= 0.0F)
                {
                    logs::debug("Graph trace opened");
                    for (const auto& line : s_lead)
                    {
                        logs::debug("Graph trace: [{:.2f} s] {}", line.time - s_clock, line.text);
                    }
                    s_lead.clear();
                }
                s_windowRemaining = kWindowSeconds;
            }
            for (auto& text : s_lines)
            {
                if (s_windowRemaining > 0.0F)
                {
                    logs::debug("Graph trace: {}", text);
                    continue;
                }
                s_lead.push_back({ .time = s_clock, .text = std::move(text) });
                if (s_lead.size() > kMaximumLeadLines)
                {
                    s_lead.pop_front();
                }
            }
            s_lines.clear();
        }

        void Close()
        {
            if (s_runRepeats > 0)
            {
                logs::debug("Graph trace: event={} repeated {} more times", s_runEvent, s_runRepeats);
            }
            s_runRepeats = 0;
            s_runEvent.clear();
            logs::debug("Graph trace closed");
        }
    }

    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName)
    {
        const std::string_view name{ a_eventName };
        if (name.empty())
        {
            return;
        }
        const std::scoped_lock lock{ s_lock };
        if (s_queued.size() >= kMaximumQueuedEvents)
        {
            ++s_droppedEvents;
            return;
        }
        s_queued.emplace_back(name);
    }

    void Update(const LocalFrame& a_local, float a_delta)
    {
        const float delta = FrameTime::Clamp(a_delta);
        s_clock += delta;
        if (s_windowRemaining > 0.0F)
        {
            s_windowRemaining -= delta;
            if (s_windowRemaining <= 0.0F)
            {
                Close();
            }
        }

        std::size_t dropped = 0;
        {
            const std::scoped_lock lock{ s_lock };
            s_drained.swap(s_queued);
            dropped = std::exchange(s_droppedEvents, 0);
        }
        if (dropped > 0)
        {
            s_lines.push_back(std::format("dropped {} events over the queue limit", dropped));
        }

        if (!GraphProbe::ReadTrace(a_local.player, s_sample))
        {
            s_graph = nullptr;
            s_drained.clear();
            s_lines.clear();
            return;
        }
        if (s_sample.graph != s_graph || s_sample.activeGraph != s_activeGraph)
        {
            s_graph = s_sample.graph;
            s_activeGraph = s_sample.activeGraph;
            s_moddedEvents = std::move(s_sample.moddedEventNames);
            s_runEvent.clear();
            s_runRepeats = 0;
            RememberMachines();
            RememberWords();
            LogGraphChange();
        }
        if (!s_sample.vanillaPrefix)
        {
            s_drained.clear();
            s_lines.clear();
            return;
        }

        bool trigger = false;
        AddEvents(trigger);
        AddMachineChanges();
        AddVariableChanges(trigger);
        Emit(trigger);
    }
}
#endif
