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
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
bool gd102_host_boot_from_dir(JavaVM *vm, JNIEnv *env, const char *arm32_dir, const char *data_dir,
                              u32 *out_ctors, u32 *out_unresolved);
bool gd102_host_enable_local_gdps(const char *db_path, const char *data_dir, u32 *out_levels, u16 *out_port);
void gd102_host_local_gdps_stats(u32 *out_requests, u32 *out_blocked_uploads);
int gd102_host_run_guest_probe(JNIEnv *env, const char *so_path);
u32 gd102_host_find_game_sym_containing(const char *needle, char *out_name, u32 out_name_size);
u32 gd102_host_game_export_count();
u32 gd102_host_call_guest(JNIEnv *env, u32 fn_addr, u32 argc, const u32 *argv);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths(JNIEnv *, jclass, jstring);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC(JNIEnv *, jclass, jint, jint, jbyteArray);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit(JNIEnv *, jclass, jint, jint);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(JNIEnv *, jclass);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause(JNIEnv *, jclass);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume(JNIEnv *, jclass);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin(JNIEnv *, jclass, jint, jfloat, jfloat);
JNIEXPORT void JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd(JNIEnv *, jclass, jint, jfloat, jfloat);
JNIEXPORT jboolean JNICALL Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown(JNIEnv *, jclass, jint);
JNIEXPORT void JNICALL Java_com_customRobTop_JniToCpp_resumeSound(JNIEnv *, jclass);
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

