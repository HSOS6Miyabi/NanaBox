#include "pch.h"
#include "AboutPage.h"
#if __has_include("AboutPage.g.cpp")
#include "AboutPage.g.cpp"
#endif

#include <Windows.h>

#include <Mile.Project.Version.h>

#include <winrt/Windows.UI.Xaml.Documents.h>

#include "Utils.h"

namespace winrt::NanaBox::implementation
{
    AboutPage::AboutPage(
        _In_ HWND WindowHandle,
        std::function<void(std::wstring const&)> ExternalAction) :
        m_WindowHandle(WindowHandle), m_ExternalAction(std::move(ExternalAction))
    {
        ::SetWindowTextW(
            this->m_WindowHandle,
            Mile::WinRT::GetLocalizedString(
                L"AboutPage/GridTitleTextBlock/Text").c_str());
    }

    void AboutPage::InitializeComponent()
    {
        AboutPageT::InitializeComponent();

        this->Version().Text(winrt::hstring(
            L"NanaBox " MILE_PROJECT_VERSION_STRING
            L" (" MILE_PROJECT_DOT_VERSION_STRING L")"));
    }

    void AboutPage::GitHubButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        constexpr wchar_t Address[] = L"https://github.com/M2Team/NanaBox";
        if (this->m_ExternalAction)
        {
            this->m_ExternalAction(Address);
            return;
        }
        SHELLEXECUTEINFOW ExecInfo = {};
        ExecInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
        ExecInfo.lpVerb = L"open";
        ExecInfo.lpFile = Address;
        ExecInfo.nShow = SW_SHOWNORMAL;
        ::ShellExecuteExW(&ExecInfo);
    }

    void AboutPage::CancelButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
    }
}
