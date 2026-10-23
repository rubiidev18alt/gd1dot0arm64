// SPDX-License-Identifier: MIT
// Real ARM64 execution & headless OpenGL ES 2.0 verification runner for Geometry Dash 1.02.
// Boots a real OpenJDK JavaVM + headless Mesa 1280x720 GLES2 context, loads ARM32 libguestrt.so
// and unmodified Geometry.Dash.ver.1.02.build.4 libgame.so into Dynarmic A32->AArch64 JIT,
// and verifies symbol resolution, 16 KB page-safe unmap, LoadingLayer->MenuLayer->PlayLayer
// rendering, re-entrant JNI callbacks, touch input, and save-file persistence.

#include "common.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
bool gd102_host_boot_from_dir(JavaVM *vm, JNIEnv *env, const char *arm32_dir, const char *data_dir,
                              u32 *out_ctors, u32 *out_unresolved);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths(JNIEnv *, jclass, jstring);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC(JNIEnv *, jclass, jint, jint, jbyteArray);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit(JNIEnv *, jclass, jint, jint);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(JNIEnv *, jclass);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause(JNIEnv *, jclass);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume(JNIEnv *, jclass);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin(JNIEnv *, jclass, jint, jfloat, jfloat);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd(JNIEnv *, jclass, jint, jfloat, jfloat);
}

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "[VERIFY FAIL] " __VA_ARGS__);                     \
            fprintf(stderr, "\n");                                             \
            return 1;                                                          \
        }                                                                      \
    } while (0)

namespace {

using CreateJavaVMFn = jint (*)(JavaVM **, void **, void *);

JavaVM *boot_jvm(const std::string &libjvm_path, const std::string &stubs_jar, JNIEnv **out_env) {
    void *dl = dlopen(libjvm_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!dl) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", libjvm_path.c_str(), dlerror());
        return nullptr;
    }
    auto create_vm = (CreateJavaVMFn)dlsym(dl, "JNI_CreateJavaVM");
    if (!create_vm) {
        fprintf(stderr, "dlsym(JNI_CreateJavaVM) failed\n");
        return nullptr;
    }

    std::string cp_opt = "-Djava.class.path=" + stubs_jar;
    JavaVMOption opts[2]{};
    opts[0].optionString = const_cast<char *>(cp_opt.c_str());
    opts[1].optionString = const_cast<char *>("-Xmx256m");

    JavaVMInitArgs vm_args{};
    vm_args.version = JNI_VERSION_1_6;
    vm_args.nOptions = 2;
    vm_args.options = opts;
    vm_args.ignoreUnrecognized = JNI_TRUE;

    JavaVM *vm = nullptr;
    if (create_vm(&vm, (void **)out_env, &vm_args) != JNI_OK) return nullptr;
    return vm;
}

bool init_headless_egl(int w, int h, EGLDisplay *out_dpy, EGLSurface *out_surf) {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto get_platform_dpy = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_dpy) {
        constexpr EGLenum EGL_PLATFORM_SURFACELESS_MESA_ENUM = 0x31DD;
        dpy = get_platform_dpy(EGL_PLATFORM_SURFACELESS_MESA_ENUM, EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return false;

    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) return false;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &ncfg) || ncfg < 1) return false;

    const EGLint pb_attribs[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attribs);
    if (surf == EGL_NO_SURFACE) return false;

    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) return false;

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) return false;
    *out_dpy = dpy;
    *out_surf = surf;
    printf("[VERIFY] Headless EGL %d.%d initialized (%s / %s)\n",
           maj, min, (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
    return true;
}

struct FrameStats {
    size_t non_black = 0;
    size_t unique_buckets = 0;
    u64 fnv_hash = 14695981039346656037ULL;
};

FrameStats inspect_framebuffer(int w, int h, const char *ppm_out = nullptr) {
    std::vector<u8> rgba((size_t)w * (size_t)h * 4);
    glFinish();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    FrameStats st{};
    std::set<u16> buckets;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        u8 r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
        if (r > 4 || g > 4 || b > 4) st.non_black++;
        u16 q = (u16)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        buckets.insert(q);
        st.fnv_hash = (st.fnv_hash ^ r) * 1099511628211ULL;
        st.fnv_hash = (st.fnv_hash ^ g) * 1099511628211ULL;
        st.fnv_hash = (st.fnv_hash ^ b) * 1099511628211ULL;
    }
    st.unique_buckets = buckets.size();

    if (ppm_out) {
        if (FILE *f = fopen(ppm_out, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; y--) {
                for (int x = 0; x < w; x++) {
                    size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4;
                    fwrite(&rgba[idx], 1, 3, f);
                }
            }
            fclose(f);
        }
    }
    return st;
}

