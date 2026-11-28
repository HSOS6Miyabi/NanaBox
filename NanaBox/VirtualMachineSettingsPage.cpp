#include "pch.h"
#include "VirtualMachineSettingsPage.h"
#if __has_include("VirtualMachineSettingsPage.g.cpp")
#include "VirtualMachineSettingsPage.g.cpp"
#endif

#include "Utils.h"
#include "ConfigurationManager.h"
#include "NanaBoxResources.h"
#include "MainWindow.h"
#include "SettingsText.h"
#include "WorkspaceHistory.h"
#include <shellapi.h>

#include <algorithm>
#include <charconv>
#include <cmath>
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
        wchar_t const* Fallback = L"Settings";
        bool Chinese = PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
        for (auto const& Entry : ::NanaBox::SettingsText::Strings)
        {
            if (Entry.Key == Name)
            {
                Fallback = Chinese ? Entry.Chinese : Entry.English;
                break;
            }
        }
        return Mile::WinRT::GetLocalizedString(winrt::hstring(ResourcePath), Fallback);
    }

    using VmmsFieldMap = std::map<std::wstring_view, std::vector<LPCWSTR>>;

    VmmsFieldMap const& VmmsActionFields()
    {
        static const VmmsFieldMap Fields = {
            {L"VmmsCreate", {L"VmmsNameTextBox", L"VmmsGenerationComboBox", L"VmmsMemoryRangePanel", L"VmmsPathTextBox"}},
            {L"VmmsRename", {L"VmmsNameTextBox"}},
            {L"VmmsCheckpointCreate", {L"VmmsNameTextBox"}},
            {L"VmmsCheckpointRename", {L"VmmsNameTextBox", L"VmmsCheckpointsComboBox"}},
            {L"VmmsSetMemory", {L"VmmsMemoryRangePanel"}},
            {L"VmmsDynamicMemory", {L"VmmsMemoryRangePanel", L"SettingsLabel206", L"VmmsMinimumMemoryRangePanel", L"VmmsMaximumMemoryRangePanel", L"VmmsMemoryBufferRangePanel", L"VmmsMemoryPriorityRangePanel"}},
            {L"VmmsStaticMemory", {L"VmmsMemoryRangePanel"}},
            {L"VmmsSetProcessor", {L"VmmsProcessorRangePanel"}},
            {L"VmmsCheckpointRestore", {L"VmmsCheckpointsComboBox"}},
            {L"VmmsCheckpointRemove", {L"VmmsCheckpointsComboBox"}},
            {L"VmmsCheckpointExport", {L"VmmsCheckpointsComboBox", L"VmmsPathTextBox"}},
            {L"VmmsImportRegister", {L"VmmsPathTextBox"}},
            {L"VmmsImportCopy", {L"VmmsPathTextBox", L"VmmsDestinationTextBox"}},
            {L"VmmsExport", {L"VmmsPathTextBox"}},
            {L"VmmsMoveStorage", {L"VmmsPathTextBox"}},
            {L"VmmsMigrate", {L"SettingsLabel207", L"SettingsLabel208", L"VmmsRemoteHostTextBox", L"VmmsRemotePathTextBox"}},
            {L"VmmsReplicationEnable", {L"SettingsLabel207", L"SettingsLabel208", L"VmmsRemoteHostTextBox", L"VmmsReplicaPortTextBox", L"VmmsReplicaFrequencyComboBox", L"VmmsReplicaHistoryRangePanel", L"VmmsReplicaCertificateTextBox", L"VmmsReplicaAuthComboBox", L"VmmsReplicaCompressionCheckBox"}},
            {L"VmmsCpuPolicy", {L"VmmsCpuReserveRangePanel", L"VmmsCpuMaximumRangePanel", L"VmmsCpuWeightRangePanel", L"VmmsCpuNestedCheckBox"}},
            {L"VmmsGpuAdd", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsGpuSourceComboBox", L"SettingsLabel212", L"VmmsGpuVRAMGrid", L"VmmsGpuEncodeGrid", L"VmmsGpuDecodeGrid", L"VmmsGpuComputeGrid"}},
            {L"VmmsGpuRemove", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox"}},
            {L"VmmsGpuQuotas", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"SettingsLabel212", L"VmmsGpuVRAMGrid", L"VmmsGpuEncodeGrid", L"VmmsGpuDecodeGrid", L"VmmsGpuComputeGrid"}},
            {L"VmmsDiskAttach", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicePathTextBox", L"VmmsControllerTypeComboBox", L"VmmsControllerNumberRangePanel", L"VmmsControllerLocationRangePanel"}},
            {L"VmmsDiskDetach", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox"}},
            {L"VmmsDiskQoS", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsDiskMinIopsTextBox", L"VmmsDiskMaxIopsTextBox"}},
            {L"VmmsDvdAdd", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicePathTextBox", L"VmmsControllerTypeComboBox", L"VmmsControllerNumberRangePanel", L"VmmsControllerLocationRangePanel"}},
            {L"VmmsDvdMount", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsDevicePathTextBox"}},
            {L"VmmsDvdRemove", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox"}},
            {L"VmmsNicAdd", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDeviceNameTextBox"}},
            {L"VmmsNicRemove", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox"}},
            {L"VmmsNicConnect", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsDeviceSwitchComboBox"}},
            {L"VmmsNicDisconnect", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox"}},
            {L"VmmsNicMac", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsNicMacTextBox"}},
            {L"VmmsNicVlan", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsNicVlanRangePanel"}},
            {L"VmmsNicGuard", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsNicDhcpGuardCheckBox", L"VmmsNicRouterGuardCheckBox", L"VmmsNicSpoofCheckBox"}},
            {L"VmmsIntegration", {L"SettingsLabel210", L"SettingsLabel211", L"VmmsDevicesComboBox", L"VmmsIntegrationEnabledCheckBox"}},
            {L"VmmsSecureBoot", {L"VmmsSecureBootCheckBox", L"VmmsSecureBootTemplateComboBox"}},
            {L"VmmsAutomaticActions", {L"VmmsAutomaticStartComboBox", L"VmmsAutomaticStopComboBox", L"VmmsAutomaticDelayTextBox"}},
            {L"VmmsCheckpointPolicy", {L"VmmsCheckpointTypeComboBox", L"VmmsAutomaticCheckpointsCheckBox"}},
        };
        return Fields;
    }

    template<typename PageType>
    void UpdateVmmsActionFields(PageType* Root, winrt::ComboBox const& Picker)
    {
        auto Item = Picker.SelectedItem().try_as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>();
        if (!Item) return;
        auto Action = winrt::unbox_value<winrt::hstring>(Item.Tag());
        auto const& Fields = ::VmmsActionFields();
        auto Current = Fields.find(std::wstring_view(Action.c_str()));
        for (auto const& Entry : Fields)
            for (auto Name : Entry.second)
                if (auto Control = Root->FindName(Name).template try_as<winrt::FrameworkElement>())
                    Control.Visibility(Current != Fields.end() &&
                        std::any_of(Current->second.begin(), Current->second.end(),
                            [Name](LPCWSTR Candidate) { return std::wstring_view(Candidate) == Name; })
                        ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
    }

    template<typename PageType>
    std::wstring DescribeVmmsParameters(PageType* Root, winrt::hstring const& Action)
    {
        auto const& Fields = ::VmmsActionFields();
        auto Current = Fields.find(std::wstring_view(Action.c_str()));
        if (Current == Fields.end()) return {};
        std::wstring Result;
        using namespace winrt::Windows::UI::Xaml::Controls;
        auto Add = [&](winrt::hstring const& Label, winrt::hstring const& Value)
        {
            if (!Label.empty() && !Value.empty())
                Result += L"\n" + std::wstring(Label.c_str()) + L": " + Value.c_str();
        };
        std::function<void(winrt::FrameworkElement const&)> Describe;
        Describe = [&](winrt::FrameworkElement const& Control)
        {
            if (auto Input = Control.try_as<TextBox>())
            {
                auto Label = winrt::unbox_value_or<winrt::hstring>(Input.Header(), winrt::hstring());
                if (Label.empty()) Label = winrt::AutomationProperties::GetName(Input);
                Add(Label, Input.Text());
            }
            else if (auto Choice = Control.try_as<ComboBox>())
            {
                if (auto Selected = Choice.SelectedItem().try_as<ComboBoxItem>())
                    Add(winrt::unbox_value_or<winrt::hstring>(Choice.Header(), winrt::hstring()),
                        winrt::unbox_value_or<winrt::hstring>(Selected.Content(), winrt::hstring()));
            }
            else if (auto Check = Control.try_as<CheckBox>())
                Add(winrt::unbox_value_or<winrt::hstring>(Check.Content(), winrt::hstring()),
                    ::GetSettingsString(Check.IsChecked().Value() ? L"ResultYes" : L"ResultNo"));
            else if (auto Group = Control.try_as<Panel>())
                for (auto const& Child : Group.Children())
                    if (auto Field = Child.try_as<winrt::FrameworkElement>()) Describe(Field);
        };
        for (auto Name : Current->second)
            if (auto Control = Root->FindName(Name).template try_as<winrt::FrameworkElement>())
                Describe(Control);
        return Result;
    }

    void ShowSettingsError(
        _In_ HWND WindowHandle)
    {
        ::ShowErrorMessageDialog(WindowHandle, Mile::WinRT::ToHResultError());
    }



    void BindRange(winrt::Windows::UI::Xaml::Controls::Slider const& Slider,
        winrt::TextBox const& Input, winrt::hstring const& Label,
        bool GrowMaximum, double AbsoluteMaximum, int DecimalPlaces)
    {
        winrt::AutomationProperties::SetName(Slider, Label);
        winrt::AutomationProperties::SetName(Input, Label);
        auto Updating = std::make_shared<bool>(false);
        auto SyncSlider = [Weak = winrt::make_weak(Slider), Updating, GrowMaximum, AbsoluteMaximum](
            winrt::IInspectable const& Sender, winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs const&)
        {
            if (*Updating) return;
            auto Text = winrt::to_string(Sender.as<winrt::TextBox>().Text());
            double Value = 0;
            auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
            if (Text.empty() || Parsed.ec != std::errc() || Parsed.ptr != Text.data() + Text.size() || !std::isfinite(Value)) return;
            if (auto Control = Weak.get())
            {
                if (Value < Control.Minimum() || Value > AbsoluteMaximum) return;
                *Updating = true;
                if (GrowMaximum && Value > Control.Maximum()) Control.Maximum(Value);
                if (Value <= Control.Maximum()) Control.Value(Value);
                *Updating = false;
            }
        };
        auto SyncInput = [Weak = winrt::make_weak(Input), Updating, DecimalPlaces](
            winrt::IInspectable const&, winrt::Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& Args)
        {
            if (*Updating) return;
            if (auto Control = Weak.get())
            {
                char Buffer[64];
                auto Converted = std::to_chars(Buffer, Buffer + sizeof(Buffer), Args.NewValue(), std::chars_format::fixed, DecimalPlaces);
                if (Converted.ec != std::errc()) return;
                std::string Value(Buffer, Converted.ptr);
                if (DecimalPlaces > 0)
                {
                    while (!Value.empty() && Value.back() == '0') Value.pop_back();
                    if (!Value.empty() && Value.back() == '.') Value.pop_back();
                }
                *Updating = true;
                Control.Text(winrt::to_hstring(Value));
                *Updating = false;
            }
        };
        Input.TextChanged(SyncSlider);
        Slider.ValueChanged(SyncInput);
        SyncSlider(Input, nullptr);
    }

    double RecommendedProcessorMaximum()
    {
        auto Count = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return (std::max)(4.0, static_cast<double>(Count));
    }

    double RecommendedMemoryMaximum()
    {
        MEMORYSTATUSEX Status{};
        Status.dwLength = sizeof(Status);
        if (!::GlobalMemoryStatusEx(&Status)) return 16384;
        return (std::max)(4096.0, static_cast<double>(Status.ullTotalPhys / (1024 * 1024)));
    }

    void BindVisibility(winrt::CheckBox const& Toggle, winrt::FrameworkElement const& Element)
    {
        auto Update = [Weak = winrt::make_weak(Element)](
            winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
        {
            if (auto Target = Weak.get())
                Target.Visibility(Sender.as<winrt::CheckBox>().IsChecked().Value()
                    ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
        };
        Toggle.Checked(Update);
        Toggle.Unchecked(Update);
        Update(Toggle, nullptr);
    }

    winrt::hstring DescribeSettingsResult(winrt::hstring const& Raw)
    {
        if (Raw.empty()) return ::GetSettingsString(L"ResultWaiting");
        using Json = nlohmann::json;
        auto Data = Json::parse(winrt::to_string(Raw), nullptr, false);
        if (Data.is_array()) return ::GetSettingsString(L"ResultInfo");
        if (Data.is_discarded() || !Data.is_object())
        {
            auto Text = winrt::to_string(Raw);
            auto Position = Text.find('{');
            if (Position != std::string::npos && Json::parse(Text.substr(Position), nullptr, false).is_structured())
                return winrt::to_hstring(Text.substr(0, Position));
            return Raw;
        }
        std::wstring Summary;
        auto Add = [&](LPCWSTR Label, std::wstring const& Value)
        {
            if (Value.empty()) return;
            if (!Summary.empty()) Summary += L"\n";
            Summary += ::GetSettingsString(Label).c_str();
            Summary += L": ";
            Summary += Value;
        };
        auto Operation = Data.value("Operation", std::string());
        if (Data.contains("Success") && Data["Success"].is_boolean())
        {
            LPCWSTR Key = Data["Success"].get<bool>() ? L"ResultSuccess" : L"ResultFailure";
            if (Data["Success"].get<bool>())
            {
                if (Operation == "Refresh") Key = L"ResultRefresh";
                else if (Operation == "Create" && Data.contains("Count")) Key = L"ResultCreateNetwork";
                else if (Operation == "Delete" && Data.contains("Count")) Key = L"ResultDeleteNetwork";
            }
            Summary = ::GetSettingsString(Key).c_str();
        }
        else Summary = ::GetSettingsString(L"ResultInfo").c_str();

        auto Fields = [&](Json const& Object)
        {
            if (!Object.is_object()) return;
            auto Field = [&](char const* Name, LPCWSTR Label)
            {
                auto It = Object.find(Name);
                if (It == Object.end()) return;
                if (It->is_string())
                {
                    auto Text = It->get<std::string>();
                    auto Position = Text.find('{');
                    auto Parsed = Json::parse(Text, nullptr, false);
                    if (Parsed.is_structured()) Add(Label, ::GetSettingsString(L"PropertyStructuredValue").c_str());
                    else if (Position != std::string::npos && Json::parse(Text.substr(Position), nullptr, false).is_structured())
                        Add(Label, winrt::to_hstring(Text.substr(0, Position)).c_str());
                    else Add(Label, winrt::to_hstring(Text).c_str());
                }
                else if (It->is_boolean()) Add(Label, ::GetSettingsString(It->get<bool>() ? L"ResultYes" : L"ResultNo").c_str());
                else if (It->is_number()) Add(Label, winrt::to_hstring(It->dump()).c_str());
            };
            Field("Name", L"ResultName");
            Field("Type", L"ResultType");
            Field("State", L"ResultState");
            Field("Count", L"ResultCount");
            Field("HostName", L"ResultHost");
            Field("Alive", L"ResultAlive");
            Field("Path", L"ResultPath");
            Field("IpAddress", L"ResultAddress");
            Field("ApiAvailable", L"ResultApi");
            Field("WslDependencyAvailable", L"ResultWsl");
            Field("HcnReachable", L"ResultHcn");
            Field("Error", L"ResultError");
            if (!Object.contains("Count"))
                for (auto const& Key : {"Items", "Rules", "Shares", "Networks"})
                    if (Object.contains(Key) && Object[Key].is_array())
                        Add(L"ResultCount", std::to_wstring(Object[Key].size()));
            for (auto const& Entry : {std::pair{"VirtualSize", L"ResultSize"}, std::pair{"PhysicalSize", L"ResultPhysicalSize"}})
                if (Object.contains(Entry.first) && Object[Entry.first].is_number_unsigned())
                    Add(Entry.second, std::to_wstring(Object[Entry.first].get<std::uint64_t>() / (1024 * 1024)));
        };
        Fields(Data);
        if (Data.contains("Data")) Fields(Data["Data"]);
        if (Data.contains("Properties")) Fields(Data["Properties"]);
        return winrt::hstring(Summary);
    }


    winrt::hstring PropertyCaption(std::string const& Key)
    {
        static const std::map<std::string, LPCWSTR> Labels = {
            {"Id", L"PropertyId"}, {"Name", L"ResultName"}, {"Type", L"ResultType"},
            {"Flags", L"PropertyFlags"}, {"SchemaVersion", L"PropertySchemaVersion"},
            {"Major", L"PropertyMajor"}, {"Minor", L"PropertyMinor"},
            {"Subnets", L"PropertySubnets"}, {"Routes", L"PropertyRoutes"},
            {"NextHop", L"PropertyGateway"}, {"GatewayAddress", L"PropertyGateway"},
            {"Policies", L"PropertyPolicies"}, {"IpAddressPrefix", L"PropertyAddressPrefix"},
            {"DestinationPrefix", L"PropertyAddressPrefix"}, {"Protocol", L"PropertyProtocol"},
            {"PortMappings", L"PropertyPortMappings"}, {"ExternalPort", L"PropertyExternalPort"},
            {"InternalPort", L"PropertyInternalPort"}, {"HostComputeEndpoints", L"PropertyEndpoints"},
            {"UseUtc", L"PropertyUseUtc"}, {"Chipset", L"PropertyChipset"},
            {"PropertyTypes", L"PropertyPropertyTypes"}, {"RequestType", L"PropertyRequestType"},
            {"ResourceType", L"PropertyResourceType"}, {"Settings", L"PropertySettings"},
            {"Properties", L"PropertyProperties"}, {"Items", L"PropertyItems"},
            {"Operation", L"PropertyOperation"}, {"Success", L"PropertySuccess"},
            {"Data", L"PropertyData"}, {"Details", L"PropertyDetails"}, {"VM", L"PropertyVm"},
            {"State", L"ResultState"}, {"Error", L"ResultError"}, {"Count", L"ResultCount"},
            {"MemoryMiB", L"PropertyMemoryMiB"}, {"ProcessorCount", L"PropertyProcessorCount"},
            {"Devices", L"PropertyDevices"}, {"Checkpoints", L"PropertyCheckpoints"},
            {"FrontendVIPs", L"PropertyFrontendAddresses"}, {"Path", L"ResultPath"}
        };
        if (auto Found = Labels.find(Key); Found != Labels.end()) return ::GetSettingsString(Found->second);
        std::string Label;
        for (std::size_t Index = 0; Index < Key.size(); ++Index)
        {
            char Ch = Key[Index];
            if (Ch == '_') { Label += ' '; continue; }
            if (Index && Ch >= 'A' && Ch <= 'Z' &&
                ((Key[Index-1] >= 'a' && Key[Index-1] <= 'z') ||
                 (Index+1 < Key.size() && Key[Index+1] >= 'a' && Key[Index+1] <= 'z')))
                Label += ' ';
            Label += Ch;
        }
        return winrt::to_hstring(Label);
    }

    nlohmann::json ExpandStructuredValue(nlohmann::json const& Value)
    {
        if (Value.is_string())
        {
            auto Text = Value.get<std::string>();
            auto Parsed = nlohmann::json::parse(Text, nullptr, false);
            if (Parsed.is_structured()) return Parsed;
            auto Position = Text.find('{');
            if (Position != std::string::npos)
            {
                Parsed = nlohmann::json::parse(Text.substr(Position), nullptr, false);
                if (Parsed.is_structured())
                    return nlohmann::json{{"Error", Text.substr(0, Position)}, {"Details", std::move(Parsed)}};
            }
        }
        return Value;
    }

    winrt::hstring PropertyValue(nlohmann::json const& Value)
    {
        if (Value.is_boolean()) return ::GetSettingsString(Value.get<bool>() ? L"PropertyOn" : L"PropertyOff");
        if (Value.is_null()) return ::GetSettingsString(L"PropertyEmpty");
        if (Value.is_structured()) return ::GetSettingsString(L"PropertyStructuredValue");
        if (Value.is_string()) return winrt::to_hstring(Value.get<std::string>());
        return winrt::to_hstring(Value.dump());
    }

    using ResultNode = winrt::Windows::UI::Xaml::Controls::TreeViewNode;
    ResultNode BuildResultNode(winrt::hstring const& Name, nlohmann::json const& Input,
        unsigned Depth, std::size_t& Remaining)
    {
        ResultNode Node;
        auto Value = ::ExpandStructuredValue(Input);
        std::wstring Caption(Name.c_str());
        if (Remaining) --Remaining;
        if (!Value.is_structured())
            Caption += L": " + std::wstring(::PropertyValue(Value).c_str());
        else if (Value.empty())
            Caption += L": " + std::wstring(::GetSettingsString(L"PropertyEmptyGroup").c_str());
        Node.Content(winrt::box_value(winrt::hstring(Caption)));
        if (Value.is_structured() && !Value.empty())
        {
            if (Depth >= 24 || !Remaining)
            {
                ResultNode More;
                More.Content(winrt::box_value(::GetSettingsString(L"PropertyMore")));
                Node.Children().Append(More);
                return Node;
            }
            std::size_t Index = 0;
            for (auto It = Value.begin(); It != Value.end(); ++It)
            {
                if (!Remaining)
                {
                    ResultNode More;
                    More.Content(winrt::box_value(::GetSettingsString(L"PropertyMore")));
                    Node.Children().Append(More);
                    break;
                }
                auto Label = Value.is_object() ? ::PropertyCaption(It.key()) :
                    winrt::hstring(std::wstring(::GetSettingsString(L"PropertyItem").c_str()) + L" " + std::to_wstring(++Index));
                Node.Children().Append(::BuildResultNode(Label, It.value(), Depth + 1, Remaining));
            }
            Node.IsExpanded(Depth == 0);
        }
        return Node;
    }

    void FillResultTree(winrt::Windows::UI::Xaml::Controls::TreeView const& Tree,
        winrt::hstring const& Raw)
    {
        Tree.RootNodes().Clear();
        auto Value = ::ExpandStructuredValue(nlohmann::json(winrt::to_string(Raw)));
        if (!Value.is_structured()) return;
        std::size_t Remaining = 8192;
        if (Value.is_object())
            for (auto It = Value.begin(); It != Value.end() && Remaining; ++It)
                Tree.RootNodes().Append(::BuildResultNode(::PropertyCaption(It.key()), It.value(), 0, Remaining));
        else
            Tree.RootNodes().Append(::BuildResultNode(::GetSettingsString(L"PropertyItems"), Value, 0, Remaining));
    }

    std::wstring CopyResultTree(winrt::Windows::UI::Xaml::Controls::TreeView const& Tree)
    {
        std::wstring Text;
        std::function<void(ResultNode const&, unsigned)> Append = [&](ResultNode const& Node, unsigned Depth)
        {
            Text.append(Depth * 2, L' ');
            Text += winrt::unbox_value<winrt::hstring>(Node.Content()).c_str();
            Text += L"\r\n";
            for (auto const& Child : Node.Children()) Append(Child, Depth + 1);
        };
        for (auto const& Root : Tree.RootNodes()) Append(Root, 0);
        return Text;
    }

    bool ParseIpv4(std::string_view Text, std::uint32_t& Value)
    {
        Value = 0;
        for (int Part = 0; Part < 4; ++Part)
        {
            auto Dot = Text.find('.');
            if ((Part == 3) != (Dot == std::string_view::npos)) return false;
            auto Octet = Text.substr(0, Dot);
            unsigned int Number = 0;
            auto Parsed = std::from_chars(Octet.data(), Octet.data() + Octet.size(), Number);
            if (Octet.empty() || Parsed.ec != std::errc() || Parsed.ptr != Octet.data() + Octet.size() || Number > 255)
                return false;
            Value = (Value << 8) | Number;
            if (Part != 3) Text.remove_prefix(Dot + 1);
        }
        return true;
    }

    winrt::hstring BuildNatNetworkSettings(winrt::hstring const& Subnet, winrt::hstring const& Gateway)
    {
        using Json = nlohmann::json;
        auto PrefixText = winrt::to_string(Subnet);
        auto Slash = PrefixText.find('/');
        std::uint32_t Address = 0;
        unsigned int Prefix = 0;
        if (Slash == std::string::npos || !::ParseIpv4(std::string_view(PrefixText).substr(0, Slash), Address))
            throw winrt::hresult_invalid_argument(::GetSettingsString(L"InvalidIpv4Subnet"));
        auto Begin = PrefixText.data() + Slash + 1;
        auto End = PrefixText.data() + PrefixText.size();
        auto Parsed = std::from_chars(Begin, End, Prefix);
        if (Parsed.ec != std::errc() || Parsed.ptr != End || Prefix < 1 || Prefix > 30)
            throw winrt::hresult_invalid_argument(::GetSettingsString(L"InvalidIpv4Subnet"));
        std::uint32_t Mask = 0xffffffffu << (32 - Prefix);
        if (Address & ~Mask) throw winrt::hresult_invalid_argument(::GetSettingsString(L"InvalidIpv4Subnet"));
        Json SubnetSettings = {{"IpAddressPrefix", PrefixText}};
        auto GatewayText = winrt::to_string(Gateway);
        if (!GatewayText.empty())
        {
            std::uint32_t NextHop = 0;
            if (!::ParseIpv4(GatewayText, NextHop) || (NextHop & Mask) != Address ||
                NextHop == Address || (NextHop & ~Mask) == ~Mask)
                throw winrt::hresult_invalid_argument(::GetSettingsString(L"InvalidIpv4Gateway"));
            SubnetSettings["Routes"] = Json::array({{{"NextHop", GatewayText}, {"DestinationPrefix", "0.0.0.0/0"}}});
        }
        Json Settings = {{"Type", "NAT"}, {"SchemaVersion", {{"Major", 2}, {"Minor", 0}}},
            {"Ipams", Json::array({{{"Type", "Static"}, {"Subnets", Json::array({SubnetSettings})}}})}};
        return winrt::to_hstring(Settings.dump());
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


    winrt::hstring FormatCpuPercentage(std::uint64_t Value)
    {
        auto Text = std::to_wstring(Value / 1000);
        if (auto Remainder = Value % 1000)
        {
            auto Fraction = std::to_wstring(Remainder + 1000).substr(1);
            while (!Fraction.empty() && Fraction.back() == L'0') Fraction.pop_back();
            Text += L"." + Fraction;
        }
        return winrt::hstring(Text);
    }

    std::uint64_t ReadCpuPercentage(winrt::TextBox const& Input)
    {
        auto Text = winrt::to_string(Input.Text());
        auto Dot = Text.find('.');
        auto Whole = std::string_view(Text).substr(0, Dot);
        auto Fraction = Dot == std::string::npos ? std::string_view() : std::string_view(Text).substr(Dot + 1);
        unsigned int Integer = 0, Decimal = 0;
        auto Parsed = std::from_chars(Whole.data(), Whole.data() + Whole.size(), Integer);
        if (Whole.empty() || Parsed.ec != std::errc() || Parsed.ptr != Whole.data() + Whole.size() ||
            Integer > 100 || (Dot != std::string::npos && (Fraction.empty() || Fraction.size() > 3)))
            ::InvalidSetting(::GetSettingsString(L"ProcessorLimitTextBox.Header"));
        if (!Fraction.empty())
        {
            auto ParsedFraction = std::from_chars(Fraction.data(), Fraction.data() + Fraction.size(), Decimal);
            if (ParsedFraction.ec != std::errc() || ParsedFraction.ptr != Fraction.data() + Fraction.size())
                ::InvalidSetting(::GetSettingsString(L"ProcessorLimitTextBox.Header"));
            for (std::size_t Index = Fraction.size(); Index < 3; ++Index) Decimal *= 10;
        }
        auto Result = static_cast<std::uint64_t>(Integer) * 1000 + Decimal;
        if (Result > 100000) ::InvalidSetting(::GetSettingsString(L"ProcessorLimitTextBox.Header"));
        return Result;
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
        winrt::Windows::UI::Xaml::Controls::RowDefinition LabelRow;
        LabelRow.Height(winrt::GridLengthHelper::Auto());
        Row.RowDefinitions().Append(LabelRow);
        winrt::Windows::UI::Xaml::Controls::RowDefinition InputRow;
        InputRow.Height(winrt::GridLengthHelper::Auto());
        Row.RowDefinitions().Append(InputRow);
        Row.RowSpacing(4);

        winrt::TextBlock Label;
        Label.Text(::GetSettingsString(LabelResource));
        Label.TextWrapping(winrt::TextWrapping::Wrap);
        Label.VerticalAlignment(winrt::VerticalAlignment::Center);
        Row.Children().Append(Label);

        winrt::hstring Help = ::GetSettingsString(HelpResource);
        winrt::AutomationProperties::SetName(Input, Label.Text());
        winrt::AutomationProperties::SetAutomationId(Input, winrt::hstring(LabelResource));
        winrt::AutomationProperties::SetHelpText(Input, Help);
        winrt::ToolTipService::SetToolTip(Input, winrt::box_value(Help));
        winrt::Grid::SetRow(Input, 1);
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
        Input.MinWidth(0);
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
        auto Prefix = winrt::AutomationProperties::GetAutomationId(Picker);
        if (!Prefix.empty())
            winrt::AutomationProperties::SetAutomationId(Item,
                winrt::hstring(std::wstring(Prefix.c_str()) + L"_" + Value.c_str()));
        winrt::ToolTipService::SetToolTip(Item, winrt::box_value(Value));
        Picker.Items().Append(Item);
    }


    using ValueChoices = std::vector<std::pair<winrt::hstring, winrt::hstring>>;

    void BindValuePicker(winrt::ComboBox const& Picker, winrt::TextBox const& Input)
    {
        winrt::AutomationProperties::SetName(Picker, winrt::AutomationProperties::GetName(Input));
        winrt::AutomationProperties::SetHelpText(Picker, winrt::AutomationProperties::GetHelpText(Input));
        Picker.SelectionChanged([Weak = winrt::make_weak(Input)](
            winrt::IInspectable const& Sender, winrt::SelectionChangedEventArgs const&)
        {
            if (auto Target = Weak.get())
            {
                auto Choice = Sender.as<winrt::ComboBox>().SelectedItem().try_as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>();
                if (!Choice) return;
                auto Value = winrt::unbox_value<winrt::hstring>(Choice.Tag());
                bool Custom = Value == L"@custom";
                Target.Visibility(Custom ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
                if (!Custom && Target.Text() != Value) Target.Text(Value);
            }
        });
    }

    void PopulateValuePicker(winrt::ComboBox const& Picker, winrt::TextBox const& Input,
        ValueChoices const& Choices, LPCWSTR CustomResource)
    {
        auto Current = Input.Text();
        Picker.Items().Clear();
        int Selection = static_cast<int>(Choices.size());
        auto Prefix = winrt::AutomationProperties::GetAutomationId(Picker);
        for (auto const& Choice : Choices)
        {
            if (Choice.second == Current) Selection = static_cast<int>(Picker.Items().Size());
            ::AddPickerItem(Picker, Choice.first, Choice.second);
            winrt::AutomationProperties::SetAutomationId(Picker.Items().GetAt(Picker.Items().Size() - 1)
                .as<winrt::Windows::UI::Xaml::Controls::ComboBoxItem>(),
                winrt::hstring(std::wstring(Prefix.c_str()) + L"_" + Choice.second.c_str()));
        }
        ::AddPickerItem(Picker, ::GetSettingsString(CustomResource), L"@custom");
        Picker.SelectedIndex(Selection);
        Input.Visibility(Selection == static_cast<int>(Choices.size()) ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
    }

    ValueChoices KeyboardLayouts()
    {
        return {
            {::GetSettingsString(L"LayoutHost"), L"Host"},
            {::GetSettingsString(L"LayoutUs"), L"00000409"},
            {::GetSettingsString(L"LayoutUk"), L"00000809"},
            {::GetSettingsString(L"LayoutJapanese"), L"00000411"},
            {::GetSettingsString(L"LayoutGerman"), L"00000407"},
            {::GetSettingsString(L"LayoutFrench"), L"0000040C"},
            {::GetSettingsString(L"LayoutChinese"), L"00000804"},
            {::GetSettingsString(L"LayoutKorean"), L"00000412"},
            {::GetSettingsString(L"LayoutSpanish"), L"0000040A"}
        };
    }

    ValueChoices HotkeyChoices()
    {
        ValueChoices Choices = {
            {::GetSettingsString(L"KeyBreak"), winrt::to_hstring(VK_CANCEL)},
            {L"Home", winrt::to_hstring(VK_HOME)},
            {L"End", winrt::to_hstring(VK_END)},
            {L"Insert", winrt::to_hstring(VK_INSERT)},
            {L"Delete", winrt::to_hstring(VK_DELETE)},
            {::GetSettingsString(L"KeyPageUp"), winrt::to_hstring(VK_PRIOR)},
            {::GetSettingsString(L"KeyPageDown"), winrt::to_hstring(VK_NEXT)},
            {::GetSettingsString(L"KeyLeft"), winrt::to_hstring(VK_LEFT)},
            {::GetSettingsString(L"KeyRight"), winrt::to_hstring(VK_RIGHT)},
            {::GetSettingsString(L"KeyUp"), winrt::to_hstring(VK_UP)},
            {::GetSettingsString(L"KeyDown"), winrt::to_hstring(VK_DOWN)},
            {::GetSettingsString(L"KeySpace"), winrt::to_hstring(VK_SPACE)},
            {::GetSettingsString(L"KeyEnter"), winrt::to_hstring(VK_RETURN)},
            {::GetSettingsString(L"KeyEscape"), winrt::to_hstring(VK_ESCAPE)},
            {L"Tab", winrt::to_hstring(VK_TAB)}
        };
        for (int Key = 'A'; Key <= 'Z'; ++Key)
            Choices.emplace_back(winrt::hstring(std::wstring(1, static_cast<wchar_t>(Key))), winrt::to_hstring(Key));
        for (int Key = '0'; Key <= '9'; ++Key)
            Choices.emplace_back(winrt::hstring(std::wstring(1, static_cast<wchar_t>(Key))), winrt::to_hstring(Key));
        for (int Key = VK_F1; Key <= VK_F24; ++Key)
            Choices.emplace_back(winrt::hstring(L"F" + std::to_wstring(Key - VK_F1 + 1)), winrt::to_hstring(Key));
        return Choices;
    }

    void FillNetworkPicker(winrt::ComboBox const& Picker, std::vector<::NanaBox::HostNetworkInfo> const& Networks,
        bool AttachableOnly = false, winrt::hstring const& CurrentNetwork = {})
    {
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        winrt::hstring Previous = CurrentNetwork;
        if (!AttachableOnly)
            if (auto Selected = Picker.SelectedItem().try_as<Item>())
                Previous = winrt::unbox_value<winrt::hstring>(Selected.Tag());
        Picker.Items().Clear();
        if (AttachableOnly) ::AddPickerItem(Picker, ::GetSettingsString(L"DefaultNetworkChoice"), L"");
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
        if (AttachableOnly && !Previous.empty())
        {
            bool Present = false;
            for (auto const& Entry : Picker.Items())
                if (winrt::unbox_value<winrt::hstring>(Entry.as<Item>().Tag()) == Previous) Present = true;
            if (!Present) ::AddPickerItem(Picker, winrt::hstring(Mile::FormatWideString(
                ::GetSettingsString(L"Unavailable").c_str(), Previous.c_str())), Previous);
        }
        int Selection = Picker.Items().Size() ? 0 : -1;
        for (std::uint32_t Index = 0; Index < Picker.Items().Size(); ++Index)
            if (winrt::unbox_value<winrt::hstring>(Picker.Items().GetAt(Index).as<Item>().Tag()) == Previous)
            {
                Selection = static_cast<int>(Index);
                break;
            }
        Picker.SelectedIndex(Selection);
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


namespace NanaBox
{
    class StructuredSettingsEditor
    {
        using Json = nlohmann::json;
        using Panel = winrt::Windows::UI::Xaml::Controls::StackPanel;
        using Box = winrt::Windows::UI::Xaml::Controls::TextBox;
        using Choice = winrt::Windows::UI::Xaml::Controls::ComboBox;
        using Check = winrt::Windows::UI::Xaml::Controls::CheckBox;
        using Button = winrt::Windows::UI::Xaml::Controls::Button;
        using Item = winrt::Windows::UI::Xaml::Controls::ComboBoxItem;
        using Block = winrt::Windows::UI::Xaml::Controls::TextBlock;
        using Automation = winrt::Windows::UI::Xaml::Automation::AutomationProperties;
        enum class Kind { Text, Integer, Decimal, Boolean, Object, List, Empty };

        static Kind ValueKind(Json const& Value)
        {
            if (Value.is_object()) return Kind::Object;
            if (Value.is_array()) return Kind::List;
            if (Value.is_boolean()) return Kind::Boolean;
            if (Value.is_number_integer()) return Kind::Integer;
            if (Value.is_number_float()) return Kind::Decimal;
            if (Value.is_null()) return Kind::Empty;
            return Kind::Text;
        }

        static void Identify(winrt::Windows::UI::Xaml::DependencyObject const& Element,
            std::wstring const& Name)
        {
            Automation::SetAutomationId(Element, winrt::hstring(Name));
        }

        struct Node : std::enable_shared_from_this<Node>
        {
            Panel Container{nullptr}, Values{nullptr}, ChildrenPanel{nullptr};
            Box NameInput{nullptr}, Scalar{nullptr};
            Choice TypeInput{nullptr};
            Check Boolean{nullptr};
            std::vector<std::shared_ptr<Node>> Children;
            std::wstring Id;
            Kind Type = Kind::Object;
            unsigned Depth = 0;
            std::size_t Next = 0;

            static std::shared_ptr<Node> Make(Panel const& Host, std::wstring Identifier,
                unsigned Level, bool IsRoot, bool HasName, std::string const& Key, Json const& Value)
            {
                auto Result = std::make_shared<Node>();
                Result->Id = std::move(Identifier);
                Result->Depth = Level;
                Result->Container = IsRoot ? Host : Panel();
                Result->Container.Spacing(6);
                if (!IsRoot)
                {
                    Result->Container.Margin(winrt::ThicknessHelper::FromLengths(10, 4, 0, 8));
                    if (HasName)
                    {
                        Result->NameInput = Box();
                        Result->NameInput.Header(winrt::box_value(::GetSettingsString(L"PropertyName")));
                        Result->NameInput.Text(winrt::to_hstring(Key));
                        Identify(Result->NameInput, Result->Id + L"_Name");
                        Result->Container.Children().Append(Result->NameInput);
                    }
                    Result->TypeInput = Choice();
                    Result->TypeInput.Header(winrt::box_value(::GetSettingsString(L"PropertyType")));
                    Result->TypeInput.HorizontalAlignment(winrt::HorizontalAlignment::Stretch);
                    Identify(Result->TypeInput, Result->Id + L"_Type");
                    constexpr std::pair<LPCWSTR, LPCWSTR> Types[] = {
                        {L"Text", L"PropertyText"}, {L"Integer", L"PropertyInteger"},
                        {L"Decimal", L"PropertyDecimal"}, {L"Boolean", L"PropertyBoolean"},
                        {L"Object", L"PropertyObject"}, {L"List", L"PropertyList"},
                        {L"Empty", L"PropertyEmpty"}
                    };
                    for (auto const& Entry : Types)
                    {
                        Item Option;
                        Option.Content(winrt::box_value(::GetSettingsString(Entry.second)));
                        Identify(Option, Result->Id + L"_Type_" + Entry.first);
                        Result->TypeInput.Items().Append(Option);
                    }
                    Result->TypeInput.SelectedIndex(static_cast<int>(ValueKind(Value)));
                    Result->Container.Children().Append(Result->TypeInput);
                }
                Result->Values = Panel();
                Result->Values.Spacing(6);
                Result->Container.Children().Append(Result->Values);
                Result->LoadValue(Value);
                if (!IsRoot)
                {
                    Result->TypeInput.SelectionChanged([Weak = std::weak_ptr<Node>(Result)](
                        winrt::IInspectable const& Sender, winrt::SelectionChangedEventArgs const&)
                    {
                        if (auto Self = Weak.lock())
                        {
                            auto Index = Sender.as<Choice>().SelectedIndex();
                            if (Index < 0 || static_cast<Kind>(Index) == Self->Type) return;
                            switch (static_cast<Kind>(Index))
                            {
                            case Kind::Text: Self->LoadValue(""); break;
                            case Kind::Integer: Self->LoadValue(0); break;
                            case Kind::Decimal: Self->LoadValue(0.0); break;
                            case Kind::Boolean: Self->LoadValue(false); break;
                            case Kind::Object: Self->LoadValue(Json::object()); break;
                            case Kind::List: Self->LoadValue(Json::array()); break;
                            case Kind::Empty: Self->LoadValue(nullptr); break;
                            }
                        }
                    });
                }
                return Result;
            }

            void Append(std::string const& Key, Json const& Value)
            {
                if (Depth >= 32) throw winrt::hresult_invalid_argument(::GetSettingsString(L"PropertyDepth"));
                auto Child = Make(ChildrenPanel, Id + L"_" + std::to_wstring(Next++),
                    Depth + 1, false, Type == Kind::Object, Key, Value);
                Button Remove;
                Remove.Content(winrt::box_value(::GetSettingsString(L"PropertyRemove")));
                Identify(Remove, Child->Id + L"_Remove");
                Remove.Click([Weak = weak_from_this(), ChildId = Child->Id](
                    winrt::IInspectable const&, winrt::RoutedEventArgs const&)
                {
                    if (auto Self = Weak.lock())
                    {
                        for (std::size_t Index = 0; Index < Self->Children.size(); ++Index)
                            if (Self->Children[Index]->Id == ChildId)
                            {
                                Self->ChildrenPanel.Children().RemoveAt(static_cast<std::uint32_t>(Index));
                                Self->Children.erase(Self->Children.begin() + Index);
                                break;
                            }
                    }
                });
                Child->Container.Children().Append(Remove);
                ChildrenPanel.Children().Append(Child->Container);
                Children.push_back(std::move(Child));
            }

            void LoadValue(Json const& Value)
            {
                Type = ValueKind(Value);
                Values.Children().Clear();
                Children.clear();
                Scalar = nullptr;
                Boolean = nullptr;
                ChildrenPanel = nullptr;
                Next = 0;
                if (Type == Kind::Object || Type == Kind::List)
                {
                    ChildrenPanel = Panel();
                    ChildrenPanel.Spacing(8);
                    Values.Children().Append(ChildrenPanel);
                    if (Type == Kind::Object)
                        for (auto It = Value.begin(); It != Value.end(); ++It) Append(It.key(), It.value());
                    else
                        for (auto const& Entry : Value) Append("", Entry);
                    Button Add;
                    Add.Content(winrt::box_value(::GetSettingsString(Type == Kind::Object ? L"PropertyAdd" : L"PropertyAddItem")));
                    Identify(Add, Id + L"_Add");
                    Add.IsEnabled(Depth < 32);
                    Add.Click([Weak = weak_from_this()](
                        winrt::IInspectable const&, winrt::RoutedEventArgs const&)
                    {
                        if (auto Self = Weak.lock()) Self->Append("", "");
                    });
                    Values.Children().Append(Add);
                }
                else if (Type == Kind::Boolean)
                {
                    Boolean = Check();
                    Boolean.Content(winrt::box_value(::GetSettingsString(L"PropertyOn")));
                    Boolean.IsChecked(Value.get<bool>());
                    Identify(Boolean, Id + L"_Value");
                    Values.Children().Append(Boolean);
                }
                else if (Type != Kind::Empty)
                {
                    Scalar = Box();
                    Scalar.Header(winrt::box_value(::GetSettingsString(L"PropertyValue")));
                    Scalar.Text(Value.is_string() ? winrt::to_hstring(Value.get<std::string>()) : winrt::to_hstring(Value.dump()));
                    Scalar.TextWrapping(winrt::TextWrapping::Wrap);
                    Identify(Scalar, Id + L"_Value");
                    Values.Children().Append(Scalar);
                }
                else
                {
                    Block Empty;
                    Empty.Text(::GetSettingsString(L"PropertyEmpty"));
                    Values.Children().Append(Empty);
                }
            }

            Json Read() const
            {
                if (Type == Kind::Object)
                {
                    Json Value = Json::object();
                    for (auto const& Child : Children)
                    {
                        auto Key = winrt::to_string(Child->NameInput.Text());
                        if (Key.empty() || Value.contains(Key))
                            throw winrt::hresult_invalid_argument(::GetSettingsString(L"PropertyInvalidName"));
                        Value[Key] = Child->Read();
                    }
                    return Value;
                }
                if (Type == Kind::List)
                {
                    Json Value = Json::array();
                    for (auto const& Child : Children) Value.push_back(Child->Read());
                    return Value;
                }
                if (Type == Kind::Boolean) return Boolean.IsChecked().Value();
                if (Type == Kind::Empty) return nullptr;
                auto Text = winrt::to_string(Scalar.Text());
                if (Type == Kind::Text) return Text;
                auto Number = Json::parse(Text, nullptr, false);
                if ((Type == Kind::Integer && !Number.is_number_integer()) ||
                    (Type == Kind::Decimal && !Number.is_number()))
                    throw winrt::hresult_invalid_argument(::GetSettingsString(L"PropertyInvalidNumber"));
                if (Type == Kind::Decimal) return Number.get<double>();
                return Number;
            }
        };
        std::shared_ptr<Node> m_Root;

    public:
        StructuredSettingsEditor(Panel const& Host, std::wstring Id)
        {
            m_Root = Node::Make(Host, std::move(Id), 0, true, false, "", Json::object());
            Button Clear;
            Clear.Content(winrt::box_value(::GetSettingsString(L"PropertyClear")));
            Identify(Clear, m_Root->Id + L"_Clear");
            Clear.Click([Weak = std::weak_ptr<Node>(m_Root)](
                winrt::IInspectable const&, winrt::RoutedEventArgs const&)
            {
                if (auto Root = Weak.lock()) Root->LoadValue(Json::object());
            });
            Host.Children().InsertAt(0, Clear);
        }

        void Load(Json const& Value)
        {
            if (!Value.is_object())
                throw winrt::hresult_invalid_argument(::GetSettingsString(L"PropertyObjectExpected"));
            m_Root->LoadValue(Value);
        }
        Json Read() const { return m_Root->Read(); }
    };
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


    void VirtualMachineSettingsPage::SetOperationResult(std::wstring const& Id, winrt::hstring const& Raw)
    {
        if (Id == L"ConfigurationStatusTextBox")
            this->ConfigurationStatusTextBox().Text(::DescribeSettingsResult(Raw));
        else
        {
            auto Summary = this->FindName(winrt::hstring(L"Summary" + Id)).try_as<winrt::TextBlock>();
            auto Tree = this->FindName(winrt::hstring(Id + L"Tree")).try_as<winrt::Windows::UI::Xaml::Controls::TreeView>();
            try
            {
                if (Summary) Summary.Text(::DescribeSettingsResult(Raw));
                if (Tree) ::FillResultTree(Tree, Raw);
            }
            catch (...)
            {
                if (Summary) Summary.Text(::GetSettingsString(L"ResultInfo"));
                if (Tree) Tree.RootNodes().Clear();
            }
        }
        if (this->m_HostOperations->ReportTestResult)
            this->m_HostOperations->ReportTestResult(Id, Raw);
    }

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
        using namespace winrt::Windows::UI::Xaml::Controls;
        for (auto const& Binding : ::NanaBox::SettingsText::Bindings)
        {
            auto Element = this->FindName(Binding.Element).try_as<winrt::Windows::UI::Xaml::DependencyObject>();
            if (!Element) continue;
            auto Value = ::GetSettingsString(Binding.Key);
            std::wstring_view Property = Binding.Property;
            if (Property == L"Text")
            {
                if (auto Text = Element.try_as<TextBlock>()) Text.Text(Value);
            }
            else if (Property == L"Content")
            {
                if (auto Control = Element.try_as<ContentControl>()) Control.Content(winrt::box_value(Value));
            }
            else if (Property == L"Header")
            {
                if (auto Box = Element.try_as<TextBox>()) Box.Header(winrt::box_value(Value));
                else if (auto Choice = Element.try_as<ComboBox>()) Choice.Header(winrt::box_value(Value));
            }
            else if (Property == L"AutomationProperties.Name")
                winrt::AutomationProperties::SetName(Element, Value);
            else if (Property == L"AutomationProperties.HelpText")
                winrt::AutomationProperties::SetHelpText(Element, Value);
            else if (Property == L"ToolTipService.ToolTip")
                winrt::ToolTipService::SetToolTip(Element, winrt::box_value(Value));
        }
        this->SettingsNavigation().SelectionChanged([Weak = this->get_weak()](
            winrt::IInspectable const& Sender, winrt::SelectionChangedEventArgs const&)
        {
            if (auto Page = Weak.get())
            {
                winrt::FrameworkElement Sections[] = {
                    Page->SectionGeneral(),
                    Page->SectionGpu(),
                    Page->SectionStorage(),
                    Page->SectionNetwork(),
                    Page->SectionShares(),
                    Page->SectionSession(),
                    Page->SectionSerial(),
                    Page->SectionGuestTools(),
                    Page->SectionHostManagement(),
                    Page->SectionVmms(),
                    Page->SectionHcn(),
                    Page->SectionAdvanced(),
                };
                auto Index = Sender.as<ListView>().SelectedIndex();
                if (auto NavigationItem = Sender.as<ListView>().SelectedItem().try_as<ListViewItem>())
                    Page->SectionHeading().Text(winrt::AutomationProperties::GetName(NavigationItem));
                for (int i = 0; i < static_cast<int>(sizeof(Sections) / sizeof(Sections[0])); ++i)
                    Sections[i].Visibility(i == Index ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
            }
        });
        this->ProcessorCountSlider().Maximum((std::min)(::RecommendedProcessorMaximum(), 4294967295.0));
        ::BindRange(this->ProcessorCountSlider(), this->ProcessorCountTextBox(), ::GetSettingsString(L"ProcessorCountLabel.Text"), true, 4294967295.0, 0);
        this->MemorySizeSlider().Maximum((std::min)(::RecommendedMemoryMaximum(), 9007199254740991.0));
        ::BindRange(this->MemorySizeSlider(), this->MemorySizeTextBox(), ::GetSettingsString(L"MemorySizeLabel.Text"), true, 9007199254740991.0, 0);
        ::BindRange(this->ProcessorLimitSlider(), this->ProcessorLimitTextBox(), ::GetSettingsString(L"ProcessorLimitTextBox.Header"), false, 100.0, 3);
        ::BindRange(this->ProcessorWeightSlider(), this->ProcessorWeightTextBox(), ::GetSettingsString(L"ProcessorWeightTextBox.Header"), false, 10000.0, 0);
        ::BindRange(this->DpiScaleSlider(), this->DpiScaleTextBox(), ::GetSettingsString(L"DpiScale.Header"), false, 500.0, 0);
        this->VmmsMemorySlider().Maximum((std::min)(::RecommendedMemoryMaximum(), 16777216.0));
        ::BindRange(this->VmmsMemorySlider(), this->VmmsMemoryTextBox(), ::GetSettingsString(L"VmmsMemoryTextBox.Header"), true, 16777216.0, 0);
        this->VmmsProcessorSlider().Maximum((std::min)(::RecommendedProcessorMaximum(), 2048.0));
        ::BindRange(this->VmmsProcessorSlider(), this->VmmsProcessorTextBox(), ::GetSettingsString(L"VmmsProcessorTextBox.Header"), true, 2048.0, 0);
        this->VmmsMinimumMemorySlider().Maximum((std::min)(::RecommendedMemoryMaximum(), 16777216.0));
        ::BindRange(this->VmmsMinimumMemorySlider(), this->VmmsMinimumMemoryTextBox(), ::GetSettingsString(L"VmmsMinimumMemoryTextBox.Header"), true, 16777216.0, 0);
        this->VmmsMaximumMemorySlider().Maximum((std::min)(::RecommendedMemoryMaximum(), 16777216.0));
        ::BindRange(this->VmmsMaximumMemorySlider(), this->VmmsMaximumMemoryTextBox(), ::GetSettingsString(L"VmmsMaximumMemoryTextBox.Header"), true, 16777216.0, 0);
        ::BindRange(this->VmmsMemoryBufferSlider(), this->VmmsMemoryBufferTextBox(), ::GetSettingsString(L"VmmsMemoryBufferTextBox.Header"), false, 2000.0, 0);
        ::BindRange(this->VmmsMemoryPrioritySlider(), this->VmmsMemoryPriorityTextBox(), ::GetSettingsString(L"VmmsMemoryPriorityTextBox.Header"), false, 100.0, 0);
        ::BindRange(this->VmmsCpuReserveSlider(), this->VmmsCpuReserveTextBox(), ::GetSettingsString(L"VmmsCpuReserveTextBox.Header"), false, 100.0, 0);
        ::BindRange(this->VmmsCpuMaximumSlider(), this->VmmsCpuMaximumTextBox(), ::GetSettingsString(L"VmmsCpuMaximumTextBox.Header"), false, 100.0, 0);
        ::BindRange(this->VmmsCpuWeightSlider(), this->VmmsCpuWeightTextBox(), ::GetSettingsString(L"VmmsCpuWeightTextBox.Header"), false, 10000.0, 0);
        ::BindRange(this->VmmsReplicaHistorySlider(), this->VmmsReplicaHistoryTextBox(), ::GetSettingsString(L"VmmsReplicaHistoryTextBox.Header"), false, 24.0, 0);
        ::BindRange(this->VmmsNicVlanSlider(), this->VmmsNicVlanTextBox(), ::GetSettingsString(L"VmmsNicVlanTextBox.Header"), false, 4094.0, 0);
        ::BindRange(this->VmmsControllerNumberSlider(), this->VmmsControllerNumberTextBox(), ::GetSettingsString(L"VmmsControllerNumberTextBox.Header"), false, 3.0, 0);
        ::BindRange(this->VmmsControllerLocationSlider(), this->VmmsControllerLocationTextBox(), ::GetSettingsString(L"VmmsControllerLocationTextBox.Header"), false, 63.0, 0);
        this->SettingsNavigation().SelectedIndex(0);
        this->VmmsActionComboBox().SelectionChanged([Weak = this->get_weak()](
            winrt::IInspectable const& Sender, winrt::SelectionChangedEventArgs const&)
        {
            if (auto Page = Weak.get())
                ::UpdateVmmsActionFields(Page.get(), Sender.as<winrt::ComboBox>());
        });
        ::UpdateVmmsActionFields(this, this->VmmsActionComboBox());
        this->SectionHeading().Text(::GetSettingsString(L"TabGeneral.Content"));
        this->SizeChanged([Weak = this->get_weak()](winrt::IInspectable const&, winrt::Windows::UI::Xaml::SizeChangedEventArgs const& Args)
        {
            if (auto Page = Weak.get())
                Page->SettingsNavigationColumn().Width(winrt::GridLengthHelper::FromPixels(Args.NewSize().Width < 720 ? 164 : 208));
        });
        this->m_NetworkEditor = std::make_shared<::NanaBox::StructuredSettingsEditor>(this->NetworkAdvancedSettingsEditor(), L"NetworkAdvancedSettingsEditor");
        this->m_HcnEditor = std::make_shared<::NanaBox::StructuredSettingsEditor>(this->HcnSettingsEditor(), L"HcnSettingsEditor");
        this->m_QueryEditor = std::make_shared<::NanaBox::StructuredSettingsEditor>(this->HcsQueryEditor(), L"HcsQueryEditor");
        this->m_SystemEditor = std::make_shared<::NanaBox::StructuredSettingsEditor>(this->AdditionalHcsSettingsEditor(), L"AdditionalHcsSettingsEditor");
        ::BindVisibility(this->DetailsNetworkPropertiesTextBox(), this->NetworkPropertiesTextBoxTree());
        this->SummaryNetworkPropertiesTextBox().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->DetailsNetworkDiagnosticsTextBox(), this->NetworkDiagnosticsTextBoxTree());
        this->SummaryNetworkDiagnosticsTextBox().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->DetailsHostManagementResult(), this->HostManagementResultTree());
        this->SummaryHostManagementResult().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->DetailsHcnResultTextBox(), this->HcnResultTextBoxTree());
        this->SummaryHcnResultTextBox().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->DetailsDiskResultTextBox(), this->DiskResultTextBoxTree());
        this->SummaryDiskResultTextBox().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->DetailsGuestToolsResult(), this->GuestToolsResultTree());
        this->SummaryGuestToolsResult().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->DetailsVmmsResultTextBox(), this->VmmsResultTextBoxTree());
        this->SummaryVmmsResultTextBox().Text(::GetSettingsString(L"ResultWaiting"));
        ::BindVisibility(this->MirroredNetworkingCheckBox(), this->MirroredOptionsPanel());
        ::BindVisibility(this->NetworkExpertCheckBox(), this->NetworkExpertCheckBoxPanel());
        ::BindVisibility(this->HcnExpertCheckBox(), this->HcnExpertCheckBoxPanel());
        auto UpdateHcnForm = [Weak = this->get_weak()]()
        {
            if (auto Page = Weak.get())
            {
                auto Choice = Page->HcnKindComboBox().SelectedItem().try_as<ComboBoxItem>();
                if (!Choice) return;
                auto Kind = winrt::unbox_value<winrt::hstring>(Choice.Tag());
                bool Expert = Page->HcnExpertCheckBox().IsChecked().Value();
                auto Visible = [](bool Show) { return Show ? winrt::Visibility::Visible : winrt::Visibility::Collapsed; };
                Page->HcnGuidedPanel().Visibility(Visible(!Expert));
                Page->HcnNameTextBox().Visibility(Visible(Expert || Kind == L"Network"));
                Page->HcnNetworkIdTextBox().Visibility(Visible(Expert));
                Page->HcnParentNetworkPanel().Visibility(Visible(Kind == L"Endpoint"));
                Page->HcnNamespacePanel().Visibility(Visible(Kind == L"Namespace"));
                Page->HcnEndpointReferencesPanel().Visibility(Visible(Kind == L"Namespace" || Kind == L"LoadBalancer" || Kind == L"Route"));
                Page->HcnLoadBalancerPanel().Visibility(Visible(Kind == L"LoadBalancer"));
                Page->HcnRoutePanel().Visibility(Visible(Kind == L"Route"));
            }
        };
        this->HcnKindComboBox().SelectionChanged([UpdateHcnForm](winrt::IInspectable const&, winrt::SelectionChangedEventArgs const&) { UpdateHcnForm(); });
        this->HcnExpertCheckBox().Checked([UpdateHcnForm](winrt::IInspectable const&, winrt::RoutedEventArgs const&) { UpdateHcnForm(); });
        this->HcnExpertCheckBox().Unchecked([UpdateHcnForm](winrt::IInspectable const&, winrt::RoutedEventArgs const&) { UpdateHcnForm(); });
        UpdateHcnForm();
        ::BindVisibility(this->HcsQueryExpertCheckBox(), this->HcsQueryExpertCheckBoxPanel());
        auto UpdateQueryMode = [Weak = winrt::make_weak(this->HcsQueryModeComboBox())](winrt::IInspectable const& Sender, winrt::RoutedEventArgs const&)
        {
            if (auto Choice = Weak.get()) Choice.Visibility(Sender.as<winrt::CheckBox>().IsChecked().Value()
                ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
        };
        this->HcsQueryExpertCheckBox().Checked(UpdateQueryMode);
        this->HcsQueryExpertCheckBox().Unchecked(UpdateQueryMode);
        ::BindVisibility(this->HcsExtensionsExpertCheckBox(), this->HcsExtensionsExpertCheckBoxPanel());
        auto UpdateNetworkMode = [Weak = this->get_weak()]()
        {
            if (auto Page = Weak.get())
            {
                bool Expert = Page->NetworkExpertCheckBox().IsChecked().Value();
                Page->NetworkBasicPanel().Visibility(Expert ? winrt::Visibility::Collapsed : winrt::Visibility::Visible);
                Page->NetworkNatPanel().Visibility(Page->NetworkModeComboBox().SelectedIndex() == 1
                    ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
            }
        };
        this->NetworkModeComboBox().SelectionChanged([UpdateNetworkMode](winrt::IInspectable const&, winrt::SelectionChangedEventArgs const&) { UpdateNetworkMode(); });
        this->NetworkExpertCheckBox().Checked([UpdateNetworkMode](winrt::IInspectable const&, winrt::RoutedEventArgs const&) { UpdateNetworkMode(); });
        this->NetworkExpertCheckBox().Unchecked([UpdateNetworkMode](winrt::IInspectable const&, winrt::RoutedEventArgs const&) { UpdateNetworkMode(); });
        UpdateNetworkMode();
        ::BindValuePicker(this->ClipboardKeyboardLayoutComboBox(), this->ClipboardKeyboardLayoutTextBox());
        ::BindValuePicker(this->FullScreenHotkeyComboBox(), this->FullScreenHotkeyTextBox());
        ::BindValuePicker(this->CtrlEscHotkeyComboBox(), this->CtrlEscHotkeyTextBox());
        ::BindValuePicker(this->AltEscHotkeyComboBox(), this->AltEscHotkeyTextBox());
        ::BindValuePicker(this->AltTabHotkeyComboBox(), this->AltTabHotkeyTextBox());
        ::BindValuePicker(this->AltShiftTabHotkeyComboBox(), this->AltShiftTabHotkeyTextBox());
        ::BindValuePicker(this->AltSpaceHotkeyComboBox(), this->AltSpaceHotkeyTextBox());
        ::BindValuePicker(this->CtrlAltDelHotkeyComboBox(), this->CtrlAltDelHotkeyTextBox());
        ::BindValuePicker(this->FocusReleaseLeftHotkeyComboBox(), this->FocusReleaseLeftHotkeyTextBox());
        ::BindValuePicker(this->FocusReleaseRightHotkeyComboBox(), this->FocusReleaseRightHotkeyTextBox());
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
            this->ProcessorLimitTextBox().Text(::FormatCpuPercentage(this->m_Configuration.ProcessorLimit));
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
            this->ClipboardWindowsUnicodeCheckBox().IsChecked(Configuration.ClipboardWindowsUnicode);
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
            ::PopulateValuePicker(this->ClipboardKeyboardLayoutComboBox(), this->ClipboardKeyboardLayoutTextBox(), ::KeyboardLayouts(), L"LayoutCustom");
            auto KeyChoices = ::HotkeyChoices();
            ::PopulateValuePicker(this->FullScreenHotkeyComboBox(), this->FullScreenHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->CtrlEscHotkeyComboBox(), this->CtrlEscHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->AltEscHotkeyComboBox(), this->AltEscHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->AltTabHotkeyComboBox(), this->AltTabHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->AltShiftTabHotkeyComboBox(), this->AltShiftTabHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->AltSpaceHotkeyComboBox(), this->AltSpaceHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->CtrlAltDelHotkeyComboBox(), this->CtrlAltDelHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->FocusReleaseLeftHotkeyComboBox(), this->FocusReleaseLeftHotkeyTextBox(), KeyChoices, L"KeyCustom");
            ::PopulateValuePicker(this->FocusReleaseRightHotkeyComboBox(), this->FocusReleaseRightHotkeyTextBox(), KeyChoices, L"KeyCustom");
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
            this->m_SystemEditor->Load(this->m_Configuration.AdditionalHcsSettings.empty()
                ? nlohmann::json::object() : nlohmann::json::parse(this->m_Configuration.AdditionalHcsSettings));
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
            bool CanEdit = !this->m_MainWindow ||
                this->m_Configuration.GuestType == NanaBox::GuestType::Windows;
            this->VirtualSmbShareSection().Visibility(CanEdit
                ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
            this->VirtualSmbLiveNote().Visibility(this->m_MainWindow
                ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
            this->VirtualSmbSharePanel().Children().Clear();
            this->m_VirtualSmbShareSettings.clear();
            this->AddVirtualSmbShareButton().IsEnabled(CanEdit && (!this->m_MainWindow ||
                !this->m_Configuration.VirtualSmbShares.empty() || this->m_Configuration.Gpu.EnableHostDriverStore));
            if (CanEdit)
            {
                for (auto const& Configuration :
                    this->m_Configuration.VirtualSmbShares)
                {
                    this->AddVirtualSmbShare(Configuration, this->m_MainWindow != nullptr);
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
                Result.ProcessorLimit = ::ReadCpuPercentage(this->ProcessorLimitTextBox());
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
            Result.Keyboard.ClipboardWindowsUnicode = Result.GuestType == NanaBox::GuestType::Windows &&
                winrt::unbox_value<bool>(this->ClipboardWindowsUnicodeCheckBox().IsChecked());
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
                auto Additional = this->m_SystemEditor->Read();
                Result.AdditionalHcsSettings = Additional.empty() ? std::string() : Additional.dump();
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
            if (!this->m_MainWindow || Result.GuestType == NanaBox::GuestType::Windows)
            {
                Result.VirtualSmbShares.clear();
                for (VirtualSmbShareSettings const& Row :
                    this->m_VirtualSmbShareSettings)
                {
                    NanaBox::VirtualSmbShareConfiguration Configuration;
                    Configuration.ReadOnly =
                        Row.ReadOnly.IsChecked().Value();
                    Configuration.AutoMount = Row.AutoMount.IsChecked().Value();
                    Configuration.NoDirectmap = Row.NoDirectmap.IsChecked().Value();
                    for (auto const& [Key, Control] : Row.Options)
                    {
                        auto Index = Control.SelectedIndex();
                        if (Index == 1 || Index == 2) Configuration.Options.emplace(Key, Index == 1);
                    }
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
                this->SetOperationResult(L"ConfigurationStatusTextBox", this->m_HostOperations->ExternalAction(L"OpenConfiguration", Path));
                return;
            }
            Path = ::GetAbsolutePath(Path);
            DWORD Attributes = ::GetFileAttributesW(Path.c_str());
            if (Attributes == INVALID_FILE_ATTRIBUTES) winrt::throw_last_error();
            if (Attributes & FILE_ATTRIBUTE_DIRECTORY) throw winrt::hresult_invalid_argument();
            wchar_t SystemDirectory[MAX_PATH] = {};
            UINT Length = ::GetSystemDirectoryW(SystemDirectory, MAX_PATH);
            if (Length == 0) winrt::throw_last_error();
            if (Length >= MAX_PATH) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
            std::wstring Editor = std::wstring(SystemDirectory, Length) + L"\\notepad.exe";
            std::wstring Parameters = ::QuoteCommandLineArgument(Path);
            SHELLEXECUTEINFOW Info = {};
            Info.cbSize = sizeof(Info);
            Info.hwnd = this->m_WindowHandle;
            Info.fMask = SEE_MASK_FLAG_NO_UI;
            Info.lpVerb = L"open";
            Info.lpFile = Editor.c_str();
            Info.lpParameters = Parameters.c_str();
            Info.nShow = SW_SHOWNORMAL;
            winrt::check_bool(::ShellExecuteExW(&Info));
            this->ConfigurationStatusTextBox().Text(::GetSettingsString(L"ConfigurationEditorOpened"));
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
                this->SetOperationResult(L"ConfigurationStatusTextBox", this->m_HostOperations->ExternalAction(L"Reference", L""));
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
                ::NanaBox::WorkspaceHistory::TryRemember(FilePath);
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
        winrt::StackPanel Advanced;
        Advanced.Spacing(8);
        Row.DeviceInterface = ::AddSettingTextBox(
            Advanced,
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
            Advanced,
            L"GpuDevicePartitionIdLabel",
            L"GpuDevicePartitionIdHelp",
            winrt::to_hstring(PartitionId),
            false);
        Row.PartitionId.HorizontalAlignment(winrt::HorizontalAlignment::Left);

        winrt::CheckBox Expert;
        Expert.Content(winrt::box_value(::GetSettingsString(L"GpuExpert")));
        winrt::AutomationProperties::SetAutomationId(Expert, L"GpuAdapterExpertCheckBox");
        Row.Container.Children().Append(Expert);
        Row.Container.Children().Append(Advanced);
        ::BindVisibility(Expert, Advanced);

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
            !this->GuestLinuxShareNote() || !this->GuestWindowsShareNote() || !this->GuestVsmbMountPanel()) return;
        auto Type = static_cast<NanaBox::GuestType>(this->GuestTypeComboBox().SelectedIndex());
        bool WindowsGuest = Type == NanaBox::GuestType::Windows;
        if (this->ClipboardGtkUnicodeCheckBox())
            this->ClipboardGtkUnicodeCheckBox().IsEnabled(Type == NanaBox::GuestType::Linux);
        if (this->ClipboardWindowsUnicodeCheckBox())
            this->ClipboardWindowsUnicodeCheckBox().IsEnabled(WindowsGuest);
        this->GuestManualMountPanel().Visibility(winrt::Visibility::Visible);
        this->GuestVsmbMountPanel().Visibility(WindowsGuest ? winrt::Visibility::Visible : winrt::Visibility::Collapsed);
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
            if (Resource == "Plan9Mount" || Resource == "Plan9Unmount" ||
                Resource == "VirtualSmbMount" || Resource == "VirtualSmbUnmount")
            {
                Name = winrt::to_string(this->GuestPlan9NameTextBox().Text());
                if (Name.empty()) throw winrt::hresult_invalid_argument(::GetSettingsString(
                    Resource == "VirtualSmbMount" || Resource == "VirtualSmbUnmount" ?
                    L"VirtualSmbShareNameLabel" : L"Plan9ShareNameLabel"));
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
            this->SetOperationResult(L"GuestToolsResult", winrt::to_hstring(nlohmann::json{
                {"Success", false}, {"Error", winrt::to_string(Error.message())}}.dump(2)));
        }
    }

    void VirtualMachineSettingsPage::SetGuestToolsBusy(bool Busy, bool Cancellable)
    {
        this->m_GuestToolsRunning = Busy;
        for (auto const& Button : {this->GuestHeartbeatControl(), this->GuestInformationControl(),
            this->GuestDiagnosticsControl(), this->GuestPlan9Control(), this->GuestPlan9MountControl(),
            this->GuestPlan9UnmountControl(), this->GuestVsmbMountControl(), this->GuestVsmbUnmountControl(),
            this->GuestClockQueryControl(), this->GuestClockSyncControl(),
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
        this->SetOperationResult(L"GuestToolsResult", ::GetSettingsString(L"DiskOperationRunning"));
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
            this->SetOperationResult(L"GuestToolsResult", winrt::to_hstring(Result));
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
                Data.SetText(winrt::hstring(std::wstring(this->SummaryNetworkDiagnosticsTextBox().Text().c_str()) + L"\r\n" + ::CopyResultTree(this->NetworkDiagnosticsTextBoxTree())));
                winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(Data);
                winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
                return;
            }
            if (Action != L"Host" && this->m_HostOperations->NetworkDiagnostics)
            {
                this->SetOperationResult(L"NetworkDiagnosticsTextBox", this->m_HostOperations->NetworkDiagnostics(Action == L"Retry"));
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
                    if (auto Page = Weak.get()) Page->SetOperationResult(L"NetworkDiagnosticsTextBox", Report);
                });
            });
        }
        catch (...) { this->SetOperationResult(L"NetworkDiagnosticsTextBox", Mile::WinRT::ToHResultError().message()); }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunHostPreflight()
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        this->SetOperationResult(L"NetworkDiagnosticsTextBox", ::GetSettingsString(L"DiskOperationRunning"));
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
            this->SetOperationResult(L"NetworkDiagnosticsTextBox", Result);
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
        this->NetworkAdvancedSettingsEditorContainer().IsEnabled(!Busy);
        this->NetworkModeComboBox().IsEnabled(!Busy);
        this->NetworkSubnetTextBox().IsEnabled(!Busy);
        this->NetworkGatewayTextBox().IsEnabled(!Busy);
        this->NetworkExpertCheckBox().IsEnabled(!Busy);
    }

    void VirtualMachineSettingsPage::RefreshNetworksButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        this->RunNetworkManagementOperation(L"Refresh");
    }

    void VirtualMachineSettingsPage::CreateNetworkButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (this->m_NetworkManagementRunning) return;
        try
        {
            winrt::hstring Settings;
            if (this->NetworkExpertCheckBox().IsChecked().Value())
                {
                    auto Properties = this->m_NetworkEditor->Read();
                    Settings = Properties.empty() ? winrt::hstring() : winrt::to_hstring(Properties.dump());
                }
            else if (this->NetworkModeComboBox().SelectedIndex() == 1)
                Settings = ::BuildNatNetworkSettings(this->NetworkSubnetTextBox().Text(), this->NetworkGatewayTextBox().Text());
            this->RunNetworkManagementOperation(L"Create", this->NetworkNameTextBox().Text(),
                this->NetworkFirewallCheckBox().IsChecked().Value(), Settings);
        }
        catch (...)
        {
            this->SetOperationResult(L"NetworkPropertiesTextBox", winrt::to_hstring(nlohmann::json{
                {"Success", false}, {"Operation", "Create"}, {"Error", winrt::to_string(Mile::WinRT::ToHResultError().message())}}.dump(2)));
        }
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
        this->SetOperationResult(L"NetworkPropertiesTextBox", ::GetSettingsString(L"DiskOperationRunning"));
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
                Result = {{"Success", true}, {"Operation", winrt::to_string(Action)}, {"Count", Networks.size()},
                    {"Networks", Json::array()}};
                for (auto const& Network : Networks)
                    Result["Networks"].push_back({{"Id", winrt::to_string(::FromGuid(Network.Id))},
                        {"Name", winrt::to_string(Network.Name)}, {"Type", winrt::to_string(Network.Type)}});
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
                    if (Row.NetworkPicker) ::FillNetworkPicker(Row.NetworkPicker, this->m_HostNetworks, true, Row.NetworkId.Text());
                if (Action == L"Create" && this->NetworkNameTextBox().Text() == Value)
                {
                    this->NetworkNameTextBox().Text(L"");
                    this->m_NetworkEditor->Load(nlohmann::json::object());
                }
            }
            this->SetOperationResult(L"NetworkPropertiesTextBox", winrt::to_hstring(Result.dump(2)));
        }
        catch (...)
        {
            this->m_NetworkManagementRunning = false;
            try { this->SetNetworkManagementBusy(false); this->SetOperationResult(L"NetworkPropertiesTextBox", Mile::WinRT::ToHResultError().message()); }
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
            this->SetOperationResult(L"HostManagementResult", ::GetSettingsString(L"DiskOperationRunning"));
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
                this->SetOperationResult(L"HostManagementResult", winrt::to_hstring(Json{{"Success", false}, {"Operation", winrt::to_string(Action)},
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
                    winrt::AutomationProperties::SetAutomationId(Value, winrt::hstring(
                        std::wstring(winrt::AutomationProperties::GetAutomationId(Box).c_str()) + L"_" + winrt::to_hstring(Id).c_str()));
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
            this->SetOperationResult(L"HostManagementResult", winrt::to_hstring(Result.dump(2)));
        }
        catch (...)
        {
            this->m_HostManagementRunning = false;
            try { this->SetOperationResult(L"HostManagementResult", Mile::WinRT::ToHResultError().message()); }
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
            {
                if (this->HcnExpertCheckBox().IsChecked().Value())
                    Data["Settings"] = this->m_HcnEditor->Read();
                else
                {
                    auto Kind = Data.at("Kind").get<std::string>();
                    Json Settings = {{"SchemaVersion", {{"Major", 2}, {"Minor", 0}}}};
                    auto References = [&]()
                    {
                        Json Ids = Json::array();
                        for (auto const& Entry : this->HcnEndpointReferencesList().SelectedItems())
                            Ids.push_back(winrt::to_string(winrt::unbox_value<winrt::hstring>(
                                Entry.as<winrt::Windows::UI::Xaml::Controls::ListViewItem>().Tag())));
                        if (Ids.empty()) throw winrt::hresult_invalid_argument(::GetSettingsString(L"HcnSelectReferences"));
                        return Ids;
                    };
                    auto Address = [](winrt::TextBox const& Input)
                    {
                        auto Value = winrt::to_string(Input.Text());
                        std::uint32_t AddressValue = 0;
                        if (!::ParseIpv4(Value, AddressValue))
                            throw winrt::hresult_invalid_argument(::GetSettingsString(L"HcnInvalidAddress"));
                        return Value;
                    };
                    auto Port = [](winrt::TextBox const& Input)
                    {
                        auto Value = winrt::to_string(Input.Text());
                        unsigned int PortValue = 0;
                        auto Parsed = std::from_chars(Value.data(), Value.data() + Value.size(), PortValue);
                        if (Parsed.ec != std::errc() || Parsed.ptr != Value.data() + Value.size() ||
                            PortValue < 1 || PortValue > 65535)
                            throw winrt::hresult_invalid_argument(::GetSettingsString(L"HcnInvalidPort"));
                        return PortValue;
                    };
                    if (Kind == "Network" && Action == L"HcnCreate")
                    {
                        Settings["Type"] = "ICS";
                        Settings["IsolateSwitch"] = true;
                        Settings["Flags"] = ::NanaBox::HcnNetworkFlags::EnableDns | ::NanaBox::HcnNetworkFlags::EnableDhcp;
                    }
                    else if (Kind == "Endpoint" && Action == L"HcnCreate")
                    {
                        if (!this->HcnParentNetworkComboBox().SelectedItem())
                            throw winrt::hresult_invalid_argument(::GetSettingsString(L"HcnSelectParentNetwork"));
                        Data["NetworkId"] = Selected(this->HcnParentNetworkComboBox());
                    }
                    else if (Kind == "Namespace" && Action == L"HcnCreate")
                        Settings["Type"] = Selected(this->HcnNamespaceTypeComboBox());
                    else if (Kind == "Namespace" && Action == L"HcnModify")
                    {
                        auto Ids = References();
                        if (Ids.size() != 1) throw winrt::hresult_invalid_argument(::GetSettingsString(L"HcnSelectNamespaceEndpoint"));
                        Settings = {{"ResourceType", "Endpoint"}, {"RequestType", Selected(this->HcnNamespaceActionComboBox())},
                            {"Settings", {{"EndpointId", Ids[0]}}}};
                    }
                    else if (Kind == "LoadBalancer")
                    {
                        Settings["HostComputeEndpoints"] = References();
                        Settings["FrontendVIPs"] = Json::array({Address(this->HcnFrontendIpTextBox())});
                        Settings["PortMappings"] = Json::array({{
                            {"Protocol", this->HcnProtocolComboBox().SelectedIndex() == 1 ? 17 : 6},
                            {"ExternalPort", Port(this->HcnExternalPortTextBox())},
                            {"InternalPort", Port(this->HcnInternalPortTextBox())}}});
                    }
                    else if (Kind == "Route" && Action == L"HcnCreate")
                    {
                        auto Destination = winrt::to_string(this->HcnDestinationTextBox().Text());
                        auto Slash = Destination.find('/');
                        std::uint32_t NetworkAddress = 0;
                        unsigned int Prefix = 0;
                        if (Slash == std::string::npos || !::ParseIpv4(std::string_view(Destination).substr(0, Slash), NetworkAddress))
                            throw winrt::hresult_invalid_argument(::GetSettingsString(L"InvalidIpv4Subnet"));
                        auto PrefixEnd = Destination.data() + Destination.size();
                        auto ParsedPrefix = std::from_chars(Destination.data() + Slash + 1, PrefixEnd, Prefix);
                        if (ParsedPrefix.ec != std::errc() || ParsedPrefix.ptr != PrefixEnd || Prefix > 32 ||
                            (Prefix == 0 ? NetworkAddress != 0 : (NetworkAddress & ~(0xffffffffu << (32 - Prefix))) != 0))
                            throw winrt::hresult_invalid_argument(::GetSettingsString(L"InvalidIpv4Subnet"));
                        Settings["HostComputeEndpoints"] = References();
                        Settings["Setting"] = Json::array({{
                            {"DestinationPrefix", Destination}, {"NextHop", Address(this->HcnNextHopTextBox())},
                            {"NeedEncap", this->HcnEncapsulationCheckBox().IsChecked().Value()}}});
                    }
                    else throw winrt::hresult_invalid_argument(::GetSettingsString(L"HcnExpertRequired"));
                    Data["Settings"] = std::move(Settings);
                }
            }
            if ((Action == L"HcnModify" || Action == L"HcnDelete") &&
                ::MessageBoxW(this->m_WindowHandle, ::GetSettingsString(L"HostManagementConfirm").c_str(),
                    L"NanaBox", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
            this->m_HcnResourceRunning = true;
            this->HcnKindComboBox().IsEnabled(false);
            this->SetOperationResult(L"HcnResultTextBox", ::GetSettingsString(L"DiskOperationRunning"));
            this->RunHcnResourceOperation(Action, Data.dump());
        }
        catch (...)
        {
            this->m_HcnResourceRunning = false;
            this->HcnKindComboBox().IsEnabled(true);
            this->SetOperationResult(L"HcnResultTextBox", Mile::WinRT::ToHResultError().message());
        }
    }

    winrt::fire_and_forget VirtualMachineSettingsPage::RunHcnResourceOperation(
        winrt::hstring Action, std::string Arguments)
    {
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        using Json = nlohmann::json;
        Json Result, Items, ParentNetworks, EndpointReferences;
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
            if (Action == L"HcnList")
            {
                Items = Result.at("Items");
                auto Kind = Data.at("Kind").get<std::string>();
                try
                {
                    if (Kind == "Endpoint")
                        ParentNetworks = Run(L"HcnList", Json{{"Kind", "Network"}}).at("Items");
                    if (Kind == "Namespace" || Kind == "LoadBalancer" || Kind == "Route")
                        EndpointReferences = Run(L"HcnList", Json{{"Kind", "Endpoint"}}).at("Items");
                }
                catch (...) { Result["ReferenceRefreshError"] = winrt::to_string(Mile::WinRT::ToHResultError().message()); }
            }
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
                if (ParentNetworks.is_array())
                {
                    auto ParentChoice = this->HcnParentNetworkComboBox();
                    ParentChoice.Items().Clear();
                    for (auto const& Parent : ParentNetworks)
                        if (Parent.value("Managed", false))
                            ::AddPickerItem(ParentChoice, winrt::to_hstring(Parent.at("Name").get<std::string>()),
                                winrt::to_hstring(Parent.at("Id").get<std::string>()));
                    if (ParentChoice.Items().Size()) ParentChoice.SelectedIndex(0);
                }
                if (EndpointReferences.is_array())
                {
                    auto ReferenceList = this->HcnEndpointReferencesList();
                    ReferenceList.Items().Clear();
                    for (auto const& Reference : EndpointReferences)
                    {
                        if (!Reference.value("Managed", false)) continue;
                        winrt::Windows::UI::Xaml::Controls::ListViewItem ReferenceItem;
                        ReferenceItem.Content(winrt::box_value(winrt::to_hstring(Reference.at("Name").get<std::string>())));
                        ReferenceItem.Tag(winrt::box_value(winrt::to_hstring(Reference.at("Id").get<std::string>())));
                        winrt::AutomationProperties::SetAutomationId(ReferenceItem, L"HcnEndpointReference");
                        ReferenceList.Items().Append(ReferenceItem);
                    }
                }
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
                        winrt::AutomationProperties::SetAutomationId(Item, winrt::hstring(
                            std::wstring(winrt::AutomationProperties::GetAutomationId(Box).c_str()) + L"_" + winrt::to_hstring(Id).c_str()));
                        Box.Items().Append(Item);
                    }
                    if (Selection >= 0) Box.SelectedIndex(Selection);
                    else if (Box.Items().Size()) Box.SelectedIndex(0);
                }
            }
            Result["Operation"] = winrt::to_string(Action);
            this->SetOperationResult(L"HcnResultTextBox", winrt::to_hstring(Result.dump(2)));
        }
        catch (...) { this->SetOperationResult(L"HcnResultTextBox", Mile::WinRT::ToHResultError().message()); }
    }

    void VirtualMachineSettingsPage::VmmsSelectionChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        if (auto Box = this->VmmsCheckpointsComboBox()) Box.Items().Clear();
        if (auto Box = this->VmmsDevicesComboBox()) Box.Items().Clear();
        if (auto Box = this->VmmsDeviceSwitchComboBox()) Box.Items().Clear();
        if (auto Box = this->VmmsGpuSourceComboBox()) Box.Items().Clear();
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
                        Confirmation += ::DescribeVmmsParameters(this, Action);
                    }
                    else if (Action == L"VmmsCheckpointPolicy")
                    {
                        Data["Type"] = Selected(this->VmmsCheckpointTypeComboBox());
                        Data["Enabled"] = this->VmmsAutomaticCheckpointsCheckBox().IsChecked().Value();
                        if (Data["Type"] == "Disabled" && Data["Enabled"] == true)
                            throw winrt::hresult_invalid_argument(L"Automatic checkpoints require checkpoints to be enabled.");
                        Confirmation += ::DescribeVmmsParameters(this, Action);
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
                        Confirmation += ::DescribeVmmsParameters(this, Action);
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
                            Data["Frequency"] = std::stoul(Selected(this->VmmsReplicaFrequencyComboBox()));
                            if (Data["Frequency"] != 30 && Data["Frequency"] != 300 && Data["Frequency"] != 900)
                                throw winrt::hresult_invalid_argument(L"Replication interval must be 30, 300 or 900 seconds.");
                            Data["RecoveryHistory"] = Number(this->VmmsReplicaHistoryTextBox(), 0, 24);
                            Data["Authentication"] = Selected(this->VmmsReplicaAuthComboBox());
                            Data["Thumbprint"] = winrt::to_string(this->VmmsReplicaCertificateTextBox().Text());
                            Data["Compression"] = this->VmmsReplicaCompressionCheckBox().IsChecked().Value();
                        }
                        Confirmation += ::DescribeVmmsParameters(this, Action);
                    }
                    else if (Action == L"VmmsCpuPolicy")
                    {
                        Data["Reserve"] = Number(this->VmmsCpuReserveTextBox(), 0, 100);
                        Data["Maximum"] = Number(this->VmmsCpuMaximumTextBox(), 0, 100);
                        Data["Weight"] = Number(this->VmmsCpuWeightTextBox(), 1, 10000);
                        Data["Nested"] = this->VmmsCpuNestedCheckBox().IsChecked().Value();
                        if (Data["Reserve"] > Data["Maximum"]) throw winrt::hresult_invalid_argument(L"CPU reserve exceeds maximum.");
                        Confirmation += ::DescribeVmmsParameters(this, Action);
                    }
                    else if (Action == L"VmmsGpuAdd" || Action == L"VmmsGpuRemove" || Action == L"VmmsGpuQuotas")
                    {
                        if (Action == L"VmmsGpuAdd") Data["GpuId"] = Selected(this->VmmsGpuSourceComboBox());
                        else Data["DeviceId"] = Selected(this->VmmsDevicesComboBox());
                        if (Action != L"VmmsGpuRemove")
                        {
                            Json Quotas = Json::object();
                            auto SetQuota = [&](char const* Key, winrt::TextBox const& Box)
                            {
                                if (!Box.Text().empty()) Quotas[Key] = std::to_string(Number(Box, 1, UINT64_MAX));
                            };
                            SetQuota("MinPartitionVRAM", this->VmmsGpuMinVRAMTextBox());
                            SetQuota("OptimalPartitionVRAM", this->VmmsGpuOptimalVRAMTextBox());
                            SetQuota("MaxPartitionVRAM", this->VmmsGpuMaxVRAMTextBox());
                            SetQuota("MinPartitionEncode", this->VmmsGpuMinEncodeTextBox());
                            SetQuota("OptimalPartitionEncode", this->VmmsGpuOptimalEncodeTextBox());
                            SetQuota("MaxPartitionEncode", this->VmmsGpuMaxEncodeTextBox());
                            SetQuota("MinPartitionDecode", this->VmmsGpuMinDecodeTextBox());
                            SetQuota("OptimalPartitionDecode", this->VmmsGpuOptimalDecodeTextBox());
                            SetQuota("MaxPartitionDecode", this->VmmsGpuMaxDecodeTextBox());
                            SetQuota("MinPartitionCompute", this->VmmsGpuMinComputeTextBox());
                            SetQuota("OptimalPartitionCompute", this->VmmsGpuOptimalComputeTextBox());
                            SetQuota("MaxPartitionCompute", this->VmmsGpuMaxComputeTextBox());
                            if (Action == L"VmmsGpuQuotas" && Quotas.empty())
                                throw winrt::hresult_invalid_argument(L"Specify at least one GPU quota to change.");
                            Data["Quotas"] = std::move(Quotas);
                        }
                        Confirmation += ::DescribeVmmsParameters(this, Action);
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
                        Confirmation += ::DescribeVmmsParameters(this, Action);
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
                        Confirmation += ::DescribeVmmsParameters(this, Action);
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
                Data["Query"] = Json::object();
                if (this->HcsQueryExpertCheckBox().IsChecked().Value())
                {
                    Data["Query"] = this->m_QueryEditor->Read();
                }
                else if (this->HcsQueryModeComboBox().SelectedIndex() > 0)
                    Data["Query"]["PropertyTypes"] = Json::array({Selected(this->HcsQueryModeComboBox())});
                if (!Data["Query"].is_object())
                    throw winrt::hresult_invalid_argument(L"Property query must be a JSON object.");
            }
            else if (Action != L"VmmsList" && Action != L"HcsInventory") throw winrt::hresult_invalid_argument();
            this->m_VmmsRunning = true;
            this->VmmsForm().IsEnabled(false);
            this->SetOperationResult(L"VmmsResultTextBox", ::GetSettingsString(L"DiskOperationRunning"));
            this->RunVmmsOperation(Action, Data.dump());
        }
        catch (...)
        {
            this->m_VmmsRunning = false;
            this->VmmsForm().IsEnabled(true);
            this->SetOperationResult(L"VmmsResultTextBox", winrt::to_hstring(Json{{"Success", false},
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
                    winrt::AutomationProperties::SetAutomationId(Value, winrt::hstring(
                        std::wstring(winrt::AutomationProperties::GetAutomationId(Box).c_str()) + L"_" + winrt::to_hstring(Id).c_str()));
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
                        winrt::AutomationProperties::SetAutomationId(Value, winrt::hstring(
                            std::wstring(winrt::AutomationProperties::GetAutomationId(Box).c_str()) + L"_" + winrt::to_hstring(Id).c_str()));
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
                    auto Sources = Details.value("PartitionableGpus", Json::array());
                    for (auto& Source : Sources)
                        if (Source.at("Id") == "auto") Source["Name"] = winrt::to_string(::GetSettingsString(L"VmmsGpuAutomatic"));
                    Fill(this->VmmsGpuSourceComboBox(), Sources, "auto");
                }
            }
            if (!Result.contains("Success")) Result["Success"] = true;
            Result["Operation"] = winrt::to_string(Action);
            this->SetOperationResult(L"VmmsResultTextBox", winrt::to_hstring(Result.dump(2)));
        }
        catch (...)
        {
            this->m_VmmsRunning = false;
            try
            {
                this->VmmsForm().IsEnabled(true);
                this->SetOperationResult(L"VmmsResultTextBox", Mile::WinRT::ToHResultError().message());
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
            this->SetOperationResult(L"DiskResultTextBox", ::GetSettingsString(L"DiskOperationRunning"));
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
            this->SetOperationResult(L"DiskResultTextBox", winrt::to_hstring(Result));
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
        winrt::StackPanel Advanced;
        Advanced.Spacing(8);
        Row.MacAddress = ::AddSettingTextBox(
            Advanced,
            L"NetworkAdapterMacAddressLabel",
            L"NetworkAdapterMacAddressHelp",
            winrt::to_hstring(Configuration.MacAddress),
            false);
        Row.NetworkId = ::AddSettingTextBox(
            Advanced, L"NetworkIdLabel", L"NetworkIdHelp",
            winrt::to_hstring(Configuration.NetworkId), this->m_MainWindow && Existing);
        Row.FirewallCreatorId = ::AddSettingTextBox(
            Advanced, L"FirewallCreatorLabel", L"FirewallCreatorHelp",
            winrt::to_hstring(Configuration.FirewallCreatorId), this->m_MainWindow && Existing);
        if (!(this->m_MainWindow && Existing))
        {
            Row.NetworkPicker = ::MakeHostPicker(Row.Container, Row.NetworkId, L"HostNetworkLabel", L"HostNetworkHelp");
            ::FillNetworkPicker(Row.NetworkPicker, this->m_HostNetworks, true, Row.NetworkId.Text());
        }
        Row.SuggestedSwitchId = ::AddSettingTextBox(
            Advanced,
            L"NetworkAdapterSuggestedSwitchIdLabel",
            L"NetworkAdapterSuggestedSwitchIdHelp",
            winrt::to_hstring(Configuration.SuggestedSwitchId),
            this->m_MainWindow && Existing);
        Row.SuggestedSwitchName = ::AddSettingTextBox(
            Advanced,
            L"NetworkAdapterSuggestedSwitchNameLabel",
            L"NetworkAdapterSuggestedSwitchNameHelp",
            winrt::to_hstring(Configuration.SuggestedSwitchName),
            this->m_MainWindow && Existing);
        Row.SuggestedSwitchSubnet = ::AddSettingTextBox(
            Advanced,
            L"NetworkAdapterSuggestedSwitchSubnetLabel",
            L"NetworkAdapterSuggestedSwitchSubnetHelp",
            winrt::to_hstring(Configuration.SuggestedSwitchSubnet),
            this->m_MainWindow && Existing);

        winrt::CheckBox Expert;
        Expert.Content(winrt::box_value(::GetSettingsString(L"AdapterExpert")));
        winrt::AutomationProperties::SetAutomationId(Expert, L"NetworkAdapterExpertCheckBox");
        Row.Container.Children().Append(Expert);
        Row.Container.Children().Append(Advanced);
        ::BindVisibility(Expert, Advanced);

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
        Row.Path.MinWidth(0);
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
        PathRow.ColumnDefinitions().Append(winrt::ColumnDefinition());
        PathRow.ColumnDefinitions().Append(BrowseColumn);
        winrt::Grid::SetColumnSpan(PathRow.Children().GetAt(0).as<winrt::FrameworkElement>(), 2);
        winrt::Grid::SetColumn(BrowseButton, 1);
        winrt::Grid::SetRow(BrowseButton, 1);
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
        NanaBox::VirtualSmbShareConfiguration const& Configuration, bool Existing)
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
        Row.AutoMount = ::AddSettingCheckBox(Row.Container, L"VirtualSmbAutoMountLabel",
            L"VirtualSmbAutoMountHelp", Configuration.AutoMount, false);
        Row.NoDirectmap = ::AddSettingCheckBox(Row.Container, L"VirtualSmbNoDirectmapLabel",
            L"VirtualSmbNoDirectmapHelp", Configuration.NoDirectmap, false);
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

        winrt::StackPanel Advanced;
        Advanced.Spacing(4);
        Advanced.Visibility(winrt::Visibility::Collapsed);
        winrt::CheckBox ShowOptions;
        ShowOptions.Content(winrt::box_value(::GetSettingsString(L"VirtualSmbShowOptions")));
        winrt::AutomationProperties::SetAutomationId(ShowOptions, L"VirtualSmbShowOptions");
        ShowOptions.IsChecked(!Configuration.Options.empty());
        Advanced.Visibility(Configuration.Options.empty()?winrt::Visibility::Collapsed:winrt::Visibility::Visible);
        ShowOptions.Checked([Weak = winrt::make_weak(Advanced)](auto const&, auto const&)
        {
            if (auto Panel = Weak.get()) Panel.Visibility(winrt::Visibility::Visible);
        });
        ShowOptions.Unchecked([Weak = winrt::make_weak(Advanced)](auto const&, auto const&)
        {
            if (auto Panel = Weak.get()) Panel.Visibility(winrt::Visibility::Collapsed);
        });
        Row.Container.Children().Append(ShowOptions);
        for (auto Key : ::NanaBox::VirtualSmbDirectoryOptions)
        {
            auto Prefix = L"VirtualSmbOption" + std::wstring(winrt::to_hstring(Key).c_str());
            winrt::ComboBox Choice;
            Choice.HorizontalAlignment(winrt::HorizontalAlignment::Stretch);
            for (auto const& [Id, Resource] : {std::pair{L"Default",L"VirtualSmbOptionDefault"},
                std::pair{L"Enabled",L"VirtualSmbOptionEnabled"},std::pair{L"Disabled",L"VirtualSmbOptionDisabled"}})
            {
                winrt::Windows::UI::Xaml::Controls::ComboBoxItem Item;
                Item.Content(winrt::box_value(::GetSettingsString(Resource)));
                winrt::AutomationProperties::SetAutomationId(Item, winrt::hstring(Prefix + Id));
                Choice.Items().Append(Item);
            }
            auto It = Configuration.Options.find(Key);
            Choice.SelectedIndex(It == Configuration.Options.end() ? 0 : It->second ? 1 : 2);
            ::AddSettingRow(Advanced, Choice, (Prefix + L"Label").c_str(), (Prefix + L"Help").c_str(), true);
            Row.Options.emplace(Key, Choice);
        }
        Row.Container.Children().Append(Advanced);
        Row.ReadOnly.IsEnabled(!Existing);
        Row.NoDirectmap.IsEnabled(!Existing);
        Row.Path.IsEnabled(!Existing);
        Row.Name.IsEnabled(!Existing);
        for (auto const& [Key, Control] : Row.Options) Control.IsEnabled(!Existing);

        winrt::Windows::UI::Xaml::Controls::Button RemoveButton;
        RemoveButton.IsEnabled(!Existing);
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
