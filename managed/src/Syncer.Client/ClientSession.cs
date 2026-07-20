using System.Net;
using System.Net.Sockets;

namespace Syncer;

internal sealed class ClientSession : IAsyncDisposable
{
    private readonly TcpClient client;
    private readonly NetworkStream stream;
    private uint requestId;
    private ClientSession(TcpClient client) { this.client = client; stream = client.GetStream(); }

    public static async Task<ClientSession> ConnectAsync(int port, string token, CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < 100; ++attempt)
        {
            cancellationToken.ThrowIfCancellationRequested(); var tcp = new TcpClient(AddressFamily.InterNetwork);
            try
            {
                await tcp.ConnectAsync(IPAddress.Loopback, port, cancellationToken).ConfigureAwait(false);
                var session = new ClientSession(tcp); var writer = new ProtocolWriter(); writer.WriteString(token); writer.WriteString(ProtocolConstants.Version);
                var request = session.NextRequest(); await ProtocolCodec.SendAsync(session.stream, new(MessageType.Hello, request, writer.ToArray()), cancellationToken).ConfigureAwait(false);
                var response = await ProtocolCodec.ReceiveAsync(session.stream, cancellationToken).ConfigureAwait(false); ThrowIfError(response);
                if (response.Type != MessageType.HelloAck || response.RequestId != request)
                    throw new SyncerException(SyncerErrorCode.Transfer, "invalid Android server handshake response");
                var reader = new ProtocolReader(response.Payload); _ = reader.ReadString(); reader.RequireEnd(); return session;
            }
            catch (OperationCanceledException) { tcp.Dispose(); throw; }
            catch (Exception error) when (error is SocketException or SyncerException or IOException)
            {
                tcp.Dispose();
                if (attempt == 99) break;
                await Task.Delay(50, cancellationToken).ConfigureAwait(false);
            }
        }
        throw new SyncerException(SyncerErrorCode.Adb, "timed out connecting to the Android server through ADB");
    }

    public Task InitializeAsync(string path, bool adopt, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(path); writer.WriteByte(adopt ? (byte)1 : (byte)0); return OperationAsync(MessageType.Initialize, writer.ToArray(), cancellationToken); }

    public async Task<IReadOnlyDictionary<string, ManifestEntry>> ScanAsync(string path, CancellationToken cancellationToken)
    {
        var writer = new ProtocolWriter(); writer.WriteString(path); var request = NextRequest();
        await ProtocolCodec.SendAsync(stream, new(MessageType.Scan, request, writer.ToArray()), cancellationToken).ConfigureAwait(false);
        var manifest = new Dictionary<string, ManifestEntry>(StringComparer.Ordinal);
        while (true)
        {
            var response = await ProtocolCodec.ReceiveAsync(stream, cancellationToken).ConfigureAwait(false); ThrowIfError(response);
            if (response.RequestId != request) throw new SyncerException(SyncerErrorCode.Transfer, "destination manifest response has wrong request ID");
            if (response.Type == MessageType.ManifestEnd) break;
            var reader = new ProtocolReader(response.Payload);
            void Add(ManifestEntry entry) { if (!manifest.TryAdd(entry.Path, entry)) throw new SyncerException(SyncerErrorCode.Transfer, "duplicate destination manifest path"); }
            if (response.Type == MessageType.ManifestEntry) Add(ProtocolCodec.ReadManifestEntry(ref reader));
            else if (response.Type == MessageType.ManifestBatch)
            {
                var count = reader.ReadUInt32(); if (count > 100_000) throw new SyncerException(SyncerErrorCode.Transfer, "destination manifest batch entry count exceeds limit");
                for (uint index = 0; index < count; ++index) Add(ProtocolCodec.ReadManifestEntry(ref reader));
            }
            else throw new SyncerException(SyncerErrorCode.Transfer, "invalid destination manifest response");
            reader.RequireEnd();
        }
        return manifest;
    }

    public Task SelectRootAsync(string path, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(path); return OperationAsync(MessageType.SelectRoot, writer.ToArray(), cancellationToken); }
    public Task RestoreAsync(string recoveryId, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(recoveryId); return OperationAsync(MessageType.Restore, writer.ToArray(), cancellationToken); }
    public Task PrepareAsync(string recoveryId, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(recoveryId); return OperationAsync(MessageType.Prepare, writer.ToArray(), cancellationToken); }
    public Task MakeDirectoryAsync(ManifestEntry entry, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(entry.Path); writer.WriteInt64(entry.ModifiedSeconds); return OperationAsync(MessageType.MakeDirectory, writer.ToArray(), cancellationToken); }
    public Task CommitFileAsync(string path, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(path); return OperationAsync(MessageType.CommitFile, writer.ToArray(), cancellationToken); }
    public Task DeletePathAsync(string path, CancellationToken cancellationToken)
    { var writer = new ProtocolWriter(); writer.WriteString(path); return OperationAsync(MessageType.DeletePath, writer.ToArray(), cancellationToken); }

    public async Task ValidatePlanAsync(IReadOnlyList<DestinationPrecondition> preconditions, CancellationToken cancellationToken)
    {
        var begin = 0;
        while (begin < preconditions.Count)
        {
            var end = begin; var encodedSize = 4;
            while (end < preconditions.Count)
            {
                var value = preconditions[end]; var next = 4 + ProtocolWriter.Utf8.GetByteCount(value.Path) + 1 + (value.Exists ? 4 + ProtocolWriter.Utf8.GetByteCount(value.Entry!.Path) + 1 + 8 + 8 : 0);
                if (end != begin && encodedSize + next > ProtocolConstants.MaximumPayload - 1024) break;
                if (encodedSize + next > ProtocolConstants.MaximumPayload) throw new SyncerException(SyncerErrorCode.Safety, "saved plan contains an oversized destination path");
                encodedSize += next; end++;
            }
            var writer = new ProtocolWriter(); writer.WriteUInt32(checked((uint)(end - begin)));
            for (var index = begin; index < end; ++index)
            {
                var value = preconditions[index]; writer.WriteString(value.Path); writer.WriteByte(value.Exists ? (byte)1 : (byte)0);
                if (value.Exists) ProtocolCodec.WriteManifestEntry(writer, value.Entry!);
            }
            await OperationAsync(MessageType.ValidatePlan, writer.ToArray(), cancellationToken).ConfigureAwait(false); begin = end;
        }
    }

    public async Task UploadAsync(SourceFile source, bool direct, IProgress<SyncerProgress>? progress, CancellationToken cancellationToken)
    {
        VerifySource(source); FileStream input;
        try { input = new FileStream(source.PhysicalPath, FileMode.Open, FileAccess.Read, FileShare.Read, ProtocolConstants.TransferChunkSize, true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SyncerException(SyncerErrorCode.SourceChanged, $"cannot open source file: {source.Entry.Path}", error); }
        await using (input)
        {
            var writer = new ProtocolWriter(); writer.WriteString(source.Entry.Path); writer.WriteUInt64(source.Entry.Size); writer.WriteInt64(source.Entry.ModifiedSeconds); writer.WriteByte(direct ? (byte)UploadPlacement.Direct : (byte)UploadPlacement.Staged);
            await OperationAsync(MessageType.PutBegin, writer.ToArray(), cancellationToken).ConfigureAwait(false);
            var hash = new Blake3(); var buffer = new byte[ProtocolConstants.TransferChunkSize]; ulong total = 0;
            while (true)
            {
                int count;
                try { count = await input.ReadAsync(buffer, cancellationToken).ConfigureAwait(false); }
                catch (OperationCanceledException) { throw; }
                catch (IOException error) { throw new SyncerException(SyncerErrorCode.SourceChanged, $"source changed while reading: {source.Entry.Path}", error); }
                if (count == 0) break;
                hash.Update(buffer.AsSpan(0, count)); total += (uint)count;
                await ProtocolCodec.SendAsync(stream, new(MessageType.PutChunk, NextRequest(), buffer[..count]), cancellationToken).ConfigureAwait(false);
                progress?.Report(new(direct ? SyncerProgressStage.TransferringFile : SyncerProgressStage.StagingFile, source.Entry.Path, null, total, source.Entry.Size));
            }
            if (total != source.Entry.Size) throw new SyncerException(SyncerErrorCode.SourceChanged, $"source changed while reading: {source.Entry.Path}");
            var end = new ProtocolWriter(); end.WriteBytes(hash.FinalizeHash()); var request = NextRequest();
            await ProtocolCodec.SendAsync(stream, new(MessageType.PutEnd, request, end.ToArray()), cancellationToken).ConfigureAwait(false);
            await ExpectOkAsync(request, cancellationToken).ConfigureAwait(false);
        }
        VerifySource(source);
    }

    public async Task ShutdownAsync()
    { try { await OperationAsync(MessageType.Shutdown, [], CancellationToken.None).ConfigureAwait(false); } catch { } }
    public void Cancel() { try { client.Client.Shutdown(SocketShutdown.Both); } catch { } }
    public ValueTask DisposeAsync() { client.Dispose(); return ValueTask.CompletedTask; }

    private uint NextRequest() => ++requestId;
    private async Task OperationAsync(MessageType type, byte[] payload, CancellationToken cancellationToken)
    { var request = NextRequest(); await ProtocolCodec.SendAsync(stream, new(type, request, payload), cancellationToken).ConfigureAwait(false); await ExpectOkAsync(request, cancellationToken).ConfigureAwait(false); }
    private async Task ExpectOkAsync(uint request, CancellationToken cancellationToken)
    {
        var response = await ProtocolCodec.ReceiveAsync(stream, cancellationToken).ConfigureAwait(false); ThrowIfError(response);
        if (response.Type != MessageType.OperationOk || response.RequestId != request) throw new SyncerException(SyncerErrorCode.Transfer, "unexpected Android server response");
    }
    private static void ThrowIfError(Frame frame)
    {
        if (frame.Type != MessageType.Error) return; var reader = new ProtocolReader(frame.Payload);
        var code = (SyncerErrorCode)reader.ReadUInt32(); var message = reader.ReadString(); reader.RequireEnd(); throw new SyncerException(code, message);
    }
    private static void VerifySource(SourceFile source)
    {
        try
        {
            var info = new FileInfo(source.PhysicalPath);
            if (!info.Exists || (ulong)info.Length != source.Entry.Size || new DateTimeOffset(info.LastWriteTimeUtc).ToUnixTimeSeconds() != source.Entry.ModifiedSeconds)
                throw new SyncerException(SyncerErrorCode.SourceChanged, $"source changed during synchronization: {source.Entry.Path}");
        }
        catch (SyncerException) { throw; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SyncerException(SyncerErrorCode.SourceChanged, $"source changed during synchronization: {source.Entry.Path}", error); }
    }
}
