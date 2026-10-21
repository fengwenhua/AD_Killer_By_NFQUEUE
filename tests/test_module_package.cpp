#include "module_package.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <unistd.h>

void word(std::string &out, uint32_t value, unsigned count) {
    for (unsigned n=0;n<count;++n) out.push_back(static_cast<char>(value>>(8*n)));
}
std::string zip(const std::string &text, bool compressed) {
    std::string bytes=text;
    if (compressed) {
        z_stream stream{};
        assert(deflateInit2(&stream,6,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)==Z_OK);
        bytes.resize(compressBound(text.size()));
        stream.next_in=reinterpret_cast<Bytef *>(const_cast<char *>(text.data())); stream.avail_in=static_cast<uInt>(text.size());
        stream.next_out=reinterpret_cast<Bytef *>(bytes.data()); stream.avail_out=static_cast<uInt>(bytes.size());
        assert(deflate(&stream,Z_FINISH)==Z_STREAM_END);
        bytes.resize(stream.total_out); deflateEnd(&stream);
    }
    const uint32_t crc=static_cast<uint32_t>(crc32(0,reinterpret_cast<const Bytef *>(text.data()),static_cast<uInt>(text.size())));
    const std::string name="module.prop";
    std::string out;
    word(out,0x04034b50,4); word(out,20,2); word(out,0,2); word(out,compressed?8:0,2);
    word(out,0,4); word(out,crc,4); word(out,static_cast<uint32_t>(bytes.size()),4); word(out,static_cast<uint32_t>(text.size()),4);
    word(out,static_cast<uint32_t>(name.size()),2); word(out,0,2); out+=name; out+=bytes;
    const auto offset=static_cast<uint32_t>(out.size());
    word(out,0x02014b50,4); word(out,20,2); word(out,20,2); word(out,0,2); word(out,compressed?8:0,2);
    word(out,0,4); word(out,crc,4); word(out,static_cast<uint32_t>(bytes.size()),4); word(out,static_cast<uint32_t>(text.size()),4);
    word(out,static_cast<uint32_t>(name.size()),2); word(out,0,2); word(out,0,2); word(out,0,2); word(out,0,2); word(out,0,4); word(out,0,4); out+=name;
    const auto central=static_cast<uint32_t>(out.size())-offset;
    word(out,0x06054b50,4); word(out,0,2); word(out,0,2); word(out,1,2); word(out,1,2);
    word(out,central,4); word(out,offset,4); word(out,0,2);
    return out;
}
int main() {
    char pattern[]="/tmp/adk_module_identity.XXXXXX";
    auto created=mkdtemp(pattern); assert(created);
    const auto directory=std::filesystem::path(created), path=directory/"package.zip";
    const std::string text="id=AD_Killer_By_NFQUEUE\nversion=v1.2.2\nversionCode=12201\nreleaseChannel=stable\n";
    auto validate=[&](const std::string &bytes) {
        {std::ofstream file(path,std::ios::binary); file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));}
        return validate_module_identity(path,"1.2.2","stable",12200);
    };
    for (bool compressed : {false,true}) {
        auto bytes=zip(text,compressed);
        assert(validate(bytes).empty());
        assert(!validate_module_identity(path,"1.2.1","stable",121).empty());
        assert(!validate_module_identity(path,"1.2.2","preview",12200).empty());
        assert(!validate_module_identity(path,"1.2.2","stable",12201).empty());
        assert(!validate_module_identity(path,"1.2.2","stable",12202).empty());
        assert(!validate(zip(text+"releaseChannel=preview\n",compressed)).empty());
        assert(!validate(zip(text+std::string(17000,'x'),compressed)).empty());
        auto wrong=text; wrong.replace(wrong.find("stable"),6,"preview");
        assert(!validate(zip(wrong,compressed)).empty());
        assert(!validate(zip("id=other\nversion=v1.2.2\nversionCode=12201\nreleaseChannel=stable\n",compressed)).empty());
        assert(!validate(bytes.substr(0,bytes.size()-1)).empty());
        bytes[30+11]^=1; // corrupt stored/deflated module.prop data
        assert(!validate(bytes).empty());
    }
    assert(!validate("PK\003\004fake").empty());
    std::filesystem::remove_all(directory);
    std::cout<<"ZIP stored/deflate identity, channel, build ordering, CRC, bounds and malformed metadata tests passed\n";
}
