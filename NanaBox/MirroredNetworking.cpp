#include <winsock2.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <sddl.h>
#include <ShlObj.h>
#include <knownfolders.h>
#include <Mile.Json.h>
#include "MirroredNetworking.h"
#include "HostCompute.h"
#include "Utils.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    using Json = nlohmann::json;
    using namespace NanaBox;
    constexpr GUID GnsServiceId{27453, 0xfacb, 0x11e6, {0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3}};
    constexpr std::uint32_t MaxMessage = 256 * 1024;

    std::string GuidString(GUID const& Value)
    {
        return winrt::to_string(::FromGuid(Value));
    }

    struct HcnText
    {
        PWSTR Value = nullptr;
        ~HcnText() { ::CoTaskMemFree(Value); }
        HcnText() = default;
        HcnText(HcnText const&) = delete;
        HcnText& operator=(HcnText const&) = delete;
        std::string String() const { return Value ? winrt::to_string(winrt::hstring(Value)) : std::string(); }
    };

    void CheckHcn(HRESULT Result, HcnText const& Error)
    {
        if (FAILED(Result))
            throw winrt::hresult_error(Result, Error.Value ? winrt::hstring(Error.Value) : winrt::hstring(L"HCN operation failed"));
    }

    std::vector<winrt::guid> MirroredNetworks(bool Firewall)
    {
        unsigned Flags = HcnNetworkFlags::EnableNonPersistent | HcnNetworkFlags::EnableFlowSteering;
        if (Firewall) Flags |= HcnNetworkFlags::EnableFirewall;
        Json Query = {{"Flags", 0}, {"Filter", Json({{"Flags", Flags}}).dump()}};
        HcnText Result, Error;
        auto Text = winrt::to_hstring(Query.dump());
        auto Hr = ::HcnEnumerateNetworks(Text.c_str(), &Result.Value, &Error.Value);
        CheckHcn(Hr, Error);
        auto Ids = Json::parse(Result.String());
        if (!Ids.is_array()) throw winrt::hresult_error(E_UNEXPECTED);
        std::vector<winrt::guid> Networks;
        for (auto const& Id : Ids)
        {
            auto Guid = winrt::guid(Id.get<std::string>());
            auto Properties = QueryHostNetwork(Guid);
            if ((Properties.Flags & Flags) == Flags) Networks.push_back(Guid);
        }
        return Networks;
    }

    void ModifyVm(HCS_SYSTEM Vm, std::string const& Path, char const* Operation, Json const& Settings)
    {
        HcsOperation Handle;
        Handle.attach(::HcsCreateOperation(nullptr, nullptr));
        winrt::check_pointer(Handle.get());
        Json Request = {{"ResourcePath", Path}, {"RequestType", Operation}, {"Settings", Settings}};
        auto Text = winrt::to_hstring(Request.dump());
        winrt::check_hresult(::HcsModifyComputeSystem(Vm, Handle.get(), Text.c_str(), nullptr));
        PWSTR Result = nullptr;
        HRESULT Hr = ::HcsWaitForOperationResult(Handle.get(), 15000, &Result);
        auto Free = Mile::ScopeExitTaskHandler([&]() { ::LocalFree(Result); });
        if (FAILED(Hr)) throw winrt::hresult_error(Hr, Result ? winrt::hstring(Result) : winrt::hstring(L"HCS operation failed"));
    }

    struct Socket
    {
        SOCKET Value = INVALID_SOCKET;
        ~Socket() { if (Value != INVALID_SOCKET) ::closesocket(Value); }
    };

    void CheckSocket(int Status)
    {
        if (Status == SOCKET_ERROR)
            winrt::throw_hresult(HRESULT_FROM_WIN32(::WSAGetLastError()));
    }

    using Deadline = std::chrono::steady_clock::time_point;

    void WaitSocket(SOCKET S, bool Writing, Deadline End, std::atomic<bool> const& Stop)
    {
        while (!Stop.load())
        {
            if (std::chrono::steady_clock::now() >= End)
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            fd_set Ready, Except;
            FD_ZERO(&Ready); FD_SET(S, &Ready);
            FD_ZERO(&Except); FD_SET(S, &Except);
            timeval Timeout{0, 200000};
            int Result = ::select(0, Writing ? nullptr : &Ready, Writing ? &Ready : nullptr, &Except, &Timeout);
            CheckSocket(Result);
            if (Result > 0) return;
        }
        winrt::throw_hresult(E_ABORT);
    }

    void Transfer(SOCKET S, char* Data, size_t Size, bool Writing, Deadline End, std::atomic<bool> const& Stop)
    {
        while (Size)
        {
            WaitSocket(S, Writing, End, Stop);
            int Count = Writing ? ::send(S, Data, static_cast<int>(Size), 0) :
                ::recv(S, Data, static_cast<int>(Size), 0);
            if (Count == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) continue;
            CheckSocket(Count);
            if (!Count) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED));
            Data += Count;
            Size -= Count;
        }
    }

    void GuestRequest(winrt::guid const& RuntimeId, Json const& Request, std::atomic<bool> const& Stop)
    {
        auto Body = Request.dump();
        if (Body.empty() || Body.size() > MaxMessage) throw winrt::hresult_invalid_argument();
        Socket Connection;
        Connection.Value = ::socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        if (Connection.Value == INVALID_SOCKET) CheckSocket(SOCKET_ERROR);
        SOCKADDR_HV Address{};
        Address.Family = AF_HYPERV;
        Address.VmId = RuntimeId;
        Address.ServiceId = GnsServiceId;
        u_long NonBlocking = 1;
        CheckSocket(::ioctlsocket(Connection.Value, FIONBIO, &NonBlocking));
        int Connected = ::connect(Connection.Value, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
        if (Connected == SOCKET_ERROR)
        {
            int Error = ::WSAGetLastError();
            if (Error != WSAEWOULDBLOCK && Error != WSAEINPROGRESS) CheckSocket(SOCKET_ERROR);
            WaitSocket(Connection.Value, true, std::chrono::steady_clock::now() + std::chrono::seconds(3), Stop);
            int ConnectError = 0;
            int Length = sizeof(ConnectError);
            CheckSocket(::getsockopt(Connection.Value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&ConnectError), &Length));
            if (ConnectError) winrt::throw_hresult(HRESULT_FROM_WIN32(ConnectError));
        }
        auto End = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        auto Length = ::htonl(static_cast<u_long>(Body.size()));
        Transfer(Connection.Value, reinterpret_cast<char*>(&Length), sizeof(Length), true, End, Stop);
        Transfer(Connection.Value, Body.data(), Body.size(), true, End, Stop);
        Transfer(Connection.Value, reinterpret_cast<char*>(&Length), sizeof(Length), false, End, Stop);
        Length = ::ntohl(Length);
        if (!Length || Length > MaxMessage) throw winrt::hresult_error(E_UNEXPECTED);
        std::string Response(Length, '\0');
        Transfer(Connection.Value, Response.data(), Response.size(), false, End, Stop);
        auto Reply = Json::parse(Response);
        if (Reply.value("Version", 0) != 1 || !Reply.value("Success", false))
            throw winrt::hresult_error(E_FAIL, winrt::to_hstring(Reply.value("Error", std::string("Guest network request failed"))));
    }

    std::string AddressString(SOCKADDR_INET const& Address)
    {
        char Buffer[INET6_ADDRSTRLEN]{};
        if (Address.si_family == AF_INET)
        {
            if (!::InetNtopA(AF_INET, &Address.Ipv4.sin_addr, Buffer, ARRAYSIZE(Buffer))) return {};
        }
        else if (Address.si_family == AF_INET6)
        {
            if (!::InetNtopA(AF_INET6, &Address.Ipv6.sin6_addr, Buffer, ARRAYSIZE(Buffer))) return {};
        }
        return Buffer;
    }

    std::string AddressString(SOCKADDR const* Address)
    {
        if (!Address) return {};
        SOCKADDR_INET Copy{};
        if (Address->sa_family == AF_INET) Copy.Ipv4 = *reinterpret_cast<SOCKADDR_IN const*>(Address);
        else if (Address->sa_family == AF_INET6) Copy.Ipv6 = *reinterpret_cast<SOCKADDR_IN6 const*>(Address);
        else return {};
        return AddressString(Copy);
    }

    bool IsUsableAddress(SOCKADDR_INET const& Address)
    {
        if (Address.si_family == AF_INET)
        {
            unsigned Value = ::ntohl(Address.Ipv4.sin_addr.s_addr);
            return Value && (Value >> 24) != 127 && (Value >> 28) != 14 && Value != 0xffffffff;
        }
        if (Address.si_family == AF_INET6)
        {
            auto const& Value = Address.Ipv6.sin6_addr;
            return !IN6_IS_ADDR_UNSPECIFIED(&Value) && !IN6_IS_ADDR_LOOPBACK(&Value) &&
                !IN6_IS_ADDR_MULTICAST(&Value) && !IN6_IS_ADDR_LINKLOCAL(&Value);
        }
        return false;
    }

    struct Snapshot
    {
        std::map<std::string, Json> Items;
        Json Dns = Json::array();
        Json Interface = Json::object();
    };

    Snapshot ReadHostSnapshot(winrt::guid const& InterfaceId)
    {
        NET_LUID Luid{};
        GUID NativeInterfaceId = InterfaceId;
        winrt::check_win32(::ConvertInterfaceGuidToLuid(&NativeInterfaceId, &Luid));
        MIB_IF_ROW2 If{};
        If.InterfaceLuid = Luid;
        winrt::check_win32(::GetIfEntry2(&If));
        Snapshot Result;
        Result.Interface = {{"Connected", If.OperStatus == IfOperStatusUp}, {"NlMtu", If.Mtu}, {"Mirrored", true}};
        if (If.OperStatus != IfOperStatusUp) return Result;
        PMIB_UNICASTIPADDRESS_TABLE Addresses = nullptr;
        winrt::check_win32(::GetUnicastIpAddressTable(AF_UNSPEC, &Addresses));
        auto FreeAddresses = Mile::ScopeExitTaskHandler([&]() { ::FreeMibTable(Addresses); });
        for (ULONG i = 0; i < Addresses->NumEntries; ++i)
        {
            auto const& Row = Addresses->Table[i];
            if (Row.InterfaceLuid.Value != Luid.Value || !IsUsableAddress(Row.Address) ||
                (Row.DadState != IpDadStatePreferred && Row.DadState != IpDadStateDeprecated)) continue;
            auto Address = AddressString(Row.Address);
            if (Address.empty()) continue;
            Json Settings = {{"Address", Address}, {"OnLinkPrefixLength", static_cast<unsigned>(Row.OnLinkPrefixLength)}, {"Mirrored", true}};
            Result.Items.emplace("IPAddress|" + Settings.dump(), Json{{"ResourceType", "IPAddress"}, {"Settings", Settings}});
        }
        PMIB_IPFORWARD_TABLE2 Routes = nullptr;
        winrt::check_win32(::GetIpForwardTable2(AF_UNSPEC, &Routes));
        auto FreeRoutes = Mile::ScopeExitTaskHandler([&]() { ::FreeMibTable(Routes); });
        for (ULONG i = 0; i < Routes->NumEntries; ++i)
        {
            auto const& Row = Routes->Table[i];
            if (Row.InterfaceLuid.Value != Luid.Value || Row.Loopback) continue;
            auto const& Prefix = Row.DestinationPrefix.Prefix;
            auto Destination = AddressString(Prefix);
            auto Hop = AddressString(Row.NextHop);
            if (Destination.empty()) continue;
            // Exclude host-local and multicast routes; guest networking owns those.
            if (Prefix.si_family == AF_INET)
            {
                unsigned V = ::ntohl(Prefix.Ipv4.sin_addr.s_addr);
                if ((V >> 24) == 127 || (V >> 28) == 14 || V == 0xffffffff) continue;
            }
            else if (Prefix.si_family == AF_INET6 &&
                (IN6_IS_ADDR_MULTICAST(&Prefix.Ipv6.sin6_addr) || IN6_IS_ADDR_LOOPBACK(&Prefix.Ipv6.sin6_addr) ||
                    IN6_IS_ADDR_LINKLOCAL(&Prefix.Ipv6.sin6_addr))) continue;
            Json Settings = {{"DestinationPrefix", Destination + "/" + std::to_string(Row.DestinationPrefix.PrefixLength)},
                {"NextHop", Hop}, {"Metric", Row.Metric}};
            Result.Items.emplace("Route|" + Settings.dump(), Json{{"ResourceType", "Route"}, {"Settings", Settings}});
        }
        ULONG Bytes = 16384;
        std::vector<BYTE> Buffer(Bytes);
        ULONG Error = ERROR_BUFFER_OVERFLOW;
        for (int Retry = 0; Retry < 3 && Error == ERROR_BUFFER_OVERFLOW; ++Retry)
        {
            Buffer.resize(Bytes);
            Error = ::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(Buffer.data()), &Bytes);
        }
        winrt::check_win32(Error);
        for (auto Adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(Buffer.data()); Adapter; Adapter = Adapter->Next)
        {
            if (Adapter->Luid.Value != Luid.Value) continue;
            for (auto Dns = Adapter->FirstDnsServerAddress; Dns; Dns = Dns->Next)
            {
                auto Value = AddressString(Dns->Address.lpSockaddr);
                if (!Value.empty()) Result.Dns.push_back(Value);
            }
            break;
        }
        return Result;
    }

    void RegisterGuestSocket()
    {
        constexpr wchar_t Path[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\{00006b3d-facb-11e6-bd58-64006a7986d3}";
        constexpr wchar_t Name[] = L"NanaBox Guest Network Service";
        HKEY Key = nullptr;
        DWORD Disposition = 0;
        winrt::check_win32(::RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &Key, &Disposition));
        auto Close = Mile::ScopeExitTaskHandler([&]() { ::RegCloseKey(Key); });
        if (Disposition == REG_CREATED_NEW_KEY)
        {
            winrt::check_win32(::RegSetValueExW(Key, L"ElementName", 0, REG_SZ,
                reinterpret_cast<BYTE const*>(Name), sizeof(Name)));
        }
        else
        {
            wchar_t Existing[256]{};
            DWORD Bytes = sizeof(Existing);
            winrt::check_win32(::RegGetValueW(Key, nullptr, L"ElementName", RRF_RT_REG_SZ,
                nullptr, Existing, &Bytes));
            if (::wcscmp(Existing, Name) != 0)
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"The NanaBox guest socket ID is registered to another service.");
        }
    }

    std::wstring WslDependencyPath()
    {
        PWSTR Folder = nullptr;
        winrt::check_hresult(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &Folder));
        auto Free = Mile::ScopeExitTaskHandler([&]() { ::CoTaskMemFree(Folder); });
        return std::wstring(Folder) + L"\\WSL\\wsldeps.dll";
    }

    template<typename T>
    T Import(HMODULE Module, const char* Name)
    {
        auto Address = ::GetProcAddress(Module, Name);
        if (!Address) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), winrt::to_hstring(Name));
        return reinterpret_cast<T>(Address);
    }

    unsigned ParsePort(std::string const& Value)
    {
        unsigned Port = 0;
        auto End = Value.data() + Value.size();
        auto R = std::from_chars(Value.data(), End, Port);
        if (R.ec != std::errc() || R.ptr != End || !Port || Port > 65535)
            throw winrt::hresult_invalid_argument(L"Invalid mirrored port");
        return Port;
    }
}

