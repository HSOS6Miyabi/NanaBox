// Included by GuestTool.cpp after the UTF-8 conversion helpers.
class InputHandle
{
    HANDLE Value;
public:
    explicit InputHandle(HANDLE value = nullptr) : Value(value) {}
    ~InputHandle() { if (Value && Value != INVALID_HANDLE_VALUE) CloseHandle(Value); }
    InputHandle(InputHandle const&) = delete;
    InputHandle& operator=(InputHandle const&) = delete;
    HANDLE Get() const { return Value; }
    explicit operator bool() const { return Value && Value != INVALID_HANDLE_VALUE; }
};

[[noreturn]] void InputError(char const* operation)
{
    throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
}

std::vector<INPUT> UnicodeInputEvents(std::wstring_view text)
{
    if (text.size() > 4096) GuestFail("Unicode input exceeds 4096 UTF-16 code units");
    std::vector<INPUT> events;
    events.reserve(text.size() * 2);
    auto append = [&](WORD key, WORD scan, DWORD flags)
    {
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wVk = key;
        event.ki.wScan = scan;
        event.ki.dwFlags = flags;
        events.push_back(event);
        event.ki.dwFlags |= KEYEVENTF_KEYUP;
        events.push_back(event);
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
        auto ch = static_cast<std::uint32_t>(text[i]);
        if (ch == '\r' || ch == '\n')
        {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            append(VK_RETURN, 0, 0);
            continue;
        }
        if (ch == '\t') { append(VK_TAB, 0, 0); continue; }
        auto scalar = ch;
        bool pair = ch >= 0xd800 && ch <= 0xdbff;
        if (pair)
        {
            if (i + 1 >= text.size() || text[i + 1] < 0xdc00 || text[i + 1] > 0xdfff)
                GuestFail("Invalid UTF-16 surrogate pair");
            scalar = 0x10000 + ((ch - 0xd800) << 10) + (text[i + 1] - 0xdc00);
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff) GuestFail("Invalid UTF-16 surrogate pair");
        if (scalar < 32 || (scalar >= 127 && scalar < 160) ||
            (scalar >= 0xfdd0 && scalar <= 0xfdef) || (scalar & 0xffffu) >= 0xfffe)
            GuestFail("Unsupported control character or Unicode noncharacter");
        append(0, static_cast<WORD>(ch), KEYEVENTF_UNICODE);
        if (pair) append(0, static_cast<WORD>(text[++i]), KEYEVENTF_UNICODE);
    }
    return events;
}

std::wstring InputText(std::string const& utf8)
{
    if (utf8.size() > 4096 * 4) GuestFail("Unicode input exceeds size limit");
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0 || size > 4096) GuestFail("Invalid or oversized UTF-8 input");
    std::wstring text(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), text.data(), size) != size)
        GuestFail("Invalid UTF-8 input");
    return text;
}

DWORD ChooseInputSession(std::vector<std::pair<DWORD, USHORT>> const& active, DWORD console, bool enhanced)
{
    DWORD selected = 0;
    for (auto const& entry : active)
    {
        if (!entry.first || entry.first == MAXDWORD) continue;
        if (enhanced ? entry.second == 2 : (entry.first == console && entry.second == 0))
        {
            if (selected) GuestFail("More than one active session matches; disconnect other sessions");
            selected = entry.first;
        }
    }
    if (!selected) GuestFail("No active logged-on session matches the selected console mode");
    return selected;
}

DWORD ActiveInputSession(bool enhanced)
{
    PWTS_SESSION_INFOW rows = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &rows, &count))
        InputError("Enumerate guest sessions");
    struct Free { PVOID Value; ~Free() { if (Value) WTSFreeMemory(Value); } } freeRows{rows};
    std::vector<std::pair<DWORD, USHORT>> active;
    for (DWORD i = 0; i < count; ++i)
    {
        auto const& row = rows[i];
        if (row.State != WTSActive || !row.SessionId) continue;
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, row.SessionId, WTSUserName, &name, &bytes))
            InputError("Query guest session user");
        Free freeName{name};
        if (!name || bytes < sizeof(wchar_t) * 2 || !name[0]) continue;
        LPWSTR protocol = nullptr;
        bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, row.SessionId, WTSClientProtocolType, &protocol, &bytes))
            InputError("Query guest session protocol");
        Free freeProtocol{protocol};
        if (bytes != sizeof(USHORT) || !protocol) GuestFail("Invalid guest session protocol");
        USHORT type = 0;
        std::memcpy(&type, protocol, sizeof(type));
        active.emplace_back(row.SessionId, type);
    }
    return ChooseInputSession(active, WTSGetActiveConsoleSessionId(), enhanced);
}

std::wstring InputLogonSid(HANDLE token)
{
    DWORD bytes = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &bytes);
    if (!bytes || bytes > 1024 * 1024) InputError("Query input token group size");
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token, TokenGroups, buffer.data(), bytes, &bytes))
        InputError("Query input token groups");
    auto groups = reinterpret_cast<TOKEN_GROUPS*>(buffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i)
    {
        auto const& group = groups->Groups[i];
        if ((group.Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(group.Sid, &sid)) InputError("Convert input logon SID");
        struct Free { HLOCAL Value; ~Free() { LocalFree(Value); } } freeSid{sid};
        return sid;
    }
    GuestFail("The input token has no logon SID");
}

std::wstring InputPipeName(std::wstring_view nonce)
{
    if (nonce.size() != 32 || !std::all_of(nonce.begin(), nonce.end(), [](wchar_t ch)
        { return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'); }))
        GuestFail("Invalid input pipe identifier");
    return L"\\\\.\\pipe\\NanaBox.GuestInput." + std::wstring(nonce);
}

void ReadInputPipe(HANDLE pipe, void* buffer, DWORD bytes)
{
    auto position = static_cast<BYTE*>(buffer);
    while (bytes)
    {
        DWORD read = 0;
        if (!ReadFile(pipe, position, bytes, &read, nullptr) || !read)
            GuestFail("Incomplete Unicode input frame");
        bytes -= read;
        position += read;
    }
}

bool DefaultDesktop(HDESK desktop)
{
    wchar_t name[128]{};
    DWORD needed = 0;
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) &&
        _wcsicmp(name, L"Default") == 0;
}

enum class InputExit : DWORD { Success = 0, Invalid = 10, Desktop = 11, Focus = 12,
    Modifiers = 13, Injection = 14, Session = 15, Pipe = 16 };

InputExit InjectUnicode(std::vector<INPUT>& events, DWORD session)
{
    DWORD actual = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &actual) || actual != session || !actual)
        return InputExit::Session;
    struct Desktop { HDESK Value; ~Desktop() { if (Value) CloseDesktop(Value); } }
        input{OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)};
    if (!DefaultDesktop(input.Value) || !DefaultDesktop(GetThreadDesktop(GetCurrentThreadId())))
        return InputExit::Desktop;
    HWND foreground = GetForegroundWindow();
    DWORD process = 0, foregroundSession = 0;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!foreground || !GetWindowThreadProcessId(foreground, &process) ||
        !ProcessIdToSessionId(process, &foregroundSession) || foregroundSession != session ||
        !GetGUIThreadInfo(0, &info) || !info.hwndFocus)
        return InputExit::Focus;
    for (int key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return InputExit::Modifiers;
    if (GetForegroundWindow() != foreground) return InputExit::Focus;
    if (events.empty()) return InputExit::Success;
    auto count = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    if (count != events.size())
    {
        if (count < events.size() && count % 2)
        {
            auto event = events[count - 1];
            event.ki.dwFlags |= KEYEVENTF_KEYUP;
            SendInput(1, &event, sizeof(INPUT));
        }
        return InputExit::Injection;
    }
    return InputExit::Success;
}

constexpr wchar_t InputTestText[] = L"Hello \u4e16\u754c \U0001f600 \u20ac A!";

