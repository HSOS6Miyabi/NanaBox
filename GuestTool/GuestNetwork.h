#pragma once

    struct Ip
    {
        int Family = 0;
        std::array<unsigned char, 16> Bytes{};
        static Ip Parse(std::string const& text)
        {
            Ip ip;
            if (text.empty() || text.find('\0') != std::string::npos) GuestFail("Invalid IP address");
            if (inet_pton(AF_INET, text.c_str(), ip.Bytes.data()) == 1) ip.Family = 4;
            else if (inet_pton(AF_INET6, text.c_str(), ip.Bytes.data()) == 1) ip.Family = 6;
            else GuestFail("Invalid IP address");
            return ip;
        }
        std::string Text() const
        {
            char out[INET6_ADDRSTRLEN]{};
            if (!inet_ntop(Family == 4 ? AF_INET : AF_INET6, Bytes.data(), out, sizeof(out)))
                GuestFail("Cannot format IP address");
            return out;
        }
        unsigned Bits() const { return Family == 4 ? 32u : 128u; }
        bool Zero() const
        { return std::all_of(Bytes.begin(), Bytes.begin() + Bits()/8, [](auto c) { return c == 0; }); }
        bool Loopback() const
        {
            if (Family == 4) return Bytes[0] == 127;
            return Bytes[15] == 1 && std::all_of(Bytes.begin(), Bytes.begin()+15, [](auto c) { return c == 0; });
        }
        bool Multicast() const { return Family == 4 ? (Bytes[0] & 0xf0) == 0xe0 : Bytes[0] == 0xff; }
        bool LinkLocal() const { return Family == 6 && Bytes[0] == 0xfe && (Bytes[1] & 0xc0) == 0x80; }
        Ip Mapped() const
        {
            if (Family != 6 || Bytes[10] != 255 || Bytes[11] != 255 ||
                !std::all_of(Bytes.begin(), Bytes.begin()+10, [](auto c) { return c == 0; })) return *this;
            Ip v4; v4.Family = 4; std::copy(Bytes.begin()+12, Bytes.end(), v4.Bytes.begin()); return v4;
        }
    };

    std::string NetworkPrefix(std::string text, unsigned* bits = nullptr)
    {
        auto slash = text.find('/');
        auto ip = Ip::Parse(text.substr(0, slash));
        unsigned prefix = ip.Bits();
        if (slash != std::string::npos)
        {
            auto part = text.substr(slash + 1);
            if (part.empty() || !std::all_of(part.begin(), part.end(), [](char c){ return c >= '0' && c <= '9'; }) ||
                part.size() > 3) GuestFail("Invalid network prefix");
            prefix = static_cast<unsigned>(std::stoul(part));
            if (prefix > ip.Bits()) GuestFail("Invalid network prefix");
        }
        for (unsigned i = prefix; i < ip.Bits(); ++i)
            ip.Bytes[i/8] &= static_cast<unsigned char>(~(0x80u >> (i % 8)));
        if (bits) *bits = prefix;
        return ip.Text() + "/" + std::to_string(prefix);
    }

    std::string Mac(std::string const& text)
    {
        std::string out;
        std::size_t pos = 0;
        for (unsigned pair = 0; pair < 6; ++pair)
        {
            for (unsigned i = 0; i < 2; ++i)
            {
                if (pos >= text.size()) GuestFail("Invalid MAC address");
                auto c = text[pos++];
                if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) GuestFail("Invalid MAC address");
                out += c;
            }
            if (pair != 5 && pos < text.size() && (text[pos] == '-' || text[pos] == ':')) ++pos;
        }
        if (pos != text.size()) GuestFail("Invalid MAC address");
        return out;
    }

    std::string Scalar(Json const& v)
    {
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return v.dump();
        GuestFail("Unexpected numeric network property");
    }

    bool Boolean(Json const& value, char const* key, bool fallback = false)
    {
        return value.contains(key) ? value.at(key).get<bool>() : fallback;
    }

    std::string AddressKey(Json const& v)
    {
        return Scalar(v.at("Family")) + "|" + v.at("Address").get<std::string>() + "|" + Scalar(v.at("Prefix"));
    }

    std::string RouteKey(Json const& v)
    {
        return Scalar(v.at("Family")) + "|" + v.at("Destination").get<std::string>() + "|" +
            v.at("NextHop").get<std::string>() + "|" + Scalar(v.at("Metric"));
    }

    auto RouteOrder(Json const& v)
    {
        unsigned prefix = 0;
        (void)NetworkPrefix(v.at("Destination").get<std::string>(), &prefix);
        return std::make_tuple(!Ip::Parse(v.at("NextHop").get<std::string>()).Zero(),
            v.at("Family").get<int>(), -static_cast<int>(prefix), v.at("Metric").get<std::uint64_t>(),
            v.at("Destination").get<std::string>(), v.at("NextHop").get<std::string>());
    }

    std::vector<std::string> Words(std::string const& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> result;
        for (std::string word; stream >> word;) result.push_back(std::move(word));
        return result;
    }

    Json NormalizeResource(std::string const& resource, std::string const& action, Json const& s)
    {
        if (!s.is_object() || (action != "Add" && action != "Remove" && action != "Update"))
            GuestFail("Invalid network operation");
        if (resource == "IPAddress")
        {
            auto ip = Ip::Parse(s.at("Address").get<std::string>());
            if (ip.Zero() || ip.Loopback() || ip.Multicast()) GuestFail("Invalid interface address");
            Json out = {{"Address", ip.Text()}, {"Family", ip.Family},
                {"Prefix", Integer(s.at("OnLinkPrefixLength"), 0, ip.Bits())}};
            if (Boolean(s, "Mirrored")) out["Mirrored"] = true;
            return out;
        }
        if (resource == "Route")
        {
            auto destination = NetworkPrefix(s.at("DestinationPrefix").get<std::string>());
            auto ip = Ip::Parse(destination.substr(0, destination.find('/')));
            auto hopText = s.value("NextHop", std::string());
            auto hop = Ip::Parse(hopText.empty() ? (ip.Family == 4 ? "0.0.0.0" : "::") : hopText);
            if (hop.Family != ip.Family || hop.Multicast() || hop.Loopback()) GuestFail("Invalid next hop");
            return {{"Family", ip.Family}, {"Destination", destination}, {"NextHop", hop.Text()},
                {"Metric", Integer(s.contains("Metric") ? s.at("Metric") : Json(0), 0, UINT32_MAX)}};
        }
        if (resource == "Interface" && action != "Remove")
        {
            Json out = {{"Connected", s.at("Connected").get<bool>()}, {"Mtu", Integer(s.at("NlMtu"),576,65535)}};
            if (Boolean(s, "Mirrored")) out["Mirrored"] = true;
            for (auto key : {"IPv4Metric", "IPv6Metric"})
                if (s.contains(key)) out[key] = Integer(s.at(key), 0, UINT32_MAX);
            return out;
        }
        if (resource == "PortRange" && action == "Update")
        {
            auto start = Integer(s.at("Start"), 1024, 65535);
            auto end = Integer(s.at("End"), start, 65535);
            if (end - start + 1 < 255) GuestFail("Dynamic port range must contain at least 255 ports");
            return {{"Start", start}, {"End", end}};
        }
        if (resource == "DNS")
        {
            auto servers = s.value("ServerList", Json::array());
            if (servers.is_string())
            {
                auto text = servers.get<std::string>();
                for (auto& ch : text) if (ch == ',' || ch == ';') ch = ' ';
                servers = Words(text);
            }
            if (!servers.is_array() || servers.size() > 16 || (servers.empty() && action != "Remove"))
                GuestFail("Invalid DNS server list");
            Json out = {{"Servers", Json::array()}};
            for (auto const& value : servers) out["Servers"].push_back(Ip::Parse(value.get<std::string>()).Text());
            if (s.contains("Suffix"))
            {
                auto suffix = s.at("Suffix").get<std::string>();
                while (!suffix.empty() && suffix.back() == '.') suffix.pop_back();
                for (auto& c : suffix) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c-'A'+'a');
                if (suffix.size() > 253) GuestFail("Invalid DNS suffix");
                if (!suffix.empty())
                {
                    for (std::size_t start = 0;;)
                    {
                        auto end = suffix.find('.', start);
                        auto label = suffix.substr(start, end == std::string::npos ? end : end-start);
                        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
                            !std::all_of(label.begin(), label.end(), [](char c)
                            { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
                            GuestFail("Invalid DNS suffix");
                        if (end == std::string::npos) break;
                        start = end + 1;
                    }
                }
                out["Suffix"] = suffix;
            }
            return out;
        }
        if (resource == "Reconcile" && action == "Update")
        {
            auto interface = NormalizeResource("Interface","Update",s.at("Interface"));
            if (!Boolean(interface,"Mirrored")) GuestFail("Reconcile requires a dedicated mirrored interface");
            auto const& addresses = s.at("Addresses");
            auto const& routes = s.at("Routes");
            if (!addresses.is_array() || !routes.is_array() || addresses.size() > 256 || routes.size() > 4096)
                GuestFail("Invalid network snapshot size");
            Json a = Json::array(), r = Json::array();
            std::set<std::string> ak, rk;
            for (auto const& raw : addresses)
            {
                auto v = NormalizeResource("IPAddress","Update",raw);
                if (v.at("Family") == 6 && interface.at("Mtu") < 1280) GuestFail("IPv6 requires MTU 1280");
                if (!ak.insert(AddressKey(v)).second) GuestFail("Duplicate address");
                a.push_back(v);
            }
            for (auto const& raw : routes)
            {
                auto v = NormalizeResource("Route","Update",raw);
                if (v.at("Family") == 6 && interface.at("Mtu") < 1280) GuestFail("IPv6 requires MTU 1280");
                if (!rk.insert(RouteKey(v)).second) GuestFail("Duplicate route");
                r.push_back(v);
            }
            auto const& dnsRaw = s.at("DNS");
            auto dns = NormalizeResource("DNS",dnsRaw.value("ServerList",Json::array()).empty() ? "Remove" : "Update",dnsRaw);
            if (!Boolean(interface,"Connected") && (!a.empty() || !r.empty() || !dns.at("Servers").empty()))
                GuestFail("Disconnected adapter must have an empty network configuration");
            std::sort(r.begin(),r.end(),[](auto const& l,auto const& q){return RouteOrder(l)<RouteOrder(q);});
            return {{"Interface",interface},{"Addresses",a},{"Routes",r},{"DNS",dns},
                {"PortRange",NormalizeResource("PortRange","Update",s.at("PortRange"))},
                {"DiscoverPorts",Boolean(s,"DiscoverPorts")},{"DnsTunnel",Boolean(s,"DnsTunnel")}};
        }
        GuestFail("Unsupported network resource or operation");
    }

    using Arguments = std::vector<std::string>;
    using NetworkRunner = std::function<std::string(Arguments const&)>;

#ifndef _WIN32
    std::string RunNetworkCommand(Arguments const& args)
    {
        if (args.empty() || (args[0] != "ip" && args[0] != "sysctl" && args[0] != "resolvectl"))
            GuestFail("Unsupported network command");
        std::vector<char*> argv;
        for (auto const& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        int pipefd[2]{};
        if (pipe2(pipefd,O_CLOEXEC)) GuestFail("Cannot create command pipe");
        auto child = fork();
        if (child < 0) { close(pipefd[0]);close(pipefd[1]);GuestFail("Cannot start network command"); }
        if (child == 0)
        {
            close(pipefd[0]);
            if (dup2(pipefd[1],STDOUT_FILENO)<0 || dup2(pipefd[1],STDERR_FILENO)<0) _exit(127);
            close(pipefd[1]);
            execvp(argv[0],argv.data());
            _exit(127);
        }
        close(pipefd[1]);
        struct Child
        {
            int Pipe; pid_t Id; bool Reaped=false;
            ~Child()
            {
                close(Pipe);
                if (!Reaped)
                {
                    kill(Id,SIGKILL);
                    int status;
                    while (waitpid(Id,&status,0)<0 && errno==EINTR) {}
                }
            }
        } process{pipefd[0],child};
        auto end = Mono::now()+std::chrono::seconds(30);
        std::string output;
        std::array<char,8192> buffer{};
        while (!Stop && Mono::now()<end)
        {
            pollfd fd{process.Pipe,POLLIN,0};
            auto n = poll(&fd,1,100);
            if (n<0 && errno==EINTR) continue;
            if (n<0) GuestFail("Network command output poll failed");
            if (!n) continue;
            auto count = read(process.Pipe,buffer.data(),buffer.size());
            if (count<0 && errno==EINTR) continue;
            if (count<0) GuestFail("Cannot read network command output");
            if (!count) break;
            if (output.size()+static_cast<std::size_t>(count)>4*1024*1024) GuestFail("Network command output too large");
            output.append(buffer.data(),static_cast<std::size_t>(count));
        }
        while (!Stop && Mono::now()<end)
        {
            int status=0;
            auto n=waitpid(child,&status,WNOHANG);
            if (n==child)
            {
                process.Reaped=true;
                if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("Network command failed: "+output.substr(0,2000));
                return output;
            }
            if(n<0 && errno!=EINTR) GuestFail("Cannot wait for network command");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        GuestFail("Network command deadline expired");
    }

    std::string ReadText(fs::path const& path)
    {
        std::ifstream file(path);
        if(!file) GuestFail("Cannot read guest network state");
        return std::string(std::istreambuf_iterator<char>(file),{});
    }

    std::string FamilyArgument(int family) { return family==4?"-4":"-6"; }
    using NetworkEntries = std::map<std::string,Json>;

    std::vector<Json> Ordered(NetworkEntries const& entries, bool reverse=false)
    {
        std::vector<Json> result;
        for(auto const& e:entries) result.push_back(e.second);
        std::sort(result.begin(),result.end(),[&](auto const& a,auto const& b)
            {return reverse?RouteOrder(b)<RouteOrder(a):RouteOrder(a)<RouteOrder(b);});
        return result;
    }

    Json ListeningPorts(Json const& addresses,fs::path const& root="/proc/net",bool includeLoopback=false)
    {
        std::set<std::string> accepted;
        for(auto const& value:addresses) accepted.insert(Ip::Parse(value.at("Address").get<std::string>()).Mapped().Text());
        std::set<std::pair<std::string,unsigned>> ports;
        for(auto protocol:{"TCP","UDP"}) for(int family:{4,6})
        {
            std::string name=protocol[0]=='T'?"tcp":"udp";
            if(family==6) name+="6";
            std::ifstream file(root/name);
            std::string line;
            std::getline(file,line);
            while(std::getline(file,line))
            {
                auto fields=Words(line);
                if(fields.size()<4 || (protocol[0]=='T' && fields[3]!="0A")) continue;
                auto remote=fields[2].find(':');
                if(remote==std::string::npos || (protocol[0]=='U' && fields[2].substr(remote+1)!="0000"))continue;
                auto colon=fields[1].find(':');
                if(colon!=(family==4?8u:32u))continue;
                Ip ip;ip.Family=family;
                try
                {
                    auto raw=fields[1].substr(0,colon);
                    for(unsigned i=0;i<ip.Bits()/8;++i)
                        ip.Bytes[i]=static_cast<unsigned char>(std::stoul(raw.substr(2*i,2),nullptr,16));
                    const std::uint32_t native=1;
                    if(*reinterpret_cast<unsigned char const*>(&native)==1)
                        for(unsigned i=0;i<ip.Bits()/8;i+=4)std::reverse(ip.Bytes.begin()+i,ip.Bytes.begin()+i+4);
                    auto port=std::stoul(fields[1].substr(colon+1),nullptr,16);
                    ip=ip.Mapped();
                    if(port && port<=65535 && ((!ip.Loopback() && (ip.Zero() || accepted.count(ip.Text()))) ||
                        (includeLoopback && ip.Text()=="127.0.0.1")))
                        ports.emplace(protocol,static_cast<unsigned>(port));
                }catch(...){}
            }
        }
        Json result=Json::array();
        for(auto const& port:ports) result.push_back({{"Protocol",port.first},{"Port",port.second}});
        return result;
    }

#include "LoopbackRouting.h"

    class LinuxNetwork
    {
        NetworkRunner Run;
        fs::path Sys;
        static constexpr std::uint64_t TableBase=0x40000000ull;
        using Rule=std::tuple<std::uint64_t,std::string,std::string>;

        Json Read(Arguments const& args){return Json::parse(Run(args));}
        static std::string Zero(int family){return family==4?"0.0.0.0":"::";}
        static std::uint64_t Table(std::uint64_t index)
        {
            if(!index || index>0x7fffffff)GuestFail("Invalid policy routing interface index");
            return TableBase+index;
        }
        static bool Protocol(Json const& v)
        {return v.contains("protocol") && Scalar(v.at("protocol"))=="242";}
        static std::string TableId(Json const& v)
        {return v.contains("table")?Scalar(v.at("table")):"254";}
        Json Link(std::string const& dev)
        {
            auto rows=Read({"ip","-j","link","show","dev",dev});
            if(rows.size()!=1)GuestFail("Mirrored link not found");
            return rows.at(0);
        }
        static bool Up(Json const& link)
        {auto const& flags=link.at("flags");return std::find(flags.begin(),flags.end(),Json("UP"))!=flags.end();}
        NetworkEntries Addresses(std::string const& dev)
        {
            NetworkEntries result;
            for(auto const& row:Read({"ip","-j","address","show","dev",dev}))
                for(auto const& item:row.value("addr_info",Json::array()))
                {
                    auto ip=Ip::Parse(item.at("local").get<std::string>());
                    if(ip.Zero()||ip.Loopback()||ip.Multicast()||ip.LinkLocal())continue;
                    Json entry={{"Address",ip.Text()},{"Family",ip.Family},{"Prefix",item.at("prefixlen")},{"Mirrored",true}};
                    result.emplace(AddressKey(entry),entry);
                }
            return result;
        }
        NetworkEntries Routes(std::string const& dev,bool managed=false,std::uint64_t table=0,int onlyFamily=0)
        {
            NetworkEntries result;
            for(int family:{4,6})
            {
                if(onlyFamily && family!=onlyFamily)continue;
                Arguments args;
                if(table)args={"ip","-N",FamilyArgument(family),"-j","route","show","table","all"};
                else
                {
                    args={"ip",FamilyArgument(family),"-j","route","show","table","main","dev",dev};
                    if(managed)args.insert(args.end(),{"proto","242"});
                }
                for(auto const& row:Read(args))
                {
                    if(table && TableId(row)!=std::to_string(table))continue;
                    auto type=row.contains("type")?Scalar(row.at("type")):"unicast";
                    if(table && (!Protocol(row)||(type!="1"&&type!="unicast")))
                        GuestFail("Policy routing table contains an unmanaged route");
                    if(type!="1"&&type!="unicast")continue;
                    auto dest=row.value("dst",std::string("default"));
                    if(dest=="default")dest=Zero(family)+"/0";
                    auto hops=row.value("nexthops",Json::array());
                    if(hops.empty())hops=Json::array({row});
                    for(auto const& hop:hops)
                    {
                        auto owner=hop.value("dev",row.value("dev",dev));
                        if(owner!=dev){if(table)GuestFail("Policy route belongs to another adapter");continue;}
                        Json v={{"Family",family},{"Destination",NetworkPrefix(dest)},
                            {"NextHop",Ip::Parse(hop.value("gateway",Zero(family))).Text()},
                            {"Metric",row.value("metric",std::uint64_t(0))}};
                        result.emplace(RouteKey(v),v);
                    }
                }
            }
            return result;
        }
        void ChangeRoute(std::string const& dev,std::string const& verb,Json const& route,std::uint64_t table=0)
        {
            auto hop=route.at("NextHop").get<std::string>();
            Arguments args={"ip",FamilyArgument(route.at("Family").get<int>()),"route",
                verb=="add"?"append":verb,route.at("Destination").get<std::string>()};
            if(!Ip::Parse(hop).Zero())args.insert(args.end(),{"via",hop});
            args.insert(args.end(),{"dev",dev,"metric",Scalar(route.at("Metric")),"proto","242"});
            if(table)
            {
                args.insert(args.end(),{"table",std::to_string(table)});
                if(verb=="add"&&!Ip::Parse(hop).Zero())args.push_back("onlink");
            }
            Run(args);
        }
        void ChangeAddress(std::string const& dev,std::string const& verb,Json const& a)
        {
            auto family=a.at("Family").get<int>();
            Arguments args={"ip",FamilyArgument(family),"address",verb,
                a.at("Address").get<std::string>()+"/"+Scalar(a.at("Prefix")),"dev",dev};
            if(verb!="del")
            {
                args.push_back("noprefixroute");
                if(family==6&&Boolean(a,"Mirrored"))args.push_back("nodad");
            }
            Run(args);
        }
        Json Rules(int family){return Read({"ip","-N",FamilyArgument(family),"-j","rule","show"});}
        static Rule RuleKey(Json const& row)
        {
            auto source=row.value("src",std::string("all"));
            if(source!="all")source=NetworkPrefix(source);
            return {Integer(row.at("priority"),0,UINT32_MAX),source,row.value("oif",std::string())};
        }
        void ChangeRule(int family,std::string verb,std::uint64_t table,Rule const& rule)
        {
            auto const& [priority,source,dev]=rule;
            Arguments args={"ip",FamilyArgument(family),"rule",std::move(verb),"pref",std::to_string(priority)};
            if(source!="all")args.insert(args.end(),{"from",source});
            if(!dev.empty())args.insert(args.end(),{"oif",dev});
            args.insert(args.end(),{"lookup",std::to_string(table),"protocol","242"});
            Run(args);
        }
        void Policy(std::string const& dev,std::uint64_t table,Json const& interface,
            NetworkEntries const& addresses,NetworkEntries const& routes)
        {
            for(int family:{4,6})
            {
                NetworkEntries desired;
                std::set<Rule> wanted,actual;
                if(Boolean(interface,"Connected"))
                    for(auto const& route:routes) if(route.second.at("Family")==family)desired.insert(route);
                if(!desired.empty())
                {
                    wanted.emplace(20000,"all",dev);
                    auto metric=interface.value(family==4?"IPv4Metric":"IPv6Metric",std::uint64_t(0));
                    auto priority=21000+std::min<std::uint64_t>(metric,9999);
                    for(auto const& a:addresses)if(a.second.at("Family")==family)
                    {
                        auto ip=Ip::Parse(a.second.at("Address").get<std::string>());
                        wanted.emplace(priority,ip.Text()+"/"+std::to_string(ip.Bits()),"");
                    }
                }
                auto rows=Rules(family);
                for(auto const& row:rows)
                    if(TableId(row)==std::to_string(table)&&!Protocol(row))GuestFail("Policy routing table has an unmanaged rule");
                auto current=Routes(dev,false,table,family);
                for(auto const& row:rows)if(TableId(row)==std::to_string(table))
                {
                    auto rule=RuleKey(row);
                    if(!wanted.count(rule)||!actual.insert(rule).second)ChangeRule(family,"del",table,rule);
                }
                for(auto const& route:Ordered(current,true))if(!desired.count(RouteKey(route)))ChangeRoute(dev,"del",route,table);
                for(auto const& route:Ordered(desired))if(!current.count(RouteKey(route)))ChangeRoute(dev,"add",route,table);
                for(auto const& rule:wanted)if(!actual.count(rule))ChangeRule(family,"add",table,rule);
                actual.clear();
                for(auto const& row:Rules(family))if(TableId(row)==std::to_string(table))
                    if(!Protocol(row)||!actual.insert(RuleKey(row)).second)GuestFail("Guest policy rules did not converge");
                if(actual!=wanted)GuestFail("Guest policy rules did not converge");
                auto after=Routes(dev,false,table,family);
                if(after.size()!=desired.size())GuestFail("Guest policy routes did not converge");
                for(auto const& route:desired)if(!after.count(route.first))GuestFail("Guest policy routes did not converge");
            }
        }
        void ReapRules()
        {
            std::set<std::uint64_t> active;
            for(auto const& link:Read({"ip","-j","link","show"}))active.insert(Table(Integer(link.at("ifindex"),1,0x7fffffff)));
            for(int family:{4,6})for(auto const& row:Rules(family))
            {
                if(!Protocol(row))continue;
                std::uint64_t table=0;
                try{std::size_t used=0;auto value=TableId(row);table=std::stoull(value,&used);if(used!=value.size())continue;}catch(...){continue;}
                if(table>TableBase && table<=TableBase+0x7fffffff && !active.count(table))
                    ChangeRule(family,"del",table,RuleKey(row));
            }
        }
        std::vector<std::string> Resolver(std::string const& command,std::string const& dev)
        {
            auto text=Run({"resolvectl",command,dev});
            auto colon=text.find(':');
            if(colon==std::string::npos)GuestFail("Cannot read guest resolver settings");
            auto values=Words(text.substr(colon+1));
            if(command=="dns")for(auto& v:values)v=Ip::Parse(v.substr(0,v.find('%'))).Text();
            return values;
        }
        void SetResolver(std::string const& command,std::string const& dev,std::vector<std::string> const& values)
        {
            Arguments args={"resolvectl",command,dev};
            if(values.empty())args.push_back("");
            else args.insert(args.end(),values.begin(),values.end());
            Run(args);
        }
    public:
        explicit LinuxNetwork(NetworkRunner runner=RunNetworkCommand,fs::path sys="/sys/class/net"):
            Run(std::move(runner)),Sys(std::move(sys)){}
        std::string Adapter(std::string const& mac)
        {
            std::vector<std::string> found;
            for(auto const& p:fs::directory_iterator(Sys))
            {
                try
                {
                    if(!fs::exists(p.path()/"device"))continue;
                    auto words=Words(ReadText(p.path()/"address"));
                    if(words.size()==1 && Mac(words[0])==mac)found.push_back(p.path().filename().string());
                }catch(...){}
            }
            if(found.size()!=1||found[0].empty()||found[0].front()=='-'||
                found[0].find_first_of("/\r\n")!=std::string::npos)GuestFail("Expected exactly one matching hardware adapter");
            return found[0];
        }
        Json Diagnostics()
        {
            auto adapters=Read({"ip","-j","address","show"});
            Json addresses=Json::array(),gpu=Json::array();
            for(auto const& row:adapters)for(auto const& a:row.value("addr_info",Json::array()))addresses.push_back(a.at("local"));
            std::error_code ec;
            for(auto const& card:fs::directory_iterator("/sys/class/drm",ec))
            {
                auto name=card.path().filename().string();
                if(name.rfind("card",0)!=0 || name.size()<=4 ||
                    !std::all_of(name.begin()+4,name.end(),[](char c){return c>='0'&&c<='9';}))continue;
                try{gpu.push_back({{"Name",name},{"Vendor",Words(ReadText(card.path()/"device/vendor")).at(0)},
                    {"Device",Words(ReadText(card.path()/"device/device")).at(0)}});}catch(...){}
            }
            return {{"OperatingSystem","Linux"},{"Addresses",addresses},{"Adapters",adapters},{"Gpu",gpu},
                {"Dxg",fs::exists("/dev/dxg")},{"DynamicPortRange",Words(ReadText("/proc/sys/net/ipv4/ip_local_port_range"))}};
        }
        void Apply(std::string const& target,std::string const& resource,std::string const& action,Json const& s)
        {
            auto dev=Adapter(target);
            if(resource=="PortRange")Run({"sysctl","-w","net.ipv4.ip_local_port_range="+Scalar(s.at("Start"))+" "+Scalar(s.at("End"))});
            else if(resource=="Interface")
            {
                if(Boolean(s,"Mirrored"))for(auto name:{"accept_ra","autoconf","dad_transmits"})
                    Run({"sysctl","-w","net/ipv6/conf/"+dev+"/"+name+"=0"});
                Run({"ip","link","set","dev",dev,"mtu",Scalar(s.at("Mtu"))});
                Run({"ip","link","set","dev",dev,Boolean(s,"Connected")?"up":"down"});
            }
            else if(resource=="IPAddress")
            {
                if(action!="Remove")ChangeAddress(dev,"replace",s);
                else if(Addresses(dev).count(AddressKey(s)))ChangeAddress(dev,"del",s);
            }
            else if(resource=="Route")
            {
                if(action=="Remove")
                {if(Routes(dev,true).count(RouteKey(s)))ChangeRoute(dev,"del",s);}
                else
                {if(!Routes(dev).count(RouteKey(s)))ChangeRoute(dev,"add",s);}
            }
            else if(resource=="DNS")
            {
                if(action=="Remove")Run({"resolvectl","revert",dev});
                else SetResolver("dns",dev,s.at("Servers").get<std::vector<std::string>>());
            }
            else GuestFail("Unsupported network operation");
        }
        Json Reconcile(std::string const& target,Json const& s)
        {
            auto dev=Adapter(target);
            auto const& interface=s.at("Interface");
            NetworkEntries addresses,routes;
            for(auto const& a:s.at("Addresses"))addresses.emplace(AddressKey(a),a);
            for(auto r:s.at("Routes"))
            {
                auto family=r.at("Family").get<int>();
                auto metric=std::min<std::uint64_t>(UINT32_MAX,r.at("Metric").get<std::uint64_t>()+
                    interface.value(family==4?"IPv4Metric":"IPv6Metric",std::uint64_t(0)));
                if(family==6 && !metric)metric=1;
                r["Metric"]=metric;routes.emplace(RouteKey(r),r);
            }
            auto link=Link(dev);
            for(auto const& v:std::vector<std::tuple<int,std::string,std::string>>{
                {4,"accept_local","1"},{4,"rp_filter","0"},{4,"arp_filter","1"},{6,"accept_ra","0"},
                {6,"autoconf","0"},{6,"dad_transmits","0"},{6,"use_tempaddr","0"}})
            {
                auto key="net/ipv"+std::to_string(std::get<0>(v))+"/conf/"+dev+"/"+std::get<1>(v);
                if(Words(Run({"sysctl","-n",key}))!=std::vector<std::string>{std::get<2>(v)})
                    Run({"sysctl","-w",key+"="+std::get<2>(v)});
            }
            if(link.at("mtu")!=interface.at("Mtu"))Run({"ip","link","set","dev",dev,"mtu",Scalar(interface.at("Mtu"))});
            if(Boolean(interface,"Connected")&&!Up(link))Run({"ip","link","set","dev",dev,"up"});
            for(auto const& route:Ordered(Routes(dev,true),true))if(!routes.count(RouteKey(route)))ChangeRoute(dev,"del",route);
            auto actual=Addresses(dev);
            for(auto const& a:actual)if(!addresses.count(a.first))ChangeAddress(dev,"del",a.second);
            for(auto const& a:addresses)if(!actual.count(a.first))ChangeAddress(dev,"replace",a.second);
            auto current=Routes(dev);
            for(auto const& route:Ordered(routes))if(!current.count(RouteKey(route)))ChangeRoute(dev,"add",route);
            auto table=Table(Integer(link.at("ifindex"),1,0x7fffffff));
            Policy(dev,table,interface,addresses,routes);ReapRules();
            auto dns=s.at("DNS").at("Servers").get<std::vector<std::string>>();
            if(Resolver("dns",dev)!=dns)SetResolver("dns",dev,dns);
            std::vector<std::string> domains;
            bool suffix=s.at("DNS").contains("Suffix");
            if(suffix && !s.at("DNS").at("Suffix").get<std::string>().empty())domains.push_back(s.at("DNS").at("Suffix").get<std::string>());
            if(suffix && Resolver("domain",dev)!=domains)SetResolver("domain",dev,domains);
            auto const& ports=s.at("PortRange");
            std::vector<std::string> range={Scalar(ports.at("Start")),Scalar(ports.at("End"))};
            if(Words(Run({"sysctl","-n","net.ipv4.ip_local_port_range"}))!=range)
                Apply(target,"PortRange","Update",ports);
            if(!Boolean(interface,"Connected")&&Up(Link(dev)))Run({"ip","link","set","dev",dev,"down"});
            actual=Addresses(dev);current=Routes(dev);link=Link(dev);
            if(actual.size()!=addresses.size())GuestFail("Guest addresses did not converge");
            for(auto const& a:addresses)if(!actual.count(a.first))GuestFail("Guest addresses did not converge");
            for(auto const& r:routes)if(!current.count(r.first))GuestFail("Guest routes did not converge");
            for(auto const& r:Routes(dev,true))if(!routes.count(r.first))GuestFail("Stale managed route remains");
            if(Up(link)!=Boolean(interface,"Connected")||link.at("mtu")!=interface.at("Mtu"))GuestFail("Guest link state did not converge");
            if(Resolver("dns",dev)!=dns || (suffix&&Resolver("domain",dev)!=domains) ||
                Words(Run({"sysctl","-n","net.ipv4.ip_local_port_range"}))!=range)GuestFail("Guest resolver or port range did not converge");
            return {{"PolicyRoutingTable",table},{"ListeningPorts",Boolean(s,"DiscoverPorts")&&Boolean(interface,"Connected")?
                ListeningPorts(s.at("Addresses")):Json::array()}};
        }
    };
#else
#include "WindowsNetworkScripts.h"

    struct ProcessHandle
    {
        HANDLE Value=nullptr;
        ~ProcessHandle(){Close();}
        void Close(){if(Value&&Value!=INVALID_HANDLE_VALUE){CloseHandle(Value);Value=nullptr;}}
        ProcessHandle()=default;
        ProcessHandle(ProcessHandle const&)=delete;
        ProcessHandle& operator=(ProcessHandle const&)=delete;
    };

    std::string RunWindowsNetwork(std::string_view script,Json const& input=Json::object())
    {
        // The command is embedded, and input is passed only as JSON on stdin.
        std::vector<unsigned char> commandBytes;
        for(unsigned char c:script){commandBytes.push_back(c);commandBytes.push_back(0);}
        auto encoded=Encode(commandBytes);
        wchar_t system[MAX_PATH]{};
        auto length=GetSystemDirectoryW(system,MAX_PATH);
        if(!length||length>=MAX_PATH)GuestFail("Cannot find Windows system directory");
        auto app=std::wstring(system)+L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        auto command=L"\""+app+L"\" -NoProfile -NonInteractive -EncodedCommand "+std::wstring(encoded.begin(),encoded.end());
        if(command.size()>=32767)GuestFail("Windows network command is too large");
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        ProcessHandle inRead,inWrite,outRead,outWrite,errRead,errWrite;
        if(!CreatePipe(&inRead.Value,&inWrite.Value,&security,0)||
           !CreatePipe(&outRead.Value,&outWrite.Value,&security,0)||
           !CreatePipe(&errRead.Value,&errWrite.Value,&security,0))GuestFail("Cannot create Windows command pipes");
        if(!SetHandleInformation(inWrite.Value,HANDLE_FLAG_INHERIT,0)||
           !SetHandleInformation(outRead.Value,HANDLE_FLAG_INHERIT,0)||
           !SetHandleInformation(errRead.Value,HANDLE_FLAG_INHERIT,0))GuestFail("Cannot protect Windows command pipes");
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=inRead.Value;startup.hStdOutput=outWrite.Value;startup.hStdError=errWrite.Value;
        PROCESS_INFORMATION pi{};
        if(!CreateProcessW(app.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi))
            GuestFail("Cannot start Windows network management command");
        ProcessHandle child;child.Value=pi.hProcess;
        CloseHandle(pi.hThread);inRead.Close();outWrite.Close();errWrite.Close();
        auto body=input.dump();
        std::atomic<bool> writeOk{true};
        std::thread writer([&]()
        {
            std::size_t offset=0;
            while(offset<body.size())
            {
                DWORD n=0;
                if(!WriteFile(inWrite.Value,body.data()+offset,
                    static_cast<DWORD>(body.size()-offset),&n,nullptr)||!n){writeOk=false;break;}
                offset+=n;
            }
            inWrite.Close();
        });
        struct Finish
        {
            HANDLE Child;std::thread& Writer;
            ~Finish()
            {
                if(WaitForSingleObject(Child,0)!=WAIT_OBJECT_0)TerminateProcess(Child,1);
                if(Writer.joinable())Writer.join();
            }
        } finish{child.Value,writer};
        std::string output, errors;
        auto end=Mono::now()+std::chrono::seconds(65);
        auto drain=[&](HANDLE handle,std::string& result)
        {
            DWORD available=0;
            if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr))
            {
                if(GetLastError()==ERROR_BROKEN_PIPE)return false;
                GuestFail("Cannot read Windows network command");
            }
            if(!available)return false;
            char buffer[8192];DWORD n=0;
            if(!ReadFile(handle,buffer,std::min<DWORD>(available,static_cast<DWORD>(sizeof(buffer))),&n,nullptr))
                GuestFail("Cannot read Windows network command");
            if(result.size()+n>4*1024*1024)GuestFail("Windows network command output too large");
            result.append(buffer,n);
            return n!=0;
        };
        while(!Stop && Mono::now()<end)
        {
            bool more=drain(outRead.Value,output);
            more=drain(errRead.Value,errors)||more;
            if(more)continue;
            if(WaitForSingleObject(child.Value,20)==WAIT_OBJECT_0)
            {
                if(!drain(outRead.Value,output)&&!drain(errRead.Value,errors))break;
            }
        }
        if(WaitForSingleObject(child.Value,1000)!=WAIT_OBJECT_0)GuestFail("Windows network command deadline expired");
        if(writer.joinable())writer.join();
        DWORD status=0;
        if(!GetExitCodeProcess(child.Value,&status)||status||!writeOk)
            throw std::runtime_error("Windows network command failed: "+(errors.empty()?output:errors).substr(0,2000));
        return output;
    }

    class WindowsNetwork
    {
    public:
        Json Diagnostics(){return Json::parse(RunWindowsNetwork(WINDOWS_DIAGNOSTICS));}
        Json Reconcile(std::string const& target,Json const& settings)
        {return Json::parse(RunWindowsNetwork(WINDOWS_RECONCILE,{{"Target",target},{"Settings",settings}}));}
        void Apply(std::string const& target,std::string const& resource,std::string const& action,Json const& settings)
        {(void)RunWindowsNetwork(WINDOWS_SCRIPT,{{"Target",target},{"Resource",resource},{"Action",action},{"Settings",settings}});}
    };
#endif

    Json NetworkConnectivity(Json const& settings)
    {
        Fields(settings,{"Address","Port"});
        auto ip=Ip::Parse(settings.at("Address").get<std::string>());
        if(ip.Zero()||ip.Multicast())GuestFail("Invalid connectivity target");
        auto port=Integer(settings.at("Port"),1,65535);
        sockaddr_storage remote{};
        int size=0;
        int family=ip.Family==4?AF_INET:AF_INET6;
        if(family==AF_INET)
        {
            auto* a=reinterpret_cast<sockaddr_in*>(&remote);
            a->sin_family=AF_INET;a->sin_port=htons(static_cast<unsigned short>(port));
            std::memcpy(&a->sin_addr,ip.Bytes.data(),4);size=sizeof(*a);
        }
        else
        {
            auto* a=reinterpret_cast<sockaddr_in6*>(&remote);
            a->sin6_family=AF_INET6;a->sin6_port=htons(static_cast<unsigned short>(port));
            std::memcpy(&a->sin6_addr,ip.Bytes.data(),16);size=sizeof(*a);
        }
#ifdef _WIN32
        auto sock=socket(family,SOCK_STREAM,IPPROTO_TCP);
        if(sock==INVALID_SOCKET)GuestFail("Cannot open connectivity socket");
        struct CloseSocket{SOCKET S;~CloseSocket(){closesocket(S);}} cleanup{sock};
        u_long enabled=1;
        if(ioctlsocket(sock,FIONBIO,&enabled))GuestFail("Cannot configure connectivity socket");
        if(connect(sock,reinterpret_cast<sockaddr*>(&remote),size))
        {
            if(WSAGetLastError()!=WSAEWOULDBLOCK)GuestFail("Guest TCP connection failed");
            fd_set writable,errors;
            FD_ZERO(&writable);FD_SET(sock,&writable);FD_ZERO(&errors);FD_SET(sock,&errors);
            timeval timeout{5,0};
            if(select(0,nullptr,&writable,&errors,&timeout)<=0)GuestFail("Guest TCP connection timed out");
            int error=0,length=sizeof(error);
            if(getsockopt(sock,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)||error)
                GuestFail("Guest TCP connection failed");
        }
        int length=sizeof(remote);
#else
        auto sock=socket(family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,IPPROTO_TCP);
        if(sock<0)GuestFail("Cannot open connectivity socket");
        struct CloseSocket{int S;~CloseSocket(){close(S);}} cleanup{sock};
        if(connect(sock,reinterpret_cast<sockaddr*>(&remote),static_cast<socklen_t>(size)))
        {
            if(errno!=EINPROGRESS)GuestFail("Guest TCP connection failed");
            pollfd fd{sock,POLLOUT,0};
            if(poll(&fd,1,5000)<=0)GuestFail("Guest TCP connection timed out");
            int error=0;socklen_t length=sizeof(error);
            if(getsockopt(sock,SOL_SOCKET,SO_ERROR,&error,&length)||error)GuestFail("Guest TCP connection failed");
        }
        socklen_t length=sizeof(remote);
#endif
        if(getsockname(sock,reinterpret_cast<sockaddr*>(&remote),&length))GuestFail("Cannot query local connection address");
        Ip local;local.Family=ip.Family;
        if(family==AF_INET)std::memcpy(local.Bytes.data(),&reinterpret_cast<sockaddr_in*>(&remote)->sin_addr,4);
        else std::memcpy(local.Bytes.data(),&reinterpret_cast<sockaddr_in6*>(&remote)->sin6_addr,16);
        return {{"LocalAddress",local.Text()},{"RemoteAddress",ip.Text()},{"Port",port}};
    }

    class Network
    {
#ifdef _WIN32
        WindowsNetwork Backend;
#else
        LinuxNetwork Backend;
        std::unique_ptr<LoopbackRouting> Loopback;
#endif
        bool DnsEnabled=false;
    public:
        explicit Network(fs::path const& state = {})
        {
#ifndef _WIN32
            if(!state.empty())Loopback=std::make_unique<LoopbackRouting>(state);
#else
            if(!state.empty())GuestFail("GELNIC routing requires Linux");
#endif
        }
        void EnableDns(){DnsEnabled=true;}
        Json Dispatch(std::string const& resource,std::string const& action,Json const& request)
        {
            auto const& settings=request.at("Settings");
            if(resource=="Diagnostics"&&action=="Query")
            {Fields(settings,{});return Backend.Diagnostics();}
            if(resource=="Connectivity"&&action=="Query")return NetworkConnectivity(settings);
            if(resource=="LoopbackReconcile"&&action=="Update")
            {
#ifdef _WIN32
                GuestFail("GELNIC routing requires Linux");
#else
                if(!Loopback)GuestFail("GELNIC routing is not enabled");
                if(!settings.at("Enabled").get<bool>())
                {
                    Fields(settings,{"Enabled"});Loopback->Clear();return {{"Loopback",false}};
                }
                auto targetMac=Mac(settings.at("AdapterMac").get<std::string>());
                auto gelnic=Backend.Adapter(targetMac);
                auto const& rawInterfaces=settings.at("Interfaces");
                if(!rawInterfaces.is_array()||rawInterfaces.size()>128)GuestFail("Invalid loopback interface inventory");
                std::map<std::string,std::vector<std::string>> interfaces;
                std::set<std::string> uniqueAddresses;
                for(auto const& entry:rawInterfaces)
                {
                    auto dev=Backend.Adapter(Mac(entry.at("AdapterMac").get<std::string>()));
                    auto const& addresses=entry.at("Addresses");
                    if(!addresses.is_array()||addresses.size()>256||dev==gelnic||interfaces.count(dev))
                        GuestFail("Invalid loopback interface");
                    auto& values=interfaces[dev];
                    for(auto const& address:addresses)
                    {
                        auto ip=Ip::Parse(address.get<std::string>());
                        if(ip.Family!=4||ip.Zero()||ip.Loopback()||ip.Multicast()||(ip.Bytes[0]==169&&ip.Bytes[1]==254)||
                            !uniqueAddresses.insert(ip.Text()).second)GuestFail("Invalid mirrored loopback address");
                        values.push_back(ip.Text());
                    }
                }
                auto ports=settings.value("LocalPorts",Json::array());
                if(!ports.is_array()||ports.size()>256)GuestFail("Invalid local-only port list");
                std::set<unsigned> local;
                for(auto const& value:ports)local.insert(static_cast<unsigned>(Integer(value,1,65535)));
                auto result=Loopback->Reconcile(gelnic,interfaces,local);
                if(Boolean(settings,"DiscoverPorts"))
                {
                    Json listening=Json::array();
                    for(auto const& entry:ListeningPorts(Json::array(),"/proc/net",true))
                        if(!local.count(entry.at("Port").get<unsigned>()))listening.push_back(entry);
                    result["ListeningPorts"]=listening;
                }
                return result;
#endif
            }
            auto target=Mac(request.at("AdapterMac").get<std::string>());
            auto normalized=NormalizeResource(resource,action,settings);
            if(resource=="Reconcile")
            {
                bool tunnel=Boolean(normalized,"DnsTunnel");
                if(tunnel)
                {
                    if(!DnsEnabled)GuestFail("DNS tunneling is not enabled in this network service");
#ifdef _WIN32
                    normalized["DNS"]["Servers"]=Boolean(normalized.at("Interface"),"Connected")?
                        Json::array({"127.0.0.1"}):Json::array();
#else
                    normalized["DNS"]["Servers"]=Json::array();
#endif
                }
                auto reply=Backend.Reconcile(target,normalized);
                if(tunnel)reply["DnsTunnel"]=true;
                return reply;
            }
            Backend.Apply(target,resource,action,normalized);
            return Json::object();
        }
    };

#ifndef _WIN32
    void NativeNetworkIntegrationTest()
    {
        auto root=fs::temp_directory_path()/("NanaBox-Network-"+Token());
        fs::create_directories(root/"nb-test0/device");
        struct Cleanup
        {
            fs::path Path;
            ~Cleanup()
            {
                try{RunNetworkCommand({"ip","link","del","nb-test0"});}catch(...){}
                std::error_code ec;fs::remove_all(Path,ec);
            }
        } cleanup{root};
        {std::ofstream file(root/"nb-test0/address");file<<"00:15:5d:12:34:56\n";}
        RunNetworkCommand({"ip","link","add","nb-test0","type","veth","peer","name","nb-test1"});
        RunNetworkCommand({"ip","link","set","nb-test1","up"});
        RunNetworkCommand({"ip","link","set","nb-test0","address","00:15:5d:12:34:56"});
        std::vector<std::string> dns,domains;
        NetworkRunner runner=[&](Arguments const& args)
        {
            if(args[0]!="resolvectl")return RunNetworkCommand(args);
            auto& values=args[1]=="dns"?dns:domains;
            if(args.size()==3)
            {
                std::string text="Link 1 (nb-test0):";
                for(auto const& value:values)text+=" "+value;
                return text;
            }
            values.clear();
            for(std::size_t i=3;i<args.size();++i)if(!args[i].empty())values.push_back(args[i]);
            return std::string();
        };
        LinuxNetwork backend(runner,root);
        Json raw={{"Interface",{{"Connected",true},{"NlMtu",1500},{"Mirrored",true}}},
            {"Addresses",Json::array({{{"Address","192.0.2.10"},{"OnLinkPrefixLength",24},{"Mirrored",true}},
                {{"Address","2001:db8::10"},{"OnLinkPrefixLength",64},{"Mirrored",true}}})},
            {"Routes",Json::array({{{"DestinationPrefix","0.0.0.0/0"},{"NextHop","192.0.2.1"},{"Metric",10}},
                {{"DestinationPrefix","192.0.2.0/24"},{"Metric",10}},
                {{"DestinationPrefix","::/0"},{"NextHop","2001:db8::1"}},
                {{"DestinationPrefix","2001:db8::/64"}}})},
            {"DNS",{{"ServerList",Json::array({"192.0.2.53"})},{"Suffix","Example.Test."}}},
            {"PortRange",{{"Start",49152},{"End",53247}}}};
        auto desired=NormalizeResource("Reconcile","Update",raw);
        auto result=backend.Reconcile("00155d123456",desired);
        auto table=Scalar(result.at("PolicyRoutingTable"));
        for(auto family:{"-4","-6"})
        {
            auto routes=Json::parse(RunNetworkCommand({"ip",family,"-j","route","show","table",table}));
            if(routes.size()!=2)GuestFail("Native policy table routes missing");
        }
        RunNetworkCommand({"ip","-4","address","add","198.18.0.10/24","dev","nb-test1","noprefixroute"});
        RunNetworkCommand({"ip","-4","route","add","198.18.0.0/24","dev","nb-test1","proto","242"});
        RunNetworkCommand({"ip","-4","route","append","default","via","198.18.0.1","dev","nb-test1","metric","5","proto","242"});
        auto checkRoute=[&](Arguments args,std::string const& expected)
        {
            auto rows=Json::parse(RunNetworkCommand(args));
            if(rows.empty()||rows.at(0).at("dev")!=expected)GuestFail("Native source or interface routing failed");
        };
        checkRoute({"ip","-4","-j","route","get","203.0.113.8","from","192.0.2.10"},"nb-test0");
        checkRoute({"ip","-4","-j","route","get","203.0.113.8"},"nb-test1");
        checkRoute({"ip","-6","-j","route","get","2001:db8:ffff::1","from","2001:db8::10"},"nb-test0");
        RunNetworkCommand({"ip","-4","rule","add","pref","19000","from","203.0.113.1/32","lookup",table,"protocol","99"});
        bool refused=false;
        try{backend.Reconcile("00155d123456",desired);}catch(...){refused=true;}
        RunNetworkCommand({"ip","-4","rule","del","pref","19000","from","203.0.113.1/32","lookup",table,"protocol","99"});
        if(!refused)GuestFail("Foreign routing table was accepted");
        RunNetworkCommand({"ip","-4","route","del","default","via","192.0.2.1","dev","nb-test0","metric","10","proto","242"});
        RunNetworkCommand({"ip","-4","address","del","192.0.2.10/24","dev","nb-test0"});
        LinuxNetwork restarted(runner,root);
        restarted.Reconcile("00155d123456",desired);
        restarted.Reconcile("00155d123456",desired);
        if(dns!=std::vector<std::string>{"192.0.2.53"}||domains!=std::vector<std::string>{"example.test"})
            GuestFail("Native DNS reconciliation failed");
        raw["Interface"]["Connected"]=false;
        raw["Addresses"]=Json::array();raw["Routes"]=Json::array();
        raw["DNS"]={{"ServerList",Json::array()},{"Suffix",""}};
        restarted.Reconcile("00155d123456",NormalizeResource("Reconcile","Update",raw));
        if(!dns.empty()||!domains.empty())GuestFail("Native DNS cleanup failed");
        auto other=RunNetworkCommand({"ip","-4","route","show","dev","nb-test1","proto","242"});
        if(other.find("198.18.0.0/24")==std::string::npos||other.find("default via 198.18.0.1")==std::string::npos)
            GuestFail("Unrelated adapter routes were removed");
        std::cout<<"Native Linux mirrored network integration passed\n";
    }
#endif

    void NetworkValidationTest()
    {
#ifdef _WIN32
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2,2),&data))GuestFail("WSAStartup failed");
        struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;
