#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace NanaBox
{
    struct KeyboardTextEvent
    {
        std::uint8_t ScanCode;
        bool KeyUp;
    };

    using KeyboardTextBatch = std::vector<KeyboardTextEvent>;

    // RDP SendKeys accepts physical scan codes and at most 20 events per call.
    // Use the US keyboard layout; each batch ends with all injected keys released.
    inline std::vector<KeyboardTextBatch> EncodeKeyboardText(std::wstring_view Text)
    {
        if (Text.size() > 4096)
            throw std::invalid_argument("Keyboard input is limited to 4096 characters.");
        constexpr std::array<std::uint8_t, 26> Letters = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
            0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
            0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
        constexpr std::wstring_view Plain = L"1234567890-=[]\\;',./` ";
        constexpr std::wstring_view Shifted = L"!@#$%^&*()_+{}|:\"<>?~";
        constexpr std::array<std::uint8_t, 22> Codes = {
            0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
            0x0c, 0x0d, 0x1a, 0x1b, 0x2b, 0x27, 0x28, 0x33, 0x34, 0x35, 0x29, 0x39};
        std::vector<KeyboardTextBatch> Result;
        for (std::size_t I = 0; I < Text.size(); ++I)
        {
            auto Ch = Text[I];
            std::uint8_t Code = 0;
            bool Shift = false;
            if (Ch >= L'a' && Ch <= L'z') Code = Letters[Ch - L'a'];
            else if (Ch >= L'A' && Ch <= L'Z') { Code = Letters[Ch - L'A']; Shift = true; }
            else if (Ch == L'\r' || Ch == L'\n')
            {
                Code = 0x1c;
                if (Ch == L'\r' && I + 1 < Text.size() && Text[I + 1] == L'\n') ++I;
            }
            else if (Ch == L'\t') Code = 0x0f;
            else if (auto Position = Plain.find(Ch); Position != std::wstring_view::npos)
                Code = Codes[Position];
            else if (auto Position = Shifted.find(Ch); Position != std::wstring_view::npos)
            { Code = Codes[Position]; Shift = true; }
            else throw std::invalid_argument("Keyboard input supports printable ASCII, tabs and line breaks (US keyboard layout).");
            if (Result.empty() || Result.back().size() + (Shift ? 4 : 2) > 20)
                Result.emplace_back();
            auto& Batch = Result.back();
            if (Shift) Batch.push_back({0x2a, false});
            Batch.push_back({Code, false});
            Batch.push_back({Code, true});
            if (Shift) Batch.push_back({0x2a, true});
        }
        return Result;
    }
}
