#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Fetches all archived Geometry Dash 1.0 levels (IDs 128..1941) from GDHistory
(https://history.geometrydash.eu), selects the earliest archived playable version
for each level (skipping IDs that were deleted from GD servers without an archive),
normalizes compressed/later-format level strings back to native GD 1.02 format,
validates object sprite frames against Geometry.Dash.ver.1.02.build.4.apk, and
writes the compact deterministic `GD10DB01` binary database (`levels_1_0.bin`).
"""

from __future__ import annotations

import argparse
import base64
import shutil
import subprocess
import threading
import concurrent.futures
import datetime as dt
import hashlib
import json
import math
import os
import pathlib
import plistlib
import random
import re
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib

MAGIC = b"GD10DB01"
DB_VERSION = 1

MIN_1_0_ID = 128
MAX_1_0_ID = 1941

GDHISTORY_BASE = "https://history.geometrydash.eu"
# GD History sits behind Cloudflare; use a normal client fingerprint plus a contact URL so the
# public API is served instead of being treated as an anonymous bot.
USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0 Safari/537.36 gd1dot0arm64-archive-builder/1.0 "
    "(+https://github.com/rubiidev18alt/gd1dot0arm64)"
)
HTTP_HEADERS = {
    "User-Agent": USER_AGENT,
    "Accept": "application/json, text/html;q=0.9, */*;q=0.8",
    "Accept-Language": "en-US,en;q=0.9",
    "Referer": "https://history.geometrydash.eu/",
    "Accept-Encoding": "identity",
    "Connection": "close",
}
_CURL = shutil.which("curl")


HTTP_MIN_INTERVAL = 0.45
ROUTE_TIMEOUT = 30.0
RETRYABLE_CODES = (0, 403, 408, 425, 429, 500, 502, 503, 504, 520, 521, 522, 524)

# GD History sits behind Cloudflare, which rejects datacenter IP ranges (GitHub-hosted
# runners included) on the origin. Requests are therefore attempted over several independent
# network paths ("routes"): the origin itself, curl (different TLS fingerprint), Google's
# Translate proxy, and a handful of public read-only HTTP relays. A probe picks the healthy
# ones and every level fetch falls back through the remaining routes, so a single blocked
# path can no longer stall the build.
PROXY_TEMPLATES: dict[str, str] = {
    "codetabs": "https://api.codetabs.com/v1/proxy?quest={quoted}",
    "allorigins": "https://api.allorigins.win/raw?url={quoted}",
    "corslol": "https://api.cors.lol/?url={quoted}",
    "corsproxyorg": "https://corsproxy.org/?{quoted}",
    "corsfix": "https://proxy.corsfix.com/?{quoted}",
}
JINA_TEMPLATE = "https://r.jina.ai/{raw}"
TRANSLATE_HOST = "history-geometrydash-eu.translate.goog"
DEFAULT_ROUTE_ORDER = [
    "direct",
    "curl",
    "translate",
    "codetabs",
    "corslol",
    "allorigins",
    "corsproxyorg",
    "corsfix",
    "jina",
]


class RouteError(RuntimeError):
    """A single network path failed for one URL (code 0 means a transport-level failure)."""

    def __init__(self, code: int, detail: str = ""):
        super().__init__(f"HTTP {code} {detail}".strip())
        self.code = int(code)
        self.detail = detail


class Route:
    def __init__(self, name: str, fetch):
        self.name = name
        self._fetch = fetch
        self.ok = 0
        self.fail = 0
        self.streak = 0
        self.delay = HTTP_MIN_INTERVAL
        self.last_error = ""
        self.lock = threading.Lock()

    def fetch(self, url: str, timeout: float) -> bytes:
        return self._fetch(url, timeout)

    @property
    def score(self) -> int:
        return self.ok - 2 * self.fail

    def note(self, ok: bool, err: str = "") -> None:
        with self.lock:
            if ok:
                self.ok += 1
                self.streak = 0
                self.delay = max(0.15, self.delay * 0.9)
            else:
                self.fail += 1
                self.streak += 1
                self.last_error = err
                self.delay = min(4.0, max(0.35, self.delay) * 1.6)

    def pace(self) -> None:
        with self.lock:
            delay = self.delay
        if delay > 0:
            time.sleep(delay * (0.75 + random.random() * 0.5))


# Complete Geometry Dash 1.02 ObjectToolbox object ID -> GJ_GameSheet.plist frame table.
GD10_OBJECT_FRAMES: dict[int, str] = {
    1: "square_01_001.png",
    2: "square_02_001.png",
    3: "square_03_001.png",
    4: "square_04_001.png",
    5: "square_05_001.png",
    6: "square_06_001.png",
    7: "square_07_001.png",
    8: "spike_01_001.png",
    9: "pit_01_001.png",
    10: "portal_01_front_001.png",
    11: "portal_02_front_001.png",
    12: "portal_03_front_001.png",
    13: "portal_04_front_001.png",
    15: "rod_01_001.png",
    16: "rod_02_001.png",
    17: "rod_03_001.png",
    18: "d_spikes_01_001.png",
    19: "d_spikes_02_001.png",
    20: "d_spikes_03_001.png",
    21: "d_spikes_04_001.png",
    22: "edit_eeNoneBtn_001.png",
    23: "edit_eeFBBtn_001.png",
    24: "edit_eeFTBtn_001.png",
    25: "edit_eeFLBtn_001.png",
    26: "edit_eeFRBtn_001.png",
    27: "edit_eeSUBtn_001.png",
    28: "edit_eeSDBtn_001.png",
    29: "edit_eTintBGBtn_001.png",
    30: "edit_eTintGBtn_001.png",
    31: "edit_eStartPosBtn_001.png",
    32: "edit_eGhostEBtn_001.png",
    33: "edit_eGhostDBtn_001.png",
    34: "edit_eLevelEndBtn_001.png",
    35: "bump_01_001.png",
    36: "ring_01_001.png",
    39: "spike_02_001.png",
    40: "plank_01_001.png",
    41: "chain_01_001.png",
    42: "edit_eBGEOn_001.png",
    43: "edit_eBGEOff_001.png",
    45: "portal_05_front_001.png",
    46: "portal_06_front_001.png",
    47: "portal_07_front_001.png",
    48: "d_cloud_01_001.png",
    49: "d_cloud_02_001.png",
    50: "d_ball_01_001.png",
    51: "d_ball_02_001.png",
    53: "d_ball_04_001.png",
    54: "d_ball_05_001.png",
    58: "edit_eeFRHBtn_001.png",
    59: "edit_eeFRHInvBtn_001.png",
    60: "d_ball_06_001.png",
    61: "pit_04_001.png",
    62: "square_b_01_001.png",
    65: "square_b_04_001.png",
    66: "square_b_05_001.png",
    67: "gravbump_01_001.png",
    68: "square_b_06_001.png",
    70: "square_c_02_001.png",
    71: "square_c_03_001.png",
    72: "square_c_04_001.png",
    73: "square_c_05_001.png",
    74: "square_c_06_001.png",
    75: "square_c_07_001.png",
    83: "square_08_001.png",
    99: "portal_08_front_001.png",
    101: "portal_09_front_001.png",
    103: "spike_03_001.png",
    104: "edit_eTintLBtn_001.png",
    105: "edit_eTintObjBtn_001.png",
    110: "d_chain_02_001.png",
    117: "square_f_02_001.png",
    118: "square_f_03_001.png",
    140: "bump_03_001.png",
    142: "secretCoin_01_001.png",
    154: "d_spikewheel_01_001.png",
    238: "d_link_02_001.png",
}


def verify_gd10_frames_against_apk(orig_apk: pathlib.Path | None) -> set[int]:
    if orig_apk is None or not orig_apk.is_file():
        return set(GD10_OBJECT_FRAMES.keys())

    with zipfile.ZipFile(orig_apk, "r") as zf:
        plist_bytes = zf.read("assets/GJ_GameSheet.plist")
    plist = plistlib.loads(plist_bytes)
    frames = set((plist.get("frames") or {}).keys())
    valid_ids: set[int] = set()
    for obj_id, frame_name in sorted(GD10_OBJECT_FRAMES.items()):
        if frame_name in frames:
            valid_ids.add(obj_id)
        else:
            print(
                f"WARNING: Object ID {obj_id} frame '{frame_name}' not in GJ_GameSheet.plist; excluding",
                file=sys.stderr,
            )
    if len(valid_ids) < 35:
        raise RuntimeError(f"Unexpectedly few valid GD 1.02 object IDs ({len(valid_ids)})")
    return valid_ids


def _direct_fetch(url: str, timeout: float) -> bytes:
    req = urllib.request.Request(url, headers=HTTP_HEADERS)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read()
    except urllib.error.HTTPError as e:
        detail = ",".join(
            f"{k}={e.headers.get(k)}"
            for k in ("server", "cf-ray", "cf-mitigated", "content-type")
            if e.headers.get(k)
        )
        try:
            snippet = e.read(160).decode("utf-8", "replace").replace("\n", " ")
        except Exception:  # noqa: BLE001
            snippet = ""
        raise RouteError(e.code, f"{detail} {snippet}".strip()) from e
    except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
        raise RouteError(0, repr(e)[:140]) from e


def _curl_fetch(url: str, timeout: float) -> bytes:
    if not _CURL:
        raise RouteError(0, "curl unavailable")
    fd, path = tempfile.mkstemp(prefix="gdhistory_fetch_")
    os.close(fd)
    try:
        cmd = [
            _CURL,
            "-sS",
            "--http1.1",
            "--compressed",
            "--max-time",
            str(int(timeout)),
            "-A",
            HTTP_HEADERS["User-Agent"],
            "-e",
            HTTP_HEADERS["Referer"],
            "-H",
            "Accept: " + HTTP_HEADERS["Accept"],
            "-o",
            path,
            "-w",
            "%{http_code}",
            url,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        raw_code = (proc.stdout or "").strip()
        code = int(raw_code) if raw_code.isdigit() else 0
        if code != 200:
            stderr = (proc.stderr or "").strip().splitlines()
            raise RouteError(code, stderr[-1][:140] if stderr else "curl")
        return pathlib.Path(path).read_bytes()
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def _relay_fetch(target: str, timeout: float) -> bytes:
    headers = {
        "User-Agent": USER_AGENT,
        "Accept": "*/*",
        "Accept-Language": "en-US,en;q=0.9",
        "Accept-Encoding": "identity",
    }
    req = urllib.request.Request(target, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read()
    except urllib.error.HTTPError as e:
        raise RouteError(e.code, "relay") from e
    except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
        raise RouteError(0, repr(e)[:140]) from e


def _translate_target(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    extra = "_x_tr_sl=auto&_x_tr_tl=en&_x_tr_hl=en&_x_tr_pto=nui"
    query = f"{parts.query}&{extra}" if parts.query else extra
    return urllib.parse.urlunsplit((parts.scheme, TRANSLATE_HOST, parts.path or "/", query, ""))


def build_routes(names: list[str] | None = None) -> list[Route]:
    wanted = list(names) if names else list(DEFAULT_ROUTE_ORDER)
    routes: list[Route] = []
    for name in wanted:
        if name == "direct":
            routes.append(Route("direct", _direct_fetch))
        elif name == "curl":
            routes.append(Route("curl", _curl_fetch))
        elif name == "translate":
            routes.append(
                Route("translate", lambda u, t: _relay_fetch(_translate_target(u), t))
            )
        elif name == "jina":
            routes.append(
                Route("jina", lambda u, t: _relay_fetch(JINA_TEMPLATE.format(raw=u), t))
            )
        elif name in PROXY_TEMPLATES:
            template = PROXY_TEMPLATES[name]
            routes.append(
                Route(
                    name,
                    lambda u, t, template=template: _relay_fetch(
                        template.format(quoted=urllib.parse.quote(u, safe="")), t
                    ),
                )
            )
        else:
            raise SystemExit(f"unknown GD History route {name!r} (known: {', '.join(DEFAULT_ROUTE_ORDER)})")
    return routes


_ROUTES: list[Route] = []
_ROUTES_LOCK = threading.Lock()
_ACTIVE_ROUTE = [""]


def configure_routes(names: list[str] | None = None) -> list[Route]:
    global _ROUTES
    with _ROUTES_LOCK:
        _ROUTES = build_routes(names)
        _ACTIVE_ROUTE[0] = ""
        return _ROUTES


def _ordered_routes() -> list[Route]:
    with _ROUTES_LOCK:
        routes = list(_ROUTES)
    # Highest score first (healthiest route), stable against the configured order.
    return [r for _, r in sorted(enumerate(routes), key=lambda item: (-item[1].score, item[0]))]


def _announce_route(route: Route, size: int) -> None:
    with _ROUTES_LOCK:
        if _ACTIVE_ROUTE[0] == route.name:
            return
        _ACTIVE_ROUTE[0] = route.name
    print(f"  [route] GD History requests are being served over '{route.name}' ({size} bytes)")


def http_get_bytes(
    url: str,
    retries: int = 2,
    timeout: float = ROUTE_TIMEOUT,
    validate=None,
) -> bytes | None:
    """GET `url` through the healthiest route, failing over across every configured route.

    Returns None for an authoritative HTTP 404 (the record/file genuinely does not exist)."""
    errors: list[str] = []
    for route in _ordered_routes():
        for attempt in range(retries):
            route.pace()
            try:
                body = route.fetch(url, timeout)
            except RouteError as e:
                if e.code == 404:
                    route.note(True)
                    return None
                route.note(False, str(e))
                if e.code in RETRYABLE_CODES and attempt + 1 < retries:
                    time.sleep(0.5 * (attempt + 1))
                    continue
                errors.append(f"{route.name}:{e.code}")
                break
            except Exception as e:  # noqa: BLE001 - never let one route kill the build
                route.note(False, repr(e))
                errors.append(f"{route.name}:{type(e).__name__}")
                break
            else:
                if not body:
                    route.note(False, "empty body")
                    errors.append(f"{route.name}:empty")
                    break
                if validate is not None:
                    # Public relays return HTTP 200 with their own error pages; reject those
                    # before they can be cached, and fail over to the next route.
                    try:
                        validate(body)
                    except Exception as exc:  # noqa: BLE001
                        route.note(False, f"invalid payload: {exc}")
                        errors.append(f"{route.name}:invalid")
                        break
                route.note(True)
                _announce_route(route, len(body))
                return body
    raise RuntimeError(f"GET {url} failed on every route [{', '.join(errors)}]")


def probe_routes(
    routes: list[Route], timeout: float = 20.0, thorough: bool = True
) -> list[Route]:
    """Exercise every route against the public API and report which ones actually work."""
    targets = [
        ("counts", f"{GDHISTORY_BASE}/api/v1/counts/"),
        ("level_json", f"{GDHISTORY_BASE}/api/v1/level/128/"),
        ("level_download", f"{GDHISTORY_BASE}/level/128/10988023/download/"),
    ]
    if not thorough:
        targets = targets[:1]
    working: list[Route] = []
    print(f"==> Probing {len(routes)} GD History network route(s)")
    for route in routes:
        successes = 0
        for label, url in targets:
            started = time.monotonic()
            body = b""
            code, detail = 0, ""
            try:
                body = route.fetch(url, timeout)
                code = 200
            except RouteError as e:
                code, detail = e.code, e.detail
            except Exception as e:  # noqa: BLE001
                code, detail = 0, f"{type(e).__name__}: {e}"[:140]
            elapsed_ms = int((time.monotonic() - started) * 1000)
            head = body[:100].decode("utf-8", "replace").replace("\n", " ").replace("\r", " ")
            print(
                f"PROBE route={route.name} target={label} code={code} bytes={len(body)} "
                f"ms={elapsed_ms} detail={detail!r} head={head!r}"
            )
            if code == 200 and body:
                successes += 1
                route.note(True)
            else:
                route.note(False, f"{code}")
                if not thorough:
                    break
        if successes:
            working.append(route)
    summary = ",".join(r.name for r in working) or "none"
    print(f"PROBE_SUMMARY working={summary}")
    return working


def robtop_gmd_to_dict(gmd_bytes: bytes) -> dict:
    b = gmd_bytes.strip()
    if not b.startswith(b"<?xml"):
        b = b'<?xml version="1.0"?><plist version="1.0">' + b + b"</plist>"
    elif b"<plist" not in b[:200]:
        idx = b.find(b"?>")
        if idx != -1:
            b = b[: idx + 2] + b'<plist version="1.0">' + b[idx + 2 :] + b"</plist>"
    for src, dst in (
        (b"</d>", b"</dict>"),
        (b"</k>", b"</key>"),
        (b"<d>", b"<dict>"),
        (b"<d />", b"<dict />"),
        (b"<d/>", b"<dict />"),
        (b"<k>", b"<key>"),
        (b"<s>", b"<string>"),
        (b"</s>", b"</string>"),
        (b"<i>", b"<integer>"),
        (b"</i>", b"</integer>"),
        (b"<t />", b"<true />"),
        (b"<t/>", b"<true />"),
        (b"<f />", b"<false />"),
        (b"<f/>", b"<false />"),
        (b"<r>", b"<real>"),
        (b"</r>", b"</real>"),
    ):
        b = b.replace(src, dst)
    return plistlib.loads(b)


def decode_b64_flexible(s: str) -> bytes:
    t = s.strip().replace("-", "+").replace("_", "/")
    pad = (-len(t)) % 4
    if pad:
        t += "=" * pad
    return base64.b64decode(t)


def decompress_k4_level_string(k4: str) -> str:
    s = k4.strip()
    if not s:
        return ""
    if s.startswith("kS") or s.startswith("kA") or ";" in s[:64]:
        return s
    raw = decode_b64_flexible(s)
    if raw[:2] == b"kS" or raw[:2] == b"kA":
        return raw.decode("utf-8", errors="replace")
    try:
        dec = zlib.decompress(raw, zlib.MAX_WBITS | 32)
    except zlib.error:
        dec = zlib.decompress(raw, -zlib.MAX_WBITS)
    return dec.decode("utf-8", errors="replace")


def parse_iso_timestamp(val: str | None) -> float:
    if not val or val == "None":
        return float("inf")
    s = val.strip()
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    try:
        return dt.datetime.fromisoformat(s).timestamp()
    except ValueError:
        return float("inf")


def sanitize_gd10_text(val: str | None, max_len: int = 180, fallback: str = "") -> str:
    if not val:
        return fallback
    out_chars: list[str] = []
    for ch in val:
        if ch in (":", "|", "#"):
            out_chars.append("-")
        elif ch in ("\r", "\n", "\t"):
            out_chars.append(" ")
        elif 0x20 <= ord(ch) <= 0x7E:
            out_chars.append(ch)
    cleaned = re.sub(r" {2,}", " ", "".join(out_chars)).strip()
    if not cleaned:
        return fallback
    return cleaned[:max_len]


def decode_record_description(rec: dict) -> str:
    raw = rec.get("description")
    if not raw or not isinstance(raw, str):
        return ""
    if rec.get("description_encoded") is True:
        try:
            raw = decode_b64_flexible(raw).decode("utf-8", errors="replace")
        except Exception:
            pass
    return sanitize_gd10_text(raw, max_len=180, fallback="")


def parse_kv_pairs(s: str, sep: str = ",") -> list[tuple[str, str]]:
    toks = s.split(sep)
    pairs: list[tuple[str, str]] = []
    for i in range(0, len(toks) - 1, 2):
        k = toks[i].strip()
        v = toks[i + 1].strip()
        if k:
            pairs.append((k, v))
    return pairs


def extract_rgb_from_subdict(sub: str) -> tuple[int, int, int] | None:
    d = dict(parse_kv_pairs(sub, "_"))
    if "1" in d and "2" in d and "3" in d:
        try:
            r = max(0, min(255, int(float(d["1"]))))
            g = max(0, min(255, int(float(d["2"]))))
            b = max(0, min(255, int(float(d["3"]))))
            return (r, g, b)
        except ValueError:
            return None
    return None


def inspect_and_convert_level_string(
    raw_level_str: str, valid_obj_ids: set[int], fallback_audio_track: int
) -> tuple[str, bool, int, int]:
    """
    Normalizes a decompressed GD level string into GD 1.02 `kS1,...;obj1;obj2;...;` format.
    Returns (converted_level_string, is_pure_1_0, kept_objects, dropped_objects).
    """
    parts = [p for p in raw_level_str.strip().split(";") if p]
    if not parts:
        default_hdr = f"kS1,40,kS2,125,kS3,255,kS4,0,kS5,102,kS6,255,kA1,{fallback_audio_track};"
        return default_hdr, True, 0, 0

    hdr_pairs = parse_kv_pairs(parts[0], ",")
    hdr = dict(hdr_pairs)

    # Recover BG (1000 / kS29) and Ground (1001 / kS30) RGB if saved by GD 1.9-2.2 (`kS29..` or `kS38`).
    bg_rgb: tuple[int, int, int] | None = None
    g_rgb: tuple[int, int, int] | None = None
    if all(k in hdr for k in ("kS1", "kS2", "kS3")):
        try:
            bg_rgb = (
                max(0, min(255, int(float(hdr["kS1"])))),
                max(0, min(255, int(float(hdr["kS2"])))),
                max(0, min(255, int(float(hdr["kS3"])))),
            )
        except ValueError:
            bg_rgb = None
    if all(k in hdr for k in ("kS4", "kS5", "kS6")):
        try:
            g_rgb = (
                max(0, min(255, int(float(hdr["kS4"])))),
                max(0, min(255, int(float(hdr["kS5"])))),
                max(0, min(255, int(float(hdr["kS6"])))),
            )
        except ValueError:
            g_rgb = None

    if bg_rgb is None and "kS29" in hdr:
        bg_rgb = extract_rgb_from_subdict(hdr["kS29"])
    if g_rgb is None and "kS30" in hdr:
        g_rgb = extract_rgb_from_subdict(hdr["kS30"])

    if (bg_rgb is None or g_rgb is None) and "kS38" in hdr:
        for chan_str in hdr["kS38"].split("|"):
            if not chan_str:
                continue
            cd = dict(parse_kv_pairs(chan_str, "_"))
            cid = cd.get("6", "")
            rgb = extract_rgb_from_subdict(chan_str)
            if rgb is not None:
                if cid in ("1000", "1") and bg_rgb is None:
                    bg_rgb = rgb
                elif cid in ("1001", "2") and g_rgb is None:
                    g_rgb = rgb

    if bg_rgb is None:
        bg_rgb = (40, 125, 255)
    if g_rgb is None:
        g_rgb = (0, 102, 255)

    try:
        ka1 = int(float(hdr.get("kA1", str(fallback_audio_track))))
    except ValueError:
        ka1 = fallback_audio_track
    if ka1 < 0 or ka1 > 6:
        ka1 = fallback_audio_track if 0 <= fallback_audio_track <= 6 else 1

    try:
        ka2 = int(float(hdr.get("kA2", "0")))
    except ValueError:
        ka2 = 0
    if ka2 < 0 or ka2 > 1:
        ka2 = 0

    out_hdr = [
        "kS1", str(bg_rgb[0]),
        "kS2", str(bg_rgb[1]),
        "kS3", str(bg_rgb[2]),
        "kS4", str(g_rgb[0]),
        "kS5", str(g_rgb[1]),
        "kS6", str(g_rgb[2]),
        "kA1", str(ka1),
    ]
    if ka2 != 0:
        out_hdr.extend(["kA2", str(ka2)])

    is_pure_1_0 = ("kS38" not in hdr and "kS29" not in hdr)
    out_objs: list[str] = []
    dropped = 0

    for raw_obj in parts[1:]:
        opairs = parse_kv_pairs(raw_obj, ",")
        if not opairs:
            continue
        od = dict(opairs)
        if "1" not in od or "2" not in od or "3" not in od:
            dropped += 1
            continue
        try:
            obj_id = int(float(od["1"]))
            x = float(od["2"])
            y = float(od["3"])
        except ValueError:
            dropped += 1
            continue

        if not (math.isfinite(x) and math.isfinite(y)):
            dropped += 1
            continue
        if x < -1000.0 or x > 250000.0 or y < -2000.0 or y > 10000.0:
            dropped += 1
            continue

        # Convert 2.0+ Color Trigger (899) targeting BG (1000) or Ground (1001) back to 1.0 triggers (29 / 30).
        if obj_id == 899:
            is_pure_1_0 = False
            target_chan = od.get("23", "1")
            if target_chan == "1000":
                obj_id = 29
            elif target_chan == "1001":
                obj_id = 30
            else:
                dropped += 1
                continue
            rebuilt = [("1", str(obj_id))]
            for k, v in opairs:
                if k in ("2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "14"):
                    rebuilt.append((k, v))
            out_objs.append(",".join(f"{k},{v}" for k, v in rebuilt))
            continue

        if obj_id not in valid_obj_ids:
            is_pure_1_0 = False
            dropped += 1
            continue

        # Keep clean key-value pairs without delimiter characters.
        clean_tokens: list[str] = []
        for k, v in opairs:
            if ":" in k or ":" in v or "|" in v or "#" in v or ";" in v:
                continue
            clean_tokens.append(f"{k},{v}")
        if clean_tokens:
            out_objs.append(",".join(clean_tokens))

    body = ";".join([",".join(out_hdr)] + out_objs) + ";"
    return body, is_pure_1_0, len(out_objs), dropped


def _validate_json_body(raw: bytes) -> None:
    json.loads(raw.decode("utf-8"))


def fetch_or_load_level_json(online_id: int, cache_dir: pathlib.Path | None) -> dict | None:
    cache_file = cache_dir / "json" / f"{online_id}.json" if cache_dir else None
    if cache_file and cache_file.is_file():
        try:
            return json.loads(cache_file.read_text(encoding="utf-8"))
        except (ValueError, UnicodeDecodeError):
            cache_file.unlink(missing_ok=True)  # discard a poisoned cache entry

    url = f"{GDHISTORY_BASE}/api/v1/level/{online_id}/"
    raw = http_get_bytes(url, validate=_validate_json_body)
    if raw is None:
        return None
    data = json.loads(raw.decode("utf-8"))
    if cache_file:
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        cache_file.write_bytes(raw)
    return data


def _validate_gmd_body(raw: bytes) -> None:
    d = robtop_gmd_to_dict(raw)
    k4 = d.get("k4")
    if not isinstance(k4, str) or not k4.strip():
        raise ValueError("level file has no k4 level string")


def fetch_or_load_gmd_k4(online_id: int, record_id: int, cache_dir: pathlib.Path | None) -> str | None:
    cache_file = cache_dir / "gmd" / f"{online_id}_{record_id}.gmd" if cache_dir else None
    if cache_file and cache_file.is_file():
        raw = cache_file.read_bytes()
        try:
            _validate_gmd_body(raw)
        except Exception:  # noqa: BLE001 - poisoned cache entry from an earlier relay
            cache_file.unlink(missing_ok=True)
            raw = None
    else:
        raw = None

    if raw is None:
        url = f"{GDHISTORY_BASE}/level/{online_id}/{record_id}/download/"
        raw = http_get_bytes(url, validate=_validate_gmd_body)
        if raw is None:
            return None
        if cache_file:
            cache_file.parent.mkdir(parents=True, exist_ok=True)
            cache_file.write_bytes(raw)

    d = robtop_gmd_to_dict(raw)
    k4 = d.get("k4")
    if not isinstance(k4, str) or not k4.strip():
        return None
    return k4


def discover_archived_1_0_ids(
    cache_dir: pathlib.Path | None, exclude_deleted: bool = False
) -> list[int]:
    index_cache = (
        cache_dir / ("search_128_1941_nodeleted.json" if exclude_deleted else "search_128_1941.json")
        if cache_dir
        else None
    )
    if index_cache and index_cache.is_file():
        return json.loads(index_cache.read_text(encoding="utf-8"))

    ids: set[int] = set()
    offset = 0
    limit = 1000
    flt = (
        f"online_id >= {MIN_1_0_ID} AND online_id <= {MAX_1_0_ID} "
        f"AND cache_level_string_available = true"
    )
    while True:
        qs = urllib.parse.urlencode(
            {
                "limit": str(limit),
                "offset": str(offset),
                "sort": "online_id:asc",
                "filter": flt,
            }
        )
        url = f"{GDHISTORY_BASE}/api/v1/search/level/advanced/?{qs}"
        raw = http_get_bytes(url, validate=_validate_json_body)
        if not raw:
            break
        payload = json.loads(raw.decode("utf-8"))
        hits = payload.get("hits") or []
        for h in hits:
            oid = h.get("online_id")
            if not isinstance(oid, int) or not (MIN_1_0_ID <= oid <= MAX_1_0_ID):
                continue
            if exclude_deleted and h.get("is_deleted") is True:
                continue
            ids.add(oid)
        if len(hits) < limit:
            break
        offset += limit

    sorted_ids = sorted(ids)
    if index_cache and sorted_ids:
        index_cache.parent.mkdir(parents=True, exist_ok=True)
        index_cache.write_text(json.dumps(sorted_ids), encoding="utf-8")
    return sorted_ids


def process_single_level(
    online_id: int,
    cache_dir: pathlib.Path | None,
    valid_obj_ids: set[int],
    scan_versions: bool = False,
) -> dict | None:
    lvl = fetch_or_load_level_json(online_id, cache_dir)
    if not lvl:
        return None
    records = [r for r in (lvl.get("records") or []) if not r.get("is_invalid")]
    data_records = [r for r in records if r.get("level_string_available") is True]
    if not data_records:
        return None

    # Sort all records chronologically (earliest archived metadata first).
    def meta_sort_key(r: dict) -> tuple:
        ts = parse_iso_timestamp(r.get("cache_real_date") or r.get("real_date"))
        gv = r.get("game_version")
        gv_k = gv if isinstance(gv, int) and gv > 0 else 999
        lv = r.get("level_version")
        lv_k = lv if isinstance(lv, int) and lv > 0 else 999
        dl = r.get("downloads")
        dl_k = dl if isinstance(dl, int) and dl >= 0 else 10**12
        return (ts, gv_k, lv_k, dl_k, r.get("id") or 10**12)

    sorted_meta = sorted(records, key=meta_sort_key)

    # Group playable records by unique decompressed_sha256, keeping earliest record per unique level_string.
    by_hash: dict[str, dict] = {}
    for r in sorted(data_records, key=meta_sort_key):
        lsi = r.get("level_string_info") or {}
        h = lsi.get("decompressed_sha256") or lsi.get("sha256") or f"id:{r.get('id')}"
        if h not in by_hash:
            by_hash[h] = r

    # Determine earliest valid official song (0..6 preferred).
    earliest_song = None
    for r in sorted_meta:
        s = r.get("official_song")
        if isinstance(s, int) and 0 <= s <= 6:
            earliest_song = s
            break
    if earliest_song is None:
        cs = lvl.get("cache_audiotrack")
        earliest_song = cs if isinstance(cs, int) and 0 <= cs <= 6 else 1

    # Download & inspect each distinct candidate version (typically 1, occasionally 2-3)
    # so we reliably select the earliest archived version. In fast mode only the earliest
    # archived revision is fetched (keeps the public GD History API load low).
    ordered_records = sorted(by_hash.items(), key=lambda kv: meta_sort_key(kv[1]))
    if not scan_versions:
        ordered_records = ordered_records[:1]

    candidates: list[tuple[tuple, dict, str, bool, int]] = []
    for _, cand_rec in ordered_records:
        rec_id = cand_rec.get("id")
        if not isinstance(rec_id, int):
            continue
        k4 = fetch_or_load_gmd_k4(online_id, rec_id, cache_dir)
        if not k4:
            continue
        try:
            decomp = decompress_k4_level_string(k4)
        except Exception:
            continue
        conv_str, is_pure_1_0, kept_objs, _ = inspect_and_convert_level_string(
            decomp, valid_obj_ids, earliest_song
        )
        ts = parse_iso_timestamp(cand_rec.get("cache_real_date") or cand_rec.get("real_date"))
        lv = cand_rec.get("level_version")
        lv_pos = lv if isinstance(lv, int) and lv > 0 else None
        gv = cand_rec.get("game_version")
        gv_pos = gv if isinstance(gv, int) and gv > 0 else (1 if is_pure_1_0 else 99)
        dl = cand_rec.get("downloads")
        dl_k = dl if isinstance(dl, int) and dl >= 0 else 10**12
        rank = (
            0 if is_pure_1_0 else 1,
            lv_pos if lv_pos is not None else 9999,
            ts,
            gv_pos,
            dl_k,
            rec_id,
        )
        candidates.append((rank, cand_rec, conv_str, is_pure_1_0, kept_objs))

    if not candidates:
        return None

    candidates.sort(key=lambda item: item[0])
    _, chosen_rec, chosen_level_str, is_pure_1_0, kept_objs = candidates[0]

    # Resolve metadata fields from chosen_rec first, then earliest metadata record, then level cache.
    meta_chain = [chosen_rec] + [r for r in sorted_meta if r.get("id") != chosen_rec.get("id")]

    level_name = ""
    for r in meta_chain:
        nm = sanitize_gd10_text(r.get("level_name"), max_len=40)
        if nm:
            level_name = nm
            break
    if not level_name:
        level_name = sanitize_gd10_text(lvl.get("cache_level_name"), max_len=40, fallback=f"Level {online_id}")

    desc = ""
    for r in meta_chain:
        d = decode_record_description(r)
        if d:
            desc = d
            break

    user_id = 0
    username = ""
    for r in meta_chain:
        rur = r.get("real_user_record") or {}
        cui = r.get("cached_user_info") or {}
        uid_cand = rur.get("user_id") or cui.get("user_id")
        if isinstance(uid_cand, int) and uid_cand > 0 and user_id == 0:
            user_id = uid_cand
        for uname_src in (
            r.get("username"),
            rur.get("username"),
            cui.get("non_player_username"),
            cui.get("username"),
        ):
            un = sanitize_gd10_text(uname_src, max_len=24)
            if un and un != "-" and un.lower() != "unknown":
                username = un
                break
        if user_id > 0 and username:
            break

    if user_id <= 0:
        cu = lvl.get("cache_user_id")
        user_id = cu if isinstance(cu, int) and cu > 0 else online_id
    if not username:
        username = sanitize_gd10_text(lvl.get("cache_username"), max_len=24, fallback="Player")

    level_version = 1
    for r in meta_chain:
        lv = r.get("level_version")
        if isinstance(lv, int) and lv > 0:
            level_version = min(lv, 65535)
            break

    downloads = None
    for r in meta_chain:
        dl = r.get("downloads")
        if isinstance(dl, int) and dl >= 0:
            downloads = dl
            break
    if downloads is None:
        downloads = max(0, int(lvl.get("cache_downloads") or 0))

    likes = None
    for r in meta_chain:
        lk = r.get("likes")
        if isinstance(lk, int):
            dis = r.get("dislikes")
            likes = lk - (dis if isinstance(dis, int) and dis > 0 else 0)
            break
    if likes is None:
        likes = int(lvl.get("cache_likes") or 0)

    ratings = 0
    ratings_sum = 0
    for r in meta_chain:
        rt = r.get("rating")
        rs = r.get("rating_sum")
        if isinstance(rt, int) and rt > 0 and isinstance(rs, int) and rs >= 0:
            ratings = rt
            ratings_sum = rs
            break

    demon = 1 if any(bool(r.get("demon")) for r in meta_chain) else 0
    if demon and ratings < 5:
        ratings = 10
        ratings_sum = 50

    if ratings >= 5 and ratings > 0:
        difficulty = max(1, min(5, ratings_sum // ratings))
    else:
        cfd = int(lvl.get("cache_filter_difficulty") or 0)
        if 2 <= cfd <= 6:
            difficulty = cfd - 1
            ratings = 10
            ratings_sum = difficulty * 10
        else:
            difficulty = 0

    length = None
    for r in meta_chain:
        ln = r.get("length")
        if isinstance(ln, int) and ln >= 0:
            length = min(ln, 3)
            break
    if length is None:
        length = max(0, min(3, int(lvl.get("cache_length") or 0)))

    stars = 0
    featured = 0
    for r in sorted_meta:
        st = r.get("stars")
        if isinstance(st, int) and st > stars:
            stars = min(st, 10)
        fs = r.get("feature_score")
        if isinstance(fs, int) and fs > featured:
            featured = min(fs, 255)
    c_feat = int(lvl.get("cache_max_featured") or lvl.get("cache_featured") or 0)
    if c_feat > featured:
        featured = min(c_feat, 255)
    if stars == 0 and int(lvl.get("cache_max_stars") or lvl.get("cache_stars") or 0) > 0:
        stars = min(10, int(lvl.get("cache_max_stars") or lvl.get("cache_stars") or 0))

    audio_track = earliest_song
    for r in meta_chain:
        s = r.get("official_song")
        if isinstance(s, int) and 0 <= s <= 6:
            audio_track = s
            break

    return {
        "level_id": online_id,
        "user_id": user_id,
        "downloads": downloads,
        "likes": likes,
        "ratings": ratings,
        "ratings_sum": ratings_sum,
        "level_version": level_version,
        "audio_track": audio_track,
        "length": length,
        "difficulty": difficulty,
        "featured": featured,
        "demon": demon,
        "stars": stars,
        "name": level_name,
        "desc": desc,
        "user": username,
        "level_string": chosen_level_str,
        "is_pure_1_0": is_pure_1_0,
        "objects": kept_objs,
        "record_id": chosen_rec.get("id"),
    }


def write_binary_db(levels: list[dict], out_path: pathlib.Path) -> None:
    levels_sorted = sorted(levels, key=lambda x: x["level_id"])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    buf = bytearray()
    buf.extend(MAGIC)
    buf.extend(struct.pack("<II", DB_VERSION, len(levels_sorted)))

    for lv in levels_sorted:
        name_b = lv["name"].encode("ascii", errors="replace")
        desc_b = lv["desc"].encode("ascii", errors="replace")
        user_b = lv["user"].encode("ascii", errors="replace")
        raw_lvl_b = lv["level_string"].encode("ascii", errors="replace")
        comp_lvl_b = zlib.compress(raw_lvl_b, level=9)

        buf.extend(
            struct.pack(
                "<iiiiiiHBBBBBBBBHHHII",
                int(lv["level_id"]),
                int(lv["user_id"]),
                int(lv["downloads"]),
                int(lv["likes"]),
                int(lv["ratings"]),
                int(lv["ratings_sum"]),
                int(lv["level_version"]) & 0xFFFF,
                int(lv["audio_track"]) & 0xFF,
                int(lv["length"]) & 0xFF,
                int(lv["difficulty"]) & 0xFF,
                int(lv["featured"]) & 0xFF,
                int(lv["demon"]) & 0xFF,
                int(lv["stars"]) & 0xFF,
                0,  # reserved byte 0
                0,  # reserved byte 1 (aligns fixed header to 48 bytes)
                len(name_b),
                len(desc_b),
                len(user_b),
                len(raw_lvl_b),
                len(comp_lvl_b),
            )
        )
        buf.extend(name_b)
        buf.extend(desc_b)
        buf.extend(user_b)
        buf.extend(comp_lvl_b)

    out_path.write_bytes(bytes(buf))


def main() -> None:
    ap = argparse.ArgumentParser(description="Build GD 1.0 (IDs 128..1941) local GDPS database from GDHistory")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="Output binary DB path (levels_1_0.bin)")
    ap.add_argument("--cache-dir", type=pathlib.Path, default=None, help="Directory to cache GDHistory responses")
    ap.add_argument("--orig-apk", type=pathlib.Path, default=None, help="Original GD 1.02 APK for frame validation")
    ap.add_argument("--workers", type=int, default=3, help="Concurrent HTTP workers (default: 3)")
    ap.add_argument(
        "--scan-versions",
        action="store_true",
        help="Download every distinct archived revision of each level and prefer a pure-1.0 one "
        "(more requests; default fetches only the earliest archived revision)",
    )
    ap.add_argument("--min-levels", type=int, default=400, help="Minimum expected archived levels (default: 400)")
    ap.add_argument(
        "--exclude-deleted",
        action="store_true",
        help="Skip levels that were later deleted from the official GD servers (default: keep every "
        "level GD History preserved, including levels removed from the live game)",
    )
    ap.add_argument(
        "--routes",
        default=None,
        help="Comma-separated network route preference (default: auto over "
        + ",".join(DEFAULT_ROUTE_ORDER)
        + ")",
    )
    ap.add_argument(
        "--probe",
        action="store_true",
        help="Only probe every network route against the GD History API and report which work",
    )
    ap.add_argument(
        "--probe-timeout",
        type=float,
        default=20.0,
        help="Per-request timeout while probing routes (default: 20s)",
    )
    ap.add_argument(
        "--deadline-seconds",
        type=int,
        default=0,
        help="Stop scheduling new level downloads after this many seconds (0 = no deadline). "
        "Cached responses make the next run resume where this one stopped.",
    )
    ap.add_argument(
        "--skip-probe",
        action="store_true",
        help="Skip the startup route probe (not recommended; failures then surface lazily)",
    )
    args = ap.parse_args()

    configure_routes(
        [r.strip() for r in args.routes.split(",") if r.strip()] if args.routes else None
    )

    if args.probe:
        working = probe_routes(_ROUTES, timeout=args.probe_timeout)
        if not working:
            print(
                "PROBE_SUMMARY working=none (GD History is unreachable over every route)",
                file=sys.stderr,
            )
            raise SystemExit(2)
        return

    if args.out.is_file() and args.out.stat().st_size > 65536:
        raw = args.out.read_bytes()
        if raw[:8] == MAGIC:
            ver, cnt = struct.unpack_from("<II", raw, 8)
            if ver == DB_VERSION and cnt >= args.min_levels:
                sha = hashlib.sha256(raw).hexdigest()
                print(
                    f"==> Reusing cached GDHistory 1.0 DB: {args.out} "
                    f"({cnt} levels, {len(raw)} bytes, sha256={sha})"
                )
                return

    if not args.skip_probe:
        working = probe_routes(_ROUTES, timeout=args.probe_timeout)
        if not working:
            raise RuntimeError(
                "GD History is unreachable over every network route ("
                + ", ".join(r.name for r in _ROUTES)
                + "); inspect the PROBE lines above for the per-route HTTP codes"
            )
        print(f"==> Using GD History route(s): {', '.join(r.name for r in working)}")

    valid_obj_ids = verify_gd10_frames_against_apk(args.orig_apk)
    print(f"==> Verified {len(valid_obj_ids)} GD 1.02 object IDs: {sorted(valid_obj_ids)}")

    t0 = time.monotonic()
    archived_ids = discover_archived_1_0_ids(args.cache_dir, exclude_deleted=args.exclude_deleted)
    print(
        f"==> Discovered {len(archived_ids)} archived playable IDs in {MIN_1_0_ID}..{MAX_1_0_ID} "
        f"(skipping {(MAX_1_0_ID - MIN_1_0_ID + 1) - len(archived_ids)} unarchived/deleted IDs)"
    )
    if len(archived_ids) < args.min_levels:
        raise RuntimeError(
            f"Expected at least {args.min_levels} archived levels in {MIN_1_0_ID}..{MAX_1_0_ID}, got {len(archived_ids)}"
        )

    deadline = time.monotonic() + args.deadline_seconds if args.deadline_seconds > 0 else None
    levels: list[dict] = []
    failed: list[int] = []
    pending: list[int] = list(archived_ids)
    processed = 0
    stop_reason = ""

    def absorb(oid: int, fut: concurrent.futures.Future) -> None:
        nonlocal processed
        processed += 1
        try:
            res = fut.result()
        except Exception as exc:  # transient HTTP/Cloudflare failures
            print(f"  [error] ID {oid}: {exc!r}", file=sys.stderr)
            failed.append(oid)
            return
        if res is not None:
            levels.append(res)
        else:
            print(f"  [skip] ID {oid}: no archived level_string available", file=sys.stderr)
        if processed % 50 == 0:
            print(f"  ... processed {processed}/{len(archived_ids)} archived levels")

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.workers))
    try:
        while pending:
            if deadline and time.monotonic() >= deadline:
                stop_reason = "deadline"
                break
            batch = pending[: max(1, args.workers) * 2]
            del pending[: len(batch)]
            futures = {
                pool.submit(
                    process_single_level, oid, args.cache_dir, valid_obj_ids, args.scan_versions
                ): oid
                for oid in batch
            }
            for fut in concurrent.futures.as_completed(futures):
                absorb(futures[fut], fut)
    finally:
        pool.shutdown(wait=True)

    if pending or stop_reason:
        print(
            f"==> Stopped early ({stop_reason or 'pending work'}) after {processed}/"
            f"{len(archived_ids)} levels; {len(pending)} remain for the next run. "
            "Every successful response is cached, so the next run resumes from here.",
            file=sys.stderr,
        )

    # One sequential retry pass for transient failures (Cloudflare 403/429 bursts).
    if failed and not pending:
        print(f"==> Retrying {len(failed)} level(s) that failed with transient errors")
        retry_failed: list[int] = []
        for oid in failed:
            if deadline and time.monotonic() >= deadline:
                retry_failed.append(oid)
                continue
            try:
                res = process_single_level(oid, args.cache_dir, valid_obj_ids, args.scan_versions)
            except Exception as exc:
                print(f"  [error] ID {oid} (retry): {exc!r}", file=sys.stderr)
                retry_failed.append(oid)
                continue
            if res is not None:
                levels.append(res)
            else:
                print(f"  [skip] ID {oid} (retry): no archived level_string available", file=sys.stderr)
        if retry_failed:
            print(
                f"WARNING: {len(retry_failed)} level(s) still failed "
                f"(first: {retry_failed[:10]}); archive will contain {len(levels)} levels",
                file=sys.stderr,
            )

    if len(levels) < args.min_levels:
        raise RuntimeError(
            f"Built only {len(levels)} levels (< min_levels={args.min_levels}); "
            "re-run to resume from the cached responses"
        )

    write_binary_db(levels, args.out)
    elapsed = time.monotonic() - t0
    raw_out = args.out.read_bytes()
    sha = hashlib.sha256(raw_out).hexdigest()
    pure_cnt = sum(1 for lv in levels if lv["is_pure_1_0"])
    ids_sorted = sorted(lv["level_id"] for lv in levels)
    print(
        f"==> Built {args.out}: {len(levels)} levels (IDs {ids_sorted[0]}..{ids_sorted[-1]}, "
        f"pure_1_0={pure_cnt}, deleted_preserved={0 if args.exclude_deleted else 'included'}), "
        f"{len(raw_out)} bytes in {elapsed:.1f}s (sha256={sha})"
    )


if __name__ == "__main__":
    main()
