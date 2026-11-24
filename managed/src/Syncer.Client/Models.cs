namespace Syncer;

public enum SyncerErrorCode
{
    Success = 0,
    Usage = 2,
    Adb = 3,
    Safety = 4,
    Transfer = 5,
    SourceChanged = 6,
    Cancelled = 7,
}

public sealed class SyncerException : Exception
{
    public SyncerException(SyncerErrorCode errorCode, string message) : base(message) => ErrorCode = errorCode;
    public SyncerException(SyncerErrorCode errorCode, string message, Exception innerException)
        : base(message, innerException) => ErrorCode = errorCode;
    public SyncerErrorCode ErrorCode { get; }
}

public sealed record SyncerClientOptions
{
    public string? AdbPath { get; init; }
    public string? DeviceSerial { get; init; }
    public string? ServerDirectory { get; init; }
}

/// <summary>Describes an Android device reported by ADB.</summary>
public sealed record SyncerDevice(
    string Id,
    string Serial,
    string DisplayName,
    string State,
    string? Model = null,
    string? Product = null,
    string? Device = null,
    string? TransportId = null,
    string? Connection = null)
{
    /// <summary>Gets whether ADB reports the device as ready for synchronization.</summary>
    public bool IsReady => StringComparer.Ordinal.Equals(State, "device");
}

public sealed record InitializeOptions
{
    public bool Adopt { get; init; }
}

public sealed record PushOptions
{
    public IReadOnlyList<string> Exclusions { get; init; } = Array.Empty<string>();
    public TimeSpan ModificationTimeTolerance { get; init; } = TimeSpan.FromSeconds(60);
    public ulong? MaximumRemovals { get; init; }
    public bool DeleteExtraneous { get; init; } = true;
    public bool Direct { get; init; }
    public string? ExpectedPlanDigest { get; init; }
}

public enum EntryType : byte { File = 1, Directory = 2, Other = 3 }

public sealed record ManifestEntry(string Path, EntryType Type, ulong Size, long ModifiedSeconds);

public enum PlanAction : byte
{
    CreateDirectory,
    ReplaceWithDirectory,
    AddFile,
    UpdateFile,
    ReplaceWithFile,
    DeleteFile,
    DeleteDirectory,
    DeleteOther,
    RecordTimestamp,
}

public sealed record PlannedChange(ManifestEntry Entry, PlanAction Action, string Reason)
{
    public string Path => Entry.Path;
}

public sealed class SyncPlan
{
    internal SyncPlan(
        string source, string destination, string deviceSerial, PushOptions options,
        IReadOnlyList<PlannedChange> directories, IReadOnlyList<PlannedChange> files,
        IReadOnlyList<PlannedChange> deletions, IReadOnlyList<PlannedChange> timestamps,
        ulong transferBytes, ulong removalCount,
        string digest, IReadOnlyDictionary<string, SourceFile> sourceFiles,
        IReadOnlyList<DestinationPrecondition> preconditions)
    {
        Source = source;
        Destination = destination;
        DeviceSerial = deviceSerial;
        Options = options;
        Directories = directories;
        Files = files;
        Deletions = deletions;
        Timestamps = timestamps;
        TransferBytes = transferBytes;
        RemovalCount = removalCount;
        Digest = digest;
        SourceFiles = sourceFiles;
        Preconditions = preconditions;
    }

    public string Source { get; }
    public string Destination { get; }
    public string DeviceSerial { get; }
    public PushOptions Options { get; }
    public IReadOnlyList<PlannedChange> Directories { get; }
    public IReadOnlyList<PlannedChange> Files { get; }
    public IReadOnlyList<PlannedChange> Deletions { get; }
    /// <summary>
    /// Unchanged files whose destination time is not yet in the timestamp catalog. Applying
    /// records the source time without transferring data, so later comparisons cannot drift
    /// when the destination filesystem reinterprets stored local times.
    /// </summary>
    public IReadOnlyList<PlannedChange> Timestamps { get; }
    public ulong TransferBytes { get; }
    public ulong RemovalCount { get; }
    public string Digest { get; }
    public bool HasChanges => Directories.Count != 0 || Files.Count != 0 || Deletions.Count != 0;

    internal IReadOnlyDictionary<string, SourceFile> SourceFiles { get; }
    internal IReadOnlyList<DestinationPrecondition> Preconditions { get; }
}

public sealed record OperationCounts(int Files, int Directories, int Deletions, ulong Bytes = 0, int Timestamps = 0);

public enum SyncStatus { Success, NoChanges, DryRun, Restored, Initialized }

public sealed record InitializeResult(string DeviceSerial, string Destination, TimeSpan Duration);

public sealed record SyncResult(
    SyncStatus Status,
    string DeviceSerial,
    string Source,
    string Destination,
    bool Direct,
    long ModificationTimeToleranceSeconds,
    ulong? MaximumRemovals,
    ulong RemovalCount,
    string PlanDigest,
    string? PlanFile,
    string? RecoveryId,
    OperationCounts Planned,
    OperationCounts Applied,
    int QuarantinedCount,
    ulong TransferredBytes,
    TimeSpan Duration,
    IReadOnlyList<PlannedChange> Actions);

public sealed record RestoreResult(string DeviceSerial, string Destination, string RecoveryId, TimeSpan Duration);

public enum SyncerProgressStage
{
    ScanningSource,
    ScanningDestination,
    SelectedChange,
    StagingFile,
    TransferringFile,
    Applying,
    CompletedChange,
    FailedChange,
    RecordingTimestamps,
}

public sealed record SyncerProgress(
    SyncerProgressStage Stage,
    string? Path = null,
    PlannedChange? Change = null,
    ulong BytesCompleted = 0,
    ulong? BytesTotal = null,
    int ItemCount = 0);

internal sealed record SourceFile(ManifestEntry Entry, string PhysicalPath);
internal sealed record SourceTree(
    IReadOnlyDictionary<string, ManifestEntry> Manifest,
    IReadOnlyDictionary<string, SourceFile> Files);
internal sealed record DestinationPrecondition(string Path, bool Exists, ManifestEntry? Entry);
internal sealed record DestinationManifest(
    IReadOnlyDictionary<string, ManifestEntry> Entries,
    IReadOnlySet<string> UntrackedTimes);
