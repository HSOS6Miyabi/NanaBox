// Copyright (c) NanaBox contributors. SPDX-License-Identifier: MIT
#include "../GuestTool/Plan9Client.h"
#include <cassert>
#include <iostream>
#include <set>

using namespace NanaBox::P9;

int main()
{
    std::set<std::uint32_t> fids;
    unsigned reads=0,writes=0,clunks=0,removes=0;
    bool badTag=false,partial=false,shortRead=false;
    std::uint64_t initial=0x100000000ull;
    auto qid=[](Writer& w){w.U8(0).U32(1).U64(42);};
    auto exchange=[&](Bytes const& bytes,std::uint32_t max)
    {
        Reader req(bytes);
        assert(req.U32()==bytes.size()&&bytes.size()<=max);
        auto type=req.U8();auto tag=req.U16();
        Writer response;std::uint8_t result=static_cast<std::uint8_t>(type+1);
        switch(type)
        {
        case 100: assert(tag==0xffff&&req.U32()==MaximumFrame&&req.String()=="9P2000.L");response.U32(512).String("9P2000.L");break;
        case 104: assert(req.U32()==1&&req.U32()==NoFid&&req.String()==""&&req.String()=="Share"&&req.U32()==NoFid);fids.insert(1);qid(response);break;
        case 110:
        {
            assert(fids.count(req.U32()));auto id=req.U32();auto n=req.U16();
            assert(n<=16);for(unsigned i=0;i<n;++i)assert(Component(req.String()));
            fids.insert(id);auto actual=partial?std::uint16_t(1):n;response.U16(actual);
            for(unsigned i=0;i<actual;++i)qid(response);
            break;
        }
        case 12: assert(fids.count(req.U32()));assert(req.U32()==2);qid(response);response.U32(128);break;
        case 14: assert(fids.count(req.U32()));assert(req.String()=="new");assert(req.U32()==0xc2&&req.U32()==0644&&req.U32()==100);qid(response);response.U32(128);break;
        case 24:
            assert(fids.count(req.U32()));assert(req.U64()==0x3fff);response.U64(0x3fff);qid(response);
            response.U32(0100644).U32(100).U32(100).U64(1).U64(0).U64(initial+400).U64(4096).U64(8);
            for(unsigned i=0;i<10;++i)response.U64(0);
            break;
        case 116:
        {
            assert(fids.count(req.U32()));auto offset=req.U64();auto n=req.U32();
            assert(n<=128);assert(offset==initial+reads*128);
            ++reads;auto actual=shortRead?0:n;
            response.U32(actual);for(unsigned i=0;i<actual;++i)response.U8(static_cast<std::uint8_t>((offset+i)%251));
            break;
        }
        case 118:
        {
            assert(fids.count(req.U32()));auto offset=req.U64();auto n=req.U32();
            assert(n<=128);assert(offset==initial+writes*128);
            ++writes;auto data=req.Raw(n);for(unsigned i=0;i<n;++i)assert(data[i]==static_cast<std::uint8_t>((offset+i)%251));
            response.U32(n);break;
        }
        case 40:
        {
            assert(fids.count(req.U32()));auto offset=req.U64();assert(req.U32()>0);
            if(offset){assert(offset==99);response.U32(0);break;}
            Writer entry;qid(entry);entry.U64(99).U8(8).String("file");
            response.U32(static_cast<std::uint32_t>(entry.Data.size())).Raw(entry.Data.data(),entry.Data.size());break;
        }
        case 26:assert(fids.count(req.U32()));assert(req.U32()==8&&req.U32()==0&&req.U32()==0&&req.U32()==0&&req.U64()==initial);for(int i=0;i<4;++i)assert(req.U64()==0);break;
        case 50:assert(fids.count(req.U32()));assert(req.U32()==0);break;
        case 8:assert(fids.count(req.U32()));response.U32(0).U32(4096).U64(100).U64(20).U64(10).U64(30).U64(15).U64(1).U32(255);break;
        case 72:assert(fids.count(req.U32())&&req.String()=="dir"&&req.U32()==0755&&req.U32()==100);qid(response);break;
        case 74:assert(fids.count(req.U32())&&req.String()=="old"&&fids.count(req.U32())&&req.String()=="new");break;
        case 120:assert(fids.erase(req.U32())==1);++clunks;break;
        case 122:assert(fids.erase(req.U32())==1);++removes;break;
        default:assert(false);
        }
        req.End();
        Writer frame;frame.U32(static_cast<std::uint32_t>(7+response.Data.size())).U8(result).U16(badTag?std::uint16_t(tag+1):tag).Raw(response.Data.data(),response.Data.size());
        return frame.Data;
    };
    {
        Client c(exchange);c.Connect("Share");
        {
            auto f=c.Walk("/a/b/c");
            assert(c.Getattr(f).Size==initial+400);c.Open(f,2);
            Bytes buffer(400);assert(c.Read(f,initial,buffer.data(),buffer.size())==400&&reads==4);
            for(unsigned i=0;i<buffer.size();++i)assert(buffer[i]==static_cast<std::uint8_t>((initial+i)%251));
            assert(c.Write(f,initial,buffer.data(),buffer.size())==400&&writes==4);
            shortRead=true;assert(c.Read(f,initial+512,buffer.data(),10)==0);
            c.Setattr(f,8,0,initial);c.Fsync(f);
            assert(c.Statfs(f).Available==10);
        }
        {
            auto root=c.Walk("/");
            auto dir=c.Readdir(root,0);assert(dir.size()==1&&dir[0].Name=="file"&&dir[0].Offset==99);
            assert(c.Readdir(root,99).empty());
            c.Mkdir(root,"dir",0755,100);c.Rename(root,"old",root,"new");
            auto newFile=c.Walk("/");c.Create(newFile,"new",0xc2,0644,100);c.Remove(newFile);
            assert(removes==1);
        }
        {
            std::vector<std::string> longPath(40,"folder");
            auto f=c.Walk(longPath);assert(fids.size()==2);
        }
        partial=true;
        try { (void)c.Walk("/a/b"); assert(false); } catch(RemoteError const& e){assert(e.Number==2);}
        partial=false;assert(fids.size()==1);
        for(auto path:{"/../escape","/a//b","a","/a/"})
            try{(void)c.Walk(path);assert(false);}catch(RemoteError const& e){assert(e.Number==22);}
        assert(clunks>=7);
    }
    assert(fids.empty());
    {
        Client c(exchange);c.Connect("Share");badTag=true;
        for(int i=0;i<2;++i)try{(void)c.Walk("/");assert(false);}catch(ProtocolError const&){}
    }
    std::cout<<"9P2000.L client wire, fid lifecycle, random I/O and malformed response tests passed.\n";
}
