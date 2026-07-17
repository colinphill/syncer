# Design

## Command lifecycle

For each `init` or `push`, the client resolves one ready ADB device, queries its ABI, deploys the matching `syncerd`, and creates an ADB forward from an allocated localhost TCP port to a random Android abstract Unix socket. Deployments are content-addressed with BLAKE3, allowing an unchanged server binary already on the device to be reused. The server receives a random 256-bit token on its fixed-safe command line and requires it in the first framed message. It accepts one connection and applies a 30-second socket idle timeout.

Boost.Asio owns the Windows TCP socket and Android local stream acceptor/socket. The framing functions are templates over Asio synchronous stream concepts, keeping the binary protocol transport-independent. User-controlled destination paths never appear in an Android shell command; they travel in bounded frames and are validated server-side.

## Wire protocol

Every frame begins with a 16-byte, network-byte-order header:

| Field | Type |
|---|---|
| Magic (`SYNC`) | `uint32` |
| Protocol version | `uint16` |
| Message type | `uint16` |
| Request ID | `uint32` |
| Payload length | `uint32` |

Payloads are capped at 2 MiB. Strings are UTF-8 prefixed by a `uint32` length. Integers are fixed-width and big-endian. Manifest paths use `/`, are relative, and cannot contain empty, `.` or `..` components.

The sequence is handshake, initialize or scan, mutation preparation, staged uploads, directory creation, file commits, deletion, directory timestamp restoration, and shutdown. Destination manifest entries are batched into bounded frames. Upload chunks are one-way frames; `put_end` responds after declared size and BLAKE3 verification.

## Filesystem transaction

Source scanning finishes before device mutation. Links and Windows junctions are followed at their logical link paths. Canonical directory identities on the active recursion ancestry detect cycles while allowing one external tree at multiple logical paths.

The Android scan never follows destination links. It enumerates independent directories with up to four bounded workers, while a single thread serializes manifest batches to the client. Uploads are staged under `.syncer/tmp`. Only after all changed files stage successfully does the client create required directories, commit files, and delete extras deepest-first. Failures can leave earlier committed entries updated, but never expose a partially written file.

The server canonicalizes destinations and requires them strictly below internal shared storage or a mounted removable volume exposed as `/storage/<volume-id>`. The relative alias `music` searches only removable volumes, case-insensitively, and never falls back to `/storage/emulated`; all other relative paths are rejected. It rejects linked parents while resolving operation paths and reserves `.syncer`. A visible `.syncer/lock` marker is paired with a path-keyed `flock(LOCK_EX|LOCK_NB)` file under `/data/local/tmp/syncer/locks`; the native backing filesystem is required because recent Android emulated-storage implementations reject advisory locking.

The server attempts to apply source file modification times directly. When an Android shared-storage implementation rejects timestamp changes, `.syncer/timestamps` records versioned logical file times. Destination scans use a logical time only while the entry type and file size still match, preventing needless retransfers while retaining detection of ordinary external changes. Planning ignores file modification-time differences of up to 60 seconds by default; `--mtime-tolerance` overrides that threshold. Directory modification times are normalized to zero during scans and are never compared, applied, or cataloged.

## Release layout

```text
syncer.exe
servers/
  arm64-v8a/syncerd
  armeabi-v7a/syncerd
  x86_64/syncerd
  x86/syncerd
```
