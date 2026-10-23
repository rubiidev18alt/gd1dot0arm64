#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Deterministic Android 16 (API 36) + ARM64 manifest, apktool.yml, and smali patcher
for Geometry Dash 1.02 (build 4, com.robtopx.geometryjump).
"""

import argparse
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


def patch_apktool_yml(apk_dir: pathlib.Path, local_gdps: bool = False) -> None:
    yml_path = apk_dir / "apktool.yml"
    txt = yml_path.read_text(encoding="utf-8")
    txt = re.sub(r"minSdkVersion:\s*'?\d+'?", "minSdkVersion: '24'", txt)
    txt = re.sub(r"targetSdkVersion:\s*'?\d+'?", "targetSdkVersion: '36'", txt)

    entries = ["arm32/libguestrt.so", "arm32/libgame.so", "mp3", "ogg"]
    if local_gdps:
        entries.append("gdps/levels_1_0.bin")
    for entry in entries:
        if entry not in txt:
            txt = txt.replace("doNotCompress:\n", f"doNotCompress:\n- {entry}\n", 1)

    yml_path.write_text(txt, encoding="utf-8")


def qualify_class(cls_name: str, base_pkg: str) -> str:
    if not cls_name:
        return cls_name
    if cls_name.startswith("."):
        return f"{base_pkg}{cls_name}"
    if "." not in cls_name:
        return f"{base_pkg}.{cls_name}"
    return cls_name


def rewrite_pkg_prefix(val: str, old_pkg: str, new_pkg: str) -> str:
    if val == old_pkg:
        return new_pkg
    if val.startswith(old_pkg + "."):
        return new_pkg + val[len(old_pkg) :]
    return val


def patch_strings_and_res_namespaces(
    apk_dir: pathlib.Path, orig_pkg: str, new_pkg: str, app_label: str
) -> None:
    res_dir = apk_dir / "res"
    if not res_dir.is_dir():
        return

    # 1. Set app_name across res/values*/strings.xml so launcher, installer, and Settings
    #    display the requested application label (e.g. "gd 1.02 arm64").
    values_default = res_dir / "values" / "strings.xml"
    string_files = sorted(res_dir.glob("values*/strings.xml"))
    if values_default not in string_files and values_default.parent.is_dir():
        values_default.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n</resources>\n',
            encoding="utf-8",
        )
        string_files.append(values_default)

    for s_path in string_files:
        tree = ET.parse(s_path)
        root = tree.getroot()
        found = False
        for elem in root.findall("string"):
            if elem.get("name") == "app_name":
                elem.text = app_label
                found = True
        if not found and s_path == values_default:
            el = ET.SubElement(root, "string")
            el.set("name", "app_name")
            el.text = app_label
        tree.write(s_path, encoding="utf-8", xml_declaration=True)

    # 2. Rewrite any legacy custom XML schema URI referencing orig_pkg so aapt2 links cleanly.
    if new_pkg != orig_pkg:
        old_uri = f"http://schemas.android.com/apk/res/{orig_pkg}"
        new_uri = f"http://schemas.android.com/apk/res/{new_pkg}"
        for xml_file in res_dir.rglob("*.xml"):
            raw = xml_file.read_text(encoding="utf-8", errors="replace")
            if old_uri in raw:
                xml_file.write_text(raw.replace(old_uri, new_uri), encoding="utf-8")


def patch_manifest(apk_dir: pathlib.Path, new_pkg: str, app_label: str) -> None:
    manifest_path = apk_dir / "AndroidManifest.xml"
    tree = ET.parse(manifest_path)
    root = tree.getroot()
    orig_pkg = root.get("package", "com.robtopx.geometryjump")

    patch_strings_and_res_namespaces(apk_dir, orig_pkg, new_pkg, app_label)

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

    # Expand relative component class names against orig_pkg BEFORE changing <manifest package="...">,
    # and rewrite any custom permissions/authorities so this APK co-installs cleanly alongside
    # regular Geometry Dash (com.robtopx.geometryjump) with zero PackageManager conflicts.
    if new_pkg != orig_pkg:
        app_cls = app.get(attr_key("name"))
        if app_cls:
            app.set(attr_key("name"), qualify_class(app_cls, orig_pkg))

        for comp_tag in ("activity", "activity-alias", "service", "receiver", "provider"):
            for comp in app.findall(comp_tag):
                cname = comp.get(attr_key("name"))
                if cname:
                    comp.set(attr_key("name"), qualify_class(cname, orig_pkg))
                if comp_tag == "activity-alias":
                    targ = comp.get(attr_key("targetActivity"))
                    if targ:
                        comp.set(attr_key("targetActivity"), qualify_class(targ, orig_pkg))
                if comp_tag == "provider":
                    auths = comp.get(attr_key("authorities"))
                    if auths:
                        comp.set(
                            attr_key("authorities"),
                            ";".join(
                                rewrite_pkg_prefix(a, orig_pkg, new_pkg) for a in auths.split(";")
                            ),
                        )

        for elem in root.iter():
            if elem.tag in (
                "permission",
                "uses-permission",
                "uses-permission-sdk-23",
                "action",
                "category",
            ):
                ename = elem.get(attr_key("name"))
                if ename:
                    elem.set(attr_key("name"), rewrite_pkg_prefix(ename, orig_pkg, new_pkg))
            for attr_n in ("permission", "taskAffinity"):
                aval = elem.get(attr_key(attr_n))
                if aval:
                    elem.set(attr_key(attr_n), rewrite_pkg_prefix(aval, orig_pkg, new_pkg))

        root.set("package", new_pkg)

    # Set application label to @string/app_name (updated above in strings.xml).
    app.set(attr_key("label"), "@string/app_name")

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

    # Patch main launcher activity (.GeometryJump / com.robtopx.geometryjump.GeometryJump)
    # and ensure every Android 12+ (API 31-36) component with an <intent-filter> declares android:exported.
    for comp_tag in ("activity", "activity-alias", "service", "receiver"):
        for comp in app.findall(comp_tag):
            if comp.find("intent-filter") is not None and comp.get(attr_key("exported")) is None:
                comp.set(attr_key("exported"), "false")

    patched_main = False
    for act in app.findall("activity"):
        name = act.get(attr_key("name"), "")
        if name in (".GeometryJump", "com.robtopx.geometryjump.GeometryJump", f"{orig_pkg}.GeometryJump"):
            act.set(attr_key("label"), "@string/app_name")
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
        """    .locals 1

    const-string v0, "https://play.google.com/store/apps/details?id=com.robtopx.geometryjump"

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

    # 3. Count real rendered frames for the FPS overlay: hook onDrawFrame right after nativeRender().
    #    `ModernPlatformHelper.onFrameRendered()` only bumps a counter (and raises the GL thread to
    #    display priority once), so this stays a no-op for frame pacing.
    if "onFrameRendered" in txtr:
        print("  - Cocos2dxRenderer.onDrawFrame -> onFrameRendered (already present)")
    else:
        odf_pat = re.compile(
            r"(\.method public onDrawFrame\(Ljavax/microedition/khronos/opengles/GL10;\)V\b.*?)"
            r"(\n\.end method)",
            re.DOTALL,
        )
        odf = odf_pat.search(txtr)
        if not odf:
            raise RuntimeError("Cocos2dxRenderer.onDrawFrame not found")
        body = odf.group(1)
        hook = (
            "    invoke-static {}, Lorg/cocos2dx/lib/Cocos2dxRenderer;->nativeRender()V\n\n"
            "    invoke-static {}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->onFrameRendered()V"
        )
        if "invoke-static {}, Lorg/cocos2dx/lib/Cocos2dxRenderer;->nativeRender()V" not in body:
            raise RuntimeError("Cocos2dxRenderer.onDrawFrame nativeRender call not found")
        body = body.replace(
            "    invoke-static {}, Lorg/cocos2dx/lib/Cocos2dxRenderer;->nativeRender()V", hook, 1
        )
        txtr = txtr[: odf.start(1)] + body + txtr[odf.end(1):]
        print("  - Cocos2dxRenderer.onDrawFrame -> onFrameRendered (FPS counter)")

    pr.write_text(txtr, encoding="utf-8")


