# SegmentedObjectPool

## Overview / 概述

SegmentedObjectPool 是一个高性能 C++ 对象池模板，提供连续内存分配、快速获取和回收对象的能力。适用于即时消息、游戏数据、交易系统或任何性能敏感的场景。

SegmentedObjectPool is a high-performance C++ object pool template that provides contiguous memory allocation, fast object acquisition, and recycling. Suitable for IM, game data, Exhanging System, or other performance-sensitive scenarios.

## Features / 特性

- 统一分配对象 / Unified object allocation
- 避免对象频繁申请释放 / Avoid frequent allocation/deallocation
- 对象在内存中连续地址分配 / Objects allocated in contiguous memory
- 支持动态扩容 / Supports dynamic segment growth
- CRTP 模式的 PooledObject 支持统一创建与回收 / CRTP-based PooledObject supports unified create and recycle
- 可检查对象是否已被回收 / Check if an object is recycled
- 索引区间管理空闲对象，提升局部性 / Manage free objects with bitmap and index tracking for better locality

## Implementation Notes / 实现说明

对象池使用 **Segment** 管理一片连续内存区域：

- 每个 Segment 内包含一个对象数组和一份位图 `used`，标记每个槽是否已使用。
- 使用栈索引，高效获得可用对象
## Getting Started / 快速开始

### Define a Pooled Object / 定义一个对象

```cpp
struct Bullet : public PooledObject<Bullet> {
    int x = 0;
    int y = 0;
    Bullet() = default;
    Bullet(int nx, int ny) : x(nx), y(ny) {}
    void reset() override { x = 0; y = 0; }
    void fire(int nx, int ny) { x = nx; y = ny; }
};
```

### Allocate / Recycle / 分配与回收

```cpp
    Bullet* b1 = Bullet::create(10, 20);
    Bullet* b2 = Bullet::create();
    b2->fire(30, 40);

    std::cout << "Bullet1: (" << b1->x << "," << b1->y << ") at " << static_cast<void*>(b1) << "\n";
    
    std::cout << "Size of Bullet " << sizeof(Bullet) << "\n";

    std::cout << "Bullet2: (" << b2->x << "," << b2->y << ") at " << static_cast<void*>(b2) << "\n";

    b1->recycle();
    b2->recycle();

    std::cout << "b1 recycled? " << b1->is_recycled() << "\n";
    std::cout << "b2 recycled? " << b2->is_recycled() << "\n";

    Bullet* b3 = Bullet::create(50, 60);
    Bullet* b4 = Bullet::create(230, 170);
    Bullet* b5 = Bullet::create(435, 520);


    std::cout << "Bullet3: (" << b3->x << "," << b3->y << ") at " << static_cast<void*>(b3) << "\n";
    std::cout << "Bullet4: (" << b4->x << "," << b4->y << ") at " << static_cast<void*>(b4) << "\n";
    std::cout << "b3 recycled? " << b3->is_recycled() << "\n";
    std::cout << "Bullet5: (" << b5->x << "," << b5->y << ") at " << static_cast<void*>(b5) << "\n";
    b3->recycle();
```

成功重用对象，并将对象分配在互相靠近的内存地址上。

Reuse the objects successfully. Allocated all objects close to each other in the same memory section.

```bash
Bullet1: (10,20) at 0x150008000   # 第一个对象，地址 0x150008000
                                  # First object, allocated at 0x150008000

Size of Bullet24                  # 输出对象大小信息
                                  # Output object size info

Bullet2: (30,40) at 0x150008018   # 第二个对象，地址紧邻第一个（相隔 0x18 字节）
                                  # Second object, address is right next to the first (offset 0x18)

b1 recycled? 1                    # Bullet1 已被回收复用
                                  # Bullet1 has been recycled and reused

b2 recycled? 1                    # Bullet2 已被回收复用
                                  # Bullet2 has been recycled and reused

Bullet3: (50,60) at 0x150008000   # Bullet3 复用了 Bullet1 的内存（地址相同）
                                  # Bullet3 reused Bullet1's memory (same address)

Bullet4: (230,170) at 0x150008018 # Bullet4 复用了 Bullet2 的内存（地址相同）
                                  # Bullet4 reused Bullet2's memory (same address)

b3 recycled? 0                    # Bullet3 当前仍在使用，没有被回收
                                  # Bullet3 is still in use, not yet recycled

Bullet5: (435,520) at 0x150008030 # 新分配的对象，地址继续向后排布，内存相邻
                                  # A newly allocated object, placed next in memory
                                    # (addresses are contiguous)
```

性能测试：分配对象，并对对象数组进行遍历的性能差距

Performance Test: The performance difference between allocating objects and traversing an array of objects

```bash

2500 objects, New allocating took 112 microseconds
2500 objects, Pool allocating took 30 microseconds

5000 objects, New allocating took 326 microseconds
5000 objects, Pool allocating took 73 microseconds

10000 objects, New allocating took 283 microseconds
10000 objects, Pool allocating took 74 microseconds

100000 objects, New allocating took 2954 microseconds
100000 objects, Pool allocating took 953 microseconds

```

进行大量对象分配和遍历时，性能差距可达到3-6倍。

During the process of performing a large number of object allocations and iterations, 
the performance gap can reach 3 to 6 times.

## Platform Support / 平台支持

- Windows
- Linux
- macOS (Darwin)

## License / 许可证

MIT License


