#pragma once

#include <nlohmann/json.hpp>

#include <ostream>
#include <string>

namespace pacificdb::observability {

struct StructuredEvent {
    std::string severity;
    std::string subsystem;
    std::string code;
    std::string operationId;
    std::string message;
    nlohmann::json fields = nlohmann::json::object();
};

std::string formatStructuredEvent(const StructuredEvent& event);
void emitStructuredEvent(std::ostream& output, const StructuredEvent& event);

}  // namespace pacificdb::observability
