//! Unit tests for rolling hash structures and hash warmup algorithms.

use zopfli_rs::hash::ZopfliHash;

#[test]
fn test_hash_init() {
    let window_size = 32768;
    let hash = ZopfliHash::new(window_size);
    assert_eq!(hash.head.len(), 65536);
    assert_eq!(hash.prev.len(), window_size);
    assert_eq!(hash.hashval.len(), window_size);
    assert_eq!(hash.val, 0);

    assert_eq!(hash.head2.len(), 65536);
    assert_eq!(hash.prev2.len(), window_size);
    assert_eq!(hash.hashval2.len(), window_size);
    assert_eq!(hash.val2, 0);

    assert_eq!(hash.same.len(), window_size);
}

#[test]
fn test_hash_warmup_and_update() {
    let window_size = 32768;
    let mut hash = ZopfliHash::new(window_size);

    // Mock some array
    let array = b"abcdefgabc";
    let end = array.len();

    // Warmup at position 0 (should update self.val with 2 bytes)
    hash.warmup(array, 0, end);
    // Let's verify value:
    // c1 = 'a' (97) -> val = (0 << 5 ^ 97) & 32767 = 97
    // c2 = 'b' (98) -> val = (97 << 5 ^ 98) & 32767 = (3104 ^ 98) & 32767 = 3138
    assert_eq!(hash.val, 3138);

    // Now update at pos 0 (updates hash.val with the third byte 'c' (99))
    // hpos = 0 & WINDOW_MASK = 0.
    // pos + MIN_MATCH <= end => 0 + 3 <= 10. True.
    // byte = array[2] = 'c' (99).
    // val = (3138 << 5 ^ 99) & 32767 = (100416 ^ 99) & 32767 = (100387) & 32767 = 2083.
    hash.update(array, 0, end);
    assert_eq!(hash.val, 2083);
    assert_eq!(hash.hashval[0], 2083);
    assert_eq!(hash.head[2083], 0);
    assert_eq!(hash.prev[0], 0); // No previous occurrence of hash 2083 yet.
}
