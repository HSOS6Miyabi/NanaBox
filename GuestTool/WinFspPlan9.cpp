// Copyright (c) NanaBox contributors. SPDX-License-Identifier: MIT
// WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos.
// https://github.com/winfsp/winfsp
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#include <hvsocket.h>
#include <winfsp/winfsp.h>
#include <fuse.h>
#include <fcntl.h>
#include <cerrno>
#include <climits>
#include <atomic>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include "Plan9Client.h"
#include "WddmMapping.h"

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"advapi32.lib")

namespace
{
    using namespace NanaBox::P9;
    struct Handle
    {
        HANDLE Value;
        explicit Handle(HANDLE h=nullptr):Value(h){}
        ~Handle(){if(Value&&Value!=INVALID_HANDLE_VALUE)CloseHandle(Value);}
        Handle(Handle const&)=delete;
        Handle& operator=(Handle const&)=delete;
    };

    std::string Utf8(std::wstring_view value)
    {
        if(value.empty())return {};
        auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
        if(!n)throw RemoteError(22);
        std::string result(n,'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),n,nullptr,nullptr)!=n)
            throw RemoteError(22);
        return result;
    }
    std::wstring Wide(std::string_view value)
    {
        if(value.empty())return {};
        auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
        if(!n)throw RemoteError(22);
        std::wstring result(n,L'\0');
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),n)!=n)
            throw RemoteError(22);
        return result;
    }
    bool ValidName(std::string_view value)
    {
        if(!Component(value))return false;
        try
        {
            auto wide=Wide(value);
            if(wide.size()>255)return false;
            // Semicolons and commas are legal in file names. The GPU helper
            // excludes semicolons because its paths are also used in PATH.
            for(auto& ch:wide)if(ch==L';')ch=L'_';
            return NanaBox::Wddm::SafeComponent(wide);
        }
        catch(...){return false;}
    }
    std::vector<std::string> Path(std::string_view value)
    {
        auto parts=Components(value);
        for(auto const& part:parts)if(!ValidName(part))throw RemoteError(22);
        return parts;
    }

    class Connection
    {
        SOCKET Socket=INVALID_SOCKET;
        void Transfer(void* buffer,std::size_t size,bool writing)
        {
            auto bytes=static_cast<char*>(buffer);
            while(size)
            {
                auto n=writing?send(Socket,bytes,static_cast<int>(size),0):recv(Socket,bytes,static_cast<int>(size),0);
                if(n<=0)throw ProtocolError(writing?"9P send failed":"9P receive failed");
                bytes+=n;size-=static_cast<std::size_t>(n);
            }
        }
    public:
        Connection(unsigned port,bool loopback)
        {
            WSADATA data{};
            if(WSAStartup(MAKEWORD(2,2),&data))throw ProtocolError("Winsock initialization failed");
            try
            {
                Socket=socket(loopback?AF_INET:AF_HYPERV,SOCK_STREAM,loopback?IPPROTO_TCP:HV_PROTOCOL_RAW);
                if(Socket==INVALID_SOCKET)throw ProtocolError("Cannot open 9P socket");
                u_long nonblocking=1;
                if(ioctlsocket(Socket,FIONBIO,&nonblocking))throw ProtocolError("Cannot configure 9P socket");
                int result;
                if(loopback)
                {
                    sockaddr_in addr{};
                    addr.sin_family=AF_INET;addr.sin_port=htons(static_cast<unsigned short>(port));
                    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
                    result=connect(Socket,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
                }
                else
                {
                    constexpr GUID parent{0xa42e7cda,0xd03f,0x480c,{0x9c,0xc2,0xa4,0xde,0x20,0xab,0xb8,0x78}};
                    SOCKADDR_HV addr{};addr.Family=AF_HYPERV;addr.VmId=parent;
                    addr.ServiceId=GUID{port,0xfacb,0x11e6,{0xbd,0x58,0x64,0,0x6a,0x79,0x86,0xd3}};
                    result=connect(Socket,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
                }
                if(result)
                {
                    if(WSAGetLastError()!=WSAEWOULDBLOCK)throw ProtocolError("Cannot connect to 9P server");
                    fd_set writable,errors;FD_ZERO(&writable);FD_ZERO(&errors);FD_SET(Socket,&writable);FD_SET(Socket,&errors);
                    timeval timeout{5,0};
                    if(select(0,nullptr,&writable,&errors,&timeout)<=0)throw ProtocolError("9P connection timed out");
                    int error=0,length=sizeof(error);
                    if(getsockopt(Socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)||error)
                        throw ProtocolError("9P connection failed");
                }
                nonblocking=0;
                DWORD timeout=5000;
                if(ioctlsocket(Socket,FIONBIO,&nonblocking)||
                    setsockopt(Socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout))||
                    setsockopt(Socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout)))
                    throw ProtocolError("Cannot configure 9P socket deadlines");
            }
            catch(...){if(Socket!=INVALID_SOCKET)closesocket(Socket);WSACleanup();throw;}
        }
        ~Connection(){if(Socket!=INVALID_SOCKET)closesocket(Socket);WSACleanup();}
        Connection(Connection const&)=delete;
        Bytes Exchange(Bytes const& request,std::uint32_t maximum)
        {
            Transfer(const_cast<std::uint8_t*>(request.data()),request.size(),true);
            Bytes response(4);Transfer(response.data(),4,false);
            Reader header(response);
            auto length=header.U32();
            if(length<7||length>maximum)throw ProtocolError("Invalid 9P message length");
            response.resize(length);Transfer(response.data()+4,length-4,false);
            return response;
        }
    };

    struct Context
    {
        Connection Wire;
        Client P9;
        bool ReadOnly;
        std::uint32_t Gid=0;
        Handle Stop,Done;
        std::thread Watch;
        std::atomic<bool> StartFailed{false};
        Context(unsigned port,bool loopback,bool readOnly,std::wstring const& event):
            Wire(port,loopback),
            P9([this](Bytes const& bytes,std::uint32_t max){return Wire.Exchange(bytes,max);},!loopback),
            ReadOnly(readOnly),Stop(event.empty()?nullptr:OpenEventW(SYNCHRONIZE,FALSE,event.c_str())),
            Done(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {
            if(!Done.Value||(!event.empty()&&!Stop.Value))throw ProtocolError("Cannot open mount stop event");
        }
        ~Context(){StopWatch();}
        void StopWatch(){if(Watch.joinable()){SetEvent(Done.Value);Watch.join();}}
        void Mutable()const{if(ReadOnly)throw RemoteError(30);}
    };
    Context& Ctx(){return *static_cast<Context*>(fuse_get_context()->private_data);}
    struct OpenFile
    {
        Client::Fid Id;
        bool Writable,Directory;
        OpenFile(Client::Fid fid,bool writable,bool directory):Id(std::move(fid)),Writable(writable),Directory(directory){}
    };
    OpenFile& File(fuse_file_info const* fi)
    {
        if(!fi||!fi->fh)throw RemoteError(9);
        return *reinterpret_cast<OpenFile*>(static_cast<std::uintptr_t>(fi->fh));
    }
    void Save(fuse_file_info* fi,Client::Fid fid,bool writable,bool directory)
    {
        auto file=std::make_unique<OpenFile>(std::move(fid),writable,directory);
        fi->fh=reinterpret_cast<std::uintptr_t>(file.release());
    }
    int ErrorNumber(std::uint32_t linux)
    {
        switch(linux)
        {
        case 1:return EPERM;case 2:return ENOENT;case 4:return EINTR;
        case 5:return EIO;case 6:return ENXIO;case 9:return EBADF;
        case 11:return EAGAIN;case 12:return ENOMEM;case 13:return EACCES;
        case 16:return EBUSY;case 17:return EEXIST;case 18:return EXDEV;
        case 19:return ENODEV;case 20:return ENOTDIR;case 21:return EISDIR;
        case 22:return EINVAL;case 24:return EMFILE;case 27:return EFBIG;
        case 28:return ENOSPC;case 30:return EROFS;case 31:return EMLINK;
        case 34:return ERANGE;case 36:return ENAMETOOLONG;case 38:return ENOSYS;
        case 39:return ENOTEMPTY;case 40:return ELOOP;case 75:return EOVERFLOW;
        case 95:return ENOTSUP;case 110:return ETIMEDOUT;default:return EIO;
        }
    }
    template<class F> int Guard(F&& f) noexcept
    {
        try{return f();}
        catch(RemoteError const& e){return -ErrorNumber(e.Number);}
        catch(std::bad_alloc const&){return -ENOMEM;}
        catch(std::exception const& e){std::cerr<<"9P: "<<e.what()<<'\n';return -EIO;}
        catch(...){return -EIO;}
    }
    std::uint32_t LinuxFlags(int flags,bool create)
    {
        auto access=flags&3;
        if(access==3)throw RemoteError(22);
        auto n=static_cast<std::uint32_t>(access);
        if(flags&_O_TRUNC)n|=0x200;
        if(create)n|=0x40|0x80;
        // WinFsp supplies the effective offset for append writes.
        return n;
    }
    std::pair<Client::Fid,std::string> Parent(char const* path)
    {
        auto parts=Path(path);
        if(parts.empty())throw RemoteError(16);
        auto name=std::move(parts.back());parts.pop_back();
        return {Ctx().P9.Walk(parts),std::move(name)};
    }
    bool Directory(Attr const& a){return (a.Mode&0170000u)==0040000u;}
    void CheckType(Attr const& a)
    {
        auto type=a.Mode&0170000u;
        if(type!=0040000u&&type!=0100000u)throw RemoteError(95);
    }
    void Metadata(Attr const& a,fuse_stat* st)
    {
        CheckType(a);
        if(a.Size>INT64_MAX||a.Blocks>INT64_MAX)throw RemoteError(75);
        *st={};
        st->st_ino=a.Id.Path;st->st_mode=a.Mode&0170777u;
        if(Ctx().ReadOnly)st->st_mode&=~0222u;
        st->st_nlink=static_cast<fuse_nlink_t>((std::min<std::uint64_t>)(a.Links?a.Links:1,65535));
        st->st_uid=a.Uid;st->st_gid=a.Gid;
        st->st_size=static_cast<fuse_off_t>(a.Size);
        st->st_blksize=static_cast<fuse_blksize_t>(a.BlockSize&&a.BlockSize<=INT32_MAX?a.BlockSize:4096);
        st->st_blocks=static_cast<fuse_blkcnt_t>(a.Blocks);
        auto time=[](fuse_timespec& t,std::uint64_t sec,std::uint64_t nsec)
        {
            if(sec>253402300799ull||nsec>=1000000000ull)return;
            t.tv_sec=static_cast<decltype(t.tv_sec)>(sec);
            t.tv_nsec=static_cast<decltype(t.tv_nsec)>(nsec);
        };
        if(a.Valid&0x20)time(st->st_atim,a.AccessSec,a.AccessNsec);
        if(a.Valid&0x40)time(st->st_mtim,a.ModifySec,a.ModifyNsec);
        if(a.Valid&0x80)time(st->st_ctim,a.ChangeSec,a.ChangeNsec);
        if(a.Valid&0x800)time(st->st_birthtim,a.BirthSec,a.BirthNsec);
    }
    int Getattr(char const* path,fuse_stat* st)
    {return Guard([&]{auto f=Ctx().P9.Walk(Path(path));Metadata(Ctx().P9.Getattr(f),st);return 0;});}
    int Fgetattr(char const*,fuse_stat* st,fuse_file_info* fi)
    {return Guard([&]{Metadata(Ctx().P9.Getattr(File(fi).Id),st);return 0;});}
    int Open(char const* path,fuse_file_info* fi)
    {
        return Guard([&]{
            auto flags=LinuxFlags(fi->flags,false);
            bool write=(flags&3)!=0;
            if(write||(flags&0x200))Ctx().Mutable();
            auto f=Ctx().P9.Walk(Path(path));auto a=Ctx().P9.Getattr(f);CheckType(a);
            if(Directory(a))throw RemoteError(21);
            Ctx().P9.Open(f,flags);Save(fi,std::move(f),write,false);return 0;
        });
    }
    int Create(char const* path,fuse_mode_t mode,fuse_file_info* fi)
    {
        return Guard([&]{
            Ctx().Mutable();auto pair=Parent(path);
            auto flags=LinuxFlags(fi->flags,true);
            Ctx().P9.Create(pair.first,pair.second,flags,mode&0777u,Ctx().Gid);
            Save(fi,std::move(pair.first),(flags&3)!=0,false);return 0;
        });
    }
    int Opendir(char const* path,fuse_file_info* fi)
    {
        return Guard([&]{
            auto f=Ctx().P9.Walk(Path(path));if(!Directory(Ctx().P9.Getattr(f)))throw RemoteError(20);
            Ctx().P9.Open(f,0);Save(fi,std::move(f),false,true);return 0;
        });
    }
    int Release(char const*,fuse_file_info* fi)
    {
        if(fi&&fi->fh){delete reinterpret_cast<OpenFile*>(static_cast<std::uintptr_t>(fi->fh));fi->fh=0;}
        return 0;
    }
    int Read(char const*,char* buffer,std::size_t size,fuse_off_t offset,fuse_file_info* fi)
    {
        return Guard([&]{
            if(offset<0||size>INT_MAX)throw RemoteError(22);
            auto& file=File(fi);if(file.Directory)throw RemoteError(21);
            return static_cast<int>(Ctx().P9.Read(file.Id,static_cast<std::uint64_t>(offset),buffer,size));
        });
    }
    int Write(char const*,char const* buffer,std::size_t size,fuse_off_t offset,fuse_file_info* fi)
    {
        return Guard([&]{
            Ctx().Mutable();if(offset<0||size>INT_MAX)throw RemoteError(22);
            auto& file=File(fi);if(!file.Writable||file.Directory)throw RemoteError(9);
            return static_cast<int>(Ctx().P9.Write(file.Id,static_cast<std::uint64_t>(offset),buffer,size));
        });
    }
    int Fsync(char const*,int dataOnly,fuse_file_info* fi)
    {
        return Guard([&]{
            auto& file=File(fi);
            if(!Ctx().ReadOnly)Ctx().P9.Fsync(file.Id,dataOnly!=0);
            return 0;
        });
    }
    int Flush(char const* path,fuse_file_info* fi)
    {
        return Guard([&]{if(File(fi).Writable)return Fsync(path,0,fi);return 0;});
    }
    int Readdir(char const*,void* buffer,fuse_fill_dir_t fill,fuse_off_t offset,fuse_file_info* fi)
    {
        return Guard([&]{
            auto& file=File(fi);if(!file.Directory||offset!=0)throw RemoteError(22);
            auto& p9=Ctx().P9;
            std::set<std::uint64_t> cookies;
            std::set<std::string> names;
            std::uint64_t cursor=0;
            for(;;)
            {
                auto entries=p9.Readdir(file.Id,cursor);if(entries.empty())break;
                for(auto const& e:entries)
                {
                    if(!cookies.insert(e.Offset).second||cookies.size()>1000000)throw ProtocolError("Invalid or excessive directory enumeration");
                    cursor=e.Offset;
                    if(e.Name=="."||e.Name=="..")
                    {
                        if(names.insert(e.Name).second&&fill(buffer,e.Name.c_str(),nullptr,0))return -ENOMEM;
                        continue;
                    }
                    if(!ValidName(e.Name)||!names.insert(e.Name).second)continue;
                    // WinFsp performs the final getattr for entries without a supplied stat.
                    if(fill(buffer,e.Name.c_str(),nullptr,0))return -ENOMEM;
                }
            }
            return 0;
        });
    }
    int Statfs(char const* path,fuse_statvfs* st)
    {
        return Guard([&]{
            auto f=Ctx().P9.Walk(Path(path));auto s=Ctx().P9.Statfs(f);
            *st={};st->f_bsize=s.BlockSize;st->f_frsize=s.BlockSize;
            st->f_blocks=s.Blocks;st->f_bfree=s.Free;st->f_bavail=s.Available;
            st->f_files=s.Files;st->f_ffree=s.FreeFiles;st->f_favail=s.FreeFiles;
            st->f_fsid=s.Id;st->f_namemax=(std::min)(s.NameLength,255u);return 0;
        });
    }
    int Mkdir(char const* path,fuse_mode_t mode)
    {return Guard([&]{Ctx().Mutable();auto p=Parent(path);Ctx().P9.Mkdir(p.first,p.second,mode&0777u,Ctx().Gid);return 0;});}
    int Remove(char const* path,bool directory)
    {
        return Guard([&]{
            Ctx().Mutable();auto parts=Path(path);if(parts.empty())throw RemoteError(16);
            auto f=Ctx().P9.Walk(parts);auto a=Ctx().P9.Getattr(f);CheckType(a);
            if(Directory(a)!=directory)throw RemoteError(directory?20:21);
            Ctx().P9.Remove(f);return 0;
        });
    }
    int Unlink(char const* path){return Remove(path,false);}
    int Rmdir(char const* path){return Remove(path,true);}
    int Rename(char const* oldPath,char const* newPath)
    {return Guard([&]{Ctx().Mutable();auto a=Parent(oldPath);auto b=Parent(newPath);Ctx().P9.Rename(a.first,a.second,b.first,b.second);return 0;});}
    int Truncate(char const* path,fuse_off_t length)
    {
        return Guard([&]{
            Ctx().Mutable();if(length<0)throw RemoteError(22);
            auto f=Ctx().P9.Walk(Path(path));if(Directory(Ctx().P9.Getattr(f)))throw RemoteError(21);
            Ctx().P9.Setattr(f,8,0,static_cast<std::uint64_t>(length));return 0;
        });
    }
    int Ftruncate(char const*,fuse_off_t length,fuse_file_info* fi)
    {
        return Guard([&]{
            Ctx().Mutable();auto& f=File(fi);
            if(length<0||!f.Writable||f.Directory)throw RemoteError(22);
            Ctx().P9.Setattr(f.Id,8,0,static_cast<std::uint64_t>(length));return 0;
        });
    }
    int Chmod(char const* path,fuse_mode_t mode)
    {return Guard([&]{Ctx().Mutable();auto f=Ctx().P9.Walk(Path(path));Ctx().P9.Setattr(f,1,mode&0777u);return 0;});}
    int Utimens(char const* path,fuse_timespec const times[2])
    {
        return Guard([&]{
            Ctx().Mutable();auto f=Ctx().P9.Walk(Path(path));
            if(!times){Ctx().P9.Setattr(f,0x30);return 0;}
            std::uint32_t flags=0;std::uint64_t seconds[2]{},nanos[2]{};
            for(unsigned i=0;i<2;++i)
            {
                // POSIX UTIME_OMIT and UTIME_NOW (nanosecond sentinel values).
                auto nsec=times[i].tv_nsec;
                if(nsec==1073741822)continue;
                flags|=i?0x20:0x10;
                if(nsec==1073741823)continue;
                if(times[i].tv_sec<0||nsec<0||nsec>=1000000000)throw RemoteError(22);
                flags|=i?0x100:0x80;
                seconds[i]=static_cast<std::uint64_t>(times[i].tv_sec);nanos[i]=static_cast<std::uint64_t>(nsec);
            }
            if(flags)Ctx().P9.Setattr(f,flags,0,0,seconds[0],nanos[0],seconds[1],nanos[1]);
            return 0;
        });
    }
    void* Init(fuse_conn_info* conn)
    {
        auto& ctx=Ctx();
        if(ctx.ReadOnly)conn->want|=conn->capable&FSP_FUSE_CAP_READ_ONLY;
        if(ctx.Stop.Value)
        {
            auto fs=fuse_get_context()->fuse;
            try
            {
                ctx.Watch=std::thread([&ctx,fs]{
                    HANDLE events[]={ctx.Done.Value,ctx.Stop.Value};
                    if(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0+1)fuse_exit(fs);
                });
            }
            catch(...){ctx.StartFailed=true;fuse_exit(fs);}
        }
        return &ctx;
    }
    void Destroy(void* data){static_cast<Context*>(data)->StopWatch();}
    fuse_operations Operations()
    {
        fuse_operations ops{};
        ops.init=Init;ops.destroy=Destroy;
        ops.getattr=Getattr;ops.fgetattr=Fgetattr;ops.open=Open;ops.create=Create;
        ops.read=Read;ops.write=Write;ops.flush=Flush;ops.fsync=Fsync;ops.release=Release;
        ops.opendir=Opendir;ops.readdir=Readdir;ops.releasedir=Release;ops.fsyncdir=Fsync;
        ops.statfs=Statfs;ops.mkdir=Mkdir;ops.unlink=Unlink;ops.rmdir=Rmdir;ops.rename=Rename;
        ops.truncate=Truncate;ops.ftruncate=Ftruncate;ops.chmod=Chmod;ops.utimens=Utimens;
        return ops;
    }
}

