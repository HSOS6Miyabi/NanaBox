#include <boost/json/src.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <utility>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace j = boost::json;
using j::array;
using j::object;
using j::value;
constexpr uint32_t ControlPort = 27456;
constexpr size_t MaxMessage = 256 * 1024;
constexpr const char *DriverShare = "NanaBox.HostDrivers";
constexpr const char *LibraryShare = "NanaBox.HostLxssLib";

[[noreturn]] void fail(const std::string &message)
{
    throw std::runtime_error(message);
}
[[noreturn]] void sysfail(const std::string &message)
{
    throw std::system_error(errno, std::generic_category(), message);
}
struct Fd
{
    int n;
    explicit Fd(int fd) : n(fd)
    {
        if (n < 0)
            sysfail("open socket or file");
    }
    ~Fd()
    {
        close(n);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    operator int() const
    {
        return n;
    }
};
std::string str(const value &v)
{
    const auto &s = v.as_string();
    return {s.data(), s.size()};
}
std::string str(const object &o, const char *key)
{
    return str(o.at(key));
}
uint32_t port(const value &v)
{
    uint64_t n;
    if (v.is_int64() && v.as_int64() > 0)
        n = static_cast<uint64_t>(v.as_int64());
    else if (v.is_uint64())
        n = v.as_uint64();
    else
        fail("Invalid port");
    if (!n || n >= UINT32_MAX || (n >= 27453 && n <= 27457))
        fail("Invalid or reserved port");
    return static_cast<uint32_t>(n);
}
std::string name(const value &v)
{
    auto s = str(v);
    if (s.empty() || s.size() > 200 || s == "." || s == ".." || s.front() == ' ' || s.back() == ' ' ||
        std::any_of(s.begin(), s.end(), [](unsigned char c) {
            return c < 32 || c == 127 || c == '/' || c == '\\' || c == ',' || c == ':';
        }))
        fail("Invalid Plan 9 share name");
    return s;
}
std::string folded(std::string s)
{
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}
bool gpuShare(const std::string &s)
{
    return s == DriverShare || s == LibraryShare;
}
object descriptor(const value &v)
{
    auto o = v.as_object();
    if (o.size() != 4)
        fail("Invalid share descriptor");
    auto n = name(o.at("Name"));
    (void)port(o.at("Port"));
    if (!o.at("AutoMount").is_bool() || !o.at("ReadOnly").is_bool())
        fail("Invalid share flags");
    if (gpuShare(n) && !o.at("ReadOnly").as_bool())
        fail("GPU share must be read-only");
    return o;
}
object inventory(const value &v)
{
    const auto &a = v.as_array();
    if (a.size() > 128)
        fail("Too many shares");
    object result;
    std::set<std::string> seen;
    for (const auto &item : a)
    {
        auto o = descriptor(item);
        auto n = str(o, "Name");
        if (!seen.insert(folded(n)).second)
            fail("Duplicate share name");
        result[n] = std::move(o);
    }
    return result;
}
std::string source(const object &o)
{
    return "nanabox-9p:" + std::to_string(port(o.at("Port"))) + ":" + str(o, "Name");
}
void writeAll(int fd, const char *p, size_t size)
{
    while (size)
    {
        auto n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            sysfail("write");
        if (!n)
            fail("Short write");
        p += n;
        size -= static_cast<size_t>(n);
    }
}
std::string readAll(int fd, size_t size)
{
    std::string s(size, '\0');
    size_t done = 0;
    while (done < size)
    {
        auto n = read(fd, s.data() + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            sysfail("read");
        if (!n)
            fail("Incomplete frame");
        done += static_cast<size_t>(n);
    }
    return s;
}
value receive(int fd)
{
    auto h = readAll(fd, 4);
    uint32_t n;
    std::memcpy(&n, h.data(), 4);
    n = ntohl(n);
    if (!n || n > MaxMessage)
        fail("Invalid frame size");
    return j::parse(readAll(fd, n));
}
void sendFrame(int fd, const value &v)
{
    auto s = j::serialize(v);
    if (s.empty() || s.size() > MaxMessage)
        fail("Invalid response size");
    auto n = htonl(static_cast<uint32_t>(s.size()));
    writeAll(fd, reinterpret_cast<char *>(&n), 4);
    writeAll(fd, s.data(), s.size());
}
void checkDirectory(const fs::path &p, bool privateDir = false)
{
    struct stat st{};
    if (lstat(p.c_str(), &st))
        sysfail("stat " + p.string());
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (privateDir ? 0077 : 0022)))
        fail("Unsafe directory: " + p.string());
}
void ensureDirectory(const fs::path &p, mode_t mode = 0755)
{
    if (mkdir(p.c_str(), mode) && errno != EEXIST)
        sysfail("mkdir " + p.string());
    checkDirectory(p, mode == 0700);
}
bool prepareDirectory(const fs::path &p)
{
    if (!p.is_absolute() || p == p.root_path() || std::find(p.begin(), p.end(), fs::path("..")) != p.end())
        fail("Choose an absolute mount directory other than /");
    for (auto parent = p.parent_path(); !parent.empty(); parent = parent.parent_path())
    {
        checkDirectory(parent);
        if (parent == parent.root_path())
            break;
    }
    bool created = mkdir(p.c_str(), 0755) == 0;
    if (!created && errno != EEXIST)
        sysfail("mkdir " + p.string());
    checkDirectory(p);
    if (!fs::is_empty(p))
        fail("Mount directory is not empty: " + p.string());
    return created;
}
void atomicWrite(const fs::path &p, const std::string &data, mode_t mode)
{
    std::string pattern = (p.parent_path() / ".nanabox-XXXXXX").string();
    Fd fd(mkstemp(pattern.data()));
    try
    {
        if (fchmod(fd, mode))
            sysfail("chmod");
        writeAll(fd, data.data(), data.size());
        if (fsync(fd))
            sysfail("fsync");
        if (rename(pattern.c_str(), p.c_str()))
            sysfail("rename");
    }
    catch (...)
    {
        unlink(pattern.c_str());
        throw;
    }
}
std::string readFile(const fs::path &p)
{
    Fd fd(open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat st{};
    if (fstat(fd, &st))
        sysfail("fstat");
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) || st.st_size < 0 ||
        st.st_size > 4 * 1024 * 1024)
        fail("Unsafe file: " + p.string());
    return readAll(fd, static_cast<size_t>(st.st_size));
}
std::string unescape(const std::string &s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7' && s[i + 2] >= '0' &&
            s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7')
        {
            r += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + s[i + 3] - '0');
            i += 3;
        }
        else
            r += s[i];
    }
    return r;
}
struct Mount
{
    uint64_t id;
    std::string type, source;
    bool ro;
};
using MountTable = std::map<std::string, std::vector<Mount>>;
MountTable mountTable()
{
    std::ifstream f("/proc/self/mountinfo");
    if (!f)
        fail("Cannot open mountinfo");
    MountTable result;
    std::string line;
    while (std::getline(f, line))
    {
        auto sep = line.find(" - ");
        if (sep == std::string::npos)
            fail("Invalid mountinfo");
        std::istringstream a(line.substr(0, sep)), b(line.substr(sep + 3));
        uint64_t id, parent;
        std::string dev, root, target, opts, type, src;
        if (!(a >> id >> parent >> dev >> root >> target >> opts) || !(b >> type >> src))
            fail("Invalid mountinfo");
        result[unescape(target)].push_back(
            {id, type, unescape(src), ("," + opts + ",").find(",ro,") != std::string::npos});
    }
    return result;
}
void nativeMount(const object &item, const fs::path &target)
{
    Fd fd(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_HOST;
    addr.svm_port = port(item.at("Port"));
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        if (errno != EINPROGRESS)
            sysfail("connect 9P");
        pollfd p{fd, POLLOUT, 0};
        int rc;
        do
        {
            rc = poll(&p, 1, 5000);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            sysfail("poll");
        if (!rc)
            fail("9P connection timed out");
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len))
            sysfail("SO_ERROR");
        if (err)
        {
            errno = err;
            sysfail("connect 9P");
        }
    }
    if (fcntl(fd, F_SETFL, 0))
        sysfail("set blocking socket");
    int size = 65536;
    for (auto opt : {SO_RCVBUF, SO_SNDBUF})
        if (setsockopt(fd, SOL_SOCKET, opt, &size, sizeof(size)))
            sysfail("socket buffer");
    auto opts = "trans=fd,rfdno=" + std::to_string(fd.n) + ",wfdno=" + std::to_string(fd.n) +
                ",msize=65536,cache=none,aname=" + str(item, "Name");
    unsigned long flags = MS_NOSUID | MS_NODEV;
    if (item.at("ReadOnly").as_bool())
        flags |= MS_RDONLY;
    if (mount(source(item).c_str(), target.c_str(), "9p", flags, opts.c_str()))
        sysfail("mount " + target.string());
}
void nativeUnmount(const fs::path &p)
{
    if (umount2(p.c_str(), 0))
        sysfail("unmount " + p.string());
}
void refreshLoader()
{
    pid_t child = fork();
    if (child < 0)
        sysfail("fork ldconfig");
    if (!child)
    {
        execl("/sbin/ldconfig", "ldconfig", "-X", nullptr);
        execl("/usr/sbin/ldconfig", "ldconfig", "-X", nullptr);
        _exit(127);
    }
    int status;
    pid_t rc;
    do
    {
        rc = waitpid(child, &status, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        sysfail("wait ldconfig");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("ldconfig failed");
}
constexpr const char *Marker = "# Managed by NanaBox GuestTool\n";
void managedFile(const fs::path &path, const std::string &contents, bool enable, bool &changed)
{
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0)
    {
        auto before = readFile(path);
        if (before.compare(0, std::strlen(Marker), Marker) != 0)
        {
            if (!enable)
                return;
            fail("Configuration belongs to another application: " + path.string());
        }
        if (!enable)
        {
            if (unlink(path.c_str()))
                sysfail("remove " + path.string());
            changed = true;
        }
        else if (before != contents)
        {
            atomicWrite(path, contents, 0644);
            changed = true;
        }
    }
    else if (errno != ENOENT)
        sysfail("stat " + path.string());
    else if (enable)
    {
        checkDirectory(path.parent_path());
        atomicWrite(path, contents, 0644);
        changed = true;
    }
}
void gpuEnvironment(bool active)
{
    bool changed = false;
    if (active)
    {
        for (const fs::path &path :
             {fs::path("/etc/ld.so.conf.d/90-nanabox-gpu.conf"), fs::path("/etc/profile.d/nanabox-gpu.sh")})
        {
            checkDirectory(path.parent_path());
            struct stat st{};
            if (lstat(path.c_str(), &st) == 0)
            {
                if (readFile(path).compare(0, std::strlen(Marker), Marker) != 0)
                    fail("Configuration belongs to another application: " + path.string());
            }
            else if (errno != ENOENT)
                sysfail("stat " + path.string());
        }
    }
    managedFile("/etc/ld.so.conf.d/90-nanabox-gpu.conf", std::string(Marker) + "/usr/lib/wsl/lib\n", active, changed);
    managedFile("/etc/profile.d/nanabox-gpu.sh",
                std::string(Marker) +
                    "if mountpoint -q /usr/lib/wsl/lib; then\n"
                    "  export PATH=\"/usr/lib/wsl/lib${PATH:+:$PATH}\"\n"
                    "  export LD_LIBRARY_PATH=\"/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
                    "  export LIBRARY_PATH=\"/usr/lib/wsl/lib${LIBRARY_PATH:+:$LIBRARY_PATH}\"\n"
                    "fi\n",
                active, changed);
    static auto last = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (changed || (active && now - last >= std::chrono::seconds(60)))
    {
        refreshLoader();
        last = now;
    }
}
struct Platform
{
    std::function<MountTable()> mounts = mountTable;
    std::function<void(const object &, const fs::path &)> mount = nativeMount;
    std::function<void(const fs::path &)> unmount = nativeUnmount;
    std::function<bool(const fs::path &)> prepare = prepareDirectory;
    std::function<void(bool)> gpu = gpuEnvironment;
};
class Manager
{
    fs::path root, media, gpuRoot;
    Platform p;
    using Clock = std::chrono::steady_clock;
    std::map<std::string, Clock::time_point> retry;
    Fd lock()
    {
        ensureDirectory(root, 0700);
        Fd fd(open((root / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        // Return a new descriptor referring to the same locked open file description.
        if (flock(fd, LOCK_EX))
            sysfail("lock");
        return Fd(fcntl(fd, F_DUPFD_CLOEXEC, 0));
    }
    object load()
    {
        struct stat st{};
        auto path = root / "state.json";
        if (lstat(path.c_str(), &st))
        {
            if (errno != ENOENT)
                sysfail("stat state");
            return {{"Shares", object{}}, {"Mounted", object{}}, {"Suppressed", array{}}, {"Errors", object{}}};
        }
        auto o = j::parse(readFile(path)).as_object();
        for (auto key : {"Shares", "Mounted", "Errors"})
            (void)o.at(key).as_object();
        (void)o.at("Suppressed").as_array();
        return o;
    }
    void save(const object &state)
    {
        atomicWrite(root / "state.json", j::serialize(state), 0600);
    }
    static bool suppressed(const object &state, const std::string &key)
    {
        for (const auto &v : state.at("Suppressed").as_array())
            if (str(v) == key)
                return true;
        return false;
    }
    static void unsuppress(object &state, const std::string &key)
    {
        auto &a = state.at("Suppressed").as_array();
        a.erase(std::remove_if(a.begin(), a.end(), [&](const value &v) { return str(v) == key; }), a.end());
    }
    fs::path automatic(const std::string &key) const
    {
        if (key == DriverShare)
            return gpuRoot / "drivers";
        if (key == LibraryShare)
            return gpuRoot / "lib";
        return media / key;
    }
    void prepareParent(const std::string &key)
    {
        const auto &path = gpuShare(key) ? gpuRoot : media;
        checkDirectory(path.parent_path());
        ensureDirectory(path);
    }
    bool current(const object &record, uint64_t *id = nullptr)
    {
        auto table = p.mounts();
        auto target = str(record, "Target");
        auto it = table.find(target);
        if (it == table.end())
            return false;
        const auto &rows = it->second;
        const auto &item = record.at("Share").as_object();
        if (rows.size() != 1 || rows[0].type != "9p" || rows[0].source != source(item))
            fail("Mount directory occupied by another filesystem");
        const auto &saved = record.at("Id");
        if (!saved.is_null() && j::value(rows[0].id) != saved)
            fail("Mount ID changed");
        if (rows[0].ro != item.at("ReadOnly").as_bool())
            fail("Unexpected mount access mode");
        if (id)
            *id = rows[0].id;
        return true;
    }
    void drop(object &state, const std::string &key)
    {
        auto &mounted = state.at("Mounted").as_object();
        auto record = mounted.at(key).as_object();
        auto target = str(record, "Target");
        if (current(record))
        {
            p.unmount(target);
            if (p.mounts().count(target))
                fail("Mount directory still occupied");
        }
        if (record.at("Created").as_bool())
            rmdir(target.c_str());
        mounted.erase(key);
        save(state);
    }
    void attach(object &state, const object &item, const fs::path &path, bool manual)
    {
        auto key = str(item, "Name");
        if (!path.is_absolute() || path.native().find('\0') != std::string::npos ||
            std::find(path.begin(), path.end(), fs::path("..")) != path.end())
            fail("Invalid mount path");
        auto target = path.lexically_normal().string();
        auto &mounted = state.at("Mounted").as_object();
        if (auto old = mounted.if_contains(key))
        {
            auto record = old->as_object();
            if (record.at("Share") != item || str(record, "Target") != target)
                fail("Unmount the existing share before changing options");
            if (current(record))
                return;
            drop(state, key);
        }
        if (p.mounts().count(target))
            fail("Mount directory occupied");
        bool created = p.prepare(target);
        object record = {
            {"Share", item}, {"Target", target}, {"Created", created}, {"Manual", manual}, {"Id", nullptr}};
        mounted[key] = record;
        save(state);
        try
        {
            p.mount(item, target);
            uint64_t id;
            if (!current(record, &id))
                fail("9P mount missing from mountinfo");
            record["Id"] = id;
            mounted[key] = record;
            save(state);
        }
        catch (...)
        {
            try
            {
                if (!current(record))
                    drop(state, key);
            }
            catch (...)
            {
            }
            throw;
        }
    }
    object status(const object &state)
    {
        const auto &shares = state.at("Shares").as_object();
        const auto &mounted = state.at("Mounted").as_object();
        const auto &errors = state.at("Errors").as_object();
        std::set<std::string> keys;
        for (auto &e : shares)
            keys.insert(std::string(e.key()));
        for (auto &e : mounted)
            keys.insert(std::string(e.key()));
        array list;
        for (auto &key : keys)
        {
            auto s = shares.if_contains(key), m = mounted.if_contains(key);
            object row = s ? s->as_object() : m->as_object().at("Share").as_object();
            row["Available"] = s != nullptr;
            row["MountPoint"] = m ? str(m->as_object(), "Target") : automatic(key).string();
            row["Mounted"] = false;
            row["Suppressed"] = suppressed(state, key);
            if (m)
                try
                {
                    row["Mounted"] = current(m->as_object());
                }
                catch (const std::exception &e)
                {
                    row["Error"] = e.what();
                }
            if (auto e = errors.if_contains(key))
                row["Error"] = *e;
            list.push_back(row);
        }
        return {{"Shares", list}};
    }
    void syncGpu(const object &state)
    {
        auto m = state.at("Mounted").as_object().if_contains(LibraryShare);
        bool active = false;
        if (m && state.at("Shares").as_object().contains(LibraryShare))
        {
            auto &record = m->as_object();
            active = str(record, "Target") == automatic(LibraryShare).string() &&
                     record.at("Share").as_object().at("ReadOnly").as_bool() && current(record);
        }
        p.gpu(active);
    }

  public:
    Manager(fs::path r = "/run/nanabox-plan9", fs::path m = "/media", fs::path g = "/usr/lib/wsl", Platform ops = {})
        : root(std::move(r)), media(std::move(m)), gpuRoot(std::move(g)), p(std::move(ops))
    {
    }
    object query()
    {
        auto guard = lock();
        return status(load());
    }
    object update(object desired)
    {
        auto guard = lock();
        auto state = load();
        auto &shares = state.at("Shares").as_object();
        if (shares != desired)
        {
            auto old = shares;
            shares = std::move(desired);
            auto &s = state.at("Suppressed").as_array();
            s.erase(std::remove_if(s.begin(), s.end(),
                                   [&](const value &v) {
                                       auto key = str(v);
                                       auto a = old.if_contains(key), b = shares.if_contains(key);
                                       return !a || !b || *a != *b;
                                   }),
                    s.end());
            for (auto it = retry.begin(); it != retry.end();)
            {
                auto a = old.if_contains(it->first), b = shares.if_contains(it->first);
                if (!a || !b || *a != *b)
                    it = retry.erase(it);
                else
                    ++it;
            }
            save(state);
        }
        return status(state);
    }
    void reconcile(size_t maxMounts = 1)
    {
        auto guard = lock();
        auto state = load();
        auto &shares = state.at("Shares").as_object();
        auto &mounted = state.at("Mounted").as_object();
        object errors;
        std::vector<std::string> keys;
        for (auto &e : mounted)
            keys.emplace_back(e.key());
        for (auto &key : keys)
            try
            {
                auto record = mounted.at(key).as_object(), old = record.at("Share").as_object();
                auto d = shares.if_contains(key);
                bool manual = record.at("Manual").as_bool();
                if (!d || d->as_object().at("Port") != old.at("Port") ||
                    (d->as_object().at("ReadOnly").as_bool() && !old.at("ReadOnly").as_bool()) ||
                    (!manual && (!d->as_object().at("AutoMount").as_bool() ||
                                 d->as_object().at("ReadOnly") != old.at("ReadOnly") ||
                                 str(record, "Target") != automatic(key).string())))
                    drop(state, key);
                else if (!current(record))
                {
                    if (manual && !suppressed(state, key))
                        state.at("Suppressed").as_array().push_back(j::value(key));
                    drop(state, key);
                }
            }
            catch (const std::exception &e)
            {
                errors[key] = std::string(e.what()).substr(0, 500);
            }
        size_t attempts = 0;
        for (auto &entry : shares)
        {
            auto key = std::string(entry.key());
            auto item = descriptor(entry.value());
            if (!item.at("AutoMount").as_bool() || suppressed(state, key) || mounted.contains(key) ||
                errors.contains(key))
                continue;
            if (attempts >= maxMounts || Clock::now() < retry[key])
            {
                if (auto e = state.at("Errors").as_object().if_contains(key))
                    errors[key] = *e;
                continue;
            }
            ++attempts;
            try
            {
                prepareParent(key);
                attach(state, item, automatic(key), false);
                retry.erase(key);
            }
            catch (const std::exception &e)
            {
                errors[key] = std::string(e.what()).substr(0, 500);
                retry[key] = Clock::now() + std::chrono::seconds(15);
            }
        }
        try
        {
            syncGpu(state);
        }
        catch (const std::exception &e)
        {
            errors[LibraryShare] = std::string(e.what()).substr(0, 500);
        }
        if (state.at("Errors") != errors)
        {
            state["Errors"] = errors;
            save(state);
        }
    }
    object manualMount(const std::string &key, const std::string &path = "", uint32_t manualPort = 0,
                       bool readOnly = false)
    {
        name(j::value(key));
        auto guard = lock();
        auto state = load();
        auto &shares = state.at("Shares").as_object();
        auto item = shares.if_contains(key);
        if (!item)
        {
            if (!manualPort || gpuShare(key))
                fail("Unknown share; specify its port or wait for host inventory");
            shares[key] =
                descriptor(object{{"Name", key}, {"Port", manualPort}, {"ReadOnly", readOnly}, {"AutoMount", false}});
            item = shares.if_contains(key);
        }
        else if (manualPort && port(item->as_object().at("Port")) != manualPort)
            fail("Port differs from host configuration");
        auto d = item->as_object();
        if (readOnly)
            d["ReadOnly"] = true;
        if (path.empty())
            prepareParent(key);
        attach(state, d, path.empty() ? automatic(key) : fs::path(path), true);
        unsuppress(state, key);
        state.at("Errors").as_object().erase(key);
        retry.erase(key);
        save(state);
        syncGpu(state);
        return status(state);
    }
    object manualUnmount(const std::string &key)
    {
        name(j::value(key));
        auto guard = lock();
        auto state = load();
        auto &shares = state.at("Shares").as_object();
        auto &mounted = state.at("Mounted").as_object();
        if (!shares.contains(key) && !mounted.contains(key))
            fail("Unknown share");
        if (mounted.contains(key))
            drop(state, key);
        if (!suppressed(state, key))
            state.at("Suppressed").as_array().push_back(j::value(key));
        save(state);
        syncGpu(state);
        return status(state);
    }
    object prepareRemove(const std::string &key, uint32_t n)
    {
        name(j::value(key));
        auto guard = lock();
        auto state = load();
        auto &shares = state.at("Shares").as_object();
        auto &mounted = state.at("Mounted").as_object();
        if (auto s = shares.if_contains(key))
            if (port(s->as_object().at("Port")) != n)
                fail("Port differs from share");
        if (auto m = mounted.if_contains(key))
        {
            if (port(m->as_object().at("Share").as_object().at("Port")) != n)
                fail("Port differs from mount");
            drop(state, key);
        }
        auto src = "nanabox-9p:" + std::to_string(n) + ":" + key;
        for (auto &row : p.mounts())
            for (auto &m : row.second)
                if (m.type == "9p" && m.source == src)
                    fail("Share still has an active mount");
        shares.erase(key);
        state.at("Errors").as_object().erase(key);
        unsuppress(state, key);
        save(state);
        syncGpu(state);
        return status(state);
    }
    void cleanup()
    {
        auto guard = lock();
        auto state = load();
        std::vector<std::string> keys;
        for (auto &e : state.at("Mounted").as_object())
            keys.emplace_back(e.key());
        std::string errors;
        for (auto &key : keys)
            try
            {
                drop(state, key);
            }
            catch (const std::exception &e)
            {
                errors += key + ": " + e.what() + "\n";
            }
        syncGpu(state);
        if (!errors.empty())
            fail(errors);
    }
};
object dispatch(Manager &manager, const value &input)
{
    try
    {
        const auto &req = input.as_object();
        if (req.at("Version") != 1 || str(req, "ResourceType") != "Plan9Shares")
            fail("Invalid Plan 9 request");
        auto action = str(req, "RequestType");
        const auto &settings = req.at("Settings").as_object();
        object data;
        if (action == "Query" && settings.empty())
            data = manager.query();
        else if (action == "Update" && settings.size() == 1)
            data = manager.update(inventory(settings.at("Shares")));
        else if (action == "Mount" && settings.size() == 2)
            data = manager.manualMount(name(settings.at("Name")), str(settings, "Target"));
        else if (action == "Unmount" && settings.size() == 1)
            data = manager.manualUnmount(name(settings.at("Name")));
        else if (action == "PrepareRemove" && settings.size() == 2)
            data = manager.prepareRemove(name(settings.at("Name")), port(settings.at("Port")));
        else
            fail("Invalid Plan 9 operation");
        return {{"Version", 1}, {"Success", true}, {"Data", data}};
    }
    catch (const std::exception &e)
    {
        return {{"Version", 1}, {"Success", false}, {"Error", std::string(e.what()).substr(0, 2000)}};
    }
}
void serve(Manager &manager)
{
    Fd listener(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = ControlPort;
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(listener, 4))
        sysfail("listen Plan 9");
    for (;;)
    {
        pollfd poller{listener, POLLIN, 0};
        int n = poll(&poller, 1, 2000);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            sysfail("poll");
        }
        if (n && poller.revents & POLLIN)
            try
            {
                sockaddr_vm peer{};
                socklen_t len = sizeof(peer);
                Fd conn(accept4(listener, reinterpret_cast<sockaddr *>(&peer), &len, SOCK_CLOEXEC));
                if (peer.svm_cid != VMADDR_CID_HOST)
                    continue;
                timeval timeout{8, 0};
                for (int opt : {SO_RCVTIMEO, SO_SNDTIMEO})
                    if (setsockopt(conn, SOL_SOCKET, opt, &timeout, sizeof(timeout)))
                        sysfail("timeout");
                sendFrame(conn, dispatch(manager, receive(conn)));
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << '\n';
            }
        try
        {
            manager.reconcile();
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
        }
    }
}
#ifndef NANABOX_PLAN9_TEST
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    try
    {
        if (geteuid() != 0)
            fail("Run GuestTool as root");
        if (argc < 2)
            fail(
                "Usage: nanabox-plan9 serve|list|mount NAME [TARGET] [--port PORT] [--read-only]|unmount NAME|cleanup");
        Manager manager;
        std::string cmd = argv[1];
        object result;
        if (cmd == "serve" && argc == 2)
        {
            serve(manager);
            return 0;
        }
        if (cmd == "cleanup" && argc == 2)
        {
            manager.cleanup();
            return 0;
        }
        if (cmd == "list" && argc == 2)
            result = manager.query();
        else if (cmd == "unmount" && argc == 3)
            result = manager.manualUnmount(argv[2]);
        else if (cmd == "mount" && argc >= 3)
        {
            std::string path;
            uint32_t n = 0;
            bool ro = false;
            for (int i = 3; i < argc; ++i)
            {
                std::string arg = argv[i];
                if (arg == "--read-only")
                    ro = true;
                else if (arg == "--port" && i + 1 < argc)
                {
                    size_t consumed = 0;
                    std::string num = argv[++i];
                    auto raw = std::stoull(num, &consumed);
                    if (consumed != num.size())
                        fail("Invalid port");
                    n = port(j::value(raw));
                }
                else if (path.empty() && arg.rfind("--", 0) != 0)
                    path = arg;
                else
                    fail("Invalid mount option");
            }
            result = manager.manualMount(argv[2], path, n, ro);
        }
        else
            fail("Invalid command");
        std::cout << j::serialize(result) << '\n';
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#endif
