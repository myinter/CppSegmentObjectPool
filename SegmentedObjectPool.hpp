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
 * 5. 申请空间大小依据操作系统的内存页面大小,最高效利用内存,杜绝内部碎片。并且以分段方式动态申请内存进行对象池扩容。 / The size of the allocated space is determined by the memory page size of the operating system. This ensures the most efficient use of memory and eliminates internal fragmentation. And dynamically apply for memory in segments to expand the object pool.
 * 6. 适用于即时消息、游戏数据等性能敏感场景 / Suitable for IM, game data, and other performance-sensitive scenarios
 *
 * 该对象池模板通过分段内存管理，提供快速的对象获取与回收，
 * 保证对象连续分配，同时减少操作系统频繁分配内存开销。
 *
 * This object pool template manages memory in segments,
 * providing fast object allocation and deallocation. Objects are allocated contiguously,
 * reducing OS allocation overhead, suitable for high-performance applications.
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

template <class T>
class SegmentedObjectPool {
    static_assert(!std::is_abstract<T>::value, "T must be a complete, non-abstract type");

    struct Segment {
        std::byte* data = nullptr;
        std::size_t capacity = 0;
        std::size_t next_uninit = 0;
        Segment() = default;
        Segment(std::byte* d, std::size_t cap) : data(d), capacity(cap), next_uninit(0) {}
    };

    struct FreeNode { FreeNode* next; };

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

    template <class... Args>
    T* allocate(Args&&... args) {
        if (free_list_) {
            FreeNode* node = free_list_;
            free_list_ = node->next;
            T* obj = reinterpret_cast<T*>(node);
            new (obj) T(std::forward<Args>(args)...);
            obj->mark_in_use();
            ++live_count_;
            return obj;
        }
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

    void deallocate(T* p) noexcept {
        if (!p) return;
        p->~T();
        FreeNode* node = reinterpret_cast<FreeNode*>(p);
        node->next = free_list_;
        free_list_ = node;
        --live_count_;
    }

    void clear() noexcept {
        for (auto& seg : segments_) {
            ::operator delete[](seg.data, std::align_val_t(alignof(T)));
            seg.data = nullptr;
        }
        segments_.clear();
        free_list_ = nullptr;
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
    FreeNode* free_list_ = nullptr;
    std::size_t page_size_ = detail::os_page_size();
    std::size_t slot_size_ = 0;
    std::size_t pages_per_segment_base_ = 0;
    double growth_factor_ = 1.0;
    std::size_t next_pages_hint_ = 0;
    std::size_t live_count_ = 0;
};

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

    bool is_recycled() const noexcept { return recycled_; }
    void mark_in_use() noexcept { recycled_ = false; }

private:
    bool recycled_ = false;
};
