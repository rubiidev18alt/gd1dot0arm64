// SPDX-License-Identifier: MIT
#include "jni_bridge.h"
#include "cpu.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jni {
namespace {

JavaVM *g_vm = nullptr;
u32 g_env_table = 0;
u32 g_vm_table = 0;
u32 g_gvm = 0;

std::mutex g_cl_lock;
jobject g_class_loader = nullptr;
jmethodID g_load_class_mid = nullptr;

// Tagged 32-bit jobject handles:
//   tag 1 -> thread-local reference
//   tag 2 -> global reference
//   tag 3 -> weak global reference
std::mutex g_ref_lock;
std::vector<jobject> g_globals;
std::vector<u32> g_globals_free;
std::vector<jweak> g_weaks;
std::vector<u32> g_weaks_free;

std::mutex g_id_lock;
std::vector<jmethodID> g_mids;
std::vector<std::string> g_msigs;
std::unordered_map<jmethodID, u32> g_mid_map;
std::vector<jfieldID> g_fids;
std::unordered_map<jfieldID, u32> g_fid_map;

struct Mirror {
    void *host;
    u32 guest;
    u32 size;
    jobject global_ref;
};
std::mutex g_mirror_lock;
std::unordered_map<void *, Mirror> g_mirrors;

struct ThreadJNI {
    JNIEnv *env = nullptr;
    u32 genv = 0;
    std::vector<jobject> locals;
    std::vector<size_t> frames;
    bool attached_by_us = false;
    ~ThreadJNI() {
        if (genv) mem::hfree(genv);
        if (env && attached_by_us && g_vm) g_vm->DetachCurrentThread();
    }
};
thread_local ThreadJNI t_state;

JNIEnv *host_env() {
    if (t_state.env) return t_state.env;
    JNIEnv *e = nullptr;
    if (g_vm->GetEnv((void **)&e, JNI_VERSION_1_6) != JNI_OK) {
#ifdef __ANDROID__
        if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) fatal("AttachCurrentThread failed");
#else
        if (g_vm->AttachCurrentThread((void **)&e, nullptr) != JNI_OK) fatal("AttachCurrentThread failed");
#endif
        t_state.attached_by_us = true;
    }
    t_state.env = e;
    return e;
}

template <class T> u32 add_ref(std::vector<T> &vec, std::vector<u32> &freelist, T ref, u32 tag) {
    if (!ref) return 0;
    std::lock_guard<std::mutex> lk(g_ref_lock);
    u32 idx;
    if (!freelist.empty()) {
        idx = freelist.back();
        freelist.pop_back();
        vec[idx] = ref;
    } else {
        idx = (u32)vec.size();
        vec.push_back(ref);
    }
    return ((idx + 1) << 2) | tag;
}

u32 mid_to_guest(jmethodID id, const char *sig) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lk(g_id_lock);
    auto it = g_mid_map.find(id);
    if (it != g_mid_map.end()) return it->second;
    u32 h = (u32)g_mids.size() + 1;
    g_mids.push_back(id);
    g_msigs.emplace_back(sig);
    g_mid_map[id] = h;
    return h;
}
jmethodID guest_to_mid(u32 h, const char **sig = nullptr) {
    if (!h) return nullptr;
    std::lock_guard<std::mutex> lk(g_id_lock);
    if (sig) *sig = g_msigs[h - 1].c_str();
    return g_mids[h - 1];
}
u32 fid_to_guest(jfieldID id) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lk(g_id_lock);
    auto it = g_fid_map.find(id);
    if (it != g_fid_map.end()) return it->second;
    u32 h = (u32)g_fids.size() + 1;
    g_fids.push_back(id);
    g_fid_map[id] = h;
    return h;
}
jfieldID guest_to_fid(u32 h) {
    if (!h) return nullptr;
    std::lock_guard<std::mutex> lk(g_id_lock);
    return g_fids[h - 1];
}

struct Args {
    GuestThread &t;
    int pos = 0;
    explicit Args(GuestThread &t) : t(t) {}
    u32 w() {
        u32 v = pos < 4 ? t.r(pos) : rd<u32>(t.sp() + (u32)(pos - 4) * 4);
        pos++;
        return v;
    }
    u64 d() {
        if (pos & 1) pos++;
        u32 lo = w(), hi = w();
        return lo | ((u64)hi << 32);
    }
    jobject o() { return from_guest(w()); }
};

void build_jvalues(const char *sig, std::vector<jvalue> &out, auto &&w32, auto &&w64) {
    out.clear();
    const char *p = sig;
    if (*p == '(') p++;
    while (*p && *p != ')') {
        jvalue v{};
        char c = *p++;
        if (c == '[') {
            while (*p == '[') p++;
            if (*p == 'L') while (*p && *p != ';') p++;
            if (*p) p++;
            v.l = from_guest(w32());
        } else if (c == 'L') {
            while (*p && *p != ';') p++;
            if (*p) p++;
            v.l = from_guest(w32());
        } else if (c == 'J') {
            v.j = (jlong)w64();
        } else if (c == 'D') {
            u64 b = w64();
            memcpy(&v.d, &b, 8);
        } else if (c == 'F') {
            u32 b = w32();
            memcpy(&v.f, &b, 4);
        } else if (c == 'Z') {
            v.z = (jboolean)(w32() & 1);
        } else if (c == 'B') {
            v.b = (jbyte)w32();
        } else if (c == 'C') {
            v.c = (jchar)w32();
        } else if (c == 'S') {
            v.s = (jshort)w32();
        } else {
            v.i = (jint)w32();
        }
        out.push_back(v);
    }
}

