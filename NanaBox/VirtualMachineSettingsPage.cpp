#include "pch.h"
#include "VirtualMachineSettingsPage.h"
#if __has_include("VirtualMachineSettingsPage.g.cpp")
#include "VirtualMachineSettingsPage.g.cpp"
#endif

#include "Utils.h"
#include "ConfigurationManager.h"
#include "NanaBoxResources.h"
#include "MainWindow.h"

#include <algorithm>
#include <charconv>
#include <cwchar>
#include <map>
#include <sstream>
#include <string_view>
#include <set>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <virtdisk.h>
#pragma comment(lib, "virtdisk.lib")
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

namespace winrt
{
    using Windows::UI::Core::CoreDispatcherPriority;
    using Windows::UI::Xaml::Visibility;
    using Windows::UI::Xaml::ThicknessHelper;
    using Windows::UI::Xaml::TextWrapping;
    using Windows::UI::Xaml::FrameworkElement;
    using Windows::UI::Xaml::GridLengthHelper;
    using Windows::UI::Xaml::HorizontalAlignment;
    using Windows::UI::Xaml::VerticalAlignment;
    using Windows::UI::Xaml::Automation::AutomationProperties;
    using Windows::UI::Xaml::Controls::ColumnDefinition;
    using Windows::UI::Xaml::Controls::Grid;
    using Windows::UI::Xaml::Controls::SelectionChangedEventArgs;
    using Windows::UI::Xaml::Controls::TextBlock;
    using Windows::UI::Xaml::Controls::ToolTipService;
}

namespace
{
    winrt::hstring GetSettingsString(
        _In_ LPCWSTR Name)
    {
        std::wstring ResourcePath = L"VirtualMachineSettingsPage/";
        ResourcePath.append(Name);
        return Mile::WinRT::GetLocalizedString(winrt::hstring(ResourcePath));
    }

    void ShowSettingsError(
        _In_ HWND WindowHandle)
    {
        ::ShowErrorMessageDialog(WindowHandle, Mile::WinRT::ToHResultError());
    }

    std::wstring GetRelativeFilePath(
        std::wstring const& ConfigurationFilePath,
        std::wstring const& FilePath)
    {
        if (!ConfigurationFilePath.empty() && !FilePath.empty() &&
            !::PathIsRelativeW(FilePath.c_str()))
        {
            std::wstring RelativePath(MAX_PATH, L'\0');
            if (::PathRelativePathToW(
                &RelativePath[0],
                ConfigurationFilePath.c_str(),
                FILE_ATTRIBUTE_NORMAL,
                FilePath.c_str(),
                FILE_ATTRIBUTE_NORMAL))
            {
                RelativePath.resize(std::wcslen(RelativePath.c_str()));
                return RelativePath;
            }
        }
        return FilePath;
    }

    void InvalidSetting(
        winrt::hstring const& Name)
    {
        throw winrt::hresult_invalid_argument(winrt::hstring(
            Mile::FormatWideString(
                ::GetSettingsString(L"InvalidValue").c_str(),
                Name.c_str())));
    }

    template <typename ValueType>
    ValueType ReadSettingInteger(
        winrt::TextBox const& Input)
    {
        std::string Text = winrt::to_string(Input.Text());
        ValueType Value = 0;
        const char* End = Text.data() + Text.size();
        std::from_chars_result Result = std::from_chars(
            Text.data(),
            End,
            Value);
        if (Result.ec != std::errc() || Result.ptr != End)
        {
            ::InvalidSetting(winrt::AutomationProperties::GetName(Input));
        }
        return Value;
    }

    std::int32_t ReadSettingIndex(
        winrt::ComboBox const& Input)
    {
        std::int32_t Index = Input.SelectedIndex();
        if (Index < 0 ||
            static_cast<std::uint32_t>(Index) >= Input.Items().Size())
        {
            ::InvalidSetting(winrt::AutomationProperties::GetName(Input));
        }
        return Index;
    }

    std::vector<std::string> ReadSettingLines(
        winrt::TextBox const& Input)
    {
        std::vector<std::string> Result;
        std::string Text = winrt::to_string(Input.Text());
        std::replace(Text.begin(), Text.end(), '\r', '\n');
        std::istringstream Stream(Text);
        std::string Line;
        while (std::getline(Stream, Line))
        {
            if (!Line.empty())
            {
                Result.push_back(Line);
            }
        }
        return Result;
    }

    winrt::hstring MakeSettingLines(
        std::vector<std::string> const& Values)
    {
        std::string Result;
        for (std::string const& Value : Values)
        {
            if (!Result.empty())
            {
                Result.push_back('\n');
            }
            Result.append(Value);
        }
        return winrt::to_hstring(Result);
    }

    winrt::Grid AddSettingRow(
        winrt::StackPanel const& Panel,
        winrt::FrameworkElement const& Input,
        _In_ LPCWSTR LabelResource,
        _In_ LPCWSTR HelpResource,
        _In_ bool Visible)
    {
        winrt::Grid Row;
        Row.Visibility(Visible
            ? winrt::Visibility::Visible
            : winrt::Visibility::Collapsed);
        double LabelColumnWidth = 250 - Panel.Margin().Left;
        winrt::ColumnDefinition LabelColumn;
        LabelColumn.Width(winrt::GridLengthHelper::FromPixels(LabelColumnWidth));
        Row.ColumnDefinitions().Append(LabelColumn);
        winrt::ColumnDefinition SpacingColumn;
        SpacingColumn.Width(winrt::GridLengthHelper::FromPixels(12));
        Row.ColumnDefinitions().Append(SpacingColumn);
        Row.ColumnDefinitions().Append(winrt::ColumnDefinition());

        winrt::TextBlock Label;
        Label.Text(::GetSettingsString(LabelResource));
        Label.MinWidth(LabelColumnWidth);
        Label.TextWrapping(winrt::TextWrapping::Wrap);
        Label.VerticalAlignment(winrt::VerticalAlignment::Center);
        Row.Children().Append(Label);

        winrt::hstring Help = ::GetSettingsString(HelpResource);
        winrt::AutomationProperties::SetName(Input, Label.Text());
        winrt::AutomationProperties::SetAutomationId(Input, winrt::hstring(LabelResource));
        winrt::AutomationProperties::SetHelpText(Input, Help);
        winrt::ToolTipService::SetToolTip(Input, winrt::box_value(Help));
        winrt::Grid::SetColumn(Input, 2);
        Row.Children().Append(Input);
        Panel.Children().Append(Row);
        return Row;
    }

    winrt::TextBox AddSettingTextBox(
        winrt::StackPanel const& Panel,
        _In_ LPCWSTR LabelResource,
        _In_ LPCWSTR HelpResource,
        winrt::hstring const& Value,
        _In_ bool Hidden)
    {
        winrt::TextBox Input;
        Input.Text(Value);
        Input.MinWidth(250);
        Input.HorizontalAlignment(winrt::HorizontalAlignment::Stretch);
        ::AddSettingRow(Panel, Input, LabelResource, HelpResource, !Hidden);
        return Input;
    }

    winrt::CheckBox AddSettingCheckBox(
        winrt::StackPanel const& Panel,
        _In_ LPCWSTR LabelResource,
        _In_ LPCWSTR HelpResource,
        _In_ bool Value,
        _In_ bool Hidden)
    {
        winrt::CheckBox Input;
        Input.IsChecked(Value);
        Input.HorizontalAlignment(winrt::HorizontalAlignment::Left);
        ::AddSettingRow(Panel, Input, LabelResource, HelpResource, !Hidden);
        return Input;
    }

    winrt::ComboBox MakeHostPicker(
        winrt::StackPanel const& Panel, winrt::TextBox const& Target,
        LPCWSTR Label, LPCWSTR Help)
    {
        winrt::ComboBox Picker;
        Picker.HorizontalAlignment(winrt::HorizontalAlignment::Stretch);
        ::AddSettingRow(Panel, Picker, Label, Help, true);
        Picker.SelectionChanged([WeakTarget = winrt::make_weak(Target)](
            winrt::IInspectable const& Sender, winrt::SelectionChangedEventArgs const&)
        {
            auto Item = Sender.as<winrt::ComboBox>().SelectedItem();
            if (auto Text = WeakTarget.get())
                if (Item) Text.Text(winrt::unbox_value<winrt::hstring>(
                    Item.as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>().Tag()));
        });
        return Picker;
    }

    void AddPickerItem(winrt::ComboBox const& Picker, winrt::hstring const& Name,
        winrt::hstring const& Value)
    {
        winrt::Windows::UI::Xaml::Controls::ComboBoxItem Item;
        Item.Content(winrt::box_value(Name));
        Item.Tag(winrt::box_value(Value));
        winrt::ToolTipService::SetToolTip(Item, winrt::box_value(Value));
        Picker.Items().Append(Item);
    }

    void FillNetworkPicker(winrt::ComboBox const& Picker, std::vector<::NanaBox::HostNetworkInfo> const& Networks, bool AttachableOnly = false)
    {
        Picker.Items().Clear();
        for (auto const& Network : Networks)
        {
            bool Mirrored = (Network.Flags & NanaBox::HcnNetworkFlags::EnableFlowSteering) != 0;
            if (AttachableOnly && Mirrored) continue;
            auto Id = ::FromGuid(Network.Id);
            std::wstring Label(Network.Name.c_str());
            Label += L" (";
            Label += Network.Type.c_str();
            if (Mirrored) Label += L", Mirrored";
            Label += L")";
            ::AddPickerItem(Picker, winrt::hstring(Label), Id);
        }
        Picker.PlaceholderText(Picker.Items().Size() ? winrt::hstring() : ::GetSettingsString(L"NoHostNetworks"));
    }

    std::vector<std::string> ReadRedirectionSelection(
        winrt::StackPanel const& Panel)
    {
        std::vector<std::string> Result;
        for (winrt::IInspectable const& Child : Panel.Children())
        {
            winrt::CheckBox Choice = Child.as<winrt::CheckBox>();
            if (Choice.IsChecked().Value())
            {
                Result.push_back(winrt::to_string(
                    winrt::unbox_value<winrt::hstring>(Choice.Tag())));
            }
        }
        return Result;
    }

    void PopulateRedirectionChoices(
        winrt::StackPanel const& Panel,
        std::map<std::string, winrt::hstring> const& Available,
        std::vector<std::string> const& Selected)
    {
        std::map<std::string, winrt::hstring> Choices = Available;
        for (std::string const& Key : Selected)
        {
            if (Choices.end() == Choices.find(Key))
            {
                Choices[Key] = winrt::hstring(Mile::FormatWideString(
                    ::GetSettingsString(L"Unavailable").c_str(),
                    winrt::to_hstring(Key).c_str()));
            }
        }

        winrt::StackPanel Updated;
        for (auto const& Choice : Choices)
        {
            winrt::CheckBox Input;
            Input.Content(winrt::box_value(Choice.second));
            Input.Tag(winrt::box_value(winrt::to_hstring(Choice.first)));
            Input.IsChecked(Selected.end() != std::find(
                Selected.begin(), Selected.end(), Choice.first));
            Updated.Children().Append(Input);
        }
        Panel.Children().Clear();
        while (Updated.Children().Size())
        {
            auto Child = Updated.Children().GetAt(0);
            Updated.Children().RemoveAt(0);
            Panel.Children().Append(Child);
        }
    }
}

namespace winrt::NanaBox::implementation
{
    VirtualMachineSettingsPage::VirtualMachineSettingsPage(
        _In_ HWND WindowHandle,
        std::wstring const& ConfigurationFilePath,
        std::wstring const& Name,
        _In_opt_ NanaBox::MainWindow* MainWindow,
        std::function<std::string(std::string const&)> GuestQuery,
        std::shared_ptr<::NanaBox::SettingsHostOperations> HostOperations) :
        m_HostOperations(HostOperations ? std::move(HostOperations) : std::make_shared<::NanaBox::SettingsHostOperations>()),
        m_GuestQuery(std::move(GuestQuery)),
        m_WindowHandle(WindowHandle),
        m_ConfigurationFilePath(ConfigurationFilePath),
        m_MainWindow(MainWindow)
    {
        this->m_Configuration.Name = winrt::to_string(Name);
        if (ConfigurationFilePath.empty())
        {
            this->m_Configuration.GuestType = NanaBox::GuestType::Windows;
            this->m_Configuration.ProcessorCount = 2;
            this->m_Configuration.MemorySize = 2048;
            NanaBox::NetworkAdapterConfiguration NetworkAdapter;
            NetworkAdapter.Connected = true;
            this->m_Configuration.NetworkAdapters.push_back(NetworkAdapter);
        }
        this->UpdateWindowTitle();

        HICON ApplicationIconHandle = reinterpret_cast<HICON>(::LoadImageW(
            ::GetModuleHandleW(nullptr),
            MAKEINTRESOURCE(IDI_NANABOX),
            IMAGE_ICON,
            256,
            256,
            LR_SHARED));
        if (ApplicationIconHandle)
        {
            ::SendMessageW(
                this->m_WindowHandle,
                WM_SETICON,
                ICON_BIG,
                reinterpret_cast<LPARAM>(ApplicationIconHandle));
            ::SendMessageW(
                this->m_WindowHandle,
                WM_SETICON,
                ICON_SMALL,
                reinterpret_cast<LPARAM>(ApplicationIconHandle));
        }
    }

    VirtualMachineSettingsPage::~VirtualMachineSettingsPage() = default;

    void VirtualMachineSettingsPage::ReloadConfiguration()
    {
        auto Content = ::ReadAllTextFromUtf8TextFile(this->m_ConfigurationFilePath);
        auto Configuration = NanaBox::DeserializeConfiguration(Content);
        this->m_LoadedConfigurationContent = std::move(Content);
        this->m_Configuration = Configuration;
        this->PopulateControls();
        this->ConfigurationStatusTextBox().Text(::GetSettingsString(L"ConfigurationLoaded"));
    }

    void VirtualMachineSettingsPage::ShowConfigurationError()
    {
        this->ConfigurationStatusTextBox().Text(Mile::WinRT::ToHResultError().message());
    }

    void VirtualMachineSettingsPage::UpdateWindowTitle()
    {
        LPCWSTR TitleResource = this->m_ConfigurationFilePath.empty()
            ? L"CreateTitle"
            : L"Title";
        if (!this->m_Configuration.Name.empty())
        {
            TitleResource = this->m_ConfigurationFilePath.empty()
                ? L"NamedCreateTitle"
                : L"NamedTitle";
        }
        winrt::hstring Name = winrt::to_hstring(this->m_Configuration.Name);
        ::SetWindowTextW(
            this->m_WindowHandle,
            Mile::FormatWideString(
                ::GetSettingsString(TitleResource).c_str(),
                Name.c_str()).c_str());
    }

    void VirtualMachineSettingsPage::InitializeComponent()
    {
        VirtualMachineSettingsPageT::InitializeComponent();
        this->AssignmentModeComboBox().SelectionChanged([
            WeakSection = winrt::make_weak(this->GpuDeviceSection())](
                winrt::IInspectable const& Sender,
                winrt::SelectionChangedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Arguments);
            if (auto Section = WeakSection.get())
            {
                Section.Visibility(
                    Sender.as<winrt::ComboBox>().SelectedIndex() ==
                        static_cast<std::int32_t>(NanaBox::GpuAssignmentMode::List)
                        ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
            }
        });
        if (this->m_ConfigurationFilePath.empty())
        {
            this->PopulateControls();
        }
        else
        {
            try
            {
                this->ReloadConfiguration();
            }
            catch (...)
            {
                this->PopulateControls();
                this->ShowConfigurationError();
            }
        }
        this->RunNetworkManagementOperation(L"Refresh");
    }

