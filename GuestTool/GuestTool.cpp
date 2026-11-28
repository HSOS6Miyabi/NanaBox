// NanaBox native guest integration service.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <hvsocket.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/random.h>
#include <linux/vm_sockets.h>
#include <linux/netlink.h>
#include <linux/inet_diag.h>
#include <linux/sock_diag.h>
#include <sys/un.h>
#include <grp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <openssl/evp.h>
#endif
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <tuple>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    using Mono = std::chrono::steady_clock;
    constexpr std::size_t Chunk = 65536, MaxFrame = 256 * 1024;
    constexpr std::uint64_t MaxFile = INT64_MAX;
    constexpr unsigned Port = 27457;
    constexpr auto SessionLifetime = std::chrono::minutes(15);
    const auto Started = Mono::now();
    std::atomic<bool> Stop{false};

    [[noreturn]] void Fail(char const* message)
    {
        throw std::runtime_error(message);
    }

    std::uint64_t Integer(Json const& value, std::uint64_t min, std::uint64_t max)
    {
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
            Fail("Expected an unsigned integer");
        auto number = value.get<std::uint64_t>();
        if (number < min || number > max) Fail("Integer outside allowed range");
        return number;
    }

    void Fields(Json const& value, std::initializer_list<char const*> keys)
    {
        if (!value.is_object() || value.size() != keys.size()) Fail("Invalid request fields");
        for (auto key : keys) if (!value.contains(key)) Fail("Missing request field");
    }

    bool HexString(std::string_view value, std::size_t length)
    {
        return value.size() == length && std::all_of(value.begin(), value.end(), [](char c)
            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::string Hex(unsigned char const* bytes, std::size_t count)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(count * 2);
        for (std::size_t i = 0; i < count; ++i)
        {
            result += digits[bytes[i] >> 4];
            result += digits[bytes[i] & 15];
        }
        return result;
    }

    std::string Token()
    {
        std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) Fail("Cannot generate transfer token");
#else
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            auto n = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) Fail("Cannot generate transfer token");
            offset += static_cast<std::size_t>(n);
        }
