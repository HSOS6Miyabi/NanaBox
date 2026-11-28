#pragma once

#include "SponsorPage.g.h"

#include <Windows.h>
#include <functional>
#include <string>

namespace winrt
{
    using Windows::Foundation::IInspectable;
    using Windows::UI::Xaml::RoutedEventArgs;
}

namespace winrt::NanaBox::implementation
{
    struct SponsorPage : SponsorPageT<SponsorPage>
    {
    public:

        SponsorPage(
            _In_ HWND WindowHandle = nullptr,
            std::function<void(std::wstring const&)> ExternalAction = {});

        void InitializeComponent();

        void ContributeButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        winrt::fire_and_forget BuySponsorEditionButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void SponsorEditionPolicyButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

    private:

        HWND m_WindowHandle;
        std::function<void(std::wstring const&)> m_ExternalAction;
    };
}

namespace winrt::NanaBox::factory_implementation
{
    struct SponsorPage :
        SponsorPageT<SponsorPage, implementation::SponsorPage>
    {
    };
}
