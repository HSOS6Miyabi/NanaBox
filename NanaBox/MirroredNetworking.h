#pragma once

#include "NanaBox.Configuration.Specification.h"
#include <memory>
#include <string>
#include <winrt/base.h>

namespace NanaBox
{
    // Owns the HCN guest network service and the endpoints hot-added to one VM.
    class MirroredNetworking
    {
    public:
        MirroredNetworking(VirtualMachineConfiguration const& Configuration, winrt::guid const& RuntimeId);
        ~MirroredNetworking();
        MirroredNetworking(MirroredNetworking const&) = delete;
        MirroredNetworking& operator=(MirroredNetworking const&) = delete;
        std::wstring Status() const;
        void PauseSynchronization();
        void ResumeSynchronization();
        void DetachForSave();
        void VmExited() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
