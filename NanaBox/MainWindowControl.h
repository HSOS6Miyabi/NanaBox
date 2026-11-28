#pragma once

#include "MainWindowControl.g.h"

#include <winrt/Windows.System.h>
#include <winrt/Windows.Services.Store.h>

#include <Windows.h>

#include "Utils.h"

namespace winrt
{
    using Windows::Foundation::IInspectable;
    using Windows::Services::Store::StoreContext;
    using Windows::System::DispatcherQueue;
    using Windows::UI::Xaml::RoutedEventArgs;
}

namespace NanaBox
{
    namespace MainWindowCommands
    {
        enum
        {
            EnterBasicSession = 0x1001,
            EnterEnhancedSession = 0x1002,
            EnterFullScreen = 0x1003,
            PauseVirtualMachine = 0x1004,
            ResumeVirtualMachine = 0x1005,
            RestartVirtualMachine = 0x1006,
            VirtualMachineSettings = 0x1007,
            RedirectKeyboardNavigation = 0x1008,
            ReleaseKeyboardNavigation = 0x1009,
            RestoreVirtualMachineWindowSize = 0x100A,
            CopyScreenshot = 0x100B,
            ToggleRecording = 0x100C,
            CreateVirtualHardDisk = 0x100D,
            ResizeVirtualHardDisk = 0x100E,
            CompactVirtualHardDisk = 0x100F,
            About = 0x1010,
            ShutdownVirtualMachine = 0x1011,
            PowerOffVirtualMachine = 0x1012,
            SaveVirtualMachine = 0x1013,
            Sponsor = 0x1014,
            TypeClipboardText = 0x1015,
        };
    }
}

namespace winrt::NanaBox
{
    using namespace ::NanaBox;
}

namespace winrt::NanaBox::implementation
{
    struct MainWindowControl : MainWindowControlT<MainWindowControl>
    {
    public:

        MainWindowControl(
            _In_ HWND WindowHandle = nullptr,
            bool EnableStore = true);

        void InitializeComponent();

        void PageLoaded(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void EnhancedSessionButtonChanged(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void FullScreenButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void PauseVirtualMachineButtonChanged(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void PowerActionClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);

        void RestartVirtualMachineButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void VirtualMachineSettingsButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void RedirectKeyboardNavigationButtonChanged(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void RestoreVirtualMachineWindowSizeButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void TypeClipboardTextButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void CopyScreenshotButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void RecordVideoButtonChanged(
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

        void SponsorButtonClick(
            winrt::IInspectable const& sender,
            winrt::RoutedEventArgs const& e);

        void AcquireSponsorEdition();
        void SetToggleState(winrt::Windows::UI::Xaml::Controls::AppBarToggleButton const& Button, bool Checked);

    private:

        HWND m_WindowHandle;
        bool m_EnableStore;
        bool m_TogglesReady = false;
        bool m_SynchronizingToggle = false;
        winrt::DispatcherQueue m_DispatcherQueue = nullptr;
        winrt::StoreContext m_StoreContext = nullptr;

        static bool CheckSponsorEditionLicense(winrt::StoreContext const& Context);

        void RefreshSponsorButtonContent();
    };
}

namespace winrt::NanaBox::factory_implementation
{
    struct MainWindowControl : MainWindowControlT<
        MainWindowControl,
        implementation::MainWindowControl>
    {
    };
}
