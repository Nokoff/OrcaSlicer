# Repository Guidelines

## Project Structure & Module Organization
Snapmaker_Orca’s C++17 sources live in `src/`, split by feature modules and platform adapters. User assets, icons, and printer presets are in `resources/`; translations stay in `localization/`. Tests sit in `tests/`, grouped by domain (`libslic3r/`, `sla_print/`, etc.) with fixtures under `tests/data/`. CMake helpers reside in `cmake/`, and longer references in `doc/` and `SoftFever_doc/`. Automation scripts belong in `scripts/` and `tools/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots—do not modify without mirroring upstream tags.

## Build, Test, and Development Commands
Use out-of-source builds:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` configures dependencies and generates build files.
- `cmake --build build --target Snapmaker_Orca --config Release` compiles the app; add `--parallel` to speed up.
- `cmake --build build --target tests` then `ctest --test-dir build --output-on-failure` runs automated suites.
Platform helpers such as `build_linux.sh`, `build_release_macos.sh`, and `build_release_vs2022.bat` wrap the same flow with toolchain flags. Use `build_release_macos.sh -sx` when reproducing macOS build issues, and `scripts/DockerBuild.sh` for reproducible container builds.

## Coding Style & Naming Conventions
`.clang-format` enforces 4-space indents, a 140-column limit, aligned initializers, and brace wrapping for classes and functions. Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on your PATH. Prefer `CamelCase` for classes, `snake_case` for functions and locals, and `SCREAMING_CASE` for constants, matching conventions in `src/`. Keep headers self-contained and align include order with the IWYU pragmas.

## Testing Guidelines
Unit tests rely on Catch2 (`tests/catch2/`). Name specs after the component under test—for example `tests/libslic3r/TestPlanarHole.cpp`—and tag long-running cases so `ctest -L fast` remains useful. Cover new algorithms with deterministic fixtures or sample G-code stored in `tests/data/`. Document manual printer validation or regression slicer checks in your PR when automated coverage is insufficient.

## Commit & Pull Request Guidelines
The history favors concise, sentence-style subject lines with optional issue references, e.g., `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR. Complete `.github/pull_request_template.md`, include reproduction steps or screenshots for UI changes, and mention impacted presets or translations. Link issues via `Closes #NNNN` when applicable, and call out dependency bumps or profile migrations for maintainer review.

## Security & Configuration Tips
Follow `SECURITY.md` for vulnerability reporting. Keep API tokens and printer credentials out of tracked configs; use `sandboxes/` for experimental settings. When touching third-party code in `deps_src/`, record the upstream commit or release in your PR description and run the relevant platform build script to confirm integration.

## Cursor Cloud specific instructions

Snapmaker_Orca is a single monolithic desktop GUI slicer (wxWidgets + OpenGL). There are no companion servers/daemons/databases to start — the product is the one `snapmaker-orca` process. All networking (OctoPrint, MQTT, Bonjour, cloud login) is outbound/optional and not needed to slice.

### Build model (heavy, two-phase, snapshotted)
- Build is two-phase and slow on 4 cores: vendored deps (`bash build_linux.sh -dr`, ~14 min) then the slicer (`bash build_linux.sh -str`, ~17 min). Both `deps/build/` and `build/` are baked into the VM snapshot, so a fresh cloud session normally already has them — do an incremental `cmake --build build --config Release --target Snapmaker_Orca` after pulling code rather than a full rebuild.
- Standard build/test commands live in `README`/`CLAUDE.md`/this file's "Build, Test, and Development Commands"; `build_linux.sh -u` installs system deps (already done in the snapshot).
- Non-obvious compiler gotcha: on this Ubuntu 24.04 image the default `cc`/`c++` resolve to **clang 18** (not gcc), and CMake auto-picks it. clang needs `libstdc++-14-dev` (installed in the snapshot); without it linking fails with `cannot find -lstdc++`. `build_linux.sh` only forces gcc/clang explicitly when you pass `-l`.
- `build_linux.sh` requires ≥10 GB free RAM; pass `-r` to skip the RAM/disk precheck.

### Tests
- There is no `tests` meta-target. Build the executables directly, e.g. `cmake --build build --config Release --target libslic3r_tests fff_print_tests libnest2d_tests slic3rutils_tests`.
- Run with `ctest --test-dir build -C Release --output-on-failure` (must pass `-C Release`; this is a Ninja Multi-Config tree). A chunk of tests fail out of the box (`libnest2d` arrange tests, several `fff_print` "Objects could not fit on the bed" cases, and a couple SEGSEGVs) — these are pre-existing in the fork and CI does not run `ctest` (workflows only build). Treat those specific failures as expected, not environment breakage.

### Lint
- Lint = `clang-format` (config in `.clang-format`, clang-format 18 installed). Existing sources are not fully formatted, so only run `clang-format -i` on files you touch; a repo-wide `--dry-run --Werror` will report many pre-existing violations.

### Running the app headlessly
- A desktop X server is available on `DISPLAY=:1` with software OpenGL (Mesa `llvmpipe`, GL 4.5), which is enough for the GUI and 3D viewport. Launch with `cd build/package && DISPLAY=:1 WEBKIT_DISABLE_DMABUF_RENDERER=1 GDK_BACKEND=x11 ./snapmaker-orca` (the `package/` launcher sets `LC_ALL=C` and library paths; `WEBKIT_DISABLE_DMABUF_RENDERER=1` avoids WebKit rendering issues under software GL). First run shows SSL-cert / region / EULA / printer wizards.
- For headless slicing without the GUI wizard, the same binary has a CLI: `./snapmaker-orca --slice 0 --export-3mf out.3mf --load-settings "machine.json;process.json" --load-filaments "filament.json" model.stl`. Exported Snapmaker sliced files are `.gcode.3mf` containers (the raw G-code is at `Metadata/plate_1.gcode` inside the zip).
- Binaries after a build: `build/package/bin/snapmaker-orca` (packaged, use the `build/package/snapmaker-orca` launcher), `build/src/Release/snapmaker-orca`, and the CLI validator `build/src/Release/Snapmaker_Orca_profile_validator`.
