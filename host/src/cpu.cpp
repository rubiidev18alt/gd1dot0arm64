// SPDX-License-Identifier: MIT
#include "cpu.h"
#include "loader.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unistd.h>

using namespace Dynarmic;

namespace {
constexpr int MAX_CPUS = 128;
ExclusiveMonitor *g_monitor = nullptr;
std::mutex g_cpu_ids_lock;
bool g_cpu_ids[MAX_CPUS] = {};

struct Thunk {
    const char *name;
    ThunkFn fn;
};
Thunk g_thunks[GUEST_THUNK_MAX];
std::atomic<u32> g_nthunks{0};
std::mutex g_thunk_lock;

thread_local GuestThread *t_cur = nullptr;
std::mutex g_all_lock;
std::vector<GuestThread *> g_all;

void report_null_access(GuestThread *t, u32 addr, const char *what);

// CP15 user thread ID registers (TPIDRURO / TPIDRURW) and ARMv6-style C7 memory barriers.
class CP15 final : public A32::Coprocessor {
public:
    explicit CP15(GuestThread *t) : t(t) {}
    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg,
                                                     unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2)
            return &tpidrurw;
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C7)
            return Callback{[](void *, u32, u32) -> u64 { return 0; }, std::nullopt};
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0) {
            if (opc2 == 3) return &t->tls_block;
            if (opc2 == 2) return &tpidrurw;
        }
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<u8>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<u8>) override { return std::nullopt; }

private:
    GuestThread *t;
    u32 tpidrurw = 0;
};
}  // namespace

struct GuestThread::Callbacks final : A32::UserCallbacks {
    GuestThread *t;
    bool svc_hit = false;
    u32 svc = 0;

    explicit Callbacks(GuestThread *t) : t(t) {}

