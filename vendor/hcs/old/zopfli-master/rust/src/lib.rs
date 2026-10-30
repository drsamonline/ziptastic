//! A pure Rust implementation of the Zopfli compression algorithm.
//!
//! Zopfli is a DEFLATE-compatible compression algorithm that performs exhaustive
//! search to find the optimal block splitting and LZ77 parsing, achieving higher
//! compression ratio at the cost of significantly slower compression speed.

#![warn(missing_docs)]

pub mod blocksplitter;
pub mod cache;
pub mod checksum;
pub mod deflate;
pub mod error;
pub mod ffi;
pub mod gzip_container;
pub mod hash;
pub mod katajainen;
pub mod lz77;
pub mod squeeze;
pub mod symbols;
pub mod tree;
pub mod util;
pub mod zlib_container;

pub use blocksplitter::{split, split_lz77, split_simple};
pub use cache::ZopfliLongestMatchCache;
pub use error::Error;
pub use lz77::{
    find_longest_match, lz77_greedy, Lz77Store, UninitializedLz77Store, ZopfliBlockState,
};
pub use squeeze::{lz77_optimal, lz77_optimal_fixed};
pub use util::{BlockType, Format, SafeOptions};
