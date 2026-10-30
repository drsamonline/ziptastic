//! Unit tests for Adler32 and CRC32 checksum algorithms.

use zopfli_rs::checksum::{adler32, crc32};

#[test]
fn test_crc32_standard() {
    // Standard test vectors
    assert_eq!(crc32(b""), 0);
    assert_eq!(crc32(b"a"), 0xe8b7be43);
    assert_eq!(crc32(b"abc"), 0x352441c2);
    assert_eq!(crc32(b"message digest"), 0x20159d7f);
    assert_eq!(crc32(b"abcdefghijklmnopqrstuvwxyz"), 0x4c2750bd);
}

#[test]
fn test_adler32_standard() {
    // Standard test vectors
    assert_eq!(adler32(b""), 1);
    assert_eq!(adler32(b"a"), 0x00620062);
    assert_eq!(adler32(b"abc"), 0x024d0127);
    assert_eq!(adler32(b"message digest"), 0x29750586);
    assert_eq!(adler32(b"abcdefghijklmnopqrstuvwxyz"), 0x90860b20);
}
