#include "pch.h"

#include "MainWindow.h"
#include "ExitConfirmationPage.h"

#include <Mile.Xaml.h>

#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include <ShlObj.h>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include "Utils.h"
#include "NetworkReconciliation.h"
#include "PowerTransition.h"
#include "Plan9Reconciliation.h"
#include "KeyboardText.h"

#include <map>
#include <optional>

#include "NanaBoxResources.h"

#include "VmmsCertificate.h"

namespace winrt
{
    using Windows::UI::Core::CoreDispatcherPriority;
    using Windows::ApplicationModel::DataTransfer::Clipboard;
    using Windows::ApplicationModel::DataTransfer::DataPackage;
    using Windows::Storage::Streams::RandomAccessStreamReference;
    using Windows::UI::Xaml::Hosting::DesktopWindowXamlSource;
}

NanaBox::MainWindow::MainWindow(
    std::wstring const& ConfigurationFilePath) :
    m_ConfigurationFilePath(ConfigurationFilePath)
{

}

int NanaBox::MainWindow::OnCreate(
    LPCREATESTRUCT lpCreateStruct)
{
    UNREFERENCED_PARAMETER(lpCreateStruct);

    if (this->DefWindowProcW() != 0)
    {
        return -1;
    }

    this->m_ApplicationIcon.LoadIconW(
        MAKEINTRESOURCE(IDI_NANABOX),
        256,
        256,
        LR_SHARED);

    this->SetIcon(this->m_ApplicationIcon, TRUE);
    this->SetIcon(this->m_ApplicationIcon, FALSE);

    this->m_MainWindowControl =
        winrt::make<winrt::NanaBox::implementation::MainWindowControl>(
            this->m_hWnd);

    if (FAILED(::MileXamlSetXamlContentForContentWindow(
        this->m_hWnd,
        winrt::get_abi(this->m_MainWindowControl))))
    {
        return -1;
    }

    try
    {
        ::ShutdownBlockReasonCreate(
            this->m_hWnd,
            Mile::WinRT::GetLocalizedString(
                L"ExitConfirmationPage/GridTitleTextBlock/Text").c_str());

        this->InitializeVirtualMachine();
        this->m_RedirectKeyboardNavigation =
            this->m_Configuration.Keyboard.RedirectKeyCombinations;

        winrt::check_hresult(::RDPBASE_CreateInstance(
            nullptr,
            CLSID_RDPRuntimeSTAContext,
            IID_IRDPENCPlatformContext,
            this->m_PlatformContext.put_void()));
        winrt::check_hresult(this->m_PlatformContext->InitializeInstance());
        this->m_RdpNamedPipeCallbacks.attach(
            new NanaBox::RdpNamedPipeCallbacks(this));

        this->RdpClientInitialize();
    }
    catch (...)
    {
        ::ShowErrorMessageDialog(Mile::WinRT::ToHResultError());
        return -1;
    }

    return 0;
}

