#pragma once

#include <RE/Skyrim.h>

namespace DynamicForm
{
    [[nodiscard]] bool AssignFormID(RE::TESForm& a_form);

    template <class T>
    [[nodiscard]] T* Create()
    {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<T>();
        auto* form = factory ? factory->Create() : nullptr;
        return form && AssignFormID(*form) ? form : nullptr;
    }
}
