// test_xml_numeric.cpp — spreadsheet XML cell values must be exact or declined.
//
// The contract is the same as the CSV transform's: every accepted buffer
// round-trips byte-for-byte, and every cell that cannot be reproduced exactly is
// left in the skeleton as raw bytes rather than normalised. A cell holding "007"
// must not come back as "7", and a sheet of floats must decline instead of being
// quietly rewritten.
#include <string>
#include <vector>

#include "harness.hpp"
#include "precompressors/xml_numeric_wrap.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string str(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

const char* kHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>";

// Two integer columns, cells carrying r attributes, exactly the shape an .xlsx
// inflates to.
std::string make_sheet(size_t rows) {
  std::string s = kHead;
  long a = 1000, b = 500000;
  for (size_t i = 1; i <= rows; ++i) {
    a += 3;
    b += static_cast<long>(i % 7);
    s += "<row r=\"" + std::to_string(i) + "\"><c r=\"A" + std::to_string(i) + "\"><v>" +
         std::to_string(a) + "</v></c><c r=\"B" + std::to_string(i) + "\"><v>" +
         std::to_string(b) + "</v></c></row>";
  }
  s += "</sheetData></worksheet>";
  return s;
}

// Same but with cells carrying styling and no r attribute: the column then comes
// from the cell's ordinal position in the row.
std::string make_sheet_no_refs(size_t rows) {
  std::string s = kHead;
  long a = -5000, b = 90;
  for (size_t i = 1; i <= rows; ++i) {
    a += 7;
    b -= 2;
    s += "<row><c s=\"1\"><v>" + std::to_string(a) + "</v></c><c s=\"1\"><v>" +
         std::to_string(b) + "</v></c></row>";
  }
  s += "</sheetData></worksheet>";
  return s;
}

void round_trip(const std::string& text, const char* what) {
  std::vector<uint8_t> packed;
  CHECK_MSG(xml_numeric_pack(bytes(text), packed), what);
  CHECK_MSG(packed.size() < text.size(), "transform did not shrink the data");
  std::vector<uint8_t> back;
  CHECK_MSG(xml_numeric_unpack(packed, back), "unpack failed");
  CHECK_MSG(str(back) == text, "round-trip is not byte-exact");
}

}  // namespace

TEST(round_trips_spreadsheet_xml) { round_trip(make_sheet(500), "declined a clean sheet"); }

TEST(round_trips_without_r_attributes) {
  round_trip(make_sheet_no_refs(500), "declined a sheet whose cells carry no r attribute");
}

TEST(round_trips_negative_and_zero_values) {
  std::string s = kHead;
  for (int i = 1; i <= 200; ++i) {
    s += "<row r=\"" + std::to_string(i) + "\"><c r=\"A" + std::to_string(i) + "\"><v>" +
         std::to_string(i % 3 == 0 ? 0 : -i * 13) + "</v></c></row>";
  }
  s += "</sheetData></worksheet>";
  round_trip(s, "declined negative and zero values");
}

// One unparseable cell must not cost the rest of the sheet, and must come back
// byte-identical.
TEST(non_canonical_cells_stay_raw_and_still_round_trip) {
  std::string s = kHead;
  for (int i = 1; i <= 300; ++i) {
    std::string v;
    if (i == 7) v = "007";          // leading zero
    else if (i == 11) v = "+5";     // explicit sign
    else if (i == 13) v = "5.0";    // float
    else if (i == 17) v = "1.2E5";  // scientific
    else if (i == 19) v = "-0";     // negative zero: canonical, so it must survive
    else v = std::to_string(1000 + i);
    s += "<row r=\"" + std::to_string(i) + "\"><c r=\"A" + std::to_string(i) + "\"><v>" + v +
         "</v></c></row>";
  }
  s += "</sheetData></worksheet>";
  round_trip(s, "declined a sheet with one odd cell");
}

TEST(declines_a_sheet_of_floats_and_strings) {
  std::string s = kHead;
  for (int i = 1; i <= 2000; ++i) {
    s += "<row r=\"" + std::to_string(i) + "\"><c r=\"A" + std::to_string(i) + "\"><v>" +
         std::to_string(i * 1.5) + ".25</v></c><c r=\"B" + std::to_string(i) +
         "\" t=\"s\"><v>text" + std::to_string(i) + "</v></c></row>";
  }
  s += "</sheetData></worksheet>";
  std::vector<uint8_t> packed;
  CHECK_MSG(!xml_numeric_pack(bytes(s), packed), "accepted a sheet with no integer cells");
}

TEST(declines_prose) {
  const std::string text =
      "This is a paragraph about spreadsheets, not a spreadsheet. It mentions "
      "<v> and <c r=\"A1\"> but carries no cell values worth coding at all.";
  std::vector<uint8_t> packed;
  CHECK_MSG(!xml_numeric_pack(bytes(text), packed), "accepted prose as spreadsheet XML");
}

// An empty <v></v> already in the input would be indistinguishable from the
// placeholders the transform creates, so the whole buffer must decline.
TEST(declines_preexisting_empty_value_tag) {
  std::string s = make_sheet(300);
  s.insert(s.find("</sheetData>"), "<row r=\"999\"><c r=\"A999\"><v></v></c></row>");
  std::vector<uint8_t> packed;
  CHECK_MSG(!xml_numeric_pack(bytes(s), packed), "accepted a sheet with an empty <v></v>");
}

// Column state has to be per column: two interleaved columns whose values move in
// opposite directions are the case a single running delta would encode badly.
TEST(keeps_per_column_state) {
  std::string s = kHead;
  for (int i = 1; i <= 400; ++i) {
    s += "<row r=\"" + std::to_string(i) + "\"><c r=\"A" + std::to_string(i) + "\"><v>" +
         std::to_string(i) + "</v></c><c r=\"B" + std::to_string(i) + "\"><v>" +
         std::to_string(1000000 - i) + "</v></c></row>";
  }
  s += "</sheetData></worksheet>";
  round_trip(s, "declined interleaved opposite-direction columns");
}

// The decoder must refuse a stream it did not write rather than guess.
TEST(decoder_rejects_damaged_payload) {
  const std::string text = make_sheet(400);
  std::vector<uint8_t> packed;
  CHECK_MSG(xml_numeric_pack(bytes(text), packed), "declined a clean sheet");

  std::vector<uint8_t> truncated(packed.begin(), packed.end() - 3);
  std::vector<uint8_t> back;
  CHECK_MSG(!xml_numeric_unpack(truncated, back), "accepted a truncated payload");

  std::vector<uint8_t> wrong_magic = packed;
  wrong_magic[1] = 'Z';
  CHECK_MSG(!xml_numeric_unpack(wrong_magic, back), "accepted a wrong magic");

  std::vector<uint8_t> extra = packed;
  extra.push_back(0);
  CHECK_MSG(!xml_numeric_unpack(extra, back), "accepted a payload with trailing bytes");
}

TEST(unpack_is_deterministic) {
  const std::string text = make_sheet(300);
  std::vector<uint8_t> a, b;
  CHECK_MSG(xml_numeric_pack(bytes(text), a), "declined a clean sheet");
  CHECK_MSG(xml_numeric_pack(bytes(text), b), "declined a clean sheet");
  CHECK_MSG(a == b, "packing the same input twice gave different bytes");
}

TEST_MAIN()