void NanaBox::MainWindow::OnCommand(
    UINT uNotifyCode,
    int nID,
    ATL::CWindow wndCtl)
{
    UNREFERENCED_PARAMETER(wndCtl);

    switch (nID)
    {
    case NanaBox::MainWindowCommands::EnterBasicSession:
    {
        this->m_RdpClientMode = RdpClientMode::BasicSession;
        this->m_NeedRdpClientModeChange = true;

        this->m_RdpClient->Disconnect();

        break;
    }
    case NanaBox::MainWindowCommands::EnterEnhancedSession:
    {
        this->m_RdpClientMode = RdpClientMode::EnhancedSession;
        this->m_NeedRdpClientModeChange = true;

        this->m_RdpClient->Disconnect();

        break;
    }
    case NanaBox::MainWindowCommands::EnterFullScreen:
    {
        // If ZoomLevel doesn't equal 100, Basic Session Mode can't enter full
        // screen.
        if (this->m_RdpClientMode == NanaBox::RdpClientMode::BasicSession)
        {
            this->m_RecommendedZoomLevel = 100;
            VARIANT RawZoomLevel;
            RawZoomLevel.vt = VT_UI4;
            RawZoomLevel.uintVal = this->m_RecommendedZoomLevel;
            this->m_RdpClient->Property(
                L"ZoomLevel",
                RawZoomLevel);
        }

        this->m_RdpClient->FullScreen(true);

        break;
    }
    case NanaBox::MainWindowCommands::PauseVirtualMachine:
    case NanaBox::MainWindowCommands::ResumeVirtualMachine:
    {
        try
        {
            if (this->m_VirtualMachineRunning && !this->m_PowerTransitionPending)
                this->SetVirtualMachinePaused(nID == NanaBox::MainWindowCommands::PauseVirtualMachine);
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_hWnd, Mile::WinRT::ToHResultError()); }
        this->UpdatePowerControls();
        break;
    }
    case NanaBox::MainWindowCommands::RestartVirtualMachine:
    case NanaBox::MainWindowCommands::ShutdownVirtualMachine:
    case NanaBox::MainWindowCommands::PowerOffVirtualMachine:
    case NanaBox::MainWindowCommands::SaveVirtualMachine:
    {
        if (!this->m_VirtualMachineRunning || !this->m_VirtualMachine ||
            (this->m_PowerTransitionPending && nID != NanaBox::MainWindowCommands::PowerOffVirtualMachine)) break;
        try
        {
            if (nID == NanaBox::MainWindowCommands::RestartVirtualMachine ||
                nID == NanaBox::MainWindowCommands::ShutdownVirtualMachine)
            {
                bool WasPaused = this->m_VirtualMachinePaused;
                if (WasPaused) this->SetVirtualMachinePaused(false);
                try
                {
                    NanaBox::RequestVmStop(nID == NanaBox::MainWindowCommands::RestartVirtualMachine,
                        this->m_PowerTransitionPending, this->m_VirtualMachineRestarting,
                        [&]() { this->m_VirtualMachine->Shutdown(); });
                }
                catch (...)
                {
                    if (WasPaused) { try { this->SetVirtualMachinePaused(true); } catch (...) {} }
                    throw;
                }
            }
            else if (nID == NanaBox::MainWindowCommands::SaveVirtualMachine)
                this->SaveAndCloseVirtualMachine();
            else if (::MessageBoxW(this->m_hWnd,
                Mile::WinRT::GetLocalizedString(L"MainWindow/ForcePowerOffConfirm").c_str(),
                L"NanaBox", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            {
                NanaBox::RequestVmStop(false, this->m_PowerTransitionPending,
                    this->m_VirtualMachineRestarting, [&]() { this->m_VirtualMachine->Terminate(); });
            }
        }
        catch (...)
        {
            ::ShowErrorMessageDialog(this->m_hWnd, Mile::WinRT::ToHResultError());
        }
        this->UpdatePowerControls();
        break;
    }
    case NanaBox::MainWindowCommands::VirtualMachineSettings:
    {
        if (!this->m_VirtualMachineRunning || this->m_PowerTransitionPending) break;
        ::ShowVirtualMachineSettingsWindow(
            this->m_hWnd,
            this->m_ConfigurationFilePath,
            Mile::ToWideString(CP_UTF8, this->m_Configuration.Name),
            this);

        break;
    }
    case NanaBox::MainWindowCommands::RedirectKeyboardNavigation:
    case NanaBox::MainWindowCommands::ReleaseKeyboardNavigation:
    {
        this->m_RedirectKeyboardNavigation =
            nID == NanaBox::MainWindowCommands::RedirectKeyboardNavigation;
        this->m_NeedRdpClientModeChange = true;

        this->m_RdpClient->Disconnect();

        break;
    }
    case NanaBox::MainWindowCommands::RestoreVirtualMachineWindowSize:
    {
        this->ShowWindow(SW_RESTORE);

        UINT DpiValue = ::GetDpiForWindow(this->m_hWnd);
        UINT RdpDpiValue = static_cast<UINT>(::MulDiv(USER_DEFAULT_SCREEN_DPI,
            NanaBox::EffectiveDisplayScale(this->m_Configuration.VideoMonitor,
                static_cast<std::uint32_t>(::MulDiv(100, DpiValue, USER_DEFAULT_SCREEN_DPI)),
                this->m_RdpClientMode == RdpClientMode::BasicSession), 100));

        RECT WindowRect;
        winrt::check_bool(this->GetWindowRect(&WindowRect));
        RECT ClientRect;
        winrt::check_bool(this->GetClientRect(&ClientRect));
        WindowRect.right += ::MulDiv(
            this->m_Configuration.VideoMonitor.HorizontalResolution,
            RdpDpiValue,
            USER_DEFAULT_SCREEN_DPI) - ClientRect.right;
        WindowRect.bottom += ::MulDiv(
            this->m_Configuration.VideoMonitor.VerticalResolution,
            RdpDpiValue,
            USER_DEFAULT_SCREEN_DPI) + ::MulDiv(
                this->m_RecommendedMainWindowControlHeight,
                DpiValue,
                USER_DEFAULT_SCREEN_DPI) - ClientRect.bottom;

        this->SetWindowPos(
            nullptr,
            &WindowRect,
            SWP_NOZORDER | SWP_NOACTIVATE);

        break;
    }
    case NanaBox::MainWindowCommands::TypeClipboardText:
    {
        try
        {
            if (!this->m_VirtualMachineRunning || this->m_VirtualMachinePaused ||
                this->m_PowerTransitionPending || !this->m_RdpClient ||
                this->m_RdpClient->Connected() != 1)
                throw winrt::hresult_error(E_FAIL, L"The virtual machine console is not connected or is paused.");
            std::wstring Text;
            {
                winrt::check_bool(::OpenClipboard(this->m_hWnd));
                auto Close = Mile::ScopeExitTaskHandler([]() { ::CloseClipboard(); });
                auto Data = ::GetClipboardData(CF_UNICODETEXT);
                if (!Data) throw winrt::hresult_invalid_argument(L"The clipboard does not contain text.");
                auto Buffer = static_cast<wchar_t const*>(::GlobalLock(Data));
                winrt::check_pointer(Buffer);
                auto Unlock = Mile::ScopeExitTaskHandler([&]() { ::GlobalUnlock(Data); });
                auto Capacity = (std::min)(::GlobalSize(Data) / sizeof(wchar_t), std::size_t{4097});
                std::size_t Length = 0;
                while (Length < Capacity && Buffer[Length]) ++Length;
                if (Length == Capacity)
                    throw winrt::hresult_invalid_argument(L"Clipboard text is invalid or exceeds 4096 characters.");
                Text.assign(Buffer, Length);
            }
            std::vector<NanaBox::KeyboardTextBatch> Batches;
            try { Batches = NanaBox::EncodeKeyboardText(Text); }
            catch (std::invalid_argument const& Error)
            { throw winrt::hresult_invalid_argument(winrt::to_hstring(Error.what())); }
            if (Batches.empty()) break;
            for (auto const& Batch : Batches)
            {
                VARIANT_BOOL KeyUp[20]{};
                LONG KeyData[20]{};
                for (std::size_t I = 0; I < Batch.size(); ++I)
                {
                    KeyUp[I] = Batch[I].KeyUp ? VARIANT_TRUE : VARIANT_FALSE;
                    KeyData[I] = static_cast<LONG>(1u |
                        (static_cast<std::uint32_t>(Batch[I].ScanCode) << 16) |
                        (Batch[I].KeyUp ? 0xc0000000u : 0u));
                }
                this->m_RdpClient->SendKeys(static_cast<LONG>(Batch.size()), KeyUp, KeyData);
            }
            this->m_RdpClientWindow.SetFocus();
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_hWnd, Mile::WinRT::ToHResultError()); }
        break;
    }
    case NanaBox::MainWindowCommands::CopyScreenshot:
    case NanaBox::MainWindowCommands::ToggleRecording:
    {
        bool Recording = nID == NanaBox::MainWindowCommands::ToggleRecording;
        auto Control = winrt::get_self<
            winrt::NanaBox::implementation::MainWindowControl>(
                this->m_MainWindowControl);
        Control->SetToggleState(Control->RecordVideoButton(), this->m_Recording.load());
        if (uNotifyCode == NanaBox::MainWindowNotifications::CaptureStateChanged)
        {
            if (!Recording && this->m_Screenshot)
            {
                winrt::DataPackage Data;
                Data.SetBitmap(winrt::RandomAccessStreamReference::
                    CreateFromStream(this->m_Screenshot));
                winrt::Clipboard::SetContent(Data);
                winrt::Clipboard::Flush();
                this->m_Screenshot = nullptr;
            }
            if (!Recording || !this->m_Recording)
            {
                (Recording ? this->m_RecordingThread :
                    this->m_ScreenshotThread).close();
            }
        }
        else
        {
            this->StartCapture(Recording);
        }
        Control->RecordVideoButton().IsEnabled(this->m_Recording || !this->m_RecordingThread);
        break;
    }
    case NanaBox::MainWindowCommands::CreateVirtualHardDisk:
    {
        ::ShowNewVirtualHardDiskDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::ResizeVirtualHardDisk:
    {
        ::ShowResizeVirtualHardDiskDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::CompactVirtualHardDisk:
    {
        ::ShowCompactVirtualHardDiskDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::About:
    {
        ::ShowAboutDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::Sponsor:
    {
        winrt::get_self<winrt::NanaBox::implementation::MainWindowControl>(
            this->m_MainWindowControl)->AcquireSponsorEdition();
        break;
    }
    default:
        break;
    }
}

void NanaBox::MainWindow::StartCapture(_In_ bool Recording)
{
    winrt::handle& Thread = Recording ?
        this->m_RecordingThread : this->m_ScreenshotThread;
    winrt::handle& Stop = Recording ?
        this->m_RecordingStop : this->m_ScreenshotStop;
    if (Thread)
    {
        if (Recording)
        {
            ::SetEvent(Stop.get());
        }
        return;
    }
    try
    {
        std::wstring FilePath;
        if (Recording)
        {
            COMDLG_FILTERSPEC Type = {};
            Type.pszName = L"MP4 (*.mp4)";
            Type.pszSpec = L"*.mp4";
            LPWSTR RawFilePath = nullptr;
            HRESULT Result = ::ShowFileDialog(
                this->m_hWnd,
                TRUE,
                1,
                &Type,
                nullptr,
                L"mp4",
                nullptr,
                &RawFilePath);
            if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return;
            }
            winrt::check_hresult(Result);
            auto FreeFilePath = Mile::ScopeExitTaskHandler([RawFilePath]()
            {
                ::CoTaskMemFree(RawFilePath);
            });
            FilePath = RawFilePath;
        }
        winrt::DesktopWindowXamlSource XamlSource = nullptr;
        winrt::copy_from_abi(XamlSource,
            ::GetPropW(this->m_hWnd, L"XamlWindowSource"));
        HWND ToolbarHandle = nullptr;
        winrt::check_hresult(XamlSource.as<
            IDesktopWindowXamlSourceNative>()->get_WindowHandle(&ToolbarHandle));
        Stop.attach(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        winrt::check_bool(Stop.get());
        Thread.attach(Mile::CreateThread([this, Recording, FilePath,
            ToolbarHandle, WindowHandle = this->m_hWnd, StopEvent = Stop.get()]()
        {
            ::SetThreadDescription(::GetCurrentThread(), L"NanaBox capture");
            std::optional<winrt::hresult_error> Error;
            try
            {
                auto Stream = NanaBox::CaptureVirtualMachine(
                    WindowHandle, ToolbarHandle, StopEvent, FilePath, [&]()
                {
                    this->m_Recording = true;
                    ::PostMessageW(WindowHandle, WM_COMMAND, MAKEWPARAM(
                        NanaBox::MainWindowCommands::ToggleRecording,
                        NanaBox::MainWindowNotifications::CaptureStateChanged), 0);
                });
                if (!Recording)
                {
                    this->m_Screenshot = Stream;
                }
            }
            catch (...)
            {
                Error = Mile::WinRT::ToHResultError();
            }
            if (Recording)
            {
                this->m_Recording = false;
            }
            ::PostMessageW(WindowHandle, WM_COMMAND, MAKEWPARAM(
                Recording ? NanaBox::MainWindowCommands::ToggleRecording :
                    NanaBox::MainWindowCommands::CopyScreenshot,
                NanaBox::MainWindowNotifications::CaptureStateChanged), 0);
            if (Error &&
                Error->code() != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                ::ShowErrorMessageDialog(*Error);
            }
        }));
        winrt::check_bool(Thread.get());
    }
    catch (...)
    {
        ::ShowErrorMessageDialog(Mile::WinRT::ToHResultError());
    }
}

void NanaBox::MainWindow::ReloadVirtualMachineSettings(std::function<void(winrt::hstring)> Completion)
{
    this->m_MainWindowControl.Dispatcher().RunAsync(
        winrt::CoreDispatcherPriority::Normal,
        [this, Completion = std::move(Completion)]()
    {
        winrt::hstring Error;
        try { this->TryReloadVirtualMachine(); }
        catch (...) { Error = Mile::WinRT::ToHResultError().message(); }
        if (Completion) Completion(Error);
    });
}

void NanaBox::MainWindow::RequestNetworkDiagnostics(bool Retry, std::function<void(winrt::hstring)> Completion)
{
    this->m_MainWindowControl.Dispatcher().RunAsync(
        winrt::CoreDispatcherPriority::Normal,
        [this, Retry, Completion = std::move(Completion)]()
    {
        winrt::hstring Text;
        try
        {
            if (this->m_MirroredNetworking)
            {
                if (Retry) this->m_MirroredNetworking->RequestRefresh();
                auto Data = nlohmann::json::parse(this->m_MirroredNetworking->Diagnostics());
                Data["RefreshRequested"] = Retry;
                Text = winrt::to_hstring(Data.dump(2));
            }
            else
                Text = winrt::to_hstring(nlohmann::json{{"Mode", "Conventional"},
                    {"RuntimeId", this->m_VirtualMachineGuid}, {"Running", this->m_VirtualMachineRunning},
                    {"MirroredEnabled", this->m_Configuration.MirroredNetworking}}.dump(2));
        }
        catch (...) { Text = winrt::to_hstring(nlohmann::json{{"Error",
            winrt::to_string(Mile::WinRT::ToHResultError().message())}}.dump(2)); }
        if (Completion) Completion(Text);
    });
}

void NanaBox::MainWindow::OnTimer(
    UINT_PTR nIDEvent)
{
    if (nIDEvent == NanaBox::MainWindowTimerEvents::MirroredNetworkStatus)
    {
        if (this->m_MirroredNetworking)
        {
            std::wstring Title = this->m_WindowTitle + L" | Mirrored: " +
                (this->m_VirtualMachinePaused ? std::wstring(L"Paused") : this->m_MirroredNetworking->Status());
            this->SetWindowTextW(Title.c_str());
        }
        return;
    }
    if (nIDEvent == NanaBox::MainWindowTimerEvents::SyncDisplaySettings)
    {
        if (!this->m_RdpClient || !this->m_Configuration.VideoMonitor.EnableContentResizing ||
            this->m_RdpClientMode != RdpClientMode::EnhancedVideoSyncedSession)
        {
            return;
        }

        UINT64 CurrentCheckPoint = ::GetTickCount64();
        if (this->m_SyncDisplaySettingsCheckPoint > CurrentCheckPoint)
        {
            return;
        }
        else
        {
            this->m_SyncDisplaySettingsCheckPoint = CurrentCheckPoint + 200;
        }

        CSize DisplayResolution = this->m_RecommendedDisplayResolution;
        if (DisplayResolution.cx < 200 || DisplayResolution.cy < 200) return;
        if (this->m_DisplayResolution == DisplayResolution && this->m_AppliedDisplayScale == this->m_RecommendedZoomLevel)
        {
            return;
        }

        try
        {
            this->m_RdpClient->UpdateSessionDisplaySettings(
                DisplayResolution.cx,
                DisplayResolution.cy,
                DisplayResolution.cx,
                DisplayResolution.cy,
                0,
                this->m_RecommendedZoomLevel,
                100);

            this->m_DisplayResolution = DisplayResolution;
            this->m_AppliedDisplayScale = this->m_RecommendedZoomLevel;
        }
        catch (...)
        {

        }
    }
}

void NanaBox::MainWindow::OnSize(
    UINT nType,
    CSize size)
{
    UNREFERENCED_PARAMETER(nType);
    UNREFERENCED_PARAMETER(size);

    if (nType == SIZE_MINIMIZED)
    {
        return;
    }

    winrt::DesktopWindowXamlSource XamlSource = nullptr;
    winrt::copy_from_abi(
        XamlSource,
        ::GetPropW(this->m_hWnd, L"XamlWindowSource"));
    if (!XamlSource)
    {
        return;
    }

    UINT DpiValue = ::GetDpiForWindow(this->m_hWnd);

    this->m_RecommendedZoomLevel = NanaBox::EffectiveDisplayScale(this->m_Configuration.VideoMonitor,
        static_cast<std::uint32_t>(::MulDiv(100, DpiValue, USER_DEFAULT_SCREEN_DPI)),
        this->m_RdpClientMode == RdpClientMode::BasicSession);
    if (this->m_RdpClientMode == RdpClientMode::BasicSession && this->m_RecommendedMainWindowControlHeight == 0)
        this->m_RecommendedZoomLevel = 100;

    int MainWindowControlScaledHeight = ::MulDiv(
        this->m_RecommendedMainWindowControlHeight,
        DpiValue,
        USER_DEFAULT_SCREEN_DPI);

    MARGINS Margins = {};
    Margins.cyTopHeight = MainWindowControlScaledHeight;
    ::DwmExtendFrameIntoClientArea(this->m_hWnd, &Margins);

    RECT RdpClientRect;
    winrt::check_bool(this->GetClientRect(&RdpClientRect));
    RdpClientRect.top += MainWindowControlScaledHeight;

    this->m_RdpClientWindow.SetWindowPos(
        nullptr,
        &RdpClientRect,
        SWP_NOZORDER | SWP_NOACTIVATE);

    winrt::com_ptr<IDesktopWindowXamlSourceNative> XamlSourceNative =
        XamlSource.as<IDesktopWindowXamlSourceNative>();

    HWND XamlWindowHandle = nullptr;
    winrt::check_hresult(
        XamlSourceNative->get_WindowHandle(&XamlWindowHandle));
    ::SetWindowPos(
        XamlWindowHandle,
        nullptr,
        0,
        0,
        RdpClientRect.right - RdpClientRect.left,
        MainWindowControlScaledHeight,
        SWP_SHOWWINDOW);

    this->m_RecommendedDisplayResolution = CSize(
        RdpClientRect.right - RdpClientRect.left,
        RdpClientRect.bottom - RdpClientRect.top);

    if (this->m_RdpClient && this->m_RdpClient->Connected() &&
        RdpClientMode::BasicSession == this->m_RdpClientMode)
    {
        VARIANT RawZoomLevel;
        RawZoomLevel.vt = VT_UI4;
        RawZoomLevel.uintVal =
            this->m_Configuration.VideoMonitor.DisableBasicSessionDpiScaling
            ? 100
            : this->m_RecommendedZoomLevel;
        this->m_RdpClient->Property(
            L"ZoomLevel",
            RawZoomLevel);
    }
}

void NanaBox::MainWindow::OnSetFocus(
    ATL::CWindow wndOld)
{
    UNREFERENCED_PARAMETER(wndOld);

    this->m_RdpClientWindow.SetFocus();
}

void NanaBox::MainWindow::OnActivate(
    UINT nState,
    BOOL bMinimized,
    ATL::CWindow wndOther)
{
    UNREFERENCED_PARAMETER(wndOther);

    if (bMinimized || nState == WA_INACTIVE)
    {
        return;
    }

    this->m_RdpClientWindow.SetFocus();
}

void NanaBox::MainWindow::OnClose()
{
    if (!this->m_VirtualMachineRunning)
    {
        this->DestroyWindow();
        return;
    }

    HWND WindowHandle = ::CreateXamlDialog(this->m_hWnd);
    if (!WindowHandle)
    {
        return;
    }

    winrt::NanaBox::ExitConfirmationPage Window =
        winrt::make<winrt::NanaBox::implementation::ExitConfirmationPage>(
            WindowHandle);
    ::ShowXamlDialog(
        WindowHandle,
        480,
        320,
        winrt::get_abi(Window),
        this->m_hWnd);

    switch (Window.Status())
    {
    case winrt::NanaBox::ExitConfirmationStatus::Suspend:
    {
        try { this->SaveAndCloseVirtualMachine(); }
        catch (...) { ::ShowErrorMessageDialog(this->m_hWnd, Mile::WinRT::ToHResultError()); }
        break;
    }
    case winrt::NanaBox::ExitConfirmationStatus::PowerOff:
    {
        try
        {
            NanaBox::RequestVmStop(false, this->m_PowerTransitionPending,
                this->m_VirtualMachineRestarting, [&]() { this->m_VirtualMachine->Terminate(); });
        }
        catch (...) { ::ShowErrorMessageDialog(this->m_hWnd, Mile::WinRT::ToHResultError()); }
        this->UpdatePowerControls();
        break;
    }
    default:
        break;
    }
}

void NanaBox::MainWindow::UpdatePowerControls()
{
    if (!this->m_MainWindowControl) return;
    auto Control = winrt::get_self<winrt::NanaBox::implementation::MainWindowControl>(this->m_MainWindowControl);
    bool Enabled = this->m_VirtualMachineRunning && !this->m_PowerTransitionPending;
    Control->SetToggleState(Control->PauseVirtualMachineButton(), this->m_VirtualMachinePaused);
    Control->PauseVirtualMachineButton().IsEnabled(Enabled);
    Control->RestartVirtualMachineButton().IsEnabled(Enabled);
    Control->TypeClipboardTextButton().IsEnabled(Enabled && !this->m_VirtualMachinePaused);
    // Keep the power menu available while a graceful shutdown is pending.
    Control->PowerMenuButton().IsEnabled(this->m_VirtualMachineRunning);
    Control->ShutdownAction().IsEnabled(Enabled);
    Control->SaveAction().IsEnabled(Enabled);
}

void NanaBox::MainWindow::SetVirtualMachinePaused(bool Paused)
{
    if (!this->m_VirtualMachine || this->m_VirtualMachinePaused == Paused) return;
    if (Paused)
    {
        if (this->m_MirroredNetworking) this->m_MirroredNetworking->PauseSynchronization();
        try { this->m_VirtualMachine->Pause(); }
        catch (...)
        {
            if (this->m_MirroredNetworking) this->m_MirroredNetworking->ResumeSynchronization();
            throw;
        }
        this->m_VirtualMachinePaused = true;
    }
    else
    {
        this->m_VirtualMachine->Resume();
        this->m_VirtualMachinePaused = false;
        if (this->m_MirroredNetworking) this->m_MirroredNetworking->ResumeSynchronization();
    }
}

void NanaBox::MainWindow::CleanupExitedVirtualMachine()
{
    this->KillTimer(NanaBox::MainWindowTimerEvents::MirroredNetworkStatus);
    this->m_Plan9MountManager.reset();
    if (this->m_MirroredNetworking) this->m_MirroredNetworking->VmExited();
    this->m_MirroredNetworking.reset();
    this->m_VirtualMachine = nullptr;
    if (this->m_OwnsVmResources)
    {
        for (auto const& Adapter : this->m_Configuration.NetworkAdapters)
        {
            try { NanaBox::ComputeNetworkDeleteEndpoint(this->m_Configuration.Name, Adapter); }
            catch (...) { ::OutputDebugStringW(Mile::WinRT::ToHResultError().message().c_str()); }
        }
    }
    this->m_OwnsVmResources = false;
}

LRESULT NanaBox::MainWindow::OnVirtualMachineExited(UINT, WPARAM Generation, LPARAM, BOOL&)
{
    if (Generation != this->m_VirtualMachineGeneration || !this->m_OwnsVmResources) return 0;
    bool Restart = this->m_VirtualMachineRestarting;
    this->m_VirtualMachineRunning = false;
    this->m_VirtualMachinePaused = false;
    this->m_PowerTransitionPending = false;
    this->m_VirtualMachineRestarting = false;
    this->CleanupExitedVirtualMachine();
    this->UpdatePowerControls();
    if (Restart)
    {
        try
        {
            if (this->m_RdpNamedPipe)
            {
                this->m_RdpNamedPipe->TerminateInstance();
                this->m_RdpNamedPipe = nullptr;
            }
            if (this->m_RdpClient) this->RdpClientUninitialize();
            this->InitializeVirtualMachine();
            this->RdpClientInitialize();
            this->UpdatePowerControls();
            return 0;
        }
        catch (...)
        {
            auto Error = Mile::WinRT::ToHResultError();
            if (this->m_VirtualMachineRunning && this->m_VirtualMachine)
            {
                try { this->m_VirtualMachine->Terminate(); }
                catch (...) { ::ShowErrorMessageDialog(this->m_hWnd, Error); return 0; }
                this->m_VirtualMachineRunning = false;
                this->CleanupExitedVirtualMachine();
            }
            ::ShowErrorMessageDialog(this->m_hWnd, Error);
        }
    }
    this->PostMessageW(WM_CLOSE);
    return 0;
}

void NanaBox::MainWindow::SaveAndCloseVirtualMachine()
{
    if (!this->m_VirtualMachineRunning || !this->m_VirtualMachine || this->m_PowerTransitionPending) return;
    auto Expected = ::ReadAllTextFromUtf8TextFile(this->m_ConfigurationFilePath);
    if (Expected != this->m_PersistedConfigurationContent)
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
            L"Reload pending configuration changes before saving the virtual machine state.");
    auto Original = this->m_Configuration;
    auto Saved = Original;
    GUID Id{};
    winrt::check_hresult(::CoCreateGuid(&Id));
    Saved.SaveStateFile = "NanaBox.SaveState-" + winrt::to_string(::FromGuid(Id)) + ".vmrs";
    auto Path = ::GetAbsolutePath(winrt::to_hstring(Saved.SaveStateFile).c_str());
    if (::PathFileExistsW(Path.c_str())) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS));
    nlohmann::json Options = {{"SaveType", "ToFile"}, {"SaveStateFilePath", winrt::to_string(Path)}};
    auto SavedContent = NanaBox::SerializeConfiguration(Saved);
    auto PublishedContent = std::string("\xEF\xBB\xBF") + SavedContent;
    bool InitiallyPaused = this->m_VirtualMachinePaused;
    std::wstring RecoveryErrors;
    NanaBox::SaveStateOperations Operations;
    Operations.ResumeVm = [&]() { this->m_VirtualMachine->Resume(); this->m_VirtualMachinePaused = false; };
    Operations.PauseVm = [&]()
    {
        this->m_VirtualMachine->Pause();
        this->m_VirtualMachinePaused = true;
    };
    Operations.DetachNetwork = [&]() { this->m_MirroredNetworking->DetachForSave(); };
    Operations.RestoreNetwork = [&]() { this->m_MirroredNetworking->ResumeSynchronization(); };
    Operations.DisableGpu = [&]()
    {
        NanaBox::GpuConfiguration Disabled;
        NanaBox::ComputeSystemUpdateGpu(this->m_VirtualMachine, Disabled);
    };
    Operations.RestoreGpu = [&]() { NanaBox::ComputeSystemUpdateGpu(this->m_VirtualMachine, Original.Gpu); };
    Operations.Save = [&]() { this->m_VirtualMachine->Save(winrt::to_hstring(Options.dump())); };
    Operations.Publish = [&]()
    {
        ::WriteAllTextToUtf8TextFile(this->m_ConfigurationFilePath, SavedContent, &Expected);
        this->m_Configuration = Saved;
        this->m_PersistedConfigurationContent = PublishedContent;
    };
    Operations.Unpublish = [&]()
    {
        ::WriteAllTextToUtf8TextFile(this->m_ConfigurationFilePath, Expected.substr(3), &PublishedContent);
        this->m_Configuration = Original;
        this->m_PersistedConfigurationContent = Expected;
    };
    Operations.RemoveSave = [&]()
    {
        if (!::DeleteFileW(Path.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
            winrt::throw_last_error();
    };
    Operations.Terminate = [&]() { this->m_VirtualMachine->Terminate(); };
    Operations.RollbackError = [&](char const* Stage)
    {
        RecoveryErrors += L"\n" + std::wstring(winrt::to_hstring(Stage).c_str()) + L": " +
            Mile::WinRT::ToHResultError().message().c_str();
    };
    try
    {
        NanaBox::SaveStateTransaction(InitiallyPaused,
            Original.Gpu.AssignmentMode != NanaBox::GpuAssignmentMode::Disabled,
            this->m_MirroredNetworking != nullptr, Operations);
        this->m_PowerTransitionPending = true;
    }
    catch (...)
    {
        auto Error = Mile::WinRT::ToHResultError();
        if (this->m_MirroredNetworking && this->m_VirtualMachinePaused)
        {
            try { this->m_MirroredNetworking->PauseSynchronization(); }
            catch (...) { RecoveryErrors += L"\n" + std::wstring(Mile::WinRT::ToHResultError().message().c_str()); }
        }
        this->UpdatePowerControls();
        throw winrt::hresult_error(Error.code(),
            winrt::hstring(std::wstring(Error.message().c_str()) + RecoveryErrors));
    }
    this->UpdatePowerControls();
}

void NanaBox::MainWindow::OnDestroy()
{
    ++this->m_VirtualMachineGeneration;
    if (this->m_VirtualMachineRunning && this->m_VirtualMachine)
    {
        try { this->m_VirtualMachine->Terminate(); this->m_VirtualMachineRunning = false; }
        catch (...) {}
    }
    if (!this->m_VirtualMachineRunning) this->CleanupExitedVirtualMachine();
    else this->m_MirroredNetworking.reset();
    this->m_Plan9MountManager.reset();
    if (this->m_VmMutex) { ::ReleaseMutex(this->m_VmMutex.get()); this->m_VmMutex.close(); }
    ::SetEvent(this->m_RecordingStop.get());
    ::SetEvent(this->m_ScreenshotStop.get());
    HANDLE Threads[2] = {};
    Threads[0] = this->m_RecordingThread.get();
    Threads[1] = this->m_ScreenshotThread.get();
    for (HANDLE Thread : Threads)
    {
        if (Thread)
        {
            while (::MsgWaitForMultipleObjects(
                1, &Thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1)
            {
                MSG Message;
                ::PeekMessageW(&Message, nullptr, 0, 0, PM_NOREMOVE);
            }
        }
    }
    // Just kill the process directly for reducing the Windows error reports.
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

BOOL NanaBox::MainWindow::OnQueryEndSession(
    UINT nSource,
    UINT uLogOff)
{
    UNREFERENCED_PARAMETER(nSource);
    UNREFERENCED_PARAMETER(uLogOff);

    // Notify the OS to prevent shut down because shut down automatically
    // without exiting NanaBox may corrupt the user's virtual machine.
    return FALSE;
}

void NanaBox::MainWindow::InitializeVirtualMachine()
{
    std::string ConfigurationFileContent = ::ReadAllTextFromUtf8TextFile(
        this->m_ConfigurationFilePath);

    this->m_Configuration = NanaBox::DeserializeConfiguration(
        ConfigurationFileContent);

    winrt::hstring HcsVmId = winrt::to_hstring(this->m_Configuration.Name);
    if (this->m_Configuration.Name.empty()) throw winrt::hresult_invalid_argument(L"Virtual machine name is required.");
    if (!this->m_VmMutex)
    {
        std::wstring Name(HcsVmId.c_str());
        ::CharUpperBuffW(Name.data(), static_cast<DWORD>(Name.size()));
        std::uint64_t Hash = 14695981039346656037ull;
        for (auto Ch : Name) { Hash ^= Ch; Hash *= 1099511628211ull; }
        std::wstring MutexName = L"Global\\NanaBox.VM." + std::to_wstring(Hash);
        winrt::handle Mutex(::CreateMutexW(nullptr, FALSE, MutexName.c_str()));
        winrt::check_pointer(Mutex.get());
        DWORD Wait = ::WaitForSingleObject(Mutex.get(), 0);
        if (Wait == WAIT_TIMEOUT) throw winrt::hresult_error(HCS_E_SYSTEM_ALREADY_EXISTS);
        if (Wait != WAIT_OBJECT_0 && Wait != WAIT_ABANDONED) winrt::throw_last_error();
        this->m_VmMutex = std::move(Mutex);
        this->m_LockedVmName = this->m_Configuration.Name;
    }
    else if (::_stricmp(this->m_LockedVmName.c_str(), this->m_Configuration.Name.c_str()) != 0)
        throw winrt::hresult_invalid_argument(L"The virtual machine name changed while it was running.");

    {
        NanaBox::HcsSystem Existing;
        HRESULT Status = ::HcsOpenComputeSystem(HcsVmId.c_str(), GENERIC_READ, Existing.put());
        if (SUCCEEDED(Status)) winrt::throw_hresult(HCS_E_SYSTEM_ALREADY_EXISTS);
        if (Status != HCS_E_SYSTEM_NOT_FOUND) winrt::check_hresult(Status);
    }
    NanaBox::NormalizeNetworkAdapterIds(this->m_Configuration.NetworkAdapters);

    for (NanaBox::ScsiDeviceConfiguration& ScsiDevice
        : this->m_Configuration.ScsiDevices)
    {
        NanaBox::ValidateScsiDeviceConfiguration(HcsVmId, ScsiDevice);
    }

    if (this->m_Configuration.GuestStateFile.empty())
    {
        this->m_Configuration.GuestStateFile =
            this->m_Configuration.Name + ".vmgs";
    }
    {
        std::wstring GuestStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.GuestStateFile));
        if (!::PathFileExistsW(GuestStateFile.c_str()))
        {
            winrt::check_hresult(::HcsCreateEmptyGuestStateFile(
                GuestStateFile.c_str()));
        }

        winrt::check_hresult(::HcsGrantVmAccess(
            HcsVmId.c_str(),
            GuestStateFile.c_str()));
    }

    if (this->m_Configuration.RuntimeStateFile.empty())
    {
        this->m_Configuration.RuntimeStateFile =
            this->m_Configuration.Name + ".vmrs";
    }
    {
        std::wstring RuntimeStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.RuntimeStateFile));
        if (!::PathFileExistsW(RuntimeStateFile.c_str()))
        {
            winrt::check_hresult(::HcsCreateEmptyRuntimeStateFile(
                RuntimeStateFile.c_str()));
        }

        winrt::check_hresult(::HcsGrantVmAccess(
            HcsVmId.c_str(),
            RuntimeStateFile.c_str()));
    }

    if (!this->m_Configuration.SaveStateFile.empty())
    {
        std::wstring SaveStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.SaveStateFile));
        if (::PathFileExistsW(SaveStateFile.c_str()))
        {
            winrt::check_hresult(::HcsGrantVmAccess(
                HcsVmId.c_str(),
                SaveStateFile.c_str()));
        }
    }

    // Validate every existing endpoint before modifying any host resource.
    for (auto const& Adapter : this->m_Configuration.NetworkAdapters)
        (void)NanaBox::HcnOpenOwnedEndpoint(winrt::guid(Adapter.EndpointId), this->m_Configuration.Name);

    std::vector<NanaBox::NetworkAdapterConfiguration> CreatedEndpoints;
    bool Initialized = false;
    auto Rollback = Mile::ScopeExitTaskHandler([&]()
    {
        if (Initialized) return;
        ++this->m_VirtualMachineGeneration;
        bool Stopped = !this->m_VirtualMachine;
        if (this->m_VirtualMachine)
        {
            try { this->m_VirtualMachine->Terminate(); Stopped = true; } catch (...) {}
        }
        this->m_Plan9MountManager.reset();
        if (Stopped && this->m_MirroredNetworking) this->m_MirroredNetworking->VmExited();
        this->m_MirroredNetworking.reset();
        this->m_VirtualMachine = nullptr;
        this->m_VirtualMachineRunning = false;
        if (!Stopped) return;
        for (auto const& Adapter : CreatedEndpoints)
        {
            try { NanaBox::ComputeNetworkDeleteEndpoint(this->m_Configuration.Name, Adapter); } catch (...) {}
        }
    });
    for (auto& Adapter : this->m_Configuration.NetworkAdapters)
    {
        NanaBox::ComputeNetworkDeleteEndpoint(this->m_Configuration.Name, Adapter);
        if (Adapter.Connected)
        {
            NanaBox::ComputeNetworkCreateEndpoint(this->m_Configuration.Name, Adapter);
            CreatedEndpoints.push_back(Adapter);
        }
    }

    auto HcsConfiguration = NanaBox::MakeHcsConfiguration(this->m_Configuration);
    auto HcsDocument = nlohmann::json::parse(HcsConfiguration);
    auto const& Devices = HcsDocument.at("VirtualMachine").at("Devices");
    if (Devices.contains("Plan9"))
    {
        for (auto const& Share : Devices.at("Plan9").at("Shares"))
        {
            auto Path = winrt::to_hstring(Share.at("Path").get<std::string>());
            auto Attributes = ::GetFileAttributesW(Path.c_str());
            if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & FILE_ATTRIBUTE_DIRECTORY))
                throw winrt::hresult_invalid_argument(L"Each Plan 9 share must refer to an existing host directory.");
        }
    }
    this->m_VirtualMachine = winrt::make_self<NanaBox::ComputeSystem>(
        HcsVmId, winrt::to_hstring(HcsConfiguration));

    UINT_PTR Generation = ++this->m_VirtualMachineGeneration;
    HWND WindowHandle = this->m_hWnd;
    this->m_VirtualMachine->SystemExited.add([WindowHandle, Generation](winrt::hstring const&)
    {
        ::PostMessageW(WindowHandle, WM_APP + 0x42, Generation, 0);
    });

    this->m_VirtualMachine->Start();

    NanaBox::ComputeSystemUpdateGpu(
        this->m_VirtualMachine,
        this->m_Configuration.Gpu);

    this->m_VirtualMachineRunning = true;

    nlohmann::json Properties = nlohmann::json::parse(
        winrt::to_string(this->m_VirtualMachine->GetProperties()));
    this->m_VirtualMachineGuid = Properties["RuntimeId"];
    this->m_Plan9Builtins = nlohmann::json::array();
    for (auto const& Entry : NanaBox::MakePlan9GuestManifest(this->m_Configuration, HcsDocument))
    {
        if (std::none_of(this->m_Configuration.Plan9Shares.begin(), this->m_Configuration.Plan9Shares.end(),
            [&](auto const& Share) { return Entry.at("Name") == Share.Name; }))
            this->m_Plan9Builtins.push_back(Entry);
    }
    this->PublishPlan9Inventory();
    if (this->m_Configuration.MirroredNetworking)
    {
        this->m_MirroredNetworking = std::make_unique<NanaBox::MirroredNetworking>(
            this->m_Configuration, winrt::guid(this->m_VirtualMachineGuid));
        this->SetTimer(NanaBox::MainWindowTimerEvents::MirroredNetworkStatus, 1500);
    }

    std::wstring RestoredPath;
    if (!this->m_Configuration.SaveStateFile.empty())
    {
        RestoredPath = ::GetAbsolutePath(winrt::to_hstring(this->m_Configuration.SaveStateFile).c_str());
        this->m_Configuration.SaveStateFile.clear();
    }
    auto UpdatedContent = NanaBox::SerializeConfiguration(this->m_Configuration);
    ::WriteAllTextToUtf8TextFile(this->m_ConfigurationFilePath, UpdatedContent, &ConfigurationFileContent);
    this->m_PersistedConfigurationContent = std::string("\xEF\xBB\xBF") + UpdatedContent;
    this->m_OwnsVmResources = true;
    this->m_VirtualMachinePaused = false;
    this->m_PowerTransitionPending = false;
    this->m_WindowTitle = Mile::ToWideString(
        CP_UTF8,
        Mile::FormatString(
            "%s - NanaBox",
            this->m_Configuration.Name.c_str()));
    this->SetWindowTextW(this->m_WindowTitle.c_str());
    Initialized = true;
    if (!RestoredPath.empty()) ::MileDeleteFileIgnoreReadonlyAttribute(RestoredPath.c_str());
    this->UpdatePowerControls();
}

