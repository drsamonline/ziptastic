//! Unit tests for DEFLATE symbols, run-length coding, and length/distance conversion helpers.

#![forbid(unsafe_code)]

use zopfli_rs::symbols::*;

#[test]
fn test_dist_lookups_small() {
    // Exact mapping check for dist < 5
    assert_eq!(get_dist_symbol(1), 0);
    assert_eq!(get_dist_symbol(2), 1);
    assert_eq!(get_dist_symbol(3), 2);
    assert_eq!(get_dist_symbol(4), 3);
    assert_eq!(get_dist_symbol(5), 4); // dist 5 -> symbol 4

    assert_eq!(get_dist_extra_bits(1), 0);
    assert_eq!(get_dist_extra_bits(2), 0);
    assert_eq!(get_dist_extra_bits(3), 0);
    assert_eq!(get_dist_extra_bits(4), 0);
    assert_eq!(get_dist_extra_bits(5), 1);

    assert_eq!(get_dist_extra_bits_value(1), 0);
    assert_eq!(get_dist_extra_bits_value(2), 0);
    assert_eq!(get_dist_extra_bits_value(3), 0);
    assert_eq!(get_dist_extra_bits_value(4), 0);
    assert_eq!(get_dist_extra_bits_value(5), 0);
    assert_eq!(get_dist_extra_bits_value(6), 1);
}

#[test]
fn test_dist_lookups_boundaries() {
    // Test boundaries and power-of-twos up to max window size (32768)
    // Dist 192, 193
    assert_eq!(get_dist_symbol(192), 14);
    assert_eq!(get_dist_symbol(193), 15);

    // Dist 2048, 2049
    assert_eq!(get_dist_symbol(2048), 21);
    assert_eq!(get_dist_symbol(2049), 22);

    // Extra bits
    assert_eq!(get_dist_extra_bits(192), 6);
    assert_eq!(get_dist_extra_bits(193), 6);
    assert_eq!(get_dist_extra_bits(257), 7);
    assert_eq!(get_dist_extra_bits(2048), 9);
    assert_eq!(get_dist_extra_bits(2049), 10);
    assert_eq!(get_dist_extra_bits(32768), 13);

    // Extra bits values
    assert_eq!(get_dist_extra_bits_value(192), 63); // 192 - (1 + 128) = 63
    assert_eq!(get_dist_extra_bits_value(193), 0); // 193 - (1 + 192) = 0
}

#[test]
fn test_length_lookups() {
    // Check ranges for length 3..=258
    assert_eq!(get_length_symbol(3), 257);
    assert_eq!(get_length_symbol(4), 258);
    assert_eq!(get_length_symbol(257), 284);
    assert_eq!(get_length_symbol(258), 285);

    assert_eq!(get_length_extra_bits(3), 0);
    assert_eq!(get_length_extra_bits(4), 0);
    assert_eq!(get_length_extra_bits(11), 1);
    assert_eq!(get_length_extra_bits(12), 1);
    assert_eq!(get_length_extra_bits(258), 0);

    assert_eq!(get_length_extra_bits_value(3), 0);
    assert_eq!(get_length_extra_bits_value(11), 0);
    assert_eq!(get_length_extra_bits_value(12), 1);
    assert_eq!(get_length_extra_bits_value(258), 0);
}

#[test]
fn test_symbol_extra_bits() {
    // Length symbol extra bits
    assert_eq!(get_length_symbol_extra_bits(257), 0);
    assert_eq!(get_length_symbol_extra_bits(265), 1);
    assert_eq!(get_length_symbol_extra_bits(285), 0);

    // Distance symbol extra bits
    assert_eq!(get_dist_symbol_extra_bits(0), 0);
    assert_eq!(get_dist_symbol_extra_bits(4), 1);
    assert_eq!(get_dist_symbol_extra_bits(29), 13);
}
