# Syncer wire protocol 6

This document is the wire contract shared by the native Android server and the native and managed
host clients. Protocol 6 is intentionally strict: an implementation rejects another version rather
than attempting feature negotiation.

## Framing and primitive values

Every frame starts with a 16-byte header. All integers in the header and payload are unsigned,
fixed-width, and big-endian unless explicitly described as signed.

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | Magic `SYNC` (`0x53594e43`) |
| 4 | 2 | Protocol version (`6`) |
| 6 | 2 | Message type |
| 8 | 4 | Request identifier |
| 12 | 4 | Payload byte length |

Payloads may not exceed 2 MiB. A string is a `u32` byte length followed by that many strict UTF-8
bytes. `i64` values use two's-complement representation. A manifest entry is `string path`, `u8
type`, `u64 size`, `i64 modified_seconds`; types are file `1`, directory `2`, and other `3`.
Relative paths use `/`, are nonempty, and may not contain empty, `.`, or `..` components.

Responses repeat the initiating request identifier. Upload chunks are the only one-way request.
An `operation_ok` has an empty payload. An `error` contains `u32 exit_code`, `string message`, using
the stable exit categories 2 through 7 documented in the README.

## Messages

| ID | Name | Payload | Response |
|---:|---|---|---|
| 1 | `hello` | `string token`, `string client_version` | `hello_ack` |
| 2 | `hello_ack` | `string server_version` | — |
| 3 | `initialize` | `string device_path`, `u8 adopt` | `operation_ok` |
| 4 | `scan` | `string device_path` | manifest stream |
| 5 | `manifest_entry` | one manifest entry | — |
| 6 | `manifest_end` | empty | — |
| 7 | `make_directory` | `string path`, `i64 modified_seconds` | `operation_ok` |
| 8 | `put_begin` | `string path`, `u64 size`, `i64 modified_seconds`, `u8 placement` | `operation_ok` |
| 9 | `put_chunk` | raw file bytes | none |
| 10 | `put_end` | 32-byte BLAKE3 digest | `operation_ok` |
| 11 | `commit_file` | `string path` | `operation_ok` |
| 12 | `delete_path` | `string path` | `operation_ok` |
| 13 | `set_mtime` | `string path`, `i64 modified_seconds` | `operation_ok` |
| 14 | `operation_ok` | empty | — |
| 15 | `error` | `u32 exit_code`, `string message` | — |
| 16 | `shutdown` | empty | `operation_ok` |
| 17 | `manifest_batch` | `u32 count`, then `count` manifest entries | — |
| 18 | `prepare` | `string recovery_id` | `operation_ok` |
| 19 | `select_root` | `string device_path` | `operation_ok` |
| 20 | `validate_plan` | `u32 count`, then preconditions | `operation_ok` |
| 21 | `restore` | `string recovery_id` | `operation_ok` |

Upload placement is staged `0` or direct `1`. A precondition is `string path`, `u8 exists`, followed
by a manifest entry when `exists` is nonzero. Large precondition collections are split across
multiple `validate_plan` requests. A scan returns zero or more `manifest_batch` frames (legacy
single `manifest_entry` is also accepted), followed by `manifest_end`, all with the scan request ID.

## Required sequences

The first frame is always `hello`, authenticated with the random token passed to `syncerd` on its
command line. Normal push is `scan`, `prepare`, staged uploads, replacement quarantine,
directory creation, file commits, destination-only quarantine, and `shutdown`. Direct mode omits
`prepare`, permanently removes replaced and destination-only paths, places directory creation
before uploads, and does not send `commit_file`. Saved-plan execution uses
`select_root`, one or more `validate_plan` requests, then the same mutation sequence. Restore uses
`select_root`, `restore`, and `shutdown`.

The cross-language byte and plan-digest fixtures are in `tests/protocol-v6-vectors.txt`. Both native
and managed tests consume that file.