#endif
        if(Mac("00-15-5D-12-34-56")!="00155d123456")GuestFail("MAC normalization failed");
        Json settings={{"Interface",{{"Connected",true},{"NlMtu",1500},{"Mirrored",true}}},
            {"Addresses",Json::array({{{"Address","192.0.2.10"},{"OnLinkPrefixLength",24}}})},
            {"Routes",Json::array({{{"DestinationPrefix","0.0.0.0/0"},{"NextHop","192.0.2.1"},{"Metric",10}},
                {{"DestinationPrefix","192.0.2.8/24"},{"Metric",10}}})},
            {"DNS",{{"ServerList",Json::array({"192.0.2.53"})},{"Suffix","Corp.Example."}}},
            {"PortRange",{{"Start",49152},{"End",53247}}}};
        auto normalized=NormalizeResource("Reconcile","Update",settings);
        if(normalized.at("Routes").at(0).at("Destination")!="192.0.2.0/24" ||
            normalized.at("DNS").at("Suffix")!="corp.example")GuestFail("Route or DNS normalization failed");
        settings["Routes"].push_back(settings["Routes"].at(0));
        bool refused=false;
        try{(void)NormalizeResource("Reconcile","Update",settings);}catch(...){refused=true;}
        if(!refused)GuestFail("Duplicate route accepted");
        refused=false;
        try{(void)NormalizeResource("IPAddress","Add",{{"Address","127.0.0.1"},{"OnLinkPrefixLength",8}});}catch(...){refused=true;}
        if(!refused)GuestFail("Invalid interface address accepted");
    }
