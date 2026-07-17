# syncer

[![CI](https://github.com/colinphill/syncer/actions/workflows/ci.yml/badge.svg)](https://github.com/colinphill/syncer/actions/workflows/ci.yml)

`syncer` is a one-shot C++20 tool that mirrors a Windows directory into a managed Android shared-storage directory over ADB. Its native Android server is deployed and run only for the duration of a command; it does not install an APK or leave a daemon running.

Packaged server binaries are content-addressed when deployed, so an unchanged binary is reused on later commands instead of being pushed over ADB again.

Boost.Asio provides the synchronous TCP and Android local-socket transports. Boost.Program_options, Boost.JSON, and Boost.UUID provide CLI parsing, machine output, and session identifiers. CMake fetches the pinned Boost release when a suitable installation is unavailable.

## Safety model

- The destination must be an initialized subdirectory of Android shared storage. The shared-storage root itself is rejected.
- `init` accepts a new or empty folder. Initializing an existing non-empty folder requires `--adopt`.
- Push deletes destination-only entries by default. Use `--dry-run` to inspect the plan or `--no-delete` to retain them.
- `.syncer/` is reserved for destination identity, locking, and temporary uploads.
- Excluded paths are neither copied nor deleted.
- Files are compared by size and whole-second modification time, with a default 60-second tolerance for timestamp noise. Use `--mtime-tolerance SECONDS` to override it; `0` restores exact comparison. A same-size edit within the tolerance is intentionally not detected in v1.
- File modification times are applied where the shared-storage implementation permits it. A versioned logical timestamp catalog in `.syncer/` preserves comparison semantics when Android rejects timestamp updates from the ADB shell user. Directory modification times are ignored.
- Changed files are staged and BLAKE3-verified before planned destination changes begin. Replacement is atomic per file, but the complete tree update is not transactional.

## Build

Requirements are CMake 3.22+, Visual Studio with C++ support, an Android NDK supporting API 26, Ninja, and Android platform-tools. Set `ANDROID_NDK_HOME` to the NDK directory. Boost 1.81+ may be installed locally; otherwise CMake fetches the pinned source release.

```powershell
cmake --preset windows
cmake --build --preset windows --config Release
ctest --test-dir out/build/windows -C Release --output-on-failure
scripts/build-package.ps1
```

The packaging script builds `syncerd` for `arm64-v8a`, `armeabi-v7a`, `x86_64`, and `x86` and creates the runtime layout under `out/package/`.

## CI/CD

GitHub Actions builds and tests the Windows client plus all four Android server ABIs for pushes and pull requests targeting `main`. The workflow uses Android NDK `27.3.13750724`, matching the supported local toolchain.

Pushing a tag matching the CMake project version, such as `v0.1.0`, creates a Windows x64 release ZIP and publishes it as a GitHub release with generated notes. The Release workflow can also be run manually to produce a downloadable workflow artifact without publishing a GitHub release.

## Usage

```text
syncer init [--adb PATH] [--serial SERIAL] [--adopt] DEVICE_PATH
syncer push [--adb PATH] [--serial SERIAL] [--exclude GLOB]... [--mtime-tolerance SECONDS] [--no-delete] [--dry-run] [--json] LOCAL_SOURCE DEVICE_PATH
syncer version
```

```powershell
syncer init /sdcard/MyMirror
syncer push --dry-run C:\work\assets /sdcard/MyMirror
syncer push --exclude "**/*.tmp" C:\work\assets /sdcard/MyMirror
syncer push --mtime-tolerance 0 C:\work\assets /sdcard/MyMirror
syncer init music
syncer push C:\Music music
```

As a convenience, `music` may be used instead of an absolute `DEVICE_PATH`. It searches mounted removable storage volumes under `/storage/<volume-id>` for a top-level folder named `music`, case-insensitively; it never selects internal emulated storage. `init music` creates `Music` when exactly one removable volume is mounted and no matching folder exists. Multiple removable volumes require an absolute path, and other relative device paths are rejected.

ADB is found from `--adb`, `ANDROID_HOME`/`ANDROID_SDK_ROOT`, Android Studio's default SDK directory on Windows, or `PATH`. If multiple devices are ready, select one with `--serial` or `ANDROID_SERIAL`.

Exit codes are `0` success, `2` usage, `3` ADB/device, `4` safety/preflight, `5` protocol/transfer, and `6` source changed during transfer. With `--json`, stdout contains one schema-versioned result or error object; progress goes to stderr.

Before applying or dry-running a push, the client prints every selected change and the comparison reason to the progress stream. Reasons distinguish missing paths, type replacements, size or modification-time differences, and destination-only deletions. With `--json`, these explanations remain on stderr so stdout stays valid JSON.

See [docs/DESIGN.md](docs/DESIGN.md) for details and [docs/PLAN.md](docs/PLAN.md) for the saved implementation plan.
