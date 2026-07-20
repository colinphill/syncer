using System.CommandLine;
using System.CommandLine.Parsing;
using System.Text.Json;
using System.Text.Json.Nodes;
using Syncer;

return Tool.Run(args);

internal static class Tool
{
    private static string Version => typeof(SyncerClient).Assembly.GetName().Version!.ToString(3);

    public static int Run(string[] args)
    {
        var root = new RootCommand("Mirror a host directory into managed Android shared storage over ADB.");
        root.Subcommands.Add(InitCommand()); root.Subcommands.Add(PushCommand()); root.Subcommands.Add(ApplyCommand());
        root.Subcommands.Add(RestoreCommand()); root.Subcommands.Add(VersionCommand());
        var parsed = root.Parse(args);
        if (parsed.Errors.Count != 0)
        {
            var message = string.Join("; ", parsed.Errors.Select(error => error.Message));
            if (args.Contains("--json", StringComparer.Ordinal)) WriteJson(ErrorJson((int)SyncerErrorCode.Usage, message));
            else foreach (var error in parsed.Errors) Console.Error.WriteLine($"syncer-managed: {error.Message}");
            return (int)SyncerErrorCode.Usage;
        }
        return parsed.Invoke();
    }

    private static Command InitCommand()
    {
        var adb = StringOption("--adb", "path to adb"); var serial = StringOption("--serial", "ADB device serial"); var cancel = Flag("--cancel-stdin", "accept a line containing 'cancel' on stdin");
        var adopt = Flag("--adopt", "adopt a non-empty destination"); var destination = RequiredArgument("device-path", "Android destination path");
        var command = new Command("init", "Initialize a managed destination") { adb, serial, cancel, adopt, destination };
        command.SetAction(parse => Execute(parse.GetValue(cancel), false, async token =>
        {
            var client = Client(parse, adb, serial); var result = await client.InitializeAsync(parse.GetValue(destination)!, new() { Adopt = parse.GetValue(adopt) }, cancellationToken: token);
            Console.Out.WriteLine($"Initialized {result.Destination} on {result.DeviceSerial}"); return 0;
        }));
        return command;
    }

    private static Command PushCommand()
    {
        var adb = StringOption("--adb", "path to adb"); var serial = StringOption("--serial", "ADB device serial"); var cancel = Flag("--cancel-stdin", "accept a line containing 'cancel' on stdin");
        var excludes = new Option<string[]>("--exclude") { Description = "case-sensitive exclusion glob", AllowMultipleArgumentsPerToken = false };
        var tolerance = new Option<long>("--mtime-tolerance") { Description = "modification-time tolerance in seconds", DefaultValueFactory = _ => 60 };
        var maximum = new Option<ulong?>("--max-removals") { Description = "maximum destination-only or type-conflicting entries" };
        var expected = StringOption("--expect-plan", "require the recomputed plan to match a reviewed digest"); var save = StringOption("--save-plan", "save the dry-run plan for later execution");
        var noDelete = Flag("--no-delete", "keep destination-only paths"); var direct = Flag("--direct", "write files without temporary staging");
        var dryRun = Flag("--dry-run", "calculate without mutating"); var json = Flag("--json", "write JSON result to stdout");
        var source = RequiredArgument("local-source", "host source directory"); var destination = RequiredArgument("device-path", "Android destination path");
        var command = new Command("push", "Synchronize a directory") { adb, serial, cancel, excludes, tolerance, maximum, expected, save, noDelete, direct, dryRun, json, source, destination };
        command.SetAction(parse => Execute(parse.GetValue(cancel), parse.GetValue(json), async token =>
        {
            var toleranceValue = parse.GetValue(tolerance); if (toleranceValue < 0) throw new ArgumentException("--mtime-tolerance cannot be negative");
            var savePath = parse.GetValue(save); if (savePath is not null && !parse.GetValue(dryRun)) throw new ArgumentException("--save-plan requires --dry-run");
            var pushOptions = new PushOptions
            {
                Exclusions = parse.GetValue(excludes) ?? [], ModificationTimeTolerance = TimeSpan.FromSeconds(toleranceValue),
                MaximumRemovals = parse.GetValue(maximum), ExpectedPlanDigest = parse.GetValue(expected),
                DeleteExtraneous = !parse.GetValue(noDelete), Direct = parse.GetValue(direct),
            };
            var client = Client(parse, adb, serial); var progress = new InlineProgress(RenderProgress); var started = System.Diagnostics.Stopwatch.StartNew();
            if (parse.GetValue(dryRun))
            {
                var plan = await client.PlanPushAsync(parse.GetValue(source)!, parse.GetValue(destination)!, pushOptions, progress, token);
                if (savePath is not null) await client.SavePlanAsync(plan, savePath, token);
                if (parse.GetValue(json)) WriteJson(PlanJson(plan, started.Elapsed, savePath));
                else Console.Out.WriteLine($"Dry run: {plan.Files.Count} file(s), {plan.Directories.Count} directorie(s), {plan.Deletions.Count} deletion(s), {plan.TransferBytes} byte(s)");
            }
            else
            {
                var result = await client.PushAsync(parse.GetValue(source)!, parse.GetValue(destination)!, pushOptions, progress, token);
                if (parse.GetValue(json)) WriteJson(ResultJson(result));
                else Console.Out.WriteLine($"Synchronized: {result.Planned.Files} file(s), {result.Planned.Directories} directorie(s), {result.Planned.Deletions} deletion(s), {result.Planned.Bytes} byte(s)");
            }
            return 0;
        }));
        return command;
    }

