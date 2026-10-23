// SPDX-License-Identifier: MIT
#include "common.h"
#include "cpu.h"
#include "jni_bridge.h"
#include "loader.h"
#include "local_gdps.h"
#include "natives.h"
#include "paths.h"
#include "thunks_gl.h"

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

void register_libc_thunks();
void register_pthread_thunks();
void register_gl_thunks();
void bind_guest_stdio();

namespace {

std::mutex g_crash_lock;
std::string g_crash_log_path;

std::mutex g_boot_lock;
bool g_core_inited = false;
bool g_guest_loaded = false;
Module *g_rt_mod = nullptr;
Module *g_game_mod = nullptr;

std::atomic<bool> g_surface_fresh{true};
std::atomic<bool> g_game_inited{false};

struct GuestExports {
    // com.customRobTop.JniToCpp (11)
    u32 didCacheInterstitial = 0;
    u32 didClickInterstitial = 0;
    u32 didCloseInterstitial = 0;
    u32 didDismissInterstitial = 0;
    u32 hideLoadingCircle = 0;
    u32 itemPurchaseFailed = 0;
    u32 itemPurchased = 0;
    u32 itemRefunded = 0;
    u32 promoImageDownloaded = 0;
    u32 resumeSound = 0;
    u32 showInterstitialFailed = 0;

    // org.cocos2dx.lib.* (15)
    u32 accel_onSensorChanged = 0;
    u32 activity_nativeSetPaths = 0;
    u32 bitmap_nativeInitBitmapDC = 0;
    u32 renderer_nativeDeleteBackward = 0;
    u32 renderer_nativeGetContentText = 0;
    u32 renderer_nativeInit = 0;
    u32 renderer_nativeInsertText = 0;
    u32 renderer_nativeKeyDown = 0;
    u32 renderer_nativeOnPause = 0;
    u32 renderer_nativeOnResume = 0;
    u32 renderer_nativeRender = 0;
    u32 renderer_nativeTouchesBegin = 0;
    u32 renderer_nativeTouchesCancel = 0;
    u32 renderer_nativeTouchesEnd = 0;
    u32 renderer_nativeTouchesMove = 0;
} g_fn;

void mkdirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0700);
    }
}

void write_synthetic_cpuinfo32(const std::string &dir) {
    std::string cpuinfo = dir + "/cpuinfo32";
    if (FILE *f = fopen(cpuinfo.c_str(), "w")) {
        for (int i = 0; i < 8; i++) {
            fprintf(f,
                    "processor\t: %d\n"
                    "model name\t: ARMv7 Processor rev 0 (v7l)\n"
                    "BogoMIPS\t: 38.40\n"
                    "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt lpae evtstrm\n"
                    "CPU implementer\t: 0x41\n"
                    "CPU architecture: 7\n"
                    "CPU variant\t: 0x0\n"
                    "CPU part\t: 0xd05\n"
                    "CPU revision\t: 0\n\n",
                    i);
        }
        fputs("Hardware\t: Android ARM64 Bridge\n", f);
        fclose(f);
        paths::add_mapping("/proc/cpuinfo", cpuinfo);
    }
}

u32 req_sym(const char *sym) {
    u32 a = loader::lookup(sym, g_game_mod);
    if (!a) fatal("guest libgame.so missing required JNI export: %s", sym);
    return a;
}

