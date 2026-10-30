//! Unit tests for the DEFLATE stream coding and low-level bit writing engine.

use zopfli_rs::deflate::deflate;
use zopfli_rs::util::{BlockType, SafeOptions};

#[test]
fn test_deflate_empty_input() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    let mut bp = 0u8;

    // Deflating an empty input should succeed
    let res = deflate(&options, BlockType::DynamicTree, true, b"", &mut bp, &mut out);
    assert!(res.is_ok());
    // Out size should be > 0 because it writes at least the end-of-block/empty-block sequence
    assert!(!out.is_empty());
}

#[test]
fn test_deflate_uncompressed_basic() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    let mut bp = 0u8;

    let input = b"Hello DEFLATE uncompressed!";
    let res = deflate(&options, BlockType::Uncompressed, true, input, &mut bp, &mut out);
    assert!(res.is_ok());
    assert!(out.len() > input.len());
}

#[test]
fn test_deflate_fixed_tree_basic() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    let mut bp = 0u8;

    let input = b"Hello DEFLATE fixed tree! Hello DEFLATE fixed tree! Hello DEFLATE fixed tree!";
    let res = deflate(&options, BlockType::FixedTree, true, input, &mut bp, &mut out);
    assert!(res.is_ok());
    assert!(!out.is_empty());
}

#[test]
fn test_deflate_dynamic_tree_basic() {
    let options = SafeOptions::default();
    let mut out = Vec::new();
    let mut bp = 0u8;

    let input = b"Hello DEFLATE dynamic tree! Repeat Hello DEFLATE dynamic tree! Again and again!";
    let res = deflate(&options, BlockType::DynamicTree, true, input, &mut bp, &mut out);
    assert!(res.is_ok());
    assert!(!out.is_empty());
}
