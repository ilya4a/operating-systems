# Disk Monitor tests

This project uses two complementary levels of automated tests:

- **Unit tests in C++** exercise `Config`, `DirectoryMonitor`, and the PID-file/locking parts of `Daemon` independently.
- **`smoke_test.sh` integration tests** start the real executable and exercise daemonization, relative config paths, PID locking, inotify events, `SIGHUP`, `SIGTERM`, and restart behavior.

No external C++ test framework is required. The unit tests are ordinary executables registered with CTest. The integration test runs in a temporary directory, uses a copied config generated from `daemon.conf.in`, and removes its temporary files during cleanup.

## Run everything

From the project root on Linux:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure
```

The full CTest run contains these suites:

- `config_unit_tests`
- `directory_monitor_unit_tests`
- `daemon_unit_tests`
- `daemon_smoke_tests` (if `bash` and `python3` are available)

## Run only a test group

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R config_unit_tests
ctest --test-dir cmake-build-debug --output-on-failure -R directory_monitor_unit_tests
ctest --test-dir cmake-build-debug --output-on-failure -R daemon_unit_tests
ctest --test-dir cmake-build-debug --output-on-failure -R daemon_smoke_tests
```

Run all C++ unit tests without the slower daemon lifecycle test:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -L unit
```

Run just the full integration test:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -L integration
```

## Run the smoke test directly

After building the application:

```bash
bash tests/smoke_test.sh \
  --config-template tests/daemon.conf.in \
  --binary ./cmake-build-debug/disk-monitor
```

The script also supports a standalone strict build, useful when you want to check compilation plus behavior without using the existing CMake build directory:

```bash
bash tests/smoke_test.sh \
  --config-template tests/daemon.conf.in \
  --build
```

The `--build` mode requires `g++`. It compiles with C++17 and `-Wall -Wextra -Wpedantic -Werror` into a temporary directory.

## What the tests cover

### `ConfigTests.cpp`

Configuration defaults, custom values, whitespace and comments, multiple directories, relative/absolute path resolution, lexical normalization, malformed lines, unknown keys, empty values, missing directories, interval boundaries and invalid intervals, and failed reload preserving the previous configuration.

### `DirectoryMonitorTests.cpp`

Invalid arguments and paths, starting and stopping, event-log behavior, creating and deleting files, multiple watched directories, successful reconfiguration, failed reconfiguration preserving old watches, interrupt-descriptor behavior, repeated stop, and preventing recursive logging when the event log is inside a watched directory.

### `DaemonTests.cpp`

PID-file validation and contents, truncating stale contents, duplicate preparation, `flock` exclusion, release/reacquisition, persistent lock sidecar, symbolic-link rejection, invalid path types, missing parent directories, and safe cleanup if the PID path is replaced.

### `smoke_test.sh`

Initial invalid configuration is rejected before daemonization; valid startup while launched from outside the config directory; PID file stores the child PID; `setsid()`, `chdir("/")`, and standard streams redirected to `/dev/null`; a second instance cannot acquire the PID lock; create, access, modify, rename and delete events; successful `SIGHUP` changes watched directory; invalid `SIGHUP` leaves the previous watches active; `SIGTERM` and `SIGINT` stop the daemon, write the stop marker and remove the PID file; and a complete restart cycle. Checks of system-log visibility are optional because unprivileged users may not be able to read the journal.

## Notes

- The smoke test uses temporary directories and does not depend on the project's real `daemon.conf`, `watch/`, `logs/`, or `run/` directories.
- A `.lock` sidecar can remain after a normal run. That is intentional: the lock file provides a stable inode for `flock()` across restarts. The smoke test's own temporary copy is removed on exit.
- If a smoke test fails, inspect the printed failure message and the relevant log tail. The test is deliberately non-silent so failures are diagnosable.
- Avoid running the test against the production daemon or pointing the test template at real user directories. Use the provided `tests/daemon.conf.in`.
