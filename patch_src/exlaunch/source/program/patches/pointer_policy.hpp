#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pshowcoop::pointerguard {
struct Range { std::uintptr_t start; std::size_t size; };
struct Layout { Range alias, heap, main; };
struct Mapping { Range range; bool readable; };

// Subtraction keeps both the requested span and the range end overflow-safe.
constexpr bool Contains(Range r, std::uintptr_t a, std::size_t n) {
    return n != 0 && n - 1 <= std::numeric_limits<std::uintptr_t>::max() - a &&
        n <= r.size && a >= r.start && a - r.start <= r.size - n;
}
constexpr bool Legacy(std::uintptr_t a) {
    return (a & 7) == 0 && a >= 0x1000000000ULL && a < 0x4000000000ULL;
}
template<class Query>
bool Accept(const Layout& layout, std::uintptr_t a, std::size_t n, Query query) {
    if (a == 0 || (a & 7) != 0) return false;
    // UE allocations are observed in Alias, not just the kernel Heap region.
    // Main is also needed: existing callers check native vtable/data pointers.
    if (!(Contains(layout.alias, a, n) || Contains(layout.heap, a, n) ||
          Contains(layout.main, a, n))) return false;
    Mapping mapping{};
    return query(a, mapping) && mapping.readable && Contains(mapping.range, a, n);
}
} // namespace pshowcoop::pointerguard
