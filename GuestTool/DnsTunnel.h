#pragma once

    using DnsBytes=std::vector<unsigned char>;

    std::size_t DnsQuestionEnd(DnsBytes const& packet)
    {
        if(packet.size()<17||packet.size()>65535||(packet[2]&0xf8)||packet[4]!=0||packet[5]!=1||
            packet[6]||packet[7]||packet[8]||packet[9])Fail("Invalid DNS query");
        std::size_t offset=12;
        while(offset<packet.size()&&offset<267)
        {
            auto length=packet[offset++];
            if(!length)
            {
                if(offset+4<=packet.size())return offset+4;
                break;
            }
            if(length>63||offset+length>=packet.size())break;
            offset+=length;
        }
        Fail("Invalid DNS question");
    }

    DnsBytes DnsFailure(DnsBytes const& packet)
    {
        auto end=DnsQuestionEnd(packet);
        DnsBytes result={packet[0],packet[1],static_cast<unsigned char>(0x80|(packet[2]&1)),0x82,0,1,0,0,0,0,0,0};
        result.insert(result.end(),packet.begin()+12,packet.begin()+static_cast<std::ptrdiff_t>(end));
        return result;
    }

    class DnsTunnel
    {
        struct Job
        {
            DnsBytes Packet,Response;
            bool Tcp=false,Done=false;
            Mono::time_point Deadline;
            std::mutex Mutex;
            std::condition_variable Wake;
        };
        struct TaskPool
        {
            std::atomic<bool>& Closing;
            std::mutex Mutex;
            std::condition_variable Wake;
            std::deque<std::function<void()>> Tasks;
            std::vector<std::thread> Workers;
            explicit TaskPool(std::atomic<bool>& closing):Closing(closing){}
            void Start(unsigned count)
            {
                for(unsigned i=0;i<count;++i)Workers.emplace_back([this]()
                {
                    while(!Closing&&!Stop)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock lock(Mutex);
                            Wake.wait_for(lock,std::chrono::milliseconds(100),[&](){return Closing||!Tasks.empty();});
                            if(Closing||Stop)break;
                            if(Tasks.empty())continue;
                            task=std::move(Tasks.front());Tasks.pop_front();
                        }
                        try{task();}catch(...){}
                    }
                });
            }
            bool Submit(std::function<void()> task)
            {
                std::lock_guard lock(Mutex);
                if(Closing||Stop||Tasks.size()>=32)return false;
                Tasks.push_back(std::move(task));Wake.notify_one();return true;
            }
            void Join()
            {
                Wake.notify_all();
                for(auto& worker:Workers)if(worker.joinable())worker.join();
                std::lock_guard lock(Mutex);Tasks.clear();
            }
        };
        std::atomic<bool> Closing{false};
        std::mutex Mutex;
        std::condition_variable Wake;
        std::deque<std::shared_ptr<Job>> Queue;
        unsigned Active=0;
        std::unique_ptr<Socket> Tcp,Udp,Host;
        TaskPool TcpPool{Closing},UdpPool{Closing};
        std::vector<std::thread> Threads;
        unsigned LocalPort=0;
        std::chrono::seconds Timeout{18};

        static void Complete(std::shared_ptr<Job> const& job,DnsBytes response={})
        {
            std::lock_guard lock(job->Mutex);
            job->Response=std::move(response);job->Done=true;job->Wake.notify_all();
        }
        void Io(SocketType socket,void* data,std::size_t size,bool writing,Mono::time_point end)
        {Transfer(socket,static_cast<char*>(data),size,writing,end,Closing);}
        DnsBytes ReadPacket(SocketType socket,std::size_t count,Mono::time_point end)
        {
            DnsBytes bytes(count);
            if(count)Io(socket,bytes.data(),count,false,end);
            return bytes;
        }
        void WritePacket(SocketType socket,DnsBytes& bytes,Mono::time_point end)
        {if(!bytes.empty())Io(socket,bytes.data(),bytes.size(),true,end);}

        static std::unique_ptr<Socket> LocalSocket(int kind,unsigned port)
        {
#ifdef _WIN32
            auto s=std::make_unique<Socket>(socket(AF_INET,kind,0));
            BOOL exclusive=TRUE;
            if(s->Value==InvalidSocket||setsockopt(s->Value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<char*>(&exclusive),sizeof(exclusive)))Fail("Cannot create exclusive DNS listener");
#else
            auto s=std::make_unique<Socket>(socket(AF_INET,kind|SOCK_CLOEXEC,0));
            if(s->Value==InvalidSocket)Fail("Cannot open DNS listener");
#endif
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<unsigned short>(port));
            address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(bind(s->Value,reinterpret_cast<sockaddr*>(&address),sizeof(address)))Fail("Cannot bind loopback DNS listener");
            if(kind==SOCK_STREAM&&listen(s->Value,32))Fail("Cannot listen on local DNS socket");
            Nonblocking(s->Value);return s;
        }
        void LocalTcp(SocketType socket)
        {
            Nonblocking(socket);
            while(!Closing&&!Stop)
            {
                auto end=Mono::now()+std::chrono::seconds(20);
                unsigned short length=0;
                Io(socket,&length,sizeof(length),false,end);
                auto packet=ReadPacket(socket,ntohs(length),end);
                auto response=Resolve(packet,true);
                length=htons(static_cast<unsigned short>(response.size()));
                end=Mono::now()+std::chrono::seconds(5);
                Io(socket,&length,sizeof(length),true,end);WritePacket(socket,response,end);
            }
        }
        void AcceptTcp()
        {
            while(!Closing&&!Stop)
            {
                if(!Wait(Tcp->Value,false,Mono::now()+std::chrono::milliseconds(200),Closing))continue;
#ifdef _WIN32
                auto value=accept(Tcp->Value,nullptr,nullptr);
#else
                auto value=accept4(Tcp->Value,nullptr,nullptr,SOCK_CLOEXEC);
#endif
                if(value==InvalidSocket)continue;
                auto s=std::make_shared<Socket>(value);
                TcpPool.Submit([this,s](){LocalTcp(s->Value);});
            }
        }
        void ReceiveUdp()
        {
            while(!Closing&&!Stop)
            {
                if(!Wait(Udp->Value,false,Mono::now()+std::chrono::milliseconds(200),Closing))continue;
                sockaddr_in remote{};
#ifdef _WIN32
                int length=sizeof(remote);
#else
                socklen_t length=sizeof(remote);
#endif
                DnsBytes packet(65535);
                auto n=recvfrom(Udp->Value,reinterpret_cast<char*>(packet.data()),
                    static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&remote),&length);
                if(n<=0)continue;
                packet.resize(static_cast<std::size_t>(n));
                auto sendReply=[this,remote](DnsBytes const& reply)
                {
                    if(!Closing&&!Stop)sendto(Udp->Value,reinterpret_cast<char const*>(reply.data()),
                        static_cast<int>(reply.size()),0,reinterpret_cast<sockaddr const*>(&remote),sizeof(remote));
                };
                auto shared=std::make_shared<DnsBytes>(std::move(packet));
                if(!UdpPool.Submit([this,shared,sendReply](){sendReply(Resolve(*shared,false));}))
                    try{sendReply(DnsFailure(*shared));}catch(...){}
            }
        }
        void AcceptHost()
        {
            while(!Closing&&!Stop)
            {
                if(!Wait(Host->Value,false,Mono::now()+std::chrono::milliseconds(200),Closing))continue;
                auto value=AcceptParent(Host->Value);
                if(value==InvalidSocket)continue;
                Socket connection(value);
                HostChannel(connection.Value);
            }
        }
    public:
        explicit DnsTunnel(unsigned port=53,bool listenHost=true)
        {
            try
            {
                Tcp=LocalSocket(SOCK_STREAM,port);
                sockaddr_in actual{};
#ifdef _WIN32
                int length=sizeof(actual);
#else
                socklen_t length=sizeof(actual);
#endif
                if(getsockname(Tcp->Value,reinterpret_cast<sockaddr*>(&actual),&length))
                    Fail("Cannot inspect DNS listener");
                LocalPort=ntohs(actual.sin_port);
                Udp=LocalSocket(SOCK_DGRAM,LocalPort);
                if(listenHost)Host=OpenParentListener(27454,4);
                TcpPool.Start(8);UdpPool.Start(8);
                Threads.emplace_back([this](){try{AcceptTcp();}catch(...){}});
                Threads.emplace_back([this](){try{ReceiveUdp();}catch(...){}});
                if(Host)for(unsigned i=0;i<4;++i)Threads.emplace_back([this](){try{AcceptHost();}catch(...){}});
            }
            catch(...){Close();throw;}
        }
        ~DnsTunnel(){Close();}
        DnsTunnel(DnsTunnel const&)=delete;
        DnsTunnel& operator=(DnsTunnel const&)=delete;
        unsigned PortNumber()const{return LocalPort;}
        unsigned Channels()
        {std::lock_guard lock(Mutex);return Active;}
        DnsBytes Resolve(DnsBytes const& packet,bool tcp)
        {
            (void)DnsQuestionEnd(packet);
            auto job=std::make_shared<Job>();
            job->Packet=packet;job->Tcp=tcp;job->Deadline=Mono::now()+Timeout;
            {
                std::lock_guard lock(Mutex);
                if(!Active||Closing||Stop||Queue.size()>=64)return DnsFailure(packet);
                Queue.push_back(job);Wake.notify_one();
            }
            std::unique_lock lock(job->Mutex);
            while(!job->Done&&!Closing&&!Stop&&Mono::now()<job->Deadline)
                job->Wake.wait_for(lock,std::chrono::milliseconds(100));
            return job->Response.empty()?DnsFailure(packet):job->Response;
        }
        void HostChannel(SocketType socket)
        {
            {
                std::lock_guard lock(Mutex);
                if(Closing||Stop||Active>=4)return;
                ++Active;
            }
            struct ActiveChannel
            {
                DnsTunnel& Owner;
                ~ActiveChannel()
                {
                    std::lock_guard lock(Owner.Mutex);
                    if(!--Owner.Active)
                    {
                        for(auto const& job:Owner.Queue)Complete(job);
                        Owner.Queue.clear();
                    }
                }
            } active{*this};
            try
            {
                Nonblocking(socket);
                while(!Closing&&!Stop)
                {
                    std::shared_ptr<Job> job;
                    {
                        std::unique_lock lock(Mutex);
                        Wake.wait_for(lock,std::chrono::seconds(1),[&](){return Closing||Stop||!Queue.empty();});
                        if(Closing||Stop)break;
                        if(!Queue.empty()){job=Queue.front();Queue.pop_front();}
                    }
                    auto end=Mono::now()+std::chrono::seconds(20);
                    if(!job)
                    {
                        std::uint32_t zero=0;Io(socket,&zero,sizeof(zero),true,end);continue;
                    }
                    struct JobGuard
                    {
                        std::shared_ptr<Job> Work;bool Finished=false;
                        ~JobGuard(){if(!Finished)Complete(Work);}
                    } guard{job};
                    if(Mono::now()>=job->Deadline)continue;
                    auto length=htonl(static_cast<std::uint32_t>(job->Packet.size()+1));
                    Io(socket,&length,sizeof(length),true,end);
                    unsigned char tcp=job->Tcp?1:0;Io(socket,&tcp,1,true,end);
                    WritePacket(socket,job->Packet,end);
                    Io(socket,&length,sizeof(length),false,end);length=ntohl(length);
                    if(length>65535)Fail("Invalid DNS response length");
                    auto response=ReadPacket(socket,length,end);
                    if(!response.empty()&&(response.size()<12||response[0]!=job->Packet[0]||
                        response[1]!=job->Packet[1]||!(response[2]&0x80)))Fail("Invalid DNS response");
                    Complete(job,std::move(response));guard.Finished=true;
                }
            }
            catch(...){}
        }
        void Close()
        {
            if(Closing.exchange(true))return;
            {
                std::lock_guard lock(Mutex);
                for(auto const& job:Queue)Complete(job);
                Queue.clear();Wake.notify_all();
            }
            for(auto& thread:Threads)if(thread.joinable())thread.join();
            TcpPool.Join();UdpPool.Join();
        }
    };

    void DnsSelfTest()
    {
#ifdef _WIN32
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2,2),&data))Fail("WSAStartup failed");
        struct Wsa{~Wsa(){WSACleanup();}} wsa;
