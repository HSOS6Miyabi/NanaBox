// Included in the Windows guest service implementation namespace.
// User directory mappings are independent of the lifetime of the VSMB transport.
class WindowsVsmbMounts
{
public:
    struct Operations
    {
        std::function<bool(VirtualSmbDescriptor const&)> Mapped;
        std::function<void(VirtualSmbDescriptor const&)> Mount,Unmount;
        std::function<Json()> Load;
        std::function<void(Json const&)> Save;
    };
private:
    struct Record
    {
        VirtualSmbDescriptor Share;
        std::optional<bool> Manual;
        bool Available=false;
        std::string Error;
        bool Wanted() const {return Available&&Manual.value_or(Share.AutoMount);}
    };
    fs::path Root;
    Operations Ops;
    bool Loaded=false,Enabled=true;
    std::vector<Record> Records;
    std::string InventoryError;
    static fs::path DefaultRoot()
    {
        wchar_t value[32768]{};
        auto size=GetEnvironmentVariableW(L"ProgramData",value,static_cast<DWORD>(std::size(value)));
        if(!size||size>=std::size(value))throw std::runtime_error("ProgramData is unavailable");
        return fs::path(value)/L"NanaBox Guest Shares";
    }
    static bool SameProvider(VirtualSmbDescriptor const& a,VirtualSmbDescriptor const& b)
    {
        return a.Name==b.Name&&a.ReadOnly==b.ReadOnly&&a.NoDirectmap==b.NoDirectmap;
    }
    static Json Serialize(std::vector<Record> const& records)
    {
        Json result=Json::array();
        for(auto const& record:records)
        {
            auto row=record.Share.Identity();
            if(record.Manual)row["Manual"]=*record.Manual;
            result.push_back(std::move(row));
        }
        return result;
    }
    void Load()
    {
        if(Loaded)return;
        auto state=Ops.Load();
        auto descriptors=ParseVirtualSmbInventory(state);
        std::vector<Record> restored;
        for(size_t i=0;i<descriptors.size();++i)
        {
            if(descriptors[i].Internal)throw std::runtime_error("Reserved share in saved VSMB mount state");
            Record record{descriptors[i],{},false,{}};
            if(state[i].contains("Manual"))record.Manual=state[i].at("Manual").get<bool>();
            restored.push_back(std::move(record));
        }
        Records=std::move(restored);
        Loaded=true;
    }
    void Save(std::vector<Record> const& next)
    {
        auto state=Serialize(next);
        if(state!=Serialize(Records))Ops.Save(state);
        Records=next;
    }
    static void CheckRoot(fs::path const& root)
    {
        ValidatePlan9Root(root);
    }
    void Reconcile(Record& row)
    {
        if(!Enabled)return;
        try
        {
            if(row.Wanted())Ops.Mount(row.Share);
            else Ops.Unmount(row.Share);
            row.Error.clear();
        }
        catch(std::exception const& e){row.Error=e.what();}
    }
    Operations NativeOps()
    {
        return Operations{
            [this](auto const& share)
            {
                CheckRoot(Root);
                return VsmbClient.Mapped(Root/share.WideName,share.WideName,share.ReadOnly);
            },
            [this](auto const& share)
            {
                CheckRoot(Root);
                if(!VsmbClient.Ready(Root/share.WideName,share.WideName,share.ReadOnly))
                    VsmbClient.Ensure(Root/share.WideName,share.WideName,share.ReadOnly);
            },
            [this](auto const& share)
            {
                CheckRoot(Root);
                (void)VsmbClient.Detach(Root/share.WideName,share.WideName,share.ReadOnly);
            },
            []()
            {
                Key key(StateKey,KEY_READ);
                auto value=key.Read(L"VirtualSmbMountState");
                return value.empty()?Json::array():Json::parse(Utf8(value));
            },
            [](Json const& state)
            {
                Key key(StateKey,KEY_SET_VALUE);
                key.Write(L"VirtualSmbMountState",Wide(state.dump()));
            }
        };
    }
public:
    WindowsVsmbMounts():Root(DefaultRoot()),Ops(NativeOps()),Enabled(NativeMode){}
    WindowsVsmbMounts(fs::path root,Operations ops):Root(std::move(root)),Ops(std::move(ops)){}
    void Update(std::vector<VirtualSmbDescriptor> const& inventory)
    {
        try
        {
            if(!Enabled)
            {
                Records.clear();
                for(auto const& share:inventory)
                    if(!share.Internal)Records.push_back({share,{},true,"Native VSMB integration is disabled"});
                return;
            }
            Load();
            std::vector<Record> next;
            // Remove only exact, previously published directory mappings.
            // Keep a failed removal in the journal and retry on the next update.
            for(auto const& old:Records)
            {
                auto it=std::find_if(inventory.begin(),inventory.end(),[&](auto const& s)
                    {return !s.Internal&&SameProvider(s,old.Share);});
                if(it!=inventory.end())
                {
                    auto row=old;
                    if(row.Share.AutoMount!=it->AutoMount)row.Manual.reset();
                    row.Share=*it;row.Available=true;
                    next.push_back(std::move(row));
                }
                else
                {
                    auto row=old;row.Available=false;row.Manual.reset();
                    Reconcile(row);
                    if(!row.Error.empty())next.push_back(std::move(row));
                }
            }
            for(auto const& share:inventory)
            {
                if(share.Internal)continue;
                // A stale mapping with this name must be detached before
                // the new definition can claim its directory.
                if(std::none_of(next.begin(),next.end(),[&](auto const& row)
                    {return Equal(row.Share.WideName,share.WideName);}))
                    next.push_back({share,{},true,{}});
            }
            // Persist definitions before creating a mapping, so a service
            // restart can identify and safely detach the exact old mapping.
            if(next.size()>224)throw std::runtime_error("Too many pending VSMB mappings");
            Save(next);
            InventoryError.clear();
            for(auto& row:Records)
                if(row.Available)Reconcile(row);
        }
        catch(std::exception const& e){InventoryError=e.what();}
    }
    void Action(std::string const& action,std::string const& name)
    {
        if(!Enabled)throw std::runtime_error("Native VSMB integration is disabled");
        Load();
        auto wide=Wide(name);
        if(name.size()>200||!ValidWindowsPlan9Name(wide)||InternalPlan9Name(wide))
            throw std::runtime_error("Invalid or reserved Virtual SMB share name");
        auto next=Records;
        auto it=std::find_if(next.begin(),next.end(),[&](auto const& row)
            {return row.Available&&Equal(row.Share.WideName,wide);});
        if(it==next.end())throw std::runtime_error("Unknown Virtual SMB share; wait for the host inventory");
        if(action!="VsmbMount"&&action!="VsmbUnmount")
            throw std::runtime_error("Unsupported Virtual SMB mount operation");
        auto index=static_cast<size_t>(it-next.begin());
        it->Manual=action=="VsmbMount";
        Save(next);
        Reconcile(Records[index]);
        if(!Records[index].Error.empty())throw std::runtime_error(Records[index].Error);
    }
    Json Query()
    {
        Json shares=Json::array(),errors=Json::array();
        if(!InventoryError.empty())errors.push_back(InventoryError);
        for(auto const& record:Records)
        {
            auto row=record.Share.Identity();
            row["Available"]=record.Available;
            row["Suppressed"]=record.Available&&!record.Wanted();
            row["Target"]=Utf8((Root/record.Share.WideName).wstring());
            row["Mounted"]=false;
            auto error=record.Error;
            if(Enabled)
            {
                try{row["Mounted"]=Ops.Mapped(record.Share);}
                catch(std::exception const& e){if(error.empty())error=e.what();}
            }
            if(!error.empty())
            {
                row["Error"]=error;
                errors.push_back(record.Share.Name+": "+error);
            }
            shares.push_back(std::move(row));
        }
        return {{"VirtualSmbEnabled",Enabled},{"VirtualSmbRoot",Utf8(Root.wstring())},
            {"VirtualSmbShares",shares},{"ShareErrors",errors}};
    }
};

