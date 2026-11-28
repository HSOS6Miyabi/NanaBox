#include "pch.h"
#include "QuickStartPage.h"
#if __has_include("QuickStartPage.g.cpp")
#include "QuickStartPage.g.cpp"
#endif

#include "Utils.h"

#include "NanaBoxResources.h"

#include <ShlObj.h>

#include "ConfigurationManager.h"
#include "WorkspaceHistory.h"
#include <filesystem>
#include <shellapi.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <chrono>
#include <winrt/Windows.UI.Text.h>
#include <string_view>

namespace
{
    struct WelcomeString { wchar_t const* Key; wchar_t const* English; wchar_t const* Chinese; };
    constexpr WelcomeString WelcomeStrings[] = {
        {L"WorkspaceTitle", L"NanaBox \u2014 Virtual machines", L"NanaBox \u2014 \u865a\u62df\u673a"},
        {L"WorkspaceHeading", L"Your virtual machines", L"\u6211\u7684\u865a\u62df\u673a"},
        {L"WorkspaceSubtitle", L"Create, open and manage your virtual machine workspaces.", L"\u521b\u5efa\u3001\u6253\u5f00\u548c\u7ba1\u7406\u865a\u62df\u673a\u5de5\u4f5c\u533a\u3002"},
        {L"WorkspaceActions", L"Get started", L"\u5feb\u901f\u5f00\u59cb"},
        {L"WorkspaceTools", L"Virtual disk tools", L"\u865a\u62df\u78c1\u76d8\u5de5\u5177"},
        {L"WorkspaceOpen", L"Open and start", L"\u6253\u5f00\u5e76\u542f\u52a8"},
        {L"WorkspaceNew", L"Create virtual machine", L"\u521b\u5efa\u865a\u62df\u673a"},
        {L"WorkspaceAdd", L"Add to workspace", L"\u6dfb\u52a0\u5230\u5de5\u4f5c\u533a"},
        {L"WorkspaceBrowseSettings", L"Open settings file", L"\u6253\u5f00\u865a\u62df\u673a\u8bbe\u7f6e"},
        {L"WorkspaceCreateDisk", L"Create virtual disk", L"\u521b\u5efa\u865a\u62df\u78c1\u76d8"},
        {L"WorkspaceResizeDisk", L"Resize virtual disk", L"\u8c03\u6574\u78c1\u76d8\u5bb9\u91cf"},
        {L"WorkspaceCompactDisk", L"Compact virtual disk", L"\u538b\u7f29\u865a\u62df\u78c1\u76d8"},
        {L"WorkspaceAbout", L"About NanaBox", L"\u5173\u4e8e NanaBox"},
        {L"WorkspaceRecent", L"Recent workspaces", L"\u6700\u8fd1\u7684\u865a\u62df\u673a\u5de5\u4f5c\u533a"},
        {L"WorkspaceSearch", L"Search by name or path", L"\u6309\u540d\u79f0\u6216\u8def\u5f84\u641c\u7d22"},
        {L"WorkspaceRefresh", L"Refresh", L"\u5237\u65b0"},
        {L"WorkspaceClear", L"Clear history", L"\u6e05\u9664\u5386\u53f2\u8bb0\u5f55"},
        {L"WorkspaceStart", L"Start", L"\u542f\u52a8"},
        {L"WorkspaceSettings", L"Settings", L"\u8bbe\u7f6e"},
        {L"WorkspaceFolder", L"Open folder", L"\u6253\u5f00\u6240\u5728\u6587\u4ef6\u5939"},
        {L"WorkspaceRemove", L"Remove from list", L"\u4ece\u5217\u8868\u79fb\u9664"},
        {L"WorkspaceEmpty", L"No recent virtual machines. Create a new virtual machine or add an existing configuration to get started.", L"\u6682\u65e0\u6700\u8fd1\u4f7f\u7528\u7684\u865a\u62df\u673a\u3002\u53ef\u4ee5\u521b\u5efa\u865a\u62df\u673a\uff0c\u6216\u6dfb\u52a0\u5df2\u6709\u7684\u865a\u62df\u673a\u914d\u7f6e\u3002"},
        {L"WorkspaceNoMatches", L"No virtual machines match your search.", L"\u6ca1\u6709\u7b26\u5408\u641c\u7d22\u6761\u4ef6\u7684\u865a\u62df\u673a\u3002"},
        {L"WorkspaceLoading", L"Loading recent workspaces\u2026", L"\u6b63\u5728\u52a0\u8f7d\u6700\u8fd1\u7684\u5de5\u4f5c\u533a\u2026"},
        {L"WorkspaceReady", L"Select a virtual machine to start it or change its settings.", L"\u9009\u62e9\u4e00\u53f0\u865a\u62df\u673a\uff0c\u5373\u53ef\u542f\u52a8\u6216\u4fee\u6539\u8bbe\u7f6e\u3002"},
        {L"WorkspaceMissing", L"Configuration file unavailable", L"\u914d\u7f6e\u6587\u4ef6\u4e0d\u53ef\u7528"},
        {L"WorkspaceInvalid", L"Configuration could not be read", L"\u65e0\u6cd5\u8bfb\u53d6\u914d\u7f6e"},
        {L"WorkspaceUnknown", L"Other operating system", L"\u5176\u4ed6\u64cd\u4f5c\u7cfb\u7edf"},
        {L"WorkspaceCpu", L"vCPU", L"\u4e2a\u865a\u62df\u5904\u7406\u5668"},
        {L"WorkspaceMemory", L"MiB memory", L"MiB \u5185\u5b58"},
        {L"WorkspaceClearConfirm", L"Clear the list of recent virtual machines? Your virtual machine files will be kept.", L"\u6e05\u9664\u6700\u8fd1\u4f7f\u7528\u7684\u865a\u62df\u673a\u5217\u8868\uff1f\u865a\u62df\u673a\u6587\u4ef6\u5c06\u4e88\u4ee5\u4fdd\u7559\u3002"},
        {L"WorkspaceRemoved", L"Removed from the list. The virtual machine files are unchanged.", L"\u5df2\u4ece\u5217\u8868\u79fb\u9664\uff0c\u865a\u62df\u673a\u6587\u4ef6\u672a\u53d7\u5f71\u54cd\u3002"},
        {L"WorkspaceCleared", L"Recent workspace history cleared.", L"\u5df2\u6e05\u9664\u6700\u8fd1\u7684\u5de5\u4f5c\u533a\u5386\u53f2\u8bb0\u5f55\u3002"},
        {L"WorkspaceFileType", L"NanaBox virtual machine configuration", L"NanaBox \u865a\u62df\u673a\u914d\u7f6e"},
        {L"WorkspaceCannotRead", L"The selected virtual machine configuration could not be read. Verify the file location and try again.", L"\u65e0\u6cd5\u8bfb\u53d6\u6240\u9009\u865a\u62df\u673a\u914d\u7f6e\uff0c\u8bf7\u68c0\u67e5\u6587\u4ef6\u4f4d\u7f6e\u540e\u91cd\u8bd5\u3002"},
        {L"WorkspaceHistoryError", L"The recent workspace list could not be updated.", L"\u65e0\u6cd5\u66f4\u65b0\u6700\u8fd1\u7684\u5de5\u4f5c\u533a\u5217\u8868\u3002"},
    };

