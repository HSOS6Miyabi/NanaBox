#pragma once
#include "VirtualMachineSettingsPage.h"
#include <Mile.Json.h>
#include <map>
#include <mutex>

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
            std::map<std::string, Json> Firewall;
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
        Operations->CreateNetwork = [Data, NewId](winrt::hstring const& Name, bool Firewall)
        {
            std::lock_guard Lock(Data->Mutex);
            if (Name.empty()) throw winrt::hresult_invalid_argument(L"Network name is required.");
            auto FullName = winrt::to_hstring("NanaBox.Managed." + winrt::to_string(Name));
            for (auto const& Network : Data->Networks)
                if (Network.Name == FullName) throw winrt::hresult_error(E_FAIL, L"Network name already exists.");
            auto Id = NewId();
            unsigned Flags = HcnNetworkFlags::EnableDns | HcnNetworkFlags::EnableDhcp |
                HcnNetworkFlags::EnablePersistent | (Firewall ? HcnNetworkFlags::EnableFirewall : 0);
            auto Properties = Json{{"Id", Id}, {"Name", winrt::to_string(FullName)}, {"Type", "ICS"}, {"Flags", Flags}};
            Data->Networks.push_back({winrt::guid(Id), FullName, L"ICS", Flags, winrt::to_hstring(Properties.dump())});
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
            if (Action == "SwitchList") Result = Data->Switches;
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
