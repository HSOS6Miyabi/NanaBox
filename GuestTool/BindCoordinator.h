// Linux synchronous bind reservations. Included by GuestTool.cpp.
using BindRequest = std::array<unsigned char,20>;
using BoundPort = std::pair<unsigned,unsigned>;

std::set<BoundPort> BoundPorts()
{
    Socket fd(socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_SOCK_DIAG));
    if(fd.Value<0)Fail("Cannot open socket diagnostics");
    Nonblocking(fd.Value);
    sockaddr_nl local{}; local.nl_family=AF_NETLINK;
    if(bind(fd.Value,reinterpret_cast<sockaddr*>(&local),sizeof(local)))Fail("Cannot bind socket diagnostics");
    sockaddr_nl kernel{};kernel.nl_family=AF_NETLINK;
    std::set<BoundPort> ports;
    std::uint32_t sequence=0;
    for(unsigned family:{AF_INET,AF_INET6})
    for(unsigned protocol:{IPPROTO_TCP,IPPROTO_UDP})
    {
        struct {nlmsghdr Header;inet_diag_req_v2 Body;} req{};
        req.Header.nlmsg_len=sizeof(req);req.Header.nlmsg_type=SOCK_DIAG_BY_FAMILY;
        req.Header.nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP;req.Header.nlmsg_seq=++sequence;
        req.Body.sdiag_family=static_cast<__u8>(family);
        req.Body.sdiag_protocol=static_cast<__u8>(protocol);req.Body.idiag_states=UINT32_MAX;
        if(sendto(fd.Value,&req,sizeof(req),0,reinterpret_cast<sockaddr*>(&kernel),sizeof(kernel))!=sizeof(req))
            Fail("Cannot request socket diagnostics");
        auto end=Mono::now()+std::chrono::seconds(3);
        bool done=false;
        while(!done)
        {
            if(!Wait(fd.Value,false,end))Fail("Socket diagnostics timed out");
            alignas(nlmsghdr) std::array<char,256*1024> bytes{};
            sockaddr_nl sender{};iovec io{bytes.data(),bytes.size()};
            msghdr msg{};msg.msg_name=&sender;msg.msg_namelen=sizeof(sender);msg.msg_iov=&io;msg.msg_iovlen=1;
            auto n=recvmsg(fd.Value,&msg,0);
            if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
            if(n<=0||(msg.msg_flags&MSG_TRUNC)||sender.nl_pid!=0)Fail("Invalid socket diagnostic response");
            int remain=static_cast<int>(n);
            for(auto* header=reinterpret_cast<nlmsghdr*>(bytes.data());NLMSG_OK(header,remain);header=NLMSG_NEXT(header,remain))
            {
                if(header->nlmsg_seq!=sequence)continue;
                if(header->nlmsg_flags&NLM_F_DUMP_INTR)Fail("Socket diagnostic dump interrupted");
                if(header->nlmsg_type==NLMSG_DONE)
                {
                    if(NLMSG_PAYLOAD(header,0)>=sizeof(int))
                    {
                        int error=0;std::memcpy(&error,NLMSG_DATA(header),sizeof(error));
                        if(error)Fail("Socket diagnostic dump failed");
                    }
                    done=true;break;
                }
                if(header->nlmsg_type==NLMSG_ERROR)Fail("Socket diagnostic request failed");
                if(header->nlmsg_type!=SOCK_DIAG_BY_FAMILY||NLMSG_PAYLOAD(header,0)<sizeof(inet_diag_msg))
                    Fail("Invalid socket diagnostic record");
                inet_diag_msg entry{};std::memcpy(&entry,NLMSG_DATA(header),sizeof(entry));
                if(entry.id.idiag_sport)ports.emplace(protocol,ntohs(entry.id.idiag_sport));
            }
        }
    }
    return ports;
}

