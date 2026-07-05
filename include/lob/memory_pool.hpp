#pragma once
#include "lob/types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// Intrusive node for the pool-backed doubly linked FIFO per price level.
//
// Free invariant:  when a node sits on the free list, Node::next holds the index
//                  of the next free node (or NIL); all other fields are unspecified.
// Live invariant:  when a node is live in a level's FIFO, prev/next link it to its
//                  neighbors (NIL at the boundaries); slot records its price level;
//                  side records the order side (needed for modify's cancel+re-add).
struct Node {
    OrderId       id;
    Quantity      qty;
    std::uint32_t next;   // next live neighbor in FIFO, OR next free slot in free list
    std::uint32_t prev;   // prev live neighbor in FIFO
    std::uint32_t slot;   // price-level slot index (valid when live)
    Side          side;   // order side (valid when live; needed by modify)

    static constexpr std::uint32_t NIL = 0xFFFFFFFFu;
};

// Fixed-capacity arena with an O(1) intrusive free list threaded through Node::next.
// allocate() pops free_head_; deallocate() pushes back.
// No heap activity on the matching hot path after construction.
class NodePool {
public:
    explicit NodePool(std::size_t capacity)
        : arena_(capacity), free_head_(capacity == 0 ? Node::NIL : 0u) {
        // Build forward free list: slot i -> i+1 -> ... -> NIL.
        for (std::size_t i = 0; i < capacity; ++i)
            arena_[i].next = (i + 1 < capacity)
                             ? static_cast<std::uint32_t>(i + 1)
                             : Node::NIL;
    }

    // Pop the free-list head. Returns NIL if the arena is exhausted.
    std::uint32_t allocate() {
        if (free_head_ == Node::NIL) return Node::NIL;
        const std::uint32_t idx = free_head_;
        free_head_ = arena_[idx].next;
        return idx;
    }

    // Push idx back onto the free-list head.
    void deallocate(std::uint32_t idx) {
        arena_[idx].next = free_head_;
        free_head_ = idx;
    }

    Node&       operator[](std::uint32_t idx)       { return arena_[idx]; }
    const Node& operator[](std::uint32_t idx) const { return arena_[idx]; }

    // Rebuild the free list over the entire arena (used by ArrayOrderBook::clear).
    void clear() {
        const std::size_t n = arena_.size();
        free_head_ = n == 0 ? Node::NIL : 0u;
        for (std::size_t i = 0; i < n; ++i)
            arena_[i].next = (i + 1 < n)
                             ? static_cast<std::uint32_t>(i + 1)
                             : Node::NIL;
    }

private:
    std::vector<Node> arena_;   // never grows after construction
    std::uint32_t     free_head_;
};

} // namespace lob
