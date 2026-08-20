#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "memalloc/detail/align.hpp"
#include "memalloc/detail/debug.hpp"

namespace memalloc {

class arena_resource : public std::pmr::memory_resource {
public:
    static constexpr std::size_t default_alignment = detail::max(alignof(std::max_align_t), 16);

    explicit arena_resource(std::size_t capacity,
                            std::size_t alignment = default_alignment,
                            std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(validated_upstream(upstream)),
          capacity_(validated_capacity(capacity)),
          alignment_(validated_alignment(alignment)),
          base_(static_cast<std::byte*>(upstream_->allocate(capacity_, alignment_))) {}

    arena_resource(const arena_resource&) = delete;
    arena_resource& operator=(const arena_resource&) = delete;

    ~arena_resource() override {
        upstream_->deallocate(base_, capacity_, alignment_);
    }

    void reset() noexcept {
        used_ = 0;
    }

    bool owns(const void* address) const noexcept {
        const auto* byte_address = static_cast<const std::byte*>(address);
        return byte_address >= base_ && byte_address < base_ + capacity_;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t used() const noexcept {
        return used_;
    }

    std::size_t remaining() const noexcept {
        return capacity_ - used_;
    }

    std::size_t high_water_mark() const noexcept {
        return high_water_mark_;
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (alignment > alignment_ || detail::align_up_overflows(used_, alignment)) {
            throw std::bad_alloc();
        }
        const std::size_t offset = detail::align_up(used_, alignment);
        if (offset > capacity_ || bytes > capacity_ - offset) {
            throw std::bad_alloc();
        }
        used_ = offset + bytes;
        high_water_mark_ = detail::max(high_water_mark_, used_);
        return base_ + offset;
    }

    void do_deallocate(void* address, std::size_t, std::size_t) override {
        MEMALLOC_DEBUG_CHECK(owns(address), "arena_resource::deallocate of a foreign pointer");
        (void)address;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    static std::pmr::memory_resource* validated_upstream(std::pmr::memory_resource* upstream) {
        if (upstream == nullptr) {
            throw std::invalid_argument("memalloc::arena_resource: upstream is null");
        }
        return upstream;
    }

    static std::size_t validated_capacity(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("memalloc::arena_resource: capacity is zero");
        }
        return capacity;
    }

    static std::size_t validated_alignment(std::size_t alignment) {
        if (!detail::is_power_of_two(alignment)) {
            throw std::invalid_argument("memalloc::arena_resource: alignment is not a power of two");
        }
        return alignment;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t capacity_;
    std::size_t alignment_;
    std::byte* base_;
    std::size_t used_ = 0;
    std::size_t high_water_mark_ = 0;
};

}
