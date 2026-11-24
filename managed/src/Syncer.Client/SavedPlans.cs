using System.Text.Json;
using System.Text.Json.Nodes;
using System.Security.Cryptography;

namespace Syncer;

internal static class SavedPlans
{
    private const int Schema = 2;

    public static SyncPlan Create(
        string source, string destination, string serial, PushOptions options,
        PlanParts parts, ulong removals, IReadOnlyDictionary<string, SourceFile> sourceFiles,
        IReadOnlyList<DestinationPrecondition> preconditions)
    {
        var digest = ComputeDigest(source, destination, serial, options, parts.Directories, parts.Files, parts.Deletions, parts.Timestamps);
        return new SyncPlan(source, destination, serial, NormalizeOptions(options), parts.Directories, parts.Files,
            parts.Deletions, parts.Timestamps, parts.TransferBytes, removals, digest, sourceFiles, preconditions);
    }

    public static string ComputeDigest(
        string source, string destination, string serial, PushOptions options,
        IReadOnlyList<PlannedChange> directories, IReadOnlyList<PlannedChange> files,
        IReadOnlyList<PlannedChange> deletions, IReadOnlyList<PlannedChange> timestamps)
    {
        var writer = new ProtocolWriter(); writer.WriteString("SYNCER-PLAN"); writer.WriteUInt32(2);
        writer.WriteString(source); writer.WriteString(destination); writer.WriteString(serial);
        writer.WriteByte(options.DeleteExtraneous ? (byte)0 : (byte)1); writer.WriteByte(options.Direct ? (byte)1 : (byte)0);
        writer.WriteInt64(ToToleranceSeconds(options)); writer.WriteUInt32(checked((uint)options.Exclusions.Count));
        foreach (var exclusion in options.Exclusions) writer.WriteString(exclusion);
        WriteChanges(writer, directories); WriteChanges(writer, files); WriteChanges(writer, deletions); WriteChanges(writer, timestamps);
        return Blake3.Hex(Blake3.Hash(writer.ToArray()));
    }

    public static async Task SaveAsync(SyncPlan plan, string path, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var root = new JsonObject
        {
            ["schema"] = Schema, ["syncer_version"] = ProtocolConstants.Version,
            ["protocol"] = ProtocolConstants.ProtocolVersion, ["source"] = plan.Source,
            ["destination"] = plan.Destination, ["device"] = plan.DeviceSerial,
            ["direct"] = plan.Options.Direct, ["no_delete"] = !plan.Options.DeleteExtraneous,
            ["mtime_tolerance_seconds"] = ToToleranceSeconds(plan.Options),
            ["excludes"] = new JsonArray(plan.Options.Exclusions.Select(value => (JsonNode?)JsonValue.Create(value)).ToArray()),
            ["plan_digest"] = plan.Digest, ["removal_count"] = plan.RemovalCount,
            ["directories"] = ChangesJson(plan.Directories, null),
            ["files"] = ChangesJson(plan.Files, plan.SourceFiles),
            ["deletions"] = ChangesJson(plan.Deletions, null),
            ["timestamps"] = ChangesJson(plan.Timestamps, null),
            ["preconditions"] = PreconditionsJson(plan.Preconditions),
        };
        var fullPath = Path.GetFullPath(path); var directory = Path.GetDirectoryName(fullPath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        var temporary = fullPath + ".tmp." + Convert.ToHexString(RandomNumberGenerator.GetBytes(6)).ToLowerInvariant();
        try
        {
            await using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, true))
                await JsonSerializer.SerializeAsync(output, root, cancellationToken: cancellationToken).ConfigureAwait(false);
            File.Move(temporary, fullPath, false);
        }
        catch (OperationCanceledException) { TryDelete(temporary); throw; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { TryDelete(temporary); throw new SyncerException(SyncerErrorCode.Transfer, "cannot write saved plan", error); }
    }