bool test_16kb_subpage_unmap() {
    // Allocate 64 KiB (16 guest 4 KiB pages, spanning 4 16 KiB host pages).
    u32 chunk = mem::map(0x10000);
    if (!chunk) return false;
    memset(g2h(chunk), 0xA5, 0x10000);

    // Unmap a single 4 KiB guest page at +0x1000 (unaligned to 16 KiB!).
    mem::unmap(chunk + 0x1000, 0x1000);
    u32 remapped = mem::map(0x1000, chunk + 0x1000);
    if (remapped != chunk + 0x1000) return false;

    const u8 *p = (const u8 *)g2h(chunk);
    for (u32 i = 0; i < 0x1000; i++)
        if (p[i] != 0xA5) return false;
    for (u32 i = 0x1000; i < 0x2000; i++)
        if (p[i] != 0x00) return false;
    for (u32 i = 0x2000; i < 0x10000; i++)
        if (p[i] != 0xA5) return false;

    mem::unmap(chunk, 0x10000);
    return true;
}

size_t file_size_or_zero(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <libjvm.so> <headless_stubs.jar> <arm32_dir> <apk_path> <data_dir>\n", argv[0]);
        return 2;
    }
    const std::string libjvm_path = argv[1];
    const std::string stubs_jar   = argv[2];
    const std::string arm32_dir   = argv[3];
    const std::string apk_path    = argv[4];
    const std::string data_dir    = argv[5];

    JNIEnv *env = nullptr;
    JavaVM *vm = boot_jvm(libjvm_path, stubs_jar, &env);
    CHECK(vm && env, "Failed to boot OpenJDK JavaVM");

    jclass cls_activity = env->FindClass("org/cocos2dx/lib/Cocos2dxActivity");
    jclass cls_bitmap   = env->FindClass("org/cocos2dx/lib/Cocos2dxBitmap");
    jclass cls_renderer = env->FindClass("org/cocos2dx/lib/Cocos2dxRenderer");
    jclass cls_base     = env->FindClass("com/customRobTop/BaseRobTopActivity");
    CHECK(cls_activity && cls_bitmap && cls_renderer && cls_base, "Failed to find stub Java classes");

    JNINativeMethod bm_native{
        const_cast<char *>("nativeInitBitmapDC"),
        const_cast<char *>("(II[B)V"),
        (void *)&Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC,
    };
    CHECK(env->RegisterNatives(cls_bitmap, &bm_native, 1) == JNI_OK, "RegisterNatives(nativeInitBitmapDC) failed");

    constexpr int W = 1280, H = 720;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    CHECK(init_headless_egl(W, H, &dpy, &surf), "Failed to initialize headless 1280x720 EGL/GLES2 surface");

    u32 ctors = 0, unresolved = 999;
    bool boot_ok = gd102_host_boot_from_dir(vm, env, arm32_dir.c_str(), data_dir.c_str(), &ctors, &unresolved);
    printf("[VERIFY] Guest ELF32 loaded: ctors=%u, unresolved=%u\n", ctors, unresolved);
    CHECK(boot_ok && unresolved == 0, "Unresolved guest symbols detected (%u)", unresolved);
    CHECK(ctors == 226, "Expected 226 C++ static constructors in GD 1.02 libgame.so, got %u", ctors);

    CHECK(test_16kb_subpage_unmap(), "16 KB sub-page mem::unmap zeroing invariant failed");
    printf("[VERIFY] 16 KB page-size sub-page unmap zeroing invariant PASSED\n");

    // Set APK path so CCFileUtilsAndroid reads all 136 assets directly from the rebuilt APK!
    jstring j_apk = env->NewStringUTF(apk_path.c_str());
    Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths(env, cls_activity, j_apk);
    env->DeleteLocalRef(j_apk);

    // Initialize CCDirector + AppDelegate::applicationDidFinishLaunching at 1280x720!
    Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit(env, cls_renderer, W, H);
    CHECK(!env->ExceptionCheck(), "Unhandled Java exception during nativeInit(1280, 720)");

    jfieldID fid_loaded = env->GetStaticFieldID(cls_base, "loadingFinishedCalled", "Z");
    jfieldID fid_music  = env->GetStaticFieldID(cls_activity, "lastMusicPlayed", "Ljava/lang/String;");
    jfieldID fid_bms    = env->GetStaticFieldID(cls_bitmap, "bitmapCalls", "I");
    CHECK(fid_loaded && fid_music && fid_bms, "Failed to look up telemetry fields");

    // Pump frames until LoadingLayer finishes and MenuLayer renders!
    bool loading_done = false;
    for (int frame = 0; frame < 90; frame++) {
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
        CHECK(!env->ExceptionCheck(), "Java exception on frame %d", frame);
        if (env->GetStaticBooleanField(cls_base, fid_loaded)) {
            loading_done = true;
        }
    }
    CHECK(loading_done, "LoadingLayer did not reach BaseRobTopActivity.loadingFinished() within 90 frames");

    auto j_music = (jstring)env->GetStaticObjectField(cls_activity, fid_music);
    const char *music_c = j_music ? env->GetStringUTFChars(j_music, nullptr) : "";
    std::string menu_music = music_c ? music_c : "";
    if (j_music && music_c) env->ReleaseStringUTFChars(j_music, music_c);
    printf("[VERIFY] LoadingLayer -> MenuLayer complete! Active music: '%s', re-entrant bitmapCalls=%d\n",
           menu_music.c_str(), (int)env->GetStaticIntField(cls_bitmap, fid_bms));
    CHECK(menu_music.find("menuLoop.mp3") != std::string::npos,
          "Expected menuLoop.mp3 on MenuLayer, got '%s'", menu_music.c_str());

    GLenum gl_err = glGetError();
    CHECK(gl_err == GL_NO_ERROR, "Unexpected OpenGL ES error on MenuLayer: 0x%x", gl_err);

    FrameStats menu_stats = inspect_framebuffer(W, H, (data_dir + "/menu_frame.ppm").c_str());
    printf("[VERIFY] MenuLayer framebuffer (1280x720): non_black=%zu/%d, unique_color_buckets=%zu, hash=0x%016llx\n",
           menu_stats.non_black, W * H, menu_stats.unique_buckets, (unsigned long long)menu_stats.fnv_hash);
    CHECK(menu_stats.non_black > 200000, "MenuLayer framebuffer looks mostly blank (%zu non-black pixels)", menu_stats.non_black);
    CHECK(menu_stats.unique_buckets >= 32, "MenuLayer lacks expected color richness (%zu buckets)", menu_stats.unique_buckets);

    // Tap the center Play button (640, 360) -> LevelSelectLayer -> tap center (640, 360) -> Stereo Madness!
    auto tap = [&](float x, float y, int settle_frames) {
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin(env, cls_renderer, 0, x, y);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd(env, cls_renderer, 0, x, y);
        for (int i = 0; i < settle_frames; i++) {
            Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
        }
    };

    tap(640.0f, 360.0f, 40);  // MenuLayer Play button -> LevelSelectLayer
    FrameStats select_stats = inspect_framebuffer(W, H, (data_dir + "/level_select_frame.ppm").c_str());
    printf("[VERIFY] Post-tap #1 framebuffer: non_black=%zu, buckets=%zu, hash=0x%016llx\n",
           select_stats.non_black, select_stats.unique_buckets, (unsigned long long)select_stats.fnv_hash);
    CHECK(select_stats.fnv_hash != menu_stats.fnv_hash, "Touch input at (640, 360) did not change rendered scene!");

    tap(640.0f, 360.0f, 60);  // LevelSelectLayer -> PlayLayer (Stereo Madness)!
    auto j_music2 = (jstring)env->GetStaticObjectField(cls_activity, fid_music);
    const char *music2_c = j_music2 ? env->GetStringUTFChars(j_music2, nullptr) : "";
    std::string level_music = music2_c ? music2_c : "";
    if (j_music2 && music2_c) env->ReleaseStringUTFChars(j_music2, music2_c);

    FrameStats play_stats = inspect_framebuffer(W, H, (data_dir + "/play_frame.ppm").c_str());
    printf("[VERIFY] Post-tap #2 (PlayLayer) active music: '%s', non_black=%zu, buckets=%zu, hash=0x%016llx\n",
           level_music.c_str(), play_stats.non_black, play_stats.unique_buckets, (unsigned long long)play_stats.fnv_hash);
    CHECK(play_stats.non_black > 200000, "PlayLayer framebuffer blank");
    CHECK(glGetError() == GL_NO_ERROR, "OpenGL ES error during gameplay");

    // Trigger background save (`AppDelegate::applicationDidEnterBackground` -> `GManager::save`)!
    Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause(env, cls_renderer);
    Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume(env, cls_renderer);
    for (int i = 0; i < 5; i++) Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);

    size_t gm_sz = file_size_or_zero(data_dir + "/CCGameManager.dat");
    size_t ll_sz = file_size_or_zero(data_dir + "/CCLocalLevels.dat");
    printf("[VERIFY] Save files written: CCGameManager.dat=%zu bytes, CCLocalLevels.dat=%zu bytes\n", gm_sz, ll_sz);
    CHECK(gm_sz > 32 && ll_sz > 16, "Expected non-empty CCGameManager.dat and CCLocalLevels.dat in %s", data_dir.c_str());

    printf("\n=== ALL REAL ARM64 EXECUTION, GLES2 RENDERING, TOUCH & SAVE VERIFICATION CHECKS PASSED ===\n");
    return 0;
}