    private static Command ApplyCommand()
    {
        var adb = StringOption("--adb", "path to adb"); var serial = StringOption("--serial", "ADB device serial"); var cancel = Flag("--cancel-stdin", "accept a line containing 'cancel' on stdin");
        var json = Flag("--json", "write JSON result to stdout"); var planPath = new Option<string>("--plan") { Description = "saved plan file to execute", Required = true };
        var command = new Command("apply", "Apply a saved plan") { adb, serial, cancel, json, planPath };
        command.SetAction(parse => Execute(parse.GetValue(cancel), parse.GetValue(json), async token =>
        {
            var client = Client(parse, adb, serial); var path = parse.GetValue(planPath)!; var plan = await client.LoadPlanAsync(path, token);
            var result = (await client.ApplyPlanAsync(plan, new InlineProgress(RenderProgress), token)) with { PlanFile = path };
            if (parse.GetValue(json)) WriteJson(ResultJson(result));
            else Console.Out.WriteLine($"Applied saved plan: {result.Planned.Files} file(s), {result.Planned.Directories} directorie(s), {result.Planned.Deletions} deletion(s), {result.Planned.Bytes} byte(s)");
            return 0;
        }));
        return command;
    }

    private static Command RestoreCommand()
    {
        var adb = StringOption("--adb", "path to adb"); var serial = StringOption("--serial", "ADB device serial"); var cancel = Flag("--cancel-stdin", "accept a line containing 'cancel' on stdin");
        var json = Flag("--json", "write JSON result to stdout"); var recovery = new Option<string>("--recovery") { Description = "recovery run identifier", Required = true };
        var destination = RequiredArgument("device-path", "Android destination path"); var command = new Command("restore", "Restore a recovery run") { adb, serial, cancel, json, recovery, destination };
        command.SetAction(parse => Execute(parse.GetValue(cancel), parse.GetValue(json), async token =>
        {
            var client = Client(parse, adb, serial); var result = await client.RestoreAsync(parse.GetValue(destination)!, parse.GetValue(recovery)!, cancellationToken: token);
            if (parse.GetValue(json)) WriteJson(new JsonObject { ["schema"] = 1, ["status"] = "restored", ["device"] = result.DeviceSerial, ["destination"] = result.Destination, ["recovery_id"] = result.RecoveryId, ["duration_ms"] = (long)result.Duration.TotalMilliseconds, ["error"] = null });
            else Console.Out.WriteLine($"Restored recovery run {result.RecoveryId} at {result.Destination} on {result.DeviceSerial}");
            return 0;
        }));
        return command;
    }

