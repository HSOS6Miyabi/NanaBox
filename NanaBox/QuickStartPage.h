#pragma once

#include "QuickStartPage.g.h"

#include <Windows.h>

#include <string>
#include <vector>
#include <cstdint>

namespace winrt
{
    using Windows::Foundation::IInspectable;
    using Windows::UI::Xaml::RoutedEventArgs;
}

namespace winrt::NanaBox::implementation
{
    struct QuickStartPage : QuickStartPageT<QuickStartPage>
    {
    public:

        QuickStartPage(
            _In_ HWND WindowHandle = nullptr,
            _Out_opt_ std::wstring* ConfigurationFilePath = nullptr,
            std::wstring const& HistoryKey = {});

        ~QuickStartPage();

        void InitializeComponent();
        void WorkspaceSelectionChanged(winrt::IInspectable const&, winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void WorkspaceSearchChanged(winrt::IInspectable const&, winrt::Windows::UI::Xaml::Controls::TextChangedEventArgs const&);
        void RefreshWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void AddWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void StartWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void WorkspaceSettingsButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void WorkspaceFolderButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void RemoveWorkspaceButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void ClearWorkspacesButtonClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);

        void LaunchVirtualMachineButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void CreateVirtualMachineButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void VirtualMachineSettingsButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void CreateVirtualHardDiskButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void ResizeVirtualHardDiskButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void CompactVirtualHardDiskButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void AboutButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

    private:

        struct Workspace
        {
            std::wstring Path, Name, Guest;
            std::uint32_t Processors = 0;
            std::uint64_t Memory = 0;
            bool Exists = false, Valid = false;
        };
        std::wstring m_HistoryKey;
        std::vector<std::wstring> m_HistoryPaths;
        std::vector<Workspace> m_Workspaces;
        winrt::Windows::UI::Xaml::DispatcherTimer m_Timer{nullptr};
        winrt::handle m_SettingsThread;
        bool m_Refreshing = false, m_RefreshPending = false;
        winrt::fire_and_forget RefreshWorkspaces(bool Force);
        void RenderWorkspaces();
        void UpdateWorkspaceActions();
        std::wstring SelectedWorkspacePath();
        std::wstring BrowseWorkspace();
        void LaunchWorkspace(std::wstring const& Path);
        void OpenWorkspaceSettings(std::wstring const& Path);

        HWND m_WindowHandle;
        std::wstring* m_ConfigurationFilePath;
    };
}

namespace winrt::NanaBox::factory_implementation
{
    struct QuickStartPage :
        QuickStartPageT<QuickStartPage, implementation::QuickStartPage>
    {
    };
}
