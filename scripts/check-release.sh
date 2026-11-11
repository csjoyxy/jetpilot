#!/usr/bin/env bash
set -u

# scripts/check-release.sh: Release verification script for JetPilot
#
# Requirements:
# 1. Info.plist CFBundleShortVersionString == jetpilotd.c JP_VERSION; CFBundleVersion is positive integer.
# 2. Source code (src/) must not contain: restart_spike, SBSLaunch, springboard.launchapplications, debug_fake, __ActivateSuspended.
# 3. entitlements-daemon.plist contains com.apple.frontboard.launchapplications.
# 4. Print count of JPLOG lines for each level (ERR/WARN/INFO/DEBUG) - diagnostic only.
#
# Returns exit 1 on any check (1, 2, 3) failure; 0 if all pass.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FAILED=0

echo "=== JetPilot Release Gate Check ==="

# -----------------------------------------------------------------------------
# Check 1: Version consistency & CFBundleVersion
# -----------------------------------------------------------------------------
echo "--- Check 1: Version Consistency & Build Number ---"
INFO_PLIST="$REPO_ROOT/src/app/Info.plist"
JETPILOTD_C="$REPO_ROOT/src/jetpilotd.c"

if [ ! -f "$INFO_PLIST" ]; then
    echo "[FAIL] Missing Info.plist at $INFO_PLIST"
    FAILED=1
elif [ ! -f "$JETPILOTD_C" ]; then
    echo "[FAIL] Missing jetpilotd.c at $JETPILOTD_C"
    FAILED=1
else
    # Extract CFBundleShortVersionString and CFBundleVersion
    if command -v python3 >/dev/null 2>&1; then
        short_ver=$(python3 -c "import plistlib; p=plistlib.load(open('$INFO_PLIST', 'rb')); print(p.get('CFBundleShortVersionString', ''))")
        bundle_ver=$(python3 -c "import plistlib; p=plistlib.load(open('$INFO_PLIST', 'rb')); print(p.get('CFBundleVersion', ''))")
    else
        short_ver=$(sed -n 's/.*<key>CFBundleShortVersionString<\/key><string>\([^<]*\)<\/string>.*/\1/p' "$INFO_PLIST")
        bundle_ver=$(sed -n 's/.*<key>CFBundleVersion<\/key><string>\([^<]*\)<\/string>.*/\1/p' "$INFO_PLIST")
    fi

    # Extract JP_VERSION from jetpilotd.c
    jp_ver=$(sed -n 's/.*#define[[:space:]]*JP_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$JETPILOTD_C")

    echo "Info.plist CFBundleShortVersionString: $short_ver"
    echo "Info.plist CFBundleVersion:            $bundle_ver"
    echo "jetpilotd.c JP_VERSION:                $jp_ver"

    # Compare versions
    if [ -z "$short_ver" ] || [ -z "$jp_ver" ] || [ "$short_ver" != "$jp_ver" ]; then
        echo "[FAIL] Version mismatch: Info.plist ('$short_ver') != jetpilotd.c ('$jp_ver')"
        FAILED=1
    else
        echo "[OK] Version strings match: $short_ver"
    fi

    # Check CFBundleVersion is a positive integer
    if [[ "$bundle_ver" =~ ^[1-9][0-9]*$ ]]; then
        echo "[OK] CFBundleVersion is a positive integer: $bundle_ver"
    else
        echo "[FAIL] CFBundleVersion is not a positive integer: '$bundle_ver'"
        FAILED=1
    fi
fi

# -----------------------------------------------------------------------------
# Check 2: Forbidden identifiers in source code
# -----------------------------------------------------------------------------
echo "--- Check 2: Forbidden Strings in src/ ---"
FORBIDDEN_PATTERN="restart_spike|SBSLaunch|springboard\.launchapplications|debug_fake|__ActivateSuspended"
matches=$(grep -rnE "$FORBIDDEN_PATTERN" "$REPO_ROOT/src" 2>/dev/null || true)

if [ -n "$matches" ]; then
    echo "[FAIL] Forbidden string(s) detected in src/:"
    echo "$matches"
    FAILED=1
else
    echo "[OK] No forbidden strings found in src/"
fi

# -----------------------------------------------------------------------------
# Check 3: FrontBoard entitlement in entitlements-daemon.plist
# -----------------------------------------------------------------------------
echo "--- Check 3: FrontBoard Entitlement ---"
ENTITLEMENTS_FILE="$REPO_ROOT/src/entitlements-daemon.plist"

if [ ! -f "$ENTITLEMENTS_FILE" ]; then
    echo "[FAIL] Missing entitlements file at $ENTITLEMENTS_FILE"
    FAILED=1
elif grep -q "com.apple.frontboard.launchapplications" "$ENTITLEMENTS_FILE" 2>/dev/null; then
    echo "[OK] entitlements-daemon.plist contains com.apple.frontboard.launchapplications"
else
    echo "[FAIL] entitlements-daemon.plist missing com.apple.frontboard.launchapplications"
    FAILED=1
fi

# -----------------------------------------------------------------------------
# Check 4: JPLOG counts per level (diagnostic only)
# -----------------------------------------------------------------------------
echo "--- Check 4: JPLOG Counts per Level ---"
if [ -f "$JETPILOTD_C" ]; then
    cnt_err=$(grep -cE 'JPLOG\s*\(\s*JP_ERR\b' "$JETPILOTD_C" 2>/dev/null || echo 0)
    cnt_warn=$(grep -cE 'JPLOG\s*\(\s*JP_WARN\b' "$JETPILOTD_C" 2>/dev/null || echo 0)
    cnt_info=$(grep -cE 'JPLOG\s*\(\s*JP_INFO\b' "$JETPILOTD_C" 2>/dev/null || echo 0)
    cnt_debug=$(grep -cE 'JPLOG\s*\(\s*JP_DEBUG\b' "$JETPILOTD_C" 2>/dev/null || echo 0)

    echo "JPLOG(ERR):   $cnt_err"
    echo "JPLOG(WARN):  $cnt_warn"
    echo "JPLOG(INFO):  $cnt_info"
    echo "JPLOG(DEBUG): $cnt_debug"
else
    echo "Cannot count JPLOG: $JETPILOTD_C missing"
fi

# -----------------------------------------------------------------------------
# Final Gate Result
# -----------------------------------------------------------------------------
echo "==================================="
if [ "$FAILED" -ne 0 ]; then
    echo "Result: Release check FAILED"
    exit 1
else
    echo "Result: Release check PASSED"
    exit 0
fi