    private static Command VersionCommand()
    {
        var command = new Command("version", "Display version information");
        command.SetAction(_ => { Console.Out.WriteLine($"syncer-managed {Version} (protocol 6)"); return 0; }); return command;
    }

    private static int Execute(bool cancelStdin, bool json, Func<CancellationToken, Task<int>> action)
    {
        using var cancellation = new CancellationTokenSource();
        if (cancelStdin) _ = MonitorStdinAsync(cancellation);
        try { return action(cancellation.Token).GetAwaiter().GetResult(); }
        catch (OperationCanceledException) { return Error(SyncerErrorCode.Cancelled, "synchronization cancelled", json); }
        catch (ArgumentException error) { return Error(SyncerErrorCode.Usage, error.Message, json); }
        catch (SyncerException error) { return Error(error.ErrorCode, error.Message, json); }
        catch (Exception error) { return Error(SyncerErrorCode.Transfer, error.Message, json); }
    }
    private static async Task MonitorStdinAsync(CancellationTokenSource cancellation)
    {
        try { while (!cancellation.IsCancellationRequested && await Console.In.ReadLineAsync(cancellation.Token) is { } line) if (line == "cancel") { await cancellation.CancelAsync(); return; } }
        catch (OperationCanceledException) { }
    }
    private static int Error(SyncerErrorCode code, string message, bool json)
    {
        if (json) WriteJson(ErrorJson((int)code, message)); else Console.Error.WriteLine($"syncer-managed: {message}"); return (int)code;
    }
    private static SyncerClient Client(ParseResult parse, Option<string?> adb, Option<string?> serial) => new(new() { AdbPath = parse.GetValue(adb), DeviceSerial = parse.GetValue(serial) });
    private static Option<string?> StringOption(string name, string description) => new(name) { Description = description };
    private static Option<bool> Flag(string name, string description) => new(name) { Description = description };
    private static Argument<string> RequiredArgument(string name, string description) => new(name) { Description = description };

    private static void RenderProgress(SyncerProgress value)
    {
        switch (value.Stage)
        {
            case SyncerProgressStage.ScanningSource: Console.Error.WriteLine("Scanning source..."); break;
            case SyncerProgressStage.ScanningDestination: Console.Error.WriteLine("Scanning destination..."); break;
            case SyncerProgressStage.SelectedChange when value.Change is null: Console.Error.WriteLine(value.Path == "none" ? "Selected changes: none" : "Selected changes:"); break;
            case SyncerProgressStage.SelectedChange when value.Change is not null: Console.Error.WriteLine($"  {ActionName(value.Change.Action)} \"{value.Change.Path}\" -- {value.Change.Reason}"); break;
            case SyncerProgressStage.StagingFile when value.BytesCompleted == 0: Console.Error.WriteLine($"Staging {value.Path}"); break;
            case SyncerProgressStage.TransferringFile when value.BytesCompleted == 0: Console.Error.WriteLine($"Transferring {value.Path}"); break;
        }
    }
    private static string ActionName(PlanAction action) => action switch
    {
        PlanAction.CreateDirectory => "CREATE DIRECTORY", PlanAction.ReplaceWithDirectory => "REPLACE WITH DIRECTORY",
        PlanAction.AddFile => "ADD FILE", PlanAction.UpdateFile => "UPDATE FILE", PlanAction.ReplaceWithFile => "REPLACE WITH FILE",
        PlanAction.DeleteFile => "DELETE FILE", PlanAction.DeleteDirectory => "DELETE DIRECTORY", _ => "DELETE ENTRY",
    };

