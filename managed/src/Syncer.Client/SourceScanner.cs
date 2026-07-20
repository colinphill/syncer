namespace Syncer;

internal static class SourceScanner
{
    public static Task<SourceTree> ScanAsync(string requestedRoot, GlobSet exclusions, CancellationToken cancellationToken) =>
        Task.Run(() => Scan(requestedRoot, exclusions, cancellationToken), cancellationToken);

    public static long ModifiedSeconds(string path)
    {
        try { return new DateTimeOffset(File.GetLastWriteTimeUtc(path)).ToUnixTimeSeconds(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SyncerException(SyncerErrorCode.Safety, $"cannot inspect source entry: {path}", error); }
    }

    private static SourceTree Scan(string requestedRoot, GlobSet exclusions, CancellationToken cancellationToken)
    {
        string root;
        try { root = Path.GetFullPath(requestedRoot); if (!Directory.Exists(root)) throw new IOException(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        { throw new SyncerException(SyncerErrorCode.Safety, "source must be an accessible directory", error); }
        var manifest = new Dictionary<string, ManifestEntry>(StringComparer.Ordinal);
        var files = new Dictionary<string, SourceFile>(StringComparer.Ordinal);
        var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        var ancestors = new HashSet<string>(comparer);

        void Walk(FileSystemInfo input, string logical, bool isRoot)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!isRoot)
            {
                if (Planner.PathDepth(logical) == 1 && logical == ProtocolConstants.ReservedDirectory)
                    throw new SyncerException(SyncerErrorCode.Safety, "source contains the reserved top-level .syncer path");
                if (exclusions.MatchesExact(logical)) return;
            }
            FileSystemInfo effective = input;
            try
            {
                if ((input.Attributes & FileAttributes.ReparsePoint) != 0 &&
                    ShouldResolveReparsePoint(input.FullName))
                    effective = input.ResolveLinkTarget(true) ?? throw new IOException();
                effective.Refresh();
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { throw new SyncerException(SyncerErrorCode.Safety, $"source contains a broken symbolic link: {input.FullName}", error); }

            if ((effective.Attributes & FileAttributes.Directory) != 0)
            {
                var identity = Path.GetFullPath(effective.FullName).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                if (!ancestors.Add(identity)) throw new SyncerException(SyncerErrorCode.Safety, $"source contains a recursive symbolic link or junction at: {logical}");
                if (!isRoot) manifest.Add(logical, new(logical, EntryType.Directory, 0, 0));
                try
                {
                    foreach (var child in new DirectoryInfo(effective.FullName).EnumerateFileSystemInfos())
                    {
                        var name = child.Name; var childLogical = logical.Length == 0 ? name : logical + "/" + name;
                        Walk(child, childLogical, false);
                    }
                }
                catch (SyncerException) { throw; }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                { throw new SyncerException(SyncerErrorCode.Safety, $"cannot enumerate source directory: {effective.FullName}", error); }
                finally { ancestors.Remove(identity); }
            }
            else if (effective is FileInfo file && file.Exists)
            {
                var entry = new ManifestEntry(logical, EntryType.File, checked((ulong)file.Length), new DateTimeOffset(file.LastWriteTimeUtc).ToUnixTimeSeconds());
                manifest.Add(logical, entry); files.Add(logical, new(entry, effective.FullName));
            }
            else throw new SyncerException(SyncerErrorCode.Safety, $"source contains an unsupported file type: {logical}");
        }
        Walk(new DirectoryInfo(root), string.Empty, true);
        return new SourceTree(manifest, files);
    }

    // Windows exposes DFS/SMB namespace entries as reparse points. ResolveLinkTarget can turn
    // those UNC paths into malformed application-relative "UNC\server\share" paths; enumerating
    // the original DirectoryInfo lets Windows follow the network namespace correctly.
    internal static bool ShouldResolveReparsePoint(string fullName) =>
        !(OperatingSystem.IsWindows() && IsUncPath(fullName));

    private static bool IsUncPath(string path) =>
        path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase) ||
        path.StartsWith(@"\\", StringComparison.Ordinal) &&
        !path.StartsWith(@"\\?\", StringComparison.Ordinal) &&
        !path.StartsWith(@"\\.\", StringComparison.Ordinal);
}
