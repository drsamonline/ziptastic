//! Unit and validation tests for greedy LZ77 match-finding.

use zopfli_rs::lz77::{UninitializedLz77Store, ZopfliBlockState};
use zopfli_rs::util::SafeOptions;

#[test]
fn test_lz77_store_new() {
    let data = b"hello world";
    let uninit = UninitializedLz77Store::new();
    let store = uninit.initialize(data);
    assert_eq!(store.litlens.len(), 0);
    assert_eq!(store.dists.len(), 0);
    assert_eq!(store.pos.len(), 0);
    assert_eq!(store.ll_counts.len(), 0);
    assert_eq!(store.d_counts.len(), 0);
    assert_eq!(store.data, data);
}

#[test]
fn test_lz77_store_append_and_histogram_small() {
    let data = b"abcabcabc";
    let mut store = UninitializedLz77Store::new().initialize(data);

    // Append 'a' at 0
    store.store_lit_len_dist(97, 0, 0);
    // Append 'b' at 1
    store.store_lit_len_dist(98, 0, 1);
    // Append 'c' at 2
    store.store_lit_len_dist(99, 0, 2);
    // Append match "abc" (length 3, dist 3) at 3
    store.store_lit_len_dist(3, 3, 3);

    assert_eq!(store.litlens.len(), 4);
    assert_eq!(store.dists.len(), 4);
    assert_eq!(store.pos.len(), 4);

    let mut ll_counts = [0; 288];
    let mut d_counts = [0; 32];
    store.get_histogram(0, 4, &mut ll_counts, &mut d_counts);

    assert_eq!(ll_counts[97], 1);
    assert_eq!(ll_counts[98], 1);
    assert_eq!(ll_counts[99], 1);
    // get_length_symbol(3) is 257
    assert_eq!(ll_counts[257], 1);
    // get_dist_symbol(3) is 2
    assert_eq!(d_counts[2], 1);
}

#[test]
fn test_lz77_store_large_histogram() {
    let data = vec![0u8; 1000];
    let mut store = UninitializedLz77Store::new().initialize(&data);

    // Store more than 300 items to wrap around both NUM_LL (288) and NUM_D (32) boundaries
    let mut count = 0;
    while count < 350 {
        if count % 2 == 0 {
            store.store_lit_len_dist(65, 0, count);
        } else {
            store.store_lit_len_dist(3, 4, count);
        }
        count += 1;
    }

    assert_eq!(store.litlens.len(), 350);

    let mut ll_counts = [0; 288];
    let mut d_counts = [0; 32];

    // Test small range (direct counting)
    store.get_histogram(10, 20, &mut ll_counts, &mut d_counts);
    assert_eq!(ll_counts[65], 5);
    assert_eq!(ll_counts[257], 5); // symbol 257
    assert_eq!(d_counts[3], 5); // dist symbol for dist 4 is 3

    // Test large range (optimized cumulative subtraction)
    let mut ll_counts_large = [0; 288];
    let mut d_counts_large = [0; 32];
    store.get_histogram(5, 345, &mut ll_counts_large, &mut d_counts_large);

    // There are 340 items in the range 5 to 345.
    // Indices in range: 5 to 345. Even index -> count % 2 == 0 (literal 65).
    // Odd index -> length 3/dist 4 (symbol 257/dist symbol 3).
    // Range contains 170 even indexes and 170 odd indexes.
    assert_eq!(ll_counts_large[65], 170);
    assert_eq!(ll_counts_large[257], 170);
    assert_eq!(d_counts_large[3], 170);
}

#[test]
fn test_block_state() {
    let options = SafeOptions::default();
    let state = ZopfliBlockState { options: &options, lmc: None, blockstart: 10, blockend: 100 };
    assert_eq!(state.blockstart, 10);
    assert_eq!(state.blockend, 100);
}

#[test]
fn test_lz77_store_invalid_literal_symbols_do_not_panic() {
    let litlens = [500u16, 1000u16];
    let dists = [0u16, 0u16];
    let pos = [0u32, 1u32];
    let data = b"";
    let view = zopfli_rs::lz77::Lz77StoreView {
        litlens: &litlens,
        dists: &dists,
        pos: &pos,
        data,
        ll_counts: &[],
        d_counts: &[],
    };

    let mut ll_counts = [0; 288];
    let mut d_counts = [0; 32];
    view.get_histogram(0, 2, &mut ll_counts, &mut d_counts);
    assert_eq!(ll_counts[0], 0);
}

#[test]
fn test_calculate_block_symbol_size_small_invalid_literal_do_not_panic() {
    let litlens = [500u16, 1000u16];
    let dists = [0u16, 0u16];
    let pos = [0u32, 1u32];
    let data = b"";
    let view = zopfli_rs::lz77::Lz77StoreView {
        litlens: &litlens,
        dists: &dists,
        pos: &pos,
        data,
        ll_counts: &[],
        d_counts: &[],
    };

    let ll_lengths = [1u32; 288];
    let d_lengths = [1u32; 32];
    let size = zopfli_rs::squeeze::calculate_block_symbol_size_small(
        &ll_lengths,
        &d_lengths,
        &view,
        0,
        2,
    );
    assert_eq!(size, 1);
}
