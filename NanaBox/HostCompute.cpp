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
#include <wincrypt.h>
#include <cwchar>
#include <string>
#pragma comment(lib, "crypt32.lib")
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
    if ($value.Length -gt 253 -or $value -notmatch '^[A-Za-z0-9][A-Za-z0-9.-]*
    [pscustomobject]@{
        Id=[string]$vm.Id; Name=$vm.Name; State=[string]$vm.State
        Generation=[int]$vm.Generation; Version=[string]$vm.Version; Path=$vm.Path
        CPUUsage=$vm.CPUUsage; MemoryAssigned=$vm.MemoryAssigned; Uptime=[string]$vm.Uptime
        CheckpointType=[string]$vm.CheckpointType; ReplicationState=[string]$vm.ReplicationState
    }
}
function VmDetails($vm) {
    $cpu = Get-VMProcessor -VM $vm
    $memory = Get-VMMemory -VM $vm
    @{
        VM=(VmInfo $vm)
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
                    if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}
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

winrt::hstring NanaBox::RunHostManagement(winrt::hstring const& Action, winrt::hstring const& Arguments)
{
    auto Data = nlohmann::json::parse(winrt::to_string(Arguments));
    if (!Data.is_object()) throw winrt::hresult_invalid_argument();
    Data["Action"] = winrt::to_string(Action);
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
    std::wstring Command = L"powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand ";
    Command += EncodeHostCommand(Script.data(), Script.size() * sizeof(wchar_t));
    if (Command.size() > 32000) throw winrt::hresult_invalid_argument();

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
    HANDLE RawOutput = ::CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (RawOutput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Output(RawOutput);
    HANDLE RawInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &Security, OPEN_EXISTING, 0, nullptr);
    if (RawInput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Input(RawInput);

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdOutput = Startup.hStdError = Output.get();
    Startup.hStdInput = Input.get();
    PROCESS_INFORMATION Process{};
    winrt::check_bool(::CreateProcessW(Exe.c_str(), Command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &Startup, &Process));
    winrt::handle Child(Process.hProcess), Thread(Process.hThread);
    DWORD Wait = ::WaitForSingleObject(Child.get(), Vmms ? 30u * 60u * 1000u : 60000u);
    if (Wait == WAIT_TIMEOUT)
    {
        ::TerminateProcess(Child.get(), ERROR_TIMEOUT);
        ::WaitForSingleObject(Child.get(), 5000);
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Host management timed out. Refresh the affected resources before retrying.");
    }
    if (Wait != WAIT_OBJECT_0) winrt::throw_last_error();
    DWORD Exit = 0;
    winrt::check_bool(::GetExitCodeProcess(Child.get(), &Exit));
    LARGE_INTEGER Size{}, Start{};
    winrt::check_bool(::GetFileSizeEx(Output.get(), &Size));
    if (Size.QuadPart < 0 || Size.QuadPart > 4 * 1024 * 1024) throw winrt::hresult_error(E_UNEXPECTED);
    winrt::check_bool(::SetFilePointerEx(Output.get(), Start, nullptr, FILE_BEGIN));
    std::string Result(static_cast<size_t>(Size.QuadPart), '\0');
    DWORD Read = 0;
    if (!Result.empty())
        winrt::check_bool(::ReadFile(Output.get(), Result.data(), static_cast<DWORD>(Result.size()), &Read, nullptr));
    Result.resize(Read);
    if (Exit != 0)
        throw winrt::hresult_error(E_FAIL, winrt::to_hstring(Result.empty() ? "Host management failed." : Result));
    return winrt::to_hstring(nlohmann::json::parse(Result).dump());
}
) { throw 'Specify a valid destination host name.' }
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
function VmDetails($vm) {
    $cpu = Get-VMProcessor -VM $vm
    $memory = Get-VMMemory -VM $vm
    @{
        VM=(VmInfo $vm)
        Processor=@{Count=$cpu.Count; Maximum=$cpu.Maximum; Reserve=$cpu.Reserve; RelativeWeight=$cpu.RelativeWeight; ExposeVirtualizationExtensions=$cpu.ExposeVirtualizationExtensions}
        Memory=@{Startup=$memory.Startup; Minimum=$memory.Minimum; Maximum=$memory.Maximum; DynamicMemoryEnabled=$memory.DynamicMemoryEnabled; Buffer=$memory.Buffer; Priority=$memory.Priority}
        Checkpoints=@(Get-VMSnapshot -VM $vm | ForEach-Object { [pscustomobject]@{Id=[string]$_.Id; Name=$_.Name; CreationTime=[string]$_.CreationTime; ParentId=[string]$_.ParentSnapshotId; Type=[string]$_.SnapshotType} })
        Disks=@(Get-VMHardDiskDrive -VM $vm | Select-Object ControllerType,ControllerNumber,ControllerLocation,Path,DiskNumber)
        NetworkAdapters=@(Get-VMNetworkAdapter -VM $vm | Select-Object Name,SwitchName,MacAddress,IPAddresses,Status)
        IntegrationServices=@(Get-VMIntegrationService -VM $vm | Select-Object Name,Enabled,PrimaryStatusDescription,SecondaryStatusDescription)
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

winrt::hstring NanaBox::RunHostManagement(winrt::hstring const& Action, winrt::hstring const& Arguments)
{
    auto Data = nlohmann::json::parse(winrt::to_string(Arguments));
    if (!Data.is_object()) throw winrt::hresult_invalid_argument();
    Data["Action"] = winrt::to_string(Action);
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
    std::wstring Command = L"powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand ";
    Command += EncodeHostCommand(Script.data(), Script.size() * sizeof(wchar_t));
    if (Command.size() > 32000) throw winrt::hresult_invalid_argument();

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
    HANDLE RawOutput = ::CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (RawOutput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Output(RawOutput);
    HANDLE RawInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &Security, OPEN_EXISTING, 0, nullptr);
    if (RawInput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Input(RawInput);

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdOutput = Startup.hStdError = Output.get();
    Startup.hStdInput = Input.get();
    PROCESS_INFORMATION Process{};
    winrt::check_bool(::CreateProcessW(Exe.c_str(), Command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &Startup, &Process));
    winrt::handle Child(Process.hProcess), Thread(Process.hThread);
    DWORD Wait = ::WaitForSingleObject(Child.get(), Vmms ? 30u * 60u * 1000u : 60000u);
    if (Wait == WAIT_TIMEOUT)
    {
        ::TerminateProcess(Child.get(), ERROR_TIMEOUT);
        ::WaitForSingleObject(Child.get(), 5000);
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Host management timed out. Refresh the affected resources before retrying.");
    }
    if (Wait != WAIT_OBJECT_0) winrt::throw_last_error();
    DWORD Exit = 0;
    winrt::check_bool(::GetExitCodeProcess(Child.get(), &Exit));
    LARGE_INTEGER Size{}, Start{};
    winrt::check_bool(::GetFileSizeEx(Output.get(), &Size));
    if (Size.QuadPart < 0 || Size.QuadPart > 4 * 1024 * 1024) throw winrt::hresult_error(E_UNEXPECTED);
    winrt::check_bool(::SetFilePointerEx(Output.get(), Start, nullptr, FILE_BEGIN));
    std::string Result(static_cast<size_t>(Size.QuadPart), '\0');
    DWORD Read = 0;
    if (!Result.empty())
        winrt::check_bool(::ReadFile(Output.get(), Result.data(), static_cast<DWORD>(Result.size()), &Read, nullptr));
    Result.resize(Read);
    if (Exit != 0)
        throw winrt::hresult_error(E_FAIL, winrt::to_hstring(Result.empty() ? "Host management failed." : Result));
    return winrt::to_hstring(nlohmann::json::parse(Result).dump());
}
) { throw 'Specify a valid certificate thumbprint.' }
                    $parameters.CertificateThumbprint = $thumbprint
                }
                Enable-VMReplication @parameters | Out-Null
            }
            'VmmsReplicationStart' { Start-VMInitialReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsReplicationSuspend' { Suspend-VMReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsReplicationResume' { Resume-VMReplication -VM $vm -Confirm:$false | Out-Null }
            'VmmsReplicationRemove' { Remove-VMReplication -VM $vm -Confirm:$false | Out-Null }
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

winrt::hstring NanaBox::RunHostManagement(winrt::hstring const& Action, winrt::hstring const& Arguments)
{
    auto Data = nlohmann::json::parse(winrt::to_string(Arguments));
    if (!Data.is_object()) throw winrt::hresult_invalid_argument();
    Data["Action"] = winrt::to_string(Action);
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
    std::wstring Command = L"powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand ";
    Command += EncodeHostCommand(Script.data(), Script.size() * sizeof(wchar_t));
    if (Command.size() > 32000) throw winrt::hresult_invalid_argument();

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
    HANDLE RawOutput = ::CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (RawOutput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Output(RawOutput);
    HANDLE RawInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &Security, OPEN_EXISTING, 0, nullptr);
    if (RawInput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Input(RawInput);

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdOutput = Startup.hStdError = Output.get();
    Startup.hStdInput = Input.get();
    PROCESS_INFORMATION Process{};
    winrt::check_bool(::CreateProcessW(Exe.c_str(), Command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &Startup, &Process));
    winrt::handle Child(Process.hProcess), Thread(Process.hThread);
    DWORD Wait = ::WaitForSingleObject(Child.get(), Vmms ? 30u * 60u * 1000u : 60000u);
    if (Wait == WAIT_TIMEOUT)
    {
        ::TerminateProcess(Child.get(), ERROR_TIMEOUT);
        ::WaitForSingleObject(Child.get(), 5000);
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Host management timed out. Refresh the affected resources before retrying.");
    }
    if (Wait != WAIT_OBJECT_0) winrt::throw_last_error();
    DWORD Exit = 0;
    winrt::check_bool(::GetExitCodeProcess(Child.get(), &Exit));
    LARGE_INTEGER Size{}, Start{};
    winrt::check_bool(::GetFileSizeEx(Output.get(), &Size));
    if (Size.QuadPart < 0 || Size.QuadPart > 4 * 1024 * 1024) throw winrt::hresult_error(E_UNEXPECTED);
    winrt::check_bool(::SetFilePointerEx(Output.get(), Start, nullptr, FILE_BEGIN));
    std::string Result(static_cast<size_t>(Size.QuadPart), '\0');
    DWORD Read = 0;
    if (!Result.empty())
        winrt::check_bool(::ReadFile(Output.get(), Result.data(), static_cast<DWORD>(Result.size()), &Read, nullptr));
    Result.resize(Read);
    if (Exit != 0)
        throw winrt::hresult_error(E_FAIL, winrt::to_hstring(Result.empty() ? "Host management failed." : Result));
    return winrt::to_hstring(nlohmann::json::parse(Result).dump());
}
) { throw 'Specify a valid destination host name.' }
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
function VmDetails($vm) {
    $cpu = Get-VMProcessor -VM $vm
    $memory = Get-VMMemory -VM $vm
    @{
        VM=(VmInfo $vm)
        Processor=@{Count=$cpu.Count; Maximum=$cpu.Maximum; Reserve=$cpu.Reserve; RelativeWeight=$cpu.RelativeWeight; ExposeVirtualizationExtensions=$cpu.ExposeVirtualizationExtensions}
        Memory=@{Startup=$memory.Startup; Minimum=$memory.Minimum; Maximum=$memory.Maximum; DynamicMemoryEnabled=$memory.DynamicMemoryEnabled; Buffer=$memory.Buffer; Priority=$memory.Priority}
        Checkpoints=@(Get-VMSnapshot -VM $vm | ForEach-Object { [pscustomobject]@{Id=[string]$_.Id; Name=$_.Name; CreationTime=[string]$_.CreationTime; ParentId=[string]$_.ParentSnapshotId; Type=[string]$_.SnapshotType} })
        Disks=@(Get-VMHardDiskDrive -VM $vm | Select-Object ControllerType,ControllerNumber,ControllerLocation,Path,DiskNumber)
        NetworkAdapters=@(Get-VMNetworkAdapter -VM $vm | Select-Object Name,SwitchName,MacAddress,IPAddresses,Status)
        IntegrationServices=@(Get-VMIntegrationService -VM $vm | Select-Object Name,Enabled,PrimaryStatusDescription,SecondaryStatusDescription)
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

winrt::hstring NanaBox::RunHostManagement(winrt::hstring const& Action, winrt::hstring const& Arguments)
{
    auto Data = nlohmann::json::parse(winrt::to_string(Arguments));
    if (!Data.is_object()) throw winrt::hresult_invalid_argument();
    Data["Action"] = winrt::to_string(Action);
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
    std::wstring Command = L"powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand ";
    Command += EncodeHostCommand(Script.data(), Script.size() * sizeof(wchar_t));
    if (Command.size() > 32000) throw winrt::hresult_invalid_argument();

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
    HANDLE RawOutput = ::CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &Security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (RawOutput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Output(RawOutput);
    HANDLE RawInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &Security, OPEN_EXISTING, 0, nullptr);
    if (RawInput == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle Input(RawInput);

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdOutput = Startup.hStdError = Output.get();
    Startup.hStdInput = Input.get();
    PROCESS_INFORMATION Process{};
    winrt::check_bool(::CreateProcessW(Exe.c_str(), Command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &Startup, &Process));
    winrt::handle Child(Process.hProcess), Thread(Process.hThread);
    DWORD Wait = ::WaitForSingleObject(Child.get(), Vmms ? 30u * 60u * 1000u : 60000u);
    if (Wait == WAIT_TIMEOUT)
    {
        ::TerminateProcess(Child.get(), ERROR_TIMEOUT);
        ::WaitForSingleObject(Child.get(), 5000);
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Host management timed out. Refresh the affected resources before retrying.");
    }
    if (Wait != WAIT_OBJECT_0) winrt::throw_last_error();
    DWORD Exit = 0;
    winrt::check_bool(::GetExitCodeProcess(Child.get(), &Exit));
    LARGE_INTEGER Size{}, Start{};
    winrt::check_bool(::GetFileSizeEx(Output.get(), &Size));
    if (Size.QuadPart < 0 || Size.QuadPart > 4 * 1024 * 1024) throw winrt::hresult_error(E_UNEXPECTED);
    winrt::check_bool(::SetFilePointerEx(Output.get(), Start, nullptr, FILE_BEGIN));
    std::string Result(static_cast<size_t>(Size.QuadPart), '\0');
    DWORD Read = 0;
    if (!Result.empty())
        winrt::check_bool(::ReadFile(Output.get(), Result.data(), static_cast<DWORD>(Result.size()), &Read, nullptr));
    Result.resize(Read);
    if (Exit != 0)
        throw winrt::hresult_error(E_FAIL, winrt::to_hstring(Result.empty() ? "Host management failed." : Result));
    return winrt::to_hstring(nlohmann::json::parse(Result).dump());
}
