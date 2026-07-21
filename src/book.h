#pragma once

#include <cassert>
#include <cstdint>
#include <vector>
#include <map>
#include "../third_party/ankerl/unordered_dense.h"

// "No order" sentinel for the intrusive prev/next links and level head/tail.
// Valid indices are 0..Capacity-1, so UINT32_MAX can never collide.
constexpr uint32_t NIL = UINT32_MAX;

struct Order
{
    uint8_t side; // 1 for bid; 0 for ask
    uint32_t price = 0;
    uint32_t shares = 0;
    uint32_t prev = NIL; // prev index
    uint32_t next = NIL; // next index
};


constexpr uint32_t pool_cap = 1 << 15;
template <typename T, uint32_t Capacity>
class Pool
{
    std::vector<T> memory; // the actual storage
    std::vector<uint32_t> free_list; // keeps track of the next free index
    uint32_t next_free_index = 0;

public:

    Pool() : memory(Capacity), free_list(Capacity)
    {
        for (uint32_t i = 0; i < Capacity; i++)
        {
            free_list[i] = i;
        }
    }

    uint32_t allocate()
    {
        assert(next_free_index < Capacity && "pool overflow");

        uint32_t index = free_list[next_free_index];
        next_free_index++;
        return index;
    }

    void deallocate(uint32_t index)
    {
        assert(next_free_index > 0 && "pool underflow / double free");

        next_free_index--;
        free_list[next_free_index] = index;
    }

    // Drop all allocations at once. The free_list must be restored to the
    // identity permutation (allocate/deallocate churn leaves stale entries in
    // the allocated region), so this is O(Capacity) — a cold-path reset only.
    void reset()
    {
        for (uint32_t i = 0; i < Capacity; i++) free_list[i] = i;
        next_free_index = 0;
    }

    // Turn a pool index back into the order it names.
    T& operator[](uint32_t index) { return memory[index]; }
    const T& operator[](uint32_t index) const { return memory[index]; }
};

struct Level
{
    uint32_t price = 0;
    uint32_t quantity = 0; // stores the sum of all orders at the price level
    uint32_t head = NIL; // index
    uint32_t tail = NIL; // index
};

class Book
{
    Pool<Order, pool_cap> orders_; // mempool for orders
    Pool<Level, pool_cap> levels_; // mempool for levels
    std::map<uint32_t, uint32_t> bids_, asks_; // price -> level idx
    ankerl::unordered_dense::map<uint64_t, uint32_t> id_to_order_; // order id to order index in the mempool

    std::map<uint32_t, uint32_t>& side_for(uint8_t side)
    {
        return side ? bids_ : asks_;
    }

    void unlink_and_free(decltype(id_to_order_)::iterator it)
    {
        auto order_idx = it->second;
        auto& o = orders_[order_idx];
        auto &m = side_for(o.side);
        auto lit = m.find(o.price);
        assert(lit != m.end() && "order price not in book");
        auto level_idx = lit->second;
        auto& l = levels_[level_idx];

        l.quantity -= o.shares;

        // only 1 order in the level
        if (o.prev == NIL && o.next == NIL)
        {
            m.erase(o.price);
            orders_.deallocate(order_idx);
            levels_.deallocate(level_idx);
        }else if (o.prev == NIL) // order is the head
        {
            l.head = o.next;
            auto& next_order = orders_[o.next];
            next_order.prev = NIL;
            orders_.deallocate(order_idx);
        }else if (o.next == NIL) // order is the tail
        {
            l.tail = o.prev;
            auto& prev_order = orders_[o.prev];
            prev_order.next = NIL;
            orders_.deallocate(order_idx);
        }else
        {
            auto& next_order = orders_[o.next];
            auto& prev_order = orders_[o.prev];
            next_order.prev = o.prev;
            prev_order.next = o.next;
            orders_.deallocate(order_idx);
        }
        id_to_order_.erase(it);
    }

public:

    Book() { id_to_order_.reserve(pool_cap); }

    void add_order(uint64_t order_id, uint8_t side, uint32_t price, uint32_t shares)
    {
        // make order struct
        auto orders_idx = orders_.allocate();
        Order& o = orders_[orders_idx];
        id_to_order_[order_id] = orders_idx;
        o.price = price;
        o.shares = shares;
        o.side = side;
        o.prev = o.next = NIL; // we add this because it might contain prev and next from last allocate

        // find level in correct side map
        auto& m = side_for(side);
        auto it = m.find(price);
        uint32_t level_idx;
        if (it != m.end())
        {
            level_idx = it->second;
        }else
        {
            level_idx = levels_.allocate();
            Level& nl = levels_[level_idx];
            nl.price = price;
            nl.head = nl.tail = NIL;
            nl.quantity = 0;
            m.emplace(price, level_idx);
        }
        Level& l = levels_[level_idx];
        l.quantity += shares;

        // append the order to the tail
        if (l.tail == NIL)
        {
            l.head = orders_idx;
            l.tail = orders_idx;
        }else
        {
            o.prev = l.tail;
            orders_[l.tail].next = orders_idx;
            l.tail = orders_idx;
        }
    }

    void delete_order(uint64_t order_id)
    {
        auto it = id_to_order_.find(order_id);
        if (it != id_to_order_.end())
        {
            unlink_and_free(it);
        }
    }

    void execute(uint64_t order_id, uint32_t quantity)
    {
        auto it = id_to_order_.find(order_id);
        if (it == id_to_order_.end()) return;

        Order& o = orders_[it->second];
        assert(quantity <= o.shares && "over-execution");

        if (quantity == o.shares) {
            unlink_and_free(it);
        } else {
            o.shares -= quantity;
            Level& l = levels_[side_for(o.side).at(o.price)];
            l.quantity -= quantity;
        }
    }

    void update(uint64_t orig_ref, uint64_t new_ref, uint32_t new_price, uint32_t new_shares)
    {
        auto it = id_to_order_.find(orig_ref);
        if (it != id_to_order_.end())
        {
            Order& o = orders_[it->second];
            auto side = o.side;
            unlink_and_free(it);
            add_order(new_ref, side, new_price, new_shares);
        }
    }

    // Abandon all state — used only after a detected sequence gap, where the
    // book is unrecoverably inconsistent (a dropped message could have left
    // dangling order/level references). Not on any correctness path (gaps=0).
    void reset()
    {
        bids_.clear();
        asks_.clear();
        id_to_order_.clear();
        orders_.reset();
        levels_.reset();
    }

    struct ToB { uint32_t bid_px = 0, bid_qty = 0, ask_px = 0, ask_qty = 0; };

    ToB top_of_book() const
    {
        ToB t;
        if (!bids_.empty()) { const Level& l = levels_[bids_.rbegin()->second]; t.bid_px = l.price; t.bid_qty = l.quantity; }
        if (!asks_.empty()) { const Level& l = levels_[asks_.begin()->second]; t.ask_px = l.price; t.ask_qty = l.quantity; }
        return t;
    }
};