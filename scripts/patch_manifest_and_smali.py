#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Deterministic Android 16 (API 36) + ARM64 manifest, apktool.yml, and smali patcher
for Geometry Dash 1.02 (build 4, com.robtopx.geometryjump).
"""

import pathlib
import re
import sys
import xml.etree.ElementTree as ET

ANDROID_NS = "http://schemas.android.com/apk/res/android"
ET.register_namespace("android", ANDROID_NS)


def attr_key(name: str) -> str:
    return f"{{{ANDROID_NS}}}{name}"


def replace_method(text: str, method_header: str, new_body: str) -> str:
    pattern = re.compile(
        r"^(" + re.escape(method_header) + r")\s*\n(.*?)^\.end method",
        re.MULTILINE | re.DOTALL,
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly 1 match for `{method_header}`, found {len(matches)}")
    replacement = f"{method_header}\n{new_body.rstrip()}\n.end method"
    return pattern.sub(lambda _: replacement, text, count=1)


def patch_apktool_yml(apk_dir: pathlib.Path) -> None:
    yml_path = apk_dir / "apktool.yml"
    txt = yml_path.read_text(encoding="utf-8")
    txt = re.sub(r"minSdkVersion:\s*'?\d+'?", "minSdkVersion: '24'", txt)
    txt = re.sub(r"targetSdkVersion:\s*'?\d+'?", "targetSdkVersion: '36'", txt)

    for entry in ("arm32/libguestrt.so", "arm32/libgame.so", "mp3", "ogg"):
        if entry not in txt:
            txt = txt.replace("doNotCompress:\n", f"doNotCompress:\n- {entry}\n", 1)

    yml_path.write_text(txt, encoding="utf-8")


def patch_manifest(apk_dir: pathlib.Path) -> None:
    manifest_path = apk_dir / "AndroidManifest.xml"
    tree = ET.parse(manifest_path)
    root = tree.getroot()

    # Remove legacy invasive/deprecated permissions that trigger Android 10-16 warnings or denials.
    drop_perms = {
        "android.permission.READ_PHONE_STATE",
        "android.permission.WRITE_EXTERNAL_STORAGE",
        "android.permission.READ_EXTERNAL_STORAGE",
    }
    for elem in list(root.findall("uses-permission")):
        if elem.get(attr_key("name")) in drop_perms:
            root.remove(elem)

    app = root.find("application")
    if app is None:
        raise RuntimeError("AndroidManifest.xml missing <application>")

    # Android 16 + 16 KB page alignment + Boomlings HTTP cleartext compatibility:
    app.set(attr_key("extractNativeLibs"), "false")
    app.set(attr_key("usesCleartextTraffic"), "true")
    app.set(attr_key("enableOnBackInvokedCallback"), "false")
    app.set(attr_key("resizeableActivity"), "true")
    app.set(attr_key("allowBackup"), "true")

    # Ultrawide aspect ratio support (up to 21.6:9 / 2.4) for modern phones.
    has_max_aspect = any(
        m.get(attr_key("name")) == "android.max_aspect" for m in app.findall("meta-data")
    )
    if not has_max_aspect:
        meta = ET.SubElement(app, "meta-data")
        meta.set(attr_key("name"), "android.max_aspect")
        meta.set(attr_key("value"), "2.4")

    # Remove defunct Chartboost impression activity if declared.
    for act in list(app.findall("activity")):
        name = act.get(attr_key("name"), "")
        if "chartboost" in name.lower():
            app.remove(act)

    # Patch main launcher activity (.GeometryJump).
    patched_main = False
    for act in app.findall("activity"):
        name = act.get(attr_key("name"), "")
        if name in (".GeometryJump", "com.robtopx.geometryjump.GeometryJump"):
            act.set(attr_key("exported"), "true")
            act.set(attr_key("screenOrientation"), "sensorLandscape")
            act.set(
                attr_key("configChanges"),
                "keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|smallestScreenSize|uiMode|density",
            )
            patched_main = True

    if not patched_main:
        raise RuntimeError("Main activity .GeometryJump not found in AndroidManifest.xml")

    tree.write(manifest_path, encoding="utf-8", xml_declaration=True)


def patch_geometry_jump(smali_root: pathlib.Path) -> None:
    p = smali_root / "com/robtopx/geometryjump/GeometryJump.smali"
    txt = p.read_text(encoding="utf-8")

    # 1. Always succeed GLES 2.0 detection (all API 24+ devices & headless CI emulators support GLES 2.0+).
    txt = replace_method(
        txt,
        ".method private detectOpenGLES20()Z",
        """    .locals 1

    const/4 v0, 0x1

    return v0""",
    )

    # 2. Boot the ARM32->ARM64 bridge at the very start of onCreate (before super.onCreate)
    #    and preserve the EGL context across pause/resume.
    old_super = "    invoke-super {p0, p1}, Lorg/cocos2dx/lib/Cocos2dxActivity;->onCreate(Landroid/os/Bundle;)V"
    if old_super not in txt:
        raise RuntimeError("GeometryJump.onCreate super call not found")
    txt = txt.replace(
        old_super,
        "    invoke-static {p0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->initBridge(Landroid/app/Activity;)V\n\n"
        + old_super,
        1,
    )

    old_egl_ver = "    invoke-virtual {v0, v3}, Lorg/cocos2dx/lib/Cocos2dxGLSurfaceView;->setEGLContextClientVersion(I)V"
    if old_egl_ver not in txt:
        raise RuntimeError("setEGLContextClientVersion call not found in GeometryJump.onCreate")
    txt = txt.replace(
        old_egl_ver,
        old_egl_ver
        + "\n\n    const/4 v3, 0x1\n\n"
        + "    invoke-virtual {v0, v3}, Landroid/opengl/GLSurfaceView;->setPreserveEGLContextOnPause(Z)V",
        1,
    )

    # 3. Null-safe onPause & immersive onResume + onWindowFocusChanged.
    txt = replace_method(
        txt,
        ".method protected onPause()V",
        """    .locals 1

    invoke-super {p0}, Lorg/cocos2dx/lib/Cocos2dxActivity;->onPause()V

    iget-object v0, p0, Lcom/robtopx/geometryjump/GeometryJump;->mGLView:Lorg/cocos2dx/lib/Cocos2dxGLSurfaceView;

    if-eqz v0, :cond_0

    invoke-virtual {v0}, Lorg/cocos2dx/lib/Cocos2dxGLSurfaceView;->onPause()V

    :cond_0
    return-void""",
    )

    txt = replace_method(
        txt,
        ".method protected onResume()V",
        """    .locals 1

    invoke-super {p0}, Lorg/cocos2dx/lib/Cocos2dxActivity;->onResume()V

    invoke-static {p0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->applyImmersive(Landroid/app/Activity;)V

    iget-object v0, p0, Lcom/robtopx/geometryjump/GeometryJump;->mGLView:Lorg/cocos2dx/lib/Cocos2dxGLSurfaceView;

    if-eqz v0, :cond_0

    invoke-virtual {v0}, Lorg/cocos2dx/lib/Cocos2dxGLSurfaceView;->onResume()V

    :cond_0
    return-void""",
    )

    # 4. Neutralize defunct Flurry analytics in onStart / onStop.
    txt = replace_method(
        txt,
        ".method public onStart()V",
        """    .locals 0

    invoke-super {p0}, Lorg/cocos2dx/lib/Cocos2dxActivity;->onStart()V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public onStop()V",
        """    .locals 0

    invoke-super {p0}, Lorg/cocos2dx/lib/Cocos2dxActivity;->onStop()V

    return-void""",
    )

    if "onWindowFocusChanged(Z)V" not in txt:
        txt += """

.method public onWindowFocusChanged(Z)V
    .locals 0

    invoke-super {p0, p1}, Landroid/app/Activity;->onWindowFocusChanged(Z)V

    if-eqz p1, :cond_0

    invoke-static {p0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->applyImmersive(Landroid/app/Activity;)V

    :cond_0
    return-void
.end method
"""

    p.write_text(txt, encoding="utf-8")


