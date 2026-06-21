#!/usr/bin/env bash
#
# Smoke tests for MultiThreadHttpServer.
# Builds the server, starts it on a test port, exercises every endpoint and
# the error paths, then shuts it down. Exits non-zero if any check fails.
#
# Usage: ./test.sh

set -u

PORT=8089
BASE="http://localhost:$PORT"
PASS=0
FAIL=0

# --- assertions ------------------------------------------------------------

# check_status <description> <expected_code> <curl args...>
check_status() {
    local desc="$1" expected="$2"
    shift 2
    local code
    code=$(curl -s -o /dev/null -w "%{http_code}" "$@")
    if [ "$code" = "$expected" ]; then
        echo "  PASS: $desc (got $code)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected $expected, got $code)"
        FAIL=$((FAIL + 1))
    fi
}

# check_body <description> <substring> <curl args...>
check_body() {
    local desc="$1" needle="$2"
    shift 2
    local out
    out=$(curl -s "$@")
    if printf '%s' "$out" | grep -q -- "$needle"; then
        echo "  PASS: $desc (body contains '$needle')"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (body '$out' missing '$needle')"
        FAIL=$((FAIL + 1))
    fi
}

# check_valid_json <description> <curl args...>
check_valid_json() {
    local desc="$1"
    shift
    local out
    out=$(curl -s "$@")
    if printf '%s' "$out" | python3 -m json.tool >/dev/null 2>&1; then
        echo "  PASS: $desc (valid JSON)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (invalid JSON: '$out')"
        FAIL=$((FAIL + 1))
    fi
}

# --- build & launch --------------------------------------------------------

echo "Building..."
if ! make >/dev/null; then
    echo "Build failed."
    exit 1
fi

echo "Starting server on port $PORT..."
./server "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!

# make sure we always clean up the server, even if a check aborts
cleanup() {
    kill -INT "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
}
trap cleanup EXIT

# wait for the server to start accepting connections
for _ in $(seq 1 20); do
    if curl -s -o /dev/null "$BASE/api/health"; then
        break
    fi
    sleep 0.2
done

# --- tests -----------------------------------------------------------------

echo
echo "JSON API:"
check_body        "GET /api/health"        '"status":"ok"'  "$BASE/api/health"
check_valid_json  "GET /api/time is JSON"                   "$BASE/api/time"
check_valid_json  "POST /api/echo is JSON"                  -X POST "$BASE/api/echo" -d 'hello'
# special characters in the body must still produce valid JSON (escaping)
check_valid_json  "POST /api/echo escapes quotes"           -X POST "$BASE/api/echo" --data-binary 'he"llo'

echo
echo "Static files:"
check_status      "GET / serves index.html"   200  "$BASE/"
check_status      "GET /style.css"            200  "$BASE/style.css"

echo
echo "Routing & errors:"
check_status      "query string still routes" 200  "$BASE/api/health?x=1"
check_status      "unknown path is 404"       404  "$BASE/nope"
check_status      "path traversal blocked"    400  --path-as-is "$BASE/../etc/passwd"
check_status      "wrong method is 405"       405  -X POST "$BASE/api/health"

# --- summary ---------------------------------------------------------------

echo
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