bool WindowsVsmbMountStateTest()
{
    Json journal=Json::array();
    std::map<std::string,bool> mapped;
    bool failDetach=false;
    unsigned mounts=0,detaches=0;
    auto operations=WindowsVsmbMounts::Operations{
        [&](auto const& s){auto it=mapped.find(s.Name);return it!=mapped.end()&&it->second==s.ReadOnly;},
        [&](auto const& s)
        {
            auto it=mapped.find(s.Name);
            if(it!=mapped.end()&&it->second!=s.ReadOnly)throw std::runtime_error("Mount conflict");
            if(it==mapped.end()){mapped[s.Name]=s.ReadOnly;++mounts;}
        },
        [&](auto const& s)
        {
            auto it=mapped.find(s.Name);
            if(it==mapped.end())return;
            if(it->second!=s.ReadOnly||failDetach)throw std::runtime_error("Mount busy");
            mapped.erase(it);++detaches;
        },
        [&](){return journal;},
        [&](Json const& s){journal=s;}
    };
    VirtualSmbDescriptor automatic{"Projects",L"Projects",false,false,true,false};
    VirtualSmbDescriptor manual{"ReadOnly",L"ReadOnly",true,false,false,true};
    VirtualSmbDescriptor internal{"NanaBox.HostDrivers",L"NanaBox.HostDrivers",true,true,true,false};
    auto inventory=std::vector<VirtualSmbDescriptor>{automatic,manual,internal};
    fs::path root=L"C:\\ProgramData\\NanaBox Guest Shares";
    {
        WindowsVsmbMounts manager(root,operations);
        manager.Update(inventory);
        if(mapped.size()!=1||!mapped.count("Projects")||journal.size()!=2)return false;
        manager.Action("VsmbMount","readonly");
        if(mapped.size()!=2||!mapped.at("ReadOnly"))return false;
        manager.Action("VsmbUnmount","Projects");
        if(mapped.count("Projects")||!manager.Query()["VirtualSmbShares"][0]["Suppressed"].get<bool>())return false;
        manager.Update(inventory);
        if(mapped.size()!=1||mounts!=2)return false;
    }
    WindowsVsmbMounts manager(root,operations);
    manager.Update(inventory);
    if(mapped.size()!=1||!mapped.count("ReadOnly")||mounts!=2)return false;
    failDetach=true;
    manager.Update({automatic});
    auto status=manager.Query();
    if(!mapped.count("ReadOnly")||status["ShareErrors"].empty()||journal.size()!=2)return false;
    failDetach=false;
    manager.Update({automatic});
    if(!mapped.empty()||journal.size()!=1||detaches!=2)return false;
    manager.Action("VsmbMount","Projects");
    automatic.ReadOnly=true;
    manager.Update({automatic});
    if(mapped.size()!=1||!mapped.at("Projects"))return false;
    for(auto const& bad:{"../escape","NanaBox.HostDrivers","missing"})
    {
        bool rejected=false;
        try{manager.Action("VsmbMount",bad);}catch(std::exception const&){rejected=true;}
        if(!rejected)return false;
    }
    manager.Update({});
    return mapped.empty()&&journal.empty();
}


