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

#include <Mile.Json.h>
#include <SetupAPI.h>
#include <memory>
#include <string_view>
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
            Query.c_str(),
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

std::vector<NanaBox::HostGpuInfo> NanaBox::EnumeratePartitionableGpus()
{
    DeviceInfoSet Set{::SetupDiGetClassDevsW(&GpuInterfaceClass, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
    winrt::check_bool(Set.Value != INVALID_HANDLE_VALUE);
    std::vector<HostGpuInfo> Result;
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
        Result.push_back({DeviceName(Set.Value, Device), winrt::hstring(Path)});
    }
    return Result;
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

void NanaBox::CreateManagedNetwork(winrt::hstring const& Name, bool Firewall)
{
    if (Name.empty()) throw winrt::hresult_invalid_argument();
    GUID Id{};
    winrt::check_hresult(::CoCreateGuid(&Id));
    nlohmann::json Settings = {
        {"Name", winrt::to_string(winrt::hstring(ManagedNetworkPrefix)) + winrt::to_string(Name)},
        {"Type", "ICS"}, {"IsolateSwitch", true},
        {"Flags", HcnNetworkFlags::EnableDns | HcnNetworkFlags::EnableDhcp |
            HcnNetworkFlags::EnablePersistent | (Firewall ? HcnNetworkFlags::EnableFirewall : 0)}
    };
    NanaBox::HcnCreateNetwork(Id, winrt::to_hstring(Settings.dump()));
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