def patch_base_robtop(smali_root: pathlib.Path) -> None:
    p = smali_root / "com/customRobTop/BaseRobTopActivity.smali"
    txt = p.read_text(encoding="utf-8")

    # Replace TelephonyManager IMEI/deviceId lookup (crashes on Android 10-16) with ANDROID_ID UUID.
    txt = replace_method(
        txt,
        ".method public static getUserID()Ljava/lang/String;",
        """    .locals 1

    sget-object v0, Lcom/customRobTop/BaseRobTopActivity;->me:Lcom/customRobTop/BaseRobTopActivity;

    invoke-static {v0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->getSafeUserId(Landroid/content/Context;)Ljava/lang/String;

    move-result-object v0

    return-object v0""",
    )

    # Replace deprecated NetworkInfo check with NetworkCapabilities check.
    txt = replace_method(
        txt,
        ".method public static isNetworkAvailable()Z",
        """    .locals 1

    sget-object v0, Lcom/customRobTop/BaseRobTopActivity;->me:Lcom/customRobTop/BaseRobTopActivity;

    invoke-static {v0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->isNetworkAvailable(Landroid/content/Context;)Z

    move-result v0

    return v0""",
    )

    # Neutralize FlurryAgent.logEvent.
    txt = replace_method(
        txt,
        ".method public static logEvent(Ljava/lang/String;)V",
        """    .locals 0

    return-void""",
    )

    # Android 14+ (API 34+) RECEIVER_EXPORTED flag requirement on dynamic BroadcastReceiver registration.
    old_reg = "    invoke-virtual {p0, v1, v0}, Lcom/customRobTop/BaseRobTopActivity;->registerReceiver(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;"
    new_reg = "    invoke-static {p0, v1, v0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->registerScreenReceiver(Landroid/content/Context;Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;"
    if old_reg not in txt:
        raise RuntimeError("BaseRobTopActivity.registerReceiver call not found")
    txt = txt.replace(old_reg, new_reg, 1)

    p.write_text(txt, encoding="utf-8")

    # BaseRobTopActivity$4 (openURL Runnable): guard against missing browser / malformed URL.
    p4 = smali_root / "com/customRobTop/BaseRobTopActivity$4.smali"
    txt4 = p4.read_text(encoding="utf-8")
    txt4 = replace_method(
        txt4,
        ".method public run()V",
        """    .locals 2

    invoke-static {}, Lcom/customRobTop/BaseRobTopActivity;->access$0()Lcom/customRobTop/BaseRobTopActivity;

    move-result-object v0

    iget-object v1, p0, Lcom/customRobTop/BaseRobTopActivity$4;->val$url:Ljava/lang/String;

    invoke-static {v0, v1}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->safeOpenUrl(Landroid/content/Context;Ljava/lang/String;)V

    return-void""",
    )
    p4.write_text(txt4, encoding="utf-8")

    # BaseRobTopActivity$6 (downloadImage Runnable): replace removed Apache ByteArrayBuffer with ByteArrayOutputStream.
    p6 = smali_root / "com/customRobTop/BaseRobTopActivity$6.smali"
    txt6 = p6.read_text(encoding="utf-8")
    if "Lorg/apache/http/util/ByteArrayBuffer;" not in txt6:
        raise RuntimeError("Expected ByteArrayBuffer in BaseRobTopActivity$6.smali")
    txt6 = txt6.replace(
        "Lorg/apache/http/util/ByteArrayBuffer;->append(I)V",
        "Ljava/io/ByteArrayOutputStream;->write(I)V",
    )
    txt6 = txt6.replace(
        "Lorg/apache/http/util/ByteArrayBuffer;",
        "Ljava/io/ByteArrayOutputStream;",
    )
    p6.write_text(txt6, encoding="utf-8")

    # AppRater.openRate: safe URL launch on devices without Play Store.
    pr = smali_root / "com/customRobTop/AppRater.smali"
    txtr = pr.read_text(encoding="utf-8")
    txtr = replace_method(
        txtr,
        ".method public static openRate(Landroid/content/Context;)V",
        """    .locals 2

    new-instance v0, Ljava/lang/StringBuilder;

    const-string v1, "https://play.google.com/store/apps/details?id="

    invoke-direct {v0, v1}, Ljava/lang/StringBuilder;-><init>(Ljava/lang/String;)V

    invoke-virtual {p0}, Landroid/content/Context;->getPackageName()Ljava/lang/String;

    move-result-object v1

    invoke-virtual {v0, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    move-result-object v0

    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;

    move-result-object v0

    invoke-static {p0, v0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->safeOpenUrl(Landroid/content/Context;Ljava/lang/String;)V

    return-void""",
    )
    pr.write_text(txtr, encoding="utf-8")