void NanaBox::MainWindow::PublishPlan9Inventory()
{
    bool Windows = this->m_Configuration.GuestType == NanaBox::GuestType::Windows;
    if (this->m_Configuration.GuestType != NanaBox::GuestType::Linux && !Windows) return;
    if (Windows && !this->m_Configuration.Gpu.EnableHostDriverStore) return;
    auto Manifest = this->m_Plan9Builtins;
    for (auto const& Share : this->m_Configuration.Plan9Shares)
        Manifest.push_back({{"Name", Share.Name}, {"Port", Share.Port},
            {"ReadOnly", Share.ReadOnly}, {"AutoMount", Share.AutoMount}});
    if (Windows)
    {
        auto const& Gpu = this->m_Configuration.Gpu;
        std::vector<std::string> Selected;
        if (Gpu.AssignmentMode == NanaBox::GpuAssignmentMode::List)
            for (auto const& Device : Gpu.SelectedDevices) Selected.push_back(Device.first);
        Manifest = {{"Shares", Manifest}, {"GpuDevices", Selected},
            {"AllGpuDevices", Gpu.AssignmentMode == NanaBox::GpuAssignmentMode::Mirror ||
                Gpu.AssignmentMode == NanaBox::GpuAssignmentMode::Default}};
    }
    if (this->m_Plan9MountManager)
        this->m_Plan9MountManager->Update(Manifest.dump());
    else
        this->m_Plan9MountManager = std::make_unique<NanaBox::Plan9MountManager>(
            winrt::guid(this->m_VirtualMachineGuid), Manifest.dump());
}

