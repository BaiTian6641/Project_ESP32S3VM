#!/usr/bin/env bash
# ============================================================================
# ESP32-S3 CI Soak Test — Concurrent Wi-Fi + BLE Stress (S8-T3)
#
# Runs the ESP32-S3 QEMU simulator with both Wi-Fi and BLE subsystems
# active under deterministic RNG mode for a configurable duration.
# Verifies:
#   1. No crash/hang within the timeout window
#   2. Coexistence slot counters advance (both Wi-Fi and BLE grants > 0)
#   3. Clean shutdown (exit code 0 from QEMU)
#
# Usage:
#   ./tests/ci_soak_test.sh [--duration SECONDS] [--seed SEED] [--firmware PATH]
#
# Environment variables (override defaults):
#   QEMU_BIN       — path to qemu-system-xtensa (default: qemu/build/qemu-system-xtensa)
#   SOAK_DURATION  — test duration in seconds (default: 1800 = 30 min)
#   RNG_SEED       — deterministic RNG seed (default: 42)
#   FIRMWARE       — path to ESP32-S3 firmware image to boot
#   COEX_BASE      — coex MMIO base (default: 0x600D2000)
#
# Exit codes:
#   0 — PASS: no crash, coex counters advanced
#   1 — FAIL: QEMU crashed or hung
#   2 — FAIL: coex counters did not advance (scheduling deadlock)
#   3 — SKIP: firmware not found or QEMU binary missing
# ============================================================================

set -euo pipefail

# ---- Defaults ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

QEMU_BIN="${QEMU_BIN:-$PROJECT_DIR/qemu/build/qemu-system-xtensa}"
SOAK_DURATION="${SOAK_DURATION:-1800}"
RNG_SEED="${RNG_SEED:-42}"
FIRMWARE="${FIRMWARE:-}"
COEX_BASE="${COEX_BASE:-0x600D2000}"
QMP_SOCK=""
LOG_FILE=""

# ---- Argument parsing ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) SOAK_DURATION="$2"; shift 2 ;;
        --seed)     RNG_SEED="$2"; shift 2 ;;
        --firmware) FIRMWARE="$2"; shift 2 ;;
        --help|-h)
            head -30 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown argument: $1"; exit 3 ;;
    esac
done

# ---- Validation ----
if [[ ! -x "$QEMU_BIN" ]]; then
    echo "SKIP: QEMU binary not found at $QEMU_BIN"
    exit 3
fi

if [[ -z "$FIRMWARE" || ! -f "$FIRMWARE" ]]; then
    echo "SKIP: No firmware image specified or file not found."
    echo "  Usage: $0 --firmware /path/to/esp32s3_app.bin"
    echo "  (Set FIRMWARE env var or use --firmware flag)"
    exit 3
fi

# ---- Temp files ----
cleanup() {
    local exit_code=$?
    [[ -n "${QEMU_PID:-}" ]] && kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    [[ -n "$QMP_SOCK" && -S "$QMP_SOCK" ]] && rm -f "$QMP_SOCK"
    [[ -n "$LOG_FILE" && -f "$LOG_FILE" ]] && rm -f "$LOG_FILE"
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

QMP_SOCK="$(mktemp -u /tmp/qemu-soak-qmp.XXXXXX).sock"
LOG_FILE="$(mktemp /tmp/qemu-soak-log.XXXXXX)"

# ---- Banner ----
echo "=========================================="
echo " ESP32-S3 CI Soak Test"
echo "=========================================="
echo " QEMU:      $QEMU_BIN"
echo " Firmware:  $FIRMWARE"
echo " Duration:  ${SOAK_DURATION}s"
echo " RNG seed:  $RNG_SEED"
echo " COEX base: $COEX_BASE"
echo " QMP sock:  $QMP_SOCK"
echo " Log file:  $LOG_FILE"
echo "=========================================="

# ---- Launch QEMU ----
"$QEMU_BIN" \
    -machine esp32s3 \
    -nographic \
    -no-reboot \
    -drive "file=$FIRMWARE,if=mtd,format=raw" \
    -global misc.esp32s3.rng.deterministic=true \
    -global "misc.esp32s3.rng.seed=$RNG_SEED" \
    -qmp "unix:$QMP_SOCK,server,nowait" \
    -serial file:"$LOG_FILE" \
    &
QEMU_PID=$!
echo "QEMU started (PID $QEMU_PID)"

# Wait for QMP socket
for i in $(seq 1 30); do
    [[ -S "$QMP_SOCK" ]] && break
    sleep 0.5
done

if [[ ! -S "$QMP_SOCK" ]]; then
    echo "FAIL: QMP socket did not appear within 15 seconds"
    exit 1
fi

# QMP handshake
qmp_cmd() {
    local payload="$1"
    python3 - "$QMP_SOCK" "$payload" <<'PY'
import json
import socket
import sys

sock_path = sys.argv[1]
payload = json.loads(sys.argv[2])

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
f = s.makefile('rwb', buffering=0)

# greeting
f.readline()

# capabilities
f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
while True:
    line = f.readline()
    if not line:
        print("{}")
        sys.exit(0)
    obj = json.loads(line.decode())
    if "return" in obj or "error" in obj:
        break

# command
f.write((json.dumps(payload) + "\n").encode())
while True:
    line = f.readline()
    if not line:
        print("{}")
        break
    obj = json.loads(line.decode())
    if "return" in obj or "error" in obj:
        print(json.dumps(obj))
        break
PY
}

qmp_read_word() {
    local addr="$1"
    local resp
    resp="$(qmp_cmd "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"xp /1wx $addr\"}}")"
    python3 - "$resp" <<'PY'
import json
import re
import sys

text = sys.argv[1]
try:
    obj = json.loads(text)
    out = obj.get("return", "")
except Exception:
    out = ""

m = re.search(r'0x([0-9a-fA-F]+)', out)
print(str(int(m.group(1), 16)) if m else "0")
PY
}

