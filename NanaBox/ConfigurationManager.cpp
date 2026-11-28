/*
 * PROJECT:    NanaBox
 * FILE:       ConfigurationManager.cpp
 * PURPOSE:    Implementation for the Virtual Machine Configuration Manager
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#include "ConfigurationManager.h"

#include "Utils.h"
#include "KeyboardText.h"

#include <Mile.Helpers.Base.h>

#include <NanaBox.Configuration.Parser.h>

#include <Shlwapi.h>
#include <set>
#include <algorithm>
#include <cctype>

namespace NanaBox
{
    namespace ResolutionType
    {
        enum
        {
            Unspecified = 0,
            Maximum = 2,
            Single = 3,
            Default = 4
        };
    }
}

void NanaBox::ValidateScsiDeviceConfiguration(
    winrt::hstring const& HcsVmId,
    NanaBox::ScsiDeviceConfiguration const& Configuration)
{
    std::wstring Path = Mile::ToWideString(CP_UTF8, Configuration.Path);
    if (NanaBox::ScsiDeviceType::PhysicalDevice != Configuration.Type)
    {
        Path = ::GetAbsolutePath(Path);
        if (!::PathFileExistsW(Path.c_str()))
        {
            return;
        }
    }
    winrt::check_hresult(::HcsGrantVmAccess(HcsVmId.c_str(), Path.c_str()));
}

nlohmann::json NanaBox::MakeHcsComPortConfiguration(
    std::string const& NamedPipe)
{
    nlohmann::json Result;

    Result["NamedPipe"] = NamedPipe;

    return Result;
}

nlohmann::json NanaBox::MakeHcsNetworkAdapterConfiguration(
    NanaBox::NetworkAdapterConfiguration const& Configuration)
{
    nlohmann::json Result;

    if (!Configuration.Connected)
    {
        Result["ConnectionState"] = "Disabled";
    }
    Result["EndpointId"] = Configuration.EndpointId;
    Result["MacAddress"] = Configuration.MacAddress;

    return Result;
}

nlohmann::json NanaBox::MakeHcsScsiDeviceConfiguration(
    NanaBox::ScsiDeviceConfiguration const& Configuration)
{
    nlohmann::json Result;

    switch (Configuration.Type)
    {
    case NanaBox::ScsiDeviceType::VirtualDisk:
    {
        Result["Type"] = "VirtualDisk";
        break;
    }
    case NanaBox::ScsiDeviceType::VirtualImage:
    {
        Result["Type"] = "Iso";
        break;
    }
    case NanaBox::ScsiDeviceType::PhysicalDevice:
    {
        Result["Type"] = "PassThru";
        break;
    }
    default:
        Result["Type"] = "";
        break;
    }
    Result["Path"] = ::GetAbsoluteUtf8Path(Configuration.Path);

    return Result;
}

namespace
{
    std::string LowerHcsKey(std::string Key)
    {
        std::transform(Key.begin(), Key.end(), Key.begin(),
            [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
        return Key;
    }

    void MergeHcsSettings(nlohmann::json& Target, nlohmann::json const& Additional,
        std::string const& Path, unsigned Depth = 0)
    {
        if (!Additional.is_object() || Depth > 32)
            throw std::invalid_argument("Invalid HCS settings object or nesting depth: " + Path);
        // Keep a single source of truth for settings that have dedicated
        // controls, runtime updates or host-side resource provisioning.
        static const std::map<std::string, std::set<std::string>> Managed = {
            {"virtualmachine", {"gueststate", "restorestate"}},
            {"virtualmachine/chipset", {"baseboardserialnumber", "chassisserialnumber", "chassisassettag", "systeminformation"}},
            {"virtualmachine/chipset/uefi", {"console", "applysecureboottemplate", "secureboottemplateid"}},
            {"virtualmachine/computetopology/memory", {"sizeinmb", "allowovercommit", "enabledeferredcommit"}},
            {"virtualmachine/computetopology/processor", {"count", "limit", "weight", "exposevirtualizationextensions"}},
            {"virtualmachine/computetopology/compatibility", {"guestfeatureset"}},
            {"virtualmachine/securitysettings", {"enabletpm", "isolation"}},
            {"virtualmachine/storageqos", {"iopsmaximum", "bandwidthmaximum"}},
            {"virtualmachine/devices", {"videomonitor", "enhancedmodevideo", "keyboard", "mouse", "comports",
                "networkadapters", "scsi", "gpu", "gpupartition", "plan9", "virtualsmb"}}
        };
        std::set<std::string> Seen;
        auto Reserved = Managed.find(LowerHcsKey(Path));
        for (auto It = Additional.begin(); It != Additional.end(); ++It)
        {
            auto Key = LowerHcsKey(It.key());
            auto Child = Path + "/" + It.key();
            if (Key.empty() || !Seen.insert(Key).second ||
                (Reserved != Managed.end() && Reserved->second.count(Key)))
                throw std::invalid_argument("Duplicate or managed HCS property: " + Child);
            auto Existing = Target.end();
            for (auto Current = Target.begin(); Current != Target.end(); ++Current)
            {
                if (LowerHcsKey(Current.key()) == Key)
                {
                    Existing = Current;
                    break;
                }
            }
            if (Existing != Target.end())
            {
                if (Existing.key() != It.key() || !Existing->is_object() || !It->is_object())
                    throw std::invalid_argument("HCS property conflicts with a generated setting: " + Child);
                MergeHcsSettings(*Existing, *It, Child, Depth + 1);
            }
            else if (It->is_object())
            {
                auto Object = nlohmann::json::object();
                MergeHcsSettings(Object, *It, Child, Depth + 1);
                Target[It.key()] = std::move(Object);
            }
            else
                Target[It.key()] = *It;
        }
    }
}

namespace
{
    void CheckShareComponent(std::string const& Name)
    {
        if (Name.empty() || Name.size() > 200 || Name == "." || Name == ".." ||
            Name.front() == ' ' || Name.back() == ' ' || Name.find_first_of("/\\,:") != std::string::npos ||
            std::any_of(Name.begin(), Name.end(), [](unsigned char Ch) { return Ch < 32 || Ch == 127; }))
            throw std::invalid_argument("Invalid Plan9 share or allowed file name: " + Name);
        (void)winrt::to_hstring(Name);
    }

    bool SamePlan9Name(std::string const& A, std::string const& B)
    {
        auto Left = winrt::to_hstring(A), Right = winrt::to_hstring(B);
        return ::CompareStringOrdinal(Left.c_str(), -1, Right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }
}

void NanaBox::ValidatePlan9Shares(VirtualMachineConfiguration const& Configuration)
{
    std::vector<std::string> Names;
    Names = {"HostDriverStore", "NanaBox.HostDrivers", "NanaBox.HostLxssLib"};
    if (Names.size() + Configuration.Plan9Shares.size() > 128)
        throw std::invalid_argument("At most 128 Plan9 shares are supported");
    for (auto const& Share : Configuration.Plan9Shares)
    {
        CheckShareComponent(Share.Name);
        if (Share.Path.empty() || Share.Path.find('\0') != std::string::npos)
            throw std::invalid_argument("Plan9 share requires a valid host path");
        if (!Share.Port || Share.Port == UINT32_MAX || (Share.Port >= 27453 && Share.Port <= 27457))
            throw std::invalid_argument("Invalid or reserved Plan9 port");
        if (::_strnicmp(Share.Name.c_str(), "NanaBox.GpuExtra.", 17) == 0 ||
            std::any_of(Names.begin(), Names.end(), [&](auto const& Name) { return SamePlan9Name(Name, Share.Name); }))
            throw std::invalid_argument("Duplicate or reserved Plan9 share name: " + Share.Name);
        Names.push_back(Share.Name);
        std::vector<std::string> Allowed;
        if (Share.AllowedFiles.size() > 4096) throw std::invalid_argument("Too many allowed Plan9 files");
        for (auto const& File : Share.AllowedFiles)
        {
            CheckShareComponent(File);
            if (std::any_of(Allowed.begin(), Allowed.end(), [&](auto const& Name) { return SamePlan9Name(Name, File); }))
                throw std::invalid_argument("Duplicate allowed Plan9 file: " + File);
            Allowed.push_back(File);
        }
    }
    std::vector<std::string> SmbNames = {"HostDriverStore", "NanaBox.HostDrivers", "NanaBox.HostLxssLib"};
    if (Configuration.VirtualSmbShares.size() > 128)
        throw std::invalid_argument("At most 128 Virtual SMB shares are supported");
    for (auto const& Share : Configuration.VirtualSmbShares)
    {
        CheckShareComponent(Share.Name);
        if (Share.Path.empty() || Share.Path.find('\0') != std::string::npos ||
            ::_strnicmp(Share.Name.c_str(), "NanaBox.GpuExtra.", 17) == 0 ||
            std::any_of(SmbNames.begin(), SmbNames.end(), [&](auto const& Name) { return SamePlan9Name(Name, Share.Name); }))
            throw std::invalid_argument("Invalid, duplicate or reserved Virtual SMB share: " + Share.Name);
        SmbNames.push_back(Share.Name);
    }
}

nlohmann::json NanaBox::MakeHcsPlan9Share(Plan9ShareConfiguration const& Share)
{
    std::uint32_t Flags = Share.ReadOnly ? Plan9ShareFlags::ReadOnly : Plan9ShareFlags::None;
    if (Share.LinuxMetadata) Flags |= Plan9ShareFlags::LinuxMetadata;
    if (Share.CaseSensitive) Flags |= Plan9ShareFlags::CaseSensitive;
    if (Share.UseShareRootIdentity) Flags |= Plan9ShareFlags::UseShareRootIdentity;
    if (Share.AllowOptions) Flags |= Plan9ShareFlags::AllowOptions;
    if (Share.AllowSubPaths) Flags |= Plan9ShareFlags::AllowSubPaths;
    if (Share.UnlimitedConnections) Flags |= Plan9ShareFlags::UnlimitedConnections;
    if (!Share.AllowedFiles.empty()) Flags |= Plan9ShareFlags::RestrictFileAccess;
    nlohmann::json Result = {{"Name", Share.Name}, {"AccessName", Share.Name},
        {"Path", ::GetAbsoluteUtf8Path(Share.Path)}, {"Port", Share.Port}, {"Flags", Flags}};
    if (!Share.AllowedFiles.empty()) Result["AllowedFiles"] = Share.AllowedFiles;
    return Result;
}

bool NanaBox::EqualPlan9ShareName(Plan9ShareConfiguration const& Left, Plan9ShareConfiguration const& Right)
{
    return SamePlan9Name(Left.Name, Right.Name);
}

void NanaBox::ValidatePlan9HostDirectory(Plan9ShareConfiguration const& Share)
{
    auto Path = winrt::to_hstring(::GetAbsoluteUtf8Path(Share.Path));
    auto Attributes = ::GetFileAttributesW(Path.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & FILE_ATTRIBUTE_DIRECTORY))
        throw winrt::hresult_invalid_argument(winrt::to_hstring("Plan9 share directory does not exist: " + Share.Path));
}

nlohmann::json NanaBox::MakeHcsPlan9Modification(Plan9ShareConfiguration const& Share, bool Add)
{
    auto Settings = Add ? MakeHcsPlan9Share(Share) : nlohmann::json{
        {"Name", Share.Name}, {"AccessName", Share.Name}, {"Port", Share.Port}};
    return {{"ResourcePath", "VirtualMachine/Devices/Plan9/Shares"},
        {"RequestType", Add ? "Add" : "Remove"}, {"Settings", std::move(Settings)}};
}

void NanaBox::ComputeSystemModifyPlan9Share(winrt::com_ptr<ComputeSystem> const& Instance,
    Plan9ShareConfiguration const& Share, bool Add)
{
    if (Add) ValidatePlan9HostDirectory(Share);
    Instance->Modify(winrt::to_hstring(MakeHcsPlan9Modification(Share, Add).dump()));
}

nlohmann::json NanaBox::MakePlan9GuestManifest(VirtualMachineConfiguration const& Configuration,
    nlohmann::json const& HcsConfiguration)
{
    auto Result = nlohmann::json::array();
    auto const& Devices = HcsConfiguration.at("VirtualMachine").at("Devices");
    if (!Devices.contains("Plan9")) return Result;
    for (auto const& Share : Devices.at("Plan9").at("Shares"))
    {
        auto Name = Share.at("AccessName").get<std::string>();
        bool AutoMount = true;
        for (auto const& Configured : Configuration.Plan9Shares)
            if (Configured.Name == Name) AutoMount = Configured.AutoMount;
        Result.push_back({{"Name", Name}, {"Port", Share.at("Port")},
            {"ReadOnly", (Share.at("Flags").get<std::uint32_t>() & Plan9ShareFlags::ReadOnly) != 0},
            {"AutoMount", AutoMount}});
    }
    return Result;
}

std::string NanaBox::MakeHcsConfiguration(
    NanaBox::VirtualMachineConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["SchemaVersion"]["Major"] = 2;
    if (!Configuration.HcsSchemaMinor || Configuration.HcsSchemaMinor > 65535)
        throw std::invalid_argument("Invalid HCS schema minor version");
    NanaBox::ValidatePlan9Shares(Configuration);
    if (!NanaBox::ValidClipboardKeyboardLayout(Configuration.Keyboard.ClipboardKeyboardLayout))
        throw std::invalid_argument("Clipboard keyboard layout must be Host or an eight-digit hexadecimal KLID.");
    auto Minor = Configuration.HcsSchemaMinor;
    for (auto const& Share : Configuration.Plan9Shares)
        if (!Share.AllowedFiles.empty()) Minor = (std::max)(Minor, 2u);
    if (Configuration.GuestType == NanaBox::GuestType::Windows && Configuration.Gpu.EnableHostDriverStore)
        Minor = (std::max)(Minor, 2u);
    Result["SchemaVersion"]["Minor"] = Minor;

    Result["Owner"] = Configuration.Name;

    Result["ShouldTerminateOnLastHandleClosed"] = true;

    nlohmann::json VirtualMachine;

    nlohmann::json Chipset;
    {
        nlohmann::json Uefi;
        {
            switch (Configuration.ComPorts.UefiConsole)
            {
            case NanaBox::UefiConsoleMode::Default:
                Uefi["Console"] = "Default";
                break;
            case NanaBox::UefiConsoleMode::ComPort1:
                Uefi["Console"] = "ComPort1";
                break;
            case NanaBox::UefiConsoleMode::ComPort2:
                Uefi["Console"] = "ComPort2";
                break;
            default:
                Uefi["Console"] = "Disabled";
                break;
            }

            if (Configuration.SecureBoot)
            {
                Uefi["ApplySecureBootTemplate"] = "Apply";
                Uefi["SecureBootTemplateId"] =
                    "1734c6e8-3154-4dda-ba5f-a874cc483422";
            }
        }
        Chipset["Uefi"] = Uefi;

        if (!Configuration.ChipsetInformation.BaseBoardSerialNumber.empty())
        {
            Chipset["BaseBoardSerialNumber"] =
                Configuration.ChipsetInformation.BaseBoardSerialNumber;
        }
        if (!Configuration.ChipsetInformation.ChassisSerialNumber.empty())
        {
            Chipset["ChassisSerialNumber"] =
                Configuration.ChipsetInformation.ChassisSerialNumber;
        }
        if (!Configuration.ChipsetInformation.ChassisAssetTag.empty())
        {
            Chipset["ChassisAssetTag"] =
                Configuration.ChipsetInformation.ChassisAssetTag;
        }

        if (::MileIsWindowsVersionAtLeast(10, 0, 20348))
        {
            nlohmann::json SystemInformation;
            {
                if (!Configuration.ChipsetInformation.Manufacturer.empty())
                {
                    SystemInformation["Manufacturer"] =
                        Configuration.ChipsetInformation.Manufacturer;
                }
                if (!Configuration.ChipsetInformation.ProductName.empty())
                {
                    SystemInformation["ProductName"] =
                        Configuration.ChipsetInformation.ProductName;
                }
                if (!Configuration.ChipsetInformation.Version.empty())
                {
                    SystemInformation["Version"] =
                        Configuration.ChipsetInformation.Version;
                }
                if (!Configuration.ChipsetInformation.SerialNumber.empty())
                {
                    SystemInformation["SerialNumber"] =
                        Configuration.ChipsetInformation.SerialNumber;
                }
                if (!Configuration.ChipsetInformation.UUID.empty())
                {
                    SystemInformation["UUID"] =
                        Configuration.ChipsetInformation.UUID;
                }
                if (!Configuration.ChipsetInformation.SKUNumber.empty())
                {
                    SystemInformation["SKUNumber"] =
                        Configuration.ChipsetInformation.SKUNumber;
                }
                if (!Configuration.ChipsetInformation.Family.empty())
                {
                    SystemInformation["Family"] =
                        Configuration.ChipsetInformation.Family;
                }
            }
            Chipset["SystemInformation"] = SystemInformation;
        }
    }
    VirtualMachine["Chipset"] = Chipset;

    nlohmann::json ComputeTopology;
    {
        nlohmann::json Memory;
        {
            Memory["SizeInMB"] = Configuration.MemorySize;
            Memory["AllowOvercommit"] = Configuration.AllowMemoryOvercommit;
            if (Configuration.EnableDeferredCommit)
                Memory["EnableDeferredCommit"] = true;
        }
        ComputeTopology["Memory"] = Memory;

        nlohmann::json Processor;
        {
            Processor["Count"] = Configuration.ProcessorCount;
            if (Configuration.ProcessorLimit) Processor["Limit"] = Configuration.ProcessorLimit;
            if (Configuration.ProcessorWeight) Processor["Weight"] = Configuration.ProcessorWeight;
            if (Configuration.ExposeVirtualizationExtensions)
            {
                Processor["ExposeVirtualizationExtensions"] = true;
            }
        }
        ComputeTopology["Processor"] = Processor;

        if (::MileIsWindowsVersionAtLeast(10, 0, 26100) &&
            !Configuration.Policies.empty())
        {
            std::uint32_t GuestFeatureSet = 0;
            for (std::string const& Policy : Configuration.Policies)
            {
                if ("LegacyPcrMeasurement" == Policy)
                {
                    GuestFeatureSet |= 0x00000002;
                }
                else if ("DisableSha384Pcr" == Policy)
                {
                    GuestFeatureSet |= 0x00000004;
                }
                else if ("LegacyRteWidth" == Policy)
                {
                    GuestFeatureSet |= 0x00000008;
                }
                else if ("LegacyTrustedLaunchMemoryMap" == Policy)
                {
                    GuestFeatureSet |= 0x00000010;
                }
                else if ("LegacyApicSelectionLogic" == Policy)
                {
                    GuestFeatureSet |= 0x00000020;
                }
                else if ("LegacyApicManganeseOverride" == Policy)
                {
                    GuestFeatureSet |= 0x00000040;
                }
                else if ("LegacyGpaLayout" == Policy)
                {
                    GuestFeatureSet |= 0x00000080;
                }
                else if ("ForceLegacyHcl" == Policy)
                {
                    GuestFeatureSet |= 0x00000100;
                }
                else if ("ForceCurrentHcl" == Policy)
                {
                    GuestFeatureSet |= 0x00000200;
                }
                else if ("UseExtendedMaxVpPerSocketLimit" == Policy)
                {
                    GuestFeatureSet |= 0x00000400;
                }
            }
            if (GuestFeatureSet)
            {
                // UseGuestFeatureSetFlags
                GuestFeatureSet |= 0x00000001;
            }
            ComputeTopology["Compatibility"]["GuestFeatureSet"] =
                GuestFeatureSet;
        }
    }
    VirtualMachine["ComputeTopology"] = ComputeTopology;
    if (Configuration.StorageIopsMaximum)
        VirtualMachine["StorageQoS"]["IopsMaximum"] = Configuration.StorageIopsMaximum;
    if (Configuration.StorageBandwidthMaximum)
        VirtualMachine["StorageQoS"]["BandwidthMaximum"] = Configuration.StorageBandwidthMaximum;

    // Note: Skip Configuration.Gpu because it need to add at runtime.

    nlohmann::json Devices;
    {
        bool IsDefaultResolution = (
            1024 == Configuration.VideoMonitor.HorizontalResolution &&
            768 == Configuration.VideoMonitor.VerticalResolution);

        nlohmann::json VideoMonitor;
        if (::MileIsWindowsVersionAtLeast(10, 0, 20348) && IsDefaultResolution)
        {
            VideoMonitor["ResolutionType"] = NanaBox::ResolutionType::Default;
        }
        VideoMonitor["HorizontalResolution"] =
            Configuration.VideoMonitor.HorizontalResolution;
        VideoMonitor["VerticalResolution"] =
            Configuration.VideoMonitor.VerticalResolution;
        {
            nlohmann::json ConnectionOptions;
            ConnectionOptions["NamedPipe"] = Mile::FormatString(
                "\\\\.\\pipe\\%s.BasicSession",
                Configuration.Name.c_str());
            nlohmann::json AccessSids;
            AccessSids.push_back(::GetCurrentProcessUserStringSid());
            ConnectionOptions["AccessSids"] = AccessSids;
            VideoMonitor["ConnectionOptions"] = ConnectionOptions;
        }
        Devices["VideoMonitor"] = VideoMonitor;

        nlohmann::json EnhancedModeVideo;
        {
            nlohmann::json ConnectionOptions;
            ConnectionOptions["NamedPipe"] = Mile::FormatString(
                "\\\\.\\pipe\\%s.EnhancedSession",
                Configuration.Name.c_str());
            nlohmann::json AccessSids;
            AccessSids.push_back(::GetCurrentProcessUserStringSid());
            ConnectionOptions["AccessSids"] = AccessSids;
            EnhancedModeVideo["ConnectionOptions"] = ConnectionOptions;
        }
        Devices["EnhancedModeVideo"] = EnhancedModeVideo;

        Devices["Keyboard"] = nlohmann::json::object();

        Devices["Mouse"] = nlohmann::json::object();

        nlohmann::json ComPorts;
        if (!Configuration.ComPorts.ComPort1.empty())
        {
            ComPorts["0"] = NanaBox::MakeHcsComPortConfiguration(
                Configuration.ComPorts.ComPort1);
        }
        if (!Configuration.ComPorts.ComPort2.empty())
        {
            ComPorts["1"] = NanaBox::MakeHcsComPortConfiguration(
                Configuration.ComPorts.ComPort2);
        }
        Devices["ComPorts"] = ComPorts;

        if (!Configuration.NetworkAdapters.empty())
        {
            nlohmann::json NetworkAdapters;
            for (NanaBox::NetworkAdapterConfiguration const& NetworkAdapter
                : Configuration.NetworkAdapters)
            {
                if (!NetworkAdapter.Connected)
                {
                    continue;
                }
                NetworkAdapters[NetworkAdapter.EndpointId] =
                    NanaBox::MakeHcsNetworkAdapterConfiguration(NetworkAdapter);
            }
            Devices["NetworkAdapters"] = NetworkAdapters;
        }

        if (!Configuration.ScsiDevices.empty())
        {
            nlohmann::json ScsiDevices;
            std::uint32_t Count = 0;
            for (NanaBox::ScsiDeviceConfiguration const& ScsiDevice
                : Configuration.ScsiDevices)
            {
                ScsiDevices[std::to_string(Count++)] =
                    NanaBox::MakeHcsScsiDeviceConfiguration(ScsiDevice);
            }
            nlohmann::json ScsiController;
            ScsiController["Attachments"] = ScsiDevices;
            Devices["Scsi"]["NanaBox Scsi Controller"] = ScsiController;
        }

        {
            nlohmann::json Plan9Shares;
            nlohmann::json VirtualSmbShares;

            if (Configuration.Gpu.EnableHostDriverStore)
            {
                std::wstring SystemDirectoryPath(MAX_PATH, L'\0');
                SystemDirectoryPath.resize(::GetSystemDirectoryW(
                    SystemDirectoryPath.data(),
                    static_cast<UINT>(SystemDirectoryPath.size())));

                const bool LinuxGuest = Configuration.GuestType == NanaBox::GuestType::Linux;
                const bool WindowsGuest = Configuration.GuestType == NanaBox::GuestType::Windows;
                const std::uint32_t Plan9SharePort = 50001;

                typedef struct { LPCSTR Name; LPCWSTR Path; } ShareItem;

                const ShareItem Plan9ShareItems[] =
                {
                    { "HostDriverStore", L"\\DriverStore" },
                    { "NanaBox.HostDrivers", L"\\DriverStore\\FileRepository" },
                    { "NanaBox.HostLxssLib", L"\\lxss\\lib" }
                };
                const std::size_t Plan9ShareItemsCount =
                    sizeof(Plan9ShareItems) / sizeof(*Plan9ShareItems);

                const ShareItem VirtualSmbShareItems[] =
                {
                    { "NanaBox.HostDrivers", L"\\DriverStore\\FileRepository" },
                    { "NanaBox.HostLxssLib", L"\\lxss\\lib" }
                };
                const std::size_t VirtualSmbShareItemsCount =
                    sizeof(VirtualSmbShareItems) / sizeof(*VirtualSmbShareItems);

                for (size_t i = 0; i < Plan9ShareItemsCount; ++i)
                {
                    if ((LinuxGuest && i == 0) || (WindowsGuest && i == 2))
                        continue;

                    std::wstring SharePath(
                        SystemDirectoryPath + Plan9ShareItems[i].Path);

                    if (!::PathFileExistsW(SharePath.c_str()))
                    {
                        continue;
                    }

                    nlohmann::json Current;
                    Current["Name"] = Plan9ShareItems[i].Name;
                    Current["AccessName"] = Plan9ShareItems[i].Name;
                    Current["Path"] = Mile::ToString(CP_UTF8, SharePath);
                    Current["Port"] = Plan9SharePort;
                    std::uint32_t Flags = NanaBox::Plan9ShareFlags::ReadOnly;
                    if (LinuxGuest)
                        Flags |= NanaBox::Plan9ShareFlags::AllowOptions;
                    Current["Flags"] = Flags;
                    Plan9Shares.push_back(Current);
                }

                for (size_t i = 0; !LinuxGuest && i < VirtualSmbShareItemsCount; ++i)
                {
                    if (WindowsGuest && i == 1)
                        continue;

                    std::wstring SharePath(
                        SystemDirectoryPath + VirtualSmbShareItems[i].Path);

                    if (!::PathFileExistsW(SharePath.c_str()))
                    {
                        continue;
                    }

                    nlohmann::json Current;
                    Current["Name"] = VirtualSmbShareItems[i].Name;
                    Current["Path"] = Mile::ToString(CP_UTF8, SharePath);
                    Current["Options"]["ReadOnly"] = true;
                    Current["Options"]["ShareRead"] = true;
                    Current["Options"]["CacheIo"] = true;
                    Current["Options"]["PseudoOplocks"] = true;
                    Current["Options"]["SupportCloudFiles"] = true;
                    VirtualSmbShares.push_back(Current);
                }
            }

            for (NanaBox::Plan9ShareConfiguration const& Plan9Share
                : Configuration.Plan9Shares)
            {
                Plan9Shares.push_back(NanaBox::MakeHcsPlan9Share(Plan9Share));
            }
            if (!Plan9Shares.empty())
            {
                Devices["Plan9"]["Shares"] = Plan9Shares;
            }

            for (NanaBox::VirtualSmbShareConfiguration const& VirtualSmbShare
                : Configuration.VirtualSmbShares)
            {
                nlohmann::json Current;
                Current["Name"] = VirtualSmbShare.Name;
                Current["Path"] = ::GetAbsoluteUtf8Path(VirtualSmbShare.Path);
                if (VirtualSmbShare.ReadOnly)
                {
                    Current["Options"]["ReadOnly"] = true;
                    Current["Options"]["ShareRead"] = true;
                    Current["Options"]["CacheIo"] = true;
                    Current["Options"]["PseudoOplocks"] = true;
                }
                Current["Options"]["SupportCloudFiles"] = true;
                VirtualSmbShares.push_back(Current);
            }
            if (!VirtualSmbShares.empty())
            {
                Devices["VirtualSmb"]["Shares"] = VirtualSmbShares;
                Devices["VirtualSmb"]["DirectFileMappingInMB"] = 2048;
            }
        }
    }
    VirtualMachine["Devices"] = Devices;

    if (Configuration.Tpm)
    {
        nlohmann::json SecuritySettings;
        SecuritySettings["EnableTpm"] = true;
        SecuritySettings["Isolation"]["IsolationType"] = "GuestStateOnly";
        VirtualMachine["SecuritySettings"] = SecuritySettings;
    }

    if (!Configuration.GuestStateFile.empty())
    {
        VirtualMachine["GuestState"]["GuestStateFilePath"] =
            ::GetAbsoluteUtf8Path(Configuration.GuestStateFile);
    }

    if (!Configuration.RuntimeStateFile.empty())
    {
        VirtualMachine["GuestState"]["RuntimeStateFilePath"] =
            ::GetAbsoluteUtf8Path(Configuration.RuntimeStateFile);
    }

    if (!Configuration.SaveStateFile.empty())
    {
        VirtualMachine["RestoreState"]["SaveStateFilePath"] =
            ::GetAbsoluteUtf8Path(Configuration.SaveStateFile);
    }

    if (!Configuration.AdditionalHcsSettings.empty())
    {
        auto const& Text = Configuration.AdditionalHcsSettings;
        if (Text.size() > 1024 * 1024) throw std::invalid_argument("Additional HCS settings exceed 1 MiB");
        MergeHcsSettings(VirtualMachine, nlohmann::json::parse(Text), "VirtualMachine");
    }

    Result["VirtualMachine"] = VirtualMachine;

    return Result.dump(2);
}

void NanaBox::ComputeNetworkCreateEndpoint(
    std::string const& Owner,
    NanaBox::NetworkAdapterConfiguration& Configuration)
{
    NanaBox::HcnNetwork NetworkHandle;
    winrt::guid SwitchId = NanaBox::NanaBoxSwitchId;
    if (!Configuration.NetworkId.empty())
    {
        SwitchId = winrt::guid(Configuration.NetworkId);
        auto Info = NanaBox::QueryHostNetwork(SwitchId);
        if (Info.Flags & NanaBox::HcnNetworkFlags::EnableFlowSteering)
            throw winrt::hresult_error(E_NOTIMPL, L"Mirrored networks require guest network synchronization.");
        NetworkHandle = NanaBox::HcnOpenNetwork(SwitchId);
    }
    else
    {
        bool UseSuggestedSwitchId = false;
        bool UseSuggestedSwitchName = false;
        bool UseSuggestedSwitchSubnet = false;
        if (!Configuration.SuggestedSwitchId.empty())
        {
            try
            {
                winrt::guid CandidateSwitchId = winrt::guid(
                    Configuration.SuggestedSwitchId);
                if (NanaBox::NanaBoxSwitchId != CandidateSwitchId)
                {
                    SwitchId = CandidateSwitchId;
                    UseSuggestedSwitchId = true;
                }
            }
            catch (...)
            {
            }
        }
        nlohmann::json Settings;
        {
            std::string SwitchName = "NanaBox";
            if (UseSuggestedSwitchId)
            {
                if (Configuration.SuggestedSwitchName.empty() ||
                    "NanaBox" == Configuration.SuggestedSwitchName)
                {
                    SwitchName.push_back('_');
                    SwitchName.append(winrt::to_string(::FromGuid(SwitchId)));
                }
                else
                {
                    SwitchName = Configuration.SuggestedSwitchName;
                }
                UseSuggestedSwitchName = true;
            }
            Settings["Name"] = SwitchName;
        }
        Settings["Type"] = "ICS";
        Settings["IsolateSwitch"] = true;
        Settings["Flags"] =
            NanaBox::HcnNetworkFlags::EnableDns |
            NanaBox::HcnNetworkFlags::EnableDhcp |
            NanaBox::HcnNetworkFlags::EnableNonPersistent;
        if (!Configuration.SuggestedSwitchSubnet.empty())
        {
            std::uint8_t Octets[4] = {};
            std::uint8_t PrefixLength = 0;
            if (5 == ::sscanf_s(
                Configuration.SuggestedSwitchSubnet.c_str(),
                "%hhu.%hhu.%hhu.%hhu/%hhu",
                &Octets[0],
                &Octets[1],
                &Octets[2],
                &Octets[3],
                &PrefixLength))
            {
                std::string FormalizedSubnet = Mile::FormatString(
                    "%hhu.%hhu.%hhu.%hhu/%hhu",
                    Octets[0],
                    Octets[1],
                    Octets[2],
                    Octets[3],
                    PrefixLength);
                std::string FormalizedGateway = Mile::FormatString(
                    "%hhu.%hhu.%hhu.1",
                    Octets[0],
                    Octets[1],
                    Octets[2]);
                nlohmann::json Subnet;
                {
                    Subnet["AddressPrefix"] = FormalizedSubnet;
                    Subnet["GatewayAddress"] = FormalizedGateway;
                    Subnet["ObjectType"] = 5; // Subnet

                    nlohmann::json IpSubnet;
                    {
                        IpSubnet["IpAddressPrefix"] = FormalizedSubnet;
                        IpSubnet["Flags"] = 3; // EnableDns | EnableDhcp
                        IpSubnet["ObjectType"] = 6; // IpSubnet
                    }
                    Subnet["IpSubnets"].push_back(IpSubnet);
                }
                Settings["Subnets"].push_back(Subnet);
                UseSuggestedSwitchSubnet = true;
            }
        }
        try
        {
            NetworkHandle = NanaBox::HcnCreateNetwork(
                SwitchId,
                winrt::to_hstring(Settings.dump()));
        }
        catch (winrt::hresult_error const& ex)
        {
            if (HCN_E_NETWORK_ALREADY_EXISTS == ex.code())
            {
                try
                {
                    NetworkHandle = NanaBox::HcnOpenNetwork(SwitchId);
                }
                catch (...)
                {
                }
            }
        }
        if (!NetworkHandle)
        {
            SwitchId = NanaBox::NanaBoxSwitchId;
            UseSuggestedSwitchId = false;
            Settings["Name"] = "NanaBox";
            UseSuggestedSwitchName = false;
            Settings.erase("Subnets");
            UseSuggestedSwitchSubnet = false;
            try
            {
                NetworkHandle = NanaBox::HcnCreateNetwork(
                    SwitchId,
                    winrt::to_hstring(Settings.dump()));
            }
            catch (winrt::hresult_error const& ex)
            {
                if (HCN_E_NETWORK_ALREADY_EXISTS == ex.code())
                {
                    NetworkHandle = NanaBox::HcnOpenNetwork(SwitchId);
                }
                else
                {
                    throw;
                }
            }
        }
    }

    GUID EndpointId;
    if (Configuration.EndpointId.empty())
    {
        winrt::check_hresult(::CoCreateGuid(&EndpointId));
        Configuration.EndpointId = winrt::to_string(::FromGuid(EndpointId));
    }
    else
    {
        EndpointId = winrt::guid(Configuration.EndpointId);
    }

    NanaBox::HcnEndpoint EndpointHandle;
    nlohmann::json Settings;
    Settings["SchemaVersion"]["Major"] = 2;
    Settings["SchemaVersion"]["Minor"] = 0;
    Settings["Owner"] = Owner;
    Settings["HostComputeNetwork"] = winrt::to_string(::FromGuid(SwitchId));
    if (!Configuration.MacAddress.empty())
    {
        Settings["MacAddress"] = Configuration.MacAddress;
    }

    if (!Configuration.FirewallCreatorId.empty())
    {
        auto Creator = winrt::guid(Configuration.FirewallCreatorId);
        auto Network = NanaBox::QueryHostNetwork(SwitchId);
        if (!(Network.Flags & NanaBox::HcnNetworkFlags::EnableFirewall))
            throw winrt::hresult_error(E_INVALIDARG, L"The selected HCN network does not enable Hyper-V firewall.");
        Settings["SchemaVersion"]["Minor"] = 16;
        Settings["Policies"] = nlohmann::json::array({
            {{"Type", "PortName"}, {"Settings", {{"Name", ""}}}},
            {{"Type", "Firewall"}, {"Settings", {
                {"VmCreatorId", winrt::to_string(::FromGuid(Creator))}, {"PolicyFlags", 0}}}}
        });
    }

    EndpointHandle = NanaBox::HcnCreateEndpoint(
        NetworkHandle,
        EndpointId,
        winrt::to_hstring(Settings.dump()));

    try
    {
        nlohmann::json Properties = nlohmann::json::parse(winrt::to_string(
            NanaBox::HcnQueryEndpointProperties(EndpointHandle)));
        Configuration.MacAddress = Properties.at("MacAddress").get<std::string>();
    }
    catch (...)
    {
        // This invocation created the endpoint and therefore owns its lifetime.
        EndpointHandle.close();
        try { NanaBox::HcnDeleteEndpoint(EndpointId); } catch (...) {}
        throw;
    }
}

void NanaBox::ComputeNetworkDeleteEndpoint(
    std::string const& Owner,
    NanaBox::NetworkAdapterConfiguration const& Configuration)
{
    if (!Configuration.EndpointId.empty())
        NanaBox::HcnDeleteOwnedEndpoint(winrt::guid(Configuration.EndpointId), Owner);
}

void NanaBox::NormalizeNetworkAdapterIds(std::vector<NetworkAdapterConfiguration>& Adapters)
{
    std::set<std::string> Ids;
    for (auto& Adapter : Adapters)
    {
        GUID Id{};
        if (Adapter.EndpointId.empty())
            winrt::check_hresult(::CoCreateGuid(&Id));
        else
            Id = winrt::guid(Adapter.EndpointId);
        if (::IsEqualGUID(Id, GUID_NULL))
            throw winrt::hresult_invalid_argument(L"Network endpoint ID must not be zero.");
        Adapter.EndpointId = winrt::to_string(::FromGuid(Id));
        if (!Ids.insert(Adapter.EndpointId).second)
            throw winrt::hresult_invalid_argument(L"Duplicate network endpoint ID.");
    }
}

void NanaBox::ComputeSystemUpdateMemorySize(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    std::uint64_t const& MemorySize)
{
    nlohmann::json Result;

    Result["ResourcePath"] = "VirtualMachine/ComputeTopology/Memory/SizeInMB";
    Result["RequestType"] = "Update";
    Result["Settings"] = MemorySize;

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemAddComPort(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    std::uint32_t const& PortID,
    std::string const& NamedPipe)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/ComPorts/%d",
        PortID);
    Result["RequestType"] = "Add";
    Result["Settings"] = NanaBox::MakeHcsComPortConfiguration(NamedPipe);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemRemoveComPort(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    std::uint32_t const& PortID,
    std::string const& NamedPipe)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/ComPorts/%d",
        PortID);
    Result["RequestType"] = "Remove";
    Result["Settings"] = NanaBox::MakeHcsComPortConfiguration(NamedPipe);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemUpdateComPort(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    std::uint32_t const& PortID,
    std::string const& NamedPipe)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/ComPorts/%d",
        PortID);
    Result["RequestType"] = "Update";
    Result["Settings"] = NanaBox::MakeHcsComPortConfiguration(NamedPipe);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemAddNetworkAdapter(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    NanaBox::NetworkAdapterConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/NetworkAdapters/%s",
        Configuration.EndpointId.c_str());
    Result["RequestType"] = "Add";
    Result["Settings"] = NanaBox::MakeHcsNetworkAdapterConfiguration(
        Configuration);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemRemoveNetworkAdapter(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    NanaBox::NetworkAdapterConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/NetworkAdapters/%s",
        Configuration.EndpointId.c_str());
    Result["RequestType"] = "Remove";
    Result["Settings"] = NanaBox::MakeHcsNetworkAdapterConfiguration(
        Configuration);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemAddScsiDevice(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    std::uint32_t const& DeviceID,
    NanaBox::ScsiDeviceConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/Scsi/NanaBox Scsi Controller/Attachments/%d",
        DeviceID);
    Result["RequestType"] = "Add";
    Result["Settings"] = NanaBox::MakeHcsScsiDeviceConfiguration(Configuration);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

void NanaBox::ComputeSystemUpdateScsiDevice(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    std::uint32_t const& DeviceID,
    NanaBox::ScsiDeviceConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["ResourcePath"] = Mile::FormatString(
        "VirtualMachine/Devices/Scsi/NanaBox Scsi Controller/Attachments/%d",
        DeviceID);
    Result["RequestType"] = "Update";
    Result["Settings"] = NanaBox::MakeHcsScsiDeviceConfiguration(Configuration);

    Instance->Modify(winrt::to_hstring(Result.dump()));
}

nlohmann::json NanaBox::MakeHcsGpuModification(
    NanaBox::GpuConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["ResourcePath"] = "VirtualMachine/ComputeTopology/Gpu";
    Result["RequestType"] = "Update";

    nlohmann::json Settings;

    Settings["AssignmentMode"] = "Disabled";
    if (NanaBox::GpuAssignmentMode::Default == Configuration.AssignmentMode)
    {
        Settings["AssignmentMode"] = "Default";
    }
    else if (NanaBox::GpuAssignmentMode::Mirror == Configuration.AssignmentMode)
    {
        Settings["AssignmentMode"] = "Mirror";
    }
    else if (NanaBox::GpuAssignmentMode::List == Configuration.AssignmentMode)
    {
        if (!Configuration.SelectedDevices.empty())
        {
            Settings["AssignmentMode"] = "List";
            nlohmann::json Devices;
            for (std::pair<std::string, std::uint16_t> const& Device
                : Configuration.SelectedDevices)
            {
                Devices[Device.first] = Device.second;
            }
            Settings["AssignmentRequest"] = Devices;
        }
    }
    Settings["AllowVendorExtension"] = true;

    Result["Settings"] = Settings;
    return Result;
}

void NanaBox::ComputeSystemUpdateGpu(
    winrt::com_ptr<NanaBox::ComputeSystem> const& Instance,
    GpuConfiguration const& Configuration)
{
    Instance->Modify(winrt::to_hstring(MakeHcsGpuModification(Configuration).dump()));
}

void NanaBox::RemoteDesktopUpdateKeyboardConfiguration(
    winrt::com_ptr<NanaBox::RdpClient> const& Instance,
    NanaBox::KeyboardConfiguration& Configuration)
{
    try
    {
        Instance->KeyboardHookMode(
            Configuration.RedirectKeyCombinations ? 1 : 2);
    }
    catch (...)
    {
        Configuration.RedirectKeyCombinations =
            (Instance->KeyboardHookMode() == 1);
    }

    try
    {
        Instance->HotKeyFullScreen(
            Configuration.FullScreenHotkey);
    }
    catch (...)
    {
        Configuration.FullScreenHotkey =
            Instance->HotKeyFullScreen();
    }

    try
    {
        Instance->HotKeyCtrlEsc(
            Configuration.CtrlEscHotkey);
    }
    catch (...)
    {
        Configuration.CtrlEscHotkey =
            Instance->HotKeyCtrlEsc();
    }

    try
    {
        Instance->HotKeyAltEsc(
            Configuration.AltEscHotkey);
    }
    catch (...)
    {
        Configuration.AltEscHotkey =
            Instance->HotKeyAltEsc();
    }

    try
    {
        Instance->HotKeyAltTab(
            Configuration.AltTabHotkey);
    }
    catch (...)
    {
        Configuration.AltTabHotkey =
            Instance->HotKeyAltTab();
    }

    try
    {
        Instance->HotKeyAltShiftTab(
            Configuration.AltShiftTabHotkey);
    }
    catch (...)
    {
        Configuration.AltShiftTabHotkey =
            Instance->HotKeyAltShiftTab();
    }

    try
    {
        Instance->HotKeyAltSpace(
            Configuration.AltSpaceHotkey);
    }
    catch (...)
    {
        Configuration.AltSpaceHotkey =
            Instance->HotKeyAltSpace();
    }

    try
    {
        Instance->HotKeyCtrlAltDel(
            Configuration.CtrlAltDelHotkey);
    }
    catch (...)
    {
        Configuration.CtrlAltDelHotkey =
            Instance->HotKeyCtrlAltDel();
    }

    try
    {
        Instance->HotKeyFocusReleaseLeft(
            Configuration.FocusReleaseLeftHotkey);
    }
    catch (...)
    {
        Configuration.FocusReleaseLeftHotkey =
            Instance->HotKeyFocusReleaseLeft();
    }

    try
    {
        Instance->HotKeyFocusReleaseRight(
            Configuration.FocusReleaseRightHotkey);
    }
    catch (...)
    {
        Configuration.FocusReleaseRightHotkey =
            Instance->HotKeyFocusReleaseRight();
    }
}

void NanaBox::RemoteDesktopUpdateEnhancedSessionConfiguration(
    winrt::com_ptr<NanaBox::RdpClient> const& Instance,
    NanaBox::EnhancedSessionConfiguration& Configuration)
{
    try
    {
        Instance->AudioRedirectionMode(
            Configuration.RedirectAudio ? 0 : 2);
    }
    catch (...)
    {
        Configuration.RedirectAudio =
            (Instance->AudioRedirectionMode() == 0);
    }

    try
    {
        Instance->AudioCaptureRedirectionMode(
            Configuration.RedirectAudioCapture);
    }
    catch (...)
    {
        Configuration.RedirectAudioCapture =
            Instance->AudioCaptureRedirectionMode();
    }

    try
    {
        Instance->RedirectDrives(
            Configuration.RedirectDrives);
    }
    catch (...)
    {
        Configuration.RedirectDrives =
            Instance->RedirectDrives();
    }

    try
    {
        Instance->RedirectPrinters(
            Configuration.RedirectPrinters);
    }
    catch (...)
    {
        Configuration.RedirectPrinters =
            Instance->RedirectPrinters();
    }

    try
    {
        Instance->RedirectPorts(
            Configuration.RedirectPorts);
    }
    catch (...)
    {
        Configuration.RedirectPorts =
            Instance->RedirectPorts();
    }

    try
    {
        Instance->RedirectSmartCards(
            Configuration.RedirectSmartCards);
    }
    catch (...)
    {
        Configuration.RedirectSmartCards =
            Instance->RedirectSmartCards();
    }

    try
    {
        Instance->RedirectClipboard(
            Configuration.RedirectClipboard);
    }
    catch (...)
    {
        Configuration.RedirectClipboard =
            Instance->RedirectClipboard();
    }

    try
    {
        Instance->RedirectDevices(
            Configuration.RedirectDevices);
    }
    catch (...)
    {
        Configuration.RedirectDevices =
            Instance->RedirectDevices();
    }

    try
    {
        Instance->RedirectPOSDevices(
            Configuration.RedirectPOSDevices);
    }
    catch (...)
    {
        Configuration.RedirectPOSDevices =
            Instance->RedirectPOSDevices();
    }

    try
    {
        Instance->RedirectDynamicDrives(
            Configuration.RedirectDynamicDrives);
    }
    catch (...)
    {
        Configuration.RedirectDynamicDrives =
            Instance->RedirectDynamicDrives();
    }

    try
    {
        Instance->RedirectDynamicDevices(
            Configuration.RedirectDynamicDevices);
    }
    catch (...)
    {
        Configuration.RedirectDynamicDevices =
            Instance->RedirectDynamicDevices();
    }

    try
    {
        ULONG Count = Instance->DriveCount();
        for (ULONG i = 0; i < Count; ++i)
        {
            try
            {
                NanaBox::RdpDrive Current = Instance->DriveByIndex(i);
                for (std::string const& Drive : Configuration.Drives)
                {
                    if ((Drive[0] - 'A') == static_cast<char>(
                        Current.DriveLetterIndex()))
                    {
                        Current.RedirectionState(true);
                        break;
                    }
                }
            }
            catch (...)
            {

            }
        }
    }
    catch (...)
    {

    }

    try
    {
        ULONG Count = Instance->DeviceCount();
        for (ULONG i = 0; i < Count; ++i)
        {
            try
            {
                NanaBox::RdpDevice Current = Instance->DeviceByIndex(i);
                for (std::string const& Device : Configuration.Devices)
                {
                    if (Device == winrt::to_string(Current.DeviceInstanceId()))
                    {
                        Current.RedirectionState(true);
                        break;
                    }
                }
            }
            catch (...)
            {

            }
        }
    }
    catch (...)
    {

    }
}

NanaBox::VirtualMachineConfiguration NanaBox::DeserializeConfiguration(
    std::string const& Configuration)
{
    nlohmann::json ParsedJson = nlohmann::json::parse(Configuration);

    nlohmann::json RootJson = Mile::Json::GetSubKey(ParsedJson, "NanaBox");

    if ("VirtualMachine" != Mile::Json::ToString(
        Mile::Json::GetSubKey(RootJson, "Type")))
    {
        throw std::exception("Invalid Virtual Machine Configuration");
    }

    NanaBox::VirtualMachineConfiguration Result =
        NanaBox::ToVirtualMachineConfiguration(RootJson);

    if (Result.Version < 1 || Result.Version > 1)
    {
        throw std::exception("Invalid Version");
    }

    if (!Result.ProcessorCount)
    {
        throw std::exception("Invalid Processor Count");
    }

    if (!Result.MemorySize)
    {
        throw std::exception("Invalid Memory Size");
    }

    if (Result.MirroredBindCoordination && (!Result.MirroredNetworking || Result.GuestType != NanaBox::GuestType::Linux))
        throw std::exception("Bind coordination requires mirrored networking and a Linux guest");
    if (Result.MirroredDnsTunneling && !Result.MirroredNetworking)
        throw std::exception("DNS tunneling requires mirrored networking");
    if (Result.MirroredLoopback && (!Result.MirroredNetworking || Result.GuestType != NanaBox::GuestType::Linux))
        throw std::exception("GELNIC loopback requires mirrored networking and a Linux guest");
    if (Result.MirroredHostAddressLoopback && !Result.MirroredLoopback)
        throw std::exception("Host address loopback requires GELNIC loopback");

    NanaBox::ValidatePlan9Shares(Result);
    if (!Result.AdditionalHcsSettings.empty())
        (void)NanaBox::MakeHcsConfiguration(Result);

    return Result;
}

std::string NanaBox::SerializeConfiguration(
    NanaBox::VirtualMachineConfiguration const& Configuration)
{
    nlohmann::json Result;

    Result["NanaBox"] = NanaBox::FromVirtualMachineConfiguration(Configuration);
    Result["NanaBox"]["Type"] = "VirtualMachine";

    return Result.dump(2);
}
