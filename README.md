# syncer

[![CI](https://github.com/colinphill/syncer/actions/workflows/ci.yml/badge.svg)](https://github.com/colinphill/syncer/actions/workflows/ci.yml)

`syncer` is a one-shot C++20 tool that mirrors a Windows, macOS, or Linux directory into a managed Android shared-storage directory over ADB. Its native Android server is deployed and run only for the duration of a command; it does not install an APK or leave a daemon running.

Packaged server binaries are content-addressed when deployed, so an unchanged binary is reused on later commands instead of being pushed over ADB again.

Boost.Asio provides the synchronous TCP and Android local-socket transports. Boost.Program_options, Boost.JSON, and Boost.UUID provide CLI parsing, machine output, and session identifiers. CMake fetches the pinned Boost release when a suitable installation is unavailable.

## Safety model

- The destination must be an initialized subdirectory of Android shared storage. The shared-storage root itself is rejected.
- Internal shared storage is discovered from `EXTERNAL_STORAGE`, `/sdcard`, and standard
  `/storage` aliases so validation also works when an ADB-launched process has a sparse environment.
- `init` accepts a new or empty folder. Initializing an existing non-empty folder requires `--adopt`.
- Push quarantines destination-only and replaced entries under `.syncer/recovery/<run-id>` by
  default. Use `--dry-run` to inspect the plan, `--no-delete` to retain destination-only entries,
  or `--max-removals COUNT` to enforce a removal ceiling. Omitting `--max-removals` means no
  maximum; `--max-removals 0` forbids every planned removal.
- Protocol-6 recovery journals record installed as well as quarantined paths. `restore` removes
  the installed side, reinstates quarantined originals, and preserves displaced current content.
- `.syncer/` is reserved for destination identity, locking, and temporary uploads.
- Excluded paths are neither copied nor deleted.
- Files are compared by size and whole-second modification time, with a default 60-second tolerance for timestamp noise. Use `--mtime-tolerance SECONDS` to override it; `0` restores exact comparison. A same-size edit within the tolerance is intentionally not detected in v1.
- File modification times are applied where the shared-storage implementation permits it. A versioned logical timestamp catalog in `.syncer/` preserves comparison semantics when Android rejects timestamp updates from the ADB shell user. Directory modification times are ignored.
- By default, changed files are staged and BLAKE3-verified before planned destination changes begin. Replacement is atomic per file, but the complete tree update is not transactional.
- `--direct` streams each changed file straight to its destination without using temporary upload space. It creates no recovery run, journal, or quarantined copies, so replacements and deletions are permanent and cannot be restored. Each file is still BLAKE3-verified, but replacement is not atomic: a failed transfer removes the incomplete target, and earlier changes may already have been applied.

## Build

Requirements are CMake 3.22+, a C++20 compiler, Ninja (on macOS and Linux), an Android NDK supporting API 26, and Android platform-tools. Set `ANDROID_NDK_HOME` to the NDK directory when building the Android server. Boost 1.81+ may be installed locally; otherwise CMake fetches the pinned source release.

The managed client additionally requires the .NET 10 SDK:

```powershell
dotnet restore managed/Syncer.Managed.slnx
dotnet build managed/Syncer.Managed.slnx --configuration Release --no-restore
dotnet test managed/Syncer.Managed.slnx --configuration Release --no-build --no-restore
scripts/build-managed-packages.ps1
```

The last command expects the native packaging script to have produced all four Android servers. It
creates `Syncer.Client.0.2.0.nupkg` and the `Syncer.Tool` package under `out/package/nuget`. The tool
installs as `syncer-managed`; it intentionally does not replace the native `syncer` command.

Windows:

```powershell
cmake --preset windows
cmake --build --preset windows --config Release
ctest --test-dir out/build/windows -C Release --output-on-failure
scripts/build-package.ps1
```

Linux:

```sh
cmake --preset linux
cmake --build --preset linux
ctest --test-dir out/build/linux --output-on-failure
bash scripts/build-package.sh
```

macOS:

```sh
cmake --preset macos
cmake --build --preset macos
ctest --test-dir out/build/macos --output-on-failure
bash scripts/build-package.sh
```

The packaging scripts build `syncerd` for `arm64-v8a`, `armeabi-v7a`, `x86_64`, and `x86` and create the runtime layout under `out/package/`. The PowerShell script produces the Windows package; the shell script produces the package for its Linux or macOS host.

## CI/CD

GitHub Actions builds and tests the Windows, macOS, and Linux clients plus all four Android server ABIs for pushes and pull requests targeting `main`. The workflow uses Android NDK `27.3.13750724`, matching the supported local toolchain.

Pushing a tag matching both the CMake and managed project version, such as `v0.2.0`, creates a
Windows x64 release ZIP plus `Syncer.Client` and `Syncer.Tool` NuGet packages. The workflow
publishes those assets and their SHA-256 files in a GitHub release with generated notes. It can
also be run manually to produce a downloadable workflow artifact without publishing a release.

## Usage

```text
syncer init [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--adopt] DEVICE_PATH
syncer push [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--exclude GLOB]... [--mtime-tolerance SECONDS] [--max-removals COUNT] [--expect-plan DIGEST] [--save-plan FILE] [--no-delete] [--direct] [--dry-run] [--json] LOCAL_SOURCE DEVICE_PATH
syncer apply [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--json] --plan FILE
syncer restore [--adb PATH] [--serial SERIAL] [--cancel-stdin] [--json] --recovery ID DEVICE_PATH
syncer version
```

```powershell
syncer init /sdcard/MyMirror
syncer push --dry-run --save-plan sync-plan.json C:\work\assets /sdcard/MyMirror
syncer apply --plan sync-plan.json
syncer restore --recovery RECOVERY_ID /sdcard/MyMirror
syncer push --exclude "**/*.tmp" C:\work\assets /sdcard/MyMirror
syncer push --mtime-tolerance 0 C:\work\assets /sdcard/MyMirror
syncer push --direct C:\Music music
syncer init music
syncer push C:\Music music
```

`--save-plan` persists the complete ordered action list, exact source-file identities, selected
device, and affected destination-path preconditions. `syncer apply --plan` executes that artifact
without rescanning either complete tree; it validates the saved digest, the device identity, every
source file being transferred, and the affected destination paths before mutation. `--expect-plan`
remains available for callers that prefer a fresh full rescan. Successful staged mutations return
the recovery run identifier; direct mutations return no recovery identifier. `--max-removals`
counts destination-only entries and entries removed by a
type replacement; ordinary file updates do not consume the ceiling.

`syncer restore` accepts a recovery identifier returned by a successful protocol-6 mutation.
It refuses incomplete, legacy, already-restored, or structurally unsafe runs. Current paths that
must be displaced are retained under `.syncer/recovery/<run-id>/displaced`, making restore
non-destructive even if destination content changed after the sync.

Integrations may pass `--cancel-stdin` and write a line containing `cancel` to standard input.
The client interrupts its active protocol operation, waits for the Android server to release the
destination lock, removes the ADB forward, and exits with code 7. Closing stdin alone does not
cancel an operation.

As a convenience, `music` may be used instead of an absolute `DEVICE_PATH`. It searches mounted removable storage volumes under `/storage/<volume-id>` for a top-level folder named `music`, case-insensitively; it never selects internal emulated storage. `init music` creates `Music` when exactly one removable volume is mounted and no matching folder exists. Multiple removable volumes require an absolute path, and other relative device paths are rejected.

ADB is found from `--adb`, `ANDROID_HOME`/`ANDROID_SDK_ROOT`, Android Studio's default SDK directory for the host platform, or `PATH`. If multiple devices are ready, select one with `--serial` or `ANDROID_SERIAL`.

Exit codes are `0` success, `2` usage, `3` ADB/device, `4` safety/preflight, `5` protocol/transfer,
`6` source changed during transfer, and `7` cancelled. With `--json`, stdout contains one
schema-versioned result or error object; progress goes to stderr.

Before applying or dry-running a push, the client prints every selected change and the comparison reason to the progress stream. Reasons distinguish missing paths, type replacements, size or modification-time differences, and destination-only deletions. With `--json`, these explanations remain on stderr so stdout stays valid JSON.

See [docs/DESIGN.md](docs/DESIGN.md) for details and [docs/PLAN.md](docs/PLAN.md) for the saved implementation plan.
The complete protocol-6 wire contract is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).
