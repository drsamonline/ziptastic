//! Error types for the Zopfli Rust library.

#![forbid(unsafe_code)]

use thiserror::Error;

/// Error types for the Zopfli Rust library.
#[derive(Debug, Error, PartialEq, Eq, Clone, Copy)]
pub enum Error {
    /// Too few maxbits to represent symbols in Huffman coding.
    #[error("Too few maxbits to represent symbols in Huffman coding")]
    TooFewMaxBits,

    /// Internal count exceeded 9-bit representation in Katajainen algorithm.
    #[error("Internal count exceeded 9-bit representation in Katajainen algorithm")]
    KatajainenCountOverflow,

    /// Invalid frequency or length slices provided (length mismatch).
    #[error("Invalid frequency or length slices provided (length mismatch)")]
    SliceLengthMismatch,

    /// Invalid compression format specified.
    #[error("Invalid compression format specified")]
    InvalidFormat,

    /// Output buffer capacity exceeded or allocation failed.
    #[error("Output buffer capacity exceeded or allocation failed")]
    CapacityExceeded,

    /// Invalid deflate block type.
    #[error("Invalid deflate block type")]
    InvalidBlockType,

    /// Internal compression error.
    #[error("Internal compression error")]
    CompressionFailed,
}
