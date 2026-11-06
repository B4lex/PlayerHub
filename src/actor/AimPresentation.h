#pragma once

struct RemoteActor;

namespace AimPresentation
{
    void InstallHooks();

    void SetLookPitch(RemoteActor& a_remote, float a_pitch);
}
