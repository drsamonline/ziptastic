//! Zlib container compression.

#![forbid(unsafe_code)]

use crate::error::Error;
use crate::util::{BlockType, SafeOptions};

/// Compresses the data according to the zlib specification, RFC 1950.
pub fn zlib_compress(
    options: &SafeOptions,
    in_data: &[u8],
    out: &mut Vec<u8>,
) -> Result<(), Error> {
    let checksum = crate::checksum::adler32(in_data);
    let mut bp = 0u8;

    let cmfflg = 256 * 120 + 3 * 64;
    let fcheck = 31 - (cmfflg % 31);
    let final_header = cmfflg + fcheck;

    out.push((final_header / 256) as u8);
    out.push((final_header % 256) as u8);

    crate::deflate::deflate(
        options,
        BlockType::DynamicTree,
        /* final_block= */ true,
        in_data,
        &mut bp,
        out,
    )?;

    // Append Adler32 checksum in BIG-ENDIAN format
    out.push((checksum >> 24) as u8);
    out.push((checksum >> 16) as u8);
    out.push((checksum >> 8) as u8);
    out.push(checksum as u8);

    if options.verbose {
        let outsize = out.len();
        let compression = if in_data.is_empty() {
            0.0
        } else {
            100.0 * (in_data.len() as f64 - outsize as f64) / in_data.len() as f64
        };
        eprintln!(
            "Original Size: {}, Zlib: {}, Compression: {:.6}% Removed",
            in_data.len(),
            outsize,
            compression
        );
    }

    Ok(())
}
