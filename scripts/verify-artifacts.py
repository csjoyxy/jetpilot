#!/usr/bin/env python3
"""
verify-artifacts.py: Post-package signature and entitlements verification for JetPilot.

Verifies:
1. Embedded entitlements extracted via `ldid -e` match source plists exactly
   (src/entitlements-app.plist and src/entitlements-daemon.plist).
2. Daemon embedded entitlements contain com.apple.frontboard.launchapplications.
3. Code signature integrity:
   - Validates Mach-O header and LC_CODE_SIGNATURE
   - Validates SuperBlob and CodeDirectory structures
   - Verifies all code page hashes (SHA-256 / SHA-1) against binary pages
   - Verifies special slot hashes (entitlements blob slot -5)
   - Runs `codesign --verify` if codesign tool is present
4. Diagnostic logging into CI output:
   - CDHash (20-byte candidate and 32-byte full)
   - Hash type
   - Embedded entitlements
   - cputype / cpusubtype
   - Binary file size and code page count
   - otool -hv (Mach header)
   - otool -l (LC_BUILD_VERSION: minos, sdk, tool)
"""

import argparse
import hashlib
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


EXTRA_SEARCH_PATHS = [
    "/data/home/bin",
    "/data/home/theos/toolchain/linux/iphone/bin",
    "/usr/local/bin",
    "/opt/homebrew/bin",
    "/usr/bin",
]


def find_tool(name: str) -> str | None:
    path = shutil.which(name)
    if path:
        return path
    for d in EXTRA_SEARCH_PATHS:
        cand = os.path.join(d, name)
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def parse_macho(data: bytes, file_path: str):
    if len(data) < 32:
        raise ValueError(f"File too small for Mach-O: {len(data)} bytes ({file_path})")

    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack(
        "<IIIIIIII", data[:32]
    )

    if magic == 0xFEEDFACF:
        is_64 = True
        header_size = 32
    elif magic == 0xFEEDFACE:
        is_64 = False
        header_size = 28
    else:
        raise ValueError(f"Invalid Mach-O magic: 0x{magic:08x} ({file_path})")

    cputype_names = {
        0x0100000C: "ARM64",
        0x01000007: "X86_64",
        0x0000000C: "ARM",
        0x00000007: "I386",
    }
    cputype_str = cputype_names.get(cputype, f"0x{cputype:08x}")
    cpusubtype_str = "ALL" if cpusubtype == 0 else f"0x{cpusubtype:08x}"

    offset = header_size
    cs_offset, cs_size = 0, 0
    build_version = None

    for _ in range(ncmds):
        if offset + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack("<II", data[offset : offset + 8])
        if cmd == 0x1D:  # LC_CODE_SIGNATURE
            _, _, cs_offset, cs_size = struct.unpack("<IIII", data[offset : offset + 16])
        elif cmd == 0x32:  # LC_BUILD_VERSION
            _, _, platform, minos, sdk, ntools = struct.unpack(
                "<IIIIII", data[offset : offset + 24]
            )
            minos_str = f"{(minos >> 16) & 0xff}.{(minos >> 8) & 0xff}.{minos & 0xff}"
            sdk_str = f"{(sdk >> 16) & 0xff}.{(sdk >> 8) & 0xff}.{sdk & 0xff}"
            build_version = {
                "platform": platform,
                "minos": minos_str,
                "sdk": sdk_str,
                "ntools": ntools,
            }
        offset += cmdsize

    return {
        "magic": magic,
        "is_64": is_64,
        "cputype": cputype,
        "cputype_str": cputype_str,
        "cpusubtype": cpusubtype,
        "cpusubtype_str": cpusubtype_str,
        "ncmds": ncmds,
        "cs_offset": cs_offset,
        "cs_size": cs_size,
        "build_version": build_version,
    }


