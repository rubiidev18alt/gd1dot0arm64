// SPDX-License-Identifier: MIT
#pragma once
#include "common.h"

#include <initializer_list>
#include <memory>

namespace Dynarmic::A32 {
class Jit;
}

struct GuestThread;
using ThunkFn = void (*)(GuestThread &);

struct GuestThread {
    struct Callbacks;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::unique_ptr<Callbacks> cb;
    int processor_id = 0;

    u32 stack_base = 0;
    u32 stack_size = 0;
    u32 tls_block = 0;
    u32 errno_addr = 0;

    u32 handle = 0;
    std::vector<u32> tls_values;

    int depth = 0;
    bool pc_changed = false;
    u32 cur_svc = 0;

    u32 &r(int i);
    u32 sp();
    u32 cpsr();

    u32 arg(int idx) { return idx < 4 ? r(idx) : rd<u32>(sp() + (u32)(idx - 4) * 4); }
    void ret32(u32 v) { r(0) = v; }
    void ret64(u64 v) {
        r(0) = (u32)v;
        r(1) = (u32)(v >> 32);
    }
    void set_errno(int e) { wr<s32>(errno_addr, e); }
};

struct GuestThreadExit {
    u32 value;
};

GuestThread &cur_thread();
GuestThread *cur_thread_or_null();
void destroy_cur_thread();
void cpu_assign_handle(u32 h);

namespace cpu {
constexpr u32 SVC_RETURN = 0;

void init();
u64 call(u32 fn, const u32 *args, size_t nargs);
inline u64 call(u32 fn, std::initializer_list<u32> args) { return call(fn, args.begin(), args.size()); }
inline u32 call32(u32 fn, std::initializer_list<u32> args) { return (u32)call(fn, args); }

u32 add_thunk(const char *name, ThunkFn fn);
const char *thunk_name(u32 id);
void dump_state(GuestThread &t);
void request_dump_all();
}  // namespace cpu