#endif
        DnsBytes packet={0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
        auto failure=DnsFailure(packet);
        if(failure.size()!=packet.size()||failure[3]!=0x82)Fail("DNS failure reply malformed");
        auto invalid=packet;invalid[12]=0xc0;
        bool rejected=false;
        try{DnsQuestionEnd(invalid);}catch(...){rejected=true;}
        if(!rejected)Fail("Compressed DNS query accepted");
        DnsTunnel tunnel(0,false);
        if(tunnel.Resolve(packet,false)!=failure)Fail("Disconnected DNS tunnel did not return SERVFAIL");

        // Use a loopback TCP connection to exercise the real host frame protocol
        // on both operating systems without requiring Hyper-V.
        Socket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(listener.Value==InvalidSocket||bind(listener.Value,reinterpret_cast<sockaddr*>(&address),sizeof(address))||
            listen(listener.Value,1))Fail("Cannot create mock DNS host");
#ifdef _WIN32
        int length=sizeof(address);
#else
        socklen_t length=sizeof(address);
#endif
        if(getsockname(listener.Value,reinterpret_cast<sockaddr*>(&address),&length))Fail("Cannot get mock DNS port");
        Socket host(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        if(host.Value==InvalidSocket||connect(host.Value,reinterpret_cast<sockaddr*>(&address),sizeof(address)))
            Fail("Cannot connect mock DNS host");
        Socket channel(accept(listener.Value,nullptr,nullptr));
        if(channel.Value==InvalidSocket)Fail("Cannot accept mock DNS host");
        Nonblocking(host.Value);
        std::exception_ptr workerError;
        auto answer=packet;answer[2]=0x81;answer[3]=0x80;
        std::thread guest([&](){tunnel.HostChannel(channel.Value);});
        std::thread worker;
        try
        {
            for(unsigned i=0;i<100&&!tunnel.Channels();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if(!tunnel.Channels())Fail("Mock DNS channel did not start");
            worker=std::thread([&]()
            {
                try
                {
                    for(unsigned i=0;i<2;)
                    {
                        auto end=Mono::now()+std::chrono::seconds(8);
                        std::uint32_t size=0;Transfer(host.Value,reinterpret_cast<char*>(&size),sizeof(size),false,end);
                        size=ntohl(size);if(!size)continue;
                        DnsBytes frame(size);Transfer(host.Value,reinterpret_cast<char*>(frame.data()),frame.size(),false,end);
                        if(frame.size()!=packet.size()+1||frame[0]!=i||!std::equal(packet.begin(),packet.end(),frame.begin()+1))
                            Fail("DNS host request framing mismatch");
                        size=htonl(static_cast<std::uint32_t>(answer.size()));
                        Transfer(host.Value,reinterpret_cast<char*>(&size),sizeof(size),true,end);
                        Transfer(host.Value,reinterpret_cast<char*>(answer.data()),answer.size(),true,end);
                        ++i;
                    }
                }catch(...){workerError=std::current_exception();}
            });
            sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            local.sin_port=htons(static_cast<unsigned short>(tunnel.PortNumber()));
            Socket udp(socket(AF_INET,SOCK_DGRAM,0));
            if(udp.Value==InvalidSocket||connect(udp.Value,reinterpret_cast<sockaddr*>(&local),sizeof(local)))
                Fail("Cannot connect local DNS UDP");
            Nonblocking(udp.Value);
            auto end=Mono::now()+std::chrono::seconds(8);
            Transfer(udp.Value,reinterpret_cast<char*>(packet.data()),packet.size(),true,end);
            DnsBytes result(answer.size());
            Transfer(udp.Value,reinterpret_cast<char*>(result.data()),result.size(),false,end);
            if(result!=answer)Fail("DNS UDP reply mismatch");
            Socket tcp(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
            if(tcp.Value==InvalidSocket||connect(tcp.Value,reinterpret_cast<sockaddr*>(&local),sizeof(local)))
                Fail("Cannot connect local DNS TCP");
            Nonblocking(tcp.Value);
            auto size=htons(static_cast<unsigned short>(packet.size()));
            end=Mono::now()+std::chrono::seconds(8);
            Transfer(tcp.Value,reinterpret_cast<char*>(&size),sizeof(size),true,end);
            Transfer(tcp.Value,reinterpret_cast<char*>(packet.data()),packet.size(),true,end);
            Transfer(tcp.Value,reinterpret_cast<char*>(&size),sizeof(size),false,end);
            if(ntohs(size)!=answer.size())Fail("DNS TCP length mismatch");
            Transfer(tcp.Value,reinterpret_cast<char*>(result.data()),result.size(),false,end);
            if(result!=answer)Fail("DNS TCP reply mismatch");
            worker.join();if(workerError)std::rethrow_exception(workerError);
        }
        catch(...)
        {
            tunnel.Close();
#ifdef _WIN32
            shutdown(host.Value,SD_BOTH);
#else
            shutdown(host.Value,SHUT_RDWR);
#endif
            if(worker.joinable())worker.join();
            guest.join();throw;
        }
        tunnel.Close();guest.join();
        std::cout<<"Native DNS UDP/TCP tunnel tests passed\n";
    }
