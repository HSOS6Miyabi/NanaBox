#define NANABOX_PLAN9_TEST
#include "Plan9.cpp"
#include <cassert>

object share(const std::string &n, bool ro = false, bool automatic = true, int portNumber = 50001)
{
    return {{"Name", n}, {"ReadOnly", ro}, {"AutoMount", automatic}, {"Port", portNumber}};
}
object request(const char *action, object settings = {})
{
    return {{"Version", 1}, {"ResourceType", "Plan9Shares"}, {"RequestType", action}, {"Settings", settings}};
}
void expectFailure(const std::function<void()> &action)
{
    bool rejected = false;
    try
    {
        action();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);
}
int main()
{
    assert(unescape("/media/Shared\\040Files") == "/media/Shared Files");
    for (const auto *n : {"", "..", "a/b", "a\\b", "a,b", "a:b", " x", "x\n"})
        expectFailure([&]() { descriptor(share(n)); });
    for (int n : {0, -1, 27453, 27456})
        expectFailure([&]() { descriptor(share("Projects", false, true, n)); });
    expectFailure([]() { inventory(array{share("Projects"), share("projects")}); });
    expectFailure([]() { descriptor(share(LibraryShare, false)); });
    char temporary[] = "/tmp/nanabox-plan9-tests-XXXXXX";
    if (!mkdtemp(temporary))
        sysfail("mkdtemp");
    const fs::path root(temporary);
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            fs::remove_all(path);
        }
    } cleanup{root};
    MountTable mounts;
    unsigned calls = 0;
    uint64_t nextId = 1;
    bool busy = false, failFirst = false, gpu = false;
    Platform mock;
    mock.mounts = [&]() { return mounts; };
    mock.mount = [&](const object &item, const fs::path &target) {
        ++calls;
        if (failFirst && str(item, "Name") == "Projects")
            fail("server unavailable");
        mounts[target.string()] = {{nextId++, "9p", source(item), item.at("ReadOnly").as_bool()}};
    };
    mock.unmount = [&](const fs::path &target) {
        if (busy)
            fail("busy");
        mounts.erase(target.string());
    };
    mock.prepare = [](const fs::path &) { return false; };
    mock.gpu = [&](bool active) { gpu = active; };
    Manager mgr(root / "state", root / "media", root / "wsl", mock);
    auto projects = share("Projects");
    auto library = share(LibraryShare, true);
    auto drivers = share(DriverShare, true);
    auto desired = inventory(array{projects, library, drivers, share("Manual", true, false)});
    assert(
        dispatch(mgr, request("Update", {{"Shares", array{projects, library, drivers, share("Manual", true, false)}}}))
            .at("Success")
            .as_bool());
    mgr.reconcile(128);
    assert(calls == 3 && gpu && mounts.count((root / "media/Projects").string()) &&
           mounts.count((root / "wsl/drivers").string()) && mounts.count((root / "wsl/lib").string()));
    mgr.reconcile(128);
    assert(calls == 3);
    mgr.manualUnmount("Projects");
    mgr.reconcile(128);
    assert(calls == 3);
    mgr.update(desired);
    mgr.reconcile(128);
    assert(calls == 3);
    mgr.manualMount("Projects", (root / "custom").string());
    assert(mounts.count((root / "custom").string()));
    mgr.manualMount("Manual");
    assert(mounts.at((root / "media/Manual").string())[0].ro);
    busy = true;
    expectFailure([&]() { mgr.prepareRemove("Projects", 50001); });
    assert(mounts.count((root / "custom").string()));
    busy = false;
    expectFailure([&]() { mgr.prepareRemove("Projects", 50002); });
    mgr.prepareRemove("Projects", 50001);
    assert(!mounts.count((root / "custom").string()));
    mgr.update({});
    mgr.reconcile(128);
    assert(mounts.empty() && !gpu);
    // Explicit local mounts survive host inventory refresh and service restart.
    const auto localTarget = (root / "local").string();
    mgr.manualMount("Local", localTarget, 50002, true);
    mgr.update(desired);
    mgr.reconcile(128);
    assert(mounts.at(localTarget)[0].ro);
    Manager localRestart(root / "state", root / "media", root / "wsl", mock);
    localRestart.update({});
    localRestart.reconcile(128);
    assert(mounts.size() == 1 && mounts.count(localTarget));
    expectFailure([&]() { localRestart.manualMount("local", (root / "other").string(), 50002); });
    expectFailure([&]() { localRestart.manualMount("nanabox.hostdrivers", (root / "other").string(), 50002); });
    // A host definition takes ownership and cannot be shadowed by a local mount.
    localRestart.update(inventory(array{share("Local", true, false, 50003)}));
    localRestart.reconcile(128);
    assert(!mounts.count(localTarget));
    localRestart.manualMount("Local", localTarget);
    assert(mounts.at(localTarget)[0].source == source(share("Local", true, false, 50003)));
    localRestart.update({});
    localRestart.reconcile(128);
    assert(mounts.empty());
    localRestart.manualMount("Local", localTarget, 50002);
    localRestart.manualUnmount("Local");
    assert(mounts.empty() && localRestart.query().at("Shares").as_array().empty());
    // Foreign filesystems must be left intact, and unavailable shares must retry without starving others.
    auto target = (root / "media/Projects").string();
    mounts[target] = {{nextId++, "tmpfs", "tmpfs", false}};
    mgr.update(inventory(array{projects}));
    mgr.reconcile(128);
    assert(mounts.at(target)[0].type == "tmpfs");
    assert(mgr.query().at("Shares").as_array()[0].as_object().contains("Error"));
    mounts.clear();
    mgr.update({});
    mgr.reconcile();
    mgr.update(inventory(array{projects, share("Second")}));
    failFirst = true;
    mgr.reconcile();
    mgr.reconcile();
    assert(mounts.count((root / "media/Second").string()));
    failFirst = false;
    Manager restarted(root / "state", root / "media", root / "wsl", mock);
    restarted.reconcile(128);
    assert(mounts.count(target));
    restarted.cleanup();
    assert(mounts.empty() && !gpu);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Fd a(pair[0]), b(pair[1]);
    sendFrame(a, request("Query"));
    auto response = dispatch(restarted, receive(b));
    assert(response.at("Success").as_bool());
    auto bad = dispatch(restarted, request("Update", {{"Shares", array{share("x"), share("X")}}}));
    assert(!bad.at("Success").as_bool());
    auto config = root / "managed.conf";
    bool changed = false;
    managedFile(config, std::string(Marker) + "/test\n", true, changed);
    assert(changed && fs::exists(config));
    changed = false;
    managedFile(config, std::string(Marker) + "/test\n", true, changed);
    assert(!changed);
    managedFile(config, "", false, changed);
    assert(changed && !fs::exists(config));
    atomicWrite(config, "foreign\n", 0644);
    changed = false;
    managedFile(config, "", false, changed);
    assert(!changed && readFile(config) == "foreign\n");
    expectFailure([&]() { managedFile(config, std::string(Marker), true, changed); });
    std::cout << "Plan 9 protocol, mount reconciliation, GPU paths and managed environment tests passed\n";
}