void collect_args(Args &a, int style, const char *sig, std::vector<jvalue> &out) {
    if (style == 0) {
        build_jvalues(sig, out, [&] { return a.w(); }, [&] { return a.d(); });
    } else if (style == 1) {
        u32 va = a.w();
        build_jvalues(
            sig, out,
            [&] {
                u32 v = rd<u32>(va);
                va += 4;
                return v;
            },
            [&] {
                va = (va + 7) & ~7u;
                u64 v = rd<u64>(va);
                va += 8;
                return v;
            });
    } else {
        u32 arr = a.w();
        int idx = 0;
        build_jvalues(sig, out, [&] { return rd<u32>(arr + (idx++) * 8); }, [&] { return rd<u64>(arr + (idx++) * 8); });
    }
}

JNIEnv *E() { return host_env(); }

template <char RET, int STYLE, int KIND> void jni_call(GuestThread &t) {
    Args a(t);
    a.w();
    jobject recv = a.o();
    jclass cls = KIND == 1 ? (jclass)a.o() : (jclass)recv;
    const char *sig = nullptr;
    jmethodID mid = guest_to_mid(a.w(), &sig);
    if (!mid || (KIND != 2 && !recv) || (KIND >= 1 && !cls)) {
        if constexpr (RET == 'J' || RET == 'D') t.ret64(0);
        else if constexpr (RET != 'V') t.ret32(0);
        return;
    }
    std::vector<jvalue> jv;
    collect_args(a, STYLE, sig, jv);
    const jvalue *p = jv.empty() ? nullptr : jv.data();
    JNIEnv *e = E();
    if (e->ExceptionCheck()) e->ExceptionClear();

#define DISPATCH(Type, ctype, setret)                                              \
    do {                                                                           \
        ctype r{};                                                                 \
        if constexpr (KIND == 0) r = e->Call##Type##MethodA(recv, mid, p);         \
        else if constexpr (KIND == 1) r = e->CallNonvirtual##Type##MethodA(recv, cls, mid, p); \
        else r = e->CallStatic##Type##MethodA(cls, mid, p);                        \
        if (e->ExceptionCheck()) {                                                 \
            e->ExceptionDescribe();                                                \
            e->ExceptionClear();                                                   \
            r = ctype{};                                                           \
        }                                                                          \
        setret;                                                                    \
    } while (0)

    if constexpr (RET == 'V') {
        if constexpr (KIND == 0) e->CallVoidMethodA(recv, mid, p);
        else if constexpr (KIND == 1) e->CallNonvirtualVoidMethodA(recv, cls, mid, p);
        else e->CallStaticVoidMethodA(cls, mid, p);
        if (e->ExceptionCheck()) {
            e->ExceptionDescribe();
            e->ExceptionClear();
        }
    } else if constexpr (RET == 'L') {
        DISPATCH(Object, jobject, t.ret32(to_guest(r)));
    } else if constexpr (RET == 'Z') {
        DISPATCH(Boolean, jboolean, t.ret32(r));
    } else if constexpr (RET == 'B') {
        DISPATCH(Byte, jbyte, t.ret32((u32)(s32)r));
    } else if constexpr (RET == 'C') {
        DISPATCH(Char, jchar, t.ret32(r));
    } else if constexpr (RET == 'S') {
        DISPATCH(Short, jshort, t.ret32((u32)(s32)r));
    } else if constexpr (RET == 'I') {
        DISPATCH(Int, jint, t.ret32((u32)r));
    } else if constexpr (RET == 'J') {
        DISPATCH(Long, jlong, t.ret64((u64)r));
    } else if constexpr (RET == 'F') {
        DISPATCH(Float, jfloat, ({ u32 b; memcpy(&b, &r, 4); t.ret32(b); }));
    } else if constexpr (RET == 'D') {
        DISPATCH(Double, jdouble, ({ u64 b; memcpy(&b, &r, 8); t.ret64(b); }));
    }
#undef DISPATCH
}

template <int STYLE> void jni_new_object(GuestThread &t) {
    Args a(t);
    a.w();
    jclass cls = (jclass)a.o();
    const char *sig = nullptr;
    jmethodID mid = guest_to_mid(a.w(), &sig);
    if (!cls || !mid) return t.ret32(0);
    std::vector<jvalue> jv;
    collect_args(a, STYLE, sig, jv);
    JNIEnv *e = E();
    if (e->ExceptionCheck()) e->ExceptionClear();
    jobject obj = e->NewObjectA(cls, mid, jv.empty() ? nullptr : jv.data());
    if (e->ExceptionCheck()) {
        e->ExceptionDescribe();
        e->ExceptionClear();
        obj = nullptr;
    }
    t.ret32(to_guest(obj));
}

template <char K, bool STATIC> void jni_get_field(GuestThread &t) {
    Args a(t);
    a.w();
    jobject target = a.o();
    jfieldID fid = guest_to_fid(a.w());
    JNIEnv *e = E();
#define GF(Type, ctype, setret)                                                    \
    do {                                                                           \
        ctype r = STATIC ? e->GetStatic##Type##Field((jclass)target, fid) : e->Get##Type##Field(target, fid); \
        setret;                                                                    \
    } while (0)
    if constexpr (K == 'L') GF(Object, jobject, t.ret32(to_guest(r)));
    else if constexpr (K == 'Z') GF(Boolean, jboolean, t.ret32(r));
    else if constexpr (K == 'B') GF(Byte, jbyte, t.ret32((u32)(s32)r));
    else if constexpr (K == 'C') GF(Char, jchar, t.ret32(r));
    else if constexpr (K == 'S') GF(Short, jshort, t.ret32((u32)(s32)r));
    else if constexpr (K == 'I') GF(Int, jint, t.ret32((u32)r));
    else if constexpr (K == 'J') GF(Long, jlong, t.ret64((u64)r));
    else if constexpr (K == 'F') GF(Float, jfloat, ({ u32 b; memcpy(&b, &r, 4); t.ret32(b); }));
    else if constexpr (K == 'D') GF(Double, jdouble, ({ u64 b; memcpy(&b, &r, 8); t.ret64(b); }));
#undef GF
}