    private static JsonObject ResultJson(SyncResult value) => new()
    {
        ["schema"] = 1, ["status"] = value.Status == SyncStatus.NoChanges ? "no_changes" : "success", ["device"] = value.DeviceSerial,
        ["source"] = value.Source, ["destination"] = value.Destination, ["direct"] = value.Direct,
        ["mtime_tolerance_seconds"] = value.ModificationTimeToleranceSeconds, ["max_removals"] = value.MaximumRemovals is { } max ? JsonValue.Create(max) : null,
        ["removal_count"] = value.RemovalCount, ["plan_digest"] = value.PlanDigest, ["plan_file"] = value.PlanFile,
        ["actions"] = ActionsJson(value.Actions), ["recovery_id"] = value.RecoveryId,
        ["planned"] = CountsJson(value.Planned), ["applied"] = CountsJson(value.Applied), ["quarantined_count"] = value.QuarantinedCount,
        ["transferred_bytes"] = value.TransferredBytes, ["duration_ms"] = (long)value.Duration.TotalMilliseconds, ["error"] = null,
    };
    private static JsonObject PlanJson(SyncPlan plan, TimeSpan duration, string? path) => new()
    {
        ["schema"] = 1, ["status"] = "dry_run", ["device"] = plan.DeviceSerial, ["source"] = plan.Source, ["destination"] = plan.Destination,
        ["direct"] = plan.Options.Direct, ["mtime_tolerance_seconds"] = (long)plan.Options.ModificationTimeTolerance.TotalSeconds,
        ["max_removals"] = plan.Options.MaximumRemovals is { } max ? JsonValue.Create(max) : null, ["removal_count"] = plan.RemovalCount,
        ["plan_digest"] = plan.Digest, ["plan_file"] = path, ["actions"] = ActionsJson(plan.Directories.Concat(plan.Files).Concat(plan.Deletions)),
        ["recovery_id"] = null, ["planned"] = CountsJson(new(plan.Files.Count, plan.Directories.Count, plan.Deletions.Count, plan.TransferBytes)),
        ["applied"] = CountsJson(new(0, 0, 0)), ["quarantined_count"] = 0, ["transferred_bytes"] = 0, ["duration_ms"] = (long)duration.TotalMilliseconds, ["error"] = null,
    };
    private static JsonObject ErrorJson(int code, string message) => new()
    {
        ["schema"] = 1, ["status"] = "error", ["device"] = null, ["source"] = null, ["destination"] = null,
        ["plan_digest"] = null, ["actions"] = new JsonArray(), ["plan_file"] = null, ["recovery_id"] = null, ["removal_count"] = 0,
        ["planned"] = CountsJson(new(0, 0, 0)), ["applied"] = CountsJson(new(0, 0, 0)), ["quarantined_count"] = 0,
        ["transferred_bytes"] = 0, ["duration_ms"] = 0, ["error"] = new JsonObject { ["code"] = code, ["message"] = message },
    };
    private static JsonObject CountsJson(OperationCounts value) => new() { ["files"] = value.Files, ["directories"] = value.Directories, ["deletions"] = value.Deletions, ["bytes"] = value.Bytes };
    private static JsonArray ActionsJson(IEnumerable<PlannedChange> actions) => new(actions.Select(change => (JsonNode)new JsonObject
    {
        ["action"] = ActionId(change.Action), ["path"] = change.Path, ["reason"] = change.Reason,
        ["type"] = change.Entry.Type.ToString().ToLowerInvariant(), ["size"] = change.Entry.Size, ["modified_seconds"] = change.Entry.ModifiedSeconds,
    }).ToArray());
    private static string ActionId(PlanAction action) => action switch
    {
        PlanAction.CreateDirectory => "create_directory", PlanAction.ReplaceWithDirectory => "replace_with_directory",
        PlanAction.AddFile => "add_file", PlanAction.UpdateFile => "update_file", PlanAction.ReplaceWithFile => "replace_with_file",
        PlanAction.DeleteFile => "delete_file", PlanAction.DeleteDirectory => "delete_directory", _ => "delete_other",
    };
    private static void WriteJson(JsonNode node) => Console.Out.WriteLine(node.ToJsonString(new JsonSerializerOptions { WriteIndented = false }));
    private sealed class InlineProgress(Action<SyncerProgress> action) : IProgress<SyncerProgress> { public void Report(SyncerProgress value) => action(value); }
}
