// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/mman.h>

#ifdef PS5_NATIVE
extern "C" int sceKernelAvailableFlexibleMemorySize(std::size_t* size);
#endif

namespace Eden {
// A fiber's stack: one per guest thread (Eden's KThread runs on a fiber). Anonymous CPU mappings
// use the console's flexible pool, leaving direct memory for guest RAM and GPU resources. Guard
// pages surround the usable stack. When the flexible pool runs low, the stack comes from the heap,
// with canaries at both ends.
//
// Size. Eden gives every fiber 2 MiB, and on the PS5 a stack is memory from its first byte: about
// 150 guest threads in Mario Kart World took 302 MiB of the heap (core commit 9c83502ed8 had also
// moved them there from the flexible pool, where the port's edit no longer applied). A game's own
// threads run its code in the JIT and need little of their host stack; Eden's service threads run
// the emulated system (file systems, the GPU driver) and get more. EDEN_FIBER_STACK_KIB=<n> gives
// every fiber n KiB (the testing switch "Guest thread stacks": 1024, or 2048 as before).
class FiberStack {
public:
    static constexpr std::size_t GuardBytes = 16384;
    static constexpr std::size_t UserBytes = 512 * 1024;     // a game's own threads
    static constexpr std::size_t SystemBytes = 1024 * 1024;  // Eden's service, main and idle threads
    // Flexible memory left for everything else that maps it (thread stacks, the C library).
    static constexpr std::size_t FlexibleMargin = 96u << 20;

    // Set by KThread::InitializeThread for the fiber it makes next on this thread.
    inline static thread_local bool next_is_user = false;

    // The size of the next fiber's stack.
    static std::size_t NextSize() {
        static const std::size_t forced = [] {
            const char* value = std::getenv("EDEN_FIBER_STACK_KIB");
            const unsigned long kib = value != nullptr ? std::strtoul(value, nullptr, 10) : 0;
            if (kib == 0) return std::size_t{0};
            const std::size_t bytes = std::min<std::size_t>(std::max<unsigned long>(kib, 256), 8192) * 1024;
            return (bytes + GuardBytes - 1) / GuardBytes * GuardBytes;
        }();
        const bool user = next_is_user;
        next_is_user = false;
        return forced != 0 ? forced : user ? UserBytes : SystemBytes;
    }

    FiberStack() = default;
    FiberStack(const FiberStack&) = delete;
    FiberStack& operator=(const FiberStack&) = delete;
    ~FiberStack() {
        if (mapping) {
            if (munmap(mapping, bytes + 2 * GuardBytes)) std::abort();
            flexible_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        } else if (fallback) {
            if (std::memcmp(fallback.get(), Canary.data(), CanaryBytes) ||
                std::memcmp(base + bytes, Canary.data(), CanaryBytes)) std::abort();
            fallback_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
    }
    void Allocate(std::size_t size) {
        if (!size || size % GuardBytes || bytes) std::abort();
        bytes = size;
        std::size_t flexible_free = ~std::size_t{0};
#ifdef PS5_NATIVE
        if (sceKernelAvailableFlexibleMemorySize(&flexible_free) != 0) flexible_free = 0;
#endif
        void* reserved = flexible_free >= bytes + 2 * GuardBytes + FlexibleMargin
                             ? mmap(nullptr, bytes + 2 * GuardBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                             : MAP_FAILED;
        if (reserved != MAP_FAILED) {
            auto* usable = static_cast<unsigned char*>(reserved) + GuardBytes;
            if (mprotect(usable, bytes, PROT_READ | PROT_WRITE) == 0) {
                mapping = reserved;
                base = usable;
                flexible_bytes.fetch_add(bytes, std::memory_order_relaxed);
            } else if (munmap(reserved, bytes + 2 * GuardBytes)) {
                std::abort();
            }
        }
        if (!base) {
            fallback.reset(new unsigned char[bytes + 2 * CanaryBytes]);
            base = fallback.get() + CanaryBytes;
            std::memcpy(fallback.get(), Canary.data(), CanaryBytes);
            std::memcpy(base + bytes, Canary.data(), CanaryBytes);
            fallback_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        const auto count = allocations.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((count & (count - 1)) == 0 || (count % 64) == 0) {
            std::fprintf(stderr,
                         "EDEN_FIBER_STACK allocations=%llu flexible=%llu fallback=%llu stack_bytes=%zu "
                         "flexible_free=%zu\n",
                         count, flexible_bytes.load(), fallback_bytes.load(), bytes, flexible_free);
        }
    }
    unsigned char* data() noexcept { return base; }
    std::size_t size() const noexcept { return bytes; }
    inline static std::atomic<unsigned long long> flexible_bytes{}, fallback_bytes{}, allocations{};
private:
    inline static constexpr std::array<unsigned long long, 2> Canary{
        0x8d569a12e3c047bfULL, 0x729db60f24e1ac83ULL};
    static constexpr std::size_t CanaryBytes = sizeof(Canary);
    void* mapping{};
    unsigned char* base{};
    std::size_t bytes{};
    std::unique_ptr<unsigned char[]> fallback;
};
} // namespace Eden
