// Included by WddmGuest.cpp inside its implementation namespace.
namespace Vsmb = NanaBox::Vsmb;

class NativeHandle
{
    HANDLE Value = INVALID_HANDLE_VALUE;
public:
    explicit NativeHandle(HANDLE h=INVALID_HANDLE_VALUE):Value(h){}
    ~NativeHandle(){if(Valid())CloseHandle(Value);}
    NativeHandle(NativeHandle const&)=delete;
    NativeHandle& operator=(NativeHandle const&)=delete;
    bool Valid()const{return Value!=INVALID_HANDLE_VALUE&&Value!=nullptr;}
    HANDLE Get()const{return Value;}
    void Reset(HANDLE h){if(Valid())CloseHandle(Value);Value=h;}
};

std::runtime_error NtError(char const* operation,NTSTATUS status)
{
    return std::runtime_error(std::string(operation)+" (NTSTATUS "+std::to_string(static_cast<std::uint32_t>(status))+")");
}

class NativeVsmb
{
    using CreateNt=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
    using ControlNt=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,ULONG,PVOID,ULONG,PVOID,ULONG);
    using SetupBinding=HRESULT(WINAPI*)(HANDLE,ULONG,LPCWSTR,LPCWSTR,LPCWSTR*,ULONG);
    using QueryBindings=HRESULT(WINAPI*)(ULONG,HANDLE,LPCWSTR,PSID,PULONG,PVOID);
    using RemoveBinding=HRESULT(WINAPI*)(HANDLE,LPCWSTR);
    HMODULE Module=nullptr;
    SetupBinding Setup=nullptr;
    QueryBindings Query=nullptr;
    RemoveBinding Remove=nullptr;
    NativeHandle Keepalive;
    bool TransportReady=false;
    std::map<std::wstring,std::pair<std::wstring,bool>> Owned;

    template<class T> static T Import(HMODULE module,char const* name)
    {
        auto address=GetProcAddress(module,name);
        if(!address)throw std::runtime_error(std::string("Unavailable Windows interface: ")+name);
        return reinterpret_cast<T>(address);
    }
    static std::wstring NtShare(std::wstring const& name)
    {
        if(!Wddm::SafeComponent(name))throw std::runtime_error("Invalid Virtual SMB share name");
        return L"\\Device\\vmsmb\\VSMB-{dcc079ae-60ba-4d07-847c-3493609c0870}\\"+name;
    }
    static HANDLE OpenDevice(std::wstring const& path)
    {
        auto create=Import<CreateNt>(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile");
        if(path.size()>=32767)throw std::runtime_error("NT path too long");
        UNICODE_STRING name{};
        name.Buffer=const_cast<wchar_t*>(path.c_str());
        name.Length=static_cast<USHORT>(path.size()*sizeof(wchar_t));
        name.MaximumLength=name.Length;
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length=sizeof(attributes);attributes.ObjectName=&name;attributes.Attributes=0x40;
        IO_STATUS_BLOCK io{};
        HANDLE handle=INVALID_HANDLE_VALUE;
        auto status=create(&handle,FILE_LIST_DIRECTORY|FILE_TRAVERSE|SYNCHRONIZE,&attributes,&io,nullptr,0,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,1,0x20,nullptr,0);
        if(status<0)throw NtError("Open redirector device",status);
        return handle;
    }
    static NTSTATUS Control(HANDLE handle,ULONG code,std::vector<unsigned char>& request)
    {
        auto control=Import<ControlNt>(GetModuleHandleW(L"ntdll.dll"),"NtFsControlFile");
        IO_STATUS_BLOCK io{};
        NativeHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(!event.Valid())Error("Create redirector control event");
        auto status=control(handle,event.Get(),nullptr,nullptr,&io,code,request.data(),
            static_cast<ULONG>(request.size()),nullptr,0);
        if(status==0x103)
        {
            if(WaitForSingleObject(event.Get(),INFINITE)!=WAIT_OBJECT_0)Error("Wait for redirector control");
            status=io.Status;
        }
        return status;
    }
    static unsigned Build()
    {
        using Version=LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);
        if(Import<Version>(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion")(&version)<0)
            throw std::runtime_error("Cannot query Windows version");
        return version.dwBuildNumber;
    }
    static void EnsureWorkstation()
    {
        struct Sc{SC_HANDLE H;~Sc(){if(H)CloseServiceHandle(H);}};
        Sc manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};
        if(!manager.H)Error("Open service manager");
        Sc service{OpenServiceW(manager.H,L"LanmanWorkstation",SERVICE_START|SERVICE_QUERY_STATUS)};
        if(!service.H)Error("Open Workstation service");
        if(!StartServiceW(service.H,0,nullptr)&&GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)
            Error("Start Workstation service");
        for(unsigned i=0;i<100&&!Stop;++i)
        {
            SERVICE_STATUS status{};
            if(!QueryServiceStatus(service.H,&status))Error("Query Workstation service");
            if(status.dwCurrentState==SERVICE_RUNNING)return;
            if(status.dwCurrentState==SERVICE_STOPPED)break;
            Sleep(100);
        }
        throw std::runtime_error("Workstation service did not start");
    }
    static void EnableDriverPrivilege()
    {
        HANDLE raw=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES,&raw))
            Error("Open GuestTool token");
        NativeHandle token(raw);
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount=1;
        if(!LookupPrivilegeValueW(nullptr,SE_LOAD_DRIVER_NAME,&privileges.Privileges[0].Luid))Error("Look up driver privilege");
        privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        if(!AdjustTokenPrivileges(token.Get(),FALSE,&privileges,0,nullptr,nullptr)||GetLastError()!=ERROR_SUCCESS)
            Error("Enable driver privilege");
    }
    static HANDLE OpenShare(std::wstring const& name)
    {
        auto path=L"\\\\?\\GLOBALROOT"+NtShare(name);
        return CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|SYNCHRONIZE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    }
    void Connect(std::wstring const& share)
    {
        if(Keepalive.Valid())return;
        if(!TransportReady)
        {
            auto build=Build();
            if(build<18362)throw std::runtime_error("Native Virtual SMB mounting requires Windows build 18362 or later");
            EnsureWorkstation();
            EnableDriverPrivilege();
            // If another integration agent already initialized VSMB, reuse its
            // connection rather than rebinding the shared system transport.
            Keepalive.Reset(OpenShare(share));
            if(!Keepalive.Valid())
            {
                NativeHandle redirector(OpenDevice(L"\\Device\\LanmanRedirector"));
                auto layouts=Vsmb::Layouts(build);
                if(layouts.empty())throw std::runtime_error("Unsupported Windows redirector layout");
                NTSTATUS status=static_cast<NTSTATUS>(0xc00000bbu);
                for(auto layout:layouts)
                {
                    auto request=Vsmb::StartRequest(layout);
                    status=Control(redirector.Get(),0x001403a0,request);
                    auto value=static_cast<std::uint32_t>(status);
                    if(status>=0||value==0xc0000035u)break;
                    // Only retry a compatible structure on size/parameter errors.
                    if(value!=0xc0000004u&&value!=0xc000000du&&value!=0xc0000023u)break;
                }
                if(status<0&&static_cast<std::uint32_t>(status)!=0xc0000035u)
                    throw NtError("Initialize VSMB redirector",status);
                NativeHandle instance(OpenDevice(L"\\Device\\vmsmb"));
                auto bind=Vsmb::BindRequest();
                status=Control(instance.Get(),0x001401b0,bind);
                if(status<0&&static_cast<std::uint32_t>(status)!=0xc0000035u)
                    throw NtError("Bind VSMB transport",status);
                TransportReady=true;
            }
            else TransportReady=true;
        }
        if(!Keepalive.Valid())Keepalive.Reset(OpenShare(share));
        if(!Keepalive.Valid())Error("Open VSMB keepalive share");
        NativeActive=true;
        Status(SERVICE_RUNNING,0);
    }
    void Load()
    {
        if(Module)return;
        Module=LoadLibraryExW(L"bindfltapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!Module)Error("Load Bind Filter API");
        try
        {
            Setup=Import<SetupBinding>(Module,"BfSetupFilter");
            Query=Import<QueryBindings>(Module,"BfGetMappings");
            Remove=Import<RemoveBinding>(Module,"BfRemoveMapping");
        }
        catch(...){FreeLibrary(Module);Module=nullptr;throw;}
    }
    static std::wstring WideName(std::u16string const& value){return std::wstring(value.begin(),value.end());}
    static std::wstring NtPath(fs::path const& path)
    {
        auto text=path.wstring();
        if(text.size()<3||text[1]!=L':'||text[2]!=L'\\')throw std::runtime_error("A local DOS mount path is required");
        wchar_t name[32768]{};
        if(!QueryDosDeviceW(text.substr(0,2).c_str(),name,static_cast<DWORD>(std::size(name))))
            Error("Resolve mount volume");
        return std::wstring(name)+text.substr(2);
    }
    static bool Within(std::wstring const& path,std::wstring const& parent)
    {
        return Equal(path,parent)||(path.size()>parent.size()&&path[parent.size()]==L'\\'&&
            Equal(path.substr(0,parent.size()),parent));
    }
    std::vector<Vsmb::Binding> Bindings(fs::path const& path)
    {
        Load();
        auto volume=path.root_path().wstring();
        ULONG bytes=65536;
        std::vector<unsigned char> buffer;
        for(unsigned attempt=0;attempt<4;++attempt)
        {
            if(bytes<12||bytes>8*1024*1024)throw std::runtime_error("Bind Filter mapping inventory is too large");
            buffer.assign(bytes,0);
            auto allocated=bytes;
            auto hr=Query(1,nullptr,volume.c_str(),nullptr,&bytes,buffer.data());
            if(SUCCEEDED(hr)&&bytes<=allocated)
            {
                buffer.resize(bytes);
                return Vsmb::ParseBindings(buffer);
            }
            if(bytes<=allocated)
            {
                if(hr!=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)&&hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA))
                    throw std::runtime_error("Cannot enumerate Bind Filter mappings: "+std::to_string(static_cast<unsigned long>(hr)));
                bytes=allocated*2;
            }
        }
        throw std::runtime_error("Cannot read Bind Filter mapping inventory");
    }
    bool Existing(fs::path const& path,std::wstring const& name,bool readOnly)
    {
        auto nt=NtPath(path),target=NtShare(name);
        bool match=false;
        for(auto const& b:Bindings(path))
        {
            auto root=WideName(b.Root);
            if(Equal(root,nt))
            {
                if(match||b.Targets.size()!=1||!Equal(WideName(b.Targets[0]),target)||
                    (b.Flags&0x20)==0||((b.Flags&1)!=0)!=readOnly)
                    throw std::runtime_error("Mount path is occupied by an unrelated Bind Filter mapping");
                match=true;
            }
            else if(Within(nt,root)||Within(root,nt))
                throw std::runtime_error("Mount path overlaps an existing Bind Filter mapping");
        }
        return match;
    }
    static void PrepareDirectory(fs::path const& path)
    {
        // Reject all reparse points from the volume root to the target.
        fs::path current=path.root_path();
        for(auto const& part:path.relative_path())
        {
            current/=part;
            auto attributes=GetFileAttributesW(current.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES)
            {
                auto code=GetLastError();
                if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND)Error("Inspect mount directory");
                if(!CreateDirectoryW(current.c_str(),nullptr))Error("Create mount directory");
                attributes=GetFileAttributesW(current.c_str());
            }
            if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||
                (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Unsafe Virtual SMB mount directory");
        }
        if(!fs::is_empty(path))throw std::runtime_error("Virtual SMB mount directory is not empty");
    }
