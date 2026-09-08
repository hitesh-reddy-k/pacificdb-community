#pragma once
#include <nlohmann/json.hpp>

using json = nlohmann::json;

inline void applyUpdateOps(json& doc, const json& update) {

    /* ---------- $set ---------- */
    if (update.contains("$set")) {
        for (auto& [k, v] : update["$set"].items()) {
            doc[k] = v;
        }
    }

    /* ---------- $unset ---------- */
    if (update.contains("$unset")) {
        for (auto& k : update["$unset"]) {
            doc.erase(k.get<std::string>());
        }
    }

    /* ---------- $inc ---------- */
    if (update.contains("$inc")) {
        for (auto& [k, v] : update["$inc"].items()) {
            if (!doc.contains(k) || !doc[k].is_number()) {
                doc[k] = 0;
            }
            doc[k] = doc[k].get<long long>() + v.get<long long>();
        }
    }
}
