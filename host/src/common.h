// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "gd102-arm64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#endif
#define LOG_TAG "gd102-arm64"
#define LOGI(...) do { fprintf(stderr, "[I/" LOG_TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOGW(...) do { fprintf(stderr, "[W/" LOG_TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOGE(...) do { fprintf(stderr, "[E/" LOG_TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#endif

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// Base of the 4 GiB guest address space in host virtual memory.
// Guest 32-bit address `a` maps to host pointer `g_base + a` with zero masking.
extern u8 *g_base;

inline void *g2h(u32 a) { return a ? (void *)(g_base + a) : nullptr; }
inline u32 h2g(const void *p) { return p ? (u32)((const u8 *)p - g_base) : 0; }
inline bool is_guest_ptr(const void *p) {
    auto d = (uintptr_t)p - (uintptr_t)g_base;
    return d >= 0x10000 && d < 0x100000000ULL;
}

template <class T> struct G {
    u32 a;
    T *get() const { return (T *)g2h(a); }
    operator T *() const { return get(); }
    explicit operator bool() const { return a != 0; }
};

template <class T> inline T rd(u32 a) {
    T v;
    memcpy(&v, g_base + a, sizeof(T));
    return v;
}
template <class T> inline void wr(u32 a, T v) { memcpy(g_base + a, &v, sizeof(T)); }

// Guest address space layout (within 4 GiB):
//   0x00000000 .. 0x00010000  null guard region (PROT_NONE, 64 KiB = 4x 16 KiB pages)
//   0x00010000 .. 0x00030000  SVC thunk stubs (one 4-byte `svc #id` instruction per host/JNI import)
//   0x00100000 .. 0x08000000  host-owned guest heap (TLS, JNI tables, dirent/FILE wrappers)
//   0x08000000 .. 0xFFF00000  ELF32 modules, guest dlmalloc mmap chunks, thread stacks
constexpr u32 GUEST_THUNK_BASE = 0x00010000;
constexpr u32 GUEST_THUNK_MAX  = 0x00008000;
constexpr u32 GUEST_HHEAP_BASE = 0x00100000;
constexpr u32 GUEST_HHEAP_SIZE = 0x07F00000;
constexpr u32 GUEST_MMAP_START = 0x08000000;
constexpr u32 GUEST_MMAP_END   = 0xFFF00000;

namespace mem {
void init();
size_t host_page_size();
u32 map(u32 size, u32 fixed_addr = 0);
void unmap(u32 addr, u32 size);
u32 halloc(u32 size, u32 align = 8);
void hfree(u32 addr);
u32 hstrdup(const char *s);
}  // namespace mem

void crash_note(const char *line);
[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