template <char K, bool STATIC> void jni_set_field(GuestThread &t) {
    Args a(t);
    a.w();
    jobject target = a.o();
    jfieldID fid = guest_to_fid(a.w());
    JNIEnv *e = E();
#define SF(Type, val)                                                              \
    do {                                                                           \
        if constexpr (STATIC) e->SetStatic##Type##Field((jclass)target, fid, val); \
        else e->Set##Type##Field(target, fid, val);                                \
    } while (0)
    if constexpr (K == 'L') SF(Object, a.o());
    else if constexpr (K == 'Z') SF(Boolean, (jboolean)(a.w() & 1));
    else if constexpr (K == 'B') SF(Byte, (jbyte)a.w());
    else if constexpr (K == 'C') SF(Char, (jchar)a.w());
    else if constexpr (K == 'S') SF(Short, (jshort)a.w());
    else if constexpr (K == 'I') SF(Int, (jint)a.w());
    else if constexpr (K == 'J') SF(Long, (jlong)a.d());
    else if constexpr (K == 'F') {
        u32 b = a.w();
        float f;
        memcpy(&f, &b, 4);
        SF(Float, f);
    } else if constexpr (K == 'D') {
        u64 b = a.d();
        double d;
        memcpy(&d, &b, 8);
        SF(Double, d);
    }
#undef SF
}

int elem_size(char k) {
    switch (k) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default: return 8;
    }
}

template <char K> void jni_new_array(GuestThread &t) {
    Args a(t);
    a.w();
    jsize n = (jsize)a.w();
    JNIEnv *e = E();
    jobject r = nullptr;
    if constexpr (K == 'Z') r = e->NewBooleanArray(n);
    else if constexpr (K == 'B') r = e->NewByteArray(n);
    else if constexpr (K == 'C') r = e->NewCharArray(n);
    else if constexpr (K == 'S') r = e->NewShortArray(n);
    else if constexpr (K == 'I') r = e->NewIntArray(n);
    else if constexpr (K == 'J') r = e->NewLongArray(n);
    else if constexpr (K == 'F') r = e->NewFloatArray(n);
    else if constexpr (K == 'D') r = e->NewDoubleArray(n);
    t.ret32(to_guest(r));
}

template <char K> void array_region(JNIEnv *e, jarray arr, jsize start, jsize len, void *buf, bool set) {
#define AR(Type, ctype)                                                            \
    do {                                                                           \
        if (set) e->Set##Type##ArrayRegion((ctype##Array)arr, start, len, (const ctype *)buf); \
        else e->Get##Type##ArrayRegion((ctype##Array)arr, start, len, (ctype *)buf); \
    } while (0)
    if constexpr (K == 'Z') AR(Boolean, jboolean);
    else if constexpr (K == 'B') AR(Byte, jbyte);
    else if constexpr (K == 'C') AR(Char, jchar);
    else if constexpr (K == 'S') AR(Short, jshort);
    else if constexpr (K == 'I') AR(Int, jint);
    else if constexpr (K == 'J') AR(Long, jlong);
    else if constexpr (K == 'F') AR(Float, jfloat);
    else if constexpr (K == 'D') AR(Double, jdouble);
#undef AR
}

template <char K> void jni_get_elements(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    jsize n = e->GetArrayLength(arr);
    u32 g = mem::halloc((u32)(n * elem_size(K)) + 8);
    array_region<K>(e, arr, 0, n, g2h(g), false);
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}

template <char K> void jni_release_elements(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    u32 elems = a.w();
    s32 mode = (s32)a.w();
    JNIEnv *e = E();
    if (mode != JNI_ABORT) {
        jsize n = e->GetArrayLength(arr);
        array_region<K>(e, arr, 0, n, g2h(elems), true);
    }
    if (mode != JNI_COMMIT) mem::hfree(elems);
}

template <char K> void jni_get_region(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    jsize start = (jsize)a.w(), len = (jsize)a.w();
    array_region<K>(E(), arr, start, len, g2h(a.w()), false);
}
template <char K> void jni_set_region(GuestThread &t) {
    Args a(t);
    a.w();
    jarray arr = (jarray)a.o();
    jsize start = (jsize)a.w(), len = (jsize)a.w();
    array_region<K>(E(), arr, start, len, g2h(a.w()), true);
}

char array_kind(JNIEnv *e, jarray arr) {
    static jclass z, b, c, s, i, j, f;
    static std::once_flag once;
    std::call_once(once, [&] {
        auto mk = [&](const char *n) {
            jclass loc = e->FindClass(n);
            jclass glob = (jclass)e->NewGlobalRef(loc);
            e->DeleteLocalRef(loc);
            return glob;
        };
        z = mk("[Z"); b = mk("[B"); c = mk("[C"); s = mk("[S");
        i = mk("[I"); j = mk("[J"); f = mk("[F");
    });
    if (e->IsInstanceOf(arr, b)) return 'B';
    if (e->IsInstanceOf(arr, i)) return 'I';
    if (e->IsInstanceOf(arr, f)) return 'F';
    if (e->IsInstanceOf(arr, s)) return 'S';
    if (e->IsInstanceOf(arr, c)) return 'C';
    if (e->IsInstanceOf(arr, z)) return 'Z';
    if (e->IsInstanceOf(arr, j)) return 'J';
    return 'D';
}

