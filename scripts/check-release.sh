#!/usr/bin/env bash
# P0-2: -e 逐处审计过（python3 版本提取、verify-artifacts 调用、JPLOG 计数均已加保护），见各处注释
set -euo pipefail

# scripts/check-release.sh: Release verification script for JetPilot
#
# Requirements:
# 1. Info.plist CFBundleShortVersionString == jetpilotd.c JP_VERSION; CFBundleVersion is positive integer.
# 2. Source code (src/) must not contain: restart_spike, SBSLaunch, springboard.launchapplications, debug_fake, __ActivateSuspended.
# 3. entitlements-daemon.plist contains com.apple.frontboard.launchapplications, and when artifacts are built,
#    verifies embedded entitlements, code signatures, and page hashes of the packaged binaries.
# 4. Print count of JPLOG lines for each level (ERR/WARN/INFO/DEBUG) - diagnostic only.
# (Note: E1 预审 L7 / 复审 L5 确认已由 382763c 覆盖及环境适配)
#
# Returns exit 1 on any check failure; 0 if all pass.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FAILED=0
ARTIFACT_PATH=""
SKIP_ARTIFACT=0

# Parse options
# 预审 L8: 遇到未知参数直接报 [FAIL] unknown argument 并退出，只认显式选项
while [ $# -gt 0 ]; do
    case "$1" in
        --artifact|-a)
            if [ $# -gt 1 ]; then
                ARTIFACT_PATH="$2"
                shift 2
            else
                echo "[FAIL] --artifact requires a file path"
                exit 1
            fi
            ;;
        --skip-artifact)
            SKIP_ARTIFACT=1
            shift
            ;;
        *)
            echo "[FAIL] unknown argument: $1"
            exit 1
            ;;
    esac
done

# If no explicit artifact provided and not skipped, auto-detect built tipa in src/
# 预审 L9: 若本地自动探测到 src/JetPilot.tipa 但比源文件旧，打 [WARN] stale artifact, skipped 并跳过
if [ -z "$ARTIFACT_PATH" ] && [ "$SKIP_ARTIFACT" -eq 0 ] && [ -f "$REPO_ROOT/src/JetPilot.tipa" ]; then
    if [ "$REPO_ROOT/src/JetPilot.tipa" -ot "$REPO_ROOT/src/jetpilotd.c" ] || \
       [ "$REPO_ROOT/src/JetPilot.tipa" -ot "$REPO_ROOT/src/app/main.m" ]; then
        echo "[WARN] stale artifact, skipped: src/JetPilot.tipa is older than source files"
    else
        ARTIFACT_PATH="$REPO_ROOT/src/JetPilot.tipa"
    fi
fi

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
        # P0-2: set -e 保护。python3 解析失败（损坏 plist 等）时置空，走下方 [FAIL] Version mismatch 的显式报错路径
        short_ver=$(python3 -c "import plistlib; p=plistlib.load(open('$INFO_PLIST', 'rb')); print(p.get('CFBundleShortVersionString', ''))") || short_ver=""
        bundle_ver=$(python3 -c "import plistlib; p=plistlib.load(open('$INFO_PLIST', 'rb')); print(p.get('CFBundleVersion', ''))") || bundle_ver=""
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
# Check 3: FrontBoard entitlement in entitlements-daemon.plist and built artifacts
# -----------------------------------------------------------------------------
echo "--- Check 3: FrontBoard Entitlement & Artifact Entitlements ---"
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

if [ -n "$ARTIFACT_PATH" ]; then
    echo "--- Check 3b: Artifact Entitlements & Signature Verification ($ARTIFACT_PATH) ---"
    if [ ! -f "$ARTIFACT_PATH" ]; then
        echo "[FAIL] Specified artifact not found: $ARTIFACT_PATH"
        FAILED=1
    elif [ ! -f "$SCRIPT_DIR/verify-artifacts.py" ]; then
        echo "[FAIL] Missing verification script at $SCRIPT_DIR/verify-artifacts.py"
        FAILED=1
    else
        # P0-1: --allow-unverified 已移除。UNVERIFIED 由 verify-artifacts.py 以 exit 2 报出，属硬失败，
        # 不再存在「带 UNVERIFIED 状态文件仍 exit 0」的路径，据此删除下方 WARN 分支与结尾 PASSED(UNVERIFIED) 特殊文案。
        # P0-2: set -e 下先落变量再取退出码，避免 python3 非零退出导致脚本中途裸退。
        ver_rc=0
        python3 "$SCRIPT_DIR/verify-artifacts.py" --tipa "$ARTIFACT_PATH" || ver_rc=$?
        if [ $ver_rc -ne 0 ]; then
            echo "[FAIL] Verification script execution failed (exit $ver_rc) for $ARTIFACT_PATH"
            FAILED=1
        else
            echo "[OK] Artifact entitlements and signature verification passed for $ARTIFACT_PATH"
        fi
    fi
else
    echo "[INFO] No built artifact provided or found; skipping artifact verification."
fi

# -----------------------------------------------------------------------------
# Check 4: JPLOG counts per level (diagnostic only)
# -----------------------------------------------------------------------------
# F1 修复 (P0-2)：grep -c 零匹配时 exit 1 且已输出 "0"，旧写法 `|| echo 0` 会再补一行，变量成两行 "0"。
# jplog_count：零匹配干净返回 0；grep 真实错误（exit>=2）显式报错并置 FAILED（诊断项，不中断后续检查）。
jplog_count() {
    local pattern="$1" file="$2" out rc
    rc=0
    out=$(grep -cE "$pattern" "$file" 2>/dev/null) || rc=$?
    if [ "$rc" -le 1 ]; then
        printf '%s\n' "${out:-0}"
    else
        echo "[FAIL] JPLOG count grep error (exit $rc) for pattern: $pattern" >&2
        FAILED=1
        printf 'ERR\n'
    fi
}

echo "--- Check 4: JPLOG Counts per Level ---"
if [ -f "$JETPILOTD_C" ]; then
    cnt_err=$(jplog_count 'JPLOG\s*\(\s*JP_ERR\b' "$JETPILOTD_C")
    cnt_warn=$(jplog_count 'JPLOG\s*\(\s*JP_WARN\b' "$JETPILOTD_C")
    cnt_info=$(jplog_count 'JPLOG\s*\(\s*JP_INFO\b' "$JETPILOTD_C")
    cnt_debug=$(jplog_count 'JPLOG\s*\(\s*JP_DEBUG\b' "$JETPILOTD_C")

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
    # P0-1: UNVERIFIED 现在必然走 verify-artifacts.py exit 2 → 上面的 FAILED 分支，此处只剩纯 PASSED
    echo "Result: Release check PASSED"
    exit 0
fi
