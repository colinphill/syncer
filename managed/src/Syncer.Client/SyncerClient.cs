using System.Diagnostics;
using System.Security.Cryptography;

namespace Syncer;

public sealed class SyncerClient
{
    private readonly SyncerClientOptions options;
    public SyncerClient(SyncerClientOptions? options = null) => this.options = options ?? new();

    /// <summary>Enumerates every Android device currently reported by ADB.</summary>
    public Task<IReadOnlyList<SyncerDevice>> EnumerateDevicesAsync(
        CancellationToken cancellationToken = default) =>
        AdbClient.EnumerateAsync(options, cancellationToken);

    public async Task<InitializeResult> InitializeAsync(
        string devicePath, InitializeOptions? initializeOptions = null,
        IProgress<SyncerProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(devicePath); var started = Stopwatch.StartNew();
        var adb = await AdbClient.CreateAsync(options, null, cancellationToken).ConfigureAwait(false);
        await WithServerAsync(adb, (session, token) => session.InitializeAsync(devicePath, initializeOptions?.Adopt ?? false, token), cancellationToken).ConfigureAwait(false);
        return new(adb.Serial, devicePath, started.Elapsed);
    }

    public async Task<SyncPlan> PlanPushAsync(
        string sourcePath, string devicePath, PushOptions? pushOptions = null,
        IProgress<SyncerProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        ValidatePush(sourcePath, devicePath, pushOptions ??= new()); var exclusions = new GlobSet(pushOptions.Exclusions);
        progress?.Report(new(SyncerProgressStage.ScanningSource)); var sourceTask = SourceScanner.ScanAsync(sourcePath, exclusions, cancellationToken);
        var adb = await AdbClient.CreateAsync(options, null, cancellationToken).ConfigureAwait(false);
        return await WithServerAsync(adb, async (session, token) =>
        {
            progress?.Report(new(SyncerProgressStage.ScanningDestination)); var destination = await session.ScanAsync(devicePath, token).ConfigureAwait(false);
            var source = await sourceTask.ConfigureAwait(false); return BuildPlan(sourcePath, devicePath, adb.Serial, pushOptions, exclusions, source, destination, progress);
        }, cancellationToken).ConfigureAwait(false);
    }

    public async Task<SyncResult> PushAsync(
        string sourcePath, string devicePath, PushOptions? pushOptions = null,
        IProgress<SyncerProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        ValidatePush(sourcePath, devicePath, pushOptions ??= new()); var started = Stopwatch.StartNew(); var exclusions = new GlobSet(pushOptions.Exclusions);
        progress?.Report(new(SyncerProgressStage.ScanningSource)); var sourceTask = SourceScanner.ScanAsync(sourcePath, exclusions, cancellationToken);
        var adb = await AdbClient.CreateAsync(options, null, cancellationToken).ConfigureAwait(false);
        return await WithServerAsync(adb, async (session, token) =>
        {
            progress?.Report(new(SyncerProgressStage.ScanningDestination)); var destination = await session.ScanAsync(devicePath, token).ConfigureAwait(false);
            var source = await sourceTask.ConfigureAwait(false); var plan = BuildPlan(sourcePath, devicePath, adb.Serial, pushOptions, exclusions, source, destination, progress);
            var applied = await ApplyOnSessionAsync(session, plan, source, true, progress, token).ConfigureAwait(false);
            return Result(plan, applied, started.Elapsed, null);
        }, cancellationToken).ConfigureAwait(false);
    }

    public Task SavePlanAsync(SyncPlan plan, string path, CancellationToken cancellationToken = default)
    { ArgumentNullException.ThrowIfNull(plan); return SavedPlans.SaveAsync(plan, path, cancellationToken); }
    public Task<SyncPlan> LoadPlanAsync(string path, CancellationToken cancellationToken = default) => SavedPlans.LoadAsync(path, cancellationToken);

    public async Task<SyncResult> ApplyPlanAsync(
        SyncPlan plan, IProgress<SyncerProgress>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(plan); var started = Stopwatch.StartNew(); ReportChanges(plan, progress);
        var adb = await AdbClient.CreateAsync(options, plan.DeviceSerial, cancellationToken).ConfigureAwait(false);
        if (adb.Serial != plan.DeviceSerial) throw new SyncerException(SyncerErrorCode.Safety, "saved plan belongs to a different Android device");
        return await WithServerAsync(adb, async (session, token) =>
        {
            await session.SelectRootAsync(plan.Destination, token).ConfigureAwait(false);
            await session.ValidatePlanAsync(plan.Preconditions, token).ConfigureAwait(false);
            var applied = await ApplyOnSessionAsync(session, plan, null, false, progress, token).ConfigureAwait(false);
            return Result(plan, applied, started.Elapsed, null);
        }, cancellationToken).ConfigureAwait(false);
    }