// Convert arbitrary C byte string into valid JNI Modified UTF-8 so Android ART CheckJNI never
// aborts on 4-byte UTF-8 emojis (U+10000..U+10FFFF) or legacy Latin-1 characters in GD levels.
std::string sanitize_modified_utf8(const char *src) {
    const auto *s = (const u8 *)src;
    std::string out;
    out.reserve(strlen(src) + 8);
    while (*s) {
        u8 b0 = *s;
        if (b0 < 0x80) {
            out.push_back((char)b0);
            s++;
            continue;
        }
        // Valid 2-byte UTF-8 (U+0080..U+07FF)
        if (b0 >= 0xC2 && b0 <= 0xDF && (s[1] & 0xC0) == 0x80) {
            out.push_back((char)b0);
            out.push_back((char)s[1]);
            s += 2;
            continue;
        }
        // Valid 3-byte UTF-8 (U+0800..U+FFFF, excluding UTF-16 surrogates only if malformed)
        if (b0 >= 0xE0 && b0 <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            u32 cp = ((u32)(b0 & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (u32)(s[2] & 0x3F);
            if (cp >= 0x0800) {
                out.push_back((char)b0);
                out.push_back((char)s[1]);
                out.push_back((char)s[2]);
                s += 3;
                continue;
            }
        }
        // Standard 4-byte UTF-8 (U+10000..U+10FFFF) -> CESU-8 / Modified UTF-8 surrogate pair
        if (b0 >= 0xF0 && b0 <= 0xF4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            u32 cp = ((u32)(b0 & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12) | ((u32)(s[2] & 0x3F) << 6) | (u32)(s[3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                cp -= 0x10000;
                u32 hi = 0xD800 | (cp >> 10);
                u32 lo = 0xDC00 | (cp & 0x3FF);
                auto emit3 = [&](u32 u) {
                    out.push_back((char)(0xE0 | (u >> 12)));
                    out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (u & 0x3F)));
                };
                emit3(hi);
                emit3(lo);
                s += 4;
                continue;
            }
        }
        // Fallback: interpret stray byte as Latin-1 U+0080..U+00FF
        out.push_back((char)(0xC0 | (b0 >> 6)));
        out.push_back((char)(0x80 | (b0 & 0x3F)));
        s++;
    }
    return out;
}

void j_GetVersion(GuestThread &t) { t.ret32(JNI_VERSION_1_6); }

void j_FindClass(GuestThread &t) {
    Args a(t);
    a.w();
    const char *name = (const char *)g2h(a.w());
    if (!name) return t.ret32(0);
    JNIEnv *e = E();
    if (e->ExceptionCheck()) e->ExceptionClear();
    jclass cls = e->FindClass(name);
    if (!cls || e->ExceptionCheck()) {
        jobject cl = nullptr;
        jmethodID mid = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_cl_lock);
            cl = g_class_loader;
            mid = g_load_class_mid;
        }
        if (cl && mid) {
            e->ExceptionClear();
            std::string dotted(name);
            for (char &ch : dotted)
                if (ch == '/') ch = '.';
            jstring jname = e->NewStringUTF(dotted.c_str());
            cls = (jclass)e->CallObjectMethod(cl, mid, jname);
            e->DeleteLocalRef(jname);
        }
    }
    if (!cls || e->ExceptionCheck()) {
        e->ExceptionClear();
        return t.ret32(0);
    }
    t.ret32(to_guest(cls));
}

void j_GetSuperclass(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->GetSuperclass((jclass)a.o()))); }
void j_IsAssignableFrom(GuestThread &t) {
    Args a(t); a.w();
    jclass c1 = (jclass)a.o(), c2 = (jclass)a.o();
    t.ret32(E()->IsAssignableFrom(c1, c2));
}
void j_Throw(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->Throw((jthrowable)a.o())); }
void j_ThrowNew(GuestThread &t) {
    Args a(t); a.w();
    jclass c = (jclass)a.o();
    t.ret32((u32)E()->ThrowNew(c, (const char *)g2h(a.w())));
}
void j_ExceptionOccurred(GuestThread &t) { t.ret32(to_guest(E()->ExceptionOccurred())); }
void j_ExceptionDescribe(GuestThread &) { E()->ExceptionDescribe(); }
void j_ExceptionClear(GuestThread &) { E()->ExceptionClear(); }
void j_ExceptionCheck(GuestThread &t) { t.ret32(E()->ExceptionCheck()); }
void j_FatalError(GuestThread &t) { Args a(t); a.w(); fatal("JNI FatalError: %s", (const char *)g2h(a.w())); }

void j_PushLocalFrame(GuestThread &t) {
    Args a(t); a.w();
    s32 cap = (s32)a.w();
    t_state.frames.push_back(t_state.locals.size());
    t.ret32((u32)E()->PushLocalFrame(cap));
}
void j_PopLocalFrame(GuestThread &t) {
    Args a(t); a.w();
    jobject res = a.o();
    jobject r = E()->PopLocalFrame(res);
    if (!t_state.frames.empty()) {
        t_state.locals.resize(t_state.frames.back());
        t_state.frames.pop_back();
    }
    t.ret32(to_guest(r));
}
void j_EnsureLocalCapacity(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->EnsureLocalCapacity((s32)a.w())); }

