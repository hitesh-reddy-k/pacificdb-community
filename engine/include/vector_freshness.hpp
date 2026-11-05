#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>

namespace pacificdb {
// Process-local certificates for derived indexes. Hash collisions conservatively
// invalidate unrelated collections; they can never make an old certificate valid.
// No persistent data format, WAL or acknowledgement semantics depend on this state.
class VectorFreshness {
    struct Counter {
        std::atomic<uint64_t> active{0}, revision{0};
        std::atomic<bool> poisoned{false};
        void bump() noexcept {
            if (revision.fetch_add(1) == std::numeric_limits<uint64_t>::max())
                poisoned.store(true);
        }
    };
public:
    struct Stamp {
        uint64_t root, collection, visibility, configuration;
        Stamp(uint64_t r = 0, uint64_t c = 0, uint64_t v = 0)
            : root(r), collection(c), visibility(v), configuration(0) {}
        bool operator==(const Stamp& other) const noexcept {
            return root == other.root && collection == other.collection &&
                   visibility == other.visibility && configuration == other.configuration;
        }
        bool operator!=(const Stamp& other) const noexcept { return !(*this == other); }
        // Only a source with separately proven complete visibility may use this.
        // Ordinary certificates retain strict equality, including visibility.
        bool sameSourceAtOrAfter(const Stamp& previous) const noexcept {
            return root == previous.root && collection == previous.collection &&
                   configuration == previous.configuration && visibility >= previous.visibility;
        }
    };
    class Mutation {
    public:
        explicit Mutation(Counter& counter) noexcept : counter_(counter) {
            counter_.active.fetch_add(1); counter_.bump();
        }
        ~Mutation() { counter_.bump(); counter_.active.fetch_sub(1); }
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;
    private:
        Counter& counter_;
    };
    Mutation collectionMutation(const std::string& key) noexcept { return Mutation(slot(key)); }
    Mutation rootMutation() noexcept { return Mutation(root_); }
    std::optional<Stamp> capture(const std::string& key, uint64_t visibility) const noexcept {
        const auto& counter = slot(key);
        const auto root = root_.revision.load();
        const auto collection = counter.revision.load();
        if (root_.active.load() || counter.active.load() ||
            root_.poisoned.load() || counter.poisoned.load()) return {};
        if (root != root_.revision.load() || collection != counter.revision.load()) return {};
        return Stamp{root, collection, visibility};
    }
private:
    Counter& slot(const std::string& key) noexcept {
        return slots_[std::hash<std::string>{}(key) % slots_.size()];
    }
    const Counter& slot(const std::string& key) const noexcept {
        return slots_[std::hash<std::string>{}(key) % slots_.size()];
    }
    Counter root_;
    std::array<Counter, 4096> slots_;
};
}
