// Windows GPU-PV driver integration for NanaBox GuestTool.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <hvsocket.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <sfc.h>
#include <winternl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cfg.h>
#include <functional>
#include <nlohmann/json.hpp>
#include "WddmMapping.h"
#include "VsmbProtocol.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
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
#pragma comment(lib, "sfc.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace
{
    namespace fs = std::filesystem;
    namespace Wddm = NanaBox::Wddm;
    using Json = nlohmann::json;
    constexpr wchar_t ServiceName[] = L"NanaBoxGuestGpu";
    constexpr wchar_t StateKey[] = L"SOFTWARE\\NanaBox\\GuestTool\\Gpu";
    constexpr wchar_t EnvironmentKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    constexpr GUID ParentId{0xa42e7cda, 0xd03f, 0x480c, {0x9c, 0xc2, 0xa4, 0xde, 0x20, 0xab, 0xb8, 0x78}};
    constexpr GUID ServiceId{27456, 0xfacb, 0x11e6, {0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3}};
    constexpr std::uint32_t MaximumMessage = 256 * 1024;
    std::atomic<bool> Stop{false};
    SERVICE_STATUS_HANDLE ServiceHandle = nullptr;
    std::wstring CirnoPath, WinFspPlan9Path;
    bool AutomaticRestart = true;
    bool NativeMode = true;
    std::atomic<bool> NativeActive{false}, Plan9Active{false};
    void Status(DWORD state,DWORD error);

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
            if (status != ERROR_SUCCESS || size > 2 * 1024 * 1024 || size % sizeof(wchar_t) ||
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

    struct VirtualSmbDescriptor
    {
        std::string Name;
        std::wstring WideName;
        bool ReadOnly;
        bool Internal;
        bool AutoMount = true;
        bool NoDirectmap = false;
        Json Identity() const
        {
            return {{"Name",Name},{"ReadOnly",ReadOnly},{"AutoMount",AutoMount},{"NoDirectmap",NoDirectmap}};
        }
    };

    std::vector<VirtualSmbDescriptor> ParseVirtualSmbInventory(Json const& shares)
    {
        if (!shares.is_array() || shares.size() > 224)
            throw std::runtime_error("Invalid Virtual SMB inventory");
        std::vector<VirtualSmbDescriptor> result;
        for (auto const& share : shares)
        {
            auto name = share.at("Name").get<std::string>();
            auto wide = Wide(name);
            auto readOnly = share.at("ReadOnly").get<bool>();
            if (name.size() > 200 || !Wddm::SafeComponent(wide) || wide.front() == L' ' ||
                wide.find(L',') != std::wstring::npos ||
                std::any_of(result.begin(), result.end(), [&](auto const& entry) { return Equal(entry.WideName, wide); }))
                throw std::runtime_error("Invalid or duplicate Virtual SMB share name");
            bool internal = Wddm::ExtraShareName(name);
            for (auto const& reserved : {L"NanaBox.HostDrivers", L"HostDriverStore", L"NanaBox.HostLxssLib"})
            {
                if (!Equal(wide, reserved)) continue;
                if (wide != reserved) throw std::runtime_error("Invalid reserved Virtual SMB share name");
                internal = true;
            }
            constexpr std::wstring_view prefix = L"NanaBox.GpuExtra.";
            if (wide.size() >= prefix.size() &&
                Equal(wide.substr(0, prefix.size()), std::wstring(prefix)) && !internal)
                throw std::runtime_error("Invalid supplemental GPU share name");
            if (internal && !readOnly) throw std::runtime_error("GPU share must be read-only");
            result.push_back({std::move(name), std::move(wide), readOnly, internal,
                share.value("AutoMount",true),share.value("NoDirectmap",false)});
        }
        return result;
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

#include "NativeVsmb.h"
#include "WindowsPlan9Mounts.h"
#include "WindowsVsmbMounts.h"

    class DriverMount
    {
        HANDLE Process = nullptr;
        fs::path Repository;
        std::wstring Share;
        bool Legacy;
    public:
        DriverMount() : Repository(fs::path(SystemDirectory()) / L"HostDriverStore" / L"FileRepository"),
            Share(L"NanaBox.HostDrivers"), Legacy(true) {}
        explicit DriverMount(std::wstring name) :
            Repository(fs::path(SystemDirectory()) / L"HostDriverStoreExtras" / name),
            Share(std::move(name)), Legacy(false) {}
        DriverMount(DriverMount const&) = delete;
        DriverMount& operator=(DriverMount const&) = delete;
        ~DriverMount() { if (Process) CloseHandle(Process); }
        fs::path const& Root() const { return Repository; }
        bool Ready() const
        {
            if (NativeMode) return VsmbClient.Ready(Repository, Share, true);
            return Mounted(Repository, Share.c_str()) ||
                (Legacy && Mounted(Repository.parent_path(), L"HostDriverStore") && fs::is_directory(Repository));
        }
        bool Ensure(unsigned attempts = 30)
        {
            if (NativeMode) return VsmbClient.Ensure(Repository, Share, true);
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
                std::wstring cmd = L"\"" + CirnoPath + L"\" Mount HvSocket 50001 " + Share + L" \"" +
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
            for (unsigned i = 0; i < attempts && !Stop; ++i)
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
        auto slash = id.find_last_of(L'\\');
        auto suffix = std::wstring_view(L".nanabox-gpu-original");
        if (slash == std::wstring::npos || !Wddm::SafeComponent(id.substr(slash + 1)) ||
            !Wddm::SafeSystemDirectory(id.substr(0, slash)) ||
            (id.size() >= suffix.size() && Wddm::Fold(id.substr(id.size() - suffix.size())) == suffix))
            throw std::runtime_error("Invalid GPU destination");
        return fs::path(WindowsDirectory()) / id;
    }

    void ValidateDestinationParent(fs::path const& target, bool create)
    {
        auto relative = target.parent_path().lexically_relative(fs::path(WindowsDirectory()));
        if (!Wddm::SafeSystemDirectory(relative.wstring())) throw std::runtime_error("Invalid GPU directory");
        auto path = fs::path(WindowsDirectory());
        for (auto const& part : relative)
        {
            path /= part;
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
            {
                auto error = GetLastError();
                if (!create || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND))
                    throw std::runtime_error("Guest system directory is missing");
                if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    Error("Create GPU driver directory");
                attr = GetFileAttributesW(path.c_str());
            }
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
                (attr & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Unsafe GPU destination directory");
        }
    }

    fs::path BackupPath(fs::path const& destination)
    {
        auto result = destination;
        result += L".nanabox-gpu-original";
        return result;
    }

    bool PlainFile(fs::path const& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
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

    void MoveOriginal(fs::path const& destination)
    {
        auto backup = BackupPath(destination);
        if (!PlainFile(destination) || SfcIsFileProtected(nullptr, destination.c_str()) ||
            ExistsEntry(backup)) throw std::runtime_error("Guest file cannot be safely replaced");
        if (!MoveFileExW(destination.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH))
            Error("Preserve original GPU file");
    }

    void RestoreOriginal(fs::path const& destination)
    {
        auto backup = BackupPath(destination);
        if (!PlainFile(backup) || ExistsEntry(destination))
            throw std::runtime_error("Original GPU file cannot be safely restored");
        if (!MoveFileExW(backup.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            Error("Restore original GPU file");
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
        std::vector<std::wstring> const& desired, bool removeObsolete = true)
    {
        Key environment(EnvironmentKey, KEY_READ | KEY_SET_VALUE);
        DWORD type = REG_EXPAND_SZ;
        auto original = environment.Read(name, &type);
        auto entries = SplitPath(original);
        auto owned = SplitPath(state.Read(ownedName));
        std::vector<std::wstring> nextOwned;
        if (removeObsolete)
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](auto const& value)
                { return Contains(owned, value) && !Contains(desired, value); }), entries.end());
        else
            for (auto const& value : owned)
                if (Contains(entries, value) && !Contains(desired, value)) nextOwned.push_back(value);
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

    // Verify that the Windows memory manager can create an executable image
    // section on the mapped filesystem before restarting a GPU.
    void VerifyImageMapping(fs::path const& path)
    {
        auto extension = Wddm::Fold(path.extension().wstring());
        if (extension != L".dll" && extension != L".exe") return;
        NativeHandle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_EXECUTE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.Valid()) Error("Open mapped driver image");
        NativeHandle section(CreateFileMappingW(file.Get(), nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr));
        if (!section.Valid()) Error("Create mapped driver image section");
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

    // Compare canonical inventories, ignoring enumeration order.
    std::string DriverGeneration(Json const& inventory)
    {
        Json canonical = Json::object();
        for (auto name : {"Packages", "Files", "ExtraFiles", "DriverVersions"})
        {
            std::vector<std::string> entries;
            for (auto const& value : inventory.value(name, Json::array()))
                entries.push_back(value.dump());
            std::sort(entries.begin(), entries.end());
            entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
            canonical[name] = entries;
        }
        return canonical.dump();
    }

    struct RenderDevice
    {
        std::wstring Instance;
        bool Disabled = false;
    };

    template<class Visitor>
    void VisitVirtualRenderDevices(Visitor visit)
    {
        constexpr GUID display{0x4d36e968, 0xe325, 0x11ce, {0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
        HDEVINFO set = SetupDiGetClassDevsW(&display, nullptr, nullptr, DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) Error("Enumerate virtual render devices");
        struct Close { HDEVINFO Set; ~Close() { SetupDiDestroyDeviceInfoList(Set); } } close{set};
        for (DWORD index = 0;; ++index)
        {
            SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(set, index, &device))
            {
                if (GetLastError() != ERROR_NO_MORE_ITEMS) Error("Enumerate display devices");
                break;
            }
            wchar_t id[4096]{}, service[256]{}, instance[4096]{};
            DWORD type = 0, size = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type,
                reinterpret_cast<PBYTE>(id), sizeof(id), &size) || type != REG_MULTI_SZ ||
                size > sizeof(id) || size < 2 * sizeof(wchar_t)) continue;
            if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_SERVICE, &type,
                reinterpret_cast<PBYTE>(service), sizeof(service), nullptr) || type != REG_SZ) continue;
            service[255] = 0;
            bool match = false;
            auto end = id + size / sizeof(wchar_t);
            for (auto value = id; value < end && *value;)
            {
                auto last = std::find(value, end, L'\0');
                if (last == end) break;
                if (Wddm::IsVirtualRenderDevice(std::wstring_view(value, last - value), service)) match = true;
                value = last + 1;
            }
            if (!match || !SetupDiGetDeviceInstanceIdW(set, &device, instance,
                static_cast<DWORD>(std::size(instance)), nullptr)) continue;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS)
                throw std::runtime_error("Cannot query virtual render device state");
            visit(set, device, RenderDevice{instance, (status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED});
        }
    }

    std::vector<RenderDevice> VirtualRenderDevices()
    {
        std::vector<RenderDevice> devices;
        VisitVirtualRenderDevices([&](HDEVINFO, SP_DEVINFO_DATA&, RenderDevice const& device)
            { devices.push_back(device); });
        return devices;
    }

    bool RestartVirtualRenderDevice(std::wstring const& instance)
    {
        bool found = false, reboot = false;
        VisitVirtualRenderDevices([&](HDEVINFO set, SP_DEVINFO_DATA& device, RenderDevice const& candidate)
        {
            if (!Equal(instance, candidate.Instance)) return;
            found = true;
            if (candidate.Disabled) throw std::runtime_error("Virtual render device is disabled");
            SP_PROPCHANGE_PARAMS change{};
            change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            change.StateChange = DICS_PROPCHANGE;
            change.Scope = DICS_FLAG_GLOBAL;
            if (!SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader, sizeof(change)) ||
                !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device))
                Error("Restart virtual render device");
            SP_DEVINSTALL_PARAMS_W params{}; params.cbSize = sizeof(params);
            if (!SetupDiGetDeviceInstallParamsW(set, &device, &params))
                Error("Query virtual render device restart result");
            reboot = (params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
            if (!reboot)
            {
                ULONG status = 0, problem = 0;
                if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS ||
                    !(status & DN_STARTED) || (status & DN_HAS_PROBLEM))
                    throw std::runtime_error("Virtual render device did not return to the started state");
            }
        });
        if (!found) throw std::runtime_error("Virtual render device is no longer present");
        return reboot;
    }

    class RestartTracker
    {
        struct Entry
        {
            std::string Generation;
            std::chrono::steady_clock::time_point Retry{};
            std::string State, Error;
        };
        std::map<std::wstring, Entry> Devices;
    public:
        Json Update(std::string const& generation, std::vector<RenderDevice> const& devices,
            std::function<bool(std::wstring const&)> const& restart,
            std::chrono::steady_clock::time_point now)
        {
            std::set<std::wstring> present;
            Json result = Json::array();
            for (auto const& device : devices)
            {
                auto id = Wddm::Fold(device.Instance);
                present.insert(id);
                auto& entry = Devices[id];
                if (entry.Generation != generation) entry = Entry{generation, {}, {}, {}};
                if (device.Disabled)
                {
                    // Respect administrator-disabled devices and retry only after they are enabled.
                    entry = Entry{generation, {}, "Disabled", {}};
                }
                else if (entry.State != "Restarted" && entry.State != "RebootRequired" && now >= entry.Retry)
                {
                    try
                    {
                        entry.State = restart(device.Instance) ? "RebootRequired" : "Restarted";
                        entry.Error.clear();
                    }
                    catch (std::exception const& error)
                    {
                        entry.State = "RetryPending";
                        entry.Error = error.what();
                        entry.Retry = now + std::chrono::minutes(1);
                    }
                }
                result.push_back({{"InstanceId", Utf8(device.Instance)}, {"State", entry.State}, {"Error", entry.Error}});
            }
            for (auto it = Devices.begin(); it != Devices.end();)
                if (!present.count(it->first)) it = Devices.erase(it); else ++it;
            return result;
        }
    };

    // Recover an interrupted atomic link replacement without treating the
    // previous journal entry as the owner of a different guest file.
    void ReconcilePendingLink(fs::path const& target, std::wstring const& id,
        std::wstring const& expected, Links& links)
    {
        if (OwnedLink(target, expected)) links[id] = expected;
        else
        {
            auto previous = links.find(id);
            if (previous != links.end() && !OwnedLink(target, previous->second))
                links.erase(previous);
        }
    }

    class GpuMappingLifecycle
    {
        bool Enabled = false;
        bool Clean = false;
    public:
        bool Active() const { return Enabled; }
        template<class Cleanup>
        void Configure(bool enabled, Cleanup cleanup)
        {
            Enabled = enabled;
            if (enabled) Clean = false;
            else if (!Clean)
            {
                cleanup();
                Clean = true;
            }
        }
    };

    class Manager
    {
        GpuMappingLifecycle Lifecycle;
        WindowsPlan9Mounts UserPlan9;
        WindowsVsmbMounts UserVsmb;
        DriverMount Mount;
        RestartTracker Restart;
        std::string VerifiedGeneration;
        std::set<std::wstring> VerifiedImages;
        std::map<std::string, std::unique_ptr<DriverMount>> ExtraMounts;
        Json Last = {{"Mounted", false}, {"Files", Json::array()}, {"Errors", Json::array()}};
        Links ReadLinks(Key const& key, wchar_t const* name = L"Links")
        {
            auto value = key.Read(name);
            if (value.empty()) return {};
            auto object = Json::parse(Utf8(value));
            Links result;
            for (auto it = object.begin(); it != object.end(); ++it)
                result.emplace(Wide(it.key()), Wide(it.value().get<std::string>()));
            return result;
        }
        void SaveLinks(Key const& key, Links const& links, wchar_t const* name = L"Links")
        {
            Json object = Json::object();
            for (auto const& pair : links) object[Utf8(pair.first)] = Utf8(pair.second);
            key.Write(name, Wide(object.dump()));
        }

        void RecoverPending(Key const& key, Links& links, Links& pending)
        {
            for (auto it = pending.begin(); it != pending.end();)
            {
                auto target = Destination(it->first);
                ValidateDestinationParent(target, false);
                ReconcilePendingLink(target, it->first, it->second, links);
                SaveLinks(key, links);
                it = pending.erase(it);
                SaveLinks(key, pending, L"PendingLinks");
            }
        }

        void RecoverBackups(Key const& key, Links& links, Links& backups)
        {
            for (auto it = backups.begin(); it != backups.end();)
            {
                auto const id = it->first;
                auto target = Destination(id);
                ValidateDestinationParent(target, false);
                auto backup = BackupPath(target);
                auto link = links.find(id);
                bool owned = link != links.end() && OwnedLink(target, link->second);
                if (ExistsEntry(backup))
                {
                    if (!PlainFile(backup)) throw std::runtime_error("Unsafe original GPU backup");
                    if (owned) { ++it; continue; }
                    if (OwnedLink(target, it->second))
                    {
                        links[id] = it->second;
                        SaveLinks(key, links);
                        ++it;
                        continue;
                    }
                    if (ExistsEntry(target))
                    {
                        // A file installed by another program is never overwritten.
                        ++it;
                        continue;
                    }
                    RestoreOriginal(target);
                }
                else if (owned || OwnedLink(target, it->second) || !PlainFile(target))
                    throw std::runtime_error("Original GPU file backup is missing");
                // Either an interrupted replacement was rolled back or its original
                // file was never moved.
                links.erase(id);
                SaveLinks(key, links);
                it = backups.erase(it);
                SaveLinks(key, backups, L"Backups");
            }
        }

        void RemoveMapping(Key const& key, Links& links, Links& backups, std::wstring const& id)
        {
            auto target = Destination(id);
            ValidateDestinationParent(target, false);
            auto link = links.find(id);
            auto backup = backups.find(id);
            if (link != links.end() && OwnedLink(target, link->second))
            {
                if (!DeleteFileW(target.c_str())) Error("Remove GPU symbolic link");
            }
            if (backup != backups.end())
            {
                if (ExistsEntry(target))
                    throw std::runtime_error("Cannot restore original GPU file over another guest file");
                RestoreOriginal(target);
                backups.erase(backup);
                SaveLinks(key, backups, L"Backups");
            }
            links.erase(id);
            SaveLinks(key, links);
        }
    public:
        Json Query()
        {
            auto result = Last;
            result["Mounted"] = Lifecycle.Active() && Mount.Ready();
            result["Backend"] = NativeMode ? "VirtualSmb" : "Plan9";
            result["NativeTransportActive"] = NativeActive.load();
            result.update(UserVsmb.Query());
            result.update(UserPlan9.Query());
            return result;
        }
        Json Plan9Action(std::string const& action,Json const& settings)
        {
            if(!settings.is_object() || settings.size()!=(action=="Unmount"?1u:2u))
                throw std::runtime_error("Invalid Plan 9 mount request");
            UserPlan9.Action(action,settings);
            return Query();
        }
        Json VsmbAction(std::string const& action,Json const& settings)
        {
            if(!settings.is_object()||settings.size()!=1||!settings.contains("Name"))
                throw std::runtime_error("Invalid Virtual SMB mount request");
            UserVsmb.Action(action,settings.at("Name").get<std::string>());
            return Query();
        }
        Json Update(Json const& settings)
        {
            auto const& shares = settings.at("Shares");
            auto smbShares = settings.value("VirtualSmbShares", Json::array());
            bool gpuEnabled = settings.value("GpuEnabled", true);
            auto descriptors = ParseVirtualSmbInventory(smbShares);
            UserPlan9.Update(shares);
            UserVsmb.Update(descriptors);
            auto report=UserVsmb.Query();
            auto mounted=report.at("VirtualSmbShares");
            auto mountErrors=report.at("ShareErrors");
            if (!gpuEnabled)
            {
                Last={{"Mounted",false},{"Files",Json::array()},{"Errors",mountErrors},{"VirtualSmbShares",mounted},
                    {"Backend",NativeMode?"VirtualSmb":"Plan9"}};
                try { Lifecycle.Configure(false, [&]() { Cleanup(); }); }
                catch (std::exception const& error)
                { Last["Errors"].push_back(std::string("GPU mapping cleanup: ") + error.what()); }
                return Last;
            }
            auto const& wddm = settings.at("Wddm");
            if (!shares.is_array() || !wddm.at("Packages").is_array() || !wddm.at("Files").is_array() ||
                !wddm.at("Errors").is_array() || wddm.at("Packages").size() > 128 || wddm.at("Files").size() > 1024)
                throw std::runtime_error("Invalid WDDM inventory");
            bool shared = false;
            std::set<std::string> extraShares;
            for (auto const& share : NativeMode ? smbShares : shares)
            {
                auto name = share.value("Name", std::string());
                if ((!NativeMode && share.value("Port", 0) != 50001) || !share.value("ReadOnly", false)) continue;
                if (name == "NanaBox.HostDrivers") shared = true;
                else if (Wddm::ExtraShareName(name)) extraShares.insert(name);
            }
            if (!shared) throw std::runtime_error("Read-only host GPU repository is unavailable");
            Lifecycle.Configure(true, []() {});
            Last = {{"Mounted", false}, {"Files", Json::array()}, {"Errors", wddm.at("Errors")},
                {"VirtualSmbShares", mounted}, {"ShareErrors", mountErrors}, {"Backend", NativeMode ? "VirtualSmb" : "Plan9"}};
            for (auto const& error : wddm.value(NativeMode ? "VirtualSmbErrors" : "Plan9Errors", Json::array()))
                Last["Errors"].push_back(error);
            if (!Mount.Ensure())
            {
                Last["Errors"].push_back("GPU repository is not mounted");
                return Last;
            }
            Last["Mounted"] = true;
            auto generation = DriverGeneration(wddm);
            if (generation != VerifiedGeneration)
            {
                VerifiedImages.clear();
                VerifiedGeneration = generation;
            }
            std::set<std::wstring> packages;
            std::vector<std::wstring> directories;
            for (auto const& value : wddm.at("Packages"))
            {
                auto package = Wide(value.get<std::string>());
                if (!Wddm::SafeComponent(package)) throw std::runtime_error("Invalid GPU package");
                packages.insert(Wddm::Fold(package));
                auto folder = Mount.Root() / package;
                if (!fs::is_directory(folder))
                    Last["Errors"].push_back("GPU package directory is unavailable: " + Utf8(package));
                else if (!Contains(directories, folder.wstring()))
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
            if (wddm.contains("ExtraFiles"))
            {
                auto const& extraFiles = wddm.at("ExtraFiles");
                if (!extraFiles.is_array() || extraFiles.size() > 4096 || extraShares.size() > 64)
                    throw std::runtime_error("Invalid supplemental GPU inventory");
                std::map<std::string, bool> ready;
                for (auto const& file : extraFiles)
                {
                    auto name = file.at("Share").get<std::string>();
                    auto source = Wide(file.at("Source").get<std::string>());
                    auto destination = Wide(file.at("Destination").get<std::string>());
                    auto directory = Wide(file.at("Directory").get<std::string>());
                    if (!Wddm::SafeComponent(source) ||
                        !Wddm::SafeComponent(destination) || !Wddm::SafeSystemDirectory(directory) ||
                        !Equal(source, destination))
                        throw std::runtime_error("Invalid supplemental GPU file mapping");
                    if (!extraShares.count(name))
                    {
                        Last["Errors"].push_back("Supplemental GPU share is unavailable: " + name);
                        continue;
                    }
                    auto id = LinkId(directory, destination);
                    // Explicit WDDM copy rules take precedence over the device inventory.
                    if (desired.count(id)) continue;
                    try
                    {
                        auto state = ready.emplace(name, false);
                        if (state.second)
                        {
                            if (!ExtraMounts.count(name) && ExtraMounts.size() >= 96)
                                throw std::runtime_error("Too many GPU mount generations");
                            auto& mount = ExtraMounts[name];
                            if (!mount) mount = std::make_unique<DriverMount>(Wide(name));
                            state.first->second = mount->Ensure(0);
                            if (!state.first->second)
                                Last["Errors"].push_back("Supplemental GPU share is not mounted: " + name);
                        }
                        if (!state.first->second) continue;
                        desired.emplace(id, Item{ExtraMounts.at(name)->Root() / source, true});
                    }
                    catch (std::exception const& error)
                    {
                        Last["Errors"].push_back(name + ": " + error.what());
                    }
                }
            }
            Key key(StateKey, KEY_READ | KEY_SET_VALUE);
            auto links = ReadLinks(key);
            auto backups = ReadLinks(key, L"Backups");
            auto pending = ReadLinks(key, L"PendingLinks");
            RecoverPending(key, links, pending);
            RecoverBackups(key, links, backups);
            for (auto const& entry : desired)
            {
                auto const& id = entry.first;
                auto const& item = entry.second;
                Json status = {{"Destination", Utf8(id)}, {"Source", Utf8(item.Source.wstring())}};
                try
                {
                    if (collisions.count(id)) throw std::runtime_error("Multiple GPU packages request the same file");
                    auto target = Destination(id);
                    ValidateDestinationParent(target, true);
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
                    else if (existing == links.end() && ExistsEntry(target) &&
                        SfcIsFileProtected(nullptr, target.c_str()))
                    {
                        status["State"] = "KeptProtectedGuestFile";
                        Last["Errors"].push_back(Utf8(id) + ": Windows protects the existing file");
                    }
                    else
                    {
                        if (NativeMode && !VerifiedImages.count(item.Source.wstring()))
                        {
                            VerifyImageMapping(item.Source);
                            VerifiedImages.insert(item.Source.wstring());
                        }
                        if (existing == links.end() && ExistsEntry(target))
                        {
                            if (backups.count(id) || ExistsEntry(BackupPath(target)) || !PlainFile(target))
                                throw std::runtime_error("Existing guest file or link is not safely replaceable");
                            // Persist the intended replacement before moving the original.
                            backups[id] = item.Source.wstring();
                            SaveLinks(key, backups, L"Backups");
                            MoveOriginal(target);
                        }
                        auto previous = existing == links.end() ? nullptr : &existing->second;
                        if (!previous || !Equal(*previous, item.Source.wstring()))
                        {
                            pending[id] = item.Source.wstring();
                            SaveLinks(key, pending, L"PendingLinks");
                        }
                        try { InstallLink(target, item.Source, previous); }
                        catch (...)
                        {
                            if (previous == nullptr && backups.count(id) &&
                                PlainFile(BackupPath(target)) && !ExistsEntry(target))
                            {
                                RestoreOriginal(target);
                                backups.erase(id);
                                SaveLinks(key, backups, L"Backups");
                            }
                            throw;
                        }
                        links[id] = item.Source.wstring();
                        SaveLinks(key, links);
                        if (pending.erase(id)) SaveLinks(key, pending, L"PendingLinks");
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
                std::set<std::wstring> obsolete;
                for (auto const& link : links) if (!desired.count(link.first)) obsolete.insert(link.first);
                for (auto const& backup : backups) if (!desired.count(backup.first)) obsolete.insert(backup.first);
                for (auto const& id : obsolete)
                {
                    try { RemoveMapping(key, links, backups, id); }
                    catch (std::exception const& error)
                    { Last["Errors"].push_back(Utf8(id) + ": " + error.what()); }
                }
            }
            Last["VerifiedImageCount"] = VerifiedImages.size();
            Last["Packages"] = wddm.at("Packages");
            bool complete = Last["Errors"].empty();
            UpdateEnvironment(key, L"Path", L"OwnedPath", directories, complete);
            UpdateEnvironment(key, L"LIB", L"OwnedLib", directories, complete);
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 1000, &ignored);
            Last["AutomaticRestart"] = AutomaticRestart;
            Last["Devices"] = Json::array();
            if (AutomaticRestart && complete && !packages.empty())
            {
                try
                {
                    Last["Devices"] = Restart.Update(generation, VirtualRenderDevices(),
                        RestartVirtualRenderDevice, std::chrono::steady_clock::now());
                    if (Last["Devices"].empty()) Last["RestartStatus"] = "WaitingForVirtualRenderDevice";
                    for (auto const& device : Last["Devices"])
                        if (device.at("State") == "RetryPending")
                            Last["Errors"].push_back(device.at("Error"));
                }
                catch (std::exception const& error) { Last["Errors"].push_back(error.what()); }
            }
            return Last;
        }
        void Cleanup()
        {
            Key key(StateKey, KEY_READ | KEY_SET_VALUE);
            auto links = ReadLinks(key);
            auto backups = ReadLinks(key, L"Backups");
            auto pending = ReadLinks(key, L"PendingLinks");
            RecoverPending(key, links, pending);
            RecoverBackups(key, links, backups);
            std::set<std::wstring> ids;
            for (auto const& link : links) ids.insert(link.first);
            for (auto const& backup : backups) ids.insert(backup.first);
            std::string errors;
            for (auto const& id : ids)
            {
                try { RemoveMapping(key, links, backups, id); }
                catch (std::exception const& error)
                {
                    if (!errors.empty()) errors += "; ";
                    errors += Utf8(id) + ": " + error.what();
                }
            }
            UpdateEnvironment(key, L"Path", L"OwnedPath", {});
            UpdateEnvironment(key, L"LIB", L"OwnedLib", {});
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 1000, &ignored);
            if (!errors.empty()) throw std::runtime_error(errors);
            VerifiedImages.clear();
            VerifiedGeneration.clear();
            Restart = RestartTracker{};
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
        address.VmId = ParentId;
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
                else if(request.at("RequestType")=="Mount" || request.at("RequestType")=="Unmount")
                    response=manager.Plan9Action(request.at("RequestType").get<std::string>(),request.at("Settings"));
                else if(request.at("RequestType")=="VsmbMount" || request.at("RequestType")=="VsmbUnmount")
                    response=manager.VsmbAction(request.at("RequestType").get<std::string>(),request.at("Settings"));
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
        status.dwControlsAccepted = state == SERVICE_RUNNING ?
            SERVICE_ACCEPT_SHUTDOWN | ((NativeActive || Plan9Active) ? 0u : SERVICE_ACCEPT_STOP) : 0;
        status.dwWin32ExitCode = error;
        status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
        if (ServiceHandle) SetServiceStatus(ServiceHandle, &status);
    }

    void WINAPI Control(DWORD control)
    {
        if ((control == SERVICE_CONTROL_STOP && !NativeActive && !Plan9Active) || control == SERVICE_CONTROL_SHUTDOWN)
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
            WinFspPlan9Path = key.Read(L"WinFspPlan9Path");
            if(!WinFspPlan9Path.empty() && !fs::path(WinFspPlan9Path).is_absolute())
                throw std::runtime_error("Invalid WinFsp Plan 9 executable path");
            auto backend = key.Read(L"Backend");
            NativeMode = backend == L"VirtualSmb" || (backend.empty() && CirnoPath.empty());
            if (!backend.empty() && backend != L"VirtualSmb" && backend != L"Plan9")
                throw std::runtime_error("Unknown Windows share backend");
            AutomaticRestart = key.Read(L"AutomaticRestart") != L"0";
            if (!NativeMode && (CirnoPath.empty() || !fs::path(CirnoPath).is_absolute()))
                throw std::runtime_error("Mile.Cirno path is not configured");
            Serve();
            Status(SERVICE_STOPPED);
        }
        catch (...) { Status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR); }
    }

    int SelfTest()
    {
        if(!WindowsVsmbMountStateTest())return 64;
        {
            auto share=[](std::string name,unsigned port,bool readOnly,bool automatic)
            {return Json{{"Name",name},{"Port",port},{"ReadOnly",readOnly},{"AutoMount",automatic}};};
            auto rows=ParseWindowsPlan9Inventory(Json::array({share("Projects",564,false,true),
                share("Manual",565,true,false),share("NanaBox.HostDrivers",50001,true,true)}));
            if(rows.size()!=2 || rows[0].Port!=564 || !rows[0].AutoMount || !rows[1].ReadOnly || rows[1].AutoMount) return 60;
            for(auto const& bad:{Json::array({share("CON",564,false,true)}),
                Json::array({share("Projects",27456,false,true)}),
                Json::array({share("Projects",564,false,true),share("projects",565,true,false)}),
                Json::array({share("NanaBox.HostDrivers",50001,false,true)}),
                Json::array({share("../bad",564,false,true)})})
            {
                bool rejected=false;try{(void)ParseWindowsPlan9Inventory(bad);}catch(std::exception const&){rejected=true;}
                if(!rejected) return 61;
            }
            fs::path root=L"C:\\ProgramData\\NanaBox Guest 9P";
            if(WindowsPlan9Target(root,rows[0],"")!=root/L"Projects" ||
                WindowsPlan9Target(root,rows[0],"Custom")!=root/L"Custom") return 62;
            for(auto bad:{"../escape","C:\\Windows\\System32","C:\\ProgramData\\NanaBox Guest 9P\\..\\Escape","CON"})
            {
                bool rejected=false;try{(void)WindowsPlan9Target(root,rows[0],bad);}catch(std::exception const&){rejected=true;}
                if(!rejected) return 63;
            }
        }
        {
            auto share = [](std::string name, bool readOnly) -> Json
            { return {{"Name",std::move(name)},{"ReadOnly",readOnly}}; };
            auto valid = ParseVirtualSmbInventory(Json::array({
                share("Projects", false), share("NanaBox.HostDrivers", true),
                share("NanaBox.GpuExtra.0123456789abcdef", true)}));
            if (valid.size()!=3 || valid[0].Internal || !valid[1].Internal ||
                !valid[2].Internal || valid[0].ReadOnly) return 39;
            std::vector<Json> invalid = {
                Json::array({share("CON.txt", true)}),
                Json::array({share("name.", false)}),
                Json::array({share("name", true), share("NAME", false)}),
                Json::array({share("\xc3\x84", true), share("\xc3\xa4", true)}),
                Json::array({share("NanaBox.HostDrivers", false)}),
                Json::array({share("nanabox.hostdrivers", true)}),
                Json::array({share("NanaBox.GpuExtra.invalid", true)}),
                Json::array({Json{{"Name","Projects"},{"ReadOnly","false"}}}),
                Json::object()
            };
            for (auto const& value : invalid)
            {
                bool rejected = false;
                try { (void)ParseVirtualSmbInventory(value); }
                catch (std::exception const&) { rejected = true; }
                if (!rejected) return 40;
            }
        }
        {
            GpuMappingLifecycle lifecycle;
            unsigned calls = 0;
            auto cleanup = [&]() { ++calls; };
            if (lifecycle.Active()) return 33;
            lifecycle.Configure(false, cleanup);
            lifecycle.Configure(false, cleanup);
            if (lifecycle.Active() || calls != 1) return 34;
            lifecycle.Configure(true, cleanup);
            if (!lifecycle.Active() || calls != 1) return 35;
            bool failed = false;
            try
            {
                lifecycle.Configure(false, [&]() { ++calls; throw std::runtime_error("busy"); });
            }
            catch (std::runtime_error const&) { failed = true; }
            if (!failed || lifecycle.Active() || calls != 2) return 36;
            lifecycle.Configure(false, cleanup);
            lifecycle.Configure(false, cleanup);
            if (calls != 3) return 37;
            lifecycle.Configure(true, cleanup);
            lifecycle.Configure(false, cleanup);
            if (calls != 4 || lifecycle.Active()) return 38;
        }
        {
            for(auto layout:{88u,92u,108u,116u,120u})
            {
                auto request=Vsmb::StartRequest(layout);
                if(Vsmb::Get32(request,0)!=120||Vsmb::Get32(request,4)!=30||
                    request[layout-4]!=1||request[layout]!=0x5c||request[layout+1]!=0||
                    request.size()!=layout+26) return 25;
            }
            if(Vsmb::Layouts(19045)!=std::vector<unsigned>{88}||
                Vsmb::Layouts(22631)!=std::vector<unsigned>{92}||
                Vsmb::Layouts(26100)!=std::vector<unsigned>{120,116})return 26;
            auto bind=Vsmb::BindRequest();
            if(Vsmb::Get32(bind,4)!=2||Vsmb::Get32(bind,8)!=bind.size()-12)return 27;
            std::vector<unsigned char> bytes(100);
            Vsmb::Put32(bytes,0,100);Vsmb::Put32(bytes,8,1);
            Vsmb::Put32(bytes,12,4);Vsmb::Put32(bytes,16,40);
            Vsmb::Put32(bytes,20,0x21);Vsmb::Put32(bytes,24,1);Vsmb::Put32(bytes,28,32);
            Vsmb::Put32(bytes,32,4);Vsmb::Put32(bytes,36,44);
            Vsmb::PutName(bytes,40,u"AB");Vsmb::PutName(bytes,44,u"CD");
            auto mappings=Vsmb::ParseBindings(bytes);
            if(mappings.size()!=1||mappings[0].Root!=u"AB"||mappings[0].Flags!=0x21||
                mappings[0].Targets!=std::vector<std::u16string>{u"CD"})return 28;
            Vsmb::Put32(bytes,36,UINT32_MAX);
            bool refused=false;try{Vsmb::ParseBindings(bytes);}catch(...){refused=true;}
            if(!refused)return 29;
        }
        if (!Wddm::IsVirtualRenderDevice(L"PCI\\VEN_1414&DEV_008E&SUBSYS_00000000", L"VirtualRender") ||
            Wddm::IsVirtualRenderDevice(L"PCI\\VEN_1414&DEV_008E0", L"VirtualRender") ||
            Wddm::IsVirtualRenderDevice(L"PCI\\VEN_10DE&DEV_008E", L"VirtualRender") ||
            Wddm::IsVirtualRenderDevice(L"PCI\\VEN_1414&DEV_008E", L"BasicDisplay")) return 15;
        {
            Json a = {{"Packages", Json::array({"a", "b"})}, {"DriverVersions", Json::array({"1"})}};
            Json b = {{"Packages", Json::array({"b", "a"})}, {"DriverVersions", Json::array({"1"})}};
            if (DriverGeneration(a) != DriverGeneration(b)) return 16;
            b["DriverVersions"] = Json::array({"2"});
            if (DriverGeneration(a) == DriverGeneration(b)) return 17;
            RestartTracker tracker;
            auto now = std::chrono::steady_clock::now();
            unsigned calls = 0;
            auto restart = [&](std::wstring const&) { ++calls; return false; };
            std::vector<RenderDevice> devices = {{L"GPU-A", false}, {L"GPU-B", true}};
            auto result = tracker.Update("v1", devices, restart, now);
            if (calls != 1 || result[0]["State"] != "Restarted" || result[1]["State"] != "Disabled") return 18;
            tracker.Update("v1", devices, restart, now);
            if (calls != 1) return 19;
            tracker.Update("v2", devices, restart, now);
            if (calls != 2) return 20;
            devices[1].Disabled = false;
            auto fail = [&](std::wstring const&) -> bool { ++calls; throw std::runtime_error("restart failed"); };
            result = tracker.Update("v2", devices, fail, now);
            if (calls != 3 || result[1]["State"] != "RetryPending") return 21;
            tracker.Update("v2", devices, restart, now);
            if (calls != 3) return 22;
            result = tracker.Update("v2", devices, [&](std::wstring const&) { ++calls; return true; },
                now + std::chrono::minutes(1));
            if (calls != 4 || result[1]["State"] != "RebootRequired") return 23;
            tracker.Update("v2", devices, restart, now + std::chrono::minutes(2));
            if (calls != 4) return 24;
        }
        auto m = Wddm::MakeMapping(L"gpu.inf_amd64_test", {L"sub\\source.dll", L"target.dll"}, true, true);
        if (!m || m->Source != L"gpu.inf_amd64_test\\sub\\source.dll" ||
            m->Destination != L"target.dll" || m->Directory != L"SysWOW64" || !m->WhenNewer) return 1;
        for (auto bad : {L"..\\x.dll", L"x:stream", L"COM1.dll", L"\\x.dll", L"x\\", L"x. "})
            if (Wddm::MakeMapping(L"gpu", {bad}, false, false)) return 2;
        if (Wddm::MakeMapping(L"gpu", {L"x.dll", L"\\test.dll"}, false, false)) return 3;
        if (!Wddm::SafeSystemDirectory(L"System32\\drivers\\NVIDIA Corporation") ||
            Wddm::SafeSystemDirectory(L"System32\\..\\Temp") ||
            Wddm::SafeSystemDirectory(L"System32\\HostDriverStore\\FileRepository") ||
            Wddm::SafeSystemDirectory(L"Program Files") ||
            !Wddm::ExtraShareName("NanaBox.GpuExtra.0123456789abcdef") ||
            Wddm::ExtraShareName("NanaBox.GpuExtra.0123456789abcde/")) return 10;
        if (Wddm::NormalizeServiceImage(L"\\SystemRoot\\System32\\DriverStore\\gpu.sys", L"C:\\Windows") !=
                L"C:\\Windows\\System32\\DriverStore\\gpu.sys" ||
            Wddm::NormalizeServiceImage(L"\"\\??\\C:\\Windows\\System32\\gpu.sys\"", L"C:\\Windows") !=
                L"C:\\Windows\\System32\\gpu.sys" ||
            Wddm::NormalizeServiceImage(L"%SystemRoot%\\System32\\gpu.sys", L"C:\\Windows") !=
                L"C:\\Windows\\System32\\gpu.sys") return 14;
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
        Links interrupted{{L"test", source.wstring()}};
        ReconcilePendingLink(link, L"test", original.wstring(), interrupted);
        if (interrupted.at(L"test") != original.wstring()) return 30;
        ReconcilePendingLink(link, L"test", source.wstring(), interrupted);
        if (interrupted.at(L"test") != original.wstring()) return 31;
        ReconcilePendingLink(source, L"test", original.wstring(), interrupted);
        if (!interrupted.empty() || !fs::is_regular_file(source)) return 32;
        MoveOriginal(original);
        if (ExistsEntry(original) || !PlainFile(BackupPath(original))) return 11;
        InstallLink(original, source, nullptr);
        if (!OwnedLink(original, source.wstring()) || !DeleteFileW(original.c_str())) return 12;
        RestoreOriginal(original);
        if (!PlainFile(original) || ExistsEntry(BackupPath(original))) return 13;
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") return SelfTest();
        if (argc == 4 && std::wstring_view(argv[1]) == L"--plan9-manager-test")
            return WindowsPlan9ManagerTest(argv[2],argv[3]);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--bind-filter-self-test") return VsmbClient.LocalMappingTest();
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
