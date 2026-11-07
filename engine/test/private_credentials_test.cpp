#include "private_credentials.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

#ifdef _WIN32
static void setAcl(const std::filesystem::path& file, bool publicAccess, bool nullAcl=false, bool claimOwner=false) {
    HANDLE token = nullptr; assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    assert(GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes));
    const auto user = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
    alignas(DWORD) unsigned char world[SECURITY_MAX_SID_SIZE]; DWORD worldSize=sizeof(world);
    assert(CreateWellKnownSid(WinWorldSid,nullptr,world,&worldSize));
    EXPLICIT_ACCESSW entry{};entry.grfAccessPermissions=publicAccess?GENERIC_READ:GENERIC_ALL;
    entry.grfAccessMode=SET_ACCESS;entry.Trustee.TrusteeForm=TRUSTEE_IS_SID;
    entry.Trustee.ptstrName=static_cast<LPWSTR>(publicAccess?static_cast<void*>(world):user);
    PACL acl=nullptr;assert(SetEntriesInAclW(1,&entry,nullptr,&acl)==ERROR_SUCCESS);
    // Claim ownership once while the fresh fixture grants WRITE_OWNER.
    // Its later deliberately read-only ACL still lets the owner restore the
    // DACL, but cannot authorize an unnecessary WRITE_OWNER request.
    const auto result=SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()),SE_FILE_OBJECT,
        (claimOwner?OWNER_SECURITY_INFORMATION:0)|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
        claimOwner?user:nullptr,nullptr,nullAcl?nullptr:acl,nullptr);
    if(result!=ERROR_SUCCESS){
        std::cerr<<"FIXTURE_ACL_ERROR code="<<result<<" claim_owner="<<claimOwner
                 <<" public_access="<<publicAccess<<" null_acl="<<nullAcl<<std::endl;
    }
    assert(result==ERROR_SUCCESS);
    LocalFree(acl);CloseHandle(token);
}
#endif
int main() {
    OwnedTestRoot root("private-credentials");const auto file=root.dataRoot()/"accounts.json";
    {std::ofstream output(file);output<<"[{\"username\":\"fixture\"}]";}
#ifdef _WIN32
    setAcl(file,false,false,true);
#else
    assert(::chmod(file.c_str(),0600)==0);
#endif
    assert(pacificdb::readPrivateCredentials(file)=="[{\"username\":\"fixture\"}]");
    const auto rejects=[](const auto& path){bool refused=false;try{(void)pacificdb::readPrivateCredentials(path);}catch(const std::exception&){refused=true;}assert(refused);};
    rejects(root.dataRoot()/"missing");rejects(root.dataRoot());
#ifdef _WIN32
    setAcl(file,true);rejects(file);
    setAcl(file,false,true);rejects(file);
    setAcl(file,false);
#else
    for(const auto mode:{0644,0620,0601}){assert(::chmod(file.c_str(),mode)==0);rejects(file);}
    assert(::chmod(file.c_str(),0600)==0);
    const auto link=root.dataRoot()/"link";std::filesystem::create_symlink(file,link);rejects(link);
    const auto fifo=root.dataRoot()/"fifo";assert(::mkfifo(fifo.c_str(),0600)==0);rejects(fifo);
#endif
    {std::ofstream output(file);output<<std::string(1024*1024,'x');}
    assert(pacificdb::readPrivateCredentials(file).size()==1024*1024);
    {std::ofstream output(file,std::ios::app);output<<'x';}
    rejects(file);
    std::cout<<"PRIVATE_CREDENTIALS_OWNERSHIP_ACCESS_REGULAR_FILE_SIZE_FENCES_PASS\n";
}