def patch_chartboost_manager(smali_root: pathlib.Path) -> None:
    p = smali_root / "com/customRobTop/GameAdManagerCB.smali"
    txt = p.read_text(encoding="utf-8")

    txt = replace_method(
        txt,
        ".method public setup(Lcom/customRobTop/BaseRobTopActivity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
        """    .locals 0

    iput-object p1, p0, Lcom/customRobTop/GameAdManagerCB;->cAct_:Lcom/customRobTop/BaseRobTopActivity;

    iput-object p0, p0, Lcom/customRobTop/GameAdManagerCB;->me_:Lcom/customRobTop/GameAdManagerCB;

    return-void""",
    )

    for void_m in (
        ".method public cacheInterstitial()V",
        ".method public cacheInterstitial(Ljava/lang/String;)V",
        ".method public onDestroy()V",
        ".method public onStart()V",
        ".method public onStop()V",
        ".method public showInterstitial()V",
        ".method public showInterstitial(Ljava/lang/String;)V",
    ):
        txt = replace_method(txt, void_m, "    .locals 0\n\n    return-void")

    for false_m in (
        ".method public hasCachedInterstitial()Z",
        ".method public hasCachedInterstitial(Ljava/lang/String;)Z",
        ".method public onBackPressed()Z",
    ):
        txt = replace_method(txt, false_m, "    .locals 1\n\n    const/4 v0, 0x0\n\n    return v0")

    p.write_text(txt, encoding="utf-8")


