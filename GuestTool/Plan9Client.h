// Copyright (c) NanaBox contributors. SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NanaBox::P9
{
    using Bytes = std::vector<std::uint8_t>;
    constexpr std::uint32_t NoFid = 0xffffffffu;
    constexpr std::uint32_t MaximumFrame = 1u << 20;
    // 9P2000.L uses Linux errno values, independently of the client CRT.
    struct RemoteError : std::runtime_error
    {
        std::uint32_t Number;
        explicit RemoteError(std::uint32_t number) :
            std::runtime_error("9P error " + std::to_string(number)), Number(number) {}
    };
    struct ProtocolError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
    class Writer
    {
    public:
        Bytes Data;
        Writer& U8(std::uint8_t n) { Data.push_back(n); return *this; }
        Writer& U16(std::uint16_t n) { for (unsigned i=0;i<2;++i) U8(static_cast<std::uint8_t>(n>>(i*8))); return *this; }
        Writer& U32(std::uint32_t n) { for (unsigned i=0;i<4;++i) U8(static_cast<std::uint8_t>(n>>(i*8))); return *this; }
        Writer& U64(std::uint64_t n) { for (unsigned i=0;i<8;++i) U8(static_cast<std::uint8_t>(n>>(i*8))); return *this; }
        Writer& Raw(void const* p, std::size_t size)
        {
            if (size > MaximumFrame || Data.size() > MaximumFrame - size) throw ProtocolError("Oversized 9P request");
            if (size)
            {
                auto first=static_cast<std::uint8_t const*>(p);
                Data.insert(Data.end(),first,first+size);
            }
            return *this;
        }
        Writer& String(std::string_view s)
        {
            if (s.size()>65535) throw ProtocolError("Oversized 9P string");
            U16(static_cast<std::uint16_t>(s.size()));
            return Raw(s.data(),s.size());
        }
    };
    class Reader
    {
        Bytes Data;
        std::size_t Position=0;
    public:
        explicit Reader(Bytes data) : Data(std::move(data)) {}
        std::size_t Remaining() const { return Data.size()-Position; }
        void Need(std::size_t n) const { if (n>Remaining()) throw ProtocolError("Truncated 9P response"); }
        std::uint8_t U8() { Need(1); return Data[Position++]; }
        std::uint16_t U16() { std::uint16_t n=0; for(unsigned i=0;i<2;++i)n|=static_cast<std::uint16_t>(std::uint16_t(U8())<<(8*i)); return n; }
        std::uint32_t U32() { std::uint32_t n=0; for(unsigned i=0;i<4;++i)n|=std::uint32_t(U8())<<(8*i); return n; }
        std::uint64_t U64() { std::uint64_t n=0; for(unsigned i=0;i<8;++i)n|=std::uint64_t(U8())<<(8*i); return n; }
        std::string String()
        {
            auto n=U16(); Need(n);
            std::string result(reinterpret_cast<char const*>(Data.data()+Position),n);
            Position+=n; return result;
        }
        Bytes Raw(std::size_t n)
        {
            Need(n);
            auto begin=Data.begin()+static_cast<std::ptrdiff_t>(Position);
            Bytes result(begin,begin+static_cast<std::ptrdiff_t>(n));
            Position+=n; return result;
        }
        void Copy(void* to,std::size_t n)
        {
            Need(n);
            if(n)std::memcpy(to,Data.data()+Position,n);
            Position+=n;
        }
        void End() const { if(Remaining())throw ProtocolError("Unexpected trailing 9P data"); }
    };
    struct Qid
    {
        std::uint8_t Type;
        std::uint32_t Version;
        std::uint64_t Path;
    };
    inline Qid ReadQid(Reader& r) { return {r.U8(),r.U32(),r.U64()}; }
    struct Attr
    {
        std::uint64_t Valid=0;
        Qid Id{};
        std::uint32_t Mode=0,Uid=0,Gid=0;
        std::uint64_t Links=0,Device=0,Size=0,BlockSize=0,Blocks=0;
        std::uint64_t AccessSec=0,AccessNsec=0,ModifySec=0,ModifyNsec=0,ChangeSec=0,ChangeNsec=0,BirthSec=0,BirthNsec=0;
    };
    struct Space
    {
        std::uint32_t Type=0,BlockSize=0;
        std::uint64_t Blocks=0,Free=0,Available=0,Files=0,FreeFiles=0,Id=0;
        std::uint32_t NameLength=0;
    };
    struct Entry { Qid Id; std::uint64_t Offset; std::uint8_t Type; std::string Name; };

    inline bool Component(std::string_view value)
    {
        return !value.empty() && value.size()<=1020 && value!="." && value!=".." &&
            value.find_first_of("/\\") == std::string_view::npos &&
            value.find('\0') == std::string_view::npos;
    }
    inline std::vector<std::string> Components(std::string_view path)
    {
        if(path.empty()||path.front()!='/'||path.size()>32768) throw RemoteError(22);
        std::vector<std::string> result;
        for(std::size_t start=1;start<path.size();)
        {
            auto end=path.find('/',start);
            if(end==std::string_view::npos)end=path.size();
            auto part=path.substr(start,end-start);
            if(!Component(part)||result.size()>=1024)throw RemoteError(22);
            result.emplace_back(part);
            if(end==path.size())break;
            start=end+1;
            if(start==path.size())throw RemoteError(22);
        }
        return result;
    }
    class Client
    {
    public:
        using Exchange = std::function<Bytes(Bytes const&,std::uint32_t)>;
        class Fid
        {
            friend class Client;
            Client* Owner=nullptr;
            std::uint32_t Value=NoFid;
            std::uint32_t IoUnit=0;
            Fid(Client* owner,std::uint32_t value):Owner(owner),Value(value){}
        public:
            Fid()=default;
            ~Fid(){ Reset(); }
            Fid(Fid const&)=delete;
            Fid& operator=(Fid const&)=delete;
            Fid(Fid&& other) noexcept :
                Owner(std::exchange(other.Owner,nullptr)),Value(std::exchange(other.Value,NoFid)),IoUnit(other.IoUnit){}
            Fid& operator=(Fid&& other) noexcept
            {
                if(this!=&other)
                {
                    Reset();Owner=std::exchange(other.Owner,nullptr);
                    Value=std::exchange(other.Value,NoFid);IoUnit=other.IoUnit;
                }
                return *this;
            }
            void Reset() noexcept
            {
                if(Owner&&Value!=NoFid)Owner->Clunk(Value);
                Owner=nullptr;Value=NoFid;
            }
        };
    private:
        Exchange Transport;
        std::mutex Lock;
        std::uint32_t FrameSize=MaximumFrame;
        std::uint32_t NextFid=2;
        std::uint16_t NextTag=0;
        bool Failed=false,Attached=false;
        bool HcsFlush;
        Reader Call(std::uint8_t type,Writer const& body)
        {
            std::lock_guard<std::mutex> lock(Lock);
            if(Failed)throw ProtocolError("9P connection is unusable");
            if(body.Data.size()>FrameSize-7)throw ProtocolError("9P request exceeds negotiated size");
            auto tag=type==100?std::uint16_t(0xffff):NextTag++;
            if(NextTag==0xffff)NextTag=0;
            Writer frame;
            frame.U32(static_cast<std::uint32_t>(body.Data.size()+7)).U8(type).U16(tag)
                .Raw(body.Data.data(),body.Data.size());
            try
            {
                auto bytes=Transport(frame.Data,FrameSize);
                auto size=bytes.size();
                if(size<7||size>FrameSize)throw ProtocolError("Invalid 9P frame size");
                Reader r(std::move(bytes));
                if(r.U32()!=size)throw ProtocolError("Invalid 9P frame length");
                auto response=r.U8();
                if(r.U16()!=tag)throw ProtocolError("Mismatched 9P tag");
                if(response==7)
                {
                    auto error=r.U32();r.End();
                    if(!error)throw ProtocolError("Invalid 9P errno");
                    throw RemoteError(error);
                }
                if(response!=type+1)throw ProtocolError("Unexpected 9P response type");
                return r;
            }
            catch(RemoteError const&){throw;}
            catch(...){Failed=true;throw;}
        }
        std::uint32_t Allocate()
        {
            std::lock_guard<std::mutex> lock(Lock);
            if(NextFid==NoFid)throw ProtocolError("9P file identifier space exhausted");
            return NextFid++;
        }
        std::uint32_t Number(Fid const& fid) const
        {
            if(fid.Owner!=this||fid.Value==NoFid)throw RemoteError(9);
            return fid.Value;
        }
        void Clunk(std::uint32_t id) noexcept
        {
            try { Call(120,Writer().U32(id)).End(); } catch(...) {}
        }
        Fid WalkPart(std::uint32_t source,std::vector<std::string> const& names,std::size_t start,std::size_t count)
        {
            auto id=Allocate();
            Writer w;w.U32(source).U32(id).U16(static_cast<std::uint16_t>(count));
            for(std::size_t i=0;i<count;++i)w.String(names[start+i]);
            auto r=Call(110,w);
            auto actual=r.U16();
            // A partial walk with at least one qid establishes a new fid.
            Fid f(this,id);
            if(actual>count)throw ProtocolError("Invalid 9P walk count");
            for(unsigned i=0;i<actual;++i)(void)ReadQid(r);
            r.End();
            if(actual!=count)
            {
                if(!actual){f.Owner=nullptr;f.Value=NoFid;}
                throw RemoteError(2);
            }
            return f;
        }
        std::uint32_t Limit(Fid const& f) const
        {
            auto size=FrameSize-24;
            return f.IoUnit?std::min(size,f.IoUnit):size;
        }
    public:
        explicit Client(Exchange exchange,bool hcsFlush=false):Transport(std::move(exchange)),HcsFlush(hcsFlush){}
        ~Client(){if(Attached)Clunk(1);}
        Client(Client const&)=delete;
        Client& operator=(Client const&)=delete;
        void Connect(std::string_view name)
        {
            if(Attached)throw ProtocolError("9P client is already attached");
            auto version=Call(100,Writer().U32(MaximumFrame).String("9P2000.L"));
            auto size=version.U32();
            auto protocol=version.String();version.End();
            if(size<256||size>MaximumFrame||protocol!="9P2000.L")throw ProtocolError("9P2000.L negotiation failed");
            FrameSize=size;
            auto attach=Call(104,Writer().U32(1).U32(NoFid).String("").String(name).U32(NoFid));
            (void)ReadQid(attach);attach.End();Attached=true;
        }
        Fid Walk(std::vector<std::string> const& parts)
        {
            if(!Attached)throw ProtocolError("9P client is not attached");
            for(auto const& part:parts)if(!Component(part))throw RemoteError(22);
            Fid current;
            auto source=std::uint32_t(1);
            for(std::size_t start=0;;)
            {
                auto count=std::min<std::size_t>(16,parts.size()-start);
                // The negotiated msize can be smaller than a full 16-element walk.
                std::size_t bytes=17;
                std::size_t fitting=0;
                while(fitting<count&&bytes+2+parts[start+fitting].size()<=FrameSize)
                {bytes+=2+parts[start+fitting].size();++fitting;}
                if(count&&!fitting)throw RemoteError(36);
                current=WalkPart(source,parts,start,fitting);
                start+=fitting;
                if(start==parts.size())return current;
                source=current.Value;
            }
        }
        Fid Walk(std::string_view path){return Walk(Components(path));}
        Fid Child(Fid const& directory,std::string const& name)
        {
            if(!Component(name))throw RemoteError(22);
            return WalkPart(Number(directory),std::vector<std::string>{name},0,1);
        }
        Attr Getattr(Fid const& f)
        {
            auto r=Call(24,Writer().U32(Number(f)).U64(0x3fffull));
            Attr a;
            a.Valid=r.U64();a.Id=ReadQid(r);a.Mode=r.U32();a.Uid=r.U32();a.Gid=r.U32();
            a.Links=r.U64();a.Device=r.U64();a.Size=r.U64();a.BlockSize=r.U64();a.Blocks=r.U64();
            a.AccessSec=r.U64();a.AccessNsec=r.U64();a.ModifySec=r.U64();a.ModifyNsec=r.U64();
            a.ChangeSec=r.U64();a.ChangeNsec=r.U64();a.BirthSec=r.U64();a.BirthNsec=r.U64();
            (void)r.U64();(void)r.U64();r.End();
            if(!(a.Valid&1)||!(a.Valid&0x200))throw ProtocolError("9P server omitted required metadata");
            return a;
        }
        void Open(Fid& f,std::uint32_t flags)
        {
            auto r=Call(12,Writer().U32(Number(f)).U32(flags));
            (void)ReadQid(r);f.IoUnit=r.U32();r.End();
        }
        void Create(Fid& parent,std::string_view name,std::uint32_t flags,std::uint32_t mode,std::uint32_t gid)
        {
            if(!Component(name))throw RemoteError(22);
            auto r=Call(14,Writer().U32(Number(parent)).String(name).U32(flags).U32(mode).U32(gid));
            (void)ReadQid(r);parent.IoUnit=r.U32();r.End();
        }
        void Mkdir(Fid const& parent,std::string_view name,std::uint32_t mode,std::uint32_t gid)
        {
            if(!Component(name))throw RemoteError(22);
            auto r=Call(72,Writer().U32(Number(parent)).String(name).U32(mode).U32(gid));
            (void)ReadQid(r);r.End();
        }
        void Remove(Fid& f)
        {
            auto id=Number(f);
            // Tremove implicitly clunks the fid, even when removal fails.
            f.Owner=nullptr;f.Value=NoFid;
            Call(122,Writer().U32(id)).End();
        }
        void Rename(Fid const& oldParent,std::string_view oldName,Fid const& newParent,std::string_view newName)
        {
            if(!Component(oldName)||!Component(newName))throw RemoteError(22);
            Call(74,Writer().U32(Number(oldParent)).String(oldName).U32(Number(newParent)).String(newName)).End();
        }
        std::size_t Read(Fid const& f,std::uint64_t offset,void* buffer,std::size_t size)
        {
            if(size>std::numeric_limits<std::uint64_t>::max()-offset)throw RemoteError(22);
            auto data=static_cast<std::uint8_t*>(buffer);
            std::size_t total=0;
            while(total<size)
            {
                auto count=static_cast<std::uint32_t>(std::min<std::size_t>(Limit(f),size-total));
                try
                {
                    auto r=Call(116,Writer().U32(Number(f)).U64(offset+total).U32(count));
                    auto actual=r.U32();
                    if(actual>count||actual!=r.Remaining())throw ProtocolError("Invalid 9P read count");
                    r.Copy(data+total,actual);r.End();total+=actual;
                    if(!actual)break;
                }
                catch(RemoteError const&){if(total)return total;throw;}
            }
            return total;
        }
        std::size_t Write(Fid const& f,std::uint64_t offset,void const* buffer,std::size_t size)
        {
            if(size>std::numeric_limits<std::uint64_t>::max()-offset)throw RemoteError(22);
            auto data=static_cast<std::uint8_t const*>(buffer);
            std::size_t total=0;
            while(total<size)
            {
                auto count=static_cast<std::uint32_t>(std::min<std::size_t>(Limit(f),size-total));
                try
                {
                    auto r=Call(118,Writer().U32(Number(f)).U64(offset+total).U32(count).Raw(data+total,count));
                    auto actual=r.U32();r.End();
                    if(actual>count)throw ProtocolError("Invalid 9P write count");
                    if(!actual){if(total)return total;throw RemoteError(5);}
                    total+=actual;
                }
                catch(RemoteError const&){if(total)return total;throw;}
            }
            return total;
        }
        std::vector<Entry> Readdir(Fid const& f,std::uint64_t offset)
        {
            auto count=Limit(f);
            auto r=Call(40,Writer().U32(Number(f)).U64(offset).U32(count));
            auto size=r.U32();
            if(size>count||size!=r.Remaining())throw ProtocolError("Invalid 9P directory count");
            std::vector<Entry> result;
            while(r.Remaining())
            {
                Entry item{ReadQid(r),r.U64(),r.U8(),r.String()};
                if(item.Name.empty()||item.Name.find('\0')!=std::string::npos||
                    item.Name.find('/')!=std::string::npos||item.Offset==offset)
                    throw ProtocolError("Invalid 9P directory entry");
                offset=item.Offset;result.push_back(std::move(item));
            }
            return result;
        }
        void Setattr(Fid const& f,std::uint32_t valid,std::uint32_t mode=0,std::uint64_t size=0,
            std::uint64_t atime=0,std::uint64_t ansec=0,std::uint64_t mtime=0,std::uint64_t mnsec=0)
        {
            Call(26,Writer().U32(Number(f)).U32(valid).U32(mode).U32(0).U32(0)
                .U64(size).U64(atime).U64(ansec).U64(mtime).U64(mnsec)).End();
        }
        void Fsync(Fid const& f,bool dataOnly=false)
        {
            Writer w;w.U32(Number(f));
            if(!HcsFlush)w.U32(dataOnly?1u:0u);
            Call(50,w).End();
        }
        Space Statfs(Fid const& f)
        {
            auto r=Call(8,Writer().U32(Number(f)));
            Space s{r.U32(),r.U32(),r.U64(),r.U64(),r.U64(),r.U64(),r.U64(),r.U64(),r.U32()};
            r.End();
            if(!s.BlockSize||s.Available>s.Free||s.Free>s.Blocks)throw ProtocolError("Invalid 9P filesystem capacity");
            return s;
        }
    };
}
