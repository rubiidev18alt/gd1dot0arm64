// SPDX-License-Identifier: MIT
#pragma once
#include "common.h"
#include <jni.h>

namespace jni {

void init(JavaVM *vm);
void set_class_loader(JNIEnv *env, jobject context);

u32 guest_vm();
u32 guest_env();

void bind_env(JNIEnv *env);
u32 to_guest(jobject local_ref);
jobject from_guest(u32 handle);

struct NativeFrame {
    size_t mark;
    JNIEnv *prev_env;
    explicit NativeFrame(JNIEnv *env);
    ~NativeFrame();
};

}  // namespace jni
