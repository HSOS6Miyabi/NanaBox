/*
 * PROJECT:    Mouri Internal Library Essentials
 * FILE:       NanaBox.Configuration.Parser.cpp
 * PURPOSE:    Implementation for Virtual Machine Configuration Parser
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#include "NanaBox.Configuration.Parser.h"
#include <stdexcept>

nlohmann::json NanaBox::FromGuestType(
    NanaBox::GuestType const& Value)
{
    if (NanaBox::GuestType::Windows == Value)
    {
        return "Windows";
    }
    else if (NanaBox::GuestType::Linux == Value)
    {
        return "Linux";
    }

    return "Unknown";
}

NanaBox::GuestType NanaBox::ToGuestType(
    nlohmann::json const& Value)
{
    std::string RawValue = Mile::Json::ToString(Value);

    if (0 == std::strcmp(RawValue.c_str(), "Windows"))
    {
        return NanaBox::GuestType::Windows;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "Linux"))
    {
        return NanaBox::GuestType::Linux;
    }

    return NanaBox::GuestType::Unknown;
}

nlohmann::json NanaBox::FromUefiConsoleMode(
    NanaBox::UefiConsoleMode const& Value)
{
    if (NanaBox::UefiConsoleMode::Default == Value)
    {
        return "Default";
    }
    else if (NanaBox::UefiConsoleMode::ComPort1 == Value)
    {
        return "ComPort1";
    }
    else if (NanaBox::UefiConsoleMode::ComPort2 == Value)
    {
        return "ComPort2";
    }

    return "Disabled";
}

NanaBox::UefiConsoleMode NanaBox::ToUefiConsoleMode(
    nlohmann::json const& Value)
{
    std::string RawValue = Mile::Json::ToString(Value);

    if (0 == std::strcmp(RawValue.c_str(), "Default"))
    {
        return NanaBox::UefiConsoleMode::Default;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "ComPort1"))
    {
        return NanaBox::UefiConsoleMode::ComPort1;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "ComPort2"))
    {
        return NanaBox::UefiConsoleMode::ComPort2;
    }

    return NanaBox::UefiConsoleMode::Disabled;
}

nlohmann::json NanaBox::FromGpuAssignmentMode(
    NanaBox::GpuAssignmentMode const& Value)
{
    if (NanaBox::GpuAssignmentMode::Default == Value)
    {
        return "Default";
    }
    else if (NanaBox::GpuAssignmentMode::List == Value)
    {
        return "List";
    }
    else if (NanaBox::GpuAssignmentMode::Mirror == Value)
    {
        return "Mirror";
    }

    return "Disabled";
}

NanaBox::GpuAssignmentMode NanaBox::ToGpuAssignmentMode(
    nlohmann::json const& Value)
{
    std::string RawValue = Mile::Json::ToString(Value);

    if (0 == std::strcmp(RawValue.c_str(), "Default"))
    {
        return NanaBox::GpuAssignmentMode::Default;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "List"))
    {
        return NanaBox::GpuAssignmentMode::List;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "Mirror"))
    {
        return NanaBox::GpuAssignmentMode::Mirror;
    }

    return NanaBox::GpuAssignmentMode::Disabled;
}

nlohmann::json NanaBox::FromScsiDeviceType(
    NanaBox::ScsiDeviceType const& Value)
{
    if (NanaBox::ScsiDeviceType::VirtualDisk == Value)
    {
        return "VirtualDisk";
    }
    else if (NanaBox::ScsiDeviceType::VirtualImage == Value)
    {
        return "VirtualImage";
    }
    else if (NanaBox::ScsiDeviceType::PhysicalDevice == Value)
    {
        return "PhysicalDevice";
    }

    return "";
}

NanaBox::ScsiDeviceType NanaBox::ToScsiDeviceType(
    nlohmann::json const& Value)
{
    std::string RawValue = Mile::Json::ToString(Value);

    if (0 == std::strcmp(RawValue.c_str(), "VirtualDisk"))
    {
        return NanaBox::ScsiDeviceType::VirtualDisk;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "VirtualImage"))
    {
        return NanaBox::ScsiDeviceType::VirtualImage;
    }
    else if (0 == std::strcmp(RawValue.c_str(), "PhysicalDevice"))
    {
        return NanaBox::ScsiDeviceType::PhysicalDevice;
    }

    return NanaBox::ScsiDeviceType::Unknown;
}

nlohmann::json NanaBox::FromComPortsConfiguration(
    NanaBox::ComPortsConfiguration const& Value)
{
    nlohmann::json Result;

    Result["UefiConsole"] = NanaBox::FromUefiConsoleMode(Value.UefiConsole);

    if (!Value.ComPort1.empty())
    {
        Result["ComPort1"] = Value.ComPort1;
    }

    if (!Value.ComPort2.empty())
    {
        Result["ComPort2"] = Value.ComPort2;
    }

    return Result;
}

NanaBox::ComPortsConfiguration NanaBox::ToComPortsConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::ComPortsConfiguration Result;

    Result.UefiConsole = NanaBox::ToUefiConsoleMode(
        Mile::Json::GetSubKey(Value, "UefiConsole"));

    Result.ComPort1 = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "ComPort1"),
        Result.ComPort1);

    Result.ComPort2 = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "ComPort2"),
        Result.ComPort2);

    return Result;
}

nlohmann::json NanaBox::FromGpuConfiguration(
    NanaBox::GpuConfiguration const& Value)
{
    nlohmann::json Result;

    Result["AssignmentMode"] = NanaBox::FromGpuAssignmentMode(
        Value.AssignmentMode);

    if (Value.EnableHostDriverStore)
    {
        Result["EnableHostDriverStore"] = Value.EnableHostDriverStore;
    }

    if (!Value.SelectedDevices.empty())
    {
        nlohmann::json SelectedDevices;
        for (std::pair<std::string, std::uint16_t> const& SelectedDevice
            : Value.SelectedDevices)
        {
            if (0xFFFF == SelectedDevice.second)
            {
                SelectedDevices.push_back(SelectedDevice.first);
            }
            else
            {
                nlohmann::json Current;
                Current["DeviceInterface"] = SelectedDevice.first;
                Current["PartitionId"] = SelectedDevice.second;
                SelectedDevices.push_back(Current);
            }
        }
        Result["SelectedDevices"] = SelectedDevices;
    }

    return Result;
}

NanaBox::GpuConfiguration NanaBox::ToGpuConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::GpuConfiguration Result;

    Result.AssignmentMode = NanaBox::ToGpuAssignmentMode(
        Mile::Json::GetSubKey(Value, "AssignmentMode"));

    Result.EnableHostDriverStore = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "EnableHostDriverStore"),
        Result.EnableHostDriverStore);

    for (nlohmann::json const& SelectedDevice : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "SelectedDevices")))
    {
        std::string DeviceInterface = Mile::Json::ToString(SelectedDevice);
        if (!DeviceInterface.empty())
        {
            Result.SelectedDevices[DeviceInterface] = 0xFFFF;
        }
        else
        {
            DeviceInterface = Mile::Json::ToString(
                Mile::Json::GetSubKey(SelectedDevice, "DeviceInterface"));
            if (!DeviceInterface.empty())
            {
                Result.SelectedDevices[DeviceInterface] =
                    static_cast<std::uint16_t>(
                        Mile::Json::ToUInt64(Mile::Json::GetSubKey(
                            SelectedDevice, "PartitionId")));
            }
        }
    }

    if (Result.SelectedDevices.empty() &&
        Result.AssignmentMode == NanaBox::GpuAssignmentMode::List)
    {
        Result.AssignmentMode = NanaBox::GpuAssignmentMode::Disabled;
    }

    if (Result.AssignmentMode != NanaBox::GpuAssignmentMode::List)
    {
        Result.SelectedDevices.clear();
    }

    return Result;
}

nlohmann::json NanaBox::FromNetworkAdapterConfiguration(
    NanaBox::NetworkAdapterConfiguration const& Value)
{
    nlohmann::json Result;

    Result["Connected"] = Value.Connected;

    if (!Value.MacAddress.empty())
    {
        Result["MacAddress"] = Value.MacAddress;
    }

    if (!Value.EndpointId.empty())
    {
        Result["EndpointId"] = Value.EndpointId;
    }

    if (!Value.NetworkId.empty())
    {
        Result["NetworkId"] = Value.NetworkId;
    }

    if (!Value.FirewallCreatorId.empty())
    {
        Result["FirewallCreatorId"] = Value.FirewallCreatorId;
    }

    if (!Value.SuggestedSwitchId.empty())
    {
        Result["SuggestedSwitchId"] = Value.SuggestedSwitchId;
    }

    if  (!Value.SuggestedSwitchName.empty())
    {
        Result["SuggestedSwitchName"] = Value.SuggestedSwitchName;
    }

    if (!Value.SuggestedSwitchSubnet.empty())
    {
        Result["SuggestedSwitchSubnet"] = Value.SuggestedSwitchSubnet;
    }

    return Result;
}

NanaBox::NetworkAdapterConfiguration NanaBox::ToNetworkAdapterConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::NetworkAdapterConfiguration Result;

    Result.Connected = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "Connected"),
        Result.Connected);

    Result.MacAddress = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "MacAddress"),
        Result.MacAddress);

    Result.EndpointId = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "EndpointId"),
        Result.EndpointId);

    Result.NetworkId = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "NetworkId"));

    Result.FirewallCreatorId = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "FirewallCreatorId"));

    Result.SuggestedSwitchId = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "SuggestedSwitchId"),
        Result.SuggestedSwitchId);

    Result.SuggestedSwitchName = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "SuggestedSwitchName"),
        Result.SuggestedSwitchName);

    Result.SuggestedSwitchSubnet = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "SuggestedSwitchSubnet"),
        Result.SuggestedSwitchSubnet);

    return Result;
}

nlohmann::json NanaBox::FromScsiDeviceConfiguration(
    NanaBox::ScsiDeviceConfiguration const& Value)
{
    nlohmann::json Result;

    Result["Type"] = NanaBox::FromScsiDeviceType(Value.Type);

    if (!Value.Path.empty())
    {
        Result["Path"] = Value.Path;
    }

    return Result;
}

NanaBox::ScsiDeviceConfiguration NanaBox::ToScsiDeviceConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::ScsiDeviceConfiguration Result;

    Result.Type = NanaBox::ToScsiDeviceType(
        Mile::Json::GetSubKey(Value, "Type"));

    Result.Path = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Path"),
        Result.Path);

    return Result;
}

nlohmann::json NanaBox::FromKeyboardConfiguration(
    NanaBox::KeyboardConfiguration const& Value)
{
    nlohmann::json Result;

    if (!Value.RedirectKeyCombinations)
    {
        Result["RedirectKeyCombinations"] = false;
    }

    if (Value.ClipboardKeyboardLayout != "00000409")
        Result["ClipboardKeyboardLayout"] = Value.ClipboardKeyboardLayout;

    if (VK_CANCEL != Value.FullScreenHotkey)
    {
        Result["FullScreenHotkey"] = Value.FullScreenHotkey;
    }

    if (VK_HOME != Value.CtrlEscHotkey)
    {
        Result["CtrlEscHotkey"] = Value.CtrlEscHotkey;
    }

    if (VK_INSERT != Value.AltEscHotkey)
    {
        Result["AltEscHotkey"] = Value.AltEscHotkey;
    }

    if (VK_PRIOR != Value.AltTabHotkey)
    {
        Result["AltTabHotkey"] = Value.AltTabHotkey;
    }

    if (VK_NEXT != Value.AltShiftTabHotkey)
    {
        Result["AltShiftTabHotkey"] = Value.AltShiftTabHotkey;
    }

    if (VK_DELETE != Value.AltSpaceHotkey)
    {
        Result["AltSpaceHotkey"] = Value.AltSpaceHotkey;
    }

    if (VK_END != Value.CtrlAltDelHotkey)
    {
        Result["CtrlAltDelHotkey"] = Value.CtrlAltDelHotkey;
    }

    if (VK_LEFT != Value.FocusReleaseLeftHotkey)
    {
        Result["FocusReleaseLeftHotkey"] = Value.FocusReleaseLeftHotkey;
    }

    if (VK_RIGHT != Value.FocusReleaseRightHotkey)
    {
        Result["FocusReleaseRightHotkey"] = Value.FocusReleaseRightHotkey;
    }

    return Result;
}

NanaBox::KeyboardConfiguration NanaBox::ToKeyboardConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::KeyboardConfiguration Result;

    Result.RedirectKeyCombinations = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectKeyCombinations"),
        Result.RedirectKeyCombinations);

    Result.ClipboardKeyboardLayout = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "ClipboardKeyboardLayout"), Result.ClipboardKeyboardLayout);

    Result.FullScreenHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "FullScreenHotkey"),
            Result.FullScreenHotkey));

    Result.CtrlEscHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "CtrlEscHotkey"),
            Result.CtrlEscHotkey));

    Result.AltEscHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "AltEscHotkey"),
            Result.AltEscHotkey));

    Result.AltTabHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "AltTabHotkey"),
            Result.AltTabHotkey));

    Result.AltShiftTabHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "AltShiftTabHotkey"),
            Result.AltShiftTabHotkey));

    Result.AltSpaceHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "AltSpaceHotkey"),
            Result.AltSpaceHotkey));

    Result.CtrlAltDelHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "CtrlAltDelHotkey"),
            Result.CtrlAltDelHotkey));

    Result.FocusReleaseLeftHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "FocusReleaseLeftHotkey"),
            Result.FocusReleaseLeftHotkey));

    Result.FocusReleaseRightHotkey =
        static_cast<std::int32_t>(Mile::Json::ToInt64(
            Mile::Json::GetSubKey(Value, "FocusReleaseRightHotkey"),
            Result.FocusReleaseRightHotkey));

    return Result;
}

nlohmann::json NanaBox::FromEnhancedSessionConfiguration(
    NanaBox::EnhancedSessionConfiguration const& Value)
{
    nlohmann::json Result;

    if (!Value.RedirectAudio)
    {
        Result["RedirectAudio"] = false;
    }

    if (Value.RedirectAudioCapture)
    {
        Result["RedirectAudioCapture"] = true;
    }

    if (Value.RedirectDrives)
    {
        Result["RedirectDrives"] = true;
    }

    if (Value.RedirectPrinters)
    {
        Result["RedirectPrinters"] = true;
    }

    if (Value.RedirectPorts)
    {
        Result["RedirectPorts"] = true;
    }

    if (Value.RedirectSmartCards)
    {
        Result["RedirectSmartCards"] = true;
    }

    if (!Value.RedirectClipboard)
    {
        Result["RedirectClipboard"] = false;
    }

    if (Value.RedirectDevices)
    {
        Result["RedirectDevices"] = true;
    }

    if (Value.RedirectPOSDevices)
    {
        Result["RedirectPOSDevices"] = true;
    }

    if (Value.RedirectDynamicDrives)
    {
        Result["RedirectDynamicDrives"] = true;
    }

    if (Value.RedirectDynamicDevices)
    {
        Result["RedirectDynamicDevices"] = true;
    }

    if (!Value.Drives.empty())
    {
        nlohmann::json Drives;
        for (std::string const& Drive : Value.Drives)
        {
            std::string DriveString = Drive;
            DriveString.resize(1);
            DriveString[0] = static_cast<char>(std::toupper(DriveString[0]));

            if (DriveString[0] < 'A' || DriveString[0] > 'Z')
            {
                continue;
            }

            Drives.push_back(DriveString);
        }
        Result["Drives"] = Drives;
    }

    if (!Value.Devices.empty())
    {
        nlohmann::json Devices;
        for (std::string const& Device : Value.Devices)
        {
            Devices.push_back(Device);
        }
        Result["Devices"] = Devices;
    }

    return Result;
}

NanaBox::EnhancedSessionConfiguration NanaBox::ToEnhancedSessionConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::EnhancedSessionConfiguration Result;

    Result.RedirectAudio = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectAudio"),
        Result.RedirectAudio);

    Result.RedirectAudioCapture = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectAudioCapture"),
        Result.RedirectAudioCapture);

    Result.RedirectDrives = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectDrives"),
        Result.RedirectDrives);

    Result.RedirectPrinters = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectPrinters"),
        Result.RedirectPrinters);

    Result.RedirectPorts = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectPorts"),
        Result.RedirectPorts);

    Result.RedirectSmartCards = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectSmartCards"),
        Result.RedirectSmartCards);

    Result.RedirectClipboard = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectClipboard"),
        Result.RedirectClipboard);

    Result.RedirectDevices = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectDevices"),
        Result.RedirectDevices);

    Result.RedirectPOSDevices = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectPOSDevices"),
        Result.RedirectPOSDevices);

    Result.RedirectDynamicDrives = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectDynamicDrives"),
        Result.RedirectDynamicDrives);

    Result.RedirectDynamicDevices = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "RedirectDynamicDevices"),
        Result.RedirectDynamicDevices);

    for (nlohmann::json const& Drive : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "Drives")))
    {
        std::string DriveString = Mile::Json::ToString(Drive);
        DriveString.resize(1);
        DriveString[0] = static_cast<char>(std::toupper(DriveString[0]));

        if (DriveString[0] < 'A' || DriveString[0] > 'Z')
        {
            continue;
        }

        Result.Drives.push_back(DriveString);
    }

    for (nlohmann::json const& Device : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "Devices")))
    {
        std::string DeviceString = Mile::Json::ToString(Device);
        if (!DeviceString.empty())
        {
            Result.Devices.push_back(DeviceString);
        }
    }

    return Result;
}

nlohmann::json NanaBox::FromChipsetInformationConfiguration(
    NanaBox::ChipsetInformationConfiguration const& Value)
{
    nlohmann::json Result;

    if (!Value.BaseBoardSerialNumber.empty())
    {
        Result["BaseBoardSerialNumber"] = Value.BaseBoardSerialNumber;
    }

    if (!Value.ChassisSerialNumber.empty())
    {
        Result["ChassisSerialNumber"] = Value.ChassisSerialNumber;
    }

    if (!Value.ChassisAssetTag.empty())
    {
        Result["ChassisAssetTag"] = Value.ChassisAssetTag;
    }

    if (!Value.Manufacturer.empty())
    {
        Result["Manufacturer"] = Value.Manufacturer;
    }

    if (!Value.ProductName.empty())
    {
        Result["ProductName"] = Value.ProductName;
    }

    if (!Value.Version.empty())
    {
        Result["Version"] = Value.Version;
    }

    if (!Value.SerialNumber.empty())
    {
        Result["SerialNumber"] = Value.SerialNumber;
    }

    if (!Value.UUID.empty())
    {
        Result["UUID"] = Value.UUID;
    }

    if (!Value.SKUNumber.empty())
    {
        Result["SKUNumber"] = Value.SKUNumber;
    }

    if (!Value.Family.empty())
    {
        Result["Family"] = Value.Family;
    }

    return Result;
}

NanaBox::ChipsetInformationConfiguration NanaBox::ToChipsetInformationConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::ChipsetInformationConfiguration Result;

    Result.BaseBoardSerialNumber = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "BaseBoardSerialNumber"),
        Result.BaseBoardSerialNumber);

    Result.ChassisSerialNumber = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "ChassisSerialNumber"),
        Result.ChassisSerialNumber);

    Result.ChassisAssetTag = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "ChassisAssetTag"),
        Result.ChassisAssetTag);

    Result.Manufacturer = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Manufacturer"),
        Result.Manufacturer);

    Result.ProductName = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "ProductName"),
        Result.ProductName);

    Result.Version = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Version"),
        Result.Version);

    Result.SerialNumber = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "SerialNumber"),
        Result.SerialNumber);

    Result.UUID = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "UUID"),
        Result.UUID);

    Result.SKUNumber = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "SKUNumber"),
        Result.SKUNumber);

    Result.Family = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Family"),
        Result.Family);

    return Result;
}

nlohmann::json NanaBox::FromVideoMonitorConfiguration(
    NanaBox::VideoMonitorConfiguration const& Value)
{
    nlohmann::json Result;

    if (1024 != Value.HorizontalResolution)
    {
        Result["HorizontalResolution"] = Value.HorizontalResolution;
    }

    if (768 != Value.VerticalResolution)
    {
        Result["VerticalResolution"] = Value.VerticalResolution;
    }

    if (Value.DisableBasicSessionDpiScaling)
    {
        Result["DisableBasicSessionDpiScaling"] = true;
    }
    if (Value.EnableDpiScalingValueOverride) Result["EnableDpiScalingValueOverride"] = true;
    if (!Value.EnableContentResizing) Result["EnableContentResizing"] = false;
    if (!Value.ShowFullScreenModeConnectionBar) Result["ShowFullScreenModeConnectionBar"] = false;
    if (Value.OverriddenDpiScalingValue != 100) Result["OverriddenDpiScalingValue"] = Value.OverriddenDpiScalingValue;

    return Result;
}

NanaBox::VideoMonitorConfiguration NanaBox::ToVideoMonitorConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::VideoMonitorConfiguration Result;

    Result.HorizontalResolution =
        static_cast<std::uint16_t>(Mile::Json::ToUInt64(
            Mile::Json::GetSubKey(Value, "HorizontalResolution"),
            Result.HorizontalResolution));

    Result.VerticalResolution =
        static_cast<std::uint16_t>(Mile::Json::ToUInt64(
            Mile::Json::GetSubKey(Value, "VerticalResolution"),
            Result.VerticalResolution));

    Result.DisableBasicSessionDpiScaling = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "DisableBasicSessionDpiScaling"),
        Result.DisableBasicSessionDpiScaling);
    Result.EnableDpiScalingValueOverride = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "EnableDpiScalingValueOverride"), Result.EnableDpiScalingValueOverride);
    Result.EnableContentResizing = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "EnableContentResizing"), Result.EnableContentResizing);
    Result.ShowFullScreenModeConnectionBar = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "ShowFullScreenModeConnectionBar"), Result.ShowFullScreenModeConnectionBar);
    auto Scale = Mile::Json::ToUInt64(Mile::Json::GetSubKey(Value, "OverriddenDpiScalingValue"), Result.OverriddenDpiScalingValue);
    if (Scale < 100 || Scale > 500) throw std::invalid_argument("Display scaling must be between 100 and 500 percent");
    Result.OverriddenDpiScalingValue = static_cast<std::uint32_t>(Scale);

    return Result;
}

nlohmann::json NanaBox::FromPlan9ShareConfiguration(
    Plan9ShareConfiguration const& Value)
{
    nlohmann::json Result;

    if (Value.ReadOnly)
    {
        Result["ReadOnly"] = true;
    }

    if (!Value.AutoMount) Result["AutoMount"] = false;
    if (Value.LinuxMetadata) Result["LinuxMetadata"] = true;
    if (Value.CaseSensitive) Result["CaseSensitive"] = true;
    if (Value.UseShareRootIdentity) Result["UseShareRootIdentity"] = true;
    if (Value.AllowOptions) Result["AllowOptions"] = true;
    if (Value.AllowSubPaths) Result["AllowSubPaths"] = true;
    if (Value.UnlimitedConnections) Result["UnlimitedConnections"] = true;
    if (!Value.AllowedFiles.empty()) Result["AllowedFiles"] = Value.AllowedFiles;
    Result["Port"] = Value.Port;

    if (!Value.Path.empty())
    {
        Result["Path"] = Value.Path;
    }

    if (!Value.Name.empty())
    {
        Result["Name"] = Value.Name;
    }

    return Result;
}

NanaBox::Plan9ShareConfiguration NanaBox::ToPlan9ShareConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::Plan9ShareConfiguration Result;

    Result.ReadOnly = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "ReadOnly"),
        Result.ReadOnly);

    if (!Value.is_object()) throw std::invalid_argument("Invalid Plan9 share");
    if (Value.contains("AutoMount"))
    {
        if (!Value.at("AutoMount").is_boolean()) throw std::invalid_argument("AutoMount must be Boolean");
        Result.AutoMount = Value.at("AutoMount").get<bool>();
    }
    if (Value.contains("LinuxMetadata"))
    {
        if (!Value.at("LinuxMetadata").is_boolean()) throw std::invalid_argument("LinuxMetadata must be Boolean");
        Result.LinuxMetadata = Value.at("LinuxMetadata").get<bool>();
    }
    if (Value.contains("CaseSensitive"))
    {
        if (!Value.at("CaseSensitive").is_boolean()) throw std::invalid_argument("CaseSensitive must be Boolean");
        Result.CaseSensitive = Value.at("CaseSensitive").get<bool>();
    }
    if (Value.contains("UseShareRootIdentity"))
    {
        if (!Value.at("UseShareRootIdentity").is_boolean()) throw std::invalid_argument("UseShareRootIdentity must be Boolean");
        Result.UseShareRootIdentity = Value.at("UseShareRootIdentity").get<bool>();
    }
    if (Value.contains("AllowOptions"))
    {
        if (!Value.at("AllowOptions").is_boolean()) throw std::invalid_argument("AllowOptions must be Boolean");
        Result.AllowOptions = Value.at("AllowOptions").get<bool>();
    }
    if (Value.contains("AllowSubPaths"))
    {
        if (!Value.at("AllowSubPaths").is_boolean()) throw std::invalid_argument("AllowSubPaths must be Boolean");
        Result.AllowSubPaths = Value.at("AllowSubPaths").get<bool>();
    }
    if (Value.contains("UnlimitedConnections"))
    {
        if (!Value.at("UnlimitedConnections").is_boolean()) throw std::invalid_argument("UnlimitedConnections must be Boolean");
        Result.UnlimitedConnections = Value.at("UnlimitedConnections").get<bool>();
    }
    if (Value.contains("Port"))
    {
        auto const& Port = Value.at("Port");
        if (!Port.is_number_integer() || Port < 1 || Port >= UINT32_MAX)
            throw std::invalid_argument("Invalid Plan9 port");
        Result.Port = Port.get<std::uint32_t>();
    }
    if (Value.contains("AllowedFiles"))
    {
        auto const& Files = Value.at("AllowedFiles");
        if (!Files.is_array()) throw std::invalid_argument("AllowedFiles must be an array");
        Result.AllowedFiles = Files.get<std::vector<std::string>>();
    }

    Result.Path = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Path"),
        Result.Path);

    Result.Name = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Name"),
        Result.Name);

    return Result;
}

nlohmann::json NanaBox::FromVirtualSmbShareConfiguration(
    VirtualSmbShareConfiguration const& Value)
{
    nlohmann::json Result;

    if (Value.ReadOnly)
    {
        Result["ReadOnly"] = true;
    }

    if (!Value.Path.empty())
    {
        Result["Path"] = Value.Path;
    }

    if (!Value.Name.empty())
    {
        Result["Name"] = Value.Name;
    }

    return Result;
}

NanaBox::VirtualSmbShareConfiguration NanaBox::ToVirtualSmbShareConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::VirtualSmbShareConfiguration Result;

    Result.ReadOnly = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "ReadOnly"),
        Result.ReadOnly);

    Result.Path = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Path"),
        Result.Path);

    Result.Name = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Name"),
        Result.Name);

    return Result;
}

nlohmann::json NanaBox::FromVirtualMachineConfiguration(
    NanaBox::VirtualMachineConfiguration const& Value)
{
    nlohmann::json Result;

    Result["Version"] = Value.Version;

    Result["GuestType"] = NanaBox::FromGuestType(Value.GuestType);

    Result["Name"] = Value.Name;

    Result["ProcessorCount"] = Value.ProcessorCount;

    Result["MemorySize"] = Value.MemorySize;
    if (Value.ProcessorLimit) Result["ProcessorLimit"] = Value.ProcessorLimit;
    if (Value.ProcessorWeight) Result["ProcessorWeight"] = Value.ProcessorWeight;
    if (!Value.AllowMemoryOvercommit) Result["AllowMemoryOvercommit"] = false;
    if (Value.EnableDeferredCommit) Result["EnableDeferredCommit"] = true;
    if (Value.StorageIopsMaximum) Result["StorageIopsMaximum"] = Value.StorageIopsMaximum;
    if (Value.StorageBandwidthMaximum) Result["StorageBandwidthMaximum"] = Value.StorageBandwidthMaximum;

    Result["ComPorts"] = NanaBox::FromComPortsConfiguration(Value.ComPorts);

    Result["Gpu"] = NanaBox::FromGpuConfiguration(Value.Gpu);

    if (Value.MirroredNetworking)
    {
        Result["MirroredNetworking"] = true;
    }
    if (!Value.MirroredAutoReservePorts)
        Result["MirroredAutoReservePorts"] = false;
    if (Value.MirroredBindCoordination) Result["MirroredBindCoordination"] = true;
    if (Value.MirroredDnsTunneling) Result["MirroredDnsTunneling"] = true;
    if (Value.MirroredLoopback) Result["MirroredLoopback"] = true;
    if (Value.MirroredHostAddressLoopback) Result["MirroredHostAddressLoopback"] = true;
    if (Value.HcsSchemaMinor != 1) Result["HcsSchemaMinor"] = Value.HcsSchemaMinor;
    if (!Value.AdditionalHcsSettings.empty())
    {
        auto Additional = nlohmann::json::parse(Value.AdditionalHcsSettings);
        if (!Additional.is_object()) throw std::invalid_argument("AdditionalHcsSettings must be a JSON object");
        if (!Additional.empty()) Result["AdditionalHcsSettings"] = std::move(Additional);
    }
    if (!Value.MirroredFirewallCreatorId.empty())
    {
        Result["MirroredFirewallCreatorId"] = Value.MirroredFirewallCreatorId;
    }
    if (!Value.MirroredTcpPorts.empty())
    {
        Result["MirroredTcpPorts"] = Value.MirroredTcpPorts;
    }
    if (!Value.MirroredUdpPorts.empty())
    {
        Result["MirroredUdpPorts"] = Value.MirroredUdpPorts;
    }
    if (!Value.MirroredIgnoredPorts.empty()) Result["MirroredIgnoredPorts"] = Value.MirroredIgnoredPorts;

    if (!Value.NetworkAdapters.empty())
    {
        nlohmann::json NetworkAdapters;
        for (NanaBox::NetworkAdapterConfiguration const& NetworkAdapter
            : Value.NetworkAdapters)
        {
            NetworkAdapters.push_back(
                NanaBox::FromNetworkAdapterConfiguration(NetworkAdapter));
        }
        Result["NetworkAdapters"] = NetworkAdapters;
    }

    if (!Value.ScsiDevices.empty())
    {
        nlohmann::json ScsiDevices;
        for (NanaBox::ScsiDeviceConfiguration const& ScsiDevice
            : Value.ScsiDevices)
        {
            if (NanaBox::ScsiDeviceType::Unknown == ScsiDevice.Type)
            {
                continue;
            }

            ScsiDevices.push_back(
                NanaBox::FromScsiDeviceConfiguration(ScsiDevice));
        }
        Result["ScsiDevices"] = ScsiDevices;
    }

    if (Value.SecureBoot)
    {
        Result["SecureBoot"] = Value.SecureBoot;
    }

    if (Value.Tpm)
    {
        Result["Tpm"] = Value.Tpm;
    }

    if (!Value.GuestStateFile.empty())
    {
        Result["GuestStateFile"] = Value.GuestStateFile;
    }

    if (!Value.RuntimeStateFile.empty())
    {
        Result["RuntimeStateFile"] = Value.RuntimeStateFile;
    }

    if (!Value.SaveStateFile.empty())
    {
        Result["SaveStateFile"] = Value.SaveStateFile;
    }

    if (Value.ExposeVirtualizationExtensions)
    {
        Result["ExposeVirtualizationExtensions"] =
            Value.ExposeVirtualizationExtensions;
    }

    {
        nlohmann::json Keyboard =
            NanaBox::FromKeyboardConfiguration(Value.Keyboard);
        if (!Keyboard.empty())
        {
            Result["Keyboard"] = Keyboard;
        }
    }

    {
        nlohmann::json EnhancedSession =
            NanaBox::FromEnhancedSessionConfiguration(Value.EnhancedSession);
        if (!EnhancedSession.empty())
        {
            Result["EnhancedSession"] = EnhancedSession;
        }
    }

    {
        nlohmann::json ChipsetInformation =
            NanaBox::FromChipsetInformationConfiguration(
                Value.ChipsetInformation);
        if (!ChipsetInformation.empty())
        {
            Result["ChipsetInformation"] = ChipsetInformation;
        }
    }

    {
        nlohmann::json VideoMonitor =
            NanaBox::FromVideoMonitorConfiguration(Value.VideoMonitor);
        if (!VideoMonitor.empty())
        {
            Result["VideoMonitor"] = VideoMonitor;
        }
    }

    if (!Value.Policies.empty())
    {
        nlohmann::json Policies;
        for (std::string const& Policy : Value.Policies)
        {
            Policies.push_back(Policy);
        }
        Result["Policies"] = Policies;
    }

    if (!Value.Plan9Shares.empty())
    {
        nlohmann::json Plan9Shares;
        for (NanaBox::Plan9ShareConfiguration const& Plan9Share
            : Value.Plan9Shares)
        {
            if (Plan9Share.Path.empty() || Plan9Share.Name.empty())
                throw std::invalid_argument("Plan9 share requires a path and name");

            Plan9Shares.push_back(
                NanaBox::FromPlan9ShareConfiguration(Plan9Share));
        }
        Result["Plan9Shares"] = Plan9Shares;
    }

    if (!Value.VirtualSmbShares.empty())
    {
        nlohmann::json VirtualSmbShares;
        for (NanaBox::VirtualSmbShareConfiguration const& VirtualSmbShare
            : Value.VirtualSmbShares)
        {
            if (VirtualSmbShare.Path.empty() || VirtualSmbShare.Name.empty())
            {
                continue;
            }

            VirtualSmbShares.push_back(
                NanaBox::FromVirtualSmbShareConfiguration(VirtualSmbShare));
        }
        Result["VirtualSmbShares"] = VirtualSmbShares;
    }

    return Result;
}

NanaBox::VirtualMachineConfiguration NanaBox::ToVirtualMachineConfiguration(
    nlohmann::json const& Value)
{
    NanaBox::VirtualMachineConfiguration Result;

    Result.Version = static_cast<std::uint32_t>(Mile::Json::ToUInt64(
        Mile::Json::GetSubKey(Value, "Version")));

    Result.GuestType = NanaBox::ToGuestType(
        Mile::Json::GetSubKey(Value, "GuestType"));

    Result.Name = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "Name"),
        Result.Name);

    Result.ProcessorCount = static_cast<std::uint32_t>(Mile::Json::ToUInt64(
        Mile::Json::GetSubKey(Value, "ProcessorCount")));

    Result.MemorySize = Mile::Json::ToUInt64(
        Mile::Json::GetSubKey(Value, "MemorySize"));

    auto ReadUnsigned = [&](char const* Name, std::uint64_t Maximum) -> std::uint64_t
    {
        if (!Value.contains(Name)) return 0;
        auto const& Field = Value.at(Name);
        if (!Field.is_number_unsigned() &&
            (!Field.is_number_integer() || Field.get<std::int64_t>() < 0))
            throw std::invalid_argument(std::string("Invalid integer: ") + Name);
        auto Number = Field.get<std::uint64_t>();
        if (Number > Maximum) throw std::invalid_argument(std::string("Out of range: ") + Name);
        return Number;
    };
    auto ReadBoolean = [&](char const* Name, bool Default) -> bool
    {
        if (!Value.contains(Name)) return Default;
        auto const& Field = Value.at(Name);
        if (!Field.is_boolean()) throw std::invalid_argument(std::string("Invalid boolean: ") + Name);
        return Field.get<bool>();
    };
    Result.ProcessorLimit = ReadUnsigned("ProcessorLimit", 100000);
    Result.ProcessorWeight = ReadUnsigned("ProcessorWeight", 10000);
    Result.AllowMemoryOvercommit = ReadBoolean("AllowMemoryOvercommit", true);
    Result.EnableDeferredCommit = ReadBoolean("EnableDeferredCommit", false);
    Result.StorageIopsMaximum = ReadUnsigned("StorageIopsMaximum", UINT64_MAX);
    Result.StorageBandwidthMaximum = ReadUnsigned("StorageBandwidthMaximum", UINT64_MAX);
    if (Result.EnableDeferredCommit && !Result.AllowMemoryOvercommit)
        throw std::invalid_argument("EnableDeferredCommit requires AllowMemoryOvercommit");

    Result.ComPorts = NanaBox::ToComPortsConfiguration(
        Mile::Json::GetSubKey(Value, "ComPorts"));

    Result.Gpu = NanaBox::ToGpuConfiguration(
        Mile::Json::GetSubKey(Value, "Gpu"));

    Result.MirroredNetworking = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "MirroredNetworking"));
    Result.MirroredAutoReservePorts = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "MirroredAutoReservePorts"), Result.MirroredAutoReservePorts);
    Result.MirroredBindCoordination = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "MirroredBindCoordination"));
    Result.MirroredDnsTunneling = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "MirroredDnsTunneling"));
    Result.MirroredLoopback = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "MirroredLoopback"));
    Result.MirroredHostAddressLoopback = Mile::Json::ToBoolean(Mile::Json::GetSubKey(Value, "MirroredHostAddressLoopback"));
    if (Value.contains("HcsSchemaMinor"))
    {
        auto const& Minor = Value.at("HcsSchemaMinor");
        if (!Minor.is_number_integer() || Minor < 1 || Minor > 65535)
            throw std::invalid_argument("HcsSchemaMinor must be an integer from 1 through 65535");
        Result.HcsSchemaMinor = Minor.get<std::uint32_t>();
    }
    if (Value.contains("AdditionalHcsSettings"))
    {
        auto const& Additional = Value.at("AdditionalHcsSettings");
        if (!Additional.is_object()) throw std::invalid_argument("AdditionalHcsSettings must be a JSON object");
        if (!Additional.empty()) Result.AdditionalHcsSettings = Additional.dump(2);
    }
    Result.MirroredFirewallCreatorId = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "MirroredFirewallCreatorId"));
    for (auto const& Port : Mile::Json::ToArray(Mile::Json::GetSubKey(Value, "MirroredTcpPorts")))
        Result.MirroredTcpPorts.push_back(Mile::Json::ToString(Port));
    for (auto const& Port : Mile::Json::ToArray(Mile::Json::GetSubKey(Value, "MirroredUdpPorts")))
        Result.MirroredUdpPorts.push_back(Mile::Json::ToString(Port));
    for (auto const& Port : Mile::Json::ToArray(Mile::Json::GetSubKey(Value, "MirroredIgnoredPorts")))
        Result.MirroredIgnoredPorts.push_back(Mile::Json::ToString(Port));

    for (nlohmann::json const& NetworkAdapter : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "NetworkAdapters")))
    {
        Result.NetworkAdapters.push_back(
            NanaBox::ToNetworkAdapterConfiguration(NetworkAdapter));
    }

    for (nlohmann::json const& ScsiDevice : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "ScsiDevices")))
    {
        NanaBox::ScsiDeviceConfiguration Current =
            NanaBox::ToScsiDeviceConfiguration(ScsiDevice);

        if (NanaBox::ScsiDeviceType::Unknown == Current.Type)
        {
            continue;
        }

        if (Current.Path.empty() &&
            Current.Type != NanaBox::ScsiDeviceType::VirtualImage)
        {
            continue;
        }

        Result.ScsiDevices.push_back(Current);
    }

    Result.SecureBoot = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "SecureBoot"),
        Result.SecureBoot);

    Result.Tpm = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "Tpm"),
        Result.Tpm);

    Result.GuestStateFile = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "GuestStateFile"),
        Result.GuestStateFile);

    Result.RuntimeStateFile = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "RuntimeStateFile"),
        Result.RuntimeStateFile);

    Result.SaveStateFile = Mile::Json::ToString(
        Mile::Json::GetSubKey(Value, "SaveStateFile"),
        Result.SaveStateFile);

    Result.ExposeVirtualizationExtensions = Mile::Json::ToBoolean(
        Mile::Json::GetSubKey(Value, "ExposeVirtualizationExtensions"),
        Result.ExposeVirtualizationExtensions);

    Result.Keyboard = NanaBox::ToKeyboardConfiguration(
        Mile::Json::GetSubKey(Value, "Keyboard"));

    Result.EnhancedSession = NanaBox::ToEnhancedSessionConfiguration(
        Mile::Json::GetSubKey(Value, "EnhancedSession"));

    Result.ChipsetInformation = NanaBox::ToChipsetInformationConfiguration(
        Mile::Json::GetSubKey(Value, "ChipsetInformation"));

    Result.VideoMonitor = NanaBox::ToVideoMonitorConfiguration(
        Mile::Json::GetSubKey(Value, "VideoMonitor"));

    for (nlohmann::json const& Policy : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "Policies")))
    {
        std::string PolicyString = Mile::Json::ToString(Policy);
        if (!PolicyString.empty())
        {
            Result.Policies.push_back(PolicyString);
        }
    }

    for (nlohmann::json const& Plan9Share : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "Plan9Shares")))
    {
        NanaBox::Plan9ShareConfiguration Current =
            NanaBox::ToPlan9ShareConfiguration(Plan9Share);

        if (Current.Path.empty() || Current.Name.empty())
            throw std::invalid_argument("Plan9 share requires a path and name");

        Result.Plan9Shares.push_back(Current);
    }

    for (nlohmann::json const& VirtualSmbShare : Mile::Json::ToArray(
        Mile::Json::GetSubKey(Value, "VirtualSmbShares")))
    {
        NanaBox::VirtualSmbShareConfiguration Current =
            NanaBox::ToVirtualSmbShareConfiguration(VirtualSmbShare);

        if (Current.Path.empty() || Current.Name.empty())
        {
            continue;
        }

        Result.VirtualSmbShares.push_back(Current);
    }

    return Result;
}
