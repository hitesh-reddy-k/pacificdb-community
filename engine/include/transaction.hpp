#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

enum class TxState {
    ACTIVE,
    COMMITTED,
    ABORTED
};

struct Transaction {
    std::string id;
    TxState state = TxState::ACTIVE;
    std::vector<nlohmann::json> walBuffer;
};
