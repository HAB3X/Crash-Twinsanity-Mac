#include "address.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Ee
{
namespace
{
constexpr u32 WindowShift = 26;
constexpr std::uintptr_t WindowMask = (std::uintptr_t{1} << WindowShift) - 1;
constexpr u32 Slots = 64;

std::atomic<std::uintptr_t> g_Windows[Slots];
std::mutex g_Mutex;
}

u32 DmaAddress(const void* pointer)
{
    if (pointer == nullptr)
    {
        return 0;
    }

    auto host = reinterpret_cast<std::uintptr_t>(pointer);
    std::uintptr_t window = host & ~WindowMask;
    for (u32 slot = 1; slot < Slots; slot++)
    {
        std::uintptr_t mapped = g_Windows[slot].load(std::memory_order_acquire);
        if (mapped == window)
        {
            return slot << WindowShift | static_cast<u32>(host & WindowMask);
        }

        if (mapped == 0)
        {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_Mutex);
    for (u32 slot = 1; slot < Slots; slot++)
    {
        std::uintptr_t mapped = g_Windows[slot].load(std::memory_order_acquire);
        if (mapped == window)
        {
            return slot << WindowShift | static_cast<u32>(host & WindowMask);
        }

        if (mapped == 0)
        {
            g_Windows[slot].store(window, std::memory_order_release);
            return slot << WindowShift | static_cast<u32>(host & WindowMask);
        }
    }

    std::fprintf(stderr, "graphics: more than %u windows of memory hold DMA data\n", Slots - 1);
    std::abort();
}

u8* HostAddress(u32 address)
{
    u32 slot = address >> WindowShift;
    std::uintptr_t window = g_Windows[slot].load(std::memory_order_acquire);
    if (slot == 0 || window == 0)
    {
        return nullptr;
    }

    return reinterpret_cast<u8*>(window | (address & WindowMask));
}
}
