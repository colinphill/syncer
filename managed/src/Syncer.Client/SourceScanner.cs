using System.IO.Enumeration;

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
        var manifest = new Dictionary<string, ManifestEntry>(4096, StringComparer.Ordinal);
        var files = new Dictionary<string, SourceFile>(4096, StringComparer.Ordinal);
        var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        var ancestors = new HashSet<string>(comparer);

        void Walk(ScanEntry input, string logical, bool isRoot)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!isRoot)
            {
                if (Planner.PathDepth(logical) == 1 && logical == ProtocolConstants.ReservedDirectory)
                    throw new SyncerException(SyncerErrorCode.Safety, "source contains the reserved top-level .syncer path");
                if (exclusions.MatchesExact(logical)) return;
            }
            string physical = input.FullPath;
            var attributes = input.Attributes;
            ulong size = input.Size;
            DateTime modifiedUtc = input.ModifiedUtc;
            try
            {
                if ((attributes & FileAttributes.ReparsePoint) != 0 &&
                    ShouldResolveReparsePoint(physical))
                {
                    FileSystemInfo link = (attributes & FileAttributes.Directory) != 0
                        ? new DirectoryInfo(physical)
                        : new FileInfo(physical);
                    FileSystemInfo effective = link.ResolveLinkTarget(true) ?? throw new IOException();
                    effective.Refresh();
                    physical = effective.FullName;
                    attributes = effective.Attributes;
                    size = effective is FileInfo file ? checked((ulong)file.Length) : 0;
                    modifiedUtc = effective.LastWriteTimeUtc;
                }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { throw new SyncerException(SyncerErrorCode.Safety, $"source contains a broken symbolic link: {input.FullPath}", error); }

            if ((attributes & FileAttributes.Directory) != 0)
            {
                var identity = Path.GetFullPath(physical).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                if (!ancestors.Add(identity)) throw new SyncerException(SyncerErrorCode.Safety, $"source contains a recursive symbolic link or junction at: {logical}");
                if (!isRoot) manifest.Add(logical, new(logical, EntryType.Directory, 0, 0));
                try
                {
                    foreach (var child in Enumerate(physical))
                    {
                        var name = child.Name; var childLogical = logical.Length == 0 ? name : logical + "/" + name;
                        Walk(child, childLogical, false);
                    }
                }
                catch (SyncerException) { throw; }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                { throw new SyncerException(SyncerErrorCode.Safety, $"cannot enumerate source directory: {physical}", error); }
                finally { ancestors.Remove(identity); }
            }
            else
            {
                var entry = new ManifestEntry(logical, EntryType.File, size, new DateTimeOffset(modifiedUtc).ToUnixTimeSeconds());
                manifest.Add(logical, entry); files.Add(logical, new(entry, physical));
            }
        }

        var rootInfo = new DirectoryInfo(root);
        try { rootInfo.Refresh(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SyncerException(SyncerErrorCode.Safety, "source must be an accessible directory", error); }
        Walk(new(rootInfo.FullName, rootInfo.Name, rootInfo.Attributes, 0, rootInfo.LastWriteTimeUtc), string.Empty, true);
        return new SourceTree(manifest, files);
    }

    private static FileSystemEnumerable<ScanEntry> Enumerate(string directory) =>
        new(directory, TransformEntry, new EnumerationOptions
        {
            RecurseSubdirectories = false,
            BufferSize = 64 * 1024,
            IgnoreInaccessible = false,
            AttributesToSkip = 0,
        });

    private static ScanEntry TransformEntry(ref FileSystemEntry entry)
    {
        var attributes = entry.Attributes;
        return new(entry.ToFullPath(), entry.FileName.ToString(), attributes,
            (attributes & FileAttributes.Directory) != 0 ? 0 : checked((ulong)entry.Length),
            entry.LastWriteTimeUtc.UtcDateTime);
    }

    private readonly record struct ScanEntry(
        string FullPath,
        string Name,
        FileAttributes Attributes,
        ulong Size,
        DateTime ModifiedUtc);

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
