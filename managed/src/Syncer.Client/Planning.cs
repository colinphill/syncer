namespace Syncer;

internal sealed class GlobSet
{
    private readonly string[] patterns;
    public GlobSet(IEnumerable<string> patterns) => this.patterns = patterns.Select(Normalize).ToArray();
    private static string Normalize(string pattern)
    {
        pattern = pattern.Replace('\\', '/');
        while (pattern.StartsWith("./", StringComparison.Ordinal)) pattern = pattern[2..];
        return pattern.TrimEnd('/');
    }
    public bool MatchesExact(string path) => patterns.Any(pattern => GlobMatch(pattern, path));
    public bool Matches(string path)
    {
        if (MatchesExact(path)) return true;
        for (var slash = path.LastIndexOf('/'); slash >= 0; slash = slash == 0 ? -1 : path.LastIndexOf('/', slash - 1))
            if (patterns.Any(pattern => GlobMatch(pattern, path[..slash]))) return true;
        return false;
    }
    internal static bool GlobMatch(string pattern, string path)
    {
        var patternBytes = ProtocolWriter.Utf8.GetBytes(pattern); var pathBytes = ProtocolWriter.Utf8.GetBytes(path);
        var memo = new sbyte[(patternBytes.Length + 1) * (pathBytes.Length + 1)]; Array.Fill(memo, (sbyte)-1);
        bool Match(int p, int s)
        {
            var key = p * (pathBytes.Length + 1) + s;
            if (memo[key] >= 0) return memo[key] != 0;
            bool result;
            if (p == patternBytes.Length) result = s == pathBytes.Length;
            else if (patternBytes[p] == (byte)'*')
            {
                var doubleStar = p + 1 < patternBytes.Length && patternBytes[p + 1] == (byte)'*'; var next = p + (doubleStar ? 2 : 1);
                result = Match(next, s);
                if (!result && doubleStar && next < patternBytes.Length && patternBytes[next] == (byte)'/') result = Match(next + 1, s);
                if (!result && s < pathBytes.Length && (doubleStar || pathBytes[s] != (byte)'/')) result = Match(p, s + 1);
            }
            else if (s < pathBytes.Length && patternBytes[p] == (byte)'?') result = pathBytes[s] != (byte)'/' && Match(p + 1, s + 1);
            else result = s < pathBytes.Length && patternBytes[p] == pathBytes[s] && Match(p + 1, s + 1);
            memo[key] = result ? (sbyte)1 : (sbyte)0; return result;
        }
        return Match(0, 0);
    }
}

internal sealed record PlanParts(
    IReadOnlyList<PlannedChange> Directories,
    IReadOnlyList<PlannedChange> Files,
    IReadOnlyList<PlannedChange> Deletions,
    IReadOnlyList<PlannedChange> Timestamps,
    ulong TransferBytes);

internal static class Planner
{
    public static bool IsValidRelativePath(string path)
    {
        if (string.IsNullOrEmpty(path) || path[0] == '/' || path[^1] == '/' || path.Contains('\\') || path.Contains('\0')) return false;
        return path.Split('/').All(part => part.Length != 0 && part != "." && part != "..");
    }
    public static int PathDepth(string path) => 1 + path.Count(character => character == '/');

    public static PlanParts MakePlan(
        IReadOnlyDictionary<string, ManifestEntry> source,
        IReadOnlyDictionary<string, ManifestEntry> destination,
        GlobSet exclusions, bool deleteExtras, ulong mtimeToleranceSeconds,
        IReadOnlySet<string>? untrackedDestinationTimes = null)
    {
        var directories = new List<PlannedChange>(); var files = new List<PlannedChange>(); var deletions = new List<PlannedChange>();
        var timestamps = new List<PlannedChange>(); ulong bytes = 0;
        foreach (var (path, sourceEntry) in source)
        {
            var exists = destination.TryGetValue(path, out var destinationEntry);
            var changedType = exists && destinationEntry!.Type != sourceEntry.Type;
            if (sourceEntry.Type == EntryType.Directory)
            {
                if (!exists) directories.Add(new(sourceEntry, PlanAction.CreateDirectory, "destination path is missing"));
                else if (changedType) directories.Add(new(sourceEntry, PlanAction.ReplaceWithDirectory, $"destination path is a {TypeName(destinationEntry!.Type)}"));
            }
            else if (sourceEntry.Type == EntryType.File)
            {
                PlannedChange? change = null;
                if (!exists) change = new(sourceEntry, PlanAction.AddFile, "destination path is missing");
                else if (changedType) change = new(sourceEntry, PlanAction.ReplaceWithFile, $"destination path is a {TypeName(destinationEntry!.Type)}");
                else
                {
                    var untracked = untrackedDestinationTimes?.Contains(path) == true;
                    var difference = MtimeDifference(sourceEntry.ModifiedSeconds, destinationEntry!.ModifiedSeconds);
                    if (destinationEntry.Size != sourceEntry.Size ||
                        (difference > mtimeToleranceSeconds && !(untracked && MatchesDaylightSavingShift(difference, mtimeToleranceSeconds))))
                        change = new(sourceEntry, PlanAction.UpdateFile, MetadataReason(sourceEntry, destinationEntry, mtimeToleranceSeconds));
                    else if (untracked)
                        timestamps.Add(new(sourceEntry, PlanAction.RecordTimestamp, difference > mtimeToleranceSeconds
                            ? DaylightSavingShiftReason(sourceEntry, destinationEntry, mtimeToleranceSeconds) : UntrackedTimeReason));
                }
                if (change is not null) { files.Add(change); bytes += sourceEntry.Size; }
            }
        }
        if (deleteExtras)
            foreach (var (path, entry) in destination)
                if (!source.ContainsKey(path) && !exclusions.Matches(path))
                    deletions.Add(new(entry, DeletionAction(entry.Type), "path exists only at the destination and deletion is enabled"));
        directories.Sort((left, right) => { var depth = PathDepth(left.Path).CompareTo(PathDepth(right.Path)); return depth != 0 ? depth : Utf8PathComparer.Instance.Compare(left.Path, right.Path); });
        files.Sort((left, right) => Utf8PathComparer.Instance.Compare(left.Path, right.Path));
        timestamps.Sort((left, right) => Utf8PathComparer.Instance.Compare(left.Path, right.Path));
        deletions.Sort((left, right) => { var depth = PathDepth(right.Path).CompareTo(PathDepth(left.Path)); return depth != 0 ? depth : Utf8PathComparer.Instance.Compare(right.Path, left.Path); });
        return new(directories, files, deletions, timestamps, bytes);
    }