def verify_code_signature(data: bytes, cs_offset: int, cs_size: int, bin_path: str):
    if not cs_offset or not cs_size:
        raise ValueError(f"Missing LC_CODE_SIGNATURE in {bin_path}")

    if cs_offset + 12 > len(data):
        raise ValueError(f"LC_CODE_SIGNATURE offset out of bounds: {cs_offset}")

    cs_magic, cs_len, count = struct.unpack(">III", data[cs_offset : cs_offset + 12])
    if cs_magic != 0xFADE0CC0:
        raise ValueError(
            f"Invalid SuperBlob magic: 0x{cs_magic:08x} (expected 0xfade0cc0) in {bin_path}"
        )

    cd_offset = None
    ent_offset = None
    for i in range(count):
        slot_type, slot_off = struct.unpack(
            ">II", data[cs_offset + 12 + i * 8 : cs_offset + 20 + i * 8]
        )
        if slot_type == 0:  # CSSLOT_CODEDIRECTORY
            cd_offset = cs_offset + slot_off
        elif slot_type == 5:  # CSSLOT_ENTITLEMENTS
            ent_offset = cs_offset + slot_off

    if cd_offset is None:
        raise ValueError(f"CSSLOT_CODEDIRECTORY not found in signature ({bin_path})")

    # CodeDirectory struct unpacking
    (
        cd_magic,
        cd_len,
        cd_ver,
        cd_flags,
        hash_off,
        ident_off,
        n_special,
        n_code,
        code_limit,
        hash_size,
        hash_type,
        platform,
        page_size_exp,
        spare2,
    ) = struct.unpack(">IIIIIIIIIBBBBI", data[cd_offset : cd_offset + 44])

    if cd_magic != 0xFADE0C02:
        raise ValueError(
            f"Invalid CodeDirectory magic: 0x{cd_magic:08x} (expected 0xfade0c02) in {bin_path}"
        )

    if hash_type == 2:
        hasher_name = "SHA256"
        hasher = hashlib.sha256
    elif hash_type == 1:
        hasher_name = "SHA1"
        hasher = hashlib.sha1
    else:
        raise ValueError(f"Unsupported hash type: {hash_type} in {bin_path}")

    cd_bytes = data[cd_offset : cd_offset + cd_len]
    cdhash_full = hasher(cd_bytes).hexdigest()
    cdhash_20 = cdhash_full[:40]
    page_size = 1 << page_size_exp

    hashes_base = cd_offset + hash_off
    # Verify positive code page hashes
    for i in range(n_code):
        page_start = i * page_size
        page_end = min((i + 1) * page_size, code_limit)
        actual_hash = hasher(data[page_start:page_end]).digest()
        expected_hash = data[
            hashes_base + i * hash_size : hashes_base + (i + 1) * hash_size
        ]
        if actual_hash != expected_hash:
            raise ValueError(
                f"Code page {i} hash mismatch ({page_start}..{page_end}): "
                f"expected {expected_hash.hex()}, got {actual_hash.hex()} in {bin_path}"
            )

    # Verify entitlements slot (-5) if present
    if ent_offset and n_special >= 5:
        expected_ent_hash = data[
            hashes_base - 5 * hash_size : hashes_base - 4 * hash_size
        ]
        ent_magic, ent_len = struct.unpack(">II", data[ent_offset : ent_offset + 8])
        actual_ent_hash = hasher(data[ent_offset : ent_offset + ent_len]).digest()
        if actual_ent_hash != expected_ent_hash:
            raise ValueError(
                f"Entitlements slot (-5) hash mismatch: "
                f"expected {expected_ent_hash.hex()}, got {actual_ent_hash.hex()} in {bin_path}"
            )

    return {
        "cdhash_20": cdhash_20,
        "cdhash_full": cdhash_full,
        "hash_name": hasher_name,
        "hash_type": hash_type,
        "hash_size": hash_size,
        "n_code": n_code,
        "code_limit": code_limit,
        "page_size": page_size,
        "n_special": n_special,
        "has_ent_slot": ent_offset is not None,
    }


def compare_entitlements(embedded_dict: dict, expected_dict: dict, name: str) -> list[str]:
    diffs = []
    all_keys = set(embedded_dict.keys()) | set(expected_dict.keys())
    for k in sorted(all_keys):
        if k not in embedded_dict:
            diffs.append(f"Missing key in embedded entitlements: {k}")
        elif k not in expected_dict:
            diffs.append(f"Unexpected extra key in embedded entitlements: {k}")
        elif embedded_dict[k] != expected_dict[k]:
            diffs.append(
                f"Value mismatch for key '{k}': embedded={embedded_dict[k]!r}, expected={expected_dict[k]!r}"
            )
    return diffs


