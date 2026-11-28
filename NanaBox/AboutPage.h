#pragma once

#include "AboutPage.g.h"

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
    struct AboutPage : AboutPageT<AboutPage>
    {
    public:

        AboutPage(
            _In_ HWND WindowHandle = nullptr,
            std::function<void(std::wstring const&)> ExternalAction = {});

        void InitializeComponent();

        void GitHubButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void CancelButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

    private:

        HWND m_WindowHandle;
        std::function<void(std::wstring const&)> m_ExternalAction;
    };
}

namespace winrt::NanaBox::factory_implementation
{
    struct AboutPage :
        AboutPageT<AboutPage, implementation::AboutPage>
    {
    };
}
