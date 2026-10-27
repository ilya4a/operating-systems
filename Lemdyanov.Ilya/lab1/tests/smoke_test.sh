#!/usr/bin/env bash
# Smoke/integration tests for the Linux inotify Disk Monitor.
# The configuration template must use the application's real syntax and contain
# exactly one @WATCH_DIR@ token and exactly one @EVENT_LOG@ token. Keep all
# other required keys (including interval_seconds if Config still requires it).

set -Eeuo pipefail

usage() {
    cat <<'USAGE'
Usage:
  tests/smoke_test.sh --config-template PATH [--binary PATH | --build]

Options:
  --config-template PATH  A copy of your valid config file with these tokens:
                          @WATCH_DIR@ and @EVENT_LOG@ in place of their values.
  --binary PATH           Existing compiled disk-monitor executable.
  --build                 Build from main.cpp, Config.cpp, DirectoryMonitor.cpp,
                          and Daemon.cpp, with strict warnings.

Examples:
  tests/smoke_test.sh --config-template tests/daemon.conf.in --binary ./disk-monitor
  tests/smoke_test.sh --config-template tests/daemon.conf.in --build

The test runs the program in a temporary directory, changes no user data, and
cleans up its process and files on exit. Linux, bash, python3, and (for --build)
g++ are required.
USAGE
}

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }
info() { printf 'INFO: %s\n' "$*"; }

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$SCRIPT_DIR"
if [[ ! -f "$ROOT/main.cpp" && -f "$ROOT/../main.cpp" ]]; then
    ROOT="$(cd -- "$ROOT/.." && pwd -P)"
fi

