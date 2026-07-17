# ADB Folder Synchronizer Implementation Plan

Build a C++20, one-shot CLI that mirrors a Windows folder to a managed Android shared-storage folder. A command-scoped native Android server is selected by ABI, deployed under `/data/local/tmp/syncer/`, reached through an authenticated ADB-forwarded abstract socket, and shut down after each operation.

The implementation includes:

- A bounded, versioned binary protocol shared by the Windows client and Android server.
- Boost.Asio transport, Boost.Program_options CLI parsing, Boost.JSON output, and Boost.UUID session identifiers.
- Whole-file staging with BLAKE3 validation and atomic replacement.
- Size/whole-second-mtime comparison for files, source link following, empty-directory and file-mtime preservation, ignored directory mtimes, case-sensitive glob exclusions, and default deletion.
- A server-enforced shared-storage boundary, initialized `.syncer` marker, nonblocking root lock, path traversal checks, and explicit `--adopt` flow.
- Windows commands `init`, `push`, and `version`, stable exit categories, dry-run support, and a schema-versioned JSON summary.
- Android API 26+ builds for arm64-v8a, armeabi-v7a, x86_64, and x86, with portable core boundaries for later client platforms.

Deferred from v1 are pull and bidirectional sync, watch mode, persistent daemons, resumable or block-delta transfers, compression, metadata beyond mtimes, and checksum comparison for apparently unchanged files.
