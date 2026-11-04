// block_index.hpp — reaching a block without walking the stream.
//
// Until format v8 the only way to find a block was to decode from the front: the
// trailer recorded how many blocks there were and a digest over their headers, but not
// where any of them started. Every block already carries its own SHA-256 and, in
// parallel mode, is decoded against itself, so the blocks were independently decodable
// and simply unreachable.
//
// v8 adds a table of absolute block offsets to the trailer. This header is the reader
// for it. What it buys, in the order the release plan asked for it:
//
//   - random access      — decode one block without reading the others;
//   - resumable decode   — restart at a block boundary after a crash;
//   - parallel decode    — hand blocks to threads without a sequential scan first;
//   - hatch-style extraction, which is heavenzip's to build (docs/HATCHING.md) and
//     needs addressable blocks to exist here first.
//
// The index is verified, not trusted. `read_index` reads every block *header* at its
// recorded offset and re-derives the manifest digest from them, so a table that points
// somewhere else is refused with manifest-mismatch — the same error a dropped or
// reordered block produces. It reads no payload byte: on a 100-block archive that is a
// few kilobytes instead of the whole file.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/block.hpp"
#include "chimera/format.hpp"

namespace chce {

// Where every block is, and what it claims to be. Positions are absolute within the
// buffer handed to `read_index`, which may carry an SFX stub or a joined split set in
// front of the stream; `stream_offset` says where the stream itself begins.
struct StreamIndex {
  FileHeader header;
  uint64_t stream_offset = 0;   // offset of "CHCE" in the buffer
  uint64_t trailer_offset = 0;  // offset of "ZTIX" in the buffer
  // Per block, in order. `block_offsets` points at the block header, `body_offsets` at
  // the payload that follows it.
  std::vector<uint64_t> block_offsets;
  std::vector<uint64_t> body_offsets;
  std::vector<BlockHeader> blocks;
  // Bytes the index actually touched: the stream header, the trailer and every block
  // header. The number to quote against the size of the stream.
  uint64_t bytes_read = 0;

  size_t size() const { return blocks.size(); }

  // Where this block's plaintext belongs in the decoded stream, as a sum of the
  // uncompressed sizes before it. Exact, because the block loop emits every input byte
  // exactly once.
  uint64_t uncompressed_offset(uint64_t index) const;
};

// Reads the header, jumps to the trailer by arithmetic from the end of the buffer, reads
// the offset table, then validates it by reading each block header at its offset and
// recomputing the manifest. Reads no payload.
Err read_index(std::span<const uint8_t> in, StreamIndex& out);

// True when a block may be decoded on its own: parallel mode encodes every block against
// itself, so no block references another. Archive mode does not, which is the whole
// difference between the two modes.
bool blocks_are_independently_decodable(const FileHeader& h);

// True when no Layer 0 or stream-level transform ran, so a block's plaintext is the
// caller's bytes rather than an intermediate of the inverse chain.
bool stream_is_untransformed(const FileHeader& h);

// Decodes blocks [first, last] inclusive and appends their plaintext to `out`.
//
// Requires a stream whose blocks are independently decodable and untransformed: a
// partial decode cannot run the inverse chain, which needs the whole stream. Anything
// else is refused with not-self-contained rather than answered wrongly.
//
// Each block's SHA-256 is checked before its bytes are appended, so a damaged block is
// named by index and contributes nothing. When the range covers every block the
// whole-stream CRC-32 is checked too, which makes `first = 0` a complete verified decode.
//
// Implemented in decompressor.cpp, beside the per-mode decoder it calls.
Err decode_blocks(std::span<const uint8_t> in, const StreamIndex& index, uint64_t first,
                  uint64_t last, std::vector<uint8_t>& out, uint64_t* bad_block = nullptr);

// The whole stream, decoded on `threads` threads, each taking a contiguous run of blocks
// from the index. This is the fourth feature the release plan listed — parallel decode
// without a sequential scan.
//
// It is a separate entry point rather than a rewrite of `decompress()`, because the
// measurement did not support a rewrite: on a 225-block, 1.38 MB archive the two paths
// cannot be told apart (810–817 ms walking against 800–842 ms from the index, with a ±40 ms
// spread between runs on two cores), and the table cannot be the reason either way —
// removing a 10 KB header scan from an 800 ms decode is microseconds. See FINDINGS.md §3c.
//
// Where the index genuinely pays is a caller that never holds the whole stream: with an
// index in hand it reads each block's bytes and no others, and that is I/O this project
// has not measured because its own API takes a whole buffer.
//
// Same refusals as `decode_blocks`, and the same promise when it succeeds: every block's
// SHA-256 checked before its bytes are kept, the whole-stream CRC-32 checked at the end.
Err decode_blocks_parallel(std::span<const uint8_t> in, const StreamIndex& index,
                           unsigned threads, std::vector<uint8_t>& out,
                           uint64_t* bad_block = nullptr);

}  // namespace chce