void resolve_guest_exports() {
    g_fn.didCacheInterstitial   = req_sym("Java_com_customRobTop_JniToCpp_didCacheInterstitial");
    g_fn.didClickInterstitial   = req_sym("Java_com_customRobTop_JniToCpp_didClickInterstitial");
    g_fn.didCloseInterstitial   = req_sym("Java_com_customRobTop_JniToCpp_didCloseInterstitial");
    g_fn.didDismissInterstitial = req_sym("Java_com_customRobTop_JniToCpp_didDismissInterstitial");
    g_fn.hideLoadingCircle      = req_sym("Java_com_customRobTop_JniToCpp_hideLoadingCircle");
    g_fn.itemPurchaseFailed     = req_sym("Java_com_customRobTop_JniToCpp_itemPurchaseFailed");
    g_fn.itemPurchased          = req_sym("Java_com_customRobTop_JniToCpp_itemPurchased");
    g_fn.itemRefunded           = req_sym("Java_com_customRobTop_JniToCpp_itemRefunded");
    g_fn.promoImageDownloaded   = req_sym("Java_com_customRobTop_JniToCpp_promoImageDownloaded");
    g_fn.resumeSound            = req_sym("Java_com_customRobTop_JniToCpp_resumeSound");
    g_fn.showInterstitialFailed = req_sym("Java_com_customRobTop_JniToCpp_showInterstitialFailed");

    g_fn.accel_onSensorChanged         = req_sym("Java_org_cocos2dx_lib_Cocos2dxAccelerometer_onSensorChanged");
    g_fn.activity_nativeSetPaths       = req_sym("Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths");
    g_fn.bitmap_nativeInitBitmapDC     = req_sym("Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
    g_fn.renderer_nativeDeleteBackward = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
    g_fn.renderer_nativeGetContentText = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText");
    g_fn.renderer_nativeInit           = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    g_fn.renderer_nativeInsertText     = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
    g_fn.renderer_nativeKeyDown        = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
    g_fn.renderer_nativeOnPause        = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
    g_fn.renderer_nativeOnResume       = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
    g_fn.renderer_nativeRender         = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
    g_fn.renderer_nativeTouchesBegin   = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    g_fn.renderer_nativeTouchesCancel  = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
    g_fn.renderer_nativeTouchesEnd     = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    g_fn.renderer_nativeTouchesMove    = req_sym("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
}

void ensure_core_init(JavaVM *vm) {
    if (g_core_inited) return;
    mem::init();
    cpu::init();
    jni::init(vm);
    register_libc_thunks();
    register_pthread_thunks();
    register_gl_thunks();
    g_core_inited = true;
}

u32 finish_guest_boot(JNIEnv *env, const std::string &data_dir) {
    bind_guest_stdio();
    loader::run_initializers(g_rt_mod);
    u32 ctors = loader::run_initializers(g_game_mod);
    resolve_guest_exports();

    if (u32 onload = loader::lookup("JNI_OnLoad", g_game_mod)) {
        jni::NativeFrame frame(env);
        u32 ver = cpu::call32(onload, {jni::guest_vm(), 0});
        LOGI("guest JNI_OnLoad returned 0x%x", ver);
    }

    g_guest_loaded = true;
    LOGI("Geometry Dash 1.02 ARM32->ARM64 bridge ready (data=%s, unresolved=%zu)",
         data_dir.c_str(), loader::unresolved_symbols().size());
    return ctors;
}

#ifdef __ANDROID__
Module *load_asset_elf(AAssetManager *am, const char *asset_path, const char *soname) {
    AAsset *a = AAssetManager_open(am, asset_path, AASSET_MODE_BUFFER);
    if (!a) fatal("missing required APK asset: %s", asset_path);
    size_t len = (size_t)AAsset_getLength(a);
    const void *buf = AAsset_getBuffer(a);
    Module *m = nullptr;
    if (buf) {
        m = loader::load_bytes(soname, (const u8 *)buf, len);
    } else {
        std::vector<u8> copy(len);
        if (AAsset_read(a, copy.data(), len) != (int)len)
            fatal("short read on APK asset: %s", asset_path);
        m = loader::load_bytes(soname, copy.data(), copy.size());
    }
    AAsset_close(a);
    return m;
}
#endif

}  // namespace

void crash_note(const char *line) {
    std::lock_guard<std::mutex> lk(g_crash_lock);
    if (g_crash_log_path.empty()) return;
    if (FILE *f = fopen(g_crash_log_path.c_str(), "a")) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

[[noreturn]] void fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOGE("FATAL: %s", buf);
    crash_note(buf);
    abort();
}

#define EXPORT_JNI extern "C" __attribute__((visibility("default"))) JNIEXPORT

EXPORT_JNI jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    std::lock_guard<std::mutex> lk(g_boot_lock);
    ensure_core_init(vm);
    return JNI_VERSION_1_6;
}

