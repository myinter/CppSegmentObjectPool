//
//  SegmentedObjectPool.hpp
//  Test
//
//  Created by hiung on 2024/6/13.
//

/*
 * SegmentedObjectPool 对象池模板 / Segmented Object Pool Template
 *
 * 功能 / Features:
 * 1. 统一分配对象 / Unified object allocation
 * 2. 避免对象反复申请释放 / Avoid frequent allocation/deallocation
 * 3. 对象在内存中连续地址分配 / Objects allocated in contiguous memory
 * 4. 对象连续分配，极大提高遍历对象的过程中的缓存命中率 / Continuous object allocation significantly reduces the occurrence of cache misses when accessing objects.
 * 5. 使用数组索引代替链表管理可用对象，提高内存局部性 / Use array indices instead of linked list to manage free objects, improving memory locality.
 * 6. 申请空间大小依据操作系统的内存页面大小,最高效利用内存,杜绝内部碎片。并且以分段方式动态申请内存进行对象池扩容。 / The size of the allocated space is determined by the memory page size of the operating system. This ensures the most efficient use of memory and eliminates internal fragmentation. And dynamically apply for memory in segments to expand the object pool.
 * 7. 适用于即时消息、游戏数据等性能敏感场景 / Suitable for IM, game data, and other performance-sensitive scenarios
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>
#include <memory>
#include <type_traits>
#include <cassert>
#include <utility>
#include <iostream>
#include <algorithm>
#include <unistd.h>

#if defined(_WIN32)
  #define NOMINMAX
  #include <windows.h>
#endif

namespace detail {
inline std::size_t os_page_size() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return static_cast<std::size_t>(si.dwPageSize ? si.dwPageSize : 4096);
#elif defined(_SC_PAGESIZE)
    long pz = ::sysconf(_SC_PAGESIZE);
    return static_cast<std::size_t>(pz > 0 ? pz : 4096);
#elif defined(_SC_PAGE_SIZE)
    long pz = ::sysconf(_SC_PAGE_SIZE);
    return static_cast<std::size_t>(pz > 0 ? pz : 4096);
#elif defined(__APPLE__) && defined(__MACH__)
    long page_size = getpagesize();
    return page_size;
#else
    return 4096;
#endif
}

constexpr std::size_t gcd(std::size_t a, std::size_t b) noexcept {
    while (b != 0) { auto t = a % b; a = b; b = t; }
    return a;
}
constexpr std::size_t lcm(std::size_t a, std::size_t b) noexcept {
    return (a / gcd(a,b)) * b;
}
constexpr std::size_t round_up(std::size_t x, std::size_t align) noexcept {
    if (align == 0) return x;
    std::size_t r = x % align;
    return r ? (x + (align - r)) : x;
}
} // namespace detail

// ----------------------------
// SegmentedObjectPool 定义 / Definition
// ----------------------------
template <class T>
class SegmentedObjectPool {
    static_assert(!std::is_abstract<T>::value, "T must be a complete, non-abstract type");

    struct Segment {
        std::byte* data = nullptr;                // 内存块 / Memory block
        std::size_t capacity = 0;                 // 可容纳对象数 / Number of objects
        std::size_t next_uninit = 0;              // 尚未构造的下一个索引 / Next uninitialized index
        std::vector<uint8_t> free_flags;          // 空闲标记数组 / Free slot flags (0 = used, 1 = free)
        std::size_t free_hint = 0;                // 下次分配的搜索起点 / Hint index for next search

        Segment() = default;
        Segment(std::byte* d, std::size_t cap) : data(d), capacity(cap), next_uninit(0), free_flags(cap, 0), free_hint(0) {}
    };

public:
    using value_type = T;

    static SegmentedObjectPool& instance() {
        static SegmentedObjectPool inst;
        return inst;
    }

    explicit SegmentedObjectPool(std::size_t min_pages_per_segment = 0, double growth = 1.0)
    : page_size_(detail::os_page_size()),
      slot_size_(detail::round_up(std::max(sizeof(T), sizeof(void*)), alignof(T))),
      pages_per_segment_base_(compute_min_pages(min_pages_per_segment)),
      growth_factor_(growth > 1.0 ? growth : 1.0) {}

    ~SegmentedObjectPool() { clear(); }
    SegmentedObjectPool(const SegmentedObjectPool&) = delete;
    SegmentedObjectPool& operator=(const SegmentedObjectPool&) = delete;

    // 分配对象 / Allocate object
    template <class... Args>
    T* allocate(Args&&... args) {
        // 1. 优先复用空闲对象 / Reuse freed objects first
        for (auto& seg : segments_) {
            for (std::size_t i = seg.free_hint; i < seg.capacity; ++i) {
                if (seg.free_flags[i] == 1) {
                    seg.free_flags[i] = 0; // 标记为占用 / Mark as used
                    seg.free_hint = i + 1; // 更新搜索起点 / Update search hint
                    T* obj = reinterpret_cast<T*>(seg.data + i * slot_size_);
                    new (obj) T(std::forward<Args>(args)...);
                    obj->mark_in_use();
                    ++live_count_;
                    return obj;
                }
            }
            // 如果未找到空闲，重置hint为0以便下次扫描 / Reset hint if no free found
            seg.free_hint = 0;
        }

        // 2. 分配未初始化空间 / Allocate from uninitialized slots
        if (!segments_.empty()) {
            Segment& seg = segments_.back();
            if (seg.next_uninit < seg.capacity) {
                std::byte* ptr = seg.data + seg.next_uninit * slot_size_;
                ++seg.next_uninit;
                T* obj = reinterpret_cast<T*>(ptr);
                new (obj) T(std::forward<Args>(args)...);
                obj->mark_in_use();
                ++live_count_;
                return obj;
            }
        }

        // 3. 如果没有空位，扩容新段 / Add new segment if full
        add_segment_();
        Segment& seg = segments_.back();
        std::byte* ptr = seg.data + seg.next_uninit * slot_size_;
        ++seg.next_uninit;
        T* obj = reinterpret_cast<T*>(ptr);
        new (obj) T(std::forward<Args>(args)...);
        obj->mark_in_use();
        ++live_count_;
        return obj;
    }

    // 回收对象 / Deallocate object
    void deallocate(T* p) noexcept {
        if (!p) return;
        // 找到所在的 segment 并标记空闲 / Locate segment and mark as free
        for (auto& seg : segments_) {
            std::uintptr_t base = reinterpret_cast<std::uintptr_t>(seg.data);
            std::uintptr_t end  = base + seg.capacity * slot_size_;
            std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
            if (addr >= base && addr < end) {
                std::size_t index = (addr - base) / slot_size_;
                p->~T();
                seg.free_flags[index] = 1; // 标记为空闲 / Mark as free
                if (index < seg.free_hint) seg.free_hint = index; // 更新hint，保证快速找到 / Update hint
                --live_count_;
                return;
            }
        }
    }

    // 清空池子 / Clear all memory
    void clear() noexcept {
        for (auto& seg : segments_) {
            ::operator delete[](seg.data, std::align_val_t(alignof(T)));
            seg.data = nullptr;
        }
        segments_.clear();
        live_count_ = 0;
        next_pages_hint_ = pages_per_segment_base_;
    }

    std::size_t live() const noexcept { return live_count_; }
    std::size_t segments() const noexcept { return segments_.size(); }
    std::size_t capacity_total() const noexcept {
        std::size_t c = 0; for (auto const& s : segments_) c += s.capacity; return c; }

private:
    std::size_t compute_min_pages(std::size_t user_min_pages) const noexcept {
        const std::size_t ps = page_size_;
        const std::size_t ss = slot_size_;
        const std::size_t l = detail::lcm(ps, ss);
        std::size_t min_pages = l / ps;
        if (user_min_pages > 0) {
            std::size_t k = (user_min_pages + min_pages - 1) / min_pages;
            min_pages *= k;
        }
        return min_pages;
    }

    void add_segment_() {
        if (segments_.empty()) {
            next_pages_hint_ = pages_per_segment_base_;
        } else {
            double target = static_cast<double>(next_pages_hint_) * growth_factor_;
            std::size_t pages = static_cast<std::size_t>(target);
            if (pages < next_pages_hint_ + pages_per_segment_base_)
                pages = next_pages_hint_ + pages_per_segment_base_;
            std::size_t rem = pages % pages_per_segment_base_;
            if (rem) pages += (pages_per_segment_base_ - rem);
            next_pages_hint_ = pages;
        }
        const std::size_t seg_bytes = next_pages_hint_ * page_size_;
        const std::size_t capacity = seg_bytes / slot_size_;
        std::byte* raw = reinterpret_cast<std::byte*>(::operator new[](seg_bytes, std::align_val_t(alignof(T))));
        segments_.emplace_back(raw, capacity);
    }

private:
    std::vector<Segment> segments_;
    std::size_t page_size_ = detail::os_page_size();
    std::size_t slot_size_ = 0;
    std::size_t pages_per_segment_base_ = 0;
    double growth_factor_ = 1.0;
    std::size_t next_pages_hint_ = 0;
    std::size_t live_count_ = 0;
};

// ----------------------------
// PooledObject 基类 / Base class for pooled objects
// ----------------------------
template <class Derived>
struct PooledObject {
    virtual ~PooledObject() = default;
    virtual void reset() {}

    static Derived* create(auto&&... args) {
        return SegmentedObjectPool<Derived>::instance().allocate(std::forward<decltype(args)>(args)...);
    }

    void recycle() {
        this->reset();
        recycled_ = true;
        SegmentedObjectPool<Derived>::instance().deallocate(static_cast<Derived*>(this));
    }

    inline bool is_recycled() const noexcept { return recycled_; }
    inline void mark_in_use() noexcept { recycled_ = false; }

private:
    bool recycled_ = false;
};
