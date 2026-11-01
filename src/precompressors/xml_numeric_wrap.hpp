// xml_numeric_wrap.hpp — spreadsheet XML cell values coded as delta + varint.
//
// numeric_wrap handles delimited text. An .xlsx is not delimited text: precomp
// inflates it and hands us XML,
//
//   <row r="1"><c r="A1"><v>1003</v></c><c r="B1"><v>500123</v></c></row>
//
// where the numbers live inside <v> tags. The CSV transform has nothing to bite
// on there, which is the whole difference between the -50.7% recursion currently
// delivers on a nested xlsx and the -89.4% measured on that file class.
//
// Measured on a 2-column x 40,000-row sheet (docs/XML_NUMERIC_MEASUREMENT.md):
//   sheet1.xml 3,250,852 -> 148,615 through the engine today, 71,963 with this
//   transform: -51.6%. A sheet of random 9-digit integers still gains 16.5%,
//   because binary varints beat decimal characters whatever the deltas do. A
//   sheet of floats and strings declines and costs nothing.
//
// Design notes, all settled by measurement rather than argument:
//   * per-column delta state, one interleaved stream in document order. Grouping
//     the values into contiguous per-column streams measured 71,964 against
//     71,963 — nothing — so the column-grouping machinery is not worth its
//     exactness risk. Dropping column state altogether also compresses to
//     71,994 but leaves a 3x larger intermediate stream, which is a ratio risk
//     on less regular data.
//   * decline per cell, not per column. A float, a string or a non-canonical
//     integer keeps its bytes in the skeleton untouched; the rest of the sheet
//     still transforms. That is strictly better than the CSV transform's
//     all-or-nothing columns.
//
// Exactness is the whole contract, as ever: the caller verifies
// unpack(pack(x)) == x byte-for-byte before accepting, and the layout is built so
// that a wrong assignment fails that compare rather than silently mis-filling.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

// True when this build has the XML cell transform.
bool xml_numeric_available();

// Cheap pre-check: does this look like spreadsheet XML with numeric cells?
bool xml_numeric_likely(std::span<const uint8_t> in);

// Payload:
//   "XN1" | varint runs | varint skeleton_len | skeleton | stream
//   skeleton = the input with the digits of every accepted <v> removed, so each
//              accepted cell reads <v></v>; everything else is byte-identical.
//   stream   = per accepted cell, in document order, zigzag varint of the delta
//              from the previous value in that same column.
//
// Returns false (declining the whole buffer) when there is nothing to gain, when
// the buffer already contains an empty <v></v> that would make the refill
// positions ambiguous, or when the packed form is not smaller.
bool xml_numeric_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);

// Rebuilds the original bytes. Returns false rather than producing bytes it is
// not sure about; the caller still byte-compares the result before trusting it.
bool xml_numeric_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
