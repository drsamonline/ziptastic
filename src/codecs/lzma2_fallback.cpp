// lzma2_fallback.cpp — thin, defensive wrapper around liblzma's raw LZMA2
// encoder and decoder. Vendored source: vendor/hcs/old/xz-master (0BSD).
#include "lzma2_fallback.hpp"

#include <lzma.h>

#include <cstdlib>

namespace chce {
namespace {

// `xz -9` is preset 9 with a 64 MiB dictionary; `-9e` is the same plus
// LZMA_PRESET_EXTREME. Extreme is not a superset: measured on Silesia it wins
// clearly on some inputs (xml: -4.1% against xz -9) and loses slightly on
// others (dickens: +0.02%, 608 bytes worse). The fallback therefore encodes
// with *both* and keeps the smaller — that is what makes "never worse than
// xz -9" a measurement rather than a hope. Phase 12 may make the second pass
// optional, since it costs 2x encode time.
constexpr uint32_t kPresetNormal = 9;
constexpr uint32_t kPresetExtreme = 9 | LZMA_PRESET_EXTREME;
constexpr uint32_t kDictSize = 64u << 20;

// Upper bound on encoded filter properties. LZMA2 needs exactly one byte;
// the ceiling exists so a hostile stream cannot make us allocate more.
constexpr uint32_t kMaxPropsSize = 16;

// Builds the one-filter LZMA2 chain. `opt` must outlive the filters.
bool build_encoder_filters(lzma_options_lzma& opt, lzma_filter filters[2],
                           uint32_t preset) {
  if (lzma_lzma_preset(&opt, preset) != false) return false;  // true == error
  opt.dict_size = kDictSize;
  filters[0].id = LZMA_FILTER_LZMA2;
  filters[0].options = &opt;
  filters[1].id = LZMA_VLI_UNKNOWN;
  filters[1].options = nullptr;
  return true;
}

// One encoder pass at a given preset.
Err encode_once(std::span<const uint8_t> in, uint32_t preset,
                std::vector<uint8_t>& payload, std::vector<uint8_t>& props) {
  lzma_options_lzma opt;
  lzma_filter filters[2];
  if (!build_encoder_filters(opt, filters, preset)) return Err::Corrupt;

  // The buffer size must be asked for: liblzma sizes it per filter. LZMA2's
  // property is one byte (the dictionary-size code), but we do not hardcode
  // that — a different filter chain must not silently overflow here.
  uint32_t props_size = 0;
  if (lzma_properties_size(&props_size, &filters[0]) != LZMA_OK ||
      props_size == 0 || props_size > kMaxPropsSize) {
    return Err::Corrupt;
  }
  uint8_t props_buf[kMaxPropsSize];
  if (lzma_properties_encode(&filters[0], props_buf) != LZMA_OK) {
    return Err::Corrupt;
  }
  props.assign(props_buf, props_buf + props_size);

  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_raw_encoder(&strm, filters) != LZMA_OK) return Err::Corrupt;

  // Start optimistic and grow; LZMA2 needs little more than 1.02x on
  // incompressible data, but the growth path keeps that an observation
  // rather than an assumption.
  payload.assign(in.size() / 2 + 1024, 0);
  strm.next_in = in.data();
  strm.avail_in = in.size();

  size_t out_pos = 0;
  for (;;) {
    if (out_pos == payload.size()) payload.resize(payload.size() * 2 + 1024);
    strm.next_out = payload.data() + out_pos;
    strm.avail_out = payload.size() - out_pos;

    const lzma_ret r = lzma_code(&strm, LZMA_FINISH);
    out_pos = payload.size() - strm.avail_out;
    if (r == LZMA_STREAM_END) break;
    if (r != LZMA_OK) {
      lzma_end(&strm);
      return Err::Corrupt;
    }
  }
  lzma_end(&strm);
  payload.resize(out_pos);
  return Err::Ok;
}

}  // namespace

bool lzma2_available() { return true; }

Err lzma2_encode(std::span<const uint8_t> in, std::vector<uint8_t>& payload,
                 std::vector<uint8_t>& props) {
  if (in.empty()) return Err::BadBlockSize;

  std::vector<uint8_t> best_payload, best_props, try_payload, try_props;
  Err first_err = Err::Corrupt;
  bool have = false;

  for (uint32_t preset : {kPresetNormal, kPresetExtreme}) {
    const Err e = encode_once(in, preset, try_payload, try_props);
    if (e != Err::Ok) {
      if (!have) first_err = e;
      continue;
    }
    if (!have || try_payload.size() < best_payload.size()) {
      best_payload = std::move(try_payload);
      best_props = std::move(try_props);
      have = true;
    }
  }

  if (!have) return first_err;
  payload = std::move(best_payload);
  props = std::move(best_props);
  return Err::Ok;
}

Err lzma2_decode(std::span<const uint8_t> payload, std::span<const uint8_t> props,
                 uint64_t expected_size, std::vector<uint8_t>& out) {
  if (props.empty() || props.size() > kMaxPropsSize) {
    return Err::BadMode;
  }
  if (expected_size == 0 || expected_size > kMaxBlockSize) {
    return Err::BadBlockSize;
  }

  lzma_filter filters[2];
  filters[0].id = LZMA_FILTER_LZMA2;
  filters[0].options = nullptr;
  filters[1].id = LZMA_VLI_UNKNOWN;
  filters[1].options = nullptr;

  if (lzma_properties_decode(&filters[0], nullptr, props.data(), props.size()) !=
      LZMA_OK) {
    return Err::BadMode;
  }

  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_raw_decoder(&strm, filters) != LZMA_OK) {
    std::free(filters[0].options);
    return Err::Corrupt;
  }

  // The exact output size is known from the block header, so the decoder can
  // never be talked into allocating more than the stream declared.
  std::vector<uint8_t> buf(static_cast<size_t>(expected_size));
  strm.next_in = payload.data();
  strm.avail_in = payload.size();
  strm.next_out = buf.data();
  strm.avail_out = buf.size();

  const lzma_ret r = lzma_code(&strm, LZMA_FINISH);
  const bool complete =
      r == LZMA_STREAM_END && strm.avail_in == 0 && strm.avail_out == 0;

  lzma_end(&strm);
  std::free(filters[0].options);

  if (!complete) return Err::Corrupt;
  out = std::move(buf);
  return Err::Ok;
}

}  // namespace chce
