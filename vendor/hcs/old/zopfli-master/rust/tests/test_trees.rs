//! Unit tests for Huffman tree building, bit length calculations, and code construction.

#![forbid(unsafe_code)]

use zopfli_rs::katajainen::length_limited_code_lengths;
use zopfli_rs::tree::{calculate_bit_lengths, calculate_entropy, lengths_to_symbols};

#[test]
fn test_single_symbol_tree() {
    let mut frequencies = vec![0; 288];
    frequencies[42] = 100; // only symbol 42 occurs

    let mut bitlengths = vec![0; 288];
    let res = length_limited_code_lengths(&frequencies, 15, &mut bitlengths);
    assert!(res.is_ok());

    // Single symbol should receive exactly 1 bit
    assert_eq!(bitlengths[42], 1);
    for (i, &len) in bitlengths.iter().enumerate() {
        if i != 42 {
            assert_eq!(len, 0);
        }
    }
}

#[test]
fn test_empty_frequencies() {
    let frequencies = vec![0; 288];
    let mut bitlengths = vec![0; 288];
    let res = length_limited_code_lengths(&frequencies, 15, &mut bitlengths);
    assert!(res.is_ok());
    for len in bitlengths {
        assert_eq!(len, 0);
    }
}

#[test]
fn test_length_limited_fibonacci() {
    // Skewed input: Fibonacci frequencies
    // Fibonacci tree has deep levels. For size 30, it can exceed 15 bits.
    let mut frequencies = vec![0; 30];
    let mut a = 1;
    let mut b = 1;
    for freq in &mut frequencies {
        let c = a + b;
        *freq = c;
        a = b;
        b = c;
    }

    let mut bitlengths = vec![0; 30];
    // We enforce maxbits = 7.
    let res = length_limited_code_lengths(&frequencies, 7, &mut bitlengths);
    assert!(res.is_ok());

    // Check that no bitlength exceeds 7
    for &len in &bitlengths {
        assert!(len <= 7);
        assert!(len > 0); // Since every symbol occurs
    }
}

#[test]
fn test_lengths_to_symbols_basic() {
    // Basic test from RFC 1951 section 3.2.2
    // Symbol  Length  Code
    // A       2.      10
    // B       1       0
    // C       3       110
    // D       3       111
    let lengths = vec![2, 1, 3, 3];
    let mut symbols = vec![0; 4];
    let res = lengths_to_symbols(&lengths, 3, &mut symbols);
    assert!(res.is_ok());

    assert_eq!(symbols[0], 0b10); // Code 2
    assert_eq!(symbols[1], 0b0); // Code 0
    assert_eq!(symbols[2], 0b110); // Code 6
    assert_eq!(symbols[3], 0b111); // Code 7
}

#[test]
fn test_calculate_entropy_basic() {
    let count = vec![10, 10, 20];
    let mut bitlengths = vec![0.0; 3];
    let res = calculate_entropy(&count, &mut bitlengths);
    assert!(res.is_ok());

    // Total = 40. P(A) = 0.25, P(B) = 0.25, P(C) = 0.5
    // -log2(0.25) = 2.0, -log2(0.5) = 1.0
    assert!((bitlengths[0] - 2.0).abs() < 1e-5);
    assert!((bitlengths[1] - 2.0).abs() < 1e-5);
    assert!((bitlengths[2] - 1.0).abs() < 1e-5);
}

#[test]
fn test_calculate_bit_lengths_skips_katajainen_if_possible() {
    // Simple balanced distribution
    let count = vec![10, 10, 10, 10];
    let mut bitlengths = vec![0; 4];
    let res = calculate_bit_lengths(&count, 15, &mut bitlengths);
    assert!(res.is_ok());
    // Should be standard Huffman depth 2
    for len in bitlengths {
        assert_eq!(len, 2);
    }
}

#[test]
fn test_length_limited_code_lengths_errors() {
    use zopfli_rs::error::Error;

    // Test SliceLengthMismatch
    let frequencies = vec![1, 2, 3, 4, 5];
    let mut bitlengths = vec![0; 4];
    let res = length_limited_code_lengths(&frequencies, 15, &mut bitlengths);
    assert_eq!(res, Err(Error::SliceLengthMismatch));

    // Test TooFewMaxBits where 2^maxbits < numsymbols
    let frequencies = vec![1, 2, 3, 4, 5]; // 5 symbols
    let mut bitlengths = vec![0; 5];
    let res = length_limited_code_lengths(&frequencies, 2, &mut bitlengths); // 2^2 = 4 < 5
    assert_eq!(res, Err(Error::TooFewMaxBits));

    // Test TooFewMaxBits with negative maxbits
    let res = length_limited_code_lengths(&frequencies, -1, &mut bitlengths);
    assert_eq!(res, Err(Error::TooFewMaxBits));

    // Test TooFewMaxBits with maxbits > 32
    let res = length_limited_code_lengths(&frequencies, 33, &mut bitlengths);
    assert_eq!(res, Err(Error::TooFewMaxBits));
}

#[test]
fn test_lengths_to_symbols_errors() {
    use zopfli_rs::error::Error;

    // Test SliceLengthMismatch
    let lengths = vec![2, 1, 3, 3];
    let mut symbols = vec![0; 3];
    let res = lengths_to_symbols(&lengths, 3, &mut symbols);
    assert_eq!(res, Err(Error::SliceLengthMismatch));

    // Test TooFewMaxBits with maxbits > 32
    let mut symbols = vec![0; 4];
    let res = lengths_to_symbols(&lengths, 33, &mut symbols);
    assert_eq!(res, Err(Error::TooFewMaxBits));
}

#[test]
fn test_lengths_to_symbols_overflow() {
    let lengths = vec![1, 1];
    let mut symbols = vec![0; 2];
    // This should not panic
    let res = lengths_to_symbols(&lengths, 32, &mut symbols);
    assert!(res.is_ok() || res.is_err());
}
