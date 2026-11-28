/*
 * PROJECT:    NanaBox
 * FILE:       HostCompute.h
 * PURPOSE:    Definition for the Host Compute API C++/WinRT wrapper
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#ifndef NANABOX_HOSTCOMPUTE
#define NANABOX_HOSTCOMPUTE

#if (defined(__cplusplus) && __cplusplus >= 201703L)
#elif (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#else
#error "[HostCompute] You should use a C++ compiler with the C++17 standard."
#endif

#include <Windows.h>
#include <computecore.h>
#include <computenetwork.h>
#include <cstdint>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>

#include <Mile.Helpers.CppWinRT.h>

namespace NanaBox
{
    namespace Plan9ShareFlags
    {
        enum
        {
            None = 0x00000000,
            ReadOnly = 0x00000001,
            LinuxMetadata = 0x00000004,
            CaseSensitive = 0x00000008,
            UseShareRootIdentity = 0x00000010,
            AllowOptions = 0x00000020,
            AllowSubPaths = 0x00000040,
            RestrictFileAccess = 0x00000080,
            UnlimitedConnections = 0x00000100,
        };
    }

    struct HcsOperationTraits
    {
        using type = HCS_OPERATION;

        static void close(type value) noexcept
        {
            ::HcsCloseOperation(value);
        }

        static constexpr type invalid() noexcept
        {
            return nullptr;
        }
    };

    using HcsOperation = winrt::handle_type<HcsOperationTraits>;

    struct HcsSystemTraits
    {
        using type = HCS_SYSTEM;

        static void close(type value) noexcept
        {
            ::HcsCloseComputeSystem(value);
        }

        static constexpr type invalid() noexcept
        {
            return nullptr;
        }
    };

    using HcsSystem = winrt::handle_type<HcsSystemTraits>;

    struct HcnNetworkTraits
    {
        using type = HCN_NETWORK;

        static void close(type value) noexcept
        {
            ::HcnCloseNetwork(value);
        }

        static constexpr type invalid() noexcept
        {
            return nullptr;
        }
    };

    using HcnNetwork = winrt::handle_type<HcnNetworkTraits>;

    struct HcnEndpointTraits
    {
        using type = HCN_ENDPOINT;

        static void close(type value) noexcept
        {
            ::HcnCloseEndpoint(value);
        }

        static constexpr type invalid() noexcept
        {
            return nullptr;
        }
    };

    using HcnEndpoint = winrt::handle_type<HcnEndpointTraits>;

    struct ComputeSystem : winrt::implements<ComputeSystem, IUnknown>
    {
    public:

        ComputeSystem(
            winrt::hstring const& Id,
            winrt::hstring const& Configuration);

        ComputeSystem(
            winrt::hstring const& Id);

        void Start();

        void Shutdown();

        void Terminate();

        void Pause(
            winrt::hstring const& Options = winrt::hstring());

        void Resume();

        void Save(
            winrt::hstring const& Options);

        winrt::hstring GetProperties(
            winrt::hstring const& PropertyQuery = winrt::hstring());

        void Modify(
            winrt::hstring const& Configuration);

        Mile::WinRT::Event<winrt::delegate<winrt::hstring>> SystemExited;
        Mile::WinRT::Event<winrt::delegate<>> SystemRdpEnhancedModeStateChanged;

    private:

        HcsOperation m_Operation;
        HcsSystem m_ComputeSystem;

        static void CALLBACK ComputeSystemCallback(
            HCS_EVENT* Event,
            void* Context);
    };

    winrt::hstring HcsGetServiceProperties(
        winrt::hstring const& PropertyQuery = winrt::hstring());

    const winrt::guid DefaultSwitchId = winrt::guid(
        "C08CB7B8-9B3C-408E-8E30-5E16A3AEB444");

    const winrt::guid NanaBoxSwitchId = winrt::guid(
        "85C8668E-36FE-447F-AAE0-BA159A0142A7");

    namespace HcnNetworkFlags
    {
        enum
        {
            None = 0,
            EnableDns = 1,
            EnableDhcp = 2,
            EnableMirroring = 4,
            EnableNonPersistent = 8,
            EnablePersistent = 16,
            IsolateVSwitch = 32,
            EnableFlowSteering = 64,
            DisableSharing = 128,
            EnableFirewall = 256,
            SuppressMediaDisconnect = 512,
            DisableHostPort = 1024,
            WeakHostReceiveAdapter = 2048,
            WeakHostSendAdapter = 4096,
            EnableIov = 8192,
        };
    }

    HcnNetwork HcnCreateNetwork(
        winrt::guid const& NetworkId,
        winrt::hstring const& Settings);

    HcnNetwork HcnOpenNetwork(
        winrt::guid const& NetworkId);

    struct HostNetworkInfo
    {
        winrt::guid Id;
        winrt::hstring Name;
        winrt::hstring Type;
        std::uint32_t Flags = 0;
        winrt::hstring Properties;
    };

    struct HostGpuInfo
    {
        winrt::hstring Name;
        winrt::hstring DeviceInterface;
    };

    std::vector<HostGpuInfo> EnumeratePartitionableGpus();
    std::vector<HostNetworkInfo> EnumerateHostNetworks();
    HostNetworkInfo QueryHostNetwork(winrt::guid const& Id);
    winrt::hstring BuildManagedNetworkSettings(winrt::hstring const& Name, bool Firewall,
        winrt::hstring const& CustomSettings = {});
    void CreateManagedNetwork(winrt::hstring const& Name, bool Firewall,
        winrt::hstring const& CustomSettings);
    void DeleteManagedNetwork(winrt::guid const& Id);
    winrt::hstring RunHostManagement(winrt::hstring const& Action, winrt::hstring const& Arguments);
    bool HostManagementSelfTest();

    HcnEndpoint HcnCreateEndpoint(
        HcnNetwork const& NetworkHandle,
        winrt::guid const& EndpointId,
        winrt::hstring const& Settings);

    void HcnDeleteEndpoint(
        winrt::guid const& EndpointId);

    // Return an empty handle if the endpoint does not exist; reject foreign ownership.
    HcnEndpoint HcnOpenOwnedEndpoint(winrt::guid const& EndpointId, std::string const& Owner);
    void HcnDeleteOwnedEndpoint(winrt::guid const& EndpointId, std::string const& Owner);
    bool IsEndpointOwnedBy(winrt::hstring const& Properties, std::string const& Owner);

    winrt::hstring HcnQueryEndpointProperties(
        HcnEndpoint const& EndpointHandle,
        winrt::hstring const& Query = winrt::hstring());
}

namespace winrt::NanaBox
{
    using namespace ::NanaBox;
}

#endif // !NANABOX_HOSTCOMPUTE
