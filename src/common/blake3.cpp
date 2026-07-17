#include "syncer/blake3.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <new>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace syncer {
namespace {

constexpr std::array<std::uint32_t, 8> kIv{
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
constexpr std::uint32_t kChunkStart = 1U;
constexpr std::uint32_t kChunkEnd = 2U;
constexpr std::uint32_t kParent = 4U;
constexpr std::uint32_t kRoot = 8U;
constexpr std::size_t kBlockLength = 64;
constexpr std::size_t kChunkLength = 1024;

constexpr std::array<std::array<std::uint8_t, 16>, 7> kSchedules{{
    {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
    {{2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8}},
    {{3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1}},
    {{10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6}},
    {{12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4}},
    {{9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7}},
    {{11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13}},
}};

std::uint32_t load32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

void store32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

void mix(std::array<std::uint32_t, 16>& state, std::size_t a, std::size_t b,
         std::size_t c, std::size_t d, std::uint32_t x, std::uint32_t y) {
    state[a] = state[a] + state[b] + x;
    state[d] = std::rotr(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = std::rotr(state[b] ^ state[c], 12);
    state[a] = state[a] + state[b] + y;
    state[d] = std::rotr(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = std::rotr(state[b] ^ state[c], 7);
}

std::array<std::uint32_t, 16> compress(const std::array<std::uint32_t, 8>& cv,
                                       const std::array<std::uint32_t, 16>& block,
                                       std::uint64_t counter, std::uint32_t block_length,
                                       std::uint32_t flags) {
    std::array<std::uint32_t, 16> state{};
    std::copy(cv.begin(), cv.end(), state.begin());
    std::copy_n(kIv.begin(), 4, state.begin() + 8);
    state[12] = static_cast<std::uint32_t>(counter);
    state[13] = static_cast<std::uint32_t>(counter >> 32U);
    state[14] = block_length;
    state[15] = flags;

    for (const auto& schedule : kSchedules) {
        mix(state, 0, 4, 8, 12, block[schedule[0]], block[schedule[1]]);
        mix(state, 1, 5, 9, 13, block[schedule[2]], block[schedule[3]]);
        mix(state, 2, 6, 10, 14, block[schedule[4]], block[schedule[5]]);
        mix(state, 3, 7, 11, 15, block[schedule[6]], block[schedule[7]]);
        mix(state, 0, 5, 10, 15, block[schedule[8]], block[schedule[9]]);
        mix(state, 1, 6, 11, 12, block[schedule[10]], block[schedule[11]]);
        mix(state, 2, 7, 8, 13, block[schedule[12]], block[schedule[13]]);
        mix(state, 3, 4, 9, 14, block[schedule[14]], block[schedule[15]]);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        state[i] ^= state[i + 8];
        state[i + 8] ^= cv[i];
    }
    return state;
}

std::array<std::uint32_t, 16> block_words(const std::array<std::uint8_t, 64>& bytes) {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = load32(bytes.data() + i * 4);
    return words;
}

struct Output {
    std::array<std::uint32_t, 8> input_cv{};
    std::array<std::uint32_t, 16> block{};
    std::uint64_t counter{};
    std::uint32_t block_length{};
    std::uint32_t flags{};

    [[nodiscard]] std::array<std::uint32_t, 8> chaining_value() const {
        const auto words = compress(input_cv, block, counter, block_length, flags);
        std::array<std::uint32_t, 8> result{};
        std::copy_n(words.begin(), 8, result.begin());
        return result;
    }

    [[nodiscard]] Digest root_digest() const {
        const auto words = compress(input_cv, block, 0, block_length, flags | kRoot);
        Digest result{};
        for (std::size_t i = 0; i < 8; ++i) store32(result.data() + i * 4, words[i]);
        return result;
    }
};

Output parent_output(const std::array<std::uint32_t, 8>& left,
                     const std::array<std::uint32_t, 8>& right) {
    Output output{};
    output.input_cv = kIv;
    std::copy(left.begin(), left.end(), output.block.begin());
    std::copy(right.begin(), right.end(), output.block.begin() + 8);
    output.block_length = 64;
    output.flags = kParent;
    return output;
}

struct ChunkState {
    std::array<std::uint32_t, 8> cv{kIv};
    std::uint64_t counter{};
    std::array<std::uint8_t, 64> block{};
    std::size_t block_length{};
    std::size_t blocks_compressed{};

    explicit ChunkState(std::uint64_t chunk_counter = 0) : counter(chunk_counter) {}
    [[nodiscard]] std::size_t length() const { return blocks_compressed * kBlockLength + block_length; }
    [[nodiscard]] std::uint32_t start_flag() const { return blocks_compressed == 0 ? kChunkStart : 0; }

    void update(std::span<const std::uint8_t> input) {
        while (!input.empty()) {
            if (block_length == kBlockLength) {
                const auto result = compress(cv, block_words(block), counter, 64, start_flag());
                std::copy_n(result.begin(), 8, cv.begin());
                ++blocks_compressed;
                block.fill(0);
                block_length = 0;
            }
            const auto take = std::min(input.size(), kBlockLength - block_length);
            std::copy_n(input.begin(), take, block.begin() + static_cast<std::ptrdiff_t>(block_length));
            block_length += take;
            input = input.subspan(take);
        }
    }

    [[nodiscard]] Output output() const {
        return Output{cv, block_words(block), counter, static_cast<std::uint32_t>(block_length),
                      start_flag() | kChunkEnd};
    }
};

} // namespace

struct Blake3::Impl {
    ChunkState chunk{};
    std::vector<std::array<std::uint32_t, 8>> stack;

    void push_chunk(std::array<std::uint32_t, 8> cv, std::uint64_t total_chunks) {
        while ((total_chunks & 1U) == 0U) {
            cv = parent_output(stack.back(), cv).chaining_value();
            stack.pop_back();
            total_chunks >>= 1U;
        }
        stack.push_back(cv);
    }
};

Blake3::Blake3() {
    static_assert(sizeof(Impl) <= std::tuple_size_v<decltype(storage_)>);
    new (storage_.data()) Impl();
}
Blake3::~Blake3() { impl()->~Impl(); }
Blake3::Impl* Blake3::impl() { return std::launder(reinterpret_cast<Impl*>(storage_.data())); }
const Blake3::Impl* Blake3::impl() const { return std::launder(reinterpret_cast<const Impl*>(storage_.data())); }

void Blake3::update(std::span<const std::uint8_t> input) {
    auto* state = impl();
    while (!input.empty()) {
        if (state->chunk.length() == kChunkLength) {
            const auto total_chunks = state->chunk.counter + 1;
            state->push_chunk(state->chunk.output().chaining_value(), total_chunks);
            state->chunk = ChunkState(total_chunks);
        }
        const auto take = std::min(input.size(), kChunkLength - state->chunk.length());
        state->chunk.update(input.first(take));
        input = input.subspan(take);
    }
}

Digest Blake3::finalize() const {
    const auto* state = impl();
    auto output = state->chunk.output();
    for (auto it = state->stack.rbegin(); it != state->stack.rend(); ++it) {
        output = parent_output(*it, output.chaining_value());
    }
    return output.root_digest();
}

std::string digest_hex(const Digest& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        output[i * 2] = digits[digest[i] >> 4U];
        output[i * 2 + 1] = digits[digest[i] & 0xFU];
    }
    return output;
}

} // namespace syncer
