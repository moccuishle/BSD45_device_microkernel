#!/usr/bin/env bash
# test_device.sh - S401 unified device model: user-space devfs_server with a
#                  uniform device registry; service exit returns EIO (no kernel
#                  TRAP) and the service can be restarted.
# test_device.sh - S401 统一设备模型：用户态 devfs_server 统一注册表；服务退出
#                  返回 EIO（内核不 TRAP）且服务可重启。
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

echo "S401 acceptance: unified device model + fault isolation"
log="$("$ws/tools/run_qemu_test.sh" -Iso "$iso" \
    -Keys "shell|/bin/devtest|exit 0" -WaitFor "user exit" -TimeoutSec 90)"

check "registry"  "$log" "dev: registry ["
check "ops"       "$log" "dev: uniform ops ok"
check "eio"       "$log" "dev: fault isolation ok (EIO)"
check "restart"   "$log" "dev: restart ok"
check "verdict"   "$log" "dev: PASS"

# 关键负向断言：内核绝不能 TRAP
if printf '%s' "$log" | grep -qiE 'kernel TRAP|panic|triple fault'; then
    echo "  FAIL kernel trapped on device service crash" >&2
    fails=$((fails + 1))
else
    echo "  ok   no kernel trap on device service crash"
fi

if [ "$fails" -ne 0 ]; then echo "S401 device model: FAILED ($fails)" >&2; exit 1; fi
echo "S401 device model: PASSED"
exit 0
