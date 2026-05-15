# Bitmap-Indexed Coalescing Range

## The problem

snmalloc's `LargeBuddyRange` only stores power-of-two blocks. A request for 5
chunks must be served from an 8-chunk buddy block, wasting 3 chunks. We want
to store blocks at their actual size and use snmalloc's full size class
sequence at the range level.

## The core idea: search upward, skip a mask

Free blocks are binned by the set of size classes they can serve. To allocate,
search upward through bins — any larger block can be carved down. This almost
works perfectly, but some bins hold blocks whose alignment is too poor to
serve certain smaller, more-aligned sizes. Those bins must be masked out
during the search.

The mechanism: `find_first_set(bitmap & ~skip_mask)`. The skip mask depends
only on the requested size class, not on the block. It's a small constant
that can be precomputed.

## Why skips exist

snmalloc's size classes follow `S = 2^e + m · 2^(e−B)`, where `B` is the
number of intermediate bits. Each size class has a natural alignment
`align(S) = S & ~(S−1)`.

A size class with high alignment needs padding to reach an aligned address
within a block. A block of a *larger* size class with *lower* alignment may
not have room for that padding. Concretely: a block of size 5 at address 1
can serve size 5 (alignment 1) but cannot serve size 4 (alignment 4) — there
aren't enough chunks left after padding to the first 4-aligned address.

Same size block, different address, different capability. This is what creates
the need for separate bins and skip masks.

## The general structure

At each exponent level, the distinct "servable sets" (which size classes a
block can serve) form a structure with some incomparable pairs. Exhaustive
enumeration shows:

| B | Mantissas/exponent | Bins/exponent | Max skip mask bits |
|---|-------------------:|--------------:|-------------------:|
| 1 | 2                  | 2             | 0                  |
| 2 | 4                  | 5             | 1                  |
| 3 | 8                  | 13            | 4                  |
| 4 | 16                 | 34            | 11                 |

Each bin corresponds to a distinct servable set. The bins are ordered so that
upward search is almost always correct — the skip mask handles the exceptions.

For any B, the structure is:
- **Most requests need no skips.** Only size classes with alignment higher
  than expected for their position in the sequence need to mask anything.
- **The skip mask is a small constant** per size class, precomputable at
  compile time.
- **The mechanism is identical** regardless of B:
  `find_first_set(bitmap & ~skip_mask, start_bit)`.

`prototype/skip_analysis.py` verifies this exhaustively for B = 1, 2, 3.

## The bitmap design

Each free block gets one bin based on its size and alignment. Within each
exponent, there are as many bins as there are distinct servable sets (5 for
B=2, 13 for B=3). A flat bitmap tracks which bins are non-empty.

To allocate size class `(e, m)`:

1. Compute the **start bit** — the first bin that could serve this size class.
2. Compute the **skip mask** — bits for bins that can't serve this request.
3. `find_first_set(bitmap & ~skip_mask, start_bit)` → pop a block from that
   bin.

The returned block may not be exactly aligned for the requested size class.
The caller **carves** the aligned region and returns any prefix/suffix
remainders to the free pool.

## Contrast with buddy allocators

A buddy allocator guarantees alignment by construction — a 16-chunk buddy is
always 16-aligned — but wastes space by decomposing everything into
power-of-two pieces.

This design stores blocks at their actual size (no decomposition, no waste)
and handles alignment at allocation time by carving. The skip mask makes
lookup O(1) despite blocks having arbitrary size and alignment.

## Concrete example (B = 2)

At exponent `e = 2`, the size classes are 4, 5, 6, 7. There are 5 bins,
each labeled by the set of size classes it can serve at this exponent:

    Bin 0: serves {4}
    Bin 1: serves {5}
    Bin 2: serves {4, 5}
    Bin 3: serves {4, 5, 6}
    Bin 4: serves {4, 5, 6, 7}

Allocation searches upward from the smallest sufficient bin:

    Request for 7: can use bin 4                       → search bits {4}
    Request for 6: can use bins 3, 4                   → search bits {3, 4}
    Request for 5: can use bins 1, 2, 3, 4             → search bits {1, 2, 3, 4}
    Request for 4: can use bins 0, 2, 3, 4 — skip 1   → search bits {0, 2, 3, 4}

Only the request for size 4 needs to skip a bin: bin 1 holds blocks that can
serve 5 but not 4. The skip mask is just bit 1.

## Concrete example (B = 3)

At exponent `e = 4`, the size classes are 16, 18, 20, 22, 24, 26, 28, 30.
There are 13 bins. The skip analysis shows:

    Request for 16 (align 16): must skip bins for {18}, {20}, {22}, {26}
    Request for 24 (align  8): must skip bin for {26}
    All other requests: no skips needed

The pattern: size 16 has high alignment and must skip 4 bins whose blocks
are large enough but too poorly aligned. Size 24 is a "sub-power-of-two"
(alignment 8) and must skip 1 bin. All odd-coefficient sizes have low
alignment and never need to skip anything.

Same mechanism, wider mask, same `find_first_set(bitmap & ~mask)` operation.
