#pragma once
#include <nlohmann/json.hpp>
#include <lz4.h>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace pacificdb {
// Derived cache only. MessagePack preserves authoritative numeric precision;
// LZ4 is lossless. Neither this envelope nor graph files change WAL/SST formats.
inline constexpr size_t nativeVectorRecordLimit = 16ULL*1024*1024;
inline std::string packNativeVectorRecord(const nlohmann::json& document){
    const auto raw=nlohmann::json::to_msgpack(document);
    if(raw.empty()||raw.size()>nativeVectorRecordLimit)throw std::runtime_error("vector_index_cached_record_byte_limit");
    std::string compressed(LZ4_compressBound(static_cast<int>(raw.size())),'\0');
    const int size=LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),compressed.data(),raw.size(),compressed.size());
    const bool use=size>0&&size<static_cast<int>(raw.size());
    std::string out="PVR1";out.push_back(use?1:0);
    for(unsigned i=0;i<4;++i)out.push_back(static_cast<char>(raw.size()>>(8*i)));
    if(use)out.append(compressed.data(),size);else out.append(reinterpret_cast<const char*>(raw.data()),raw.size());
    return out;
}
inline std::string unpackNativeVectorRecord(const std::string& packed){
    if(packed.size()<9||packed.compare(0,4,"PVR1")!=0)throw std::runtime_error("vector_index_cached_record_corrupt");
    uint32_t length=0;for(unsigned i=0;i<4;++i)length|=uint32_t(static_cast<unsigned char>(packed[5+i]))<<(8*i);
    if(!length||length>nativeVectorRecordLimit||packed.size()-9>nativeVectorRecordLimit)throw std::runtime_error("vector_index_cached_record_corrupt");
    std::string out(length,'\0');
    if(packed[4]==1){if(LZ4_decompress_safe(packed.data()+9,out.data(),packed.size()-9,length)!=static_cast<int>(length))throw std::runtime_error("vector_index_cached_record_corrupt");}
    else if(packed[4]==0&&length==packed.size()-9)out.assign(packed.data()+9,length);
    else throw std::runtime_error("vector_index_cached_record_corrupt");
    return out;
}
}
