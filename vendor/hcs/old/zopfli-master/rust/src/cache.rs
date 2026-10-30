//! Longest match cache to speed up Zopfli LZ77 match-finding.

#![forbid(unsafe_code)]

use crate::util::MIN_MATCH;

/// The length of cached longest matches.
pub const ZOPFLI_CACHE_LENGTH: usize = 8;

/// Longest match cache to speed up finding matches.
#[derive(Debug, Clone)]
pub struct ZopfliLongestMatchCache {
    /// Cached match lengths.
    pub length: Vec<u16>,
    /// Cached match distances.
    pub dist: Vec<u16>,
    /// Cached sub-lengths.
    pub sublen: Vec<u8>,
}

impl ZopfliLongestMatchCache {
    /// Initializes a new cache with the specified blocksize.
    pub fn new(blocksize: usize) -> Self {
        Self {
            length: vec![1; blocksize],
            dist: vec![0; blocksize],
            sublen: vec![0; ZOPFLI_CACHE_LENGTH * 3 * blocksize],
        }
    }

    /// Stores sublen array in the cache.
    pub fn sublen_to_cache(&mut self, sublen: &[u16], pos: usize, length: usize) {
        if ZOPFLI_CACHE_LENGTH == 0 {
            return;
        }

        let cache_idx = ZOPFLI_CACHE_LENGTH * pos * 3;
        if cache_idx + ZOPFLI_CACHE_LENGTH * 3 > self.sublen.len() {
            return;
        }

        if length < MIN_MATCH {
            return;
        }

        let cache = &mut self.sublen[cache_idx..cache_idx + ZOPFLI_CACHE_LENGTH * 3];
        let mut j = 0;
        let mut bestlength = 0;

        for i in 3..=length {
            if i == length || (i + 1 < sublen.len() && sublen[i] != sublen[i + 1]) {
                if j < ZOPFLI_CACHE_LENGTH {
                    cache[j * 3] = (i - 3) as u8;
                    let dist_val = if i < sublen.len() { sublen[i] } else { 0 };
                    cache[j * 3 + 1] = (dist_val % 256) as u8;
                    cache[j * 3 + 2] = ((dist_val >> 8) % 256) as u8;
                    bestlength = i;
                    j += 1;
                }
                if j >= ZOPFLI_CACHE_LENGTH {
                    break;
                }
            }
        }

        if j < ZOPFLI_CACHE_LENGTH {
            debug_assert_eq!(bestlength, length);
            cache[(ZOPFLI_CACHE_LENGTH - 1) * 3] = (bestlength - 3) as u8;
        } else {
            debug_assert!(bestlength <= length);
        }
    }

    /// Extracts sublen array from the cache.
    pub fn cache_to_sublen(&self, pos: usize, length: usize, sublen: &mut [u16]) {
        if ZOPFLI_CACHE_LENGTH == 0 {
            return;
        }

        if length < MIN_MATCH {
            return;
        }

        let cache_idx = ZOPFLI_CACHE_LENGTH * pos * 3;
        if cache_idx + ZOPFLI_CACHE_LENGTH * 3 > self.sublen.len() {
            return;
        }

        let maxlength = self.max_cached_sublen(pos, length) as usize;
        let mut prevlength = 0;
        let cache = &self.sublen[cache_idx..cache_idx + ZOPFLI_CACHE_LENGTH * 3];

        for chunk in cache.chunks_exact(3) {
            let len_val = (chunk[0] as usize) + 3;
            let dist = (chunk[1] as u16) + 256 * (chunk[2] as u16);
            for i in prevlength..=len_val {
                if i < sublen.len() {
                    sublen[i] = dist;
                }
            }
            if len_val == maxlength {
                break;
            }
            prevlength = len_val + 1;
        }
    }

    /// Returns the length up to which could be stored in the cache.
    pub fn max_cached_sublen(&self, pos: usize, _length: usize) -> u32 {
        if ZOPFLI_CACHE_LENGTH == 0 {
            return 0;
        }

        let cache_idx = ZOPFLI_CACHE_LENGTH * pos * 3;
        if cache_idx + ZOPFLI_CACHE_LENGTH * 3 > self.sublen.len() {
            return 0;
        }

        let cache = &self.sublen[cache_idx..cache_idx + ZOPFLI_CACHE_LENGTH * 3];
        if cache[1] == 0 && cache[2] == 0 {
            return 0; // No sublen cached.
        }

        (cache[(ZOPFLI_CACHE_LENGTH - 1) * 3] as u32) + 3
    }
}