EXPORT_JNI void JNICALL Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeBridgeInit(
    JNIEnv *env, jclass, jobject context, jobject asset_manager, jstring apk_path_j, jstring data_dir_j) {
    std::lock_guard<std::mutex> lk(g_boot_lock);
    JavaVM *vm = nullptr;
    env->GetJavaVM(&vm);
    ensure_core_init(vm);
    jni::set_class_loader(env, context);

    if (g_guest_loaded) return;

    const char *data_c = data_dir_j ? env->GetStringUTFChars(data_dir_j, nullptr) : nullptr;
    std::string data_dir = data_c ? data_c : "/data/data/com.robtopx.geometryjump";
    if (data_c) env->ReleaseStringUTFChars(data_dir_j, data_c);

    const char *apk_c = apk_path_j ? env->GetStringUTFChars(apk_path_j, nullptr) : nullptr;
    std::string apk_path = apk_c ? apk_c : "";
    if (apk_c) env->ReleaseStringUTFChars(apk_path_j, apk_c);
    (void)apk_path;

    std::string pkg_name;
    if (context) {
        jclass ctx_cls = env->GetObjectClass(context);
        if (ctx_cls) {
            jmethodID mid = env->GetMethodID(ctx_cls, "getPackageName", "()Ljava/lang/String;");
            if (mid) {
                jstring pkg_j = (jstring)env->CallObjectMethod(context, mid);
                if (!env->ExceptionCheck() && pkg_j) {
                    const char *pkg_c = env->GetStringUTFChars(pkg_j, nullptr);
                    if (pkg_c) {
                        pkg_name = pkg_c;
                        env->ReleaseStringUTFChars(pkg_j, pkg_c);
                    }
                    env->DeleteLocalRef(pkg_j);
                }
            }
            env->ExceptionClear();
            env->DeleteLocalRef(ctx_cls);
        }
    }

    mkdirs(data_dir);
    {
        std::lock_guard<std::mutex> clk(g_crash_lock);
        g_crash_log_path = data_dir + "/crash.log";
    }
    paths::add_mapping("/data/data/com.robtopx.geometryjump", data_dir);
    if (!pkg_name.empty()) {
        paths::add_mapping("/data/data/" + pkg_name, data_dir);
    }
    write_synthetic_cpuinfo32(data_dir);

#ifdef __ANDROID__
    AAssetManager *am = AAssetManager_fromJava(env, asset_manager);
    if (!am) fatal("AAssetManager_fromJava returned null");
    if (AAsset *gdps_a = AAssetManager_open(am, "gdps/levels_1_0.bin", AASSET_MODE_BUFFER)) {
        size_t gdps_len = (size_t)AAsset_getLength(gdps_a);
        const void *gdps_buf = AAsset_getBuffer(gdps_a);
        bool ok = false;
        if (gdps_buf) {
            ok = local_gdps::init_from_bytes((const u8 *)gdps_buf, gdps_len, data_dir);
        } else {
            std::vector<u8> copy(gdps_len);
            if (AAsset_read(gdps_a, copy.data(), gdps_len) == (int)gdps_len) {
                ok = local_gdps::init_from_bytes(copy.data(), copy.size(), data_dir);
            }
        }
        AAsset_close(gdps_a);
        if (!ok) fatal("failed to initialize embedded GD 1.0 local server");
    } else if (pkg_name == "com.robtopx.geomgdps102") {
        fatal("missing required asset gdps/levels_1_0.bin for com.robtopx.geomgdps102");
    }
    g_rt_mod = load_asset_elf(am, "arm32/libguestrt.so", "libguestrt.so");
    g_game_mod = load_asset_elf(am, "arm32/libgame.so", "libgame.so");
    finish_guest_boot(env, data_dir);
#else
    (void)asset_manager;
#endif
}

EXPORT_JNI void JNICALL Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeOnSurfaceCreated(
    JNIEnv *, jclass) {
    gl::reset_context_state();
    g_surface_fresh.store(true, std::memory_order_release);
}

EXPORT_JNI jboolean JNICALL Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeLocalGdpsEnabled(
    JNIEnv *, jclass) {
    return local_gdps::is_enabled() ? JNI_TRUE : JNI_FALSE;
}

// Host test/verification entry point for Linux ARM64 headless execution (`gd102_verify_runner`).
extern "C" __attribute__((visibility("default"))) bool gd102_host_boot_from_dir(
    JavaVM *vm, JNIEnv *env, const char *arm32_dir, const char *data_dir_c,
    u32 *out_ctors, u32 *out_unresolved) {
    std::lock_guard<std::mutex> lk(g_boot_lock);
    ensure_core_init(vm);
    if (!g_guest_loaded) {
        std::string data_dir = data_dir_c ? data_dir_c : "/tmp/gd102_data";
        mkdirs(data_dir);
        {
            std::lock_guard<std::mutex> clk(g_crash_lock);
            g_crash_log_path = data_dir + "/crash.log";
        }
        paths::add_mapping("/data/data/com.robtopx.geometryjump", data_dir);
        paths::add_mapping("/data/data/com.robtopx.geometryj102", data_dir);
        paths::add_mapping("/data/data/com.robtopx.geomgdps102", data_dir);
        write_synthetic_cpuinfo32(data_dir);

        std::string dir = arm32_dir;
        g_rt_mod = loader::load(dir + "/libguestrt.so");
        g_game_mod = loader::load(dir + "/libgame.so");
        u32 ctors = finish_guest_boot(env, data_dir);
        if (out_ctors) *out_ctors = ctors;
        if (out_unresolved) *out_unresolved = (u32)loader::unresolved_symbols().size();
    }
    return loader::unresolved_symbols().empty();
}