    public static async Task<SyncPlan> LoadAsync(string path, CancellationToken cancellationToken)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.Length > 256L * 1024 * 1024) throw new SyncerException(SyncerErrorCode.Safety, "saved plan is missing or too large");
            await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, true);
            var node = await JsonNode.ParseAsync(input, cancellationToken: cancellationToken).ConfigureAwait(false);
            var root = node?.AsObject() ?? throw new JsonException();
            if (Required<int>(root, "schema") != Schema || Required<ushort>(root, "protocol") != ProtocolConstants.ProtocolVersion)
                throw new SyncerException(SyncerErrorCode.Safety, "saved plan has an unsupported schema or protocol");
            var source = Required<string>(root, "source"); var destination = Required<string>(root, "destination");
            var serial = Required<string>(root, "device"); var claimedDigest = Required<string>(root, "plan_digest");
            var options = new PushOptions
            {
                Direct = Required<bool>(root, "direct"), DeleteExtraneous = !Required<bool>(root, "no_delete"),
                ModificationTimeTolerance = TimeSpan.FromSeconds(Required<long>(root, "mtime_tolerance_seconds")),
                Exclusions = RequiredArray(root, "excludes").Select(value => value!.GetValue<string>()).ToArray(),
            };
            var sources = new Dictionary<string, SourceFile>(StringComparer.Ordinal);
            var directories = ParseChanges(RequiredArray(root, "directories"), false, sources);
            var files = ParseChanges(RequiredArray(root, "files"), true, sources);
            var deletions = ParseChanges(RequiredArray(root, "deletions"), false, sources);
            var timestamps = ParseChanges(RequiredArray(root, "timestamps"), false, sources);
            ValidateActions(directories, files, deletions, timestamps);
            var preconditions = ParsePreconditions(RequiredArray(root, "preconditions"));
            var digest = ComputeDigest(source, destination, serial, options, directories, files, deletions, timestamps);
            if (!string.Equals(digest, claimedDigest, StringComparison.Ordinal))
                throw new SyncerException(SyncerErrorCode.Safety, "saved plan digest does not match its contents");
            var bytes = files.Aggregate<PlannedChange, ulong>(0, (value, change) => value + change.Entry.Size);
            return new SyncPlan(source, destination, serial, options, directories, files, deletions, timestamps, bytes,
                Required<ulong>(root, "removal_count"), claimedDigest, sources, preconditions);
        }
        catch (OperationCanceledException) { throw; }
        catch (SyncerException) { throw; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SyncerException(SyncerErrorCode.Transfer, "cannot read saved plan", error); }
        catch (Exception error) when (error is JsonException or InvalidOperationException or KeyNotFoundException or FormatException or OverflowException)
        { throw new SyncerException(SyncerErrorCode.Safety, "saved plan is invalid", error); }
    }

    public static long ToToleranceSeconds(PushOptions options)
    {
        if (options.ModificationTimeTolerance < TimeSpan.Zero || options.ModificationTimeTolerance.Ticks % TimeSpan.TicksPerSecond != 0)
            throw new ArgumentOutOfRangeException(nameof(options), "ModificationTimeTolerance must be a nonnegative whole number of seconds.");
        return checked((long)options.ModificationTimeTolerance.TotalSeconds);
    }

    private static PushOptions NormalizeOptions(PushOptions options) => options with { Exclusions = options.Exclusions.ToArray() };
    private static void WriteChanges(ProtocolWriter writer, IReadOnlyList<PlannedChange> changes)
    {
        writer.WriteUInt32(checked((uint)changes.Count));
        foreach (var change in changes) { writer.WriteByte((byte)change.Action); ProtocolCodec.WriteManifestEntry(writer, change.Entry); writer.WriteString(change.Reason); }
    }
    private static JsonArray ChangesJson(IReadOnlyList<PlannedChange> changes, IReadOnlyDictionary<string, SourceFile>? sources)
    {
        var result = new JsonArray();
        foreach (var change in changes)
        {
            var value = new JsonObject { ["action"] = ActionId(change.Action), ["entry"] = EntryJson(change.Entry), ["reason"] = change.Reason };
            if (sources is not null) value["source_path"] = sources[change.Path].PhysicalPath;
            result.Add(value);
        }
        return result;
    }
    private static JsonArray PreconditionsJson(IReadOnlyList<DestinationPrecondition> values)
    {
        var result = new JsonArray();
        foreach (var value in values)
        {
            var node = new JsonObject { ["path"] = value.Path, ["exists"] = value.Exists };
            if (value.Exists) node["entry"] = EntryJson(value.Entry!); result.Add(node);
        }
        return result;
    }
    private static JsonObject EntryJson(ManifestEntry entry) => new() { ["path"] = entry.Path, ["type"] = (byte)entry.Type, ["size"] = entry.Size, ["modified_seconds"] = entry.ModifiedSeconds };
    private static ManifestEntry ParseEntry(JsonNode? value)
    {
        var node = value?.AsObject() ?? throw new JsonException();
        var entry = new ManifestEntry(Required<string>(node, "path"), (EntryType)Required<byte>(node, "type"), Required<ulong>(node, "size"), Required<long>(node, "modified_seconds"));
        if (!Planner.IsValidRelativePath(entry.Path) || entry.Type is not (EntryType.File or EntryType.Directory or EntryType.Other))
            throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains an invalid entry");
        return entry;
    }
    private static List<PlannedChange> ParseChanges(JsonArray array, bool hasSources, IDictionary<string, SourceFile> sources)
    {
        if (array.Count > 1_000_000) throw new SyncerException(SyncerErrorCode.Safety, "saved plan has too many actions");
        var result = new List<PlannedChange>(array.Count);
        foreach (var item in array)
        {
            var node = item?.AsObject() ?? throw new JsonException(); var entry = ParseEntry(node["entry"]);
            var change = new PlannedChange(entry, ParseAction(Required<string>(node, "action")), Required<string>(node, "reason")); result.Add(change);
            if (hasSources && !sources.TryAdd(entry.Path, new(entry, Required<string>(node, "source_path"))))
                throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains duplicate source files");
        }
        return result;
    }
    private static List<DestinationPrecondition> ParsePreconditions(JsonArray array)
    {
        var result = new List<DestinationPrecondition>(array.Count);
        foreach (var item in array)
        {
            var node = item?.AsObject() ?? throw new JsonException(); var path = Required<string>(node, "path"); var exists = Required<bool>(node, "exists");
            var entry = exists ? ParseEntry(node["entry"]) : null;
            if (!Planner.IsValidRelativePath(path) || (entry is not null && entry.Path != path))
                throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains an invalid precondition");
            result.Add(new(path, exists, entry));
        }
        return result;
    }
    private static void ValidateActions(IEnumerable<PlannedChange> directories, IEnumerable<PlannedChange> files, IEnumerable<PlannedChange> deletions, IEnumerable<PlannedChange> timestamps)
    {
        if (timestamps.Any(c => c.Entry.Type != EntryType.File || c.Action != PlanAction.RecordTimestamp))
            throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains an invalid timestamp record");
        if (directories.Any(c => c.Entry.Type != EntryType.Directory || c.Action is not (PlanAction.CreateDirectory or PlanAction.ReplaceWithDirectory)) ||
            files.Any(c => c.Entry.Type != EntryType.File || c.Action is not (PlanAction.AddFile or PlanAction.UpdateFile or PlanAction.ReplaceWithFile)) ||
            deletions.Any(c => c.Action != (c.Entry.Type switch { EntryType.File => PlanAction.DeleteFile, EntryType.Directory => PlanAction.DeleteDirectory, _ => PlanAction.DeleteOther })))
            throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains an invalid action");
    }
    private static string ActionId(PlanAction action) => action switch
    {
        PlanAction.CreateDirectory => "create_directory", PlanAction.ReplaceWithDirectory => "replace_with_directory",
        PlanAction.AddFile => "add_file", PlanAction.UpdateFile => "update_file", PlanAction.ReplaceWithFile => "replace_with_file",
        PlanAction.DeleteFile => "delete_file", PlanAction.DeleteDirectory => "delete_directory", PlanAction.DeleteOther => "delete_other",
        PlanAction.RecordTimestamp => "record_timestamp", _ => throw new ArgumentOutOfRangeException(nameof(action)),
    };
    private static PlanAction ParseAction(string action) => action switch
    {
        "create_directory" => PlanAction.CreateDirectory, "replace_with_directory" => PlanAction.ReplaceWithDirectory,
        "add_file" => PlanAction.AddFile, "update_file" => PlanAction.UpdateFile, "replace_with_file" => PlanAction.ReplaceWithFile,
        "delete_file" => PlanAction.DeleteFile, "delete_directory" => PlanAction.DeleteDirectory, "delete_other" => PlanAction.DeleteOther,
        "record_timestamp" => PlanAction.RecordTimestamp,
        _ => throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains an unknown action"),
    };
    private static T Required<T>(JsonObject node, string name)
    {
        var value = node[name] ?? throw new JsonException();
        return value.GetValue<T>();
    }
    private static JsonArray RequiredArray(JsonObject node, string name) => node[name]?.AsArray() ?? throw new JsonException();
    private static void TryDelete(string path) { try { File.Delete(path); } catch { } }
}