void NanaBox::MainWindow::TryReloadVirtualMachine()
{
    if (!this->m_VirtualMachineRunning || !this->m_VirtualMachine || this->m_PowerTransitionPending)
        throw winrt::hresult_error(E_PENDING, L"The virtual machine is changing power state.");
    std::wstring Errors;
    auto Report = [&](std::wstring const& Setting)
    {
        auto Error = Mile::WinRT::ToHResultError();
        Errors += Setting + L": " + Error.message().c_str() + L"\n";
    };
    std::string ConfigurationFileContent =
        ::ReadAllTextFromUtf8TextFile(this->m_ConfigurationFilePath);

    NanaBox::VirtualMachineConfiguration Configuration =
        NanaBox::DeserializeConfiguration(ConfigurationFileContent);

    NanaBox::NormalizeNetworkAdapterIds(Configuration.NetworkAdapters);
    if (this->m_Configuration.MemorySize != Configuration.MemorySize)
    {
        try
        {
            NanaBox::ComputeSystemUpdateMemorySize(
                this->m_VirtualMachine,
                Configuration.MemorySize);
            this->m_Configuration.MemorySize =
                Configuration.MemorySize;
        }
        catch (...)
        {
            Report(L"Memory");
        }
    }

    if (0 != ::_stricmp(
        this->m_Configuration.ComPorts.ComPort1.c_str(),
        Configuration.ComPorts.ComPort1.c_str()))
    {
        try
        {
            if (this->m_Configuration.ComPorts.ComPort1.empty())
            {
                NanaBox::ComputeSystemAddComPort(
                    this->m_VirtualMachine,
                    0,
                    Configuration.ComPorts.ComPort1);
            }
            else if (Configuration.ComPorts.ComPort1.empty())
            {
                NanaBox::ComputeSystemRemoveComPort(
                    this->m_VirtualMachine,
                    0,
                    this->m_Configuration.ComPorts.ComPort1);
            }
            else
            {
                NanaBox::ComputeSystemUpdateComPort(
                    this->m_VirtualMachine,
                    0,
                    Configuration.ComPorts.ComPort1);
            }

            this->m_Configuration.ComPorts.ComPort1 =
                Configuration.ComPorts.ComPort1;
        }
        catch (...)
        {
            Report(L"COM1");
        }
    }

    if (0 != ::_stricmp(
        this->m_Configuration.ComPorts.ComPort2.c_str(),
        Configuration.ComPorts.ComPort2.c_str()))
    {
        try
        {
            if (this->m_Configuration.ComPorts.ComPort2.empty())
            {
                NanaBox::ComputeSystemAddComPort(
                    this->m_VirtualMachine,
                    1,
                    Configuration.ComPorts.ComPort2);
            }
            else if (Configuration.ComPorts.ComPort2.empty())
            {
                NanaBox::ComputeSystemRemoveComPort(
                    this->m_VirtualMachine,
                    1,
                    this->m_Configuration.ComPorts.ComPort2);
            }
            else
            {
                NanaBox::ComputeSystemUpdateComPort(
                    this->m_VirtualMachine,
                    1,
                    Configuration.ComPorts.ComPort2);
            }

            this->m_Configuration.ComPorts.ComPort2 =
                Configuration.ComPorts.ComPort2;
        }
        catch (...)
        {
            Report(L"COM2");
        }
    }

    if (this->m_Configuration.Gpu.AssignmentMode != Configuration.Gpu.AssignmentMode ||
        this->m_Configuration.Gpu.SelectedDevices != Configuration.Gpu.SelectedDevices)
    try
    {
        NanaBox::ComputeSystemUpdateGpu(
            this->m_VirtualMachine,
            Configuration.Gpu);
        this->m_Configuration.Gpu.AssignmentMode = Configuration.Gpu.AssignmentMode;
        this->m_Configuration.Gpu.SelectedDevices = Configuration.Gpu.SelectedDevices;
        this->PublishPlan9Inventory();
    }
    catch (...)
    {
        Report(L"GPU assignment");
    }

    {
        auto ReportAdapter = [&](NanaBox::NetworkAdapterConfiguration const& Adapter, char const* Operation)
        {
            Report(winrt::to_hstring(std::string(Operation) + " network adapter " + Adapter.EndpointId).c_str());
        };
        this->m_Configuration.NetworkAdapters = NanaBox::ReconcileNetworkAdapters(
            this->m_Configuration.NetworkAdapters, Configuration.NetworkAdapters,
            [&](NanaBox::NetworkAdapterConfiguration& Adapter)
            {
                if (Adapter.Connected)
                {
                    NanaBox::ComputeSystemRemoveNetworkAdapter(this->m_VirtualMachine, Adapter);
                    Adapter.Connected = false;
                }
                NanaBox::ComputeNetworkDeleteEndpoint(this->m_Configuration.Name, Adapter);
            },
            [&](NanaBox::NetworkAdapterConfiguration& Adapter)
            {
                NanaBox::ComputeNetworkDeleteEndpoint(this->m_Configuration.Name, Adapter);
                NanaBox::ComputeNetworkCreateEndpoint(this->m_Configuration.Name, Adapter);
                try
                {
                    Adapter.Connected = true;
                    NanaBox::ComputeSystemAddNetworkAdapter(this->m_VirtualMachine, Adapter);
                }
                catch (...)
                {
                    Adapter.Connected = false;
                    try { NanaBox::ComputeNetworkDeleteEndpoint(this->m_Configuration.Name, Adapter); } catch (...) {}
                    throw;
                }
            },
            ReportAdapter);
    }

    {
        std::size_t PreviousCount = this->m_Configuration.ScsiDevices.size();
        std::size_t NewCount = Configuration.ScsiDevices.size();
        if (PreviousCount <= NewCount)
        {
            for (std::uint32_t i = 0; i < NewCount; ++i)
            {
                NanaBox::ScsiDeviceConfiguration& Current =
                    Configuration.ScsiDevices[i];

                if (i < PreviousCount)
                {
                    NanaBox::ScsiDeviceConfiguration& Previous =
                        this->m_Configuration.ScsiDevices[i];

                    if (Previous.Type == Current.Type)
                    {
                        if (0 != ::_stricmp(
                            Previous.Path.c_str(),
                            Current.Path.c_str()))
                        {
                            try
                            {
                                NanaBox::ValidateScsiDeviceConfiguration(
                                    winrt::to_hstring(this->m_Configuration.Name), Current);
                                NanaBox::ComputeSystemUpdateScsiDevice(
                                    this->m_VirtualMachine,
                                    i,
                                    Current);
                                Previous.Path = Current.Path;
                            }
                            catch (...)
                            {
                                Report(L"SCSI device " + std::to_wstring(i));
                            }
                        }
                    }
                    else Errors += L"Changing the type of an attached SCSI device requires shutting down the VM.\n";
                }
                else
                {
                    try
                    {
                        NanaBox::ValidateScsiDeviceConfiguration(
                            winrt::to_hstring(this->m_Configuration.Name),
                            Current);

                        NanaBox::ComputeSystemAddScsiDevice(
                            this->m_VirtualMachine,
                            i,
                            Current);
                        this->m_Configuration.ScsiDevices.push_back(Current);
                    }
                    catch (...)
                    {
                        Report(L"Add SCSI device " + std::to_wstring(i));
                        // Later attachment indices depend on this device.
                        break;
                    }
                }
            }
        }
        else Errors += L"Removing attached SCSI devices requires shutting down the VM.\n";
    }

    {
        auto const& Current = this->m_Configuration.Plan9Shares;
        auto const& Desired = Configuration.Plan9Shares;
        auto SameProvider = [](auto const& Left, auto const& Right)
        {
            return NanaBox::MakeHcsPlan9Share(Left) == NanaBox::MakeHcsPlan9Share(Right);
        };
        bool Changed = Current.size() != Desired.size() ||
            !std::equal(Current.begin(), Current.end(), Desired.begin(), Desired.end(),
                [&](auto const& Left, auto const& Right)
                { return Left.AutoMount == Right.AutoMount && SameProvider(Left, Right); });
        if (Changed)
        {
            try
            {
                if (this->m_Configuration.GuestType != NanaBox::GuestType::Linux || this->m_VirtualMachinePaused)
                    throw winrt::hresult_invalid_argument(L"Live Plan 9 changes require a running Linux guest.");
                for (auto const& Share : Desired)
                {
                    NanaBox::ValidatePlan9HostDirectory(Share);
                    for (auto const& Builtin : this->m_Plan9Builtins)
                    {
                        NanaBox::Plan9ShareConfiguration Identity;
                        Identity.Name = Builtin.at("Name").get<std::string>();
                        if (NanaBox::EqualPlan9ShareName(Share, Identity))
                            throw winrt::hresult_invalid_argument(L"The share name is reserved for the host driver store.");
                    }
                }
                // No inventory update can remount a share during its removal.
                this->m_Plan9MountManager.reset();
                this->m_Configuration.Plan9Shares = NanaBox::ReconcilePlan9Shares(
                    Current, Desired, NanaBox::EqualPlan9ShareName, SameProvider,
                    [&](auto const& Share)
                    {
                        NanaBox::PrepareGuestPlan9ShareRemoval(winrt::guid(this->m_VirtualMachineGuid),
                            Share.Name, Share.Port);
                    },
                    [&](auto const& Share)
                    {
                        NanaBox::ComputeSystemModifyPlan9Share(this->m_VirtualMachine, Share, false);
                    },
                    [&](auto const& Share)
                    {
                        NanaBox::ComputeSystemModifyPlan9Share(this->m_VirtualMachine, Share, true);
                    },
                    [&](auto const& Share, char const* Action)
                    {
                        Report(winrt::to_hstring(std::string(Action) + " Plan9 share " + Share.Name).c_str());
                    });
            }
            catch (...)
            {
                Report(L"Plan9 shares");
            }
            try { this->PublishPlan9Inventory(); }
            catch (...) { Report(L"Publish Plan9 inventory"); }
        }
    }

    try
    {
        // Validate changes with a temporary client; the connected client cannot
        // apply them until reinitialized after disconnection.
        winrt::com_ptr<NanaBox::RdpClient> TemporaryRdpClient =
            winrt::make_self<NanaBox::RdpClient>();

        try
        {
            NanaBox::RemoteDesktopUpdateKeyboardConfiguration(
                TemporaryRdpClient,
                Configuration.Keyboard);
            this->m_Configuration.Keyboard =
                Configuration.Keyboard;
            this->m_RedirectKeyboardNavigation =
                Configuration.Keyboard.RedirectKeyCombinations;
        }
        catch (...)
        {
            Report(L"Keyboard settings");
        }

        try
        {
            NanaBox::RemoteDesktopUpdateEnhancedSessionConfiguration(
                TemporaryRdpClient,
                Configuration.EnhancedSession);
            this->m_Configuration.EnhancedSession =
                Configuration.EnhancedSession;
        }
        catch (...)
        {
            Report(L"Enhanced session settings");
        }
    }
    catch (...)
    {
        Report(L"RDP client initialization");
    }

    {
        auto& Video = this->m_Configuration.VideoMonitor;
        Video.DisableBasicSessionDpiScaling = Configuration.VideoMonitor.DisableBasicSessionDpiScaling;
        Video.EnableDpiScalingValueOverride = Configuration.VideoMonitor.EnableDpiScalingValueOverride;
        Video.OverriddenDpiScalingValue = Configuration.VideoMonitor.OverriddenDpiScalingValue;
        Video.EnableContentResizing = Configuration.VideoMonitor.EnableContentResizing;
        Video.ShowFullScreenModeConnectionBar = Configuration.VideoMonitor.ShowFullScreenModeConnectionBar;
    }

    auto ActualContent = NanaBox::SerializeConfiguration(this->m_Configuration);
    ::WriteAllTextToUtf8TextFile(this->m_ConfigurationFilePath, ActualContent, &ConfigurationFileContent);
    this->m_PersistedConfigurationContent = std::string("\xEF\xBB\xBF") + ActualContent;

    this->m_NeedRdpClientModeChange = true;
    if (this->m_RdpClient)
    {
        try { this->m_RdpClient->Disconnect(); }
        catch (...) { Report(L"Reconnect remote desktop"); }
    }
    if (!Errors.empty())
        throw winrt::hresult_error(E_FAIL, winrt::hstring(Errors));
}

