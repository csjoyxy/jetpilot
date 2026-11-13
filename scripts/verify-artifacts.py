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


def verify_code_signature(data: bytes, cs_offset: int, cs_size: int, bin_path: str, quiet: bool = False):
    if not cs_offset or not cs_size:
        raise ValueError(f"Missing LC_CODE_SIGNATURE in {bin_path}")

    if cs_offset + 12 > len(data):
        raise ValueError(f"LC_CODE_SIGNATURE offset out of bounds: {cs_offset}")

    cs_magic, cs_len, count = struct.unpack(">III", data[cs_offset : cs_offset + 12])
    if cs_magic != 0xFADE0CC0:
        raise ValueError(
            f"Invalid SuperBlob magic: 0x{cs_magic:08x} (expected 0xfade0cc0) in {bin_path}"
        )

    slots_by_type = {}
    cds_to_verify = []  # list of (slot_type, blob_offset)

    for i in range(count):
        slot_type, slot_off = struct.unpack(
            ">II", data[cs_offset + 12 + i * 8 : cs_offset + 20 + i * 8]
        )
        blob_offset = cs_offset + slot_off
        slots_by_type[slot_type] = blob_offset

        if blob_offset + 8 <= len(data):
            blob_magic, blob_len = struct.unpack(">II", data[blob_offset : blob_offset + 8])
            # CSMAGIC_CODEDIRECTORY is 0xFADE0C02.
            # Slot 0 is primary CD, 0x1000..0x1005 are alternate CDs (CoreTrust bypass / TrollStore)
            if blob_magic == 0xFADE0C02 or slot_type == 0 or (0x1000 <= slot_type <= 0x1005):
                cds_to_verify.append((slot_type, blob_offset))

    if not cds_to_verify:
        raise ValueError(f"No CodeDirectory found in signature ({bin_path})")

    cd_results = []
    all_cds_ok = True

    for slot_type, cd_offset in cds_to_verify:
        if slot_type == 0:
            slot_name = "CSSLOT_CODEDIRECTORY (0x0000, primary)"
        elif 0x1000 <= slot_type <= 0x1005:
            slot_name = f"CSSLOT_ALTERNATE (0x{slot_type:04x}, CoreTrust alternate)"
        else:
            slot_name = f"Slot 0x{slot_type:04x}"

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
                f"Invalid CodeDirectory magic: 0x{cd_magic:08x} at slot 0x{slot_type:04x} in {bin_path}"
            )

        if hash_type == 2:
            hasher_name = "SHA256"
            hasher = hashlib.sha256
        elif hash_type == 1:
            hasher_name = "SHA1"
            hasher = hashlib.sha1
        else:
            raise ValueError(f"Unsupported hash type: {hash_type} at slot 0x{slot_type:04x} in {bin_path}")

        cd_bytes = data[cd_offset : cd_offset + cd_len]
        cdhash_full = hasher(cd_bytes).hexdigest()
        cdhash_20 = cdhash_full[:40]
        page_size = 1 << page_size_exp
        hashes_base = cd_offset + hash_off

        # 1. Verify code page hashes
        code_page_errors = []
        for p in range(n_code):
            page_start = p * page_size
            page_end = min((p + 1) * page_size, code_limit)
            actual_hash = hasher(data[page_start:page_end]).digest()
            expected_hash = data[
                hashes_base + p * hash_size : hashes_base + (p + 1) * hash_size
            ]
            if actual_hash != expected_hash:
                code_page_errors.append(
                    f"Page {p} ({page_start}..{page_end}): expected {expected_hash.hex()}, got {actual_hash.hex()}"
                )

        # 2. Verify all special slots (1..n_special)
        special_slot_errors = []
        special_slots_checked = []
        for s in range(1, n_special + 1):
            expected_ent_hash = data[
                hashes_base - s * hash_size : hashes_base - (s - 1) * hash_size
            ]
            is_zero = (expected_ent_hash == b"\x00" * hash_size)

            if s in slots_by_type:
                s_off = slots_by_type[s]
                s_magic, s_len = struct.unpack(">II", data[s_off : s_off + 8])
                actual_s_hash = hasher(data[s_off : s_off + s_len]).digest()
                if actual_s_hash != expected_ent_hash:
                    special_slot_errors.append(
                        f"Special slot -{s} hash mismatch: expected {expected_ent_hash.hex()}, got {actual_s_hash.hex()}"
                    )
                else:
                    special_slots_checked.append(f"-{s}")
            else:
                if not is_zero:
                    special_slot_errors.append(
                        f"Special slot -{s} expected in CD but missing from SuperBlob"
                    )

        cd_ok = (len(code_page_errors) == 0) and (len(special_slot_errors) == 0)
        if not cd_ok:
            all_cds_ok = False

        cd_info = {
            "slot_type": slot_type,
            "slot_name": slot_name,
            "hash_name": hasher_name,
            "hash_type": hash_type,
            "hash_size": hash_size,
            "page_size": page_size,
            "n_code": n_code,
            "code_limit": code_limit,
            "n_special": n_special,
            "special_checked": special_slots_checked,
            "cdhash_20": cdhash_20,
            "cdhash_full": cdhash_full,
            "code_page_errors": code_page_errors,
            "special_slot_errors": special_slot_errors,
            "ok": cd_ok,
        }
        cd_results.append(cd_info)

        if not quiet:
            if cd_ok:
                print(
                    f"[OK] CodeDirectory {slot_name}: {hasher_name}, all {n_code} code page hashes verified "
                    f"(limit {code_limit}B, special slots {special_slots_checked} verified)."
                )
            else:
                print(f"[FAIL] CodeDirectory {slot_name} verification FAILED:")
                for err in code_page_errors:
                    print(f"  - Code Page Error: {err}")
                for err in special_slot_errors:
                    print(f"  - Special Slot Error: {err}")

    if not all_cds_ok:
        raise ValueError(f"One or more CodeDirectories failed integrity verification in {bin_path}")

    # Primary CD for top-level summary
    primary_cd = cd_results[0]
    return {
        "cdhash_20": primary_cd["cdhash_20"],
        "cdhash_full": primary_cd["cdhash_full"],
        "hash_name": primary_cd["hash_name"],
        "hash_type": primary_cd["hash_type"],
        "hash_size": primary_cd["hash_size"],
        "n_code": primary_cd["n_code"],
        "code_limit": primary_cd["code_limit"],
        "page_size": primary_cd["page_size"],
        "n_special": primary_cd["n_special"],
        "has_ent_slot": 5 in slots_by_type,
        "cds": cd_results,
        "cd_count": len(cd_results),
        "all_cds_ok": all_cds_ok,
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


DEFAULT_REF_URL = "https://github.com/csjoyxy/jetpilot/releases/download/v0.7.7/JetPilot.tipa"
EXPECTED_REF_SIZE = 383565


def fetch_reference_tipa(ref_path: str | None, ref_url: str | None) -> str | None:
    if ref_path:
        if os.path.isfile(ref_path):
            return os.path.abspath(ref_path)
        print(f"[WARN] Specified reference tipa not found: {ref_path}")
        return None

    cache_path = os.path.join(tempfile.gettempdir(), "JetPilot_v0.7.7_ref.tipa")
    if os.path.isfile(cache_path) and os.path.getsize(cache_path) == EXPECTED_REF_SIZE:
        return cache_path

    url = ref_url or DEFAULT_REF_URL
    print(f"[INFO] Fetching v0.7.7 reference package from: {url}")
    curl_bin = find_tool("curl")
    if curl_bin:
        try:
            res = subprocess.run(
                [curl_bin, "-sSL", "-f", "-L", "--connect-timeout", "10", "-m", "60", url, "-o", cache_path],
                capture_output=True,
                text=True,
            )
            if res.returncode == 0 and os.path.isfile(cache_path) and os.path.getsize(cache_path) > 0:
                print(f"[OK] Downloaded v0.7.7 reference package ({os.path.getsize(cache_path)} bytes).")
                return cache_path
        except Exception as e:
            print(f"[WARN] curl failed: {e}")

    try:
        import urllib.request
        req = urllib.request.Request(url, headers={"User-Agent": "JetPilot-CI-Verifier"})
        with urllib.request.urlopen(req, timeout=15) as resp, open(cache_path, "wb") as out_f:
            out_f.write(resp.read())
        if os.path.isfile(cache_path) and os.path.getsize(cache_path) > 0:
            print(f"[OK] Downloaded v0.7.7 reference package via urllib ({os.path.getsize(cache_path)} bytes).")
            return cache_path
    except Exception as e:
        print(f"[WARN] urllib fallback failed: {e}")

    print(f"[WARN] Could not obtain v0.7.7 reference package; proceeding with current build verification.")
    return None


def extract_app_dir(tipa_path: str, extract_dir: str) -> str:
    with zipfile.ZipFile(tipa_path, "r") as z:
        z.extractall(extract_dir)
    app_dir = os.path.join(extract_dir, "Payload", "JetPilot.app")
    if not os.path.isdir(app_dir):
        cand = os.path.join(extract_dir, "JetPilot.app")
        if os.path.isdir(cand):
            app_dir = cand
        else:
            raise FileNotFoundError(f"Payload/JetPilot.app not found inside {tipa_path}")
    return app_dir


def print_comparison_table(ref_results: dict, cur_results: dict):
    print("================================================================================")
    print("=== Comparative Verification: v0.7.7 (Reference) vs Current Build ===")
    print("================================================================================")
    for bin_name in ["jetpilotd", "JetPilot"]:
        ref = ref_results.get(bin_name)
        cur = cur_results.get(bin_name)
        if not ref or not cur:
            continue
        print(f"\n--- Target: {bin_name} ---")
        row_fmt = "  {:<26} {:<24} {:<24}"
        print(row_fmt.format("Field", "v0.7.7 (Reference)", "Current Build"))
        print("  " + "-" * 74)
        print(row_fmt.format("File Size", f"{ref['size']} bytes", f"{cur['size']} bytes"))
        print(row_fmt.format("CPU Arch", f"{ref['cputype']}/{ref['cpusubtype']}", f"{cur['cputype']}/{cur['cpusubtype']}"))
        print(row_fmt.format("LC_BUILD_VERSION (minos)", f"{ref['minos']} (sdk {ref['sdk']})", f"{cur['minos']} (sdk {cur['sdk']})"))
        
        ref_n_code = ref['sig']['n_code'] if ref.get('sig') else "N/A"
        cur_n_code = cur['sig']['n_code'] if cur.get('sig') else "N/A"
        print(row_fmt.format("Code Pages", f"{ref_n_code} pages", f"{cur_n_code} pages"))

        ref_cds = [f"0x{cd['slot_type']:04x} ({cd['hash_name']})" for cd in ref['sig']['cds']] if ref.get('sig') else []
        cur_cds = [f"0x{cd['slot_type']:04x} ({cd['hash_name']})" for cd in cur['sig']['cds']] if cur.get('sig') else []
        print(row_fmt.format("CodeDirectories", ", ".join(ref_cds), ", ".join(cur_cds)))
        
        ref_cdhash = (ref['sig']['cdhash_20'][:16] + "...") if ref.get('sig') else "N/A"
        cur_cdhash = (cur['sig']['cdhash_20'][:16] + "...") if cur.get('sig') else "N/A"
        print(row_fmt.format("Primary CDHash", ref_cdhash, cur_cdhash))
        
        ref_ent = "Matched [OK]" if ref.get('ent_ok') else "Mismatch [FAIL]"
        cur_ent = "Matched [OK]" if cur.get('ent_ok') else "Mismatch [FAIL]"
        print(row_fmt.format("Entitlements vs Plist", ref_ent, cur_ent))
        
        if bin_name == "jetpilotd":
            ref_fb = "true [OK]" if ref.get('frontboard_ok') else "missing [FAIL]"
            cur_fb = "true [OK]" if cur.get('frontboard_ok') else "missing [FAIL]"
            print(row_fmt.format("FrontBoard Entitlement", ref_fb, cur_fb))
        
        ref_sig = "PASSED [OK]" if ref.get('sig_ok') else "FAILED [FAIL]"
        cur_sig = "PASSED [OK]" if cur.get('sig_ok') else "FAILED [FAIL]"
        print(row_fmt.format("Page Hash Integrity", ref_sig, cur_sig))
        
        print(row_fmt.format("codesign --verify", ref.get('codesign_verdict', 'N/A'), cur.get('codesign_verdict', 'N/A')))
    print("\n================================================================================\n")


def write_verification_status(
    status_path: str,
    tipa_path: str,
    is_verified: bool,
    failures: list[str],
    codesign_warnings: list[str],
    cur_results: dict | None,
):
    sha256 = ""
    size = 0
    if os.path.isfile(tipa_path):
        size = os.path.getsize(tipa_path)
        with open(tipa_path, "rb") as f:
            sha256 = hashlib.sha256(f.read()).hexdigest()

    status_str = "VERIFIED" if is_verified else "UNVERIFIED"
    lines = [
        f"STATUS: {status_str}",
        f"TIPA_PATH: {tipa_path}",
        f"TIPA_SIZE: {size}",
        f"TIPA_SHA256: {sha256}",
    ]
    if codesign_warnings:
        lines.append(f"CODESIGN_WARNINGS: {'; '.join(codesign_warnings)}")
    else:
        lines.append("CODESIGN_WARNINGS: NONE")

    if failures:
        lines.append("FAILURES:")
        for fail in failures:
            lines.append(f"  - {fail}")
    else:
        lines.append("FAILURES: NONE")

    if cur_results:
        for bname, binfo in cur_results.items():
            if binfo and binfo.get("sig"):
                cd_count = binfo["sig"].get("cd_count", 0)
                lines.append(f"BINARY_{bname.upper()}_CDS: {cd_count}")

    content = "\n".join(lines) + "\n"
    with open(status_path, "w", encoding="utf-8") as f:
        f.write(content)

    # Mirror to src/VERIFICATION_STATUS if in repo
    parent = os.path.dirname(os.path.abspath(tipa_path))
    repo_status_file = os.path.join(parent, "VERIFICATION_STATUS")
    try:
        with open(repo_status_file, "w", encoding="utf-8") as f:
            f.write(content)
    except Exception:
        pass


def verify_binary(bin_path: str, expected_plist_path: str, is_daemon: bool, quiet: bool = False) -> tuple[bool, dict]:
    bin_name = os.path.basename(bin_path)
    if not quiet:
        print(f"============================================================")
        print(f"=== Verifying Binary: {bin_name} ===")
        print(f"============================================================")

    errors = []
    info = {
        "bin_name": bin_name,
        "bin_path": bin_path,
        "size": 0,
        "cputype": "Unknown",
        "cpusubtype": "Unknown",
        "minos": "N/A",
        "sdk": "N/A",
        "sig": None,
        "ent_ok": False,
        "frontboard_ok": None,
        "sig_ok": False,
        "codesign_verdict": "SKIPPED",
        "overall_ok": False,
        "errors": errors,
    }

    if not os.path.isfile(bin_path):
        err = f"Binary not found at: {bin_path}"
        errors.append(err)
        if not quiet:
            print(f"[FAIL] {err}")
        return False, info
    if not os.path.isfile(expected_plist_path):
        err = f"Expected entitlements file not found: {expected_plist_path}"
        errors.append(err)
        if not quiet:
            print(f"[FAIL] {err}")
        return False, info

    with open(bin_path, "rb") as f:
        data = f.read()
    info["size"] = len(data)

    # 1. Mach-O Structure
    try:
        macho = parse_macho(data, bin_path)
        info["cputype"] = macho["cputype_str"]
        info["cpusubtype"] = macho["cpusubtype_str"]
        if macho.get("build_version"):
            info["minos"] = macho["build_version"].get("minos", "N/A")
            info["sdk"] = macho["build_version"].get("sdk", "N/A")
    except Exception as e:
        err = f"Failed parsing Mach-O structure: {e}"
        errors.append(err)
        if not quiet:
            print(f"[FAIL] {err}")
        return False, info

    # 2. Extract Embedded Entitlements via ldid -e or fallback to Mach-O SuperBlob slot 5
    ldid_bin = find_tool("ldid")
    embedded_raw = None
    embedded_ent = None
    if ldid_bin:
        try:
            proc = subprocess.run([ldid_bin, "-e", bin_path], capture_output=True, check=True)
            embedded_raw = proc.stdout
            embedded_ent = plistlib.loads(embedded_raw)
        except Exception as e:
            if not quiet:
                print(f"[WARN] ldid -e failed: {e}; falling back to direct extraction")

    if embedded_ent is None and macho.get("cs_offset"):
        try:
            cs_offset = macho["cs_offset"]
            if cs_offset + 12 <= len(data):
                cs_magic, cs_len, count = struct.unpack(">III", data[cs_offset : cs_offset + 12])
                if cs_magic == 0xFADE0CC0:
                    for i in range(count):
                        slot_type, slot_off = struct.unpack(">II", data[cs_offset + 12 + i * 8 : cs_offset + 20 + i * 8])
                        if slot_type == 5:
                            e_off = cs_offset + slot_off
                            emagic, elen = struct.unpack(">II", data[e_off : e_off + 8])
                            embedded_raw = data[e_off + 8 : e_off + elen]
                            embedded_ent = plistlib.loads(embedded_raw)
                            break
        except Exception as e:
            err = f"Direct entitlements extraction failed: {e}"
            errors.append(err)
            if not quiet:
                print(f"[FAIL] {err}")
            return False, info

    if embedded_ent is None:
        err = "Could not extract embedded entitlements (ldid missing and no CSSLOT_ENTITLEMENTS)"
        errors.append(err)
        if not quiet:
            print(f"[FAIL] {err}")
        return False, info

    # 3. Compare with Expected Entitlements
    try:
        with open(expected_plist_path, "rb") as f:
            expected_ent = plistlib.load(f)
    except Exception as e:
        err = f"Failed reading expected plist {expected_plist_path}: {e}"
        errors.append(err)
        if not quiet:
            print(f"[FAIL] {err}")
        return False, info

    diffs = compare_entitlements(embedded_ent, expected_ent, bin_name)
    if diffs:
        errors.extend(diffs)
        if not quiet:
            print(f"[FAIL] Embedded entitlements mismatch against {os.path.basename(expected_plist_path)}:")
            for d in diffs:
                print(f"  - {d}")
        return False, info
    info["ent_ok"] = True
    if not quiet:
        print(f"[OK] Embedded entitlements match {os.path.basename(expected_plist_path)} exactly.")

    # FrontBoard entitlement check for daemon
    if is_daemon:
        fb_key = "com.apple.frontboard.launchapplications"
        fb_val = (embedded_ent.get(fb_key) is True)
        info["frontboard_ok"] = fb_val
        if not fb_val:
            err = f"Daemon missing required entitlement: {fb_key}"
            errors.append(err)
            if not quiet:
                print(f"[FAIL] {err}")
            return False, info
        if not quiet:
            print(f"[OK] Daemon has required entitlement: {fb_key} == true")

    # 4. Code Signature & Page Hashes
    try:
        sig = verify_code_signature(data, macho["cs_offset"], macho["cs_size"], bin_path, quiet=quiet)
        info["sig"] = sig
        info["sig_ok"] = True
        if not quiet:
            print(
                f"[OK] Signature integrity: {sig['cd_count']} CodeDirectory structure(s) verified."
            )
    except Exception as e:
        err = f"Code signature verification failed: {e}"
        errors.append(err)
        if not quiet:
            print(f"[FAIL] {err}")
        return False, info

    # 5. External codesign --verify (if available) - NON-BLOCKING WARNING ONLY
    codesign_bin = find_tool("codesign")
    if codesign_bin:
        r_cs = subprocess.run(
            [codesign_bin, "--verify", "--verbose=2", bin_path],
            capture_output=True,
            text=True,
        )
        if r_cs.returncode != 0:
            cs_out = (r_cs.stderr or r_cs.stdout or "").strip()
            first_line = cs_out.splitlines()[0] if cs_out else f"exit code {r_cs.returncode}"
            info["codesign_verdict"] = f"WARN: {first_line}"
            if not quiet:
                print(f"[WARN] codesign --verify reported failure (non-blocking):\n{cs_out}")
        else:
            info["codesign_verdict"] = "PASSED"
            if not quiet:
                print("[OK] codesign --verify verification passed.")
    else:
        info["codesign_verdict"] = "SKIPPED (no codesign)"
        if not quiet:
            print("[INFO] codesign tool not found on this system; page hashes verified directly.")

    # 6. Detailed Diagnostics to CI Log
    if not quiet:
        print("--- Diagnostic Information ---")
        print(f"File Path:        {bin_path}")
        print(f"File Size:        {len(data)} bytes")
        print(f"Page Count:       {sig['n_code']} pages (page size {sig['page_size']} bytes, code limit {sig['code_limit']})")
        print(f"CPU Type/Subtype: {macho['cputype_str']} / {macho['cpusubtype_str']}")
        print(f"Hash Type:        {sig['hash_name']} (type={sig['hash_type']}, size={sig['hash_size']} bytes)")
        print(f"Candidate CDHash: {sig['cdhash_20']}")
        print(f"Full CDHash:      {sig['cdhash_full']}")
        print("Embedded Entitlements (ldid -e):")
        if embedded_raw:
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

    info["overall_ok"] = True
    return True, info


def verify_tipa_package(
    tipa_path: str, app_plist: str, daemon_plist: str, is_reference: bool = False
) -> tuple[bool, dict, list[str], list[str]]:
    label = "v0.7.7 Reference Package" if is_reference else "Current Package"
    if not is_reference:
        print(f"============================================================")
        print(f"=== Verifying {label}: {tipa_path} ===")
        print(f"============================================================")
    temp_dir = tempfile.mkdtemp(prefix="jp_pkg_")
    results = {}
    failures = []
    codesign_warnings = []
    try:
        app_dir = extract_app_dir(tipa_path, temp_dir)
        daemon_bin = os.path.join(app_dir, "jetpilotd")
        app_bin = os.path.join(app_dir, "JetPilot")

        daemon_ok, daemon_info = verify_binary(daemon_bin, daemon_plist, is_daemon=True, quiet=is_reference)
        app_ok, app_info = verify_binary(app_bin, app_plist, is_daemon=False, quiet=is_reference)
        results["jetpilotd"] = daemon_info
        results["JetPilot"] = app_info

        if not daemon_ok:
            for err in daemon_info.get("errors", ["daemon verification failed"]):
                failures.append(f"jetpilotd: {err}")
        if not app_ok:
            for err in app_info.get("errors", ["app verification failed"]):
                failures.append(f"JetPilot: {err}")

        if daemon_info.get("codesign_verdict", "").startswith("WARN:"):
            codesign_warnings.append(f"jetpilotd: {daemon_info['codesign_verdict']}")
        if app_info.get("codesign_verdict", "").startswith("WARN:"):
            codesign_warnings.append(f"JetPilot: {app_info['codesign_verdict']}")

        pkg_ok = daemon_ok and app_ok
        return pkg_ok, results, failures, codesign_warnings
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


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
    parser.add_argument(
        "--compare-tipa",
        default=None,
        help="Path to reference tipa for comparative verification (default: v0.7.7)",
    )
    parser.add_argument(
        "--compare-url",
        default=None,
        help="URL to fetch reference tipa from (default: GitHub Releases v0.7.7)",
    )
    parser.add_argument(
        "--no-compare",
        action="store_true",
        help="Skip comparison with reference package",
    )
    parser.add_argument(
        "--status-file",
        default=None,
        help="Path to write verification status file (default: <tipa_path>.verification)",
    )
    parser.add_argument(
        "--allow-unverified",
        action="store_true",
        help="Do not exit with failure code if artifact is UNVERIFIED; mark status and exit 0",
    )
    args = parser.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    tipa_path = args.tipa_opt or args.tipa_pos or os.path.join(repo_root, "src", "JetPilot.tipa")
    app_plist = args.app_plist or os.path.join(repo_root, "src", "entitlements-app.plist")
    daemon_plist = args.daemon_plist or os.path.join(repo_root, "src", "entitlements-daemon.plist")

    if not os.path.isfile(tipa_path):
        print(f"[FAIL] tipa file not found: {tipa_path}")
        sys.exit(1)

    cur_ok, cur_results, failures, codesign_warnings = verify_tipa_package(
        tipa_path, app_plist, daemon_plist, is_reference=False
    )

    # Reference package comparison (v0.7.7)
    if not args.no_compare:
        ref_path = fetch_reference_tipa(args.compare_tipa, args.compare_url)
        if ref_path and os.path.isfile(ref_path):
            try:
                ref_ok, ref_results, _, _ = verify_tipa_package(
                    ref_path, app_plist, daemon_plist, is_reference=True
                )
                print_comparison_table(ref_results, cur_results)
            except Exception as e:
                print(f"[WARN] Error during reference comparison: {e}")
        else:
            print("[INFO] Comparison skipped: v0.7.7 reference package could not be fetched.")

    # Write verification status file
    status_file = args.status_file or (tipa_path + ".verification")
    write_verification_status(
        status_file,
        tipa_path,
        cur_ok,
        failures,
        codesign_warnings,
        cur_results,
    )
    print(f"[INFO] Verification status written to: {status_file}")

    # Set GitHub Actions output if available
    gh_output = os.environ.get("GITHUB_OUTPUT")
    if gh_output:
        try:
            with open(gh_output, "a", encoding="utf-8") as f:
                f.write(f"package_status={'VERIFIED' if cur_ok else 'UNVERIFIED'}\n")
                f.write(f"is_verified={'true' if cur_ok else 'false'}\n")
        except Exception:
            pass

    if not cur_ok:
        if args.allow_unverified:
            print("============================================================")
            print("[WARN] Artifact verification completed with errors; package marked as UNVERIFIED (non-blocking).")
            print("============================================================")
            sys.exit(0)
        else:
            print("============================================================")
            print("[FAIL] Package verification FAILED (package marked as UNVERIFIED).")
            print("============================================================")
            sys.exit(2)

    print("============================================================")
    print("[SUCCESS] All package signatures and entitlements PASSED.")
    print("============================================================")
    sys.exit(0)


if __name__ == "__main__":
    main()
