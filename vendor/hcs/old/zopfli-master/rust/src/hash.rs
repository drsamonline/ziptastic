//! Zopfli hash structures and algorithms.
//!
//! This module provides `ZopfliHash` which is used for finding longest matches
//! in LZ77 compression by using a sliding window hash.

#![forbid(unsafe_code)]

use crate::util::{MIN_MATCH, WINDOW_MASK};
use std::cmp;

const HASH_SENTINEL: i32 = -1;

/// Represents the rolling and secondary hashes used in the Zopfli LZ77 compressor.
///
/// It maintains the hash value history, previous occurrences, repetitions, and
/// a secondary hash designed to improve split-block matching.
#[derive(Debug, Clone)]
pub struct ZopfliHash {
    /// Hash value to index of its most recent occurrence.
    /// Size is always 65536, initialized to -1.
    pub head: Vec<i32>,
    /// Index to index of previous occurrence of same hash.
    /// Size is `window_size`, initialized to `i as u16` (prev[i] = i).
    pub prev: Vec<u16>,
    /// Index to hash value at this index.
    /// Size is `window_size`, initialized to -1.
    pub hashval: Vec<i32>,
    /// Current rolling hash value.
    pub val: i32,

    /// Secondary hash value to index of its most recent occurrence.
    /// Size is always 65536, initialized to -1.
    pub head2: Vec<i32>,
    /// Index to index of previous occurrence of same secondary hash.
    /// Size is `window_size`, initialized to `i as u16` (prev2[i] = i).
    pub prev2: Vec<u16>,
    /// Index to secondary hash value at this index.
    /// Size is `window_size`, initialized to -1.
    pub hashval2: Vec<i32>,
    /// Current secondary rolling hash value.
    pub val2: i32,

    /// Amount of repetitions of same byte after this index.
    /// Size is `window_size`, initialized to 0.
    pub same: Vec<u16>,
}

impl ZopfliHash {
    /// Creates and initializes a new `ZopfliHash` with the specified window size.
    ///
    /// # Arguments
    ///
    /// * `window_size` - The size of the sliding window (typically `WINDOW_SIZE` = 32768).
    pub fn new(window_size: usize) -> Self {
        Self {
            head: vec![-1; 65536],
            prev: (0..window_size).map(|i| i as u16).collect(),
            hashval: vec![-1; window_size],
            val: 0,
            head2: vec![-1; 65536],
            prev2: (0..window_size).map(|i| i as u16).collect(),
            hashval2: vec![-1; window_size],
            val2: 0,
            same: vec![0; window_size],
        }
    }

    /// Prepopulates the rolling hash with the initial characters before starting updates.
    ///
    /// This corresponds to `ZopfliWarmupHash` in the original C library.
    ///
    /// # Arguments
    ///
    /// * `array` - The input byte array.
    /// * `pos` - The current index position in the array.
    /// * `end` - The logical end boundary in the array.
    #[allow(clippy::collapsible_if)]
    pub fn warmup(&mut self, array: &[u8], pos: usize, end: usize) {
        if pos < end {
            if let Some(&c) = array.get(pos) {
                self.val = (((self.val) << 5) ^ (c as i32)) & 32767;
            }
        }
        if pos + 1 < end {
            if let Some(&c) = array.get(pos + 1) {
                self.val = (((self.val) << 5) ^ (c as i32)) & 32767;
            }
        }
    }

    /// Updates the hash values based on the current position in the array.
    ///
    /// This corresponds to `ZopfliUpdateHash` in the original C library.
    ///
    /// # Arguments
    ///
    /// * `array` - The input byte array.
    /// * `pos` - The current index position in the array.
    /// * `end` - The logical end boundary in the array.
    pub fn update(&mut self, array: &[u8], pos: usize, end: usize) {
        // Assertions that give the compiler static size guarantees so bounds checks are optimized away
        assert!(end <= array.len());
        assert!(pos < end);
        assert_eq!(self.hashval.len(), 32768);
        assert_eq!(self.prev.len(), 32768);
        assert_eq!(self.same.len(), 32768);
        assert_eq!(self.hashval2.len(), 32768);
        assert_eq!(self.prev2.len(), 32768);
        assert_eq!(self.head.len(), 65536);
        assert_eq!(self.head2.len(), 65536);

        let hpos = pos & WINDOW_MASK;

        let byte = if pos + MIN_MATCH <= end { array[pos + MIN_MATCH - 1] } else { 0 };

        self.val = (((self.val) << 5) ^ (byte as i32)) & (WINDOW_MASK as i32);

        self.hashval[hpos] = self.val;

        let val_idx = self.val as usize;
        let val_head = self.head[val_idx];
        if val_head != HASH_SENTINEL {
            let val_head_idx = val_head as usize;
            if self.hashval[val_head_idx] == self.val {
                self.prev[hpos] = val_head as u16;
            } else {
                self.prev[hpos] = hpos as u16;
            }
        } else {
            self.prev[hpos] = hpos as u16;
        }
        self.head[val_idx] = hpos as i32;

        // Update "same"
        let mut amount: usize = 0;
        if pos > 0 {
            let prev_same_idx = (pos - 1) & WINDOW_MASK;
            let prev_same = self.same[prev_same_idx];
            if prev_same > 1 {
                amount = (prev_same as usize) - 1;
            }
        }

        let b = array[pos];
        let limit = cmp::min(end, pos + 1 + 65535);
        if pos + amount + 1 < limit {
            let slice = &array[pos + amount + 1..limit];
            for &x in slice {
                if x == b {
                    amount += 1;
                } else {
                    break;
                }
            }
        }
        self.same[hpos] = amount as u16;

        // Update the secondary hash
        let same_val = self.same[hpos];
        self.val2 =
            (((same_val as i32 - MIN_MATCH as i32) & 255) ^ self.val) & (WINDOW_MASK as i32);

        self.hashval2[hpos] = self.val2;

        let val2_idx = self.val2 as usize;
        let val_head2 = self.head2[val2_idx];
        if val_head2 != HASH_SENTINEL {
            let val_head2_idx = val_head2 as usize;
            if self.hashval2[val_head2_idx] == self.val2 {
                self.prev2[hpos] = val_head2 as u16;
            } else {
                self.prev2[hpos] = hpos as u16;
            }
        } else {
            self.prev2[hpos] = hpos as u16;
        }
        self.head2[val2_idx] = hpos as i32;
    }
}
