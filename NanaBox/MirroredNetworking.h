#pragma once

#include "NanaBox.Configuration.Specification.h"
#include <memory>
#include <atomic>
#include <string>
#include <winrt/base.h>

namespace NanaBox
{
    std::string CheckMirroredHostSupport();
    std::string QueryGuestTools(winrt::guid const& RuntimeId, std::string const& Resource);
    std::string RunGuestToolsAction(winrt::guid const& RuntimeId, std::string const& Action,
        std::wstring const& LocalPath, std::string const& GuestName, bool Overwrite,
        std::shared_ptr<std::atomic<bool>> const& Cancellation = {});
    bool GuestToolsSelfTest();
    void PrepareGuestPlan9ShareRemoval(winrt::guid const& RuntimeId, std::string const& Name, std::uint32_t Port);
    class Plan9MountManager
    {
    public:
        Plan9MountManager(winrt::guid const& RuntimeId, std::string Manifest);
        ~Plan9MountManager();
        Plan9MountManager(Plan9MountManager const&) = delete;
        Plan9MountManager& operator=(Plan9MountManager const&) = delete;
        void Update(std::string Manifest);
    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
    // Owns the HCN guest network service and the endpoints hot-added to one VM.
    class MirroredNetworking
    {
    public:
        MirroredNetworking(VirtualMachineConfiguration const& Configuration, winrt::guid const& RuntimeId);
        ~MirroredNetworking();
        MirroredNetworking(MirroredNetworking const&) = delete;
        MirroredNetworking& operator=(MirroredNetworking const&) = delete;
        std::wstring Status() const;
        std::string Diagnostics() const;
        void RequestRefresh() noexcept;
        void PauseSynchronization();
        void ResumeSynchronization();
        void DetachForSave();
        void VmExited() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
