#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gc::detail
{
// Storage for GC blocks (object header + object). Owned by one domain and
// used from its owner thread only.
//
// Small blocks come from 64 KiB pages split into size classes; a freed block
// goes back to its page's free list, and an emptied page is returned unless
// it is the last page with free space in its class. Blocks above the largest
// class, or with alignment above 16, are allocated individually.
class block_allocator
{
public:
    static constexpr std::size_t page_size = std::size_t{64} * 1024;
    static constexpr std::size_t small_align = 16;
    static constexpr std::uint8_t large_class = 0xFF;

    struct allocation
    {
        void* block;
        std::uint8_t size_class;
    };

    block_allocator() noexcept;
    ~block_allocator();

    block_allocator(const block_allocator&) = delete;
    block_allocator& operator=(const block_allocator&) = delete;

    // Throws std::bad_alloc.
    allocation allocate(std::size_t size, std::size_t align);
    void deallocate(void* block, std::size_t size, std::size_t align, std::uint8_t size_class) noexcept;

    // Abandons all pages without freeing them, for shutdown with objects
    // that are still referenced from outside.
    void leak() noexcept;

    [[nodiscard]] std::size_t committed_bytes() const noexcept { return committed_; }
    [[nodiscard]] std::size_t page_count() const noexcept { return pages_; }

private:
    struct page;
    struct size_class_state
    {
        std::size_t block_size = 0;
        page* available = nullptr; // pages with at least one free block
    };

    static constexpr std::size_t class_count = 24;

    page* new_page(std::uint8_t cls);
    void release_page(page* p) noexcept;
    static void unlink(page*& head, page* p) noexcept;
    static void push(page*& head, page* p) noexcept;

    std::array<size_class_state, class_count> classes_{};
    page* all_pages_ = nullptr; // every page, for teardown
    std::size_t committed_ = 0;
    std::size_t pages_ = 0;
    bool leaked_ = false;
};
} // namespace gc::detail