int UnicodeInputChild(std::wstring_view nonce, std::wstring_view sessionText, bool test)
{
    try
    {
        if (sessionText.empty() || sessionText.size() > 10 ||
            !std::all_of(sessionText.begin(), sessionText.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            return static_cast<int>(InputExit::Session);
        auto number = std::stoull(std::wstring(sessionText));
        DWORD current = 0;
        if (!number || number >= MAXDWORD || !ProcessIdToSessionId(GetCurrentProcessId(), &current) || current != number)
            return static_cast<int>(InputExit::Session);
        auto name = InputPipeName(nonce);
        InputHandle pipe(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe) return static_cast<int>(InputExit::Pipe);
        std::uint32_t size = 0;
        ReadInputPipe(pipe.Get(), &size, sizeof(size));
        if (size > 4096) return static_cast<int>(InputExit::Invalid);
        std::wstring text(size, L'\0');
        if (size) ReadInputPipe(pipe.Get(), text.data(), static_cast<DWORD>(size * sizeof(wchar_t)));
        auto events = UnicodeInputEvents(text);
        if (test) return text == InputTestText ? 0 : static_cast<int>(InputExit::Invalid);
        return static_cast<int>(InjectUnicode(events, current));
    }
    catch (...) { return static_cast<int>(InputExit::Invalid); }
}

std::wstring InputExecutable()
{
    std::vector<wchar_t> path(32768);
    auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) InputError("Resolve GuestTool executable");
    return std::wstring(path.data(), size);
}

void WaitInputIo(HANDLE pipe, OVERLAPPED& operation, HANDLE process, DWORD& bytes)
{
    HANDLE handles[] = {operation.hEvent, process};
    auto wait = WaitForMultipleObjects(2, handles, FALSE, 5000);
    if (wait != WAIT_OBJECT_0)
    {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        GuestFail("Guest input helper did not complete the pipe transfer");
    }
    if (!GetOverlappedResult(pipe, &operation, &bytes, FALSE)) InputError("Complete input pipe operation");
}

template<class Launch, class Check>
void SendInputFrame(std::wstring const& text, DWORD session, HANDLE user, bool test, Launch launch, Check check)
{
    auto random = Token();
    std::wstring nonce(random.begin(), random.end());
    auto name = InputPipeName(nonce);
    auto sid = InputLogonSid(user);
    auto sddl = L"D:P(A;;GA;;;SY)(A;;" + std::wstring(test ? L"GA" : L"GR") + L";;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        InputError("Create input pipe security descriptor");
    struct Free { HLOCAL Value; ~Free() { LocalFree(Value); } } freeDescriptor{descriptor};
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    InputHandle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 16384, 0, 5000, &security));
    if (!pipe) InputError("Create private Unicode input pipe");
    auto executable = InputExecutable();
    auto command = L"\"" + executable + L"\" " +
        (test ? L"--unicode-input-test-child " : L"--unicode-input-child ") + nonce + L" " + std::to_wstring(session);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION child{};
    if (!launch(executable, command, startup, child)) InputError("Launch guest input helper");
    InputHandle process(child.hProcess), thread(child.hThread);
    struct ProcessGuard
    {
        HANDLE Process;
        ~ProcessGuard()
        {
            if (WaitForSingleObject(Process, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(Process, 1);
                WaitForSingleObject(Process, 2000);
            }
        }
    } guard{process.Get()};
    InputHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) InputError("Create input pipe completion event");
    OVERLAPPED operation{};
    operation.hEvent = event.Get();
    DWORD bytes = 0;
    if (!ConnectNamedPipe(pipe.Get(), &operation))
    {
        auto error = GetLastError();
        if (error == ERROR_IO_PENDING) WaitInputIo(pipe.Get(), operation, process.Get(), bytes);
        else if (error != ERROR_PIPE_CONNECTED) InputError("Connect input pipe");
    }
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe.Get(), &clientPid) || clientPid != child.dwProcessId)
        GuestFail("Unexpected Unicode input pipe client");
    check();
    if (Stop) GuestFail("Guest service is stopping");
    std::vector<BYTE> payload(sizeof(std::uint32_t) + text.size() * sizeof(wchar_t));
    auto size = static_cast<std::uint32_t>(text.size());
    std::memcpy(payload.data(), &size, sizeof(size));
    if (size) std::memcpy(payload.data() + sizeof(size), text.data(), text.size() * sizeof(wchar_t));
    ResetEvent(event.Get());
    operation = OVERLAPPED{};
    operation.hEvent = event.Get();
    if (!WriteFile(pipe.Get(), payload.data(), static_cast<DWORD>(payload.size()), nullptr, &operation))
    {
        if (GetLastError() != ERROR_IO_PENDING) InputError("Write input pipe");
        WaitInputIo(pipe.Get(), operation, process.Get(), bytes);
    }
    else if (!GetOverlappedResult(pipe.Get(), &operation, &bytes, FALSE))
        InputError("Complete input pipe write");
    if (bytes != payload.size()) GuestFail("Incomplete Unicode input pipe write");
    if (WaitForSingleObject(process.Get(), 5000) != WAIT_OBJECT_0)
        GuestFail("Guest input helper timed out; input may be incomplete");
    DWORD code = 0;
    if (!GetExitCodeProcess(process.Get(), &code)) InputError("Read input helper result");
    switch (static_cast<InputExit>(code))
    {
    case InputExit::Success: return;
    case InputExit::Desktop: GuestFail("The guest is locked or on a secure desktop");
    case InputExit::Focus: GuestFail("No focused window in the guest input session");
    case InputExit::Modifiers: GuestFail("Release modifier keys before typing clipboard text");
    case InputExit::Injection: GuestFail("Windows rejected keyboard input or accepted only part of it");
    case InputExit::Session: GuestFail("Guest input session changed");
    default: GuestFail("Guest input helper failed");
    }
}

