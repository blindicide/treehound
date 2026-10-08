# Building Treehound

Requires Linux, a C17 compiler, CMake 3.20+, SQLite 3.34+ with FTS5 and GTK4 4.6+. GTK is linked only by the graphical/CLI client. Python 3, Xvfb and dbus-run-session are test-only dependencies.

On Debian/Ubuntu: `apt install build-essential cmake pkg-config libgtk-4-dev libsqlite3-dev python3 xvfb dbus-x11`.
On Fedora: `dnf install gcc cmake pkgconf-pkg-config gtk4-devel sqlite-devel python3 xorg-x11-server-Xvfb dbus-daemon`.

```sh
cmake -S . -B build -DTREEHOUND_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
build/treehoundd
# In another terminal:
build/treehound
```

Use `-DTREEHOUND_BUILD_GUI=OFF` for a GTK-free CLI/daemon build, `-DTREEHOUND_BUILD_TESTS=OFF` to omit test dependencies, and `-DTREEHOUND_SANITIZE=ON` for ASan/UBSan. The GUI test is registered when both Xvfb and dbus-run-session are installed; CI installs both. Test fixtures use private temporary roots and never index your home.

The GUI can start a sibling `treehoundd` on demand. Closing the window leaves the daemon running. This does not enable any systemd service. `treehoundd --help` documents explicit config/database/socket overrides.

## Opt-in million-file benchmark

```sh
python3 tests/benchmarks/million.py build /path/to/isolated-output 1000000 initial
```

This creates one million actual files, launches the real daemon, queries its IPC,
measures Linux process memory/CPU and runs ten GTK launches under Xvfb. It needs
at least 1,002,000 free inodes and approximately 3 GB of space; fixtures, databases
and JSON evidence remain available for inspection. Reusing an index name measures
startup reconciliation; a new name measures initial indexing. Do not run competing
heavy tests while measuring. See [PERFORMANCE.md](PERFORMANCE.md) for measured
results and the limits of virtualized-host measurements.