    public async Task<RestoreResult> RestoreAsync(
        string devicePath, string recoveryId, IProgress<SyncerProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(devicePath); ArgumentException.ThrowIfNullOrWhiteSpace(recoveryId); var started = Stopwatch.StartNew();
        var adb = await AdbClient.CreateAsync(options, null, cancellationToken).ConfigureAwait(false);
        await WithServerAsync(adb, async (session, token) => { await session.SelectRootAsync(devicePath, token).ConfigureAwait(false); await session.RestoreAsync(recoveryId, token).ConfigureAwait(false); }, cancellationToken).ConfigureAwait(false);
        return new(adb.Serial, devicePath, recoveryId, started.Elapsed);
    }

    private static SyncPlan BuildPlan(
        string sourcePath, string devicePath, string serial, PushOptions options, GlobSet exclusions,
        SourceTree source, DestinationManifest scanned, IProgress<SyncerProgress>? progress)
    {
        var destination = scanned.Entries;
        var parts = Planner.MakePlan(source.Manifest, destination, exclusions, options.DeleteExtraneous,
            checked((ulong)SavedPlans.ToToleranceSeconds(options)), scanned.UntrackedTimes);
        var removals = Planner.RemovalCount(parts, destination);
        if (options.MaximumRemovals is { } maximum && removals > maximum)
            throw new SyncerException(SyncerErrorCode.Safety, $"plan removes {removals} entries, exceeding MaximumRemovals {maximum}");
        var plan = SavedPlans.Create(sourcePath, devicePath, serial, options, parts, removals, source.Files, Planner.BuildPreconditions(parts, destination));
        if (options.ExpectedPlanDigest is { } expected && expected != plan.Digest)
            throw new SyncerException(SyncerErrorCode.SourceChanged, "the synchronization plan changed after preview");
        ReportChanges(plan, progress); return plan;
    }

    private static void ReportChanges(SyncPlan plan, IProgress<SyncerProgress>? progress)
    {
        progress?.Report(new(SyncerProgressStage.SelectedChange, plan.HasChanges ? null : "none"));
        foreach (var change in plan.Directories.Concat(plan.Files).Concat(plan.Deletions)) progress?.Report(new(SyncerProgressStage.SelectedChange, change.Path, change));
    }