    public static IReadOnlyList<DestinationPrecondition> BuildPreconditions(PlanParts plan, IReadOnlyDictionary<string, ManifestEntry> destination)
    {
        var values = new SortedDictionary<string, DestinationPrecondition>(Utf8PathComparer.Instance);
        void Add(string path) { if (path.Length != 0 && !values.ContainsKey(path)) values[path] = destination.TryGetValue(path, out var entry) ? new(path, true, entry) : new(path, false, null); }
        void AddParents(string path) { Add(path); while (path.LastIndexOf('/') is var slash && slash >= 0) { path = path[..slash]; Add(path); } }
        void AddChange(PlannedChange change)
        {
            AddParents(change.Path);
            if (change.Action is PlanAction.ReplaceWithDirectory or PlanAction.ReplaceWithFile)
                foreach (var path in destination.Keys.Where(path => path.StartsWith(change.Path + "/", StringComparison.Ordinal))) Add(path);
        }
        foreach (var change in plan.Directories) AddChange(change);
        foreach (var change in plan.Files) AddChange(change);
        foreach (var change in plan.Deletions) AddChange(change);
        foreach (var change in plan.Timestamps) AddChange(change);
        return values.Values.ToArray();
    }

    public static ulong RemovalCount(PlanParts plan, IReadOnlyDictionary<string, ManifestEntry> destination)
    {
        var removals = new HashSet<string>(plan.Deletions.Select(change => change.Path), StringComparer.Ordinal);
        void AddConflict(PlannedChange change)
        {
            if (change.Action is not (PlanAction.ReplaceWithDirectory or PlanAction.ReplaceWithFile)) return;
            foreach (var path in destination.Keys.Where(path => path == change.Path || path.StartsWith(change.Path + "/", StringComparison.Ordinal))) removals.Add(path);
        }
        foreach (var change in plan.Directories) AddConflict(change); foreach (var change in plan.Files) AddConflict(change);
        return (ulong)removals.Count;
    }

    /// <summary>
    /// Destination filesystems that store local time (FAT, exFAT without a valid UTC offset)
    /// reinterpret stored times when the device's daylight-saving offset changes.
    /// </summary>
    internal const ulong DaylightSavingShiftSeconds = 3600;
    private const string UntrackedTimeReason = "destination modification time is not recorded in the timestamp catalog";
    private static bool MatchesDaylightSavingShift(ulong difference, ulong tolerance) =>
        (difference >= DaylightSavingShiftSeconds ? difference - DaylightSavingShiftSeconds : DaylightSavingShiftSeconds - difference) <= tolerance;
    private static string DaylightSavingShiftReason(ManifestEntry source, ManifestEntry destination, ulong tolerance) =>
        $"destination modification time differs by {MtimeDifference(source.ModifiedSeconds, destination.ModifiedSeconds)} seconds " +
        $"(source {source.ModifiedSeconds}, destination {destination.ModifiedSeconds}), matching a one-hour daylight-saving shift within the tolerance of {tolerance} seconds";
    private static ulong MtimeDifference(long left, long right) => left >= right ? (ulong)left - (ulong)right : (ulong)right - (ulong)left;
    private static string MetadataReason(ManifestEntry source, ManifestEntry destination, ulong tolerance)
    {
        var reasons = new List<string>();
        if (source.Size != destination.Size) reasons.Add($"size differs (source {source.Size} bytes, destination {destination.Size} bytes)");
        var difference = MtimeDifference(source.ModifiedSeconds, destination.ModifiedSeconds);
        if (difference > tolerance) reasons.Add($"modification time differs by {difference} seconds (source {source.ModifiedSeconds}, destination {destination.ModifiedSeconds}; tolerance {tolerance} seconds)");
        return string.Join("; ", reasons);
    }
    private static string TypeName(EntryType type) => type switch { EntryType.File => "file", EntryType.Directory => "directory", _ => "unsupported entry" };
    private static PlanAction DeletionAction(EntryType type) => type switch { EntryType.File => PlanAction.DeleteFile, EntryType.Directory => PlanAction.DeleteDirectory, _ => PlanAction.DeleteOther };
}

internal sealed class Utf8PathComparer : IComparer<string>
{
    public static Utf8PathComparer Instance { get; } = new();
    public int Compare(string? left, string? right)
    {
        if (ReferenceEquals(left, right)) return 0; if (left is null) return -1; if (right is null) return 1;
        var leftBytes = ProtocolWriter.Utf8.GetBytes(left); var rightBytes = ProtocolWriter.Utf8.GetBytes(right); var length = Math.Min(leftBytes.Length, rightBytes.Length);
        for (var index = 0; index < length; ++index) { var result = leftBytes[index].CompareTo(rightBytes[index]); if (result != 0) return result; }
        return leftBytes.Length.CompareTo(rightBytes.Length);
    }
}
