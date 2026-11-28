// Included inside the Windows guest service implementation namespace.
struct WindowsPlan9Share
{
    std::string Name;
    std::wstring WideName;
    std::uint32_t Port;
    bool ReadOnly, AutoMount;
    Json Identity() const { return {{"Name",Name},{"Port",Port},{"ReadOnly",ReadOnly},{"AutoMount",AutoMount}}; }
};

bool InternalPlan9Name(std::wstring const& name)
{
    for(auto internal : {L"NanaBox.HostDrivers",L"HostDriverStore",L"NanaBox.HostLxssLib"})
        if(Equal(name,internal)) return true;
    constexpr std::wstring_view prefix=L"NanaBox.GpuExtra.";
    return name.size()>=prefix.size() && Equal(name.substr(0,prefix.size()),std::wstring(prefix));
}

bool ValidWindowsPlan9Name(std::wstring const& name)
{
    return Wddm::SafeComponent(name) && name.front()!=L' ' && name.find(L',')==std::wstring::npos;
}

std::uint32_t WindowsPlan9Port(Json const& value)
{
    if(!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>()<=0))
        throw std::runtime_error("Invalid Plan 9 port");
    auto port=value.get<std::uint64_t>();
    if(!port || port>=UINT32_MAX || (port>=27453 && port<=27457))
        throw std::runtime_error("Invalid or reserved Plan 9 port");
    return static_cast<std::uint32_t>(port);
}

std::vector<WindowsPlan9Share> ParseWindowsPlan9Inventory(Json const& shares)
{
    if(!shares.is_array() || shares.size()>224) throw std::runtime_error("Invalid Plan 9 inventory");
    std::vector<WindowsPlan9Share> result;
    std::vector<std::wstring> seen;
    for(auto const& row:shares)
    {
        auto name=row.at("Name").get<std::string>();
        auto wide=Wide(name);
        auto port=WindowsPlan9Port(row.at("Port"));
        bool readOnly=row.at("ReadOnly").get<bool>(), automatic=row.at("AutoMount").get<bool>();
        if(name.size()>200 || !ValidWindowsPlan9Name(wide) ||
           std::any_of(seen.begin(),seen.end(),[&](auto const& n){return Equal(n,wide);}))
            throw std::runtime_error("Invalid or duplicate Windows Plan 9 share");
        seen.push_back(wide);
        if(InternalPlan9Name(wide))
        {
            if(!readOnly) throw std::runtime_error("Internal Plan 9 shares must be read-only");
            continue;
        }
        result.push_back({std::move(name),std::move(wide),port,readOnly,automatic});
    }
    return result;
}

fs::path WindowsPlan9Root()
{
    wchar_t value[32768]{};
    auto size=GetEnvironmentVariableW(L"ProgramData",value,static_cast<DWORD>(std::size(value)));
    if(!size || size>=std::size(value)) throw std::runtime_error("ProgramData is unavailable");
    return fs::path(value)/L"NanaBox Guest 9P";
}

