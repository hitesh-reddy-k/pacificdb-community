#include "native_vector_checkpoint.hpp"
#include "test_failpoint.hpp"
#include <openssl/evp.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace pacificdb { namespace {
namespace fs=std::filesystem;
using Digest=std::array<unsigned char,32>;
class Hash {
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),EVP_MD_CTX_free};
public:
    Hash(){if(!context||EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("checkpoint SHA256 initialization failed");}
    void add(const void* p,size_t n){if(EVP_DigestUpdate(context.get(),p,n)!=1)throw std::runtime_error("checkpoint SHA256 update failed");}
    void number(uint64_t n){unsigned char b[8];for(unsigned i=0;i<8;++i)b[i]=n>>(8*i);add(b,8);}
    void text(std::string_view s){number(s.size());add(s.data(),s.size());}
    Digest finish(){Digest d;unsigned n=0;if(EVP_DigestFinal_ex(context.get(),d.data(),&n)!=1||n!=d.size())throw std::runtime_error("checkpoint SHA256 finalize failed");return d;}
};
Digest hashText(std::string_view s){Hash h;h.text(s);return h.finish();}
std::string hex(const Digest& d){std::ostringstream out;for(auto b:d)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return out.str();}
Digest source(const HNSWIndex& graph,const HNSWIndex::Check& check){
    Hash hash;hash.text("PacificDB derived HNSW cosine-normalization-v1, packed-record-v1");
    graph.visitLive([&](std::string_view id,const float* v,size_t count,std::string_view record){
        hash.text(id);hash.number(count);
        for(size_t i=0;i<count;++i){uint32_t bits;std::memcpy(&bits,v+i,4);hash.number(bits);}
        hash.text(record);
    },check);return hash.finish();
}
Digest bodyHash(std::istream& input,const HNSWIndex::Check& check){
    Hash h;std::array<char,65536> block;
    while(input){if(check)check();input.read(block.data(),block.size());h.add(block.data(),input.gcount());}
    if(!input.eof())throw std::runtime_error("checkpoint checksum read failed");return h.finish();
}
void regular(const fs::path& path){
    if(!fs::is_regular_file(fs::symlink_status(path)))throw std::runtime_error("checkpoint is not a regular file");
#ifndef _WIN32
    struct stat info{};if(lstat(path.c_str(),&info)||info.st_uid!=geteuid()||info.st_nlink!=1||(info.st_mode&0077))throw std::runtime_error("unsafe checkpoint file ownership/permissions");
#endif
}
void directory(const fs::path& path){
    std::error_code ec;const bool made=fs::create_directory(path,ec);
    if(ec||!fs::is_directory(fs::symlink_status(path)))throw std::runtime_error("unsafe checkpoint directory");
#ifndef _WIN32
    if(made&&chmod(path.c_str(),0700))throw std::runtime_error("checkpoint directory permission failure");
    struct stat info{};if(lstat(path.c_str(),&info)||info.st_uid!=geteuid()||(info.st_mode&0077))throw std::runtime_error("unsafe checkpoint directory ownership/permissions");
#endif
}
void sync(const fs::path& path,bool isDirectory=false){
#ifdef _WIN32
    if(isDirectory)return; // File flush + replace-with-write-through below.
    auto handle=CreateFileW(path.wstring().c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("checkpoint sync open failed");
    const bool ok=FlushFileBuffers(handle)!=0;CloseHandle(handle);if(!ok)throw std::runtime_error("checkpoint sync failed");
#else
    const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|(isDirectory?O_DIRECTORY:0));
    if(fd<0)throw std::runtime_error("checkpoint sync open failed");
    const int result=::fsync(fd);::close(fd);if(result)throw std::runtime_error("checkpoint sync failed");
#endif
}
class BoundedBuffer:public std::streambuf {
    std::streambuf* target;size_t bytes=0,maximum;
public:BoundedBuffer(std::streambuf* target,size_t maximum):target(target),maximum(maximum){}
protected:
    std::streamsize xsputn(const char* p,std::streamsize n)override{
        if(n<0||size_t(n)>maximum-bytes)throw std::runtime_error("vector_checkpoint_disk_byte_limit");
        const auto written=target->sputn(p,n);bytes+=written;return written;
    }
    int overflow(int ch)override{if(ch==traits_type::eof())return traits_type::not_eof(ch);const char c=ch;return xsputn(&c,1)==1?ch:traits_type::eof();}
    int sync()override{return target->pubsync();}
};
struct Store {
    fs::path directory,path;Digest binding;size_t maximum;
    bool restore(HNSWIndex& graph,const HNSWIndex::Check& check)const{
        if(!fs::exists(directory))return false;
        pacificdb::directory(directory);
        if(!fs::exists(fs::symlink_status(path)))return false;
        regular(path);const auto length=fs::file_size(path);
        if(length<104||length>maximum)throw std::runtime_error("invalid vector checkpoint size");
        std::ifstream input(path,std::ios::binary);std::array<char,8> magic{};Digest recordedSource{},recordedBinding{},recordedBody{};
        input.read(magic.data(),8);input.read(reinterpret_cast<char*>(recordedSource.data()),32);
        input.read(reinterpret_cast<char*>(recordedBinding.data()),32);input.read(reinterpret_cast<char*>(recordedBody.data()),32);
        if(!input||std::string(magic.data(),8)!="PDBANN01")throw std::runtime_error("unsupported vector checkpoint header");
        if(recordedBinding!=binding||recordedSource!=source(graph,check))return false;
        if(bodyHash(input,check)!=recordedBody)throw std::runtime_error("vector checkpoint checksum mismatch");
        input.clear();input.seekg(104);graph.readLinks(input,check);return true;
    }
    void save(const HNSWIndex& graph,const HNSWIndex::Check& check)const{
        // Disk quota/atomic replacement only; no database, Raft or graph write
        // mutex is held during these operations. Searches retain shared access.
        static std::mutex diskWriter;std::lock_guard<std::mutex> guard(diskWriter);
        directoryCreate();
        const auto temporary=fs::path(path.string()+".tmp");
        if(fs::exists(fs::symlink_status(temporary))){regular(temporary);fs::remove(temporary);}
        std::vector<fs::directory_entry> old;uintmax_t used=0;
        for(const auto& entry:fs::directory_iterator(directory)){
            if(entry.path().extension()==".tmp"){regular(entry.path());fs::remove(entry.path());continue;}
            if(!entry.is_regular_file()||entry.path().extension()!=".graph")continue;
            regular(entry.path());used+=entry.file_size();old.push_back(entry);
        }
        constexpr uintmax_t quota=2ULL*1024*1024*1024;
        std::sort(old.begin(),old.end(),[](const auto& a,const auto& b){return a.last_write_time()<b.last_write_time();});
        size_t remaining=old.size();
        for(const auto& entry:old){if(used<=quota-maximum&&remaining<=31)break;used-=entry.file_size();fs::remove(entry.path());--remaining;}
#ifndef _WIN32
        const int created=::open(temporary.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600);
        if(created<0)throw std::runtime_error("checkpoint temporary creation failed");::close(created);
#endif
        struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove(p,ec);}}cleanup{temporary};
        const auto sourceHash=source(graph,check);
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);BoundedBuffer buffer(file.rdbuf(),maximum);
        std::ostream output(&buffer);output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write("PDBANN01",8);output.write(reinterpret_cast<const char*>(sourceHash.data()),32);
        output.write(reinterpret_cast<const char*>(binding.data()),32);Digest placeholder{};output.write(reinterpret_cast<const char*>(placeholder.data()),32);
        test::hitFailpoint("FP_VECTOR_CHECKPOINT_BEFORE_BODY",1);
        graph.writeLinks(output,check);output.flush();file.close();
        std::ifstream body(temporary,std::ios::binary);body.seekg(104);const auto digest=bodyHash(body,check);body.close();
        std::fstream update(temporary,std::ios::binary|std::ios::in|std::ios::out);update.seekp(72);update.write(reinterpret_cast<const char*>(digest.data()),32);update.flush();
        if(!update)throw std::runtime_error("checkpoint checksum publication failed");update.close();sync(temporary);
        test::hitFailpoint("FP_VECTOR_CHECKPOINT_BEFORE_RENAME",1);if(check)check();
#ifdef _WIN32
        if(!MoveFileExW(temporary.wstring().c_str(),path.wstring().c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("checkpoint rename failed");
#else
        fs::rename(temporary,path);
#endif
        sync(directory,true);test::hitFailpoint("FP_VECTOR_CHECKPOINT_AFTER_RENAME",1);
    }
    void directoryCreate()const{pacificdb::directory(directory);}
};
} // namespace
VectorIndexGeneration::Checkpoints nativeVectorCheckpoints(const fs::path& root,const std::string& identity,const json& spec,size_t maximum){
    if(maximum<1024||maximum>1024ULL*1024*1024)throw std::invalid_argument("invalid checkpoint byte limit");
    const auto normalized=fs::weakly_canonical(root);
    auto store=std::make_shared<Store>();store->directory=normalized.parent_path()/(normalized.filename().string()+".vector-derived-v1");
    store->path=store->directory/(hex(hashText(identity))+".graph");store->binding=hashText(identity+"\n"+spec.dump());store->maximum=maximum;
    return {[store](HNSWIndex& graph,const HNSWIndex::Check& check){return store->restore(graph,check);},
            [store](const HNSWIndex& graph,const HNSWIndex::Check& check){store->save(graph,check);}};
}
}
