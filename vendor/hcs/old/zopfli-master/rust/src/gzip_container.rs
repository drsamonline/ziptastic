//! Gzip container compression.

#![forbid(unsafe_code)]

use crate::error::Error;
use crate::util::{BlockType, SafeOptions};

/// Compresses the data according to the gzip specification, RFC 1952.
pub fn gzip_compress(
    options: &SafeOptions,
    in_data: &[u8],
    out: &mut Vec<u8>,
) -> Result<(), Error> {
    let crcvalue = crate::checksum::crc32(in_data);
    let mut bp = 0u8;

    out.push(31); // ID1
    out.push(139); // ID2
    out.push(8); // CM (Deflate)
    out.push(0); // FLG

    // MTIME
    out.push(0);
    out.push(0);
    out.push(0);
    out.push(0);

    out.push(2); // XFL (2 indicates best compression)
    out.push(3); // OS (3 indicates Unix)

    crate::deflate::deflate(
        options,
        BlockType::DynamicTree,
        /* final_block= */ true,
        in_data,
        &mut bp,
        out,
    )?;

    // Append CRC32 in little-endian format
    out.push(crcvalue as u8);
    out.push((crcvalue >> 8) as u8);
    out.push((crcvalue >> 16) as u8);
    out.push((crcvalue >> 24) as u8);

    // Append ISIZE (input size modulo 2^32) in little-endian format
    let insize = in_data.len() as u32;
    out.push(insize as u8);
    out.push((insize >> 8) as u8);
    out.push((insize >> 16) as u8);
    out.push((insize >> 24) as u8);

    if options.verbose {
        let outsize = out.len();
        let compression = if in_data.is_empty() {
            0.0
        } else {
            100.0 * (in_data.len() as f64 - outsize as f64) / in_data.len() as f64
        };
        eprintln!(
            "Original Size: {}, Gzip: {}, Compression: {:.6}% Removed",
            in_data.len(),
            outsize,
            compression
        );
    }

    Ok(())
}
