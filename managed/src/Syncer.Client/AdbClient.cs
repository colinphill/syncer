namespace Syncer;

internal sealed class AdbClient
{
    private readonly string executable;
    private readonly ServerBinaries servers;
    private AdbClient(string executable, string serial, ServerBinaries servers) { this.executable = executable; Serial = serial; this.servers = servers; }
    public string Serial { get; }

    public static async Task<AdbClient> CreateAsync(SyncerClientOptions options, string? fallbackSerial, CancellationToken cancellationToken)
    {
        var executable = Locate(options.AdbPath); var requested = options.DeviceSerial ?? fallbackSerial ?? Environment.GetEnvironmentVariable("ANDROID_SERIAL");
        var servers = new ServerBinaries(options.ServerDirectory);
        if (!string.IsNullOrWhiteSpace(requested))
        {
            var state = await ProcessRunner.RunAsync(executable, ["-s", requested, "get-state"], cancellationToken).ConfigureAwait(false);
            if (state.ExitCode != 0 || LastLine(state.Output) != "device") throw new SyncerException(SyncerErrorCode.Adb, $"selected ADB device is not ready: {requested}");
            return new(executable, requested, servers);
        }
        var result = await ProcessRunner.RunAsync(executable, ["devices"], cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Adb, $"adb devices failed: {result.Output.Trim()}");
        var devices = AdbOutput.ReadyDevices(result.Output);
        if (devices.Length == 0) throw new SyncerException(SyncerErrorCode.Adb, "no ready ADB device was found");
        if (devices.Length != 1) throw new SyncerException(SyncerErrorCode.Adb, "multiple ADB devices are ready; select one with DeviceSerial");
        return new(executable, devices[0], servers);
    }

    public async Task<string> DeployServerAsync(CancellationToken cancellationToken)
    {
        var abiResult = await RunAsync(["shell", "getprop", "ro.product.cpu.abilist"], cancellationToken).ConfigureAwait(false);
        if (abiResult.ExitCode != 0 || string.IsNullOrEmpty(LastLine(abiResult.Output)))
            abiResult = await RunAsync(["shell", "getprop", "ro.product.cpu.abi"], cancellationToken).ConfigureAwait(false);
        if (abiResult.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Adb, "cannot query Android CPU ABI");
        var reported = LastLine(abiResult.Output); var abi = AdbOutput.SelectAbi(reported);
        if (abi is null) throw new SyncerException(SyncerErrorCode.Adb, $"device has no packaged server ABI: {reported}");
        var local = await servers.ResolveAsync(abi, cancellationToken).ConfigureAwait(false);
        var digest = await FileDigestAsync(local, cancellationToken).ConfigureAwait(false);
        var directory = $"/data/local/tmp/syncer/{ProtocolConstants.Version}/{digest}"; var remote = directory + "/syncerd";
        var result = await RunAsync(["shell", "test", "-x", remote], cancellationToken).ConfigureAwait(false);
        if (result.ExitCode == 0) return remote;
        result = await RunAsync(["shell", "mkdir", "-p", directory], cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Adb, $"cannot create Android deployment directory: {result.Output.Trim()}");
        result = await RunAsync(["push", local, remote], cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Adb, $"cannot deploy Android server: {result.Output.Trim()}");
        result = await RunAsync(["shell", "chmod", "755", remote], cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Adb, $"cannot make Android server executable: {result.Output.Trim()}");
        return remote;
    }

    public async Task<int> CreateForwardAsync(string socketName, CancellationToken cancellationToken)
    {
        var result = await RunAsync(["forward", "tcp:0", "localabstract:" + socketName], cancellationToken).ConfigureAwait(false);
        if (result.ExitCode != 0) throw new SyncerException(SyncerErrorCode.Adb, $"cannot create ADB forwarding: {result.Output.Trim()}");
        if (!AdbOutput.TryParsePort(LastLine(result.Output), out var port))
            throw new SyncerException(SyncerErrorCode.Adb, $"ADB returned an invalid forwarded port: {LastLine(result.Output)}");
        return port;
    }
    public async Task RemoveForwardAsync(int port)
    { try { await RunAsync(["forward", "--remove", $"tcp:{port}"], CancellationToken.None).ConfigureAwait(false); } catch { } }
    public RunningProcess LaunchServer(string remotePath, string socketName, string token) =>
        ProcessRunner.Start(executable, ["-s", Serial, "shell", remotePath, "--socket", socketName, "--token", token]);
    private Task<ProcessResult> RunAsync(IEnumerable<string> arguments, CancellationToken cancellationToken) =>
        ProcessRunner.RunAsync(executable, ["-s", Serial, .. arguments], cancellationToken);

    private static string Locate(string? requested)
    {
        if (!string.IsNullOrWhiteSpace(requested))
        {
            if (!File.Exists(requested)) throw new SyncerException(SyncerErrorCode.Adb, "AdbPath does not name an existing file");
            return Path.GetFullPath(requested);
        }
        foreach (var variable in new[] { "ANDROID_HOME", "ANDROID_SDK_ROOT" })
        {
            var root = Environment.GetEnvironmentVariable(variable);
            if (!string.IsNullOrWhiteSpace(root)) { var value = Path.Combine(root, "platform-tools", OperatingSystem.IsWindows() ? "adb.exe" : "adb"); if (File.Exists(value)) return value; }
        }
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (!string.IsNullOrEmpty(home))
        {
            var value = OperatingSystem.IsMacOS() ? Path.Combine(home, "Library", "Android", "sdk", "platform-tools", "adb") :
                OperatingSystem.IsWindows() ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk", "platform-tools", "adb.exe") :
                Path.Combine(home, "Android", "Sdk", "platform-tools", "adb");
            if (File.Exists(value)) return value;
        }
        var name = OperatingSystem.IsWindows() ? "adb.exe" : "adb";
        foreach (var directory in (Environment.GetEnvironmentVariable("PATH") ?? string.Empty).Split(Path.PathSeparator))
        { var value = Path.Combine(directory.Length == 0 ? Environment.CurrentDirectory : directory, name); if (File.Exists(value)) return Path.GetFullPath(value); }
        throw new SyncerException(SyncerErrorCode.Adb, "adb was not found; set AdbPath or install Android platform-tools");
    }
    private static string LastLine(string value) => value.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries).Select(line => line.Trim()).LastOrDefault(line => line.Length != 0) ?? string.Empty;
    private static async Task<string> FileDigestAsync(string path, CancellationToken cancellationToken)
    {
        try
        {
            await using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, true); var hash = new Blake3(); var buffer = new byte[65536];
            while (await input.ReadAsync(buffer, cancellationToken).ConfigureAwait(false) is var count && count != 0) hash.Update(buffer.AsSpan(0, count));
            return Blake3.Hex(hash.FinalizeHash());
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SyncerException(SyncerErrorCode.Adb, "cannot read packaged Android server", error); }
    }
}

internal static class AdbOutput
{
    private static readonly string[] SupportedAbis = ["arm64-v8a", "armeabi-v7a", "x86_64", "x86"];
    public static string[] ReadyDevices(string output) => output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
        .Select(line => line.Split('\t')).Where(parts => parts.Length >= 2 && parts[1].Trim() == "device").Select(parts => parts[0]).ToArray();
    public static string? SelectAbi(string reported) => reported.Split(',').FirstOrDefault(value => SupportedAbis.Contains(value, StringComparer.Ordinal));
    public static bool TryParsePort(string value, out int port) => int.TryParse(value, out port) && port is > 0 and <= 65535;
}
