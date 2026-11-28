#pragma once

#include "VirtualMachineSettingsPage.g.h"

#include "NanaBox.Configuration.Specification.h"
#include "RdpClient.h"
#include "HostCompute.h"

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <vector>

namespace NanaBox
{
    class MainWindow;

    struct SettingsHostOperations
    {
        std::function<std::vector<HostNetworkInfo>()> EnumerateNetworks = EnumerateHostNetworks;
        std::function<HostNetworkInfo(winrt::guid const&)> QueryNetwork = QueryHostNetwork;
        std::function<void(winrt::hstring const&, bool, winrt::hstring const&)> CreateNetwork = CreateManagedNetwork;
        std::function<void(winrt::guid const&)> DeleteNetwork = DeleteManagedNetwork;
        std::function<winrt::hstring(winrt::hstring const&, winrt::hstring const&)> Manage = RunHostManagement;
        std::function<winrt::hstring(bool)> NetworkDiagnostics;
        std::function<std::string(std::string const&, std::wstring const&, std::string const&,
            bool, std::shared_ptr<std::atomic<bool>> const&)> GuestAction;
        std::function<winrt::hstring(winrt::hstring const&, std::wstring const&)> ExternalAction;
    };
}

namespace winrt::NanaBox
{
    using ::NanaBox::MainWindow;
}

namespace winrt
{
    using Windows::Foundation::IInspectable;
    using Windows::UI::Xaml::RoutedEventArgs;
    using Windows::UI::Xaml::Controls::CheckBox;
    using Windows::UI::Xaml::Controls::ComboBox;
    using Windows::UI::Xaml::Controls::StackPanel;
    using Windows::UI::Xaml::Controls::TextBox;
}

namespace winrt::NanaBox::implementation
{
    struct VirtualMachineSettingsPage :
        VirtualMachineSettingsPageT<VirtualMachineSettingsPage>
    {
    public:

        VirtualMachineSettingsPage(
            _In_ HWND WindowHandle = nullptr,
            std::wstring const& ConfigurationFilePath = std::wstring(),
            std::wstring const& Name = std::wstring(),
            _In_opt_ NanaBox::MainWindow* MainWindow = nullptr,
            std::function<std::string(std::string const&)> GuestQuery = {},
            std::shared_ptr<::NanaBox::SettingsHostOperations> HostOperations = {});

        ~VirtualMachineSettingsPage();

        void InitializeComponent();

        void ReloadButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void OpenButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void ReferenceButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void ValidateConfigurationButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);

