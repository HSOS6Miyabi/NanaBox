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
#include "QuickStartPage.h"
#include "VirtualMachineSettingsPage.h"
#include <shellapi.h>
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
            NanaBox::NetworkAdapterConfiguration Adapter;
            Adapter.Connected = true;
            Adapter.NetworkId = "85c8668e-36fe-447f-aae0-ba159a0142a7";
            Adapter.FirewallCreatorId = "06b34f94-82ae-4ad9-9f35-3a2a7da8508c";
            Config.NetworkAdapters.push_back(Adapter);
            Config.Gpu.AssignmentMode = NanaBox::GpuAssignmentMode::List;
            Config.Gpu.SelectedDevices["test-device"] = 65535;
            auto Loaded = NanaBox::DeserializeConfiguration(NanaBox::SerializeConfiguration(Config));
            if (Loaded.NetworkAdapters.size() != 1 ||
                Loaded.NetworkAdapters[0].NetworkId != Adapter.NetworkId ||
                Loaded.NetworkAdapters[0].FirewallCreatorId != Adapter.FirewallCreatorId ||
                Loaded.Gpu.SelectedDevices.at("test-device") != 65535) return 1;
            auto Request = NanaBox::MakeHcsGpuModification(Loaded.Gpu);
            if (Request["Settings"]["AssignmentMode"] != "List" ||
                Request["Settings"]["AssignmentRequest"]["test-device"] != 65535) return 2;
            Loaded.Gpu.AssignmentMode = NanaBox::GpuAssignmentMode::Mirror;
            if (NanaBox::MakeHcsGpuModification(Loaded.Gpu)["Settings"]["AssignmentMode"] != "Mirror") return 3;
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
