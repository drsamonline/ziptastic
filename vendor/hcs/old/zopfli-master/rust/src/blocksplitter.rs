//! Block splitting selection algorithms.

#![forbid(unsafe_code)]

use crate::lz77::{lz77_greedy, Lz77Store, UninitializedLz77Store, ZopfliBlockState};
use crate::squeeze;
use crate::util::{SafeOptions, LARGE_FLOAT};

const DIRECT_SEARCH_LIMIT: usize = 1024;
const MIN_BLOCK_SIZE: usize = 10;

/// Finds minimum of function `f(i)` where `i` is in range `start..end` (excluding end).
/// Uses direct search for small ranges and a recursive sampling strategy for large ranges.
/// Returns a tuple of `(best_index, min_cost)`.
#[allow(clippy::needless_range_loop)]
pub fn find_minimum<F>(mut f: F, start: usize, end: usize) -> (usize, f64)
where
    F: FnMut(usize) -> f64,
{
    if end - start < DIRECT_SEARCH_LIMIT {
        let mut best = LARGE_FLOAT;
        let mut result = start;
        for i in start..end {
            let v = f(i);
            if v < best {
                best = v;
                result = i;
            }
        }
        (result, best)
    } else {
        const NUM: usize = 9;
        let mut start = start;
        let mut end = end;
        let mut lastbest = LARGE_FLOAT;
        let mut pos = start;

        loop {
            if end - start <= NUM {
                break;
            }

            let mut p = [0usize; NUM];
            let mut vp = [0.0f64; NUM];

            for i in 0..NUM {
                p[i] = start + (i + 1) * ((end - start) / (NUM + 1));
                vp[i] = f(p[i]);
            }

            let mut besti = 0;
            let mut best = vp[0];
            for i in 1..NUM {
                if vp[i] < best {
                    best = vp[i];
                    besti = i;
                }
            }

            if best > lastbest {
                break;
            }

            start = if besti == 0 { start } else { p[besti - 1] };
            end = if besti == NUM - 1 { end } else { p[besti + 1] };

            pos = p[besti];
            lastbest = best;
        }
        (pos, lastbest)
    }
}

/// Helper function to insert a value into a sorted vector while keeping it sorted.
fn add_sorted(value: usize, out: &mut Vec<usize>) {
    let pos = out.binary_search(&value).unwrap_or_else(|e| e);
    out.insert(pos, value);
}

/// Finds the largest splittable block.
/// Returns `Option<(lstart, lend)>`.
fn find_largest_splittable_block(
    lz77size: usize,
    done: &[bool],
    splitpoints: &[usize],
) -> Option<(usize, usize)> {
    let mut longest = 0;
    let mut result = None;
    let npoints = splitpoints.len();
    for i in 0..=npoints {
        let start = if i == 0 { 0 } else { splitpoints[i - 1] };
        let end = if i == npoints { lz77size.saturating_sub(1) } else { splitpoints[i] };
        if start < done.len() && !done[start] && end > start && (end - start) > longest {
            longest = end - start;
            result = Some((start, end));
        }
    }
    result
}

/// Prints block split points to stderr.
fn print_block_split_points(lz77: &Lz77Store, lz77splitpoints: &[usize]) {
    let mut splitpoints = Vec::new();
    let mut pos = 0;
    let mut npoints = 0;
    let nlz77points = lz77splitpoints.len();

    if nlz77points > 0 {
        for i in 0..lz77.litlens.len() {
            let length = if lz77.dists[i] == 0 { 1 } else { lz77.litlens[i] as usize };
            if lz77splitpoints[npoints] == i {
                splitpoints.push(pos);
                npoints += 1;
                if npoints == nlz77points {
                    break;
                }
            }
            pos += length;
        }
    }
    debug_assert_eq!(npoints, nlz77points);

    eprint!("block split points: ");
    for &pt in &splitpoints {
        eprint!("{} ", pt);
    }
    eprint!("(hex:");
    for &pt in &splitpoints {
        eprint!(" {:x}", pt);
    }
    eprintln!(")");
}

