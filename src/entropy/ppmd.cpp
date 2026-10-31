// ppmd.cpp — thin wrapper around 7-Zip's PPMd var.H.
#include "ppmd.hpp"

#include <cstdlib>
#include <cstring>

extern "C" {
#include <Ppmd7.h>
}

namespace chce {
namespace {

constexpr unsigned kOrder = 6;
constexpr UInt32 kMemSize = 16u << 20;  // 16 MiB of model, ample for 256 KiB blocks

void* ppmd_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void ppmd_free(ISzAllocPtr, void* address) { std::free(address); }

const ISzAlloc kAlloc = {ppmd_alloc, ppmd_free};

struct OutStream {
  IByteOut vt;
  std::vector<uint8_t>* out;
};

void out_write(IByteOutPtr p, Byte b) {
  // The interface pointer is const-qualified; the struct it points into is ours
  // and is not const, so const_cast is the honest conversion here.
  const_cast<OutStream*>(reinterpret_cast<const OutStream*>(p))->out->push_back(b);
}

struct InStream {
  IByteIn vt;
  const uint8_t* cur;
  const uint8_t* end;
  bool overrun;
};

Byte in_read(IByteInPtr p) {
  InStream* s = const_cast<InStream*>(reinterpret_cast<const InStream*>(p));
  if (s->cur >= s->end) {
    s->overrun = true;
    return 0;
  }
  return *s->cur++;
}

}  // namespace

bool ppmd_available() { return true; }

bool ppmd_compress_block(std::span<const uint8_t> block, std::vector<uint8_t>& out) {
  if (block.empty()) return false;

  CPpmd7 p;
  Ppmd7_Construct(&p);
  if (!Ppmd7_Alloc(&p, kMemSize, &kAlloc)) return false;
  Ppmd7_Init(&p, kOrder);

  std::vector<uint8_t> coded;
  coded.reserve(block.size());
  OutStream os;
  os.vt.Write = out_write;
  os.out = &coded;

  // The range encoder state is the caller's job: Ppmd7z_EncodeSymbols only
  // emits symbols. Flush afterwards or the last bytes are lost.
  p.rc.enc.Stream = &os.vt;
  p.rc.enc.Low = 0;
  p.rc.enc.Range = 0xFFFFFFFF;
  p.rc.enc.CacheSize = 1;
  p.rc.enc.Cache = 0;

  Ppmd7z_EncodeSymbols(&p, block.data(), block.data() + block.size());
  Ppmd7z_Flush_RangeEnc(&p);

  Ppmd7_Free(&p, &kAlloc);

  if (coded.empty() || coded.size() >= block.size()) return false;
  out = std::move(coded);
  return true;
}

Err ppmd_decompress_block(std::span<const uint8_t> body, uint32_t out_size,
                          std::vector<uint8_t>& out) {
  if (body.size() < 5) return Err::Truncated;

  CPpmd7 p;
  Ppmd7_Construct(&p);
  if (!Ppmd7_Alloc(&p, kMemSize, &kAlloc)) return Err::Corrupt;
  Ppmd7_Init(&p, kOrder);

  InStream is;
  is.vt.Read = in_read;
  is.cur = body.data();
  is.end = body.data() + body.size();
  is.overrun = false;

  p.rc.dec.Stream = &is.vt;
  if (!Ppmd7z_RangeDec_Init(&p.rc.dec)) {
    Ppmd7_Free(&p, &kAlloc);
    return Err::Corrupt;
  }

  const size_t start = out.size();
  for (uint32_t i = 0; i < out_size; ++i) {
    const int sym = Ppmd7z_DecodeSymbol(&p);
    if (sym < 0 || is.overrun) {
      out.resize(start);
      Ppmd7_Free(&p, &kAlloc);
      return Err::Corrupt;
    }
    out.push_back(static_cast<uint8_t>(sym));
  }
  Ppmd7_Free(&p, &kAlloc);
  return Err::Ok;
}

}  // namespace chce