#ifndef __ANDROID__
extern "C" __attribute__((visibility("default"))) bool gd102_host_enable_local_gdps(
    const char *db_path, const char *data_dir_c, u32 *out_levels, u16 *out_port) {
    if (!db_path) return false;
    std::string data_dir = data_dir_c ? data_dir_c : "/tmp/gd102_data";
    if (!local_gdps::init_from_file(db_path, data_dir)) return false;
    if (out_levels) *out_levels = local_gdps::level_count();
    if (out_port) *out_port = local_gdps::port();
    return true;
}

extern "C" __attribute__((visibility("default"))) void gd102_host_local_gdps_stats(
    u32 *out_requests, u32 *out_blocked_uploads) {
    if (out_requests) *out_requests = local_gdps::requests_handled();
    if (out_blocked_uploads) *out_blocked_uploads = local_gdps::blocked_upload_count();
}

extern "C" __attribute__((visibility("default"))) u32 gd102_host_lookup_game_sym(const char *sym) {
    if (!sym || !g_game_mod) return 0;
    return loader::lookup(sym, g_game_mod);
}

// Finds the first exported libgame.so symbol whose (mangled) name contains `needle`.
extern "C" __attribute__((visibility("default"))) u32 gd102_host_find_game_sym_containing(
    const char *needle, char *out_name, u32 out_name_size) {
    if (!needle || !g_game_mod) return 0;
    const std::string n(needle);
    for (const auto &kv : g_game_mod->exports) {
        if (kv.first.find(n) == std::string::npos) continue;
        if (out_name && out_name_size) {
            snprintf(out_name, out_name_size, "%s", kv.first.c_str());
        }
        return kv.second;
    }
    if (out_name && out_name_size) out_name[0] = '\0';
    return 0;
}

extern "C" __attribute__((visibility("default"))) u32 gd102_host_game_export_count() {
    return g_game_mod ? (u32)g_game_mod->exports.size() : 0;
}

// Loads a tiny ARM32 shared object (tests/guest_gdps_probe.c) into the guest address space,
// runs its static initializers, and invokes its exported `gdps_probe_run()` entry point so the
// local GDPS server is exercised through real ARM32 guest code, real JNI/thunk boundaries, and
// the same gethostbyname()/connect() path libgame.so uses for www.boomlings.com.
extern "C" __attribute__((visibility("default"))) int gd102_host_run_guest_probe(
    JNIEnv *env, const char *so_path) {
    if (!so_path) return -1;
    Module *probe = loader::load(so_path);
    if (!probe) return -2;
    loader::run_initializers(probe);
    u32 fn = loader::lookup("gdps_probe_run", probe);
    if (!fn) return -3;
    jni::NativeFrame frame(env);
    return (int)cpu::call32(fn, {});
}

extern "C" __attribute__((visibility("default"))) u32 gd102_host_call_guest(
    JNIEnv *env, u32 fn_addr, u32 argc, const u32 *argv) {
    if (!fn_addr) return 0;
    jni::NativeFrame frame(env);
    return (u32)cpu::call(fn_addr, argv, (size_t)argc);
}

extern "C" __attribute__((visibility("default"))) const char *gd102_host_guest_cstr(u32 guest_addr) {
    if (!guest_addr) return nullptr;
    return (const char *)g2h(guest_addr);
}
#endif

// ------------------------------------------------------------------
// 26 Geometry Dash 1.02 JNI exports forwarded into guest ARM32 libgame.so
// ------------------------------------------------------------------