    winrt::hstring WelcomeText(std::wstring_view Key)
    {
        bool Chinese = PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
        for (auto const& Entry : WelcomeStrings)
            if (Key == Entry.Key)
            {
                auto Fallback = Chinese ? Entry.Chinese : Entry.English;
                auto Resource = std::wstring(L"QuickStartPage/") + Entry.Key;
                auto Text = Mile::WinRT::GetLocalizedString(winrt::hstring(Resource), Fallback);
                return Text.empty() || std::wstring_view(Text.c_str()) == Resource ? winrt::hstring(Fallback) : Text;
            }
        return {};
    }

    struct WelcomeBinding { wchar_t const* Name; wchar_t const* Key; wchar_t const* Property; };
    constexpr WelcomeBinding WelcomeBindings[] = {
        {L"QuickStartCreateVirtualMachineButtonText", L"WorkspaceNew", L"Text"},
        {L"QuickStartCreateVirtualMachineButton", L"WorkspaceNew", L"Name"},
        {L"QuickStartLaunchVirtualMachineButtonText", L"WorkspaceOpen", L"Text"},
        {L"QuickStartLaunchVirtualMachineButton", L"WorkspaceOpen", L"Name"},
        {L"QuickStartAddWorkspaceButtonText", L"WorkspaceAdd", L"Text"},
        {L"QuickStartAddWorkspaceButton", L"WorkspaceAdd", L"Name"},
        {L"QuickStartVirtualMachineSettingsButtonText", L"WorkspaceBrowseSettings", L"Text"},
        {L"QuickStartVirtualMachineSettingsButton", L"WorkspaceBrowseSettings", L"Name"},
        {L"ToolsLabel", L"WorkspaceTools", L"Text"},
        {L"QuickStartCreateVirtualHardDiskButtonText", L"WorkspaceCreateDisk", L"Text"},
        {L"QuickStartCreateVirtualHardDiskButton", L"WorkspaceCreateDisk", L"Name"},
        {L"QuickStartResizeVirtualHardDiskButtonText", L"WorkspaceResizeDisk", L"Text"},
        {L"QuickStartResizeVirtualHardDiskButton", L"WorkspaceResizeDisk", L"Name"},
        {L"QuickStartCompactVirtualHardDiskButtonText", L"WorkspaceCompactDisk", L"Text"},
        {L"QuickStartCompactVirtualHardDiskButton", L"WorkspaceCompactDisk", L"Name"},
        {L"QuickStartAboutButtonText", L"WorkspaceAbout", L"Text"},
        {L"QuickStartAboutButton", L"WorkspaceAbout", L"Name"},
        {L"WorkspaceStartButtonText", L"WorkspaceStart", L"Text"},
        {L"WorkspaceStartButton", L"WorkspaceStart", L"Name"},
        {L"WorkspaceSettingsButtonText", L"WorkspaceSettings", L"Text"},
        {L"WorkspaceSettingsButton", L"WorkspaceSettings", L"Name"},
        {L"WorkspaceFolderButtonText", L"WorkspaceFolder", L"Text"},
        {L"WorkspaceFolderButton", L"WorkspaceFolder", L"Name"},
        {L"WorkspaceRemoveButtonText", L"WorkspaceRemove", L"Text"},
        {L"WorkspaceRemoveButton", L"WorkspaceRemove", L"Name"},
        {L"WorkspaceSearchBox", L"WorkspaceSearch", L"Placeholder"},
        {L"WelcomeHeading", L"WorkspaceHeading", L"Text"},
        {L"WelcomeSubtitle", L"WorkspaceSubtitle", L"Text"},
        {L"ActionsLabel", L"WorkspaceActions", L"Text"},
        {L"RecentLabel", L"WorkspaceRecent", L"Text"},
        {L"WorkspaceRefreshButton", L"WorkspaceRefresh", L"Name"},
        {L"WorkspaceClearButton", L"WorkspaceClear", L"Content"},
    };