struct NanaBox::MirroredNetworking::Impl
{
    using RegisterRpc = HRESULT (WINAPI*)(PSECURITY_DESCRIPTOR, GUID*);
    using UnregisterRpc = HRESULT (WINAPI*)(GUID const*);
    using CreateService = decltype(&::HcnCreateGuestNetworkService);
    using DeleteService = decltype(&::HcnDeleteGuestNetworkService);
    using CloseService = decltype(&::HcnCloseGuestNetworkService);
    using ModifyService = decltype(&::HcnModifyGuestNetworkService);
    using ReserveRange = HRESULT (WINAPI*)(HCN_GUESTNETWORKSERVICE, USHORT, HCN_PORT_RANGE_RESERVATION*, HANDLE*);
    using ReservePort = HRESULT (WINAPI*)(HCN_GUESTNETWORKSERVICE, HCN_PORT_PROTOCOL, HCN_PORT_ACCESS, USHORT, HANDLE*);
    using ReleasePort = HRESULT (WINAPI*)(HANDLE);

    struct Endpoint
    {
        winrt::guid Id{};
        winrt::guid HostInterface{};
        std::string Mac;
        bool Attached = false;
    };

    winrt::guid RuntimeId;
    VirtualMachineConfiguration Configuration;
    HcsSystem Vm;
    HCN_GUESTNETWORKSERVICE Service = nullptr;
    HMODULE Dependencies = nullptr;
    HMODULE NetworkModule = nullptr;
    GUID RpcId{};
    bool RpcRegistered = false;
    bool WsaStarted = false;
    HCN_PORT_RANGE_RESERVATION Range{};
    std::vector<HANDLE> Reservations;
    ReleasePort Release = nullptr;
    CreateService CreateGns = nullptr;
    DeleteService DeleteGns = nullptr;
    CloseService CloseGns = nullptr;
    ModifyService ModifyGns = nullptr;
    UnregisterRpc Unregister = nullptr;
    std::map<std::string, Endpoint> Endpoints;
    std::atomic<bool> Stop{false};
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::wstring CurrentStatus = L"Connecting";
    std::thread Worker;