// com.customRobTop.JniToCpp (11)
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_didCacheInterstitial(JNIEnv *e, jclass c, jstring loc) {
    natives::invoke_guest<void>(g_fn.didCacheInterstitial, e, c, (jobject)loc);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_didClickInterstitial(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.didClickInterstitial, e, c);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_didCloseInterstitial(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.didCloseInterstitial, e, c);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_didDismissInterstitial(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.didDismissInterstitial, e, c);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_hideLoadingCircle(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.hideLoadingCircle, e, c);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_itemPurchaseFailed(JNIEnv *e, jclass c, jstring sku) {
    natives::invoke_guest<void>(g_fn.itemPurchaseFailed, e, c, (jobject)sku);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_itemPurchased(JNIEnv *e, jclass c, jstring sku) {
    natives::invoke_guest<void>(g_fn.itemPurchased, e, c, (jobject)sku);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_itemRefunded(JNIEnv *e, jclass c, jstring sku) {
    natives::invoke_guest<void>(g_fn.itemRefunded, e, c, (jobject)sku);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_promoImageDownloaded(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.promoImageDownloaded, e, c);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_resumeSound(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.resumeSound, e, c);
}
EXPORT_JNI void JNICALL Java_com_customRobTop_JniToCpp_showInterstitialFailed(JNIEnv *e, jclass c) {
    natives::invoke_guest<void>(g_fn.showInterstitialFailed, e, c);
}

// org.cocos2dx.lib.Cocos2dxAccelerometer (1)
EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxAccelerometer_onSensorChanged(
    JNIEnv *e, jclass c, jfloat x, jfloat y, jfloat z, jlong ts) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.accel_onSensorChanged, e, c, x, y, z, ts);
}

// org.cocos2dx.lib.Cocos2dxActivity (1)
EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths(JNIEnv *e, jclass c, jstring apk) {
    natives::invoke_guest<void>(g_fn.activity_nativeSetPaths, e, c, (jobject)apk);
}

// org.cocos2dx.lib.Cocos2dxBitmap (1)
EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC(
    JNIEnv *e, jclass c, jint w, jint h, jbyteArray pixels) {
    natives::invoke_guest<void>(g_fn.bitmap_nativeInitBitmapDC, e, c, w, h, (jobject)pixels);
}

// org.cocos2dx.lib.Cocos2dxRenderer (12)
EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit(
    JNIEnv *e, jclass c, jint w, jint h) {
    if (w <= 0 || h <= 0) return;
    // Normalize to landscape dimensions so transient lock-screen / cutout portrait sizes never
    // permanently initialize CCEGLView in portrait aspect ratio.
    jint lw = std::max(w, h);
    jint lh = std::min(w, h);
    bool expected_fresh = true;
    if (!g_surface_fresh.compare_exchange_strong(expected_fresh, false, std::memory_order_acq_rel) &&
        g_game_inited.load(std::memory_order_acquire)) {
        return;
    }
    gl::reset_context_state();
    natives::invoke_guest<void>(g_fn.renderer_nativeInit, e, c, lw, lh);
    g_game_inited.store(true, std::memory_order_release);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(JNIEnv *e, jclass c) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeRender, e, c);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause(JNIEnv *e, jclass c) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeOnPause, e, c);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume(JNIEnv *e, jclass c) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeOnResume, e, c);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin(
    JNIEnv *e, jclass c, jint id, jfloat x, jfloat y) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeTouchesBegin, e, c, id, x, y);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd(
    JNIEnv *e, jclass c, jint id, jfloat x, jfloat y) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeTouchesEnd, e, c, id, x, y);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove(
    JNIEnv *e, jclass c, jintArray ids, jfloatArray xs, jfloatArray ys) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeTouchesMove, e, c, (jobject)ids, (jobject)xs, (jobject)ys);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel(
    JNIEnv *e, jclass c, jintArray ids, jfloatArray xs, jfloatArray ys) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeTouchesCancel, e, c, (jobject)ids, (jobject)xs, (jobject)ys);
}

EXPORT_JNI jboolean JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown(
    JNIEnv *e, jclass c, jint key_code) {
    if (!g_game_inited.load(std::memory_order_acquire)) return JNI_FALSE;
    return natives::invoke_guest<jboolean>(g_fn.renderer_nativeKeyDown, e, c, key_code);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText(
    JNIEnv *e, jclass c, jstring text) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeInsertText, e, c, (jobject)text);
}

EXPORT_JNI void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward(JNIEnv *e, jclass c) {
    if (!g_game_inited.load(std::memory_order_acquire)) return;
    natives::invoke_guest<void>(g_fn.renderer_nativeDeleteBackward, e, c);
}

EXPORT_JNI jstring JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText(JNIEnv *e, jclass c) {
    if (!g_game_inited.load(std::memory_order_acquire)) return e->NewStringUTF("");
    return natives::invoke_guest<jstring>(g_fn.renderer_nativeGetContentText, e, c);
}