#endif
        return Hex(bytes.data(), bytes.size());
    }

    std::string Filename(Json const& value)
    {
        auto name = value.get<std::string>();
        if (name.empty() || name.size() > 240 || name.front() == '.' ||
            name.back() == ' ' || name.back() == '.') Fail("Invalid filename");
        for (unsigned char c : name)
            if (c < 32 || c == 127 || std::string_view("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos)
                Fail("Invalid filename");
        auto stem = name.substr(0, name.find('.'));
        for (auto& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            stem == "CONIN$" || stem == "CONOUT$") Fail("Reserved filename");
        if (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0)
        {
            auto n = stem.substr(3);
            if ((n.size() == 1 && n[0] >= '0' && n[0] <= '9') ||
                n == "\xc2\xb9" || n == "\xc2\xb2" || n == "\xc2\xb3") Fail("Reserved filename");
        }
        return name;
    }

    constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string Encode(std::vector<unsigned char> const& bytes)
    {
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (std::size_t i = 0; i < bytes.size(); i += 3)
        {
            unsigned n = static_cast<unsigned>(bytes[i]) << 16;
            if (i + 1 < bytes.size()) n |= static_cast<unsigned>(bytes[i+1]) << 8;
            if (i + 2 < bytes.size()) n |= bytes[i+2];
            out += Alphabet[(n >> 18) & 63]; out += Alphabet[(n >> 12) & 63];
            out += i + 1 < bytes.size() ? Alphabet[(n >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? Alphabet[n & 63] : '=';
        }
        return out;
    }

    std::vector<unsigned char> Decode(std::string const& text)
    {
        if (text.empty() || text.size() % 4 || text.size() > (Chunk + 2) / 3 * 4)
            Fail("Invalid base64 chunk length");
        std::vector<unsigned char> bytes;
        bytes.reserve(text.size() / 4 * 3);
        for (std::size_t i = 0; i < text.size(); i += 4)
        {
            unsigned n = 0, pad = 0;
            for (std::size_t j = 0; j < 4; ++j)
            {
                char c = text[i+j];
                n <<= 6;
                if (c == '=')
                {
                    if (j < 2 || i + 4 != text.size()) Fail("Invalid base64 padding");
                    ++pad;
                }
                else
                {
                    auto v = Alphabet.find(c);
                    if (pad || v == std::string_view::npos) Fail("Invalid base64 character");
                    n |= static_cast<unsigned>(v);
                }
            }
            if ((pad == 1 && (n & 0xffu)) || (pad == 2 && (n & 0xffffu))) Fail("Noncanonical base64 padding");
            bytes.push_back(static_cast<unsigned char>(n >> 16));
            if (pad < 2) bytes.push_back(static_cast<unsigned char>(n >> 8));
            if (!pad) bytes.push_back(static_cast<unsigned char>(n));
        }
        if (bytes.empty() || bytes.size() > Chunk) Fail("Invalid transfer chunk size");
        return bytes;
    }

    class Digest
    {
#ifdef _WIN32
        BCRYPT_ALG_HANDLE Algorithm = nullptr;
        BCRYPT_HASH_HANDLE Hash = nullptr;
        std::vector<unsigned char> Object;
        void Close() noexcept
        {
            if (Hash) BCryptDestroyHash(Hash);
            if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
        }
#else
        EVP_MD_CTX* Hash = nullptr;
#endif
    public:
        Digest()
        {
#ifdef _WIN32
            try
            {
                if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                    Fail("Cannot initialize SHA-256");
                DWORD size = 0, actual = 0;
                if (BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                    sizeof(size), &actual, 0) < 0) Fail("Cannot allocate SHA-256");
                Object.resize(size);
                if (BCryptCreateHash(Algorithm, &Hash, Object.data(), size, nullptr, 0, 0) < 0)
                    Fail("Cannot create SHA-256");
            }
            catch (...) { Close(); throw; }
#else
            Hash = EVP_MD_CTX_new();
            if (!Hash || EVP_DigestInit_ex(Hash, EVP_sha256(), nullptr) != 1)
            { EVP_MD_CTX_free(Hash); Fail("Cannot initialize SHA-256"); }
#endif
        }
        ~Digest()
        {
#ifdef _WIN32
            Close();
#else
            EVP_MD_CTX_free(Hash);
#endif
        }
        Digest(Digest const&) = delete;
        Digest& operator=(Digest const&) = delete;
        void Update(std::vector<unsigned char> const& bytes)
        {
            if (bytes.empty()) return;
#ifdef _WIN32
            if (BCryptHashData(Hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0)
                Fail("SHA-256 update failed");
#else
            if (EVP_DigestUpdate(Hash, bytes.data(), bytes.size()) != 1) Fail("SHA-256 update failed");
#endif
        }
        std::string Finish()
        {
            std::array<unsigned char, 32> out{};
#ifdef _WIN32
            if (BCryptFinishHash(Hash, out.data(), static_cast<ULONG>(out.size()), 0) < 0)
                Fail("SHA-256 finalization failed");
#else
            unsigned size = 0;
            if (EVP_DigestFinal_ex(Hash, out.data(), &size) != 1 || size != out.size())
                Fail("SHA-256 finalization failed");
#endif
            return Hex(out.data(), out.size());
        }
    };

    struct FileInfo
    {
        std::uint64_t Size = 0, Modified = 0, ModifiedSub = 0, Identity = 0, Device = 0;
        bool operator==(FileInfo const& other) const
        {
            return Size == other.Size && Modified == other.Modified && ModifiedSub == other.ModifiedSub &&
                Identity == other.Identity && Device == other.Device;
        }
    };

    class File
    {
#ifdef _WIN32
        HANDLE Value = INVALID_HANDLE_VALUE;
#else
        int Value = -1;
#endif
    public:
        File() = default;
        ~File() { Close(); }
        File(File const&) = delete;
        File& operator=(File const&) = delete;
        void Close() noexcept
        {
#ifdef _WIN32
            if (Value != INVALID_HANDLE_VALUE) { CloseHandle(Value); Value = INVALID_HANDLE_VALUE; }
#else
            if (Value >= 0) { close(Value); Value = -1; }
#endif
        }
        void Open(fs::path const& path, bool upload)
        {
#ifdef _WIN32
            Value = CreateFileW(path.c_str(), upload ? GENERIC_WRITE : GENERIC_READ,
                upload ? 0 : FILE_SHARE_READ, nullptr, upload ? CREATE_NEW : OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (Value == INVALID_HANDLE_VALUE) Fail("Cannot open regular exchange file");
#else
            Value = open(path.c_str(), (upload ? O_CREAT | O_EXCL | O_WRONLY : O_RDONLY | O_NONBLOCK) |
                O_NOFOLLOW | O_CLOEXEC, 0600);
            if (Value < 0) Fail("Cannot open regular exchange file");
#endif
            (void)Info();
        }
        FileInfo Info() const
        {
#ifdef _WIN32
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(Value, &info) || GetFileType(Value) != FILE_TYPE_DISK ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                Fail("Exchange file is not a regular file");
            return {(static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
                (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
                0, (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow, info.dwVolumeSerialNumber};
#else
            struct stat info{};
            if (fstat(Value, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) Fail("Exchange file is not regular");
            return {static_cast<std::uint64_t>(info.st_size), static_cast<std::uint64_t>(info.st_mtim.tv_sec),
                static_cast<std::uint64_t>(info.st_mtim.tv_nsec), static_cast<std::uint64_t>(info.st_ino),
                static_cast<std::uint64_t>(info.st_dev)};
#endif
        }
        void Write(std::vector<unsigned char> const& data)
        {
            std::size_t offset = 0;
            while (offset < data.size())
            {
#ifdef _WIN32
                DWORD n = 0;
                if (!WriteFile(Value, data.data() + offset, static_cast<DWORD>(data.size() - offset), &n, nullptr) || !n)
                    Fail("Exchange file write failed");
#else
                auto n = write(Value, data.data() + offset, data.size() - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) Fail("Exchange file write failed");
#endif
                offset += static_cast<std::size_t>(n);
            }
        }
        std::vector<unsigned char> Read(std::size_t length)
        {
            std::vector<unsigned char> data(length);
            std::size_t offset = 0;
            while (offset < length)
            {
#ifdef _WIN32
                DWORD n = 0;
                if (!ReadFile(Value, data.data() + offset, static_cast<DWORD>(length - offset), &n, nullptr) || !n)
                    Fail("Exchange file was truncated or unreadable");
#else
                auto n = read(Value, data.data() + offset, length - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) Fail("Exchange file was truncated or unreadable");
#endif
                offset += static_cast<std::size_t>(n);
            }
            return data;
        }
        void Flush()
        {
#ifdef _WIN32
            if (!FlushFileBuffers(Value)) Fail("Cannot flush exchange file");
#else
            if (fsync(Value)) Fail("Cannot flush exchange file");
#endif
        }
    };

    // The installer restricts the exchange directory to the service account.
    bool RegularOrAbsent(fs::path const& path)
    {
#ifdef _WIN32
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return false;
            Fail("Cannot inspect exchange file");
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            Fail("Exchange path is not a regular file");
#else
        struct stat info{};
        if (lstat(path.c_str(), &info))
        {
            if (errno == ENOENT) return false;
            Fail("Cannot inspect exchange file");
        }
        if (!S_ISREG(info.st_mode)) Fail("Exchange path is not a regular file");
#endif
        return true;
    }

    void CheckRoot(fs::path const& root)
    {
        if (!root.is_absolute()) Fail("Exchange root must be absolute");
#ifdef _WIN32
        auto attrs = GetFileAttributesW(root.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
            (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) Fail("Unsafe exchange root");
#else
        struct stat info{};
        if (lstat(root.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
            (info.st_mode & 0022)) Fail("Unsafe exchange root ownership or permissions");
#endif
    }

    void Publish(fs::path const& from, fs::path const& to, bool overwrite)
    {
        if (RegularOrAbsent(to) && !overwrite) Fail("Destination already exists");
#ifdef _WIN32
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH |
            (overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) Fail("Cannot publish exchange file");
#else
        if (overwrite)
        {
            if (rename(from.c_str(), to.c_str())) Fail("Cannot publish exchange file");
        }
        else
        {
            if (link(from.c_str(), to.c_str())) Fail("Cannot publish exchange file without replacement");
            // Publication succeeded. Session destruction retries temporary cleanup.
            (void)unlink(from.c_str());
        }
#endif
    }

    class Exchange
    {
        struct Session
        {
            File Stream;
            Digest Sha;
            fs::path Target, Temporary;
            std::string Name;
            FileInfo Original;
            std::uint64_t Size = 0, Offset = 0;
            bool Upload = false, Overwrite = false, OwnTemporary = false;
            Mono::time_point Touched = Mono::now();
            ~Session()
            {
                Stream.Close();
                if (OwnTemporary)
                {
                    std::error_code ec;
                    fs::remove(Temporary, ec);
                }
            }
        };
        fs::path Root;
        std::map<std::string, std::unique_ptr<Session>> Sessions;
    public:
        explicit Exchange(fs::path root) : Root(std::move(root))
        {
            CheckRoot(Root);
            for (auto const& item : fs::directory_iterator(Root))
            {
                auto name = item.path().filename().u8string();
                constexpr std::string_view prefix = ".nanabox-upload-";
                if (name.rfind(prefix, 0) == 0 && HexString(std::string_view(name).substr(prefix.size()), 32))
                {
                    // The path is never followed; an unexpected directory is left intact.
                    if (fs::is_regular_file(item.symlink_status()) || fs::is_symlink(item.symlink_status()))
                        fs::remove(item.path());
                }
            }
        }
        void Reap()
        {
            auto now = Mono::now();
            for (auto it = Sessions.begin(); it != Sessions.end();)
                if (now - it->second->Touched > SessionLifetime) it = Sessions.erase(it);
                else ++it;
        }
        Json Request(std::string const& action, Json const& settings)
        {
            Reap();
            CheckRoot(Root);
            if (action == "BeginUpload" || action == "OpenDownload")
            {
                bool upload = action == "BeginUpload";
                if (upload) Fields(settings, {"Name", "Size", "Overwrite"});
                else Fields(settings, {"Name"});
                if (Sessions.size() >= 16) Fail("Too many active file transfers");
                auto name = Filename(settings.at("Name"));
                auto token = Token();
                if (Sessions.count(token)) Fail("Duplicate transfer token");
                auto session = std::make_unique<Session>();
                session->Name = name;
                session->Target = Root / fs::u8path(name);
                session->Upload = upload;
                if (upload)
                {
                    session->Size = Integer(settings.at("Size"), 0, MaxFile);
                    session->Overwrite = settings.at("Overwrite").get<bool>();
                    if (RegularOrAbsent(session->Target) && !session->Overwrite) Fail("Destination already exists");
                    session->Temporary = Root / (".nanabox-upload-" + token);
                    session->Stream.Open(session->Temporary, true);
                    session->OwnTemporary = true;
                }
                else
                {
                    if (!RegularOrAbsent(session->Target)) Fail("Source file does not exist");
                    session->Stream.Open(session->Target, false);
                    session->Original = session->Stream.Info();
                    session->Size = session->Original.Size;
                    if (session->Size > MaxFile) Fail("Source file exceeds maximum size");
                }
                auto size = session->Size;
                Sessions.emplace(token, std::move(session));
                return {{"Token", token}, {"Name", name}, {"Size", size}, {"ChunkSize", Chunk}};
            }
            if (action == "Write") Fields(settings, {"Token", "Offset", "Data"});
            else if (action == "Read") Fields(settings, {"Token", "Offset", "Length"});
            else if (action == "Commit") Fields(settings, {"Token", "Sha256"});
            else if (action == "Abort" || action == "FinishDownload") Fields(settings, {"Token"});
            else Fail("Unsupported file transfer operation");
            auto token = settings.at("Token").get<std::string>();
            if (!HexString(token, 32)) Fail("Invalid transfer token");
            if (action == "Abort") { Sessions.erase(token); return {{"Aborted", true}}; }
            auto it = Sessions.find(token);
            if (it == Sessions.end()) Fail("Unknown or expired transfer token");
            auto& session = *it->second;
            session.Touched = Mono::now();
            if (action == "Write" || action == "Read")
            {
                if ((action == "Write") != session.Upload) Fail("Incorrect transfer direction");
                if (Integer(settings.at("Offset"), 0, MaxFile) != session.Offset) Fail("Unexpected transfer offset");
                if (session.Upload)
                {
                    auto data = Decode(settings.at("Data").get<std::string>());
                    if (data.size() > session.Size - session.Offset) Fail("Chunk exceeds declared file size");
                    try { session.Stream.Write(data); session.Sha.Update(data); }
                    catch (...) { Sessions.erase(it); throw; }
                    session.Offset += data.size();
                    return {{"Offset", session.Offset}};
                }
                auto size = static_cast<std::size_t>(std::min(Integer(settings.at("Length"), 1, Chunk),
                    session.Size - session.Offset));
                try
                {
                    auto data = session.Stream.Read(size);
                    session.Sha.Update(data);
                    session.Offset += data.size();
                    return {{"Offset", session.Offset}, {"Data", Encode(data)}, {"End", session.Offset == session.Size}};
                }
                catch (...) { Sessions.erase(it); throw; }
            }
            if ((action == "Commit") != session.Upload) Fail("Incorrect transfer direction");
            // Move the session out before finalization so any failure cleans it up.
            auto final = std::move(it->second);
            Sessions.erase(it);
            if (final->Offset != final->Size) Fail("Incomplete transfer");
            auto hash = final->Sha.Finish();
            if (final->Upload)
            {
                auto expected = settings.at("Sha256").get<std::string>();
                if (!HexString(expected, 64) || expected != hash) Fail("SHA-256 mismatch");
                final->Stream.Flush();
                final->Stream.Close();
                Publish(final->Temporary, final->Target, final->Overwrite);
            }
            else if (!(final->Original == final->Stream.Info())) Fail("Source changed during download");
            return {{"Name", final->Name}, {"Size", final->Size}, {"Sha256", hash}};
        }
    };

    std::int64_t Now()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void SetClock(std::uint64_t milliseconds)
    {
#ifdef _WIN32
        ULARGE_INTEGER ticks{};
        ticks.QuadPart = (milliseconds + 11644473600000ull) * 10000ull;
        FILETIME file{ticks.LowPart, ticks.HighPart};
        SYSTEMTIME value{};
        if (!FileTimeToSystemTime(&file, &value) || !SetSystemTime(&value)) Fail("Cannot set guest UTC time");
#else
        timespec value{static_cast<time_t>(milliseconds / 1000),
            static_cast<long>((milliseconds % 1000) * 1000000)};
        if (clock_settime(CLOCK_REALTIME, &value)) Fail("Cannot set guest UTC time");
#endif
    }

    std::uint64_t Uptime()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Mono::now() - Started).count());
    }

    std::string Utc()
    {
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        char value[40]{};
        if (!std::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &tm)) Fail("Cannot format UTC time");
        return value;
    }

#ifdef _WIN32
    std::string Utf8(std::wstring const& input)
    {
        if (input.empty()) return {};
        auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (!n) Fail("Invalid UTF-16");
        std::string value(static_cast<std::size_t>(n), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), value.data(), n, nullptr, nullptr)) Fail("Invalid UTF-16");
        return value;
    }
#endif

    Json Information(bool exchange, bool clock)
    {
        std::string name, os, release, version, arch;
        unsigned processors = std::thread::hardware_concurrency();
#ifdef _WIN32
        os = "Windows";
        wchar_t hostname[256]{};
        DWORD count = static_cast<DWORD>(std::size(hostname));
        if (GetComputerNameW(hostname, &count)) name = Utf8(std::wstring(hostname, count));
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        arch = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64" :
            info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "AMD64" : "x86";
        using VersionFunction = LONG (WINAPI*)(OSVERSIONINFOW*);
        auto module = GetModuleHandleW(L"ntdll.dll");
        auto address = module ? GetProcAddress(module, "RtlGetVersion") : nullptr;
        if (address)
        {
            OSVERSIONINFOW details{};
            details.dwOSVersionInfoSize = sizeof(details);
            auto get = reinterpret_cast<VersionFunction>(address);
            if (get(&details) >= 0)
            {
                release = std::to_string(details.dwMajorVersion) + "." + std::to_string(details.dwMinorVersion);
                version = release + "." + std::to_string(details.dwBuildNumber);
            }
        }
#else
        utsname info{};
        if (uname(&info)) Fail("Cannot query guest system information");
        name = info.nodename; os = info.sysname; release = info.release; version = info.version; arch = info.machine;
#endif
        return {{"HostName", name}, {"OperatingSystem", os}, {"Release", release}, {"Version", version},
            {"Architecture", arch}, {"LogicalProcessorCount", processors}, {"UtcTime", Utc()},
            {"ServiceUptimeSeconds", Uptime()}, {"ProtocolVersion", 1},
            {"QueryCapabilities", {"Heartbeat", "GuestInformation", "TimeSync", "FileExchange"}},
            {"Capabilities", {{"FileExchange", exchange}, {"ClockSet", clock}, {"FileChunkSize", Chunk},
                {"NativeGuestTool", true}}}};
    }

#include "GuestNetwork.h"

    class Agent
    {
        std::unique_ptr<Exchange> Files;
        bool Clock;
        std::unique_ptr<Network> Net;
    public:
        Agent(fs::path const& root, bool clock, bool network = false,fs::path const& loopback={}) : Clock(clock)
        {
            if (!root.empty()) Files = std::make_unique<Exchange>(root);
            if (network) Net = std::make_unique<Network>(loopback);
        }
        void Reap() { if (Files) Files->Reap(); }
        void EnableDns() { if (Net) Net->EnableDns(); }
        Json Dispatch(Json const& request)
        {
            try
            {
                if (!request.is_object()) Fail("Invalid GuestTool request");
                if (Integer(request.at("Version"), 1, 1) != 1) Fail("Unsupported protocol");
                auto resource = request.at("ResourceType").get<std::string>();
                auto action = request.at("RequestType").get<std::string>();
                auto const& settings = request.at("Settings");
                Json result;
                if (resource == "Heartbeat" || resource == "GuestInformation" ||
                    resource == "TimeSync" || resource == "FileExchange")
                    Fields(request, {"Version","ResourceType","RequestType","Settings"});
                if (resource == "Heartbeat" && action == "Query")
                {
                    Fields(settings, {});
                    result = {{"Alive", true}, {"ProtocolVersion", 1}, {"ServiceUptimeSeconds", Uptime()},
                        {"NativeGuestTool", true}};
                }
                else if (resource == "GuestInformation" && action == "Query")
                {
                    Fields(settings, {});
                    result = Information(static_cast<bool>(Files), Clock);
                }
                else if (resource == "TimeSync")
                {
                    if (action == "Query") Fields(settings, {});
                    else if (action == "Update")
                    {
                        Fields(settings, {"UnixMilliseconds"});
                        auto value = Integer(settings.at("UnixMilliseconds"), 946684800000ull, 4102444800000ull);
                        if (!Clock) Fail("Clock synchronization is disabled");
                        SetClock(value);
                    }
                    else Fail("Unsupported clock operation");
                    result = {{"UnixMilliseconds", Now()}, {"ClockSetEnabled", Clock}};
                }
                else if (resource == "FileExchange")
                {
                    if (!Files) Fail("File exchange is disabled");
                    result = Files->Request(action, settings);
                }
                else if (Net) result = Net->Dispatch(resource,action,request);
                else Fail("Unsupported GuestTool resource or operation");
                return {{"Version", 1}, {"Success", true}, {"Data", result}};
            }
            catch (std::exception const& e)
            {
                return {{"Version", 1}, {"Success", false}, {"Error", std::string(e.what()).substr(0, 4000)}};
            }
        }
    };

#ifdef _WIN32
    using SocketType = SOCKET;
    constexpr SocketType InvalidSocket = INVALID_SOCKET;
#else
    using SocketType = int;
    constexpr SocketType InvalidSocket = -1;
#endif
    struct Socket
    {
        SocketType Value;
        explicit Socket(SocketType value) : Value(value) {}
        ~Socket()
        {
            if (Value != InvalidSocket)
#ifdef _WIN32
                closesocket(Value);
#else
                close(Value);
#endif
        }
        Socket(Socket const&) = delete;
        Socket& operator=(Socket const&) = delete;
    };

    bool Wait(SocketType s, bool writing, Mono::time_point deadline, std::atomic<bool> const& closing = Stop)
    {
        while (!Stop && !closing && Mono::now() < deadline)
        {
#ifdef _WIN32
            fd_set ready, except;
            FD_ZERO(&ready); FD_SET(s, &ready);
            FD_ZERO(&except); FD_SET(s, &except);
            timeval timeout{0, 100000};
            auto n = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &except, &timeout);
            if (n == SOCKET_ERROR) Fail("Guest socket select failed");
#else
            pollfd fd{s, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
            auto n = poll(&fd, 1, 100);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) Fail("Guest socket poll failed");
#endif
            if (n > 0) return true;
        }
        return false;
    }

    void Nonblocking(SocketType socket)
    {
#ifdef _WIN32
        u_long enabled = 1;
        if (ioctlsocket(socket, FIONBIO, &enabled)) Fail("Cannot configure guest socket");
#else
        auto flags = fcntl(socket, F_GETFL);
        if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK)) Fail("Cannot configure guest socket");
#endif
    }

    void Transfer(SocketType s, char* bytes, std::size_t size, bool writing, Mono::time_point end,
        std::atomic<bool> const& closing = Stop)
    {
        while (size)
        {
            if (!Wait(s, writing, end, closing)) Fail("Guest socket deadline expired");
#ifdef _WIN32
            auto n = writing ? send(s, bytes, static_cast<int>(size), 0) : recv(s, bytes, static_cast<int>(size), 0);
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            auto n = writing ? send(s, bytes, size, MSG_NOSIGNAL) : recv(s, bytes, size, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
#endif
            if (n <= 0) Fail("Incomplete guest socket message");
            bytes += n; size -= static_cast<std::size_t>(n);
        }
    }

    Json Receive(SocketType s, Mono::time_point end)
    {
        std::uint32_t size = 0;
        Transfer(s, reinterpret_cast<char*>(&size), sizeof(size), false, end);
        size = ntohl(size);
        if (!size || size > MaxFrame) Fail("Invalid guest frame length");
        std::string body(size, '\0');
        Transfer(s, body.data(), body.size(), false, end);
        return Json::parse(body);
    }

    void Send(SocketType s, Json const& json, Mono::time_point end)
    {
        auto body = json.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (body.empty() || body.size() > MaxFrame)
            body = R"({"Version":1,"Success":false,"Error":"Guest response exceeds maximum frame size"})";
        auto size = htonl(static_cast<std::uint32_t>(body.size()));
        Transfer(s, reinterpret_cast<char*>(&size), sizeof(size), true, end);
        Transfer(s, body.data(), body.size(), true, end);
    }

    void Handle(SocketType s, Agent& agent)
    {
        Nonblocking(s);
        auto end = Mono::now() + std::chrono::seconds(10);
        Json response;
        try { response = agent.Dispatch(Receive(s, end)); }
        catch (std::exception const& e) { response = {{"Version", 1}, {"Success", false}, {"Error", e.what()}}; }
        Send(s, response, Mono::now() + std::chrono::seconds(5));
    }

    std::unique_ptr<Socket> OpenParentListener(unsigned port,int backlog)
    {
#ifdef _WIN32
        auto server=std::make_unique<Socket>(socket(AF_HYPERV,SOCK_STREAM,HV_PROTOCOL_RAW));
        constexpr GUID parent{0xa42e7cda,0xd03f,0x480c,{0x9c,0xc2,0xa4,0xde,0x20,0xab,0xb8,0x78}};
        GUID service{port,0xfacb,0x11e6,{0xbd,0x58,0x64,0,0x6a,0x79,0x86,0xd3}};
        SOCKADDR_HV address{};address.Family=AF_HYPERV;address.VmId=parent;address.ServiceId=service;
#else
        auto server=std::make_unique<Socket>(socket(AF_VSOCK,SOCK_STREAM|SOCK_CLOEXEC,0));
        sockaddr_vm address{};address.svm_family=AF_VSOCK;address.svm_cid=VMADDR_CID_ANY;address.svm_port=port;
#endif
        if(server->Value==InvalidSocket||bind(server->Value,reinterpret_cast<sockaddr*>(&address),sizeof(address))||
            listen(server->Value,backlog))Fail("Cannot listen on GuestTool Hyper-V socket");
        Nonblocking(server->Value);return server;
    }

    SocketType AcceptParent(SocketType listener)
    {
#ifdef _WIN32
        SOCKADDR_HV peer{};int length=sizeof(peer);
        return accept(listener,reinterpret_cast<sockaddr*>(&peer),&length);
#else
        sockaddr_vm peer{};socklen_t length=sizeof(peer);
        auto value=accept4(listener,reinterpret_cast<sockaddr*>(&peer),&length,SOCK_CLOEXEC);
        if(value!=InvalidSocket&&peer.svm_cid!=VMADDR_CID_HOST){close(value);return InvalidSocket;}
        return value;
#endif
    }

#include "DnsTunnel.h"
#ifndef _WIN32
#include "BindCoordinator.h"
#endif

    void Serve(Agent& agent,bool network=false,bool dns=false,bool bindCoordination=false)
    {
#ifdef _WIN32
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2,2),&data))Fail("WSAStartup failed");
        struct Wsa{~Wsa(){WSACleanup();}} cleanup;
#endif
        auto server=OpenParentListener(network?27453u:Port,8);
        std::unique_ptr<DnsTunnel> tunnel;
        if(dns){tunnel=std::make_unique<DnsTunnel>();agent.EnableDns();}
#ifndef _WIN32
        std::unique_ptr<BindCoordinator> coordinator;
        if(bindCoordination)
        {
            CheckBindSupport();
            auto* group=getgrnam("nanabox-bind");
            if(!group)Fail("Missing nanabox-bind group");
            coordinator=std::make_unique<BindCoordinator>("/run/nanabox-gns/bind.sock");
            coordinator->Start(true,group->gr_gid);
        }
#else
        if(bindCoordination)Fail("Bind coordination requires Linux");
#endif
        while(!Stop)
        {
            agent.Reap();
            if(!Wait(server->Value,false,Mono::now()+std::chrono::seconds(1)))continue;
            Socket client(AcceptParent(server->Value));
            if(client.Value==InvalidSocket)continue;
            try{Handle(client.Value,agent);}
            catch(std::exception const& error){std::cerr<<error.what()<<'\n';}
        }
    }

    Json Request(std::string resource, std::string action, Json settings = Json::object())
    {
        return {{"Version", 1}, {"ResourceType", std::move(resource)}, {"RequestType", std::move(action)},
            {"Settings", std::move(settings)}};
    }

    void Require(bool value, char const* message)
    {
        if (!value) Fail(message);
    }

    void SelfTest()
    {
        NetworkValidationTest();
        DnsSelfTest();
        auto root = fs::temp_directory_path() / ("NanaBox-GuestTool-" + Token());
        Require(fs::create_directory(root), "Cannot create test directory");
#ifndef _WIN32
        Require(chmod(root.c_str(), 0700) == 0, "Cannot protect test directory");
#endif
        struct Cleanup { fs::path Root; ~Cleanup() { std::error_code ec; fs::remove_all(Root, ec); } } cleanup{root};
        Agent agent(root, false);
        auto call = [&](std::string resource, std::string action, Json settings = Json::object())
        {
            auto result = agent.Dispatch(Request(std::move(resource), std::move(action), std::move(settings)));
            if (!result.value("Success", false)) throw std::runtime_error(result.value("Error", "Request failed"));
            return result.at("Data");
        };
        auto bad = [&](std::string resource, std::string action, Json settings)
        {
            Require(!agent.Dispatch(Request(std::move(resource), std::move(action), std::move(settings)))
                .value("Success", true), "Invalid request was accepted");
        };
        Require(call("Heartbeat", "Query").at("NativeGuestTool") == true, "Heartbeat failed");
        Require(call("GuestInformation", "Query").at("Capabilities").at("FileExchange") == true, "Capabilities failed");
        Require(call("TimeSync", "Query").at("ClockSetEnabled") == false, "Clock capability failed");
        bad("TimeSync", "Update", {{"UnixMilliseconds", 1700000000000ull}});
        for (auto name : {"../escape", "CON.dll", "a:b", ".hidden", "bad.", "COM1.txt"})
            bad("FileExchange", "BeginUpload", {{"Name", name}, {"Size", 0}, {"Overwrite", false}});
        std::vector<unsigned char> data(Chunk + 19);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i % 251);
        Digest digest; digest.Update(data); auto sha = digest.Finish();
        auto start = call("FileExchange", "BeginUpload", {{"Name", "payload.bin"}, {"Size", data.size()}, {"Overwrite", false}});
        auto token = start.at("Token").get<std::string>();
        bad("FileExchange", "Write", {{"Token", token}, {"Offset", 1}, {"Data", "YWJj"}});
        bad("FileExchange", "Write", {{"Token", token}, {"Offset", 0}, {"Data", "YR=="}});
        for (std::size_t offset = 0; offset < data.size();)
        {
            auto count = std::min(Chunk, data.size() - offset);
            std::vector<unsigned char> part(data.begin() + static_cast<std::ptrdiff_t>(offset),
                data.begin() + static_cast<std::ptrdiff_t>(offset + count));
            auto result = call("FileExchange", "Write", {{"Token", token}, {"Offset", offset}, {"Data", Encode(part)}});
            offset += count;
            Require(result.at("Offset") == offset, "Upload offset mismatch");
            Require(!fs::exists(root / "payload.bin"), "Upload published before commit");
        }
        Require(call("FileExchange", "Commit", {{"Token", token}, {"Sha256", sha}}).at("Sha256") == sha, "Upload hash mismatch");
        bad("FileExchange", "BeginUpload", {{"Name", "payload.bin"}, {"Size", 0}, {"Overwrite", false}});
        token = call("FileExchange", "OpenDownload", {{"Name", "payload.bin"}}).at("Token").get<std::string>();
        std::vector<unsigned char> actual;
        while (actual.size() < data.size())
        {
            auto reply = call("FileExchange", "Read", {{"Token", token}, {"Offset", actual.size()}, {"Length", Chunk}});
            auto part = Decode(reply.at("Data").get<std::string>());
            actual.insert(actual.end(), part.begin(), part.end());
        }
        Require(actual == data, "File payload mismatch");
        Require(call("FileExchange", "FinishDownload", {{"Token", token}}).at("Sha256") == sha, "Download hash mismatch");
        token = call("FileExchange", "BeginUpload", {{"Name", "payload.bin"}, {"Size", 0}, {"Overwrite", true}}).at("Token").get<std::string>();
        bad("FileExchange", "Commit", {{"Token", token}, {"Sha256", std::string(64, '0')}});
        Require(fs::file_size(root / "payload.bin") == data.size(), "Failed upload changed destination");
        token = call("FileExchange", "BeginUpload", {{"Name", "empty.bin"}, {"Size", 0}, {"Overwrite", false}}).at("Token").get<std::string>();
        Digest empty;
        Require(call("FileExchange", "Commit", {{"Token", token}, {"Sha256", empty.Finish()}}).at("Size") == 0, "Empty file failed");
        token = call("FileExchange", "BeginUpload", {{"Name", "aborted.bin"}, {"Size", 1}, {"Overwrite", false}}).at("Token").get<std::string>();
        call("FileExchange", "Abort", {{"Token", token}});
        Require(!fs::exists(root / "aborted.bin"), "Aborted file published");
        for (auto const& item : fs::directory_iterator(root))
            Require(item.path().filename().u8string().rfind(".nanabox-upload-", 0) != 0, "Temporary file leaked");
