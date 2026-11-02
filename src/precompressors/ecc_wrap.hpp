// ecc_wrap.hpp — raw CD sector ECC regenerated instead of stored.
//
// A raw CD image is 2,352-byte sectors: 12 sync + 4 header + (subheader) + 2,048 data
// + 4 EDC + 276 ECC. The parity is a Reed-Solomon product code over the data, so it is
// fully determined by the bytes before it — storing it costs 276 bytes a sector that
// could be recomputed.
//
// Measured (`ECM_MEASUREMENT.md`): on a synthetic Mode-1 image with a realistic
// user-data mix, 30.8% of the *compressed* size was parity. The 12% figure that used to
// be budgeted was uncompressed arithmetic; RS parity looks random, so it is
// incompressible and costs its full size in the output.
//
// **This transform is its own conformance test.** The ECC is recomputed and compared
// with what the image already holds, per sector, before the payload is returned. A wrong
// table or loop cannot round-trip a real image — it declines it. That matters more here
// than anywhere else: without the check, a broken implementation would still produce a
// perfect round trip on its own output while doing nothing on real discs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool ecc_available();

// Sector sync 00 FF*10 00 at offset 0 and a size that is a whole number of sectors.
bool ecc_likely(std::span<const uint8_t> in);

// Payload: varint sector_count | one kind byte per sector (0 = ECC regenerated,
// 1 = kept whole) | the sectors, with the 276 ECC bytes omitted for kind 0.
bool ecc_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);
bool ecc_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
