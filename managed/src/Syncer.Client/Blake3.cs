using System.Buffers.Binary;
using System.Numerics;

namespace Syncer;

internal sealed class Blake3
{
    private const uint ChunkStart = 1, ChunkEnd = 2, Parent = 4, Root = 8;
    private const int BlockLength = 64, ChunkLength = 1024;
    private static readonly uint[] Iv = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19];
    private static readonly byte[][] Schedules =
    [
        [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],
        [2,6,3,10,7,0,4,13,1,11,12,5,9,14,15,8],
        [3,4,10,12,13,2,7,14,6,5,9,0,11,15,8,1],
        [10,7,12,9,14,3,13,15,4,0,11,2,5,8,1,6],
        [12,13,9,11,15,10,14,8,7,2,5,3,0,1,6,4],
        [9,14,11,5,8,12,15,1,13,3,0,10,2,6,4,7],
        [11,15,5,0,1,9,8,6,14,10,2,12,3,4,7,13],
    ];

    private ChunkState chunk = new(0);
    private readonly List<uint[]> stack = [];

    public void Update(ReadOnlySpan<byte> input)
    {
        while (!input.IsEmpty)
        {
            if (chunk.Length == ChunkLength)
            {
                var totalChunks = chunk.Counter + 1;
                PushChunk(chunk.GetOutput().ChainingValue(), totalChunks);
                chunk = new ChunkState(totalChunks);
            }
            var take = Math.Min(input.Length, ChunkLength - chunk.Length);
            chunk.Update(input[..take]); input = input[take..];
        }
    }

    public byte[] FinalizeHash()
    {
        var output = chunk.GetOutput();
        for (var index = stack.Count - 1; index >= 0; --index)
            output = ParentOutput(stack[index], output.ChainingValue());
        return output.RootDigest();
    }

    public static byte[] Hash(ReadOnlySpan<byte> data) { var hash = new Blake3(); hash.Update(data); return hash.FinalizeHash(); }
    public static string Hex(ReadOnlySpan<byte> digest) => Convert.ToHexString(digest).ToLowerInvariant();

    private void PushChunk(uint[] value, ulong totalChunks)
    {
        while ((totalChunks & 1) == 0)
        {
            value = ParentOutput(stack[^1], value).ChainingValue(); stack.RemoveAt(stack.Count - 1); totalChunks >>= 1;
        }
        stack.Add(value);
    }

    private static void Mix(uint[] state, int a, int b, int c, int d, uint x, uint y)
    {
        state[a] = unchecked(state[a] + state[b] + x); state[d] = BitOperations.RotateRight(state[d] ^ state[a], 16);
        state[c] = unchecked(state[c] + state[d]); state[b] = BitOperations.RotateRight(state[b] ^ state[c], 12);
        state[a] = unchecked(state[a] + state[b] + y); state[d] = BitOperations.RotateRight(state[d] ^ state[a], 8);
        state[c] = unchecked(state[c] + state[d]); state[b] = BitOperations.RotateRight(state[b] ^ state[c], 7);
    }

    private static uint[] Compress(uint[] cv, uint[] block, ulong counter, uint blockLength, uint flags)
    {
        var state = new uint[16]; Array.Copy(cv, state, 8); Array.Copy(Iv, 0, state, 8, 4);
        state[12] = (uint)counter; state[13] = (uint)(counter >> 32); state[14] = blockLength; state[15] = flags;
        foreach (var s in Schedules)
        {
            Mix(state,0,4,8,12,block[s[0]],block[s[1]]); Mix(state,1,5,9,13,block[s[2]],block[s[3]]);
            Mix(state,2,6,10,14,block[s[4]],block[s[5]]); Mix(state,3,7,11,15,block[s[6]],block[s[7]]);
            Mix(state,0,5,10,15,block[s[8]],block[s[9]]); Mix(state,1,6,11,12,block[s[10]],block[s[11]]);
            Mix(state,2,7,8,13,block[s[12]],block[s[13]]); Mix(state,3,4,9,14,block[s[14]],block[s[15]]);
        }
        for (var index = 0; index < 8; ++index) { state[index] ^= state[index + 8]; state[index + 8] ^= cv[index]; }
        return state;
    }

    private static uint[] BlockWords(ReadOnlySpan<byte> bytes)
    {
        var words = new uint[16];
        for (var index = 0; index < 16; ++index) words[index] = BinaryPrimitives.ReadUInt32LittleEndian(bytes[(index * 4)..]);
        return words;
    }

    private static Output ParentOutput(uint[] left, uint[] right)
    {
        var block = new uint[16]; Array.Copy(left, block, 8); Array.Copy(right, 0, block, 8, 8);
        return new Output((uint[])Iv.Clone(), block, 0, 64, Parent);
    }

    private sealed record Output(uint[] InputCv, uint[] Block, ulong Counter, uint Length, uint Flags)
    {
        public uint[] ChainingValue() => Compress(InputCv, Block, Counter, Length, Flags)[..8];
        public byte[] RootDigest()
        {
            var words = Compress(InputCv, Block, 0, Length, Flags | Root); var result = new byte[32];
            for (var index = 0; index < 8; ++index) BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(index * 4), words[index]);
            return result;
        }
    }

    private sealed class ChunkState(ulong counter)
    {
        private uint[] cv = (uint[])Iv.Clone();
        private readonly byte[] block = new byte[64];
        private int blockLength, blocksCompressed;
        public ulong Counter { get; } = counter;
        public int Length => blocksCompressed * BlockLength + blockLength;
        private uint StartFlag => blocksCompressed == 0 ? ChunkStart : 0;
        public void Update(ReadOnlySpan<byte> input)
        {
            while (!input.IsEmpty)
            {
                if (blockLength == BlockLength)
                {
                    cv = Compress(cv, BlockWords(block), Counter, 64, StartFlag)[..8]; blocksCompressed++; Array.Clear(block); blockLength = 0;
                }
                var take = Math.Min(input.Length, BlockLength - blockLength); input[..take].CopyTo(block.AsSpan(blockLength)); blockLength += take; input = input[take..];
            }
        }
        public Output GetOutput() => new((uint[])cv.Clone(), BlockWords(block), Counter, (uint)blockLength, StartFlag | ChunkEnd);
    }
}