    template <class T> T read(u32 a) {
        if (a < 0x10000) report_null_access(t, a, "read");
        T v;
        memcpy(&v, g_base + a, sizeof(v));
        return v;
    }
    template <class T> void write(u32 a, T v) {
        if (a < 0x10000) report_null_access(t, a, "write");
        memcpy(g_base + a, &v, sizeof(v));
    }
    std::optional<u32> MemoryReadCode(u32 a) override {
        if (a < 0x10000) return std::nullopt;
        return read<u32>(a);
    }
    u8 MemoryRead8(u32 a) override { return read<u8>(a); }
    u16 MemoryRead16(u32 a) override { return read<u16>(a); }
    u32 MemoryRead32(u32 a) override { return read<u32>(a); }
    u64 MemoryRead64(u32 a) override { return read<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { write(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { write(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { write(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { write(a, v); }

    template <class T> bool cas(u32 a, T v, T expected) {
        return __atomic_compare_exchange_n((T *)(g_base + a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive8(u32 a, u8 v, u8 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(u32 a, u16 v, u16 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(u32 a, u32 v, u32 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(u32 a, u64 v, u64 e) override { return cas(a, v, e); }

    void InterpreterFallback(u32 pc, size_t n) override {
        fatal("interpreter fallback at %s (%zu insns)", loader::symbolize(pc).c_str(), n);
    }
    void CallSVC(u32 swi) override {
        svc = swi;
        svc_hit = true;
        t->jit->HaltExecution();
    }
    void ExceptionRaised(u32 pc, A32::Exception e) override {
        switch (e) {
        case A32::Exception::Yield:
        case A32::Exception::WaitForEvent:
        case A32::Exception::WaitForInterrupt:
        case A32::Exception::SendEvent:
        case A32::Exception::SendEventLocal:
        case A32::Exception::PreloadData:
        case A32::Exception::PreloadDataWithIntentToWrite:
        case A32::Exception::PreloadInstruction:
            return;
        default:
            cpu::dump_state(*t);
            fatal("guest exception %d at %s", (int)e, loader::symbolize(pc).c_str());
        }
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1ULL << 40; }
};

namespace {
void report_null_access(GuestThread *t, u32 addr, const char *what) {
    cpu::dump_state(*t);
    fatal("guest null-page %s at 0x%08x", what, addr);
}

GuestThread *create_thread() {
    auto *t = new GuestThread;
    {
        std::lock_guard<std::mutex> lk(g_cpu_ids_lock);
        int id = 0;
        while (id < MAX_CPUS && g_cpu_ids[id]) id++;
        if (id == MAX_CPUS) fatal("too many guest threads");
        g_cpu_ids[id] = true;
        t->processor_id = id;
    }
    t->stack_size = 1024 * 1024;
    t->stack_base = mem::map(t->stack_size);
    // Bionic LP32 TLS layout: slot 0 = self pointer, slot 2 = errno storage.
    t->tls_block = mem::halloc(64 * 4, 16);
    wr<u32>(t->tls_block, t->tls_block);
    t->errno_addr = t->tls_block + 2 * 4;

    t->cb = std::make_unique<GuestThread::Callbacks>(t);
    A32::UserConfig conf{};
    conf.callbacks = t->cb.get();
    conf.processor_id = t->processor_id;
    conf.global_monitor = g_monitor;
    conf.arch_version = A32::ArchVersion::v8;
    conf.fastmem_pointer = (uintptr_t)g_base;
    conf.recompile_on_fastmem_failure = true;
    conf.enable_cycle_counting = false;
    conf.define_unpredictable_behaviour = true;
    conf.code_cache_size = t->processor_id == 0 ? 128 * 1024 * 1024 : 64 * 1024 * 1024;
    conf.coprocessors[15] = std::make_shared<CP15>(t);

    t->jit = std::make_unique<A32::Jit>(conf);
    t->jit->SetCpsr(0x10);  // USR32, ARM state
    t->jit->SetFpscr(0);

    std::lock_guard<std::mutex> lk(g_all_lock);
    g_all.push_back(t);
    return t;
}

void run(GuestThread &t) {
    auto &cb = *t.cb;
    auto &R = t.jit->Regs();
    for (;;) {
        HaltReason hr = t.jit->Run();
        if (cb.svc_hit) {
            cb.svc_hit = false;
            u32 id = cb.svc;
            if (id == cpu::SVC_RETURN) return;
            if (id >= g_nthunks.load(std::memory_order_acquire)) {
                cpu::dump_state(t);
                fatal("bad svc #%u", id);
            }
            t.cur_svc = id;
            t.pc_changed = false;
            g_thunks[id].fn(t);
            if (!t.pc_changed) {
                u32 lr = R[14];
                R[15] = lr & ~1u;
                t.jit->SetCpsr((t.jit->Cpsr() & ~0x20u) | ((lr & 1u) << 5));
            }
            continue;
        }
        if ((u32)hr & (u32)HaltReason::UserDefined2) {
            cpu::dump_state(t);
            continue;
        }
        cpu::dump_state(t);
        fatal("unexpected guest halt 0x%x", (unsigned)hr);
    }
}
}  // namespace

u32 &GuestThread::r(int i) { return jit->Regs()[i]; }
u32 GuestThread::sp() { return jit->Regs()[13]; }
u32 GuestThread::cpsr() { return jit->Cpsr(); }

namespace {
thread_local u32 t_assigned_handle = 0;
}
void cpu_assign_handle(u32 h) { t_assigned_handle = h; }

GuestThread &cur_thread() {
    if (!t_cur) {
        t_cur = create_thread();
        extern u32 pthread_register_foreign(GuestThread *);
        t_cur->handle = t_assigned_handle ? t_assigned_handle : pthread_register_foreign(t_cur);
    }
    return *t_cur;
}

GuestThread *cur_thread_or_null() { return t_cur; }

void destroy_cur_thread() {
    GuestThread *t = t_cur;
    if (!t) return;
    t_cur = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_cpu_ids_lock);
        g_cpu_ids[t->processor_id] = false;
    }
    g_monitor->ClearProcessor(t->processor_id);
    {
        std::lock_guard<std::mutex> lk(g_all_lock);
        g_all.erase(std::find(g_all.begin(), g_all.end(), t));
    }
    mem::unmap(t->stack_base, t->stack_size);
    mem::hfree(t->tls_block);
    delete t;
}

namespace cpu {
void request_dump_all() {
    std::lock_guard<std::mutex> lk(g_all_lock);
    for (GuestThread *t : g_all) t->jit->HaltExecution(HaltReason::UserDefined2);
}

void init() {
    if (g_monitor) return;
    g_monitor = new ExclusiveMonitor(MAX_CPUS);
    add_thunk("<return>", nullptr);
}

u64 call(u32 fn, const u32 *args, size_t n) {
    GuestThread &t = cur_thread();
    auto &R = t.jit->Regs();
    auto saved_regs = R;
    auto saved_ext = t.jit->ExtRegs();
    u32 saved_cpsr = t.jit->Cpsr();
    u32 saved_fpscr = t.jit->Fpscr();
    bool saved_pc_changed = t.pc_changed;
    u32 saved_cur_svc = t.cur_svc;

    u32 sp = t.depth == 0 ? (t.stack_base + t.stack_size) : (R[13] - 16);
    const u32 *it = args;
    for (size_t i = 0; i < n && i < 4; i++) R[i] = *it++;
    if (n > 4) {
        sp -= (u32)(n - 4) * 4;
        sp &= ~7u;
        for (size_t i = 4; i < n; i++) wr<u32>(sp + (u32)(i - 4) * 4, *it++);
    } else {
        sp &= ~7u;
    }
    R[13] = sp;
    R[14] = GUEST_THUNK_BASE + 4 * SVC_RETURN;
    R[15] = fn & ~1u;
    t.jit->SetCpsr((saved_cpsr & ~0x20u) | 0x10u | ((fn & 1u) << 5));

    t.depth++;
    run(t);
    t.depth--;

    u64 ret = R[0] | ((u64)R[1] << 32);
    R = saved_regs;
    t.jit->ExtRegs() = saved_ext;
    t.jit->SetCpsr(saved_cpsr);
    t.jit->SetFpscr(saved_fpscr);
    t.pc_changed = saved_pc_changed;
    t.cur_svc = saved_cur_svc;
    return ret;
}

u32 add_thunk(const char *name, ThunkFn fn) {
    std::lock_guard<std::mutex> lk(g_thunk_lock);
    u32 id = g_nthunks.load();
    if (id >= GUEST_THUNK_MAX) fatal("too many thunks");
    g_thunks[id] = {strdup(name), fn};
    u32 addr = GUEST_THUNK_BASE + id * 4;
    wr<u32>(addr, 0xEF000000u | id);  // ARM: svc #id
    g_nthunks.store(id + 1, std::memory_order_release);
    return addr;
}

const char *thunk_name(u32 id) { return id < g_nthunks.load() ? g_thunks[id].name : "?"; }

void dump_state(GuestThread &t) {
    auto line = [](const char *fmt, auto... args) {
        char buf[512];
        snprintf(buf, sizeof(buf), fmt, args...);
        LOGE("%s", buf);
        crash_note(buf);
    };
    auto &R = t.jit->Regs();
    line("guest thread %u (depth=%d) state: cpsr=%08x", t.handle, t.depth, t.jit->Cpsr());
    for (int i = 0; i < 16; i += 4)
        line("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, R[i], i + 1, R[i + 1], i + 2, R[i + 2], i + 3, R[i + 3]);
    line("  pc %s", loader::symbolize(R[15]).c_str());
    line("  lr %s", loader::symbolize(R[14]).c_str());
    u32 sp = R[13];
    int found = 0;
    for (u32 a = sp; a < sp + 2048 && found < 24; a += 4) {
        if (a < t.stack_base || a >= t.stack_base + t.stack_size) break;
        u32 v = rd<u32>(a);
        Module *m = loader::find_by_addr(v);
        if (m && v - m->base < m->size && !m->name.empty()) {
            line("  stack[%03x] %s", a - sp, loader::symbolize(v).c_str());
            found++;
        }
    }
}
}  // namespace cpu