        void ApplyButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void RefreshDriveButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void RefreshDeviceButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void GuestTypeSelectionChanged(winrt::IInspectable const&, winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void GuestToolsButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        winrt::fire_and_forget RunGuestToolsQuery(std::string Resource,
            std::function<std::string(std::string const&)> Query);
        void SetGuestToolsBusy(bool Busy, bool Cancellable = false);

        void NetworkDiagnosticsButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        winrt::fire_and_forget RunHostPreflight();
        void RefreshNetworksButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void CreateNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void DeleteNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void InspectNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void HostManagementButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void HcnResourceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void HcnKindChanged(winrt::IInspectable const&, winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void VmmsButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void VmmsSelectionChanged(winrt::IInspectable const&, winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void DiskManagerButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        winrt::fire_and_forget RunDiskManagerOperation(winrt::hstring Action, winrt::hstring Path, std::uint64_t Size);
        void SerialPipeButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);

        void AddGpuDeviceButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void AddNetworkAdapterButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void AddScsiDeviceButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void AddPlan9ShareButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

        void AddVirtualSmbShareButtonClick(
            winrt::IInspectable const& Sender,
            winrt::RoutedEventArgs const& Arguments);

    private:

        struct GpuDeviceSettings
        {
            winrt::StackPanel Container = nullptr;
            winrt::TextBox DeviceInterface = nullptr;
            winrt::TextBox PartitionId = nullptr;
        };

        struct NetworkAdapterSettings
        {
            winrt::StackPanel Container = nullptr;
            std::string EndpointId;
            winrt::CheckBox Connected = nullptr;
            winrt::TextBox MacAddress = nullptr;
            winrt::TextBox NetworkId = nullptr;
            winrt::ComboBox NetworkPicker = nullptr;
            winrt::TextBox FirewallCreatorId = nullptr;
            winrt::TextBox SuggestedSwitchId = nullptr;
            winrt::TextBox SuggestedSwitchName = nullptr;
            winrt::TextBox SuggestedSwitchSubnet = nullptr;
        };

        struct ScsiDeviceSettings
        {
            winrt::StackPanel Container = nullptr;
            winrt::ComboBox Type = nullptr;
            winrt::TextBox Path = nullptr;
        };

        struct Plan9ShareSettings
        {
            winrt::StackPanel Container = nullptr;
            winrt::CheckBox ReadOnly = nullptr;
            winrt::CheckBox AutoMount = nullptr;
            winrt::CheckBox LinuxMetadata = nullptr;
            winrt::CheckBox CaseSensitive = nullptr;
            winrt::CheckBox UseShareRootIdentity = nullptr;
            winrt::CheckBox AllowOptions = nullptr;
            winrt::CheckBox AllowSubPaths = nullptr;
            winrt::CheckBox UnlimitedConnections = nullptr;
            winrt::TextBox AllowedFiles = nullptr;
            winrt::TextBox Port = nullptr;
            winrt::TextBox Path = nullptr;
            winrt::TextBox Name = nullptr;
        };

        struct VirtualSmbShareSettings
        {
            winrt::StackPanel Container = nullptr;
            winrt::CheckBox ReadOnly = nullptr;
            winrt::CheckBox AutoMount = nullptr;
            winrt::CheckBox NoDirectmap = nullptr;
            std::map<std::string, winrt::ComboBox> Options;
            winrt::TextBox Path = nullptr;
            winrt::TextBox Name = nullptr;
        };

        std::shared_ptr<::NanaBox::SettingsHostOperations> m_HostOperations;
        bool m_GuestToolsRunning = false;
        std::shared_ptr<std::atomic<bool>> m_GuestCancellation;
        std::function<std::string(std::string const&)> m_GuestQuery;
        bool m_DiskOperationRunning = false;
        bool m_HostManagementRunning = false;
        bool m_HcnResourceRunning = false;
        winrt::fire_and_forget RunHcnResourceOperation(winrt::hstring Action, std::string Arguments);
        bool m_VmmsRunning = false;
        winrt::fire_and_forget RunVmmsOperation(winrt::hstring Action, std::string Arguments);
        bool m_NetworkManagementRunning = false;
        std::vector<::NanaBox::HostNetworkInfo> m_HostNetworks;
        void SetNetworkManagementBusy(bool Busy);
        winrt::fire_and_forget RunNetworkManagementOperation(winrt::hstring Action, winrt::hstring Value = {}, bool Firewall = false, winrt::hstring CustomSettings = {});
        winrt::fire_and_forget RunHostManagementOperation(winrt::hstring Action, std::string Arguments);
        HWND m_WindowHandle = nullptr;
        std::wstring m_ConfigurationFilePath;
        std::string m_LoadedConfigurationContent;
        NanaBox::VirtualMachineConfiguration m_Configuration;
        NanaBox::MainWindow* m_MainWindow = nullptr;
        bool m_ApplyPending = false;
        std::uint64_t m_ApplyGeneration = 0;
        winrt::com_ptr<NanaBox::RdpClient> m_EnumerationClient;
        std::vector<GpuDeviceSettings> m_GpuDeviceSettings;
        std::vector<NetworkAdapterSettings> m_NetworkAdapterSettings;
        std::vector<ScsiDeviceSettings> m_ScsiDeviceSettings;
        std::vector<Plan9ShareSettings> m_Plan9ShareSettings;
        std::vector<VirtualSmbShareSettings> m_VirtualSmbShareSettings;

        void ReloadConfiguration();
        void ShowConfigurationError();

        void UpdateWindowTitle();

        void PopulateControls();

        NanaBox::VirtualMachineConfiguration ReadConfiguration();

        void RefreshDrives();

        void RefreshDevices();

        void AddGpuDevice(
            std::string const& DeviceInterface,
            _In_ std::uint16_t PartitionId);

        void AddNetworkAdapter(
            NanaBox::NetworkAdapterConfiguration const& Configuration,
            _In_ bool Existing = false);

        void BrowseScsiDevicePath(
            winrt::ComboBox const& Type,
            winrt::TextBox const& Path);

        void AddScsiDevice(
            NanaBox::ScsiDeviceConfiguration const& Configuration,
            _In_ bool Existing = false);

        void AddPlan9Share(
            NanaBox::Plan9ShareConfiguration const& Configuration);

        void AddVirtualSmbShare(
            NanaBox::VirtualSmbShareConfiguration const& Configuration, bool Existing = false);
    };
}

namespace winrt::NanaBox::factory_implementation
{
    struct VirtualMachineSettingsPage : VirtualMachineSettingsPageT<
        VirtualMachineSettingsPage,
        implementation::VirtualMachineSettingsPage>
    {
    };
}