void NanaBox::MainWindow::RdpClientOnRemoteDesktopSizeChange(
    _In_ LONG Width,
    _In_ LONG Height)
{
    if (this->m_RdpClientMode == RdpClientMode::BasicSession)
    {
        this->m_DisplayResolution = CSize(Width, Height);

        if (this->IsZoomed())
        {
            VARIANT RawZoomLevel;
            RawZoomLevel.vt = VT_UI4;
            RawZoomLevel.uintVal =
                this->m_Configuration.VideoMonitor.DisableBasicSessionDpiScaling
                ? 100
                : this->m_RecommendedZoomLevel;
            this->m_RdpClient->Property(
                L"ZoomLevel",
                RawZoomLevel);
            return;
        }

        UINT DpiValue = ::GetDpiForWindow(this->m_hWnd);

        RECT WindowRect;
        winrt::check_bool(this->GetWindowRect(&WindowRect));
        {
            RECT ClientRect;
            winrt::check_bool(this->GetClientRect(&ClientRect));

            // Reserve space for the non-client area.

            WindowRect.right -= ClientRect.right;
            WindowRect.bottom -= ClientRect.bottom;

            // Reserve space for the XAML toolbar window.

            WindowRect.bottom += ::MulDiv(
                this->m_RecommendedMainWindowControlHeight,
                DpiValue,
                USER_DEFAULT_SCREEN_DPI);

            // Reserve space for the RDP client window.

            auto Scale = NanaBox::EffectiveDisplayScale(this->m_Configuration.VideoMonitor,
                static_cast<std::uint32_t>(::MulDiv(100, DpiValue, USER_DEFAULT_SCREEN_DPI)), true);
            WindowRect.right += ::MulDiv(this->m_DisplayResolution.cx, static_cast<int>(Scale), 100);
            WindowRect.bottom += ::MulDiv(this->m_DisplayResolution.cy, static_cast<int>(Scale), 100);
        }

        this->SetWindowPos(
            nullptr,
            &WindowRect,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void NanaBox::MainWindow::RdpClientOnLoginComplete()
{
    this->m_VirtualMachineNeverConnected = true;
    if (this->m_RdpClientMode == RdpClientMode::EnhancedSession)
    {
        this->m_RdpClientMode = RdpClientMode::EnhancedVideoSyncedSession;
        this->m_DisplayResolution = CSize();
    }

    using MainWindowControlImplementation =
        winrt::NanaBox::implementation::MainWindowControl;
    MainWindowControlImplementation* Control =
        winrt::get_self<MainWindowControlImplementation>(
            this->m_MainWindowControl);
    Control->SetToggleState(Control->EnhancedSessionButton(),
        this->m_RdpClientMode != RdpClientMode::BasicSession);
    Control->SetToggleState(Control->RedirectKeyboardNavigationButton(),
        this->m_RedirectKeyboardNavigation);
}

void NanaBox::MainWindow::RdpClientOnDisconnected(
    _In_ LONG DisconnectReason)
{
    this->m_MouseCaptureMode = false;
    ::ClipCursor(nullptr);

    if (this->m_RdpNamedPipe)
    {
        this->m_RdpNamedPipe->TerminateInstance();
        this->m_RdpNamedPipe = nullptr;
    }

    if (this->m_RdpClientMode == RdpClientMode::EnhancedVideoSyncedSession)
    {
        this->m_RdpClientMode = RdpClientMode::EnhancedSession;
    }

    if (this->m_VirtualMachineRunning && !this->m_PowerTransitionPending)
    {
        // I have not found the actual meaning of DisconnectReason even in the
        // reverse engineering view of mstscax.dll. The only thing I know is that
        // it will return the internal error description string when using the
        // GetErrorDescription method of the RDP client.
        if (DisconnectReason == 4 && !this->m_VirtualMachineNeverConnected)
        {
            if (!this->m_VmmsCertificateCreationAttempted)
            {
                this->m_VmmsCertificateCreationAttempted = true;
                if (::CreateVmmsCertificate())
                {
                    try
                    {
                        this->RdpClientConnect();
                        return;
                    }
                    catch (...)
                    {

                    }
                }
            }

            ::ShowMessageDialog(
                nullptr,
                Mile::WinRT::GetLocalizedString(
                    L"Messages/VirtualMachineConnectionFailedInstructionText"),
                Mile::WinRT::GetLocalizedString(
                    L"Messages/VirtualMachineConnectionFailedContentText"));

            this->m_VirtualMachine->Terminate();
        }
        else if (this->m_NeedRdpClientModeChange)
        {
            try
            {
                this->RdpClientUninitialize();
                this->RdpClientInitialize();

                // Ensure the UI layout will be refreshed via sending WM_SIZE
                // message directly. It's OK because we don't use wParam and
                // lParam in WM_SIZE message.
                this->SendMessageW(WM_SIZE);
            }
            catch (...)
            {

            }

            this->m_NeedRdpClientModeChange = false;
        }
        else
        {
            try
            {
                this->RdpClientConnect();
            }
            catch (...)
            {

            }
        }
    }
}

void NanaBox::MainWindow::RdpClientOnRequestGoFullScreen()
{
    HMONITOR MonitorHandle = ::MonitorFromWindow(
        this->m_hWnd,
        MONITOR_DEFAULTTONULL);
    winrt::check_pointer(MonitorHandle);
    MONITORINFOEX MonitorInfo;
    MonitorInfo.cbSize = sizeof(MONITORINFOEX);
    winrt::check_bool(::GetMonitorInfoW(
        MonitorHandle,
        &MonitorInfo));
    DEVMODE DevMode;
    DevMode.dmSize = sizeof(DEVMODE);
    winrt::check_bool(::EnumDisplaySettingsW(
        MonitorInfo.szDevice,
        ENUM_CURRENT_SETTINGS,
        &DevMode));

    winrt::check_bool(this->GetWindowRect(
        &this->m_RememberedMainWindowRect));
    this->m_RememberedMainWindowStyle =
        this->GetWindowLongPtrW(GWL_STYLE);

    RECT FullScreenRect;
    FullScreenRect.left = 0;
    FullScreenRect.top = 0;
    FullScreenRect.right = DevMode.dmPelsWidth;
    FullScreenRect.bottom = DevMode.dmPelsHeight;

    // Resize RDP client window and hide the XAML control window by calculating
    // in OnSize method automatically.
    this->m_RecommendedMainWindowControlHeight = 0;

    this->SetWindowLongPtrW(
        GWL_STYLE,
        WS_VISIBLE | WS_POPUP);
    // SetWindowPos will send WM_SIZE, RDP Zooming will be re-enabled in Basic
    // Session Mode.
    this->SetWindowPos(
        HWND_TOP,
        &FullScreenRect,
        SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void NanaBox::MainWindow::RdpClientOnRequestLeaveFullScreen()
{
    this->m_RecommendedMainWindowControlHeight =
        this->m_MainWindowControlHeight;
    this->SetWindowLongPtrW(
        GWL_STYLE,
        this->m_RememberedMainWindowStyle);
    this->SetWindowPos(
        nullptr,
        &this->m_RememberedMainWindowRect,
        SWP_NOACTIVATE);
}

void NanaBox::MainWindow::RdpClientOnRequestContainerMinimize()
{
    this->ShowWindow(SW_MINIMIZE);
}

void NanaBox::MainWindow::RdpClientOnConfirmClose(
    _Out_ VARIANT_BOOL* pfAllowClose)
{
    // Set it FALSE because we don't want to close the connection before making
    // decisions in the close confirmation dialog.
    *pfAllowClose = VARIANT_FALSE;
    this->PostMessageW(WM_CLOSE);
}

void NanaBox::MainWindow::RdpClientOnFocusReleased(
    _In_ INT Direction)
{
    UNREFERENCED_PARAMETER(Direction);

    this->m_MouseCaptureMode = !this->m_MouseCaptureMode;

    if (this->m_MouseCaptureMode)
    {
        RECT WindowRect;
        if (this->m_RdpClientWindow.GetWindowRect(&WindowRect))
        {
            ::ClipCursor(&WindowRect);
        }
    }
    else
    {
        ::ClipCursor(nullptr);
    }
}

void NanaBox::MainWindow::RdpClientInitialize()
{
    if (this->m_RdpClientMode == RdpClientMode::EnhancedVideoSyncedSession)
        this->m_RdpClientMode = RdpClientMode::EnhancedSession;
    this->m_AppliedDisplayScale = 0;
    this->m_RdpClient = winrt::make_self<NanaBox::RdpClient>();

    this->m_RdpClient->EnableEventsDispatcher();

    winrt::check_pointer(this->m_RdpClientWindow.Create(
        _T(SYSTEM_ATL_AX_WINDOW_CLASS_NAME),
        this->m_hWnd,
        this->m_RdpClientWindow.rcDefault,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN));

    winrt::check_hresult(::SystemAtlAxAttachControl(
        this->m_RdpClient->RawControl().get(),
        this->m_RdpClientWindow.m_hWnd,
        nullptr));

    this->m_RdpClient->ConnectionBarText(this->m_WindowTitle.c_str());
    this->m_RdpClient->DisplayConnectionBar(this->m_Configuration.VideoMonitor.ShowFullScreenModeConnectionBar);
    if (this->m_RdpClientMode == RdpClientMode::EnhancedSession)
        this->m_RdpClient->SmartSizing(!this->m_Configuration.VideoMonitor.EnableContentResizing);

    this->m_RdpClient->EnableAutoReconnect(false);
    this->m_RdpClient->RelativeMouseMode(true);

    this->m_RdpClient->AuthenticationLevel(0);
    this->m_RdpClient->EnableCredSspSupport(false);
    this->m_RdpClient->NegotiateSecurityLayer(true);

    NanaBox::RemoteDesktopUpdateKeyboardConfiguration(
        this->m_RdpClient,
        this->m_Configuration.Keyboard);

    try
    {
        this->m_RdpClient->KeyboardHookMode(
            this->m_RedirectKeyboardNavigation ? 1 : 2);
    }
    catch (...)
    {
        this->m_RedirectKeyboardNavigation =
            (this->m_RdpClient->KeyboardHookMode() == 1);
    }

    NanaBox::RemoteDesktopUpdateEnhancedSessionConfiguration(
        this->m_RdpClient,
        this->m_Configuration.EnhancedSession);

    this->m_RdpClient->ContainerHandledFullScreen(true);

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"AllowAxToContainerEvents", Value);
    }
    catch (...)
    {

    }

    this->m_RdpClient->PerformanceFlags(
        TS_PERF_ENABLE_ENHANCED_GRAPHICS |
        TS_PERF_ENABLE_FONT_SMOOTHING |
        TS_PERF_ENABLE_DESKTOP_COMPOSITION);

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"EnableHardwareMode", Value);
    }
    catch (...)
    {

    }

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"EnableFrameBufferRedirection", Value);
    }
    catch (...)
    {

    }

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"AllowRelativeMouseMode", Value);
    }
    catch (...)
    {

    }

    this->m_RdpClient->GrabFocusOnConnect(false);

    this->m_RdpClient->AllowPromptingForCredentials(false);

    this->m_RdpClient->OnRemoteDesktopSizeChange.add(
        { this, &NanaBox::MainWindow::RdpClientOnRemoteDesktopSizeChange });
    this->m_RdpClient->OnLoginComplete.add(
        { this, &NanaBox::MainWindow::RdpClientOnLoginComplete });
    this->m_RdpClient->OnDisconnected.add(
        { this, &NanaBox::MainWindow::RdpClientOnDisconnected });
    this->m_RdpClient->OnRequestGoFullScreen.add(
        { this, &NanaBox::MainWindow::RdpClientOnRequestGoFullScreen });
    this->m_RdpClient->OnRequestLeaveFullScreen.add(
        { this, &NanaBox::MainWindow::RdpClientOnRequestLeaveFullScreen });
    this->m_RdpClient->OnRequestContainerMinimize.add(
        { this, &NanaBox::MainWindow::RdpClientOnRequestContainerMinimize });
    this->m_RdpClient->OnConfirmClose.add(
        { this, &NanaBox::MainWindow::RdpClientOnConfirmClose });
    this->m_RdpClient->OnFocusReleased.add(
        { this, &NanaBox::MainWindow::RdpClientOnFocusReleased });

    this->SetTimer(
        NanaBox::MainWindowTimerEvents::SyncDisplaySettings,
        200);

    this->m_RdpClient->MinInputSendInterval(20);

    if (this->m_RdpClientMode == RdpClientMode::EnhancedSession)
    {
        const auto& Video = this->m_Configuration.VideoMonitor;
        this->m_RdpClient->DesktopWidth(Video.EnableContentResizing && this->m_RecommendedDisplayResolution.cx >= 200
            ? this->m_RecommendedDisplayResolution.cx : Video.HorizontalResolution);
        this->m_RdpClient->DesktopHeight(Video.EnableContentResizing && this->m_RecommendedDisplayResolution.cy >= 200
            ? this->m_RecommendedDisplayResolution.cy : Video.VerticalResolution);

        try
        {
            VARIANT Value;
            Value.vt = VT_UI4;
            Value.uintVal = NanaBox::EffectiveDisplayScale(Video,
                static_cast<std::uint32_t>(::MulDiv(100, ::GetDpiForWindow(this->m_hWnd), USER_DEFAULT_SCREEN_DPI)), false);
            this->m_RdpClient->Property(L"DesktopScaleFactor", Value);
        }
        catch (...)
        {

        }

        try
        {
            VARIANT Value;
            Value.vt = VT_UI4;
            Value.uintVal = 100;
            this->m_RdpClient->Property(L"DeviceScaleFactor", Value);
        }
        catch (...)
        {

        }
    }

    this->m_RdpClient->MarkRdpSettingsSecure(true);

    this->RdpClientConnect();
}