void j_NewGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    jobject o = a.o();
    if (!o) return t.ret32(0);
    t.ret32(add_ref(g_globals, g_globals_free, E()->NewGlobalRef(o), 2));
}
void j_DeleteGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    u32 h = a.w();
    if ((h & 3) != 2) return;
    u32 i = (h >> 2) - 1;
    jobject o;
    {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        if (i >= g_globals.size()) return;
        o = g_globals[i];
        g_globals[i] = nullptr;
        g_globals_free.push_back(i);
    }
    if (o) E()->DeleteGlobalRef(o);
}
void j_DeleteLocalRef(GuestThread &t) {
    Args a(t); a.w();
    u32 h = a.w();
    if ((h & 3) != 1) return;
    u32 i = (h >> 2) - 1;
    if (i < t_state.locals.size() && t_state.locals[i]) {
        E()->DeleteLocalRef(t_state.locals[i]);
        t_state.locals[i] = nullptr;
    }
}
void j_NewLocalRef(GuestThread &t) { Args a(t); a.w(); jobject o = a.o(); t.ret32(o ? to_guest(E()->NewLocalRef(o)) : 0); }
void j_IsSameObject(GuestThread &t) { Args a(t); a.w(); jobject x = a.o(), y = a.o(); t.ret32(E()->IsSameObject(x, y)); }
void j_AllocObject(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->AllocObject((jclass)a.o()))); }
void j_GetObjectClass(GuestThread &t) { Args a(t); a.w(); t.ret32(to_guest(E()->GetObjectClass(a.o()))); }
void j_IsInstanceOf(GuestThread &t) { Args a(t); a.w(); jobject o = a.o(); jclass c = (jclass)a.o(); t.ret32(E()->IsInstanceOf(o, c)); }

template <bool STATIC> void j_GetMethodID(GuestThread &t) {
    Args a(t); a.w();
    jclass c = (jclass)a.o();
    const char *name = (const char *)g2h(a.w());
    const char *sig = (const char *)g2h(a.w());
    if (!c || !name || !sig) return t.ret32(0);
    JNIEnv *e = E();
    if (e->ExceptionCheck()) e->ExceptionClear();
    jmethodID id = STATIC ? e->GetStaticMethodID(c, name, sig) : e->GetMethodID(c, name, sig);
    if (!id || e->ExceptionCheck()) {
        e->ExceptionClear();
        return t.ret32(0);
    }
    t.ret32(mid_to_guest(id, sig));
}
template <bool STATIC> void j_GetFieldID(GuestThread &t) {
    Args a(t); a.w();
    jclass c = (jclass)a.o();
    const char *name = (const char *)g2h(a.w());
    const char *sig = (const char *)g2h(a.w());
    if (!c || !name || !sig) return t.ret32(0);
    JNIEnv *e = E();
    if (e->ExceptionCheck()) e->ExceptionClear();
    jfieldID id = STATIC ? e->GetStaticFieldID(c, name, sig) : e->GetFieldID(c, name, sig);
    if (!id || e->ExceptionCheck()) {
        e->ExceptionClear();
        return t.ret32(0);
    }
    t.ret32(fid_to_guest(id));
}

