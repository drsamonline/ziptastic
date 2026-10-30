//! Unit tests validating container format headers (gzip, zlib) produced by the compressor.

use zopfli_rs::gzip_container::gzip_compress;
use zopfli_rs::util::SafeOptions;
use zopfli_rs::zlib_container::zlib_compress;

#[test]
fn test_gzip_container_headers_empty() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    gzip_compress(&options, b"", &mut out).unwrap();

    // Gzip empty output with fully implemented deflate (producing [3, 0]) is exactly 20 bytes:
    // Header (10 bytes): ID1=31, ID2=139, CM=8, FLG=0, MTIME=0,0,0,0, XFL=2, OS=3
    // Deflate (2 bytes): 3, 0
    // Footer: CRC32 (4 bytes little-endian) = 0, 0, 0, 0
    //         ISIZE (4 bytes little-endian) = 0, 0, 0, 0
    let expected = vec![
        31, 139, 8, 0, 0, 0, 0, 0, 2, 3, // Header
        3, 0, // Deflate body
        0, 0, 0, 0, // CRC32
        0, 0, 0, 0, // ISIZE
    ];
    assert_eq!(out, expected);
}

#[test]
fn test_gzip_container_headers_simple() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    gzip_compress(&options, b"abc", &mut out).unwrap();

    // Check header
    assert_eq!(&out[0..10], &[31, 139, 8, 0, 0, 0, 0, 0, 2, 3]);

    // Check footer
    let len = out.len();
    assert!(len >= 18);
    // CRC32 of "abc" is 0x352441c2 -> little endian [0xc2, 0x41, 0x24, 0x35]
    assert_eq!(&out[len - 8..len - 4], &[0xc2, 0x41, 0x24, 0x35]);
    // ISIZE of "abc" is 3 -> [3, 0, 0, 0]
    assert_eq!(&out[len - 4..len], &[3, 0, 0, 0]);
}

#[test]
fn test_zlib_container_headers_empty() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    zlib_compress(&options, b"", &mut out).unwrap();

    // Zlib empty output with fully implemented deflate (producing [3, 0]) is exactly 8 bytes:
    // Header (2 bytes): 120 (0x78), 218 (0xDA)
    // Deflate (2 bytes): 3, 0
    // Footer: Adler32 (4 bytes big-endian) = 0, 0, 0, 1
    let expected = vec![
        120, 218, // Header
        3, 0, // Deflate body
        0, 0, 0, 1, // Adler32
    ];
    assert_eq!(out, expected);
}

#[test]
fn test_zlib_container_headers_simple() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    zlib_compress(&options, b"abc", &mut out).unwrap();

    // Check header
    assert_eq!(&out[0..2], &[120, 218]);

    // Check footer
    let len = out.len();
    assert!(len >= 6);
    // Adler32 of "abc" is 0x024d0127 -> big endian [0x02, 0x4d, 0x01, 0x27]
    assert_eq!(&out[len - 4..len], &[0x02, 0x4d, 0x01, 0x27]);
}

#[test]
fn test_gzip_verbose_underflow_prevention() {
    let options = SafeOptions { verbose: true, ..Default::default() };
    let mut out = Vec::new();
    // Compress small input "a" (1 byte), output size will be larger (20 bytes).
    // This previously would have caused an underflow panic due to 1 - 20 in usize subtraction.
    gzip_compress(&options, b"a", &mut out).unwrap();
    assert!(out.len() > 1);
}

#[test]
fn test_zlib_verbose_underflow_prevention() {
    let options = SafeOptions { verbose: true, ..Default::default() };
    let mut out = Vec::new();
    // Compress small input "a" (1 byte), output size will be larger (10 bytes).
    // This previously would have caused an underflow panic due to 1 - 10 in usize subtraction.
    zlib_compress(&options, b"a", &mut out).unwrap();
    assert!(out.len() > 1);
}