    std::wstring Lowercase(std::wstring Text)
    {
        if (!Text.empty()) ::CharLowerBuffW(Text.data(), static_cast<DWORD>(Text.size()));
        return Text;
    }
}



namespace winrt::NanaBox
{
    using namespace ::NanaBox;
}

namespace winrt::NanaBox::implementation
{
    QuickStartPage::QuickStartPage(
        _In_ HWND WindowHandle,
        _Out_opt_ std::wstring* ConfigurationFilePath,
        std::wstring const& HistoryKey) :
        m_HistoryKey(HistoryKey.empty() ? ::NanaBox::WorkspaceHistory::DefaultKey : HistoryKey),
        m_WindowHandle(WindowHandle),
        m_ConfigurationFilePath(ConfigurationFilePath)
    {
        ::SetWindowTextW(
            this->m_WindowHandle,
            ::WelcomeText(L"WorkspaceTitle").c_str());

        HICON ApplicationIconHandle = reinterpret_cast<HICON>(::LoadImageW(
            ::GetModuleHandleW(nullptr),
            MAKEINTRESOURCE(IDI_NANABOX),
            IMAGE_ICON,
            256,
            256,
            LR_SHARED));
        if (ApplicationIconHandle)
        {
            ::SendMessageW(
                this->m_WindowHandle,
                WM_SETICON,
                TRUE,
                reinterpret_cast<LPARAM>(ApplicationIconHandle));
            ::SendMessageW(
                this->m_WindowHandle,
                WM_SETICON,
                FALSE,
                reinterpret_cast<LPARAM>(ApplicationIconHandle));
        }
    }

