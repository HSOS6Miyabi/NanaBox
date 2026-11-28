#pragma once
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NanaBox::Wddm
{
    inline std::wstring Fold(std::wstring_view value)
    {
        std::wstring result(value);
        for (auto& ch : result)
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        return result;
    }

    inline bool SafeComponent(std::wstring_view value)
    {
        if (value.empty() || value.size() > 255 || value == L"." || value == L".." ||
            value.back() == L'.' || value.back() == L' ') return false;
        for (auto ch : value)
            if (ch < 32 || ch == 127 || std::wstring_view(L"\\/:*?\"<>|;").find(ch) != std::wstring_view::npos)
                return false;
        auto stem = Fold(value.substr(0, value.find(L'.')));
        while (!stem.empty() && stem.back() == L' ') stem.pop_back();
        if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
            stem == L"conin$" || stem == L"conout$") return false;
        if (stem.size() == 4 && (stem.substr(0, 3) == L"com" || stem.substr(0, 3) == L"lpt") &&
            ((stem[3] >= L'0' && stem[3] <= L'9') || stem[3] == L'\u00b9' ||
             stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
        return true;
    }

    inline bool SafeRelativePath(std::wstring_view value)
    {
        if (value.empty() || value.size() > 2048) return false;
        while (true)
        {
            auto end = value.find(L'\\');
            if (!SafeComponent(value.substr(0, end))) return false;
            if (end == std::wstring_view::npos) return true;
            value.remove_prefix(end + 1);
        }
    }

    inline bool SafeSystemDirectory(std::wstring_view value)
    {
        if (!SafeRelativePath(value)) return false;
        auto lower = Fold(value);
        auto first = lower.substr(0, lower.find(L'\\'));
        if (first != L"system32" && first != L"syswow64") return false;
        // The mounted repositories must not be used as destinations.
        for (auto const& blocked : {L"system32\\driverstore", L"system32\\hostdriverstore",
            L"system32\\hostdriverstoreextras", L"syswow64\\driverstore"})
            if (lower == blocked || lower.rfind(std::wstring(blocked) + L"\\", 0) == 0) return false;
        return true;
    }

    inline bool ExtraShareName(std::string_view value)
    {
        constexpr std::string_view prefix = "NanaBox.GpuExtra.";
        if (value.size() != prefix.size() + 16 || value.substr(0, prefix.size()) != prefix) return false;
        for (auto ch : value.substr(prefix.size()))
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        return true;
    }

    struct Mapping
    {
        std::wstring Source;
        std::wstring Destination;
        std::wstring Directory;
        bool WhenNewer;
    };

    inline std::optional<Mapping> MakeMapping(std::wstring_view package,
        std::vector<std::wstring> const& values, bool wow64, bool newer)
    {
        if (!SafeComponent(package) || values.empty() || values.size() > 2) return {};
        auto source = values.front();
        std::replace(source.begin(), source.end(), L'/', L'\\');
        if (!SafeRelativePath(source)) return {};
        auto separator = source.find_last_of(L'\\');
        auto destination = values.size() == 2 && !values[1].empty() ?
            values[1] : source.substr(separator == std::wstring::npos ? 0 : separator + 1);
        if (!SafeComponent(destination)) return {};
        return Mapping{std::wstring(package) + L"\\" + source, destination,
            wow64 ? L"SysWOW64" : L"System32", newer};
    }
}
