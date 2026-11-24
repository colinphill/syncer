using System.Buffers.Binary;
using System.Text;

namespace Syncer;

internal static class ProtocolConstants
{
    public const string Version = "0.2.0";
    public const ushort ProtocolVersion = 7;
    public const uint Magic = 0x53594e43;
    public const int MaximumPayload = 2 * 1024 * 1024;
    public const int TransferChunkSize = 1024 * 1024;
    public const string ReservedDirectory = ".syncer";
    /// <summary>Scan-entry flag: the file time came from the destination filesystem, not the timestamp catalog.</summary>
    public const byte ScanEntryUntrackedTime = 0x01;
}

internal enum MessageType : ushort
{
    Hello = 1, HelloAck = 2, Initialize = 3, Scan = 4, ManifestEntry = 5,
    ManifestEnd = 6, MakeDirectory = 7, PutBegin = 8, PutChunk = 9,
    PutEnd = 10, CommitFile = 11, DeletePath = 12, SetMtime = 13,
    OperationOk = 14, Error = 15, Shutdown = 16, ManifestBatch = 17,
    Prepare = 18, SelectRoot = 19, ValidatePlan = 20, Restore = 21,
    RecordTimes = 22,
}

internal enum UploadPlacement : byte { Staged = 0, Direct = 1 }
internal sealed record Frame(MessageType Type, uint RequestId, byte[] Payload);

internal sealed class ProtocolWriter
{
    private readonly MemoryStream stream = new();
    public int Length => checked((int)stream.Length);
    public void WriteByte(byte value) => stream.WriteByte(value);
    public void WriteUInt16(ushort value) { Span<byte> bytes = stackalloc byte[2]; BinaryPrimitives.WriteUInt16BigEndian(bytes, value); stream.Write(bytes); }
    public void WriteUInt32(uint value) { Span<byte> bytes = stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(bytes, value); stream.Write(bytes); }
    public void WriteUInt64(ulong value) { Span<byte> bytes = stackalloc byte[8]; BinaryPrimitives.WriteUInt64BigEndian(bytes, value); stream.Write(bytes); }
    public void WriteInt64(long value) => WriteUInt64(unchecked((ulong)value));
    public void WriteBytes(ReadOnlySpan<byte> value) => stream.Write(value);
    public void WriteString(string value)
    {
        byte[] bytes;
        try { bytes = Utf8.GetBytes(value); }
        catch (EncoderFallbackException error) { throw new SyncerException(SyncerErrorCode.Transfer, "string is not valid UTF-8", error); }
        WriteUInt32(checked((uint)bytes.Length));
        stream.Write(bytes);
    }
    public byte[] ToArray() => stream.ToArray();
    internal static readonly UTF8Encoding Utf8 = new(false, true);
}

internal ref struct ProtocolReader
{
    private readonly ReadOnlySpan<byte> data;
    private int position;
    public ProtocolReader(ReadOnlySpan<byte> data) { this.data = data; position = 0; }
    private ReadOnlySpan<byte> Take(int length)
    {
        if (length < 0 || length > data.Length - position)
            throw new SyncerException(SyncerErrorCode.Transfer, "truncated protocol payload");
        var result = data.Slice(position, length); position += length; return result;
    }
    public byte ReadByte() => Take(1)[0];
    public ushort ReadUInt16() => BinaryPrimitives.ReadUInt16BigEndian(Take(2));
    public uint ReadUInt32() => BinaryPrimitives.ReadUInt32BigEndian(Take(4));
    public ulong ReadUInt64() => BinaryPrimitives.ReadUInt64BigEndian(Take(8));
    public long ReadInt64() => unchecked((long)ReadUInt64());
    public byte[] ReadBytes(int length) => Take(length).ToArray();
    public string ReadString()
    {
        var length = ReadUInt32();
        if (length > int.MaxValue) throw new SyncerException(SyncerErrorCode.Transfer, "protocol string is too large");
        try { return ProtocolWriter.Utf8.GetString(Take((int)length)); }
        catch (DecoderFallbackException error) { throw new SyncerException(SyncerErrorCode.Transfer, "protocol string is not valid UTF-8", error); }
    }
    public void RequireEnd()
    {
        if (position != data.Length) throw new SyncerException(SyncerErrorCode.Transfer, "unexpected protocol payload data");
    }
}