void CheckBindSupport()
{
    Socket fd(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(fd.Value<0||bind(fd.Value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))Fail("Cannot test TCP bind");
    socklen_t len=sizeof(addr);
    if(getsockname(fd.Value,reinterpret_cast<sockaddr*>(&addr),&len))Fail("Cannot query bound TCP port");
    if(!BoundPorts().count({IPPROTO_TCP,ntohs(addr.sin_port)}))
        Fail("Kernel does not report bound inactive TCP sockets (Linux 6.7+ required)");
}

unsigned BindPort(BindRequest const& r){return (unsigned(r[2])<<8)|r[3];}
bool ValidBind(BindRequest const& r)
{
    return (r[0]==IPPROTO_TCP||r[0]==IPPROTO_UDP)&&(r[1]==4||r[1]==6)&&BindPort(r)&&
        (r[1]==6||std::all_of(r.begin()+8,r.end(),[](auto x){return x==0;}));
}
std::uint64_t NamespaceCookie(int fd)
{
    std::uint64_t cookie=0;socklen_t size=sizeof(cookie);
    if(getsockopt(fd,SOL_SOCKET,71,&cookie,&size)||size!=sizeof(cookie))Fail("Cannot identify socket network namespace");
    return cookie;
}
bool SocketMatches(int fd,BindRequest const& r,std::uint64_t cookie)
{
    int domain=0,protocol=0;socklen_t n=sizeof(int);
    if(getsockopt(fd,SOL_SOCKET,SO_DOMAIN,&domain,&n))return false;
    n=sizeof(int);
    if(getsockopt(fd,SOL_SOCKET,SO_PROTOCOL,&protocol,&n))return false;
    if(protocol!=r[0]||(r[1]==6?domain!=AF_INET6:domain!=AF_INET&&domain!=AF_INET6))return false;
    try{return NamespaceCookie(fd)==cookie;}catch(...){return false;}
}

class BindCoordinator
{
    std::atomic<bool> Closing{false};
    fs::path Path;
    std::unique_ptr<Socket> Local,Host,Channel;
    std::mutex Mutex;
    std::condition_variable Cv;
    std::thread LocalThread,HostThread,RenewThread;
    std::map<BindRequest,Mono::time_point> Pending;
    std::uint64_t Cookie=0;
    dev_t Device=0;ino_t Inode=0;
    std::function<std::set<BoundPort>()> Inspect;
    std::function<Mono::time_point()> Clock;

    unsigned Exchange(BindRequest r)
    {
        if(!Channel)return 2;
        try
        {
            auto end=Mono::now()+std::chrono::seconds(3);
            Transfer(Channel->Value,reinterpret_cast<char*>(r.data()),r.size(),true,end,Closing);
            unsigned char status=2;
            Transfer(Channel->Value,reinterpret_cast<char*>(&status),1,false,end,Closing);
            if(status>2)Fail("Invalid reservation response");
            return status;
        }
        catch(...){Channel.reset();return 2;}
    }
    void LocalClient(int fd)
    {
        unsigned char status=2;
        try
        {
            Nonblocking(fd);
            auto end=Mono::now()+std::chrono::seconds(3);
            if(!Wait(fd,false,end,Closing))Fail("Bind request timed out");
            BindRequest request{};
            alignas(cmsghdr) std::array<char,CMSG_SPACE(4*sizeof(int))> ancillary{};
            iovec io{request.data(),request.size()};msghdr msg{};
            msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=ancillary.data();msg.msg_controllen=ancillary.size();
            auto n=recvmsg(fd,&msg,MSG_CMSG_CLOEXEC);
            struct Descriptors{std::array<int,4> Fds{};unsigned Count=0;~Descriptors(){for(unsigned i=0;i<Count;++i)close(Fds[i]);}} received;
            bool valid=true;
            for(auto* c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))
            {
                if(c->cmsg_len<CMSG_LEN(0)||c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS){valid=false;continue;}
                auto bytes=c->cmsg_len-CMSG_LEN(0);
                if(bytes%sizeof(int))valid=false;
                for(std::size_t i=0;i+sizeof(int)<=bytes;i+=sizeof(int))
                {
                    int value=-1;std::memcpy(&value,CMSG_DATA(c)+i,sizeof(value));
                    if(received.Count<received.Fds.size())received.Fds[received.Count++]=value;
                    else{close(value);valid=false;}
                }
            }
            if(n<=0||n>static_cast<ssize_t>(request.size())||!valid||(msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC))||received.Count!=1)
                Fail("Bind request requires exactly one application socket");
            auto offset=static_cast<std::size_t>(n);
            if(offset<request.size())Transfer(fd,reinterpret_cast<char*>(request.data()+offset),request.size()-offset,false,end,Closing);
            if(!ValidBind(request)||!SocketMatches(received.Fds[0],request,Cookie))Fail("Invalid application socket");
            status=static_cast<unsigned char>(Reserve(request));
        }catch(...){}
        try{Transfer(fd,reinterpret_cast<char*>(&status),1,true,Mono::now()+std::chrono::seconds(1),Closing);}catch(...){}
    }
    void AcceptLocal()
    {
        while(!Stop&&!Closing)
        {
            if(!Wait(Local->Value,false,Mono::now()+std::chrono::milliseconds(500),Closing))continue;
            Socket client(accept4(Local->Value,nullptr,nullptr,SOCK_CLOEXEC));
            if(client.Value>=0)LocalClient(client.Value);
        }
    }
    void AcceptHost()
    {
        while(!Stop&&!Closing)
        {
            if(!Wait(Host->Value,false,Mono::now()+std::chrono::milliseconds(500),Closing))continue;
            auto value=AcceptParent(Host->Value);
            if(value>=0)Attach(value);
        }
    }
    void Loop()
    {
        std::mutex waitMutex;std::unique_lock<std::mutex> lock(waitMutex);
        while(!Stop&&!Closing)
        {
            Cv.wait_for(lock,std::chrono::seconds(5),[this]{return Closing.load()||Stop.load();});
            if(Stop||Closing)break;
            try{Renew();}
            catch(std::exception const& e){std::cerr<<"Bind reservations: "<<e.what()<<'\n';std::lock_guard<std::mutex> guard(Mutex);Exchange({});}
        }
    }
