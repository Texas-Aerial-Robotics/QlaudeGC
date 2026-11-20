# QlaudeGC rosbag control. Sent over ssh to the companion computer as `bash -s`, with these variables
# prepended by QGC (all single-quoted):
#   QGC_CMD      start | stop | status
#   QGC_DOMAIN   ROS_DOMAIN_ID ("" = leave unset)
#   QGC_DIR      folder for bags (a leading ~/ means $HOME)
#   QGC_PREFIX   bag name prefix
#   QGC_EXCLUDE  regex of topics to leave out ("" = record everything)
#   QGC_SETUP    extra setup scripts to source after /opt/ros, space-separated (e.g. the workspace with
#                px4_msgs; the rover needs its ros_env.sh and tar_ws), may be ""
#   QGC_EXTRA    extra `ros2 bag record` arguments, split on spaces
# Output is KEY=VALUE lines, parsed by TarRosbag. State lives in ~/.qgc_rosbag.{pid,dir} so a restarted
# QGC (or a second one) sees a recording that is already running.

PIDF="$HOME/.qgc_rosbag.pid"
DIRF="$HOME/.qgc_rosbag.dir"

case "$QGC_DIR" in
    "~") QGC_DIR="$HOME" ;;
    "~/"*) QGC_DIR="$HOME/${QGC_DIR#\~/}" ;;
esac

running_pid() {
    [ -f "$PIDF" ] || return 1
    local p
    p=$(cat "$PIDF" 2>/dev/null)
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null && echo "$p"
}

bag_bytes() {
    du -sb "$1" 2>/dev/null | cut -f1
}

free_bytes() {
    local d="$QGC_DIR"
    [ -d "$d" ] || d="$HOME"
    df -B1 --output=avail "$d" 2>/dev/null | tail -1 | tr -d ' '
}

one_line() {
    tail -n "${2:-3}" "$1" 2>/dev/null | tr '\n\r' '  ' | cut -c1-300
}

print_status() {
    local p out
    out=$(cat "$DIRF" 2>/dev/null)
    if p=$(running_pid); then
        echo "RECORDING=1"
        echo "PID=$p"
        echo "DIR=$out"
        echo "BYTES=$(bag_bytes "$out")"
        echo "ELAPSED=$(ps -o etimes= -p "$p" | tr -d ' ')"
    else
        echo "RECORDING=0"
        if [ -f "$PIDF" ]; then
            # The recorder is gone but nobody stopped it: it crashed or was killed.
            echo "DIED=$(one_line "$out.log")"
            rm -f "$PIDF"
        fi
        [ -n "$out" ] && echo "LAST=$out" && echo "LASTBYTES=$(bag_bytes "$out")"
    fi
    echo "FREE=$(free_bytes)"
}

do_start() {
    local p
    if p=$(running_pid); then
        echo "ERROR=Already recording to $(cat "$DIRF")"
        print_status
        return
    fi
    for d in jazzy humble iron rolling; do
        if [ -f "/opt/ros/$d/setup.bash" ]; then
            . "/opt/ros/$d/setup.bash"
            break
        fi
    done
    local s
    for s in $QGC_SETUP; do
        case "$s" in "~/"*) s="$HOME/${s#\~/}" ;; esac
        # shellcheck disable=SC1090
        [ -f "$s" ] && . "$s"
    done
    if ! command -v ros2 >/dev/null 2>&1; then
        echo "ERROR=ros2 not found (no /opt/ros/<distro>/setup.bash)"
        return
    fi
    [ -n "$QGC_DOMAIN" ] && export ROS_DOMAIN_ID="$QGC_DOMAIN"
    if ! mkdir -p "$QGC_DIR"; then
        echo "ERROR=Cannot create $QGC_DIR"
        return
    fi

    local out args
    out="$QGC_DIR/${QGC_PREFIX}_$(date +%Y%m%d_%H%M%S)"
    args=(bag record -a -o "$out")
    if [ -n "$QGC_EXCLUDE" ]; then
        # Humble: -x/--exclude <regex>; Jazzy and later: --exclude-regex <regex>.
        if ros2 bag record --help 2>/dev/null | grep -q -- '--exclude-regex'; then
            args+=(--exclude-regex "$QGC_EXCLUDE")
        else
            args+=(--exclude "$QGC_EXCLUDE")
        fi
    fi
    # shellcheck disable=SC2206
    [ -n "$QGC_EXTRA" ] && args+=($QGC_EXTRA)

    # setsid: own session and process group, so it outlives this ssh session and stop can signal the group.
    # Non-interactive bash starts background jobs with SIGINT ignored; env --default-signal restores it so
    # the recorder can be stopped cleanly with SIGINT.
    setsid nohup env --default-signal=INT ros2 "${args[@]}" >"$out.log" 2>&1 </dev/null &
    p=$!
    echo "$p" >"$PIDF"
    echo "$out" >"$DIRF"
    sleep 3
    if kill -0 "$p" 2>/dev/null; then
        echo "STARTED=1"
    else
        rm -f "$PIDF"
        echo "ERROR=ros2 bag record exited: $(one_line "$out.log" 4)"
    fi
    print_status
}

do_stop() {
    local p out i
    out=$(cat "$DIRF" 2>/dev/null)
    if ! p=$(running_pid); then
        echo "ERROR=Not recording"
        print_status
        return
    fi
    # SIGINT lets rosbag2 flush and write metadata.yaml. Signal the whole group (ros2 CLI + recorder).
    kill -INT -- "-$p" 2>/dev/null || kill -INT "$p"
    for i in $(seq 1 60); do
        kill -0 "$p" 2>/dev/null || break
        sleep 0.5
    done
    if kill -0 "$p" 2>/dev/null; then
        echo "ERROR=Recorder did not exit within 30 s (pid $p); still running"
        print_status
        return
    fi
    rm -f "$PIDF"
    echo "STOPPED=1"
    [ -f "$out/metadata.yaml" ] || echo "WARNING=No metadata.yaml in $out: $(one_line "$out.log")"
    print_status
}

case "$QGC_CMD" in
    start) do_start ;;
    stop) do_stop ;;
    status) print_status ;;
    *) echo "ERROR=Unknown command '$QGC_CMD'" ;;
esac
