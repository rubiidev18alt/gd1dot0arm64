// SPDX-License-Identifier: MIT
#include "common.h"

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

// Host-side allocator inside the guest address space (TLS blocks, JNI vtables, FILE/DIR handles).
#define MSPACES 1
#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 0
#define FOOTERS 0
#define INSECURE 1
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "malloc.c"
#pragma GCC diagnostic pop
#ifdef __clang__
#pragma clang diagnostic pop
#endif

u8 *g_base = nullptr;

namespace mem {
namespace {
constexpr u32 GPAGE = 0x1000;  // 4 KiB guest page granularity expected by ARM32 ELF & dlmalloc
u32 gpage_up(u32 x) { return (x + GPAGE - 1) & ~(GPAGE - 1); }

size_t g_host_page = 4096;

struct Span {
    u32 addr;
    u32 size;
};
std::mutex g_mmap_lock;
std::vector<Span> g_free;

std::mutex g_hheap_lock;
mspace g_hspace = nullptr;
}  // namespace

size_t host_page_size() { return g_host_page; }

void init() {
    if (g_base) return;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0 && (ps & (ps - 1)) == 0) g_host_page = (size_t)ps;

    // Reserve 4 GiB + 64 KiB guard region so unmasked 32-bit offsets from g_base never escape.
    constexpr size_t RESERVE = 0x100010000ULL;
    void *p = mmap(nullptr, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("failed to reserve 4 GiB guest space: %s", strerror(errno));
    g_base = (u8 *)p;

    // Leave 0x00000000..0x00010000 (64 KiB, aligned to both 4 KiB and 16 KiB host pages) PROT_NONE
    // so guest NULL dereferences trap cleanly. Make 0x00010000..0x100000000 read-write on demand.
    if (mprotect(g_base + GUEST_THUNK_BASE, 0x100000000ULL - GUEST_THUNK_BASE, PROT_READ | PROT_WRITE) != 0)
        fatal("mprotect guest address space failed (host page %zu): %s", g_host_page, strerror(errno));

    g_free.push_back({GUEST_MMAP_START, GUEST_MMAP_END - GUEST_MMAP_START});
    g_hspace = create_mspace_with_base(g_base + GUEST_HHEAP_BASE, GUEST_HHEAP_SIZE, 0);
    if (!g_hspace) fatal("create_mspace_with_base failed");
    LOGI("guest 4 GiB space mapped at %p (host page size = %zu bytes)", g_base, g_host_page);
}

u32 map(u32 size, u32 fixed) {
    size = gpage_up(size ? size : 1);
    std::lock_guard<std::mutex> lk(g_mmap_lock);
    if (fixed) {
        u32 end = fixed + size;
        for (size_t i = 0; i < g_free.size(); i++) {
            Span s = g_free[i];
            if (s.addr <= fixed && end <= s.addr + s.size) {
                g_free.erase(g_free.begin() + (ptrdiff_t)i);
                if (fixed > s.addr) g_free.insert(g_free.begin() + (ptrdiff_t)i++, {s.addr, fixed - s.addr});
                if (end < s.addr + s.size) g_free.insert(g_free.begin() + (ptrdiff_t)i, {end, s.addr + s.size - end});
                return fixed;
            }
        }
        fatal("fixed guest mmap 0x%08x+0x%x overlaps existing allocation", fixed, size);
    }
    for (size_t i = 0; i < g_free.size(); i++) {
        if (g_free[i].size >= size) {
            u32 a = g_free[i].addr;
            g_free[i].addr += size;
            g_free[i].size -= size;
            if (g_free[i].size == 0) g_free.erase(g_free.begin() + (ptrdiff_t)i);
            return a;
        }
    }
    LOGE("guest address space exhausted requesting %u bytes", size);
    return 0;
}

void unmap(u32 addr, u32 size) {
    if (!addr || !size) return;
    size = gpage_up(size);

    // Android 15/16 16 KiB page-size compatibility:
    // Guest allocations have 4 KiB granularity, whereas madvise(MADV_DONTNEED) requires
    // host-page-aligned boundaries (`g_host_page`, 16384 on 16 KiB kernels) and fails with
    // EINVAL otherwise without zeroing memory. Zero partial head/tail sub-pages explicitly
    // and release any full interior host pages via MADV_DONTNEED.
    const uintptr_t ustart = addr;
    const uintptr_t uend = (uintptr_t)addr + size;
    const uintptr_t hstart = (ustart + g_host_page - 1) & ~(g_host_page - 1);
    const uintptr_t hend = uend & ~(g_host_page - 1);

    if (hstart < hend) {
        if (hstart > ustart) memset(g_base + ustart, 0, hstart - ustart);
        if (madvise(g_base + hstart, hend - hstart, MADV_DONTNEED) != 0)
            memset(g_base + hstart, 0, hend - hstart);
        if (uend > hend) memset(g_base + hend, 0, uend - hend);
    } else {
        memset(g_base + ustart, 0, uend - ustart);
    }

    std::lock_guard<std::mutex> lk(g_mmap_lock);
    auto it = std::lower_bound(g_free.begin(), g_free.end(), addr,
                               [](const Span &s, u32 a) { return s.addr < a; });
    it = g_free.insert(it, {addr, size});
    if (it + 1 != g_free.end() && it->addr + it->size == (it + 1)->addr) {
        it->size += (it + 1)->size;
        g_free.erase(it + 1);
    }
    if (it != g_free.begin() && (it - 1)->addr + (it - 1)->size == it->addr) {
        (it - 1)->size += it->size;
        g_free.erase(it);
    }
}

u32 halloc(u32 size, u32 align) {
    std::lock_guard<std::mutex> lk(g_hheap_lock);
    void *p = mspace_memalign(g_hspace, align, size ? size : 1);
    if (!p) fatal("host-owned guest heap exhausted (%u bytes)", size);
    memset(p, 0, size);
    return h2g(p);
}

void hfree(u32 addr) {
    if (!addr) return;
    std::lock_guard<std::mutex> lk(g_hheap_lock);
    mspace_free(g_hspace, g2h(addr));
}

u32 hstrdup(const char *s) {
    if (!s) s = "";
    u32 n = (u32)strlen(s) + 1;
    u32 a = halloc(n, 4);
    memcpy(g2h(a), s, n);
    return a;
}
}  // namespace mem
