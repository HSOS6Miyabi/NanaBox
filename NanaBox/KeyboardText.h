#pragma once

#include <Windows.h>
#include "KeyboardTextEncoding.h"

namespace NanaBox
{
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
    inline std::vector<KeyboardTextBatch> EncodeKeyboardText(std::wstring_view Text)
    {
        return EncodeKeyboardTextWithLayout(Text, MapUsCharacter);
    }

    inline std::vector<KeyboardTextBatch> EncodeKeyboardText(std::wstring_view Text, std::string_view Layout, bool GtkUnicode = false)
    {
        if (!ValidClipboardKeyboardLayout(Layout)) throw std::invalid_argument("Invalid clipboard keyboard layout identifier.");
        // The fixed US mapping also works in a firmware console.
        if (Layout == "00000409") return EncodeKeyboardTextWithLayout(Text, MapUsCharacter, GtkUnicode);
        HKL Handle = nullptr;
        if (Layout == "Host") Handle = ::GetKeyboardLayout(0);
        else
        {
            std::wstring Id(Layout.begin(), Layout.end());
            Handle = ::LoadKeyboardLayoutW(Id.c_str(), 0);
            if (!Handle) throw std::invalid_argument("The selected keyboard layout is not installed on the host.");
        }
        if (!Handle) throw std::invalid_argument("The keyboard layout is unavailable.");
        return EncodeKeyboardTextWithLayout(Text, [&](wchar_t Ch) { return MapLayoutCharacter(Ch, Handle); }, GtkUnicode);
    }

}
