#pragma once

#include <array>
#include <algorithm>
#include <optional>
#include <string>
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
    inline std::vector<KeyboardTextBatch> EncodeKeyboardTextWithLayout(
        std::wstring_view Text, Lookup Map, bool GtkUnicode = false)
    {
        if (Text.size() > 4096)
            throw std::invalid_argument("Keyboard input is limited to 4096 UTF-16 code units.");
        std::vector<KeyboardTextBatch> Result;
        auto Reserve = [&](std::size_t Count) -> KeyboardTextBatch&
        {
            if (Result.empty() || Result.back().size() + Count > 20) Result.emplace_back();
            return Result.back();
        };
        auto Append = [&](KeyboardTextStroke Stroke)
        {
            if (!Stroke.Code) throw std::invalid_argument("Invalid keyboard scan code.");
            auto& Batch = Reserve(2 + (Stroke.Shift ? 2 : 0) + (Stroke.AltGr ? 2 : 0));
            if (Stroke.Shift) Batch.push_back({0x2a, false});
            if (Stroke.AltGr) Batch.push_back({0x38, false, true});
            Batch.push_back({Stroke.Code, false, Stroke.Extended});
            Batch.push_back({Stroke.Code, true, Stroke.Extended});
            if (Stroke.AltGr) Batch.push_back({0x38, true, true});
            if (Stroke.Shift) Batch.push_back({0x2a, true});
        };
        auto AppendUnicode = [&](std::uint32_t CodePoint)
        {
            auto U = Map(L'u');
            if (!U || !U->Code || U->Shift || U->AltGr)
                throw std::invalid_argument("The selected layout cannot start GTK Unicode input.");
            auto& Batch = Reserve(6);
            Batch.push_back({0x1d, false});
            Batch.push_back({0x2a, false});
            Batch.push_back({U->Code, false, U->Extended});
            Batch.push_back({U->Code, true, U->Extended});
            Batch.push_back({0x2a, true});
            Batch.push_back({0x1d, true});
            wchar_t Digits[6]{};
            std::size_t Count = 0;
            do
            {
                Digits[Count++] = L"0123456789abcdef"[CodePoint & 15u];
                CodePoint >>= 4;
            } while (CodePoint);
            while (Count)
            {
                auto Digit = Map(Digits[--Count]);
                if (!Digit || !Digit->Code || Digit->AltGr)
                    throw std::invalid_argument("The selected layout cannot type GTK Unicode hexadecimal digits.");
                Append(*Digit);
            }
            Append({0x1c});
        };
        for (std::size_t I = 0; I < Text.size(); ++I)
        {
            std::uint32_t Ch = static_cast<std::uint16_t>(Text[I]);
            if (Ch == L'\r' || Ch == L'\n')
            {
                Append({0x1c});
                if (Ch == L'\r' && I + 1 < Text.size() && Text[I + 1] == L'\n') ++I;
                continue;
            }
            if (Ch == L'\t')
            {
                Append({0x0f});
                continue;
            }
            if (Ch >= 0xd800 && Ch <= 0xdbff)
            {
                if (I + 1 >= Text.size() || Text[I + 1] < 0xdc00 || Text[I + 1] > 0xdfff)
                    throw std::invalid_argument("Clipboard text contains an invalid UTF-16 surrogate pair.");
                Ch = 0x10000 + ((Ch - 0xd800) << 10) + (static_cast<std::uint16_t>(Text[++I]) - 0xdc00);
            }
            else if (Ch >= 0xdc00 && Ch <= 0xdfff)
                throw std::invalid_argument("Clipboard text contains an invalid UTF-16 surrogate pair.");
            if (Ch < 32 || (Ch >= 127 && Ch < 160) || (Ch >= 0xfdd0 && Ch <= 0xfdef) ||
                (Ch & 0xffffu) >= 0xfffe)
                throw std::invalid_argument("Clipboard text contains an unsupported control character or noncharacter.");
            std::optional<KeyboardTextStroke> Stroke;
            if (Ch <= 0xffff) Stroke = Map(static_cast<wchar_t>(Ch));
            if (Stroke && Stroke->Code)
                Append(*Stroke);
            else if (GtkUnicode)
                AppendUnicode(Ch);
            else
                throw std::invalid_argument("A clipboard character cannot be typed with the selected keyboard layout.");
        }
        return Result;
    }

    inline std::optional<KeyboardTextStroke> MapUsCharacter(wchar_t Ch)
    {
        constexpr std::array<std::uint8_t, 26> Letters = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
            0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
            0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
        constexpr std::wstring_view Plain = L"1234567890-=[]\\;',./` ";
        constexpr std::wstring_view Shifted = L"!@#$%^&*()_+{}|:\"<>?~";
        constexpr std::array<std::uint8_t, 22> Codes = {
            0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
            0x0c, 0x0d, 0x1a, 0x1b, 0x2b, 0x27, 0x28, 0x33, 0x34, 0x35, 0x29, 0x39};
        if (Ch >= L'a' && Ch <= L'z') return KeyboardTextStroke{Letters[Ch - L'a']};
        if (Ch >= L'A' && Ch <= L'Z') return KeyboardTextStroke{Letters[Ch - L'A'], false, true};
        if (auto Position = Plain.find(Ch); Position != std::wstring_view::npos)
            return KeyboardTextStroke{Codes[Position]};
        if (auto Position = Shifted.find(Ch); Position != std::wstring_view::npos)
            return KeyboardTextStroke{Codes[Position], false, true};
        return {};
    }

}
