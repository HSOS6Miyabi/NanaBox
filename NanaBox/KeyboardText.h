#pragma once

#include <array>
#include <algorithm>
#include <optional>
#include <string>
#include <Windows.h>
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
        bool Extended = false;
    };

    using KeyboardTextBatch = std::vector<KeyboardTextEvent>;

    inline bool ValidClipboardKeyboardLayout(std::string_view Layout)
    {
        return Layout == "Host" || (Layout.size() == 8 &&
            std::all_of(Layout.begin(), Layout.end(), [](char Ch)
            { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f') || (Ch >= 'A' && Ch <= 'F'); }));
    }

    struct KeyboardTextStroke
    {
        std::uint8_t Code;
        bool Extended = false;
        bool Shift = false;
        bool AltGr = false;
    };

    template<class Lookup>
    inline std::vector<KeyboardTextBatch> EncodeKeyboardTextWithLayout(std::wstring_view Text, Lookup Map)
    {
        if (Text.size() > 4096)
            throw std::invalid_argument("Keyboard input is limited to 4096 UTF-16 code units.");
        std::vector<KeyboardTextBatch> Result;
        for (std::size_t I = 0; I < Text.size(); ++I)
        {
            std::optional<KeyboardTextStroke> Stroke;
            auto Ch = Text[I];
            if (Ch == L'\r' || Ch == L'\n')
            {
                Stroke = KeyboardTextStroke{0x1c};
                if (Ch == L'\r' && I + 1 < Text.size() && Text[I + 1] == L'\n') ++I;
            }
            else if (Ch == L'\t') Stroke = KeyboardTextStroke{0x0f};
            else if (Ch >= 32 && Ch != 127 && !(Ch >= 0xd800 && Ch <= 0xdfff))
                Stroke = Map(Ch);
            if (!Stroke || !Stroke->Code)
                throw std::invalid_argument("A clipboard character cannot be typed with the selected keyboard layout.");
            std::size_t Count = 2 + (Stroke->Shift ? 2 : 0) + (Stroke->AltGr ? 2 : 0);
            if (Result.empty() || Result.back().size() + Count > 20) Result.emplace_back();
            auto& Batch = Result.back();
            if (Stroke->Shift) Batch.push_back({0x2a, false});
            if (Stroke->AltGr) Batch.push_back({0x38, false, true});
            Batch.push_back({Stroke->Code, false, Stroke->Extended});
            Batch.push_back({Stroke->Code, true, Stroke->Extended});
            if (Stroke->AltGr) Batch.push_back({0x38, true, true});
            if (Stroke->Shift) Batch.push_back({0x2a, true});
        }
        return Result;
    }

    inline std::optional<KeyboardTextStroke> MapLayoutCharacter(wchar_t Ch, HKL Layout)
    {
        SHORT Value = ::VkKeyScanExW(Ch, Layout);
        if (Value == -1) return {};
        auto Key = static_cast<UINT>(LOBYTE(Value));
        auto Modifiers = static_cast<unsigned>(HIBYTE(Value));
        // CTRL or ALT alone denotes a shortcut, not printable text.
        if (Modifiers != 0 && Modifiers != 1 && Modifiers != 6 && Modifiers != 7) return {};
        UINT Scan = ::MapVirtualKeyExW(Key, MAPVK_VK_TO_VSC_EX, Layout);
        if (!LOBYTE(Scan) || (HIBYTE(Scan) && HIBYTE(Scan) != 0xe0)) return {};
        BYTE State[256]{};
        if (Modifiers & 1) State[VK_SHIFT] = State[VK_LSHIFT] = 0x80;
        if ((Modifiers & 6) == 6)
        {
            State[VK_CONTROL] = State[VK_LCONTROL] = 0x80;
            State[VK_MENU] = State[VK_RMENU] = 0x80;
        }
        wchar_t Buffer[8]{};
        // Bit 2 keeps the local keyboard's dead-key state unchanged.
        int Count = ::ToUnicodeEx(Key, LOBYTE(Scan), State, Buffer, 8, 4, Layout);
        if (Count != 1 || Buffer[0] != Ch) return {};
        return KeyboardTextStroke{LOBYTE(Scan), HIBYTE(Scan) == 0xe0,
            (Modifiers & 1) != 0, (Modifiers & 6) == 6};
    }

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
            else if (auto ShiftedPosition = Shifted.find(Ch); ShiftedPosition != std::wstring_view::npos)
            { Code = Codes[ShiftedPosition]; Shift = true; }
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

    inline std::vector<KeyboardTextBatch> EncodeKeyboardText(std::wstring_view Text, std::string_view Layout)
    {
        if (!ValidClipboardKeyboardLayout(Layout)) throw std::invalid_argument("Invalid clipboard keyboard layout identifier.");
        // The fixed US mapping also works in a firmware console.
        if (Layout == "00000409") return EncodeKeyboardText(Text);
        HKL Handle = nullptr;
        if (Layout == "Host") Handle = ::GetKeyboardLayout(0);
        else
        {
            std::wstring Id(Layout.begin(), Layout.end());
            Handle = ::LoadKeyboardLayoutW(Id.c_str(), 0);
            if (!Handle) throw std::invalid_argument("The selected keyboard layout is not installed on the host.");
        }
        if (!Handle) throw std::invalid_argument("The keyboard layout is unavailable.");
        return EncodeKeyboardTextWithLayout(Text, [&](wchar_t Ch) { return MapLayoutCharacter(Ch, Handle); });
    }

}
