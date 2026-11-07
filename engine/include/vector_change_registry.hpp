#pragma once
#include "vector_change_journal.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
namespace pacificdb {
// Engine-local subscriptions only. Ordinary collections allocate no history.
// Derived bounded journals contain IDs, never authoritative vectors or payloads.
class VectorChangeRegistry {
public:
 static std::shared_ptr<VectorChangeJournal> subscribe(const std::string& root,const std::string& user,
     const std::string& database,const std::string& collection);
 static void observe(const std::string& root,const std::string& user,const std::string& database,
     const std::string& collection,const nlohmann::json& document) noexcept;
 static void invalidate(const std::string& root,const std::string& user,const std::string& database,
     const std::string& collection) noexcept;
 static void invalidateAll() noexcept;
};
}
