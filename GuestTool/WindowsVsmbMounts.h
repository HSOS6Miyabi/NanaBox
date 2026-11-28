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