public:
    ~NativeVsmb(){if(Module)FreeLibrary(Module);}
    bool Ready(fs::path const& path,std::wstring const& name,bool readOnly)
    {
        auto it=Owned.find(path.wstring());
        if(it==Owned.end()||!Equal(it->second.first,name)||it->second.second!=readOnly)return false;
        try{return Existing(path,name,readOnly)&&fs::is_directory(path);}catch(...){return false;}
    }
    bool Ensure(fs::path const& path,std::wstring const& name,bool readOnly)
    {
        Connect(name);
        NativeHandle share(OpenShare(name));
        if(!share.Valid())Error("Open Virtual SMB share");
        Load();
        if(!Existing(path,name,readOnly))
        {
            PrepareDirectory(path);
            auto target=L"\\\\?\\GLOBALROOT"+NtShare(name);
            auto hr=Setup(nullptr,0x20|(readOnly?1u:0u),path.c_str(),target.c_str(),nullptr,0);
            if(FAILED(hr))throw std::runtime_error("Bind Filter mount failed: "+std::to_string(static_cast<unsigned long>(hr)));
            if(!Existing(path,name,readOnly))throw std::runtime_error("Bind Filter mapping did not appear");
        }
        if(!fs::is_directory(path))throw std::runtime_error("Mapped Virtual SMB directory is inaccessible");
        Owned[path.wstring()]={name,readOnly};
        return true;
    }
};

NativeVsmb VsmbClient;
