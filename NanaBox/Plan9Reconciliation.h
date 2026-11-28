#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace NanaBox
{
    // Keep the actual resource configuration after partial HCS failures.
    // Prepare is responsible for detaching guest mounts before HCS removal.
    template<typename Share, typename SameName, typename SameProvider,
        typename Prepare, typename Remove, typename Add, typename Report>
    std::vector<Share> ReconcilePlan9Shares(
        std::vector<Share> const& Current, std::vector<Share> const& Desired,
        SameName&& NameEquals, SameProvider&& ProviderEquals,
        Prepare&& PrepareRemoval, Remove&& RemoveShare, Add&& AddShare, Report&& ReportError)
    {
        std::vector<Share> Actual;
        Actual.reserve(Current.size() + Desired.size());
        auto Find = [&](std::vector<Share> const& Items, Share const& ShareToFind)
        {
            return std::find_if(Items.begin(), Items.end(),
                [&](auto const& Item) { return NameEquals(Item, ShareToFind); });
        };

        for (auto const& Old : Current)
        {
            auto Replacement = Find(Desired, Old);
            if (Replacement != Desired.end() && ProviderEquals(Old, *Replacement))
            {
                Actual.push_back(*Replacement);
                continue;
            }
            try
            {
                PrepareRemoval(Old);
                RemoveShare(Old);
            }
            catch (...)
            {
                ReportError(Old, "Remove");
                Actual.push_back(Old);
                continue;
            }
            if (Replacement == Desired.end()) continue;
            try
            {
                AddShare(*Replacement);
                Actual.push_back(*Replacement);
            }
            catch (...)
            {
                ReportError(*Replacement, "Replace");
                try
                {
                    AddShare(Old);
                    Actual.push_back(Old);
                }
                catch (...)
                {
                    ReportError(Old, "Restore");
                }
            }
        }
        for (auto const& New : Desired)
        {
            if (Find(Current, New) != Current.end()) continue;
            try
            {
                AddShare(New);
                Actual.push_back(New);
            }
            catch (...)
            {
                ReportError(New, "Add");
            }
        }
        return Actual;
    }

    // Existing VSMB exports can have open handles and mapped pages in the guest.
    // Retain them until shutdown; additions and guest mount-policy changes are independent.
    template<typename Share, typename SameName, typename SameProvider,
        typename Add, typename Report>
    std::vector<Share> ReconcileVirtualSmbAdditions(
        std::vector<Share> const& Current, std::vector<Share> const& Desired,
        SameName&& NameEquals, SameProvider&& ProviderEquals, Add&& AddShare, Report&& ReportError,
        std::size_t Maximum = 128)
    {
        std::vector<Share> Actual;
        Actual.reserve(Current.size() + Desired.size());
        auto Find = [&](std::vector<Share> const& Items, Share const& Value)
        {
            return std::find_if(Items.begin(), Items.end(),
                [&](auto const& Item) { return NameEquals(Item, Value); });
        };
        for (auto const& Old : Current)
        {
            auto Replacement = Find(Desired, Old);
            if (Replacement == Desired.end())
            {
                ReportError(Old, "Remove");
                Actual.push_back(Old);
            }
            else if (!ProviderEquals(Old, *Replacement))
            {
                ReportError(Old, "Replace");
                Actual.push_back(Old);
            }
            else
                Actual.push_back(*Replacement);
        }
        for (auto const& New : Desired)
        {
            if (Find(Current, New) != Current.end()) continue;
            if (Actual.size() >= Maximum)
            {
                ReportError(New, "Limit");
                continue;
            }
            try
            {
                AddShare(New);
                Actual.push_back(New);
            }
            catch (...)
            {
                ReportError(New, "Add");
            }
        }
        return Actual;
    }
}
