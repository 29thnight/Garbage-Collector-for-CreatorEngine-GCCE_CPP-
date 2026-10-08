#include "block_allocator.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#if defined(__SANITIZE_ADDRESS__)
#    define GCCE_ASAN 1
#elif defined(__has_feature)
#    if __has_feature(address_sanitizer)
#        define GCCE_ASAN 1
#    endif
#endif

#if defined(GCCE_ASAN)
#    include <sanitizer/asan_interface.h>
#    define GCCE_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#    define GCCE_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#    define GCCE_POISON(p, n) ((void)(p), (void)(n))
#    define GCCE_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

namespace gc::detail
{
namespace
{
constexpr std::array<std::size_t, 24> class_sizes{16,  32,  48,  64,  80,   96,   112,  128,
                                                  160, 192, 224, 256, 320,  384,  448,  512,
                                                  640, 768, 896, 1024, 1280, 1536, 1792, 2048};

int class_for(std::size_t size) noexcept
{
    auto it = std::lower_bound(class_sizes.begin(), class_sizes.end(), size);
    return it == class_sizes.end() ? -1 : static_cast<int>(it - class_sizes.begin());
}

#if !defined(NDEBUG) && !defined(GCCE_ASAN)
constexpr unsigned char freed_fill = 0xDD;
#endif
} // namespace

struct block_allocator::page
{
    page* prev = nullptr;
    page* next = nullptr;
    page* all_next = nullptr;
    void* free_list = nullptr;
    unsigned char* bump = nullptr; // never-used blocks start here
    unsigned char* end = nullptr;
    std::uint32_t live = 0;
    std::uint32_t capacity = 0;
    std::uint8_t cls = 0;
    bool listed = false; // in its class's available list
};

namespace
{
// Blocks start after the page header and stay 16-byte aligned.
constexpr std::size_t page_header_size = 64;
}

block_allocator::block_allocator() noexcept
{
    static_assert(class_sizes.size() == class_count);
    static_assert(sizeof(page) <= page_header_size);
    for (std::size_t i = 0; i < class_count; ++i)
        classes_[i].block_size = class_sizes[i];
}

block_allocator::~block_allocator()
{
    if (leaked_)
        return;
    for (page* p = all_pages_; p;)
    {
        page* next = p->all_next;
        GCCE_UNPOISON(p, page_size);
        ::operator delete(static_cast<void*>(p), std::align_val_t{page_size});
        p = next;
    }
}

void block_allocator::leak() noexcept { leaked_ = true; }

void block_allocator::unlink(page*& head, page* p) noexcept
{
    if (p->prev)
        p->prev->next = p->next;
    else
        head = p->next;
    if (p->next)
        p->next->prev = p->prev;
    p->prev = p->next = nullptr;
    p->listed = false;
}

void block_allocator::push(page*& head, page* p) noexcept
{
    p->prev = nullptr;
    p->next = head;
    if (head)
        head->prev = p;
    head = p;
    p->listed = true;
}

block_allocator::page* block_allocator::new_page(std::uint8_t cls)
{
    void* memory = ::operator new(page_size, std::align_val_t{page_size});
    auto* p = ::new (memory) page{};
    p->cls = cls;
    p->bump = static_cast<unsigned char*>(memory) + page_header_size;
    const std::size_t block = classes_[cls].block_size;
    p->capacity = static_cast<std::uint32_t>((page_size - page_header_size) / block);
    p->end = p->bump + std::size_t{p->capacity} * block;
    GCCE_POISON(p->bump, page_size - page_header_size);

    p->all_next = all_pages_;
    all_pages_ = p;
    committed_ += page_size;
    ++pages_;
    push(classes_[cls].available, p);
    return p;
}

void block_allocator::release_page(page* p) noexcept
{
    if (p->listed)
        unlink(classes_[p->cls].available, p);
    // Remove from the teardown list (singly linked; pages are few).
    for (page** link = &all_pages_; *link; link = &(*link)->all_next)
    {
        if (*link == p)
        {
            *link = p->all_next;
            break;
        }
    }
    committed_ -= page_size;
    --pages_;
    GCCE_UNPOISON(p, page_size);
    ::operator delete(static_cast<void*>(p), std::align_val_t{page_size});
}

block_allocator::allocation block_allocator::allocate(std::size_t size, std::size_t align)
{
    const int cls = align <= small_align ? class_for(size) : -1;
    if (cls < 0)
    {
        const std::size_t a = std::max(align, small_align);
        void* block = ::operator new(size, std::align_val_t{a});
        committed_ += size;
        return {block, large_class};
    }

    size_class_state& state = classes_[static_cast<std::size_t>(cls)];
    page* p = state.available ? state.available : new_page(static_cast<std::uint8_t>(cls));

    void* block;
    if (p->free_list)
    {
        block = p->free_list;
        GCCE_UNPOISON(block, state.block_size);
        std::memcpy(&p->free_list, block, sizeof(void*));
    }
    else
    {
        block = p->bump;
        p->bump += state.block_size;
        GCCE_UNPOISON(block, state.block_size);
    }
    ++p->live;
    if (!p->free_list && p->bump == p->end)
        unlink(state.available, p); // full
    return {block, static_cast<std::uint8_t>(cls)};
}

void block_allocator::deallocate(void* block, std::size_t size, std::size_t align, std::uint8_t size_class) noexcept
{
    if (size_class == large_class)
    {
        committed_ -= size;
        ::operator delete(block, std::align_val_t{std::max(align, small_align)});
        return;
    }

    auto* p = reinterpret_cast<page*>(reinterpret_cast<std::uintptr_t>(block) & ~(page_size - 1));
    size_class_state& state = classes_[p->cls];

#if !defined(NDEBUG) && !defined(GCCE_ASAN)
    std::memset(block, freed_fill, state.block_size);
#endif
    std::memcpy(block, &p->free_list, sizeof(void*));
    p->free_list = block;
    GCCE_POISON(block, state.block_size);
    --p->live;

    if (!p->listed)
        push(state.available, p); // was full
    // Keep one page with free space per class; return other empty pages.
    if (p->live == 0 && (p->prev || p->next))
        release_page(p);
}
} // namespace gc::detail