    private static async Task<AppliedState> ApplyOnSessionAsync(
        ClientSession session, SyncPlan plan, SourceTree? freshSource, bool verifyWholeTree,
        IProgress<SyncerProgress>? progress, CancellationToken cancellationToken)
    {
        if (!plan.HasChanges) return new(null, 0, 0, 0, 0, 0, await RecordTimestampsAsync(session, plan, progress, cancellationToken).ConfigureAwait(false));
        string? recoveryId = null;
        if (!plan.Options.Direct)
        {
            recoveryId = Convert.ToHexString(RandomNumberGenerator.GetBytes(12)).ToLowerInvariant();
            await session.PrepareAsync(recoveryId, cancellationToken).ConfigureAwait(false);
        }
        var files = 0; var directories = 0; var quarantined = 0; ulong bytes = 0;
        if (!plan.Options.Direct)
        {
            foreach (var change in plan.Files)
            {
                await RunChangeAsync(change, SyncerProgressStage.StagingFile, reportCompletion: false,
                    progress, () => session.UploadAsync(plan.SourceFiles[change.Path], false,
                        progress, cancellationToken)).ConfigureAwait(false);
                bytes += change.Entry.Size;
            }
            if (verifyWholeTree) await VerifyTreeAsync(plan, freshSource!, cancellationToken).ConfigureAwait(false);
        }
        else if (verifyWholeTree) await VerifyTreeAsync(plan, freshSource!, cancellationToken).ConfigureAwait(false);
        foreach (var change in plan.Directories)
        {
            await RunChangeAsync(change, SyncerProgressStage.Applying, reportCompletion: true,
                progress, async () =>
                {
                    if (change.Action == PlanAction.ReplaceWithDirectory) { await session.DeletePathAsync(change.Path, cancellationToken).ConfigureAwait(false); if (recoveryId is not null) quarantined++; }
                    await session.MakeDirectoryAsync(change.Entry, cancellationToken).ConfigureAwait(false); directories++;
                }).ConfigureAwait(false);
        }
        foreach (var change in plan.Files)
        {
            await RunChangeAsync(change, SyncerProgressStage.Applying, reportCompletion: true,
                progress, async () =>
                {
                    if (change.Action is PlanAction.UpdateFile or PlanAction.ReplaceWithFile) { await session.DeletePathAsync(change.Path, cancellationToken).ConfigureAwait(false); if (recoveryId is not null) quarantined++; }
                    if (plan.Options.Direct)
                    {
                        progress?.Report(new(SyncerProgressStage.TransferringFile, change.Path, change)); await session.UploadAsync(plan.SourceFiles[change.Path], true, progress, cancellationToken).ConfigureAwait(false); bytes += change.Entry.Size;
                    }
                    else await session.CommitFileAsync(change.Path, cancellationToken).ConfigureAwait(false);
                    files++;
                }).ConfigureAwait(false);
        }
        if (plan.Options.Direct && verifyWholeTree) await VerifyTreeAsync(plan, freshSource!, cancellationToken).ConfigureAwait(false);
        var deletionPaths = new HashSet<string>(plan.Deletions.Select(change => change.Path), StringComparer.Ordinal);
        var deletionGroups = new Dictionary<string, List<PlannedChange>>(StringComparer.Ordinal);
        foreach (var change in plan.Deletions)
        {
            var root = change.Path; var parent = change.Path;
            while (parent.LastIndexOf('/') is var slash && slash >= 0) { parent = parent[..slash]; if (deletionPaths.Contains(parent)) root = parent; }
            if (!deletionGroups.TryGetValue(root, out var group)) deletionGroups.Add(root, group = []);
            group.Add(change);
        }
        foreach (var change in plan.Deletions)
        {
            if (!deletionGroups.TryGetValue(change.Path, out var group)) continue;
            progress?.Report(new(SyncerProgressStage.Applying, change.Path, change));
            try { await session.DeletePathAsync(change.Path, cancellationToken).ConfigureAwait(false); if (recoveryId is not null) quarantined++; }
            catch
            {
                foreach (var affected in group) progress?.Report(new(SyncerProgressStage.FailedChange, affected.Path, affected));
                throw;
            }
            foreach (var affected in group) progress?.Report(new(SyncerProgressStage.CompletedChange, affected.Path, affected));
        }
        var timestamps = await RecordTimestampsAsync(session, plan, progress, cancellationToken).ConfigureAwait(false);
        return new(recoveryId, files, directories, plan.Deletions.Count, quarantined, bytes, timestamps);
    }

    private static async Task<int> RecordTimestampsAsync(
        ClientSession session, SyncPlan plan, IProgress<SyncerProgress>? progress, CancellationToken cancellationToken)
    {
        if (plan.Timestamps.Count == 0) return 0;
        progress?.Report(new(SyncerProgressStage.RecordingTimestamps, ItemCount: plan.Timestamps.Count));
        await session.RecordTimesAsync(plan.Timestamps, cancellationToken).ConfigureAwait(false);
        return plan.Timestamps.Count;
    }

    internal static async Task RunChangeAsync(
        PlannedChange change, SyncerProgressStage startingStage, bool reportCompletion,
        IProgress<SyncerProgress>? progress, Func<Task> action)
    {
        progress?.Report(new(startingStage, change.Path, change));
        try { await action().ConfigureAwait(false); }
        catch
        {
            progress?.Report(new(SyncerProgressStage.FailedChange, change.Path, change));
            throw;
        }
        if (reportCompletion)
            progress?.Report(new(SyncerProgressStage.CompletedChange, change.Path, change));
    }

