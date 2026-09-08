#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>

using json = nlohmann::json;

/**
 * VersionedRow - A single row with MVCC version tracking
 *
 * Each row has:
 * - createdTxn: transaction that inserted this version
 * - deletedTxn: transaction that deleted this version (0 = not deleted)
 * - data: the actual JSON document
 *
 * For updates: old version marked deleted, new version inserted with new createdTxn
 */
struct VersionedRow {
    uint64_t createdTxn;     // Transaction that created this version
    uint64_t deletedTxn;     // Transaction that deleted this version (0 = live)
    std::string rowId;       // Document ID
    json data;               // Document content

    VersionedRow() : createdTxn(0), deletedTxn(0) {}

    VersionedRow(uint64_t created, const std::string& id, const json& d)
        : createdTxn(created), deletedTxn(0), rowId(id), data(d) {}

    /**
     * Check if this version is visible to a transaction
     * Visibility: created_txn <= txid AND (deleted_txn == 0 OR deleted_txn > txid)
     */
    bool isVisibleTo(uint64_t txid) const {
        if (createdTxn > txid) return false;  // Not yet created for this transaction
        if (deletedTxn != 0 && deletedTxn <= txid) return false;  // Was deleted before/at this transaction
        return true;
    }

    /**
     * Mark this version as deleted by a transaction
     */
    void markDeleted(uint64_t txid) {
        deletedTxn = txid;
    }

    /**
     * Check if version is completely dead (no active transaction can see it)
     */
    bool isDeadForAllTxns(uint64_t oldestActiveTxn) const {
        return deletedTxn != 0 && deletedTxn < oldestActiveTxn;
    }

    /**
     * Serialize to JSON for storage
     */
    json toJson() const {
        json j;
        j["createdTxn"] = createdTxn;
        j["deletedTxn"] = deletedTxn;
        j["rowId"] = rowId;
        j["data"] = data;
        return j;
    }

    /**
     * Deserialize from JSON
     */
    static VersionedRow fromJson(const json& j) {
        VersionedRow row;
        row.createdTxn = j.at("createdTxn");
        row.deletedTxn = j.at("deletedTxn");
        row.rowId = j.at("rowId");
        row.data = j.at("data");
        return row;
    }
};
