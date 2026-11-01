#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>

namespace pacificdb {
class VectorMemoryLimit : public std::bad_alloc {
public:
    const char* what() const noexcept override { return "vector_index_memory_budget_exceeded"; }
};
// Requested allocation bytes, not an RSS estimate. Share one budget across
// generations to account for readers retaining an old graph during publication.
class VectorAllocationBudget {
public:
    explicit VectorAllocationBudget(size_t limit) : limit_(limit) {}
    bool reserve(size_t bytes) noexcept {
        auto used = used_.load();
        while (bytes <= limit_ - used) {
            if (used_.compare_exchange_weak(used, used + bytes)) {
                auto peak = peak_.load();
                while (peak < used + bytes && !peak_.compare_exchange_weak(peak, used + bytes)) {}
                return true;
            }
        }
        return false;
    }
    void release(size_t bytes) noexcept { used_.fetch_sub(bytes); }
    size_t used() const noexcept { return used_.load(); }
    size_t peak() const noexcept { return peak_.load(); }
    size_t limit() const noexcept { return limit_; }
private:
    const size_t limit_;
    std::atomic<size_t> used_{0}, peak_{0};
};

class VectorBudgetResource final : public std::pmr::memory_resource {
public:
    VectorBudgetResource(size_t limit, std::shared_ptr<VectorAllocationBudget> shared)
        : local_(limit), shared_(std::move(shared)) {}
    const VectorAllocationBudget& accounting() const noexcept { return local_; }
private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        const size_t charged = std::max(bytes, size_t{1});
        if (!local_.reserve(charged)) throw VectorMemoryLimit();
        if (shared_ && !shared_->reserve(charged)) {
            local_.release(charged); throw VectorMemoryLimit();
        }
        try { return std::pmr::new_delete_resource()->allocate(bytes, alignment); }
        catch (...) {
            if (shared_) shared_->release(charged);
            local_.release(charged); throw;
        }
    }
    void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        const size_t charged = std::max(bytes, size_t{1});
        if (shared_) shared_->release(charged);
        local_.release(charged);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    VectorAllocationBudget local_;
    std::shared_ptr<VectorAllocationBudget> shared_;
};
}