void ValidatePlan9Root(fs::path const& root)
{
    if(!root.is_absolute()) throw std::runtime_error("Invalid Plan 9 mount root");
    auto current=root.root_path();
    for(auto const& part:root.relative_path())
    {
        current/=part;
        auto attrs=GetFileAttributesW(current.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || !(attrs&FILE_ATTRIBUTE_DIRECTORY) || (attrs&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("The protected Plan 9 mount root is missing or unsafe");
    }
}

fs::path WindowsPlan9Target(fs::path const& root,WindowsPlan9Share const& share,std::string const& requested)
{
    fs::path value=requested.empty()?fs::path(share.WideName):fs::path(Wide(requested));
    auto name=value.filename().wstring();
    if(!ValidWindowsPlan9Name(name) || InternalPlan9Name(name))
        throw std::runtime_error("Invalid Plan 9 mount directory");
    if(value.has_parent_path() && (!value.is_absolute() || !Equal(value.parent_path().wstring(),root.wstring())))
        throw std::runtime_error("Choose a direct child of the protected Plan 9 mount root");
    auto target=root/name;
    if(target.wstring().size()>=MAX_PATH-1) throw std::runtime_error("Plan 9 mount path is too long");
    return target;
}

std::wstring Plan9EventName()
{
    unsigned char bytes[16]{};
    if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
        throw std::runtime_error("Cannot generate Plan 9 control identifier");
    constexpr wchar_t digits[]=L"0123456789abcdef";
    std::wstring result=L"Local\\NanaBox.Plan9.";
    for(auto byte:bytes) {result+=digits[byte>>4];result+=digits[byte&15];}
    return result;
}

class WindowsPlan9Process
{
    NativeHandle Job,Process,StopEvent,BusyEvent;
    DWORD Pid=0;
public:
    WindowsPlan9Share Share;
    fs::path Target;
    WindowsPlan9Process(WindowsPlan9Share share,fs::path target,fs::path const& executable,bool loopback):
        Share(std::move(share)),Target(std::move(target))
    {
        if(GetFileAttributesW(Target.c_str())!=INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("Plan 9 mount target already exists");
        auto missing=GetLastError();
        if(missing!=ERROR_FILE_NOT_FOUND && missing!=ERROR_PATH_NOT_FOUND) Error("Inspect Plan 9 mount target");
        auto event=Plan9EventName(),busy=event+L".Busy";
        StopEvent.Reset(CreateEventW(nullptr,FALSE,FALSE,event.c_str()));
        if(!StopEvent.Valid() || GetLastError()==ERROR_ALREADY_EXISTS) Error("Create Plan 9 stop event");
        BusyEvent.Reset(CreateEventW(nullptr,FALSE,FALSE,busy.c_str()));
        if(!BusyEvent.Valid() || GetLastError()==ERROR_ALREADY_EXISTS) Error("Create Plan 9 busy event");
        Job.Reset(CreateJobObjectW(nullptr,nullptr));
        if(!Job.Valid()) Error("Create Plan 9 process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(Job.Get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            Error("Configure Plan 9 process job");
        auto command=L"\""+executable.wstring()+L"\" "+(loopback?L"--test-loopback ":L"--hvsocket ")+
            std::to_wstring(Share.Port)+L" \""+Share.WideName+L"\" \""+Target.wstring()+
            L"\" --all-users --stop-event "+event+L" --busy-event "+busy;
        if(Share.ReadOnly) command+=L" --read-only";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        PROCESS_INFORMATION child{};
        auto directory=executable.parent_path().wstring();
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,directory.c_str(),&startup,&child))
            Error("Start WinFsp Plan 9 client");
        Process.Reset(child.hProcess);NativeHandle thread(child.hThread);Pid=child.dwProcessId;
        if(!AssignProcessToJobObject(Job.Get(),Process.Get()) || ResumeThread(thread.Get())==DWORD(-1))
        {
            auto error=GetLastError();TerminateProcess(Process.Get(),1);WaitForSingleObject(Process.Get(),5000);
            SetLastError(error);Error("Start managed Plan 9 process");
        }
    }
    bool Alive() const {return WaitForSingleObject(Process.Get(),0)==WAIT_TIMEOUT;}
    bool Ready() const
    {
        if(!Alive()) return false;
        auto label=L"NanaBox9P-"+std::to_wstring(Pid);
        // Directory mount points may be NTFS reparse points rather than
        // registered volume mount paths. Query the mounted directory handle.
        auto attributes=GetFileAttributesW(Target.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY)) return false;
        NativeHandle directory(CreateFileW(Target.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        if(!directory.Valid()) return false;
        wchar_t volume[MAX_PATH]{},filesystem[MAX_PATH]{};
        return GetVolumeInformationByHandleW(directory.Get(),volume,MAX_PATH,nullptr,nullptr,nullptr,filesystem,MAX_PATH) &&
            Equal(volume,label) && Equal(filesystem,L"NanaBox9P");
    }
    void Signal() const {if(Alive()) SetEvent(StopEvent.Get());}
    void WaitReady()
    {
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        while(!Stop && Alive() && std::chrono::steady_clock::now()<end)
        {
            if(Ready()) return;
            if(WaitForSingleObject(Process.Get(),100)!=WAIT_TIMEOUT) break;
        }
        if(!Ready()) throw std::runtime_error("WinFsp Plan 9 mount did not become ready");
    }
    void Unmount()
    {
        if(!Alive()) return;
        ResetEvent(BusyEvent.Get());
        if(!SetEvent(StopEvent.Get())) Error("Request Plan 9 unmount");
        HANDLE events[]={Process.Get(),BusyEvent.Get()};
        auto result=WaitForMultipleObjects(2,events,FALSE,10000);
        if(result==WAIT_OBJECT_0+1) throw std::runtime_error("Plan 9 files are still open; close them before unmounting");
        if(result!=WAIT_OBJECT_0) throw std::runtime_error("Plan 9 client did not finish unmounting");
    }
};

class WindowsPlan9Mounts
{
    fs::path Executable,Root;
    bool Loopback;
    std::vector<WindowsPlan9Share> Shares;
    std::vector<std::unique_ptr<WindowsPlan9Process>> Processes;
    Json Preferences=Json::object();
    std::map<std::string,std::string> Errors;
    std::map<std::string,std::chrono::steady_clock::time_point> Retry;
    bool Loaded=false;
    auto FindShare(std::wstring const& name)
    {
        return std::find_if(Shares.begin(),Shares.end(),[&](auto const& s){return Equal(s.WideName,name);});
    }
    auto FindProcess(std::wstring const& name)
    {
        return std::find_if(Processes.begin(),Processes.end(),[&](auto const& p){return Equal(p->Share.WideName,name);});
    }
    void Active(bool value)
    {
        if(Loopback) return;
        if(Plan9Active.exchange(value)!=value && !Stop) Status(SERVICE_RUNNING,0);
    }
    void RefreshActive()
    {
        Active(std::any_of(Processes.begin(),Processes.end(),[](auto const& p){return p->Alive();}));
    }
    void Load()
    {
        if(Loaded || Executable.empty()) return;
        if(!Loopback)
        {
            Key key(StateKey,KEY_READ);
            auto value=key.Read(L"Plan9Preferences");
            if(!value.empty())
            {
                auto stored=Json::parse(Utf8(value),nullptr,false);
                if(stored.is_object() && stored.size()<=128) Preferences=std::move(stored);
            }
        }
        Loaded=true;
    }
    void Save(Json next)
    {
        if(next==Preferences) return;
        if(!Loopback)
        {
            Key key(StateKey,KEY_SET_VALUE);
            key.Write(L"Plan9Preferences",Wide(next.dump()));
        }
        Preferences=std::move(next);
    }
    void Enabled()
    {
        if(Executable.empty()) throw std::runtime_error("Install the Windows guest service with -EnableWinFspPlan9");
        ValidatePlan9Root(Root);
        auto attrs=GetFileAttributesW(Executable.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("The protected WinFsp Plan 9 client is missing");
    }
    bool Wanted(WindowsPlan9Share const& share) const
    {
        auto p=Preferences.find(share.Name);
        return p==Preferences.end()?share.AutoMount:p->at("Mounted").get<bool>();
    }
    fs::path Target(WindowsPlan9Share const& share) const
    {
        auto p=Preferences.find(share.Name);
        return WindowsPlan9Target(Root,share,p==Preferences.end()?std::string():p->value("Target",std::string()));
    }
    WindowsPlan9Process& Start(WindowsPlan9Share const& share,fs::path const& target)
    {
        Enabled();
        if(Stop) throw std::runtime_error("Guest service is stopping");
        for(auto const& p:Processes)
            if(Equal(p->Target.wstring(),target.wstring())) throw std::runtime_error("Plan 9 mount target is already managed");
        Active(true);
        try
        {
            Processes.push_back(std::make_unique<WindowsPlan9Process>(share,target,Executable,Loopback));
            return *Processes.back();
        }
        catch(...){RefreshActive();throw;}
    }
    void Drop(std::wstring const& name)
    {
        auto p=FindProcess(name);
        if(p==Processes.end()) return;
        (*p)->Unmount();
        Processes.erase(p);RefreshActive();
    }
public:
    WindowsPlan9Mounts():Executable(WinFspPlan9Path),Root(WindowsPlan9Root()),Loopback(false){}
    WindowsPlan9Mounts(fs::path executable,fs::path root):Executable(std::move(executable)),Root(std::move(root)),Loopback(true){}
    ~WindowsPlan9Mounts()
    {
        for(auto const& p:Processes) p->Signal();
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<end &&
            std::any_of(Processes.begin(),Processes.end(),[](auto const& p){return p->Alive();})) Sleep(50);
        Processes.clear();Active(false);
    }
    Json Query()
    {
        Json rows=Json::array();
        auto append=[&](WindowsPlan9Share const& share,bool available,WindowsPlan9Process* process)
        {
            auto row=share.Identity();
            auto ready=process && process->Ready();
            row["Available"]=available;row["Mounted"]=ready;
            row["Starting"]=process && process->Alive() && !ready;
            row["Suppressed"]=available && !Wanted(share);
            row["MountPoint"]=Utf8((process?process->Target:Target(share)).wstring());
            if(Executable.empty()) row["Error"]="WinFsp Plan 9 integration is disabled";
            else if(auto e=Errors.find(share.Name);e!=Errors.end()) row["Error"]=e->second;
            rows.push_back(std::move(row));
        };
        for(auto const& share:Shares)
        {
            auto p=FindProcess(share.WideName);
            append(share,true,p!=Processes.end() && (*p)->Share.Identity()==share.Identity()?p->get():nullptr);
        }
        for(auto const& p:Processes)
        {
            auto s=FindShare(p->Share.WideName);
            if(s==Shares.end() || s->Identity()!=p->Share.Identity()) append(p->Share,false,p.get());
        }
        return {{"Plan9Enabled",!Executable.empty()},{"Plan9Root",Utf8(Root.wstring())},{"Shares",rows}};
    }
    void Update(Json const& inventory)
    {
        auto next=ParseWindowsPlan9Inventory(inventory);
        Load();
        if(!Executable.empty())
        {
            Json preferences=Json::object();
            for(auto const& share:next)
            {
                auto p=Preferences.find(share.Name);
                if(p==Preferences.end() || !p->is_object()) continue;
                try
                {
                    if(p->at("Share")==share.Identity() && p->at("Mounted").is_boolean())
                    {
                        (void)WindowsPlan9Target(Root,share,p->value("Target",std::string()));
                        preferences[share.Name]=*p;
                    }
                }
                catch(std::exception const&){}
            }
            Save(std::move(preferences));
        }
        Shares=std::move(next);
        for(auto it=Retry.begin();it!=Retry.end();)
            if(std::none_of(Shares.begin(),Shares.end(),[&](auto const& s){return s.Name==it->first;}))
                it=Retry.erase(it);else ++it;
        auto now=std::chrono::steady_clock::now();
        bool removalAttempted=false;
        for(auto it=Processes.begin();it!=Processes.end();)
        {
            auto& p=**it;
            auto s=FindShare(p.Share.WideName);
            if(!p.Alive())
            {
                Errors[p.Share.Name]="WinFsp Plan 9 client exited";
                Retry[p.Share.Name]=now+std::chrono::seconds(15);
                it=Processes.erase(it);continue;
            }
            try
            {
                if(!removalAttempted && (s==Shares.end() || s->Identity()!=p.Share.Identity() ||
                    !Wanted(*s) || !Equal(Target(*s).wstring(),p.Target.wstring())))
                {
                    removalAttempted=true;
                    p.Unmount();Errors.erase(p.Share.Name);it=Processes.erase(it);continue;
                }
                if(p.Ready()) Errors.erase(p.Share.Name);
            }
            catch(std::exception const& e){Errors[p.Share.Name]=e.what();}
            ++it;
        }
        RefreshActive();
        if(Executable.empty()) return;
        unsigned starts=0;
        for(auto const& share:Shares)
        {
            if(Stop) break;
            if(!Wanted(share) || FindProcess(share.WideName)!=Processes.end() || Retry[share.Name]>now || starts>=4) continue;
            try{++starts;Start(share,Target(share));Errors.erase(share.Name);}
            catch(std::exception const& e){Errors[share.Name]=e.what();Retry[share.Name]=now+std::chrono::seconds(15);}
        }
    }
    void Action(std::string const& action,Json const& settings)
    {
        auto name=settings.at("Name").get<std::string>();auto wide=Wide(name);
        if(!ValidWindowsPlan9Name(wide) || InternalPlan9Name(wide))
            throw std::runtime_error("Invalid or reserved Plan 9 share name");
        Enabled();Load();
        auto share=FindShare(wide);
        if(share==Shares.end()) throw std::runtime_error("Unknown Plan 9 share; wait for the host inventory");
        auto preferences=Preferences;
        if(action=="Mount")
        {
            auto target=WindowsPlan9Target(Root,*share,settings.at("Target").get<std::string>());
            auto p=FindProcess(wide);
            if(p!=Processes.end() && (!(*p)->Alive() || (*p)->Share.Identity()!=share->Identity() ||
                !Equal((*p)->Target.wstring(),target.wstring())))
            {
                Drop(wide);p=Processes.end();
            }
            auto& process=p==Processes.end()?Start(*share,target):**p;
            process.WaitReady();
            preferences[share->Name]={{"Share",share->Identity()},{"Mounted",true},{"Target",Utf8(target.wstring())}};
        }
        else if(action=="Unmount")
        {
            Drop(wide);
            preferences[share->Name]={{"Share",share->Identity()},{"Mounted",false},{"Target",""}};
        }
        else throw std::runtime_error("Unsupported Plan 9 mount operation");
        Save(std::move(preferences));Errors.erase(share->Name);Retry.erase(share->Name);
    }
};

int WindowsPlan9ManagerTest(fs::path executable,fs::path root)
{
    WindowsPlan9Mounts manager(std::move(executable),std::move(root));
    std::string line;
    while(std::getline(std::cin,line))
    {
        try
        {
            auto request=Json::parse(line);
            auto action=request.at("RequestType").get<std::string>();
            if(action=="Exit") return 0;
            if(action=="Update") manager.Update(request.at("Shares"));
            else if(action!="Query") manager.Action(action,request.at("Settings"));
            std::cout<<Json{{"Success",true},{"Data",manager.Query()}}.dump()<<std::endl;
        }
        catch(std::exception const& e)
        {std::cout<<Json{{"Success",false},{"Error",e.what()}}.dump()<<std::endl;}
    }
    return 0;
}