    Impl(VirtualMachineConfiguration const& Config, winrt::guid const& Id) :
        RuntimeId(Id), Configuration(Config)
    {
    }

    void StopWorker()
    {
        Stop = true;
        Wake.notify_all();
        if (Worker.joinable()) Worker.join();
    }

    void StartWorker()
    {
        if (Worker.joinable()) return;
        Stop = false;
        Status(L"Synchronizing");
        Worker = std::thread([this]() { Run(); });
    }

    ~Impl()
    {
        StopWorker();
        for (auto& Entry : Endpoints)
        {
            try { RemoveEndpoint(Entry.second); } catch (...) {}
        }
        if (Release) for (auto Handle : Reservations) Release(Handle);
        if (Service)
        {
            HcnText Error;
            if (DeleteGns) DeleteGns(RuntimeId, &Error.Value);
            if (CloseGns) CloseGns(Service);
        }
        if (RpcRegistered && Unregister) Unregister(&RpcId);
        if (Dependencies) ::FreeLibrary(Dependencies);
        if (NetworkModule) ::FreeLibrary(NetworkModule);
        if (WsaStarted) ::WSACleanup();
    }

    void Status(std::wstring Text)
    {
        std::lock_guard Lock(Mutex);
        CurrentStatus = std::move(Text);
    }

