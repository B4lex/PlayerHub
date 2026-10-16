#include "forms/DynamicForm.h"

namespace DynamicForm
{
    bool AssignFormID(RE::TESForm& a_form)
    {
        if (a_form.GetFormID() != 0)
        {
            return true;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        const auto formID = dataHandler ? dataHandler->GetNextID() : 0;
        if (formID == 0)
        {
            return false;
        }
        a_form.SetFormID(formID, false);
        return a_form.GetFormID() == formID;
    }
}
