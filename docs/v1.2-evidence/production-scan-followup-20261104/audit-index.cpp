#include "storage_format_v2.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
 try {
  if(argc!=2)throw std::runtime_error("index directory required");
  const auto files=pacificdb::storage_v2::listIndexSegments(argv[1]);
  if(files.empty())throw std::runtime_error("no index segments");
  size_t rows=0;std::string previous;
  for(size_t f=0;f<files.size();++f){
   pacificdb::storage_v2::IndexSegment segment;std::string error;
   if(!pacificdb::storage_v2::readIndexSegment(files[f],segment,&error))throw std::runtime_error(error);
   if(segment.snapshot!=(f==0))throw std::runtime_error("unexpected snapshot sequence");
   for(const auto& m:segment.mutations){
    if(m.op!=pacificdb::storage_v2::IndexMutation::Op::Put||m.value!="vector")throw std::runtime_error("incorrect indexed field/value");
    const auto id=std::stoull(m.id);
    if(id<5||id>=1000000||std::to_string(id)!=m.id||(!previous.empty()&&previous>=m.id))throw std::runtime_error("missing/deleted/duplicate/out-of-order ID");
    previous=m.id;++rows;
   }
  }
  if(rows!=999995)throw std::runtime_error("incomplete authoritative ID coverage");
  std::cout<<"{\"status\":\"PASS\",\"entries\":"<<rows<<",\"segments\":"<<files.size()<<",\"expected_deleted_ids\":5}\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