def patch_cocos2dx_bitmap_and_renderer(smali_root: pathlib.Path) -> None:
    # 1. Cocos2dxBitmap: clamp width (v3) and height (v2) >= 1 before Bitmap.createBitmap.
    pb = smali_root / "org/cocos2dx/lib/Cocos2dxBitmap.smali"
    txtb = pb.read_text(encoding="utf-8")
    target = "    invoke-static {v3, v2, v6}, Landroid/graphics/Bitmap;->createBitmap(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"
    if target not in txtb:
        raise RuntimeError("Bitmap.createBitmap call not found in Cocos2dxBitmap.smali")
    clamp_code = """    const/4 v6, 0x1

    invoke-static {v6, v3}, Ljava/lang/Math;->max(II)I

    move-result v3

    invoke-static {v6, v2}, Ljava/lang/Math;->max(II)I

    move-result v2

    sget-object v6, Landroid/graphics/Bitmap$Config;->ARGB_8888:Landroid/graphics/Bitmap$Config;

""" + target
    txtb = txtb.replace(
        "    sget-object v6, Landroid/graphics/Bitmap$Config;->ARGB_8888:Landroid/graphics/Bitmap$Config;\n\n" + target,
        clamp_code,
        1,
    )
    pb.write_text(txtb, encoding="utf-8")

    # 2. Cocos2dxRenderer: notify bridge on fresh EGL surface creation and pass real (w, h) in onSurfaceChanged.
    pr = smali_root / "org/cocos2dx/lib/Cocos2dxRenderer.smali"
    txtr = pr.read_text(encoding="utf-8")

    sc_pat = re.compile(
        r"(\.method public onSurfaceCreated\(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;\)V\s*\n\s*\.locals \d+\s*\n)",
        re.MULTILINE,
    )
    if not sc_pat.search(txtr):
        raise RuntimeError("Cocos2dxRenderer.onSurfaceCreated not found")
    txtr = sc_pat.sub(
        r"\1\n    invoke-static {}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->nativeOnSurfaceCreated()V\n",
        txtr,
        count=1,
    )

    sch_pat = re.compile(
        r"(\.method public onSurfaceChanged\(Ljavax/microedition/khronos/opengles/GL10;II\)V\s*\n\s*\.locals \d+\s*\n)",
        re.MULTILINE,
    )
    if not sch_pat.search(txtr):
        raise RuntimeError("Cocos2dxRenderer.onSurfaceChanged not found")
    txtr = sch_pat.sub(
        r"\1\n    invoke-static {p2, p3}, Lorg/cocos2dx/lib/Cocos2dxRenderer;->nativeInit(II)V\n",
        txtr,
        count=1,
    )
    pr.write_text(txtr, encoding="utf-8")


def patch_simple_crypto(smali_root: pathlib.Path) -> None:
    p = smali_root / "com/customRobTop/SimpleCrypto.smali"
    txt = p.read_text(encoding="utf-8")

    txt = replace_method(
        txt,
        ".method public static loadAndDecryptFileToString(Ljava/lang/String;)Ljava/lang/String;",
        """.method public static loadAndDecryptFileToString(Ljava/lang/String;)Ljava/lang/String;
    .locals 1

    invoke-static {p0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->loadAndDecryptSaveFile(Ljava/lang/String;)Ljava/lang/String;

    move-result-object v0

    return-object v0
.end method""",
    )

    txt = replace_method(
        txt,
        ".method public static saveAndEncryptStringToFile(Ljava/lang/String;Ljava/lang/String;)V",
        """.method public static saveAndEncryptStringToFile(Ljava/lang/String;Ljava/lang/String;)V
    .locals 0

    invoke-static {p0, p1}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->saveAndEncryptSaveFile(Ljava/lang/String;Ljava/lang/String;)V

    return-void
.end method""",
    )

    p.write_text(txt, encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <apktool_decoded_dir>", file=sys.stderr)
        sys.exit(2)
    apk_dir = pathlib.Path(sys.argv[1]).resolve()
    smali_root = apk_dir / "smali"
    if not smali_root.is_dir():
        raise RuntimeError(f"Decoded smali directory not found: {smali_root}")

    patch_apktool_yml(apk_dir)
    patch_manifest(apk_dir)
    patch_geometry_jump(smali_root)
    patch_base_robtop(smali_root)
    patch_chartboost_manager(smali_root)
    patch_cocos2dx_bitmap_and_renderer(smali_root)
    patch_simple_crypto(smali_root)
    print(f"Successfully patched manifest, apktool.yml, and smali in {apk_dir}")


if __name__ == "__main__":
    main()