Json TypeInteractiveUnicode(std::string const& utf8, bool enhanced)
{
    auto text = InputText(utf8);
    auto events = UnicodeInputEvents(text);
    if (events.empty()) return {{"Events",0},{"Utf16Units",0}};
    auto session = ActiveInputSession(enhanced);
    HANDLE raw = nullptr;
    if (!WTSQueryUserToken(session, &raw)) InputError("Obtain logged-on guest user token");
    InputHandle user(raw);
    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, user.Get(), FALSE)) InputError("Create guest user environment");
    struct Environment { LPVOID Value; ~Environment() { DestroyEnvironmentBlock(Value); } } env{environment};
    SendInputFrame(text, session, user.Get(), false,
        [&](std::wstring const& executable, std::wstring& command, STARTUPINFOW& startup, PROCESS_INFORMATION& child)
        {
            auto directory = fs::path(executable).parent_path().wstring();
            return CreateProcessAsUserW(user.Get(), executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment, directory.c_str(), &startup, &child);
        }, [&]()
        {
            if (ActiveInputSession(enhanced) != session) GuestFail("Guest input session changed");
        });
    return {{"Events",events.size()},{"Utf16Units",text.size()},{"SessionId",session}};
}

void UnicodeInputSelfTest()
{
    auto events = UnicodeInputEvents(L"A\u4e2d\U0001f600\r\n\t");
    if (events.size() != 12 || events[2].ki.wScan != 0x4e2d ||
        events[4].ki.wScan != 0xd83d || events[6].ki.wScan != 0xde00 ||
        events[8].ki.wVk != VK_RETURN || events[10].ki.wVk != VK_TAB)
        GuestFail("Unicode input encoding failed");
    for (size_t i = 0; i < events.size(); i += 2)
        if (events[i].ki.dwFlags & KEYEVENTF_KEYUP ||
            events[i + 1].ki.dwFlags != (events[i].ki.dwFlags | KEYEVENTF_KEYUP))
            GuestFail("Unbalanced Unicode key events");
    for (auto ch : {0u, 1u, 127u, 0xd800u, 0xdc00u, 0xffffu})
    {
        bool rejected = false;
        try { (void)UnicodeInputEvents(std::wstring(1, static_cast<wchar_t>(ch))); }
        catch (std::exception const&) { rejected = true; }
        if (!rejected) GuestFail("Invalid Unicode input was accepted");
    }
    if (ChooseInputSession({{1,0},{2,2}},1,false) != 1 ||
        ChooseInputSession({{1,0},{2,2}},1,true) != 2)
        GuestFail("Guest input session selection failed");
    for (auto sessions : {std::vector<std::pair<DWORD,USHORT>>{{1,0}},
        std::vector<std::pair<DWORD,USHORT>>{{2,2},{3,2}}})
    {
        bool rejected = false;
        try { (void)ChooseInputSession(sessions,1,true); }
        catch (std::exception const&) { rejected = true; }
        if (!rejected) GuestFail("Ambiguous or absent input session accepted");
    }
    std::cout << "Unicode input encoding and session selection tests passed.\n";
}

void UnicodeInputUiTest()
{
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session)
        GuestFail("Unicode input UI test requires an interactive session");
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) InputError("Open test input token");
    InputHandle token(raw);
    SendInputFrame(InputTestText, session, token.Get(), true,
        [](std::wstring const& exe, std::wstring& cmd, STARTUPINFOW& startup, PROCESS_INFORMATION& child)
        { return CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child); }, [](){});
    HWND window = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"NanaBox Unicode input integration",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 480, 160,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) InputError("Create Unicode input test window");
    struct Window { HWND Value; ~Window() { DestroyWindow(Value); } } cleanup{window};
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 10, 10, 440, 40,
        window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!edit) InputError("Create Unicode input test edit control");
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(edit);
    auto pump = []()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    };
    pump();
    if (GetForegroundWindow() != window || GetFocus() != edit)
        GuestFail("Cannot focus isolated Unicode input test control");
    auto events = UnicodeInputEvents(InputTestText);
    if (InjectUnicode(events, session) != InputExit::Success)
        GuestFail("Windows SendInput failed in the test window");
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        pump();
        wchar_t actual[256]{};
        GetWindowTextW(edit, actual, static_cast<int>(std::size(actual)));
        if (std::wstring_view(actual) == InputTestText)
        {
            std::cout << "Private input pipe and Windows native edit Unicode input passed.\n";
            return;
        }
        Sleep(20);
    }
    GuestFail("Native edit control did not receive the expected Unicode text");
}
