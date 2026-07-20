using System.Reflection;

namespace Syncer;

internal sealed class ServerBinaries(string? requestedDirectory)
{
    private static readonly string[] Abis = ["arm64-v8a", "armeabi-v7a", "x86_64", "x86"];

    public async Task<string> ResolveAsync(string abi, CancellationToken cancellationToken)
    {
        if (!Abis.Contains(abi, StringComparer.Ordinal))
            throw new SyncerException(SyncerErrorCode.Adb, $"device has no packaged server ABI: {abi}");
        if (!string.IsNullOrWhiteSpace(requestedDirectory))
        {
            var direct = Path.Combine(requestedDirectory, abi, "syncerd");
            var nested = Path.Combine(requestedDirectory, "servers", abi, "syncerd");
            var value = File.Exists(direct) ? direct : nested;
            if (!File.Exists(value)) throw new SyncerException(SyncerErrorCode.Adb, $"packaged Android server is missing: {value}");
            return Path.GetFullPath(value);
        }

        var resource = $"Syncer.Servers.{abi}.syncerd"; var assembly = typeof(ServerBinaries).Assembly;
        await using var input = assembly.GetManifestResourceStream(resource);
        if (input is null) throw new SyncerException(SyncerErrorCode.Adb,
            "the client assembly does not contain Android servers; install the packaged NuGet or set ServerDirectory");
        using var memory = new MemoryStream(); await input.CopyToAsync(memory, cancellationToken).ConfigureAwait(false);
        var bytes = memory.ToArray(); var digest = Blake3.Hex(Blake3.Hash(bytes));
        var cacheRoot = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(cacheRoot)) cacheRoot = Path.GetTempPath();
        var target = Path.Combine(cacheRoot, "syncer", "servers", ProtocolConstants.Version, digest, abi, "syncerd");
        if (File.Exists(target)) return target;
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var temporary = target + ".tmp." + Guid.NewGuid().ToString("N");
        try
        {
            await File.WriteAllBytesAsync(temporary, bytes, cancellationToken).ConfigureAwait(false);
            try { File.Move(temporary, target, false); }
            catch (IOException) when (File.Exists(target)) { File.Delete(temporary); }
            return target;
        }
        catch { try { File.Delete(temporary); } catch { } throw; }
    }
}
