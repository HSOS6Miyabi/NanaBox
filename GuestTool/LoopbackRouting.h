#pragma once

    class LoopbackRouting
    {
        static constexpr std::uint64_t LoopTable=0x3ffffffd,HostTable=0x3ffffffe;
        static constexpr char Gateway[]="169.254.73.249",GatewayMac[]="00:11:22:33:44:55";
        using Rule=std::tuple<std::uint64_t,std::string,std::string,std::string,std::string,std::string>;
        using Route=std::tuple<std::uint64_t,std::string,std::string>;
        NetworkRunner Run;
        fs::path StatePath;

        static bool SafeDevice(std::string const& dev)
        {
            return !dev.empty()&&dev.size()<IFNAMSIZ&&dev.front()!='-'&&dev!="."&&dev!=".."&&
                std::all_of(dev.begin(),dev.end(),[](unsigned char c)
                {return c>32&&c<127&&c!='/'&&c!='='&&c!=':';});
        }
        std::string IpCommand(Arguments args)
        {
            args.insert(args.begin(),{"ip","-N","-4"});return Run(args);
        }
        Json Read(Arguments args){return Json::parse(IpCommand(std::move(args)));}
        static std::string TableId(Json const& row)
        {
            if(!row.contains("table"))return {};
            auto t=Scalar(row.at("table"));return t=="local"?"255":t;
        }
        static bool Owned(Json const& row)
        {return row.contains("protocol")&&Scalar(row.at("protocol"))=="243";}
        static Rule Key(Json const& row)
        {
            auto protocol=row.contains("ipproto")?Scalar(row.at("ipproto")):std::string();
            if(protocol.rfind("ipproto-",0)==0)protocol.erase(0,8);
            if(protocol=="6")protocol="tcp";else if(protocol=="17")protocol="udp";
            auto port=row.contains("dport")?Scalar(row.at("dport")):std::string();
            auto dash=port.find('-');
            if(dash!=std::string::npos&&port.substr(0,dash)==port.substr(dash+1))port=port.substr(0,dash);
            auto dest=row.value("dst",std::string("all"));
            if(dest!="all")dest=NetworkPrefix(dest);
            return {Integer(row.at("priority"),0,UINT32_MAX),TableId(row),row.value("iif",std::string()),protocol,dest,port};
        }
        static Rule DefaultRule(){return {0,"255","","","all",""};}
        static Rule FallbackRule(){return {2,"255","","","all",""};}
        static bool DefaultLocal(Json const& row)
        {
            if(Owned(row)||Key(row)!=DefaultRule()||row.value("src",std::string("all"))!="all"||row.contains("fwmark"))return false;
            static const std::set<std::string> fields={"priority","src","dst","table","protocol","flags"};
            for(auto const& item:row.items())if(!fields.count(item.key()))return false;
            return true;
        }
        void RuleChange(std::string verb,Rule const& rule,bool owned=true)
        {
            auto const& [priority,table,dev,protocol,destination,port]=rule;
            Arguments args={"rule",std::move(verb),"pref",std::to_string(priority)};
            if(!dev.empty())args.insert(args.end(),{"iif",dev});
            if(destination!="all")args.insert(args.end(),{"to",destination});
            if(!protocol.empty())args.insert(args.end(),{"ipproto",protocol});
            if(!port.empty())args.insert(args.end(),{"dport",port});
            args.insert(args.end(),{"lookup",table});
            if(owned)args.insert(args.end(),{"protocol","243"});
            IpCommand(args);
        }
        void RouteChange(std::string verb,Route const& route)
        {
            auto const& [table,destination,dev]=route;
            Arguments args={"route",verb,destination,"via",Gateway,"dev",dev,"table",std::to_string(table),"proto","243"};
            if(verb!="del")args.push_back("onlink");
            IpCommand(args);
        }
        static Route RouteKeyFor(Json const& row)
        {
            return {std::stoull(TableId(row)),NetworkPrefix(row.at("dst").get<std::string>()),row.at("dev").get<std::string>()};
        }
        static std::string Trim(std::string const& text)
        {auto words=Words(text);if(words.size()!=1)Fail("Invalid sysctl value");return words[0];}
        static std::string DeviceFromKey(std::string const& key)
        {
            constexpr std::string_view prefix="net/ipv4/conf/";
            if(key.rfind(prefix,0)!=0)Fail("Invalid loopback state sysctl");
            auto pos=key.find('/',prefix.size());
            if(pos==std::string::npos)Fail("Invalid loopback state sysctl");
            auto dev=key.substr(prefix.size(),pos-prefix.size()),name=key.substr(pos+1);
            if(!SafeDevice(dev)||(name!="accept_local"&&name!="rp_filter"&&name!="route_localnet"))
                Fail("Invalid loopback state sysctl");
            return dev;
        }
        Json State()
        {
            std::error_code ec;
            auto info=fs::symlink_status(StatePath,ec);
            if(info.type()==fs::file_type::not_found)return {{"Sysctls",Json::object()},{"Neighbors",Json::object()}};
            if(ec||!fs::is_regular_file(info))Fail("Unsafe loopback state file");
            struct stat st{};
            if(lstat(StatePath.c_str(),&st)||st.st_uid!=geteuid()||(st.st_mode&0022)||st.st_size>1024*1024)
                Fail("Unsafe loopback state ownership");
            auto state=Json::parse(ReadText(StatePath));
            Fields(state,{"Sysctls","Neighbors"});
            if(!state["Sysctls"].is_object()||!state["Neighbors"].is_object())Fail("Invalid loopback state");
            for(auto const& entry:state["Sysctls"].items())
            {
                (void)DeviceFromKey(entry.key());
                auto const& pair=entry.value();
                if(!pair.is_array()||pair.size()!=2)Fail("Invalid loopback sysctl record");
                for(auto const& v:pair)
                {
                    auto s=v.get<std::string>();
                    if(s!="0"&&s!="1"&&s!="2")Fail("Invalid loopback sysctl value");
                }
            }
            for(auto const& entry:state["Neighbors"].items())
                if(!SafeDevice(entry.key())||entry.value()!=true)Fail("Invalid loopback neighbor record");
            return state;
        }
        void Save(Json const& state)
        {
            CheckRoot(StatePath.parent_path());
            auto temporary=StatePath.parent_path()/(".loopback-"+Token());
            File file;
            file.Open(temporary,true);
            struct Cleanup{fs::path Path;~Cleanup(){std::error_code ec;fs::remove(Path,ec);}} cleanup{temporary};
            auto text=state.dump();
            file.Write(std::vector<unsigned char>(text.begin(),text.end()));file.Flush();file.Close();
            if(rename(temporary.c_str(),StatePath.c_str()))Fail("Cannot publish loopback state");
        }
        void Sysctl(Json& state,std::string const& dev,std::string const& name,std::string const& wanted)
        {
            auto key="net/ipv4/conf/"+dev+"/"+name;
            auto current=Trim(Run({"sysctl","-n",key}));
            if(!state["Sysctls"].contains(key))
            {
                state["Sysctls"][key]=Json::array({current,wanted});Save(state);
            }
            if(current!=wanted)Run({"sysctl","-w",key+"="+wanted});
        }
        Json Neighbors(std::string const& dev){return Read({"-j","neigh","show","to",Gateway,"dev",dev});}
        static bool ManagedNeighbor(Json const& rows)
        {
            if(rows.size()!=1||rows[0].value("lladdr",std::string())!=GatewayMac)return false;
            auto state=rows[0].value("state",Json::array());
            return std::find(state.begin(),state.end(),Json("PERMANENT"))!=state.end();
        }
        void Neighbor(Json& state,std::string const& dev)
        {
            auto rows=Neighbors(dev);
            if(!rows.empty()&&!ManagedNeighbor(rows))Fail("Loopback gateway neighbor belongs to another application");
            if(rows.empty())
            {
                state["Neighbors"][dev]=true;Save(state);
                IpCommand({"neigh","replace",Gateway,"lladdr",GatewayMac,"nud","permanent","dev",dev});
            }
        }
        void RestoreUnused(Json& state,std::set<std::string> const& devices)
        {
            std::set<std::string> links;
            for(auto const& link:Read({"-j","link","show"}))links.insert(link.at("ifname").get<std::string>());
            auto& neighbors=state["Neighbors"];
            for(auto it=neighbors.begin();it!=neighbors.end();)
            {
                auto dev=it.key();
                if(devices.count(dev)){++it;continue;}
                if(links.count(dev)&&ManagedNeighbor(Neighbors(dev)))IpCommand({"neigh","del",Gateway,"dev",dev});
                it=neighbors.erase(it);Save(state);
            }
            auto& sysctls=state["Sysctls"];
            for(auto it=sysctls.begin();it!=sysctls.end();)
            {
                auto key=it.key(),dev=DeviceFromKey(key);
                if(devices.count(dev)||(dev=="all"&&!devices.empty())){++it;continue;}
                if(dev=="all"||links.count(dev))
                {
                    auto current=Trim(Run({"sysctl","-n",key}));
                    auto original=it.value()[0].get<std::string>(),managed=it.value()[1].get<std::string>();
                    if(current==managed&&current!=original)Run({"sysctl","-w",key+"="+original});
                }
                it=sysctls.erase(it);Save(state);
            }
        }
        void Prepare(Json& state,std::set<std::string> const& devices)
        {
            for(auto const& dev:devices)
            {
                Sysctl(state,dev,"accept_local","1");Sysctl(state,dev,"rp_filter","0");
                Sysctl(state,dev,"route_localnet","1");Neighbor(state,dev);
            }
            Sysctl(state,"all","rp_filter","0");
        }
    public:
        explicit LoopbackRouting(fs::path path,NetworkRunner runner=RunNetworkCommand):
            Run(std::move(runner)),StatePath(std::move(path)){}
        Json Rules(){return Read({"-j","rule","show"});}
        Json Routes()
        {
            Json result=Json::array();
            for(auto const& row:Read({"-j","route","show","table","all"}))
                if(TableId(row)==std::to_string(LoopTable)||TableId(row)==std::to_string(HostTable))
                    result.push_back(row);
            return result;
        }
        void Clear()
        {
            auto state=State();auto rules=Rules();
            bool hasOwned=std::any_of(rules.begin(),rules.end(),[](auto const& row){return Owned(row);});
            auto routes=Routes();
            bool hasRoutes=std::any_of(routes.begin(),routes.end(),[](auto const& row){return Owned(row);});
            if(!hasOwned&&!hasRoutes&&!fs::exists(StatePath))return;
            if(!std::any_of(rules.begin(),rules.end(),[](auto const& row){return DefaultLocal(row);}))
                RuleChange("add",DefaultRule(),false);
            for(auto const& row:rules)if(Owned(row))RuleChange("del",Key(row));
            for(auto const& row:routes)if(Owned(row))RouteChange("del",RouteKeyFor(row));
            RestoreUnused(state,{});
            fs::remove(StatePath);
        }
        Json Reconcile(std::string const& gelnic,std::map<std::string,std::vector<std::string>> const& interfaces,
            std::set<unsigned> const& localPorts)
        {
            std::set<std::string> devices={gelnic};
            std::set<Route> desiredRoutes={{LoopTable,"127.0.0.1/32",gelnic}};
            for(auto const& entry:interfaces)
            {
                if(!devices.insert(entry.first).second)Fail("Duplicate loopback adapter");
                for(auto const& address:entry.second)desiredRoutes.emplace(HostTable,address+"/32",entry.first);
            }
            for(auto const& dev:devices)if(dev=="lo"||!SafeDevice(dev))Fail("A dedicated loopback adapter is required");
            std::set<Rule> desired={FallbackRule()};
            for(auto protocol:{"tcp","udp"})
            {
                for(auto const& dev:devices)desired.emplace(0,"255",dev,protocol,"all","");
                desired.emplace(1,std::to_string(LoopTable),"",protocol,"all","");
                if(desiredRoutes.size()>1)desired.emplace(1,std::to_string(HostTable),"",protocol,"all","");
                for(auto port:localPorts)desired.emplace(0,"255","",protocol,"127.0.0.1/32",std::to_string(port));
            }
            auto rules=Rules();
            std::set<Rule> actual;unsigned actualCount=0,defaults=0;
            for(auto const& row:rules)
            {
                if(Owned(row)){actual.insert(Key(row));++actualCount;}
                else if(DefaultLocal(row))++defaults;
                else if(Integer(row.at("priority"),0,UINT32_MAX)<=2)
                    Fail("Existing high-priority IPv4 rules conflict with mirrored loopback");
            }
            if(defaults>1)Fail("Duplicate local routing rules");
            auto routes=Routes();std::set<Route> actualRoutes;
            for(auto const& row:routes)
            {
                if(!Owned(row))Fail("Loopback routing table contains an unmanaged route");
                if(row.value("gateway",std::string())==Gateway)actualRoutes.insert(RouteKeyFor(row));
            }
            auto state=State();
            try
            {
                Prepare(state,devices);
                auto flags=Read({"-j","link","show","dev",gelnic}).at(0).at("flags");
                if(std::find(flags.begin(),flags.end(),Json("UP"))==flags.end())
                    IpCommand({"link","set","dev",gelnic,"up"});
                if(actualRoutes!=desiredRoutes||routes.size()!=desiredRoutes.size()||
                    actual!=desired||actualCount!=desired.size()||defaults)
                {
                    Clear();state=State();Prepare(state,devices);
                    for(auto const& route:desiredRoutes)RouteChange("replace",route);
                    RuleChange("add",FallbackRule());RuleChange("del",DefaultRule(),false);
                    for(auto const& rule:desired)if(rule!=FallbackRule())RuleChange("add",rule);
                }
                RestoreUnused(state,devices);
                actual.clear();actualCount=0;
                for(auto const& row:Rules())if(Owned(row)){actual.insert(Key(row));++actualCount;}
                if(actual!=desired||actualCount!=desired.size())Fail("Loopback policy rules did not converge");
                actualRoutes.clear();
                for(auto const& row:Routes())if(Owned(row)&&row.value("gateway",std::string())==Gateway)
                    actualRoutes.insert(RouteKeyFor(row));
                if(actualRoutes!=desiredRoutes)Fail("Loopback policy routes did not converge");
                return {{"Loopback",true},{"LoopbackDevice",gelnic},{"HostAddressRouteCount",desiredRoutes.size()-1}};
            }
            catch(...)
            {
                auto original=std::current_exception();
                try{Clear();}catch(std::exception const& e){throw std::runtime_error(std::string("Loopback rollback failed: ")+e.what());}
                std::rethrow_exception(original);
            }
        }
    };

    void NativeLoopbackIntegrationTest()
    {
        RunNetworkCommand({"ip","link","set","lo","up"});
        RunNetworkCommand({"ip","link","add","nb-gel","type","veth","peer","name","nb-peer"});
        RunNetworkCommand({"ip","link","add","nb-mirror","type","veth","peer","name","nb-other"});
        for(auto dev:{"nb-gel","nb-peer","nb-mirror","nb-other"})
            RunNetworkCommand({"ip","link","set",dev,"up"});
        RunNetworkCommand({"ip","address","add","192.0.2.10/24","dev","nb-mirror"});
        auto original=RunNetworkCommand({"sysctl","-n","net/ipv4/conf/all/rp_filter"});
        auto root=fs::temp_directory_path()/("NanaBox-Loopback-"+Token());
        fs::create_directory(root);chmod(root.c_str(),0700);
        struct Cleanup{fs::path Root;~Cleanup(){std::error_code ec;fs::remove_all(Root,ec);}} cleanup{root};
        auto state=root/"state.json";
        LoopbackRouting router(state);
        auto check=[](std::string const& destination,std::string const& protocol,std::string const& dev,bool dns=false)
        {
            Arguments args={"ip","-4","-j","route","get",destination,"ipproto",protocol};
            if(dns)args.insert(args.end(),{"dport","53"});
            auto result=Json::parse(RunNetworkCommand(args));
            if(result.at(0).at("dev")!=dev)Fail("Unexpected loopback route");
        };
        check("127.0.0.1","tcp","lo");
        try
        {
            auto report=router.Reconcile("nb-gel",{{"nb-mirror",{"192.0.2.10"}}},{53});
            if(report.at("HostAddressRouteCount")!=1)Fail("Host loopback route missing");
            check("127.0.0.1","tcp","nb-gel");check("127.0.0.1","udp","nb-gel");
            check("127.0.0.1","icmp","lo");check("127.0.0.1","udp","lo",true);
            check("192.0.2.10","tcp","nb-mirror");
            LoopbackRouting fresh(state);auto before=fresh.Rules();
            fresh.Reconcile("nb-gel",{{"nb-mirror",{"192.0.2.10"}}},{53});
            if(fresh.Rules()!=before)Fail("Loopback reconciliation is not idempotent");
            fresh.Reconcile("nb-gel",{{"nb-mirror",{}}},{53});check("192.0.2.10","tcp","lo");
        }
        catch(...){router.Clear();throw;}
        router.Clear();check("127.0.0.1","tcp","lo");
        if(RunNetworkCommand({"sysctl","-n","net/ipv4/conf/all/rp_filter"})!=original||fs::exists(state)||!router.Routes().empty())
            Fail("Loopback cleanup failed");
        std::cout<<"Native GELNIC loopback routing integration passed\n";
    }
