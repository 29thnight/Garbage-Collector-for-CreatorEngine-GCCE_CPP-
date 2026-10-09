#include <gc/gc.hpp>

#include <bit>
#include <cstdint>

// This target asks for C++17 and links only gcce::gcce. The public C++23
// requirement must supply both the language and library features below.
static_assert(std::byteswap(std::uint32_t{0x12345678}) == 0x78563412);

struct consumer_node : gc::managed
{
    gc::root_ref<consumer_node> root() { return root_from_this(); }
};
