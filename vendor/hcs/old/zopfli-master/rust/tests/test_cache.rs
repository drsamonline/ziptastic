//! Unit tests for Zopfli's longest match cache storage and retrieval operations.

use zopfli_rs::cache::ZopfliLongestMatchCache;

#[test]
fn test_cache_init() {
    let blocksize = 100;
    let cache = ZopfliLongestMatchCache::new(blocksize);
    assert_eq!(cache.length.len(), blocksize);
    assert_eq!(cache.dist.len(), blocksize);
    assert_eq!(cache.sublen.len(), 8 * 3 * blocksize);

    for &len in &cache.length {
        assert_eq!(len, 1);
    }
    for &d in &cache.dist {
        assert_eq!(d, 0);
    }
    for &s in &cache.sublen {
        assert_eq!(s, 0);
    }
}

#[test]
fn test_sublen_to_cache_and_back() {
    let blocksize = 10;
    let mut cache = ZopfliLongestMatchCache::new(blocksize);

    // Create a mock sublen array
    // Length is 259.
    let mut sublen = vec![0u16; 259];
    sublen[3] = 10;
    sublen[4] = 10;
    sublen[5] = 15;
    sublen[6] = 20;
    sublen[7] = 20;
    sublen[8] = 20;
    sublen[9] = 25;

    let pos = 2;
    let length = 9;

    cache.sublen_to_cache(&sublen, pos, length);

    assert_eq!(cache.max_cached_sublen(pos, length), 9);

    let mut restored_sublen = vec![0u16; 259];
    cache.cache_to_sublen(pos, length, &mut restored_sublen);

    // In original C Zopfli, the cache recreation loop starts populating sublen from index 0
    // even though the minimum match length is 3. Thus, restored_sublen[0..=2] get filled with
    // the distance of the first cached length (which is 10).
    assert_eq!(restored_sublen[0], 10);
    assert_eq!(restored_sublen[1], 10);
    assert_eq!(restored_sublen[2], 10);

    assert_eq!(restored_sublen[3], 10);
    assert_eq!(restored_sublen[4], 10);
    assert_eq!(restored_sublen[5], 15);
    assert_eq!(restored_sublen[6], 20);
    assert_eq!(restored_sublen[7], 20);
    assert_eq!(restored_sublen[8], 20);
    assert_eq!(restored_sublen[9], 25);
}
