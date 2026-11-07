#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static int child(const std::string& phase, const std::filesystem::path& data) {
    DatabaseEngine::init(data.string());
    if (phase == "prepare") {
        DatabaseEngine::ensureUserRoot("alice");
        DatabaseEngine::createDatabase("alice", "app", "document");
        DatabaseEngine::createCollection("alice", "app", "records");
        LSM::put("alice", "app", "records", {{"id","p"},{"version",1},{"group","old"},{"_visibility_state","COMMITTED_VISIBLE"}});
        LSM::put("alice", "app", "records", {{"id","gone"},{"version",1},{"group","old"}});
        LSM::flush("alice", "app", "records");
        LSM::put("alice", "app", "records", {{"id","p"},{"version",2},{"group","new"},{"_visibility_state","PENDING"}});
        LSM::put("alice", "app", "records", {{"id","gone"},{"version",2},{"_deleted",true}});
        LSM::flush("alice", "app", "records");
        const auto index=LSM::createSecondaryIndex("alice","app","records",{{"name","group_idx"},{"field","group"}});
        if (!index.value("complete",false)) return 4;
        // Leave acknowledged durable WAL data outside the memtable checkpoint.
        LSM::put("alice", "app", "records", {{"id","trigger"},{"version",3},{"group","old"},{"_visibility_state","COMMITTED_VISIBLE"}});
        std::_Exit(0);
    }
    const auto rows=LSM::findByField("alice","app","records","group","old",100);
    std::set<std::string> ids; for(const auto& row:rows) ids.insert(row.at("id").get<std::string>());
    const auto visible=LSM::getAll("alice","app","records");
    const bool correct=ids==std::set<std::string>{"p","trigger"} && visible.size()==2;
    std::cout<<json{{"indexed_ids",ids},{"visible_rows",visible.size()},{"recovery",LSM::indexRecoveryStatus()}}.dump()<<'\n';
    DatabaseEngine::stopNativeVectorIndexes(); LSM::stopBackgroundTasks(); WAL::shutdown();
    return correct?0:2;
}
static int runChild(const std::filesystem::path& self,const char* phase,const std::filesystem::path& data) {
#ifdef _WIN32
    const std::wstring executable=self.wstring(), mode=std::filesystem::path(phase).wstring(), directory=data.wstring();
    const wchar_t* arguments[]={executable.c_str(),mode.c_str(),directory.c_str(),nullptr};
    return static_cast<int>(_wspawnv(_P_WAIT,executable.c_str(),arguments));
#else
    const auto pid=fork(); if(pid<0) return 127;
    if(pid==0){execl(self.c_str(),self.c_str(),phase,data.c_str(),static_cast<char*>(nullptr));std::_Exit(127);}
    int status=0; if(waitpid(pid,&status,0)!=pid || !WIFEXITED(status)) return 127;
    return WEXITSTATUS(status);
#endif
}
int main(int argc,char**argv) {
#ifdef _WIN32
    _putenv_s("WAL_ENABLED","true");_putenv_s("WAL_FSYNC_ENABLED","true");_putenv_s("WAL_FLUSH_ON_COMMIT","true");
#else
    setenv("WAL_ENABLED","true",1);setenv("WAL_FSYNC_ENABLED","true",1);setenv("WAL_FLUSH_ON_COMMIT","true",1);
#endif
    try {
        if(argc==3)return child(argv[1],argv[2]);
        OwnedTestRoot root("index-visibility-recovery");
        const auto self=std::filesystem::absolute(argv[0]);
        for(const char* phase:{"prepare","verify","verify"})
            if(runChild(self,phase,root.dataRoot())!=0)throw std::runtime_error("index/read visibility differs after durable WAL recovery");
        std::cout<<"INDEX_VISIBILITY_RECOVERY_PASS\n";
    } catch(const std::exception& e){std::cerr<<"INDEX_VISIBILITY_RECOVERY_FAIL: "<<e.what()<<'\n';return 1;}
}
