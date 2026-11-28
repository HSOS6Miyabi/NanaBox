#include "pch.h"
#include "ResizeVirtualHardDiskPage.h"
#if __has_include("ResizeVirtualHardDiskPage.g.cpp")
#include "ResizeVirtualHardDiskPage.g.cpp"
#endif

#include "Utils.h"

#include <ShObjIdl_core.h>
#include <Shlwapi.h>
#include <limits>

namespace winrt
{
    using Windows::System::DispatcherQueuePriority;
}

namespace winrt::NanaBox::implementation
{
    ResizeVirtualHardDiskPage::ResizeVirtualHardDiskPage(
        _In_ HWND WindowHandle)
        : m_WindowHandle(WindowHandle)
    {
        ::SetWindowTextW(
            this->m_WindowHandle,
            Mile::WinRT::GetLocalizedString(
                L"ResizeVirtualHardDiskPage/GridTitleTextBlock/Text").c_str());
    }

    void ResizeVirtualHardDiskPage::InitializeComponent()
    {
        ResizeVirtualHardDiskPageT::InitializeComponent();

        this->m_DispatcherQueue =
            winrt::DispatcherQueue::GetForCurrentThread();
    }

    void ResizeVirtualHardDiskPage::NaturalNumberTextBoxBeforeTextChanging(
        winrt::TextBox const& sender,
        winrt::TextBoxBeforeTextChangingEventArgs const& args)
    {
        if (args.NewText().empty())
        {
            sender.Text(L"0");
            sender.SelectionStart(1);
            return;
        }

        for (const wchar_t& WideCharacter : args.NewText())
        {
            if (L'0' > WideCharacter || L'9' < WideCharacter)
            {
                // Don't allow non-digit characters.
                args.Cancel(true);
                return;
            }
        }
    }

    void ResizeVirtualHardDiskPage::FileNameBrowseButtonClickHandler(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);
        try
        {
            COMDLG_FILTERSPEC SupportedFileTypes[] =
            {
                {
                    L"Virtual Hard Disk Files (*.vhd;*.vhdx)",
                    L"*.vhd;*.vhdx"
                }
            };
            LPWSTR RawFilePath = nullptr;
            HRESULT Result = ::ShowFileDialog(
                this->m_WindowHandle,
                FALSE,
                ARRAYSIZE(SupportedFileTypes),
                SupportedFileTypes,
                nullptr,
                nullptr,
                nullptr,
                &RawFilePath);
            if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
            winrt::check_hresult(Result);
            if (!RawFilePath) throw winrt::hresult_error(E_UNEXPECTED);
            auto FreeFilePath = Mile::ScopeExitTaskHandler([RawFilePath]()
            {
                ::CoTaskMemFree(RawFilePath);
            });
            std::wstring FilePath(RawFilePath);
            if (FilePath.empty())
            {
                return;
            }

            this->FileNameTextBox().Text(FilePath);
        }
        catch (...)
        {
            ::ShowErrorMessageDialog(this->m_WindowHandle, Mile::WinRT::ToHResultError());
        }
    }

    void ResizeVirtualHardDiskPage::ResizeButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        const std::uint64_t UnitFactors[] =
        {
            1ULL, // B
            1000ULL, // KB
            (1ULL << 10), // KiB
            1000000ULL, // MB
            (1ULL << 20), // MiB
            1000000000ULL, // GB
            (1ULL << 30), // GiB
            1000000000000ULL, // RB
            (1ULL << 40), // TiB
        };
        const std::size_t MaximumUnitFactorsIndex =
            sizeof(UnitFactors) / sizeof(*UnitFactors) - 1;

        winrt::hstring Path =
            this->FileNameTextBox().Text();
        std::uint64_t Size = 0;
        try
        {
            if (Path.empty() || (::_wcsicmp(::PathFindExtensionW(Path.c_str()), L".vhd") != 0 &&
                ::_wcsicmp(::PathFindExtensionW(Path.c_str()), L".vhdx") != 0))
                throw winrt::hresult_invalid_argument(L"Specify a VHD or VHDX file.");
            auto Text = std::wstring(this->SizeTextBox().Text().c_str());
            size_t End = 0;
            auto OriginalSize = std::stoull(Text, &End);
            auto UnitIndex = this->SizeUnitComboBox().SelectedIndex();
            if (End != Text.size() || UnitIndex < 0 ||
                static_cast<size_t>(UnitIndex) > MaximumUnitFactorsIndex ||
                OriginalSize > (std::numeric_limits<std::uint64_t>::max)() / UnitFactors[UnitIndex])
                throw winrt::hresult_invalid_argument(L"Invalid virtual disk capacity.");
            Size = OriginalSize * UnitFactors[UnitIndex];
            auto Limit = ::_wcsicmp(::PathFindExtensionW(Path.c_str()), L".vhd") == 0 ? 2040ull << 30 : 64ull << 40;
            if (Size < (3ull << 20) || Size > Limit || Size % 512)
                throw winrt::hresult_invalid_argument(L"Invalid virtual disk capacity.");
        }
        catch (...)
        {
            ::MessageBoxW(this->m_WindowHandle,
                L"Specify a valid VHD/VHDX path and capacity.",
                Mile::WinRT::GetLocalizedString(L"Messages/ErrorInstructionText").c_str(),
                MB_OK | MB_ICONERROR);
            return;
        }

        winrt::hstring SuccessInstructionText =
            Mile::WinRT::GetLocalizedString(
                L"ResizeVirtualHardDiskPage/SuccessInstructionText");
        winrt::hstring SuccessContentText =
            Mile::WinRT::GetLocalizedString(
                L"ResizeVirtualHardDiskPage/SuccessContentText");

        winrt::handle(Mile::CreateThread([=]()
        {
            HWND WaitingHandle = ::ShowOperationWaitingWindow(
                this->m_WindowHandle);

            DWORD Error = ::SimpleResizeVirtualDisk(
                Path.c_str(),
                Size);

            if (ERROR_SUCCESS == Error)
            {
                ::ShowMessageDialog(
                    this->m_WindowHandle,
                    SuccessInstructionText.c_str(),
                    Mile::FormatWideString(
                        SuccessContentText.c_str(),
                        Path.c_str()).c_str());

                ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
            }
            else
            {
                ::ShowErrorMessageDialog(
                    this->m_WindowHandle,
                    winrt::hresult_error(HRESULT_FROM_WIN32(Error)));
            }

            ::SendMessageW(WaitingHandle, WM_CLOSE, 0, 0);
        }));
    }

    void ResizeVirtualHardDiskPage::CancelButtonClick(
        winrt::IInspectable const& sender,
        winrt::RoutedEventArgs const& e)
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(e);

        ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
    }

}
