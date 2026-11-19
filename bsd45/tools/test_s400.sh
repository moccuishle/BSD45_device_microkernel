#!/usr/bin/env bash
# test_s400.sh - S400 microkernel fault isolation: user-space fs_server crash
#                 returns EIO (no kernel TRAP) and the service can be restarted.
set -uo pipefail
ws="$(cd "$(dirname "$0")/.." && pwd)"
iso="${1:-$ws/out/BSD45-ELF-x86.iso}"
[ -f "$iso" ] || { echo "ISO not found: $iso" >&2; exit 1; }
fails=0

check() {
    local name="$1" log="$2" n
    shift 2
    for n in "$@"; do
        if printf '%s' "$log" | grep -qF -- "$n"; then echo "  ok   $name: found $n"
        else echo "  FAIL $name: missing $n" >&2; fails=$((fails + 1)); fi
    done
}

echo "S400 acceptance: user-space service + fault isolation"
log="$("$ws/tools/run_qemu_test.sh" -Iso "$iso" \
    -Keys "shell|/bin/srvtest|exit 0" -WaitFor "user exit" -TimeoutSec 90)"

check "roundtrip" "$log" "srv: roundtrip ok"
check "eio"       "$log" "srv: fault isolation ok (EIO)"
check "restart"   "$log" "srv: restart ok"
check "verdict"   "$log" "srv: PASS"

# 关键负向断言：内核绝不能 TRAP
if printf '%s' "$log" | grep -qiE 'kernel TRAP|panic|triple fault'; then
    echo "  FAIL kernel trapped on service crash" >&2
    fails=$((fails + 1))
else
    echo "  ok   no kernel trap on service crash"
fi

if [ "$fails" -ne 0 ]; then echo "S400 fault isolation: FAILED ($fails)" >&2; exit 1; fi
echo "S400 fault isolation: PASSED"
exit 0
