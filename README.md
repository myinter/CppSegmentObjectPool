# SegmentedObjectPool

## Overview / 概述
SegmentedObjectPool 是一个高性能 C++ 对象池模板，提供连续内存分配、快速获取和回收对象的能力。适用于即时消息、游戏数据或任何性能敏感的场景。

SegmentedObjectPool is a high-performance C++ object pool template that provides contiguous memory allocation, fast object acquisition, and recycling. Suitable for IM, game data, or other performance-sensitive scenarios.

## Features / 特性

- 统一分配对象 / Unified object allocation
- 避免对象频繁申请释放 / Avoid frequent allocation/deallocation
- 对象在内存中连续地址分配 / Objects allocated in contiguous memory
- 支持动态扩容 / Supports dynamic segment growth
- CRTP 模式的 PooledObject 支持统一创建与回收 / CRTP-based PooledObject supports unified create and recycle
- 可检查对象是否已被回收 / Check if an object is recycled

## Getting Started / 快速开始

### Include
```cpp
#include "SegmentedObjectPool.h"
```

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

std::cout << "Bullet1 at " << static_cast<void*>(b1) << "\n";
std::cout << "Bullet2 at " << static_cast<void*>(b2) << "\n";

b1->recycle();
b2->recycle();

Bullet* b3 = Bullet::create(50, 60);
std::cout << "Bullet3 at " << static_cast<void*>(b3) << "\n";

b3->recycle();
```

### Check Recycled / 检查是否回收
```cpp
if (b1->is_recycled()) {
    std::cout << "b1 has been recycled!" << std::endl;
}
```

## Platform Support / 平台支持
- Windows
- Linux
- macOS (Darwin)

## License / 许可证
MIT License


