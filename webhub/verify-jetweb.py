#!/usr/bin/env python3
"""
verify-jetweb.py: Post-package verification for JetWeb.tipa.
Validates version consistency, Mach-O ARM64 headers, LC_CODE_SIGNATURE,
and embedded ldid entitlements for both Payload/JetWeb.app/JetWeb and
Payload/JetWeb.app/jetwebctl.
"""

import argparse
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


def check_macho_arm64_signed(path: str) -> None:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 32:
        raise RuntimeError(f"{path}: file too small ({len(data)} bytes)")
    magic, cputype, _, _, ncmds, _, _, _ = struct.unpack("<IIIIIIII", data[:32])
    if magic != 0xFEEDFACF or cputype != 0x0100000C:
        raise RuntimeError(f"{path}: expected 64-bit ARM64 Mach-O, got magic=0x{magic:x} cpu=0x{cputype:x}")

    off = 32
    has_sig = False
    for _ in range(ncmds):
        if off + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack("<II", data[off : off + 8])
        if cmd == 0x1D:  # LC_CODE_SIGNATURE
            _, _, cs_off, cs_sz = struct.unpack("<IIII", data[off : off + 16])
            if cs_off > 0 and cs_sz >= 12 and cs_off + cs_sz <= len(data):
                sb_magic = struct.unpack(">I", data[cs_off : cs_off + 4])[0]
                if sb_magic == 0xFADE0CC0:
                    has_sig = True
        off += cmdsize
    if not has_sig:
        raise RuntimeError(f"{path}: missing valid LC_CODE_SIGNATURE SuperBlob")


def verify_entitlements(ldid_bin: str, bin_path: str, expected_plist_path: str) -> None:
    res = subprocess.run([ldid_bin, "-e", bin_path], capture_output=True, text=True, check=True)
    actual = plistlib.loads(res.stdout.encode("utf-8"))
    with open(expected_plist_path, "rb") as f:
        expected = plistlib.load(f)
    if actual != expected:
        raise RuntimeError(f"{bin_path}: embedded entitlements mismatch!\nactual={actual}\nexpected={expected}")


def extract_jw_version(main_m_path: str) -> str:
    with open(main_m_path, "r", encoding="utf-8") as f:
        src = f.read()
    m = re.search(r'#define\s+JW_VERSION\s+"([^"]+)"', src)
    return m.group(1) if m else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tipa", required=True)
    args = parser.parse_args()

    tipa_path = os.path.abspath(args.tipa)
    webhub_dir = os.path.dirname(os.path.abspath(__file__))
    ldid_bin = shutil.which("ldid")
    if not ldid_bin:
        print("[FAIL] ldid not found in PATH", file=sys.stderr)
        return 2

    src_ver = extract_jw_version(os.path.join(webhub_dir, "app", "main.m"))

    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(tipa_path, "r") as zf:
            zf.extractall(tmp)

        app_dir = os.path.join(tmp, "Payload", "JetWeb.app")
        app_bin = os.path.join(app_dir, "JetWeb")
        ctl_bin = os.path.join(app_dir, "jetwebctl")
        info_plist = os.path.join(app_dir, "Info.plist")

        for req in (app_bin, ctl_bin, info_plist):
            if not os.path.isfile(req):
                raise RuntimeError(f"Missing required file in tipa: {req}")

        with open(info_plist, "rb") as f:
            info = plistlib.load(f)
        if info.get("CFBundleIdentifier") != "com.lili.jetweb":
            raise RuntimeError(f"Unexpected CFBundleIdentifier: {info.get('CFBundleIdentifier')}")
        plist_ver = info.get("CFBundleShortVersionString", "")
        if not src_ver or plist_ver != src_ver:
            raise RuntimeError(f"Version mismatch: Info.plist ({plist_ver}) != JW_VERSION ({src_ver})")

        check_macho_arm64_signed(app_bin)
        check_macho_arm64_signed(ctl_bin)
        verify_entitlements(ldid_bin, app_bin, os.path.join(webhub_dir, "entitlements-app.plist"))
        verify_entitlements(ldid_bin, ctl_bin, os.path.join(webhub_dir, "entitlements-helper.plist"))

    ver_file = tipa_path + ".verification"
    with open(ver_file, "w", encoding="utf-8") as vf:
        vf.write("STATUS: VERIFIED\n")
        vf.write(f"VERSION: {plist_ver}\n")
        vf.write(f"BUNDLE_ID: {info.get('CFBundleIdentifier', '')}\n")

    print(f"[OK] JetWeb.tipa v{plist_ver} verified: ARM64 Mach-O, LC_CODE_SIGNATURE, and entitlements match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
