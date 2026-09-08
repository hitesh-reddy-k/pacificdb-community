#include "database_engine.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main() {
    const OwnedTestRoot ownedRoot("basic-engine");
    DatabaseEngine::init(ownedRoot.dataRoot().string());

    DatabaseEngine::insert("system", "testdb", "users", {{"id",1}, {"email","a@test.com"}, {"username","alpha"}});
    DatabaseEngine::insert("system", "testdb", "users", {{"id",2}, {"email","b@test.com"}, {"username","beta"}});

    auto res = DatabaseEngine::find("system","testdb","users", {{"$or", {{{"email","a@test.com"}}, {{"email","b@test.com"}}}}});
    std::cout << "[TEST] res.size() = " << res.size() << "\n";
    assert(res.size() == 2);

    bool updated = DatabaseEngine::updateOne("system","testdb","users", {{"id",2}}, {{"username","gamma"}});
    assert(updated);

    auto res3 = DatabaseEngine::find("system","testdb","users", {{"username","gamma"}});
    assert(res3.size() == 1);

    bool deleted = DatabaseEngine::deleteOne("system","testdb","users", {{"id",1}});
    assert(deleted);

    auto res4 = DatabaseEngine::find("system","testdb","users", {});
    std::cout << "[TEST] res4.size() = " << res4.size() << "\n";
    for (auto& d : res4) std::cout << d.dump() << "\n";
    assert(res4.size() == 1);

    std::cout << "✅ ALL TESTS PASSED\n";
    return 0;
}
