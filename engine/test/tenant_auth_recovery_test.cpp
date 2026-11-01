#include "owned_test_root.hpp"
#include "tenant_manager.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>

using pacificdb::tenant::TenantManager;
using pacificdb::tenant::TenantRole;
using nlohmann::json;

int main(int argc, char** argv) {
    const OwnedTestRoot root("tenant-auth-recovery");
    auto& manager = TenantManager::instance();
    manager.initialize(root.dataRoot().string());
    assert(manager.createTenant("acme", "Acme", "owner", "correct").value("success",false));
    const std::string mode = argc > 1 ? argv[1] : "lockout";
    if (mode == "access") {
        assert(manager.createTenantUser("acme", "reader", "correct", TenantRole::TENANT_READONLY, "owner").value("success",false));
        assert(manager.setTenantUserDatabaseAccess("acme", "reader", {"allowed"}, "owner").value("success",false));
        const auto login=manager.authenticateTenantUser("acme","reader","correct","127.0.0.1");
        assert(login.value("success",false));
        const auto token=login.at("token").get<std::string>();
        assert(manager.canAccessDatabase(token,"allowed"));
        assert(!manager.canAccessDatabase(token,"denied"));
    } else {
        for (int i=0;i<5;++i) assert(!manager.authenticateTenantUser("acme","owner","wrong","127.0.0.1").value("success",false));
        auto locked=manager.getTenantUser("acme","owner");
        assert(locked && locked->isLocked);
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        assert(locked->toJson().value("lockedUntil",0LL)>now);
        assert(!manager.authenticateTenantUser("acme","owner","correct","127.0.0.1").value("success",false));
        manager.loadTenantUsers("acme");
        assert(!manager.authenticateTenantUser("acme","owner","correct","127.0.0.1").value("success",false));
        const auto file=root.dataRoot()/"tenants/acme/users.json";
        json users;
        {std::ifstream in(file);in>>users;}
        for(auto& row:users["users"]) if(row.value("username","")=="owner")row["lockedUntil"]=now-1;
        {std::ofstream out(file);out<<users;}
        manager.loadTenantUsers("acme");
        assert(manager.authenticateTenantUser("acme","owner","correct","127.0.0.1").value("success",false));
        auto expired=manager.getTenantUser("acme","owner");
        assert(expired && !expired->isLocked && expired->failedLoginAttempts==0);
        for (int i=0;i<5;++i) assert(!manager.authenticateTenantUser("acme","owner","wrong","127.0.0.1").value("success",false));
        assert(manager.updateTenantUserPassword("acme","owner","reset","owner").value("success",false));
        assert(manager.authenticateTenantUser("acme","owner","reset","127.0.0.1").value("success",false));
        {std::ifstream in(file);in>>users;}
        for(auto& row:users["users"]) if(row.value("username","")=="owner"){row["isLocked"]=true;row["failedLoginAttempts"]=5;row.erase("lockedUntil");}
        {std::ofstream out(file);out<<users;}
        manager.loadTenantUsers("acme");
        assert(!manager.authenticateTenantUser("acme","owner","reset","127.0.0.1").value("success",false));
        assert(manager.updateTenantUserPassword("acme","owner","recovered","owner").value("success",false));
        assert(manager.authenticateTenantUser("acme","owner","recovered","127.0.0.1").value("success",false));
    }
    std::cout<<"TENANT_AUTH_RECOVERY_PASS "<<mode<<'\n';
}
