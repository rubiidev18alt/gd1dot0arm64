// SPDX-License-Identifier: MIT
#pragma once
#include "abi.h"
#include "jni_bridge.h"

#include <jni.h>
#include <cstring>
#include <type_traits>
#include <vector>

namespace natives {

struct Packer {
    std::vector<u32> w;
    void word(u32 v) { w.push_back(v); }
    void dword(u64 v) {
        if (w.size() & 1) w.push_back(0);  // AAPCS32: 64-bit values start at an even word slot
        w.push_back((u32)v);
        w.push_back((u32)(v >> 32));
    }
};

inline void pack(Packer &p, jint v) { p.word((u32)v); }
inline void pack(Packer &p, jboolean v) { p.word(v ? 1u : 0u); }
inline void pack(Packer &p, jfloat v) {
    u32 x;
    memcpy(&x, &v, 4);
    p.word(x);
}
inline void pack(Packer &p, jlong v) { p.dword((u64)v); }
inline void pack(Packer &p, jdouble v) {
    u64 x;
    memcpy(&x, &v, 8);
    p.dword(x);
}
inline void pack(Packer &p, jobject v) { p.word(jni::to_guest(v)); }

template <class R, class... A> R invoke_guest(u32 fn, JNIEnv *env, jobject cls_or_self, A... a) {
    if (!fn) {
        if constexpr (std::is_void_v<R>) return;
        else return R{};
    }
    jni::NativeFrame frame(env);
    Packer p;
    p.word(jni::guest_env());
    p.word(jni::to_guest(cls_or_self));
    (pack(p, a), ...);
    u64 r = cpu::call(fn, p.w.data(), p.w.size());
    if constexpr (std::is_void_v<R>) {
        (void)r;
        return;
    } else if constexpr (std::is_same_v<R, jobject> || std::is_same_v<R, jstring>) {
        jobject ho = jni::from_guest((u32)r);
        return (R)(ho ? env->NewLocalRef(ho) : nullptr);
    } else if constexpr (std::is_same_v<R, jboolean>) {
        return (jboolean)(((u32)r & 1u) ? JNI_TRUE : JNI_FALSE);
    } else if constexpr (std::is_same_v<R, jlong>) {
        return (jlong)r;
    } else if constexpr (std::is_same_v<R, jdouble>) {
        double d;
        memcpy(&d, &r, 8);
        return d;
    } else if constexpr (std::is_same_v<R, jfloat>) {
        u32 w = (u32)r;
        float f;
        memcpy(&f, &w, 4);
        return f;
    } else {
        return (R)(u32)r;
    }
}

}  // namespace natives
