#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <vector>

#include "memalloc/detail/align.hpp"
#include "memalloc/detail/debug.hpp"

namespace memalloc {

enum class pool_growth { fixed, doubling };

class pool_resource : public std::pmr::memory_resource {
public:
    pool_resource(std::size_t block_size,
                  std::size_t block_alignment,
                  std::size_t blocks_per_chunk,
                  pool_growth growth = pool_growth::fixed,
                  std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(validated_upstream(upstream)),
          alignment_(detail::max(validated_alignment(block_alignment), alignof(void*))),
          stride_(validated_stride(block_size, alignment_)),
          blocks_per_chunk_(validated_blocks_per_chunk(blocks_per_chunk, stride_)),
          growth_(growth),
          next_chunk_blocks_(blocks_per_chunk_) {}

    template <class T>
    static pool_resource for_type(std::size_t blocks_per_chunk,
                                  pool_growth growth = pool_growth::fixed,
                                  std::pmr::memory_resource* upstream = std::pmr::get_default_resource()) {
        return pool_resource(sizeof(T), alignof(T), blocks_per_chunk, growth, upstream);
    }

    pool_resource(const pool_resource&) = delete;
    pool_resource& operator=(const pool_resource&) = delete;

    ~pool_resource() override {
        free_chunks();
    }

    void release() {
        if (in_use_ != 0) {
            throw std::logic_error("memalloc::pool_resource::release with live blocks");
        }
        free_chunks();
        chunks_.clear();
        free_list_ = nullptr;
        carve_next_ = nullptr;
        carve_end_ = nullptr;
        reserved_ = 0;
        next_chunk_blocks_ = blocks_per_chunk_;
    }

    bool owns(const void* address) const noexcept {
        return find_chunk(address) != nullptr;
    }

    std::size_t block_size() const noexcept {
        return stride_;
    }

    std::size_t block_alignment() const noexcept {
        return alignment_;
    }

    std::size_t blocks_in_use() const noexcept {
        return in_use_;
    }

    std::size_t blocks_reserved() const noexcept {
        return reserved_;
    }

    std::size_t chunk_count() const noexcept {
        return chunks_.size();
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) final {
        if (bytes > stride_ || alignment > alignment_) {
            throw std::bad_alloc();
        }
        void* block = free_list_;
        if (block != nullptr) {
            free_list_ = *static_cast<void**>(block);
        } else {
            if (carve_next_ == carve_end_) {
                add_chunk();
            }
            block = carve_next_;
            carve_next_ += stride_;
        }
        ++in_use_;
        return block;
    }

    void do_deallocate(void* address, std::size_t bytes, std::size_t alignment) final {
        MEMALLOC_DEBUG_CHECK(bytes <= stride_ && alignment <= alignment_,
                             "pool_resource::deallocate with a size or alignment it cannot serve");
        (void)bytes;
        (void)alignment;
        MEMALLOC_DEBUG_CHECK(is_block_start(address),
                             "pool_resource::deallocate of a pointer this pool never handed out");
        MEMALLOC_DEBUG_CHECK(in_use_ != 0, "pool_resource::deallocate with no block in use");
        MEMALLOC_SLOW_CHECK(!is_in_free_list(address), "pool_resource::deallocate called twice");
        *static_cast<void**>(address) = free_list_;
        free_list_ = address;
        --in_use_;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept final {
        return this == &other;
    }

private:
    struct chunk {
        std::byte* memory;
        std::size_t blocks;
    };

    static std::pmr::memory_resource* validated_upstream(std::pmr::memory_resource* upstream) {
        if (upstream == nullptr) {
            throw std::invalid_argument("memalloc::pool_resource: upstream is null");
        }
        return upstream;
    }

    static std::size_t validated_alignment(std::size_t alignment) {
        if (!detail::is_power_of_two(alignment)) {
            throw std::invalid_argument("memalloc::pool_resource: alignment is not a power of two");
        }
        return alignment;
    }

    static std::size_t validated_stride(std::size_t block_size, std::size_t alignment) {
        const std::size_t wanted = detail::max(block_size, sizeof(void*));
        if (detail::align_up_overflows(wanted, alignment)) {
            throw std::invalid_argument("memalloc::pool_resource: block size overflows when aligned");
        }
        return detail::align_up(wanted, alignment);
    }

    static std::size_t validated_blocks_per_chunk(std::size_t blocks, std::size_t stride) {
        if (blocks == 0) {
            throw std::invalid_argument("memalloc::pool_resource: blocks per chunk is zero");
        }
        if (detail::product_overflows(stride, blocks)) {
            throw std::invalid_argument("memalloc::pool_resource: chunk size overflows");
        }
        return blocks;
    }

    static bool starts_before(const std::byte* address, const chunk& candidate) noexcept {
        return std::less<const std::byte*>{}(address, candidate.memory);
    }

    const chunk* find_chunk(const void* address) const noexcept {
        const auto* byte_address = static_cast<const std::byte*>(address);
        const auto after = std::upper_bound(chunks_.begin(), chunks_.end(), byte_address, &starts_before);
        if (after == chunks_.begin()) {
            return nullptr;
        }
        const chunk& candidate = *(after - 1);
        const std::less<const std::byte*> before{};
        return before(byte_address, candidate.memory + candidate.blocks * stride_) ? &candidate : nullptr;
    }

    bool is_carved(const void* address) const noexcept {
        const std::less<const std::byte*> before{};
        const auto* byte_address = static_cast<const std::byte*>(address);
        return before(byte_address, carve_next_) || !before(byte_address, carve_end_);
    }

    bool is_block_start(const void* address) const noexcept {
        const chunk* owner = find_chunk(address);
        if (owner == nullptr || !is_carved(address)) {
            return false;
        }
        const auto offset = static_cast<std::size_t>(static_cast<const std::byte*>(address) - owner->memory);
        return offset % stride_ == 0;
    }

    bool is_in_free_list(const void* address) const noexcept {
        for (void* block = free_list_; block != nullptr; block = *static_cast<void**>(block)) {
            if (block == address) {
                return true;
            }
        }
        return false;
    }

    std::size_t next_chunk_size() const {
        if (detail::product_overflows(stride_, next_chunk_blocks_)) {
            throw std::bad_alloc();
        }
        return stride_ * next_chunk_blocks_;
    }

    void add_chunk() {
        const std::size_t blocks = next_chunk_blocks_;
        const std::size_t bytes = next_chunk_size();
        if (chunks_.size() == chunks_.capacity()) {
            chunks_.reserve(detail::max(8, chunks_.capacity() * 2));
        }
        auto* memory = static_cast<std::byte*>(upstream_->allocate(bytes, alignment_));
        chunks_.insert(std::upper_bound(chunks_.begin(), chunks_.end(), memory, &starts_before),
                       chunk{memory, blocks});
        reserved_ += blocks;
        carve_next_ = memory;
        carve_end_ = memory + bytes;
        if (growth_ == pool_growth::doubling && !detail::product_overflows(2, blocks) &&
            !detail::product_overflows(stride_, blocks * 2)) {
            next_chunk_blocks_ = blocks * 2;
        }
    }

    void free_chunks() noexcept {
        for (const chunk& released : chunks_) {
            upstream_->deallocate(released.memory, released.blocks * stride_, alignment_);
        }
    }

    std::pmr::memory_resource* upstream_;
    std::size_t alignment_;
    std::size_t stride_;
    std::size_t blocks_per_chunk_;
    pool_growth growth_;
    std::size_t next_chunk_blocks_;
    std::vector<chunk> chunks_;
    void* free_list_ = nullptr;
    std::byte* carve_next_ = nullptr;
    std::byte* carve_end_ = nullptr;
    std::size_t in_use_ = 0;
    std::size_t reserved_ = 0;
};

}