#ifndef _WIN32
        fs::create_symlink(root / "payload.bin", root / "link.bin");
        bad("FileExchange", "OpenDownload", {{"Name", "link.bin"}});
        bad("FileExchange", "BeginUpload", {{"Name", "link.bin"}, {"Size", 1}, {"Overwrite", true}});
        int pair[2]{};
        Require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "Socket pair failed");
        Socket client(pair[0]), server(pair[1]);
        std::thread worker([&]() { try { Handle(server.Value, agent); } catch (...) {} });
        try
        {
            auto end = Mono::now() + std::chrono::seconds(2);
            Send(client.Value, Request("Heartbeat", "Query"), end);
            Require(Receive(client.Value, end).at("Success") == true, "Framed heartbeat failed");
        }
        catch (...) { worker.join(); throw; }
        worker.join();
#endif
        std::cout << "GuestTool native protocol and file exchange tests passed\n";
    }

    fs::path ExchangeRoot;
    bool AllowClock = false;
    bool NetworkMode = false;
    bool DnsMode = false;
    bool BindMode = false;
    fs::path LoopbackState;
#ifdef _WIN32
    SERVICE_STATUS_HANDLE StatusHandle = nullptr;
    constexpr wchar_t ServiceName[] = L"NanaBoxGuestTool";
    constexpr wchar_t NetworkServiceName[] = L"NanaBoxGuestNetwork";
    void Status(DWORD state, DWORD error = 0)
    {
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = state;
        status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        status.dwWin32ExitCode = error;
        status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
        if (StatusHandle) SetServiceStatus(StatusHandle, &status);
    }
    void WINAPI Control(DWORD control)
    {
        if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
        { Stop = true; Status(SERVICE_STOP_PENDING); }
    }
    void WINAPI ServiceMain(DWORD, LPWSTR*)
    {
        StatusHandle = RegisterServiceCtrlHandlerW(NetworkMode ? NetworkServiceName : ServiceName, Control);
        if (!StatusHandle) return;
        try
        {
            Agent agent(ExchangeRoot, AllowClock, NetworkMode, LoopbackState);
            Status(SERVICE_RUNNING);
            Serve(agent, NetworkMode, DnsMode, BindMode);
            Status(SERVICE_STOPPED);
        }
        catch (...) { Status(SERVICE_STOPPED, ERROR_GEN_FAILURE); }
    }