def patch_cocos2dx_text_layout(smali_root: pathlib.Path) -> None:
    """Replace the 2013 text layout with `ModernTextLayout`.

    The original `Cocos2dxBitmap.createTextBitmap` was written for 480x320 phones: it sizes
    the label bitmap from integer measurements and never checks the GPU's maximum texture
    size, so large labels (tablets / high-density screens) exceed `GL_MAX_TEXTURE_SIZE` and
    the text silently disappears. `ModernTextLayout` keeps the identical Java signature (the
    native side still calls this exact method and still receives pixels through
    `nativeInitBitmapDC`) but wraps with float metrics, honours explicit newlines and line
    limits, and scales the font down to the real texture limit instead of failing.
    """
    pb = smali_root / "org/cocos2dx/lib/Cocos2dxBitmap.smali"
    txt = pb.read_text()

    header = ".method public static createTextBitmap(Ljava/lang/String;Ljava/lang/String;IIII)V"
    if header not in txt:
        raise RuntimeError("Cocos2dxBitmap.createTextBitmap(String,String,IIII)V not found")

    # 6 register arguments do not fit in an invoke-static 35c list, so use /range.
    replacement = (
        header
        + "\n    .locals 0\n\n"
        + "    invoke-static/range {p0 .. p5}, Lcom/robtopx/geometryjump/ModernTextLayout;->"
        + "createTextBitmap(Ljava/lang/String;Ljava/lang/String;IIII)V\n\n"
        + "    return-void\n.end method"
    )
    pattern = re.compile(re.escape(header) + r".*?\.end method", re.DOTALL)
    txt, count = pattern.subn(lambda _m: replacement, txt, count=1)
    if count != 1:
        raise RuntimeError("failed to replace Cocos2dxBitmap.createTextBitmap body")

    # ModernTextLayout hands the finished bitmap to cocos2d-x through this helper, which is
    # private in the 2013 build. Reflection also works, but making it public is exact.
    txt = txt.replace(
        ".method private static initNativeObject(Landroid/graphics/Bitmap;)V",
        ".method public static initNativeObject(Landroid/graphics/Bitmap;)V",
        1,
    )
    pb.write_text(txt)
    print("  - Cocos2dxBitmap.createTextBitmap -> ModernTextLayout (texture-size aware)")


