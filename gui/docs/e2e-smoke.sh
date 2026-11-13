#!/usr/bin/env bash
# Reproducible end-to-end smoke test for the Go GUI against the fake CLI.
# Usage: bash docs/e2e-smoke.sh [port]
set -euo pipefail
PORT=${1:-18080}
cd "$(dirname "$0")/.."

TMP="$(mktemp -d)"
mkdir -p "$TMP"
cleanup() {
  if [ -n "${SRV:-}" ]; then
    kill "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
  fi
  sleep 0.3
  rm -rf "$TMP" 2>/dev/null || true
}
trap cleanup EXIT

go build -o "$TMP/gui.exe" ./cmd/autoremesher-gui
go build -o "$TMP/fakecli.exe" ./internal/testdata/fakecli

printf 'v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\nv -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n' > "$TMP/cube.obj"

FAKE_DELAY_MS=120 "$TMP/gui.exe" --no-open --port "$PORT" --cli "$TMP/fakecli.exe" > "$TMP/server.log" 2>&1 &
SRV=$!
sleep 2

URL=$(sed -n 's/^  url   //p' "$TMP/server.log")
BASE="${URL%/s/*}"
TOKEN=$(printf '%s' "$URL" | sed -n 's|.*/s/\([0-9a-f]*\)/.*|\1|p')
echo "server: $BASE token: $TOKEN"

echo "-- token enforcement"
test "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/info")" = "401" && echo "  api/info without token -> 401 ok"
test "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/s/deadbeef/")" = "404" && echo "  bad static token -> 404 ok"

echo "-- info"
curl -s -H "X-AR-Token: $TOKEN" "$BASE/api/info"; echo

echo "-- upload"
ID=$(curl -s -H "X-AR-Token: $TOKEN" -F "file=@$TMP/cube.obj" "$BASE/api/upload" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
echo "  inputId=$ID"

echo "-- run"
JOB=$(curl -s -H "X-AR-Token: $TOKEN" -H 'Content-Type: application/json' -d "{\"inputId\":\"$ID\",\"targetQuads\":500}" "$BASE/api/run" | sed -n 's/.*"jobId":"\([^"]*\)".*/\1/p')
echo "  jobId=$JOB"

echo "-- conflict while running"
echo -n "  second run -> "; curl -s -o /dev/null -w '%{http_code}\n' -H "X-AR-Token: $TOKEN" -H 'Content-Type: application/json' -d "{\"inputId\":\"$ID\"}" "$BASE/api/run"

echo "-- sse"
curl -sN --max-time 30 "$BASE/api/events?job=$JOB&token=$TOKEN" | grep -E '"type":"(result|end)"' || true

echo "-- model"
curl -s -o "$TMP/out.obj" -w '  http=%{http_code} bytes=%{size_download}\n' "$BASE/api/model?job=$JOB&token=$TOKEN"
echo -n "  quad faces: "; grep -c '^f ' "$TMP/out.obj" || true

echo "-- ui"
curl -s -o /dev/null -w '  index=%{http_code}\n' "$BASE/s/$TOKEN/"
curl -s -o /dev/null -w '  app.js=%{http_code}\n' "$BASE/s/$TOKEN/app.js"
echo "done"