    void QuickStartPage::InitializeComponent()
    {
        QuickStartPageT::InitializeComponent();
        using namespace winrt::Windows::UI::Xaml::Controls;
        using Automation = winrt::Windows::UI::Xaml::Automation::AutomationProperties;
        for (auto const& Binding : WelcomeBindings)
        {
            auto Control = this->FindName(Binding.Name);
            auto Text = ::WelcomeText(Binding.Key);
            std::wstring_view Property(Binding.Property);
            if (Property == L"Text")
            {
                if (auto Block = Control.try_as<TextBlock>()) Block.Text(Text);
            }
            else if (Property == L"Name")
            {
                if (auto Element = Control.try_as<winrt::Windows::UI::Xaml::DependencyObject>())
                {
                    Automation::SetName(Element, Text);
                    ToolTipService::SetToolTip(Element, winrt::box_value(Text));
                }
            }
            else if (Property == L"Content")
            {
                if (auto ButtonControl = Control.try_as<ContentControl>()) ButtonControl.Content(winrt::box_value(Text));
            }
            else if (Property == L"Placeholder")
            {
                if (auto Box = Control.try_as<TextBox>())
                {
                    Box.PlaceholderText(Text);
                    Automation::SetName(Box, Text);
                }
            }
        }
        this->SizeChanged([Weak = this->get_weak()](winrt::IInspectable const&, winrt::Windows::UI::Xaml::SizeChangedEventArgs const& Args)
        {
            if (auto Page = Weak.get())
                Page->WorkspaceSidebarColumn().Width(winrt::Windows::UI::Xaml::GridLengthHelper::FromPixels(
                    Args.NewSize().Width < 720 ? 174 : 220));
        });
        this->WorkspaceStatusText().Text(::WelcomeText(L"WorkspaceLoading"));
        this->WorkspaceEmptyText().Text(::WelcomeText(L"WorkspaceLoading"));
        this->UpdateWorkspaceActions();
        this->m_Timer = winrt::Windows::UI::Xaml::DispatcherTimer();
        this->m_Timer.Interval(std::chrono::milliseconds(750));
        this->m_Timer.Tick([Weak = this->get_weak()](winrt::IInspectable const&, winrt::IInspectable const&)
        {
            if (auto Page = Weak.get())
            {
                if (!::IsWindow(Page->m_WindowHandle))
                {
                    Page->m_Timer.Stop();
                    return;
                }
                if (Page->m_SettingsThread && ::WaitForSingleObject(Page->m_SettingsThread.get(), 0) == WAIT_OBJECT_0)
                {
                    Page->m_SettingsThread.close();
                    Page->RefreshWorkspaces(true);
                }
                else if (::IsWindowEnabled(Page->m_WindowHandle))
                    Page->RefreshWorkspaces(false);
            }
        });
        this->m_Timer.Start();
        this->RefreshWorkspaces(true);
    }

    QuickStartPage::~QuickStartPage()
    {
        try { if (this->m_Timer) this->m_Timer.Stop(); } catch (...) {}
    }

