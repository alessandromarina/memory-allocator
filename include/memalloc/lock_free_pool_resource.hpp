#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <thread>

#include "memalloc/detail/align.hpp"
#include "memalloc/detail/backoff.hpp"
#include "memalloc/detail/debug.hpp"

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4324)
#endif

namespace memalloc {

namespace detail {

inline std::size_t this_thread_slot() noexcept {
    static std::atomic<std::size_t> next_slot{0};
    thread_local std::size_t slot = 0;
    if (slot == 0) {
        slot = next_slot.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    return slot;
}

}

class lock_free_pool_resource : public std::pmr::memory_resource {
public:
    static constexpr std::size_t max_capacity = 0xFFFF'FFFEu;
    static constexpr std::size_t max_shards = 16;
    static constexpr std::size_t min_blocks_per_shard = 64;
    static constexpr bool is_always_lock_free = std::atomic<std::uint64_t>::is_always_lock_free;

    lock_free_pool_resource(std::size_t block_size,
                            std::size_t block_alignment,
                            std::size_t capacity,
                            std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(validated_upstream(upstream)),
          alignment_(validated_alignment(block_alignment)),
          stride_(validated_stride(block_size, alignment_)),
          capacity_(validated_capacity(capacity, stride_)),
          layout_{(std::uint64_t{1} << detail::bit_width(capacity_ + 1)) - 1},
          shard_mask_(shard_count_for(capacity_) - 1),
          links_offset_(detail::align_up(capacity_ * stride_, alignof(link))),
          storage_bytes_(validated_storage_bytes(links_offset_, capacity_)),
          storage_alignment_(detail::max(alignment_, alignof(link))),
          blocks_(static_cast<std::byte*>(upstream_->allocate(storage_bytes_, storage_alignment_))),
          links_(reinterpret_cast<link*>(blocks_ + links_offset_)) {
        split_into_shards();
    }

    template <class T>
    static lock_free_pool_resource for_type(
        std::size_t capacity, std::pmr::memory_resource* upstream = std::pmr::get_default_resource()) {
        return lock_free_pool_resource(sizeof(T), alignof(T), capacity, upstream);
    }

    lock_free_pool_resource(const lock_free_pool_resource&) = delete;
    lock_free_pool_resource& operator=(const lock_free_pool_resource&) = delete;

    ~lock_free_pool_resource() override {
        upstream_->deallocate(blocks_, storage_bytes_, storage_alignment_);
    }

    bool owns(const void* address) const noexcept {
        const auto* byte_address = static_cast<const std::byte*>(address);
        return byte_address >= blocks_ && byte_address < blocks_ + capacity_ * stride_;
    }

    bool is_lock_free() const noexcept {
        if constexpr (is_always_lock_free) {
            return true;
        } else {
            return shards_[0].head.is_lock_free();
        }
    }

    std::size_t block_size() const noexcept {
        return stride_;
    }

    std::size_t block_alignment() const noexcept {
        return alignment_;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t shard_count() const noexcept {
        return shard_mask_ + 1;
    }

    unsigned aba_tag_bits() const noexcept {
        return 64 - detail::bit_width(layout_.index_mask);
    }

    std::size_t blocks_in_use() const noexcept {
        std::size_t free_blocks = 0;
        for (std::size_t shard = 0; shard <= shard_mask_; ++shard) {
            std::uint32_t index = layout_.index_of(shards_[shard].head.load(std::memory_order_acquire));
            while (index < capacity_ && free_blocks < capacity_) {
                ++free_blocks;
                index = links_[index].load(std::memory_order_relaxed);
            }
        }
        return capacity_ - free_blocks;
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) final {
        if (bytes > stride_ || alignment > alignment_) {
            throw std::bad_alloc();
        }
        const head_layout layout = layout_;
        const std::size_t home = detail::this_thread_slot();
        std::uint64_t empty_head = 0;
        const std::uint32_t index = pop(shards_[home & shard_mask_].head, layout, empty_head);
        if (index != layout.no_block()) {
            return hand_out(index, layout);
        }
        return steal(home, layout);
    }

    void do_deallocate(void* address, std::size_t bytes, std::size_t alignment) final {
        MEMALLOC_DEBUG_CHECK(bytes <= stride_ && alignment <= alignment_,
                             "lock_free_pool_resource::deallocate with a size or alignment it cannot serve");
        MEMALLOC_DEBUG_CHECK(is_block_start(address),
                             "lock_free_pool_resource::deallocate of a pointer this pool never handed out");
        const std::uint32_t index = index_of_block(address);
        MEMALLOC_DEBUG_CHECK(links_[index].load(std::memory_order_relaxed) == layout_.handed_out(),
                             "lock_free_pool_resource::deallocate called twice");
        (void)bytes;
        (void)alignment;
        push(shards_[detail::this_thread_slot() & shard_mask_].head, index, layout_);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept final {
        return this == &other;
    }

private:
    using link = std::atomic<std::uint32_t>;

    static constexpr std::size_t cache_line = 64;

    struct head_layout {
        std::uint64_t index_mask;

        std::uint32_t no_block() const noexcept {
            return static_cast<std::uint32_t>(index_mask);
        }

        std::uint32_t handed_out() const noexcept {
            return no_block() - 1;
        }

        std::uint32_t index_of(std::uint64_t head) const noexcept {
            return static_cast<std::uint32_t>(head & index_mask);
        }

        std::uint64_t pack(std::uint32_t index, std::uint64_t previous_head) const noexcept {
            return ((previous_head + index_mask + 1) & ~index_mask) | (index & index_mask);
        }
    };

    struct alignas(cache_line) shard {
        std::atomic<std::uint64_t> head{0};
    };

    std::uint32_t pop(std::atomic<std::uint64_t>& head_ref,
                      const head_layout layout,
                      std::uint64_t& empty_head) noexcept {
        detail::backoff contention;
        std::uint64_t head = head_ref.load(std::memory_order_acquire);
        std::uint32_t index = layout.index_of(head);
        while (
            index != layout.no_block() &&
            !head_ref.compare_exchange_weak(head,
                                            layout.pack(links_[index].load(std::memory_order_relaxed), head),
                                            std::memory_order_acquire,
                                            std::memory_order_acquire)) {
            contention.pause();
            head = head_ref.load(std::memory_order_acquire);
            index = layout.index_of(head);
        }
        empty_head = head;
        return index;
    }

    void* hand_out(std::uint32_t index, const head_layout layout) noexcept {
#if MEMALLOC_DEBUG_CHECKS
        links_[index].store(layout.handed_out(), std::memory_order_relaxed);
#else
        (void)layout;
#endif
        return blocks_ + static_cast<std::size_t>(index) * stride_;
    }

    void* steal(std::size_t home, const head_layout layout) {
        std::uint64_t empty_heads[max_shards] = {};
        for (;;) {
            for (std::size_t step = 0; step <= shard_mask_; ++step) {
                const std::size_t shard = (home + step) & shard_mask_;
                const std::uint32_t index = pop(shards_[shard].head, layout, empty_heads[shard]);
                if (index != layout.no_block()) {
                    return hand_out(index, layout);
                }
            }
            if (every_shard_still_at(empty_heads)) {
                throw std::bad_alloc();
            }
        }
    }

    bool every_shard_still_at(const std::uint64_t* empty_heads) const noexcept {
        for (std::size_t shard = 0; shard <= shard_mask_; ++shard) {
            if (shards_[shard].head.load(std::memory_order_acquire) != empty_heads[shard]) {
                return false;
            }
        }
        return true;
    }

    void push(std::atomic<std::uint64_t>& head_ref, std::uint32_t index, const head_layout layout) noexcept {
        detail::backoff contention;
        std::uint64_t head = head_ref.load(std::memory_order_relaxed);
        links_[index].store(layout.index_of(head), std::memory_order_relaxed);
        while (!head_ref.compare_exchange_weak(
            head, layout.pack(index, head), std::memory_order_release, std::memory_order_relaxed)) {
            contention.pause();
            head = head_ref.load(std::memory_order_relaxed);
            links_[index].store(layout.index_of(head), std::memory_order_relaxed);
        }
    }

    static std::size_t shard_count_for(std::size_t capacity) noexcept {
        const std::size_t threads = detail::max(std::thread::hardware_concurrency(), 1);
        std::size_t count = 1;
        while (count < threads && count < max_shards && (count * 2) * min_blocks_per_shard <= capacity) {
            count *= 2;
        }
        return count;
    }

    static std::pmr::memory_resource* validated_upstream(std::pmr::memory_resource* upstream) {
        if (upstream == nullptr) {
            throw std::invalid_argument("memalloc::lock_free_pool_resource: upstream is null");
        }
        return upstream;
    }

    static std::size_t validated_alignment(std::size_t alignment) {
        if (!detail::is_power_of_two(alignment)) {
            throw std::invalid_argument("memalloc::lock_free_pool_resource: alignment is not a power of two");
        }
        return alignment;
    }

    static std::size_t validated_stride(std::size_t block_size, std::size_t alignment) {
        const std::size_t wanted = detail::max(block_size, 1);
        if (detail::align_up_overflows(wanted, alignment)) {
            throw std::invalid_argument(
                "memalloc::lock_free_pool_resource: block size overflows when aligned");
        }
        return detail::align_up(wanted, alignment);
    }

    static std::size_t validated_capacity(std::size_t capacity, std::size_t stride) {
        if (capacity == 0) {
            throw std::invalid_argument("memalloc::lock_free_pool_resource: capacity is zero");
        }
        if (capacity > max_capacity) {
            throw std::invalid_argument("memalloc::lock_free_pool_resource: capacity exceeds max_capacity");
        }
        if (detail::product_overflows(stride, capacity) ||
            detail::align_up_overflows(stride * capacity, alignof(link))) {
            throw std::invalid_argument("memalloc::lock_free_pool_resource: storage size overflows");
        }
        return capacity;
    }

    static std::size_t validated_storage_bytes(std::size_t links_offset, std::size_t capacity) {
        if (detail::product_overflows(sizeof(link), capacity) ||
            links_offset > detail::size_max - sizeof(link) * capacity) {
            throw std::invalid_argument("memalloc::lock_free_pool_resource: storage size overflows");
        }
        return links_offset + sizeof(link) * capacity;
    }

    void split_into_shards() noexcept {
        const std::size_t count = shard_mask_ + 1;
        for (std::size_t shard = 0; shard < count; ++shard) {
            const std::size_t begin = capacity_ / count * shard;
            const std::size_t end = shard + 1 == count ? capacity_ : capacity_ / count * (shard + 1);
            for (std::size_t index = begin; index + 1 < end; ++index) {
                ::new (static_cast<void*>(links_ + index)) link(static_cast<std::uint32_t>(index + 1));
            }
            ::new (static_cast<void*>(links_ + end - 1)) link(layout_.no_block());
            shards_[shard].head.store(begin, std::memory_order_relaxed);
        }
    }

    bool is_block_start(const void* address) const noexcept {
        if (!owns(address)) {
            return false;
        }
        const auto offset = static_cast<std::size_t>(static_cast<const std::byte*>(address) - blocks_);
        return offset % stride_ == 0;
    }

    std::uint32_t index_of_block(const void* address) const noexcept {
        const auto offset = static_cast<std::size_t>(static_cast<const std::byte*>(address) - blocks_);
        return static_cast<std::uint32_t>(offset / stride_);
    }

    std::pmr::memory_resource* upstream_;
    std::size_t alignment_;
    std::size_t stride_;
    std::size_t capacity_;
    head_layout layout_;
    std::size_t shard_mask_;
    std::size_t links_offset_;
    std::size_t storage_bytes_;
    std::size_t storage_alignment_;
    std::byte* blocks_;
    link* links_;
    shard shards_[max_shards];
};

}

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif
