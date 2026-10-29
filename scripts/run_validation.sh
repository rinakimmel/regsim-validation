#!/usr/bin/env bash
# run_validation.sh — Build, start the server, run all tests, stop the server.
#
# Usage:
#   ./scripts/run_validation.sh [--port PORT] [--asan] [--keep-server]
#
# Options:
#   --port PORT      Port for the server and validation suite (default: 5555)
#   --asan           Use the asan build (./build/asan/) instead of release
#   --keep-server    Do not stop the server after tests finish
#
# Exit code:
#   0  all tests passed
#   1  one or more tests failed, or build/server failure

set -euo pipefail

# -------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------
PORT=5555
USE_ASAN=0
KEEP_SERVER=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# -------------------------------------------------------------------------
# Parse arguments
# -------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)       PORT="$2";   shift 2 ;;
        --asan)       USE_ASAN=1;  shift   ;;
        --keep-server) KEEP_SERVER=1; shift ;;
        --help|-h)
            echo "Usage: $0 [--port PORT] [--asan] [--keep-server]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# -------------------------------------------------------------------------
# Build
# -------------------------------------------------------------------------
echo "================================================================"
echo "  regsim validation runner"
echo "  Port: ${PORT}"
echo "  ASan: ${USE_ASAN}"
echo "================================================================"
echo ""
echo "[1/4] Building..."

cd "${ROOT}"

if [[ ${USE_ASAN} -eq 1 ]]; then
    make asan 2>&1
    SERVER_BIN="${ROOT}/build/asan/regsim-server"
    VALIDATE_BIN="${ROOT}/build/asan/validate"
    TEST_CODEC_BIN="${ROOT}/build/asan/test_codec"
    TEST_REGFILE_BIN="${ROOT}/build/asan/test_regfile"
else
    make all 2>&1
    SERVER_BIN="${ROOT}/build/release/regsim-server"
    VALIDATE_BIN="${ROOT}/build/release/validate"
    TEST_CODEC_BIN="${ROOT}/build/release/test_codec"
    TEST_REGFILE_BIN="${ROOT}/build/release/test_regfile"
fi

echo "[1/4] Build OK"

# -------------------------------------------------------------------------
# Run unit tests (no server required)
# -------------------------------------------------------------------------
echo ""
echo "[2/4] Running unit tests..."

UNIT_RC=0
echo "--- test_codec ---"
"${TEST_CODEC_BIN}"   || UNIT_RC=1
echo ""
echo "--- test_regfile ---"
"${TEST_REGFILE_BIN}" || UNIT_RC=1

if [[ ${UNIT_RC} -ne 0 ]]; then
    echo ""
    echo "ERROR: Unit tests failed. Aborting validation." >&2
    exit 1
fi
echo "[2/4] Unit tests passed"

# -------------------------------------------------------------------------
# Start the server
# -------------------------------------------------------------------------
echo ""
echo "[3/4] Starting server on port ${PORT}..."

"${SERVER_BIN}" --port "${PORT}" --log-level debug &
SERVER_PID=$!

# Ensure the server is killed on exit (unless --keep-server)
cleanup() {
    if [[ ${KEEP_SERVER} -eq 0 ]]; then
        if kill -0 "${SERVER_PID}" 2>/dev/null; then
            echo ""
            echo "Stopping server (PID ${SERVER_PID})..."
            kill "${SERVER_PID}" 2>/dev/null || true
            wait "${SERVER_PID}" 2>/dev/null || true
        fi
    else
        echo "Server (PID ${SERVER_PID}) left running on port ${PORT}."
    fi
}
trap cleanup EXIT

# Wait up to 3 seconds for the server to be ready
READY=0
for i in $(seq 1 30); do
    sleep 0.1
    if nc -z 127.0.0.1 "${PORT}" 2>/dev/null; then
        READY=1
        break
    fi
done

if [[ ${READY} -eq 0 ]]; then
    echo "ERROR: Server did not start within 3 seconds." >&2
    exit 1
fi
echo "[3/4] Server is up (PID ${SERVER_PID})"

# -------------------------------------------------------------------------
# Run integration validation
# -------------------------------------------------------------------------
echo ""
echo "[4/4] Running integration validation suite..."
echo ""

VALIDATE_RC=0
"${VALIDATE_BIN}" --host 127.0.0.1 --port "${PORT}" || VALIDATE_RC=$?

echo ""
echo "================================================================"
if [[ ${VALIDATE_RC} -eq 0 ]]; then
    echo "  ALL TESTS PASSED"
else
    echo "  SOME TESTS FAILED (exit code ${VALIDATE_RC})"
fi
echo "================================================================"

exit ${VALIDATE_RC}
