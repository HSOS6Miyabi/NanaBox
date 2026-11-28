#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace NanaBox::Vsmb
{
    // LanmanRedirector request layouts documented by the version-aware NanaRun
    // VirtualSmb utility and Microsoft's hcsshim gcs-sidecar/vsmb.go.
    // The control structures use little-endian integers and UTF-16 names.
    inline std::vector<unsigned> Layouts(unsigned build)
    {
        if (build >= 26100) return {120, 116};
        if (build >= 25398) return {108};
        if (build >= 20348) return {92};
        if (build >= 17134) return {88};
        return {};
    }

    inline void Put16(std::vector<unsigned char>& bytes, std::size_t pos, std::uint16_t value)
    {
        if (pos > bytes.size() || bytes.size() - pos < 2) throw std::runtime_error("Invalid VSMB field");
        bytes[pos] = static_cast<unsigned char>(value);
        bytes[pos+1] = static_cast<unsigned char>(value >> 8);
    }
    inline void Put32(std::vector<unsigned char>& bytes, std::size_t pos, std::uint32_t value)
    {
        Put16(bytes, pos, static_cast<std::uint16_t>(value));
        Put16(bytes, pos+2, static_cast<std::uint16_t>(value >> 16));
    }
    inline std::uint32_t Get32(std::vector<unsigned char> const& bytes, std::size_t pos)
    {
        if (pos > bytes.size() || bytes.size() - pos < 4) throw std::runtime_error("Invalid Bind Filter response");
        return std::uint32_t(bytes[pos]) | (std::uint32_t(bytes[pos+1]) << 8) |
            (std::uint32_t(bytes[pos+2]) << 16) | (std::uint32_t(bytes[pos+3]) << 24);
    }
    inline void PutName(std::vector<unsigned char>& bytes, std::size_t offset, std::u16string_view name)
    {
        for (std::size_t i=0;i<name.size();++i) Put16(bytes,offset+2*i,name[i]);
    }
    inline std::vector<unsigned char> StartRequest(unsigned layout)
    {
        unsigned connectionOffset;
        switch (layout)
        {
        case 88: connectionOffset=64; break;
        case 92: connectionOffset=68; break;
        case 108: connectionOffset=76; break;
        case 116: connectionOffset=76; break;
        case 120: connectionOffset=80; break;
        default: throw std::runtime_error("Unsupported VSMB request layout");
        }
        constexpr std::u16string_view name=u"\\Device\\vmsmb";
        std::vector<unsigned char> result(layout+name.size()*2);
        // The redirector expects the current structure identifier even when
        // consuming a shorter, build-specific request body.
        Put32(result,0,120);
        Put32(result,4,30); Put32(result,8,3);
        Put16(result,12,0x1e);
        result[connectionOffset]=0x1f;
        Put32(result,connectionOffset+4,55);
        Put32(result,connectionOffset+8,10);
        Put32(result,connectionOffset+12,30);
        Put32(result,connectionOffset+16,30);
        result[layout-4]=1;
        Put16(result,layout-2,static_cast<std::uint16_t>(name.size()*2));
        PutName(result,layout,name);
        return result;
    }
    inline std::vector<unsigned char> BindRequest()
    {
        constexpr std::u16string_view transport=u"\\Device\\VMBus\\{4d12e519-17a0-4ae4-8eaa-5270fc6abdb7}-{dcc079ae-60ba-4d07-847c-3493609c0870}-0000";
        std::vector<unsigned char> result(12+transport.size()*2);
        Put16(result,0,16); Put32(result,4,2);
        Put32(result,8,static_cast<std::uint32_t>(transport.size()*2));
        PutName(result,12,transport);
        return result;
    }

    struct Binding
    {
        std::u16string Root;
        std::uint32_t Flags;
        std::vector<std::u16string> Targets;
    };
    inline std::u16string ReadName(std::vector<unsigned char> const& bytes, std::uint32_t offset, std::uint32_t size)
    {
        if (offset%2 || size%2 || offset>bytes.size() || size>bytes.size()-offset || !size)
            throw std::runtime_error("Invalid Bind Filter name");
        std::u16string value;
        value.reserve(size/2);
        for(std::size_t i=offset;i<std::size_t(offset)+size;i+=2)
        {
            auto ch=static_cast<char16_t>(unsigned(bytes[i]) | (unsigned(bytes[i+1])<<8));
            if(!ch)
            {
                if(i+2!=std::size_t(offset)+size)throw std::runtime_error("Embedded NUL in Bind Filter name");
                break;
            }
            value.push_back(ch);
        }
        if(value.empty())throw std::runtime_error("Empty Bind Filter name");
        return value;
    }
    inline std::vector<Binding> ParseBindings(std::vector<unsigned char> const& bytes)
    {
        if(bytes.size()<12||Get32(bytes,4)!=0)throw std::runtime_error("Bind Filter query failed");
        auto count=Get32(bytes,8);
        if(count>(bytes.size()-12)/20)throw std::runtime_error("Invalid Bind Filter mapping count");
        std::vector<Binding> result;
        for(std::uint32_t i=0;i<count;++i)
        {
            std::size_t p=12+std::size_t(i)*20;
            Binding b{ReadName(bytes,Get32(bytes,p+4),Get32(bytes,p)),Get32(bytes,p+8),{}};
            auto n=Get32(bytes,p+12),start=Get32(bytes,p+16);
            if(start>bytes.size()||n>(bytes.size()-start)/8)throw std::runtime_error("Invalid Bind Filter target count");
            for(std::uint32_t j=0;j<n;++j)
            {
                std::size_t q=std::size_t(start)+std::size_t(j)*8;
                b.Targets.push_back(ReadName(bytes,Get32(bytes,q+4),Get32(bytes,q)));
            }
            result.push_back(std::move(b));
        }
        return result;
    }
}