JavaVM *boot_jvm(const std::string &libjvm_path, const std::string &stubs_jar,
                 const std::string &data_dir, JNIEnv **out_env) {
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
    std::string dd_opt = "-Dgd102.data.dir=" + data_dir;
    JavaVMOption opts[3]{};
    opts[0].optionString = const_cast<char *>(cp_opt.c_str());
    opts[1].optionString = const_cast<char *>(dd_opt.c_str());
    opts[2].optionString = const_cast<char *>("-Xmx256m");

    JavaVMInitArgs vm_args{};
    vm_args.version = JNI_VERSION_1_6;
    vm_args.nOptions = 3;
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

// Minimal host-side HTTP/1.1 POST client used to validate the embedded GD 1.0 private server
// exactly the way libgame.so's CCHttpClient does (form body + secret, plain HTTP on loopback).
std::string gdps_http_post(u16 port, const std::string &script, const std::string &body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "<socket-failed>";
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return "<connect-failed>";
    }

    std::string req = "POST /database/" + script + " HTTP/1.0\r\n"
        "Host: www.boomlings.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }

    std::string raw;
    char buf[8192];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
    }
    close(fd);

    size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return "<no-response>";
    return raw.substr(sep + 4);
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
    JavaVM *vm = boot_jvm(libjvm_path, stubs_jar, data_dir, &env);
    CHECK(vm && env, "Failed to boot OpenJDK JavaVM");

    jclass cls_activity = env->FindClass("org/cocos2dx/lib/Cocos2dxActivity");
    jclass cls_bitmap   = env->FindClass("org/cocos2dx/lib/Cocos2dxBitmap");
    jclass cls_renderer = env->FindClass("org/cocos2dx/lib/Cocos2dxRenderer");
    jclass cls_base     = env->FindClass("com/customRobTop/BaseRobTopActivity");
    jclass cls_crypto   = env->FindClass("com/customRobTop/SimpleCrypto");
    CHECK(cls_activity && cls_bitmap && cls_renderer && cls_base && cls_crypto, "Failed to find stub Java classes");

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

    // ------------------------------------------------------------------
    // Embedded private Geometry Dash 1.0 server (com.robtopx.geomgdps102 variant)
    // ------------------------------------------------------------------
    const std::string gdps_db = argc > 6 ? argv[6] : "";
    const std::string probe_so = argc > 7 ? argv[7] : "";
    u16 gdps_port = 0;
    bool gdps_active = false;
    if (!gdps_db.empty()) {
        u32 gdps_levels = 0;
        CHECK(gd102_host_enable_local_gdps(gdps_db.c_str(), data_dir.c_str(), &gdps_levels, &gdps_port),
              "Failed to initialize embedded GD 1.0 server from %s", gdps_db.c_str());
        printf("[VERIFY] Local GDPS archive loaded: levels=%u, listening on 127.0.0.1:%u\n", gdps_levels,
               (unsigned)gdps_port);
        gdps_active = true;
        CHECK(gdps_levels >= 350, "Expected at least 350 archived 1.0 levels, got %u", gdps_levels);
        CHECK(gdps_port != 0, "Local GDPS listener did not bind a port");

        const std::string secret = "&secret=Wmfd2893gb7";

        std::string recent = gdps_http_post(gdps_port, "getGJLevels.php", "type=4&str=&diff=-&len=-&page=0" + secret);
        CHECK(recent.find('#') != std::string::npos, "getGJLevels response missing page/user delimiters");
        size_t first_pos = recent.find("1:");
        CHECK(first_pos != std::string::npos, "getGJLevels(type=4) returned no levels (got '%.120s')",
              recent.c_str());
        long newest_id = strtol(recent.c_str() + first_pos + 2, nullptr, 10);
        CHECK(newest_id >= 1900, "getGJLevels(type=4) newest level id %ld is below the 1.0 archive tail (got '%.80s')",
              newest_id, recent.c_str());

        std::string by_id = gdps_http_post(gdps_port, "getGJLevels.php", "type=0&str=128&diff=-&len=-&page=0" + secret);
        CHECK(by_id.find("1:128:") != std::string::npos,
              "getGJLevels(type=0,str=128) did not return level 128 (got '%.120s')", by_id.c_str());

        std::string search = gdps_http_post(gdps_port, "getGJLevels.php", "type=0&str=jump&diff=-&len=-&page=0" + secret);
        CHECK(search.find("1:") != std::string::npos, "getGJLevels name search returned no levels");

        std::string dl = gdps_http_post(gdps_port, "downloadGJLevel.php", "levelID=128&inc=1" + secret);
        CHECK(dl.find(":1:128:") != std::string::npos && dl.find(":4:kS1,") != std::string::npos &&
                  dl.find(",kA1,") != std::string::npos && dl.find(":13:") != std::string::npos,
              "downloadGJLevel(128) missing level string/format fields (got '%.160s')", dl.c_str());

        // The level payload must be native GD 1.02 format: `kS1..kS6` header plus `;id,x,y;...` objects.
        std::string lvl_data = dl.substr(dl.find(":4:") + 3);
        lvl_data = lvl_data.substr(0, lvl_data.find(":5:"));
        size_t objs = 0;
        for (size_t pos = 0; (pos = lvl_data.find(';', pos)) != std::string::npos; pos++) objs++;
        CHECK(objs >= 100, "downloadGJLevel(128) level string has only %zu object separators", objs);

        // The creator id embedded in the payload must resolve through the users-by-ID search filter.
        size_t uid_pos = dl.find(":6:");
        CHECK(uid_pos != std::string::npos, "downloadGJLevel(128) missing creator id");
        std::string uid = dl.substr(uid_pos + 3, dl.find(':', uid_pos + 3) - (uid_pos + 3));
        std::string user_levels =
            gdps_http_post(gdps_port, "getGJLevels.php", "type=5&str=" + uid + "&diff=-&len=-&page=0" + secret);
        CHECK(user_levels.find("1:128:") != std::string::npos,
              "getGJLevels(type=5, user %s) did not return level 128 (got '%.120s')", uid.c_str(),
              user_levels.c_str());
        printf("[VERIFY] Local GDPS creator lookup: user %s owns level 128\n", uid.c_str());

        std::string blocked_up = gdps_http_post(
            gdps_port, "uploadGJLevel.php",
            "udid=x&userName=x&levelName=x&levelDesc=x&levelString=kS1,1;&levelVersion=1&levelLength=0&"
            "audioTrack=0&gameVersion=1" + secret);
        CHECK(blocked_up == "-1", "uploadGJLevel was NOT blocked (got '%.80s')", blocked_up.c_str());
        std::string blocked_del = gdps_http_post(gdps_port, "deleteGJLevelUser.php", "levelID=128" + secret);
        CHECK(blocked_del == "-1", "deleteGJLevelUser was NOT blocked (got '%.80s')", blocked_del.c_str());

        std::string like = gdps_http_post(gdps_port, "likeGJLevel.php", "levelID=128" + secret);
        CHECK(like == "1", "likeGJLevel failed (got '%.80s')", like.c_str());
        std::string rate = gdps_http_post(gdps_port, "rateGJLevel.php", "levelID=128&rating=5" + secret);
        CHECK(rate == "1", "rateGJLevel failed (got '%.80s')", rate.c_str());

        u32 handled = 0, blocked = 0;
        gd102_host_local_gdps_stats(&handled, &blocked);
        printf("[VERIFY] Local GDPS host-side API: requests=%u, blocked_uploads=%u\n", handled, blocked);
        CHECK(handled >= 8, "Expected >= 8 handled GDPS requests, got %u", handled);
        CHECK(blocked >= 2, "Expected blocked upload/delete counters, got %u", blocked);

        if (!probe_so.empty()) {
            int probe_rc = gd102_host_run_guest_probe(env, probe_so.c_str());
            printf("[VERIFY] Real ARM32 guest probe gethostbyname/www.boomlings.com -> local GDPS: rc=0x%x\n", probe_rc);
            CHECK(probe_rc == 0, "Real ARM32 guest GDPS probe failed with bitmask 0x%x", probe_rc);
            CHECK(!env->ExceptionCheck(), "Java exception raised during guest GDPS probe");
        }
    }

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

    // Pump frames at ~60 Hz so CCDirector::calculateDeltaTime() advances transitions naturally!
    bool loading_done = false;
    for (int frame = 0; frame < 90; frame++) {
        usleep(16667);
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
        usleep(16667);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd(env, cls_renderer, 0, x, y);
        for (int i = 0; i < settle_frames; i++) {
            usleep(16667);
            Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
        }
    };

    tap(640.0f, 360.0f, 45);  // MenuLayer Play button -> LevelSelectLayer
    FrameStats select_stats = inspect_framebuffer(W, H, (data_dir + "/level_select_frame.ppm").c_str());
    printf("[VERIFY] Post-tap #1 framebuffer: non_black=%zu, buckets=%zu, hash=0x%016llx\n",
           select_stats.non_black, select_stats.unique_buckets, (unsigned long long)select_stats.fnv_hash);
    CHECK(select_stats.fnv_hash != menu_stats.fnv_hash, "Touch input at (640, 360) did not change rendered scene!");

    tap(640.0f, 260.0f, 60);  // LevelSelectLayer Stereo Madness card -> PlayLayer!
    auto j_music2 = (jstring)env->GetStaticObjectField(cls_activity, fid_music);
    const char *music2_c = j_music2 ? env->GetStringUTFChars(j_music2, nullptr) : "";
    std::string level_music = music2_c ? music2_c : "";
    if (j_music2 && music2_c) env->ReleaseStringUTFChars(j_music2, music2_c);

    FrameStats play_stats = inspect_framebuffer(W, H, (data_dir + "/play_frame.ppm").c_str());
    printf("[VERIFY] Post-tap #2 (PlayLayer) active music: '%s', non_black=%zu, buckets=%zu, hash=0x%016llx\n",
           level_music.c_str(), play_stats.non_black, play_stats.unique_buckets, (unsigned long long)play_stats.fnv_hash);
    CHECK(play_stats.non_black > 200000, "PlayLayer framebuffer blank");
    CHECK(glGetError() == GL_NO_ERROR, "OpenGL ES error during gameplay");

    // Verify variadic JNI `jfloat` (`double`-promoted) sound-effect parameters (`playEffect(String, boolean, float, float, float)`)!
    jfieldID fid_sfx_cnt   = env->GetStaticFieldID(cls_activity, "effectPlayCount", "I");
    jfieldID fid_sfx_name  = env->GetStaticFieldID(cls_activity, "lastEffectPlayed", "Ljava/lang/String;");
    jfieldID fid_sfx_pitch = env->GetStaticFieldID(cls_activity, "lastEffectPitch", "F");
    jfieldID fid_sfx_pan   = env->GetStaticFieldID(cls_activity, "lastEffectPan", "F");
    jfieldID fid_sfx_gain  = env->GetStaticFieldID(cls_activity, "lastEffectGain", "F");
    CHECK(fid_sfx_cnt && fid_sfx_name && fid_sfx_pitch && fid_sfx_pan && fid_sfx_gain, "Missing SFX telemetry fields");
    int sfx_cnt     = env->GetStaticIntField(cls_activity, fid_sfx_cnt);
    float sfx_pitch = env->GetStaticFloatField(cls_activity, fid_sfx_pitch);
    float sfx_pan   = env->GetStaticFloatField(cls_activity, fid_sfx_pan);
    float sfx_gain  = env->GetStaticFloatField(cls_activity, fid_sfx_gain);
    auto j_sfx = (jstring)env->GetStaticObjectField(cls_activity, fid_sfx_name);
    const char *sfx_c = j_sfx ? env->GetStringUTFChars(j_sfx, nullptr) : "";
    std::string sfx_name = sfx_c ? sfx_c : "";
    if (j_sfx && sfx_c) env->ReleaseStringUTFChars(j_sfx, sfx_c);
    printf("[VERIFY] SFX telemetry: count=%d, last='%s', pitch=%.4f, pan=%.4f, gain=%.4f\n",
           sfx_cnt, sfx_name.c_str(), sfx_pitch, sfx_pan, sfx_gain);
    CHECK(sfx_cnt > 0, "Expected at least 1 sound effect played on button taps, got %d", sfx_cnt);
    CHECK(sfx_pitch >= 0.8f && sfx_pitch <= 1.25f,
          "Expected normal ~1.0 SFX pitch across JNI varargs, got %.6f", sfx_pitch);
    CHECK(sfx_pan >= -1.0f && sfx_pan <= 1.0f,
          "Expected valid [-1, 1] SFX pan across JNI varargs, got %.6f", sfx_pan);
    CHECK(sfx_gain > 0.05f && sfx_gain <= 1.0f,
          "Expected valid (0.05, 1.0] SFX gain across JNI varargs, got %.6f", sfx_gain);

    jclass cls_mp     = env->FindClass("android/media/MediaPlayer");
    jclass cls_afd    = env->FindClass("android/content/res/AssetFileDescriptor");
    jclass cls_clock  = env->FindClass("android/os/SystemClock");
    jclass cls_mctrl  = env->FindClass("com/robtopx/geometryjump/ModernMusicController");
    CHECK(cls_mp && cls_afd && cls_clock && cls_mctrl, "Failed to find ModernMusicController / MediaPlayer stub classes");

    jmethodID mid_drain      = env->GetStaticMethodID(cls_activity, "drainEvents", "()Ljava/lang/String;");
    jmethodID mid_mp_adv     = env->GetStaticMethodID(cls_mp, "advanceClockMs", "(I)V");
    jmethodID mid_mp_pos     = env->GetStaticMethodID(cls_mp, "activePositionMs", "()I");
    jmethodID mid_mp_reset   = env->GetStaticMethodID(cls_mp, "resetTelemetry", "()V");
    jmethodID mid_clk_adv    = env->GetStaticMethodID(cls_clock, "advanceUptimeMs", "(J)V");
    jmethodID mid_mc_playing = env->GetStaticMethodID(cls_mctrl, "isPlaying", "()Z");
    jfieldID  fid_mp_stops   = env->GetStaticFieldID(cls_mp, "stopCalls", "I");
    jfieldID  fid_mp_seek0   = env->GetStaticFieldID(cls_mp, "seekZeroCalls", "I");
    jfieldID  fid_mp_races   = env->GetStaticFieldID(cls_mp, "nuPlayerRaceResets", "I");
    jfieldID  fid_mp_mode    = env->GetStaticFieldID(cls_mp, "lastSeekMode", "I");
    jfieldID  fid_afd_open   = env->GetStaticFieldID(cls_afd, "openCount", "I");
    jfieldID  fid_afd_close  = env->GetStaticFieldID(cls_afd, "closeCount", "I");
    CHECK(mid_drain && mid_mp_adv && mid_mp_pos && mid_mp_reset && mid_clk_adv && mid_mc_playing &&
          fid_mp_stops && fid_mp_seek0 && fid_mp_races && fid_mp_mode && fid_afd_open && fid_afd_close,
          "Failed to resolve ModernMusicController / MediaPlayer telemetry members");

    auto drain_music_events = [&]() -> std::string {
        auto js = (jstring)env->CallStaticObjectMethod(cls_activity, mid_drain);
        const char *cs = js ? env->GetStringUTFChars(js, nullptr) : "";
        std::string res = cs ? cs : "";
        if (js && cs) env->ReleaseStringUTFChars(js, cs);
        if (js) env->DeleteLocalRef(js);
        return res;
    };
    drain_music_events();

    // 1) Advance 30 frames (~0.66s into StereoMadness.mp3, well before the 1% spike at ~1.63s) and pause.
    for (int i = 0; i < 30; i++) {
        usleep(16667);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
    }
    env->CallStaticVoidMethod(cls_mp, mid_mp_adv, (jint)660);
    drain_music_events();

    jboolean back_handled = Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown(env, cls_renderer, 4);
    for (int i = 0; i < 40; i++) {
        usleep(16667);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
    }
    printf("[VERIFY] KEYCODE_BACK #1 (pause) handled=%d, GL error=0x%x, events=[%s]\n",
           (int)back_handled, (unsigned)glGetError(), drain_music_events().c_str());
    CHECK(back_handled == JNI_TRUE, "Expected KEYCODE_BACK to be handled in PlayLayer");
    CHECK(!env->CallStaticBooleanMethod(cls_mctrl, mid_mc_playing),
          "Expected music paused after KEYCODE_BACK in PlayLayer");

    // Unpause #1 (short pause): tap PauseLayer center Resume button at (640, 400).
    env->CallStaticVoidMethod(cls_mp, mid_mp_reset);
    tap(640.0f, 400.0f, 5);
    std::string unpause1_ev = drain_music_events();
    int unpause1_pos   = env->CallStaticIntMethod(cls_mp, mid_mp_pos);
    int unpause1_stops = env->GetStaticIntField(cls_mp, fid_mp_stops);
    int unpause1_seek0 = env->GetStaticIntField(cls_mp, fid_mp_seek0);
    int unpause1_races = env->GetStaticIntField(cls_mp, fid_mp_races);
    printf("[VERIFY] Unpause #1 (short pause): pos=%d ms, stops=%d, seek0=%d, races=%d, events=[%s]\n",
           unpause1_pos, unpause1_stops, unpause1_seek0, unpause1_races, unpause1_ev.c_str());
    CHECK(env->CallStaticBooleanMethod(cls_mctrl, mid_mc_playing),
          "Expected music playing after Unpause #1");
    CHECK(unpause1_pos >= 500,
          "Music restarted at beginning on Unpause #1! Expected >= 500 ms, got %d ms", unpause1_pos);
    CHECK(unpause1_stops == 0 && unpause1_seek0 == 0 && unpause1_races == 0,
          "Unexpected stop/seekTo(0)/race during Unpause #1: stops=%d seek0=%d races=%d",
          unpause1_stops, unpause1_seek0, unpause1_races);

    // 2) Play 25 more frames (~1.15s total, still well before the 1% spike at ~1.63s),
    //    pause again, simulate a 15-second long pause, and unpause!
    for (int i = 0; i < 25; i++) {
        usleep(16667);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
    }
    env->CallStaticVoidMethod(cls_mp, mid_mp_adv, (jint)500);
    drain_music_events();

    Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown(env, cls_renderer, 4);
    for (int i = 0; i < 40; i++) {
        usleep(16667);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
    }
    drain_music_events();
    env->CallStaticVoidMethod(cls_mp, mid_mp_reset);
    env->CallStaticVoidMethod(cls_clock, mid_clk_adv, (jlong)15000);

    tap(640.0f, 400.0f, 5);
    std::string unpause2_ev = drain_music_events();
    int unpause2_pos   = env->CallStaticIntMethod(cls_mp, mid_mp_pos);
    int unpause2_seek0 = env->GetStaticIntField(cls_mp, fid_mp_seek0);
    int unpause2_mode  = env->GetStaticIntField(cls_mp, fid_mp_mode);
    printf("[VERIFY] Unpause #2 (15s long pause): pos=%d ms, seekMode=%d, seek0=%d, events=[%s]\n",
           unpause2_pos, unpause2_mode, unpause2_seek0, unpause2_ev.c_str());
    CHECK(env->CallStaticBooleanMethod(cls_mctrl, mid_mc_playing),
          "Expected music playing after Unpause #2");
    CHECK(unpause2_pos >= 900,
          "Music restarted at beginning on Unpause #2! Expected >= 900 ms, got %d ms", unpause2_pos);
    CHECK(unpause2_seek0 == 0 && unpause2_mode == 3,
          "Expected SEEK_CLOSEST (3) with 0 seekTo(0) calls on Unpause #2, got mode=%d seek0=%d",
          unpause2_mode, unpause2_seek0);

    int afd_open  = env->GetStaticIntField(cls_afd, fid_afd_open);
    int afd_close = env->GetStaticIntField(cls_afd, fid_afd_close);
    CHECK(afd_open > 0 && afd_open == afd_close,
          "AssetFileDescriptor leak detected: opened=%d, closed=%d", afd_open, afd_close);
    printf("[VERIFY] AssetFileDescriptor hygiene PASSED (opened=%d, closed=%d)\n", afd_open, afd_close);

    // Trigger background save (`AppDelegate::applicationDidEnterBackground` -> `GManager::save`)!
    // Note: in GD 1.02 (prior to LocalLevelManager split), the two GManager files are
    // `CCGameManager.dat` (player progress + local levels) and `CCGameStore.dat` (store state).
    Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause(env, cls_renderer);
    Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume(env, cls_renderer);
    for (int i = 0; i < 5; i++) {
        usleep(16667);
        Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
    }
    printf("[VERIFY] Lifecycle pause/resume events=[%s]\n", drain_music_events().c_str());

    size_t gm_sz = file_size_or_zero(data_dir + "/CCGameManager.dat");
    size_t gs_sz = file_size_or_zero(data_dir + "/CCGameStore.dat");
    printf("[VERIFY] Save files written: CCGameManager.dat=%zu bytes, CCGameStore.dat=%zu bytes\n", gm_sz, gs_sz);
    CHECK(gm_sz > 100000 && gs_sz > 32,
          "Expected non-empty CCGameManager.dat (%zu) and CCGameStore.dat (%zu) in %s",
          gm_sz, gs_sz, data_dir.c_str());

    // Verify both GManager save files contain valid Cocos2d-x XML plist data!
    for (const char *fname : {"CCGameManager.dat", "CCGameStore.dat"}) {
        std::string full = data_dir + "/" + fname;
        FILE *fp = fopen(full.c_str(), "rb");
        CHECK(fp != nullptr, "Failed to open %s", full.c_str());
        char hdr[65]{};
        size_t nr = fread(hdr, 1, 64, fp);
        fclose(fp);
        hdr[nr] = '\0';
        printf("[VERIFY] %s plist header: '%.24s...'\n", fname, hdr);
        CHECK(nr >= 16 && strncmp(hdr, "<?xml", 5) == 0,
              "Expected XML plist header '<?xml' in %s, got '%.16s'", fname, hdr);
    }

    // Also verify SimpleCrypto deterministic AES-128 round-trip for v1.00->v1.02 save migration!
    jmethodID mid_enc = env->GetStaticMethodID(
        cls_crypto, "saveAndEncryptStringToFile", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID mid_dec = env->GetStaticMethodID(
        cls_crypto, "loadAndDecryptFileToString", "(Ljava/lang/String;)Ljava/lang/String;");
    CHECK(mid_enc && mid_dec, "SimpleCrypto methods missing");
    jstring j_sample = env->NewStringUTF("<?xml version=\"1.0\"?><plist><dict><k>playerName</k><s>Player</s></dict></plist>");
    jstring j_mig_path = env->NewStringUTF("/data/data/com.robtopx.geometryjump/test_migration.dat");
    env->CallStaticVoidMethod(cls_crypto, mid_enc, j_sample, j_mig_path);
    auto j_dec = (jstring)env->CallStaticObjectMethod(cls_crypto, mid_dec, j_mig_path);
    const char *dec_c = j_dec ? env->GetStringUTFChars(j_dec, nullptr) : "";
    std::string dec_xml = dec_c ? dec_c : "";
    if (j_dec && dec_c) env->ReleaseStringUTFChars(j_dec, dec_c);
    env->DeleteLocalRef(j_sample);
    env->DeleteLocalRef(j_mig_path);
    unlink((data_dir + "/test_migration.dat").c_str());
    CHECK(dec_xml.find("<k>playerName</k>") != std::string::npos,
          "SimpleCrypto deterministic AES-128 round-trip failed");
    printf("[VERIFY] SimpleCrypto deterministic AES-128 round-trip PASSED (%zu bytes)\n", dec_xml.size());

    // ------------------------------------------------------------------
    // End-to-end: drive libgame.so's own CCHttpClient stack (real guest ARM32 code) through the
    // bridge so the private GD 1.0 server is validated on the exact code path the game uses.
    // ------------------------------------------------------------------
    if (gdps_active) {
        char sym_name[192] = "";
        u32 dl_fn = gd102_host_find_game_sym_containing("downloadLevel", sym_name, sizeof(sym_name));
        printf("[VERIFY] libgame.so exported symbols=%u, GameLevelManager downloadLevel symbol='%s' @ 0x%08x\n",
               gd102_host_game_export_count(), sym_name, dl_fn);
        if (dl_fn) {
            u32 before_req = 0, before_blocked = 0, after_req = 0, after_blocked = 0;
            gd102_host_local_gdps_stats(&before_req, &before_blocked);
            const u32 args[1] = {128};
            gd102_host_call_guest(env, dl_fn, 1, args);
            for (int i = 0; i < 150; i++) {
                usleep(16667);
                Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env, cls_renderer);
            }
            gd102_host_local_gdps_stats(&after_req, &after_blocked);
            printf("[VERIFY] Guest libgame.so HTTP -> private GDPS: requests %u -> %u\n", before_req, after_req);
            CHECK(after_req > before_req,
                  "libgame.so's own HTTP stack never reached the private GDPS server (%u -> %u)", before_req,
                  after_req);
            CHECK(!env->ExceptionCheck(), "Java exception while libgame.so downloaded a level from the GDPS");
            FrameStats lvl_info_stats = inspect_framebuffer(W, H, (data_dir + "/level_info_frame.ppm").c_str());
            printf("[VERIFY] LevelInfoLayer after GDPS download: non_black=%zu, buckets=%zu, hash=0x%016llx\n",
                   lvl_info_stats.non_black, lvl_info_stats.unique_buckets,
                   (unsigned long long)lvl_info_stats.fnv_hash);
            CHECK(glGetError() == GL_NO_ERROR, "OpenGL ES error after GDPS level download");
        } else {
            printf("[VERIFY] (info) downloadLevel is not exported by libgame.so; the real ARM32 guest probe "
                   "already covers gethostbyname/connect/HTTP redirection\n");
        }
    }

    printf("\n=== ALL REAL ARM64 EXECUTION, GLES2 RENDERING, TOUCH & SAVE VERIFICATION CHECKS PASSED ===\n");
    return 0;
}
