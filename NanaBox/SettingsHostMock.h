#pragma once
#include "VirtualMachineSettingsPage.h"
#include <Mile.Json.h>
#include <map>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <chrono>

namespace NanaBox
{
    inline std::shared_ptr<SettingsHostOperations> MakeSettingsHostMock()
    {
        using Json = nlohmann::json;
        struct State
        {
            std::mutex Mutex;
            std::vector<HostNetworkInfo> Networks;
            Json Switches = Json::array();
            Json Creators = Json::array();
            Json Machines = Json::array();
            std::map<std::string, Json> HcnObjects;
            std::map<std::string, Json> Checkpoints;
            std::map<std::string, Json> Firewall;
            std::map<std::string, std::vector<char>> GuestFiles;
            std::int64_t GuestClock = 1800000000000ll;
            bool Plan9Mounted = true;
            std::string Plan9MountPoint = "/media/Projects";
        };
        auto Data = std::make_shared<State>();
        auto Operations = std::make_shared<SettingsHostOperations>();
        Operations->NetworkDiagnostics = [](bool Retry)
        {
            return winrt::to_hstring(Json{{"Success", true}, {"Mode", "VmNetwork"},
                {"RetryRequested", Retry}}.dump(2));
        };
        Operations->ExternalAction = [](winrt::hstring const& Action, std::wstring const& Path)
        {
            if (Action != L"OpenConfiguration" && Action != L"Reference")
                throw winrt::hresult_invalid_argument();
            if (Action == L"OpenConfiguration" && Path.empty())
                throw winrt::hresult_invalid_argument();
            return winrt::to_hstring(Json{{"Success", true}, {"Operation", winrt::to_string(Action)},
                {"Path", winrt::to_string(Path)}}.dump(2));
        };
        Operations->GuestAction = [Data](std::string const& Action, std::wstring const& Path,
            std::string const& Name, bool Overwrite, std::shared_ptr<std::atomic<bool>> const& Cancellation)
        {
            if (Name == "cancel-test.bin")
            {
                auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!Cancellation->load() && std::chrono::steady_clock::now() < Deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (Cancellation->load()) throw winrt::hresult_error(E_ABORT);
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            }
            std::lock_guard Lock(Data->Mutex);
            Json Result = Json::object();
            if (Action == "VirtualSmbShares")
                Result = {{"Backend", "VirtualSmb"}, {"Mounted", true}, {"VirtualSmbShares", Json::array({
                    Json{{"Name", "Projects"}, {"Target", "C:\\ProgramData\\NanaBox Guest Shares\\Projects"},
                        {"ReadOnly", true}, {"Mounted", true}}})}};
            else if (Action == "Plan9Shares" || Action == "Plan9Mount" || Action == "Plan9Unmount")
            {
                if (Action != "Plan9Shares")
                {
                    if (Name != "Projects") throw winrt::hresult_invalid_argument();
                    Data->Plan9Mounted = Action == "Plan9Mount";
                    if (Data->Plan9Mounted) Data->Plan9MountPoint = Path.empty() ? "/media/Projects" : winrt::to_string(Path);
                }
                Result = {{"Shares", Json::array({Json{{"Name", "Projects"}, {"MountPoint", Data->Plan9MountPoint},
                    {"Mounted", Data->Plan9Mounted}}})}};
            }
            else if (Action == "TimeQuery" || Action == "TimeSync")
            {
                if (Action == "TimeSync") Data->GuestClock = 1800000000123ll;
                Result = {{"UnixMilliseconds", Data->GuestClock}, {"ClockSetEnabled", true},
                    {"GuestOffsetMilliseconds", Action == "TimeQuery" ? 123 : 0}};
            }
            else if (Action == "FileUpload")
            {
                if (!Overwrite && Data->GuestFiles.count(Name)) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS));
                std::ifstream Input(std::filesystem::path(Path), std::ios::binary);
                if (!Input) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
                std::vector<char> Bytes((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
                if (Input.bad()) throw winrt::hresult_error(E_FAIL);
                Result = {{"Name", Name}, {"Size", Bytes.size()}};
                Data->GuestFiles[Name] = std::move(Bytes);
            }
            else if (Action == "FileDownload")
            {
                auto It = Data->GuestFiles.find(Name);
                if (It == Data->GuestFiles.end()) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
                if (!Overwrite && std::filesystem::exists(std::filesystem::path(Path)))
                    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS));
                std::ofstream Output(std::filesystem::path(Path), std::ios::binary | std::ios::trunc);
                if (!Output) throw winrt::hresult_error(E_FAIL);
                Output.write(It->second.data(), static_cast<std::streamsize>(It->second.size()));
                Output.close();
                if (!Output) throw winrt::hresult_error(E_FAIL);
                Result = {{"Name", Name}, {"Size", It->second.size()}};
            }
            else throw winrt::hresult_invalid_argument();
            return Json{{"Success", true}, {"ResourceType", Action}, {"Data", Result}}.dump(2);
        };
        auto NewId = []()
        {
            GUID Id{};
            winrt::check_hresult(::CoCreateGuid(&Id));
            wchar_t Text[40]{};
            winrt::check_bool(::StringFromGUID2(Id, Text, ARRAYSIZE(Text)) != 0);
            return winrt::to_string(winrt::hstring(Text));
        };
        Operations->EnumerateNetworks = [Data]()
        {
            std::lock_guard Lock(Data->Mutex);
            return Data->Networks;
        };
        Operations->CreateNetwork = [Data, NewId](winrt::hstring const& Name, bool Firewall,
            winrt::hstring const& CustomSettings)
        {
            auto Properties = Json::parse(winrt::to_string(BuildManagedNetworkSettings(Name, Firewall, CustomSettings)));
            auto FullName = winrt::to_hstring(Properties["Name"].get<std::string>());
            auto Type = winrt::to_hstring(Properties["Type"].get<std::string>());
            unsigned Flags = Properties["Flags"].get<unsigned>();
            std::lock_guard Lock(Data->Mutex);
            for (auto const& Network : Data->Networks)
                if (_wcsicmp(Network.Name.c_str(), FullName.c_str()) == 0)
                    throw winrt::hresult_error(E_FAIL, L"Network name already exists.");
            auto Id = NewId();
            Properties["Id"] = Id;
            Data->Networks.push_back({winrt::guid(Id), FullName, Type, Flags, winrt::to_hstring(Properties.dump())});
        };
        Operations->QueryNetwork = [Data](winrt::guid const& Id)
        {
            std::lock_guard Lock(Data->Mutex);
            for (auto const& Network : Data->Networks) if (Network.Id == Id) return Network;
            throw winrt::hresult_error(E_FAIL, L"Network does not exist.");
        };
        Operations->DeleteNetwork = [Data](winrt::guid const& Id)
        {
            std::lock_guard Lock(Data->Mutex);
            for (auto It = Data->Networks.begin(); It != Data->Networks.end(); ++It)
                if (It->Id == Id) { Data->Networks.erase(It); return; }
            throw winrt::hresult_error(E_FAIL, L"Network does not exist.");
        };
        Operations->Manage = [Data, NewId](winrt::hstring const& Command, winrt::hstring const& Arguments)
        {
            std::lock_guard Lock(Data->Mutex);
            auto A = Json::parse(winrt::to_string(Arguments));
            auto Action = winrt::to_string(Command);
            auto Text = [&](char const* Key) { return A.at(Key).get<std::string>(); };
            auto Find = [](Json& List, std::string const& Id)
            {
                for (auto It = List.begin(); It != List.end(); ++It)
                    if (It->at("Id") == Id) return It;
                throw winrt::hresult_error(E_FAIL, L"Mock resource does not exist.");
            };
            auto Name = [&]()
            {
                auto Value = Text("Name");
                if (Value.empty()) throw winrt::hresult_invalid_argument(L"Name is required.");
                return "NanaBox.Managed." + Value;
            };
            auto Result = Json{{"Success", true}};
            if (Action.starts_with("Hcn"))
            {
                auto Kind = Text("Kind");
                auto& Objects = Data->HcnObjects[Kind];
                if (Objects.is_null())
                    Objects = Json::array({Json{{"Id", NewId()}, {"Kind", Kind}, {"Name", "System resource [read-only]"},
                        {"Managed", false}, {"Properties", Json::object()}}});
                if (Action == "HcnList")
                    Result = {{"Items", Objects}};
                else if (Action == "HcnCreate")
                {
                    auto Id = NewId();
                    auto ResourceName = Kind == "Network" ? Text("Name") : Kind;
                    Objects.push_back({{"Id", Id}, {"Kind", Kind}, {"Name", ResourceName + " [managed]"},
                        {"Managed", true}, {"Properties", A.at("Settings")}});
                    Result = {{"Id", Id}, {"Kind", Kind}};
                }
                else
                {
                    auto Object = Find(Objects, Text("Id"));
                    if (Action == "HcnInspect") Result = *Object;
                    else
                    {
                        if (!Object->value("Managed", false)) throw winrt::hresult_error(E_ACCESSDENIED, L"Resource is read-only.");
                        if (Action == "HcnDelete") Objects.erase(Object);
                        else if (Action == "HcnModify" && Kind != "Route") (*Object)["Properties"] = A.at("Settings");
                        else throw winrt::hresult_invalid_argument();
                        Result = {{"Id", Text("Id")}, {"Kind", Kind}};
                    }
                }
            }
            else if (Action == "HcsInventory")
                Result = Json{{"Items", Json::array({Json{{"Id", "mock-hcs"}, {"SystemType", "VirtualMachine"}, {"State", "Running"}}})}};
            else if (Action == "HcsServiceProperties" || Action == "HcsSystemProperties")
            {
                if (!A.at("Query").is_object()) throw winrt::hresult_invalid_argument();
                if (Action == "HcsSystemProperties" && Text("Id") != "mock-hcs") throw winrt::hresult_invalid_argument();
                Result["Properties"] = Json{{"Query", A.at("Query")},
                    {"Id", Action == "HcsSystemProperties" ? Text("Id") : "mock-service"}};
            }
            else if (Action.starts_with("Vmms"))
            {
                std::string Selected;
                if (Action == "VmmsCreate" || Action == "VmmsImportRegister" || Action == "VmmsImportCopy")
                {
                    auto MachineName = Action == "VmmsCreate" ? Text("Name") : "Imported VM";
                    if (MachineName.empty()) throw winrt::hresult_invalid_argument();
                    Selected = NewId();
                    Data->Machines.push_back(Json{{"Id", Selected}, {"Name", MachineName}, {"State", "Off"},
                        {"Generation", A.value("Generation", 2)}, {"ProcessorCount", 1}, {"MemoryMiB", A.value("MemoryMiB", 2048)},
                        {"Path", A.value("Path", std::string())},
                        {"Devices", Json::array({Json{{"Id", "Service:Heartbeat"}, {"Name", "Heartbeat"}, {"Kind", "Service"}, {"Enabled", true}}})}});
                    Data->Checkpoints[Selected] = Json::array();
                }
                else if (Action != "VmmsList")
                {
                    Selected = Text("Id");
                    auto It = Find(Data->Machines, Selected);
                    auto& Machine = *It;
                    auto& Snapshots = Data->Checkpoints.at(Selected);
                    if (Action == "VmmsStart" || Action == "VmmsResume" || Action == "VmmsRestart") Machine["State"] = "Running";
                    else if (Action == "VmmsShutdown" || Action == "VmmsPowerOff") Machine["State"] = "Off";
                    else if (Action == "VmmsPause") Machine["State"] = "Paused";
                    else if (Action == "VmmsSave") Machine["State"] = "Saved";
                    else if (Action == "VmmsRename") Machine["Name"] = Text("Name");
                    else if (Action == "VmmsSetProcessor") Machine["ProcessorCount"] = A.at("Count");
                    else if (Action == "VmmsSetMemory" || Action == "VmmsStaticMemory")
                    {
                        Machine["MemoryMiB"] = A.at("MemoryMiB");
                        if (Action == "VmmsStaticMemory") Machine["DynamicMemoryEnabled"] = false;
                    }
                    else if (Action == "VmmsDynamicMemory")
                    {
                        if (A.at("MinimumMiB") > A.at("MemoryMiB") || A.at("MemoryMiB") > A.at("MaximumMiB"))
                            throw winrt::hresult_invalid_argument();
                        Machine["DynamicMemoryEnabled"] = true;
                        for (auto Key : {"MinimumMiB", "MemoryMiB", "MaximumMiB", "Buffer", "Priority"}) Machine[Key] = A.at(Key);
                    }
                    else if (Action == "VmmsMigrate")
                    {
                        if (Text("Host").empty()) throw winrt::hresult_invalid_argument();
                        Result["MigratedTo"] = Text("Host");
                        Result["MigratedId"] = Selected;
                        Data->Machines.erase(It);
                        Data->Checkpoints.erase(Selected);
                        Selected.clear();
                    }
                    else if (Action == "VmmsReplicationEnable")
                    {
                        if (Machine.contains("Replication") || Text("Host").empty()) throw winrt::hresult_invalid_argument();
                        Machine["Replication"] = A;
                        Machine["Replication"]["State"] = "ReadyForInitialReplication";
                    }
                    else if (Action == "VmmsReplicationStart" || Action == "VmmsReplicationSuspend" ||
                        Action == "VmmsReplicationResume" || Action == "VmmsReplicationRemove")
                    {
                        auto& Replica = Machine.at("Replication");
                        auto State = Replica.at("State").get<std::string>();
                        if (Action == "VmmsReplicationRemove") Machine.erase("Replication");
                        else if (Action == "VmmsReplicationStart" && State == "ReadyForInitialReplication") Replica["State"] = "Replicating";
                        else if (Action == "VmmsReplicationSuspend" && State == "Replicating") Replica["State"] = "Suspended";
                        else if (Action == "VmmsReplicationResume" && State == "Suspended") Replica["State"] = "Replicating";
                        else throw winrt::hresult_invalid_argument(L"Invalid replication state.");
                    }
                    else if (Action == "VmmsCpuPolicy") Machine["CpuPolicy"] = A;
                    else if (Action == "VmmsSecureBoot") Machine["SecureBoot"] = A;
                    else if (Action == "VmmsAutomaticActions")
                    {
                        Machine["AutomaticStartAction"] = A.at("StartAction");
                        Machine["AutomaticStartDelay"] = A.at("Delay");
                        Machine["AutomaticStopAction"] = A.at("StopAction");
                    }
                    else if (Action == "VmmsCheckpointPolicy")
                    {
                        Machine["CheckpointType"] = A.at("Type");
                        Machine["AutomaticCheckpointsEnabled"] = A.at("Enabled");
                    }
                    else if (Action == "VmmsDeleteSavedState")
                    {
                        if (Machine.at("State") != "Saved") throw winrt::hresult_invalid_argument();
                        Machine["State"] = "Off";
                    }
                    else if (Action == "VmmsLocalKeyProtector" || Action == "VmmsTpmEnable" || Action == "VmmsTpmDisable")
                    {
                        if (Machine.at("Generation") != 2 || Machine.at("State") != "Off")
                            throw winrt::hresult_invalid_argument();
                        bool HasProtector = Machine.value("HasKeyProtector", false);
                        if (Action == "VmmsLocalKeyProtector")
                        {
                            if (HasProtector) throw winrt::hresult_invalid_argument();
                            Machine["HasKeyProtector"] = true;
                        }
                        else
                        {
                            if (Action == "VmmsTpmEnable" && !HasProtector) throw winrt::hresult_invalid_argument();
                            Machine["TpmEnabled"] = Action == "VmmsTpmEnable";
                        }
                    }
                    else if (Action == "VmmsGpuAdd" || Action == "VmmsGpuRemove" || Action == "VmmsGpuQuotas")
                    {
                        if (Machine.at("State") != "Off") throw winrt::hresult_invalid_argument();
                        auto& Devices = Machine.at("Devices");
                        if (Action == "VmmsGpuAdd")
                        {
                            auto Source = Text("GpuId");
                            if (Source != "auto" && Source != "mock-gpu-path") throw winrt::hresult_invalid_argument();
                            Devices.push_back(Json{{"Id", "Gpu:" + NewId()}, {"Name", "GPU partition adapter"},
                                {"Kind", "Gpu"}, {"InstancePath", Source == "auto" ? "mock-gpu-path" : Source},
                                {"Quotas", A.at("Quotas")}});
                        }
                        else
                        {
                            auto Device = Find(Devices, Text("DeviceId"));
                            if (Device->at("Kind") != "Gpu") throw winrt::hresult_invalid_argument();
                            if (Action == "VmmsGpuRemove") Devices.erase(Device);
                            else
                            {
                                if (A.at("Quotas").empty()) throw winrt::hresult_invalid_argument();
                                (*Device)["Quotas"].update(A.at("Quotas"));
                            }
                        }
                    }
                    else if (Action == "VmmsDiskAttach" || Action == "VmmsDvdAdd" || Action == "VmmsNicAdd")
                    {
                        auto& Devices = Machine.at("Devices");
                        std::string Kind = Action == "VmmsDiskAttach" ? "Disk" : Action == "VmmsDvdAdd" ? "Dvd" : "Nic";
                        auto Id = Kind + ":" + NewId();
                        if (Kind == "Nic")
                        {
                            Devices.push_back(Json{{"Id", Id}, {"Name", Text("Name")}, {"Kind", Kind}, {"SwitchName", ""}});
                        }
                        else
                        {
                            auto Number = A.at("ControllerNumber").get<unsigned>();
                            auto Location = A.at("ControllerLocation").get<unsigned>();
                            auto Type = Kind == "Dvd" ? "SCSI" : Text("ControllerType");
                            for (auto const& Device : Devices)
                                if (Device.value("ControllerType", std::string()) == Type &&
                                    Device.value("ControllerNumber", 999u) == Number &&
                                    Device.value("ControllerLocation", 999u) == Location)
                                    throw winrt::hresult_invalid_argument(L"Controller slot is occupied.");
                            Devices.push_back(Json{{"Id", Id}, {"Name", Kind == "Disk" ? Text("Path") : "DVD"},
                                {"Kind", Kind}, {"Path", Text("Path")}, {"ControllerType", Type},
                                {"ControllerNumber", Number}, {"ControllerLocation", Location}});
                        }
                    }
                    else if (Action == "VmmsDiskDetach" || Action == "VmmsDiskQoS" || Action == "VmmsDvdMount" ||
                        Action == "VmmsDvdRemove" || Action == "VmmsNicRemove" || Action == "VmmsNicConnect" ||
                        Action == "VmmsNicDisconnect" || Action == "VmmsNicMac" || Action == "VmmsNicVlan" ||
                        Action == "VmmsNicGuard" || Action == "VmmsIntegration")
                    {
                        auto& Devices = Machine.at("Devices");
                        auto Device = Find(Devices, Text("DeviceId"));
                        auto Kind = Device->at("Kind").get<std::string>();
                        if ((Action.starts_with("VmmsDisk") && Kind != "Disk") ||
                            (Action.starts_with("VmmsDvd") && Kind != "Dvd") ||
                            (Action.starts_with("VmmsNic") && Kind != "Nic") ||
                            (Action == "VmmsIntegration" && Kind != "Service")) throw winrt::hresult_invalid_argument();
                        if (Action == "VmmsDiskDetach" || Action == "VmmsDvdRemove" || Action == "VmmsNicRemove")
                            Devices.erase(Device);
                        else if (Action == "VmmsDiskQoS")
                        {
                            (*Device)["MinimumIOPS"] = A.at("MinimumIOPS");
                            (*Device)["MaximumIOPS"] = A.at("MaximumIOPS");
                        }
                        else if (Action == "VmmsDvdMount") (*Device)["Path"] = Text("Path");
                        else if (Action == "VmmsNicConnect")
                        {
                            if (Text("SwitchId") != "8ef4654f-3c3e-46a3-a05d-002c3e702d72") throw winrt::hresult_invalid_argument();
                            (*Device)["SwitchName"] = "Test switch";
                        }
                        else if (Action == "VmmsNicDisconnect") (*Device)["SwitchName"] = "";
                        else if (Action == "VmmsNicMac") (*Device)["MacAddress"] = Text("Mac");
                        else if (Action == "VmmsNicVlan") (*Device)["Vlan"] = A.at("Vlan");
                        else if (Action == "VmmsNicGuard")
                            for (auto Key : {"DhcpGuard", "RouterGuard", "Spoofing"}) (*Device)[Key] = A.at(Key);
                        else if (Action == "VmmsIntegration") (*Device)["Enabled"] = A.at("Enabled");
                    }
                    else if (Action == "VmmsCheckpointCreate")
                        Snapshots.push_back(Json{{"Id", NewId()}, {"Name", Text("Name")}, {"MemoryMiB", Machine["MemoryMiB"]}});
                    else if (Action == "VmmsCheckpointRename") (*Find(Snapshots, Text("CheckpointId")))["Name"] = Text("Name");
                    else if (Action == "VmmsCheckpointExport")
                    {
                        (void)Find(Snapshots, Text("CheckpointId"));
                        Result["ExportedTo"] = Text("Path");
                    }
                    else if (Action == "VmmsCheckpointRestore") Machine["MemoryMiB"] = Find(Snapshots, Text("CheckpointId"))->at("MemoryMiB");
                    else if (Action == "VmmsCheckpointRemove") Snapshots.erase(Find(Snapshots, Text("CheckpointId")));
                    else if (Action == "VmmsMoveStorage") Machine["Path"] = Text("Path");
                    else if (Action == "VmmsExport") Result["ExportedTo"] = Text("Path");
                    else if (Action == "VmmsRemove")
                    {
                        if (Machine["State"] != "Off") throw winrt::hresult_error(E_FAIL, L"VM must be stopped.");
                        Data->Machines.erase(It);
                        Data->Checkpoints.erase(Selected);
                        Selected.clear();
                    }
                    else if (Action != "VmmsInspect") throw winrt::hresult_invalid_argument();
                }
                Result["Items"] = Data->Machines;
                Result["SelectedId"] = Selected.empty() ? Json() : Json(Selected);
                Result["Details"] = Selected.empty() ? Json() : Json{{"VM", *Find(Data->Machines, Selected)},
                    {"Checkpoints", Data->Checkpoints.at(Selected)}, {"Devices", Find(Data->Machines, Selected)->at("Devices")},
                    {"Switches", Json::array({Json{{"Id", "8ef4654f-3c3e-46a3-a05d-002c3e702d72"}, {"Name", "Test switch"}}})},
                    {"PartitionableGpus", Json::array({Json{{"Id", "auto"}, {"Name", "Automatic GPU selection"}},
                        Json{{"Id", "mock-gpu-path"}, {"Name", "Mock partitionable GPU"}}})}};
            }
            else if (Action == "SwitchList") Result = Data->Switches;
            else if (Action == "AdapterList")
                Result = Json::array({Json{{"Name", "Mock Ethernet"}, {"Status", "Up"}}});
            else if (Action == "CreatorList") Result = Data->Creators;
            else if (Action == "SwitchCreate")
            {
                auto FullName = Name();
                for (auto const& Entry : Data->Switches)
                    if (Entry.at("Name") == FullName)
                        throw winrt::hresult_error(E_FAIL, L"Switch name already exists.");
                auto Type = Text("Type");
                if (Type != "Private" && Type != "Internal" && Type != "External") throw winrt::hresult_invalid_argument();
                if (Type == "External" && Text("Adapter") != "Mock Ethernet") throw winrt::hresult_invalid_argument();
                Data->Switches.push_back(Json{{"Id", NewId()}, {"Name", FullName}, {"Type", Type}});
                Result["Name"] = FullName;
            }
            else if (Action == "SwitchDelete") Data->Switches.erase(Find(Data->Switches, Text("Id")));
            else if (Action == "CreatorRegister")
            {
                auto Id = NewId();
                Result = Json{{"Id", Id}, {"Name", Name()}};
                Data->Creators.push_back(Result);
                Data->Firewall.emplace(Id, Json{{"Settings", Json::array({Json{
                    {"Name", Id}, {"DefaultInboundAction", "Block"}, {"DefaultOutboundAction", "Allow"}}})},
                    {"Rules", Json::array()}});
            }
            else
            {
                auto Id = Text("Id");
                Find(Data->Creators, Id);
                auto& Firewall = Data->Firewall.at(Id);
                auto& Rules = Firewall.at("Rules");
                if (Action == "FirewallInspect") Result = Firewall;
                else if (Action == "FirewallConfigure")
                {
                    Firewall["Settings"][0]["DefaultInboundAction"] = Text("Inbound");
                    Firewall["Settings"][0]["DefaultOutboundAction"] = Text("Outbound");
                    Firewall["Settings"][0]["LoopbackEnabled"] = A.at("Loopback");
                    Firewall["Settings"][0]["AllowHostPolicyMerge"] = A.at("Merge");
                }
                else if (Action == "RuleCreate")
                {
                    auto Ports = Text("Ports");
                    if (Ports != "18080") throw winrt::hresult_invalid_argument(L"Expected port 18080.");
                    auto Direction = Text("Direction");
                    Json Rule{{"Id", "NanaBox.Managed." + NewId()}, {"Name", Text("Name")},
                        {"Direction", Direction}, {"Action", Text("RuleAction")}, {"Protocol", Text("Protocol")},
                        {"Enabled", "True"}, {"LocalPorts", Json::array()}, {"RemotePorts", Json::array()}};
                    Rule[Direction == "Inbound" ? "LocalPorts" : "RemotePorts"].push_back(Ports);
                    Rules.push_back(std::move(Rule));
                }
                else if (Action == "RuleEnable") (*Find(Rules, Text("Rule")))["Enabled"] = "True";
                else if (Action == "RuleDisable") (*Find(Rules, Text("Rule")))["Enabled"] = "False";
                else if (Action == "RuleDelete") Rules.erase(Find(Rules, Text("Rule")));
                else throw winrt::hresult_invalid_argument();
            }
            return winrt::to_hstring(Result.dump());
        };
        return Operations;
    }
}