CONFIG_TEMPLATE=""
BINARY=""
DO_BUILD=0
while (($#)); do
    case "$1" in
        --config-template)
            (($# >= 2)) || { usage; fail "--config-template needs a path"; }
            CONFIG_TEMPLATE="$2"; shift 2 ;;
        --binary)
            (($# >= 2)) || { usage; fail "--binary needs a path"; }
            BINARY="$2"; shift 2 ;;
        --build)
            DO_BUILD=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            usage; fail "unknown argument: $1" ;;
    esac
done

command -v python3 >/dev/null 2>&1 || fail "python3 not found"
command -v mktemp >/dev/null 2>&1 || fail "mktemp not found"
command -v ps >/dev/null 2>&1 || fail "ps not found"

[[ -n "$CONFIG_TEMPLATE" ]] || { usage; fail "provide --config-template"; }
[[ -f "$CONFIG_TEMPLATE" ]] || fail "config template not found: $CONFIG_TEMPLATE"
CONFIG_TEMPLATE="$(cd -- "$(dirname -- "$CONFIG_TEMPLATE")" && pwd -P)/$(basename -- "$CONFIG_TEMPLATE")"

if [[ "$DO_BUILD" -eq 1 && -n "$BINARY" ]]; then
    fail "use either --build or --binary, not both"
fi

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/disk-monitor-test.XXXXXX")"
PID=""
PID_FILE=""
LAUNCH_PID=""
SECOND_PID=""
cleanup() {
    local cleanup_status=$?
    if [[ -z "$PID" && -n "$PID_FILE" && -s "$PID_FILE" ]]; then
        PID="$(tr -d '[:space:]' < "$PID_FILE" 2>/dev/null || true)"
    fi
    if [[ -n "$LAUNCH_PID" ]] && kill -0 "$LAUNCH_PID" 2>/dev/null; then
        kill -TERM "$LAUNCH_PID" 2>/dev/null || true
        wait "$LAUNCH_PID" 2>/dev/null || true
    fi
    if [[ -n "$SECOND_PID" ]] && kill -0 "$SECOND_PID" 2>/dev/null; then
        kill -TERM "$SECOND_PID" 2>/dev/null || true
        sleep 0.05
        kill -KILL "$SECOND_PID" 2>/dev/null || true
        wait "$SECOND_PID" 2>/dev/null || true
    fi
    if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null || true
        # Do not hang cleanup if a broken build ignores SIGTERM.
        for _ in {1..30}; do
            local state
            state="$(ps -o stat= -p "$PID" 2>/dev/null | tr -d '[:space:]' || true)"
            [[ -z "$state" || "$state" == Z* ]] && break
            sleep 0.05
        done
        kill -KILL "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf -- "$TMP_ROOT"
    exit "$cleanup_status"
}
trap cleanup EXIT INT TERM HUP

if [[ "$DO_BUILD" -eq 1 ]]; then
    command -v g++ >/dev/null 2>&1 || fail "g++ not found"
    [[ -f "$ROOT/main.cpp" ]] || fail "main.cpp not found under $ROOT"
    [[ -f "$ROOT/DirectoryMonitor.cpp" ]] || fail "DirectoryMonitor.cpp not found under $ROOT"
    [[ -f "$ROOT/Daemon.cpp" ]] || fail "Daemon.cpp not found under $ROOT"

    config_source=""
    for candidate in "$ROOT/Config.cpp" "$ROOT/config.cpp"; do
        if [[ -f "$candidate" ]]; then config_source="$candidate"; break; fi
    done
    [[ -n "$config_source" ]] || fail "Config.cpp/config.cpp not found under $ROOT"

    build_dir="$TMP_ROOT/build"
    mkdir -p "$build_dir"
    sources=("$ROOT/main.cpp" "$config_source" "$ROOT/DirectoryMonitor.cpp" "$ROOT/Daemon.cpp")
    info "Building with -std=c++17 -Wall -Wextra -Wpedantic -Werror"
    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -I"$ROOT" \
        "${sources[@]}" -o "$build_dir/disk-monitor" || fail "strict compilation failed"
    BINARY="$build_dir/disk-monitor"
else
    [[ -n "$BINARY" ]] || BINARY="$ROOT/disk-monitor"
    if [[ "$BINARY" != /* ]]; then BINARY="$(cd -- "$(dirname -- "$BINARY")" && pwd -P)/$(basename -- "$BINARY")"; fi
    [[ -x "$BINARY" ]] || fail "executable not found or not executable: $BINARY (use --build or --binary)"
fi

# Test infrastructure lives in a path unrelated to the caller's working directory.
WORK="$TMP_ROOT/work"
CONFIG_DIR="$WORK/config"
OUTSIDE_DIR="$WORK/run-from-here"
WATCH_A="$CONFIG_DIR/watch-a"
WATCH_B="$CONFIG_DIR/watch-b"
CONFIG_PATH="$CONFIG_DIR/daemon.conf"
EVENT_LOG="$CONFIG_DIR/events.log"
STDOUT_LOG="$TMP_ROOT/program-output.log"
mkdir -p "$WATCH_A" "$WATCH_B" "$OUTSIDE_DIR"

# Validate the test template before starting the program.
python3 - "$CONFIG_TEMPLATE" <<'PY' || exit 1
import pathlib, sys
p = pathlib.Path(sys.argv[1])
text = p.read_text(encoding="utf-8")
for token in ("@WATCH_DIR@", "@EVENT_LOG@"):
    count = text.count(token)
    if count != 1:
        print(f"FAIL: {p} must contain {token} exactly once (found {count})", file=sys.stderr)
        sys.exit(1)
PY

# The renderer is deliberately written without sed interpolation so filesystem
# paths containing spaces or '&' cannot corrupt the generated configuration.
render_config() {
    local watch_value="$1"
    python3 - "$CONFIG_TEMPLATE" "$CONFIG_PATH" "$watch_value" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
watch_value = sys.argv[3]
text = source.read_text(encoding="utf-8")
if text.count("@WATCH_DIR@") != 1 or text.count("@EVENT_LOG@") != 1:
    print("Config template placeholders changed after validation", file=sys.stderr)
    sys.exit(1)
text = text.replace("@WATCH_DIR@", watch_value)
text = text.replace("@EVENT_LOG@", "events.log")
lines = text.splitlines()
pid_file_lines = [index for index, line in enumerate(lines)
                  if "=" in line and line.split("=", 1)[0].strip() == "pid_file"]
if len(pid_file_lines) > 1:
    print("Config template must not contain more than one pid_file setting", file=sys.stderr)
    sys.exit(1)
if pid_file_lines:
    lines[pid_file_lines[0]] = "pid_file=./disk-monitor.pid"
else:
    lines.append("pid_file=./disk-monitor.pid")
destination.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY
}

count_fixed() {
    local file="$1" pattern="$2"
    if [[ -f "$file" ]]; then grep -F -c -- "$pattern" "$file" || true; else printf '0\n'; fi
}

wait_for_pattern() {
    local file="$1" pattern="$2" timeout_seconds="${3:-5}"
    local elapsed=0
    while (( elapsed < timeout_seconds * 20 )); do
        if [[ -f "$file" ]] && grep -Fq -- "$pattern" "$file"; then return 0; fi
        if [[ -n "$PID" ]]; then
            local state
            state="$(ps -o stat= -p "$PID" 2>/dev/null | tr -d '[:space:]' || true)"
            if [[ -z "$state" || "$state" == Z* ]]; then
                printf 'Program exited before expected log line: %s\n' "$pattern" >&2
                [[ -f "$STDOUT_LOG" ]] && cat "$STDOUT_LOG" >&2
                return 1
            fi
        fi
        sleep 0.05
        ((elapsed += 1))
    done
    printf 'Timed out waiting for pattern: %s\nFile: %s\n' "$pattern" "$file" >&2
    [[ -f "$file" ]] && tail -n 80 "$file" >&2
    [[ -f "$STDOUT_LOG" ]] && cat "$STDOUT_LOG" >&2
    return 1
}

process_state() {
    ps -o stat= -p "$1" 2>/dev/null | tr -d '[:space:]' || true
}

wait_for_exit() {
    local target_pid="$1" timeout_seconds="${2:-3}"
    local tries=0 state
    while (( tries < timeout_seconds * 20 )); do
        state="$(process_state "$target_pid")"
        if [[ -z "$state" || "$state" == Z* ]]; then return 0; fi
        sleep 0.05
        ((tries += 1))
    done
    return 1
}

start_daemon() {
    local starts_before="$1"
    local launch_status candidate tries=0

    (
        cd -- "$OUTSIDE_DIR"
        exec "$BINARY" "$CONFIG_PATH"
    ) >"$STDOUT_LOG" 2>&1 &
    LAUNCH_PID=$!

    if ! wait_for_exit "$LAUNCH_PID" 5; then
        kill -TERM "$LAUNCH_PID" 2>/dev/null || true
        wait "$LAUNCH_PID" 2>/dev/null || true
        LAUNCH_PID=""
        fail "launcher did not exit after daemonization"
    fi
    set +e
    wait "$LAUNCH_PID"
    launch_status=$?
    set -e
    LAUNCH_PID=""
    [[ "$launch_status" -eq 0 ]] || {
        [[ -f "$STDOUT_LOG" ]] && cat "$STDOUT_LOG" >&2
        fail "daemon launcher exited with status $launch_status"
    }

    # The launcher PID is not the daemon PID. The daemon writes its own PID
    # after fork(), then initializes the monitor asynchronously.
    while (( tries < 200 )); do
        if [[ -s "$PID_FILE" ]]; then
            candidate="$(tr -d '[:space:]' < "$PID_FILE" 2>/dev/null || true)"
            if [[ "$candidate" =~ ^[0-9]+$ ]] && kill -0 "$candidate" 2>/dev/null; then
                PID="$candidate"
                if (( $(count_fixed "$EVENT_LOG" "MONITOR_STARTED") > starts_before )); then
                    return 0
                fi
            fi
        fi
        sleep 0.05
        ((tries += 1))
    done
    [[ -f "$STDOUT_LOG" ]] && cat "$STDOUT_LOG" >&2
    fail "daemon child did not create a ready monitor (PID file: $PID_FILE)"
}

# Query system logging when available. This is optional because some systems do
# not expose their system journal to the current user.
SYSLOG_TEST_START="$(date --iso-8601=seconds)"
SYSLOG_SINCE="$SYSLOG_TEST_START"
syslog_contains_since() {
    local needle="$1"
    if command -v journalctl >/dev/null 2>&1; then
        if journalctl -t disk-monitor --since "$SYSLOG_SINCE" -o cat --no-pager 2>/dev/null | grep -Fq -- "$needle"; then
            return 0
        fi
    fi
    for logfile in /var/log/syslog /var/log/messages; do
        if [[ -r "$logfile" ]] && tail -n 3000 "$logfile" | grep -Fq -- "$needle"; then
            return 0
        fi
    done
    return 1
}

wait_for_syslog() {
    local timeout_seconds="$1"
    shift
    local tries=0 needle
    while (( tries < timeout_seconds * 10 )); do
        for needle in "$@"; do
            if syslog_contains_since "$needle"; then return 0; fi
        done
        sleep 0.1
        ((tries += 1))
    done
    return 1
}

PID_FILE="$CONFIG_DIR/disk-monitor.pid"

# An invalid initial configuration must be rejected before daemonization and
# must not leave behind a PID file or a background daemon.
info "Checking that an invalid initial configuration is rejected"
render_config "missing-watch-for-startup-test"
set +e
(
    cd -- "$OUTSIDE_DIR"
    exec "$BINARY" "$CONFIG_PATH"
) >"$TMP_ROOT/invalid-startup.log" 2>&1
startup_status=$?
set -e
[[ "$startup_status" -ne 0 ]] || {
    cat "$TMP_ROOT/invalid-startup.log" >&2
    fail "invalid initial configuration unexpectedly succeeded"
}
[[ ! -e "$PID_FILE" ]] || fail "invalid initial configuration created a PID file"
grep -Fq -- "Not a directory" "$TMP_ROOT/invalid-startup.log" || {
    cat "$TMP_ROOT/invalid-startup.log" >&2
    fail "invalid initial configuration did not report the missing directory"
}
pass "invalid initial configuration is rejected before daemonization"

render_config "watch-a"
info "Starting daemon from a directory outside the config directory"
starts_before="$(count_fixed "$EVENT_LOG" "MONITOR_STARTED")"
start_daemon "$starts_before"
pass "daemonization, startup, and relative paths resolved against the config directory"

[[ -f "$PID_FILE" ]] || fail "PID file was not created"
pid_in_file="$(tr -d '[:space:]' < "$PID_FILE")"
[[ "$pid_in_file" == "$PID" ]] || fail "PID file contains '$pid_in_file', expected daemon PID '$PID'"
pass "PID file contains the daemon child PID, not the launcher PID"

[[ "$(readlink "/proc/$PID/cwd" 2>/dev/null || true)" == "/" ]] || \
    fail "daemon working directory is not /"
sid="$(ps -o sid= -p "$PID" 2>/dev/null | tr -d '[:space:]' || true)"
[[ "$sid" == "$PID" ]] || fail "daemon is not a session leader after setsid()"
for standard_fd in 0 1 2; do
    target="$(readlink "/proc/$PID/fd/$standard_fd" 2>/dev/null || true)"
    [[ "$target" == "/dev/null" ]] || \
        fail "standard descriptor $standard_fd is not redirected to /dev/null (target: $target)"
done
pass "setsid(), chdir(/), and standard-stream redirection are applied"

# A second process using the same PID-file path must fail before starting watches.
"$BINARY" "$CONFIG_PATH" >"$TMP_ROOT/second-instance.log" 2>&1 &
SECOND_PID=$!
if ! wait_for_exit "$SECOND_PID" 2; then
    kill -TERM "$SECOND_PID" 2>/dev/null || true
    sleep 0.05
    kill -KILL "$SECOND_PID" 2>/dev/null || true
    wait "$SECOND_PID" 2>/dev/null || true
    SECOND_PID=""
    fail "second instance did not exit promptly; it may have bypassed the PID-file lock"
fi
set +e
wait "$SECOND_PID"
second_status=$?
set -e
SECOND_PID=""
[[ "$second_status" -ne 0 ]] || fail "second instance unexpectedly started successfully"
grep -Fq -- "locked by another running instance" "$TMP_ROOT/second-instance.log" || {
    cat "$TMP_ROOT/second-instance.log" >&2
    fail "second instance did not report the PID-file lock"
}
kill -0 "$PID" 2>/dev/null || fail "first instance exited after the second instance attempted to start"
pid_after_second="$(tr -d '[:space:]' < "$PID_FILE")"
[[ "$pid_after_second" == "$PID" ]] || fail "second instance changed the active PID file"
pass "PID-file lock prevents a second instance from starting"

# Exercise common file operations and check the expected inotify event classes.
printf 'initial content\n' > "$WATCH_A/probe.txt"
wait_for_pattern "$EVENT_LOG" "CREATE $WATCH_A/probe.txt" 5 || fail "CREATE event missing"
wait_for_pattern "$EVENT_LOG" "CLOSE_WRITE $WATCH_A/probe.txt" 5 || fail "CLOSE_WRITE event missing"
cat "$WATCH_A/probe.txt" >/dev/null
wait_for_pattern "$EVENT_LOG" "ACCESS $WATCH_A/probe.txt" 5 || fail "ACCESS event missing"
printf 'appended content\n' >> "$WATCH_A/probe.txt"
wait_for_pattern "$EVENT_LOG" "MODIFY $WATCH_A/probe.txt" 5 || fail "MODIFY event missing"
mv "$WATCH_A/probe.txt" "$WATCH_A/renamed.txt"
wait_for_pattern "$EVENT_LOG" "MOVED_FROM $WATCH_A/probe.txt" 5 || fail "MOVED_FROM event missing"
wait_for_pattern "$EVENT_LOG" "MOVED_TO $WATCH_A/renamed.txt" 5 || fail "MOVED_TO event missing"
rm "$WATCH_A/renamed.txt"
wait_for_pattern "$EVENT_LOG" "DELETE $WATCH_A/renamed.txt" 5 || fail "DELETE event missing"
pass "create, read/access, modify, rename, and delete events"

# Reload successfully to watch B. MONITOR_STARTED must be added after SIGHUP.
starts_before="$(count_fixed "$EVENT_LOG" "MONITOR_STARTED")"
render_config "watch-b"
kill -HUP "$PID" || fail "could not send SIGHUP for successful reload"
reload_wait=0
while (( reload_wait < 100 )); do
    starts_now="$(count_fixed "$EVENT_LOG" "MONITOR_STARTED")"
    if (( starts_now > starts_before )); then break; fi
    state="$(process_state "$PID")"
    [[ -n "$state" && "$state" != Z* ]] || fail "monitor exited during successful SIGHUP"
    sleep 0.05
    ((reload_wait += 1))
done
(( reload_wait < 100 )) || fail "successful SIGHUP did not restart the monitor"
printf 'after successful reload\n' > "$WATCH_B/reload-probe.txt"
wait_for_pattern "$EVENT_LOG" "CREATE $WATCH_B/reload-probe.txt" 5 || fail "new directory is not watched after SIGHUP"
printf 'old directory should not be watched\n' > "$WATCH_A/after-successful-reload.txt"
sleep 0.3
if grep -Fq -- "$WATCH_A/after-successful-reload.txt" "$EVENT_LOG"; then
    fail "old directory is still being watched after successful SIGHUP"
fi
pass "successful SIGHUP switches watches to the new directory"

# Reload an invalid directory and make sure monitoring of B survives. If journald
# is readable, synchronize on the expected error message before checking B.
SYSLOG_SINCE="$(date --iso-8601=seconds)"
render_config "missing-watch-for-negative-test"
kill -HUP "$PID" || fail "could not send SIGHUP for invalid-reload test"
if wait_for_syslog 5 \
    "Configuration reload failed; keeping current configuration" \
    "Configuration reload could not be applied; keeping current configuration and watches"; then
    pass "invalid SIGHUP configuration was reported through syslog"
else
    info "Could not read the system journal; invalid-reload syslog assertion is SKIPPED"
    # Give the process time to consume SIGHUP on systems without readable syslog.
    sleep 0.5
fi
printf 'old valid directory must remain watched\n' > "$WATCH_B/after-failed-reload.txt"
wait_for_pattern "$EVENT_LOG" "CREATE $WATCH_B/after-failed-reload.txt" 5 || fail "old watches did not survive failed reload"
pass "failed SIGHUP preserves the previously active watches"

# SIGTERM should wake an indefinite poll even when there are no further file events.
kill -TERM "$PID" || fail "could not send SIGTERM"
if ! wait_for_exit "$PID" 3; then
    fail "SIGTERM did not terminate the monitor promptly"
fi
PID=""
[[ ! -e "$PID_FILE" ]] || fail "PID file was not removed on clean shutdown"
grep -Fq -- "MONITOR_STOPPED" "$EVENT_LOG" || fail "MONITOR_STOPPED missing from event log"
pass "SIGTERM promptly terminates the daemon, closes the event log, and removes the PID file"

# Complete the lifecycle by starting a fresh daemon with the same paths and
# stopping it again. The persistent .lock sidecar keeps flock() on one inode.
render_config "watch-a"
starts_before_restart="$(count_fixed "$EVENT_LOG" "MONITOR_STARTED")"
info "Restarting daemon after clean shutdown"
start_daemon "$starts_before_restart"
[[ -s "$PID_FILE" ]] || fail "PID file missing after daemon restart"
restart_pid="$(tr -d '[:space:]' < "$PID_FILE")"
[[ "$restart_pid" == "$PID" ]] || fail "restart PID file does not match daemon PID"
printf 'restart cycle\n' > "$WATCH_A/restart-probe.txt"
wait_for_pattern "$EVENT_LOG" "CREATE $WATCH_A/restart-probe.txt" 5 || fail "restarted daemon is not monitoring the configured directory"
kill -TERM "$PID" || fail "could not stop restarted daemon"
if ! wait_for_exit "$PID" 3; then
    fail "restarted daemon did not stop promptly"
fi
PID=""
[[ ! -e "$PID_FILE" ]] || fail "PID file remains after stopping restarted daemon"
pass "full start/stop/restart cycle succeeds"

# SIGINT is also a supported graceful-termination signal.
render_config "watch-a"
stopped_before_int="$(count_fixed "$EVENT_LOG" "MONITOR_STOPPED")"
info "Checking graceful shutdown on SIGINT"
start_daemon "$(count_fixed "$EVENT_LOG" "MONITOR_STARTED")"
kill -INT "$PID" || fail "could not send SIGINT"
if ! wait_for_exit "$PID" 3; then
    fail "SIGINT did not terminate the monitor promptly"
fi
PID=""
[[ ! -e "$PID_FILE" ]] || fail "PID file was not removed after SIGINT"
stopped_after_int="$(count_fixed "$EVENT_LOG" "MONITOR_STOPPED")"
(( stopped_after_int > stopped_before_int )) || fail "MONITOR_STOPPED missing after SIGINT"
pass "SIGINT gracefully terminates the daemon and removes the PID file"

# Check startup/shutdown syslog entries if the system makes them observable.
SYSLOG_SINCE="$SYSLOG_TEST_START"
if syslog_contains_since "Disk Monitor starting" && \
   syslog_contains_since "Disk Monitor stopping after signal"; then
    pass "startup and shutdown messages are visible in the system journal"
else
    info "System-log visibility check SKIPPED (journal/syslog may be inaccessible to this user)"
fi

printf '\nALL AVAILABLE SMOKE TESTS PASSED\n'
printf 'The temporary test directory will now be removed.\n'

