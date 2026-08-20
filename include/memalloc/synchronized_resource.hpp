#pragma once

#include <cstddef>
#include <memory_resource>
#include <mutex>
#include <stdexcept>

namespace memalloc {

class synchronized_resource : public std::pmr::memory_resource {
public:
    explicit synchronized_resource(std::pmr::memory_resource* wrapped)
        : wrapped_(validated_wrapped(wrapped)) {}

    synchronized_resource(const synchronized_resource&) = delete;
    synchronized_resource& operator=(const synchronized_resource&) = delete;

    std::pmr::memory_resource* wrapped() const noexcept {
        return wrapped_;
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        const std::lock_guard<std::mutex> guard(mutex_);
        return wrapped_->allocate(bytes, alignment);
    }

    void do_deallocate(void* block, std::size_t bytes, std::size_t alignment) override {
        const std::lock_guard<std::mutex> guard(mutex_);
        wrapped_->deallocate(block, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    static std::pmr::memory_resource* validated_wrapped(std::pmr::memory_resource* wrapped) {
        if (wrapped == nullptr) {
            throw std::invalid_argument("memalloc::synchronized_resource: wrapped resource is null");
        }
        return wrapped;
    }

    std::pmr::memory_resource* wrapped_;
    std::mutex mutex_;
};

}
