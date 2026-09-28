#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VITA_IP=${VITA_IP:-192.168.1.79}
VITA_FTP_PORT=${VITA_FTP_PORT:-1337}
VITA_COMPANION_PORT=${VITA_COMPANION_PORT:-1338}
DURATION=${2:-30}
GAME=${1:-}
RESULT_ROOT="$ROOT/benchmarks/results"
BUILD_DIR="$ROOT/build/ruffle"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/flashvita-bench.XXXXXX")
CONFIG_URL="ftp://$VITA_IP:$VITA_FTP_PORT/ux0:/data/FlashVita/config.ini"
EBOOT_URL="ftp://$VITA_IP:$VITA_FTP_PORT/ux0:/app/FLASHVITA/eboot.bin"
LOG_URL="ftp://$VITA_IP:$VITA_FTP_PORT/ux0:/data/FlashVita/runtime.log"

cleanup() {
    if [ -f "$TMP_DIR/config.backup" ]; then
        curl -fsS -T "$TMP_DIR/config.backup" "$CONFIG_URL" >/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

vc() {
    printf '%s\n' "$1" | nc -w 2 "$VITA_IP" "$VITA_COMPANION_PORT" >/dev/null 2>&1 || true
}

set_profile_config() {
    if curl -fsS "$CONFIG_URL" -o "$TMP_DIR/config.backup"; then
        cp "$TMP_DIR/config.backup" "$TMP_DIR/config.profile"
    else
        cat >"$TMP_DIR/config.profile" <<'EOF'
vsync=1
show_invalid_swf=1
remember_last_game=1
enable_logs=0
enable_perf_logs=0
ui_theme=0
ui_scale=1.00
EOF
    fi

    awk '
        BEGIN { logs=0; perf=0 }
        /^enable_logs=/ { print "enable_logs=1"; logs=1; next }
        /^enable_perf_logs=/ { print "enable_perf_logs=1"; perf=1; next }
        { print }
        END {
            if (!logs) print "enable_logs=1"
            if (!perf) print "enable_perf_logs=1"
        }
    ' "$TMP_DIR/config.profile" >"$TMP_DIR/config.enabled"
    curl -fsS -T "$TMP_DIR/config.enabled" "$CONFIG_URL" >/dev/null
}

build_profile() {
    cd "$ROOT"
    /usr/bin/make ENABLE_RUFFLE=1 VITA_PROFILE=1 -j8 verify
}

deploy_profile() {
    local_sha=$(shasum -a 256 "$BUILD_DIR/eboot.bin" | awk '{print $1}')
    : >"$TMP_DIR/empty.log"

    vc "destroy"
    vc "screen on"
    vc "nosleep on"
    curl -fsS -T "$TMP_DIR/empty.log" "$LOG_URL" >/dev/null || true
    curl -fsS -T "$BUILD_DIR/eboot.bin" "$EBOOT_URL" >/dev/null
    curl -fsS "$EBOOT_URL" -o "$TMP_DIR/remote-eboot.bin"
    remote_sha=$(shasum -a 256 "$TMP_DIR/remote-eboot.bin" | awk '{print $1}')

    if [ "$local_sha" != "$remote_sha" ]; then
        echo "SHA mismatch: local=$local_sha remote=$remote_sha" >&2
        exit 2
    fi

    echo "eboot SHA256: $local_sha"
    vc "launch FLASHVITA"
}

write_metadata() {
    out_dir=$1
    {
        echo "game=$GAME"
        echo "duration_s=$DURATION"
        echo "timestamp_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        echo "flashvita_commit=$(git -C "$ROOT" rev-parse HEAD)"
        echo "flashvita_dirty=$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"
        echo "ruffle_commit=$(git -C "$ROOT/third_party/ruffle" rev-parse HEAD)"
        echo "eboot_sha256=$(shasum -a 256 "$BUILD_DIR/eboot.bin" | awk '{print $1}')"
        echo "vpk_sha256=$(shasum -a 256 "$BUILD_DIR/FlashVita.vpk" | awk '{print $1}')"
        echo "vita_profile=1"
        echo "attribute2=12"
        if [ -d /Users/robin994/.local/opt/vitadb-deps/vitaGL-fresh/.git ]; then
            echo "vitagl_commit=$(git -C /Users/robin994/.local/opt/vitadb-deps/vitaGL-fresh rev-parse HEAD)"
        fi
    } >"$out_dir/metadata.txt"
}

if [ -z "$GAME" ]; then
    echo "Usage: $0 <pacman|cubefield|as3-name> [capture-seconds]" >&2
    exit 1
fi

mkdir -p "$RESULT_ROOT"
build_profile
set_profile_config
deploy_profile

echo
echo "Avvia '$GAME' sulla Vita e portalo nella scena da misurare."
printf "Premi Invio quando il gameplay e' stabile... "
read -r _
echo "Cattura per $DURATION secondi..."
sleep "$DURATION"

STAMP=$(date '+%Y%m%d-%H%M%S')
SAFE_GAME=$(printf '%s' "$GAME" | tr -cs 'A-Za-z0-9._-' '_')
OUT_DIR="$RESULT_ROOT/$STAMP-$SAFE_GAME"
mkdir -p "$OUT_DIR"
curl -fsS "$LOG_URL" -o "$OUT_DIR/runtime.log"
write_metadata "$OUT_DIR"

grep -E 'FLASHVITA_BOOT|frame_perf|ruffle_perf|avm_frame|avm_q|render ' \
    "$OUT_DIR/runtime.log" >"$OUT_DIR/perf-summary.log" || true

echo "Benchmark salvato in: $OUT_DIR"
