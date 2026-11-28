#pragma once

#include <exception>
#include <functional>

namespace NanaBox
{
    template<typename Operation>
    inline void RequestVmStop(bool Restart, bool& Pending, bool& Restarting, Operation&& Stop)
    {
        Stop();
        Restarting = Restart;
        Pending = true;
    }

    struct SaveStateOperations
    {
        std::function<void()> ResumeVm, PauseVm, DetachNetwork, RestoreNetwork,
            DisableGpu, RestoreGpu, Save, Publish, Unpublish, RemoveSave, Terminate;
        std::function<void(char const*)> RollbackError;
    };

    // Every successful transition is paired with its inverse. The saved file
    // remains intact if publishing succeeded but the configuration cannot be restored.
    inline void SaveStateTransaction(bool InitiallyPaused, bool HasGpu, bool HasMirror,
        SaveStateOperations const& Operations)
    {
        bool Paused = InitiallyPaused, Detached = false, GpuDisabled = false;
        bool SaveAttempted = false, Published = false;
        try
        {
            if (Paused && (HasGpu || HasMirror)) { Operations.ResumeVm(); Paused = false; }
            if (HasMirror) { Operations.DetachNetwork(); Detached = true; }
            if (HasGpu) { Operations.DisableGpu(); GpuDisabled = true; }
            if (!Paused) { Operations.PauseVm(); Paused = true; }
            SaveAttempted = true;
            Operations.Save();
            Operations.Publish();
            Published = true;
            Operations.Terminate();
        }
        catch (...)
        {
            auto Original = std::current_exception();
            auto Recover = [&](std::function<void()> const& Operation, char const* Name)
            {
                try { Operation(); return true; }
                catch (...)
                {
                    try { Operations.RollbackError(Name); } catch (...) {}
                    return false;
                }
            };
            if (Published && Recover(Operations.Unpublish, "Restore configuration"))
                Published = false;
            if (SaveAttempted && !Published) Recover(Operations.RemoveSave, "Remove incomplete saved state");
            if (Paused && (!InitiallyPaused || Detached || GpuDisabled))
            {
                if (Recover(Operations.ResumeVm, "Resume virtual machine")) Paused = false;
            }
            if (GpuDisabled) Recover(Operations.RestoreGpu, "Restore GPU assignment");
            if (Detached) Recover(Operations.RestoreNetwork, "Restore mirrored network");
            if (InitiallyPaused && !Paused)
                Recover(Operations.PauseVm, "Restore paused state");
            std::rethrow_exception(Original);
        }
    }
}