void NanaBox::MainWindow::RdpClientUninitialize()
{
    this->KillTimer(
        NanaBox::MainWindowTimerEvents::SyncDisplaySettings);
    if (this->m_RdpClient) this->m_RdpClient->DisableEventsDispatcher();
    this->m_RdpClientWindow.DestroyWindow();
    this->m_RdpClient = nullptr;
}

void NanaBox::MainWindow::RdpClientConnect()
{
    winrt::check_hresult(::RDPBASE_CreateInstance(
        this->m_PlatformContext.get(),
        CLSID_RDPENCNamedPipeDirectConnector,
        IID_IRDPENCNamedPipeDirectConnector,
        this->m_RdpNamedPipe.put_void()));
    winrt::check_hresult(this->m_RdpNamedPipe->InitializeInstance(
        this->m_RdpNamedPipeCallbacks.get()));
    winrt::check_hresult(this->m_RdpNamedPipe->StartConnect(
        Mile::ToWideString(
            CP_UTF8,
            Mile::FormatString(
                this->m_RdpClientMode == RdpClientMode::EnhancedSession
                ? "\\\\.\\pipe\\%s.EnhancedSession"
                : "\\\\.\\pipe\\%s.BasicSession",
                this->m_Configuration.Name.c_str())).c_str()));
}

NanaBox::RdpNamedPipeCallbacks::RdpNamedPipeCallbacks(
    _In_ MainWindow* Instance)
    : m_Instance(Instance)
{

}

void STDMETHODCALLTYPE NanaBox::RdpNamedPipeCallbacks::OnConnectionCompleted(
    _In_ IUnknown* pNetStream)
{
    this->m_Instance->m_RdpClient->ConnectWithEndpoint(pNetStream);

    this->m_Instance->m_RdpClient->Connect();

    this->m_Instance->m_RdpClientWindow.SetFocus();
}

void STDMETHODCALLTYPE NanaBox::RdpNamedPipeCallbacks::OnConnectorError(
    _In_ HRESULT hrError)
{
    UNREFERENCED_PARAMETER(hrError);

    this->m_Instance->RdpClientOnDisconnected(
        exDiscReasonServerDeniedConnection);
}