public:
    explicit BindCoordinator(fs::path path,
        std::function<std::set<BoundPort>()> inspect=BoundPorts,
        std::function<Mono::time_point()> clock=[] {return Mono::now();})
        :Path(std::move(path)),Inspect(std::move(inspect)),Clock(std::move(clock)){}
    ~BindCoordinator(){Close();}
    void Attach(int fd)
    {
        auto socket=std::make_unique<Socket>(fd);Nonblocking(fd);
        std::lock_guard<std::mutex> lock(Mutex);
        if(!Closing)Channel=std::move(socket);
    }
    unsigned Reserve(BindRequest const& r)
    {
        if(!ValidBind(r))return 2;
        std::lock_guard<std::mutex> lock(Mutex);
        if(Closing||(!Pending.count(r)&&Pending.size()>=4096))return 2;
        auto status=Exchange(r);
        if(!status)Pending[r]=Clock()+std::chrono::seconds(15);
        return status;
    }
    void Renew()
    {
        auto live=Inspect();auto now=Clock();
        std::lock_guard<std::mutex> lock(Mutex);
        for(auto it=Pending.begin();it!=Pending.end()&&!Closing&&!Stop;)
        {
            auto const& r=it->first;
            if(it->second<=now&&!live.count({r[0],BindPort(r)}))it=Pending.erase(it);
            else{Exchange(r);++it;}
        }
        Exchange({});
    }
    void Start(bool listenHost=true,gid_t group=static_cast<gid_t>(-1))
    {
        sockaddr_un addr{};addr.sun_family=AF_UNIX;
        auto name=Path.string();
        if(name.size()>=sizeof(addr.sun_path))Fail("Bind socket path too long");
        struct stat parent{};
        if(lstat(Path.parent_path().c_str(),&parent)||!S_ISDIR(parent.st_mode)||parent.st_uid!=geteuid()||(parent.st_mode&0022))
            Fail("Unsafe bind socket directory");
        std::memcpy(addr.sun_path,name.c_str(),name.size()+1);
        struct stat old{};
        if(!lstat(Path.c_str(),&old))
        {
            if(!S_ISSOCK(old.st_mode)||old.st_uid!=geteuid())Fail("Unsafe existing bind socket");
            Socket probe(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
            if(probe.Value<0||!connect(probe.Value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||errno!=ECONNREFUSED)
                Fail("Bind coordinator is already running");
            struct stat now{};
            if(lstat(Path.c_str(),&now)||now.st_dev!=old.st_dev||now.st_ino!=old.st_ino||unlink(Path.c_str()))
                Fail("Bind socket changed during recovery");
        }
        else if(errno!=ENOENT)Fail("Cannot inspect bind socket");
        Socket probe(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0));
        if(probe.Value<0)Fail("Cannot open namespace probe");
        Cookie=NamespaceCookie(probe.Value);
        try
        {
            Local=std::make_unique<Socket>(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
            if(Local->Value<0||bind(Local->Value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))Fail("Cannot create bind socket");
            struct stat identity{};
            if(lstat(Path.c_str(),&identity))Fail("Cannot identify bind socket");
            Device=identity.st_dev;Inode=identity.st_ino;
            if((group!=static_cast<gid_t>(-1)&&chown(Path.c_str(),static_cast<uid_t>(-1),group))||chmod(Path.c_str(),0660)||
                listen(Local->Value,64))Fail("Cannot configure bind socket");
            Nonblocking(Local->Value);
            if(listenHost)Host=OpenParentListener(27455,1);
            LocalThread=std::thread([this]{try{AcceptLocal();}catch(std::exception const& e){std::cerr<<e.what()<<'\n';}});
            if(Host)HostThread=std::thread([this]{try{AcceptHost();}catch(std::exception const& e){std::cerr<<e.what()<<'\n';}});
            RenewThread=std::thread([this]{Loop();});
        }catch(...){Close();throw;}
    }
    void Close()
    {
        Closing=true;Cv.notify_all();
        if(LocalThread.joinable())LocalThread.join();
        if(HostThread.joinable())HostThread.join();
        if(RenewThread.joinable())RenewThread.join();
        {std::lock_guard<std::mutex> lock(Mutex);Channel.reset();}
        Local.reset();Host.reset();
        struct stat now{};
        if(Inode&&!lstat(Path.c_str(),&now)&&S_ISSOCK(now.st_mode)&&now.st_dev==Device&&now.st_ino==Inode)unlink(Path.c_str());
        Inode=0;
    }
};

void BindSelfTest()
{
    CheckBindSupport();
    auto root=fs::temp_directory_path()/("NanaBox-Bind-"+Token());
    if(!fs::create_directory(root)||chmod(root.c_str(),0700))Fail("Cannot create bind test directory");
    struct Cleanup{fs::path Path;~Cleanup(){std::error_code ec;fs::remove_all(Path,ec);}} cleanup{root};
    auto now=Mono::now();std::set<BoundPort> live;
    BindCoordinator broker(root/"bind.sock",[&]{return live;},[&]{return now;});
    int pair[2]{};
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair))Fail("Cannot create mock host");
    Socket host(pair[0]);broker.Attach(pair[1]);
    Nonblocking(host.Value);
    std::vector<BindRequest> frames;
    std::mutex frameMutex;
    std::thread worker([&]{
        try{
            for(;;){
                BindRequest r{};auto end=Mono::now()+std::chrono::seconds(5);
                Transfer(host.Value,reinterpret_cast<char*>(r.data()),r.size(),false,end);
                {std::lock_guard<std::mutex> lock(frameMutex);frames.push_back(r);}
                unsigned char status=BindPort(r)==32000?1:0;
                Transfer(host.Value,reinterpret_cast<char*>(&status),1,true,end);
            }
        }catch(...){}
    });
    try
    {
        broker.Start(false);
        auto request=[](unsigned port){BindRequest r{};r[0]=IPPROTO_TCP;r[1]=4;r[2]=port>>8;r[3]=port&255;r[4]=127;r[7]=1;return r;};
        auto local=[&](BindRequest r,int application){
            Socket client(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));sockaddr_un addr{};addr.sun_family=AF_UNIX;
            auto path=(root/"bind.sock").string();std::memcpy(addr.sun_path,path.c_str(),path.size()+1);
            if(client.Value<0||connect(client.Value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))Fail("Cannot connect local bind test");
            alignas(cmsghdr) std::array<char,CMSG_SPACE(sizeof(int))> ancillary{};
            iovec io{r.data(),r.size()};msghdr msg{};msg.msg_iov=&io;msg.msg_iovlen=1;
            if(application>=0){msg.msg_control=ancillary.data();msg.msg_controllen=ancillary.size();auto* c=CMSG_FIRSTHDR(&msg);
                c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
                std::memcpy(CMSG_DATA(c),&application,sizeof(application));}
            if(sendmsg(client.Value,&msg,MSG_NOSIGNAL)!=static_cast<ssize_t>(r.size()))Fail("Cannot send local bind test");
            unsigned char result=2;Nonblocking(client.Value);
            Transfer(client.Value,reinterpret_cast<char*>(&result),1,false,Mono::now()+std::chrono::seconds(4));
            return result;
        };
        Socket tcp(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)),udp(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0));
        if(local(request(32000),tcp.Value)!=1||local(request(32001),tcp.Value)!=0||
            local(request(32002),udp.Value)!=2||local(request(32002),-1)!=2)Fail("Bind reservation response or descriptor validation failed");
        auto a=request(32001),b=request(32003);
        if(broker.Reserve(b)!=0)Fail("Mock reservation failed");
        now+=std::chrono::seconds(20);live.insert({IPPROTO_TCP,32001});broker.Renew();
        {std::lock_guard<std::mutex> lock(frameMutex);
            if(frames!=std::vector<BindRequest>{request(32000),a,b,a,{}})Fail("Bind lease renewal mismatch");}
        live.clear();broker.Renew();
        {std::lock_guard<std::mutex> lock(frameMutex);if(frames.size()!=6||frames.back()!=BindRequest{})Fail("Expired reservation renewed");}
        broker.Close();
    }catch(...){broker.Close();shutdown(host.Value,SHUT_RDWR);worker.join();throw;}
    shutdown(host.Value,SHUT_RDWR);worker.join();
    if(fs::exists(root/"bind.sock"))Fail("Bind socket was not removed");
    std::cout<<"Native bind coordination tests passed\n";
}
