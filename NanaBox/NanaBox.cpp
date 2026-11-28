/*
 * PROJECT:    NanaBox
 * FILE:       NanaBox.cpp
 * PURPOSE:    Implementation for NanaBox
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#include <Windows.h>

#include <ShlObj.h>

#include "pch.h"

#include "App.h"
#include "MainWindow.h"
#include "NetworkReconciliation.h"
#include "PowerTransition.h"
#include "QuickStartPage.h"
#include "VirtualMachineSettingsPage.h"
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <virtdisk.h>
#pragma comment(lib, "virtdisk.lib")
#include "SponsorPage.h"

#include <Mile.Project.Version.h>

#include "Utils.h"
#include "NanaBoxResources.h"

#include <Mile.Helpers.h>
#include <Mile.Xaml.h>

namespace
{
    const std::wstring_view g_AppUserModelID =
        L"M2Team.NanaBox"
        L"_" MILE_PROJECT_DOT_VERSION_STRING
        L"_" MILE_PROJECT_BUILD_DATE_STRING;

    WTL::CAppModule g_Module;
}

void PrerequisiteCheck()
{
    try
    {
        NanaBox::HcsGetServiceProperties();
    }
    catch (winrt::hresult_error const& ex)
    {
        winrt::hstring InstructionText;
        winrt::hstring ContentText;

        if (ex.code() == HCS_E_ACCESS_DENIED)
        {
            InstructionText = Mile::WinRT::GetLocalizedString(
                L"Messages/AccessDeniedInstructionText");
            ContentText = Mile::WinRT::GetLocalizedString(
                L"Messages/AccessDeniedContentText");
        }
        else if (ex.code() == HCS_E_SERVICE_NOT_AVAILABLE)
        {
            InstructionText = Mile::WinRT::GetLocalizedString(
                L"Messages/HyperVNotAvailableInstructionText");
            ContentText = Mile::WinRT::GetLocalizedString(
                L"Messages/HyperVNotAvailableContentText");
        }
        else
        {
            InstructionText = ex.message();
        }

        ::ShowMessageDialog(
            nullptr,
            InstructionText,
            ContentText);

        ::ExitProcess(ex.code());
    }

    return;
}

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);


    if (0 == ::_wcsicmp(lpCmdLine, L"--self-test"))
    {
        try
        {
            NanaBox::VirtualMachineConfiguration Config;
            Config.Name = "SelfTest";
            Config.GuestType = NanaBox::GuestType::Windows;
            Config.ProcessorCount = 2;
            Config.MemorySize = 2048;
            auto DefaultResources = NanaBox::DeserializeConfiguration(NanaBox::SerializeConfiguration(Config));
            if (!DefaultResources.AllowMemoryOvercommit || DefaultResources.EnableDeferredCommit ||
                DefaultResources.ProcessorLimit || DefaultResources.ProcessorWeight ||
                DefaultResources.StorageIopsMaximum || DefaultResources.StorageBandwidthMaximum) return 29;
            Config.ProcessorLimit = 75000;
            Config.ProcessorWeight = 250;
            Config.EnableDeferredCommit = true;
            Config.StorageIopsMaximum = 1000;
            Config.StorageBandwidthMaximum = 10485760;
            NanaBox::NetworkAdapterConfiguration Adapter;
            Adapter.Connected = true;
            Adapter.NetworkId = "85c8668e-36fe-447f-aae0-ba159a0142a7";
            Adapter.FirewallCreatorId = "06b34f94-82ae-4ad9-9f35-3a2a7da8508c";
            Config.NetworkAdapters.push_back(Adapter);
            Config.Gpu.AssignmentMode = NanaBox::GpuAssignmentMode::List;
            Config.Gpu.SelectedDevices["test-device"] = 65535;
            Config.VideoMonitor.EnableDpiScalingValueOverride = true;
            Config.VideoMonitor.OverriddenDpiScalingValue = 175;
            Config.VideoMonitor.EnableContentResizing = false;
            Config.VideoMonitor.ShowFullScreenModeConnectionBar = false;
            auto Loaded = NanaBox::DeserializeConfiguration(NanaBox::SerializeConfiguration(Config));
            if (Loaded.ProcessorLimit != 75000 || Loaded.ProcessorWeight != 250 ||
                !Loaded.AllowMemoryOvercommit || !Loaded.EnableDeferredCommit ||
                Loaded.StorageIopsMaximum != 1000 || Loaded.StorageBandwidthMaximum != 10485760) return 30;
            auto Hcs = nlohmann::json::parse(NanaBox::MakeHcsConfiguration(Loaded));
            auto const& Vm = Hcs.at("VirtualMachine");
            if (Vm.at("ComputeTopology").at("Processor").at("Limit") != 75000 ||
                Vm.at("ComputeTopology").at("Processor").at("Weight") != 250 ||
                Vm.at("ComputeTopology").at("Memory").at("EnableDeferredCommit") != true ||
                Vm.at("StorageQoS").at("IopsMaximum") != 1000 ||
                Vm.at("StorageQoS").at("BandwidthMaximum") != 10485760) return 31;
            for (auto const& Invalid : {nlohmann::json{{"ProcessorLimit", 100001}},
                nlohmann::json{{"ProcessorWeight", -1}}, nlohmann::json{{"StorageIopsMaximum", 1.5}},
                nlohmann::json{{"AllowMemoryOvercommit", "true"}},
                nlohmann::json{{"AllowMemoryOvercommit", false}, {"EnableDeferredCommit", true}}})
            {
                auto Bad = nlohmann::json::parse(NanaBox::SerializeConfiguration(Config));
                Bad["NanaBox"].update(Invalid);
                bool Rejected = false;
                try { NanaBox::DeserializeConfiguration(Bad.dump()); }
                catch (...) { Rejected = true; }
                if (!Rejected) return 32;
            }
            if (!Loaded.VideoMonitor.EnableDpiScalingValueOverride || Loaded.VideoMonitor.OverriddenDpiScalingValue != 175 ||
                Loaded.VideoMonitor.EnableContentResizing || Loaded.VideoMonitor.ShowFullScreenModeConnectionBar ||
                NanaBox::EffectiveDisplayScale(Loaded.VideoMonitor, 150, false) != 175) return 26;
            Loaded.VideoMonitor.DisableBasicSessionDpiScaling = true;
            if (NanaBox::EffectiveDisplayScale(Loaded.VideoMonitor, 150, true) != 100) return 27;
            if (Loaded.NetworkAdapters.size() != 1 ||
                Loaded.NetworkAdapters[0].NetworkId != Adapter.NetworkId ||
                Loaded.NetworkAdapters[0].FirewallCreatorId != Adapter.FirewallCreatorId ||
                Loaded.Gpu.SelectedDevices.at("test-device") != 65535) return 1;
            auto Request = NanaBox::MakeHcsGpuModification(Loaded.Gpu);
            if (Request["Settings"]["AssignmentMode"] != "List" ||
                Request["Settings"]["AssignmentRequest"]["test-device"] != 65535) return 2;
            Loaded.Gpu.AssignmentMode = NanaBox::GpuAssignmentMode::Mirror;
            if (NanaBox::MakeHcsGpuModification(Loaded.Gpu)["Settings"]["AssignmentMode"] != "Mirror") return 3;
            wchar_t Temp[MAX_PATH + 1]{};
            DWORD Length = ::GetTempPathW(ARRAYSIZE(Temp), Temp);
            if (!Length || Length >= ARRAYSIZE(Temp)) return 5;
            GUID Id{};
            winrt::check_hresult(::CoCreateGuid(&Id));
            std::wstring File = std::wstring(Temp) + ::FromGuid(Id).c_str() + L".7b";
            auto Cleanup = Mile::ScopeExitTaskHandler([&]() { ::DeleteFileW(File.c_str()); });
            std::string Original = NanaBox::SerializeConfiguration(Config);
            ::WriteAllTextToUtf8TextFile(File, Original);
            auto Expected = ::ReadAllTextFromUtf8TextFile(File);
            Config.Name = "Updated";
            auto Updated = NanaBox::SerializeConfiguration(Config);
            ::WriteAllTextToUtf8TextFile(File, Updated, &Expected);
            bool Conflict = false;
            try { ::WriteAllTextToUtf8TextFile(File, Original, &Expected); }
            catch (winrt::hresult_error const& Error)
            {
                Conflict = Error.code() == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            }
            if (!Conflict || NanaBox::DeserializeConfiguration(::ReadAllTextFromUtf8TextFile(File)).Name != "Updated") return 6;
            // A reader that disallows deletion must leave the previous file intact.
            winrt::file_handle Reader(::CreateFileW(File.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!Reader) return 7;
            bool Rejected = false;
            try { ::WriteAllTextToUtf8TextFile(File, Original); }
            catch (winrt::hresult_error const&) { Rejected = true; }
            Reader.close();
            if (!Rejected || NanaBox::DeserializeConfiguration(::ReadAllTextFromUtf8TextFile(File)).Name != "Updated") return 8;
            // Exercise the production reconciliation algorithm without modifying the host.
            auto Before = Adapter;
            Before.EndpointId = "a";
            auto After = Before;
            After.MacAddress = "00-15-5D-12-34-56";
            unsigned Adds = 0, Errors = 0;
            auto Report = [&](auto const&, char const*) { ++Errors; };
            auto Add = [&](auto& Value) { ++Adds; Value.Connected = true; };
            auto FailedRemoval = [&](auto&) { throw std::runtime_error("HCS removal failed"); };
            auto Result = NanaBox::ReconcileNetworkAdapters(
                std::vector{Before}, std::vector{After}, FailedRemoval, Add, Report);
            if (Adds || Errors != 1 || Result.size() != 1 ||
                !Result[0].Connected || Result[0].MacAddress != Before.MacAddress) return 19;
            auto FailedCleanup = [&](auto& Value) { Value.Connected = false; throw std::runtime_error("HCN deletion failed"); };
            Result = NanaBox::ReconcileNetworkAdapters(
                std::vector{Before}, std::vector{After}, FailedCleanup, Add, Report);
            if (Adds || Errors != 2 || Result.size() != 1 || Result[0].Connected) return 20;
            auto Remove = [](auto& Value) { Value.Connected = false; };
            Result = NanaBox::ReconcileNetworkAdapters(
                std::vector{Before}, std::vector{After}, Remove, Add, Report);
            if (Adds != 1 || Errors != 2 || Result.size() != 1 ||
                !Result[0].Connected || Result[0].MacAddress != After.MacAddress) return 21;
            auto FailedAdd = [](auto&) { throw std::runtime_error("HCS attachment failed"); };
            Result = NanaBox::ReconcileNetworkAdapters(
                std::vector{Before}, std::vector{After}, Remove, FailedAdd, Report);
            if (Errors != 3 || Result.size() != 1 || Result[0].Connected) return 22;
            auto DifferentNetwork = Before;
            DifferentNetwork.NetworkId = "different";
            if (NanaBox::SameNetworkAdapter(Before, DifferentNetwork)) return 23;
            if (!NanaBox::IsEndpointOwnedBy(LR"({"Owner":"SelfTest"})", "SelfTest") ||
                NanaBox::IsEndpointOwnedBy(LR"({"Owner":"Another"})", "SelfTest") ||
                NanaBox::IsEndpointOwnedBy(LR"({})", "SelfTest")) return 24;
            std::vector Duplicate{Before, Before};
            bool RejectedDuplicate = false;
            auto EndpointKey = winrt::to_string(::FromGuid(winrt::guid("85c8668e-36fe-447f-aae0-ba159a0142a7")));
            for (auto& Value : Duplicate) Value.EndpointId = EndpointKey;
            try { NanaBox::NormalizeNetworkAdapterIds(Duplicate); }
            catch (winrt::hresult_error const&) { RejectedDuplicate = true; }
            if (!RejectedDuplicate) return 25;
            for (bool Pending : {false, true})
            for (bool Restarting : {false, true})
            for (bool Restart : {false, true})
            for (bool Fail : {false, true})
            {
                bool P = Pending, R = Restarting, Caught = false;
                unsigned Calls = 0;
                try
                {
                    NanaBox::RequestVmStop(Restart, P, R, [&]()
                    {
                        ++Calls;
                        if (Fail) throw std::runtime_error("Injected stop failure");
                    });
                }
                catch (std::runtime_error const&) { Caught = true; }
                if (Calls != 1 || Caught != Fail || P != (Fail ? Pending : true) ||
                    R != (Fail ? Restarting : Restart)) return 28;
            }
            // Fault injection covers every forward operation in the save transaction.
            for (auto InitiallyPaused : {false, true})
            for (int Fail = 0; Fail <= 7; ++Fail)
            {
                bool Paused = InitiallyPaused, Gpu = true, Network = true;
                bool Saved = false, Published = false, Terminated = false;
                int Step = 0, RecoveryFailures = 0;
                auto Inject = [&]() { if (++Step == Fail) throw std::runtime_error("Injected power failure"); };
                NanaBox::SaveStateOperations Ops;
                Ops.ResumeVm = [&]() { Inject(); Paused = false; };
                Ops.PauseVm = [&]() { Inject(); Paused = true; };
                Ops.DetachNetwork = [&]() { Inject(); Network = false; };
                Ops.RestoreNetwork = [&]() { Network = true; };
                Ops.DisableGpu = [&]() { Inject(); Gpu = false; };
                Ops.RestoreGpu = [&]() { Gpu = true; };
                Ops.Save = [&]() { Inject(); Saved = true; };
                Ops.Publish = [&]() { Inject(); Published = true; };
                Ops.Unpublish = [&]() { Published = false; };
                Ops.RemoveSave = [&]() { Saved = false; };
                Ops.Terminate = [&]() { Inject(); Terminated = true; };
                Ops.RollbackError = [&](char const*) { ++RecoveryFailures; };
                try { NanaBox::SaveStateTransaction(InitiallyPaused, true, true, Ops); }
                catch (std::runtime_error const&)
                {
                    if (Paused != InitiallyPaused || !Gpu || !Network || Saved || Published ||
                        Terminated || RecoveryFailures) return 26;
                    continue;
                }
                if (!Paused || Gpu || Network || !Saved || !Published || !Terminated) return 27;
            }
            return 0;
        }
        catch (...) { return 4; }
    }

    if (0 == ::_wcsicmp(lpCmdLine, L"--disk-self-test"))
    {
        try
        {
            wchar_t Temp[MAX_PATH + 1]{};
            DWORD Length = ::GetTempPathW(ARRAYSIZE(Temp), Temp);
            if (!Length || Length >= ARRAYSIZE(Temp)) return 10;
            GUID Id{};
            winrt::check_hresult(::CoCreateGuid(&Id));
            std::wstring Path = Temp;
            Path += ::FromGuid(Id).c_str();
            Path += L".vhdx";
            auto Cleanup = Mile::ScopeExitTaskHandler([&]() { ::DeleteFileW(Path.c_str()); });
            HANDLE Disk = INVALID_HANDLE_VALUE;
            if (::SimpleCreateVirtualDisk(Path.c_str(), 32ull << 20, &Disk) != ERROR_SUCCESS) return 11;
            ::CloseHandle(Disk);
            if (::SimpleResizeVirtualDisk(Path.c_str(), 64ull << 20) != ERROR_SUCCESS) return 12;
            if (::SimpleCompactVirtualDisk(Path.c_str()) != ERROR_SUCCESS) return 13;
            VIRTUAL_STORAGE_TYPE Type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
            OPEN_VIRTUAL_DISK_PARAMETERS Open{};
            Open.Version = OPEN_VIRTUAL_DISK_VERSION_1;
            if (::OpenVirtualDisk(&Type, Path.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO,
                OPEN_VIRTUAL_DISK_FLAG_NONE, &Open, &Disk) != ERROR_SUCCESS) return 14;
            auto Close = Mile::ScopeExitTaskHandler([&]() { ::CloseHandle(Disk); });
            GET_VIRTUAL_DISK_INFO Info{};
            Info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
            ULONG Size = sizeof(Info);
            if (::GetVirtualDiskInformation(Disk, &Size, &Info, nullptr) != ERROR_SUCCESS) return 15;
            return Info.Size.VirtualSize == (64ull << 20) ? 0 : 16;
        }
        catch (...) { return 17; }
    }

    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    winrt::check_hresult(::SetCurrentProcessExplicitAppUserModelID(
        g_AppUserModelID.data()));

    ::SetProcessShutdownParameters(0x3FF, 0);

    winrt::com_ptr<winrt::NanaBox::implementation::App> App =
        winrt::make_self<winrt::NanaBox::implementation::App>();
    auto ExitHandler = Mile::ScopeExitTaskHandler([&]()
    {
        App->Close();
    });

    // The configuration editor also works on computers without Hyper-V.
    int ArgumentCount = 0;
    auto Arguments = ::CommandLineToArgvW(::GetCommandLineW(), &ArgumentCount);
    if (Arguments)
    {
        auto FreeArguments = Mile::ScopeExitTaskHandler([&]() { ::LocalFree(Arguments); });
        if (ArgumentCount >= 2 && 0 == ::_wcsicmp(Arguments[1], L"--query-vm"))
        {
            if (ArgumentCount != 4) return ERROR_INVALID_PARAMETER;
            try
            {
                auto Vm = winrt::make_self<NanaBox::ComputeSystem>(winrt::hstring(Arguments[2]));
                auto Properties = nlohmann::json::parse(winrt::to_string(Vm->GetProperties()));
                std::string Content = Properties.dump(2);
                ::WriteAllTextToUtf8TextFile(::GetAbsolutePath(Arguments[3]), Content);
                return 0;
            }
            catch (...) { return 18; }
        }
        // Exercise the real XAML toolbar without creating a VM or RDP connection.
        if (ArgumentCount >= 2 && 0 == ::_wcsicmp(Arguments[1], L"--test-toolbar"))
        {
            if (ArgumentCount != 3) return ERROR_INVALID_PARAMETER;
            try
            {
                struct CommandLog
                {
                    std::wstring Path;
                    std::string Content;
                    bool Failed = false;
                } Log{::GetAbsolutePath(Arguments[2]), {}};
                ::WriteAllTextToUtf8TextFile(Log.Path, Log.Content);
                HWND WindowHandle = ::CreateXamlWindow(nullptr, 0, WS_OVERLAPPEDWINDOW);
                if (!WindowHandle) return -1;
                auto CloseWindow = Mile::ScopeExitTaskHandler([&]()
                {
                    if (::IsWindow(WindowHandle)) ::DestroyWindow(WindowHandle);
                });
                if (!::SetWindowSubclass(WindowHandle,
                    [](HWND Window, UINT Message, WPARAM WParam, LPARAM LParam,
                        UINT_PTR, DWORD_PTR Reference) -> LRESULT
                    {
                        if (Message == WM_COMMAND && HIWORD(WParam) == BN_CLICKED &&
                            LOWORD(WParam) >= NanaBox::MainWindowCommands::EnterBasicSession &&
                            LOWORD(WParam) <= NanaBox::MainWindowCommands::Sponsor)
                        {
                            auto& State = *reinterpret_cast<CommandLog*>(Reference);
                            try
                            {
                                State.Content += std::to_string(LOWORD(WParam)) + "\n";
                                ::WriteAllTextToUtf8TextFile(State.Path, State.Content);
                            }
                            catch (...)
                            {
                                State.Failed = true;
                                ::PostMessageW(Window, WM_CLOSE, 0, 0);
                            }
                            return 0;
                        }
                        return ::DefSubclassProc(Window, Message, WParam, LParam);
                    }, 1, reinterpret_cast<DWORD_PTR>(&Log))) return -1;
                auto Page = winrt::make<winrt::NanaBox::implementation::MainWindowControl>(
                    WindowHandle, false);
                if (FAILED(::MileXamlSetXamlContentForContentWindow(WindowHandle, winrt::get_abi(Page))))
                    return -1;
                ::SetWindowTextW(WindowHandle, L"NanaBox Toolbar Test");
                auto Result = ::ShowXamlWindow(WindowHandle, 1000, 220, nullptr);
                return Log.Failed ? -1 : Result;
            }
            catch (...) { return -1; }
        }
        if (ArgumentCount >= 2 && 0 == ::_wcsicmp(Arguments[1], L"--edit-config"))
        {
            if (ArgumentCount != 3) return ERROR_INVALID_PARAMETER;
            try
            {
                std::wstring Path = ::GetAbsolutePath(Arguments[2]);
                HWND WindowHandle = ::CreateXamlWindow(nullptr, 0, WS_OVERLAPPEDWINDOW);
                if (!WindowHandle) return -1;
                auto Page = winrt::make<winrt::NanaBox::implementation::VirtualMachineSettingsPage>(
                    WindowHandle, Path);
                if (FAILED(::MileXamlSetXamlContentForContentWindow(WindowHandle, winrt::get_abi(Page))))
                {
                    ::DestroyWindow(WindowHandle);
                    return -1;
                }
                return ::ShowXamlWindow(WindowHandle, 900, 700, nullptr);
            }
            catch (...)
            {
                ::ShowErrorMessageDialog(Mile::WinRT::ToHResultError());
                return -1;
            }
        }
    }

    std::wstring ApplicationName;
    std::map<std::wstring, std::wstring> OptionsAndParameters;
    std::wstring UnresolvedCommandLine;

    ::SplitCommandLineEx(
        std::wstring(::GetCommandLineW()),
        std::vector<std::wstring>{ L"-", L"/", L"--" },
        std::vector<std::wstring>{ L"=", L":" },
        ApplicationName,
        OptionsAndParameters,
        UnresolvedCommandLine);

    bool AcquireSponsorEdition = false;

    for (auto& Current : OptionsAndParameters)
    {
        if (0 == ::_wcsicmp(Current.first.c_str(), L"AcquireSponsorEdition"))
        {
            AcquireSponsorEdition = true;
        }
    }

    if (AcquireSponsorEdition)
    {
        HWND WindowHandle = ::CreateXamlWindow(
            nullptr,
            WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        if (!WindowHandle)
        {
            return -1;
        }

        if (FAILED(::MileAllowNonClientDefaultDrawingForWindow(
            WindowHandle,
            FALSE)))
        {
            return -1;
        }

        winrt::NanaBox::SponsorPage Window =
            winrt::make<winrt::NanaBox::implementation::SponsorPage>(
                WindowHandle);
        if (FAILED(::MileXamlSetXamlContentForContentWindow(
            WindowHandle,
            winrt::get_abi(Window))))
        {
            ::DestroyWindow(WindowHandle);
            return -1;
        }

        ::ExitProcess(::ShowXamlWindow(WindowHandle, 460, 320, nullptr));
    }

    bool PackagedMode = Mile::WinRT::IsPackagedMode();
    std::wstring TargetBinaryPath;

    if (PackagedMode)
    {
        try
        {
            std::wstring AppBinaryPath;
            {
                std::wstring RawPath = ::GetCurrentProcessModulePath();
                std::wcsrchr(&RawPath[0], L'\\')[0] = L'\0';
                RawPath.resize(std::wcslen(RawPath.c_str()));
                AppBinaryPath = RawPath;
            }

            std::wstring TempBinaryPath;
            {
                std::wstring RawPath = ::GetLocalStateFolderPath();
                GUID TempFolderGuid;
                winrt::check_hresult(::CoCreateGuid(&TempFolderGuid));
                RawPath.append(L"\\");
                RawPath.append(::FromGuid(TempFolderGuid));
                TempBinaryPath = RawPath;
            }

            winrt::check_bool(::CreateDirectoryW(
                TempBinaryPath.c_str(),
                nullptr));

            winrt::check_bool(::CopyFileW(
                (AppBinaryPath + L"\\NanaBox.exe").c_str(),
                (TempBinaryPath + L"\\NanaBox.exe").c_str(),
                FALSE));

            winrt::check_bool(::CopyFileW(
                (AppBinaryPath + L"\\resources.pri").c_str(),
                (TempBinaryPath + L"\\resources.pri").c_str(),
                FALSE));

            winrt::check_bool(::CopyFileW(
                (AppBinaryPath + L"\\Mile.Xaml.Styles.SunValley.xbf").c_str(),
                (TempBinaryPath + L"\\Mile.Xaml.Styles.SunValley.xbf").c_str(),
                FALSE));

            TargetBinaryPath = TempBinaryPath;
        }
        catch (...)
        {
            winrt::hresult_error Exception = Mile::WinRT::ToHResultError();
            ::ShowErrorMessageDialog(Exception);
            ::ExitProcess(Exception.code());
        }
    }

    if (!::MileIsCurrentProcessElevated() || PackagedMode)
    {
        try
        {
            if (PackagedMode && !TargetBinaryPath.empty())
            {
                ApplicationName = TargetBinaryPath + L"\\NanaBox.exe";
            }
            else
            {
                ApplicationName = ::GetCurrentProcessModulePath();
            }

            SHELLEXECUTEINFOW Information = {};
            Information.cbSize = sizeof(SHELLEXECUTEINFOW);
            Information.fMask = SEE_MASK_NOCLOSEPROCESS;
            Information.lpVerb = L"runas";
            Information.nShow = nShowCmd;
            Information.lpFile = ApplicationName.c_str();
            Information.lpParameters = UnresolvedCommandLine.c_str();
            winrt::check_bool(::ShellExecuteExW(&Information));
            ::WaitForSingleObjectEx(Information.hProcess, INFINITE, FALSE);
            ::CloseHandle(Information.hProcess);

            if (PackagedMode && !TargetBinaryPath.empty())
            {
                ::SimpleRemoveDirectory(TargetBinaryPath.c_str());
            }
        }
        catch (...)
        {
            winrt::hresult_error Exception = Mile::WinRT::ToHResultError();

            if (Exception.code() != ::HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                ::ShowErrorMessageDialog(Exception);
            }

            if (PackagedMode && !TargetBinaryPath.empty())
            {
                ::SimpleRemoveDirectory(TargetBinaryPath.c_str());
            }

            ::ExitProcess(Exception.code());
        }

        ::ExitProcess(0);
    }

    ::PrerequisiteCheck();

    std::wstring ConfigurationFilePath;

    if (!UnresolvedCommandLine.empty())
    {
        ConfigurationFilePath = ::GetAbsolutePath(UnresolvedCommandLine);
    }
    else
    {
        HWND WindowHandle = ::CreateXamlWindow(
            nullptr,
            WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        if (!WindowHandle)
        {
            return -1;
        }

        if (FAILED(::MileAllowNonClientDefaultDrawingForWindow(
            WindowHandle,
            FALSE)))
        {
            return -1;
        }

        winrt::NanaBox::QuickStartPage Window =
            winrt::make<winrt::NanaBox::implementation::QuickStartPage>(
                WindowHandle,
                &ConfigurationFilePath);
        if (FAILED(::MileXamlSetXamlContentForContentWindow(
            WindowHandle,
            winrt::get_abi(Window))))
        {
            ::DestroyWindow(WindowHandle);
            return -1;
        }

        ::ShowXamlWindow(WindowHandle, 460, 460, nullptr);
        if (ConfigurationFilePath.empty())
        {
            return 0;
        }
    }

    {
        std::wstring CurrentPath = ConfigurationFilePath;
        std::wcsrchr(&CurrentPath[0], L'\\')[0] = L'\0';
        CurrentPath.resize(std::wcslen(CurrentPath.c_str()));
        ::SetCurrentDirectoryW(CurrentPath.c_str());
    }

    WTL::CMessageLoop MessageLoop;

    ::SystemAtlAxWinInit();

    g_Module.Init(nullptr, hInstance);
    g_Module.AddMessageLoop(&MessageLoop);

    NanaBox::MainWindow MainWindow(ConfigurationFilePath);
    if (!MainWindow.Create(
        nullptr,
        MainWindow.rcDefault,
        L"NanaBox",
        WS_OVERLAPPEDWINDOW))
    {
        return -1;
    }

    MainWindow.ShowWindow(nShowCmd);
    MainWindow.UpdateWindow();

    int Result = MessageLoop.Run();

    g_Module.RemoveMessageLoop();
    g_Module.Term();

    return Result;
}
