// SPDX-License-Identifier: MIT
#include "editor_keys.h"

#include "common.h"
#include "cpu.h"
#include "loader.h"

#include <jni.h>

#include <cstring>

// ---------------------------------------------------------------------------
// Layout constants. Source of truth: tools/analysis/guest_keybind_facts.txt,
// produced by scripts/inspect_guest_keybinds.sh against the real ARM32 guests.
//   kSwipeFlagOff = `strb`/`ldrb` offset inside EditorUI::toggleSwipe
//   kUiFieldOff   = the `str` that stores EditorUI::create()'s result in LevelEditorLayer::init
//   kVtblDelta    = object vptr minus the _ZTV<Class> symbol (Itanium: 2 words with RTTI)
// The runner asserts bit0/bit1 of probe_debug() on every CI run, so wrong constants here fail
// the build rather than corrupting the guest.
// ---------------------------------------------------------------------------
namespace {
constexpr u32 kVtblDelta = 8;
constexpr int kSwipeFlagOff = 348;  // toggleSwipe: 0xae<<1, identical in 1.02 and 1.1
constexpr int kUiFieldOff = -1;    // FACTS

struct Syms {
    u32 shared_director = 0;
    u32 get_running_scene = 0;
    u32 get_children = 0;
    u32 arr_count = 0;
    u32 arr_at = 0;
    u32 pos = 0;
    u32 pos_x = 0;
    u32 pos_y = 0;
    u32 set_pos = 0;
    u32 ztv_lel = 0;
    u32 ztv_ui = 0;
    u32 ztv_play = 0;
    u32 get_game_layer = 0;
    u32 toggle_swipe = 0;
    u32 zoom_in = 0;
    u32 zoom_out = 0;
    u32 undo = 0;
    u32 redo = 0;
    u32 constrain = 0;
    bool resolved = false;
};
Syms g;

u32 find1(const char *name) { return loader::lookup(name); }
u32 find2(const char *a, const char *b) {
    u32 v = find1(a);
    return v ? v : find1(b);
}

void resolve() {
    g.resolved = true;
    g.shared_director  = find1("_ZN7cocos2d10CCDirector14sharedDirectorEv");
    g.get_running_scene = find1("_ZN7cocos2d10CCDirector15getRunningSceneEv");
    g.get_children     = find1("_ZN7cocos2d6CCNode11getChildrenEv");
    g.arr_count        = find2("_ZNK7cocos2d7CCArray5countEv", "_ZN7cocos2d7CCArray5countEv");
    g.arr_at           = find1("_ZN7cocos2d7CCArray13objectAtIndexEj");
    g.pos              = find1("_ZN7cocos2d6CCNode11getPositionEv");
    g.pos_x            = find1("_ZN7cocos2d6CCNode14getPositionXEv");
    g.pos_y            = find1("_ZN7cocos2d6CCNode14getPositionYEv");
    g.set_pos          = find1("_ZN7cocos2d6CCNode11setPositionEff");
    g.ztv_lel          = find1("_ZTV16LevelEditorLayer");
    g.ztv_ui           = find1("_ZTV8EditorUI");
    g.ztv_play         = find1("_ZTV9PlayLayer");
    g.get_game_layer   = find2("_ZNK16LevelEditorLayer12getGameLayerEv",
                               "_ZN16LevelEditorLayer12getGameLayerEv");
    g.toggle_swipe     = find2("_ZN8EditorUI11toggleSwipeEv", "_ZN8EditorUI12toggleSwipeEv");
    g.zoom_in          = find1("_ZN8EditorUI6zoomInEv");
    g.zoom_out         = find1("_ZN8EditorUI7zoomOutEv");
    g.undo             = find2("_ZN8EditorUI14undoLastActionEv",
                               "_ZN16LevelEditorLayer14undoLastActionEv");
    g.redo             = find2("_ZN8EditorUI14redoLastActionEv",
                               "_ZN16LevelEditorLayer14redoLastActionEv");
    g.constrain        = find2("_ZN8EditorUI26constrainGameLayerPositionEv",
                               "_ZN8EditorUI25constrainGameLayerPositionEv");
}

struct EditorCtx {
    u32 lel = 0;
    u32 ui = 0;
};

bool constants_ok() { return kSwipeFlagOff >= 0 && kUiFieldOff >= 0; }

EditorCtx editor_ctx() {
    if (!g.resolved) resolve();
    EditorCtx ctx;
    if (!constants_ok()) return ctx;
    if (!g.shared_director || !g.get_running_scene || !g.get_children || !g.arr_count || !g.arr_at ||
        !g.ztv_lel) {
        return ctx;
    }
    u32 dir = (u32)cpu::call32(g.shared_director, {});
    if (!dir) return ctx;
    u32 scene = (u32)cpu::call32(g.get_running_scene, {dir});
    if (!scene) return ctx;
    u32 children = (u32)cpu::call32(g.get_children, {scene});
    if (!children) return ctx;
    u32 n = (u32)cpu::call32(g.arr_count, {children});
    if (n > 16) n = 16;
    for (u32 i = 0; i < n; i++) {
        u32 ch = (u32)cpu::call32(g.arr_at, {children, i});
        if (!ch) continue;
        if (rd<u32>(ch) == g.ztv_lel + kVtblDelta) {
            ctx.lel = ch;
            break;
        }
    }
    if (!ctx.lel) return ctx;
    ctx.ui = rd<u32>(ctx.lel + (u32)kUiFieldOff);
    // Defensive: a wrong field offset must degrade to "no editor", never to a wild pointer.
    if (ctx.ui && g.ztv_ui && rd<u32>(ctx.ui) != g.ztv_ui + kVtblDelta) {
        ctx.ui = 0;
    }
    return ctx;
}

float f32(u32 bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

u32 f32bits(float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    return bits;
}
}  // namespace

namespace editor_keys {

void init() { resolve(); }

int probe_debug() {
    if (!g.resolved) resolve();
    int bits = 0;
    if (constants_ok() && g.shared_director && g.get_running_scene && g.get_children && g.arr_count &&
        g.arr_at && g.ztv_lel && g.ztv_play) {
        bits |= 1;
    }
    if (!g.shared_director || !g.get_running_scene || !g.get_children || !g.arr_count || !g.arr_at) {
        return bits;
    }
    u32 dir = (u32)cpu::call32(g.shared_director, {});
    if (!dir) return bits;
    u32 scene = (u32)cpu::call32(g.get_running_scene, {dir});
    if (!scene) return bits;
    u32 children = (u32)cpu::call32(g.get_children, {scene});
    if (!children) return bits;
    u32 n = (u32)cpu::call32(g.arr_count, {children});
    if (n > 16) n = 16;
    u32 lel = 0;
    for (u32 i = 0; i < n; i++) {
        u32 ch = (u32)cpu::call32(g.arr_at, {children, i});
        if (!ch) continue;
        u32 vptr = rd<u32>(ch);
        if (g.ztv_play && vptr == g.ztv_play + kVtblDelta) bits |= 2;
        if (g.ztv_lel && vptr == g.ztv_lel + kVtblDelta) {
            bits |= 4;
            lel = ch;
        }
    }
    if (lel && constants_ok()) {
        u32 ui = rd<u32>(lel + (u32)kUiFieldOff);
        if (ui) {
            bits |= 8;
            if (g.ztv_ui && rd<u32>(ui) == g.ztv_ui + kVtblDelta) bits |= 16;
        }
    }
    return bits;
}

}  // namespace editor_keys

// ---------------------------------------------------------------------------
// JNI entry points. Called only from ModernPlatformHelper.onFrameRendered()'s drain (GL thread)
// or directly by the headless verify runner on its render thread.
// ---------------------------------------------------------------------------
#define EXPORT_JNI_KEYBIND extern "C" __attribute__((visibility("default"))) JNIEXPORT

EXPORT_JNI_KEYBIND jboolean JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorProbe(JNIEnv *, jclass) {
    return editor_ctx().ui != 0 ? JNI_TRUE : JNI_FALSE;
}

EXPORT_JNI_KEYBIND jint JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorProbeDebug(JNIEnv *, jclass) {
    return (jint)editor_keys::probe_debug();
}

EXPORT_JNI_KEYBIND void JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorZoom(JNIEnv *, jclass, jint steps) {
    EditorCtx ctx = editor_ctx();
    if (!ctx.ui || !g.zoom_in || !g.zoom_out) return;
    for (int i = 0; i < steps && i < 8; i++) cpu::call32(g.zoom_in, {ctx.ui});
    for (int i = 0; i > steps && i > -8; i--) cpu::call32(g.zoom_out, {ctx.ui});
}

EXPORT_JNI_KEYBIND void JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorSwipe(JNIEnv *, jclass, jboolean on) {
    EditorCtx ctx = editor_ctx();
    if (!ctx.ui || !g.toggle_swipe) return;
    bool cur = rd<u8>(ctx.ui + (u32)kSwipeFlagOff) != 0;
    bool want = on == JNI_TRUE;
    if (cur != want) cpu::call32(g.toggle_swipe, {ctx.ui});  // toggles flag + button sprite together
}

EXPORT_JNI_KEYBIND void JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorToggleSwipe(JNIEnv *, jclass) {
    EditorCtx ctx = editor_ctx();
    if (ctx.ui && g.toggle_swipe) cpu::call32(g.toggle_swipe, {ctx.ui});
}

EXPORT_JNI_KEYBIND void JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorUndo(JNIEnv *, jclass, jint count) {
    EditorCtx ctx = editor_ctx();
    if (!ctx.ui || !g.undo) return;
    for (int i = 0; i < count && i < 8; i++) cpu::call32(g.undo, {ctx.ui});
}

EXPORT_JNI_KEYBIND void JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorRedo(JNIEnv *, jclass, jint count) {
    EditorCtx ctx = editor_ctx();
    if (!ctx.ui || !g.redo) return;
    for (int i = 0; i < count && i < 8; i++) cpu::call32(g.redo, {ctx.ui});
}

EXPORT_JNI_KEYBIND void JNICALL
Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeEditorPan(JNIEnv *, jclass, jint dx,
                                                                   jint dy) {
    EditorCtx ctx = editor_ctx();
    if (!ctx.ui || !ctx.lel || !g.get_game_layer || !g.set_pos) return;
    u32 game_layer = (u32)cpu::call32(g.get_game_layer, {ctx.lel});
    if (!game_layer) return;
    float x = 0.0f, y = 0.0f;
    if (g.pos_x && g.pos_y) {
        x = f32((u32)cpu::call32(g.pos_x, {game_layer}));
        y = f32((u32)cpu::call32(g.pos_y, {game_layer}));
    } else if (g.pos) {
        u64 r = cpu::call(g.pos, {game_layer});
        x = f32((u32)r);
        y = f32((u32)(r >> 32));
    } else {
        return;
    }
    x += (float)dx;
    y += (float)dy;
    cpu::call32(g.set_pos, {game_layer, f32bits(x), f32bits(y)});
    if (g.constrain) cpu::call32(g.constrain, {ctx.ui});
}
