/*
 * PROJECT:    NanaBox
 * FILE:       HostCompute.cpp
 * PURPOSE:    Implementation for the Host Compute API C++/WinRT wrapper
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#include "HostCompute.h"

#pragma comment(lib, "computecore.lib")
#pragma comment(lib, "computenetwork.lib")
#pragma comment(lib, "advapi32.lib")

#include <Mile.Json.h>
#include "../GuestTool/WddmMapping.h"
#include <functional>
#include <SetupAPI.h>
#include <wincrypt.h>
#include <cwchar>
#include <string>
#pragma comment(lib, "crypt32.lib")
#include <memory>
#include <type_traits>
#include <string_view>
#include <set>
#include <vector>
#pragma comment(lib, "setupapi.lib")

namespace winrt
{
    struct hlocal_string_traits
    {
        using type = PWSTR;

        static void close(type value) noexcept
        {
            ::LocalFree(value);
        }

        static constexpr type invalid() noexcept
        {
            return nullptr;
        }
    };

    using hlocal_string = handle_type<hlocal_string_traits>;

    struct cotaskmem_string_traits
    {
        using type = PWSTR;

        static void close(type value) noexcept
        {
            ::CoTaskMemFree(value);
        }

        static constexpr type invalid() noexcept
        {
            return nullptr;
        }
    };

    using cotaskmem_string = handle_type<cotaskmem_string_traits>;
}

namespace
{
    winrt::hstring WaitForOperationResult(
        NanaBox::HcsOperation const& Operation)
    {
        winrt::hstring Result;

        winrt::hlocal_string RawResult;
        HRESULT hr = ::HcsWaitForOperationResult(
            Operation.get(),
            INFINITE,
            RawResult.put());
        if (RawResult)
        {
            Result = winrt::hstring(RawResult.get());
        }
        if (FAILED(hr))
        {
            winrt::hresult_error Exception;
            try
            {
                nlohmann::json ResultError = nlohmann::json::parse(
                    winrt::to_string(Result));
                Exception = winrt::hresult_error(
                    ResultError["Error"].get<std::int32_t>(),
                    winrt::to_hstring(
                        ResultError["ErrorMessage"].get<std::string>()));
            }
            catch (...)
            {
                Exception = winrt::hresult_error(hr, Result);
            }
            throw Exception;
        }

        return Result;
    }

    void CheckHcnCall(
        HRESULT RawErrorCode,
        winrt::cotaskmem_string const& RawErrorRecord)
    {
        winrt::hstring ErrorRecord;
        if (RawErrorRecord)
        {
            ErrorRecord = winrt::hstring(RawErrorRecord.get());
        }
        if (FAILED(RawErrorCode))
        {
            winrt::hresult_error Exception;
            try
            {
                nlohmann::json ResultError = nlohmann::json::parse(
                    winrt::to_string(ErrorRecord));
                Exception = winrt::hresult_error(
                    ResultError["ErrorCode"].get<std::int32_t>(),
                    winrt::to_hstring(
                        ResultError["Error"].get<std::string>()));
            }
            catch (...)
            {
                Exception = winrt::hresult_error(RawErrorCode, ErrorRecord);
            }
            throw Exception;
        }
    }
}

NanaBox::ComputeSystem::ComputeSystem(
    winrt::hstring const& Id,
    winrt::hstring const& Configuration)
{
    this->m_Operation.attach(::HcsCreateOperation(
        nullptr,
        nullptr));
    winrt::check_pointer(
        this->m_Operation.get());

    winrt::check_hresult(::HcsCreateComputeSystem(
        Id.c_str(),
        Configuration.c_str(),
        this->m_Operation.get(),
        nullptr,
        this->m_ComputeSystem.put()));

    ::WaitForOperationResult(
        this->m_Operation);

    winrt::check_hresult(::HcsSetComputeSystemCallback(
        this->m_ComputeSystem.get(),
        HcsEventOptionNone,
        this,
        NanaBox::ComputeSystem::ComputeSystemCallback));
}

NanaBox::ComputeSystem::ComputeSystem(
    winrt::hstring const& Id)
{
    this->m_Operation.attach(::HcsCreateOperation(
        nullptr,
        nullptr));
    winrt::check_pointer(
        this->m_Operation.get());

    winrt::check_hresult(::HcsOpenComputeSystem(
        Id.c_str(),
        GENERIC_ALL,
        this->m_ComputeSystem.put()));

    winrt::check_hresult(::HcsSetComputeSystemCallback(
        this->m_ComputeSystem.get(),
        HcsEventOptionNone,
        this,
        NanaBox::ComputeSystem::ComputeSystemCallback));
}

void NanaBox::ComputeSystem::Start()
{
    winrt::check_hresult(::HcsStartComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        nullptr));

    ::WaitForOperationResult(
        this->m_Operation);
}

void NanaBox::ComputeSystem::Shutdown()
{
    winrt::check_hresult(::HcsShutDownComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        nullptr));

    ::WaitForOperationResult(
        this->m_Operation);
}

void NanaBox::ComputeSystem::Terminate()
{
    winrt::check_hresult(::HcsTerminateComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        nullptr));

    ::WaitForOperationResult(
        this->m_Operation);
}

void NanaBox::ComputeSystem::Pause(
    winrt::hstring const& Options)
{
    winrt::check_hresult(::HcsPauseComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        Options.empty() ? nullptr : Options.c_str()));

    ::WaitForOperationResult(
        this->m_Operation);
}

void NanaBox::ComputeSystem::Resume()
{
    winrt::check_hresult(::HcsResumeComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        nullptr));

    ::WaitForOperationResult(
        this->m_Operation);
}

void NanaBox::ComputeSystem::Save(
    winrt::hstring const& Options)
{
    winrt::check_hresult(::HcsSaveComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        Options.empty() ? nullptr : Options.c_str()));

    ::WaitForOperationResult(
        this->m_Operation);
}

winrt::hstring NanaBox::ComputeSystem::GetProperties(
    winrt::hstring const& PropertyQuery)
{
    winrt::check_hresult(::HcsGetComputeSystemProperties(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        PropertyQuery.empty() ? nullptr : PropertyQuery.c_str()));

    return ::WaitForOperationResult(
        this->m_Operation);
}

void NanaBox::ComputeSystem::Modify(
    winrt::hstring const& Configuration)
{
    winrt::check_hresult(::HcsModifyComputeSystem(
        this->m_ComputeSystem.get(),
        this->m_Operation.get(),
        Configuration.c_str(),
        nullptr));

    ::WaitForOperationResult(
        this->m_Operation);
}

void CALLBACK NanaBox::ComputeSystem::ComputeSystemCallback(
    HCS_EVENT* Event,
    void* Context)
{
    NanaBox::ComputeSystem* Object =
        reinterpret_cast<NanaBox::ComputeSystem*>(Context);

    switch (Event->Type)
    {
    case HcsEventSystemExited:
        Object->SystemExited(winrt::hstring(Event->EventData));
        break;
    case HcsEventSystemCrashInitiated:
        break;
    case HcsEventSystemCrashReport:
        break;
    case HcsEventSystemRdpEnhancedModeStateChanged:
        Object->SystemRdpEnhancedModeStateChanged();
        break;
    case HcsEventSystemGuestConnectionClosed:
        break;
    case HcsEventServiceDisconnect:
        break;
    default:
        break;
    }
}

winrt::hstring NanaBox::HcsGetServiceProperties(
    winrt::hstring const& PropertyQuery)
{
    winrt::hstring Result;

    winrt::hlocal_string RawResult;
    HRESULT hr = ::HcsGetServiceProperties(
        PropertyQuery.empty() ? nullptr : PropertyQuery.c_str(),
        RawResult.put());
    if (RawResult)
    {
        Result = winrt::hstring(RawResult.get());
    }
    if (FAILED(hr))
    {
        winrt::hresult_error Exception;
        try
        {
            nlohmann::json ResultError = nlohmann::json::parse(
                winrt::to_string(Result));
            Exception = winrt::hresult_error(
                ResultError["Error"].get<std::int32_t>(),
                winrt::to_hstring(
                    ResultError["ErrorMessage"].get<std::string>()));
        }
        catch (...)
        {
            Exception = winrt::hresult_error(hr, Result);
        }
        throw Exception;
    }

    return Result;
}

NanaBox::HcnNetwork NanaBox::HcnCreateNetwork(
    winrt::guid const& NetworkId,
    winrt::hstring const& Settings)
{
    NanaBox::HcnNetwork Result;
    winrt::cotaskmem_string RawErrorRecord;
    ::CheckHcnCall(
        ::HcnCreateNetwork(
            NetworkId,
            Settings.c_str(),
            Result.put(),
            RawErrorRecord.put()),
        RawErrorRecord);
    return Result;
}

NanaBox::HcnNetwork NanaBox::HcnOpenNetwork(
    winrt::guid const& NetworkId)
{
    NanaBox::HcnNetwork Result;

    winrt::cotaskmem_string RawErrorRecord;
    ::CheckHcnCall(
        ::HcnOpenNetwork(
            NetworkId,
            Result.put(),
            RawErrorRecord.put()),
        RawErrorRecord);

    return Result;
}

NanaBox::HcnEndpoint NanaBox::HcnCreateEndpoint(
    NanaBox::HcnNetwork const& NetworkHandle,
    winrt::guid const& EndpointId,
    winrt::hstring const& Settings)
{
    NanaBox::HcnEndpoint Result;

    winrt::cotaskmem_string RawErrorRecord;
    ::CheckHcnCall(
        ::HcnCreateEndpoint(
            NetworkHandle.get(),
            EndpointId,
            Settings.c_str(),
            Result.put(),
            RawErrorRecord.put()),
        RawErrorRecord);

    return Result;
}

void NanaBox::HcnDeleteEndpoint(
    winrt::guid const& EndpointId)
{
    winrt::cotaskmem_string RawErrorRecord;
    ::CheckHcnCall(
        ::HcnDeleteEndpoint(
            EndpointId,
            RawErrorRecord.put()),
        RawErrorRecord);
}

bool NanaBox::IsEndpointOwnedBy(winrt::hstring const& Properties, std::string const& Owner)
{
    auto Data = nlohmann::json::parse(winrt::to_string(Properties));
    auto Field = Data.find("Owner");
    return !Owner.empty() && Field != Data.end() && Field->is_string() &&
        Field->get<std::string>() == Owner;
}

NanaBox::HcnEndpoint NanaBox::HcnOpenOwnedEndpoint(
    winrt::guid const& EndpointId, std::string const& Owner)
{
    HcnEndpoint Endpoint;
    winrt::cotaskmem_string Error;
    HRESULT Status = ::HcnOpenEndpoint(EndpointId, Endpoint.put(), Error.put());
    if (Status == HCN_E_ENDPOINT_NOT_FOUND) return {};
    ::CheckHcnCall(Status, Error);
    if (!NanaBox::IsEndpointOwnedBy(NanaBox::HcnQueryEndpointProperties(Endpoint), Owner))
        throw winrt::hresult_error(E_ACCESSDENIED, L"The network endpoint belongs to another owner.");
    return Endpoint;
}

void NanaBox::HcnDeleteOwnedEndpoint(winrt::guid const& EndpointId, std::string const& Owner)
{
    auto Endpoint = NanaBox::HcnOpenOwnedEndpoint(EndpointId, Owner);
    if (!Endpoint) return;
    Endpoint.close();
    try { NanaBox::HcnDeleteEndpoint(EndpointId); }
    catch (winrt::hresult_error const& Error)
    {
        if (Error.code() != HCN_E_ENDPOINT_NOT_FOUND) throw;
    }
}

winrt::hstring NanaBox::HcnQueryEndpointProperties(
    NanaBox::HcnEndpoint const& EndpointHandle,
    winrt::hstring const& Query)
{
    winrt::hstring Result;

    winrt::cotaskmem_string RawResult;
    winrt::cotaskmem_string RawErrorRecord;
    ::CheckHcnCall(
        ::HcnQueryEndpointProperties(
            EndpointHandle.get(),
            Query.empty() ? nullptr : Query.c_str(),
            RawResult.put(),
            RawErrorRecord.put()),
        RawErrorRecord);
    if (RawResult)
    {
        Result = winrt::hstring(RawResult.get());
    }

    return Result;
}


namespace
{
    constexpr wchar_t ManagedNetworkPrefix[] = L"NanaBox.Managed.";
    constexpr GUID GpuInterfaceClass =
        {0x064092b3, 0x625e, 0x43bf, {0x9e, 0xb5, 0xdc, 0x84, 0x58, 0x97, 0xdd, 0x59}};

    struct DeviceInfoSet
    {
        HDEVINFO Value;
        ~DeviceInfoSet()
        {
            if (Value != INVALID_HANDLE_VALUE)
                ::SetupDiDestroyDeviceInfoList(Value);
        }
    };

    winrt::hstring DeviceName(HDEVINFO Set, SP_DEVINFO_DATA& Device)
    {
        for (DWORD Property : {SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC})
        {
            DWORD Bytes = 0;
            ::SetupDiGetDeviceRegistryPropertyW(Set, &Device, Property, nullptr, nullptr, 0, &Bytes);
            if (!Bytes) continue;
            std::vector<wchar_t> Buffer(Bytes / sizeof(wchar_t) + 1);
            if (::SetupDiGetDeviceRegistryPropertyW(Set, &Device, Property, nullptr,
                reinterpret_cast<PBYTE>(Buffer.data()), Bytes, nullptr))
                return winrt::hstring(Buffer.data());
        }
        return winrt::hstring(L"GPU");
    }

    nlohmann::json ReadHcnIds(bool Networks)
    {
        winrt::cotaskmem_string Result, Error;
        HRESULT Status = Networks
            ? ::HcnEnumerateNetworks(nullptr, Result.put(), Error.put())
            : ::HcnEnumerateEndpoints(nullptr, Result.put(), Error.put());
        ::CheckHcnCall(Status, Error);
        if (!Result) throw winrt::hresult_error(E_UNEXPECTED);
        auto Ids = nlohmann::json::parse(winrt::to_string(winrt::hstring(Result.get())));
        if (!Ids.is_array()) throw winrt::hresult_error(E_UNEXPECTED);
        return Ids;
    }
}

namespace
{
void VisitPartitionableGpus(std::function<void(HDEVINFO, SP_DEVINFO_DATA&, std::wstring const&)> const& Visit)
{
    DeviceInfoSet Set{::SetupDiGetClassDevsW(&GpuInterfaceClass, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
    winrt::check_bool(Set.Value != INVALID_HANDLE_VALUE);
    for (DWORD Index = 0;; ++Index)
    {
        SP_DEVICE_INTERFACE_DATA Interface{};
        Interface.cbSize = sizeof(Interface);
        if (!::SetupDiEnumDeviceInterfaces(Set.Value, nullptr, &GpuInterfaceClass, Index, &Interface))
        {
            if (::GetLastError() == ERROR_NO_MORE_ITEMS) break;
            winrt::throw_last_error();
        }
        DWORD Bytes = 0;
        ::SetupDiGetDeviceInterfaceDetailW(Set.Value, &Interface, nullptr, 0, &Bytes, nullptr);
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || Bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            winrt::throw_last_error();
        auto Buffer = std::make_unique<BYTE[]>(Bytes);
        auto Detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(Buffer.get());
        Detail->cbSize = sizeof(*Detail);
        SP_DEVINFO_DATA Device{};
        Device.cbSize = sizeof(Device);
        winrt::check_bool(::SetupDiGetDeviceInterfaceDetailW(Set.Value, &Interface,
            Detail, Bytes, nullptr, &Device));
        std::wstring Path(Detail->DevicePath);
        constexpr wchar_t Suffix[] = L"\\GPUPARAV";
        constexpr size_t SuffixLength = _countof(Suffix) - 1;
        if (Path.size() < SuffixLength ||
            ::_wcsicmp(Path.c_str() + Path.size() - SuffixLength, Suffix) != 0)
            continue;
        Visit(Set.Value, Device, Path);
    }
}
}

std::vector<NanaBox::HostGpuInfo> NanaBox::EnumeratePartitionableGpus()
{
    std::vector<HostGpuInfo> Result;
    VisitPartitionableGpus([&](HDEVINFO Set, SP_DEVINFO_DATA& Device, std::wstring const& Path)
    {
        Result.push_back({DeviceName(Set, Device), winrt::hstring(Path)});
    });
    return Result;
}


namespace
{
    struct DriverRegistryKey
    {
        HKEY Value = nullptr;
        ~DriverRegistryKey() { if (Value && Value != INVALID_HANDLE_VALUE) ::RegCloseKey(Value); }
        DriverRegistryKey() = default;
        DriverRegistryKey(DriverRegistryKey const&) = delete;
        DriverRegistryKey& operator=(DriverRegistryKey const&) = delete;
    };

    std::wstring RegistryText(HKEY Key, wchar_t const* Name)
    {
        DWORD Bytes = 0;
        if (::RegGetValueW(Key, nullptr, Name, RRF_RT_REG_SZ, nullptr, nullptr, &Bytes) != ERROR_SUCCESS ||
            Bytes < sizeof(wchar_t) || Bytes > 65536) return {};
        std::vector<wchar_t> Data(Bytes / sizeof(wchar_t) + 1);
        if (::RegGetValueW(Key, nullptr, Name, RRF_RT_REG_SZ, nullptr, Data.data(), &Bytes) != ERROR_SUCCESS)
            return {};
        return Data.data();
    }

    std::wstring AdapterService(HDEVINFO Set, SP_DEVINFO_DATA& Device)
    {
        wchar_t Data[512]{};
        if (!::SetupDiGetDeviceRegistryPropertyW(Set, &Device, SPDRP_SERVICE, nullptr,
            reinterpret_cast<PBYTE>(Data), sizeof(Data), nullptr)) return {};
        Data[ARRAYSIZE(Data)-1] = 0;
        return Data;
    }

    std::wstring DriverPackage(HKEY Key)
    {
        auto Inf = RegistryText(Key, L"InfPath");
        if (Inf.empty()) return {};
        wchar_t Store[MAX_PATH]{}, System[MAX_PATH]{};
        if (!::SetupGetInfDriverStoreLocationW(Inf.c_str(), nullptr, nullptr, Store, ARRAYSIZE(Store), nullptr))
            return {};
        auto Length = ::GetSystemDirectoryW(System, ARRAYSIZE(System));
        if (!Length || Length >= ARRAYSIZE(System)) return {};
        std::wstring Root = std::wstring(System) + L"\\DriverStore\\FileRepository\\";
        std::wstring Full(Store);
        auto Slash = Full.find_last_of(L'\\');
        if (Slash == std::wstring::npos || Slash <= Root.size() ||
            ::_wcsnicmp(Full.c_str(), Root.c_str(), Root.size()) != 0) return {};
        auto Package = Full.substr(Root.size(), Slash - Root.size());
        return NanaBox::Wddm::SafeComponent(Package) ? Package : std::wstring();
    }

    void ReadCopyRules(HKEY Key, std::wstring const& Package, nlohmann::json& Result)
    {
        struct Rule { wchar_t const* Name; bool Wow64; bool Newer; };
        constexpr Rule Rules[] = {
            {L"CopyToVmOverwrite", false, false}, {L"CopyToVmWhenNewer", false, true},
            {L"CopyToVmOverwriteWow64", true, false}, {L"CopyToVmWhenNewerWow64", true, true}
        };
        for (auto const& RuleEntry : Rules)
        {
            DriverRegistryKey Sub;
            auto Open = ::RegOpenKeyExW(Key, RuleEntry.Name, 0, KEY_READ, &Sub.Value);
            if (Open == ERROR_FILE_NOT_FOUND) continue;
            if (Open != ERROR_SUCCESS)
            {
                Result["Errors"].push_back("Cannot open GPU copy rule registry key");
                continue;
            }
            for (DWORD Index = 0;; ++Index)
            {
                wchar_t Name[16384]{};
                DWORD NameLength = ARRAYSIZE(Name), Type = 0, Bytes = 0;
                auto Status = ::RegEnumValueW(Sub.Value, Index, Name, &NameLength, nullptr, &Type, nullptr, &Bytes);
                if (Status == ERROR_NO_MORE_ITEMS) break;
                if (Status != ERROR_SUCCESS || (Type != REG_SZ && Type != REG_MULTI_SZ) ||
                    !Bytes || Bytes > 65536 || Bytes % sizeof(wchar_t))
                {
                    Result["Errors"].push_back("Invalid GPU copy rule registry value");
                    if (Status != ERROR_SUCCESS) break;
                    continue;
                }
                std::vector<wchar_t> Data(Bytes / sizeof(wchar_t) + 2);
                if (::RegQueryValueExW(Sub.Value, Name, nullptr, &Type,
                    reinterpret_cast<PBYTE>(Data.data()), &Bytes) != ERROR_SUCCESS ||
                    (Type != REG_SZ && Type != REG_MULTI_SZ))
                {
                    Result["Errors"].push_back("Cannot read GPU copy rule registry value");
                    continue;
                }
                std::vector<std::wstring> Values;
                for (size_t Offset = 0, Count = Bytes / sizeof(wchar_t);
                    Offset < Count && Data[Offset];)
                {
                    std::wstring Entry(Data.data() + Offset);
                    Values.push_back(Entry);
                    Offset += Entry.size() + 1;
                }
                if (Type == REG_SZ && Values.size() != 1)
                {
                    Result["Errors"].push_back("Invalid GPU copy rule string");
                    continue;
                }
                auto Mapping = NanaBox::Wddm::MakeMapping(Package, Values, RuleEntry.Wow64, RuleEntry.Newer);
                if (!Mapping)
                {
                    Result["Errors"].push_back("Invalid GPU copy rule path");
                    continue;
                }
                nlohmann::json Item = {{"Source", winrt::to_string(Mapping->Source)},
                    {"Destination", winrt::to_string(Mapping->Destination)},
                    {"Directory", winrt::to_string(Mapping->Directory)},
                    {"WhenNewer", Mapping->WhenNewer}};
                auto& Files = Result["Files"];
                if (std::find(Files.begin(), Files.end(), Item) == Files.end())
                    Files.push_back(std::move(Item));
            }
        }
    }
}

winrt::hstring NanaBox::EnumerateGpuDriverMappings(std::vector<std::string> const& Selected, bool All)
{
    using Json = nlohmann::json;
    Json Result = {{"Packages", Json::array()}, {"Files", Json::array()}, {"Errors", Json::array()}};
    if (!All && Selected.empty()) return winrt::to_hstring(Result.dump());
    std::set<std::wstring> Packages;
    VisitPartitionableGpus([&](HDEVINFO Set, SP_DEVINFO_DATA& Device, std::wstring const& Path)
    {
        if (!All && std::none_of(Selected.begin(), Selected.end(), [&](std::string const& Id)
            { return ::_wcsicmp(winrt::to_hstring(Id).c_str(), Path.c_str()) == 0; })) return;
        DriverRegistryKey Key;
        Key.Value = ::SetupDiOpenDevRegKey(Set, &Device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (Key.Value == INVALID_HANDLE_VALUE)
        {
            Result["Errors"].push_back("Cannot open GPU adapter software registry key");
            return;
        }
        auto Package = DriverPackage(Key.Value);
        if (Package.empty())
        {
            Result["Errors"].push_back("Cannot resolve installed GPU INF in DriverStore");
            return;
        }
        if (Packages.insert(Wddm::Fold(Package)).second)
            Result["Packages"].push_back(winrt::to_string(Package));
        ReadCopyRules(Key.Value, Package, Result);
        auto Service = AdapterService(Set, Device);
        if (!Wddm::SafeComponent(Service)) return;
        DriverRegistryKey ServiceKey;
        if (::RegOpenKeyExW(Key.Value, Service.c_str(), 0, KEY_READ, &ServiceKey.Value) == ERROR_SUCCESS)
            ReadCopyRules(ServiceKey.Value, Package, Result);
    });
    return winrt::to_hstring(Result.dump());
}

NanaBox::HostNetworkInfo NanaBox::QueryHostNetwork(winrt::guid const& Id)
{
    auto Network = NanaBox::HcnOpenNetwork(Id);
    winrt::cotaskmem_string Result, Error;
    HRESULT Status = ::HcnQueryNetworkProperties(Network.get(), nullptr, Result.put(), Error.put());
    ::CheckHcnCall(Status, Error);
    if (!Result) throw winrt::hresult_error(E_UNEXPECTED);
    winrt::hstring Raw(Result.get());
    auto Json = nlohmann::json::parse(winrt::to_string(Raw));
    return {Id, winrt::to_hstring(Json.value("Name", std::string())),
        winrt::to_hstring(Json.value("Type", std::string())),
        Json.value("Flags", 0u), Raw};
}

std::vector<NanaBox::HostNetworkInfo> NanaBox::EnumerateHostNetworks()
{
    std::vector<HostNetworkInfo> Result;
    for (auto const& Id : ReadHcnIds(true))
    {
        try
        {
            Result.push_back(QueryHostNetwork(winrt::guid(Id.get<std::string>())));
        }
        catch (winrt::hresult_error const& Error)
        {
            if (Error.code() != HCN_E_NETWORK_NOT_FOUND) throw;
        }
    }
    return Result;
}

winrt::hstring NanaBox::BuildManagedNetworkSettings(winrt::hstring const& Name, bool Firewall,
    winrt::hstring const& CustomSettings)
{
    using Json = nlohmann::json;
    std::wstring_view InputName(Name.c_str(), Name.size());
    if (InputName.empty() || InputName.size() > 128 ||
        InputName.find_first_not_of(L" \t\r\n") == std::wstring_view::npos ||
        InputName.find(L'\0') != std::wstring_view::npos)
        throw winrt::hresult_invalid_argument(L"A non-empty network name of at most 128 characters is required.");
    Json Settings;
    auto Text = winrt::to_string(CustomSettings);
    if (Text.size() > 1024 * 1024)
        throw winrt::hresult_invalid_argument(L"HCN network settings exceed 1 MiB.");
    if (Text.find_first_not_of(" \t\r\n") == std::string::npos)
        Settings = {{"Type", "ICS"}, {"IsolateSwitch", true},
            {"Flags", HcnNetworkFlags::EnableDns | HcnNetworkFlags::EnableDhcp}};
    else
    {
        Settings = Json::parse(Text);
        if (!Settings.is_object() || !Settings.contains("Type") ||
            !Settings["Type"].is_string() || Settings["Type"].get<std::string>().empty())
            throw winrt::hresult_invalid_argument(L"HCN network settings must be an object with a non-empty Type.");
        for (auto It = Settings.begin(); It != Settings.end(); ++It)
        {
            const auto Key = winrt::to_hstring(It.key());
            if (_wcsicmp(Key.c_str(), L"Name") == 0 || _wcsicmp(Key.c_str(), L"Id") == 0 ||
                _wcsicmp(Key.c_str(), L"Owner") == 0 ||
                (_wcsicmp(Key.c_str(), L"Flags") == 0 && It.key() != "Flags"))
                throw winrt::hresult_invalid_argument(L"Name, Id and Owner are managed by NanaBox; use the canonical Flags key.");
        }
    }
    std::uint32_t Flags = 0;
    if (Settings.contains("Flags"))
    {
        auto const& Value = Settings["Flags"];
        if (!Value.is_number_integer() || Value < 0 || Value > UINT32_MAX)
            throw winrt::hresult_invalid_argument(L"HCN network Flags must be an unsigned 32-bit integer.");
        Flags = Value.get<std::uint32_t>();
    }
    if (Flags & (HcnNetworkFlags::EnableNonPersistent | HcnNetworkFlags::EnableFlowSteering))
        throw winrt::hresult_invalid_argument(L"Managed networks must be persistent and cannot enable host-controlled flow steering.");
    Flags |= HcnNetworkFlags::EnablePersistent;
    if (Firewall) Flags |= HcnNetworkFlags::EnableFirewall;
    Settings["Flags"] = Flags;
    Settings["Name"] = winrt::to_string(winrt::hstring(ManagedNetworkPrefix)) + winrt::to_string(Name);
    return winrt::to_hstring(Settings.dump());
}

void NanaBox::CreateManagedNetwork(winrt::hstring const& Name, bool Firewall,
    winrt::hstring const& CustomSettings)
{
    auto Settings = BuildManagedNetworkSettings(Name, Firewall, CustomSettings);
    auto FullName = winrt::to_hstring(nlohmann::json::parse(winrt::to_string(Settings))["Name"].get<std::string>());
    for (auto const& Network : EnumerateHostNetworks())
        if (_wcsicmp(Network.Name.c_str(), FullName.c_str()) == 0)
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"Network name already exists.");
    GUID Id{};
    winrt::check_hresult(::CoCreateGuid(&Id));
    NanaBox::HcnCreateNetwork(Id, Settings);
}

void NanaBox::DeleteManagedNetwork(winrt::guid const& Id)
{
    auto Info = QueryHostNetwork(Id);
    if (std::wstring_view(Info.Name.c_str()).find(ManagedNetworkPrefix) != 0)
        throw winrt::hresult_error(E_ACCESSDENIED, L"Only NanaBox-managed networks can be deleted here.");
    for (auto const& Entry : ReadHcnIds(false))
    {
        HcnEndpoint Endpoint;
        winrt::cotaskmem_string Error;
        auto EndpointId = winrt::guid(Entry.get<std::string>());
        HRESULT Status = ::HcnOpenEndpoint(EndpointId, Endpoint.put(), Error.put());
        if (Status == HCN_E_ENDPOINT_NOT_FOUND) continue;
        ::CheckHcnCall(Status, Error);
        auto Properties = nlohmann::json::parse(winrt::to_string(HcnQueryEndpointProperties(Endpoint)));
        auto NetworkId = Properties.value("HostComputeNetwork", std::string());
        if (NetworkId.empty()) NetworkId = Properties.value("VirtualNetwork", std::string());
        if (!NetworkId.empty() && winrt::guid(NetworkId) == Id)
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_BUSY), L"Disconnect all endpoints before deleting the network.");
    }
    winrt::cotaskmem_string Error;
    HRESULT Status = ::HcnDeleteNetwork(Id, Error.put());
    ::CheckHcnCall(Status, Error);
}


namespace
{
    std::wstring EncodeHostCommand(void const* Data, size_t Size)
    {
        if (Size > 1024 * 1024) throw winrt::hresult_invalid_argument();
        DWORD Count = 0;
        auto Bytes = reinterpret_cast<BYTE const*>(Data);
        winrt::check_bool(::CryptBinaryToStringW(Bytes, static_cast<DWORD>(Size),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &Count));
        std::wstring Result(Count, L'\0');
        winrt::check_bool(::CryptBinaryToStringW(Bytes, static_cast<DWORD>(Size),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, Result.data(), &Count));
        Result.resize(std::wcslen(Result.c_str()));
        return Result;
    }

    // Only this fixed command set is executable. User data is supplied as encoded JSON.
    constexpr wchar_t ManagementScript[] = LR"PS(
function ManagedCreator($id) {
    $id = ([guid]$id).ToString('B')
    $c = @(Get-NetFirewallHyperVVMCreator | Where-Object { [guid]$_.VMCreatorId -eq [guid]$id })
    if ($c.Count -ne 1 -or $c[0].FriendlyName -notlike 'NanaBox.Managed.*') {
        throw 'Select a NanaBox-managed firewall creator.'
    }
    return $id
}
function ManagedSwitch($id) {
    $s = @(Get-VMSwitch | Where-Object { $_.Id -eq [guid]$id })
    if ($s.Count -ne 1 -or $s[0].Name -notlike 'NanaBox.Managed.*') {
        throw 'Select a NanaBox-managed switch.'
    }
    return $s[0]
}
function Rule($id, $name) {
    $r = @(Get-NetFirewallHyperVRule -VMCreatorId $id | Where-Object {
        $_.Name -eq $name -and $_.Name -like 'NanaBox.Managed.*'
    })
    if ($r.Count -ne 1) { throw 'Select a NanaBox-managed rule.' }
    return $r[0]
}
try {
    $result = switch ($a.Action) {
        'SwitchList' {
            @(Get-VMSwitch | ForEach-Object {
                [pscustomobject]@{Id=[string]$_.Id; Name=$_.Name; Type=[string]$_.SwitchType; Adapter=$_.NetAdapterInterfaceDescription}
            })
        }
        'SwitchCreate' {
            $name = 'NanaBox.Managed.' + $a.Name.Trim()
            if ($name -eq 'NanaBox.Managed.' -or $a.Name.Length -gt 100) { throw 'Invalid switch name.' }
            if (@(Get-VMSwitch | Where-Object Name -eq $name).Count) { throw 'Switch name already exists.' }
            if ($a.Type -eq 'External') {
                $adapter = @(Get-NetAdapter -Physical | Where-Object Name -eq $a.Adapter)
                if ($adapter.Count -ne 1) { throw 'Specify the exact physical adapter name.' }
                New-VMSwitch -Name $name -NetAdapterName $adapter[0].Name -AllowManagementOS $true | Out-Null
            } elseif ($a.Type -in @('Internal','Private')) {
                New-VMSwitch -Name $name -SwitchType $a.Type | Out-Null
            } else { throw 'Invalid switch type.' }
            @{Success=$true; Name=$name}
        }
        'SwitchDelete' {
            $s = ManagedSwitch $a.Id
            $connected = @(Get-VM | Get-VMNetworkAdapter | Where-Object SwitchId -eq $s.Id)
            if ($connected.Count) { throw 'Disconnect all virtual machine adapters first.' }
            # No force deletion of a switch that may be in use by another workload.
            Remove-VMSwitch -VMSwitch $s -Confirm:$false
            @{Success=$true}
        }
        'AdapterList' {
            @(Get-NetAdapter -Physical | ForEach-Object {
                [pscustomobject]@{Name=$_.Name; Description=$_.InterfaceDescription; Status=[string]$_.Status}
            })
        }
        'CreatorList' {
            @(Get-NetFirewallHyperVVMCreator | ForEach-Object {
                [pscustomobject]@{Id=[string]$_.VMCreatorId; Name=$_.FriendlyName}
            })
        }
        'CreatorRegister' {
            $name = 'NanaBox.Managed.' + $a.Name.Trim()
            if ($name -eq 'NanaBox.Managed.' -or $a.Name.Length -gt 100) { throw 'Invalid creator name.' }
            $id = [guid]::NewGuid().ToString('B')
            $r = Invoke-CimMethod -Namespace root/standardcimv2 -ClassName MSFT_NetFirewallHyperVVMCreator -MethodName RegisterHyperVVMCreator -Arguments @{VMCreatorId=$id;FriendlyName=$name}
            if ($r.ReturnValue -ne 0) { throw ('Registration failed: ' + $r.ReturnValue) }
            @{Id=$id; Name=$name}
        }
        'FirewallInspect' {
            $id = ([guid]$a.Id).ToString('B')
            $setting = @(Get-NetFirewallHyperVVMSetting -Name $id -PolicyStore ActiveStore)
            $rules = @(Get-NetFirewallHyperVRule -VMCreatorId $id | ForEach-Object {
                [pscustomobject]@{Id=$_.Name; Name=$_.DisplayName; Direction=[string]$_.Direction; Action=[string]$_.Action; Protocol=[string]$_.Protocol; LocalPorts=@($_.LocalPorts); RemotePorts=@($_.RemotePorts); Enabled=[string]$_.Enabled}
            })
            @{Settings=@($setting | ForEach-Object {
                [pscustomobject]@{Name=$_.Name; Enabled=[string]$_.Enabled; DefaultInboundAction=[string]$_.DefaultInboundAction; DefaultOutboundAction=[string]$_.DefaultOutboundAction; LoopbackEnabled=[string]$_.LoopbackEnabled; AllowHostPolicyMerge=[string]$_.AllowHostPolicyMerge}
            }); Rules=$rules}
        }
        'FirewallConfigure' {
            $id = ManagedCreator $a.Id
            if ($a.Inbound -notin @('Allow','Block') -or $a.Outbound -notin @('Allow','Block')) { throw 'Invalid firewall action.' }
            Set-NetFirewallHyperVVMSetting -Name $id -Enabled True -DefaultInboundAction $a.Inbound -DefaultOutboundAction $a.Outbound -LoopbackEnabled ([string][bool]$a.Loopback) -AllowHostPolicyMerge ([string][bool]$a.Merge)
            @{Success=$true}
        }
        'RuleCreate' {
            $id = ManagedCreator $a.Id
            if ($a.Name.Trim() -eq '' -or $a.Direction -notin @('Inbound','Outbound') -or $a.RuleAction -notin @('Allow','Block') -or $a.Protocol -notin @('TCP','UDP')) { throw 'Invalid rule.' }
            $ports = @($a.Ports -split '[,\s]+' | Where-Object { $_ })
            if (-not $ports.Count) { throw 'Specify at least one port.' }
            foreach ($port in $ports) {
                if ($port -notmatch '^\d{1,5}$' -or [int]$port -lt 1 -or [int]$port -gt 65535) { throw 'Invalid port.' }
            }
            $name = 'NanaBox.Managed.' + [guid]::NewGuid().ToString()
            $parameters = @{Name=$name; DisplayName=$a.Name; VMCreatorId=$id; Direction=$a.Direction; Action=$a.RuleAction; Protocol=$a.Protocol}
            if ($a.Direction -eq 'Inbound') { $parameters.LocalPorts=$ports } else { $parameters.RemotePorts=$ports }
            New-NetFirewallHyperVRule @parameters | Out-Null
            @{Success=$true; Id=$name}
        }
        'RuleEnable' {
            $id = ManagedCreator $a.Id
            $rule = Rule $id $a.Rule
            Enable-NetFirewallHyperVRule -InputObject $rule | Out-Null
            @{Success=$true}
        }
        'RuleDisable' {
            $id = ManagedCreator $a.Id
            $rule = Rule $id $a.Rule
            Disable-NetFirewallHyperVRule -InputObject $rule | Out-Null
            @{Success=$true}
        }
        'RuleDelete' {
            $id = ManagedCreator $a.Id
            $rule = Rule $id $a.Rule
            Remove-NetFirewallHyperVRule -InputObject $rule -Confirm:$false | Out-Null
            @{Success=$true}
        }
        default { throw 'Unsupported host management operation.' }
    }
    if ($a.Action -in @('SwitchList','AdapterList','CreatorList')) { $result = @($result | Where-Object { $_ -ne $null }) }
    [Console]::WriteLine((ConvertTo-Json -InputObject $result -Depth 8 -Compress))
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
)PS";

    constexpr wchar_t VmmsManagementScript[] = LR"PS(
function Get-SelectedVM($id) {
    $items = @(Get-VM -Id ([guid]$id))
    if ($items.Count -ne 1) { throw 'The selected Hyper-V VM is no longer available.' }
    return $items[0]
}
function Get-SelectedCheckpoint($vm, $id) {
    $items = @(Get-VMSnapshot -VM $vm | Where-Object { $_.Id -eq [guid]$id })
    if ($items.Count -ne 1) { throw 'The selected checkpoint is no longer available.' }
    return $items[0]
}
function RequireName($name) {
    $value = ([string]$name).Trim()
    if (!$value -or $value.Length -gt 100 -or $value.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $value -in @('.','..')) { throw 'Specify a valid name.' }
    return $value
}
function RequireDirectory($path) {
    $item = Get-Item -LiteralPath $path
    if (!$item.PSIsContainer -or $item.PSProvider.Name -ne 'FileSystem') { throw 'Select an existing filesystem directory.' }
    return $item.FullName
}
function RequireConfiguration($path) {
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer -or $item.PSProvider.Name -ne 'FileSystem' -or $item.Extension -notin @('.vmcx','.xml')) { throw 'Select a Hyper-V configuration file (.vmcx or .xml).' }
    return $item.FullName
}
function RequireHost($name) {
    $value = ([string]$name).Trim()
    if ($value.Length -gt 253 -or $value -notmatch '^[A-Za-z0-9][A-Za-z0-9.-]*$') { throw 'Specify a valid destination host name.' }
    return $value
}
function RequireRemotePath($path) {
    $value = ([string]$path).Trim()
    if ($value -notmatch '^(?:[A-Za-z]:\\|\\\\[^\\]+\\[^\\]+)') { throw 'Specify an absolute path on the destination host.' }
    return $value
}
function VmInfo($vm) {
    [pscustomobject]@{
        Id=[string]$vm.Id; Name=$vm.Name; State=[string]$vm.State
        Generation=[int]$vm.Generation; Version=[string]$vm.Version; Path=$vm.Path
        CPUUsage=$vm.CPUUsage; MemoryAssigned=$vm.MemoryAssigned; Uptime=[string]$vm.Uptime
        CheckpointType=[string]$vm.CheckpointType; ReplicationState=[string]$vm.ReplicationState
    }
}
function RequireMedia($path, $extensions) {
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer -or $item.PSProvider.Name -ne 'FileSystem' -or $item.Extension -notin $extensions) { throw 'Choose an existing media file of the required type.' }
    return $item.FullName
}
function RequireSlot($vm, $type, $number, $location) {
    if ($type -notin @('IDE','SCSI') -or ($vm.Generation -eq 2 -and $type -eq 'IDE') -or
        $number -lt 0 -or $number -gt 3 -or $location -lt 0 -or $location -gt 63 -or
        ($type -eq 'IDE' -and ($number -gt 1 -or $location -gt 1))) { throw 'Invalid controller or slot.' }
    $occupied = @(Get-VMHardDiskDrive -VM $vm) + @(Get-VMDvdDrive -VM $vm)
    if (@($occupied | Where-Object { [string]$_.ControllerType -eq $type -and $_.ControllerNumber -eq $number -and $_.ControllerLocation -eq $location }).Count) { throw 'The selected controller slot is occupied.' }
}
function Select-Device($items, $id, $kind) {
    if ([string]::IsNullOrEmpty($id) -or !$id.StartsWith($kind + ':', [StringComparison]::Ordinal)) { throw 'Select the appropriate device first.' }
    $rawId = $id.Substring($kind.Length + 1)
    $matches = @($items | Where-Object { if ($kind -eq 'Service') { $_.Name -ceq $rawId } else { [string]$_.Id -ceq $rawId } })
    if ($matches.Count -ne 1) { throw 'The selected device is no longer available.' }
    return $matches[0]
}
function Describe-Devices($vm) {
    foreach ($disk in @(Get-VMHardDiskDrive -VM $vm)) {
        [pscustomobject]@{Id=('Disk:'+[string]$disk.Id);Name=($disk.ControllerType.ToString()+':'+$disk.ControllerNumber+':'+$disk.ControllerLocation+' '+$disk.Path);Kind='Disk';Path=$disk.Path;ControllerType=[string]$disk.ControllerType;ControllerNumber=$disk.ControllerNumber;ControllerLocation=$disk.ControllerLocation;MinimumIOPS=$disk.MinimumIOPS;MaximumIOPS=$disk.MaximumIOPS}
    }
    foreach ($dvd in @(Get-VMDvdDrive -VM $vm)) {
        [pscustomobject]@{Id=('Dvd:'+[string]$dvd.Id);Name=('DVD '+$dvd.ControllerNumber+':'+$dvd.ControllerLocation+' '+$dvd.Path);Kind='Dvd';Path=$dvd.Path;ControllerType=[string]$dvd.ControllerType;ControllerNumber=$dvd.ControllerNumber;ControllerLocation=$dvd.ControllerLocation}
    }
    foreach ($nic in @(Get-VMNetworkAdapter -VM $vm)) {
        [pscustomobject]@{Id=('Nic:'+[string]$nic.Id);Name=$nic.Name;Kind='Nic';SwitchName=$nic.SwitchName;MacAddress=$nic.MacAddress;DynamicMacAddressEnabled=$nic.DynamicMacAddressEnabled;DhcpGuard=[string]$nic.DhcpGuard;RouterGuard=[string]$nic.RouterGuard;MacAddressSpoofing=[string]$nic.MacAddressSpoofing}
    }
    foreach ($service in @(Get-VMIntegrationService -VM $vm)) {
        [pscustomobject]@{Id=('Service:'+$service.Name);Name=$service.Name;Kind='Service';Enabled=$service.Enabled;Status=$service.PrimaryStatusDescription}
    }
}
function VmDetails($vm) {
    $cpu = Get-VMProcessor -VM $vm
    $memory = Get-VMMemory -VM $vm
    @{
        VM=(VmInfo $vm)
        Devices=@(Describe-Devices $vm)
        Switches=@(Get-VMSwitch | ForEach-Object { [pscustomobject]@{Id=[string]$_.Id;Name=$_.Name;State=[string]$_.SwitchType} })
        Processor=@{Count=$cpu.Count; Maximum=$cpu.Maximum; Reserve=$cpu.Reserve; RelativeWeight=$cpu.RelativeWeight; ExposeVirtualizationExtensions=$cpu.ExposeVirtualizationExtensions}
        Memory=@{Startup=$memory.Startup; Minimum=$memory.Minimum; Maximum=$memory.Maximum; DynamicMemoryEnabled=$memory.DynamicMemoryEnabled; Buffer=$memory.Buffer; Priority=$memory.Priority}
        Checkpoints=@(Get-VMSnapshot -VM $vm | ForEach-Object { [pscustomobject]@{Id=[string]$_.Id; Name=$_.Name; CreationTime=[string]$_.CreationTime; ParentId=[string]$_.ParentSnapshotId; Type=[string]$_.SnapshotType} })
        Disks=@(Get-VMHardDiskDrive -VM $vm | Select-Object ControllerType,ControllerNumber,ControllerLocation,Path,DiskNumber)
        NetworkAdapters=@(Get-VMNetworkAdapter -VM $vm | Select-Object Name,SwitchName,MacAddress,IPAddresses,Status)
        IntegrationServices=@(Get-VMIntegrationService -VM $vm | Select-Object Name,Enabled,PrimaryStatusDescription,SecondaryStatusDescription)
        Replication=@(Get-VMReplication -VM $vm -ErrorAction SilentlyContinue | Select-Object ReplicationMode,ReplicationState,ReplicationHealth,PrimaryServer,ReplicaServer,ReplicaPort,AuthenticationType,FrequencySec,LastReplicationTime)
    }
}
try {
    Import-Module Hyper-V -ErrorAction Stop
    $selected = $null
    $result = @{Success=$true}
    if ($a.Action -eq 'VmmsCreate') {
        $name = RequireName $a.Name
        if (@(Get-VM | Where-Object Name -eq $name).Count) { throw 'A virtual machine with this name already exists.' }
        $generation = [int]$a.Generation
        $memory = [long]$a.MemoryMiB
        if ($generation -notin @(1,2) -or $memory -lt 32 -or $memory -gt 16777216) { throw 'Invalid virtual machine generation or startup memory.' }
        $new = @{Name=$name; Generation=$generation; MemoryStartupBytes=($memory*1MB); NoVHD=$true}
        if ($a.Path) { $new.Path = RequireDirectory $a.Path }
        $vm = New-VM @new
        $selected = [string]$vm.Id
    } elseif ($a.Action -in @('VmmsImportRegister','VmmsImportCopy')) {
        $config = RequireConfiguration $a.Path
        if ($a.Action -eq 'VmmsImportCopy') {
            $destination = RequireDirectory $a.Destination
            if (@(Get-ChildItem -LiteralPath $destination -Force).Count) { throw 'Choose an empty destination directory for the imported copy.' }
            $vm = Import-VM -Path $config -Copy -GenerateNewId -VirtualMachinePath $destination -VhdDestinationPath $destination -SnapshotFilePath $destination -SmartPagingFilePath $destination -Confirm:$false
        } else { $vm = Import-VM -Path $config -Register -Confirm:$false }
        $selected = [string]$vm.Id
    } elseif ($a.Action -ne 'VmmsList') {
        $vm = Get-SelectedVM $a.Id
        $selected = [string]$vm.Id
        switch ($a.Action) {
            'VmmsInspect' {}
            'VmmsStart' { Start-VM -VM $vm | Out-Null }
            'VmmsShutdown' { Stop-VM -VM $vm -Confirm:$false | Out-Null }
            'VmmsPowerOff' { Stop-VM -VM $vm -TurnOff -Force -Confirm:$false | Out-Null }
            'VmmsPause' { Suspend-VM -VM $vm | Out-Null }
            'VmmsResume' { Resume-VM -VM $vm | Out-Null }
            'VmmsSave' { Save-VM -VM $vm | Out-Null }
            'VmmsRestart' { Restart-VM -VM $vm -Confirm:$false | Out-Null }
            'VmmsRename' { Rename-VM -VM $vm -NewName (RequireName $a.Name) | Out-Null }
            'VmmsRemove' {
                if ([string]$vm.State -ne 'Off') { throw 'Shut down the virtual machine before deleting its configuration.' }
                Remove-VM -VM $vm -Confirm:$false
                $selected = $null
            }
            'VmmsCheckpointCreate' { Checkpoint-VM -VM $vm -SnapshotName (RequireName $a.Name) | Out-Null }
            'VmmsCheckpointRestore' { Restore-VMSnapshot -VMSnapshot (Get-SelectedCheckpoint $vm $a.CheckpointId) -Confirm:$false | Out-Null }
            'VmmsCheckpointRemove' { Remove-VMSnapshot -VMSnapshot (Get-SelectedCheckpoint $vm $a.CheckpointId) -Confirm:$false | Out-Null }
            'VmmsExport' { Export-VM -VM $vm -Path (RequireDirectory $a.Path) | Out-Null }
            'VmmsMoveStorage' { Move-VMStorage -VM $vm -DestinationStoragePath (RequireDirectory $a.Path) -Confirm:$false | Out-Null }
            'VmmsSetProcessor' {
                $count = [int]$a.Count
                if ($count -lt 1 -or $count -gt 2048) { throw 'Invalid virtual processor count.' }
                Set-VMProcessor -VM $vm -Count $count -Confirm:$false | Out-Null
            }
            'VmmsSetMemory' {
                $memory = [long]$a.MemoryMiB
                if ($memory -lt 32 -or $memory -gt 16777216) { throw 'Invalid startup memory.' }
                Set-VMMemory -VM $vm -StartupBytes ($memory*1MB) -Confirm:$false | Out-Null
            }
            'VmmsDynamicMemory' {
                $min = [long]$a.MinimumMiB; $startup = [long]$a.MemoryMiB; $max = [long]$a.MaximumMiB
                $buffer = [int]$a.Buffer; $priority = [int]$a.Priority
                if ($min -lt 32 -or $min -gt $startup -or $startup -gt $max -or $max -gt 16777216 -or $buffer -lt 5 -or $buffer -gt 2000 -or $priority -lt 0 -or $priority -gt 100) { throw 'Invalid dynamic memory configuration.' }
                Set-VMMemory -VM $vm -DynamicMemoryEnabled $true -MinimumBytes ($min*1MB) -StartupBytes ($startup*1MB) -MaximumBytes ($max*1MB) -Buffer $buffer -Priority $priority -Confirm:$false | Out-Null
            }
            'VmmsStaticMemory' {
                $memory = [long]$a.MemoryMiB
                if ($memory -lt 32 -or $memory -gt 16777216) { throw 'Invalid startup memory.' }
                Set-VMMemory -VM $vm -DynamicMemoryEnabled $false -StartupBytes ($memory*1MB) -Confirm:$false | Out-Null
            }
            'VmmsMigrate' {
                $destination = RequireHost $a.Host
                $parameters = @{VM=$vm; DestinationHost=$destination; Confirm=$false}
                if ($a.Path) {
                    $parameters.IncludeStorage = $true
                    $parameters.DestinationStoragePath = RequireRemotePath $a.Path
                }
                Move-VM @parameters | Out-Null
                $result.MigratedTo = $destination
                $result.MigratedId = $selected
                $selected = $null
            }
            'VmmsReplicationEnable' {
                $destination = RequireHost $a.Host
                $port = [int]$a.Port; $frequency = [int]$a.Frequency; $history = [int]$a.RecoveryHistory
                if ($port -lt 1 -or $port -gt 65535 -or $frequency -notin @(30,300,900) -or $history -lt 0 -or $history -gt 24 -or $a.Authentication -notin @('Kerberos','Certificate')) { throw 'Invalid replica configuration.' }
                $parameters = @{VM=$vm; ReplicaServerName=$destination; ReplicaServerPort=$port; AuthenticationType=$a.Authentication; ReplicationFrequencySec=$frequency; RecoveryHistory=$history; CompressionEnabled=[bool]$a.Compression; Confirm=$false}
                if ($a.Authentication -eq 'Certificate') {
                    $thumbprint = ([string]$a.Thumbprint) -replace '\s',''
                    if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$') { throw 'Specify a valid certificate thumbprint.' }
                    $parameters.CertificateThumbprint = $thumbprint
                }
                Enable-VMReplication @parameters | Out-Null
            }
            'VmmsReplicationStart' { Start-VMInitialReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsReplicationSuspend' { Suspend-VMReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsReplicationResume' { Resume-VMReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsReplicationRemove' { Remove-VMReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsCpuPolicy' {
                $reserve = [int]$a.Reserve; $maximum = [int]$a.Maximum; $weight = [int]$a.Weight
                if ($reserve -lt 0 -or $maximum -gt 100 -or $maximum -lt $reserve -or $weight -lt 1 -or $weight -gt 10000) { throw 'Invalid processor allocation.' }
                Set-VMProcessor -VM $vm -Reserve $reserve -Maximum $maximum -RelativeWeight $weight -ExposeVirtualizationExtensions ([bool]$a.Nested) -Confirm:$false | Out-Null
            }
            'VmmsDiskAttach' {
                RequireSlot $vm $a.ControllerType ([int]$a.ControllerNumber) ([int]$a.ControllerLocation)
                Add-VMHardDiskDrive -VM $vm -ControllerType $a.ControllerType -ControllerNumber ([int]$a.ControllerNumber) -ControllerLocation ([int]$a.ControllerLocation) -Path (RequireMedia $a.Path @('.vhd','.vhdx','.avhd','.avhdx')) -Confirm:$false | Out-Null
            }
            'VmmsDiskDetach' {
                Remove-VMHardDiskDrive -VMHardDiskDrive (Select-Device @(Get-VMHardDiskDrive -VM $vm) $a.DeviceId Disk) -Confirm:$false | Out-Null
            }
            'VmmsDiskQoS' {
                $minimum = [uint64]$a.MinimumIOPS; $maximum = [uint64]$a.MaximumIOPS
                if ($maximum -ne 0 -and $minimum -gt $maximum) { throw 'Minimum IOPS exceeds maximum IOPS.' }
                Set-VMHardDiskDrive -VMHardDiskDrive (Select-Device @(Get-VMHardDiskDrive -VM $vm) $a.DeviceId Disk) -MinimumIOPS $minimum -MaximumIOPS $maximum -Confirm:$false | Out-Null
            }
            'VmmsDvdAdd' {
                $type = if ($vm.Generation -eq 1) {'IDE'} else {'SCSI'}
                RequireSlot $vm $type ([int]$a.ControllerNumber) ([int]$a.ControllerLocation)
                $p = @{VM=$vm;ControllerNumber=[int]$a.ControllerNumber;ControllerLocation=[int]$a.ControllerLocation;Confirm=$false}
                if ($a.Path) { $p.Path = RequireMedia $a.Path @('.iso') }
                Add-VMDvdDrive @p | Out-Null
            }
            'VmmsDvdMount' {
                $path = if ($a.Path) { RequireMedia $a.Path @('.iso') } else { $null }
                Set-VMDvdDrive -VMDvdDrive (Select-Device @(Get-VMDvdDrive -VM $vm) $a.DeviceId Dvd) -Path $path -Confirm:$false | Out-Null
            }
            'VmmsDvdRemove' {
                Remove-VMDvdDrive -VMDvdDrive (Select-Device @(Get-VMDvdDrive -VM $vm) $a.DeviceId Dvd) -Confirm:$false | Out-Null
            }
            'VmmsNicAdd' {
                $name = RequireName $a.Name
                if (@(Get-VMNetworkAdapter -VM $vm | Where-Object Name -eq $name).Count) { throw 'A network adapter with that name already exists.' }
                Add-VMNetworkAdapter -VM $vm -Name $name -DynamicMacAddress -Confirm:$false | Out-Null
            }
            'VmmsNicRemove' { Remove-VMNetworkAdapter -VMNetworkAdapter (Select-Device @(Get-VMNetworkAdapter -VM $vm) $a.DeviceId Nic) -Confirm:$false | Out-Null }
            'VmmsNicConnect' {
                $switch = @(Get-VMSwitch -Id ([guid]$a.SwitchId))
                if ($switch.Count -ne 1) { throw 'The selected switch is no longer available.' }
                Connect-VMNetworkAdapter -VMNetworkAdapter (Select-Device @(Get-VMNetworkAdapter -VM $vm) $a.DeviceId Nic) -VMSwitch $switch[0] -Confirm:$false | Out-Null
            }
            'VmmsNicDisconnect' { Disconnect-VMNetworkAdapter -VMNetworkAdapter (Select-Device @(Get-VMNetworkAdapter -VM $vm) $a.DeviceId Nic) -Confirm:$false | Out-Null }
            'VmmsNicMac' {
                $nic = Select-Device @(Get-VMNetworkAdapter -VM $vm) $a.DeviceId Nic
                $mac = ([string]$a.Mac) -replace '[:-]',''
                if (!$mac) { Set-VMNetworkAdapter -VMNetworkAdapter $nic -DynamicMacAddress -Confirm:$false | Out-Null }
                else {
                    if ($mac -notmatch '^[0-9a-fA-F]{12}$' -or $mac -eq '000000000000' -or ([Convert]::ToInt32($mac.Substring(0,2),16) -band 1)) { throw 'Specify a nonzero unicast MAC address.' }
                    Set-VMNetworkAdapter -VMNetworkAdapter $nic -StaticMacAddress $mac -Confirm:$false | Out-Null
                }
            }
            'VmmsNicVlan' {
                $nic = Select-Device @(Get-VMNetworkAdapter -VM $vm) $a.DeviceId Nic
                $vlan = [int]$a.Vlan
                if ($vlan -lt 0 -or $vlan -gt 4094) { throw 'Invalid VLAN ID.' }
                if ($vlan -eq 0) { Set-VMNetworkAdapterVlan -VMNetworkAdapter $nic -Untagged -Confirm:$false | Out-Null }
                else { Set-VMNetworkAdapterVlan -VMNetworkAdapter $nic -Access -VlanId $vlan -Confirm:$false | Out-Null }
            }
            'VmmsNicGuard' {
                $nic = Select-Device @(Get-VMNetworkAdapter -VM $vm) $a.DeviceId Nic
                $dhcp = if ($a.DhcpGuard) {'On'} else {'Off'}
                $router = if ($a.RouterGuard) {'On'} else {'Off'}
                $spoof = if ($a.Spoofing) {'On'} else {'Off'}
                Set-VMNetworkAdapter -VMNetworkAdapter $nic -DhcpGuard $dhcp -RouterGuard $router -MacAddressSpoofing $spoof -Confirm:$false | Out-Null
            }
            'VmmsIntegration' {
                $service = Select-Device @(Get-VMIntegrationService -VM $vm) $a.DeviceId Service
                if ($a.Enabled) { Enable-VMIntegrationService -VMIntegrationService $service -Confirm:$false | Out-Null }
                else { Disable-VMIntegrationService -VMIntegrationService $service -Confirm:$false | Out-Null }
            }
            'VmmsSecureBoot' {
                if ($vm.Generation -ne 2) { throw 'Secure Boot requires a Generation 2 VM.' }
                if ($a.Template -notin @('MicrosoftWindows','MicrosoftUEFICertificateAuthority','OpenSourceShieldedVM')) { throw 'Invalid Secure Boot template.' }
                $enabled = if ($a.Enabled) {'On'} else {'Off'}
                Set-VMFirmware -VM $vm -EnableSecureBoot $enabled -SecureBootTemplate $a.Template -Confirm:$false | Out-Null
            }
            default { throw 'Unsupported Hyper-V management operation.' }
        }
    }
    $result.Items = @(Get-VM | ForEach-Object { VmInfo $_ })
    $result.SelectedId = $selected
    $result.Details = $null
    if ($selected) {
        try { $result.Details = VmDetails (Get-SelectedVM $selected) }
        catch {
            # The requested operation may have succeeded even if a later read fails.
            $result.RefreshError = $_.Exception.Message
        }
    }
    [Console]::WriteLine((ConvertTo-Json -InputObject $result -Depth 12 -Compress))
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
)PS";
}

namespace
{
    std::string RunHostScript(std::wstring const& Script, DWORD Timeout)
    {
        auto Encoded = EncodeHostCommand(Script.data(), Script.size() * sizeof(wchar_t));
        constexpr wchar_t Bootstrap[] =
            L"$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';$WarningPreference='SilentlyContinue';"
            L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
            L"try{$scriptText=[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String([Console]::In.ReadToEnd()));"
            L"& ([ScriptBlock]::Create($scriptText));exit ([int]$LASTEXITCODE)}"
            L"catch{[Console]::Error.WriteLine($_.Exception.Message);exit 1}";
        std::wstring Command = L"powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand ";
        Command += EncodeHostCommand(Bootstrap, (ARRAYSIZE(Bootstrap) - 1) * sizeof(wchar_t));
        wchar_t System[MAX_PATH]{}, Temp[MAX_PATH]{}, Name[40]{};
        auto N = ::GetSystemDirectoryW(System, ARRAYSIZE(System));
        if (!N || N >= ARRAYSIZE(System)) winrt::throw_last_error();
        N = ::GetTempPathW(ARRAYSIZE(Temp), Temp);
        if (!N || N >= ARRAYSIZE(Temp)) winrt::throw_last_error();
        GUID Id{};
        winrt::check_hresult(::CoCreateGuid(&Id));
        winrt::check_bool(::StringFromGUID2(Id, Name, ARRAYSIZE(Name)) != 0);
        std::wstring Exe = std::wstring(System) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring File = std::wstring(Temp) + L"NanaBox-" + Name + L".log";
        SECURITY_ATTRIBUTES Security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE RawOutput = ::CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (RawOutput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
        winrt::handle Output(RawOutput);
        std::wstring ErrorFile = std::wstring(Temp) + L"NanaBox-" + Name + L".error";
        HANDLE RawError = ::CreateFileW(ErrorFile.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (RawError == INVALID_HANDLE_VALUE) winrt::throw_last_error();
        winrt::handle ErrorOutput(RawError);
        std::wstring InputFile = std::wstring(Temp) + L"NanaBox-" + Name + L".input";
        HANDLE RawInput = ::CreateFileW(InputFile.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (RawInput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
        winrt::handle Input(RawInput);
        // The encoded script consists entirely of ASCII base64 characters.
        // Keep the process command line independent of the payload length.
        std::string Bytes = winrt::to_string(winrt::hstring(Encoded));
        DWORD Written = 0;
        winrt::check_bool(::WriteFile(Input.get(), Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written, nullptr));
        if (Written != Bytes.size()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        LARGE_INTEGER Beginning{};
        winrt::check_bool(::SetFilePointerEx(Input.get(), Beginning, nullptr, FILE_BEGIN));

        SIZE_T AttributeSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeSize);
        if (!AttributeSize) winrt::throw_last_error();
        auto Attributes = std::make_unique<BYTE[]>(AttributeSize);
        auto AttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(Attributes.get());
        winrt::check_bool(::InitializeProcThreadAttributeList(AttributeList, 1, 0, &AttributeSize));
        std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&::DeleteProcThreadAttributeList)>
            DeleteAttributes(AttributeList, &::DeleteProcThreadAttributeList);
        HANDLE Inherited[] = {Input.get(), Output.get(), ErrorOutput.get()};
        winrt::check_bool(::UpdateProcThreadAttribute(AttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            Inherited, sizeof(Inherited), nullptr, nullptr));

        STARTUPINFOEXW Startup{};
        Startup.StartupInfo.cb = sizeof(Startup);
        Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        Startup.StartupInfo.hStdOutput = Output.get();
        Startup.StartupInfo.hStdError = ErrorOutput.get();
        Startup.StartupInfo.hStdInput = Input.get();
        Startup.lpAttributeList = AttributeList;
        PROCESS_INFORMATION Process{};
        winrt::check_bool(::CreateProcessW(Exe.c_str(), Command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &Startup.StartupInfo, &Process));
        winrt::handle Child(Process.hProcess), Thread(Process.hThread);
        DWORD Wait = ::WaitForSingleObject(Child.get(), Timeout);
        if (Wait == WAIT_TIMEOUT)
        {
            ::TerminateProcess(Child.get(), ERROR_TIMEOUT);
            ::WaitForSingleObject(Child.get(), 5000);
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Host management timed out. Refresh the affected resources before retrying.");
        }
        if (Wait != WAIT_OBJECT_0) winrt::throw_last_error();
        DWORD Exit = 0;
        winrt::check_bool(::GetExitCodeProcess(Child.get(), &Exit));
        auto ReadOutput = [](HANDLE FileHandle)
        {
            LARGE_INTEGER Size{}, Start{};
            winrt::check_bool(::GetFileSizeEx(FileHandle, &Size));
            if (Size.QuadPart < 0 || Size.QuadPart > 4 * 1024 * 1024) throw winrt::hresult_error(E_UNEXPECTED);
            winrt::check_bool(::SetFilePointerEx(FileHandle, Start, nullptr, FILE_BEGIN));
            std::string Result(static_cast<size_t>(Size.QuadPart), '\0');
            DWORD Read = 0;
            if (!Result.empty())
                winrt::check_bool(::ReadFile(FileHandle, Result.data(), static_cast<DWORD>(Result.size()), &Read, nullptr));
            Result.resize(Read);
            return Result;
        };
        auto Result = ReadOutput(Output.get());
        if (Exit != 0)
        {
            auto Error = ReadOutput(ErrorOutput.get());
            if (Error.empty()) Error = Result;
            if (Error.empty()) Error = "Host management exited with code " + std::to_string(Exit);
            throw winrt::hresult_error(E_FAIL, winrt::to_hstring(Error));
        }
        return Result;
    }
}

bool NanaBox::HostManagementSelfTest()
{
    // Exercise the same process, stdin, handle inheritance and UTF-8 output
    // used by both host management backends, beyond the Win32 command limit.
    std::wstring Script = L"$ErrorActionPreference='Stop';"
        L"[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false);"
        L"$value='";
    Script.append(48000, L'x');
    Script += L"';[Console]::Error.WriteLine('diagnostic');[Console]::WriteLine((ConvertTo-Json -InputObject @{Length=$value.Length;Text=[string][char]0x6D4B} -Compress));exit 0";
    auto Result = nlohmann::json::parse(RunHostScript(Script, 30000));
    if (Result["Length"] != 48000 || Result["Text"] != winrt::to_string(winrt::hstring(L"\u6D4B")))
        return false;
    try { (void)RunHostScript(L"[Console]::Error.WriteLine('test failure');exit 7", 30000); }
    catch (winrt::hresult_error const& Error)
    {
        return std::wstring_view(Error.message().c_str()).find(L"test failure") != std::wstring_view::npos;
    }
    return false;
}


namespace
{
    using HcnJson = nlohmann::json;
    constexpr wchar_t ResourceRegistry[] = L"SOFTWARE\\NanaBox\\ManagedHcnResources";

    std::wstring HcnGuidName(GUID const& Id)
    {
        wchar_t Name[40]{};
        winrt::check_bool(::StringFromGUID2(Id, Name, ARRAYSIZE(Name)) != 0);
        return Name;
    }

    winrt::guid ResourceId(HcnJson const& Value)
    {
        auto Id = winrt::guid(Value.get<std::string>());
        if (Id == winrt::guid{}) throw winrt::hresult_invalid_argument(L"An object ID is required.");
        return Id;
    }

    bool RegisteredResource(std::string const& Kind, GUID const& Id)
    {
        wchar_t Value[32]{};
        DWORD Size = sizeof(Value);
        auto Status = ::RegGetValueW(HKEY_LOCAL_MACHINE, ResourceRegistry, HcnGuidName(Id).c_str(),
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, Value, &Size);
        if (Status == ERROR_FILE_NOT_FOUND || Status == ERROR_PATH_NOT_FOUND) return false;
        winrt::check_win32(Status);
        return winrt::to_string(winrt::hstring(Value)) == Kind;
    }

    struct RegistryKey
    {
        HKEY Value = nullptr;
        ~RegistryKey() { if (Value) ::RegCloseKey(Value); }
    };

    void RecordResource(std::string const& Kind, GUID const& Id)
    {
        RegistryKey Key;
        winrt::check_win32(::RegCreateKeyExW(HKEY_LOCAL_MACHINE, ResourceRegistry, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &Key.Value, nullptr));
        auto Value = winrt::to_hstring(Kind);
        winrt::check_win32(::RegSetValueExW(Key.Value, HcnGuidName(Id).c_str(), 0, REG_SZ,
            reinterpret_cast<BYTE const*>(Value.c_str()), static_cast<DWORD>((Value.size() + 1) * sizeof(wchar_t))));
    }

    void ForgetResource(GUID const& Id)
    {
        RegistryKey Key;
        auto Status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, ResourceRegistry, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &Key.Value);
        if (Status == ERROR_FILE_NOT_FOUND) return;
        winrt::check_win32(Status);
        Status = ::RegDeleteValueW(Key.Value, HcnGuidName(Id).c_str());
        if (Status != ERROR_FILE_NOT_FOUND) winrt::check_win32(Status);
    }

    void RequireManagedNetwork(winrt::guid const& Id)
    {
        auto Network = NanaBox::QueryHostNetwork(Id);
        if (!std::wstring_view(Network.Name.c_str()).starts_with(ManagedNetworkPrefix) ||
            (Network.Flags & NanaBox::HcnNetworkFlags::EnableFlowSteering))
            throw winrt::hresult_error(E_ACCESSDENIED, L"Select a NanaBox-managed non-mirrored network.");
    }

    struct HcnResourceApi
    {
        using Enumerate = HRESULT (WINAPI*)(PCWSTR, PWSTR*, PWSTR*);
        using Open = HRESULT (WINAPI*)(REFGUID, void**, PWSTR*);
        using Query = HRESULT (WINAPI*)(void*, PCWSTR, PWSTR*, PWSTR*);
        using Modify = HRESULT (WINAPI*)(void*, PCWSTR, PWSTR*);
        using Create = HRESULT (WINAPI*)(REFGUID, PCWSTR, void**, PWSTR*);
        using Delete = HRESULT (WINAPI*)(REFGUID, PWSTR*);
        using Close = HRESULT (WINAPI*)(void*);

        HMODULE Module = nullptr;
        std::string Suffix, Plural;

        explicit HcnResourceApi(std::string const& Kind)
        {
            if (Kind == "Network") { Suffix = "Network"; Plural = "Networks"; }
            else if (Kind == "Endpoint") { Suffix = "Endpoint"; Plural = "Endpoints"; }
            else if (Kind == "Namespace") { Suffix = "Namespace"; Plural = "Namespaces"; }
            else if (Kind == "LoadBalancer") { Suffix = "LoadBalancer"; Plural = "LoadBalancers"; }
            else if (Kind == "Route") { Suffix = "SdnRoute"; Plural = "SdnRoutes"; }
            else throw winrt::hresult_invalid_argument(L"Unknown HCN resource type.");
            Module = ::LoadLibraryExW(L"computenetwork.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            winrt::check_pointer(Module);
        }

        ~HcnResourceApi() { if (Module) ::FreeLibrary(Module); }
        HcnResourceApi(HcnResourceApi const&) = delete;
        HcnResourceApi& operator=(HcnResourceApi const&) = delete;

        template<typename T> T Import(std::string const& Name) const
        {
            auto Address = ::GetProcAddress(Module, Name.c_str());
            if (!Address) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), winrt::to_hstring(Name));
            return reinterpret_cast<T>(Address);
        }

        struct Handle
        {
            Close Release;
            void* Value = nullptr;
            ~Handle() { if (Value) Release(Value); }
        };

        HcnJson Ids() const
        {
            winrt::cotaskmem_string Result, Error;
            auto Status = Import<Enumerate>("HcnEnumerate" + Plural)(nullptr, Result.put(), Error.put());
            ::CheckHcnCall(Status, Error);
            if (!Result) throw winrt::hresult_error(E_UNEXPECTED);
            auto Values = HcnJson::parse(winrt::to_string(winrt::hstring(Result.get())));
            if (!Values.is_array()) throw winrt::hresult_error(E_UNEXPECTED);
            return Values;
        }

        HcnJson Properties(winrt::guid const& Id) const
        {
            Handle Object{Import<Close>("HcnClose" + Suffix)};
            winrt::cotaskmem_string Result, Error;
            auto Status = Import<Open>("HcnOpen" + Suffix)(Id, &Object.Value, Error.put());
            ::CheckHcnCall(Status, Error);
            Error.close();
            Status = Import<Query>("HcnQuery" + Suffix + "Properties")(Object.Value, nullptr, Result.put(), Error.put());
            ::CheckHcnCall(Status, Error);
            if (!Result) throw winrt::hresult_error(E_UNEXPECTED);
            return HcnJson::parse(winrt::to_string(winrt::hstring(Result.get())));
        }

        void Apply(winrt::guid const& Id, HcnJson const& Data) const
        {
            Handle Object{Import<Close>("HcnClose" + Suffix)};
            winrt::cotaskmem_string Error;
            auto Status = Import<Open>("HcnOpen" + Suffix)(Id, &Object.Value, Error.put());
            ::CheckHcnCall(Status, Error);
            Error.close();
            Status = Import<Modify>("HcnModify" + Suffix)(Object.Value, winrt::to_hstring(Data.dump()).c_str(), Error.put());
            ::CheckHcnCall(Status, Error);
        }

        void Make(winrt::guid const& Id, HcnJson const& Data) const
        {
            Handle Object{Import<Close>("HcnClose" + Suffix)};
            winrt::cotaskmem_string Error;
            auto Status = Import<Create>("HcnCreate" + Suffix)(Id, winrt::to_hstring(Data.dump()).c_str(), &Object.Value, Error.put());
            ::CheckHcnCall(Status, Error);
        }

        void Remove(winrt::guid const& Id) const
        {
            winrt::cotaskmem_string Error;
            auto Status = Import<Delete>("HcnDelete" + Suffix)(Id, Error.put());
            ::CheckHcnCall(Status, Error);
        }
    };

    void CheckHcnKeyCase(HcnJson const& Data, unsigned Depth = 0)
    {
        if (Depth > 64) throw winrt::hresult_invalid_argument(L"HCN settings are nested too deeply.");
        if (Data.is_object())
        {
            std::set<std::wstring> Keys;
            for (auto It = Data.begin(); It != Data.end(); ++It)
            {
                auto WideKey = winrt::to_hstring(It.key());
                std::wstring Key(WideKey.c_str(), WideKey.size());
                if (!Key.empty()) ::CharLowerBuffW(Key.data(), static_cast<DWORD>(Key.size()));
                if (!Keys.insert(Key).second)
                    throw winrt::hresult_invalid_argument(L"HCN settings must not repeat a property with different casing.");
                CheckHcnKeyCase(*It, Depth + 1);
            }
        }
        else if (Data.is_array())
            for (auto const& Item : Data) CheckHcnKeyCase(Item, Depth + 1);
    }

    void CheckResourceSettings(HcnJson const& Data)
    {
        if (!Data.is_object() || Data.dump().size() > 1024 * 1024)
            throw winrt::hresult_invalid_argument(L"Supply a JSON object smaller than 1 MiB.");
        CheckHcnKeyCase(Data);
        for (auto It = Data.begin(); It != Data.end(); ++It)
        {
            auto Key = winrt::to_hstring(It.key());
            if (_wcsicmp(Key.c_str(), L"ID") == 0 || _wcsicmp(Key.c_str(), L"Owner") == 0)
                throw winrt::hresult_invalid_argument(L"Resource ID and owner are assigned by NanaBox.");
        }
    }

    void CheckEndpointReferences(HcnJson const& Data)
    {
        // Load balancers and routes must only reference endpoints created
        // by this resource manager, not VM adapters or system endpoints.
        if (!Data.contains("HostComputeEndpoints") || !Data["HostComputeEndpoints"].is_array() ||
            Data["HostComputeEndpoints"].empty())
            throw winrt::hresult_invalid_argument(L"HostComputeEndpoints must contain managed endpoint IDs.");
        for (auto const& Value : Data["HostComputeEndpoints"])
            if (!RegisteredResource("Endpoint", ResourceId(Value)))
                throw winrt::hresult_error(E_ACCESSDENIED, L"Referenced endpoint is not managed by NanaBox.");
    }

    HcnJson HcnManagement(std::string const& Action, HcnJson const& Arguments)
    {
        auto Kind = Arguments.at("Kind").get<std::string>();
        HcnResourceApi Api(Kind);
        if (Action == "HcnList")
        {
            auto Items = HcnJson::array();
            for (auto const& Value : Api.Ids())
            {
                auto Id = ResourceId(Value);
                auto IdText = winrt::to_string(winrt::hstring(HcnGuidName(Id)));
                HcnJson Item = {{"Id", IdText}, {"Kind", Kind}};
                try
                {
                    auto Properties = Api.Properties(Id);
                    auto Name = Properties.value("Name", Kind);
                    bool Managed = Kind == "Network"
                        ? std::wstring_view(winrt::to_hstring(Name).c_str()).starts_with(ManagedNetworkPrefix)
                        : RegisteredResource(Kind, Id);
                    Item["Managed"] = Managed;
                    Item["Name"] = Name + (Managed ? " [managed]" : " [read-only]");
                }
                catch (winrt::hresult_error const& Error)
                {
                    Item["Name"] = Kind + " [unavailable]";
                    Item["Managed"] = false;
                    Item["Error"] = winrt::to_string(Error.message());
                }
                Items.push_back(std::move(Item));
            }
            return {{"Items", std::move(Items)}};
        }
        if (Action == "HcnCreate")
        {
            auto Data = Arguments.at("Settings");
            CheckResourceSettings(Data);
            if (!Data.contains("SchemaVersion")) Data["SchemaVersion"] = {{"Major", 2}, {"Minor", 0}};
            CheckHcnKeyCase(Data);
            GUID NewId{};
            winrt::check_hresult(::CoCreateGuid(&NewId));
            auto Id = winrt::guid(NewId);
            if (Kind == "Network")
            {
                auto Name = Arguments.at("Name").get<std::string>();
                auto Settings = NanaBox::BuildManagedNetworkSettings(winrt::to_hstring(Name), false, winrt::to_hstring(Data.dump()));
                auto FullName = winrt::to_hstring(HcnJson::parse(winrt::to_string(Settings)).at("Name").get<std::string>());
                for (auto const& Network : NanaBox::EnumerateHostNetworks())
                    if (_wcsicmp(Network.Name.c_str(), FullName.c_str()) == 0)
                        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"Network name already exists.");
                NanaBox::HcnCreateNetwork(Id, Settings);
            }
            else
            {
                if (Kind == "Endpoint")
                {
                    auto NetworkId = ResourceId(Arguments.at("NetworkId"));
                    RequireManagedNetwork(NetworkId);
                    for (auto It = Data.begin(); It != Data.end(); ++It)
                    {
                        auto Key = winrt::to_hstring(It.key());
                        if (_wcsicmp(Key.c_str(), L"HostComputeNetwork") == 0 ||
                            _wcsicmp(Key.c_str(), L"HostComputeNamespace") == 0 ||
                            _wcsicmp(Key.c_str(), L"VirtualNetwork") == 0 ||
                            _wcsicmp(Key.c_str(), L"VirtualMachine") == 0)
                            throw winrt::hresult_invalid_argument(L"Endpoint associations are managed separately.");
                    }
                    Data["HostComputeNetwork"] = winrt::to_string(winrt::hstring(HcnGuidName(NetworkId)));
                    Data["Owner"] = "NanaBox.ResourceManager";
                    auto Network = NanaBox::HcnOpenNetwork(NetworkId);
                    NanaBox::HcnCreateEndpoint(Network, Id, winrt::to_hstring(Data.dump()));
                }
                else
                {
                    if (Kind == "Namespace")
                    {
                        auto Type = Data.value("Type", std::string("Host"));
                        if (Type != "Host" && Type != "Guest")
                            throw winrt::hresult_invalid_argument(L"Only non-default namespaces can be created.");
                        for (auto It = Data.begin(); It != Data.end(); ++It)
                        {
                            auto Key = winrt::to_hstring(It.key());
                            if (_wcsicmp(Key.c_str(), L"NamespaceId") == 0 ||
                                (_wcsicmp(Key.c_str(), L"Resources") == 0 && !It->empty()))
                                throw winrt::hresult_invalid_argument(L"Create an empty namespace and attach endpoints separately.");
                        }
                        Data["Type"] = Type;
                        CheckHcnKeyCase(Data);
                    }
                    else CheckEndpointReferences(Data);
                    Api.Make(Id, Data);
                }
                try { RecordResource(Kind, Id); }
                catch (...) { try { Api.Remove(Id); } catch (...) {} throw; }
            }
            return {{"Id", winrt::to_string(winrt::hstring(HcnGuidName(Id)))}, {"Kind", Kind}};
        }
        auto Id = ResourceId(Arguments.at("Id"));
        auto IdText = winrt::to_string(winrt::hstring(HcnGuidName(Id)));
        if (Action == "HcnInspect")
        {
            auto Properties = Api.Properties(Id);
            bool Managed = Kind == "Network"
                ? std::wstring_view(winrt::to_hstring(Properties.value("Name", std::string())).c_str()).starts_with(ManagedNetworkPrefix)
                : RegisteredResource(Kind, Id);
            return {{"Id", IdText}, {"Kind", Kind}, {"Managed", Managed}, {"Properties", std::move(Properties)}};
        }
        if (Kind == "Network") RequireManagedNetwork(Id);
        else if (!RegisteredResource(Kind, Id))
            throw winrt::hresult_error(E_ACCESSDENIED, L"Select a resource created by the NanaBox resource manager.");
        if (Action == "HcnDelete")
        {
            if (Kind == "Network") NanaBox::DeleteManagedNetwork(Id);
            else
            {
                if (Kind == "Namespace" && !Api.Properties(Id).value("Resources", HcnJson::array()).empty())
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_BUSY), L"Remove namespace resources before deleting it.");
                Api.Remove(Id);
                ForgetResource(Id);
            }
        }
        else if (Action == "HcnModify")
        {
            auto Data = Arguments.at("Settings");
            CheckResourceSettings(Data);
            if (Kind == "Route")
                throw winrt::hresult_error(E_NOTIMPL, L"HCN SDN routes must be deleted and recreated to change their settings.");
            if (Kind == "LoadBalancer") CheckEndpointReferences(Data);
            else
            {
                auto Resource = Data.at("ResourceType").get<std::string>();
                auto Request = Data.at("RequestType").get<std::string>();
                if ((Request != "Add" && Request != "Remove" && Request != "Update") ||
                    (Kind == "Namespace" ? Resource != "Endpoint" : Resource != "Policy"))
                    throw winrt::hresult_invalid_argument(L"Use Policy or Endpoint modification requests for this resource type.");
                if (Kind == "Namespace")
                {
                    if (Request == "Update") throw winrt::hresult_invalid_argument(L"Use Add or Remove for namespace membership.");
                    auto Endpoint = ResourceId(Data.at("Settings").at("EndpointId"));
                    if (!RegisteredResource("Endpoint", Endpoint))
                        throw winrt::hresult_error(E_ACCESSDENIED, L"Select an endpoint created by the resource manager.");
                    Data = {{"ResourceType", Resource}, {"RequestType", Request},
                        {"Settings", {{"EndpointId", winrt::to_string(winrt::hstring(HcnGuidName(Endpoint)))}}}};
                }
                else if (!Data.at("Settings").is_object() || !Data.at("Settings").contains("Policies") ||
                    !Data.at("Settings").at("Policies").is_array())
                    throw winrt::hresult_invalid_argument(L"Settings.Policies must be an array.");
            }
            Api.Apply(Id, Data);
        }
        else throw winrt::hresult_invalid_argument();
        return {{"Id", IdText}, {"Kind", Kind}};
    }
}

winrt::hstring NanaBox::RunHostManagement(winrt::hstring const& Action, winrt::hstring const& Arguments)
{
    auto Data = nlohmann::json::parse(winrt::to_string(Arguments));
    if (!Data.is_object()) throw winrt::hresult_invalid_argument();
    Data["Action"] = winrt::to_string(Action);
    if (std::wstring_view(Action.c_str()).starts_with(L"Hcn"))
        return winrt::to_hstring(HcnManagement(winrt::to_string(Action), Data).dump());
    if (Action == L"HcsInventory")
    {
        NanaBox::HcsOperation Operation(::HcsCreateOperation(nullptr, nullptr));
        winrt::check_pointer(Operation.get());
        winrt::check_hresult(::HcsEnumerateComputeSystems(nullptr, Operation.get()));
        auto Items = nlohmann::json::parse(winrt::to_string(::WaitForOperationResult(Operation)));
        if (!Items.is_array()) throw winrt::hresult_error(E_UNEXPECTED);
        return winrt::to_hstring(nlohmann::json{{"Items", std::move(Items)}}.dump());
    }
    bool Vmms = std::wstring_view(Action.c_str()).starts_with(L"Vmms");
    auto Payload = Data.dump();
    std::wstring Script = L"$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';"
        L"$WarningPreference='SilentlyContinue';[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false);"
        L"$a=ConvertFrom-Json ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('";
    Script += EncodeHostCommand(Payload.data(), Payload.size());
    Script += L"')));";
    Script += Vmms ? VmmsManagementScript : ManagementScript;
    return winrt::to_hstring(nlohmann::json::parse(RunHostScript(Script, Vmms ? 30u * 60u * 1000u : 60000u)).dump());
}
