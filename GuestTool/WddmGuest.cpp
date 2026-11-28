// Windows GPU-PV driver integration for NanaBox GuestTool.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <hvsocket.h>
#include <nlohmann/json.hpp>
#include "WddmMapping.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    namespace fs = std::filesystem;
    namespace Wddm = NanaBox::Wddm;
    using Json = nlohmann::json;
    constexpr wchar_t ServiceName[] = L"NanaBoxGuestGpu";
    constexpr wchar_t StateKey[] = L"SOFTWARE\\NanaBox\\GuestTool\\Gpu";
    constexpr wchar_t EnvironmentKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    constexpr GUID ServiceId{27456, 0xfacb, 0x11e6, {0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3}};
    constexpr std::uint32_t MaximumMessage = 256 * 1024;
    std::atomic<bool> Stop{false};
    SERVICE_STATUS_HANDLE ServiceHandle = nullptr;
    std::wstring CirnoPath;

    [[noreturn]] void Error(char const* message)
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
    }

    std::wstring Wide(std::string const& s)
    {
        if (s.empty()) return {};
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
            static_cast<int>(s.size()), nullptr, 0);
        if (!count) Error("Invalid UTF-8");
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
            static_cast<int>(s.size()), result.data(), count)) Error("Invalid UTF-8");
        return result;
    }

    std::string Utf8(std::wstring const& s)
    {
        if (s.empty()) return {};
        int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
            static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (!count) Error("Invalid UTF-16");
        std::string result(static_cast<size_t>(count), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
            static_cast<int>(s.size()), result.data(), count, nullptr, nullptr)) Error("Invalid UTF-16");
        return result;
    }

    struct Key
    {
        HKEY Handle = nullptr;
        explicit Key(wchar_t const* path, REGSAM access)
        {
            LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, access | KEY_WOW64_64KEY, &Handle);
            if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category());
        }
        ~Key() { if (Handle) RegCloseKey(Handle); }
        Key(Key const&) = delete;
        Key& operator=(Key const&) = delete;
        std::wstring Read(wchar_t const* name, DWORD* kind = nullptr) const
        {
            DWORD size = 0, type = 0;
            auto status = RegQueryValueExW(Handle, name, nullptr, &type, nullptr, &size);
            if (status == ERROR_FILE_NOT_FOUND) return {};
            if (status != ERROR_SUCCESS || size > 128 * 1024 || size % sizeof(wchar_t) ||
                (type != REG_SZ && type != REG_EXPAND_SZ))
                throw std::runtime_error("Invalid registry value");
            std::vector<wchar_t> data(size / sizeof(wchar_t) + 1);
            status = RegQueryValueExW(Handle, name, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size);
            if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category());
            if (kind) *kind = type;
            return data.data();
        }
        void Write(wchar_t const* name, std::wstring const& value, DWORD type = REG_SZ) const
        {
            auto status = RegSetValueExW(Handle, name, 0, type,
                reinterpret_cast<BYTE const*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category());
        }
    };

    std::wstring SystemDirectory()
    {
        wchar_t buffer[MAX_PATH]{};
        auto size = GetSystemDirectoryW(buffer, MAX_PATH);
        if (!size || size >= MAX_PATH) Error("GetSystemDirectoryW");
        return buffer;
    }

    std::wstring WindowsDirectory()
    {
        wchar_t buffer[MAX_PATH]{};
        auto size = GetWindowsDirectoryW(buffer, MAX_PATH);
        if (!size || size >= MAX_PATH) Error("GetWindowsDirectoryW");
        return buffer;
    }

    bool Equal(std::wstring const& a, std::wstring const& b)
    {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    bool Mounted(fs::path const& mount, wchar_t const* label)
    {
        auto root = mount.wstring() + L"\\";
        wchar_t actual[MAX_PATH]{}, volume[MAX_PATH]{};
        return GetVolumePathNameW(root.c_str(), actual, MAX_PATH) &&
            Equal(root, actual) &&
            GetVolumeInformationW(root.c_str(), volume, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) &&
            Equal(volume, label);
    }

    class DriverMount
    {
        HANDLE Process = nullptr;
        fs::path Repository = fs::path(SystemDirectory()) / L"HostDriverStore" / L"FileRepository";
    public:
        ~DriverMount() { if (Process) CloseHandle(Process); }
        fs::path const& Root() const { return Repository; }
        bool Ready() const
        {
            return Mounted(Repository, L"NanaBox.HostDrivers") ||
                (Mounted(Repository.parent_path(), L"HostDriverStore") && fs::is_directory(Repository));
        }
        bool Ensure()
        {
            if (Ready()) return true;
            if (Process && WaitForSingleObject(Process, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(Process); Process = nullptr;
            }
            if (!Process)
            {
                if (!fs::exists(CirnoPath)) throw std::runtime_error("Mile.Cirno is missing");
                auto parent = Repository.parent_path();
                if (fs::exists(parent) && (GetFileAttributesW(parent.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw std::runtime_error("HostDriverStore is occupied by another mount");
                fs::create_directories(parent);
                if (fs::exists(Repository))
                {
                    auto attributes = GetFileAttributesW(Repository.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                        !fs::is_directory(Repository) || !fs::is_empty(Repository))
                        throw std::runtime_error("HostDriverStore/FileRepository is not an empty directory");
                }
                else fs::create_directory(Repository);
                std::wstring cmd = L"\"" + CirnoPath + L"\" Mount HvSocket 50001 NanaBox.HostDrivers \"" +
                    Repository.wstring() + L"\"";
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION process{};
                auto directory = fs::path(CirnoPath).parent_path().wstring();
                if (!CreateProcessW(CirnoPath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) Error("Start Mile.Cirno");
                CloseHandle(process.hThread);
                Process = process.hProcess;
            }
            for (unsigned i = 0; i < 30 && !Stop; ++i)
            {
                if (Ready()) return true;
                if (WaitForSingleObject(Process, 100) == WAIT_OBJECT_0) break;
            }
            return Ready();
        }
    };

    using Links = std::map<std::wstring, std::wstring>;

    std::wstring LinkId(std::wstring const& directory, std::wstring const& name)
    {
        return Wddm::Fold(directory) + L"\\" + Wddm::Fold(name);
    }

    fs::path Destination(std::wstring const& id)
    {
        auto slash = id.find(L'\\');
        if (slash == std::wstring::npos || !Wddm::SafeComponent(id.substr(slash + 1)))
            throw std::runtime_error("Invalid GPU destination");
        auto dir = Wddm::Fold(id.substr(0, slash));
        if (dir != L"system32" && dir != L"syswow64") throw std::runtime_error("Invalid GPU directory");
        return fs::path(WindowsDirectory()) / dir / id.substr(slash + 1);
    }

    std::wstring NormalizeLink(fs::path const& path)
    {
        auto value = path.lexically_normal().wstring();
        if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
        if (value.rfind(L"\\??\\", 0) == 0) value.erase(0, 4);
        return value;
    }

    bool OwnedLink(fs::path const& destination, std::wstring const& expected)
    {
        std::error_code ec;
        if (!fs::is_symlink(fs::symlink_status(destination, ec)) || ec) return false;
        auto actual = fs::read_symlink(destination, ec);
        return !ec && Equal(NormalizeLink(actual), NormalizeLink(fs::path(expected)));
    }

    bool ExistsEntry(fs::path const& destination)
    {
        std::error_code ec;
        auto info = fs::symlink_status(destination, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) throw std::system_error(ec);
        return info.type() != fs::file_type::not_found;
    }

    void InstallLink(fs::path const& destination, fs::path const& source, std::wstring const* previous)
    {
        if (previous && OwnedLink(destination, *previous))
        {
            if (Equal(*previous, source.wstring())) return;
            auto temporary = destination;
            temporary += L".nanabox-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            if (!CreateSymbolicLinkW(temporary.c_str(), source.c_str(), 0)) Error("Create GPU symbolic link");
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                auto error = GetLastError();
                DeleteFileW(temporary.c_str());
                SetLastError(error);
                Error("Replace GPU symbolic link");
            }
            return;
        }
        if (ExistsEntry(destination)) throw std::runtime_error("Existing guest file or link is not owned by GuestTool");
        if (!CreateSymbolicLinkW(destination.c_str(), source.c_str(), 0)) Error("Create GPU symbolic link");
    }

    std::vector<std::wstring> SplitPath(std::wstring_view text)
    {
        std::vector<std::wstring> result;
        if (text.empty()) return result;
        while (true)
        {
            auto end = text.find(L';');
            result.emplace_back(text.substr(0, end));
            if (end == std::wstring_view::npos) return result;
            text.remove_prefix(end + 1);
        }
    }

    std::wstring JoinPath(std::vector<std::wstring> const& entries)
    {
        std::wstring result;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i) result += L';';
            result += entries[i];
        }
        return result;
    }

    bool Contains(std::vector<std::wstring> const& entries, std::wstring const& value)
    {
        return std::any_of(entries.begin(), entries.end(), [&](auto const& entry) { return Equal(entry, value); });
    }

    void UpdateEnvironment(Key const& state, wchar_t const* name, wchar_t const* ownedName,
        std::vector<std::wstring> const& desired)
    {
        Key environment(EnvironmentKey, KEY_READ | KEY_SET_VALUE);
        DWORD type = REG_EXPAND_SZ;
        auto original = environment.Read(name, &type);
        auto entries = SplitPath(original);
        auto owned = SplitPath(state.Read(ownedName));
        std::vector<std::wstring> nextOwned;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](auto const& value)
            { return Contains(owned, value) && !Contains(desired, value); }), entries.end());
        for (auto const& value : desired)
        {
            if (!Contains(entries, value))
            {
                entries.push_back(value);
                nextOwned.push_back(value);
            }
            else if (Contains(owned, value)) nextOwned.push_back(value);
        }
        auto updated = JoinPath(entries);
        if (updated.size() >= 32767) throw std::runtime_error("GPU environment exceeds Windows size limit");
        if (updated != original) environment.Write(name, updated, type);
        state.Write(ownedName, JoinPath(nextOwned));
    }

    std::uint64_t FileVersion(fs::path const& path)
    {
        DWORD ignored = 0;
        auto bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (!bytes || bytes > 16 * 1024 * 1024) return 0;
        std::vector<char> data(bytes);
        if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data())) return 0;
        VS_FIXEDFILEINFO* info = nullptr;
        UINT size = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size) ||
            size < sizeof(VS_FIXEDFILEINFO) || !info || info->dwSignature != 0xfeef04bd) return 0;
        return (static_cast<std::uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
    }

    bool IsNewer(fs::path const& source, fs::path const& target)
    {
        auto ext = Wddm::Fold(target.extension().wstring());
        if (ext == L".dll" || ext == L".exe")
        {
            auto a = FileVersion(source), b = FileVersion(target);
            if (a != b) return a > b;
        }
        return fs::last_write_time(source) > fs::last_write_time(target);
    }

    class Manager
    {
        DriverMount Mount;
        Json Last = {{"Mounted", false}, {"Files", Json::array()}, {"Errors", Json::array()}};
        Links ReadLinks(Key const& key)
        {
            auto value = key.Read(L"Links");
            if (value.empty()) return {};
            auto object = Json::parse(Utf8(value));
            Links result;
            for (auto it = object.begin(); it != object.end(); ++it)
                result.emplace(Wide(it.key()), Wide(it.value().get<std::string>()));
            return result;
        }
        void SaveLinks(Key const& key, Links const& links)
        {
            Json object = Json::object();
            for (auto const& pair : links) object[Utf8(pair.first)] = Utf8(pair.second);
            key.Write(L"Links", Wide(object.dump()));
        }
    public:
        Json Query()
        {
            auto result = Last;
            result["Mounted"] = Mount.Ready();
            return result;
        }
        Json Update(Json const& settings)
        {
            auto const& shares = settings.at("Shares");
            auto const& wddm = settings.at("Wddm");
            if (!shares.is_array() || !wddm.at("Packages").is_array() || !wddm.at("Files").is_array() ||
                !wddm.at("Errors").is_array() || wddm.at("Packages").size() > 128 || wddm.at("Files").size() > 1024)
                throw std::runtime_error("Invalid WDDM inventory");
            bool shared = false;
            for (auto const& share : shares)
                if (share.value("Name", std::string()) == "NanaBox.HostDrivers" &&
                    share.value("Port", 0) == 50001 && share.value("ReadOnly", false)) shared = true;
            if (!shared) throw std::runtime_error("Read-only host GPU repository is unavailable");
            Last = {{"Mounted", false}, {"Files", Json::array()}, {"Errors", wddm.at("Errors")}};
            if (!Mount.Ensure())
            {
                Last["Errors"].push_back("GPU repository is not mounted");
                return Last;
            }
            Last["Mounted"] = true;
            std::set<std::wstring> packages;
            std::vector<std::wstring> directories;
            for (auto const& value : wddm.at("Packages"))
            {
                auto package = Wide(value.get<std::string>());
                if (!Wddm::SafeComponent(package)) throw std::runtime_error("Invalid GPU package");
                packages.insert(Wddm::Fold(package));
                auto folder = Mount.Root() / package;
                if (fs::is_directory(folder) && !Contains(directories, folder.wstring()))
                    directories.push_back(folder.wstring());
            }
            struct Item { fs::path Source; bool Newer; };
            std::map<std::wstring, Item> desired;
            std::set<std::wstring> collisions;
            for (auto const& file : wddm.at("Files"))
            {
                auto relative = Wide(file.at("Source").get<std::string>());
                auto destination = Wide(file.at("Destination").get<std::string>());
                auto directory = Wide(file.at("Directory").get<std::string>());
                if (!Wddm::SafeRelativePath(relative) || !Wddm::SafeComponent(destination) ||
                    (directory != L"System32" && directory != L"SysWOW64") ||
                    relative.find(L'\\') == std::wstring::npos ||
                    !packages.count(Wddm::Fold(relative.substr(0, relative.find(L'\\')))))
                    throw std::runtime_error("Invalid GPU file mapping");
                auto id = LinkId(directory, destination);
                auto source = Mount.Root() / relative;
                auto inserted = desired.emplace(id, Item{source, file.at("WhenNewer").get<bool>()});
                if (!inserted.second && !Equal(inserted.first->second.Source.wstring(), source.wstring()))
                    collisions.insert(id);
            }
            Key key(StateKey, KEY_READ | KEY_SET_VALUE);
            auto links = ReadLinks(key);
            for (auto const& entry : desired)
            {
                auto const& id = entry.first;
                auto const& item = entry.second;
                Json status = {{"Destination", Utf8(id)}, {"Source", Utf8(item.Source.wstring())}};
                try
                {
                    if (collisions.count(id)) throw std::runtime_error("Multiple GPU packages request the same file");
                    auto target = Destination(id);
                    if (!fs::is_directory(target.parent_path())) throw std::runtime_error("Guest system directory is missing");
                    if (!fs::is_regular_file(item.Source)) throw std::runtime_error("Driver file is unavailable");
                    auto existing = links.find(id);
                    if (existing != links.end() && !OwnedLink(target, existing->second))
                    {
                        links.erase(existing);
                        existing = links.end();
                        SaveLinks(key, links);
                    }
                    if (existing == links.end() && ExistsEntry(target) && item.Newer &&
                        fs::is_regular_file(target) && !IsNewer(item.Source, target))
                    {
                        status["State"] = "KeptNewerOrEqualGuestFile";
                    }
                    else
                    {
                        auto previous = existing == links.end() ? nullptr : &existing->second;
                        InstallLink(target, item.Source, previous);
                        links[id] = item.Source.wstring();
                        SaveLinks(key, links);
                        status["State"] = "Linked";
                    }
                    auto folder = item.Source.parent_path().wstring();
                    if (!Contains(directories, folder)) directories.push_back(folder);
                }
                catch (std::exception const& error)
                {
                    status["State"] = "Error";
                    status["Error"] = error.what();
                    Last["Errors"].push_back(Utf8(id) + ": " + error.what());
                }
                Last["Files"].push_back(status);
            }
            if (Last["Errors"].empty())
            {
                for (auto it = links.begin(); it != links.end();)
                {
                    if (desired.count(it->first)) { ++it; continue; }
                    auto target = Destination(it->first);
                    if (OwnedLink(target, it->second) && !DeleteFileW(target.c_str()))
                    {
                        Last["Errors"].push_back("Cannot remove obsolete GPU link: " + Utf8(it->first));
                        ++it;
                    }
                    else { it = links.erase(it); SaveLinks(key, links); }
                }
                UpdateEnvironment(key, L"Path", L"OwnedPath", directories);
                UpdateEnvironment(key, L"LIB", L"OwnedLib", directories);
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                    reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 1000, &ignored);
            }
            return Last;
        }
        void Cleanup()
        {
            Key key(StateKey, KEY_READ | KEY_SET_VALUE);
            auto links = ReadLinks(key);
            for (auto const& link : links)
            {
                auto target = Destination(link.first);
                if (OwnedLink(target, link.second) && !DeleteFileW(target.c_str()))
                    Error("Remove GPU link");
            }
            SaveLinks(key, {});
            UpdateEnvironment(key, L"Path", L"OwnedPath", {});
            UpdateEnvironment(key, L"LIB", L"OwnedLib", {});
        }
    };

    struct Socket
    {
        SOCKET Handle = INVALID_SOCKET;
        explicit Socket(SOCKET s) : Handle(s) {}
        ~Socket() { if (Handle != INVALID_SOCKET) closesocket(Handle); }
        Socket(Socket const&) = delete;
        Socket& operator=(Socket const&) = delete;
    };

    void Transfer(SOCKET socket, char* bytes, size_t length, bool write)
    {
        while (length)
        {
            int chunk = static_cast<int>(std::min<size_t>(length, 65536));
            int count = write ? send(socket, bytes, chunk, 0) : recv(socket, bytes, chunk, 0);
            if (count <= 0) throw std::runtime_error("Hyper-V socket transfer failed");
            bytes += count;
            length -= static_cast<size_t>(count);
        }
    }

    Json Receive(SOCKET socket)
    {
        std::uint32_t length = 0;
        Transfer(socket, reinterpret_cast<char*>(&length), sizeof(length), false);
        length = ntohl(length);
        if (!length || length > MaximumMessage) throw std::runtime_error("Invalid message length");
        std::string body(length, '\0');
        Transfer(socket, body.data(), body.size(), false);
        return Json::parse(body);
    }

    void Send(SOCKET socket, Json const& response)
    {
        auto body = response.dump();
        if (body.size() > MaximumMessage)
            body = R"({"Version":1,"Success":false,"Error":"GPU response exceeds size limit"})";
        auto length = htonl(static_cast<u_long>(body.size()));
        Transfer(socket, reinterpret_cast<char*>(&length), sizeof(length), true);
        Transfer(socket, body.data(), body.size(), true);
    }

    void Serve()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
        struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
        Socket server(socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW));
        if (server.Handle == INVALID_SOCKET) throw std::runtime_error("Cannot open Hyper-V socket");
        SOCKADDR_HV address{};
        address.Family = AF_HYPERV;
        address.VmId = HV_GUID_PARENT;
        address.ServiceId = ServiceId;
        if (bind(server.Handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            listen(server.Handle, 8)) throw std::runtime_error("Cannot bind GPU GuestTool socket");
        Manager manager;
        while (!Stop)
        {
            fd_set readable;
            FD_ZERO(&readable); FD_SET(server.Handle, &readable);
            timeval timeout{0, 250000};
            auto selected = select(0, &readable, nullptr, nullptr, &timeout);
            if (selected == SOCKET_ERROR) throw std::runtime_error("Hyper-V socket select failed");
            if (!selected) continue;
            SOCKADDR_HV peer{};
            int peerLength = sizeof(peer);
            Socket client(accept(server.Handle, reinterpret_cast<sockaddr*>(&peer), &peerLength));
            if (client.Handle == INVALID_SOCKET) continue;
            // The listening socket is bound to the parent partition.
            DWORD deadline = 5000;
            setsockopt(client.Handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&deadline), sizeof(deadline));
            setsockopt(client.Handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&deadline), sizeof(deadline));
            try
            {
                auto request = Receive(client.Handle);
                if (request.at("Version") != 1 || request.at("ResourceType") != "Plan9Shares")
                    throw std::runtime_error("Unsupported GPU GuestTool request");
                Json response;
                if (request.at("RequestType") == "Query") response = manager.Query();
                else if (request.at("RequestType") == "Update") response = manager.Update(request.at("Settings"));
                else throw std::runtime_error("Unsupported GPU mount operation");
                Send(client.Handle, {{"Version", 1}, {"Success", true}, {"Data", response}});
            }
            catch (std::exception const& error)
            {
                try { Send(client.Handle, {{"Version", 1}, {"Success", false}, {"Error", error.what()}}); }
                catch (...) {}
            }
        }
    }

    void Status(DWORD state, DWORD error = NO_ERROR)
    {
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = state;
        status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        status.dwWin32ExitCode = error;
        status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
        if (ServiceHandle) SetServiceStatus(ServiceHandle, &status);
    }

    void WINAPI Control(DWORD control)
    {
        if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
        {
            Stop = true;
            Status(SERVICE_STOP_PENDING);
        }
    }

    void WINAPI ServiceMain(DWORD, LPWSTR*)
    {
        ServiceHandle = RegisterServiceCtrlHandlerW(ServiceName, Control);
        if (!ServiceHandle) return;
        Status(SERVICE_RUNNING);
        try
        {
            Key key(StateKey, KEY_READ);
            CirnoPath = key.Read(L"CirnoPath");
            if (CirnoPath.empty() || !fs::path(CirnoPath).is_absolute())
                throw std::runtime_error("Mile.Cirno path is not configured");
            Serve();
            Status(SERVICE_STOPPED);
        }
        catch (...) { Status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR); }
    }

    int SelfTest()
    {
        auto m = Wddm::MakeMapping(L"gpu.inf_amd64_test", {L"sub\\source.dll", L"target.dll"}, true, true);
        if (!m || m->Source != L"gpu.inf_amd64_test\\sub\\source.dll" ||
            m->Destination != L"target.dll" || m->Directory != L"SysWOW64" || !m->WhenNewer) return 1;
        for (auto bad : {L"..\\x.dll", L"x:stream", L"COM1.dll", L"\\x.dll", L"x\\", L"x. "})
            if (Wddm::MakeMapping(L"gpu", {bad}, false, false)) return 2;
        if (Wddm::MakeMapping(L"gpu", {L"x.dll", L"\\test.dll"}, false, false)) return 3;
        if (JoinPath(SplitPath(L"C:\\first;;C:\\second;")) != L"C:\\first;;C:\\second;") return 4;
        auto root = fs::temp_directory_path() / (L"NanaBox-GpuTest-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directory(root);
        struct Remove { fs::path Path; ~Remove() { std::error_code ec; fs::remove_all(Path, ec); } } remove{root};
        auto original = root / L"original.dll", source = root / L"source.dll", link = root / L"target.dll";
        HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 5;
        CloseHandle(file);
        file = CreateFileW(original.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 6;
        CloseHandle(file);
        bool refused = false;
        try { InstallLink(original, source, nullptr); } catch (...) { refused = true; }
        if (!refused || !fs::is_regular_file(original)) return 7;
        InstallLink(link, source, nullptr);
        if (!OwnedLink(link, source.wstring())) return 8;
        auto previous = source.wstring();
        InstallLink(link, original, &previous);
        if (!OwnedLink(link, original.wstring()) || !fs::is_regular_file(source)) return 9;
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") return SelfTest();
        if (argc == 2 && std::wstring_view(argv[1]) == L"--cleanup")
        {
            Manager manager; manager.Cleanup(); return 0;
        }
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(ServiceName), ServiceMain}, {nullptr, nullptr}};
        if (!StartServiceCtrlDispatcherW(table)) Error("StartServiceCtrlDispatcherW");
        return 0;
    }
    catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
