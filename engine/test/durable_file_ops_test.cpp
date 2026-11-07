#include "durable_file_ops.hpp"
#include "owned_test_root.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
int main(){
 try{
  OwnedTestRoot root("durable-files");const auto dir=root.dataRoot();
  require(!pacificdb::durable_file::syncFile(dir/"missing"),"missing file sync falsely succeeded");
  require(!pacificdb::durable_file::syncDirectory(dir/"missing"),"missing directory sync falsely succeeded");
  const auto target=dir/"target";const auto source=dir/fs::u8path("source-\xc3\xa9");
  {std::ofstream out(target,std::ios::binary);out<<"old";}{std::ofstream out(source,std::ios::binary);out<<"new bytes";}
  require(pacificdb::durable_file::syncFile(source),"file flush failed");
  std::error_code error;require(pacificdb::durable_file::rename(source,target,error)&&!error,"atomic existing-file replacement failed");
  require(read(target)=="new bytes"&&!fs::exists(source),"replacement changed bytes or retained source");
  require(pacificdb::durable_file::syncDirectory(dir),"publication directory barrier failed");
  const auto blocked=dir/"nonempty";fs::create_directory(blocked);{std::ofstream out(blocked/"keep");out<<"keep";}
  require(!pacificdb::durable_file::rename(target,blocked,error)&&error,"invalid replacement was acknowledged");
  require(read(target)=="new bytes"&&read(blocked/"keep")=="keep","failed rename destroyed an existing artifact");
  require(pacificdb::durable_file::rename(blocked,dir/"moved",error)&&!error,"directory publication failed");
  require(read(dir/"moved/keep")=="keep"&&!fs::exists(blocked),"directory move lost child bytes");
  std::cout<<"DURABLE_FILE_OPS_PASS\n";
 }catch(const std::exception& e){std::cerr<<"DURABLE_FILE_OPS_FAIL: "<<e.what()<<'\n';return 1;}
}
