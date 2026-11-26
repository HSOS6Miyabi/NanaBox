#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace NanaBox
{
    template<typename Adapter>
    bool SameNetworkAdapter(Adapter const& A, Adapter const& B)
    {
        return A.Connected == B.Connected &&
            A.MacAddress == B.MacAddress && A.NetworkId == B.NetworkId &&
            A.FirewallCreatorId == B.FirewallCreatorId &&
            A.SuggestedSwitchId == B.SuggestedSwitchId &&
            A.SuggestedSwitchName == B.SuggestedSwitchName &&
            A.SuggestedSwitchSubnet == B.SuggestedSwitchSubnet;
    }

    // Remove must set Connected=false only after HCS has detached the device.
    // Add must set Connected=true only after both endpoint creation and HCS attachment.
    // The result records the actual state even when one operation fails.
    template<typename Adapter, typename Remove, typename Add, typename OnError>
    std::vector<Adapter> ReconcileNetworkAdapters(
        std::vector<Adapter> const& Previous, std::vector<Adapter> const& Desired,
        Remove remove, Add add, OnError error)
    {
        std::map<std::string, Adapter> Old, New;
        for (auto const& Item : Previous)
            if (Item.EndpointId.empty() || !Old.emplace(Item.EndpointId, Item).second)
                throw std::invalid_argument("Invalid previous endpoint ID");
        for (auto const& Item : Desired)
            if (Item.EndpointId.empty() || !New.emplace(Item.EndpointId, Item).second)
                throw std::invalid_argument("Invalid desired endpoint ID");

        std::set<std::string> Failed;
        for (auto& Entry : Old)
        {
            auto It = New.find(Entry.first);
            if (It != New.end() && SameNetworkAdapter(Entry.second, It->second))
                continue;
            try { remove(Entry.second); }
            catch (...)
            {
                Failed.insert(Entry.first);
                error(Entry.second, "Remove");
            }
        }

        std::vector<Adapter> Result;
        for (auto Item : Desired)
        {
            auto It = Old.find(Item.EndpointId);
            if (Failed.count(Item.EndpointId) ||
                (It != Old.end() && SameNetworkAdapter(It->second, Item)))
            {
                Result.push_back(It->second);
                continue;
            }
            bool Connect = Item.Connected;
            Item.Connected = false;
            if (Connect)
            {
                try { add(Item); }
                catch (...) { error(Item, "Add"); }
            }
            Result.push_back(Item);
        }
        for (auto const& Entry : Old)
            if (!New.count(Entry.first) && Failed.count(Entry.first))
                Result.push_back(Entry.second);
        return Result;
    }
}
