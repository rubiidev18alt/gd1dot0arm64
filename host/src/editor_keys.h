// SPDX-License-Identifier: MIT
#pragma once

// PC editor keybinds (port 1.1.2): Shift = temporary swipe, wheel = zoom, T = swipe toggle,
// Ctrl+Z / Ctrl+Shift+Z = undo/redo, +/- = zoom, arrows = pan the editor view.
//
// All guest calls MUST happen on the GL/game thread between frames (the same thread that runs
// nativeRender) — ModernPlatformHelper.onFrameRendered() drains the Java-side queues there.
// Outside the editor every entry point is a no-op: the chain below only produces a non-null
// EditorUI* while the running scene really is a LevelEditorLayer.
namespace editor_keys {
// Resolves the guest symbols. Safe to call repeatedly; first call is after libgame.so is loaded.
void init();

// Diagnostic bit mask for the verification runner:
//   bit0 (1)  = all required guest symbols resolved and layout constants present
//   bit1 (2)  = running scene contains a PlayLayer (vptr match)
//   bit2 (4)  = running scene contains a LevelEditorLayer (vptr match)
//   bit3 (8)  = the EditorUI was located for the live LevelEditorLayer
//   bit4 (16) = that pointer's vptr matches EditorUI (layout constant verified)
//   bit5 (32) = the running scene was resolved from CCDirector (field + RTTI)
//   bit6 (64) = the running scene's child array is non-empty
int probe_debug();
}  // namespace editor_keys
