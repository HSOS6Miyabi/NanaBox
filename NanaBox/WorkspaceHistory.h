#pragma once

#include "Utils.h"
#include <Mile.Helpers.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace NanaBox::WorkspaceHistory
{
    inline constexpr wchar_t DefaultKey[] = L"Software\\NanaBox\\WorkspaceHistory";
    inline constexpr std::size_t MaximumEntries = 30;

    inline bool SamePath(std::wstring const& First, std::wstring const& Second)
    {
        return ::CompareStringOrdinal(First.c_str(), -1, Second.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    inline std::vector<std::wstring> Read(std::wstring const& Key = DefaultKey)
    {
        DWORD Bytes = 0;
        auto Status = ::RegGetValueW(HKEY_CURRENT_USER, Key.c_str(), L"Paths", RRF_RT_REG_MULTI_SZ,
            nullptr, nullptr, &Bytes);
        if (Status == ERROR_FILE_NOT_FOUND || Status == ERROR_PATH_NOT_FOUND) return {};
        winrt::check_hresult(HRESULT_FROM_WIN32(Status));
        if (Bytes < 2 * sizeof(wchar_t) || Bytes > 4 * 1024 * 1024 || Bytes % sizeof(wchar_t))
            return {};
        std::vector<wchar_t> Buffer(Bytes / sizeof(wchar_t) + 2, L'\0');
        Status = ::RegGetValueW(HKEY_CURRENT_USER, Key.c_str(), L"Paths", RRF_RT_REG_MULTI_SZ,
            nullptr, Buffer.data(), &Bytes);
        if (Status == ERROR_MORE_DATA || Status == ERROR_FILE_NOT_FOUND) return {};
        winrt::check_hresult(HRESULT_FROM_WIN32(Status));
        std::vector<std::wstring> Paths;
        for (std::size_t Offset = 0; Offset < Buffer.size() && Buffer[Offset] && Paths.size() < MaximumEntries;)
        {
            std::wstring Path(Buffer.data() + Offset);
            Offset += Path.size() + 1;
            if (!Path.empty() && Path.size() < 32767 &&
                std::none_of(Paths.begin(), Paths.end(), [&](auto const& Entry) { return SamePath(Entry, Path); }))
                Paths.push_back(std::move(Path));
        }
        return Paths;
    }

    inline void Write(std::wstring const& Key, std::vector<std::wstring> const& Paths)
    {
        HKEY Handle = nullptr;
        auto Status = ::RegCreateKeyExW(HKEY_CURRENT_USER, Key.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &Handle, nullptr);
        winrt::check_hresult(HRESULT_FROM_WIN32(Status));
        auto Close = Mile::ScopeExitTaskHandler([Handle]() { ::RegCloseKey(Handle); });
        std::vector<wchar_t> Data;
        for (auto const& Path : Paths)
        {
            Data.insert(Data.end(), Path.begin(), Path.end());
            Data.push_back(L'\0');
        }
        Data.push_back(L'\0');
        if (Paths.empty()) Data.push_back(L'\0');
        winrt::check_hresult(HRESULT_FROM_WIN32(::RegSetValueExW(Handle, L"Paths", 0,
            REG_MULTI_SZ, reinterpret_cast<BYTE const*>(Data.data()),
            static_cast<DWORD>(Data.size() * sizeof(wchar_t)))));
    }

    template<typename Operation>
    inline void Modify(std::wstring const& Key, Operation Change)
    {
        winrt::handle Mutex(::CreateMutexW(nullptr, FALSE, L"Local\\NanaBox.WorkspaceHistory"));
        winrt::check_pointer(Mutex.get());
        auto Wait = ::WaitForSingleObject(Mutex.get(), 2000);
        if (Wait != WAIT_OBJECT_0 && Wait != WAIT_ABANDONED)
            winrt::throw_hresult(HRESULT_FROM_WIN32(Wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError()));
        auto Unlock = Mile::ScopeExitTaskHandler([&]() { ::ReleaseMutex(Mutex.get()); });
        auto Paths = Read(Key);
        Change(Paths);
        if (Paths.size() > MaximumEntries) Paths.resize(MaximumEntries);
        Write(Key, Paths);
    }

    inline void Remember(std::wstring const& Path, std::wstring const& Key = DefaultKey)
    {
        if (Path.empty()) return;
        auto Absolute = ::GetAbsolutePath(Path);
        Modify(Key, [&](auto& Paths)
        {
            std::erase_if(Paths, [&](auto const& Entry) { return SamePath(Entry, Absolute); });
            Paths.insert(Paths.begin(), Absolute);
        });
    }

    inline void TryRemember(std::wstring const& Path, std::wstring const& Key = DefaultKey) noexcept
    {
        try { Remember(Path, Key); } catch (...) {}
    }

    inline void Forget(std::wstring const& Path, std::wstring const& Key = DefaultKey)
    {
        Modify(Key, [&](auto& Paths)
        {
            std::erase_if(Paths, [&](auto const& Entry) { return SamePath(Entry, Path); });
        });
    }

    inline void Clear(std::wstring const& Key = DefaultKey)
    {
        Modify(Key, [](auto& Paths) { Paths.clear(); });
    }
}