    private static async Task VerifyTreeAsync(SyncPlan plan, SourceTree original, CancellationToken cancellationToken)
    {
        try
        {
            var current = await SourceScanner.ScanAsync(plan.Source, new GlobSet(plan.Options.Exclusions), cancellationToken).ConfigureAwait(false);
            if (!DictionaryEqual(current.Manifest, original.Manifest)) throw new SyncerException(SyncerErrorCode.SourceChanged, "source tree changed after it was scanned");
        }
        catch (OperationCanceledException) { throw; }
        catch (SyncerException error) when (error.ErrorCode != SyncerErrorCode.SourceChanged)
        { throw new SyncerException(SyncerErrorCode.SourceChanged, $"source tree changed after it was scanned: {error.Message}", error); }
    }
    private static bool DictionaryEqual(IReadOnlyDictionary<string, ManifestEntry> left, IReadOnlyDictionary<string, ManifestEntry> right) =>
        left.Count == right.Count && left.All(pair => right.TryGetValue(pair.Key, out var value) && value == pair.Value);
    private static SyncResult Result(SyncPlan plan, AppliedState applied, TimeSpan duration, string? planFile) => new(
        plan.HasChanges ? SyncStatus.Success : SyncStatus.NoChanges, plan.DeviceSerial, plan.Source, plan.Destination,
        plan.Options.Direct, SavedPlans.ToToleranceSeconds(plan.Options), plan.Options.MaximumRemovals, plan.RemovalCount,
        plan.Digest, planFile, applied.RecoveryId,
        new(plan.Files.Count, plan.Directories.Count, plan.Deletions.Count, plan.TransferBytes, plan.Timestamps.Count),
        new(applied.Files, applied.Directories, applied.Deletions, applied.Bytes, applied.Timestamps), applied.Quarantined,
        applied.Bytes, duration, plan.Directories.Concat(plan.Files).Concat(plan.Deletions).ToArray());
    private static void ValidatePush(string source, string destination, PushOptions options)
    { ArgumentException.ThrowIfNullOrWhiteSpace(source); ArgumentException.ThrowIfNullOrWhiteSpace(destination); _ = SavedPlans.ToToleranceSeconds(options); }

    private async Task<T> WithServerAsync<T>(AdbClient adb, Func<ClientSession, CancellationToken, Task<T>> action, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested(); var remote = await adb.DeployServerAsync(cancellationToken).ConfigureAwait(false);
        var token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant(); var socketName = "syncer." + token[..24];
        var port = await adb.CreateForwardAsync(socketName, cancellationToken).ConfigureAwait(false); RunningProcess? server = null; ClientSession? session = null;
        try
        {
            server = adb.LaunchServer(remote, socketName, token); session = await ClientSession.ConnectAsync(port, token, cancellationToken).ConfigureAwait(false);
            using var registration = cancellationToken.Register(session.Cancel); var value = await action(session, cancellationToken).ConfigureAwait(false);
            await session.ShutdownAsync().ConfigureAwait(false); await session.DisposeAsync().ConfigureAwait(false); session = null;
            var serverResult = await server.FinishAsync(CancellationToken.None).ConfigureAwait(false);
            if (serverResult.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Transfer, $"Android server exited with code {serverResult.ExitCode}: {serverResult.Output.Trim()}");
            return value;
        }
        catch (OperationCanceledException)
        {
            session?.Cancel();
            if (session is not null) { await session.DisposeAsync().ConfigureAwait(false); session = null; }
            if (server is not null) _ = await server.FinishAsync(CancellationToken.None).ConfigureAwait(false);
            throw;
        }
        catch (SyncerException error)
        {
            session?.Cancel();
            if (session is not null) { await session.DisposeAsync().ConfigureAwait(false); session = null; }
            if (server is not null) { var result = await server.FinishAsync(CancellationToken.None).ConfigureAwait(false); if (!string.IsNullOrWhiteSpace(result.Output)) throw new SyncerException(error.ErrorCode, $"{error.Message}; Android server: {result.Output.Trim()}", error); }
            throw;
        }
        catch (Exception error)
        {
            session?.Cancel();
            if (session is not null) { await session.DisposeAsync().ConfigureAwait(false); session = null; }
            var diagnostic = server is null ? string.Empty : (await server.FinishAsync(CancellationToken.None).ConfigureAwait(false)).Output.Trim();
            throw new SyncerException(SyncerErrorCode.Transfer, diagnostic.Length == 0 ? error.Message : $"{error.Message}; Android server: {diagnostic}", error);
        }
        finally
        {
            if (session is not null) await session.DisposeAsync().ConfigureAwait(false);
            if (server is not null) await server.DisposeAsync().ConfigureAwait(false);
            await adb.RemoveForwardAsync(port).ConfigureAwait(false);
        }
    }
    private Task WithServerAsync(AdbClient adb, Func<ClientSession, CancellationToken, Task> action, CancellationToken cancellationToken) =>
        WithServerAsync(adb, async (session, token) => { await action(session, token).ConfigureAwait(false); return true; }, cancellationToken);
    private sealed record AppliedState(string? RecoveryId, int Files, int Directories, int Deletions, int Quarantined, ulong Bytes, int Timestamps);
}