/// Does block splitting on LZ77 data.
/// The output split points are indices in the LZ77 data.
/// `max_blocks`: set a limit to the amount of blocks. Set to 0 to mean no limit.
pub fn split_lz77(options: &SafeOptions, lz77: &Lz77Store, max_blocks: usize) -> Vec<usize> {
    let mut splitpoints = Vec::new();

    let size = lz77.litlens.len();
    if size < MIN_BLOCK_SIZE {
        return splitpoints;
    }

    let mut done = vec![false; size];
    let mut lstart = 0;
    let mut lend = size;
    let mut numblocks = 1;

    loop {
        if max_blocks > 0 && numblocks >= max_blocks {
            break;
        }

        debug_assert!(lstart < lend);

        let split_cost_fn = |i: usize| {
            squeeze::calculate_block_size_auto_type(&lz77.as_view(), lstart, i)
                + squeeze::calculate_block_size_auto_type(&lz77.as_view(), i, lend)
        };

        let (llpos, splitcost) = find_minimum(split_cost_fn, lstart + 1, lend);

        debug_assert!(llpos > lstart);
        debug_assert!(llpos < lend);

        let origcost = squeeze::calculate_block_size_auto_type(&lz77.as_view(), lstart, lend);

        if splitcost > origcost || llpos == lstart + 1 || llpos == lend {
            done[lstart] = true;
        } else {
            add_sorted(llpos, &mut splitpoints);
            numblocks += 1;
        }

        if let Some((next_start, next_end)) =
            find_largest_splittable_block(size, &done, &splitpoints)
        {
            lstart = next_start;
            lend = next_end;
        } else {
            break;
        }

        if lend - lstart < MIN_BLOCK_SIZE {
            break;
        }
    }

    if options.verbose {
        print_block_split_points(lz77, &splitpoints);
    }

    splitpoints
}

/// Does block splitting on uncompressed data.
/// The output split points are indices in the uncompressed bytes.
///
/// `in_data`: The slice containing the uncompressed data.
/// `max_blocks`: maximum amount of blocks to split into, or 0 for no limit.
pub fn split(
    options: &SafeOptions,
    in_data: &[u8],
    instart: usize,
    inend: usize,
    max_blocks: usize,
) -> Vec<usize> {
    if in_data.is_empty() || instart >= inend {
        return Vec::new();
    }

    let mut store = UninitializedLz77Store::new().initialize(in_data);
    let mut s = ZopfliBlockState { options, lmc: None, blockstart: instart, blockend: inend };

    lz77_greedy(&mut s, in_data, instart, inend, &mut store);

    let lz77splitpoints = split_lz77(options, &store, max_blocks);

    let mut splitpoints = Vec::new();
    let mut pos = instart;
    let mut npoints = 0;
    let nlz77points = lz77splitpoints.len();

    if nlz77points > 0 {
        for i in 0..store.litlens.len() {
            let length = if store.dists[i] == 0 { 1 } else { store.litlens[i] as usize };
            if lz77splitpoints[npoints] == i {
                splitpoints.push(pos);
                npoints += 1;
                if npoints == nlz77points {
                    break;
                }
            }
            pos += length;
        }
    }
    debug_assert_eq!(npoints, nlz77points);

    splitpoints
}

/// Divides the input into equal blocks, does not take LZ77 lengths into account.
pub fn split_simple(
    _in_data: &[u8],
    instart: usize,
    inend: usize,
    block_size: usize,
) -> Vec<usize> {
    if block_size == 0 || _in_data.is_empty() || instart >= inend {
        return Vec::new();
    }
    let mut splitpoints = Vec::new();
    let mut i = instart;
    while i < inend {
        splitpoints.push(i);
        i += block_size;
    }
    splitpoints
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_find_minimum() {
        // Mock a quadratic function f(x) = (x - 5000)^2
        // on a large range start=0, end=10000.
        // It should correctly locate x = 5000.
        let f = |x: usize| {
            let diff = x as f64 - 5000.0;
            diff * diff
        };
        let (best_index, min_cost) = find_minimum(f, 0, 10000);
        assert_eq!(best_index, 5000);
        assert!(min_cost < 1e-9);
    }

    #[test]
    fn test_split_simple() {
        let in_data = vec![0u8; 1000];
        let splitpoints = split_simple(&in_data, 0, 1000, 256);
        assert_eq!(splitpoints, vec![0, 256, 512, 768]);
    }

    #[test]
    fn test_split_lz77_basic() {
        // Construct a small synthetic Lz77Store.
        let data = vec![0u8; 100];
        let mut lz77 = UninitializedLz77Store::new().initialize(&data);
        // Fill litlens with some values to make sure size > 10.
        for i in 0..20 {
            lz77.store_lit_len_dist(1, 0, i);
        }
        let options = SafeOptions::default();
        let splitpoints = split_lz77(&options, &lz77, 2);
        // It shouldn't crash and should return some split points.
        println!("Synthetic split points: {:?}", splitpoints);
    }

    #[test]
    fn test_split_simple_zero_block_size() {
        let in_data = vec![0u8; 1000];
        let splitpoints = split_simple(&in_data, 0, 1000, 0);
        assert!(splitpoints.is_empty());
    }
}
