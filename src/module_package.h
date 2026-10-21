#pragma once
#include "cli_utils.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <zlib.h>

// Inspect only bounded ZIP directory/metadata ranges, never extract or execute
// archive paths. SHA-256 is checked separately before this identity check.
inline std::string validate_module_identity(const std::string &path, const std::string &version,
                                           const std::string &channel, int installed_code) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return "无法读取安装包元数据";
    const auto length = file.tellg();
    if (length < 22 || length > 64*1024*1024) return "安装包 ZIP 大小无效";
    const uint64_t size = static_cast<uint64_t>(length);
    auto read = [&](uint64_t offset, size_t bytes) {
        std::vector<unsigned char> result;
        if (offset > size || bytes > size-offset) return result;
        result.resize(bytes);
        file.clear(); file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(bytes));
        if (!file) result.clear();
        return result;
    };
    auto u16 = [](const unsigned char *p) { return uint32_t(p[0]) | uint32_t(p[1])<<8; };
    auto u32 = [](const unsigned char *p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; };
    auto tail = read(size-std::min<uint64_t>(size,65557), static_cast<size_t>(std::min<uint64_t>(size,65557)));
    if (tail.size() < 22) return "安装包 ZIP 尾部无效";
    size_t end = tail.size();
    for (size_t i=tail.size()-22;; --i) {
        if (u32(tail.data()+i)==0x06054b50 && i+22+u16(tail.data()+i+20)==tail.size()) { end=i; break; }
        if (!i) break;
    }
    if (end==tail.size()) return "安装包不是完整 ZIP";
    const auto *e=tail.data()+end;
    const auto entries=u16(e+10), central_size=u32(e+12), central_offset=u32(e+16);
    if (u16(e+4) || u16(e+6) || u16(e+8)!=entries || entries>4096 ||
        central_size>2*1024*1024 || uint64_t(central_offset)+central_size != size-tail.size()+end)
        return "不支持的 ZIP 目录";
    auto central=read(central_offset,central_size);
    size_t at=0;
    std::string properties;
    unsigned found=0;
    for (uint32_t index=0; index<entries; ++index) {
        if (at>central.size() || central.size()-at<46 || u32(central.data()+at)!=0x02014b50) return "ZIP 目录损坏";
        const auto *c=central.data()+at;
        const size_t name_size=u16(c+28), extra=u16(c+30), comment=u16(c+32);
        const size_t record=46+name_size+extra+comment;
        if (record>central.size()-at) return "ZIP 文件名损坏";
        const std::string name(reinterpret_cast<const char *>(c+46),name_size);
        at+=record;
        if (name!="module.prop") continue;
        if (++found!=1 || u16(c+8)&1 || u16(c+34)) return "安装包模块信息重复或加密";
        const auto method=u16(c+10), compressed=u32(c+20), unpacked=u32(c+24), offset=u32(c+42);
        if (!unpacked || unpacked>16384 || compressed>32768 || (method!=0 && method!=8)) return "安装包模块信息过大或格式无效";
        auto local=read(offset,30);
        if (local.size()!=30 || u32(local.data())!=0x04034b50 || u16(local.data()+8)!=method || u16(local.data()+6)&1) return "ZIP 本地目录损坏";
        const uint64_t start=uint64_t(offset)+30+u16(local.data()+26)+u16(local.data()+28);
        if (start+compressed>central_offset) return "ZIP 元数据越界";
        auto local_name=read(uint64_t(offset)+30,u16(local.data()+26));
        if (std::string(local_name.begin(),local_name.end())!=name) return "ZIP 模块信息名称不一致";
        auto bytes=read(start,compressed);
        if (bytes.size()!=compressed) return "ZIP 模块信息读取失败";
        properties.resize(unpacked);
        if (method==0) {
            if (compressed!=unpacked) return "ZIP 元数据长度不一致";
            properties.assign(bytes.begin(),bytes.end());
        } else {
            z_stream stream{};
            stream.next_in=bytes.data(); stream.avail_in=static_cast<uInt>(bytes.size());
            stream.next_out=reinterpret_cast<Bytef *>(properties.data()); stream.avail_out=unpacked;
            if (inflateInit2(&stream,-MAX_WBITS)!=Z_OK) return "无法初始化 ZIP 解压";
            int result=inflate(&stream,Z_FINISH);
            const bool valid=result==Z_STREAM_END && stream.total_out==unpacked && stream.total_in==compressed;
            inflateEnd(&stream);
            if (!valid) return "ZIP 模块信息解压失败";
        }
        if (crc32(0,reinterpret_cast<const Bytef *>(properties.data()),unpacked)!=u32(c+16)) return "ZIP 模块信息 CRC 不一致";
    }
    if (at!=central.size() || found!=1 || properties.find('\0')!=std::string::npos) return "安装包缺少唯一模块信息";
    std::map<std::string,std::string> values;
    size_t begin=0;
    while (begin<properties.size()) {
        size_t next=properties.find('\n',begin);
        if (next==std::string::npos) next=properties.size();
        auto line=properties.substr(begin,next-begin);
        if (!line.empty() && line.back()=='\r') line.pop_back();
        auto separator=line.find('=');
        if (separator!=std::string::npos && !values.emplace(line.substr(0,separator),line.substr(separator+1)).second) return "模块信息含重复字段";
        begin=next+1;
    }
    int code=0;
    if (values["id"]!="AD_Killer_By_NFQUEUE" || values["version"]!="v"+version ||
        values["releaseChannel"]!=channel || !parse_integer_arg(values["versionCode"].c_str(),code) || code<=installed_code)
        return "安装包模块、版本、渠道或构建序号不符；请重新下载正确的发布包";
    return "";
}