def patch_cocos2dx_music(smali_root: pathlib.Path) -> None:
    """
    Replace obfuscated Cocos2dxMusic (`org/cocos2dx/lib/p.smali`) methods with
    `ModernMusicController` so pause/unpause (`playBackgroundMusic` ->
    `setBackgroundMusicTime` -> `resumeBackgroundMusic`) never triggers modern
    Android NuPlayerDriver's `stop() + prepare()` seek-to-0 reset bug.
    """
    p = smali_root / "org/cocos2dx/lib/p.smali"
    txt = p.read_text(encoding="utf-8")

    txt = replace_method(
        txt,
        ".method public constructor <init>(Landroid/content/Context;)V",
        """    .locals 0

    invoke-direct {p0}, Ljava/lang/Object;-><init>()V

    iput-object p1, p0, Lorg/cocos2dx/lib/p;->mContext:Landroid/content/Context;

    invoke-static {p1}, Lcom/robtopx/geometryjump/ModernMusicController;->onHolderConstructed(Landroid/content/Context;)V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public preloadBackgroundMusic(Ljava/lang/String;)V",
        """    .locals 1

    iget-object v0, p0, Lorg/cocos2dx/lib/p;->mContext:Landroid/content/Context;

    invoke-static {v0, p1}, Lcom/robtopx/geometryjump/ModernMusicController;->preload(Landroid/content/Context;Ljava/lang/String;)V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public playBackgroundMusic(Ljava/lang/String;Z)V",
        """    .locals 1

    iget-object v0, p0, Lorg/cocos2dx/lib/p;->mContext:Landroid/content/Context;

    invoke-static {v0, p1, p2}, Lcom/robtopx/geometryjump/ModernMusicController;->play(Landroid/content/Context;Ljava/lang/String;Z)V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public stopBackgroundMusic()V",
        """    .locals 0

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->stop()V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public pauseBackgroundMusic()V",
        """    .locals 0

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->pause()V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public resumeBackgroundMusic()V",
        """    .locals 0

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->resume()V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public rewindBackgroundMusic()V",
        """    .locals 0

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->rewind()V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public setBackgroundMusicTime(F)V",
        """    .locals 0

    invoke-static {p1}, Lcom/robtopx/geometryjump/ModernMusicController;->setTime(F)V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public isBackgroundMusicPlaying()Z",
        """    .locals 1

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->isPlaying()Z

    move-result v0

    return v0""",
    )

    txt = replace_method(
        txt,
        ".method public getBackgroundVolume()F",
        """    .locals 1

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->getVolume()F

    move-result v0

    return v0""",
    )

    txt = replace_method(
        txt,
        ".method public setBackgroundVolume(F)V",
        """    .locals 0

    invoke-static {p1}, Lcom/robtopx/geometryjump/ModernMusicController;->setVolume(F)V

    return-void""",
    )

    txt = replace_method(
        txt,
        ".method public end()V",
        """    .locals 0

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->end()V

    return-void""",
    )

    if ".method public getBackgroundMusicTime()F" in txt:
        txt = replace_method(
            txt,
            ".method public getBackgroundMusicTime()F",
            """    .locals 1

    invoke-static {}, Lcom/robtopx/geometryjump/ModernMusicController;->getTime()F

    move-result v0

    return v0""",
        )

    p.write_text(txt, encoding="utf-8")