    void VirtualMachineSettingsPage::PopulateControls()
    {
        this->UpdateWindowTitle();
        this->ReloadButton().IsEnabled(!this->m_ConfigurationFilePath.empty());
        this->OpenButton().IsEnabled(!this->m_ConfigurationFilePath.empty());
        this->RuntimeNotice().Visibility(this->m_MainWindow
            ? winrt::Visibility::Visible
            : winrt::Visibility::Collapsed);

        {
            this->GuestTypeRow().Visibility(this->m_MainWindow ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
            this->GuestTypeComboBox().SelectedIndex(static_cast<std::int32_t>(this->m_Configuration.GuestType));
            this->GuestTypeSelectionChanged(nullptr, nullptr);
            this->NameTextBox().Text(
                winrt::to_hstring(this->m_Configuration.Name));
            this->NameRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->ProcessorCountTextBox().Text(
                winrt::to_hstring(this->m_Configuration.ProcessorCount));
            this->ProcessorCountRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->MemorySizeTextBox().Text(
                winrt::to_hstring(this->m_Configuration.MemorySize));
            this->ResourceAllocationPanel().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
            this->ProcessorLimitTextBox().Text(winrt::to_hstring(this->m_Configuration.ProcessorLimit));
            this->ProcessorWeightTextBox().Text(winrt::to_hstring(this->m_Configuration.ProcessorWeight));
            this->MemoryOvercommitCheckBox().IsChecked(this->m_Configuration.AllowMemoryOvercommit);
            this->DeferredCommitCheckBox().IsChecked(this->m_Configuration.EnableDeferredCommit);
            this->StorageIopsMaximumTextBox().Text(winrt::to_hstring(this->m_Configuration.StorageIopsMaximum));
            this->StorageBandwidthMaximumTextBox().Text(winrt::to_hstring(this->m_Configuration.StorageBandwidthMaximum));
            this->SecureBootCheckBox().IsChecked(
                this->m_Configuration.SecureBoot);
            this->SecureBootRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->TpmCheckBox().IsChecked(this->m_Configuration.Tpm);
            this->TpmRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->VirtualizationExtensionsCheckBox().IsChecked(
                this->m_Configuration.ExposeVirtualizationExtensions);
            this->VirtualizationExtensionsRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
        }

        {
            NanaBox::ComPortsConfiguration const& Configuration =
                this->m_Configuration.ComPorts;
            this->UefiConsoleComboBox().SelectedIndex(
                static_cast<std::int32_t>(Configuration.UefiConsole));
            this->UefiConsoleRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->ComPort1TextBox().Text(
                winrt::to_hstring(Configuration.ComPort1));
            this->ComPort2TextBox().Text(
                winrt::to_hstring(Configuration.ComPort2));
        }

        {
            NanaBox::GpuConfiguration const& Configuration =
                this->m_Configuration.Gpu;
            this->AssignmentModeComboBox().SelectedIndex(
                static_cast<std::int32_t>(Configuration.AssignmentMode));
            this->EnableHostDriverStoreCheckBox().IsChecked(
                Configuration.EnableHostDriverStore);
            this->EnableHostDriverStoreRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
        }

        {
            this->GpuDevicePanel().Children().Clear();
            this->m_GpuDeviceSettings.clear();
            this->AddGpuDeviceButton().IsEnabled(true);
            for (auto const& Configuration :
                this->m_Configuration.Gpu.SelectedDevices)
            {
                this->AddGpuDevice(Configuration.first, Configuration.second);
            }
        }

        {
            this->NetworkStatusButton().IsEnabled(this->m_MainWindow != nullptr || bool(this->m_HostOperations->NetworkDiagnostics));
            this->NetworkRetryButton().IsEnabled((this->m_MainWindow != nullptr && this->m_Configuration.MirroredNetworking) ||
                bool(this->m_HostOperations->NetworkDiagnostics));
            this->MirroredNetworkingPanel().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
            this->MirroredNetworkingCheckBox().IsChecked(this->m_Configuration.MirroredNetworking);
            this->MirroredAutoReservePortsCheckBox().IsChecked(this->m_Configuration.MirroredAutoReservePorts);
            this->MirroredBindCoordinationCheckBox().IsChecked(this->m_Configuration.MirroredBindCoordination);
            this->MirroredDnsTunnelingCheckBox().IsChecked(this->m_Configuration.MirroredDnsTunneling);
            this->MirroredLoopbackCheckBox().IsChecked(this->m_Configuration.MirroredLoopback);
            this->MirroredHostAddressLoopbackCheckBox().IsChecked(this->m_Configuration.MirroredHostAddressLoopback);
            this->MirroredFirewallCreatorTextBox().Text(winrt::to_hstring(this->m_Configuration.MirroredFirewallCreatorId));
            this->MirroredTcpPortsTextBox().Text(::MakeSettingLines(this->m_Configuration.MirroredTcpPorts));
            this->MirroredUdpPortsTextBox().Text(::MakeSettingLines(this->m_Configuration.MirroredUdpPorts));
            this->MirroredIgnoredPortsTextBox().Text(::MakeSettingLines(this->m_Configuration.MirroredIgnoredPorts));
            this->NetworkAdapterPanel().Children().Clear();
            this->m_NetworkAdapterSettings.clear();
            this->AddNetworkAdapterButton().IsEnabled(true);
            for (auto const& Configuration :
                this->m_Configuration.NetworkAdapters)
            {
                this->AddNetworkAdapter(Configuration, true);
            }
        }

        {
            this->ScsiDevicePanel().Children().Clear();
            this->m_ScsiDeviceSettings.clear();
            this->AddScsiDeviceButton().IsEnabled(true);
            for (auto const& Configuration :
                this->m_Configuration.ScsiDevices)
            {
                this->AddScsiDevice(Configuration, true);
            }
        }

        {
            this->StateFilesSection().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->GuestStateFileTextBox().Text(
                winrt::to_hstring(this->m_Configuration.GuestStateFile));
            this->RuntimeStateFileTextBox().Text(
                winrt::to_hstring(this->m_Configuration.RuntimeStateFile));
        }

        {
            NanaBox::KeyboardConfiguration const& Configuration =
                this->m_Configuration.Keyboard;
            this->RedirectKeyCombinationsCheckBox().IsChecked(
                Configuration.RedirectKeyCombinations);
            this->ClipboardKeyboardLayoutTextBox().Text(winrt::to_hstring(Configuration.ClipboardKeyboardLayout));
            this->ClipboardGtkUnicodeCheckBox().IsChecked(Configuration.ClipboardGtkUnicode);
            this->FullScreenHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.FullScreenHotkey));
            this->CtrlEscHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.CtrlEscHotkey));
            this->AltEscHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.AltEscHotkey));
            this->AltTabHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.AltTabHotkey));
            this->AltShiftTabHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.AltShiftTabHotkey));
            this->AltSpaceHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.AltSpaceHotkey));
            this->CtrlAltDelHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.CtrlAltDelHotkey));
            this->FocusReleaseLeftHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.FocusReleaseLeftHotkey));
            this->FocusReleaseRightHotkeyTextBox().Text(
                winrt::to_hstring(Configuration.FocusReleaseRightHotkey));
        }

        {
            NanaBox::EnhancedSessionConfiguration const& Configuration =
                this->m_Configuration.EnhancedSession;
            this->RedirectAudioCheckBox().IsChecked(
                Configuration.RedirectAudio);
            this->RedirectAudioCaptureCheckBox().IsChecked(
                Configuration.RedirectAudioCapture);
            this->RedirectDrivesCheckBox().IsChecked(
                Configuration.RedirectDrives);
            this->RedirectPrintersCheckBox().IsChecked(
                Configuration.RedirectPrinters);
            this->RedirectPortsCheckBox().IsChecked(
                Configuration.RedirectPorts);
            this->RedirectSmartCardsCheckBox().IsChecked(
                Configuration.RedirectSmartCards);
            this->RedirectClipboardCheckBox().IsChecked(
                Configuration.RedirectClipboard);
            this->RedirectDevicesCheckBox().IsChecked(
                Configuration.RedirectDevices);
            this->RedirectPOSDevicesCheckBox().IsChecked(
                Configuration.RedirectPOSDevices);
            this->RedirectDynamicDrivesCheckBox().IsChecked(
                Configuration.RedirectDynamicDrives);
            this->RedirectDynamicDevicesCheckBox().IsChecked(
                Configuration.RedirectDynamicDevices);
        }

        {
            ::PopulateRedirectionChoices(
                this->DrivePanel(),
                std::map<std::string, winrt::hstring>(),
                this->m_Configuration.EnhancedSession.Drives);
            this->RefreshDrives();
        }

        {
            ::PopulateRedirectionChoices(
                this->DevicePanel(),
                std::map<std::string, winrt::hstring>(),
                this->m_Configuration.EnhancedSession.Devices);
            this->RefreshDevices();
        }

        {
            this->ChipsetInformationSection().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            NanaBox::ChipsetInformationConfiguration const& Configuration =
                this->m_Configuration.ChipsetInformation;
            this->BaseBoardSerialNumberTextBox().Text(
                winrt::to_hstring(Configuration.BaseBoardSerialNumber));
            this->ChassisSerialNumberTextBox().Text(
                winrt::to_hstring(Configuration.ChassisSerialNumber));
            this->ChassisAssetTagTextBox().Text(
                winrt::to_hstring(Configuration.ChassisAssetTag));
            this->ManufacturerTextBox().Text(
                winrt::to_hstring(Configuration.Manufacturer));
            this->ProductNameTextBox().Text(
                winrt::to_hstring(Configuration.ProductName));
            this->ChipsetVersionTextBox().Text(
                winrt::to_hstring(Configuration.Version));
            this->SerialNumberTextBox().Text(
                winrt::to_hstring(Configuration.SerialNumber));
            this->UUIDTextBox().Text(
                winrt::to_hstring(Configuration.UUID));
            this->SKUNumberTextBox().Text(
                winrt::to_hstring(Configuration.SKUNumber));
            this->FamilyTextBox().Text(
                winrt::to_hstring(Configuration.Family));
        }

        {
            NanaBox::VideoMonitorConfiguration const& Configuration =
                this->m_Configuration.VideoMonitor;
            this->HorizontalResolutionTextBox().Text(
                winrt::to_hstring(Configuration.HorizontalResolution));
            this->HorizontalResolutionRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->VerticalResolutionTextBox().Text(
                winrt::to_hstring(Configuration.VerticalResolution));
            this->VerticalResolutionRow().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->DisableDpiScalingCheckBox().IsChecked(
                Configuration.DisableBasicSessionDpiScaling);
            this->DpiOverrideCheckBox().IsChecked(Configuration.EnableDpiScalingValueOverride);
            this->DpiScaleTextBox().Text(winrt::to_hstring(Configuration.OverriddenDpiScalingValue));
            this->ContentResizingCheckBox().IsChecked(Configuration.EnableContentResizing);
            this->ConnectionBarCheckBox().IsChecked(Configuration.ShowFullScreenModeConnectionBar);
        }

        {
            this->PoliciesSection().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->PoliciesTextBox().Text(
                ::MakeSettingLines(this->m_Configuration.Policies));
            this->HcsExtensionsSection().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
            this->HcsSchemaMinorTextBox().Text(winrt::to_hstring(this->m_Configuration.HcsSchemaMinor));
            this->AdditionalHcsSettingsTextBox().Text(winrt::to_hstring(this->m_Configuration.AdditionalHcsSettings));
        }

        {
            bool CanEdit = !this->m_MainWindow || this->m_Configuration.GuestType == NanaBox::GuestType::Linux;
            this->Plan9ShareSection().Visibility(CanEdit
                ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
            this->Plan9SharePanel().Children().Clear();
            this->m_Plan9ShareSettings.clear();
            this->AddPlan9ShareButton().IsEnabled(CanEdit);
            if (CanEdit)
            {
                for (auto const& Configuration :
                    this->m_Configuration.Plan9Shares)
                {
                    this->AddPlan9Share(Configuration);
                }
            }
        }

        {
            this->VirtualSmbShareSection().Visibility(this->m_MainWindow
                ? winrt::Visibility::Collapsed
                : winrt::Visibility::Visible);
            this->VirtualSmbSharePanel().Children().Clear();
            this->m_VirtualSmbShareSettings.clear();
            this->AddVirtualSmbShareButton().IsEnabled(!this->m_MainWindow);
            if (!this->m_MainWindow)
            {
                for (auto const& Configuration :
                    this->m_Configuration.VirtualSmbShares)
                {
                    this->AddVirtualSmbShare(Configuration);
                }
            }
        }

    }

    NanaBox::VirtualMachineConfiguration
        VirtualMachineSettingsPage::ReadConfiguration()
    {
        NanaBox::VirtualMachineConfiguration Result = this->m_Configuration;

        {
            if (!this->m_MainWindow)
            {
                Result.GuestType = static_cast<NanaBox::GuestType>(::ReadSettingIndex(this->GuestTypeComboBox()));
                Result.Name =
                    winrt::to_string(
                        this->NameTextBox().Text());
                Result.ProcessorCount =
                    ::ReadSettingInteger<std::uint32_t>(
                        this->ProcessorCountTextBox());
                Result.ProcessorLimit = ::ReadSettingInteger<std::uint64_t>(this->ProcessorLimitTextBox());
                Result.ProcessorWeight = ::ReadSettingInteger<std::uint64_t>(this->ProcessorWeightTextBox());
                Result.AllowMemoryOvercommit = winrt::unbox_value<bool>(this->MemoryOvercommitCheckBox().IsChecked());
                Result.EnableDeferredCommit = winrt::unbox_value<bool>(this->DeferredCommitCheckBox().IsChecked());
                Result.StorageIopsMaximum = ::ReadSettingInteger<std::uint64_t>(this->StorageIopsMaximumTextBox());
                Result.StorageBandwidthMaximum = ::ReadSettingInteger<std::uint64_t>(this->StorageBandwidthMaximumTextBox());
                if (Result.ProcessorLimit > 100000)
                    ::InvalidSetting(::GetSettingsString(L"ProcessorLimitTextBox.Header"));
                if (Result.ProcessorWeight > 10000)
                    ::InvalidSetting(::GetSettingsString(L"ProcessorWeightTextBox.Header"));
                if (Result.EnableDeferredCommit && !Result.AllowMemoryOvercommit)
                    ::InvalidSetting(::GetSettingsString(L"DeferredCommitCheckBox.Content"));
                Result.SecureBoot =
                    winrt::unbox_value<bool>(
                        this->SecureBootCheckBox().IsChecked());
                Result.Tpm =
                    winrt::unbox_value<bool>(
                        this->TpmCheckBox().IsChecked());
                Result.ExposeVirtualizationExtensions =
                    winrt::unbox_value<bool>(
                        this->VirtualizationExtensionsCheckBox().IsChecked());
            }
            Result.MemorySize =
                ::ReadSettingInteger<std::uint64_t>(
                    this->MemorySizeTextBox());

            if (!this->m_MainWindow && Result.Name.empty())
            {
                ::InvalidSetting(::GetSettingsString(L"NameLabel.Text"));
            }
            if (!this->m_MainWindow && !Result.ProcessorCount)
            {
                ::InvalidSetting(::GetSettingsString(
                    L"ProcessorCountLabel.Text"));
            }
            if (!Result.MemorySize)
            {
                ::InvalidSetting(::GetSettingsString(
                    L"MemorySizeLabel.Text"));
            }
        }

        {
            if (!this->m_MainWindow)
            {
                Result.ComPorts.UefiConsole =
                    static_cast<NanaBox::UefiConsoleMode>(
                        ::ReadSettingIndex(this->UefiConsoleComboBox()));
            }
            Result.ComPorts.ComPort1 =
                winrt::to_string(
                    this->ComPort1TextBox().Text());
            Result.ComPorts.ComPort2 =
                winrt::to_string(
                    this->ComPort2TextBox().Text());
        }

        {
            if (!this->m_MainWindow)
            {
                Result.Gpu.EnableHostDriverStore = winrt::unbox_value<bool>(
                    this->EnableHostDriverStoreCheckBox().IsChecked());
            }
            Result.Gpu.AssignmentMode = static_cast<NanaBox::GpuAssignmentMode>(
                ::ReadSettingIndex(this->AssignmentModeComboBox()));
        }

        {
            Result.Gpu.SelectedDevices.clear();
            if (Result.Gpu.AssignmentMode == NanaBox::GpuAssignmentMode::List)
            for (GpuDeviceSettings const& Row : this->m_GpuDeviceSettings)
            {
                std::string DeviceInterface =
                    winrt::to_string(Row.DeviceInterface.Text());
                std::uint16_t PartitionId =
                    ::ReadSettingInteger<std::uint16_t>(Row.PartitionId);
                if (DeviceInterface.empty() ||
                    !Result.Gpu.SelectedDevices.emplace(
                        DeviceInterface,
                        PartitionId).second)
                {
                    ::InvalidSetting(::GetSettingsString(
                        L"GpuDeviceDeviceInterfaceLabel"));
                }
            }
        }

        {
            if (Result.Gpu.AssignmentMode == NanaBox::GpuAssignmentMode::List && Result.Gpu.SelectedDevices.empty())
                ::InvalidSetting(::GetSettingsString(L"GpuDeviceDeviceInterfaceLabel"));
            if (!this->m_MainWindow)
            {
                Result.MirroredNetworking = this->MirroredNetworkingCheckBox().IsChecked().Value();
                Result.MirroredAutoReservePorts = this->MirroredAutoReservePortsCheckBox().IsChecked().Value();
                Result.MirroredBindCoordination = this->MirroredBindCoordinationCheckBox().IsChecked().Value();
                Result.MirroredDnsTunneling = this->MirroredDnsTunnelingCheckBox().IsChecked().Value();
                Result.MirroredLoopback = this->MirroredLoopbackCheckBox().IsChecked().Value();
                Result.MirroredHostAddressLoopback = this->MirroredHostAddressLoopbackCheckBox().IsChecked().Value();
                if (Result.MirroredDnsTunneling && !Result.MirroredNetworking)
                    ::InvalidSetting(::GetSettingsString(L"MirroredDnsRequiresMirrored"));
                Result.MirroredFirewallCreatorId = winrt::to_string(this->MirroredFirewallCreatorTextBox().Text());
                if (!Result.MirroredFirewallCreatorId.empty())
                    (void)winrt::guid(Result.MirroredFirewallCreatorId);
                Result.MirroredTcpPorts = ::ReadSettingLines(this->MirroredTcpPortsTextBox());
                Result.MirroredUdpPorts = ::ReadSettingLines(this->MirroredUdpPortsTextBox());
                Result.MirroredIgnoredPorts = ::ReadSettingLines(this->MirroredIgnoredPortsTextBox());
                for (auto const& Ports : {Result.MirroredTcpPorts, Result.MirroredUdpPorts, Result.MirroredIgnoredPorts})
                {
                    std::set<unsigned> Unique;
                    for (auto const& Port : Ports)
                    {
                        unsigned Value = 0;
                        auto End = Port.data() + Port.size();
                        auto Parse = std::from_chars(Port.data(), End, Value);
                        if (Parse.ec != std::errc() || Parse.ptr != End || !Value || Value > 65535 || !Unique.insert(Value).second)
                            ::InvalidSetting(::GetSettingsString(L"MirroredPortsLabel"));
                    }
                }
                for (auto const& Port : Result.MirroredIgnoredPorts)
                    for (auto const& Ports : {Result.MirroredTcpPorts, Result.MirroredUdpPorts})
                        for (auto const& Explicit : Ports)
                            if (std::stoul(Port) == std::stoul(Explicit))
                                ::InvalidSetting(::GetSettingsString(L"MirroredPortsLabel"));
            }
            Result.NetworkAdapters.clear();
            std::set<std::string> EndpointIds;
            for (NetworkAdapterSettings const& Row :
                this->m_NetworkAdapterSettings)
            {
                NanaBox::NetworkAdapterConfiguration Configuration;
                Configuration.EndpointId = Row.EndpointId;
                Configuration.Connected =
                    Row.Connected.IsChecked().Value();
                Configuration.MacAddress =
                    winrt::to_string(Row.MacAddress.Text());
                Configuration.NetworkId = winrt::to_string(Row.NetworkId.Text());
                Configuration.FirewallCreatorId = winrt::to_string(Row.FirewallCreatorId.Text());
                if (!Configuration.NetworkId.empty())
                    (void)winrt::guid(Configuration.NetworkId);
                if (!Configuration.FirewallCreatorId.empty())
                {
                    (void)winrt::guid(Configuration.FirewallCreatorId);
                    if (Configuration.NetworkId.empty()) ::InvalidSetting(::GetSettingsString(L"NetworkIdLabel"));
                }
                Configuration.SuggestedSwitchId =
                    winrt::to_string(Row.SuggestedSwitchId.Text());
                Configuration.SuggestedSwitchName =
                    winrt::to_string(Row.SuggestedSwitchName.Text());
                Configuration.SuggestedSwitchSubnet =
                    winrt::to_string(Row.SuggestedSwitchSubnet.Text());
                if (Configuration.EndpointId.empty())
                {
                    GUID EndpointId;
                    winrt::check_hresult(::CoCreateGuid(&EndpointId));
                    Configuration.EndpointId = winrt::to_string(
                        ::FromGuid(EndpointId));
                }
                if (!Configuration.EndpointId.empty())
                {
                    winrt::guid EndpointId(Configuration.EndpointId);
                    std::string Key = winrt::to_string(::FromGuid(EndpointId));
                    Configuration.EndpointId = Key;
                    if (!EndpointIds.insert(Key).second)
                    {
                        ::InvalidSetting(::GetSettingsString(
                            L"NetworkAdapterTitle.Text"));
                    }
                }
                Result.NetworkAdapters.push_back(Configuration);
            }
        }

        {
            Result.ScsiDevices.clear();
            for (ScsiDeviceSettings const& Row : this->m_ScsiDeviceSettings)
            {
                NanaBox::ScsiDeviceConfiguration Configuration;
                Configuration.Type = static_cast<NanaBox::ScsiDeviceType>(
                    ::ReadSettingIndex(Row.Type) + 1);
                Configuration.Path = winrt::to_string(Row.Path.Text());
                if (Configuration.Path.empty() &&
                    NanaBox::ScsiDeviceType::VirtualImage != Configuration.Type)
                {
                    if (!this->m_MainWindow)
                    {
                        continue;
                    }
                    ::InvalidSetting(::GetSettingsString(
                        L"ScsiDevicePathLabel"));
                }
                Result.ScsiDevices.push_back(Configuration);
            }
        }

        {
            if (!this->m_MainWindow)
            {
                Result.GuestStateFile =
                    winrt::to_string(
                        this->GuestStateFileTextBox().Text());
                Result.RuntimeStateFile =
                    winrt::to_string(
                        this->RuntimeStateFileTextBox().Text());
            }
        }

        {
            Result.Keyboard.RedirectKeyCombinations =
                winrt::unbox_value<bool>(
                    this->RedirectKeyCombinationsCheckBox().IsChecked());
            Result.Keyboard.ClipboardKeyboardLayout = winrt::to_string(this->ClipboardKeyboardLayoutTextBox().Text());
            Result.Keyboard.ClipboardGtkUnicode = Result.GuestType == NanaBox::GuestType::Linux &&
                winrt::unbox_value<bool>(this->ClipboardGtkUnicodeCheckBox().IsChecked());
            Result.Keyboard.FullScreenHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->FullScreenHotkeyTextBox());
            Result.Keyboard.CtrlEscHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->CtrlEscHotkeyTextBox());
            Result.Keyboard.AltEscHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->AltEscHotkeyTextBox());
            Result.Keyboard.AltTabHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->AltTabHotkeyTextBox());
            Result.Keyboard.AltShiftTabHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->AltShiftTabHotkeyTextBox());
            Result.Keyboard.AltSpaceHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->AltSpaceHotkeyTextBox());
            Result.Keyboard.CtrlAltDelHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->CtrlAltDelHotkeyTextBox());
            Result.Keyboard.FocusReleaseLeftHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->FocusReleaseLeftHotkeyTextBox());
            Result.Keyboard.FocusReleaseRightHotkey =
                ::ReadSettingInteger<std::int32_t>(
                    this->FocusReleaseRightHotkeyTextBox());
        }

        {
            Result.EnhancedSession.RedirectAudio =
                winrt::unbox_value<bool>(
                    this->RedirectAudioCheckBox().IsChecked());
            Result.EnhancedSession.RedirectAudioCapture =
                winrt::unbox_value<bool>(
                    this->RedirectAudioCaptureCheckBox().IsChecked());
            Result.EnhancedSession.RedirectDrives =
                winrt::unbox_value<bool>(
                    this->RedirectDrivesCheckBox().IsChecked());
            Result.EnhancedSession.RedirectPrinters =
                winrt::unbox_value<bool>(
                    this->RedirectPrintersCheckBox().IsChecked());
            Result.EnhancedSession.RedirectPorts =
                winrt::unbox_value<bool>(
                    this->RedirectPortsCheckBox().IsChecked());
            Result.EnhancedSession.RedirectSmartCards =
                winrt::unbox_value<bool>(
                    this->RedirectSmartCardsCheckBox().IsChecked());
            Result.EnhancedSession.RedirectClipboard =
                winrt::unbox_value<bool>(
                    this->RedirectClipboardCheckBox().IsChecked());
            Result.EnhancedSession.RedirectDevices =
                winrt::unbox_value<bool>(
                    this->RedirectDevicesCheckBox().IsChecked());
            Result.EnhancedSession.RedirectPOSDevices =
                winrt::unbox_value<bool>(
                    this->RedirectPOSDevicesCheckBox().IsChecked());
            Result.EnhancedSession.RedirectDynamicDrives =
                winrt::unbox_value<bool>(
                    this->RedirectDynamicDrivesCheckBox().IsChecked());
            Result.EnhancedSession.RedirectDynamicDevices =
                winrt::unbox_value<bool>(
                    this->RedirectDynamicDevicesCheckBox().IsChecked());
        }

        {
            Result.EnhancedSession.Drives =
                ::ReadRedirectionSelection(this->DrivePanel());
            Result.EnhancedSession.Devices =
                ::ReadRedirectionSelection(this->DevicePanel());
        }

        {
            if (!this->m_MainWindow)
            {
                Result.ChipsetInformation.BaseBoardSerialNumber =
                    winrt::to_string(
                        this->BaseBoardSerialNumberTextBox().Text());
                Result.ChipsetInformation.ChassisSerialNumber =
                    winrt::to_string(
                        this->ChassisSerialNumberTextBox().Text());
                Result.ChipsetInformation.ChassisAssetTag =
                    winrt::to_string(
                        this->ChassisAssetTagTextBox().Text());
                Result.ChipsetInformation.Manufacturer =
                    winrt::to_string(
                        this->ManufacturerTextBox().Text());
                Result.ChipsetInformation.ProductName =
                    winrt::to_string(
                        this->ProductNameTextBox().Text());
                Result.ChipsetInformation.Version =
                    winrt::to_string(
                        this->ChipsetVersionTextBox().Text());
                Result.ChipsetInformation.SerialNumber =
                    winrt::to_string(
                        this->SerialNumberTextBox().Text());
                Result.ChipsetInformation.UUID =
                    winrt::to_string(
                        this->UUIDTextBox().Text());
                Result.ChipsetInformation.SKUNumber =
                    winrt::to_string(
                        this->SKUNumberTextBox().Text());
                Result.ChipsetInformation.Family =
                    winrt::to_string(
                        this->FamilyTextBox().Text());
            }
        }

        {
            if (!this->m_MainWindow)
            {
                Result.VideoMonitor.HorizontalResolution =
                    ::ReadSettingInteger<std::uint16_t>(
                        this->HorizontalResolutionTextBox());
                Result.VideoMonitor.VerticalResolution =
                    ::ReadSettingInteger<std::uint16_t>(
                        this->VerticalResolutionTextBox());
            }
            Result.VideoMonitor.DisableBasicSessionDpiScaling =
                winrt::unbox_value<bool>(
                    this->DisableDpiScalingCheckBox().IsChecked());
            Result.VideoMonitor.EnableDpiScalingValueOverride = this->DpiOverrideCheckBox().IsChecked().Value();
            Result.VideoMonitor.OverriddenDpiScalingValue = ::ReadSettingInteger<std::uint32_t>(this->DpiScaleTextBox());
            if (Result.VideoMonitor.OverriddenDpiScalingValue < 100 || Result.VideoMonitor.OverriddenDpiScalingValue > 500)
                ::InvalidSetting(::GetSettingsString(L"DpiScale.Header"));
            Result.VideoMonitor.EnableContentResizing = this->ContentResizingCheckBox().IsChecked().Value();
            Result.VideoMonitor.ShowFullScreenModeConnectionBar = this->ConnectionBarCheckBox().IsChecked().Value();
        }

        {
            if (!this->m_MainWindow)
            {
                Result.Policies =
                    ::ReadSettingLines(this->PoliciesTextBox());
                Result.HcsSchemaMinor = ::ReadSettingInteger<std::uint32_t>(this->HcsSchemaMinorTextBox());
                if (!Result.HcsSchemaMinor || Result.HcsSchemaMinor > 65535)
                    ::InvalidSetting(::GetSettingsString(L"HcsSchemaMinor.Header"));
                Result.AdditionalHcsSettings = winrt::to_string(this->AdditionalHcsSettingsTextBox().Text());
                if (!Result.AdditionalHcsSettings.empty())
                {
                    auto Additional = nlohmann::json::parse(Result.AdditionalHcsSettings);
                    if (!Additional.is_object())
                        ::InvalidSetting(::GetSettingsString(L"AdditionalHcsSettings.Header"));
                    Result.AdditionalHcsSettings = Additional.empty() ? std::string() : Additional.dump(2);
                }
            }
        }

        {
            if (!this->m_MainWindow || Result.GuestType == NanaBox::GuestType::Linux)
            {
                Result.Plan9Shares.clear();
                for (Plan9ShareSettings const& Row :
                    this->m_Plan9ShareSettings)
                {
                    NanaBox::Plan9ShareConfiguration Configuration;
                    Configuration.ReadOnly =
                        Row.ReadOnly.IsChecked().Value();
                    Configuration.AutoMount = Row.AutoMount.IsChecked().Value();
                    Configuration.LinuxMetadata = Row.LinuxMetadata.IsChecked().Value();
                    Configuration.CaseSensitive = Row.CaseSensitive.IsChecked().Value();
                    Configuration.UseShareRootIdentity = Row.UseShareRootIdentity.IsChecked().Value();
                    Configuration.AllowOptions = Row.AllowOptions.IsChecked().Value();
                    Configuration.AllowSubPaths = Row.AllowSubPaths.IsChecked().Value();
                    Configuration.UnlimitedConnections = Row.UnlimitedConnections.IsChecked().Value();
                    Configuration.AllowedFiles = ::ReadSettingLines(Row.AllowedFiles);
                    Configuration.Port =
                        ::ReadSettingInteger<std::uint32_t>(Row.Port);
                    Configuration.Path =
                        winrt::to_string(Row.Path.Text());
                    Configuration.Name =
                        winrt::to_string(Row.Name.Text());
                    if (Configuration.Path.empty() ||
                        Configuration.Name.empty())
                    {
                        ::InvalidSetting(::GetSettingsString(
                            L"Plan9ShareTitle.Text"));
                    }
                    if (Result.Gpu.EnableHostDriverStore &&
                        ("HostDriverStore" == Configuration.Name ||
                         "NanaBox.HostDrivers" == Configuration.Name ||
                         "NanaBox.HostLxssLib" == Configuration.Name))
                    {
                        ::InvalidSetting(::GetSettingsString(
                            L"Plan9ShareNameLabel"));
                    }
                    Result.Plan9Shares.push_back(Configuration);
                }
            }
        }

        {
            if (!this->m_MainWindow)
            {
                Result.VirtualSmbShares.clear();
                for (VirtualSmbShareSettings const& Row :
                    this->m_VirtualSmbShareSettings)
                {
                    NanaBox::VirtualSmbShareConfiguration Configuration;
                    Configuration.ReadOnly =
                        Row.ReadOnly.IsChecked().Value();
                    Configuration.Path =
                        winrt::to_string(Row.Path.Text());
                    Configuration.Name =
                        winrt::to_string(Row.Name.Text());
                    if (Configuration.Path.empty() ||
                        Configuration.Name.empty())
                    {
                        ::InvalidSetting(::GetSettingsString(
                            L"VirtualSmbShareTitle.Text"));
                    }
                    if (Result.Gpu.EnableHostDriverStore &&
                        ("NanaBox.HostDrivers" == Configuration.Name ||
                         "NanaBox.HostLxssLib" == Configuration.Name))
                    {
                        ::InvalidSetting(::GetSettingsString(
                            L"VirtualSmbShareNameLabel"));
                    }
                    Result.VirtualSmbShares.push_back(Configuration);
                }
            }
        }

        return NanaBox::DeserializeConfiguration(
            NanaBox::SerializeConfiguration(Result));
    }

    void VirtualMachineSettingsPage::ReloadButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            this->ReloadConfiguration();
        }
        catch (...)
        {
            this->ShowConfigurationError();
        }
    }

    void VirtualMachineSettingsPage::OpenButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);
        try
        {
            auto Path = this->m_ConfigurationFilePath;
            if (Path.empty()) return;
            if (this->m_HostOperations->ExternalAction)
            {
                this->ConfigurationStatusTextBox().Text(this->m_HostOperations->ExternalAction(L"OpenConfiguration", Path));
                return;
            }
            ::ShowMessageDialog(this->m_WindowHandle,
                Mile::WinRT::GetLocalizedString(L"VirtualMachineSettingsPage/SettingsInstructionText"),
                Mile::WinRT::GetLocalizedString(L"VirtualMachineSettingsPage/SettingsContentText"));
            OPENASINFO OpenAsInfo = {};
            OpenAsInfo.pcszFile = Path.c_str();
            OpenAsInfo.oaifInFlags = OAIF_EXEC;
            HRESULT Result = ::SHOpenWithDialog(this->m_WindowHandle, &OpenAsInfo);
            if (Result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) winrt::check_hresult(Result);
        }
        catch (...) { ::ShowSettingsError(this->m_WindowHandle); }
    }

    void VirtualMachineSettingsPage::ReferenceButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            if (this->m_HostOperations->ExternalAction)
                this->ConfigurationStatusTextBox().Text(this->m_HostOperations->ExternalAction(L"Reference", L""));
            else
                ::LaunchDocumentation();
        }
        catch (...) { ::ShowSettingsError(this->m_WindowHandle); }
    }

    void VirtualMachineSettingsPage::ValidateConfigurationButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (this->m_ApplyPending) return;
        try
        {
            auto Configuration = this->ReadConfiguration();
            auto Text = NanaBox::SerializeConfiguration(Configuration);
            (void)NanaBox::DeserializeConfiguration(Text);
            this->ConfigurationStatusTextBox().Text(::GetSettingsString(L"ConfigurationValid"));
        }
        catch (...) { this->ShowConfigurationError(); }
    }

    void VirtualMachineSettingsPage::ApplyButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        if (this->m_ApplyPending) return;
        try
        {
            auto Configuration = this->ReadConfiguration();
            {
                std::wstring FilePath = this->m_ConfigurationFilePath;
                if (FilePath.empty())
                {
                    winrt::hstring FileTypeName =
                        ::GetSettingsString(L"ConfigurationFileTypeName");
                    COMDLG_FILTERSPEC FileType = {};
                    FileType.pszName = FileTypeName.c_str();
                    FileType.pszSpec = L"*.7b";
                    LPWSTR RawFilePath = nullptr;
                    HRESULT Result = ::ShowFileDialog(
                        this->m_WindowHandle,
                        TRUE,
                        1,
                        &FileType,
                        nullptr,
                        L"7b",
                        winrt::to_hstring(Configuration.Name + ".7b").c_str(),
                        &RawFilePath);
                    if (HRESULT_FROM_WIN32(ERROR_CANCELLED) == Result)
                    {
                        return;
                    }
                    winrt::check_hresult(Result);
                    auto FreeFilePath = Mile::ScopeExitTaskHandler(
                        [RawFilePath]()
                    {
                        ::CoTaskMemFree(RawFilePath);
                    });
                    FilePath = RawFilePath;
                    if (FilePath.empty())
                    {
                        return;
                    }
                    for (auto& Device : Configuration.ScsiDevices)
                    {
                        if (Device.Type ==
                            NanaBox::ScsiDeviceType::VirtualDisk ||
                            Device.Type ==
                            NanaBox::ScsiDeviceType::VirtualImage)
                        {
                            Device.Path = winrt::to_string(
                                ::GetRelativeFilePath(
                                    FilePath,
                                    winrt::to_hstring(Device.Path).c_str()));
                        }
                    }
                }

                std::string Content =
                    NanaBox::SerializeConfiguration(Configuration);
                ::WriteAllTextToUtf8TextFile(FilePath, Content,
                    this->m_ConfigurationFilePath.empty() ? nullptr : &this->m_LoadedConfigurationContent);
                this->m_LoadedConfigurationContent = "\xEF\xBB\xBF" + Content;
                this->m_ConfigurationFilePath = FilePath;
            }
            this->m_Configuration = Configuration;
            this->PopulateControls();
            this->ConfigurationStatusTextBox().Text(::GetSettingsString(L"ConfigurationSaved"));
            if (this->m_MainWindow)
            {
                this->m_ApplyPending = true;
                this->ApplyButton().IsEnabled(false);
                this->ReloadButton().IsEnabled(false);
                this->ValidateConfigurationButton().IsEnabled(false);
                this->ConfigurationStatusTextBox().Text(::GetSettingsString(L"DiskOperationRunning"));
                auto Dispatcher = this->Dispatcher();
                auto Weak = this->get_weak();
                auto Generation = ++this->m_ApplyGeneration;
                this->m_MainWindow->ReloadVirtualMachineSettings([Dispatcher, Weak, Generation](winrt::hstring Error)
                {
                    Dispatcher.RunAsync(winrt::CoreDispatcherPriority::Normal, [Weak, Generation, Error]()
                    {
                        auto Instance = Weak.get();
                        if (!Instance || Instance->m_ApplyGeneration != Generation) return;
                        Instance->m_ApplyPending = false;
                        Instance->ApplyButton().IsEnabled(true);
                        Instance->ReloadButton().IsEnabled(true);
                        Instance->ValidateConfigurationButton().IsEnabled(true);
                        try
                        {
                            Instance->ReloadConfiguration();
                            Instance->ConfigurationStatusTextBox().Text(Error.empty() ? ::GetSettingsString(L"ConfigurationSaved") : Error);
                        }
                        catch (...) { Instance->ShowConfigurationError(); }
                    });
                });
            }
        }
        catch (...)
        {
            this->m_ApplyPending = false;
            this->ApplyButton().IsEnabled(true);
            this->ReloadButton().IsEnabled(!this->m_ConfigurationFilePath.empty());
            this->ValidateConfigurationButton().IsEnabled(true);
            this->ShowConfigurationError();
        }
    }

    void VirtualMachineSettingsPage::RefreshDrives()
    {
        try
        {
            auto Selected = ::ReadRedirectionSelection(this->DrivePanel());
            if (!this->m_EnumerationClient)
            {
                this->m_EnumerationClient =
                    winrt::make_self<NanaBox::RdpClient>();
            }
            this->m_EnumerationClient->RescanDrives(false);
            std::map<std::string, winrt::hstring> Available;
            ULONG Count = this->m_EnumerationClient->DriveCount();
            for (ULONG Index = 0; Index < Count; ++Index)
            {
                auto Drive =
                    this->m_EnumerationClient->DriveByIndex(Index);
                ULONG LetterIndex = Drive.DriveLetterIndex();
                const std::string Letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
                if (LetterIndex < Letters.size())
                {
                    std::string Key = Letters.substr(LetterIndex, 1);
                    Available[Key] = winrt::to_hstring(Key + ":");
                }
            }
            ::PopulateRedirectionChoices(
                this->DrivePanel(), Available, Selected);
            this->DriveRefreshError().Text(L"");
        }
        catch (...)
        {
            this->DriveRefreshError().Text(
                ::GetSettingsString(L"EnumerationFailed"));
        }
    }

    void VirtualMachineSettingsPage::RefreshDriveButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            this->RefreshDrives();
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }

    void VirtualMachineSettingsPage::RefreshDevices()
    {
        try
        {
            auto Selected = ::ReadRedirectionSelection(this->DevicePanel());
            if (!this->m_EnumerationClient)
            {
                this->m_EnumerationClient =
                    winrt::make_self<NanaBox::RdpClient>();
            }
            this->m_EnumerationClient->RescanDevices(false);
            std::map<std::string, winrt::hstring> Available;
            ULONG Count = this->m_EnumerationClient->DeviceCount();
            for (ULONG Index = 0; Index < Count; ++Index)
            {
                auto Device =
                    this->m_EnumerationClient->DeviceByIndex(Index);
                std::string Key = winrt::to_string(
                    Device.DeviceInstanceId());
                winrt::hstring Label = winrt::to_hstring(Key);
                try
                {
                    winrt::hstring FriendlyName = Device.FriendlyName();
                    if (!FriendlyName.empty())
                    {
                        Label = FriendlyName;
                    }
                }
                catch (...)
                {
                }
                Available[Key] = Label;
            }
            ::PopulateRedirectionChoices(
                this->DevicePanel(), Available, Selected);
            this->DeviceRefreshError().Text(L"");
        }
        catch (...)
        {
            this->DeviceRefreshError().Text(
                ::GetSettingsString(L"EnumerationFailed"));
        }
    }

    void VirtualMachineSettingsPage::RefreshDeviceButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            this->RefreshDevices();
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }

    void VirtualMachineSettingsPage::AddGpuDevice(
        std::string const& DeviceInterface,
        _In_ std::uint16_t PartitionId)
    {
        GpuDeviceSettings Row;
        Row.Container = winrt::StackPanel();
        Row.Container.Spacing(4);
        Row.Container.Margin(winrt::ThicknessHelper::FromLengths(
            8, 0, 0, 8));
        Row.DeviceInterface = ::AddSettingTextBox(
            Row.Container,
            L"GpuDeviceDeviceInterfaceLabel",
            L"GpuDeviceDeviceInterfaceHelp",
            winrt::to_hstring(DeviceInterface),
            false);
        auto Picker = ::MakeHostPicker(Row.Container, Row.DeviceInterface, L"HostGpuLabel", L"HostGpuHelp");
        try
        {
            for (auto const& Gpu : NanaBox::EnumeratePartitionableGpus())
                ::AddPickerItem(Picker, Gpu.Name, Gpu.DeviceInterface);
            if (!Picker.Items().Size()) Picker.PlaceholderText(::GetSettingsString(L"NoHostGpus"));
        }
        catch (...)
        {
            Picker.PlaceholderText(Mile::WinRT::ToHResultError().message());
        }
        Row.PartitionId = ::AddSettingTextBox(
            Row.Container,
            L"GpuDevicePartitionIdLabel",
            L"GpuDevicePartitionIdHelp",
            winrt::to_hstring(PartitionId),
            false);
        Row.PartitionId.HorizontalAlignment(winrt::HorizontalAlignment::Left);

        winrt::Windows::UI::Xaml::Controls::Button RemoveButton;
        winrt::AutomationProperties::SetAutomationId(RemoveButton, L"GpuRemove");
        RemoveButton.Content(winrt::box_value(
            ::GetSettingsString(L"Remove")));
        RemoveButton.Visibility(winrt::Visibility::Visible);
        RemoveButton.Click([
            Weak = this->get_weak(),
            WeakContainer = winrt::make_weak(Row.Container)](
                winrt::IInspectable const& Sender,
                winrt::RoutedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Sender);
            UNREFERENCED_PARAMETER(Arguments);

            auto Instance = Weak.get();
            if (!Instance)
            {
                return;
            }
            try
            {
                auto Container = WeakContainer.get();
                if (!Container)
                {
                    return;
                }
                std::uint32_t Index = 0;
                if (!Instance->GpuDevicePanel().Children().IndexOf(
                    Container, Index))
                {
                    return;
                }
                Instance->GpuDevicePanel().Children().RemoveAt(Index);
                auto& Settings = Instance->m_GpuDeviceSettings;
                Settings.erase(Settings.begin() + Index);
            }
            catch (...)
            {
                ::ShowSettingsError(Instance->m_WindowHandle);
            }
        });
        Row.Container.Children().Append(RemoveButton);
        this->m_GpuDeviceSettings.push_back(Row);
        try
        {
            this->GpuDevicePanel().Children().Append(Row.Container);
        }
        catch (...)
        {
            this->m_GpuDeviceSettings.pop_back();
            throw;
        }
    }


    void VirtualMachineSettingsPage::GuestTypeSelectionChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (!this->GuestTypeComboBox() || !this->GuestManualMountPanel() ||
            !this->GuestLinuxShareNote() || !this->GuestWindowsShareNote()) return;
        auto Type = static_cast<NanaBox::GuestType>(this->GuestTypeComboBox().SelectedIndex());
        bool WindowsGuest = Type == NanaBox::GuestType::Windows;
        if (this->ClipboardGtkUnicodeCheckBox())
            this->ClipboardGtkUnicodeCheckBox().IsEnabled(Type == NanaBox::GuestType::Linux);
        this->GuestManualMountPanel().Visibility(WindowsGuest ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
        this->GuestLinuxShareNote().Visibility(WindowsGuest ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
        this->GuestWindowsShareNote().Visibility(WindowsGuest ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
    }

    void VirtualMachineSettingsPage::GuestToolsButtonClick(winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        try
        {
            auto Resource = winrt::to_string(winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag()));
            if (Resource == "Cancel")
            {
                if (this->m_GuestCancellation) this->m_GuestCancellation->store(true);
                this->GuestCancelControl().IsEnabled(false);
                return;
            }
            if (this->m_GuestToolsRunning) return;
            if (Resource == "Plan9Shares" && static_cast<NanaBox::GuestType>(this->GuestTypeComboBox().SelectedIndex()) ==
                NanaBox::GuestType::Windows) Resource = "VirtualSmbShares";

            if (Resource == "BrowseUpload" || Resource == "BrowseDownload")
            {
                COMDLG_FILTERSPEC Types[] = {{L"All files (*.*)", L"*.*"}};
                LPWSTR Raw = nullptr;
                auto DefaultName = this->GuestFileNameTextBox().Text();
                HRESULT Hr = ::ShowFileDialog(this->m_WindowHandle, Resource == "BrowseDownload",
                    ARRAYSIZE(Types), Types, nullptr, nullptr,
                    Resource == "BrowseDownload" ? DefaultName.c_str() : nullptr, &Raw);
                auto Free = Mile::ScopeExitTaskHandler([&]() { ::CoTaskMemFree(Raw); });
                if (Hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
                winrt::check_hresult(Hr);
                if (Raw)
                {
                    std::wstring Path(Raw);
                    this->GuestFilePathTextBox().Text(winrt::hstring(Path));
                    if (Resource == "BrowseUpload" && this->GuestFileNameTextBox().Text().empty())
                        this->GuestFileNameTextBox().Text(winrt::hstring(Path.substr(Path.find_last_of(L"\\/") + 1)));
                }
                return;
            }

            bool Transfer = Resource == "FileUpload" || Resource == "FileDownload";
            std::wstring Path;
            std::string Name;
            bool Overwrite = false;
            if (Transfer)
            {
                Path = this->GuestFilePathTextBox().Text().c_str();
                Name = winrt::to_string(this->GuestFileNameTextBox().Text());
                Overwrite = this->GuestFileOverwriteCheckBox().IsChecked().Value();
                if (Path.empty() || Name.empty())
                    throw winrt::hresult_invalid_argument(::GetSettingsString(L"GuestFileRequired"));
            }
            if (Resource == "Plan9Mount" || Resource == "Plan9Unmount")
            {
                Name = winrt::to_string(this->GuestPlan9NameTextBox().Text());
                if (Name.empty()) throw winrt::hresult_invalid_argument(::GetSettingsString(L"Plan9ShareNameLabel"));
                if (Resource == "Plan9Mount") Path = this->GuestPlan9PathTextBox().Text().c_str();
            }
            if ((Resource == "TimeSync" || (Transfer && Overwrite)) &&
                ::MessageBoxW(this->m_WindowHandle,
                    ::GetSettingsString(Resource == "TimeSync" ? L"GuestClockConfirm" : L"GuestOverwriteConfirm").c_str(),
                    L"NanaBox", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;

            auto Cancellation = std::make_shared<std::atomic<bool>>(false);
            std::function<std::string(std::string const&)> Query;
            if ((Resource == "Heartbeat" || Resource == "GuestInformation" || Resource == "Diagnostics") && this->m_GuestQuery)
                Query = this->m_GuestQuery;
            else if (this->m_HostOperations->GuestAction)
                Query = [Operations = this->m_HostOperations, Path, Name, Overwrite, Cancellation](std::string const& Action)
                {
                    return Operations->GuestAction(Action, Path, Name, Overwrite, Cancellation);
                };
            else
            {
                if (!this->m_MainWindow) throw winrt::hresult_error(E_FAIL, ::GetSettingsString(L"GuestToolsUnavailable"));
                auto Id = this->m_MainWindow->GuestRuntimeId();
                Query = [Id, Path, Name, Overwrite, Cancellation](std::string const& Action)
                {
                    return ::NanaBox::RunGuestToolsAction(Id, Action, Path, Name, Overwrite, Cancellation);
                };
            }
            this->m_GuestCancellation = std::move(Cancellation);
            this->RunGuestToolsQuery(std::move(Resource), std::move(Query));
        }
        catch (...)
        {
            auto Error = Mile::WinRT::ToHResultError();
            this->GuestToolsResult().Text(winrt::to_hstring(nlohmann::json{
                {"Success", false}, {"Error", winrt::to_string(Error.message())}}.dump(2)));
        }
    }

    void VirtualMachineSettingsPage::SetGuestToolsBusy(bool Busy, bool Cancellable)
    {
        this->m_GuestToolsRunning = Busy;
        for (auto const& Button : {this->GuestHeartbeatControl(), this->GuestInformationControl(),
            this->GuestDiagnosticsControl(), this->GuestPlan9Control(), this->GuestPlan9MountControl(),
            this->GuestPlan9UnmountControl(), this->GuestClockQueryControl(), this->GuestClockSyncControl(),
            this->GuestUploadControl(), this->GuestDownloadControl(), this->GuestBrowseUploadControl(),
            this->GuestBrowseDownloadControl()})
            Button.IsEnabled(!Busy);
        this->GuestPlan9NameTextBox().IsEnabled(!Busy);
        this->GuestPlan9PathTextBox().IsEnabled(!Busy);
        this->GuestFileNameTextBox().IsEnabled(!Busy);
        this->GuestFilePathTextBox().IsEnabled(!Busy);
        this->GuestFileOverwriteCheckBox().IsEnabled(!Busy);
        this->GuestCancelControl().IsEnabled(Busy && Cancellable);
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunGuestToolsQuery(std::string Resource,
        std::function<std::string(std::string const&)> Query)
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        this->SetGuestToolsBusy(true, Resource == "FileUpload" || Resource == "FileDownload");
        this->GuestToolsResult().Text(::GetSettingsString(L"DiskOperationRunning"));
        std::string Result;
        try
        {
            co_await winrt::resume_background();
            Result = Query(Resource);
        }
        catch (...)
        {
            auto Error = Mile::WinRT::ToHResultError();
            Result = nlohmann::json{{"Success", false}, {"ResourceType", Resource},
                {"Cancelled", Error.code() == E_ABORT}, {"Error", winrt::to_string(Error.message())}}.dump(2);
        }
        try
        {
            co_await Foreground;
            this->m_GuestCancellation.reset();
            this->SetGuestToolsBusy(false);
            this->GuestToolsResult().Text(winrt::to_hstring(Result));
        }
        catch (...) {}
    }

    void VirtualMachineSettingsPage::NetworkDiagnosticsButtonClick(
        winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        try
        {
            auto Action = winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag());
            if (Action == L"Copy")
            {
                winrt::Windows::ApplicationModel::DataTransfer::DataPackage Data;
                Data.SetText(this->NetworkDiagnosticsTextBox().Text());
                winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(Data);
                winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
                return;
            }
            if (Action != L"Host" && this->m_HostOperations->NetworkDiagnostics)
            {
                this->NetworkDiagnosticsTextBox().Text(this->m_HostOperations->NetworkDiagnostics(Action == L"Retry"));
                return;
            }
            if (Action == L"Host" || !this->m_MainWindow)
            {
                this->RunHostPreflight();
                return;
            }
            auto Dispatcher = this->Dispatcher();
            auto Weak = this->get_weak();
            this->m_MainWindow->RequestNetworkDiagnostics(Action == L"Retry", [Dispatcher, Weak](winrt::hstring Report)
            {
                Dispatcher.RunAsync(winrt::CoreDispatcherPriority::Normal, [Weak, Report]()
                {
                    if (auto Page = Weak.get()) Page->NetworkDiagnosticsTextBox().Text(Report);
                });
            });
        }
        catch (...) { this->NetworkDiagnosticsTextBox().Text(Mile::WinRT::ToHResultError().message()); }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunHostPreflight()
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        this->NetworkDiagnosticsTextBox().Text(::GetSettingsString(L"DiskOperationRunning"));
        this->NetworkPreflightButton().IsEnabled(false);
        winrt::hstring Result;
        try
        {
            co_await winrt::resume_background();
            Result = winrt::to_hstring(::NanaBox::CheckMirroredHostSupport());
        }
        catch (...) { Result = Mile::WinRT::ToHResultError().message(); }
        try
        {
            co_await Foreground;
            this->NetworkDiagnosticsTextBox().Text(Result);
            this->NetworkPreflightButton().IsEnabled(true);
        }
        catch (...) {}
    }

    void VirtualMachineSettingsPage::SetNetworkManagementBusy(bool Busy)
    {
        this->m_NetworkManagementRunning = Busy;
        this->RefreshNetworksControl().IsEnabled(!Busy);
        this->CreateNetworkControl().IsEnabled(!Busy);
        this->DeleteNetworkControl().IsEnabled(!Busy);
        this->InspectNetworkControl().IsEnabled(!Busy);
        this->HostNetworksComboBox().IsEnabled(!Busy);
        this->NetworkNameTextBox().IsEnabled(!Busy);
        this->NetworkFirewallCheckBox().IsEnabled(!Busy);
        this->NetworkAdvancedSettingsTextBox().IsEnabled(!Busy);
    }

    void VirtualMachineSettingsPage::RefreshNetworksButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        this->RunNetworkManagementOperation(L"Refresh");
    }

    void VirtualMachineSettingsPage::CreateNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (this->m_NetworkManagementRunning) return;
        this->RunNetworkManagementOperation(L"Create", this->NetworkNameTextBox().Text(),
            this->NetworkFirewallCheckBox().IsChecked().Value(), this->NetworkAdvancedSettingsTextBox().Text());
    }

    void VirtualMachineSettingsPage::DeleteNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (this->m_NetworkManagementRunning) return;
        try
        {
            auto Item = this->HostNetworksComboBox().SelectedItem();
            if (!Item) return;
            auto Id = winrt::unbox_value<winrt::hstring>(
                Item.as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>().Tag());
            if (::MessageBoxW(this->m_WindowHandle, ::GetSettingsString(L"DeleteNetworkConfirm").c_str(),
                L"NanaBox", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
            this->RunNetworkManagementOperation(L"Delete", Id);
        }
        catch (...) { ::ShowSettingsError(this->m_WindowHandle); }
    }

    void VirtualMachineSettingsPage::InspectNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (this->m_NetworkManagementRunning) return;
        try
        {
            auto Item = this->HostNetworksComboBox().SelectedItem();
            if (!Item) return;
            this->RunNetworkManagementOperation(L"Inspect", winrt::unbox_value<winrt::hstring>(
                Item.as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>().Tag()));
        }
        catch (...) { ::ShowSettingsError(this->m_WindowHandle); }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunNetworkManagementOperation(
        winrt::hstring Action, winrt::hstring Value, bool Firewall, winrt::hstring CustomSettings)
    {
        if (this->m_NetworkManagementRunning) co_return;
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        using Json = nlohmann::json;
        std::vector<::NanaBox::HostNetworkInfo> Networks;
        Json Result;
        this->SetNetworkManagementBusy(true);
        this->NetworkPropertiesTextBox().Text(::GetSettingsString(L"DiskOperationRunning"));
        try
        {
            co_await winrt::resume_background();
            if (Action == L"Inspect")
            {
                Result = Json::parse(winrt::to_string(this->m_HostOperations->QueryNetwork(
                    winrt::guid(winrt::to_string(Value))).Properties));
                Result["Success"] = true;
                Result["Operation"] = "Inspect";
            }
            else
            {
                if (Action == L"Create") this->m_HostOperations->CreateNetwork(Value, Firewall, CustomSettings);
                else if (Action == L"Delete") this->m_HostOperations->DeleteNetwork(winrt::guid(winrt::to_string(Value)));
                Networks = this->m_HostOperations->EnumerateNetworks();
                Result = {{"Success", true}, {"Operation", winrt::to_string(Action)}, {"Count", Networks.size()}};
            }
        }
        catch (...) { Result = {{"Success", false}, {"Operation", winrt::to_string(Action)},
            {"Error", winrt::to_string(Mile::WinRT::ToHResultError().message())}}; }
        try
        {
            co_await Foreground;
            this->SetNetworkManagementBusy(false);
            if (Action != L"Inspect" && Result.value("Success", false))
            {
                this->m_HostNetworks = std::move(Networks);
                ::FillNetworkPicker(this->HostNetworksComboBox(), this->m_HostNetworks);
                for (auto& Row : this->m_NetworkAdapterSettings)
                    if (Row.NetworkPicker) ::FillNetworkPicker(Row.NetworkPicker, this->m_HostNetworks, true);
                if (Action == L"Create" && this->NetworkNameTextBox().Text() == Value)
                {
                    this->NetworkNameTextBox().Text(L"");
                    this->NetworkAdvancedSettingsTextBox().Text(L"");
                }
            }
            this->NetworkPropertiesTextBox().Text(winrt::to_hstring(Result.dump(2)));
        }
        catch (...)
        {
            this->m_NetworkManagementRunning = false;
            try { this->SetNetworkManagementBusy(false); this->NetworkPropertiesTextBox().Text(Mile::WinRT::ToHResultError().message()); }
            catch (...) {}
        }
    }

    void VirtualMachineSettingsPage::HostManagementButtonClick(winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        if (this->m_HostManagementRunning) return;
        using Json = nlohmann::json;
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        auto Selected = [](winrt::ComboBox const& Box) -> std::string
        {
            if (!Box.SelectedItem()) throw winrt::hresult_invalid_argument(L"Select an item first.");
            return winrt::to_string(winrt::unbox_value<winrt::hstring>(Box.SelectedItem().as<Item>().Tag()));
        };
        auto Confirm = [&]()
        {
            return ::MessageBoxW(this->m_WindowHandle, ::GetSettingsString(L"HostManagementConfirm").c_str(),
                L"NanaBox", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
        };
        try
        {
            auto Action = winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag());
            Json Data = Json::object();
            if (Action == L"SwitchCreate")
            {
                auto Type = Selected(this->ManagedSwitchTypeComboBox());
                if (Type == "External" && !Confirm()) return;
                Data = {{"Name", winrt::to_string(this->ManagedSwitchNameTextBox().Text())},
                    {"Type", Type}, {"Adapter", Type == "External" ? Selected(this->PhysicalAdaptersComboBox()) : ""}};
            }
            else if (Action == L"SwitchDelete")
            {
                Data["Id"] = Selected(this->ManagedSwitchesComboBox());
                if (!Confirm()) return;
            }
            else if (Action == L"CreatorRegister")
                Data["Name"] = winrt::to_string(this->FirewallCreatorNameTextBox().Text());
            else if (Action == L"FirewallInspect")
                Data["Id"] = Selected(this->FirewallCreatorsComboBox());
            else if (Action == L"UseCreator")
            {
                if (this->m_MainWindow) throw winrt::hresult_invalid_argument(L"Stop the VM before changing mirrored firewall settings.");
                this->MirroredFirewallCreatorTextBox().Text(winrt::to_hstring(Selected(this->FirewallCreatorsComboBox())));
                return;
            }
            else if (Action == L"FirewallConfigure")
            {
                if (!Confirm()) return;
                Data = {{"Id", Selected(this->FirewallCreatorsComboBox())},
                    {"Inbound", Selected(this->FirewallInboundComboBox())}, {"Outbound", Selected(this->FirewallOutboundComboBox())},
                    {"Loopback", this->FirewallLoopbackCheckBox().IsChecked().Value()},
                    {"Merge", this->FirewallMergeCheckBox().IsChecked().Value()}};
            }
            else if (Action == L"RuleCreate")
            {
                Data = {{"Id", Selected(this->FirewallCreatorsComboBox())},
                    {"Name", winrt::to_string(this->FirewallRuleNameTextBox().Text())},
                    {"Direction", Selected(this->FirewallDirectionComboBox())}, {"RuleAction", Selected(this->FirewallActionComboBox())},
                    {"Protocol", Selected(this->FirewallProtocolComboBox())}, {"Ports", winrt::to_string(this->FirewallPortsTextBox().Text())}};
            }
            else if (Action == L"RuleEnable" || Action == L"RuleDisable" || Action == L"RuleDelete")
            {
                Data = {{"Id", Selected(this->FirewallCreatorsComboBox())}, {"Rule", Selected(this->FirewallRulesComboBox())}};
                if (Action == L"RuleDelete" && !Confirm()) return;
            }
            else if (Action != L"SwitchList" && Action != L"AdapterList" && Action != L"CreatorList")
                throw winrt::hresult_invalid_argument();
            this->m_HostManagementRunning = true;
            this->HostManagementResult().Text(::GetSettingsString(L"DiskOperationRunning"));
            this->RunHostManagementOperation(Action, Data.dump());
        }
        catch (...) { this->m_HostManagementRunning = false; ::ShowSettingsError(this->m_WindowHandle); }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunHostManagementOperation(
        winrt::hstring Action, std::string Arguments)
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        using Json = nlohmann::json;
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        Json Result, List, Details;
        std::wstring ErrorText;
        try
        {
            co_await winrt::resume_background();
            auto Run = [Operations = this->m_HostOperations](winrt::hstring const& Command, Json const& Data) -> Json
            {
                return Json::parse(winrt::to_string(Operations->Manage(Command, winrt::to_hstring(Data.dump()))));
            };
            auto Data = Json::parse(Arguments);
            Result = Run(Action, Data);
            if (Action == L"SwitchCreate" || Action == L"SwitchDelete")
                List = Run(L"SwitchList", Json::object());
            else if (Action == L"CreatorRegister")
                List = Run(L"CreatorList", Json::object());
            else if (Action == L"FirewallConfigure" || Action == L"RuleCreate" ||
                Action == L"RuleEnable" || Action == L"RuleDisable" || Action == L"RuleDelete")
                Details = Run(L"FirewallInspect", Json{{"Id", Data.at("Id")}});
        }
        catch (...) { ErrorText = Mile::WinRT::ToHResultError().message().c_str(); }

        try
        {
            co_await Foreground;
            this->m_HostManagementRunning = false;
            if (!ErrorText.empty())
            {
                this->HostManagementResult().Text(winrt::to_hstring(Json{{"Success", false}, {"Operation", winrt::to_string(Action)},
                    {"Error", winrt::to_string(ErrorText)}}.dump(2)));
                co_return;
            }
            auto Fill = [](winrt::ComboBox const& Box, Json const& Items, char const* Key, std::string Preferred)
            {
                if (Preferred.empty() && Box.SelectedItem())
                    Preferred = winrt::to_string(winrt::unbox_value<winrt::hstring>(Box.SelectedItem().as<Item>().Tag()));
                Box.Items().Clear();
                int Selection = -1;
                for (auto const& Entry : Items)
                {
                    Item Value;
                    auto Id = Entry.at(Key).get<std::string>();
                    Value.Tag(winrt::box_value(winrt::to_hstring(Id)));
                    auto Name = Entry.value("Name", Id);
                    Value.Content(winrt::box_value(winrt::to_hstring(Name + (Name == Id ? "" : " (" + Id + ")"))));
                    if (Id == Preferred) Selection = static_cast<int>(Box.Items().Size());
                    Box.Items().Append(Value);
                }
                if (Selection >= 0) Box.SelectedIndex(Selection);
                else if (Box.Items().Size()) Box.SelectedIndex(0);
            };
            if (Action == L"SwitchList" || Action == L"SwitchCreate" || Action == L"SwitchDelete")
                Fill(this->ManagedSwitchesComboBox(), List.is_null() ? Result : List, "Id", "");
            else if (Action == L"AdapterList")
                Fill(this->PhysicalAdaptersComboBox(), Result, "Name", "");
            else if (Action == L"CreatorList" || Action == L"CreatorRegister")
            {
                Fill(this->FirewallCreatorsComboBox(), List.is_null() ? Result : List, "Id",
                    Action == L"CreatorRegister" ? Result.at("Id").get<std::string>() : "");
                this->FirewallRulesComboBox().Items().Clear();
            }
            if (Action == L"FirewallInspect") Details = Result;
            if (!Details.is_null())
            {
                Fill(this->FirewallRulesComboBox(), Details.at("Rules"), "Id", "");
                Result = Details;
            }
            if (Result.is_array())
                Result = Json{{"Items", std::move(Result)}};
            Result["Success"] = true;
            Result["Operation"] = winrt::to_string(Action);
            this->HostManagementResult().Text(winrt::to_hstring(Result.dump(2)));
        }
        catch (...)
        {
            this->m_HostManagementRunning = false;
            try { this->HostManagementResult().Text(Mile::WinRT::ToHResultError().message()); }
            catch (...) {}
        }
    }


    void VirtualMachineSettingsPage::HcnKindChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (auto Box = this->HcnResourceComboBox()) Box.Items().Clear();
        if (auto Button = this->HcnModifyButton())
        {
            auto Selected = this->HcnKindComboBox().SelectedItem().try_as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>();
            Button.IsEnabled(!Selected || winrt::unbox_value<winrt::hstring>(Selected.Tag()) != L"Route");
        }
    }

    void VirtualMachineSettingsPage::HcnResourceButtonClick(winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        if (this->m_HcnResourceRunning) return;
        using Json = nlohmann::json;
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        auto Selected = [](winrt::ComboBox const& Box)
        {
            if (!Box.SelectedItem()) throw winrt::hresult_invalid_argument(L"Select an item first.");
            return winrt::to_string(winrt::unbox_value<winrt::hstring>(Box.SelectedItem().as<Item>().Tag()));
        };
        try
        {
            auto Action = winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag());
            Json Data = {{"Kind", Selected(this->HcnKindComboBox())}};
            if (Action != L"HcnList" && Action != L"HcnCreate")
                Data["Id"] = Selected(this->HcnResourceComboBox());
            if (Action == L"HcnCreate")
            {
                Data["Name"] = winrt::to_string(this->HcnNameTextBox().Text());
                Data["NetworkId"] = winrt::to_string(this->HcnNetworkIdTextBox().Text());
            }
            if (Action == L"HcnCreate" || Action == L"HcnModify")
                Data["Settings"] = Json::parse(winrt::to_string(this->HcnSettingsTextBox().Text()));
            if ((Action == L"HcnModify" || Action == L"HcnDelete") &&
                ::MessageBoxW(this->m_WindowHandle, ::GetSettingsString(L"HostManagementConfirm").c_str(),
                    L"NanaBox", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
            this->m_HcnResourceRunning = true;
            this->HcnKindComboBox().IsEnabled(false);
            this->HcnResultTextBox().Text(::GetSettingsString(L"DiskOperationRunning"));
            this->RunHcnResourceOperation(Action, Data.dump());
        }
        catch (...)
        {
            this->m_HcnResourceRunning = false;
            this->HcnKindComboBox().IsEnabled(true);
            this->HcnResultTextBox().Text(Mile::WinRT::ToHResultError().message());
        }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunHcnResourceOperation(
        winrt::hstring Action, std::string Arguments)
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        using Json = nlohmann::json;
        Json Result, Items;
        std::wstring ErrorText;
        try
        {
            co_await winrt::resume_background();
            auto Data = Json::parse(Arguments);
            auto Run = [Operations = this->m_HostOperations](winrt::hstring const& Command, Json const& Request)
            {
                return Json::parse(winrt::to_string(Operations->Manage(Command, winrt::to_hstring(Request.dump()))));
            };
            Result = Run(Action, Data);
            if (Action == L"HcnList") Items = Result.at("Items");
            else if (Action != L"HcnInspect")
            {
                try { Items = Run(L"HcnList", Json{{"Kind", Data.at("Kind")}}).at("Items"); }
                catch (...) { Result["RefreshError"] = winrt::to_string(Mile::WinRT::ToHResultError().message()); }
            }
        }
        catch (...) { ErrorText = Mile::WinRT::ToHResultError().message().c_str(); }
        co_await Foreground;
        this->m_HcnResourceRunning = false;
        this->HcnKindComboBox().IsEnabled(true);
        try
        {
            if (!ErrorText.empty())
                Result = {{"Success", false}, {"Error", winrt::to_string(ErrorText)}};
            else
            {
                Result["Success"] = true;
                if (Items.is_array())
                {
                    auto Box = this->HcnResourceComboBox();
                    Box.Items().Clear();
                    int Selection = -1;
                    for (auto const& Entry : Items)
                    {
                        auto Id = Entry.at("Id").get<std::string>();
                        winrt::Windows::UI::Xaml::Controls::ComboBoxItem Item;
                        Item.Tag(winrt::box_value(winrt::to_hstring(Id)));
                        Item.Content(winrt::box_value(winrt::to_hstring(Entry.at("Name").get<std::string>() + " (" + Id + ")")));
                        if (Result.value("Id", std::string()) == Id) Selection = static_cast<int>(Box.Items().Size());
                        Box.Items().Append(Item);
                    }
                    if (Selection >= 0) Box.SelectedIndex(Selection);
                    else if (Box.Items().Size()) Box.SelectedIndex(0);
                }
            }
            Result["Operation"] = winrt::to_string(Action);
            this->HcnResultTextBox().Text(winrt::to_hstring(Result.dump(2)));
        }
        catch (...) { this->HcnResultTextBox().Text(Mile::WinRT::ToHResultError().message()); }
    }

    void VirtualMachineSettingsPage::VmmsSelectionChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (auto Box = this->VmmsCheckpointsComboBox()) Box.Items().Clear();
        if (auto Box = this->VmmsDevicesComboBox()) Box.Items().Clear();
        if (auto Box = this->VmmsDeviceSwitchComboBox()) Box.Items().Clear();
    }

    void VirtualMachineSettingsPage::VmmsButtonClick(winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        if (this->m_VmmsRunning) return;
        using Json = nlohmann::json;
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        auto Selected = [](winrt::ComboBox const& Box) -> std::string
        {
            if (!Box.SelectedItem()) throw winrt::hresult_invalid_argument(L"Select an item first.");
            return winrt::to_string(winrt::unbox_value<winrt::hstring>(Box.SelectedItem().as<Item>().Tag()));
        };
        auto Number = [](winrt::TextBox const& Box, std::uint64_t Minimum, std::uint64_t Maximum)
        {
            auto Text = winrt::to_string(Box.Text());
            std::uint64_t Value = 0;
            auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
            if (Parsed.ec != std::errc() || Parsed.ptr != Text.data() + Text.size() ||
                Value < Minimum || Value > Maximum) throw winrt::hresult_invalid_argument(L"Invalid numeric value.");
            return Value;
        };
        try
        {
            auto Action = winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag());
            std::wstring Confirmation;
            Json Data = Json::object();
            if (Action == L"Execute")
            {
                Action = winrt::to_hstring(Selected(this->VmmsActionComboBox()));
                Confirmation = winrt::unbox_value<winrt::hstring>(this->VmmsActionComboBox().SelectedItem().as<Item>().Content()).c_str();
                if (Action == L"VmmsCreate")
                {
                    Data["Name"] = winrt::to_string(this->VmmsNameTextBox().Text());
                    Data["Generation"] = this->VmmsGenerationComboBox().SelectedIndex() + 1;
                    Data["MemoryMiB"] = Number(this->VmmsMemoryTextBox(), 32, 16777216);
                    Data["Path"] = winrt::to_string(this->VmmsPathTextBox().Text());
                    Confirmation += L"\n" + std::wstring(this->VmmsNameTextBox().Text().c_str());
                }
                else if (Action == L"VmmsImportRegister" || Action == L"VmmsImportCopy")
                {
                    Data["Path"] = winrt::to_string(this->VmmsPathTextBox().Text());
                    Data["Destination"] = winrt::to_string(this->VmmsDestinationTextBox().Text());
                    Confirmation += L"\n" + std::wstring(this->VmmsPathTextBox().Text().c_str());
                    if (Action == L"VmmsImportCopy")
                        Confirmation += L"\n" + std::wstring(this->VmmsDestinationTextBox().Text().c_str());
                }
                else
                {
                    Data["Id"] = Selected(this->VmmsMachinesComboBox());
                    Confirmation += L"\n" + std::wstring(winrt::unbox_value<winrt::hstring>(this->VmmsMachinesComboBox().SelectedItem().as<Item>().Content()).c_str());
                    if (Action == L"VmmsRename" || Action == L"VmmsCheckpointCreate")
                    {
                        Data["Name"] = winrt::to_string(this->VmmsNameTextBox().Text());
                        Confirmation += L"\n" + std::wstring(this->VmmsNameTextBox().Text().c_str());
                    }
                    else if (Action == L"VmmsAutomaticActions")
                    {
                        Data["StartAction"] = Selected(this->VmmsAutomaticStartComboBox());
                        Data["StopAction"] = Selected(this->VmmsAutomaticStopComboBox());
                        Data["Delay"] = Number(this->VmmsAutomaticDelayTextBox(), 0, 2147483647);
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsCheckpointPolicy")
                    {
                        Data["Type"] = Selected(this->VmmsCheckpointTypeComboBox());
                        Data["Enabled"] = this->VmmsAutomaticCheckpointsCheckBox().IsChecked().Value();
                        if (Data["Type"] == "Disabled" && Data["Enabled"] == true)
                            throw winrt::hresult_invalid_argument(L"Automatic checkpoints require checkpoints to be enabled.");
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsTpmDisable" || Action == L"VmmsDeleteSavedState")
                    {
                        Confirmation += Action == L"VmmsTpmDisable"
                            ? L"\nEnsure that guest encryption recovery keys are available before disabling TPM."
                            : L"\nThe saved execution state will be discarded.";
                    }
                    else if (Action == L"VmmsCheckpointRestore" || Action == L"VmmsCheckpointRemove" ||
                        Action == L"VmmsCheckpointRename" || Action == L"VmmsCheckpointExport")
                    {
                        Data["CheckpointId"] = Selected(this->VmmsCheckpointsComboBox());
                        Confirmation += L"\n" + std::wstring(winrt::unbox_value<winrt::hstring>(this->VmmsCheckpointsComboBox().SelectedItem().as<Item>().Content()).c_str());
                        if (Action == L"VmmsCheckpointRename")
                        {
                            Data["Name"] = winrt::to_string(this->VmmsNameTextBox().Text());
                            Confirmation += L"\n" + std::wstring(this->VmmsNameTextBox().Text().c_str());
                        }
                        else if (Action == L"VmmsCheckpointExport")
                        {
                            Data["Path"] = winrt::to_string(this->VmmsPathTextBox().Text());
                            Confirmation += L"\n" + std::wstring(this->VmmsPathTextBox().Text().c_str());
                        }
                    }
                    else if (Action == L"VmmsExport" || Action == L"VmmsMoveStorage")
                    {
                        Data["Path"] = winrt::to_string(this->VmmsPathTextBox().Text());
                        Confirmation += L"\n" + std::wstring(this->VmmsPathTextBox().Text().c_str());
                    }
                    else if (Action == L"VmmsSetProcessor")
                    {
                        Data["Count"] = Number(this->VmmsProcessorTextBox(), 1, 2048);
                        Confirmation += L"\n" + std::wstring(this->VmmsProcessorTextBox().Text().c_str());
                    }
                    else if (Action == L"VmmsDynamicMemory")
                    {
                        Data["MinimumMiB"] = Number(this->VmmsMinimumMemoryTextBox(), 32, 16777216);
                        Data["MemoryMiB"] = Number(this->VmmsMemoryTextBox(), 32, 16777216);
                        Data["MaximumMiB"] = Number(this->VmmsMaximumMemoryTextBox(), 32, 16777216);
                        Data["Buffer"] = Number(this->VmmsMemoryBufferTextBox(), 5, 2000);
                        Data["Priority"] = Number(this->VmmsMemoryPriorityTextBox(), 0, 100);
                        if (Data["MinimumMiB"] > Data["MemoryMiB"] || Data["MemoryMiB"] > Data["MaximumMiB"])
                            throw winrt::hresult_invalid_argument(L"Invalid dynamic memory range.");
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsMigrate" || Action == L"VmmsReplicationEnable")
                    {
                        Data["Host"] = winrt::to_string(this->VmmsRemoteHostTextBox().Text());
                        if (Data["Host"].get<std::string>().empty()) throw winrt::hresult_invalid_argument(L"Specify a destination host.");
                        if (Action == L"VmmsMigrate")
                            Data["Path"] = winrt::to_string(this->VmmsRemotePathTextBox().Text());
                        else
                        {
                            Data["Port"] = Number(this->VmmsReplicaPortTextBox(), 1, 65535);
                            Data["Frequency"] = Number(this->VmmsReplicaFrequencyTextBox(), 30, 900);
                            if (Data["Frequency"] != 30 && Data["Frequency"] != 300 && Data["Frequency"] != 900)
                                throw winrt::hresult_invalid_argument(L"Replication interval must be 30, 300 or 900 seconds.");
                            Data["RecoveryHistory"] = Number(this->VmmsReplicaHistoryTextBox(), 0, 24);
                            Data["Authentication"] = Selected(this->VmmsReplicaAuthComboBox());
                            Data["Thumbprint"] = winrt::to_string(this->VmmsReplicaCertificateTextBox().Text());
                            Data["Compression"] = this->VmmsReplicaCompressionCheckBox().IsChecked().Value();
                        }
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsCpuPolicy")
                    {
                        Data["Reserve"] = Number(this->VmmsCpuReserveTextBox(), 0, 100);
                        Data["Maximum"] = Number(this->VmmsCpuMaximumTextBox(), 0, 100);
                        Data["Weight"] = Number(this->VmmsCpuWeightTextBox(), 1, 10000);
                        Data["Nested"] = this->VmmsCpuNestedCheckBox().IsChecked().Value();
                        if (Data["Reserve"] > Data["Maximum"]) throw winrt::hresult_invalid_argument(L"CPU reserve exceeds maximum.");
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsDiskAttach" || Action == L"VmmsDvdAdd" || Action == L"VmmsNicAdd" || Action == L"VmmsSecureBoot")
                    {
                        if (Action == L"VmmsNicAdd")
                            Data["Name"] = winrt::to_string(this->VmmsDeviceNameTextBox().Text());
                        else if (Action == L"VmmsSecureBoot")
                        {
                            Data["Enabled"] = this->VmmsSecureBootCheckBox().IsChecked().Value();
                            Data["Template"] = Selected(this->VmmsSecureBootTemplateComboBox());
                        }
                        else
                        {
                            Data["Path"] = winrt::to_string(this->VmmsDevicePathTextBox().Text());
                            Data["ControllerType"] = Selected(this->VmmsControllerTypeComboBox());
                            Data["ControllerNumber"] = Number(this->VmmsControllerNumberTextBox(), 0, 3);
                            Data["ControllerLocation"] = Number(this->VmmsControllerLocationTextBox(), 0, 63);
                            if (Action == L"VmmsDiskAttach" && Data["Path"].get<std::string>().empty())
                                throw winrt::hresult_invalid_argument(L"Choose a virtual disk file.");
                        }
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsDiskDetach" || Action == L"VmmsDiskQoS" ||
                        Action == L"VmmsDvdMount" || Action == L"VmmsDvdRemove" ||
                        Action == L"VmmsNicRemove" || Action == L"VmmsNicConnect" ||
                        Action == L"VmmsNicDisconnect" || Action == L"VmmsNicMac" ||
                        Action == L"VmmsNicVlan" || Action == L"VmmsNicGuard" || Action == L"VmmsIntegration")
                    {
                        Data["DeviceId"] = Selected(this->VmmsDevicesComboBox());
                        if (Action == L"VmmsDiskQoS")
                        {
                            Data["MinimumIOPS"] = Number(this->VmmsDiskMinIopsTextBox(), 0, UINT64_MAX);
                            Data["MaximumIOPS"] = Number(this->VmmsDiskMaxIopsTextBox(), 0, UINT64_MAX);
                        }
                        else if (Action == L"VmmsDvdMount")
                            Data["Path"] = winrt::to_string(this->VmmsDevicePathTextBox().Text());
                        else if (Action == L"VmmsNicConnect")
                            Data["SwitchId"] = Selected(this->VmmsDeviceSwitchComboBox());
                        else if (Action == L"VmmsNicMac")
                            Data["Mac"] = winrt::to_string(this->VmmsNicMacTextBox().Text());
                        else if (Action == L"VmmsNicVlan")
                            Data["Vlan"] = Number(this->VmmsNicVlanTextBox(), 0, 4094);
                        else if (Action == L"VmmsNicGuard")
                        {
                            Data["DhcpGuard"] = this->VmmsNicDhcpGuardCheckBox().IsChecked().Value();
                            Data["RouterGuard"] = this->VmmsNicRouterGuardCheckBox().IsChecked().Value();
                            Data["Spoofing"] = this->VmmsNicSpoofCheckBox().IsChecked().Value();
                        }
                        else if (Action == L"VmmsIntegration")
                            Data["Enabled"] = this->VmmsIntegrationEnabledCheckBox().IsChecked().Value();
                        Confirmation += L"\n" + std::wstring(winrt::unbox_value<winrt::hstring>(this->VmmsDevicesComboBox().SelectedItem().as<Item>().Content()).c_str());
                        Confirmation += L"\n" + std::wstring(winrt::to_hstring(Data.dump(2)).c_str());
                    }
                    else if (Action == L"VmmsSetMemory" || Action == L"VmmsStaticMemory")
                    {
                        Data["MemoryMiB"] = Number(this->VmmsMemoryTextBox(), 32, 16777216);
                        Confirmation += L"\n" + std::wstring(this->VmmsMemoryTextBox().Text().c_str()) + L" MiB";
                    }
                }
                Confirmation += L"\n\n" + std::wstring(::GetSettingsString(L"HostManagementConfirm").c_str());
                if (::MessageBoxW(this->m_WindowHandle, Confirmation.c_str(), L"NanaBox",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
            }
            else if (Action == L"VmmsInspect") Data["Id"] = Selected(this->VmmsMachinesComboBox());
            else if (Action == L"HcsServiceProperties" || Action == L"HcsSystemProperties")
            {
                if (Action == L"HcsSystemProperties") Data["Id"] = Selected(this->HcsSystemsComboBox());
                auto Query = winrt::to_string(this->HcsQueryTextBox().Text());
                Data["Query"] = Query.empty() ? Json::object() : Json::parse(Query);
                if (!Data["Query"].is_object())
                    throw winrt::hresult_invalid_argument(L"Property query must be a JSON object.");
            }
            else if (Action != L"VmmsList" && Action != L"HcsInventory") throw winrt::hresult_invalid_argument();
            this->m_VmmsRunning = true;
            this->VmmsForm().IsEnabled(false);
            this->VmmsResultTextBox().Text(::GetSettingsString(L"DiskOperationRunning"));
            this->RunVmmsOperation(Action, Data.dump());
        }
        catch (...)
        {
            this->m_VmmsRunning = false;
            this->VmmsForm().IsEnabled(true);
            this->VmmsResultTextBox().Text(winrt::to_hstring(Json{{"Success", false},
                {"Error", winrt::to_string(Mile::WinRT::ToHResultError().message())}}.dump(2)));
        }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunVmmsOperation(winrt::hstring Action, std::string Arguments)
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        using Json = nlohmann::json;
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        Json Result;
        try
        {
            co_await winrt::resume_background();
            Result = Json::parse(winrt::to_string(this->m_HostOperations->Manage(Action, winrt::to_hstring(Arguments))));
        }
        catch (...)
        {
            Result = {{"Success", false}, {"Error", winrt::to_string(Mile::WinRT::ToHResultError().message())}};
        }
        try
        {
            co_await Foreground;
            this->m_VmmsRunning = false;
            this->VmmsForm().IsEnabled(true);
            if (Action == L"HcsInventory" && Result.value("Success", true))
            {
                auto Box = this->HcsSystemsComboBox();
                Box.Items().Clear();
                for (auto const& Entry : Result.at("Items"))
                {
                    auto Id = Entry.at("Id").get<std::string>();
                    Item Value;
                    Value.Tag(winrt::box_value(winrt::to_hstring(Id)));
                    Value.Content(winrt::box_value(winrt::to_hstring(Id + " - " +
                        Entry.value("SystemType", std::string()) + " - " + Entry.value("State", std::string()))));
                    Box.Items().Append(Value);
                }
                if (Box.Items().Size()) Box.SelectedIndex(0);
            }
            else if (std::wstring_view(Action.c_str()).starts_with(L"Vmms") && Result.value("Success", true))
            {
                auto Fill = [](winrt::ComboBox const& Box, Json const& Items, std::string const& Preferred)
                {
                    Box.Items().Clear();
                    int Selection = -1;
                    for (auto const& Entry : Items)
                    {
                        auto Id = Entry.at("Id").get<std::string>();
                        auto Name = Entry.at("Name").get<std::string>();
                        Item Value;
                        Value.Tag(winrt::box_value(winrt::to_hstring(Id)));
                        auto State = Entry.value("State", std::string());
                        Value.Content(winrt::box_value(winrt::to_hstring(Name + " (" + Id + ")" + (State.empty() ? "" : " - " + State))));
                        if (Id == Preferred) Selection = static_cast<int>(Box.Items().Size());
                        Box.Items().Append(Value);
                    }
                    if (Selection >= 0) Box.SelectedIndex(Selection);
                    else if (Box.Items().Size()) Box.SelectedIndex(0);
                };
                std::string Preferred;
                if (Result.contains("SelectedId") && Result.at("SelectedId").is_string())
                    Preferred = Result.at("SelectedId").get<std::string>();
                else if (this->VmmsMachinesComboBox().SelectedItem())
                    Preferred = winrt::to_string(winrt::unbox_value<winrt::hstring>(
                        this->VmmsMachinesComboBox().SelectedItem().as<Item>().Tag()));
                Fill(this->VmmsMachinesComboBox(), Result.at("Items"), Preferred);
                if (Result.contains("Details") && Result.at("Details").is_object())
                {
                    auto const& Details = Result.at("Details");
                    Fill(this->VmmsCheckpointsComboBox(), Details.at("Checkpoints"), "");
                    auto Devices = Details.value("Devices", Json::array());
                    Fill(this->VmmsDevicesComboBox(), Devices, "");
                    Fill(this->VmmsDeviceSwitchComboBox(), Details.value("Switches", Json::array()), "");
                }
            }
            if (!Result.contains("Success")) Result["Success"] = true;
            Result["Operation"] = winrt::to_string(Action);
            this->VmmsResultTextBox().Text(winrt::to_hstring(Result.dump(2)));
        }
        catch (...)
        {
            this->m_VmmsRunning = false;
            try
            {
                this->VmmsForm().IsEnabled(true);
                this->VmmsResultTextBox().Text(Mile::WinRT::ToHResultError().message());
            }
            catch (...) {}
        }
    }

    void VirtualMachineSettingsPage::DiskManagerButtonClick(winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        if (this->m_DiskOperationRunning) return;
        try
        {
            auto Action = winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag());
            if (Action == L"Browse")
            {
                COMDLG_FILTERSPEC Types[] = {{L"Virtual disks (*.vhd;*.vhdx)", L"*.vhd;*.vhdx"}};
                LPWSTR Raw = nullptr;
                HRESULT Hr = ::ShowFileDialog(this->m_WindowHandle, FALSE, ARRAYSIZE(Types), Types,
                    nullptr, nullptr, nullptr, &Raw);
                auto Free = Mile::ScopeExitTaskHandler([&]() { ::CoTaskMemFree(Raw); });
                if (Hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
                winrt::check_hresult(Hr);
                if (Raw) this->DiskPathTextBox().Text(winrt::hstring(Raw));
                return;
            }
            auto Path = this->DiskPathTextBox().Text();
            auto Extension = ::PathFindExtensionW(Path.c_str());
            bool Vhd = ::_wcsicmp(Extension, L".vhd") == 0;
            if (Path.empty() || (!Vhd && ::_wcsicmp(Extension, L".vhdx") != 0))
                throw winrt::hresult_invalid_argument(L"Specify a VHD or VHDX file.");
            std::uint64_t Size = 0;
            if (Action == L"Create" || Action == L"Resize")
            {
                auto Value = winrt::to_string(this->DiskSizeTextBox().Text());
                std::uint64_t MiB = 0;
                auto Parsed = std::from_chars(Value.data(), Value.data() + Value.size(), MiB);
                std::uint64_t Limit = Vhd ? 2040ull * 1024 : 64ull * 1024 * 1024;
                if (Parsed.ec != std::errc() || Parsed.ptr != Value.data() + Value.size() || MiB < 3 || MiB > Limit)
                    throw winrt::hresult_invalid_argument(L"Invalid virtual disk capacity in MiB.");
                Size = MiB << 20;
            }
            this->m_DiskOperationRunning = true;
            this->DiskResultTextBox().Text(::GetSettingsString(L"DiskOperationRunning"));
            this->RunDiskManagerOperation(Action, Path, Size);
        }
        catch (...) { this->m_DiskOperationRunning = false; ::ShowSettingsError(this->m_WindowHandle); }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunDiskManagerOperation(
        winrt::hstring Action, winrt::hstring Path, std::uint64_t Size)
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        std::string Result;
        try
        {
            co_await winrt::resume_background();
            auto Inspect = [&]() {
                VIRTUAL_STORAGE_TYPE Type{VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN, VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN};
                OPEN_VIRTUAL_DISK_PARAMETERS Open{};
                Open.Version = OPEN_VIRTUAL_DISK_VERSION_1;
                HANDLE Raw = INVALID_HANDLE_VALUE;
                winrt::check_hresult(HRESULT_FROM_WIN32(::OpenVirtualDisk(&Type, Path.c_str(),
                    VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE, &Open, &Raw)));
                winrt::handle Disk(Raw);
                GET_VIRTUAL_DISK_INFO Info{};
                Info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
                ULONG Bytes = sizeof(Info);
                winrt::check_hresult(HRESULT_FROM_WIN32(::GetVirtualDiskInformation(Disk.get(), &Bytes, &Info, nullptr)));
                return nlohmann::json{{"Path", winrt::to_string(Path)}, {"VirtualSize", Info.Size.VirtualSize},
                    {"PhysicalSize", Info.Size.PhysicalSize}, {"BlockSize", Info.Size.BlockSize}, {"SectorSize", Info.Size.SectorSize}};
            };
            if (Action == L"Create")
            {
                HANDLE Raw = INVALID_HANDLE_VALUE;
                winrt::check_hresult(HRESULT_FROM_WIN32(::SimpleCreateVirtualDisk(Path.c_str(), Size, &Raw)));
                ::CloseHandle(Raw);
            }
            else if (Action == L"Resize")
            {
                if (Size < Inspect().at("VirtualSize").get<std::uint64_t>())
                    throw winrt::hresult_invalid_argument(L"Use a larger capacity to expand the virtual disk.");
                winrt::check_hresult(HRESULT_FROM_WIN32(::SimpleResizeVirtualDisk(Path.c_str(), Size)));
            }
            else if (Action == L"Compact")
                winrt::check_hresult(HRESULT_FROM_WIN32(::SimpleCompactVirtualDisk(Path.c_str())));
            else if (Action != L"Inspect")
                throw winrt::hresult_invalid_argument();
            auto Data = Inspect();
            Data["Operation"] = winrt::to_string(Action);
            Data["Success"] = true;
            Result = Data.dump(2);
        }
        catch (...)
        {
            auto Error = Mile::WinRT::ToHResultError();
            Result = nlohmann::json{{"Success", false}, {"Error", winrt::to_string(Error.message())}}.dump(2);
        }
        try
        {
            co_await Foreground;
            this->m_DiskOperationRunning = false;
            this->DiskResultTextBox().Text(winrt::to_hstring(Result));
        }
        catch (...) {}
    }

    void VirtualMachineSettingsPage::SerialPipeButtonClick(winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
    {
        try
        {
            auto Action = winrt::unbox_value<winrt::hstring>(
                Sender.as<winrt::Windows::UI::Xaml::Controls::Button>().Tag());
            auto Input = (Action == L"1" || Action == L"Clear1" || Action == L"Check1") ? this->ComPort1TextBox() : this->ComPort2TextBox();
            if (Action == L"Clear1" || Action == L"Clear2") { Input.Text(L""); this->SerialPipeStatusTextBox().Text(L""); return; }
            if (Action == L"Check1" || Action == L"Check2")
            {
                auto Pipe = Input.Text();
                if (Pipe.empty()) { this->SerialPipeStatusTextBox().Text(::GetSettingsString(L"SerialNotConfigured")); return; }
                if (std::wstring_view(Pipe.c_str()).find(LR"(\\.\pipe\)") != 0)
                { this->SerialPipeStatusTextBox().Text(::GetSettingsString(L"SerialLocalPipeOnly")); return; }
                BOOL Ready = ::WaitNamedPipeW(Pipe.c_str(), 1);
                DWORD Error = Ready ? ERROR_SUCCESS : ::GetLastError();
                this->SerialPipeStatusTextBox().Text(Ready ? ::GetSettingsString(L"SerialPipeReady") :
                    winrt::hstring(std::wstring(::GetSettingsString(L"SerialPipeUnavailable").c_str()) +
                        L" (" + std::to_wstring(Error) + L")"));
                return;
            }
            GUID Id{};
            winrt::check_hresult(::CoCreateGuid(&Id));
            std::wstring Pipe = LR"(\\.\pipe\NanaBox-)";
            Pipe += ::FromGuid(Id).c_str();
            Input.Text(winrt::hstring(Pipe));
        }
        catch (...) { ::ShowSettingsError(this->m_WindowHandle); }
    }

    void VirtualMachineSettingsPage::AddGpuDeviceButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            this->AssignmentModeComboBox().SelectedIndex(static_cast<int>(NanaBox::GpuAssignmentMode::List));
            this->AddGpuDevice(std::string(), 65535);
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }

    void VirtualMachineSettingsPage::AddNetworkAdapter(
        NanaBox::NetworkAdapterConfiguration const& Configuration,
        _In_ bool Existing)
    {
        NetworkAdapterSettings Row;
        Row.EndpointId = Configuration.EndpointId;
        Row.Container = winrt::StackPanel();
        Row.Container.Spacing(4);
        Row.Container.Margin(winrt::ThicknessHelper::FromLengths(
            8, 0, 0, 8));
        Row.Connected = ::AddSettingCheckBox(
            Row.Container,
            L"NetworkAdapterConnectedLabel",
            L"NetworkAdapterConnectedHelp",
            Configuration.Connected,
            false);
        Row.MacAddress = ::AddSettingTextBox(
            Row.Container,
            L"NetworkAdapterMacAddressLabel",
            L"NetworkAdapterMacAddressHelp",
            winrt::to_hstring(Configuration.MacAddress),
            false);
        Row.NetworkId = ::AddSettingTextBox(Row.Container, L"NetworkIdLabel", L"NetworkIdHelp",
            winrt::to_hstring(Configuration.NetworkId), this->m_MainWindow && Existing);
        Row.FirewallCreatorId = ::AddSettingTextBox(Row.Container, L"FirewallCreatorLabel", L"FirewallCreatorHelp",
            winrt::to_hstring(Configuration.FirewallCreatorId), this->m_MainWindow && Existing);
        if (!(this->m_MainWindow && Existing))
        {
            Row.NetworkPicker = ::MakeHostPicker(Row.Container, Row.NetworkId, L"HostNetworkLabel", L"HostNetworkHelp");
            ::FillNetworkPicker(Row.NetworkPicker, this->m_HostNetworks, true);
        }
        Row.SuggestedSwitchId = ::AddSettingTextBox(
            Row.Container,
            L"NetworkAdapterSuggestedSwitchIdLabel",
            L"NetworkAdapterSuggestedSwitchIdHelp",
            winrt::to_hstring(Configuration.SuggestedSwitchId),
            this->m_MainWindow && Existing);
        Row.SuggestedSwitchName = ::AddSettingTextBox(
            Row.Container,
            L"NetworkAdapterSuggestedSwitchNameLabel",
            L"NetworkAdapterSuggestedSwitchNameHelp",
            winrt::to_hstring(Configuration.SuggestedSwitchName),
            this->m_MainWindow && Existing);
        Row.SuggestedSwitchSubnet = ::AddSettingTextBox(
            Row.Container,
            L"NetworkAdapterSuggestedSwitchSubnetLabel",
            L"NetworkAdapterSuggestedSwitchSubnetHelp",
            winrt::to_hstring(Configuration.SuggestedSwitchSubnet),
            this->m_MainWindow && Existing);

        winrt::Windows::UI::Xaml::Controls::Button RemoveButton;
        winrt::AutomationProperties::SetAutomationId(RemoveButton, L"NetworkRemove");
        RemoveButton.Content(winrt::box_value(
            ::GetSettingsString(L"Remove")));
        RemoveButton.Visibility(winrt::Visibility::Visible);
        RemoveButton.Click([
            Weak = this->get_weak(),
            WeakContainer = winrt::make_weak(Row.Container)](
                winrt::IInspectable const& Sender,
                winrt::RoutedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Sender);
            UNREFERENCED_PARAMETER(Arguments);

            auto Instance = Weak.get();
            if (!Instance)
            {
                return;
            }
            try
            {
                auto Container = WeakContainer.get();
                if (!Container)
                {
                    return;
                }
                std::uint32_t Index = 0;
                if (!Instance->NetworkAdapterPanel().Children().IndexOf(
                    Container, Index))
                {
                    return;
                }
                Instance->NetworkAdapterPanel().Children().RemoveAt(Index);
                auto& Settings = Instance->m_NetworkAdapterSettings;
                Settings.erase(Settings.begin() + Index);
            }
            catch (...)
            {
                ::ShowSettingsError(Instance->m_WindowHandle);
            }
        });
        Row.Container.Children().Append(RemoveButton);
        this->m_NetworkAdapterSettings.push_back(Row);
        try
        {
            this->NetworkAdapterPanel().Children().Append(Row.Container);
        }
        catch (...)
        {
            this->m_NetworkAdapterSettings.pop_back();
            throw;
        }
    }

    void VirtualMachineSettingsPage::AddNetworkAdapterButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            NanaBox::NetworkAdapterConfiguration Configuration;
            Configuration.Connected = true;
            this->AddNetworkAdapter(Configuration);
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }

    void VirtualMachineSettingsPage::BrowseScsiDevicePath(
        winrt::ComboBox const& Type,
        winrt::TextBox const& Path)
    {
        std::int32_t Index = Type.SelectedIndex();
        if (Index != 0 && Index != 1)
        {
            return;
        }
        COMDLG_FILTERSPEC FileType = {};
        FileType.pszName = Index == 0
            ? L"Virtual Hard Disk Files (*.vhd;*.vhdx)" : L"ISO (*.iso)";
        FileType.pszSpec = Index == 0 ? L"*.vhd;*.vhdx" : L"*.iso";
        LPWSTR RawFilePath = nullptr;
        HRESULT Result = ::ShowFileDialog(
            this->m_WindowHandle,
            FALSE,
            1,
            &FileType,
            ::GetSettingsString(Index == 0
                ? L"VirtualDiskPathTip" : L"VirtualImagePathTip").c_str(),
            nullptr,
            nullptr,
            &RawFilePath);
        if (HRESULT_FROM_WIN32(ERROR_CANCELLED) == Result)
        {
            return;
        }
        winrt::check_hresult(Result);
        auto FreeFilePath = Mile::ScopeExitTaskHandler([RawFilePath]()
        {
            ::CoTaskMemFree(RawFilePath);
        });
        std::wstring FilePath(RawFilePath);
        if (!FilePath.empty())
        {
            Path.Text(winrt::hstring(::GetRelativeFilePath(
                this->m_ConfigurationFilePath,
                FilePath)));
        }
    }

    void VirtualMachineSettingsPage::AddScsiDevice(
        NanaBox::ScsiDeviceConfiguration const& Configuration,
        _In_ bool Existing)
    {
        ScsiDeviceSettings Row;
        Row.Container = winrt::StackPanel();
        Row.Container.Spacing(4);
        Row.Container.Margin(winrt::ThicknessHelper::FromLengths(
            8, 0, 0, 8));
        Row.Type = winrt::ComboBox();
        Row.Type.MinWidth(120);
        Row.Type.HorizontalAlignment(winrt::HorizontalAlignment::Left);
        Row.Type.Items().Append(winrt::box_value(
            ::GetSettingsString(L"ChoiceVirtualDisk")));
        Row.Type.Items().Append(winrt::box_value(
            ::GetSettingsString(L"ChoiceVirtualImage")));
        Row.Type.Items().Append(winrt::box_value(
            ::GetSettingsString(L"ChoicePhysicalDevice")));
        Row.Type.SelectedIndex(
            static_cast<std::int32_t>(Configuration.Type) - 1);
        Row.Type.IsEnabled(!(this->m_MainWindow && Existing));
        ::AddSettingRow(
            Row.Container,
            Row.Type,
            L"ScsiDeviceTypeLabel",
            L"ScsiDeviceTypeHelp",
            true);
        Row.Path = winrt::TextBox();
        Row.Path.Text(winrt::to_hstring(Configuration.Path));
        Row.Path.MinWidth(250);
        Row.Path.HorizontalAlignment(winrt::HorizontalAlignment::Stretch);
        winrt::Grid PathRow = ::AddSettingRow(
            Row.Container,
            Row.Path,
            L"ScsiDevicePathLabel",
            L"ScsiDevicePathHelp",
            true);

        winrt::Windows::UI::Xaml::Controls::Button BrowseButton;
        winrt::AutomationProperties::SetAutomationId(BrowseButton, L"ScsiBrowse");
        BrowseButton.Content(winrt::box_value(L"..."));
        BrowseButton.Margin(winrt::ThicknessHelper::FromLengths(8, 0, 0, 0));
        BrowseButton.Visibility(
            Configuration.Type == NanaBox::ScsiDeviceType::PhysicalDevice
                ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
        winrt::AutomationProperties::SetName(
            BrowseButton,
            ::GetSettingsString(L"BrowseScsiDevicePath"));
        winrt::ColumnDefinition BrowseColumn;
        BrowseColumn.Width(winrt::GridLengthHelper::Auto());
        PathRow.ColumnDefinitions().Append(BrowseColumn);
        winrt::Grid::SetColumn(BrowseButton, 3);
        PathRow.Children().Append(BrowseButton);
        Row.Type.SelectionChanged([
            WeakBrowse = winrt::make_weak(BrowseButton)](
                winrt::IInspectable const& Sender,
                winrt::SelectionChangedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Arguments);
            if (auto Button = WeakBrowse.get())
            {
                Button.Visibility(
                    Sender.as<winrt::ComboBox>().SelectedIndex() == 2
                        ? winrt::Visibility::Collapsed
                        : winrt::Visibility::Visible);
            }
        });
        BrowseButton.Click([
            Weak = this->get_weak(),
            WeakType = winrt::make_weak(Row.Type),
            WeakPath = winrt::make_weak(Row.Path)](
                winrt::IInspectable const& Sender,
                winrt::RoutedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Sender);
            UNREFERENCED_PARAMETER(Arguments);
            if (auto Instance = Weak.get())
            {
                try
                {
                    auto Type = WeakType.get();
                    auto Path = WeakPath.get();
                    if (Type && Path)
                    {
                        Instance->BrowseScsiDevicePath(Type, Path);
                    }
                }
                catch (...)
                {
                    ::ShowSettingsError(Instance->m_WindowHandle);
                }
            }
        });

        winrt::Windows::UI::Xaml::Controls::Button RemoveButton;
        winrt::AutomationProperties::SetAutomationId(RemoveButton, L"ScsiRemove");
        RemoveButton.Content(winrt::box_value(
            ::GetSettingsString(L"Remove")));
        RemoveButton.Visibility(!(this->m_MainWindow && Existing)
            ? winrt::Visibility::Visible
            : winrt::Visibility::Collapsed);
        RemoveButton.Click([
            Weak = this->get_weak(),
            WeakContainer = winrt::make_weak(Row.Container)](
                winrt::IInspectable const& Sender,
                winrt::RoutedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Sender);
            UNREFERENCED_PARAMETER(Arguments);

            auto Instance = Weak.get();
            if (!Instance)
            {
                return;
            }
            try
            {
                auto Container = WeakContainer.get();
                if (!Container)
                {
                    return;
                }
                std::uint32_t Index = 0;
                if (!Instance->ScsiDevicePanel().Children().IndexOf(
                    Container, Index))
                {
                    return;
                }
                Instance->ScsiDevicePanel().Children().RemoveAt(Index);
                auto& Settings = Instance->m_ScsiDeviceSettings;
                Settings.erase(Settings.begin() + Index);
            }
            catch (...)
            {
                ::ShowSettingsError(Instance->m_WindowHandle);
            }
        });
        Row.Container.Children().Append(RemoveButton);
        this->m_ScsiDeviceSettings.push_back(Row);
        try
        {
            this->ScsiDevicePanel().Children().Append(Row.Container);
        }
        catch (...)
        {
            this->m_ScsiDeviceSettings.pop_back();
            throw;
        }
    }

    void VirtualMachineSettingsPage::AddScsiDeviceButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            NanaBox::ScsiDeviceConfiguration Configuration;
            Configuration.Type = NanaBox::ScsiDeviceType::VirtualDisk;
            this->AddScsiDevice(Configuration);
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }

    void VirtualMachineSettingsPage::AddPlan9Share(
        NanaBox::Plan9ShareConfiguration const& Configuration)
    {
        Plan9ShareSettings Row;
        Row.Container = winrt::StackPanel();
        Row.Container.Spacing(4);
        Row.Container.Margin(winrt::ThicknessHelper::FromLengths(
            8, 0, 0, 8));
        Row.ReadOnly = ::AddSettingCheckBox(
            Row.Container,
            L"Plan9ShareReadOnlyLabel",
            L"Plan9ShareReadOnlyHelp",
            Configuration.ReadOnly,
            false);
        Row.Port = ::AddSettingTextBox(
            Row.Container,
            L"Plan9SharePortLabel",
            L"Plan9SharePortHelp",
            winrt::to_hstring(Configuration.Port),
            false);
        Row.Port.HorizontalAlignment(winrt::HorizontalAlignment::Left);
        Row.Path = ::AddSettingTextBox(
            Row.Container,
            L"Plan9SharePathLabel",
            L"Plan9SharePathHelp",
            winrt::to_hstring(Configuration.Path),
            false);
        winrt::Windows::UI::Xaml::Controls::Button Browse;
        Browse.Content(winrt::box_value(::GetSettingsString(L"Plan9BrowseFolder")));
        winrt::AutomationProperties::SetAutomationId(Browse, L"Plan9Browse");
        Browse.Click([Weak = this->get_weak(), Target = winrt::make_weak(Row.Path)](
            winrt::IInspectable const&, winrt::RoutedEventArgs const&)
        {
            auto Instance = Weak.get();
            auto Path = Target.get();
            if (!Instance || !Path) return;
            try
            {
                winrt::com_ptr<IFileOpenDialog> Dialog;
                winrt::check_hresult(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(Dialog.put())));
                DWORD Options = 0;
                winrt::check_hresult(Dialog->GetOptions(&Options));
                winrt::check_hresult(Dialog->SetOptions(Options | FOS_PICKFOLDERS |
                    FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT));
                winrt::check_hresult(Dialog->SetTitle(::GetSettingsString(L"Plan9BrowseFolder").c_str()));
                auto Result = Dialog->Show(Instance->m_WindowHandle);
                if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
                winrt::check_hresult(Result);
                winrt::com_ptr<IShellItem> Item;
                winrt::check_hresult(Dialog->GetResult(Item.put()));
                LPWSTR Selected = nullptr;
                winrt::check_hresult(Item->GetDisplayName(SIGDN_FILESYSPATH, &Selected));
                auto Free = Mile::ScopeExitTaskHandler([&]() { ::CoTaskMemFree(Selected); });
                Path.Text(winrt::hstring(Selected));
            }
            catch (...) { ::ShowSettingsError(Instance->m_WindowHandle); }
        });
        Row.Container.Children().Append(Browse);
        Row.Name = ::AddSettingTextBox(
            Row.Container,
            L"Plan9ShareNameLabel",
            L"Plan9ShareNameHelp",
            winrt::to_hstring(Configuration.Name),
            false);
        Row.AutoMount = ::AddSettingCheckBox(Row.Container, L"Plan9AutoMountLabel",
            L"Plan9AutoMountHelp", Configuration.AutoMount, false);
        Row.LinuxMetadata = ::AddSettingCheckBox(Row.Container, L"Plan9LinuxMetadataLabel",
            L"Plan9LinuxMetadataHelp", Configuration.LinuxMetadata, false);
        Row.CaseSensitive = ::AddSettingCheckBox(Row.Container, L"Plan9CaseSensitiveLabel",
            L"Plan9CaseSensitiveHelp", Configuration.CaseSensitive, false);
        Row.UseShareRootIdentity = ::AddSettingCheckBox(Row.Container, L"Plan9UseShareRootIdentityLabel",
            L"Plan9UseShareRootIdentityHelp", Configuration.UseShareRootIdentity, false);
        Row.AllowOptions = ::AddSettingCheckBox(Row.Container, L"Plan9AllowOptionsLabel",
            L"Plan9AllowOptionsHelp", Configuration.AllowOptions, false);
        Row.AllowSubPaths = ::AddSettingCheckBox(Row.Container, L"Plan9AllowSubPathsLabel",
            L"Plan9AllowSubPathsHelp", Configuration.AllowSubPaths, false);
        Row.UnlimitedConnections = ::AddSettingCheckBox(Row.Container, L"Plan9UnlimitedConnectionsLabel",
            L"Plan9UnlimitedConnectionsHelp", Configuration.UnlimitedConnections, false);
        Row.AllowedFiles = ::AddSettingTextBox(Row.Container, L"Plan9AllowedFilesLabel",
            L"Plan9AllowedFilesHelp", ::MakeSettingLines(Configuration.AllowedFiles), false);
        Row.AllowedFiles.AcceptsReturn(true);
        Row.AllowedFiles.MinHeight(64);

        winrt::Windows::UI::Xaml::Controls::Button RemoveButton;
        winrt::AutomationProperties::SetAutomationId(RemoveButton, L"Plan9Remove");
        RemoveButton.Content(winrt::box_value(
            ::GetSettingsString(L"Remove")));
        RemoveButton.Click([
            Weak = this->get_weak(),
            WeakContainer = winrt::make_weak(Row.Container)](
                winrt::IInspectable const& Sender,
                winrt::RoutedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Sender);
            UNREFERENCED_PARAMETER(Arguments);

            auto Instance = Weak.get();
            if (!Instance)
            {
                return;
            }
            try
            {
                auto Container = WeakContainer.get();
                if (!Container)
                {
                    return;
                }
                std::uint32_t Index = 0;
                if (!Instance->Plan9SharePanel().Children().IndexOf(
                    Container, Index))
                {
                    return;
                }
                Instance->Plan9SharePanel().Children().RemoveAt(Index);
                auto& Settings = Instance->m_Plan9ShareSettings;
                Settings.erase(Settings.begin() + Index);
            }
            catch (...)
            {
                ::ShowSettingsError(Instance->m_WindowHandle);
            }
        });
        Row.Container.Children().Append(RemoveButton);
        this->m_Plan9ShareSettings.push_back(Row);
        try
        {
            this->Plan9SharePanel().Children().Append(Row.Container);
        }
        catch (...)
        {
            this->m_Plan9ShareSettings.pop_back();
            throw;
        }
    }

    void VirtualMachineSettingsPage::AddPlan9ShareButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            NanaBox::Plan9ShareConfiguration Configuration;
            Configuration.Port = 50001;
            this->AddPlan9Share(Configuration);
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }

    void VirtualMachineSettingsPage::AddVirtualSmbShare(
        NanaBox::VirtualSmbShareConfiguration const& Configuration)
    {
        VirtualSmbShareSettings Row;
        Row.Container = winrt::StackPanel();
        Row.Container.Spacing(4);
        Row.Container.Margin(winrt::ThicknessHelper::FromLengths(
            8, 0, 0, 8));
        Row.ReadOnly = ::AddSettingCheckBox(
            Row.Container,
            L"VirtualSmbShareReadOnlyLabel",
            L"VirtualSmbShareReadOnlyHelp",
            Configuration.ReadOnly,
            false);
        Row.Path = ::AddSettingTextBox(
            Row.Container,
            L"VirtualSmbSharePathLabel",
            L"VirtualSmbSharePathHelp",
            winrt::to_hstring(Configuration.Path),
            false);
        Row.Name = ::AddSettingTextBox(
            Row.Container,
            L"VirtualSmbShareNameLabel",
            L"VirtualSmbShareNameHelp",
            winrt::to_hstring(Configuration.Name),
            false);

        winrt::Windows::UI::Xaml::Controls::Button RemoveButton;
        winrt::AutomationProperties::SetAutomationId(RemoveButton, L"SmbRemove");
        RemoveButton.Content(winrt::box_value(
            ::GetSettingsString(L"Remove")));
        RemoveButton.Click([
            Weak = this->get_weak(),
            WeakContainer = winrt::make_weak(Row.Container)](
                winrt::IInspectable const& Sender,
                winrt::RoutedEventArgs const& Arguments)
        {
            UNREFERENCED_PARAMETER(Sender);
            UNREFERENCED_PARAMETER(Arguments);

            auto Instance = Weak.get();
            if (!Instance)
            {
                return;
            }
            try
            {
                auto Container = WeakContainer.get();
                if (!Container)
                {
                    return;
                }
                std::uint32_t Index = 0;
                if (!Instance->VirtualSmbSharePanel().Children().IndexOf(
                    Container, Index))
                {
                    return;
                }
                Instance->VirtualSmbSharePanel().Children().RemoveAt(Index);
                auto& Settings = Instance->m_VirtualSmbShareSettings;
                Settings.erase(Settings.begin() + Index);
            }
            catch (...)
            {
                ::ShowSettingsError(Instance->m_WindowHandle);
            }
        });
        Row.Container.Children().Append(RemoveButton);
        this->m_VirtualSmbShareSettings.push_back(Row);
        try
        {
            this->VirtualSmbSharePanel().Children().Append(Row.Container);
        }
        catch (...)
        {
            this->m_VirtualSmbShareSettings.pop_back();
            throw;
        }
    }

    void VirtualMachineSettingsPage::AddVirtualSmbShareButtonClick(
        winrt::IInspectable const& Sender,
        winrt::RoutedEventArgs const& Arguments)
    {
        UNREFERENCED_PARAMETER(Sender);
        UNREFERENCED_PARAMETER(Arguments);

        try
        {
            NanaBox::VirtualSmbShareConfiguration Configuration;
            this->AddVirtualSmbShare(Configuration);
        }
        catch (...)
        {
            ::ShowSettingsError(this->m_WindowHandle);
        }
    }
}