int WindowsVsmbLocalIntegrationTest()
{
    auto root=fs::temp_directory_path()/("NanaBox-VSMB-Manager-"+std::to_string(GetCurrentProcessId()));
    if(!fs::create_directory(root))throw std::runtime_error("Cannot create VSMB test directory");
    auto local=root/L"mounts",source=root/L"exports";
    fs::create_directory(local);fs::create_directory(source);
    VirtualSmbDescriptor automatic{"Projects",L"Projects",false,false,true,false};
    VirtualSmbDescriptor manual{"ReadOnly",L"ReadOnly",true,false,false,true};
    auto inventory=std::vector<VirtualSmbDescriptor>{automatic,manual};
    Json journal=Json::array();
    auto ops=WindowsVsmbMounts::Operations{
        [&](auto const& s){return VsmbClient.MappedLocalForTest(local/s.WideName,source/s.WideName,s.ReadOnly);},
        [&](auto const& s){VsmbClient.MapLocalForTest(local/s.WideName,source/s.WideName,s.ReadOnly);},
        [&](auto const& s){VsmbClient.DetachLocalForTest(local/s.WideName,source/s.WideName,s.ReadOnly);},
        [&](){return journal;},[&](Json const& state){journal=state;}
    };
    auto require=[](bool result,char const* message){if(!result)throw std::runtime_error(message);};
    auto create=[](fs::path const& path)
    {
        NativeHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!file.Valid())Error("Create local VSMB integration fixture");
        char data[]="data";DWORD count=0;
        if(!WriteFile(file.Get(),data,4,&count,nullptr)||count!=4)Error("Write local VSMB integration fixture");
    };
    try
    {
        for(auto const& s:inventory)
        {
            fs::create_directory(source/s.WideName);
            create(source/s.WideName/L"host.txt");
        }
        {
            WindowsVsmbMounts mounts(local,ops);
            mounts.Update(inventory);
            auto status=mounts.Query();
            require(status.at("ShareErrors").empty(),"Automatic VSMB mapping failed");
            require(status["VirtualSmbShares"][0]["Mounted"]==true,"Automatic VSMB share is not mounted");
            require(status["VirtualSmbShares"][1]["Mounted"]==false,"Manual VSMB share mounted unexpectedly");
            require(fs::file_size(local/L"Projects"/L"host.txt")==4,"Bound source data is missing");
            create(local/L"Projects"/L"guest.txt");
            require(fs::file_size(source/L"Projects"/L"guest.txt")==4,"Writable binding did not update source");
            mounts.Action("VsmbMount","ReadOnly");
            require(fs::file_size(local/L"ReadOnly"/L"host.txt")==4,"Manual mapping unavailable");
            {
                NativeHandle write(CreateFileW((local/L"ReadOnly"/L"host.txt").c_str(),GENERIC_WRITE,
                    FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                require(!write.Valid(),"Read-only VSMB accepted write access");
            }
            mounts.Action("VsmbUnmount","Projects");
            require(!fs::exists(local/L"Projects"/L"host.txt"),"Detached local directory still exposes source");
            require(fs::exists(source/L"Projects"/L"guest.txt"),"Detachment removed host data");
        }
        {
            WindowsVsmbMounts restored(local,ops);
            restored.Update(inventory);
            auto status=restored.Query();
            require(status["VirtualSmbShares"][0]["Suppressed"]==true&&
                status["VirtualSmbShares"][0]["Mounted"]==false,"Unmount preference was not restored");
            require(status["VirtualSmbShares"][1]["Mounted"]==true,"Manual mount was not restored");
            restored.Action("VsmbUnmount","ReadOnly");
            // A pre-existing directory with files must not be overwritten.
            create(local/L"Projects"/L"unrelated.txt");
            bool conflict=false;
            try{restored.Action("VsmbMount","Projects");}catch(std::exception const&){conflict=true;}
            require(conflict&&fs::exists(local/L"Projects"/L"unrelated.txt"),"Nonempty directory conflict was not preserved");
            fs::remove(local/L"Projects"/L"unrelated.txt");
            restored.Action("VsmbMount","Projects");
            require(fs::exists(local/L"Projects"/L"guest.txt"),"Remount did not recover host data");
            restored.Update({});
            require(restored.Query()["VirtualSmbShares"].empty()&&journal.empty(),"Obsolete bindings were not removed");
            require(!fs::exists(local/L"Projects"/L"guest.txt")&&
                fs::exists(source/L"Projects"/L"guest.txt"),"Obsolete share removal damaged source");
        }
        fs::remove_all(root);
        std::cout<<"Native VSMB mount manager passed (automatic, manual, read-only, writable, restart and conflict recovery)\n";
        return 0;
    }
    catch(...)
    {
        // Never recursively delete a fixture while any mapping might remain.
        for(auto const& s:inventory)
        {
            try{VsmbClient.DetachLocalForTest(local/s.WideName,source/s.WideName,s.ReadOnly);}
            catch(...){throw std::runtime_error("VSMB fixture cleanup failed; retained isolated test directory");}
        }
        std::error_code ec;fs::remove_all(root,ec);
        throw;
    }
}