    winrt::fire_and_forget QuickStartPage::RefreshWorkspaces(bool Force)
    {
        if (this->m_Refreshing)
        {
            this->m_RefreshPending = this->m_RefreshPending || Force;
            co_return;
        }
        auto Lifetime = this->get_strong();
        winrt::apartment_context Foreground;
        std::vector<std::wstring> Paths;
        try { Paths = ::NanaBox::WorkspaceHistory::Read(this->m_HistoryKey); }
        catch (...)
        {
            this->WorkspaceStatusText().Text(::WelcomeText(L"WorkspaceHistoryError"));
            co_return;
        }
        if (!Force && Paths == this->m_HistoryPaths) co_return;
        this->m_Refreshing = true;
        this->m_HistoryPaths = Paths;
        std::vector<Workspace> Records;
        bool Failed = false;
        try
        {
            co_await winrt::resume_background();
            for (auto const& Path : Paths)
            {
                Workspace Entry;
                Entry.Path = Path;
                Entry.Name = std::filesystem::path(Path).stem().wstring();
                if (Entry.Name.empty()) Entry.Name = Path;
                WIN32_FILE_ATTRIBUTE_DATA Attributes{};
                if (::GetFileAttributesExW(Path.c_str(), GetFileExInfoStandard, &Attributes) &&
                    !(Attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    Entry.Exists = true;
                    try
                    {
                        ULARGE_INTEGER Size{};
                        Size.HighPart = Attributes.nFileSizeHigh;
                        Size.LowPart = Attributes.nFileSizeLow;
                        if (Size.QuadPart > 64ull * 1024 * 1024) throw winrt::hresult_invalid_argument();
                        auto Configuration = ::NanaBox::DeserializeConfiguration(::ReadAllTextFromUtf8TextFile(Path));
                        if (!Configuration.Name.empty())
                            Entry.Name = winrt::to_hstring(Configuration.Name).c_str();
                        Entry.Guest = Configuration.GuestType == ::NanaBox::GuestType::Windows ? L"Windows" :
                            Configuration.GuestType == ::NanaBox::GuestType::Linux ? L"Linux" : L"";
                        Entry.Processors = Configuration.ProcessorCount;
                        Entry.Memory = Configuration.MemorySize;
                        Entry.Valid = true;
                    }
                    catch (...) {}
                }
                Records.push_back(std::move(Entry));
            }
        }
        catch (...) { Failed = true; }
        try
        {
            co_await Foreground;
            if (!::IsWindow(this->m_WindowHandle)) co_return;
            this->m_Refreshing = false;
            if (Failed)
            {
                this->m_HistoryPaths.clear();
                this->WorkspaceStatusText().Text(::WelcomeText(L"WorkspaceHistoryError"));
                co_return;
            }
            this->m_Workspaces = std::move(Records);
            this->m_Refreshing = false;
            this->RenderWorkspaces();
            this->WorkspaceStatusText().Text(::WelcomeText(L"WorkspaceReady"));
            if (this->m_RefreshPending)
            {
                this->m_RefreshPending = false;
                this->RefreshWorkspaces(true);
            }
        }
        catch (...)
        {
            // The window may have closed while the refresh was in progress.
        }
    }

    std::wstring QuickStartPage::SelectedWorkspacePath()
    {
        auto List = this->WorkspaceList();
        if (!List) return {};
        auto Item = List.SelectedItem().try_as<winrt::Windows::UI::Xaml::Controls::ListViewItem>();
        return Item ? std::wstring(winrt::unbox_value<winrt::hstring>(Item.Tag()).c_str()) : std::wstring();
    }

    void QuickStartPage::RenderWorkspaces()
    {
        using namespace winrt::Windows::UI::Xaml::Controls;
        using Automation = winrt::Windows::UI::Xaml::Automation::AutomationProperties;
        if (!this->WorkspaceList() || !this->WorkspaceSearchBox() || !this->WorkspaceEmptyText()) return;
        auto Previous = this->SelectedWorkspacePath();
        auto Search = ::Lowercase(this->WorkspaceSearchBox().Text().c_str());
        auto List = this->WorkspaceList();
        List.Items().Clear();
        int SelectedIndex = -1;
        for (auto const& Entry : this->m_Workspaces)
        {
            if (!Search.empty() && ::Lowercase(Entry.Name).find(Search) == std::wstring::npos &&
                ::Lowercase(Entry.Path).find(Search) == std::wstring::npos &&
                ::Lowercase(Entry.Guest).find(Search) == std::wstring::npos) continue;
            auto Index = List.Items().Size();
            ListViewItem Item;
            Item.Tag(winrt::box_value(winrt::hstring(Entry.Path)));
            Automation::SetAutomationId(Item, winrt::hstring(L"RecentWorkspace" + std::to_wstring(Index)));
            Automation::SetName(Item, winrt::hstring(Entry.Name + L" — " + Entry.Path));
            ToolTipService::SetToolTip(Item, winrt::box_value(winrt::hstring(Entry.Path)));
            Grid Row;
            Row.ColumnSpacing(12);
            ColumnDefinition IconColumn;
            IconColumn.Width(winrt::Windows::UI::Xaml::GridLengthHelper::FromPixels(32));
            Row.ColumnDefinitions().Append(IconColumn);
            Row.ColumnDefinitions().Append(ColumnDefinition());
            FontIcon Icon;
            Icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily(L"Segoe MDL2 Assets"));
            Icon.Glyph(L"\uE7F4");
            Icon.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Top);
            Icon.Margin(winrt::Windows::UI::Xaml::ThicknessHelper::FromLengths(0, 4, 0, 0));
            Row.Children().Append(Icon);
            StackPanel Info;
            Info.Spacing(4);
            Grid::SetColumn(Info, 1);
            TextBlock Title;
            Title.Text(winrt::hstring(Entry.Name));
            Title.FontSize(17);
            Title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            Title.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
            Info.Children().Append(Title);
            TextBlock Meta;
            std::wstring Description;
            if (Entry.Valid)
            {
                Description = Entry.Guest.empty() ? std::wstring(::WelcomeText(L"WorkspaceUnknown").c_str()) : Entry.Guest;
                Description += L"  •  " + std::to_wstring(Entry.Processors) + L" " + ::WelcomeText(L"WorkspaceCpu").c_str();
                Description += L"  •  " + std::to_wstring(Entry.Memory) + L" " + ::WelcomeText(L"WorkspaceMemory").c_str();
            }
            else Description = ::WelcomeText(Entry.Exists ? L"WorkspaceInvalid" : L"WorkspaceMissing").c_str();
            Meta.Text(winrt::hstring(Description));
            Meta.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
            Info.Children().Append(Meta);
            TextBlock Location;
            Location.Text(winrt::hstring(Entry.Path));
            Location.FontSize(12);
            Location.Opacity(0.7);
            Location.TextTrimming(winrt::Windows::UI::Xaml::TextTrimming::CharacterEllipsis);
            Info.Children().Append(Location);
            Row.Children().Append(Info);
            Item.Content(Row);
            List.Items().Append(Item);
            if (::NanaBox::WorkspaceHistory::SamePath(Entry.Path, Previous)) SelectedIndex = static_cast<int>(Index);
        }
        auto Count = List.Items().Size();
        List.SelectedIndex(Count ? (SelectedIndex >= 0 ? SelectedIndex : 0) : -1);
        this->WorkspaceEmptyText().Visibility(Count ? winrt::Windows::UI::Xaml::Visibility::Collapsed : winrt::Windows::UI::Xaml::Visibility::Visible);
        this->WorkspaceEmptyText().Text(::WelcomeText(this->m_Workspaces.empty() ? L"WorkspaceEmpty" : L"WorkspaceNoMatches"));
        this->WorkspaceClearButton().IsEnabled(!this->m_Workspaces.empty());
        this->UpdateWorkspaceActions();
    }

    void QuickStartPage::UpdateWorkspaceActions()
    {
        if (!this->WorkspaceStartButton() || !this->WorkspaceSettingsButton() ||
            !this->WorkspaceFolderButton() || !this->WorkspaceRemoveButton()) return;
        auto Path = this->SelectedWorkspacePath();
        auto Found = std::find_if(this->m_Workspaces.begin(), this->m_Workspaces.end(),
            [&](auto const& Entry) { return ::NanaBox::WorkspaceHistory::SamePath(Entry.Path, Path); });
        bool Selected = !Path.empty() && Found != this->m_Workspaces.end();
        bool Valid = Selected && Found->Valid;
        this->WorkspaceStartButton().IsEnabled(Valid && this->m_ConfigurationFilePath);
        this->WorkspaceSettingsButton().IsEnabled(Valid);
        this->WorkspaceFolderButton().IsEnabled(Selected);
        this->WorkspaceRemoveButton().IsEnabled(Selected);
    }

    std::wstring QuickStartPage::BrowseWorkspace()
    {
        auto Name = std::wstring(::WelcomeText(L"WorkspaceFileType").c_str()) + L" (*.7b)";
        COMDLG_FILTERSPEC Filter{ Name.c_str(), L"*.7b" };
        LPWSTR Path = nullptr;
        auto Result = ::ShowFileDialog(this->m_WindowHandle, FALSE, 1, &Filter,
            nullptr, nullptr, nullptr, &Path);
        if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        winrt::check_hresult(Result);
        if (!Path) throw winrt::hresult_error(E_UNEXPECTED);
        auto Cleanup = Mile::ScopeExitTaskHandler([Path]() { ::CoTaskMemFree(Path); });
        return ::GetAbsolutePath(Path);
    }

    void QuickStartPage::LaunchWorkspace(std::wstring const& Path)
    {
        if (!this->m_ConfigurationFilePath || Path.empty()) return;
        // Validate again in case the file was moved after refreshing the list.
        auto Configuration = ::NanaBox::DeserializeConfiguration(::ReadAllTextFromUtf8TextFile(Path));
        UNREFERENCED_PARAMETER(Configuration);
        ::NanaBox::WorkspaceHistory::TryRemember(Path, this->m_HistoryKey);
        *this->m_ConfigurationFilePath = Path;
        ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
    }

    void QuickStartPage::OpenWorkspaceSettings(std::wstring const& Path)
    {
        if (Path.empty()) return;
        auto Configuration = ::NanaBox::DeserializeConfiguration(::ReadAllTextFromUtf8TextFile(Path));
        ::NanaBox::WorkspaceHistory::TryRemember(Path, this->m_HistoryKey);
        this->m_SettingsThread = ::ShowVirtualMachineSettingsWindow(this->m_WindowHandle, Path,
            winrt::to_hstring(Configuration.Name).c_str());
    }

    void QuickStartPage::WorkspaceSelectionChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        this->UpdateWorkspaceActions();
    }

    void QuickStartPage::WorkspaceSearchChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs const&)
    {
        this->RenderWorkspaces();
    }

    void QuickStartPage::RefreshWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        this->RefreshWorkspaces(true);
    }

    void QuickStartPage::AddWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try
        {
            auto Path = this->BrowseWorkspace();
            if (Path.empty()) return;
            (void)::NanaBox::DeserializeConfiguration(::ReadAllTextFromUtf8TextFile(Path));
            ::NanaBox::WorkspaceHistory::Remember(Path, this->m_HistoryKey);
            this->WorkspaceSearchBox().Text(L"");
            this->RefreshWorkspaces(true);
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); }
    }

    void QuickStartPage::StartWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try { this->LaunchWorkspace(this->SelectedWorkspacePath()); }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); this->RefreshWorkspaces(true); }
    }

    void QuickStartPage::WorkspaceSettingsButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try { this->OpenWorkspaceSettings(this->SelectedWorkspacePath()); }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); this->RefreshWorkspaces(true); }
    }

    void QuickStartPage::WorkspaceFolderButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try
        {
            auto Path = this->SelectedWorkspacePath();
            if (Path.empty()) return;
            auto Folder = std::filesystem::path(Path).parent_path();
            SHELLEXECUTEINFOW Info{};
            Info.cbSize = sizeof(Info);
            Info.hwnd = this->m_WindowHandle;
            Info.fMask = SEE_MASK_FLAG_NO_UI;
            Info.lpVerb = L"open";
            Info.lpFile = Folder.c_str();
            Info.nShow = SW_SHOWNORMAL;
            winrt::check_bool(::ShellExecuteExW(&Info));
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); }
    }

    void QuickStartPage::RemoveWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try
        {
            auto Path = this->SelectedWorkspacePath();
            if (Path.empty()) return;
            ::NanaBox::WorkspaceHistory::Forget(Path, this->m_HistoryKey);
            this->RefreshWorkspaces(true);
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); }
    }

    void QuickStartPage::ClearWorkspacesButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (::MessageBoxW(this->m_WindowHandle, ::WelcomeText(L"WorkspaceClearConfirm").c_str(),
            L"NanaBox", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
        try
        {
            ::NanaBox::WorkspaceHistory::Clear(this->m_HistoryKey);
            this->RefreshWorkspaces(true);
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); }
    }


    void QuickStartPage::LaunchVirtualMachineButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try { this->LaunchWorkspace(this->BrowseWorkspace()); }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); }
    }

    void QuickStartPage::CreateVirtualMachineButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        this->m_SettingsThread = ::ShowVirtualMachineSettingsWindow(this->m_WindowHandle);
    }

    void QuickStartPage::VirtualMachineSettingsButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try { this->OpenWorkspaceSettings(this->BrowseWorkspace()); }
        catch (...) { ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError()); }
    }

    void QuickStartPage::CreateVirtualHardDiskButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        ::ShowNewVirtualHardDiskDialog(this->m_WindowHandle);
    }

    void QuickStartPage::ResizeVirtualHardDiskButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        ::ShowResizeVirtualHardDiskDialog(this->m_WindowHandle);
    }

    void QuickStartPage::CompactVirtualHardDiskButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        ::ShowCompactVirtualHardDiskDialog(this->m_WindowHandle);
    }

    void QuickStartPage::AboutButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        ::ShowAboutDialog(this->m_WindowHandle);
    }
}
