#!/usr/bin/env bash
# Record playback logs for a fixed set of scenarios so that a later build can be
# compared against them. Used to check that refactoring the player core does not
# change behaviour.
#
#   Scripts/record_playback_baseline.sh [--no-build] <label> <media> [<media> ...]
#
# For every media file it launches the Debug app with SP_DEBUG, SP_VSTATS and
# one automation scenario, lets it run, quits it through Apple Events (so the
# normal stop path runs) and keeps the log. Results go to
# .build/baseline/<label>/, with summary.txt collecting the lines worth
# comparing: first frame, pacing, seeks, A/V sync, stop/join timings, errors.
#
# Quit every other Khua instance first: the app is quit by bundle identifier.
set -euo pipefail
CALLER_DIR=$(pwd)
cd "$(dirname "$0")/.."

BUILD=1
if [ "${1:-}" = "--no-build" ]; then
    BUILD=0
    shift
fi
if [ $# -lt 2 ]; then
    echo "usage: $0 [--no-build] <label> <media> [<media> ...]" >&2
    exit 2
fi
LABEL=$1
shift

# open -a needs an absolute path; a relative one is looked up as an app name.
APP="$(pwd)/.build/Build/Products/Debug/Khua.app"
BUNDLE_ID=app.khuaplayer.KhuaPlayer
OUT=.build/baseline/$LABEL

if pgrep -x Khua >/dev/null; then
    echo "error: quit every running Khua instance first" >&2
    exit 1
fi
# Media paths are relative to the caller's directory, not the repository root.
files=()
for media in "$@"; do
    case $media in
        /*) ;;
        *) media="$CALLER_DIR/$media" ;;
    esac
    [ -f "$media" ] || { echo "error: not a file: $media" >&2; exit 1; }
    files+=("$media")
done
if [ "$BUILD" = 1 ]; then
    ./Scripts/build.sh Debug
fi
[ -d "$APP" ] || { echo "error: $APP not found; build Debug first" >&2; exit 1; }

mkdir -p "$OUT"
git rev-parse HEAD > "$OUT/commit.txt" 2>/dev/null || true
sw_vers > "$OUT/system.txt" 2>/dev/null || true

# name|seconds|environment (space separated; NEXT is replaced by the next file)
SCENARIOS="
play|14|SP_AUTOSEEK=30 SP_AUTOPAUSE=1
seqseek|9|SP_SEQSEEK=1
rate|12|SP_AUTORATE=2:2,0.5:5,1:8
reopen|9|SP_SEQOPEN2=NEXT
"

wait_for_exit() {
    local i
    for i in $(seq 1 100); do
        pgrep -x Khua >/dev/null || return 0
        sleep 0.1
    done
    echo "warning: Khua did not quit; killing it" >&2
    pkill -x Khua || true
    sleep 1
}

run_one() {
    local media=$1 next=$2 name=$3 seconds=$4 envspec=$5
    local base log
    base=$(basename "$media")
    log="$OUT/${base}.${name}.log"
    local args=(-n -a "$APP" --env SP_DEBUG=1 --env SP_VSTATS=1 --env SP_AUTOMATION=1)
    local kv
    for kv in $envspec; do
        args+=(--env "${kv//NEXT/$next}")
    done
    echo "==> $base: $name (${seconds}s)"
    open "${args[@]}" --stdout "$log.stdout" --stderr "$log" "$media"
    sleep "$seconds"
    osascript -e "tell application id \"$BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
    wait_for_exit
    if [ -s "$log.stdout" ]; then cat "$log.stdout" >> "$log"; fi
    rm -f "$log.stdout"
}

count=${#files[@]}
for ((i = 0; i < count; i++)); do
    media=${files[$i]}
    next=${files[$(((i + 1) % count))]}
    while IFS='|' read -r name seconds envspec; do
        [ -n "$name" ] || continue
        if [ "$name" = reopen ] && [ "$count" -lt 2 ]; then continue; fi
        run_one "$media" "$next" "$name" "$seconds" "$envspec"
    done <<< "$SCENARIOS"
done

SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
for log in "$OUT"/*.log; do
    {
        echo "### $(basename "$log")"
        grep -E '\[Core\] (首帧|软解竞速首帧上屏|音频:)|\[Pace\] 5s|\[Seek\] 目标帧到达|\[SeekFrame\] 关键帧上屏|\[Sync\]|\[Stop\]|\[Open\] 主线程|\[Test\]|失败|error|Error' "$log" \
            | sed -E 's/^[0-9-]+ [0-9:.]+ [^ ]+\[[0-9:a-fx]+\] //' || true
        echo
    } >> "$SUMMARY"
done
echo "Logs: $OUT"
echo "Summary: $SUMMARY"