void j_NewString(GuestThread &t) {
    Args a(t); a.w();
    u32 u = a.w(), len = a.w();
    t.ret32(to_guest(E()->NewString((const jchar *)g2h(u), (jsize)len)));
}
void j_GetStringLength(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->GetStringLength((jstring)a.o())); }
void j_GetStringChars(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    jsize n = e->GetStringLength(s);
    u32 g = mem::halloc((u32)(n + 1) * 2);
    e->GetStringRegion(s, 0, n, (jchar *)g2h(g));
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
void j_ReleaseStringChars(GuestThread &t) { Args a(t); a.w(); a.w(); mem::hfree(a.w()); }

void j_NewStringUTF(GuestThread &t) {
    Args a(t); a.w();
    u32 p = a.w();
    if (!p) return t.ret32(0);
    const char *c = (const char *)g2h(p);
    bool all_ascii = true;
    for (const u8 *q = (const u8 *)c; *q; q++) {
        if (*q >= 0x80) {
            all_ascii = false;
            break;
        }
    }
    if (all_ascii) {
        t.ret32(to_guest(E()->NewStringUTF(c)));
    } else {
        std::string safe = sanitize_modified_utf8(c);
        t.ret32(to_guest(E()->NewStringUTF(safe.c_str())));
    }
}

void j_GetStringUTFLength(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->GetStringUTFLength((jstring)a.o())); }
void j_GetStringUTFChars(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    const char *c = e->GetStringUTFChars(s, nullptr);
    u32 g = mem::hstrdup(c ? c : "");
    if (c) e->ReleaseStringUTFChars(s, c);
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
void j_ReleaseStringUTFChars(GuestThread &t) { Args a(t); a.w(); a.w(); mem::hfree(a.w()); }
void j_GetStringRegion(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    jsize st = (jsize)a.w(), n = (jsize)a.w();
    E()->GetStringRegion(s, st, n, (jchar *)g2h(a.w()));
}
void j_GetStringUTFRegion(GuestThread &t) {
    Args a(t); a.w();
    auto s = (jstring)a.o();
    jsize st = (jsize)a.w(), n = (jsize)a.w();
    E()->GetStringUTFRegion(s, st, n, (char *)g2h(a.w()));
}

void j_GetArrayLength(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->GetArrayLength((jarray)a.o())); }
void j_NewObjectArray(GuestThread &t) {
    Args a(t); a.w();
    jsize n = (jsize)a.w();
    jclass c = (jclass)a.o();
    jobject init = a.o();
    t.ret32(to_guest(E()->NewObjectArray(n, c, init)));
}
void j_GetObjectArrayElement(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jobjectArray)a.o();
    t.ret32(to_guest(E()->GetObjectArrayElement(arr, (jsize)a.w())));
}
void j_SetObjectArrayElement(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jobjectArray)a.o();
    jsize i = (jsize)a.w();
    E()->SetObjectArrayElement(arr, i, a.o());
}
void j_GetPrimitiveArrayCritical(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jarray)a.o();
    u32 is_copy = a.w();
    JNIEnv *e = E();
    char k = array_kind(e, arr);
    jsize n = e->GetArrayLength(arr);
    int es = elem_size(k);
    u32 g = mem::halloc((u32)(n * es) + 8);
    void *src = e->GetPrimitiveArrayCritical(arr, nullptr);
    memcpy(g2h(g), src, (size_t)n * es);
    e->ReleasePrimitiveArrayCritical(arr, src, JNI_ABORT);
    if (is_copy) wr<u8>(is_copy, 1);
    t.ret32(g);
}
void j_ReleasePrimitiveArrayCritical(GuestThread &t) {
    Args a(t); a.w();
    auto arr = (jarray)a.o();
    u32 elems = a.w();
    s32 mode = (s32)a.w();
    JNIEnv *e = E();
    if (mode != JNI_ABORT) {
        int es = elem_size(array_kind(e, arr));
        jsize n = e->GetArrayLength(arr);
        void *dst = e->GetPrimitiveArrayCritical(arr, nullptr);
        memcpy(dst, g2h(elems), (size_t)n * es);
        e->ReleasePrimitiveArrayCritical(arr, dst, 0);
    }
    if (mode != JNI_COMMIT) mem::hfree(elems);
}
void j_RegisterNatives(GuestThread &t) {
    Args a(t); a.w();
    a.w();
    u32 methods = a.w();
    s32 n = (s32)a.w();
    for (s32 i = 0; i < n; i++)
        LOGE("guest RegisterNatives(%s %s) is not supported", (const char *)g2h(rd<u32>(methods + i * 12)),
             (const char *)g2h(rd<u32>(methods + i * 12 + 4)));
    t.ret32(0);
}
void j_UnregisterNatives(GuestThread &t) { t.ret32(0); }
void j_MonitorEnter(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->MonitorEnter(a.o())); }
void j_MonitorExit(GuestThread &t) { Args a(t); a.w(); t.ret32((u32)E()->MonitorExit(a.o())); }
void j_GetJavaVM(GuestThread &t) {
    Args a(t); a.w();
    wr<u32>(a.w(), g_gvm);
    t.ret32(0);
}
void j_NewWeakGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    jobject o = a.o();
    if (!o) return t.ret32(0);
    t.ret32(add_ref(g_weaks, g_weaks_free, E()->NewWeakGlobalRef(o), 3));
}
void j_DeleteWeakGlobalRef(GuestThread &t) {
    Args a(t); a.w();
    u32 h = a.w();
    if ((h & 3) != 3) return;
    u32 i = (h >> 2) - 1;
    jobject o;
    {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        if (i >= g_weaks.size()) return;
        o = g_weaks[i];
        g_weaks[i] = nullptr;
        g_weaks_free.push_back(i);
    }
    if (o) E()->DeleteWeakGlobalRef(o);
}
void j_NewDirectByteBuffer(GuestThread &t) {
    Args a(t); a.w();
    u32 addr = a.w();
    jlong cap = (jlong)a.d();
    t.ret32(to_guest(E()->NewDirectByteBuffer(g2h(addr), cap)));
}
void j_GetDirectBufferAddress(GuestThread &t) {
    Args a(t); a.w();
    jobject buf = a.o();
    JNIEnv *e = E();
    void *p = e->GetDirectBufferAddress(buf);
    if (!p) return t.ret32(0);
    if (is_guest_ptr(p)) return t.ret32(h2g(p));
    std::lock_guard<std::mutex> lk(g_mirror_lock);
    auto it = g_mirrors.find(p);
    if (it == g_mirrors.end()) {
        u32 size = (u32)e->GetDirectBufferCapacity(buf);
        u32 g = mem::map(size ? size : 1);
        memcpy(g2h(g), p, size);
        it = g_mirrors.emplace(p, Mirror{p, g, size, e->NewGlobalRef(buf)}).first;
    }
    t.ret32(it->second.guest);
}
void j_GetDirectBufferCapacity(GuestThread &t) { Args a(t); a.w(); t.ret64((u64)E()->GetDirectBufferCapacity(a.o())); }
void j_GetObjectRefType(GuestThread &t) { Args a(t); a.w(); t.ret32(a.w() & 3); }

void j_unimplemented(GuestThread &t) {
    cpu::dump_state(t);
    fatal("unimplemented JNI function %s", cpu::thunk_name(t.cur_svc));
}

