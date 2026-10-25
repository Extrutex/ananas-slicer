#!/usr/bin/env bash
# Starts or stops the virtual printer with a Moonraker on 127.0.0.1 (no real printer, see docker-compose.host.yml).
#
#   tests/engine/rig/rig.sh up      start it and print its URL
#   tests/engine/rig/rig.sh down    remove it and its volume
#   tests/engine/rig/rig.sh url     print the URL if it runs
#
# RIG_PORT (default 17125) is the port on 127.0.0.1.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$HERE/../../.." && pwd)"
export RIG_DIR="$HERE" RIG_PORT="${RIG_PORT:-17125}"
export VP_UID="$(id -u)" VP_GID="$(id -g)"
mkdir -p "$REPO/tests/virtual-printer/out"

compose() {
    docker compose -p ananas-engine-rig --project-directory "$REPO/tests/virtual-printer" \
        -f "$REPO/tests/virtual-printer/docker-compose.yml" -f "$HERE/docker-compose.host.yml" "$@"
}

case "${1:-}" in
    up)
        docker image inspect ananas-slicer-vp:local > /dev/null 2>&1 || compose build --quiet
        compose up -d --wait --wait-timeout 180 klipper moonraker > /dev/null
        # Moonraker is healthy when it answers on the published port.
        for _ in $(seq 1 60); do
            if curl -fsS "http://127.0.0.1:$RIG_PORT/server/info" > /dev/null 2>&1; then
                echo "http://127.0.0.1:$RIG_PORT"
                exit 0
            fi
            sleep 1
        done
        echo "the rig did not answer on 127.0.0.1:$RIG_PORT" >&2
        exit 1
        ;;
    down) compose down -v --remove-orphans > /dev/null 2>&1 || true ;;
    url) curl -fsS "http://127.0.0.1:$RIG_PORT/server/info" > /dev/null 2>&1 && echo "http://127.0.0.1:$RIG_PORT" ;;
    *) echo "usage: $0 up|down|url" >&2; exit 2 ;;
esac