internal static class ProtocolCodec
{
    public static byte[] EncodeHeader(Frame frame)
    {
        if (frame.Payload.Length > ProtocolConstants.MaximumPayload)
            throw new SyncerException(SyncerErrorCode.Transfer, "protocol frame is too large");
        var bytes = new byte[16];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16BigEndian(bytes.AsSpan(4), ProtocolConstants.ProtocolVersion);
        BinaryPrimitives.WriteUInt16BigEndian(bytes.AsSpan(6), (ushort)frame.Type);
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(8), frame.RequestId);
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(12), (uint)frame.Payload.Length);
        return bytes;
    }

    public static Frame DecodeHeader(ReadOnlySpan<byte> header)
    {
        if (header.Length != 16) throw new SyncerException(SyncerErrorCode.Transfer, "truncated protocol header");
        if (BinaryPrimitives.ReadUInt32BigEndian(header) != ProtocolConstants.Magic)
            throw new SyncerException(SyncerErrorCode.Transfer, "invalid protocol magic");
        if (BinaryPrimitives.ReadUInt16BigEndian(header[4..]) != ProtocolConstants.ProtocolVersion)
            throw new SyncerException(SyncerErrorCode.Transfer, "incompatible protocol version");
        var length = BinaryPrimitives.ReadUInt32BigEndian(header[12..]);
        if (length > ProtocolConstants.MaximumPayload)
            throw new SyncerException(SyncerErrorCode.Transfer, "protocol frame exceeds limit");
        return new Frame((MessageType)BinaryPrimitives.ReadUInt16BigEndian(header[6..]),
            BinaryPrimitives.ReadUInt32BigEndian(header[8..]), new byte[length]);
    }

    public static async Task SendAsync(Stream stream, Frame frame, CancellationToken cancellationToken)
    {
        try
        {
            await stream.WriteAsync(EncodeHeader(frame), cancellationToken).ConfigureAwait(false);
            if (frame.Payload.Length != 0)
                await stream.WriteAsync(frame.Payload, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception error) when (error is IOException or ObjectDisposedException)
        { throw new SyncerException(SyncerErrorCode.Transfer, $"socket send failed: {error.Message}", error); }
    }

    public static async Task<Frame> ReceiveAsync(Stream stream, CancellationToken cancellationToken)
    {
        try
        {
            var header = new byte[16];
            await stream.ReadExactlyAsync(header, cancellationToken).ConfigureAwait(false);
            var frame = DecodeHeader(header);
            if (frame.Payload.Length != 0)
                await stream.ReadExactlyAsync(frame.Payload, cancellationToken).ConfigureAwait(false);
            return frame;
        }
        catch (OperationCanceledException) { throw; }
        catch (SyncerException) { throw; }
        catch (Exception error) when (error is IOException or ObjectDisposedException or EndOfStreamException)
        { throw new SyncerException(SyncerErrorCode.Transfer, $"socket receive failed: {error.Message}", error); }
    }

    public static void WriteManifestEntry(ProtocolWriter writer, ManifestEntry entry)
    {
        writer.WriteString(entry.Path); writer.WriteByte((byte)entry.Type);
        writer.WriteUInt64(entry.Size); writer.WriteInt64(entry.ModifiedSeconds);
    }

    public static void WriteScanEntry(ProtocolWriter writer, ManifestEntry entry, byte flags)
    { WriteManifestEntry(writer, entry); writer.WriteByte(flags); }

    public static (ManifestEntry Entry, byte Flags) ReadScanEntry(ref ProtocolReader reader)
    {
        var entry = ReadManifestEntry(ref reader); var flags = reader.ReadByte();
        if ((flags & ~ProtocolConstants.ScanEntryUntrackedTime) != 0 ||
            ((flags & ProtocolConstants.ScanEntryUntrackedTime) != 0 && entry.Type != EntryType.File))
            throw new SyncerException(SyncerErrorCode.Transfer, "invalid manifest entry flags");
        return (entry, flags);
    }

    public static ManifestEntry ReadManifestEntry(ref ProtocolReader reader)
    {
        var entry = new ManifestEntry(reader.ReadString(), (EntryType)reader.ReadByte(), reader.ReadUInt64(), reader.ReadInt64());
        if (!Planner.IsValidRelativePath(entry.Path))
            throw new SyncerException(SyncerErrorCode.Transfer, "invalid relative path in manifest");
        if (entry.Type is not (EntryType.File or EntryType.Directory or EntryType.Other))
            throw new SyncerException(SyncerErrorCode.Transfer, "invalid manifest entry type");
        return entry;
    }
}