def patch_simple_crypto(smali_root: pathlib.Path) -> None:
    p = smali_root / "com/customRobTop/SimpleCrypto.smali"
    txt = p.read_text(encoding="utf-8")

    txt = replace_method(
        txt,
        ".method public static loadAndDecryptFileToString(Ljava/lang/String;)Ljava/lang/String;",
        """    .locals 1

    invoke-static {p0}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->loadAndDecryptSaveFile(Ljava/lang/String;)Ljava/lang/String;

    move-result-object v0

    return-object v0""",
    )

    txt = replace_method(
        txt,
        ".method public static saveAndEncryptStringToFile(Ljava/lang/String;Ljava/lang/String;)V",
        """    .locals 0

    invoke-static {p0, p1}, Lcom/robtopx/geometryjump/ModernPlatformHelper;->saveAndEncryptSaveFile(Ljava/lang/String;Ljava/lang/String;)V

    return-void""",
    )

    p.write_text(txt, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Patch Geometry Dash 1.02 manifest, resources, and smali for Android 16 ARM64."
    )
    parser.add_argument("apk_dir", type=pathlib.Path, help="Apktool decoded directory")
    parser.add_argument(
        "--package",
        default="com.robtopx.geometryj102",
        help="Target Android applicationId / manifest package name (default: com.robtopx.geometryj102)",
    )
    parser.add_argument(
        "--app-name",
        default="gd 1.02 arm64",
        help="Target Android application label (default: 'gd 1.02 arm64')",
    )
    parser.add_argument(
        "--gd-version",
        default="1.02",
        help="Geometry Dash build being patched (selects version-specific smali targets; "
        "default: 1.02)",
    )
    parser.add_argument(
        "--local-gdps",
        default="0",
        choices=("0", "1"),
        help="Embed the private local 1.0 GDPS archive (assets/gdps/levels_1_0.bin) and route all "
        "boomlings.com traffic to the in-process server (default: 0)",
    )
    args = parser.parse_args()
    local_gdps = args.local_gdps == "1"

    apk_dir = args.apk_dir.resolve()
    smali_root = apk_dir / "smali"
    if not smali_root.is_dir():
        raise RuntimeError(f"Decoded smali directory not found: {smali_root}")

    patch_apktool_yml(apk_dir, local_gdps=local_gdps)
    patch_manifest(apk_dir, args.package, args.app_name)

    # Essential for every supported build: without these the port does not run at all.
    patch_cocos2dx_bitmap_and_renderer(smali_root)
    patch_cocos2dx_text_layout(smali_root)

    # Platform / legacy-SDK fixes. The 1.1 build is five weeks newer than 1.02 and some
    # of these targets may already be fixed or renamed there, so for non-1.02 inputs a
    # missing target is a warning instead of a hard failure.
    optional = [
        ("GeometryJump", patch_geometry_jump),
        ("BaseRobTopActivity", patch_base_robtop),
        ("Chartboost", patch_chartboost_manager),
        ("Cocos2dxMusic", patch_cocos2dx_music),
        ("SimpleCrypto", patch_simple_crypto),
    ]
    skipped: list[str] = []
    for name, fn in optional:
        try:
            fn(smali_root)
        except Exception as exc:  # noqa: BLE001
            if args.gd_version == "1.02":
                raise
            skipped.append(f"{name}: {exc}")
            print(f"  ! {name} patch not applicable to {args.gd_version}: {exc}")
    if skipped:
        print(f"  ({len(skipped)} optional patch(es) skipped for GD {args.gd_version})")
    print(
        f"Successfully patched manifest (package={args.package}, label='{args.app_name}', "
        f"local_gdps={int(local_gdps)}), apktool.yml, and smali in {apk_dir}"
    )


if __name__ == "__main__":
    main()
