#!/bin/bash
# tests/integration_test.sh
# Full integration test for Redis Dashboard + Mongo Telemetry servers

set -euo pipefail

echo "=== Maze Redis + Mongo Integration Test ==="

# ====================== CONFIGURATION ======================
REDIS_PORT=8447
MONGO_HTTP_PORT=8446

MONGO_URI="${MONGO_URI:-mongodb://localhost:27017}"

MONGO_HOST="localhost"
MONGO_API_URL="https://${MONGO_HOST}:${MONGO_HTTP_PORT}/api/moves"

CERT_DIR="../https/certs"
CLIENT_CERT="$CERT_DIR/client.crt"
CLIENT_KEY="$CERT_DIR/client.key"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

log() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }

check_prerequisites() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || echo "Warning: jq not installed (output will be raw JSON)"

    [ -f "$CLIENT_CERT" ] && [ -f "$CLIENT_KEY" ] || fail "Client certificates not found in $CERT_DIR"

    if ! redis-cli ping >/dev/null 2>&1; then
        fail "Redis is not running. Start with: redis-server --daemonize yes"
    fi
    log "Redis is running"
}

start_servers() {
    echo "Starting Mongo Telemetry server (port ${MONGO_HTTP_PORT}, DB: ${MONGO_URI})..."
    pushd ../https >/dev/null
    MONGO_PORT=${MONGO_HTTP_PORT} MONGO_URI=${MONGO_URI} NON_INTERACTIVE=1 ./maze_https_mongo &
    MONGO_PID=$!
    popd >/dev/null
    sleep 2

    echo "Starting Redis Dashboard server on port ${REDIS_PORT}..."
    export MONGO_MOVE_API="https://localhost:${MONGO_HTTP_PORT}/api/moves"
    pushd ../https >/dev/null
    NON_INTERACTIVE=1 ./maze_https_redis &
    REDIS_PID=$!
    popd >/dev/null
    sleep 3

    log "Both servers started in non-interactive mode"
}

send_test_moves() {
    echo "Sending 6 test telemetry moves..."
    for i in {1..6}; do
        curl -k -s -X POST "https://localhost:${MONGO_HTTP_PORT}/move" \
            --cert "$CLIENT_CERT" \
            --key "$CLIENT_KEY" \
            -H "Content-Type: application/json" \
            -d "{
                \"event_type\": \"player_move\",
                \"input\": {\"device\": \"keyboard\", \"move_sequence\": $i},
                \"player\": {\"position\": {\"x\": $((10 + i)), \"y\": $((15 + i))}},
                \"goal_reached\": $([ $i -eq 6 ] && echo true || echo false),
                \"timestamp\": \"2026-04-13T12:0${i}:00Z\"
            }" >/dev/null
        echo "  → Sent move #$i"
        sleep 0.25
    done
    log "Test moves sent successfully"
}

verify_mongo_direct() {
    echo "Verifying MongoDB telemetry endpoint..."
    RESPONSE=$(curl -k -s --cert "$CLIENT_CERT" --key "$CLIENT_KEY" "$MONGO_API_URL")
    if command -v jq >/dev/null; then
        COUNT=$(echo "$RESPONSE" | jq '. | length')
    else
        COUNT=$(echo "$RESPONSE" | grep -o '"_id"' | wc -l)
    fi

    if [ "$COUNT" -ge 6 ]; then
        log "Mongo direct test passed ($COUNT moves)"
    else
        fail "Mongo direct test failed. Expected ≥6 moves, got $COUNT"
    fi
}

verify_redis_proxy() {
    echo "Verifying Redis proxy (/api/moves)..."
    RESPONSE=$(curl -k -s --cert "$CLIENT_CERT" --key "$CLIENT_KEY" "https://localhost:${REDIS_PORT}/api/moves")
    if command -v jq >/dev/null; then
        COUNT=$(echo "$RESPONSE" | jq '. | length')
    else
        COUNT=$(echo "$RESPONSE" | grep -o '"_id"' | wc -l)
    fi

    if [ "$COUNT" -ge 6 ]; then
        log "Redis proxy test passed ($COUNT moves returned)"
    else
        fail "Redis proxy test failed. Expected ≥6 moves, got $COUNT"
    fi
}

verify_dashboard() {
    echo "Verifying dashboard loads..."
    RESPONSE=$(curl -k -s --cert "$CLIENT_CERT" --key "$CLIENT_KEY" "https://localhost:${REDIS_PORT}/dashboard")
    if echo "$RESPONSE" | grep -q "Maze Mission Dashboard"; then
        log "Dashboard HTML served successfully"
    else
        fail "Dashboard failed to load"
    fi
}

cleanup() {
    echo "Cleaning up servers..."

    if [ -n "${MONGO_PID:-}" ]; then
        kill -TERM -$MONGO_PID 2>/dev/null || true
    fi

    if [ -n "${REDIS_PID:-}" ]; then
        kill -TERM -$REDIS_PID 2>/dev/null || true
    fi

    sleep 1

    pkill -f maze_https 2>/dev/null || true

    log "Servers stopped"
}

# ====================== MAIN ======================
check_prerequisites
start_servers
send_test_moves
sleep 1

verify_mongo_direct
verify_redis_proxy
verify_dashboard

cleanup

echo -e "\n${GREEN}=== ALL INTEGRATION TESTS PASSED ===${NC}"
echo "Dashboard available at: https://localhost:8447/dashboard"
echo "MongoDB URI used: ${MONGO_URI}"