#else
    void OnSignal(int) { Stop = true; }
#endif
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try
    {
        bool service = false;
        for (int i = 1; i < argc; ++i)
        {
            auto arg = fs::path(argv[i]).u8string();
            if (arg == "--self-test") { SelfTest(); return 0; }
#ifndef _WIN32
            if (arg == "--check-bind-support") { CheckBindSupport(); return 0; }
            if (arg == "--bind-self-test") { BindSelfTest(); return 0; }
            if (arg == "--network-self-test") { NativeNetworkIntegrationTest(); return 0; }
            if (arg == "--loopback-self-test") { NativeLoopbackIntegrationTest(); return 0; }
            if (arg == "--clear-loopback" && i + 1 < argc) { LoopbackRouting(fs::path(argv[++i])).Clear(); return 0; }
#endif
            if (arg == "--exchange-root" && i + 1 < argc) ExchangeRoot = fs::path(argv[++i]);
            else if (arg == "--allow-clock-set") AllowClock = true;
            else if (arg == "--service") service = true;
            else if (arg == "--network") NetworkMode = true;
            else if (arg == "--dns-tunnel") DnsMode = true;
            else if (arg == "--bind-coordination") BindMode = true;
            else if (arg == "--loopback-state" && i + 1 < argc) LoopbackState = fs::path(argv[++i]);
            else if (arg != "--serve") Fail("Unknown GuestTool option");
        }
        if ((DnsMode || BindMode || !LoopbackState.empty()) && !NetworkMode) Fail("Network integration options require network mode");
#ifdef _WIN32
        if (service)
        {
            SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(NetworkMode ? NetworkServiceName : ServiceName), ServiceMain}, {nullptr, nullptr}};
            if (!StartServiceCtrlDispatcherW(table)) Fail("Cannot connect to service control manager");
            return 0;
        }
#else
        if (service) Fail("Use --serve with systemd");
        signal(SIGTERM, OnSignal); signal(SIGINT, OnSignal);
#endif
        Agent agent(ExchangeRoot, AllowClock, NetworkMode, LoopbackState);
        Serve(agent, NetworkMode, DnsMode, BindMode);
        return 0;
    }
    catch (std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
}
