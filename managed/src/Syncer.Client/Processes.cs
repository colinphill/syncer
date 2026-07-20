using System.Diagnostics;
using System.Text;

namespace Syncer;

internal sealed record ProcessResult(int ExitCode, string Output);

internal static class ProcessRunner
{
    public static async Task<ProcessResult> RunAsync(string executable, IEnumerable<string> arguments, CancellationToken cancellationToken)
    {
        await using var running = Start(executable, arguments);
        try { return await running.FinishAsync(cancellationToken).ConfigureAwait(false); }
        catch (OperationCanceledException) { running.Kill(); throw; }
    }

    public static RunningProcess Start(string executable, IEnumerable<string> arguments)
    {
        var start = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true,
            RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        try
        {
            var process = Process.Start(start) ?? throw new InvalidOperationException("process did not start");
            return new RunningProcess(process);
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception)
        { throw new SyncerException(SyncerErrorCode.Adb, $"cannot start process: {error.Message}", error); }
    }
}

internal sealed class RunningProcess : IAsyncDisposable
{
    private readonly Process process;
    private readonly Task<string> stdout;
    private readonly Task<string> stderr;
    private ProcessResult? result;
    public RunningProcess(Process process)
    {
        this.process = process; stdout = process.StandardOutput.ReadToEndAsync(); stderr = process.StandardError.ReadToEndAsync();
    }
    public string Output => result?.Output ?? string.Empty;
    public async Task<ProcessResult> FinishAsync(CancellationToken cancellationToken = default)
    {
        if (result is not null) return result;
        try { await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false); }
        catch (OperationCanceledException) { Kill(); throw; }
        var output = (await stdout.ConfigureAwait(false)) + (await stderr.ConfigureAwait(false));
        return result = new ProcessResult(process.ExitCode, output);
    }
    public void Kill()
    {
        try { if (!process.HasExited) process.Kill(true); } catch { }
    }
    public async ValueTask DisposeAsync()
    {
        if (!process.HasExited) Kill();
        try { await FinishAsync().ConfigureAwait(false); } catch { }
        process.Dispose();
    }
}