def verify_binary(bin_path: str, expected_plist_path: str, is_daemon: bool) -> bool:
    bin_name = os.path.basename(bin_path)
    print(f"============================================================")
    print(f"=== Verifying Binary: {bin_name} ===")
    print(f"============================================================")

    if not os.path.isfile(bin_path):
        print(f"[FAIL] Binary not found at: {bin_path}")
        return False
    if not os.path.isfile(expected_plist_path):
        print(f"[FAIL] Expected entitlements file not found: {expected_plist_path}")
        return False

    with open(bin_path, "rb") as f:
        data = f.read()

    # 1. Mach-O Structure
    try:
        macho = parse_macho(data, bin_path)
    except Exception as e:
        print(f"[FAIL] Failed parsing Mach-O structure: {e}")
        return False

    # 2. Extract Embedded Entitlements via ldid -e
    ldid_bin = find_tool("ldid")
    if not ldid_bin:
        print("[FAIL] ldid tool not found in PATH or standard toolchain paths!")
        return False

    try:
        proc = subprocess.run([ldid_bin, "-e", bin_path], capture_output=True, check=True)
        embedded_raw = proc.stdout
        embedded_ent = plistlib.loads(embedded_raw)
    except Exception as e:
        print(f"[FAIL] ldid -e failed or output invalid plist: {e}")
        return False

    # 3. Compare with Expected Entitlements
    try:
        with open(expected_plist_path, "rb") as f:
            expected_ent = plistlib.load(f)
    except Exception as e:
        print(f"[FAIL] Failed reading expected plist {expected_plist_path}: {e}")
        return False

    diffs = compare_entitlements(embedded_ent, expected_ent, bin_name)
    if diffs:
        print(f"[FAIL] Embedded entitlements mismatch against {os.path.basename(expected_plist_path)}:")
        for d in diffs:
            print(f"  - {d}")
        return False
    print(f"[OK] Embedded entitlements match {os.path.basename(expected_plist_path)} exactly.")

    # FrontBoard entitlement check for daemon
    if is_daemon:
        fb_key = "com.apple.frontboard.launchapplications"
        if embedded_ent.get(fb_key) is not True:
            print(f"[FAIL] Daemon missing required entitlement: {fb_key}")
            return False
        print(f"[OK] Daemon has required entitlement: {fb_key} == true")

    # 4. Code Signature & Page Hashes
    try:
        sig = verify_code_signature(data, macho["cs_offset"], macho["cs_size"], bin_path)
        print(
            f"[OK] Signature integrity: all {sig['n_code']} code page hashes verified ({sig['hash_name']}, page size {sig['page_size']}B)."
        )
        if sig["has_ent_slot"]:
            print(f"[OK] Special slot -5 (entitlements blob) hash verified.")
    except Exception as e:
        print(f"[FAIL] Code signature verification failed: {e}")
        return False

    # 5. External codesign --verify (if available)
    codesign_bin = find_tool("codesign")
    if codesign_bin:
        r_cs = subprocess.run(
            [codesign_bin, "--verify", "--verbose=2", bin_path],
            capture_output=True,
            text=True,
        )
        if r_cs.returncode != 0:
            print(f"[FAIL] codesign --verify reported failure:\n{r_cs.stderr}")
            return False
        print("[OK] codesign --verify verification passed.")
    else:
        print("[INFO] codesign tool not found on this system; page hashes verified directly.")

    # 6. Detailed Diagnostics to CI Log
    print("--- Diagnostic Information ---")
    print(f"File Path:        {bin_path}")
    print(f"File Size:        {len(data)} bytes")
    print(f"Page Count:       {sig['n_code']} pages (page size {sig['page_size']} bytes, code limit {sig['code_limit']})")
    print(f"CPU Type/Subtype: {macho['cputype_str']} / {macho['cpusubtype_str']}")
    print(f"Hash Type:        {sig['hash_name']} (type={sig['hash_type']}, size={sig['hash_size']} bytes)")
    print(f"Candidate CDHash: {sig['cdhash_20']}")
    print(f"Full CDHash:      {sig['cdhash_full']}")
    print("Embedded Entitlements (ldid -e):")
    print(embedded_raw.decode("utf-8", errors="replace").strip())

    otool_bin = find_tool("otool")
    if otool_bin:
        r_hv = subprocess.run([otool_bin, "-hv", bin_path], capture_output=True, text=True)
        print("otool -hv (Mach Header):")
        print(r_hv.stdout.strip())

        r_l = subprocess.run([otool_bin, "-l", bin_path], capture_output=True, text=True)
        lines = r_l.stdout.splitlines()
        found_lc = False
        for i, line in enumerate(lines):
            if "LC_BUILD_VERSION" in line:
                start_idx = max(0, i - 1) if lines[max(0, i - 1)].startswith("Load command") else i
                print("otool -l (LC_BUILD_VERSION):")
                print("\n".join(lines[start_idx : start_idx + 9]))
                found_lc = True
                break
        if not found_lc and macho["build_version"]:
            bv = macho["build_version"]
            print(f"LC_BUILD_VERSION (parsed): minos={bv['minos']}, sdk={bv['sdk']}, platform={bv['platform']}")
    elif macho["build_version"]:
        bv = macho["build_version"]
        print(f"LC_BUILD_VERSION (parsed): minos={bv['minos']}, sdk={bv['sdk']}, platform={bv['platform']}")

    print()
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Verify signatures, page hashes, and entitlements of JetPilot package."
    )
    parser.add_argument(
        "tipa_pos",
        nargs="?",
        default=None,
        help="Path to JetPilot.tipa (positional)",
    )
    parser.add_argument(
        "--tipa",
        dest="tipa_opt",
        default=None,
        help="Path to JetPilot.tipa (named option)",
    )
    parser.add_argument(
        "--app-plist",
        default=None,
        help="Path to entitlements-app.plist",
    )
    parser.add_argument(
        "--daemon-plist",
        default=None,
        help="Path to entitlements-daemon.plist",
    )
    args = parser.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    tipa_path = args.tipa_opt or args.tipa_pos or os.path.join(repo_root, "src", "JetPilot.tipa")
    app_plist = args.app_plist or os.path.join(repo_root, "src", "entitlements-app.plist")
    daemon_plist = args.daemon_plist or os.path.join(repo_root, "src", "entitlements-daemon.plist")

    if not os.path.isfile(tipa_path):
        print(f"[FAIL] tipa file not found: {tipa_path}")
        sys.exit(1)

    print(f"Verifying tipa package: {tipa_path}")
    temp_dir = tempfile.mkdtemp(prefix="jp_verify_")
    try:
        try:
            with zipfile.ZipFile(tipa_path, "r") as z:
                z.extractall(temp_dir)
        except Exception as e:
            print(f"[FAIL] Failed unzipping tipa file {tipa_path}: {e}")
            sys.exit(1)

        app_dir = os.path.join(temp_dir, "Payload", "JetPilot.app")
        if not os.path.isdir(app_dir):
            # Check if extracted directly
            cand = os.path.join(temp_dir, "JetPilot.app")
            if os.path.isdir(cand):
                app_dir = cand
            else:
                print(f"[FAIL] Payload/JetPilot.app not found inside {tipa_path}")
                sys.exit(1)

        daemon_bin = os.path.join(app_dir, "jetpilotd")
        app_bin = os.path.join(app_dir, "JetPilot")

        daemon_ok = verify_binary(daemon_bin, daemon_plist, is_daemon=True)
        app_ok = verify_binary(app_bin, app_plist, is_daemon=False)

        if not daemon_ok or not app_ok:
            print("[FAIL] Package verification FAILED.")
            sys.exit(1)

        print("============================================================")
        print("[SUCCESS] All package signatures and entitlements PASSED.")
        print("============================================================")
        sys.exit(0)
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