int wmain(int argc,wchar_t** argv)
{
    try
    {
        std::cout<<"NanaBox 9P2000.L filesystem\nWinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos\nhttps://github.com/winfsp/winfsp\n";
        if(argc==2&&std::wstring_view(argv[1])==L"--help")
        {
            std::cout<<"Usage: nanabox-plan9-winfsp --hvsocket PORT SHARE MOUNTPOINT [--read-only] [--all-users] [--stop-event NAME]\n";
            return 0;
        }
        if(argc<5)throw ProtocolError("Specify transport, port, share and mountpoint (see --help)");
        auto mode=std::wstring_view(argv[1]);bool loopback=mode==L"--test-loopback";
        if(!loopback&&mode!=L"--hvsocket")throw ProtocolError("Unknown 9P transport");
        auto portText=std::wstring_view(argv[2]);
        if(portText.empty()||portText.size()>10||!std::all_of(portText.begin(),portText.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';}))
            throw ProtocolError("Invalid 9P port");
        auto port=std::stoull(std::wstring(portText));
        if(!port||port>=NoFid||(loopback&&port>65535))throw ProtocolError("Invalid 9P port");
        auto share=Utf8(argv[3]);if(!ValidName(share)||share.size()>200||share.find_first_of(",;")!=std::string::npos)throw ProtocolError("Invalid 9P share name");
        auto mount=Utf8(argv[4]);
        bool readOnly=false,allUsers=false;std::wstring stopEvent;
        for(int i=5;i<argc;++i)
        {
            auto arg=std::wstring_view(argv[i]);
            if(arg==L"--read-only")readOnly=true;
            else if(arg==L"--all-users")allUsers=true;
            else if(arg==L"--stop-event"&&i+1<argc)stopEvent=argv[++i];
            else throw ProtocolError("Unknown 9P option");
        }
        // Resolve only the registered WinFsp installation, without searching the
        // working directory for DLLs in a privileged guest service process.
        if(!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_APPLICATION_DIR))
            throw ProtocolError("Cannot configure safe WinFsp DLL loading");
        if(FspLoad(nullptr)<0)throw ProtocolError("Install WinFsp before mounting a 9P share");
        Context ctx(static_cast<unsigned>(port),loopback,readOnly,stopEvent);
        ctx.P9.Connect(share);
        {auto root=ctx.P9.Walk("/");auto a=ctx.P9.Getattr(root);if(!Directory(a))throw ProtocolError("9P root is not a directory");ctx.Gid=a.Gid;}
        auto options=std::string("uid=-1,gid=-1,FileInfoTimeout=0,DirInfoTimeout=0,FileSystemName=NanaBox9P,volname=NanaBox9P-")+std::to_string(GetCurrentProcessId());
        if(allUsers)options+=readOnly?",FileSecurity=D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)":",FileSecurity=D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)";
        std::vector<std::string> args{"nanabox-plan9-winfsp","-f","-o",options,mount};
        std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
        auto ops=Operations();
        auto result=fuse_main(static_cast<int>(raw.size()),raw.data(),&ops,&ctx);
        return result||ctx.StartFailed?1:0;
    }
    catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
