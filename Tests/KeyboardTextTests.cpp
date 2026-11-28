#include "../NanaBox/KeyboardTextEncoding.h"
#include <cassert>
#include <iostream>
#include <set>

using namespace NanaBox;

// The Windows console encoder accepts UTF-16 code units. Represent supplementary
// characters as two code units even on platforms with a wider wchar_t.
std::wstring scalar(std::uint32_t cp)
{
    if (cp < 0x10000) return {static_cast<wchar_t>(cp)};
    cp -= 0x10000;
    return {static_cast<wchar_t>(0xd800 + (cp >> 10)),
        static_cast<wchar_t>(0xdc00 + (cp & 1023))};
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--emit-gtk")
    {
        auto input = L"Hello " + scalar(0x4e16) + scalar(0x754c) + L" " +
            scalar(0x1f600) + L" " + scalar(0x20ac) + L" A!";
        for (const auto& batch : EncodeKeyboardTextWithLayout(input, MapUsCharacter, true))
            for (const auto& event : batch)
                std::cout << int(event.ScanCode) << ' ' << event.KeyUp << ' ' << event.Extended << '\n';
        return 0;
    }
    if (argc != 1) return 2;
    auto batches = EncodeKeyboardTextWithLayout(scalar(0x4e2d) + scalar(0x1f600), MapUsCharacter, true);
    assert(batches.size() == 2 && batches[0].size() == 16 && batches[1].size() == 18);
    unsigned count = 0;
    for (std::uint32_t cp = 160; cp <= 0x10ffff; ++cp)
    {
        if ((cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xffff) >= 0xfffe)
            continue;
        auto encoded = EncodeKeyboardTextWithLayout(scalar(cp), MapUsCharacter, true);
        std::vector<KeyboardTextEvent> events;
        for (const auto& batch : encoded)
        {
            assert(batch.size() <= 20);
            std::set<int> pressed;
            for (const auto& event : batch)
            {
                int key = event.ScanCode + (event.Extended ? 256 : 0);
                if (event.KeyUp) assert(pressed.erase(key) == 1);
                else assert(pressed.insert(key).second);
                events.push_back(event);
            }
            assert(pressed.empty());
        }
        assert(events[0].ScanCode == 0x1d && events[1].ScanCode == 0x2a && events[2].ScanCode == 0x16);
        assert(events[events.size() - 2].ScanCode == 0x1c && events.back().KeyUp);
        std::uint32_t decoded = 0;
        for (std::size_t i = 6; i + 2 < events.size(); i += 2)
        {
            assert(!events[i].KeyUp && events[i + 1].KeyUp);
            int digit = -1;
            for (int n = 0; n < 16; ++n)
                if (MapUsCharacter(L"0123456789abcdef"[n])->Code == events[i].ScanCode) digit = n;
            assert(digit >= 0);
            decoded = decoded * 16 + static_cast<std::uint32_t>(digit);
        }
        assert(cp == decoded);
        ++count;
    }
    for (auto cp : {0xd800u, 0xdc00u, 0xffffu, 0x10ffffu, 0x7fu})
    {
        bool rejected = false;
        try { EncodeKeyboardTextWithLayout(scalar(cp), MapUsCharacter, true); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    std::cout << "Validated " << count << " Unicode scalars, scan-code round trip and balanced key batches.\n";
}