echo "Soak running for ${SOAK_DURATION}s..."

# ---- Soak loop ----
ELAPSED=0
CHECK_INTERVAL=30

while [[ $ELAPSED -lt $SOAK_DURATION ]]; do
    sleep "$CHECK_INTERVAL"
    ELAPSED=$((ELAPSED + CHECK_INTERVAL))

    # Check QEMU is still running
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "FAIL: QEMU exited prematurely at ${ELAPSED}s"
        wait "$QEMU_PID" 2>/dev/null
        QEMU_EXIT=$?
        echo "  Exit code: $QEMU_EXIT"
        echo "  Last 20 lines of log:"
        tail -20 "$LOG_FILE" 2>/dev/null || true
        exit 1
    fi

    # Progress
    printf "  [%4d/%4ds] QEMU alive (PID %d)\n" "$ELAPSED" "$SOAK_DURATION" "$QEMU_PID"
done

echo "Soak duration completed. Checking coex counters..."

# ---- Read coexistence grant counters via QMP ----
COEX_WIFI_ADDR=$(printf "0x%X" $((COEX_BASE + 0x0C)))
COEX_BLE_ADDR=$(printf "0x%X" $((COEX_BASE + 0x10)))

WIFI_GRANTS="$(qmp_read_word "$COEX_WIFI_ADDR")"
BLE_GRANTS="$(qmp_read_word "$COEX_BLE_ADDR")"

echo " Coex grants: WIFI=$WIFI_GRANTS BLE=$BLE_GRANTS"

# ---- Shutdown ----
echo "Shutting down QEMU..."
qmp_cmd '{"execute":"quit"}' > /dev/null 2>&1 || true

# Give QEMU time to exit
for i in $(seq 1 10); do
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 1
done

# Force kill if still running
if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "Warning: QEMU did not exit cleanly, sending SIGKILL"
    kill -9 "$QEMU_PID" 2>/dev/null || true
fi

wait "$QEMU_PID" 2>/dev/null
QEMU_EXIT=$?

# ---- Result ----
echo ""
echo "=========================================="
echo " Soak Test Results"
echo "=========================================="
echo " Duration:   ${SOAK_DURATION}s completed"
echo " RNG seed:   $RNG_SEED"
echo " QEMU exit:  $QEMU_EXIT"
echo " Log size:   $(wc -c < "$LOG_FILE" 2>/dev/null || echo 0) bytes"
echo " WIFI grant: $WIFI_GRANTS"
echo " BLE grant:  $BLE_GRANTS"
echo "=========================================="

# Check for crash indicators in log
if grep -qi "panic\|abort\|assertion.*failed\|fatal\|segfault" "$LOG_FILE" 2>/dev/null; then
    echo "FAIL: Crash indicators found in log output"
    echo "  Matching lines:"
    grep -i "panic\|abort\|assertion.*failed\|fatal\|segfault" "$LOG_FILE" | head -5
    exit 1
fi

if [[ "$WIFI_GRANTS" -le 0 || "$BLE_GRANTS" -le 0 ]]; then
    echo "FAIL: Coexistence counters did not advance (WIFI=$WIFI_GRANTS, BLE=$BLE_GRANTS)"
    exit 2
fi

echo ""
echo "PASS: No crash/hang and coex counters advanced during ${SOAK_DURATION}s soak test"
exit 0