    std::wstring Status() const
    {
        std::lock_guard Lock(Mutex);
        return CurrentStatus;
    }

    void SetState(char const* State)
    {
        HcnText Error;
        Json Request = {{"ResourceType", "State"}, {"RequestType", "Update"}, {"Settings", {{"State", State}}}};
        auto Text = winrt::to_hstring(Request.dump());
        auto Hr = ModifyGns(Service, Text.c_str(), &Error.Value);
        CheckHcn(Hr, Error);
    }

    void Start()
    {
        WSADATA Wsa{};
        int Error = ::WSAStartup(MAKEWORD(2, 2), &Wsa);
        if (Error) winrt::throw_hresult(HRESULT_FROM_WIN32(Error));
        WsaStarted = true;
        auto Name = winrt::to_hstring(Configuration.Name);
        winrt::check_hresult(::HcsOpenComputeSystem(Name.c_str(), GENERIC_ALL, Vm.put()));

        // WSL's RPC registration component is installed under protected Program Files.
        auto Path = WslDependencyPath();
        Dependencies = ::LoadLibraryExW(Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!Dependencies)
            throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()), L"Install a compatible Microsoft WSL networking dependency in Program Files\\WSL.");
        auto Register = Import<RegisterRpc>(Dependencies, "WslDepsRegisterGnsRpcServer");
        Unregister = Import<UnregisterRpc>(Dependencies, "WslDepsUnregisterGnsRpcServer");
        PSECURITY_DESCRIPTOR Descriptor = nullptr;
        winrt::check_bool(::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:(A;;GA;;;SY)", SDDL_REVISION_1, &Descriptor, nullptr));
        auto FreeDescriptor = Mile::ScopeExitTaskHandler([&]() { ::LocalFree(Descriptor); });
        winrt::check_hresult(Register(Descriptor, &RpcId));
        RpcRegistered = true;

        bool Firewall = !Configuration.MirroredFirewallCreatorId.empty();
        if (Firewall) (void)winrt::guid(Configuration.MirroredFirewallCreatorId);
        // The filtered query asks HNS to create flow-steering networks.
        (void)MirroredNetworks(Firewall);
        Json Request = {{"VirtualMachineId", GuidString(RuntimeId)}, {"MirrorHostNetworking", true},
            {"SchemaVersion", {{"Major", 2}, {"Minor", 0}}},
            {"GnsRpcServerInformation", {{"EndpointType", "LRpc"}, {"ObjectUuid", GuidString(RpcId)}}},
            {"Flags", 3}};
        HcnText ServiceError;
        auto Text = winrt::to_hstring(Request.dump());
        NetworkModule = ::LoadLibraryExW(L"computenetwork.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        winrt::check_pointer(NetworkModule);
        CreateGns = Import<CreateService>(NetworkModule, "HcnCreateGuestNetworkService");
        DeleteGns = Import<DeleteService>(NetworkModule, "HcnDeleteGuestNetworkService");
        CloseGns = Import<CloseService>(NetworkModule, "HcnCloseGuestNetworkService");
        ModifyGns = Import<ModifyService>(NetworkModule, "HcnModifyGuestNetworkService");
        auto Hr = CreateGns(RuntimeId, Text.c_str(), &Service, &ServiceError.Value);
        CheckHcn(Hr, ServiceError);
        RegisterGuestSocket();
        SetState("Bootstrapping");
        auto AllocateRange = Import<ReserveRange>(NetworkModule, "HcnReserveGuestNetworkServicePortRange");
        auto AllocatePort = Import<ReservePort>(NetworkModule, "HcnReserveGuestNetworkServicePort");
        Release = Import<ReleasePort>(NetworkModule, "HcnReleaseGuestNetworkServicePortReservationHandle");
        HRESULT RangeResult = E_FAIL;
        for (USHORT Count = 4095; Count >= 511; Count /= 2)
        {
            HANDLE Reservation = nullptr;
            RangeResult = AllocateRange(Service, Count, &Range, &Reservation);
            if (SUCCEEDED(RangeResult))
            {
                Reservations.push_back(Reservation);
                break;
            }
            if (Reservation) Release(Reservation);
        }
        winrt::check_hresult(RangeResult);
        if (Range.startingPort < 1024 || Range.endingPort <= Range.startingPort)
            throw winrt::hresult_error(E_UNEXPECTED, L"Invalid HCN dynamic port range");
        for (auto const& List : {std::make_pair(HCN_PORT_PROTOCOL_TCP, Configuration.MirroredTcpPorts),
            std::make_pair(HCN_PORT_PROTOCOL_UDP, Configuration.MirroredUdpPorts)})
        {
            std::set<unsigned> Unique;
            for (auto const& Value : List.second)
            {
                auto Port = ParsePort(Value);
                if (!Unique.insert(Port).second) throw winrt::hresult_invalid_argument(L"Duplicate mirrored port");
                if (Port >= Range.startingPort && Port <= Range.endingPort) continue;
                HANDLE Reservation = nullptr;
                winrt::check_hresult(AllocatePort(Service, List.first, HCN_PORT_ACCESS_EXCLUSIVE, static_cast<USHORT>(Port), &Reservation));
                Reservations.push_back(Reservation);
            }
        }
        StartWorker();
    }

    void PauseSynchronization()
    {
        StopWorker();
        try
        {
            SetState("Paused");
            Status(L"Paused");
        }
        catch (...)
        {
            StartWorker();
            throw;
        }
    }

    void ResumeSynchronization()
    {
        if (Worker.joinable()) return;
        try { SetState("Desynchronized"); } catch (...) {}
        StartWorker();
    }

    void DetachForSave()
    {
        StopWorker();
        try
        {
            SetState("Paused");
            for (auto It = Endpoints.begin(); It != Endpoints.end();)
            {
                RemoveEndpoint(It->second);
                It = Endpoints.erase(It);
            }
            Status(L"Paused");
        }
        catch (...)
        {
            ResumeSynchronization();
            throw;
        }
    }

    void VmExited() noexcept
    {
        StopWorker();
        // HCS has already detached all devices, so only host endpoints remain.
        for (auto& Entry : Endpoints) Entry.second.Attached = false;
    }

    Endpoint Attach(winrt::guid const& Network)
    {
        Endpoint Entry;
        GUID EndpointId{};
        winrt::check_hresult(::CoCreateGuid(&EndpointId));
        Entry.Id = EndpointId;
        auto Info = QueryHostNetwork(Network);
        auto NetworkProperties = Json::parse(winrt::to_string(Info.Properties));
        Json Settings = {{"SchemaVersion", {{"Major", 2}, {"Minor", 0}}},
            {"HostComputeNetwork", GuidString(Network)}, {"Owner", Configuration.Name}};
        if (!Configuration.MirroredFirewallCreatorId.empty())
        {
            Settings["SchemaVersion"]["Minor"] = 16;
            Settings["Policies"] = Json::array({
                {{"Type", "PortName"}, {"Settings", {{"Name", ""}}}},
                {{"Type", "Firewall"}, {"Settings", {{"VmCreatorId", Configuration.MirroredFirewallCreatorId}, {"PolicyFlags", 0}}}}
            });
        }
        auto Handle = NanaBox::HcnCreateEndpoint(NanaBox::HcnOpenNetwork(Network), Entry.Id, winrt::to_hstring(Settings.dump()));
        try
        {
            auto Properties = Json::parse(winrt::to_string(NanaBox::HcnQueryEndpointProperties(Handle)));
            Entry.Mac = Properties.at("MacAddress").get<std::string>();
            auto Constraint = Properties.value("InterfaceConstraint", Json::object());
            if (!Constraint.contains("InterfaceGuid")) Constraint = NetworkProperties.at("InterfaceConstraint");
            Entry.HostInterface = winrt::guid(Constraint.at("InterfaceGuid").get<std::string>());
            ModifyVm(Vm.get(), "VirtualMachine/Devices/NetworkAdapters/" + GuidString(Entry.Id), "Add",
                Json{{"EndpointId", GuidString(Entry.Id)}, {"MacAddress", Entry.Mac}});
            Entry.Attached = true;
        }
        catch (...)
        {
            Handle = HcnEndpoint();
            try { NanaBox::HcnDeleteEndpoint(Entry.Id); } catch (...) {}
            throw;
        }
        return Entry;
    }

    void RemoveEndpoint(Endpoint& Entry)
    {
        if (Entry.Attached)
        {
            ModifyVm(Vm.get(), "VirtualMachine/Devices/NetworkAdapters/" + GuidString(Entry.Id), "Remove",
                Json{{"EndpointId", GuidString(Entry.Id)}, {"MacAddress", Entry.Mac}});
            Entry.Attached = false;
        }
        NanaBox::HcnDeleteOwnedEndpoint(Entry.Id, Configuration.Name);
    }

    void Synchronize(Endpoint& Entry)
    {
        auto Desired = ReadHostSnapshot(Entry.HostInterface);
        Json Addresses = Json::array(), Routes = Json::array();
        for (auto const& Item : Desired.Items)
        {
            auto const& Value = Item.second;
            if (Value["ResourceType"] == "IPAddress")
                Addresses.push_back(Value.at("Settings"));
            else if (Value["ResourceType"] == "Route")
                Routes.push_back(Value.at("Settings"));
        }
        // The agent reads and reconciles its actual state on every request.
        // No host-side cache is used to infer whether a configuration survived.
        GuestRequest(RuntimeId, Json{{"Version", 1}, {"AdapterMac", Entry.Mac},
            {"ResourceType", "Reconcile"}, {"RequestType", "Update"},
            {"Settings", {{"Interface", Desired.Interface}, {"Addresses", Addresses},
                {"Routes", Routes}, {"DNS", {{"ServerList", Desired.Dns}}},
                {"PortRange", {{"Start", Range.startingPort}, {"End", Range.endingPort}}}}}}, Stop);
    }

    void Run() noexcept
    {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch (...) { Status(L"COM initialization failed"); return; }
        auto Uninitialize = Mile::ScopeExitTaskHandler([]() { winrt::uninit_apartment(); });
        bool Synchronized = false;
        while (!Stop)
        {
            try
            {
                auto Networks = MirroredNetworks(!Configuration.MirroredFirewallCreatorId.empty());
                std::set<std::string> Active;
                for (auto const& Network : Networks) Active.insert(GuidString(Network));
                for (auto It = Endpoints.begin(); It != Endpoints.end();)
                {
                    if (!Active.count(It->first) || !It->second.Attached)
                    {
                        RemoveEndpoint(It->second);
                        It = Endpoints.erase(It);
                    }
                    else ++It;
                }
                for (auto const& Network : Networks)
                {
                    if (Stop) break;
                    auto Key = GuidString(Network);
                    if (!Endpoints.count(Key)) Endpoints.emplace(Key, Attach(Network));
                }
                if (!Stop)
                {
                    if (Endpoints.empty()) throw winrt::hresult_error(E_PENDING, L"Waiting for HCN mirrored networks");
                    for (auto& Entry : Endpoints) Synchronize(Entry.second);
                    if (!Synchronized) SetState("Synchronized");
                    Synchronized = true;
                    Status(L"Synchronized");
                }
            }
            catch (...)
            {
                if (!Stop)
                {
                    auto Error = Mile::WinRT::ToHResultError();
                    if (Synchronized)
                    {
                        try { SetState("Desynchronized"); } catch (...) {}
                        Synchronized = false;
                    }
                    Status(std::wstring(Error.message().c_str()));
                }
            }
            std::unique_lock Lock(Mutex);
            Wake.wait_for(Lock, std::chrono::seconds(5), [this]() { return Stop.load(); });
        }
    }
};

NanaBox::MirroredNetworking::MirroredNetworking(
    VirtualMachineConfiguration const& Configuration, winrt::guid const& RuntimeId) :
    m_Impl(std::make_unique<Impl>(Configuration, RuntimeId))
{
    m_Impl->Start();
}

NanaBox::MirroredNetworking::~MirroredNetworking() = default;

std::wstring NanaBox::MirroredNetworking::Status() const
{
    return m_Impl->Status();
}

void NanaBox::MirroredNetworking::PauseSynchronization()
{
    m_Impl->PauseSynchronization();
}

void NanaBox::MirroredNetworking::ResumeSynchronization()
{
    m_Impl->ResumeSynchronization();
}

void NanaBox::MirroredNetworking::DetachForSave()
{
    m_Impl->DetachForSave();
}

void NanaBox::MirroredNetworking::VmExited() noexcept
{
    m_Impl->VmExited();
}
