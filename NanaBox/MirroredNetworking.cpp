#include <winsock2.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <sddl.h>
#include <ShlObj.h>
#include <knownfolders.h>
#include <wincrypt.h>
#include <array>
#include <functional>
#include <vector>
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

    Json GuestRequest(winrt::guid const& RuntimeId, Json const& Request, std::atomic<bool> const& Stop,
        unsigned TimeoutSeconds = 90)
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
        auto End = std::chrono::steady_clock::now() + std::chrono::seconds(TimeoutSeconds);
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
        return Reply.value("Data", Json::object());
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
        std::string DnsSuffix;
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
        bool Connected = false, HasIpInterface = false;
        bool DisableV4Default = false, DisableV6Default = false;
        ULONG Mtu = If.Mtu;
        Result.Interface = {{"Connected", false}, {"NlMtu", Mtu}, {"Mirrored", true}};
        for (auto Family : {AF_INET, AF_INET6})
        {
            MIB_IPINTERFACE_ROW IpInterface{};
            ::InitializeIpInterfaceEntry(&IpInterface);
            IpInterface.Family = static_cast<ADDRESS_FAMILY>(Family);
            IpInterface.InterfaceLuid = Luid;
            if (::GetIpInterfaceEntry(&IpInterface) == NO_ERROR)
            {
                HasIpInterface = true;
                Connected = Connected || IpInterface.Connected != FALSE;
                Result.Interface[Family == AF_INET ? "IPv4Metric" : "IPv6Metric"] = IpInterface.Metric;
                if (IpInterface.Connected && IpInterface.NlMtu && IpInterface.NlMtu < Mtu) Mtu = IpInterface.NlMtu;
                if (Family == AF_INET) DisableV4Default = IpInterface.DisableDefaultRoutes != FALSE;
                else DisableV6Default = IpInterface.DisableDefaultRoutes != FALSE;
            }
        }
        if (!HasIpInterface) Connected = If.OperStatus == IfOperStatusUp;
        Result.Interface["Connected"] = Connected;
        Result.Interface["NlMtu"] = Mtu;
        if (!Connected) return Result;
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
            if (Row.DestinationPrefix.PrefixLength == 0 &&
                ((Prefix.si_family == AF_INET && DisableV4Default) ||
                 (Prefix.si_family == AF_INET6 && DisableV6Default))) continue;
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
            if (Adapter->DnsSuffix) Result.DnsSuffix = winrt::to_string(winrt::hstring(Adapter->DnsSuffix));
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
    bool OwnsService = false;
    HMODULE Dependencies = nullptr;
    HMODULE NetworkModule = nullptr;
    GUID RpcId{};
    bool RpcRegistered = false;
    bool WsaStarted = false;
    HCN_PORT_RANGE_RESERVATION Range{};
    std::vector<HANDLE> Reservations;
    using PortKey = std::pair<HCN_PORT_PROTOCOL, USHORT>;
    std::set<PortKey> ExplicitPorts;
    std::set<unsigned> IgnoredPorts;
    std::map<PortKey, HANDLE> AutomaticPorts;
    ReservePort AllocatePort = nullptr;
    ReleasePort Release = nullptr;
    CreateService CreateGns = nullptr;
    DeleteService DeleteGns = nullptr;
    CloseService CloseGns = nullptr;
    ModifyService ModifyGns = nullptr;
    UnregisterRpc Unregister = nullptr;
    std::map<std::string, Endpoint> Endpoints;
    std::atomic<bool> Stop{false};
    std::atomic<bool> NetworkChanged{true};
    std::atomic<bool> ServiceDisconnected{false};
    HCN_CALLBACK HcnCallbackHandle = nullptr;
    HANDLE AddressNotification = nullptr, RouteNotification = nullptr, InterfaceNotification = nullptr;
    std::map<std::string, std::pair<winrt::guid, std::string>> EndpointIdentities;
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::wstring CurrentStatus = L"Connecting";
    Json CurrentDiagnostics = Json::object();
    ULONGLONG LastSuccessfulSync = 0;
    unsigned ReconnectCount = 0;
    std::thread Worker;

    Impl(VirtualMachineConfiguration const& Config, winrt::guid const& Id) :
        RuntimeId(Id), Configuration(Config)
    {
    }

    void RequestRefresh() noexcept
    {
        {
            std::lock_guard Lock(Mutex);
            NetworkChanged.store(true);
        }
        Wake.notify_all();
    }

    static void CALLBACK HcnNotification(DWORD Type, void* Context, HRESULT, PCWSTR) noexcept
    {
        auto Self = static_cast<Impl*>(Context);
        if (Type == HcnNotificationServiceDisconnect) Self->ServiceDisconnected.store(true);
        // Notifications can be delivered while the HCN operation is still executing.
        // The callback never calls HCN/HCS or waits on the worker.
        if (Type == HcnNotificationServiceDisconnect ||
            Type == HcnNotificationNetworkCreate || Type == HcnNotificationNetworkPreDelete ||
            Type == HcnNotificationNetworkDelete)
            Self->RequestRefresh();
    }

    static void CALLBACK AddressChanged(void* Context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) noexcept
    {
        static_cast<Impl*>(Context)->RequestRefresh();
    }

    static void CALLBACK RouteChanged(void* Context, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) noexcept
    {
        static_cast<Impl*>(Context)->RequestRefresh();
    }

    static void CALLBACK InterfaceChanged(void* Context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) noexcept
    {
        static_cast<Impl*>(Context)->RequestRefresh();
    }

    void SubscribeHcn()
    {
        if (HcnCallbackHandle) return;
        winrt::check_hresult(::HcnRegisterServiceCallback(HcnNotification, this, &HcnCallbackHandle));
    }

    void UnsubscribeHcn() noexcept
    {
        if (HcnCallbackHandle)
        {
            ::HcnUnregisterServiceCallback(HcnCallbackHandle);
            HcnCallbackHandle = nullptr;
        }
    }

    void SubscribeHost()
    {
        SubscribeHcn();
        winrt::check_win32(::NotifyUnicastIpAddressChange(AF_UNSPEC, AddressChanged, this, FALSE, &AddressNotification));
        winrt::check_win32(::NotifyRouteChange2(AF_UNSPEC, RouteChanged, this, FALSE, &RouteNotification));
        winrt::check_win32(::NotifyIpInterfaceChange(AF_UNSPEC, InterfaceChanged, this, FALSE, &InterfaceNotification));
    }

    void UnsubscribeHost() noexcept
    {
        UnsubscribeHcn();
        for (auto Handle : {AddressNotification, RouteNotification, InterfaceNotification})
            if (Handle) ::CancelMibChangeNotify2(Handle);
        AddressNotification = RouteNotification = InterfaceNotification = nullptr;
    }

    void StopWorker()
    {
        {
            std::lock_guard Lock(Mutex);
            Stop = true;
        }
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
        UnsubscribeHost();
        for (auto& Entry : Endpoints)
        {
            try { RemoveEndpoint(Entry.second); } catch (...) {}
        }
        ReleaseGuestService();
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

    std::string Diagnostics() const
    {
        std::lock_guard Lock(Mutex);
        auto Result = CurrentDiagnostics;
        Result["Mode"] = "Mirrored";
        Result["RuntimeId"] = GuidString(RuntimeId);
        Result["Status"] = winrt::to_string(CurrentStatus);
        if (LastSuccessfulSync) Result["SecondsSinceLastSuccessfulSync"] = (::GetTickCount64() - LastSuccessfulSync) / 1000;
        return Result.dump(2);
    }

    void PublishDiagnostics(Json Report, std::wstring State, bool Success)
    {
        std::lock_guard Lock(Mutex);
        CurrentDiagnostics = std::move(Report);
        CurrentStatus = std::move(State);
        if (Success) LastSuccessfulSync = ::GetTickCount64();
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

        NetworkModule = ::LoadLibraryExW(L"computenetwork.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        winrt::check_pointer(NetworkModule);
        CreateGns = Import<CreateService>(NetworkModule, "HcnCreateGuestNetworkService");
        DeleteGns = Import<DeleteService>(NetworkModule, "HcnDeleteGuestNetworkService");
        CloseGns = Import<CloseService>(NetworkModule, "HcnCloseGuestNetworkService");
        ModifyGns = Import<ModifyService>(NetworkModule, "HcnModifyGuestNetworkService");
        AllocatePort = Import<ReservePort>(NetworkModule, "HcnReserveGuestNetworkServicePort");
        Release = Import<ReleasePort>(NetworkModule, "HcnReleaseGuestNetworkServicePortReservationHandle");
        for (auto const& Value : Configuration.MirroredIgnoredPorts)
            if (!IgnoredPorts.insert(ParsePort(Value)).second)
                throw winrt::hresult_invalid_argument(L"Duplicate ignored mirrored port");
        for (auto const& List : {Configuration.MirroredTcpPorts, Configuration.MirroredUdpPorts})
            for (auto const& Value : List)
                if (IgnoredPorts.count(ParsePort(Value)))
                    throw winrt::hresult_invalid_argument(L"An ignored mirrored port cannot be explicitly reserved");
        RegisterGuestSocket();
        SubscribeHost();
        CreateGuestService();
        StartWorker();
    }

    void ReleaseGuestService() noexcept
    {
        if (Release)
        {
            for (auto const& Entry : AutomaticPorts) Release(Entry.second);
            for (auto Handle : Reservations) Release(Handle);
        }
        AutomaticPorts.clear();
        Reservations.clear();
        ExplicitPorts.clear();
        Range = {};
        if (OwnsService && DeleteGns)
        {
            HcnText Error;
            if (SUCCEEDED(DeleteGns(RuntimeId, &Error.Value))) OwnsService = false;
        }
        if (Service && CloseGns) CloseGns(Service);
        Service = nullptr;
    }

    void CreateGuestService()
    {
        bool Firewall = !Configuration.MirroredFirewallCreatorId.empty();
        if (Firewall) (void)winrt::guid(Configuration.MirroredFirewallCreatorId);
        // Subscribe before querying so newly created flow-steering networks cannot be missed.
        (void)MirroredNetworks(Firewall);
        Json Request = {{"VirtualMachineId", GuidString(RuntimeId)}, {"MirrorHostNetworking", true},
            {"SchemaVersion", {{"Major", 2}, {"Minor", 0}}},
            {"GnsRpcServerInformation", {{"EndpointType", "LRpc"}, {"ObjectUuid", GuidString(RpcId)}}},
            {"Flags", 3}};
        HcnText ServiceError;
        auto Text = winrt::to_hstring(Request.dump());
        auto Hr = CreateGns(RuntimeId, Text.c_str(), &Service, &ServiceError.Value);
        CheckHcn(Hr, ServiceError);
        OwnsService = true;
        SetState("Bootstrapping");
        auto AllocateRange = Import<ReserveRange>(NetworkModule, "HcnReserveGuestNetworkServicePortRange");
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
                auto PortResult = AllocatePort(Service, List.first, HCN_PORT_ACCESS_EXCLUSIVE, static_cast<USHORT>(Port), &Reservation);
                if (FAILED(PortResult)) { if (Reservation) Release(Reservation); winrt::check_hresult(PortResult); }
                Reservations.push_back(Reservation);
                ExplicitPorts.emplace(List.first, static_cast<USHORT>(Port));
            }
        }
    }

    void ReconnectService()
    {
        Status(L"Reconnecting to HCN");
        UnsubscribeHcn();
        try
        {
            SubscribeHcn();
            // HNS may have preserved the GNS object and reservations during
            // its restart. Reuse that session when the context is still valid.
            if (Service)
            {
                try
                {
                    SetState("Desynchronized");
                    ++ReconnectCount;
                    return;
                }
                catch (...) {}
            }
            ReleaseGuestService();
            CreateGuestService();
            ++ReconnectCount;
        }
        catch (...)
        {
            ReleaseGuestService();
            UnsubscribeHcn();
            throw;
        }
    }

    void PauseSynchronization()
    {
        if (!Worker.joinable() && Status() == L"Paused") return;
        StopWorker();
        try
        {
            if (Service) SetState("Paused");
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
        if (Service) { try { SetState("Desynchronized"); } catch (...) {} }
        StartWorker();
    }

    void DetachForSave()
    {
        StopWorker();
        try
        {
            if (Service && Status() != L"Paused") SetState("Paused");
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
        auto Key = GuidString(Network);
        auto Identity = EndpointIdentities.find(Key);
        if (Identity == EndpointIdentities.end())
        {
            GUID EndpointId{};
            winrt::check_hresult(::CoCreateGuid(&EndpointId));
            Identity = EndpointIdentities.emplace(Key, std::make_pair(winrt::guid(EndpointId), std::string())).first;
        }
        Entry.Id = Identity->second.first;
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
        if (!Identity->second.second.empty()) Settings["MacAddress"] = Identity->second.second;
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
            Identity->second.second = Entry.Mac;
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

    std::set<PortKey> Synchronize(Endpoint& Entry, Json& Details)
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
        auto Reply = GuestRequest(RuntimeId, Json{{"Version", 1}, {"AdapterMac", Entry.Mac},
            {"ResourceType", "Reconcile"}, {"RequestType", "Update"},
            {"Settings", {{"Interface", Desired.Interface}, {"Addresses", Addresses},
                {"Routes", Routes}, {"DNS", {{"ServerList", Desired.Dns}, {"Suffix", Desired.DnsSuffix}}},
                {"DiscoverPorts", Configuration.MirroredAutoReservePorts},
                {"PortRange", {{"Start", Range.startingPort}, {"End", Range.endingPort}}}}}}, Stop);
        Details["HostInterfaceId"] = GuidString(Entry.HostInterface);
        Details["EndpointId"] = GuidString(Entry.Id);
        Details["MacAddress"] = Entry.Mac;
        Details["Interface"] = Desired.Interface;
        Details["Addresses"] = Addresses;
        Details["Routes"] = Routes;
        Details["DnsServers"] = Desired.Dns;
        Details["DnsSuffix"] = Desired.DnsSuffix;
        Details["GuestAcknowledged"] = true;
        std::set<PortKey> Ports;
        if (Configuration.MirroredAutoReservePorts)
        {
            for (auto const& Item : Reply.at("ListeningPorts"))
            {
                auto Protocol = Item.at("Protocol").get<std::string>();
                unsigned Port = Item.at("Port").get<unsigned>();
                if ((Protocol != "TCP" && Protocol != "UDP") || !Port || Port > 65535)
                    throw winrt::hresult_invalid_argument(L"Invalid guest listening port");
                if (IgnoredPorts.count(Port) || (Port >= Range.startingPort && Port <= Range.endingPort)) continue;
                Ports.emplace(Protocol == "TCP" ? HCN_PORT_PROTOCOL_TCP : HCN_PORT_PROTOCOL_UDP, static_cast<USHORT>(Port));
            }
        }
        return Ports;
    }

    std::wstring ReconcileAutomaticPorts(std::set<PortKey> const& Desired)
    {
        std::wstring Conflicts;
        for (auto It = AutomaticPorts.begin(); It != AutomaticPorts.end();)
        {
            if (!Desired.count(It->first))
            {
                winrt::check_hresult(Release(It->second));
                It = AutomaticPorts.erase(It);
            }
            else ++It;
        }
        for (auto const& Port : Desired)
        {
            if (Stop) break;
            if (ExplicitPorts.count(Port) || AutomaticPorts.count(Port)) continue;
            HANDLE Reservation = nullptr;
            auto Hr = AllocatePort(Service, Port.first, HCN_PORT_ACCESS_EXCLUSIVE, Port.second, &Reservation);
            if (SUCCEEDED(Hr)) AutomaticPorts.emplace(Port, Reservation);
            else
            {
                if (Reservation) Release(Reservation);
                if (Conflicts.size() < 256)
                    Conflicts += (Conflicts.empty() ? L"" : L", ") +
                        std::wstring(Port.first == HCN_PORT_PROTOCOL_TCP ? L"TCP " : L"UDP ") + std::to_wstring(Port.second);
            }
        }
        return Conflicts;
    }

    void Run() noexcept
    {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch (...) { Status(L"COM initialization failed"); return; }
        auto Uninitialize = Mile::ScopeExitTaskHandler([]() { winrt::uninit_apartment(); });
        bool Synchronized = false;
        while (!Stop)
        {
            NetworkChanged.exchange(false);
            Json Report = {{"Endpoints", Json::array()}};
            try
            {
                if (ServiceDisconnected.exchange(false) || !Service || !HcnCallbackHandle)
                {
                    Synchronized = false;
                    ReconnectService();
                }
                auto Networks = MirroredNetworks(!Configuration.MirroredFirewallCreatorId.empty());
                std::set<std::string> Active;
                for (auto const& Network : Networks) Active.insert(GuidString(Network));
                for (auto It = Endpoints.begin(); It != Endpoints.end();)
                {
                    bool Present = Active.count(It->first) != 0 && It->second.Attached;
                    if (Present)
                        Present = static_cast<bool>(NanaBox::HcnOpenOwnedEndpoint(It->second.Id, Configuration.Name));
                    if (!Present)
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
                    std::set<PortKey> Ports;
                    std::wstring Errors;
                    for (auto& Entry : Endpoints)
                    {
                        if (Stop) break;
                        Json Details = {{"NetworkId", Entry.first}, {"EndpointId", GuidString(Entry.second.Id)},
                            {"MacAddress", Entry.second.Mac}, {"GuestAcknowledged", false}};
                        try
                        {
                            auto List = Synchronize(Entry.second, Details);
                            Ports.insert(List.begin(), List.end());
                        }
                        catch (...)
                        {
                            auto Error = Mile::WinRT::ToHResultError().message();
                            Details["Error"] = winrt::to_string(Error);
                            Errors += std::wstring(Error.c_str()) + L"; ";
                        }
                        Report["Endpoints"].push_back(std::move(Details));
                    }
                    if (Stop) break;
                    if (!Errors.empty()) throw winrt::hresult_error(E_FAIL, winrt::hstring(Errors));
                    auto Conflicts = ReconcileAutomaticPorts(Ports);
                    if (!Synchronized)
                    {
                        try { SetState("Synchronized"); }
                        catch (...) { ServiceDisconnected.store(true); throw; }
                    }
                    Synchronized = true;
                    Report["PortRange"] = {{"Start", Range.startingPort}, {"End", Range.endingPort}};
                    Report["ExplicitPortCount"] = ExplicitPorts.size();
                    Report["AutomaticPortCount"] = AutomaticPorts.size();
                    Report["IgnoredPorts"] = IgnoredPorts;
                    Report["PortConflicts"] = winrt::to_string(Conflicts);
                    Report["HcnReconnectCount"] = ReconnectCount;
                    PublishDiagnostics(std::move(Report),
                        Conflicts.empty() ? L"Synchronized" : L"Synchronized; port conflicts: " + Conflicts, true);
                }
            }
            catch (...)
            {
                if (!Stop)
                {
                    auto Error = Mile::WinRT::ToHResultError();
                    if (Synchronized && Service)
                    {
                        try { SetState("Desynchronized"); }
                        catch (...) { ServiceDisconnected.store(true); }
                    }
                    Synchronized = false;
                    Report["Error"] = winrt::to_string(Error.message());
                    Report["HcnReconnectCount"] = ReconnectCount;
                    PublishDiagnostics(std::move(Report), std::wstring(Error.message().c_str()), false);
                }
            }
            std::unique_lock Lock(Mutex);
            // Event notifications accelerate reconciliation; the periodic check
            // still detects guest-only configuration drift and listening sockets.
            bool Changed = Wake.wait_for(Lock, std::chrono::seconds(5),
                [this]() { return Stop.load() || NetworkChanged.load(); });
            if (Changed && !Stop)
                Wake.wait_for(Lock, std::chrono::milliseconds(350), [this]() { return Stop.load(); });
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

std::string NanaBox::MirroredNetworking::Diagnostics() const
{
    return m_Impl->Diagnostics();
}

void NanaBox::MirroredNetworking::RequestRefresh() noexcept
{
    m_Impl->RequestRefresh();
}

namespace
{
    constexpr DWORD FileChunk = 64 * 1024;
    using GuestExchangeRequest = std::function<Json(std::string const&, std::string const&, Json const&)>;

    class FileDigest
    {
        HCRYPTPROV Provider{};
        HCRYPTHASH Hash{};
    public:
        FileDigest()
        {
            winrt::check_bool(::CryptAcquireContextW(&Provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT));
            if (!::CryptCreateHash(Provider, CALG_SHA_256, 0, 0, &Hash))
            {
                auto Error = ::GetLastError();
                ::CryptReleaseContext(Provider, 0);
                winrt::throw_hresult(HRESULT_FROM_WIN32(Error));
            }
        }
        ~FileDigest()
        {
            if (Hash) ::CryptDestroyHash(Hash);
            if (Provider) ::CryptReleaseContext(Provider, 0);
        }
        FileDigest(FileDigest const&) = delete;
        FileDigest& operator=(FileDigest const&) = delete;
        void Update(void const* Buffer, DWORD Size)
        {
            winrt::check_bool(::CryptHashData(Hash, static_cast<BYTE const*>(Buffer), Size, 0));
        }
        std::string Finish()
        {
            std::array<BYTE, 32> Bytes{};
            DWORD Size = static_cast<DWORD>(Bytes.size());
            winrt::check_bool(::CryptGetHashParam(Hash, HP_HASHVAL, Bytes.data(), &Size, 0));
            if (Size != Bytes.size()) throw winrt::hresult_error(E_UNEXPECTED);
            constexpr char Hex[] = "0123456789abcdef";
            std::string Result;
            for (auto Byte : Bytes) { Result += Hex[Byte >> 4]; Result += Hex[Byte & 15]; }
            return Result;
        }
    };

    std::string EncodeFileChunk(BYTE const* Bytes, DWORD Count)
    {
        DWORD Length = 0;
        winrt::check_bool(::CryptBinaryToStringA(Bytes, Count, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &Length));
        std::string Encoded(Length, '\0');
        winrt::check_bool(::CryptBinaryToStringA(Bytes, Count, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, Encoded.data(), &Length));
        Encoded.resize(Length);
        return Encoded;
    }

    std::vector<BYTE> DecodeFileChunk(std::string const& Text)
    {
        if (Text.empty()) return {};
        if (Text.size() > ((FileChunk + 2) / 3) * 4) throw winrt::hresult_invalid_argument();
        DWORD Length = 0;
        winrt::check_bool(::CryptStringToBinaryA(Text.c_str(), static_cast<DWORD>(Text.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &Length, nullptr, nullptr));
        if (Length > FileChunk) throw winrt::hresult_invalid_argument();
        std::vector<BYTE> Result(Length);
        winrt::check_bool(::CryptStringToBinaryA(Text.c_str(), static_cast<DWORD>(Text.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, Result.data(), &Length, nullptr, nullptr));
        Result.resize(Length);
        return Result;
    }

    winrt::handle OpenLocalFile(std::wstring const& Path, DWORD Access, DWORD Share, DWORD Creation)
    {
        HANDLE Raw = ::CreateFileW(Path.c_str(), Access, Share, nullptr, Creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (Raw == INVALID_HANDLE_VALUE) winrt::throw_last_error();
        winrt::handle Result(Raw);
        BY_HANDLE_FILE_INFORMATION Info{};
        winrt::check_bool(::GetFileInformationByHandle(Result.get(), &Info));
        if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            throw winrt::hresult_invalid_argument(L"Select a regular file.");
        return Result;
    }

    void WriteLocalFile(HANDLE File, BYTE const* Bytes, DWORD Length)
    {
        while (Length)
        {
            DWORD Written = 0;
            winrt::check_bool(::WriteFile(File, Bytes, Length, &Written, nullptr));
            if (!Written) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
            Bytes += Written;
            Length -= Written;
        }
    }

    std::wstring TemporaryFilePath(std::wstring const& Path)
    {
        GUID Id{};
        winrt::check_hresult(::CoCreateGuid(&Id));
        return Path + L".nanabox-" + std::wstring(::FromGuid(Id).c_str()) + L".partial";
    }

    Json ExecuteGuestTools(std::string const& Action, std::wstring const& LocalPath,
        std::string const& GuestName, bool Overwrite, GuestExchangeRequest const& Request)
    {
        if (Action == "Heartbeat" || Action == "GuestInformation" || Action == "Diagnostics" || Action == "TimeQuery")
        {
            auto Kind = Action == "TimeQuery" ? "TimeSync" : Action;
            auto Result = Request(Kind, "Query", Json::object());
            if (Action == "TimeQuery")
            {
                auto Now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                Result["HostUnixMilliseconds"] = Now;
                Result["GuestOffsetMilliseconds"] = Result.at("UnixMilliseconds").get<std::int64_t>() - Now;
            }
            return Result;
        }
        if (Action == "TimeSync")
        {
            auto Now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return Request("TimeSync", "Update", Json{{"UnixMilliseconds", Now}});
        }
        if ((Action != "FileUpload" && Action != "FileDownload") || LocalPath.empty() || GuestName.empty())
            throw winrt::hresult_invalid_argument(L"Specify a local path and a guest exchange filename.");

        std::string Token;
        auto Abort = Mile::ScopeExitTaskHandler([&]()
        {
            if (!Token.empty())
                try { Request("FileExchange", "Abort", Json{{"Token", Token}}); } catch (...) {}
        });
        FileDigest Digest;
        if (Action == "FileUpload")
        {
            auto File = OpenLocalFile(LocalPath, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
            LARGE_INTEGER Length{};
            winrt::check_bool(::GetFileSizeEx(File.get(), &Length));
            if (Length.QuadPart < 0) throw winrt::hresult_error(E_UNEXPECTED);
            auto Size = static_cast<std::uint64_t>(Length.QuadPart);
            auto Opened = Request("FileExchange", "BeginUpload",
                Json{{"Name", GuestName}, {"Size", Size}, {"Overwrite", Overwrite}});
            Token = Opened.at("Token").get<std::string>();
            auto ChunkSize = Opened.at("ChunkSize").get<DWORD>();
            if (!ChunkSize || ChunkSize > FileChunk || Opened.at("Size").get<std::uint64_t>() != Size)
                throw winrt::hresult_error(E_UNEXPECTED);
            std::array<BYTE, FileChunk> Buffer{};
            std::uint64_t Offset = 0;
            while (Offset < Size)
            {
                DWORD Count = 0;
                DWORD Wanted = static_cast<DWORD>((std::min)(static_cast<std::uint64_t>(ChunkSize), Size - Offset));
                winrt::check_bool(::ReadFile(File.get(), Buffer.data(), Wanted, &Count, nullptr));
                if (!Count) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
                Digest.Update(Buffer.data(), Count);
                auto Reply = Request("FileExchange", "Write", Json{{"Token", Token}, {"Offset", Offset},
                    {"Data", EncodeFileChunk(Buffer.data(), Count)}});
                Offset += Count;
                if (Reply.at("Offset").get<std::uint64_t>() != Offset)
                    throw winrt::hresult_error(E_UNEXPECTED, L"Guest acknowledged an incorrect file offset.");
            }
            auto Sha = Digest.Finish();
            auto Reply = Request("FileExchange", "Commit", Json{{"Token", Token}, {"Sha256", Sha}});
            if (Reply.at("Size").get<std::uint64_t>() != Size || Reply.at("Sha256") != Sha)
                throw winrt::hresult_error(E_FAIL, L"Guest file integrity check failed.");
            Token.clear();
            return Reply;
        }

        auto Opened = Request("FileExchange", "OpenDownload", Json{{"Name", GuestName}});
        Token = Opened.at("Token").get<std::string>();
        auto Size = Opened.at("Size").get<std::uint64_t>();
        if (Size > static_cast<std::uint64_t>(INT64_MAX)) throw winrt::hresult_invalid_argument();
        auto ChunkSize = Opened.at("ChunkSize").get<DWORD>();
        if (!ChunkSize || ChunkSize > FileChunk) throw winrt::hresult_error(E_UNEXPECTED);
        if (!Overwrite && ::GetFileAttributesW(LocalPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), L"The destination already exists.");
        auto Temporary = TemporaryFilePath(LocalPath);
        auto File = OpenLocalFile(Temporary, GENERIC_WRITE, 0, CREATE_NEW);
        auto Remove = Mile::ScopeExitTaskHandler([&]() { File.close(); ::DeleteFileW(Temporary.c_str()); });
        std::uint64_t Offset = 0;
        while (Offset < Size)
        {
            DWORD Wanted = static_cast<DWORD>((std::min)(static_cast<std::uint64_t>(ChunkSize), Size - Offset));
            auto Reply = Request("FileExchange", "Read", Json{{"Token", Token}, {"Offset", Offset}, {"Length", Wanted}});
            auto Bytes = DecodeFileChunk(Reply.at("Data").get<std::string>());
            if (Bytes.empty() || Bytes.size() > Wanted || Reply.at("Offset").get<std::uint64_t>() != Offset + Bytes.size())
                throw winrt::hresult_error(E_UNEXPECTED, L"Guest returned an invalid file chunk.");
            WriteLocalFile(File.get(), Bytes.data(), static_cast<DWORD>(Bytes.size()));
            Digest.Update(Bytes.data(), static_cast<DWORD>(Bytes.size()));
            Offset += Bytes.size();
            if (Reply.at("End").get<bool>() != (Offset == Size))
                throw winrt::hresult_error(E_UNEXPECTED);
        }
        auto Reply = Request("FileExchange", "FinishDownload", Json{{"Token", Token}});
        if (Reply.at("Size").get<std::uint64_t>() != Size || Reply.at("Sha256") != Digest.Finish())
            throw winrt::hresult_error(E_FAIL, L"Downloaded file integrity check failed.");
        Token.clear();
        winrt::check_bool(::FlushFileBuffers(File.get()));
        File.close();
        winrt::check_bool(::MoveFileExW(Temporary.c_str(), LocalPath.c_str(),
            MOVEFILE_WRITE_THROUGH | (Overwrite ? MOVEFILE_REPLACE_EXISTING : 0)));
        return Reply;
    }
}

std::string NanaBox::RunGuestToolsAction(winrt::guid const& RuntimeId, std::string const& Action,
    std::wstring const& LocalPath, std::string const& GuestName, bool Overwrite)
{
    RegisterGuestSocket();
    WSADATA Data{};
    auto Result = ::WSAStartup(MAKEWORD(2, 2), &Data);
    if (Result) winrt::throw_hresult(HRESULT_FROM_WIN32(Result));
    auto Cleanup = Mile::ScopeExitTaskHandler([]() { ::WSACleanup(); });
    std::atomic<bool> Stop{false};
    auto Reply = ExecuteGuestTools(Action, LocalPath, GuestName, Overwrite,
        [&](std::string const& Kind, std::string const& Verb, Json const& Settings)
        {
            return GuestRequest(RuntimeId, Json{{"Version", 1}, {"ResourceType", Kind},
                {"RequestType", Verb}, {"Settings", Settings}}, Stop, Kind == "Diagnostics" ? 40 : 15);
        });
    return Json{{"Success", true}, {"ResourceType", Action}, {"Data", Reply}}.dump(2);
}

std::string NanaBox::QueryGuestTools(winrt::guid const& RuntimeId, std::string const& Resource)
{
    return RunGuestToolsAction(RuntimeId, Resource, L"", "", false);
}

bool NanaBox::GuestToolsSelfTest()
{
    wchar_t Root[MAX_PATH + 1]{};
    auto Length = ::GetTempPathW(ARRAYSIZE(Root), Root);
    if (!Length || Length >= ARRAYSIZE(Root)) return false;
    auto Source = TemporaryFilePath(std::wstring(Root) + L"NanaBox-guest-test");
    auto Target = TemporaryFilePath(std::wstring(Root) + L"NanaBox-guest-test");
    auto Cleanup = Mile::ScopeExitTaskHandler([&]() { ::DeleteFileW(Source.c_str()); ::DeleteFileW(Target.c_str()); });
    std::vector<BYTE> Payload(2 * FileChunk + 117);
    for (size_t Index = 0; Index < Payload.size(); ++Index) Payload[Index] = static_cast<BYTE>(Index % 251);
    {
        auto File = OpenLocalFile(Source, GENERIC_WRITE, 0, CREATE_NEW);
        WriteLocalFile(File.get(), Payload.data(), static_cast<DWORD>(Payload.size()));
    }
    FileDigest Reference;
    Reference.Update(Payload.data(), static_cast<DWORD>(Payload.size()));
    auto Sha = Reference.Finish();
    std::vector<BYTE> Received;
    std::uint64_t Offset = 0;
    bool Corrupt = false, Aborted = false;
    auto Exchange = [&](std::string const& Kind, std::string const& Verb, Json const& Settings) -> Json
    {
        if (Kind != "FileExchange") throw winrt::hresult_error(E_UNEXPECTED);
        if (Verb == "BeginUpload" || Verb == "OpenDownload")
        {
            Offset = 0;
            if (Verb == "BeginUpload")
            {
                Received.clear();
                if (Settings.at("Size") != Payload.size()) throw winrt::hresult_error(E_UNEXPECTED);
            }
            return Json{{"Token", "00000000000000000000000000000001"}, {"Size", Payload.size()}, {"ChunkSize", FileChunk}};
        }
        if (Verb == "Abort") { Aborted = true; return Json{{"Aborted", true}}; }
        if (Verb == "Write")
        {
            if (Settings.at("Offset") != Offset) throw winrt::hresult_error(E_UNEXPECTED);
            auto Chunk = DecodeFileChunk(Settings.at("Data").get<std::string>());
            Received.insert(Received.end(), Chunk.begin(), Chunk.end());
            Offset += Chunk.size();
            return Json{{"Offset", Offset}};
        }
        if (Verb == "Read")
        {
            if (Settings.at("Offset") != Offset) throw winrt::hresult_error(E_UNEXPECTED);
            auto Count = static_cast<DWORD>((std::min)(static_cast<std::uint64_t>(Settings.at("Length").get<DWORD>()),
                static_cast<std::uint64_t>(Payload.size()) - Offset));
            auto Encoded = EncodeFileChunk(Payload.data() + Offset, Count);
            Offset += Count;
            return Json{{"Offset", Offset}, {"Data", Encoded}, {"End", Offset == Payload.size()}};
        }
        if (Verb == "Commit" && Settings.at("Sha256") != Sha) throw winrt::hresult_error(E_UNEXPECTED);
        if (Verb != "Commit" && Verb != "FinishDownload") throw winrt::hresult_error(E_UNEXPECTED);
        return Json{{"Name", "payload.bin"}, {"Size", Offset}, {"Sha256", Corrupt ? std::string(64, '0') : Sha}};
    };
    auto Upload = ExecuteGuestTools("FileUpload", Source, "payload.bin", false, Exchange);
    if (Upload.at("Sha256") != Sha || Received != Payload || Aborted) return false;
    Corrupt = true;
    bool Failed = false;
    try { ExecuteGuestTools("FileDownload", Target, "payload.bin", false, Exchange); }
    catch (...) { Failed = true; }
    if (!Failed || !Aborted || ::GetFileAttributesW(Target.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    Corrupt = false;
    Aborted = false;
    auto Download = ExecuteGuestTools("FileDownload", Target, "payload.bin", false, Exchange);
    if (Download.at("Sha256") != Sha || Aborted) return false;
    auto File = OpenLocalFile(Target, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    std::vector<BYTE> Actual(Payload.size());
    DWORD Count = 0;
    winrt::check_bool(::ReadFile(File.get(), Actual.data(), static_cast<DWORD>(Actual.size()), &Count, nullptr));
    if (Count != Actual.size() || Actual != Payload) return false;
    File.close();
    Failed = false;
    try { ExecuteGuestTools("FileDownload", Target, "payload.bin", false, Exchange); }
    catch (...) { Failed = true; }
    return Failed && Aborted;
}

std::string NanaBox::CheckMirroredHostSupport()
{
    Json Report = {{"Mode", "HostPreflight"}, {"ApiAvailable", false},
        {"WslDependencyAvailable", false}, {"HcnReachable", false}};
    HMODULE Module = ::LoadLibraryExW(L"computenetwork.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (Module)
    {
        auto Cleanup = Mile::ScopeExitTaskHandler([&]() { ::FreeLibrary(Module); });
        Json Missing = Json::array();
        for (auto Name : {"HcnCreateGuestNetworkService", "HcnModifyGuestNetworkService",
            "HcnDeleteGuestNetworkService", "HcnCloseGuestNetworkService",
            "HcnReserveGuestNetworkServicePortRange", "HcnReserveGuestNetworkServicePort",
            "HcnReleaseGuestNetworkServicePortReservationHandle", "HcnRegisterServiceCallback"})
            if (!::GetProcAddress(Module, Name)) Missing.push_back(Name);
        Report["ApiAvailable"] = Missing.empty();
        Report["MissingApis"] = Missing;
    }
    try
    {
        auto Path = WslDependencyPath();
        auto Attributes = ::GetFileAttributesW(Path.c_str());
        Report["WslDependencyAvailable"] = Attributes != INVALID_FILE_ATTRIBUTES &&
            !(Attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    catch (...) { Report["DependencyError"] = winrt::to_string(Mile::WinRT::ToHResultError().message()); }
    try
    {
        Json Networks = Json::array();
        for (auto const& Network : NanaBox::EnumerateHostNetworks())
            Networks.push_back({{"Id", GuidString(Network.Id)}, {"Name", winrt::to_string(Network.Name)},
                {"Type", winrt::to_string(Network.Type)}, {"Flags", Network.Flags}});
        Report["HcnReachable"] = true;
        Report["Networks"] = std::move(Networks);
    }
    catch (...) { Report["HcnError"] = winrt::to_string(Mile::WinRT::ToHResultError().message()); }
    Report["PrerequisitesAvailable"] = Report["ApiAvailable"].get<bool>() &&
        Report["WslDependencyAvailable"].get<bool>() && Report["HcnReachable"].get<bool>();
    return Report.dump(2);
}
