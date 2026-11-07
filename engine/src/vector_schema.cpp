#include "vector_schema.hpp"
#include "storage_path.hpp"
#include "test_failpoint.hpp"
#include "id_generator.hpp"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace pacificdb {
namespace {
void keys(const json& value,std::initializer_list<const char*> allowed,const char* what) {
    if(!value.is_object())throw std::invalid_argument(std::string("vector_schema ")+what+" must be an object");
    std::set<std::string> names(allowed.begin(),allowed.end());
    for(auto it=value.begin();it!=value.end();++it)if(!names.count(it.key()))
        throw std::invalid_argument("vector_schema unknown field: "+it.key().substr(0,128));
}
json space(const json& input) {
    keys(input,{"kind","dimensions","metric","required","precision"},"space");
    const auto kind=input.value("kind",std::string("dense"));
    if(kind!="dense"&&kind!="sparse")throw std::invalid_argument("vector_schema unsupported kind");
    if(!input.contains("dimensions"))throw std::invalid_argument("vector_schema dimensions required");
    const auto dimensions=vectors::unsignedInteger(input.at("dimensions"),kind=="dense"?1000000:1000000000);
    if(!dimensions)throw std::invalid_argument("vector_schema dimensions must be positive");
    auto metric=input.value("metric",std::string("cosine"));
    if(metric=="euclidean")metric="l2";if(metric=="dot_product")metric="dot";
    if(metric!="cosine"&&metric!="l2"&&metric!="dot")throw std::invalid_argument("vector_schema unsupported metric");
    if(input.contains("required")&&!input.at("required").is_boolean())throw std::invalid_argument("vector_schema required must be boolean");
    if(input.value("precision",std::string("float64"))!="float64")throw std::invalid_argument("vector_schema precision must be float64");
    return {{"kind",kind},{"dimensions",dimensions},{"metric",metric},{"precision","float64"},{"required",input.value("required",false)}};
}
const json* definition(const json& schema,const std::string& name) {
    if(name.empty())return schema.contains("default")?&schema.at("default"):nullptr;
    const auto& named=schema.at("named");auto it=named.find(name);return it==named.end()?nullptr:&*it;
}
void shape(const json& spec,const vectors::Value& value) {
    if(value.dimensions!=spec.at("dimensions")||value.sparse!=(spec.at("kind")=="sparse"))
        throw std::invalid_argument("vector_schema space kind/dimension mismatch");
}
using Schemas=std::map<std::string,std::shared_ptr<const json>>;
struct State {std::string root;Schemas schemas;};
std::shared_ptr<const State> state=std::make_shared<State>();
std::mutex writer;
std::atomic<bool> anySchemas{false}, previewLeases{false};
std::string normalized(const std::string& root){auto path=std::filesystem::path(root).lexically_normal();if(path.filename().empty()&&path!=path.root_path())path=path.parent_path();return path.string();}
std::string identity(const std::string& user,const std::string& db,const std::string& coll){return json::array({user,db,coll}).dump();}
}
bool VectorSchema::metadataUsesSchemas(const json& metadata){
    if(!metadata.is_object())return false;
    const auto collections=metadata.find("collections");if(collections==metadata.end()||!collections->is_array())return false;
    for(const auto& entry:*collections)if(entry.is_object()&&entry.contains("vector_schema"))return true;
    return false;
}
json VectorSchema::canonical(const json& input) try {
    keys(input,{"format_version","default","named","allow_unconfigured"},"definition");
    if(input.contains("format_version")&&(!input.at("format_version").is_number_integer()||input.at("format_version")!=1))throw std::invalid_argument("vector_schema unsupported format_version");
    if(input.contains("allow_unconfigured")&&!input.at("allow_unconfigured").is_boolean())throw std::invalid_argument("vector_schema allow_unconfigured must be boolean");
    json result={{"format_version",1},{"allow_unconfigured",input.value("allow_unconfigured",false)},{"named",json::object()}};
    if(input.contains("default"))result["default"]=space(input.at("default"));
    if(input.contains("named")){
        const auto& named=input.at("named");if(!named.is_object()||named.size()>64)throw std::invalid_argument("vector_schema named must have at most 64 spaces");
        for(auto it=named.begin();it!=named.end();++it){vectors::validateName(it.key());result["named"][it.key()]=space(it.value());}
    }
    if(!result.contains("default")&&result.at("named").empty())throw std::invalid_argument("vector_schema requires a vector space");
    return result;
} catch(const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
} catch(const json::exception& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
}
void VectorSchema::record(const json& schema,const json& document) try {
    if(!document.is_object())throw std::invalid_argument("vector_schema record must be an object");
    // Engine tombstones are validated at the trusted LSM entry point separately.
    size_t numbers=0;
    auto validate=[&](const std::string& name,const json& encoded){
        const auto spec=definition(schema,name);const auto value=vectors::decode(encoded);
        if(value.values.size()>1000000-numbers)throw std::invalid_argument("vector_schema record numeric limit exceeded");
        numbers+=value.values.size();
        if(spec)shape(*spec,value);
        else if(!schema.at("allow_unconfigured").get<bool>())throw std::invalid_argument("vector_schema unconfigured vector space");
    };
    if(document.contains("vector"))validate("",document.at("vector"));
    const auto found=document.find("vectors");
    if(found!=document.end()){
        if(!found->is_object()||found->size()>64)throw std::invalid_argument("vector_schema vectors must be an object with at most 64 spaces");
        for(auto it=found->begin();it!=found->end();++it){vectors::validateName(it.key());validate(it.key(),it.value());}
    }
    if(schema.contains("default")&&schema.at("default").at("required")==true&&!document.contains("vector"))
        throw std::invalid_argument("vector_schema required default vector missing");
    for(auto it=schema.at("named").begin();it!=schema.at("named").end();++it)
        if(it.value().at("required")==true&&(found==document.end()||!found->contains(it.key())))
            throw std::invalid_argument("vector_schema required named vector missing: "+it.key());
} catch(const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
} catch(const json::exception& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
}
void VectorSchema::query(const json& schema,const std::string& name,const vectors::Value& value,const std::string& rawMetric)try {
    const auto spec=definition(schema,name);
    if(!spec){if(!schema.at("allow_unconfigured").get<bool>())throw std::invalid_argument("vector_schema unconfigured query space");return;}
    shape(*spec,value);auto metric=rawMetric;if(metric=="euclidean")metric="l2";if(metric=="dot_product")metric="dot";
    if(metric!=spec->at("metric"))throw std::invalid_argument("vector_schema query metric mismatch");
} catch(const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
} catch(const json::exception& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
}
void VectorSchema::index(const json& schema,const json& spec)try {
    const auto name=spec.value("vector_name",std::string());const auto configured=definition(schema,name);
    if(!configured){if(spec.value("maintenance",std::string())=="incremental"||!schema.at("allow_unconfigured").get<bool>())throw std::invalid_argument("vector_schema unconfigured index space");return;}
    if(configured->at("kind")!="dense"||configured->at("dimensions")!=spec.at("dimensions")||configured->at("metric")!=spec.at("metric"))
        throw std::invalid_argument("vector_schema index space mismatch");
} catch(const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
} catch(const json::exception& error) {
    throw std::invalid_argument(std::string("vector_schema: ")+error.what());
}
std::shared_ptr<const json> VectorSchemaRegistry::get(const std::string& root,const std::string& user,const std::string& db,const std::string& coll){
    if(!anySchemas.load(std::memory_order_acquire))return {};
    const auto current=std::atomic_load(&state);if(current->schemas.empty())return {};
    if(current->root!=normalized(root))throw std::runtime_error("vector_schema root mismatch");
    const auto it=current->schemas.find(identity(user,db,coll));return it==current->schemas.end()?nullptr:it->second;
}
void VectorSchemaRegistry::install(const std::string& root,const std::string& user,const std::string& db,const std::string& coll,const json& schema){
    auto value=std::make_shared<const json>(VectorSchema::canonical(schema));std::lock_guard<std::mutex> lock(writer);
    const auto old=std::atomic_load(&state);auto next=std::make_shared<State>();next->root=normalized(root);
    if(old->root==next->root)next->schemas=old->schemas;
    const auto id=identity(user,db,coll);const auto it=next->schemas.find(id);
    if(it!=next->schemas.end()&&*it->second!=*value)throw std::runtime_error("vector_schema immutable collection schema mismatch");
    if(it==next->schemas.end()&&next->schemas.size()>=4096)throw std::runtime_error("vector_schema collection limit exceeded");
    next->schemas[id]=std::move(value);std::atomic_store(&state,std::shared_ptr<const State>(std::move(next)));
    anySchemas.store(true,std::memory_order_release);
}
void VectorSchemaRegistry::erase(const std::string& root,const std::string& user,const std::string& db,const std::string& coll){
    std::lock_guard<std::mutex> lock(writer);const auto old=std::atomic_load(&state);
    if(old->root!=normalized(root)||old->schemas.empty())return;
    auto next=std::make_shared<State>(*old);next->schemas.erase(identity(user,db,coll));const bool present=!next->schemas.empty();std::atomic_store(&state,std::shared_ptr<const State>(std::move(next)));anySchemas.store(present,std::memory_order_release);
}
void VectorSchemaRegistry::validateRecord(const std::string& root,const std::string& user,const std::string& db,const std::string& coll,const json& document){
    if(const auto schema=get(root,user,db,coll))VectorSchema::record(*schema,document);
}
bool VectorSchemaRegistry::leasesEnabled(){return previewLeases.load(std::memory_order_acquire)||anySchemas.load(std::memory_order_acquire);}
void VectorSchemaRegistry::load(const std::string& root){
    const char* flag=std::getenv("PACIFICDB_VECTOR_PREVIEW_ENABLED");
    previewLeases.store(flag&&(std::string(flag)=="1"||std::string(flag)=="true"),std::memory_order_release);
    namespace fs=std::filesystem;auto next=std::make_shared<State>();next->root=normalized(root);
    if(fs::exists(root))for(auto it=fs::recursive_directory_iterator(root);it!=fs::recursive_directory_iterator();++it){
        if(it.depth()>=2)it.disable_recursion_pending();
        if(it.depth()!=2||it->path().filename()!="db.meta")continue;
        const auto path=validateContainedStoragePath(fs::path(root),it->path());
        // Low-level bundle APIs can carry opaque db.meta artifacts without a
        // collection data directory (including bounded streaming fixtures). Do
        // not turn schema discovery into validation of unrelated opaque files.
        // An active engine database always creates its data directory first.
        std::ifstream in(path);if(!in)throw std::runtime_error("vector_schema cannot read collection metadata");
        in>>std::ws;
        if(in.peek()!='{' && !fs::exists(path.parent_path()/"data"))continue;
        const auto meta=json::parse(in);if(!meta.is_object())throw std::runtime_error("vector_schema invalid collection metadata");
        const auto db=path.parent_path().filename().string(),user=path.parent_path().parent_path().filename().string();
        for(const auto& entry:meta.value("collections",json::array())){
            if(!entry.is_object()||!entry.contains("vector_schema"))continue;
            const auto coll=entry.at("name").get<std::string>();validateStorageIdentifier(user,"userId");validateStorageIdentifier(db,"databaseName");validateCollectionStorageIdentifier(coll);
            if(next->schemas.size()>=4096)throw std::runtime_error("vector_schema collection limit exceeded");
            const auto id=identity(user,db,coll);auto value=std::make_shared<const json>(VectorSchema::canonical(entry.at("vector_schema")));
            if(!next->schemas.emplace(id,std::move(value)).second)throw std::runtime_error("vector_schema duplicate collection metadata");
        }
    }
    std::lock_guard<std::mutex> lock(writer);const bool present=!next->schemas.empty();std::atomic_store(&state,std::shared_ptr<const State>(std::move(next)));anySchemas.store(present,std::memory_order_release);
}
void VectorSchemaRegistry::persistMetadata(const std::string& root,const std::filesystem::path& target,const json& metadata,const std::filesystem::path& collectionMarker){
    namespace fs=std::filesystem;const auto path=validateContainedStoragePath(fs::path(root),target);
    const auto temp=validateContainedStoragePath(fs::path(root),fs::path(path.string()+".schema-"+IDGenerator::generateObjectId()+".tmp"));
    const auto bytes=metadata.dump(2);
    const auto marker=collectionMarker.empty()?fs::path():validateContainedStoragePath(fs::path(root),collectionMarker);
    // Schema failpoints use publication sequence 1, not a Raft log position.
    test::hitFailpoint("FP_VECTOR_SCHEMA_BEFORE_MARKER_SYNC",1);
#ifdef _WIN32
    if(!marker.empty()){
    HANDLE markerFile=CreateFileW(marker.wstring().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(markerFile==INVALID_HANDLE_VALUE)throw std::runtime_error("vector_schema collection marker open failed");
    const bool markerSynced=FlushFileBuffers(markerFile);CloseHandle(markerFile);
    if(!markerSynced)throw std::runtime_error("vector_schema collection marker sync failed");
    }
    HANDLE file=CreateFileW(temp.wstring().c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("vector_schema metadata create failed");
    DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);CloseHandle(file);
    test::hitFailpoint("FP_VECTOR_SCHEMA_BEFORE_PUBLICATION",1);
    if(!ok||!MoveFileExW(temp.wstring().c_str(),path.wstring().c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("vector_schema durable metadata write failed");
#else
    if(!marker.empty()){
    const int markerFile=::open(marker.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(markerFile<0)throw std::system_error(errno,std::generic_category(),"vector_schema collection marker open");
    const int markerResult=::fsync(markerFile);const auto markerError=errno;::close(markerFile);
    if(markerResult)throw std::system_error(markerError,std::generic_category(),"vector_schema collection marker sync");
    const int markerDir=::open(marker.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(markerDir<0)throw std::system_error(errno,std::generic_category(),"vector_schema marker directory open");
    const int markerDirResult=::fsync(markerDir);const auto markerDirError=errno;::close(markerDir);
    if(markerDirResult)throw std::system_error(markerDirError,std::generic_category(),"vector_schema marker directory sync");
    }
    const int file=::open(temp.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if(file<0)throw std::system_error(errno,std::generic_category(),"vector_schema metadata create");
    size_t offset=0;bool ok=true;
    while(offset<bytes.size()){const auto n=::write(file,bytes.data()+offset,bytes.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}offset+=static_cast<size_t>(n);}
    if(ok)ok=::fsync(file)==0;const auto saved=errno;::close(file);
    if(!ok){std::error_code ignored;fs::remove(temp,ignored);throw std::system_error(saved,std::generic_category(),"vector_schema metadata sync");}
    test::hitFailpoint("FP_VECTOR_SCHEMA_BEFORE_PUBLICATION",1);
    std::error_code ec;fs::rename(temp,path,ec);if(ec)throw std::system_error(ec,"vector_schema metadata publication");
    // Make both the file replacement and its database/tenant directory links
    // durable. Existing databases pay this only when they contain a schema.
    const auto boundary=fs::path(root).lexically_normal();
    for(auto directory=path.parent_path();;directory=directory.parent_path()){
        const int dir=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(dir<0)throw std::system_error(errno,std::generic_category(),"vector_schema metadata directory open");
        const int rc=::fsync(dir);const auto error=errno;::close(dir);
        if(rc)throw std::system_error(error,std::generic_category(),"vector_schema metadata directory sync");
        if(directory==boundary||directory==directory.root_path())break;
    }
#endif
    test::hitFailpoint("FP_VECTOR_SCHEMA_AFTER_PUBLICATION",1);
}
}