// ------------------------------------------------------------------ JavaVM
void vm_DestroyJavaVM(GuestThread &t) { t.ret32((u32)JNI_ERR); }
void vm_AttachCurrentThread(GuestThread &t) {
    Args a(t); a.w();
    u32 penv = a.w();
    host_env();
    wr<u32>(penv, guest_env());
    t.ret32(JNI_OK);
}
void vm_DetachCurrentThread(GuestThread &t) {
    t_state.locals.clear();
    t_state.frames.clear();
    if (t_state.env && t_state.attached_by_us) {
        g_vm->DetachCurrentThread();
        t_state.env = nullptr;
        t_state.attached_by_us = false;
    }
    t.ret32(JNI_OK);
}
void vm_GetEnv(GuestThread &t) {
    Args a(t); a.w();
    u32 penv = a.w();
    JNIEnv *e = nullptr;
    if (g_vm->GetEnv((void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        wr<u32>(penv, 0);
        return t.ret32((u32)JNI_EDETACHED);
    }
    t_state.env = e;
    wr<u32>(penv, guest_env());
    t.ret32(JNI_OK);
}

struct Entry {
    const char *name;
    ThunkFn fn;
};

#define CALL3(Name, K, KIND)                                                    \
    {"Call" Name "Method", jni_call<K, 0, KIND>}, {"Call" Name "MethodV", jni_call<K, 1, KIND>}, \
    {"Call" Name "MethodA", jni_call<K, 2, KIND>}
#define CALLS(Prefix, KIND)                                                                                   \
    CALL3(Prefix "Object", 'L', KIND), CALL3(Prefix "Boolean", 'Z', KIND), CALL3(Prefix "Byte", 'B', KIND),     \
    CALL3(Prefix "Char", 'C', KIND), CALL3(Prefix "Short", 'S', KIND), CALL3(Prefix "Int", 'I', KIND),          \
    CALL3(Prefix "Long", 'J', KIND), CALL3(Prefix "Float", 'F', KIND), CALL3(Prefix "Double", 'D', KIND),       \
    CALL3(Prefix "Void", 'V', KIND)
#define FIELDS(Op, fn, STATIC)                                                                              \
    {Op "ObjectField", fn<'L', STATIC>}, {Op "BooleanField", fn<'Z', STATIC>}, {Op "ByteField", fn<'B', STATIC>}, \
    {Op "CharField", fn<'C', STATIC>}, {Op "ShortField", fn<'S', STATIC>}, {Op "IntField", fn<'I', STATIC>},       \
    {Op "LongField", fn<'J', STATIC>}, {Op "FloatField", fn<'F', STATIC>}, {Op "DoubleField", fn<'D', STATIC>}
#define PRIMS(Pre, Post, fn)                                                                                  \
    {Pre "Boolean" Post, fn<'Z'>}, {Pre "Byte" Post, fn<'B'>}, {Pre "Char" Post, fn<'C'>},                    \
    {Pre "Short" Post, fn<'S'>}, {Pre "Int" Post, fn<'I'>}, {Pre "Long" Post, fn<'J'>},                       \
    {Pre "Float" Post, fn<'F'>}, {Pre "Double" Post, fn<'D'>}

const Entry kEnvTable[] = {
    {"reserved0", nullptr}, {"reserved1", nullptr}, {"reserved2", nullptr}, {"reserved3", nullptr},
    {"GetVersion", j_GetVersion}, {"DefineClass", nullptr}, {"FindClass", j_FindClass},
    {"FromReflectedMethod", nullptr}, {"FromReflectedField", nullptr}, {"ToReflectedMethod", nullptr},
    {"GetSuperclass", j_GetSuperclass}, {"IsAssignableFrom", j_IsAssignableFrom}, {"ToReflectedField", nullptr},
    {"Throw", j_Throw}, {"ThrowNew", j_ThrowNew}, {"ExceptionOccurred", j_ExceptionOccurred},
    {"ExceptionDescribe", j_ExceptionDescribe}, {"ExceptionClear", j_ExceptionClear}, {"FatalError", j_FatalError},
    {"PushLocalFrame", j_PushLocalFrame}, {"PopLocalFrame", j_PopLocalFrame}, {"NewGlobalRef", j_NewGlobalRef},
    {"DeleteGlobalRef", j_DeleteGlobalRef}, {"DeleteLocalRef", j_DeleteLocalRef}, {"IsSameObject", j_IsSameObject},
    {"NewLocalRef", j_NewLocalRef}, {"EnsureLocalCapacity", j_EnsureLocalCapacity}, {"AllocObject", j_AllocObject},
    {"NewObject", jni_new_object<0>}, {"NewObjectV", jni_new_object<1>}, {"NewObjectA", jni_new_object<2>},
    {"GetObjectClass", j_GetObjectClass}, {"IsInstanceOf", j_IsInstanceOf}, {"GetMethodID", j_GetMethodID<false>},
    CALLS("", 0),
    CALLS("Nonvirtual", 1),
    {"GetFieldID", j_GetFieldID<false>},
    FIELDS("Get", jni_get_field, false),
    FIELDS("Set", jni_set_field, false),
    {"GetStaticMethodID", j_GetMethodID<true>},
    CALLS("Static", 2),
    {"GetStaticFieldID", j_GetFieldID<true>},
    FIELDS("GetStatic", jni_get_field, true),
    FIELDS("SetStatic", jni_set_field, true),
    {"NewString", j_NewString}, {"GetStringLength", j_GetStringLength}, {"GetStringChars", j_GetStringChars},
    {"ReleaseStringChars", j_ReleaseStringChars}, {"NewStringUTF", j_NewStringUTF},
    {"GetStringUTFLength", j_GetStringUTFLength}, {"GetStringUTFChars", j_GetStringUTFChars},
    {"ReleaseStringUTFChars", j_ReleaseStringUTFChars}, {"GetArrayLength", j_GetArrayLength},
    {"NewObjectArray", j_NewObjectArray}, {"GetObjectArrayElement", j_GetObjectArrayElement},
    {"SetObjectArrayElement", j_SetObjectArrayElement},
    PRIMS("New", "Array", jni_new_array),
    PRIMS("Get", "ArrayElements", jni_get_elements),
    PRIMS("Release", "ArrayElements", jni_release_elements),
    PRIMS("Get", "ArrayRegion", jni_get_region),
    PRIMS("Set", "ArrayRegion", jni_set_region),
    {"RegisterNatives", j_RegisterNatives}, {"UnregisterNatives", j_UnregisterNatives},
    {"MonitorEnter", j_MonitorEnter}, {"MonitorExit", j_MonitorExit}, {"GetJavaVM", j_GetJavaVM},
    {"GetStringRegion", j_GetStringRegion}, {"GetStringUTFRegion", j_GetStringUTFRegion},
    {"GetPrimitiveArrayCritical", j_GetPrimitiveArrayCritical},
    {"ReleasePrimitiveArrayCritical", j_ReleasePrimitiveArrayCritical},
    {"GetStringCritical", j_GetStringChars}, {"ReleaseStringCritical", j_ReleaseStringChars},
    {"NewWeakGlobalRef", j_NewWeakGlobalRef}, {"DeleteWeakGlobalRef", j_DeleteWeakGlobalRef},
    {"ExceptionCheck", j_ExceptionCheck}, {"NewDirectByteBuffer", j_NewDirectByteBuffer},
    {"GetDirectBufferAddress", j_GetDirectBufferAddress}, {"GetDirectBufferCapacity", j_GetDirectBufferCapacity},
    {"GetObjectRefType", j_GetObjectRefType},
};
static_assert(sizeof(kEnvTable) / sizeof(kEnvTable[0]) == 233, "JNINativeInterface has 233 entries");

const Entry kVmTable[] = {
    {"vm_reserved0", nullptr}, {"vm_reserved1", nullptr}, {"vm_reserved2", nullptr},
    {"DestroyJavaVM", vm_DestroyJavaVM}, {"AttachCurrentThread", vm_AttachCurrentThread},
    {"DetachCurrentThread", vm_DetachCurrentThread}, {"GetEnv", vm_GetEnv},
    {"AttachCurrentThreadAsDaemon", vm_AttachCurrentThread},
};

u32 build_table(const Entry *e, size_t n) {
    u32 table = mem::halloc((u32)n * 4, 16);
    for (size_t i = 0; i < n; i++) {
        if (!e[i].name || strncmp(e[i].name, "reserved", 8) == 0 || strncmp(e[i].name, "vm_reserved", 11) == 0) {
            wr<u32>(table + (u32)i * 4, 0);
            continue;
        }
        std::string name = std::string("JNI::") + e[i].name;
        wr<u32>(table + (u32)i * 4, cpu::add_thunk(name.c_str(), e[i].fn ? e[i].fn : j_unimplemented));
    }
    return table;
}
}  // namespace

void init(JavaVM *vm) {
    g_vm = vm;
    g_env_table = build_table(kEnvTable, sizeof(kEnvTable) / sizeof(kEnvTable[0]));
    g_vm_table = build_table(kVmTable, sizeof(kVmTable) / sizeof(kVmTable[0]));
    g_gvm = mem::halloc(8);
    wr<u32>(g_gvm, g_vm_table);
}

void set_class_loader(JNIEnv *env, jobject context) {
    if (!env || !context) return;
    std::lock_guard<std::mutex> lk(g_cl_lock);
    if (g_class_loader) return;
    jclass ctx_cls = env->GetObjectClass(context);
    if (!ctx_cls) return;
    jmethodID get_cl = env->GetMethodID(ctx_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (!get_cl) {
        env->DeleteLocalRef(ctx_cls);
        return;
    }
    jobject cl = env->CallObjectMethod(context, get_cl);
    env->DeleteLocalRef(ctx_cls);
    if (!cl || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    jclass cl_cls = env->GetObjectClass(cl);
    g_load_class_mid = env->GetMethodID(cl_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(cl_cls);
    if (g_load_class_mid) g_class_loader = env->NewGlobalRef(cl);
    env->DeleteLocalRef(cl);
}

u32 guest_vm() { return g_gvm; }

u32 guest_env() {
    if (!t_state.genv) {
        t_state.genv = mem::halloc(8);
        wr<u32>(t_state.genv, g_env_table);
    }
    return t_state.genv;
}

void bind_env(JNIEnv *env) { t_state.env = env; }

u32 to_guest(jobject ref) {
    if (!ref) return 0;
    size_t floor = t_state.frames.empty() ? 0 : t_state.frames.back();
    for (size_t i = floor; i < t_state.locals.size(); i++) {
        if (!t_state.locals[i]) {
            t_state.locals[i] = ref;
            return ((u32)(i + 1) << 2) | 1;
        }
    }
    t_state.locals.push_back(ref);
    return ((u32)t_state.locals.size() << 2) | 1;
}

jobject from_guest(u32 h) {
    if (!h) return nullptr;
    u32 idx = (h >> 2) - 1;
    switch (h & 3) {
    case 1:
        return idx < t_state.locals.size() ? t_state.locals[idx] : nullptr;
    case 2: {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        return idx < g_globals.size() ? g_globals[idx] : nullptr;
    }
    case 3: {
        std::lock_guard<std::mutex> lk(g_ref_lock);
        return idx < g_weaks.size() ? g_weaks[idx] : nullptr;
    }
    default:
        return nullptr;
    }
}

static void sync_mirrors(bool to_guest_dir) {
    std::lock_guard<std::mutex> lk(g_mirror_lock);
    for (auto &[_, m] : g_mirrors) {
        if (to_guest_dir) memcpy(g2h(m.guest), m.host, m.size);
        else memcpy(m.host, g2h(m.guest), m.size);
    }
}

NativeFrame::NativeFrame(JNIEnv *env) : mark(t_state.locals.size()), prev_env(t_state.env) {
    t_state.env = env;
    sync_mirrors(true);
}
NativeFrame::~NativeFrame() {
    sync_mirrors(false);
    t_state.locals.resize(mark);
    t_state.env = prev_env;
}

}  // namespace jni
