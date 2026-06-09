# User Plan

We need to refactor the backend buddy allocator to use the more general concept in IDEA.md, which uses a more general concept of sizeclasses than powers of two to avoid internal fragmentation.

The design will use the Red-Black tree that currently underlies the buddy allocator, but in a different shape: two parallel trees instead of one-per-exponent.

Each block will be part of two structures:

* [Bin] A red-black tree of all blocks held by this Arena, in the same bin, ordered by address.
* [Range] A red-black tree of all blocks held by this Arena, ordered by address.

Note that for block of the minimum size will be handled specially as there is insufficient space to have them particpate in both structures, so they will only particpate in the first.

## Representation

We use 2 bits to represent the mode of this block of memory

00 - Minimum size, only in first red-black tree.  Single pagemap entry for this block is used for the RB-tree
01 - 2 * minimum size (2-aligned), in both red-black trees.  Two pagemap entries for this block are used for the RB-tree
10 - > 2 * minimum size, in both red-black trees.  Three pagemap entries used for this block, first two redblack tree, third stores accurate size of block.
11 - 2 * minimum size (NOT 2-aligned), in both red-black trees.  Two pagemap entries for this block are used for the RB-tree.  Goes into a size-1 bin since it cannot serve aligned size-2 requests.

This means it is possible to find the precise size of a block which can account for additional state that is lost by the binning.

## Maximal consolidation.

When a block, A, is added, we check if the predeccessor, P, and successor, S, blocks are also in the "Range" red-black tree, if they are then we can combine this block A with P and/or S if they were in the RB-tree.  We must also check the "Bin" for the minimum size RB-tree as the minimum sized blocks are not in the "Range" RB tree.

When we combine a block, we remove the blocks from the appropiate "Bin", and then add the combined block to the appropriate "Bin" for the combined block, we remove one block from the "Range" RB-tree, as we can continue to reuse its entry and not need to mutate the RB-tree.

## Allocation

Follows the IDEA.md design, find the smallest bin that can serve the request. Then add back any things that are carved off the block to the free pool.

## Multiple instances

As all the state is looked up from the RB-tree, then we can have multiple instances of the data-structure.  This allows us to have both thread-local and global RB-trees.

## Implementation

### Build Arena

This should use two RB-trees.

It should support adding and removing blocks.

There should be unit tests that check that it is functioning correctly.

There should be a runtime checked invariant that
* the system is maximally consolidated, and
* the system is consistent between the two RB-trees.

### Build LargeArenaRange

This should wrap the Arena using the snmalloc Range approach that is used in the current backend pipelines.

### Update backend to use LargeArenaRange

### Update front-end to request non-power of two size classes for the backend.

### Generalise the large size classes to no longer be just power of two.

### Fix memcpy protection

To find the start of a block will require the pagemap to additionally store an offset.

Currently, the find the start of a block.  Performs an alignment to find the start of the "slab", and uses reciprocal division to find the offset within the slab.  For large allocations, we just used the start of the slab as everything was aligned to a power of two.  We know need to do 

align(ptr, slab_size) - (offset(ptr) * slab_size) 

Here, offset is stored in the pagemap, and allows us to find the start of the block.  We will need to store the offset in every entry of the pagemap for the block as we need to support requests in each offset within the block.

## Extensions

This is not to be done in this initial implementation, but we should consider this for possible future extensions, and should not be ruled out by any design.

### Integrated Decay Range

We can extend the system by effectively having multiple "Range" RB-trees, and then use multiple ranges to track how long a block has been in the backend.  We would always add blocks to the "most recent" Range, and as time passes switch which RB-tree is considered the most recent.  The oldest one, can then be passed back to the OS, or alter whether it is MADV_FREE or MADV_DONTNEED.

---

# Implementation plan: Arena phase

## Scope of this phase

This plan covers **only** the `Arena` data structure and its standalone
unit tests. The following are explicitly deferred to follow-up plans, each of
which will become its own PLAN.md revision:

- `LargeArenaRange` — wrapping `Arena` behind snmalloc's Range API.
- Backend integration — replacing `LargeBuddyRange` in
  `backend/standard_range.h` and `backend/meta_protected_range.h`.
- Front-end requesting non-power-of-two chunk sizes from the backend.
- Generalising the large size classes to no longer be power-of-two only.
- Memcpy protection fix — storing per-chunk `offset` in the pagemap so the
  start of a large allocation can be recovered from any address within it.

The pagemap encoding chosen in this phase **must leave room** for the future
per-entry `offset` field, so the memcpy fix can land later without re-doing
the encoding work. See "Pagemap encoding" below.

## Design notes

### Bins: one Bin tree per IDEA servable-set bin

Per `IDEA.md` and `prototype/skip_analysis.py`, free blocks are classified by
the *servable set* — the set of size classes they can serve, given their size
and alignment.

For `INTERMEDIATE_BITS=B` the bin count per exponent is `B=1: 2, B=2: 5,
B=3: 13, B=4: 34`. The snmalloc default is `B=2`. The number of exponents in
range is `MAX_SIZE_BITS - MIN_CHUNK_BITS + 1`.

Each `Arena` instance owns:

- A flat array of RBTree roots, indexed by bin id (one bin id per
  (exponent, servable-set) pair).
- A flat bitmap (`size_t words[NUM_BITMAP_WORDS]`) tracking which bin
  RBTrees are non-empty. Word width tracks `bits::BITS` so 32-bit
  builds work too.

Allocation for a request of `n_chunks`:

1. `bitmap.find_for_request(n_chunks)` returns the bin id of the
   smallest serving bin (or `SIZE_MAX` if none). Internally this loads
   the per-sc `(start_word, first_mask, second_mask)` triple for
   `n_chunks` and applies one AND per word to locate the first set bit
   in a serving position.
2. Pop a block from that bin's RBTree (smallest address — `remove_min`).
   If the tree empties, `bitmap.clear(bin_id)`.
3. `carve(block, n_chunks)` splits into pre-pad / aligned request of
   exactly `n_chunks` chunks / post-pad. SC rounding stays internal:
   the SC for `n_chunks` only fixes the alignment of the request and
   the minimum block size required; any remainder beyond `n_chunks`
   rolls into `post`. Re-add any non-empty pre/post via `add_block`
   (which classifies the remainder via `bitmap.add(remainder)`).

The bin classification and per-sc search masks (`start_word`,
`first_mask`, `second_mask`) are precomputed at `constexpr` time
directly from the size-class structure (no runtime tables beyond the
bitmap of non-empty bins).

### Range: single tree across all blocks (with min-size exception)

A single RBTree per `Arena` orders all *non-min-size* free blocks by
address. This is the structure used for adjacency lookup during
consolidation:

- `predecessor(A)`: largest Range-tree entry with address less than A.
- `successor(A)` : smallest Range-tree entry with address greater than A.

Min-size blocks are **not** in the Range tree (see "Min-size special case"
below); their adjacency is detected via a `find` in the min-size Bin
RBTree.

### Block size variants and pagemap encoding

A free block occupies one or more `MIN_CHUNK_SIZE` chunks, with a pagemap
entry per chunk. The first pagemap entry of a free block carries a
**variant tag** that tells `Arena` how to interpret the other
entries in the block:

| Variant     | Value | Block size     | Alignment      | Pagemap entries used by Arena                                   |
|-------------|-------|----------------|----------------|------------------------------------------------------------------------|
| `Min`       | 0     | exactly min    | any            | 1 entry — both words store the Bin RBTree node (left/right + colour).  |
| `TwoMin`    | 1     | exactly 2× min | 2-aligned      | 2 entries — first stores Bin node, second stores Range node.            |
| `Large`     | 2     | > 2× min       | any            | 3 entries — first Bin, second Range, third stores precise block size.   |
| `OddTwo`    | 3     | exactly 2× min | **not** 2-aligned | 2 entries — first stores Bin node, second stores Range node.          |

#### Unaligned size-2 blocks (`OddTwo`)

A size-2 block at an odd chunk address (e.g. chunk 3) cannot serve any
size-2 allocation request because all size-2 SCs require 2-chunk
alignment. `bin_index({odd, 2})` correctly places such blocks into a
size-1 bin. However, the `Min` variant can only store one pagemap entry,
and a size-2 block occupies two entries and participates in the range
tree.

The `OddTwo` variant resolves this: it marks a size-2 block that is not
2-aligned. Like `TwoMin`, it uses two pagemap entries and lives in the
range tree. Unlike `TwoMin`, it goes into a size-1 bin (since it can't
serve aligned size-2 requests).

The consolidation code's `contains_min` check probes bin 0 for
single-chunk neighbours. Since `OddTwo` blocks also land in bin 0
(both `Min` and `OddTwo` have a size-1 servable set at exponent 0),
`contains_min` must filter by variant: after finding an address in
bin 0, it checks `get_variant(addr) == Min` to confirm the block is
truly single-chunk. `OddTwo` blocks are found via range-tree neighbour
lookup instead, which correctly returns their size as 2.

Note: only blocks at even chunk addresses can be `TwoMin`. The
`variant_of` function must take both size and chunk address to
distinguish `TwoMin` from `OddTwo`.

**Tree membership is the source of truth for "is this block free?".**
The variant tag is only meaningful for entries `Arena` reaches via
its own RBTrees; nothing outside the data structure probes the tag. The
tag is therefore not a state machine that needs an explicit
"BackendOwned" / "allocated" value: when a block is removed from its
trees, the tag's bits are simply not consulted again until the same
chunk(s) are re-added.

This phase needs to allow **arbitrary chunk counts** for `Large` blocks,
not just exact size-class sizes. Carving will produce non-class
remainders (e.g. for `B=2`, a 9-chunk prefix), and those must round-trip
through `add_block` / `remove_block` without any silent rounding. The
`Large` block's precise chunk count is stored in the third pagemap entry;
`Min` and `TwoMin` sizes are implicit in the variant.

**Bit positions** are an internal detail of the new Rep:

- The variant tag needs 2 bits. They live in the first word of the first
  pagemap entry of the block, in bits above `BACKEND_RESERVED_MASK`
  (bits 0–7) and the existing `RED_BIT` (bit 8) — e.g. bits 9–10. The
  chunk-aligned-address keys leave bits below `MIN_CHUNK_BITS` (=14)
  free, so this is comfortably within budget.
- The Rep's `get`/`set` for the first word must preserve **both**
  `RED_BIT` and the variant-tag bits, generalising `BuddyChunkRep`'s
  current `RED_BIT`-only preservation.

The exact bit positions are documented only inside the new Rep next to
the accessors. `BuddyChunkRep` and `largebuddyrange.h` are not modified
in this phase.

This phase does **not** define any storage for the future per-entry
`offset` field that the memcpy fix will need. The plan only claims that
choosing 2 bits above `RED_BIT` does not preclude a sensible future
offset layout; the concrete offset design is the responsibility of the
memcpy-fix follow-up plan.

### Adjacency lookup

All adjacency lookups are performed via RB-tree finds in this
`Arena`'s own trees. **No pagemap probing.** The pagemap is shared
across `Arena` instances (e.g. thread-local + global), and reading
entries owned by another instance would be unsafe under concurrent
modification. By restricting reads to RB-tree traversals — which only
follow pointers we wrote, into entries we own — adjacency detection is
race-free without any synchronisation at this layer.

For an incoming block `A` of size `S` at address `addr_A`:

- `(P_range, S_range) := Range.neighbours(addr_A)` — one walk yields both
  non-min neighbours.
  - If `P_range.addr + P_range.size == addr_A`, the non-min left
    neighbour is `P_range`; merge.
  - If `S_range.addr == addr_A + size_A`, the non-min right neighbour is
    `S_range`; merge.
- If no non-min left neighbour was found and `A` is min-eligible at its
  boundary: `MinSizeBin.find(addr_A - MIN_CHUNK_SIZE)`; if present,
  merge.
- If no non-min right neighbour was found: `MinSizeBin.find(addr_A +
  size_A)`; if present, merge.

`MinSizeBin` is the single Bin RBTree that holds all blocks whose
servable set is `{1 chunk}` (bin 0). This includes both `Min` (size-1)
and `OddTwo` (unaligned size-2) blocks. The `contains_min` helper
performs a `find` in bin 0, then checks `get_variant(addr) == Min` to
confirm the block is truly single-chunk — `OddTwo` entries are skipped
so they are handled by the range-tree neighbour lookup instead.

Min-size adjacency therefore costs at most one Bin-tree `find` per side
per `add_block`. The Range-tree `neighbours(addr_A)` query yields both
non-min neighbours in a single `O(log n)` walk; no additional pagemap
touches are introduced.

### Consolidation: reusing tree entries when possible

The user plan calls out that when consolidating `A` with predecessor `P`,
the Range tree node belonging to `P` can be reused for the consolidated
block without any RB-tree mutation: the combined block has the same
starting address as `P`, so its Range tree key is unchanged. Only the Bin
tree is mutated (remove `P` from its bin, insert combined into its new
bin).

This optimisation applies **only when `P` is non-min**, i.e. when `P` has a
Range tree entry to reuse. When `P` is min-size, `P` has no Range entry,
and the merged block (which is non-min) must be inserted into the Range
tree normally. The same applies to the `P+S` case: reuse `P`'s Range entry
only if `P` is non-min; otherwise insert the merged block into the Range
tree, then remove `S`'s entry.

When consolidating `A` with successor `S` (and no `P`), the combined block
starts at `addr_A`, not `S.addr`. Two strategies:

- **Simple (initial)**: remove `S` from the Range tree, insert combined at
  `addr_A`. Two RB-tree operations.
- **Optimised (deferred)**: walk to `S`'s parent via the path returned by
  `find`, redirect that parent's child pointer to the new node at `addr_A`,
  and copy `S`'s left/right/colour into the new node's pagemap entry. Zero
  rotations.

This plan implements the simple strategy first and gates the optimised
strategy behind a follow-up step with an A/B test.

After a merge, the combined block may itself become adjacent to a further
block (in principle yes — but if the system was maximally consolidated
before `A` was added, then `P` and `S` were not adjacent to anything else,
so a single merge step suffices). The invariant check verifies this.

### Min-size special case

A min-size free block has only one pagemap entry, which fits one RBTree
node. The Bin tree for min-size blocks uses that entry. The Range tree
excludes min-size blocks. Adjacency for min-size neighbours is found via
`MinSizeBin.find(addr)`, never by reading the pagemap directly.

### Write ordering within add/remove

Because adjacency lookups are RB-tree-only, a block is "visible to
adjacency" exactly when it is reachable from one of this `Arena`'s
RBTree roots. The ordering rules collapse to two:

- **add_block**: write the variant tag and any auxiliary data (precise
  size for `Large`) into the block's pagemap entries *before* the final
  RB-tree insertion that makes the block reachable. Anyone who finds the
  new block via a tree walk must see a fully-initialised block.
- **remove_block / consolidation**: remove the block(s) from the
  RB-tree(s) *before* reusing or overwriting their pagemap entries.
  After unlinking, the block is no longer reachable, and the entries are
  free for reuse by the next operation.

No transient "BackendOwned" marker is needed: a chunk's free-ness is
synonymous with its membership in some RBTree owned by this
`Arena`.

### Invariants (debug-only, runtime-checked)

The `Arena::invariant()` method checks:

1. **Maximally consolidated**: walking the Range tree in order, no two
   adjacent entries have `prev.addr + prev.size == curr.addr`; no min-size
   block in `MinSizeBin` is adjacent (at `addr ± MIN_CHUNK_SIZE`) to
   anything else in either tree.
2. **Cross-tree consistency**: every non-min block in the Bin trees is in
   the Range tree; every Range tree entry is in exactly one Bin tree;
   sizes agree between the two views.
3. **Bin classification correctness**: each block is in the bin determined
   by its `(addr, size)` servable set; arbitrary chunk counts (not just
   exact size-class sizes) are classified correctly.
4. **Bitmap consistency**: the non-empty-bins bitmap is set iff the
   corresponding RBTree is non-empty.
5. **Variant-tag consistency**: every block reachable from a tree root
   has a variant tag (`Min` / `TwoMin` / `Large`) matching its actual
   chunk count. (No "BackendOwned" tag is needed because tree membership
   is the source of truth for freeness.)

### Backend chunk size classes

The new bins are indexed in **chunk units** (1 chunk = `MIN_CHUNK_SIZE`
bytes), not bytes, and not the front-end `sizeclass_t` whose large variant
is currently power-of-two only. `arenabins.h` defines the
chunk-unit size-class scheme using the snmalloc size-class formula
`S = 2^e + m · 2^(e − B)` applied at **chunk-count exponents starting
from zero**. Low-exponent special cases (chunk counts 1, 2, 3, …) follow
the same pattern as `bits::from_exp_mant` in
`src/snmalloc/ds_core/sizeclassstatic.h`: at small exponents the mantissa
space is degenerate, handled by enumeration.

The public API of `ArenaBins<B>` — the integration contract
`Arena` builds on — is intentionally narrow:

- `struct range_t { size_t base; size_t size; }` — a chunk-count range
  used to describe free blocks and carved sub-ranges.
- `struct carve_t { range_t pre, req, post; }` — output of a carving
  operation; either of `pre`/`post` may have `size == 0` (absent).
- `static carve_t carve(range_t block, size_t n_chunks)` — given a
  free block and an allocation request, split into pre-pad / aligned
  request of exactly `n_chunks` chunks / post-pad. SC rounding stays
  inside the carve: the SC for `n_chunks` only fixes alignment and the
  servability precondition. Pure function; does not touch the bitmap.
- `max_supported_chunks() -> size_t` — upper bound on legal `n_chunks`;
  used for assertions.
- nested `Bitmap` — the routing layer; see below.

The `Bitmap` is a per-arena non-empty-bins bitmap that owns the
classification of `(base, size)` pairs to bin ids. Its public surface is
exactly three operations:

- `add(range_t block) -> size_t` — classify `block` into a bin, ensure
  the bit for that bin is set, return the `bin_id` so the caller
  inserts the block into `bin_trees[bin_id]`. **Idempotent**: callable
  on a block already represented in the trees; setting an already-set
  bit is a no-op. This is the only public way to learn a bin id for a
  given `(base, size)` block, including during consolidation lookups
  for neighbours that are already present.
- `find_for_request(size_t n_chunks) -> size_t` — locate the first set
  bin satisfying a request for `n_chunks`. Returns `SIZE_MAX` if no bin
  in this arena fits.
- `clear(size_t bin_id)` — caller has popped the last element from
  `bin_trees[bin_id]`; the bitmap bit is cleared.

There are deliberately no general bitmap operations (`set`/`has`/
`empty`/etc.) on the public surface — the bitmap is not a generic data
structure but a routing index whose only meaningful operations are the
three above. The `bitmap_info_t` / `carve_info_t` rodata layouts, the
`bin_index` classifier, and `bitmap_info_for_request` /
`carve_info_for_request` are private (the bitmap and `carve` consume
them internally).

The size-class encoding details — the `bitmap_info_t` / `carve_info_t`
rodata records, the bin-scheme constants (`B`, `MANTISSAS_PER_EXP`,
`BINS_PER_EXP`, `MAX_SC`), `bitmap_info_for_request` /
`carve_info_for_request`, `bin_index`, and the constexpr per-sc
accessors — are private implementation details. They are reachable
only via the friend struct `ArenaBinsTestAccess<B>` (defined in
the test translation unit, see Phase 1) so unit tests can exercise
them directly; code outside this header does not depend on
them.

**Free blocks may have arbitrary chunk counts**, including non-class
sizes that arise from carving (e.g. a 9-chunk prefix at `B=2`). The
private `bin_index` operates on arbitrary `(address, size)` pairs and
classifies into a bin by the block's servable set; the public `Bitmap`
exposes this only through `add(range_t)`, which returns the bin id.
Exact size classes appear only on the request side; free blocks store
their precise chunk count where needed (`Large` variant).

### Exponent / bin-count bounds

`Arena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>` takes **byte-size
exponent** bounds (mirroring `Buddy<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>`).
`MIN_SIZE_BITS` is the log2 of the unit of allocation; everything inside
the arena is in multiples of `1 << MIN_SIZE_BITS`. The upper bound is
**exclusive**. The total number of bins is
`(MAX_SIZE_BITS - MIN_SIZE_BITS) * BINS_PER_EXP` plus the
degenerate-low-exponent bins. Static assertions encode the exclusive
semantics; tests exercise minimum, just-below-max, and exact-max sizes
(the last triggers overflow back to the parent, mirroring `Buddy`).

The chunk-unit bin scheme is independent of `sizeclass_t::as_large()`
for now. The "Generalise the large size classes" follow-up plan will
reconcile the front-end large size classes with this scheme.

### Multiple instances

All state lives in pagemap-backed nodes and in per-instance roots/bitmaps;
no global state. Multiple `Arena` instances can coexist (thread-local
and global) for the future Range wrapper.

## Phases

Each phase produces a test gate that must pass before the next phase begins.
A phase that touches the tree itself must also keep all existing tests
(including `redblack.cc`) green.

### Reviewer protocol (applies to every phase below)

Each phase ends with **two** gates, both of which must clear before
the next phase starts:

- **Test gate** — the listed tests pass on a Debug build (per
  `.github/skills/building_and_testing.md`).
- **Review gate** — spawn a fresh-context `code-review` subagent on
  the diff added in that phase. The reviewer prompt includes:
  1. The plan section for the current phase (treat as spec).
  2. The diff produced by the phase (compared to the previous
     phase's tip).
  3. A reminder that this phase's scope is *only* what the plan
     section describes; cross-phase concerns are out of scope.
  4. A pointer to `claude.md` for codebase conventions (no raw
     compiler attributes, no C++ STL, `SNMALLOC_*`
     macros, etc.).

  Address findings, re-spawn a fresh-context reviewer, loop until
  a reviewer reports no issues. Disputes with reviewer findings
  escalate to the user, not resolved unilaterally.

Phases 0 and 6 are exempted from the review gate: Phase 0 adds no
code; Phase 6 is test-only over already-reviewed code.
Phase 7 is the final mandatory review per `claude.md`.

### Phase 0: Baseline

Per `claude.md` "Baseline the checkout before starting work": run a clean
Debug build and the full test suite via the testing subagent protocol in
`.github/skills/building_and_testing.md`. Record the results. If the baseline is
broken, stop and report — do not start implementation on a broken base.

**Test gate**: full ctest run completes; record pass/fail status of each
test for later comparison.

### Phase 1: ArenaBins — bin scheme, per-sc tables, and bitmap

Add `src/snmalloc/backend_helpers/arenabins.h` defining
`ArenaBins<INTERMEDIATE_BITS>`: the chunk-unit size-class
scheme, two per-sc rodata tables, the free-block classifier, and the
nested non-empty-bins bitmap that the allocation fast path scans.

#### Public surface — the integration contract

- `struct range_t { size_t base; size_t size; }` — chunk-count range.
- `struct carve_t { range_t pre; range_t req; range_t post; }` — output
  of a carving operation; `pre` and/or `post` may have `size == 0`.
- `static SNMALLOC_FAST_PATH carve_t carve(range_t block, size_t n_chunks)`
  — split a free `block` into pre-pad, aligned request of exactly
  `n_chunks` chunks, and post-pad. SC rounding stays internal: the SC
  for `n_chunks` only fixes alignment and the servability precondition;
  any rounding remainder absorbs into `post`. Pure. **Preconditions**
  (asserted):
  `n_chunks >= 1 && n_chunks <= max_supported_chunks()`,
  `block.size > 0`, and `block` is servable for `n_chunks` (the caller
  has already used `Bitmap::find_for_request`).
- `static constexpr size_t max_supported_chunks()` — upper bound on
  legal `n_chunks`; used for assertions.
- nested `class Bitmap` — three methods, all that other code
  calls into:
  - `size_t add(range_t block)` — classify `block`, ensure the bit
    for the resulting bin is set, return the bin id so the caller can
    insert `block` into `bin_trees[bin_id]`. **Idempotent**: also the
    way to obtain the bin id of an existing neighbour during
    consolidation. **Precondition**: `block.size >= 1 &&
    block.size <= max_supported_chunks()`.
  - `size_t find_for_request(size_t n_chunks) const` — smallest set
    bin servable for `n_chunks`; `SIZE_MAX` if none.
  - `void clear(size_t bin_id)` — caller has popped the last element
    from `bin_trees[bin_id]`; clears the bit.
  - `static constexpr size_t TOTAL_BINS` — strict upper bound on bin
    ids; exposed so callers can size `bin_trees`.

No general bitmap operations and no size-class handles are exposed.
All other members are private; the unit test reaches them through a
friend struct, defined in the test translation unit (see "Test surface"
below).

#### Bin scheme

Following `prototype/skip_analysis.py`:

- `B = INTERMEDIATE_BITS` (mantissa bits, currently restricted to
  `{1, 2, 3}`).
- `MANTISSAS_PER_EXP = 1 << B` (4 / 8 mantissa positions; 2 for B=1).
- `BINS_PER_EXP` = 2 / 5 / 13 for `B` = 1 / 2 / 3 — the count of
  distinct *servable subsets* of mantissas at each exponent. Each bin
  is a single bit in the bitmap and a single RB-tree at the
  `Arena` layer; bins are not size classes (multiple size
  classes share a bin) and not exponents (each exponent has multiple
  bins).
- `MAX_SC = ((bits::BITS - B) << B) + ((1 << B) - 1)` — one past the
  largest raw id that `bits::to_exp_mant_const<B, 0>` produces whose
  decoded size fits in `size_t`. The architectural max raw id decodes
  to `2^bits::BITS`, which overflows; the tables stop one entry short
  to keep `from_exp_mant<B,0>(MAX_SC - 1)` valid. Sizes for `B` = 1 /
  2 / 3 on 64-bit: 127 / 251 / 495.
- `max_supported_chunks() = bits::from_exp_mant<B, 0>(MAX_SC - 1)` —
  enormous in practice (far beyond any real arena).

#### Per-sc rodata tables

Two power-of-two-sized structs, each indexed by raw sc id with a
single shift+add:

```cpp
struct alignas(4 * sizeof(size_t)) bitmap_info_t {
  size_t start_word, first_mask, second_mask;
};
struct carve_info_t { size_t size_chunks, align_chunks; };
```

`alignas(4 * sizeof(size_t))` on `bitmap_info_t` rounds its `sizeof`
up to a power of two (C++ requires `sizeof(T)` to be a multiple of
`alignof(T)`), so the table indexes with a shift+add without needing
a named padding member.

Split into two tables — rather than one combined record — because the
two consumers run at different phases of allocation/free:

- `bitmap_info_t` is read by `Bitmap::find_for_request` (bin-selection
  on allocate).
- `carve_info_t` is read by `carve` (post-pop split on allocate) and by
  `bin_index`'s cascade-fit predicate (free-side classification).

The fields of `bitmap_info_t` are **pre-shifted into the bitmap's word
layout** so the search is two ANDs:

- `start_word`: the bitmap word containing the SC's lowest serving
  bin.
- `first_mask`: serve mask pre-shifted into `start_word`. Bit `i` set
  iff `words_[start_word]` bit `i` serves this SC.
- `second_mask`: serve mask carried into `start_word + 1`. When
  `start_bit` is word-aligned (`shift == 0`) there is no within-exp
  carry and every bit in that word is higher-exponent, so
  `second_mask = ~size_t(0)`.

`static_assert` pins both struct sizes (4 words and 2 words) so the
table index lowers to a shift+add.

#### Tables and classifier — populated at constexpr build time

A private `BinTable` struct holds (all `ModArray<...>`):

- `bitmap_info[MAX_SC]`, `carve_info[MAX_SC]` — the per-sc tables
  above.
- `exp_first_sc[bits::BITS + 1]` — first raw sc id at each
  ArenaBins exponent (sentinel at index `bits::BITS` equals
  `MAX_SC`). NOTE: this is not uniform stride — at the bottom of the
  encoding the low regime squashes multiple ArenaBins exponents
  into encoded-exponent 0.
- `exp_bin_base[bits::BITS + 1]` — `e * BINS_PER_EXP`, precomputed so
  `bin_index` does no runtime multiply.
- `cascade_steps[MANTISSAS_PER_EXP][MAX_CASCADE_STEPS]` — per-`m_top`
  decision lists for `bin_offset_at`.

A `static constexpr BinTable table_{}` member of `ArenaBins<B>`
holds the populated instance. Tables sit in `.rodata`; no static
initialiser runs at program start. Combined size at B=3 is on the
order of tens of KB (estimate: 16 B/sc × 495 + 32 B/sc × 495 + small
cascade table ≈ 24 KB).

The constructor populates `bitmap_info[sc]` from the canonical
`bin_subsets` table (single source of truth, matches
`prototype/skip_analysis.py`):

- `start_bin_offset_for_m(m)`: first within-exp bin offset whose
  subset contains mantissa `m`.
- `serve_mask_for_m(m)`: bitmask, relative to `start_bin_offset_for_m`,
  of bins that serve `m`. Built **positively** (bit set = "serves")
  rather than as a "skip" mask: the hot path AND's this directly
  against the bitmap word, no NOT.
- `start_bit = exp_bin_base[e] + start_bin_offset_for_m(m)`, then
  `start_word = start_bit / bits::BITS`,
  `first_mask = serve_mask << (start_bit & (bits::BITS - 1))`,
  `second_mask = (shift == 0) ? ~size_t(0) : ((mask >> (bits::BITS -
  shift)) | (~size_t(0) << shift))`.

For `cascade_steps`: for each `m_top`, the bins whose subset has
`m_top` as max element must form a strict containment chain when
sorted descending by popcount. This invariant is **checked at
constexpr build time** (`throw "..."` in the constexpr ctor surfaces
the violation as a compile error). Given the invariant, each
non-default candidate's discriminator is a single mantissa probe; the
list ends with a `NO_TEST` default.

Two free-side primitives, both private (used internally by `add` and
by `carve`):

- `bin_index(range_t block) -> size_t`: returns the bin id of `block`,
  operating on arbitrary chunk counts (not just exact SCs). Walks
  `m_top` from `MANTISSAS_PER_EXP - 1` down at the natural exponent
  `e = prev_pow2_bits(block.size)`. If alignment padding eats every
  fit at `e`, drops to `e - 1`; one drop is always sufficient (the
  smallest SC at `e - 1` has size and alignment `2^(e-1)`, so worst-
  case `size + pad < 2^e <= block.size`).
- `bitmap_info_for_request(n_chunks) -> const bitmap_info_t&`,
  `carve_info_for_request(n_chunks) -> const carve_info_t&`: single
  table read each. Both call `bits::to_exp_mant<B, 0>(n_chunks)` (the
  runtime CLZ intrinsic variant) so the encode is fast and is expected
  to be CSE'd when both calls appear in the same fast path.

#### Runtime CLZ on the fast path

Fast-path calls use the runtime intrinsic, not the
constexpr software fallback:

- `src/snmalloc/ds_core/bits.h` provides
  `template<size_t MANTISSA_BITS, size_t LOW_BITS = 0> inline
  SNMALLOC_FAST_PATH size_t to_exp_mant(size_t value)` — body
  identical to `to_exp_mant_const` but using `bits::clz` instead of
  `clz_const`. `static_assert(MANTISSA_BITS + LOW_BITS > 0, ...)` —
  the runtime variant relies on `LEADING_BIT != 0` to guarantee
  `clz`'s non-zero precondition.
- Header uses `bits::to_exp_mant<B, 0>(n_chunks)` on the
  `bin_index` / `find_for_request` / `carve` paths;
  `bits::to_exp_mant_const<B, 0>(...)` is used **only** at table
  construction time inside the constexpr `BinTable` constructor (and
  in test-only static_asserts — see "Test surface").

This is the existing snmalloc convention for paired runtime /
compile-time helpers (`clz` / `clz_const`, `next_pow2` /
`next_pow2_const`).

#### Nested `Bitmap`

```cpp
class Bitmap
{
  friend struct ArenaBinsTestAccess<INTERMEDIATE_BITS>;

public:
  static constexpr size_t TOTAL_BINS = BINS_PER_EXP * bits::BITS;

  Bitmap() : words_{} {}
  SNMALLOC_FAST_PATH size_t add(range_t block);
  SNMALLOC_FAST_PATH void   clear(size_t bin_id);
  SNMALLOC_FAST_PATH size_t find_for_request(size_t n_chunks) const;

private:
  static constexpr size_t NUM_BITMAP_WORDS =
    (TOTAL_BINS + bits::BITS - 1) / bits::BITS;

  size_t words_[NUM_BITMAP_WORDS];
};
```

- `TOTAL_BINS = BINS_PER_EXP * bits::BITS` is the strict upper bound
  on `bin_index` output: `bin_index` returns `e * BINS_PER_EXP +
  offset` with `e <= bits::BITS - 1` and `offset < BINS_PER_EXP`, so
  the maximum is `BINS_PER_EXP * bits::BITS - 1 < TOTAL_BINS`. Values:
  128 / 320 / 832 for `B` = 1 / 2 / 3 on 64-bit.
- `NUM_BITMAP_WORDS == BINS_PER_EXP` exactly (2 / 5 / 13 words); on
  32-bit each word is 4 B instead of 8 B, halving storage.
- `words_` is zero-initialised. Word width tracks `bits::BITS` so the
  AND with the precomputed masks has no width mismatch; `bits::ctz`
  on a `size_t` produces the bit index.

Friend declarations: `ArenaBins<B>` and its nested `Bitmap`
each carry their own `friend struct ArenaBinsTestAccess<...>;`
(C++ friendship does not transit to nested classes).

Static asserts on bitmap layout:

- `TOTAL_BINS == BINS_PER_EXP * bits::BITS`.
- `NUM_BITMAP_WORDS == BINS_PER_EXP`.
- `TOTAL_BINS < SIZE_MAX` — so the `SIZE_MAX` sentinel cannot collide
  with a valid bin id.
- `BINS_PER_EXP <= bits::BITS` — `find_for_request` assumes the
  within-exp range fits in a single word so the search straddles at
  most one word boundary. Holds on 32-bit (W=32) and 64-bit (W=64)
  for the current B values. If a future B pushes this above
  `bits::BITS`, the two-word body must be generalised.

`find_for_request` body:

```cpp
SNMALLOC_FAST_PATH size_t find_for_request(size_t n_chunks) const
{
  const bitmap_info_t& info = bitmap_info_for_request(n_chunks);
  SNMALLOC_ASSERT(info.start_word < NUM_BITMAP_WORDS);

  // First word: start bin + any within-exp neighbours in same word.
  size_t word = info.start_word;
  size_t bits = words_[word] & info.first_mask;
  if (bits != 0) return word * bits::BITS + bits::ctz(bits);
  if (++word == NUM_BITMAP_WORDS) return SIZE_MAX;

  // Second word: within-exp carry plus any higher-exp bits.
  bits = words_[word] & info.second_mask;
  if (bits != 0) return word * bits::BITS + bits::ctz(bits);

  // Remaining words: purely higher-exponent, any bit serves.
  while (++word < NUM_BITMAP_WORDS)
    if (words_[word] != 0) return word * bits::BITS + bits::ctz(words_[word]);
  return SIZE_MAX;
}
```

The two ANDs are the entire bin-selection cost; no shifts, no
`shift == 0` branches at runtime (folded in at construction).

#### Test surface

`ArenaBinsTestAccess<INTERMEDIATE_BITS>` is **forward-declared**
in `arenabins.h` (so the friend declarations can refer to it)
and **defined in the test translation unit**
`src/test/func/backend_arena_bins/backend_arena_bins.cc` (inside
`namespace snmalloc`). The header therefore carries no
test-only members.

What the test access struct exposes (all delegating to private
internals through the friend grant):

- Re-exports of the public types and methods, for convenience.
- The bin-scheme constants `B`, `MANTISSAS_PER_EXP`, `BINS_PER_EXP`,
  `MAX_SC`.
- `using chunk_sc_t = size_t;` — raw sc id as plain `size_t`; the
  header does NOT define a `chunk_sc_t` handle type.
- `request(n) -> size_t` — `bits::to_exp_mant<B, 0>(n)` (runtime).
- `size_chunks(sc) -> size_t`, `align_chunks(sc) -> size_t` — direct
  reads of `Bins::table_.carve_info[sc]`.
- `bitmap_info(sc) -> const bitmap_info_t&`, `carve_info(sc) -> const
  carve_info_t&` — direct table reads.
- `bitmap_info_for_request_const(n)`,
  `carve_info_for_request_const(n)` — constexpr variants that use
  `bits::to_exp_mant_const<B, 0>(n)`; used only inside
  `static_assert`s in the test file.
- `bin_index(block) -> size_t`, `bitmap_info_for_request(n)`,
  `carve_info_for_request(n)`, `carve(block, n)`,
  `max_supported_chunks()` — passthroughs to the private members.
- The canonical `bin_subsets` table.
- Raw-word access on `Bitmap`: `raw_set(b, bin_id)`, `raw_has(b,
  bin_id)`, `raw_empty(b)`, `raw_word(b, i)` — for exhaustive
  single-bit and "no other bit changed" tests.

#### Test gate

New test `src/test/func/backend_arena_bins/backend_arena_bins.cc`
(auto-discovered via `subdirlist` of `src/test/func/`; registered in
`TESTLIB_ONLY_TESTS`). For each `B ∈ {1, 2, 3}`:

- Compile-time properties via `static_assert` (`BINS_PER_EXP`,
  `MAX_SC`, sample sizes/alignments through the `_const` variants).
- Runtime/constexpr CLZ agreement:
  `to_exp_mant<B,0>(n) == to_exp_mant_const<B,0>(n)` over a
  representative range of `n` (1, every power of two and ±1, near
  `max_supported_chunks()`, several thousand random values).
- `from_exp_mant` round-trip:
  `from_exp_mant<B,0>(to_exp_mant<B,0>(n)) >= n` and minimality
  (no smaller raw id satisfies the bound).
- Bin-scheme primitives: `size_chunks(sc) >= s` for
  `sc = request(s)`; idempotence `request(size_chunks(sc)) == sc`;
  monotonicity of `request`; `align_chunks(sc)` is a power of two,
  divides `size_chunks(sc)`, and is the largest such.
- `bin_index`: enumerate `(addr_chunks, n_chunks)` over a small grid
  (including arbitrary non-class sizes) and check that `bin_index`
  matches a brute-force servable-set computation expressed via the
  canonical `bin_subsets` table.
- `Bitmap` raw smoke (via friend-struct raw-word access): set / clear
  round-trips on individual bin ids; multi-bit states; empty check.
- `find_for_request` on empty bitmap returns `SIZE_MAX` for all
  representative request sizes (including `max_supported_chunks()`).
- **Exhaustive single-bit**: for each `bin_id < TOTAL_BINS`, set
  exactly that bit (raw access) and verify
  `find_for_request(n_chunks)` matches a reference brute-force
  scanner over a representative set of request sizes. The reference
  predicate "bin b serves request n" is expressed via a
  `serves<B>(bin, n)` helper that consults `bin_subsets` directly —
  the canonical source from which the precomputed
  `start_word`/`first_mask`/`second_mask` are themselves derived, so
  any divergence in the derivation chain is caught.
- **Multi-bit randomised**: thousands of random arena states
  (uniformly random subset of bin ids) cross-checked against the
  reference scanner over representative requests.
- **Word-boundary targeted cases**: classify the table entries
  `bitmap_info_for_request_const(...)` produces by `start_bit =
  start_word * bits::BITS + bits::ctz(first_mask)` (the start bin is
  always the lowest set bit of `first_mask` by construction) into
  aligned / fits-in-one-word / boundary-straddling. For each
  category, exercise: (i) a single set bit in the first word's
  considered region; (ii) first word empty + set bit in the second
  word's within-exp carry; (iii) first word empty + set bit in the
  second word's higher-exp region; (iv) set bits only in word 3 or
  beyond.
- **`add` / `find_for_request` single-block integration**: for
  representative `(base, size)` blocks, `bin_id = bm.add({base,
  size})`, then `find_for_request(n_chunks) == bin_id` iff
  `can_serve(base, size, n_chunks)` (the brute-force predicate using
  per-class `size_chunks` / `align_chunks` from the friend struct).
- **`add` / `find_for_request` multi-block integration**: insert
  several blocks; for each request, the expected result is the
  smallest bin id among the added blocks that can serve it (or
  `SIZE_MAX`). Pins the "first serving bin" contract.
- **`add` idempotence**: calling `add(block)` twice returns the same
  bin id both times and leaves the bitmap unchanged (verified via raw
  word access before and after the second call).
- `carve`: for a representative grid of `(block, n_chunks)`, the
  output triple has `pre.base = block.base`,
  `pre.base + pre.size = req.base`, `req.size = size_chunks(sc)`,
  `req.base` is `align_chunks(sc)`-aligned,
  `req.base + req.size = post.base`, and
  `post.base + post.size = block.base + block.size`.

`MAX_SC`-related `static_assert`s use `snmalloc::bits::BITS` (not
hard-coded 64) so they hold on both 32-bit and 64-bit builds.

#### Review gate

Spec slice = the Phase 1 section above. Reviewer checks:

- Tables match the canonical `bin_subsets` (single source of truth);
  `prototype/skip_analysis.py` reproduces the same numbering.
- The in-tree header carries no test-only surface (no `chunk_sc_t`
  handle class, no `request`, no `_const` variants, no test-only
  per-sc accessors — those live only in
  `ArenaBinsTestAccess` in the test cc).
- Fast path uses runtime `bits::to_exp_mant` / `bits::clz` (not the
  `_const` variants); the `_const` variants are reachable only from
  the constexpr `BinTable` constructor and the test's
  `static_assert`s.
- `Bitmap::find_for_request` matches the reference scanner; word-
  boundary straddle is correctly handled.
- `SIZE_MAX` sentinel is unambiguous (`TOTAL_BINS << SIZE_MAX`).
- Tables sit in `.rodata` (no program-start initialiser).
- Comments earn their length: cut anything that justifies layout,
  restates code, or doesn't carry correctness-relevant information.

### Phase 2: RBTree neighbours-of-probe helper

The current `RBTree` exposes `find`, `remove_min`, `remove_path` (taking
an `RBPath`). For Range-tree adjacency lookups we don't need predecessor
and successor as independent operations — we always want **both
neighbours of a probe value** when classifying an incoming block. A
single tree walk for `K` already records exactly that information: every
"go-right" descent passes through a node with key strictly less than `K`
(predecessor candidate); every "go-left" descent passes through a node
with key strictly greater than `K` (successor candidate). The last turn
of each kind is the tight answer.

Add a single helper:

- `neighbours(K) -> stl::Pair<K, K>` — performs one walk for `K` and
  returns `(largest entry < K, smallest entry > K)`. Either component
  is `Rep::null` when no such neighbour exists.
  **Precondition**: `K` is not present in the tree. This matches the
  `Arena` use case (two free blocks cannot share a starting
  address, so `add_block` only calls `neighbours` on addresses not
  already in the tree); in Debug an assert fires if `K` is encountered
  on the descent.

This replaces two separate `O(log n)` walks per `add_block` with one and
keeps the API surface small. Implement on top of the existing tree
walking primitives (`get_root`, `get_dir`) — no structural changes to
`RBTree` required.

**Test gate**: extend `src/test/func/redblack/redblack.cc` with a
randomised test of `neighbours(K)` against `std::set::lower_bound` /
`upper_bound` as oracle, over thousands of operations and probe values
drawn from `K` values **not** present in the tree. Existing tests must
remain green.

**Review gate**: spec slice = the Phase 2 section above. Reviewer
checks: walk correctly records both turn points; behaviour at empty
tree, single-node tree, `K` smaller than all keys, `K` larger than all
keys, and `K` between two consecutive keys all match the oracle; the
"K not in tree" precondition is asserted in Debug; no structural
changes to `RBTree`'s existing invariants.

### Phase 3+4: Full Arena data structure (atomic)

Create `src/snmalloc/backend_helpers/arena.h` with:

- A `BackendArenaRep` concept describing word-level accessors over the
  three pagemap entries, the variant tag, and the large-size accessor:
  - `get_variant(addr) -> ArenaVariant` / `set_variant`
  - `get_word1(addr)` / `set_word1`, `get_word2(addr)` / `set_word2`
    (first entry, used by BinRep)
  - `get_range_word1(addr)` / `set_range_word1`,
    `get_range_word2(addr)` / `set_range_word2` (second entry, used
    by RangeRep)
  - `get_large_size_chunks(addr)` / `set_large_size_chunks` (third
    entry)
  - Rep word setters preserve only `BACKEND_RESERVED_MASK` (bits 0–7).
    RED_BIT and VARIANT_MASK preservation is handled by the adapters
    via read-modify-write.

- Two internal RBRep adapters:
  - **BinRep**: tagged `BinHandle` (root-pointer mode or child-slot
    mode dispatching to Rep word1/word2). `META_MASK = RED_BIT |
    VARIANT_MASK` preserved on `set`.
  - **RangeRep**: tagged `RangeHandle` dispatching to Rep
    range_word1/range_word2. Same `META_MASK` (paranoid masking
    defends against stale variant bits from pagemap reuse).
  - Both: `compare(k1, k2) = k1 > k2` so `remove_min` returns the
    lowest address. `null = root = 0`.

- `Arena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>`:
  - `B = 2` hardcoded; `INTERMEDIATE_BITS` wiring deferred.
  - `MIN_SIZE_BITS` selects the unit of allocation (= pagemap stride
    when used with `PagemapRep`).
  - `stl::Array<BinTree, Bins::Bitmap::TOTAL_BINS> bin_trees`
  - `RangeTree range_tree`
  - `Bins::Bitmap bitmap`

- Full `add_block(addr, size_chunks)` with consolidation:
  - Uses `range_tree.neighbours(addr)` + `contains_min()` for
    adjacency.
  - Unlinks merged neighbours from both trees and bitmap.
  - Returns overflow `{c_addr, c_size}` when consolidation grows to
    arena scale (case (ii)); returns `{0, 0}` on success.
  - Asserts `addr != 0`, alignment, and size bounds.

- Full `remove_block(n_chunks)` with carving:
  - `bitmap.find_for_request(n_chunks)` → peek min via Rep →
    remove from trees → `Bins::carve` → recursive `add_block` for
    remainders.

- Five-clause `invariant()`:
  1. Maximally consolidated (range-tree adjacency + min-block adjacency)
  2. Cross-tree consistency (forward and reverse membership checks)
  3. Bin classification correctness
  4. Bitmap consistency
  5. Variant-tag consistency

- `get_root_key()` added to `RBTree` (public method, returns root key
  or `Rep::null` when empty).

- `Bitmap::test(size_t bin_id)` added to `ArenaBins` (read-only
  accessor used by `invariant()`).

Modifications to existing files:
- `src/snmalloc/backend_helpers/arenabins.h`: added
  `Bitmap::test()` and made `bin_index` public.
- `src/snmalloc/ds_core/redblacktree.h`: added `get_root_key()`.
- `CMakeLists.txt`: added `backend_arena` to `TESTLIB_ONLY_TESTS`.

**Test gate**: `src/test/func/backend_arena/backend_arena.cc` with
MockRep and 8 test stages (A–H):
- (A) Accessor round-trips
- (B) RBTree smoke via arena
- (C) Empty-state invariant for K ∈ {4, 5, 6}
- (D) add_block without consolidation
- (E) remove_block exact + carving
- (F) Consolidation case matrix (8 cases: all P/S × min/non-min)
- (G) Overflow (interleaved + precise)
- (H) Randomised stress (50 seeds × 500 ops) with Oracle using
  `Bins::Bitmap` for exact bin-classification matching

### Phase 5: `OddTwo` variant for unaligned size-2 blocks

A size-2 block at an odd chunk address cannot serve size-2 requests
(which require 2-chunk alignment). `bin_index({odd, 2})` correctly
places it in bin 0 (size-1 servable set). But:

1. `Min` variant uses only 1 pagemap entry; a size-2 block needs 2.
2. `contains_min` probes bin 0 for single-chunk neighbours — finding
   a size-2 block there and treating it as size 1 corrupts metadata.

All changes are in `arena.h` and the test file.

1. **Add `OddTwo = 3`** to `ArenaVariant` enum.
2. **Change `variant_of`** to take `(size_chunks, chunk_index)`:
   - size 1 → `Min`
   - size 2, even chunk → `TwoMin`
   - size 2, odd chunk → `OddTwo`
   - size 3+ → `Large`
3. **Update `range_from_addr`**: `OddTwo` returns `{addr, 2}` (same as
   `TwoMin`).
4. **Update `insert_block`**: pass `addr_to_chunk(addr)` to `variant_of`.
   The `if (size_chunks >= 2)` range-tree checks already cover `OddTwo`.
5. **Update `contains_min`**: after finding addr in bin 0, check
   `Rep::get_variant(addr) == ArenaVariant::Min`. Return false
   for `OddTwo` entries.
6. **Update invariant clause 5**: pass chunk address to `variant_of`.
7. **Update invariant clause 1c** ("no two adjacent min blocks"):
   skip non-`Min` entries in bin 0 (i.e., `OddTwo` blocks).
8. **Add test cases**:
   - Odd-address size-2 block: verify variant is `OddTwo`, goes in
     correct bin, lives in range tree.
   - Consolidation with `OddTwo` predecessor/successor.
   - `contains_min` does not match `OddTwo` addresses.
   - `remove_block(1)` from an `OddTwo` block: verify carving works
     and the remainder becomes `Min`.

**Test gate**: all existing tests pass + new `OddTwo`-specific tests pass.

### Phase 6: Consolidation — reuse predecessor's Range entry (optimisation)

**Deferred.** Self-contained optimisation that saves two RB-tree operations
per predecessor consolidation. Can be added later if profiling shows it
matters. The design is recorded in the "Consolidation: reusing tree
entries when possible" section above.

### Phase 7: Multi-instance test

Instantiate two `Arena<MockRep>` over disjoint address ranges in
the same test process, drive workloads against both, verify each
invariant independently.

**Test gate**: multi-instance test passes; total memory accounted for
via both instances matches expectations.

### Phase 8: Final review and self-review

Per `claude.md` mandatory review checkpoints:

- Run the recursive principle check (self-review).
- Spawn a fresh-context reviewer subagent. Address findings. Loop until
  reviewer finds no issues.

**Test gate**: full ctest run (Debug) passes; reviewer reports no issues.

---

# Implementation plan: LargeArenaRange phase

## Scope

Build `LargeArenaRange` — a Range pipeline component that wraps
`Arena` behind snmalloc's Range API, suitable for replacing
`LargeBuddyRange`. This plan covers:

- Generalising Arena's Rep interface for pagemap compatibility.
- `PagemapRep` — adapting pagemap entries to Arena's Rep concept.
- `LargeArenaRange` — the Range wrapper with refill and overflow handling.
- Boundary-bit support for safe consolidation across PAL allocations.
- Unit tests for all of the above.

The pipeline integration (replacing `LargeBuddyRange` in `standard_range.h`
and `meta_protected_range.h`) is a separate step ("Update backend to use
LargeArenaRange") that follows once this plan is complete.

## Design

### Rep generalisation: representation-agnostic data structure

`Arena` must be representation-agnostic, mirroring how
`Buddy<>` is generic over its node `Rep` (see `buddy.h`). The
existing buddy ecosystem demonstrates the layering:

- `buddy.h` — pure data structure, no representation.
- `largebuddyrange.h` defines `BuddyChunkRep` — a pagemap-backed Rep
  (red bit at bit 8, layout chosen to coexist with the pagemap's
  reserved low bits).
- `smallbuddyrange.h` defines `BuddyInplaceRep` — an inline Rep that
  stores tree pointers in the free chunk itself (red bit at bit 0).

`Arena` must support the same two representation paths so it
can eventually replace both `LargeBuddyRange` (pagemap) and
`SmallBuddyRange` (inline) in the standard pipeline.

#### Rep concept

`Rep` provides:

- `using BinRep`  — full RBTree Rep for the bin trees.
- `using RangeRep` — full RBTree Rep for the range tree.
- `get_variant(addr)` / `set_variant(addr, v)` — block variant tag.
- `get_large_size_chunks(addr)` / `set_large_size_chunks(addr, n)` —
  precise chunk count for `Large` blocks.
- `can_consolidate(higher_addr)` — false at PAL allocation boundaries.

Each inner `BinRep` / `RangeRep` is a complete RBTree Rep (same shape
as `BuddyChunkRep` / `BuddyInplaceRep`): provides `Handle`,
`Contents`, `null`, `root`, `ref`, `get`, `set`, `is_red`, `set_red`,
`compare`, `equal`, `printable`, `name`. **All bit-packing decisions
(red bit position, mask layout) are private to the Rep** —
`Arena` carries no `RED_BIT` / `VARIANT_MASK` / `META_MASK`
constants of its own.

`Arena` instantiates `RBTree<typename Rep::BinRep>` and
`RBTree<typename Rep::RangeRep>` directly. It never inspects the bit
layout used by the Rep.

#### PagemapRep

Lives in `largearenarange.h`. Privately owns its bit layout:

- Bin tree node in pagemap entry at `addr`, Word::One/Two. `BinRep`
  packs the red bit at bit 8 and the variant tag at bits 9–10 of
  Word::One; bits 0–7 are reserved by the pagemap.
- Range tree node in pagemap entry at `addr + MIN_CHUNK_SIZE`, with
  the same layout for `RangeRep`.
- Large-size chunk count stored as `count << 8` in Word::One of the
  entry at `addr + 2*MIN_CHUNK_SIZE`.

#### MockRep (test only)

Lives in `src/test/func/backend_arena/backend_arena.cc`. Backs its
storage with an array of `mock_entry` and uses the same `RED_BIT =
1 << 8` layout (so it exercises the same code paths the pagemap Rep
will). The mock's `BinRep` and `RangeRep` are inner structs that own
their own ref/get/set/is_red/set_red implementations.

#### Future inline Rep (not in this phase)

Mirroring `BuddyInplaceRep`: tree pointers live inside the free
memory itself. `BinRep` / `RangeRep` would use pointer-low-bits for
red and variant tags. This is what enables a future
`Arena`-based replacement for `SmallBuddyRange`.

### Boundary-bit consolidation check

On platforms where `CONSOLIDATE_PAL_ALLOCS` is false (CHERI, Windows),
the pagemap sets a boundary bit on the first chunk of each PAL allocation
to prevent consolidation across allocation boundaries
(`BuddyChunkRep::can_consolidate` checks this).

Arena's `add_block` consolidation must respect the same contract.
A new method on the Rep concept:

```
static bool can_consolidate(uintptr_t higher_addr);
```

Returns `true` if the block at `higher_addr` may be consolidated with the
block immediately below it. `add_block` checks this before each merge:

- P ↔ A merge: `Rep::can_consolidate(addr)` (A is the higher address).
- A ↔ S merge: `Rep::can_consolidate(succ_addr)` (S is the higher address).

MockRep: always returns `true` (no boundaries).
PagemapRep: returns `!get_metaentry_mut(higher_addr).is_boundary()`.

### PagemapRep

Templated on `Pagemap`, `MIN_SIZE_BITS`, and `MAX_SIZE_BITS` (mirroring
`Buddy`'s shape). `MIN_SIZE_BITS` is the log2 of the pagemap stride
(snmalloc's `MIN_CHUNK_BITS` when wired through `LargeArenaRange`);
`MAX_SIZE_BITS` is needed for the large-size-shift static assertion:

```
template<
  SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
  size_t MIN_SIZE_BITS,
  size_t MAX_SIZE_BITS>
struct PagemapRep { ... };
```

Each free block uses pagemap entries at three offsets from its base
address (where `UNIT_SIZE = 1 << MIN_SIZE_BITS`):

- **Unit 0** (`addr`): Word::One / Word::Two → bin-tree node.
  Bits 9–10 of Word::One → variant tag. Bit 8 → RED_BIT. All coexist
  because `TreeRep::set` preserves `META_MASK` on writes.
- **Unit 1** (`addr + UNIT_SIZE`): Word::One / Word::Two →
  range-tree node (only for blocks ≥ 2 units).
- **Unit 2** (`addr + 2 * UNIT_SIZE`): Word::One → large chunk
  count (only for blocks ≥ 3 units). Stored as `count << 8` to avoid
  the 8 reserved low bits; recovered via `word.get() >> 8`.

**Static assertions in PagemapRep** (catch configuration errors early):

- `static_assert((VARIANT_MASK | RED_BIT) < UNIT_SIZE)` — metadata
  bits don't collide with address bits.
- `static_assert(MetaEntryBase::is_backend_allowed_value(Word::One,
  VARIANT_MASK | RED_BIT))` — all metadata bits are in the backend-
  allowed range.
- `static_assert((MAX_SIZE_BITS - MIN_SIZE_BITS) + LARGE_SIZE_SHIFT <=
  bits::BITS)` — shifted large size fits in a pagemap word.

Method mapping:

| Method | Implementation |
|--------|---------------|
| `ref_word(dir, addr)` | `get_metaentry_mut(addr).get_backend_word(dir ? One : Two)` |
| `ref_range_word(dir, addr)` | `get_metaentry_mut(addr + MCS).get_backend_word(dir ? One : Two)` |
| `get_variant(addr)` | `(ref_word(true, addr).get() & VARIANT_MASK) >> 9` |
| `set_variant(addr, v)` | RMW on `ref_word(true, addr)`: clear VARIANT_MASK, OR new value |
| `get_large_size_chunks(addr)` | `get_metaentry_mut(addr + 2*MCS).get_backend_word(One).get() >> 8` |
| `set_large_size_chunks(addr, s)` | `...get_backend_word(One) = s << 8` |
| `can_consolidate(addr)` | `!get_metaentry_mut(addr).is_boundary()` |

(`MCS = MIN_CHUNK_SIZE`)

`get_backend_word` auto-calls `claim_for_backend()` on first access to
an unowned entry, so pagemap ownership transitions happen implicitly.
The boundary bit (bit 0 of `meta`) is in the reserved-mask zone and is
preserved by both `claim_for_backend()` and `BackendStateWordRef::operator=`.

### LargeArenaRange

Outer template matches `LargeBuddyRange`'s shape so it is a drop-in
replacement in `Pipe<...>` compositions:

```
template<
  size_t REFILL_SIZE_BITS,
  size_t MAX_SIZE_BITS,
  SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
  size_t MIN_REFILL_SIZE_BITS = 0>
class LargeArenaRange
{
public:
  template<typename ParentRange = EmptyRange<>>
  class Type : public ContainsParent<ParentRange>
  {
    using ContainsParent<ParentRange>::parent;

    using PagemapRepT = PagemapRep<Pagemap, MIN_CHUNK_BITS, MAX_SIZE_BITS>;
    Arena<PagemapRepT, MIN_CHUNK_BITS, MAX_SIZE_BITS> arena;
    size_t requested_total = 0;

  public:
    static constexpr bool Aligned = true;
    static constexpr bool ConcurrencySafe = false;
    using ChunkBounds = capptr::bounds::Arena;

    capptr::Arena<void> alloc_range(size_t size);
    void dealloc_range(capptr::Arena<void> base, size_t size);
  };
};
```

**`alloc_range(size)`**:

1. `SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE)`.
2. `SNMALLOC_ASSERT((size & (MIN_CHUNK_SIZE - 1)) == 0)` — size must be
   a chunk multiple, but no power-of-two restriction. The arena handles
   any size in `[MIN_CHUNK_SIZE, 2^MAX_SIZE_BITS)`.
3. `n_chunks = size >> MIN_CHUNK_BITS`.
4. Oversize bypass: if `n_chunks >= bits::one_at_bit(MAX_SIZE_BITS - MIN_CHUNK_BITS)`,
   delegate to `parent.alloc_range(size)` (if `ParentRange::Aligned`),
   else return `nullptr`. Same as `LargeBuddyRange`.
5. `auto [addr, actual] = arena.remove_block(n_chunks)`. The arena
   carves exactly `n_chunks` chunks via `Bins::carve`; `actual` is
   always `n_chunks` on success and is asserted as such.
6. If `addr != 0`, return
   `capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(addr))`.
7. If `addr == 0`, call `refill(size)`.

**`dealloc_range(base, size)`**:

1. `SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE)`,
   `SNMALLOC_ASSERT((size & (MIN_CHUNK_SIZE - 1)) == 0)` — chunk multiple
   only; no power-of-two restriction.
2. Oversize bypass: if `size >= 2^MAX_SIZE_BITS`, delegate to
   `parent.dealloc_range(base, size)`. Same SFINAE guard as
   `LargeBuddyRange::parent_dealloc_range`.
3. `n_chunks = size >> MIN_CHUNK_BITS`,
   `auto [ov_addr, ov_size] = arena.add_block(base.unsafe_uintptr(), n_chunks)`.
4. If overflow (`ov_addr != 0`): call `dealloc_overflow(ov_addr,
   ov_size)`.

**`dealloc_overflow(addr, size_chunks)`**:

Overflow from `add_block` is forwarded directly to the parent's
`dealloc_range`. The parent does not require power-of-two input — all
non-Buddy ranges accept any chunk-aligned size, and `LargeArenaRange`
itself accepts any chunk-multiple size — so no decomposition is needed.

```
void dealloc_overflow(uintptr_t addr, size_t size_chunks)
{
  if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
  {
    auto base = capptr::Arena<void>::unsafe_from(
      reinterpret_cast<void*>(addr));
    size_t size_bytes = size_chunks << MIN_CHUNK_BITS;
    parent.dealloc_range(base, size_bytes);
  }
  else
  {
    // Global range: no parent to return to.
    SNMALLOC_CHECK(false && "Global range overflow should not happen");
  }
}
```

When `MAX_SIZE_BITS == BITS - 1` (global range), the arena covers the
entire address space. Overflow would mean all managed memory has
coalesced — this should not happen in normal operation. If it does,
abort (matching `LargeBuddyRange`'s behaviour for the unreachable
case).

**`refill(size)`** — closely follows `LargeBuddyRange::refill`:

For `ParentRange::Aligned` (the standard path):

1. Compute `refill_size = min(REFILL_SIZE, requested_total)`, clamped to
   `max(MIN_REFILL_SIZE, size)`, rounded up to next power of two.
2. `auto refill_range = parent.alloc_range(refill_size)`.
3. If `refill_range != nullptr`:
   - `requested_total += refill_size`.
   - `remainder_size = refill_size - size`.
   - If `remainder_size > 0`:
     `arena.add_block(refill_range.unsafe_uintptr() + size,
                      remainder_size >> MIN_CHUNK_BITS)`.
     Handle overflow (send to parent).
   - Return `refill_range`.
4. If `nullptr`, return `nullptr`.

The returned portion (`refill_range` to `refill_range + size`) bypasses
the arena entirely — it is not inserted or tracked. The remainder is
added to the arena for future allocations. Since the remainder comes
from a fresh refill and has no neighbours in the arena, `add_block`
performs a simple insertion with no consolidation (boundary bit on the
refill base may prevent consolidation with any pre-existing blocks
below it, which is correct).

For the unaligned parent path: over-allocate `2 * size` (with overflow
check), add everything to the arena via `add_range`, then call
`alloc_range(size)` recursively.

**`add_range(base, length)`** trims `(base, length)` to chunk boundaries
on both ends (PalRange returns page-aligned but not chunk-aligned
addresses) and inserts a single block via `add_block` — no power-of-two
decomposition is needed because `add_block` accepts any size in
`[1, 2^CHUNKS_BITS)` chunks. Any overflow from `add_block` is forwarded
to `dealloc_overflow`.

Safety guards (both from `LargeBuddyRange`):
- `static_assert((REFILL_SIZE < bits::one_at_bit(MAX_SIZE_BITS)) ||
  ParentRange::Aligned)` — prevents the unaligned path from adding a
  block that violates `add_block`'s `size_chunks < 2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)`
  precondition.
- Runtime: `SNMALLOC_ASSERT(refill_size < bits::one_at_bit(MAX_SIZE_BITS))`
  — catches the computed `refill_size` (which may be larger than
  `REFILL_SIZE` when `needed_size = 2 * size` dominates).

### Static properties

- `Aligned = true`: Arena's carving ensures that a request of
  size `n` (power-of-two, chunk-aligned) is placed at an `n`-aligned
  address within the source block. For non-power-of-two requests, the
  bin scheme's alignment rules still hold (alignment matches the
  lowest set bit of the size class).
- `ConcurrencySafe = false`: same as `LargeBuddyRange`.
- `ChunkBounds = capptr::bounds::Arena`: same as `LargeBuddyRange`.

### MAX_SIZE_BITS = BITS - 1 (global range)

The global `LargeBuddyRange` uses `MAX_SIZE_BITS = BITS - 1`, meaning
the buddy can hold up to half the address space. For LargeArenaRange:
the maximum block size in chunks is `2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)`.
On 64-bit with `MIN_CHUNK_BITS = 14`, this gives a chunk-bit width of
49 — the arena can hold up to 2^49 chunks. The arena's overflow path
returns consolidated blocks that reach this size, handled by
`dealloc_overflow` (see above).

The `large_size_chunks` field (stored shifted by 8 in a pagemap word)
needs at most 49 bits, which fits in the 56 backend-usable bits of a
64-bit pagemap word. A `static_assert((MAX_SIZE_BITS - MIN_SIZE_BITS) +
LARGE_SIZE_SHIFT <= bits::BITS)` in `PagemapRep` catches configurations
where this would overflow.

## Phases

### Phase 9: Rep generalisation + boundary support

**Status**: implemented; staged (not committed); awaiting review.

Changes to `arena.h`:

1. Delete the private `WordRef` nested struct, the `TreeRep`
   template, and all bit-layout constants
   (`RED_BIT`/`VARIANT_MASK`/`META_MASK` and `BACKEND_RESERVED_MASK`).
   `Arena` is now representation-agnostic, mirroring how
   `buddy.h` is generic over its node `Rep`.
2. Replace the internal `using BinRep = TreeRep<Rep::ref_word>` /
   `RangeRep = TreeRep<Rep::ref_range_word>` aliases with direct use
   of `typename Rep::BinRep` and `typename Rep::RangeRep` — full
   RBTree Reps supplied by the user, owning their own bit packing.
3. Update the Rep concept doc to require `BinRep`, `RangeRep`,
   `get_variant`/`set_variant`,
   `get_large_size_chunks`/`set_large_size_chunks`, and
   `can_consolidate`.
4. Add `can_consolidate` calls in `add_block` before each merge
   (predecessor and successor) and update the invariant clauses to
   tolerate boundary-blocked adjacency.

Changes to `backend_arena.cc` (test file):

5. Define `BackendArenaWordRef` (test-only proxy) at the top of the
   test file.
6. MockRep grows inner `BinRep` and `RangeRep` structs that each
   provide the full RBTree Rep interface (ref/get/set/is_red/etc.)
   over the mock-entry array. Each owns its own private bit layout
   (red bit at bit 8 to match the PagemapRep layout).
7. MockRep keeps top-level `get_variant`/`set_variant`/large-size
   accessors and adds `can_consolidate(uintptr_t) → true`.
8. New test: verify that a MockRep variant with `can_consolidate`
   returning false at a specific address prevents consolidation across
   that boundary. Test both predecessor and successor merges being
   independently blocked.

**Test gate**: all existing Arena tests pass unchanged; new
boundary test passes.

### Phase 10: PagemapRep + LargeArenaRange + tests

**Status**: implemented and tested. Committed in `9c1ca745`.

> **Note**: the design notes below were written before Phase 10d
> (bytes-throughout). The as-built code uses byte sizes everywhere
> at the arena/range API and a unified `parent_dealloc(uintptr_t,
> size_t)` helper in place of the old `dealloc_overflow` /
> `parent_dealloc_range` pair. See the Phase 10d section for the
> current shape. Where the notes below say `size_chunks`, the
> implementation uses bytes; where they say `dealloc_overflow`, the
> implementation uses `parent_dealloc`.

**Phase 10b refactor (also implemented):** `Arena` and `PagemapRep`
were both retemplated to mirror `Buddy`'s 3-parameter shape:

- `template<typename Rep, size_t MIN_SIZE_BITS, size_t MAX_SIZE_BITS> class Arena`
  — the always-zero `MIN_CHUNKS_BITS` placeholder is gone, and the unit
  of allocation is named explicitly via `MIN_SIZE_BITS` instead of being
  implicitly tied to snmalloc's global `MIN_CHUNK_BITS`. Internally,
  `UNIT_SIZE = 1 << MIN_SIZE_BITS` and `CHUNKS_BITS = MAX_SIZE_BITS -
  MIN_SIZE_BITS` replace the old `MIN_CHUNK_SIZE` / `MAX_CHUNKS_BITS`
  usages.
- `template<Pagemap, size_t MIN_SIZE_BITS, size_t MAX_SIZE_BITS> class PagemapRep`
  — owns the large-size-shift capacity static_assert
  `(MAX_SIZE_BITS - MIN_SIZE_BITS) + LARGE_SIZE_SHIFT <= bits::BITS`;
  `LARGE_SIZE_SHIFT` is private. The Rep's pagemap stride is
  `UNIT_SIZE = 1 << MIN_SIZE_BITS`.
- `LargeArenaRange::Type` wires snmalloc's `MIN_CHUNK_BITS` as
  `MIN_SIZE_BITS` for both PagemapRep and Arena:
  `PagemapRep<Pagemap, MIN_CHUNK_BITS, MAX_SIZE_BITS>` and
  `Arena<PagemapRepT, MIN_CHUNK_BITS, MAX_SIZE_BITS>`.

New file: `src/snmalloc/backend_helpers/largearenarange.h`

1. `PagemapRep<Pagemap, MIN_SIZE_BITS, MAX_SIZE_BITS>` — full Rep
   implementation using pagemap entries as described above, with all
   static assertions.
2. `LargeArenaRange<REFILL_SIZE_BITS, MAX_SIZE_BITS, Pagemap,
   MIN_REFILL_SIZE_BITS>` — the Range wrapper with `alloc_range`,
   `dealloc_range`, `refill`, and `dealloc_overflow`.

Modified: `src/snmalloc/backend_helpers/backend_helpers.h`

3. Add `#include "largearenarange.h"` so the new header is
   available through the standard include path.

New file: `src/test/func/backend_arena_range/backend_arena_range.cc`

4. Test with snmalloc's `BasicPagemap` (or a test-appropriate pagemap):
   - PagemapRep word round-trips (variant, tree words, large size).
   - LargeArenaRange `alloc_range` / `dealloc_range` smoke test with
     a simple parent range.
   - Refill: verify that allocating when the arena is empty triggers a
     parent refill and returns memory.
   - Overflow: verify that deallocating a block that triggers arena-scale
     consolidation forwards the overflow to the parent via
     `dealloc_overflow`.
   - Non-power-of-two sizes: verify `alloc_range` / `dealloc_range` work
     for chunk-multiple but non-power-of-two sizes, including sizes that
     are not representable size classes. The arena carves exactly the
     requested chunk count internally, so callers see no excess.
   - Boundary: verify that a boundary bit in the pagemap prevents
     consolidation of adjacent blocks from different refills (when
     `CONSOLIDATE_PAL_ALLOCS` is false).
   - Test at largest configured `MAX_SIZE_BITS` values, especially
     `MAX_SIZE_BITS == bits::BITS - 1` if feasible.

Modified: `CMakeLists.txt`

5. Register `backend_arena_range` in `TESTLIB_ONLY_TESTS`.

**Test gate**: LargeArenaRange tests pass; existing tests unaffected.

### Phase 11: Final review

Per `claude.md` mandatory review checkpoints:

- Spawn a fresh-context reviewer on the full diff (Phases 9–10).
- Address findings, loop until clean.

**Test gate**: full ctest run passes; reviewer reports no issues.

### Phase 10d: Bytes throughout (replace chunk-count internal API)

**Goal**: drop the `size_chunks` / chunk-count internal convention from
`Arena` and `PagemapRep` so byte sizes (multiples of UNIT_SIZE)
flow end-to-end, removing the `<< MIN_CHUNK_BITS` conversion dance at
the LargeArenaRange ↔ Arena boundary and the matching reverse
shifts inside the range wrapper.

**Substep 1 (DONE)**: generalise `ArenaBins` on a new
`MIN_SIZE_BITS` template parameter so its `range_t.size`, carve
arguments, and `max_supported_size()` are byte sizes (multiples of
`UNIT_SIZE = 1 << MIN_SIZE_BITS`). Renames inside Bins:
`size_chunks → size`, `align_chunks → align`, `max_supported_chunks
→ max_supported_size`. Tests cover `MIN_SIZE_BITS ∈ {0, 4, 14}`.

**Substep 2 (DONE)**: flip `Arena`, `PagemapRep`, and
`LargeArenaRange` to bytes throughout:
- `Arena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>` now uses
  `ArenaBins<B, MIN_SIZE_BITS>`; `add_block` / `remove_block`
  take/return bytes; `addr_to_chunk` / `chunk_to_addr` / `CHUNKS_BITS`
  deleted; `variant_of(size, addr)` works in byte units with
  parity from `(addr >> MIN_SIZE_BITS) & 1`.
- `remove_block(size)` returns a scalar `addr_t` (0 = failure). The
  size in the returned pair was tautological (always equal to the
  requested `size` on success).
- `PagemapRep::get_large_size` / `set_large_size` (renamed from
  `*_chunks`) take and return bytes; internal storage still scales
  by `MIN_SIZE_BITS` so the shifted field fits a pagemap word.
- `LargeArenaRange::add_range` / `dealloc_range` /
  `parent_dealloc` (unified from `parent_dealloc_range` and
  `dealloc_overflow`) drop chunk-count conversions; `add_range`
  uses `bits::align_up` / `bits::align_down`.
- Test scaffolding (`MockRep`, `BoundaryMockRep`, `Oracle`)
  updated; tests introduce `chunk_size(N) = N << MIN_CHUNK_BITS`
  helper.

**Test gate**: `func-backend_arena-check`, `func-backend_arena_bins-check`,
`func-backend_arena_range-check` all pass; full `ninja` build clean.

**Remaining**: code-review checkpoint for Phase 10d combined diff
before opening a PR; then proceed to Phase 12 (pipeline integration).

*Pipeline integration (replacing `LargeBuddyRange` in `standard_range.h`
and `meta_protected_range.h`) is a separate follow-up plan: "Update
backend to use LargeArenaRange."*

## Files added / changed (anticipated, this phase)

- Modified: `src/snmalloc/backend_helpers/arena.h` —
  representation-agnostic: delete private `WordRef`, `TreeRep`, and
  all bit-layout constants (`RED_BIT`/`VARIANT_MASK`/`META_MASK`/
  reserved); use `Rep::BinRep` and `Rep::RangeRep` directly;
  `can_consolidate` check in `add_block`; invariant clauses updated.
- New: `src/snmalloc/backend_helpers/largearenarange.h` —
  `PagemapRep` + `LargeArenaRange`.
- Modified: `src/snmalloc/backend_helpers/backend_helpers.h` — include
  `largearenarange.h`.
- Modified: `src/test/func/backend_arena/backend_arena.cc` — define
  `BackendArenaWordRef` test helper at top of file; MockRep updated
  (`BackendArenaWordRef` returns, `can_consolidate`); boundary tests.
- New: `src/test/func/backend_arena_range/backend_arena_range.cc` —
  Range wrapper tests.
- Modified: `CMakeLists.txt` — register `backend_arena_range` test.

## Key design decisions

1. **Representation-agnostic data structure** — `Arena`
   carries no bit-layout constants. All red/variant packing decisions
   live in the user-supplied `Rep::BinRep` / `Rep::RangeRep`, matching
   how `BuddyChunkRep` and `BuddyInplaceRep` each own their own
   layouts. This is what makes a future inline Rep (to replace
   `SmallBuddyRange`) possible.

2. **PagemapRep variant in bin-tree Word::One** — PagemapRep packs
   the variant tag at bits 9–10 of Word::One alongside the red bit
   (bit 8) and child pointer (bits ≥ MIN_CHUNK_BITS). These are
   private constants inside PagemapRep, not exposed by Arena.

3. **Large size stored shifted** — PagemapRep stores the chunk count
   as `count << 8` to avoid the pagemap's reserved low byte; recovered
   via `>> 8`. Guarded by `static_assert((MAX_SIZE_BITS - MIN_CHUNK_BITS) + 8 <= bits::BITS)`.

4. **Boundary checks in Arena** — not in LargeArenaRange.
   Consolidation decisions happen inside `add_block`, so the boundary
   check must be there. The Rep concept cleanly abstracts this via
   `can_consolidate`.

5. **Refill returns prefix directly** — like LargeBuddyRange, the
   first `size` bytes of a refill bypass the arena. Only the remainder
   enters the arena. This avoids unnecessary tree operations on the
   hot path.

6. **PagemapRep auto-claims entries** — `get_backend_word` calls
   `claim_for_backend()` on first access. No explicit ownership
   management needed in Arena or LargeArenaRange.

7. **Overflow forwarding** — `add_block` overflow may produce non-
   power-of-two sizes (consolidated blocks from multiple PAL allocs).
   `dealloc_overflow` forwards the overflow directly to the parent's
   `dealloc_range`; no power-of-two decomposition is needed because
   `LargeArenaRange` (which is what replaces `LargeBuddyRange` in
   the pipeline) accepts any chunk-multiple size.

8. **`BackendArenaWordRef` lives in the test file** — the in-tree
   `PagemapRep` returns `BackendStateWordRef` directly (mirroring
   `BuddyChunkRep` in `largebuddyrange.h`). The test-only
   `BackendArenaWordRef` proxy is defined in
   `src/test/func/backend_arena/backend_arena.cc` and used only by
   MockRep, so the in-tree headers carry no test scaffolding.

9. **No power-of-two restriction on the public API** — `alloc_range`
   and `dealloc_range` accept any chunk-multiple size; the only
   restriction is `size >= MIN_CHUNK_SIZE` and `size < 2^MAX_SIZE_BITS`.
   The arena's `Bins::carve` delivers exactly the requested chunk
   count, rolling any size-class rounding remainder into the post
   fragment that is re-inserted internally. SC rounding therefore
   stays a private arena detail. This lifts a restriction inherited
   from `LargeBuddyRange`.

## Resolved during plan review

- Overflow handling: `add_block` can return non-power-of-two sizes when
  blocks from multiple PAL allocations consolidate. `dealloc_overflow`
  forwards the overflow directly to the parent — no decomposition is
  required because `LargeArenaRange` itself accepts arbitrary
  chunk-multiple sizes and replaces `LargeBuddyRange` in the pipeline.
  (Rubber-duck finding #2 superseded by Option B refactor.)
- Handle visibility / layering: original plan promoted bit-layout
  constants and a `BackendArenaWordRef` proxy to namespace scope so
  the in-tree header and tests could share them. Subsequent review
  observed that this broke the Buddy/`BuddyChunkRep`/`BuddyInplaceRep`
  layering: the data structure should be representation-agnostic.
  Resolved by making `Arena` carry no bit-layout state and
  requiring `Rep::BinRep` / `Rep::RangeRep` to own all packing
  decisions. `PagemapRep` keeps its layout private; the
  test `BackendArenaWordRef` lives in the test file alongside MockRep.
  (Rubber-duck finding #1, then revised after layering review.)
- Size shift overflow: `static_assert((MAX_SIZE_BITS - MIN_CHUNK_BITS) + 8 <= BITS)` in
  `PagemapRep` prevents shift overflow. (Rubber-duck finding #4.)
- Unaligned refill guard: both static assert AND runtime assert copied
  from `LargeBuddyRange` to prevent `add_block` precondition violation.
  (Rubber-duck finding #6, strengthened in second review.)
- Pipeline integration (Phase 11) removed from this plan's scope —
  separate follow-up plan. (Rubber-duck finding #8.)
- `PagemapRep` templated on `MIN_SIZE_BITS` and `MAX_SIZE_BITS` so the
  size-shift static_assert is in scope. (Second review finding #1.)
- `remove_block` exact-size guarantee is scoped to power-of-two
  requests only. (Second review finding #4.)

---

## Files added / changed (Arena phase, completed)

- New: `src/snmalloc/backend_helpers/arenabins.h` —
  `range_t`, `carve_t`, `carve`, `max_supported_chunks`, and nested
  `Bitmap` with `add` / `find_for_request` / `clear` (public surface);
  the size-class encoding (`bitmap_info_t`, `carve_info_t`, constexpr
  `BinTable`, `bitmap_info_for_request` / `carve_info_for_request`,
  `bin_index`) is private and reachable via
  `ArenaBinsTestAccess` (forward-declared in the header,
  defined in the test cc) for unit tests. Templated on
  `INTERMEDIATE_BITS` for testability.
- New: `src/snmalloc/backend_helpers/arena.h` — the data structure,
  templated on a `BackendArenaRep` concept exposing variant-tag and
  node/size accessors (no pagemap-probing API).
- New: `src/test/func/backend_arena_bins/backend_arena_bins.cc` — bin
  classification tests and `find_for_request` tests for
  `B ∈ {1, 2, 3}`, using `bin_subsets` as the canonical "serves"
  predicate.
- New: `src/test/func/backend_arena/backend_arena.cc` — data-structure
  tests with a mock Rep (array-backed pagemap, modelled on `redblack.cc`).
- Modified: `src/snmalloc/ds_core/redblacktree.h` — `neighbours(K)`
  helper on `RBTree` returning `(largest < K, smallest > K)` in one walk.
- Modified: `src/test/func/redblack/redblack.cc` — randomised
  `neighbours(K)` tests against `std::set::lower_bound` /
  `upper_bound` as oracle.

No in-tree code path is changed in this phase: the existing
`LargeBuddyRange` continues to be the active large-block allocator.

## Resolved during plan review

- One Bin tree per IDEA servable-set bin (not per size class or per
  exponent).
- Scope is the Arena data structure + tests only.
- The pagemap encoding carries a 2-bit **variant tag**
  (`Min` / `TwoMin` / `Large`) on the first entry of each free block.
  Tree membership — not the tag — is the source of truth for "is this
  block free?". No transient `BackendOwned` / "claimed" tag is required.
- **No pagemap probing.** All adjacency lookups are restricted to this
  `Arena`'s own RBTrees: non-min neighbours come from a single
  `Range.neighbours(addr_A)` walk that returns both
  `(largest < addr_A, smallest > addr_A)`; min-size neighbours come from
  `MinSizeBin.find(addr_A ± MIN_CHUNK_SIZE)`. The pagemap is never read
  at speculative addresses (concurrency hazard and no defined contract
  for pagemap entries the Arena does not own).
- Free blocks may have **arbitrary chunk counts**, not just exact
  size-class sizes — carving produces non-class remainders. `bin_index`
  operates on `(addr_chunks, size_chunks)` pairs; `Large` blocks store
  their precise chunk count in the third pagemap entry.
- Write-ordering rule: when adding a free block, the variant tag and any
  auxiliary fields are written before the final RB-tree insertion that
  makes the block reachable; when removing, the block is unlinked from
  its trees before its pagemap entries are reused.
- Predecessor-Range-entry-reuse only applies when `P` is non-min.
- `add_block` returns `{0, 0}` on success; on overflow it returns the
  unabsorbed range, mirroring `Buddy::add_block`'s overflow-return
  contract. Oversize inputs (`size_chunks >= 2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)`) bypass
  `Arena` entirely — the wrapping `LargeArenaRange` layer
  handles them before calling `add_block`, and `add_block` asserts
  `size_chunks < 2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)`. The only overflow case is
  consolidation growing a coalesced block to exactly
  `2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)` (the consolidated range is returned, neighbours
  having been removed first). The future `LargeArenaRange` wrapper is
  responsible for handling overflow; the standalone `Arena` only
  exposes the contract.
- `BackendArenaRep` is a chunk-keyed accessor concept (variant tag plus
  word/size accessors for entries 1–3). `Arena` builds two
  internal `RBTree`-Rep adapters (`BinRep`, `RangeRep`) over it; user
  code never sees the adapter shape.
- Backend chunk size classes are a new chunk-unit size-class scheme in
  `arenabins.h` (not bytes), independent of the
  power-of-two-only large variant of front-end `sizeclass_t`, with
  low-exponent special cases handled in the spirit of
  `bits::from_exp_mant`.
- `Arena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>` uses byte-size
  exponent bounds with **exclusive max** semantics, matching the existing
  `Buddy<..., MIN, MAX>`.
- Multi-`B` testing is via a templated bin-table generator in a single
  test binary, not via separate CMake configurations.
- Phase 5 verifies the reuse optimisation via Range-tree insert/remove
  *call counters* at the `Arena` layer (no `RBTree` modification).

## Still open (resolve during implementation)

- ~~Exact bit positions in the first-word pagemap encoding for the
  variant-tag field.~~ **Resolved** (Phase 3+4): bits 9–10 encode
  `ArenaVariant` (`VARIANT_MASK = 0x600`); bit 8 is `RED_BIT`;
  bits 0–7 are `BACKEND_RESERVED_MASK`. Documented in
  `arena.h`.
- ~~Whether Bin tree roots are stored flat
  (`Array<Root, TOTAL_BINS>`) or exponent-keyed.~~ **Resolved**
  (Phase 3+4): flat `stl::Array<BinTree, Bins::Bitmap::TOTAL_BINS>`.
- Whether the future memcpy `offset` field is best placed in the second
  word of every pagemap entry, in dedicated entries, or in a side table.
  Out of scope for this phase; flagged for the memcpy-fix plan to design.
- Whether `INTERMEDIATE_BITS=4` (34 bins/exp) needs to be tested in this
  phase. Currently `B ∈ {1, 2, 3}` only.

---

# Phase 12: Update backend to use LargeArenaRange

## Status: implementation complete, awaiting commit approval

Substitution implemented and tested in the working tree (uncommitted on
top of `9c1ca745`). `Arena::add_block` had a latent
out-of-region pagemap-probe bug in its successor-min branch that
became reachable once `LargeArenaRange` started serving fixed-region
allocations; fixed in this phase (see "Issue found during Phase 12
test run" below). Full ctest suite passes (86/86).

Diff: 6 files, 183/45 +/- (PLAN.md, both pipeline range headers,
`arena.h`, `arenabins.h`, `backend_arena.cc`).

## Goal

Replace every `LargeBuddyRange` instantiation in the range
pipelines with `LargeArenaRange`. After this phase, snmalloc uses
the Arena bin-tree allocator instead of the power-of-two buddy
for all large-range management. The `LargeBuddyRange` and
`BuddyChunkRep` classes are **not deleted** — they remain available
for alternative configurations and external embedders. Only the
default pipeline wiring changes.

## Scope

- Modify `standard_range.h` — replace all `LargeBuddyRange` with
  `LargeArenaRange` (same template parameters).
- Modify `meta_protected_range.h` — replace all `LargeBuddyRange`
  with `LargeArenaRange` (same template parameters).
- **No other source files change.** `LargeArenaRange` is already a
  drop-in replacement: same template signature, same `Type<Parent>`
  shape, same `alloc_range`/`dealloc_range` API, same `Aligned`,
  `ConcurrencySafe`, and `ChunkBounds` constants.

## Pre-conditions

- Phase 10 (LargeArenaRange) is committed and all its tests pass
  (commit `9c1ca745`).
- Phase 11 (final review of Phases 9–10) was waived by the user;
  Phase 12 proceeds without it.
- Baseline: the checkout builds and all tests pass before this change.
  Recorded after 9c1ca745: 86/86 ctest passed, no warnings.

## Analysis of every LargeBuddyRange instantiation

### `standard_range.h`

**1. GlobalR**
```cpp
LargeBuddyRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, MinSizeBits>
```
→ `LargeArenaRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, MinSizeBits>`

- `MAX_SIZE_BITS = bits::BITS - 1` → global-range mode (no parent
  dealloc). `LargeArenaRange` handles this identically.
- `MIN_REFILL_SIZE_BITS = MinSizeBits` (Windows: 16, otherwise PAL-
  dependent). `LargeArenaRange` passes this through.
- Parent is `Base` (PalRange + PagemapRegisterRange chain). Parent is
  **unaligned** on PALs without `AlignedAllocation` (e.g. Linux mmap)
  and aligned otherwise. `LargeArenaRange::refill` currently still
  carries the aligned/unaligned dual path inherited from
  `LargeBuddyRange`; collapsing this into a single path is deferred to
  Phase 13.

**2. LargeObjectRange (local cache)**
```cpp
LargeBuddyRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>
```
→ `LargeArenaRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>`

- `MAX_SIZE_BITS = LocalCacheSizeBits = 21` (2 MiB). Non-global mode.
  Overflow goes to parent.
- `LargeArenaRange::parent_dealloc` forwards directly to parent
  without decomposition (single block returned by
  `Arena::add_block` when consolidation reaches the arena-scale
  upper bound). The size is a chunk multiple up to `2^MAX_SIZE_BITS`,
  not necessarily power-of-two — the parent must accept arbitrary
  chunk-multiple sizes.
- Wrapped in `StaticConditionalRange` — no impact on the substitution.

### `meta_protected_range.h`

**3. GlobalR** — identical to standard_range.h #1.

**4. CentralObjectRange**
```cpp
LargeBuddyRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap>
```
→ `LargeArenaRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap>`

- `MIN_REFILL_SIZE_BITS = 0` (default). Global-range mode.

**5. CentralMetaRange**
```cpp
LargeBuddyRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, page_size_bits>
```
→ `LargeArenaRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, page_size_bits>`

- Global-range mode.

**6. CentralMetaRange conditional huge-page buddy**
```cpp
stl::conditional_t<
  (max_page_chunk_size_bits > MIN_CHUNK_BITS),
  LargeBuddyRange<
    max_page_chunk_size_bits, max_page_chunk_size_bits,
    Pagemap, page_size_bits>,
  NopRange>
```
→ Replace `LargeBuddyRange` with `LargeArenaRange` inside the
  `conditional_t`.

- This is a small local cache for huge-page consolidation.
  `MAX_SIZE_BITS = max_page_chunk_size_bits` (typically
  `page_size_bits` when page_size_bits > MIN_CHUNK_BITS, e.g.
  huge pages at 21 bits).
- Non-global mode. Overflow forwarded to parent as one consolidated
  chunk-multiple block via `parent_dealloc`.

**7. ObjectRange (local)**
```cpp
LargeBuddyRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>
```
→ `LargeArenaRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>`

- Same shape as standard_range.h #2.

**8. MetaRange (local)**
```cpp
LargeBuddyRange<LocalCacheSizeBits - SubRangeRatioBits, bits::BITS - 1, Pagemap>
```
→ `LargeArenaRange<LocalCacheSizeBits - SubRangeRatioBits, bits::BITS - 1, Pagemap>`

- `REFILL_SIZE_BITS = 21 - 6 = 15`. Global-range mode.
  `MIN_REFILL_SIZE_BITS = 0`.

## Implementation

The change is a mechanical text substitution — replace the string
`LargeBuddyRange` with `LargeArenaRange` in both files. No
template parameters, no API calls, no structural changes.

### Step 1: Replace LargeBuddyRange → LargeArenaRange

In `src/snmalloc/backend/standard_range.h`:
- 2 instantiations of `LargeBuddyRange<` (GlobalR, LargeObjectRange).

In `src/snmalloc/backend/meta_protected_range.h`:
- 6 instantiations of `LargeBuddyRange<` (GlobalR, CentralObjectRange,
  CentralMetaRange, the `conditional_t` huge-page cache,
  ObjectRange, MetaRange).

### Step 2: Verify include paths

Both files include `"../backend/backend.h"` which includes
`"../backend_helpers/backend_helpers.h"` which already includes
`"largearenarange.h"`. **No new includes needed.**

### Step 3: Build and test

- Full `ctest` suite must pass. This is the primary validation:
  hundreds of functional tests exercise the full allocator pipeline.
- Specific tests to watch:
  - `func-memory-fast` — core malloc/free workloads
  - `func-pool-fast` — pool allocator
  - `func-domestication-fast` — boundary/domestication
  - `func-fixed_region-fast` — fixed-region (uses `FixedRangeConfig`
    which uses `StandardLocalState`)
  - `perf-*` — performance tests (functional correctness only)

**Test gate**: full `ctest` passes. No new tests needed — the existing
test suite exercises the pipeline end-to-end.

### Issue found during Phase 12 test run: out-of-region pagemap probe

`func-fixed_region_alloc-check` segfaulted in `PagemapRep::can_consolidate`
when `Arena::add_block` was called with a block whose
`succ_addr = addr + size` sat one chunk past the registered pagemap
range (the last 8 MiB of a 256 MiB FixedRange). The bug shape matches
the `buddy.h:90-93` comment exactly: `can_consolidate` reads the
pagemap entry at `succ_addr`, and that read is only safe once a
tree-membership test has confirmed the address is in our region.

**Fix.** In `Arena::add_block`, the successor-min branch was
reordered so the tree-membership check (`contains_min(succ_addr)`)
short-circuits before the pagemap probe (`Rep::can_consolidate`).
All other can_consolidate call sites already had their preconditions
established (either `addr` is the input block, or the address was
returned from `range_tree.neighbours()` and is in the tree).

**Regression coverage.** `MockRep` was extended with a per-chunk
`boundary` flag stored on `mock_entry`. `MockRep::can_consolidate(addr)`
now returns `!mock_store[mock_index(addr)].boundary` — faithful to the
real `PagemapRep::can_consolidate` reading `entry.is_boundary()`. The
`mock_index` bounds assertion fires on any out-of-range probe, so the
unsafe pattern trips in unit tests rather than only as a segfault in
release builds. A new test `test_block_at_arena_top_edge` adds a block
whose `succ_addr` sits one past the arena's pagemap; without the
reorder this test reproduces the original failure.

This unification also subsumed the previous `BoundaryMockRep` and its
`boundary_addrs` global `std::set`: the four boundary tests
(`test_boundary_blocks_predecessor`, `test_boundary_blocks_successor`,
`test_boundary_partial`, `test_boundary_blocks_min_predecessor`) now
run on `Arena<K>` and set `mock_store[mock_index(addr)].boundary = true`
instead. Net −35 lines in `backend_arena.cc`.

A leftover `throw "..."` in `arenabins.h:807` (used as a
constexpr-failure trick in the `BinTable` constructor) caused a build
failure in `-fno-exceptions` configurations during Phase 12. Replaced
with `SNMALLOC_CHECK(false && "...")`, which is non-constexpr and
fails compile-time evaluation the same way without requiring
exception support.

### Step 4: Retire the `ParentRange::Aligned` concept

**Deferred to Phase 13.** Originally listed here but moved out for the
following reasons (rubber-duck review):
- It touches `LargeBuddyRange`, which Phase 12 explicitly keeps
  available for alternative configurations / embedders.
- It changes the public range concept (every pass-through range loses
  a static field) — a structural change, not a wiring change.
- It would split Phase 12 across an atomic-substitution commit and a
  separate concept-cleanup commit anyway; better to make that split
  explicit in the plan.

Phase 12 ends after Step 3 with the test suite green.

## Investigated and dropped: Retire `ParentRange::Aligned`

**Status: dropped on review.** Phase 13 was deferred from Phase 12 with
the intent of collapsing `LargeArenaRange::refill`'s two-path
conditional and (optionally) removing `ParentRange::Aligned` from the
range concept. Closer inspection of the existing code found the
conditional is load-bearing, not vestigial:

- **The two paths give different capabilities, not just different
  efficiencies.** The aligned-parent path serves caller sizes up to
  `(1 << MAX_SIZE_BITS) - 1`. The unaligned-parent path's
  `while (needed_size <= refill_size)` guard caps caller size at
  ~`REFILL_SIZE / 2`. Unifying on the unaligned strategy reduces
  capability for aligned-parent configs.

- **The aligned-parent path's carve shortcut is precise, not a perf
  optimisation.** It hands the caller's `size` bytes back directly
  and calls `add_range(refill + size, refill_size - size)` —
  passing `refill_size - size` (strictly less than `refill_size`)
  to `add_block`, which satisfies `add_block`'s
  `size < 2^MAX_SIZE_BITS` precondition even when
  `REFILL_SIZE_BITS == MAX_SIZE_BITS` (the `LargeObjectRange` config
  in `standard_range.h:52-56`). A unified "add the whole refill then
  recurse" path violates that precondition for the same config.

- **The proposed "fix" for the precondition has real cost.** Either
  cut `LocalCacheSizeBits` by 1 (half the per-local cache) or bump
  `MAX_SIZE_BITS` by 1 (double the local arena's internal state),
  for no behavioural win.

- **`LargeBuddyRange` would still consume `Aligned`** under the
  agreed-minimal (a)+(ii) scope, so the field's footprint in
  pass-through ranges doesn't shrink — defeating the only
  structural-cleanup motivation.

The Arena refactor (Phases 1–12) ends with Phase 12. No Phase 13.

## Risks

1. **LargeArenaRange behaviour differences.** The bin-tree allocator
   returns blocks with different internal fragmentation characteristics
   than the power-of-two buddy. Functionally, the caller always gets
   at least the requested size (power-of-two), so correctness is
   maintained. The arena may produce different carving patterns, but
   `alloc_range` always returns exactly the requested size.

2. **Overflow behaviour.** `LargeBuddyRange::dealloc_overflow` returns
   a single block of exactly `1 << MAX_SIZE_BITS`.
   `LargeArenaRange::parent_dealloc` forwards a single block of the
   consolidated size directly to the parent. The size can be any
   chunk multiple up to `2^MAX_SIZE_BITS`, not just power-of-two, but
   the parent (now itself a `LargeArenaRange` or pass-through layer)
   accepts arbitrary chunk-multiple sizes.

3. **`FixedRangeConfig` uses `StandardLocalState`.** The fixed-region
   configuration pushes memory directly into `GlobalR.dealloc_range`.
   This works with `LargeArenaRange` because `dealloc_range` has the
   same signature and contract.

4. **Pagemap metadata footprint.** `LargeArenaRange` uses up to
   three pagemap entries per free block (`largearenarange.h:12-17`)
   — one at the base, one at `base + UNIT_SIZE`, one at
   `base + 2*UNIT_SIZE`. `LargeBuddyRange`'s `BuddyChunkRep` only
   touched the base entry. Pagemap registration covers every
   `MIN_CHUNK_SIZE` stride for the full reserved address range
   (`pagemap.h:60-65`), so this is safe in the in-tree pipeline, but
   external embedders with custom Pagemap implementations should
   verify their pagemap entries cover the per-unit stride.

## Resolved during plan review

- `largearenarange.h` was missing `#include "empty_range.h"` for
  its `EmptyRange<>` default template parameter. Fixed pre-commit.
  (Rubber-duck finding #2.)
- The `conditional_t` huge-page path in `meta_protected_range.h` may
  not be instantiated on default builds. CI tests multiple PAL
  configurations. Risk acknowledged but no custom build added — the
  conditional branch is structurally identical to other
  `LargeArenaRange` uses and shares the same template. (Rubber-duck
  finding #1.)

## Out of scope

- Deleting `LargeBuddyRange` / `BuddyChunkRep` (keep for embedders).
- Modifying `Buddy<>` or `redblacktree.h`.
- Non-power-of-two `alloc_range` requests (deferred to front-end
  generalisation phase).
- Performance benchmarking (separate task).
- Any front-end changes.

# Phase 13: Uniform exp+mantissa sizeclass encoding

## Goal

Replace the large-sizeclass encoding `from_large_class(clz(size - 1))`
(which can only represent powers of two) with the same exp+mantissa
scheme small classes already use. After this phase, **every** size
class — small or large — is represented as
`bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(global_index)`,
where `global_index` is a single continuous index that runs across
small AND large. So a single uniform table accessor works across the
whole range, and the large table is the natural continuation of the
small one.

Specifically:
- Small class `sc ∈ [0, NUM_SMALL_SIZECLASSES)` corresponds to
  `from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(sc)`
  (unchanged from today's small encoding in `sizeclassstatic.h:62-64`).
- Large class `lc ∈ [0, NUM_LARGE_CLASSES)` corresponds to
  `from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(NUM_SMALL_SIZECLASSES + lc)`.

This is the "continuation of the small exp+mantissa", NOT a separate
exp+mantissa space starting at `MAX_SMALL_SIZECLASS_BITS`. Adjacent
classes step by `2^(E - INTERMEDIATE_BITS)` continuously, with no
jump at the small/large boundary.

No front-end behaviour changes yet: the front-end still calls
`large_size_to_chunk_size(size) = next_pow2(size)` and writes the
pagemap with the corresponding pow2-rounded sizeclass. The non-pow2
large sizeclasses are **populated in the table** (so the size /
slab_mask metadata is correct should any code path query them) but
are **unreachable** from `size_to_sizeclass_full` and
`large_size_to_chunk_size` until Phase 15.

That means:
- `size_to_sizeclass_full(non_pow2_large_size)` must continue to
  return the pow2-rounded sizeclass (the one whose
  `sizeclass_full_to_size` equals `next_pow2(size)`). Phase 15
  changes this to return the exp+mantissa-rounded sizeclass.
- `large_size_to_chunk_size(size)` continues to return
  `next_pow2(size)`. Phase 15 changes this to return
  `sizeclass_full_to_size(size_to_sizeclass_full(size))`.

The two functions stay in lock-step: the front-end's reservation
size and the pagemap-recorded sizeclass must agree, or `dealloc_chunk`
gets the wrong size. Phase 15 changes both together.

Phase 13 lays the encoding ground; Phase 14 adds the per-chunk
offset; Phase 15 flips the front-end.

## Why now

- The large-class table is currently indexed by leading-zero count,
  which has exactly one entry per power-of-two size — fundamentally
  pow2-only.
- Switching to exp+mantissa multiplies the large-class count by
  `1 << INTERMEDIATE_BITS = 4` (default), taking the default
  (`MAX_SMALL_SIZECLASS_BITS=16`, `address_bits=48`) from 32 large
  entries to 128.
- Once small and large share the same exp+mantissa scheme, the
  small/large tag bit in `sizeclass_t` becomes redundant: the two
  ranges can live in a single contiguous index space, and
  `is_small()` becomes `value < 1 + NUM_SMALL_SIZECLASSES` instead
  of `(value & TAG) != 0`. This drops one bit from
  `SIZECLASS_REP_SIZE`, undoing roughly half of the alignment
  cascade widening that Phase 13 would otherwise cause.

(For the default config, `NUM_SMALL_SIZECLASSES = 44`, defined as
`size_to_sizeclass_const(MAX_SMALL_SIZECLASS_SIZE) + 1` in
`sizeclassstatic.h:53-54`. All numeric examples below use the
symbol `NUM_SMALL_SIZECLASSES` for the count, with `44` as the
default-config concrete value.)

## Uniform (untagged) sizeclass encoding

Today `sizeclass_t::value` packs `[small: TAG | sc]` and
`[large: large_class]`, with a discriminator bit at position
`TAG_SIZECLASS_BITS`. Width consumed in the pagemap word =
`TAG_SIZECLASS_BITS + 1`.

After Phase 13 the discriminator is unnecessary because the small
and large ranges are both exp+mantissa-indexed and can sit in a
single contiguous index space.

### Index 0 is reserved as the unmapped sentinel

`value == 0` MUST remain the "default / unmapped" sentinel. An
unmapped pagemap entry reads as all-zero, and the size-lookup
machinery relies on `sizeclass == 0 ⇒ size == 0` to safely answer
`malloc_usable_size` / `remaining_bytes` queries on
not-an-allocation pointers without branching on validity.

So the uniform layout reserves index 0 and shifts everything up by
one:

```
0                                                  -> unmapped sentinel
[1,                 1 + NUM_SMALL_SIZECLASSES)      -> small  (sc index = value - 1)
[1 + NUM_SMALL_SIZECLASSES,
 1 + NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES)     -> large  (lc index = value - 1 - NUM_SMALL)
```

Width consumed = `next_pow2_bits_const(1 + NUM_SMALL_SIZECLASSES +
NUM_LARGE_CLASSES)`. For the default config:
`next_pow2_bits_const(1 + 44 + 128) = next_pow2_bits_const(173) = 8`.
The tagged scheme today (without the same uniform shift) would need
`max(6, 8) + 1 = 9`. **One bit saved**
(`SIZECLASS_REP_SIZE = 256`, `REMOTE_MIN_ALIGN = 512`).

### Table padding to avoid the subtract

The shift introduces a `- 1` on the size/metadata-lookup hot path
(`sizeclass_metadata[value - 1]`). We pay that on every dealloc.

Cheaper option: pad the table by one slot at index 0 (a dummy
"default" entry whose `size` is 0 and whose `slab_mask` is 0). Then
the lookup is `sizeclass_metadata[value]` with no subtract — and
querying the sentinel returns "size 0 / slab_mask 0" naturally,
which is the answer the existing API wants for unmapped pointers.

Cost: one wasted slot per table indexed by `sizeclass_t::raw()`.
Inspect each such table in `sizeclasstable.h`:
- `sizeclass_metadata` ModArray (`sizeclass_data_fast` /
  `sizeclass_data_slow`): pad slot 0 with zeros. Worth it (hot
  path).
- `sizeclass_compress_t` reverse lookups, if any: pad similarly.
- `ChunkSizeMetadata` (if indexed by raw value): inspect first.

The wasted-slot cost is `sizeof(slot) * num_tables` ≈ tens to
hundreds of bytes — negligible compared to the hot-path subtract
saved.

### `sizeclass_t` accessors

- `from_small_class(sc) { return {sc + 1}; }`
- `from_large_class(lc) { return {1 + NUM_SMALL_SIZECLASSES + lc}; }`
- `as_small() { return value - 1; }` (asserts `is_small()`).
- `as_large() { return value - 1 - NUM_SMALL_SIZECLASSES; }`
- `is_small() { return value < 1 + NUM_SMALL_SIZECLASSES &&
                          value != 0; }` — but in practice
  `is_small` is only meaningful for non-sentinel values; the
  default-sentinel case is filtered upstream. Simplification:
  `is_small() { return value - 1 < NUM_SMALL_SIZECLASSES; }`
  (works because for `value == 0`, `value - 1` underflows to
  SIZE_MAX which is ≥ NUM_SMALL_SIZECLASSES, so returns false —
  matching today's semantics where `sizeclass_t{}` is not "small").
- `is_default() { return value == 0; }` — unchanged.
- `raw()` returns the new shifted value — audit all callers (see
  Risks).

## RemoteAllocator alignment chain

Verified by inspection of `sizeclasstable.h:18-33` and
`metadata.h:16-45`:

```
SIZECLASS_BITS              = next_pow2_bits_const(
                                 1
                                 + NUM_SMALL_SIZECLASSES
                                 + NUM_LARGE_CLASSES)
                              (uniform encoding; no separate tag bit;
                                 index 0 reserved as the unmapped sentinel)
                              (= 8 after Phase 13 with
                                 INTERMEDIATE_BITS=2 large-classes)
  -> SIZECLASS_REP_SIZE      = 1 << SIZECLASS_BITS
       (= 256 after Phase 13)
       (used as ModArray length for sizeclass_metadata; the
        encoding occupies bits 0..SIZECLASS_BITS-1)
  -> REMOTE_MIN_ALIGN        = max(CACHELINE_SIZE, SIZECLASS_REP_SIZE) << 1
       (= 512 after Phase 13)
  -> REMOTE_BACKEND_MARKER   currently hard-coded `1 << 7`. After
       Phase 13, MUST be derived:
       `static constexpr address_t REMOTE_BACKEND_MARKER =
          SIZECLASS_REP_SIZE;`
       (= bit 8 after Phase 13).
  -> BACKEND_RESERVED_MASK   = (REMOTE_BACKEND_MARKER << 1) - 1
       (= 0x1FF after Phase 13 — low 9 bits reserved for backend.)
```

NOTE: the `+ 1` previously in `SIZECLASS_REP_SIZE = 1 <<
(TAG_SIZECLASS_BITS + 1)` is dropped — that `+ 1` was for the
small/large tag bit which uniform encoding eliminates. (Phase 13 also
renames the constant to `SIZECLASS_BITS` since the encoding no longer
carries a separate tag.) The static-assert in `metadata.h:64-67` that
enforces `REMOTE_BACKEND_MARKER == SIZECLASS_REP_SIZE` continues to
hold by construction.

**Concrete instances of `RemoteAllocator`** (all places where the
alignment cost lands):

- Inline `RemoteAllocator` inside `CoreAllocator` — one per
  allocator (`corealloc.h:155-159`), allocated from the meta range.
- `unused_remote` BSS global (`commonconfig.h:120`) — once, static.
- Stack-local instances in tests `sandbox.cc:162`,
  `domestication.cc` — test-only.

No other code constrains `RemoteAllocator` alignment.

**Cost of widening `REMOTE_MIN_ALIGN` (256 → 512 in this phase):** at
most ~256 bytes additional padding per CoreAllocator /
`unused_remote` — under 1 KB total, paid once, not per allocation.
Cheap.

## Cascading bit-layout changes elsewhere

`BACKEND_RESERVED_MASK` widens from `0xFF` (8 bits) to `0x1FF` (9
bits). All backend-side `PagemapRep` layouts that sit immediately
above the reserved range must shift up by
`new_marker_pos - old_marker_pos = log2(REMOTE_BACKEND_MARKER) - 7`.

Verified consumers of `BACKEND_RESERVED_MASK` / bits immediately
above bit 7:

- `largearenarange.h:42-50`: `RED_BIT_POS = 8`,
  `VARIANT_SHIFT = 9`, `LARGE_SIZE_SHIFT = 8`. Today these sit at
  bits 8/9-10/8. After Phase 13 they shift to bits 9/10-11/9. The
  `static_assert(Entry::is_backend_allowed_value(...))` at
  `largearenarange.h:64-66` catches any miss at compile time.
- `backend_helpers/largebuddyrange.h:40-46`: `BuddyChunkRep`
  `RED_BIT = 1 << 8`. Same shift required. (The plan previously
  said "`backend_helpers/buddy.h`" — corrected. Grep `RED_BIT` to
  confirm no other site.)

The plan does the shift in terms of an existing or new constant
(e.g. `BACKEND_LAYOUT_FIRST_FREE_BIT = log2(REMOTE_BACKEND_MARKER)
+ 1`) rather than hard-coding new bit numbers — so a future widening
auto-propagates.

## Changes

### `src/snmalloc/ds/sizeclasstable.h`

- `NUM_LARGE_CLASSES`: redefine as
  `(address_bits - MAX_SMALL_SIZECLASS_BITS) << INTERMEDIATE_BITS`
  so it tracks the exp+mantissa scheme. Update the comment.
- `SIZECLASS_BITS` (renamed from `TAG_SIZECLASS_BITS` — the encoding
  no longer carries a separate tag): redefine as
  `next_pow2_bits_const(1 + NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES)`.
  The `+ 1` reserves index 0 as the unmapped sentinel.
- `SIZECLASS_REP_SIZE`: redefine as `1 << SIZECLASS_BITS` (drop
  the `+ 1` that came from the tag bit).
- `sizeclass_t`: rewrite the encoding per "Uniform (untagged)
  sizeclass encoding" (small at `value = sc + 1`, large at
  `value = 1 + NUM_SMALL_SIZECLASSES + lc`, index 0 is the
  default sentinel). Drop the `TAG` constant. Convert all accessors
  (`from_small_class`, `from_large_class`, `as_small`, `as_large`,
  `is_small`, `index`). Default-construction (`sizeclass_t{}`) and
  `is_default()` keep their `value == 0` semantics.
- Audit callers of `sizeclass_t::raw()` — the raw value's meaning
  has changed (no tag bit, shifted by 1). Most callers use it as a
  `ModArray` index into `sizeclass_metadata`, which still works
  with the size-0 padding slot.
- `sizeclass_metadata` ModArray (`sizeclass_data_fast` and
  `sizeclass_data_slow`): pad slot 0 with zero-initialised entries
  (`size = 0`, `slab_mask = 0`, all other fields zero). A
  `static_assert` after construction enforces this.
- `sizeclass_metadata` constructor: rewrite as a single contiguous
  loop over `global_index ∈ [0, NUM_SMALL_SIZECLASSES +
  NUM_LARGE_CLASSES)`, writing slot `global_index + 1` with the
  size derived from
  `bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(global_index)`.
  Small-specific fields populate only for indices < NUM_SMALL;
  large-specific fields populate only for indices ≥ NUM_SMALL. The
  current split between small (lines ~190-225) and large
  (lines 226-238) loops collapses into one, eliminating the
  pow2-vs-exp+mantissa mismatch the old large loop had.
- `large_size_to_chunk_size(size)`: **semantics unchanged in this
  phase** — continues to return `bits::next_pow2(size)`. (Phase 15
  changes the body to
  `sizeclass_full_to_size(size_to_sizeclass_full(size))`.)
- `size_to_sizeclass_full(size)`: **semantics unchanged in this
  phase** for both branches — still pow2-rounded for large.
  *Implementation* of the large branch must change because the
  old body assumed `from_large_class` was a leading-zero-count
  mapping. Phase 13 redefines `from_large_class(lc)` to mean
  `from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(NUM_SMALL_SIZECLASSES + lc)`,
  so the new body uses `bits::to_exp_mant` (the literal inverse of
  `from_exp_mant`) directly:
  ```
  size_t pow2 = bits::next_pow2(size);
  size_t global =
    bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(pow2);
  return sizeclass_t::from_large_class(global - NUM_SMALL_SIZECLASSES);
  ```
  (Pre-Phase-13 there was a separate helper
  `large_size_to_chunk_sizeclass(size)` returning an `lc` index.
  Post-Phase-13, small and large share a single global exp+mantissa
  index space, so the `+NUM_SMALL_SIZECLASSES` / `-NUM_SMALL_SIZECLASSES`
  round-trip via `lc` cancels out and the helper is removed. The
  large branch of `size_to_sizeclass_full` inlines the
  `to_exp_mant` call directly.)
  (An earlier draft tried a manual
  `(next_pow2_bits - MIN_ALLOC_STEP_BITS) << INTERMEDIATE_BITS`
  formula. That is wrong — `to_exp_mant` does not simply place the
  exponent at MANTISSA_BITS; it uses a `b` offset that makes
  consecutive pow2 inputs differ by exactly `2^INTERMEDIATE_BITS`.
  Always use `to_exp_mant`, which is the literal inverse of the
  table-build helper.)
  Phase 15 replaces `pow2` with `size` (no `next_pow2`);
  `to_exp_mant` rounds non-pow2 sizes up to the next exp+mantissa
  step.
- The non-pow2 large slots in `sizeclass_metadata` are populated
  with correct size/slab_mask values in Phase 13 but are
  unreachable from `size_to_sizeclass_full` /
  `large_size_to_chunk_size` until Phase 15. This keeps Phase 13's
  end-to-end behaviour identical to today's.
- `slab_index`, `start_of_object`, `is_start_of_object`: continue
  to use `meta.slab_mask`. The metadata table builder sets
  `slab_mask = info.align - 1` for large (where
  `info.align = size & (~size + 1)`, the natural alignment from
  `arenabins.h:741`). For pow2 sizes, `info.align == size`,
  so `slab_mask = size - 1` — matching today's value. For
  non-pow2 sizes (table-populated but unreachable in Phase 13),
  `slab_mask = info.align - 1 < size - 1`. Phase 14 adds the
  per-chunk offset that lets recovery work for non-pow2 once
  Phase 15 lights them up.
- `round_size(size)` (lines 478-501): large branch left unchanged
  in this phase — still rounds to next pow2. Phase 15 updates it
  to match the new `large_size_to_chunk_size`.

### `src/snmalloc/mem/metadata.h`

- Line 45: change
  `static constexpr address_t REMOTE_BACKEND_MARKER = 1 << 7;` to
  `static constexpr address_t REMOTE_BACKEND_MARKER =
     SIZECLASS_REP_SIZE;`
  so the marker tracks the (now-untagged) sizeclass field width.
  Adjust the comment to point at `sizeclasstable.h` for the
  derivation.
- The existing static-asserts at `metadata.h:64-67` already
  enforce the invariant; verify they still pass.
- **Add a public layout constant** on `MetaEntryBase` so backend
  code can derive shift positions without violating the protected
  access of `REMOTE_BACKEND_MARKER`. Insert into the `public:`
  section (after line 113):
  ```cpp
  /**
   * Bit position of the first bit available to backend metadata
   * layouts above the reserved region. Used by
   * `largearenarange.h` and `largebuddyrange.h` to derive
   * RED_BIT_POS, VARIANT_SHIFT, and LARGE_SIZE_SHIFT.
   */
  static constexpr size_t BACKEND_LAYOUT_FIRST_FREE_BIT =
    bits::next_pow2_bits_const(REMOTE_BACKEND_MARKER) + 1;
  ```
  The `+1` reserves `REMOTE_BACKEND_MARKER`'s own bit (it lives at
  `next_pow2_bits_const(REMOTE_BACKEND_MARKER)`).

### `src/snmalloc/backend_helpers/largearenarange.h` and `src/snmalloc/backend_helpers/largebuddyrange.h`

- Replace hard-coded `RED_BIT_POS = 8`, `VARIANT_SHIFT = 9`,
  `LARGE_SIZE_SHIFT = 8` in `largearenarange.h` with
  derivations from the new public
  `MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT`:
  `RED_BIT_POS = MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT;`
  `LARGE_SIZE_SHIFT = MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT;`
  `VARIANT_SHIFT = MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT + 1;`
  (the `+1` reserves the RED bit).
- `largearenarange.h:64-66` `static_assert` continues to enforce
  no clash with reserved bits.
- `largebuddyrange.h:40-46`: `BuddyChunkRep::RED_BIT = 1 << 8`.
  Replace with `1 << MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT`.
  (The plan previously cited the wrong filename `buddy.h` —
  corrected.)
- Grep `RED_BIT` and `1 << 8` / `<< 8` across `backend_helpers/`
  to confirm no other site needs the same shift.

### `src/snmalloc/mem/corealloc.h`

- Line 1120-1121: replace
  `size_t size = bits::one_at_bit(entry_sizeclass);`
  with
  `size_t size = sizeclass_full_to_size(entry.get_sizeclass());`
  so the dealloc-large path reads the precise sizeclass-encoded
  size instead of reconstructing it from a leading-zero count. This
  is a no-op today (pow2 only); it makes Phase 15's behaviour change
  land at a single accessor.
- Grep `corealloc.h` for other `one_at_bit(` calls that derive
  large-allocation size from an `as_large()` value and convert all
  of them. (Known candidate: `corealloc.h:1576` — verify scope.)

### `src/snmalloc/global/globalalloc.h` and other consumers

- Grep for any other consumer of `as_large()` that interprets the
  value as a leading-zero count. Convert each to
  `sizeclass_full_to_size` or to the exp+mantissa accessor.
- Audit any code that uses `sizeclass_t::raw()` directly assuming
  the tag-bit-set-means-small invariant. The uniform encoding
  changes the meaning of `raw()`.
- Verified candidates from inspection: `globalalloc.h:145-220`
  (`remaining_bytes`, `index_in_object`, `external_pointer`) — these
  go through `start_of_object`/`slab_index`, so they pick up the
  change automatically via `slab_mask`.

## User-input size bounds (`MAX_LARGE_SIZECLASS_SIZE`)

Before Phase 13, the largest representable large allocation was
`1 << (address_bits - 1)` (half the address space, derived from
`from_large_class` being a leading-zero-count mapping). The
pre-existing bound check used `size > (size_t(1) << 63)` as a sloppy
upper limit and let anything below through. With the exp+mantissa
encoding, the largest representable size is the exact value of the
top large class — sizes between that and `2^address_bits` no longer
map to any valid sizeclass and must be rejected at the API boundary.

Define a derived constant alongside the encoding:

- `ENCODED_ADDRESS_BITS = bits::min(DefaultPal::address_bits, bits::BITS - 1)`.
  Caps the encoding range one bit below the native word width so that
  `from_exp_mant(NUM_SMALL + NUM_LARGE - 1) = 1 << ENCODED_ADDRESS_BITS`
  does not overflow `size_t` on 32-bit (`address_bits == BITS == 32`).
  On x86_64 (`address_bits = 48`) this is unchanged.
- `NUM_LARGE_CLASSES = (ENCODED_ADDRESS_BITS - MAX_SMALL_SIZECLASS_BITS)
  << INTERMEDIATE_BITS` (use `ENCODED_ADDRESS_BITS`, not `address_bits`).
- `MAX_LARGE_SIZECLASS_SIZE = from_exp_mant<INTERMEDIATE_BITS,
  MIN_ALLOC_STEP_BITS>(NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES - 1)`.
- Add `static_assert(MAX_LARGE_SIZECLASS_SIZE == bits::one_at_bit(
  ENCODED_ADDRESS_BITS))` to pin the encoding invariant (a strict
  nonzero check would not catch a wrong table-build mantissa offset).
- Add `static_assert(ENCODED_ADDRESS_BITS > MAX_SMALL_SIZECLASS_BITS)`
  so `NUM_LARGE_CLASSES > 0` is structural, not coincidental.

Fan out the new bound to every site that accepts a user-supplied size
and feeds it into the size→sizeclass lookup (these were either
unguarded or had the loose `> 2^63` check):

- `src/snmalloc/mem/corealloc.h` `alloc_not_small`: replace the
  `1 << 63` bound with `MAX_LARGE_SIZECLASS_SIZE`.
- `src/snmalloc/ds/sizeclasstable.h` `round_size`: same.
- `src/snmalloc/global/globalalloc.h` `check_size`: early-return via
  `snmalloc_check_client` when `size > MAX_LARGE_SIZECLASS_SIZE`.
- `src/snmalloc/override/rust.cc` `rust_realloc`: gate the
  equality-fast-path on both aligned sizes being
  `<= MAX_LARGE_SIZECLASS_SIZE`.

Add defensive `SNMALLOC_ASSERT`s in the large branch of
`size_to_sizeclass_full`: `size != 0`, `size <=
MAX_LARGE_SIZECLASS_SIZE`. These document the preconditions at the
function whose behaviour is constrained by them ("document coupling at
the point of breakage") and turn into noisy debug failures if a future
caller path skips the bound check.

## Test gates

1. **Build**: clean build of the default config passes. The
   `static_assert(Entry::is_backend_allowed_value(...))` checks at
   `largearenarange.h:64-66` catch any bit-layout mismatch.
2. **Full ctest suite**: all existing tests pass (no behaviour
   regression — front-end still issues pow2 large requests, so
   non-pow2 large sizeclasses exist in tables but are unreachable
   from the API).
3. **Arena unit tests** (`test_backend_arena`) continue to
   pass — they exercise the shifted RED/variant bits in the pagemap
   encoding.
4. **Extend `src/test/func/sizeclass/sizeclass.cc`** with a
   `uniform_large_sizeclasses` test case:
   - For every large `sizeclass_t` index `lc ∈ [0, NUM_LARGE_CLASSES)`,
     assert `sizeclass_full_to_size(from_large_class(lc))` is
     strictly increasing in `lc`.
   - For every pow2 size `S` in
     `[MAX_SMALL_SIZECLASS_SIZE * 2, 2^(address_bits - 1)]`, assert
     `sizeclass_full_to_size(size_to_sizeclass_full(S)) == S`
     (round-trip identity on pow2 — still holds in Phase 13
     because `size_to_sizeclass_full` for large still rounds to
     next pow2).
   - For every non-pow2 size `X` strictly between adjacent pow2
     `[P, 2P)`, assert
     `sizeclass_full_to_size(size_to_sizeclass_full(X)) == 2P`
     (still pow2-rounded in Phase 13 — Phase 15 changes this).
   - Sentinel sanity: `sizeclass_t{}.raw() == 0`;
     `sizeclass_t{}.is_default()` is true;
     `sizeclass_data_fast[0].size == 0`;
     `sizeclass_data_fast[0].slab_mask == 0`;
     `is_small(sizeclass_t{})` is false.
   - Encoding sanity: `is_small(from_small_class(0))` is true;
     `is_small(from_large_class(0))` is false; small range and
     large range are disjoint and adjacent in the value space.
5. **Extend `src/test/func/release-rounding/rounding.cc`** to
   exercise non-trivial pow2 large sizeclasses. Today this test
   covers small only. Add cases that exercise
   `start_of_object` / `is_start_of_object` for the pow2 large
   sizeclasses materialised end-to-end in Phase 13. (Phase 14
   extends to the per-chunk offset; Phase 15 to non-pow2.)
   (The plan previously cited `test/func/sizeclass/rounding.cc`,
   which does not exist — corrected.)

## Risks

1. **SIZECLASS_BITS widening cascades.** Caught by the existing
   `static_assert`s in `metadata.h:64-67` and
   `largearenarange.h:64-66`.
2. **Some embedder set REMOTE_MIN_ALIGN tighter than chain allows.**
   Would surface as a compile-error on the cacheline-vs-REP_SIZE
   max. Address only if it actually fires.
3. **Stale `as_large()` callers.** Mitigation: grep + convert ALL
   uses of `as_large()` in this phase. Phase 13 is not done until
   the leading-zero-count semantics are retired.
4. **Stale `raw()` callers assuming tag-bit semantics.** The
   uniform encoding changes `raw()`'s meaning (no tag bit, shifted
   by 1). Grep all callers and convert each to the appropriate
   accessor (`as_small`, `as_large`, `is_small`, or — if it's a
   `ModArray` index — leave alone, relying on the size-0 padding
   slot at index 0 to make the no-subtract lookup return the right
   sentinel values).
5. **Adding the size-0 padding slot at index 0.** The padding slot
   in `sizeclass_metadata` must have `size = 0` and `slab_mask = 0`
   (and any other fields zero-initialised) so that
   `sizeclass_full_to_size(sizeclass_t{}) == 0` and any accidental
   slab-mask arithmetic on the sentinel returns 0 / a no-op. Verify
   by reading every field in `sizeclass_data_fast` /
   `sizeclass_data_slow`. Add a static-assert that index 0 has
   `size == 0` after table init.

## Out of scope

- Per-chunk pagemap offset (Phase 14).
- Non-pow2 reservations (Phase 15).
- Changes to the small sizeclass encoding (other than dropping
  the tag bit).
- `round_size(size)` for large: still pow2 here; Phase 15 fixes.

# Phase 14: Per-chunk offset in `ras` + combined-indexed metadata

## Goal

Recover the start address of a large allocation from any interior
address, independent of allocation alignment. Stored as a per-chunk
slab-offset in the pagemap entry, packed alongside the sizeclass in
the `ras` (`remote_and_sizeclass`) word so that the same pagemap
word loaded for the sizeclass directly yields the index into the
metadata table that already has the offset-recovery delta
pre-baked. This unlocks Phase 15.

## Design summary

- **Layout**: offset bits sit in `ras` directly above the sizeclass
  bits and directly below the `REMOTE_BACKEND_MARKER`. Reading the
  same `ras` word the sizeclass-extract path already loads, masking
  with `COMBINED_MASK` yields the combined sizeclass+offset value
  ready to use as a table index — no extra load, no shift, no OR,
  no multiply. (Default config: 11 bits of combined index; the mask
  widens from `SIZECLASS_REP_SIZE - 1` to `COMBINED_REP_SIZE - 1`
  but is still a single `and`-with-imm.)
- **Metadata table**: `sizeclass_metadata.fast_` is widened from
  `SIZECLASS_REP_SIZE` rows to `COMBINED_REP_SIZE` rows
  (= `SIZECLASS_REP_SIZE << OFFSET_BITS`). Each row gains a
  pre-computed `offset_bytes` field equal to `offset * slab_size`
  for that sizeclass. Recovery is
  `alloc_start = (addr & ~slab_mask) - offset_bytes`.
- **Code**: `start_of_object` and friends take a *combined* index
  (`size_t`); the wrapper in `globalalloc.h` passes
  `entry.get_offset_and_sizeclass()`. No branches, no extra word loads
  on the fast path.
- **Backend**: in `alloc_chunk`, the small-and-pow2-large fast path
  (`slab_size >= size`) uses the existing `set_metaentry`. The
  non-pow2-large (multi-slab-tile) path writes a per-chunk
  `ras = encode(remote, sc, slab_index)` via `concretePagemap.set`.

## Why now

- Phase 15 introduces non-pow2 reservations. The existing
  `addr & ~slab_mask` answer is wrong for non-pow2 sizes/alignments.
- A per-chunk offset is the long-identified mechanism (PLAN.md
  intro). Phase 14 implements that mechanism with offset = 0
  semantics matching the existing pow2 path — so it lands without
  changing observable behaviour for today's allocations.
- Packing the offset into `ras` (not `meta`) at the time we land
  the field avoids a second `meta`-word load on
  `__malloc_start_pointer` and avoids a runtime multiply on every
  external_pointer query.

## Design

### Bit layout of `ras`

```
ras = [ RemoteAllocator* | BACKEND_MARKER | offset_bits | sizeclass_bits ]
                                                                  ↑
                                                              low bits
```

Bit positions (low to high):
- bits `[0, SIZECLASS_BITS)`: sizeclass — **unchanged** position.
- bits `[SIZECLASS_BITS, SIZECLASS_BITS + OFFSET_BITS)`: offset
  (frontend-owned, non-zero only for non-pow2 large in Phase 15+).
- bit `[SIZECLASS_BITS + OFFSET_BITS]`: `REMOTE_BACKEND_MARKER`
  (moves up by `OFFSET_BITS` positions).
- bits above: `RemoteAllocator*` payload.

Constants:

```cpp
// in sizeclasstable.h (alongside existing SIZECLASS_BITS):
constexpr size_t OFFSET_BITS = INTERMEDIATE_BITS + 1;
constexpr size_t COMBINED_BITS = SIZECLASS_BITS + OFFSET_BITS;
constexpr size_t COMBINED_REP_SIZE = bits::one_at_bit(COMBINED_BITS);
```

`REMOTE_BACKEND_MARKER` in `metadata.h` redefines from
`SIZECLASS_REP_SIZE` to `COMBINED_REP_SIZE`. `REMOTE_MIN_ALIGN`
follows: `max(CACHELINE_SIZE, COMBINED_REP_SIZE) << 1`. For the
default config (SIZECLASS_BITS=8, OFFSET_BITS=3): the marker moves
from bit 8 to bit 11, and `REMOTE_MIN_ALIGN` from 512 B to 4096 B.

Existing `MetaEntryBase::get_sizeclass()` must continue to return
pure sizeclass; with the marker moving up, masking by
`REMOTE_WITH_BACKEND_MARKER_ALIGN - 1` would now include the offset
bits. Define a dedicated `SIZECLASS_MASK = SIZECLASS_REP_SIZE - 1`
(unchanged in value from today's effective mask) and use it
explicitly in `get_sizeclass()`. The new `COMBINED_MASK =
COMBINED_REP_SIZE - 1` is what `get_offset_and_sizeclass()` uses.

### `OFFSET_BITS` derivation

With `INTERMEDIATE_BITS = M`, the worst-case non-pow2 large
sizeclass tiles into `2^(M+1)` slabs (e.g., a 7×slab_size class
with M=2: reserve rounds up to 8 slabs, max slab index = 7). So
`OFFSET_BITS = M + 1` gives `2^(M+1)` distinct values, exactly
enough for `[0, 2^(M+1))`. A `static_assert` on
`max_large_slab_index() < (1 << OFFSET_BITS)` (existing helper at
`sizeclasstable.h:273-285`) guards against any sizeclass-table
change.

### `meta` word stays simple

The `meta` word goes back to its pre-Phase-14 layout:

```
meta = [ SlabMetadata* | META_BOUNDARY_BIT ]
```

No offset bits. No `META_FRONTEND_RESERVED_MASK`. No alignas on
`FrontendSlabMetadata`. `get_slab_metadata()` masks just
`META_BOUNDARY_BIT`. This removes a load on the pointer-recovery
hot path (no `mov (%rdx),%rcx` to fish offset out of `meta`).

### Combined-indexed metadata table

`SizeClassTable::fast_` (`sizeclasstable.h:181`) widens:

```cpp
struct sizeclass_data_fast {
  size_t size;
  size_t slab_mask;
  size_t div_mult;
  size_t mod_zero_mult;
  size_t offset_bytes;   // NEW: precomputed (combined >> SIZECLASS_BITS) * slab_size
};

ModArray<COMBINED_REP_SIZE, sizeclass_data_fast> fast_{};
```

Memory: `COMBINED_REP_SIZE × sizeof(sizeclass_data_fast)`. With
SIZECLASS_BITS=8, OFFSET_BITS=3, sizeof=40: ~80 KB. Fits L2.
(`fast_small`'s today-1KB working set still fits L1 for the
small-only paths because those index `sc.raw()` directly, which
lands in the first `SIZECLASS_REP_SIZE` rows.)

`slow_` stays sc-indexed at `SIZECLASS_REP_SIZE` rows: it is only
read by slow paths that don't care about offset.

Table initialization fills every `(sc, offset)` cell:
- Other fields duplicate the `(sc, 0)` row.
- `offset_bytes = offset * sizeclass_full_to_slab_size(sc)`.

For `offset == 0` rows: `offset_bytes = 0`. The first
`SIZECLASS_REP_SIZE` rows of the new `fast_` are byte-identical to
today's table plus a trailing `offset_bytes = 0`.

**`fast()` overloads.** Keep the existing
`fast(sizeclass_t sc)` overload (`sizeclasstable.h:186-193`)
unchanged — it forwards to `fast_[sc.raw()]`, which hits the
offset = 0 row, identical to today's behaviour. Add a new
overload `fast(size_t combined)` that does `fast_[combined]`.
Call sites that have a sizeclass_t (most existing code) keep
calling `fast(sc)`; sites that have a combined index from the
pagemap call `fast(combined)`. No source change for the majority
of existing call sites.

### Accessors on `MetaEntryBase` / `FrontendMetaEntry`

Add to `MetaEntryBase`:

```cpp
// returns the value to use as an index into sizeclass_metadata.fast_
[[nodiscard]] SNMALLOC_FAST_PATH size_t get_offset_and_sizeclass() const {
  return static_cast<size_t>(remote_and_sizeclass) & COMBINED_MASK;
}
```

Keep `get_sizeclass()` returning a `sizeclass_t` (pure sizeclass,
low SIZECLASS_BITS only). Add an offset accessor for tests /
diagnostics:

```cpp
[[nodiscard]] SNMALLOC_FAST_PATH size_t get_offset() const {
  return (static_cast<size_t>(remote_and_sizeclass) >> SIZECLASS_BITS)
       & ((1 << OFFSET_BITS) - 1);
}
```

`encode(RemoteAllocator*, sizeclass_t)` gains an optional `offset`
parameter (defaults to 0 so existing callers compile):

```cpp
[[nodiscard]] static SNMALLOC_FAST_PATH uintptr_t
encode(RemoteAllocator* remote, sizeclass_t sizeclass, size_t offset = 0) {
  return pointer_offset(
    reinterpret_cast<uintptr_t>(remote),
    sizeclass.raw() | (offset << SIZECLASS_BITS));
}
```

Compile-time check: `offset < (1 << OFFSET_BITS)` (assert).

### `start_of_object` and friends

Refactor signatures to take a combined index (`size_t`) instead of
`(sizeclass_t, slab_offset)`. The recovery formula collapses to a
single subtract because `offset_bytes` is precomputed:

```cpp
SNMALLOC_FAST_PATH constexpr address_t
start_of_object(size_t combined, address_t addr) {
  auto meta = sizeclass_metadata.fast(combined);
  address_t alloc_start = (addr & ~meta.slab_mask) - meta.offset_bytes;
  size_t index = slab_index_via(meta, addr - alloc_start);
  return alloc_start + (index * meta.size);
}
```

`slab_index_via(meta, addr)` is the existing `slab_index` body
(`sizeclasstable.h:358-383`) refactored to take an already-loaded
`sizeclass_data_fast` instead of doing its own
`sizeclass_metadata.fast(sc)` lookup. All current behaviour is
preserved: the `offset = addr & meta.slab_mask` mask, the 64-bit
reciprocal-division (`(offset * meta.div_mult) >> DIV_MULT_SHIFT`),
and the 32-bit `offset / size` fallback for `sizeof(size_t) < 8`
platforms with the `size == 0` short-circuit. The original
`slab_index(sizeclass_t sc, address_t addr)` is kept as a
one-line wrapper that resolves `sc` to a row and forwards to
`slab_index_via` so call sites that don't already have the row
(e.g., `globalalloc.h:231,260` — which today pass
`entry.get_sizeclass()`) keep compiling unchanged.

`index_in_object`, `remaining_bytes`, `is_start_of_object` follow
the same shape, all taking `size_t combined`. Where callers have
only a `sizeclass_t` (e.g., for self-allocations they did
themselves), they pass `sc.raw()` directly — that selects the
offset=0 row, equivalent to today.

### Backend write in `alloc_chunk`

For the small / pow2-large (single-slab-tile) case (`slab_size >=
size`), keep `set_metaentry(addr, size, t)` where
`t = Entry(meta, encode(remote, sc))` — encoded with offset=0
implicitly.

For multi-slab-tile (Phase 15+, currently dormant):

```cpp
size_t slab_size = sizeclass_full_to_slab_size(sizeclass);
for (size_t chunk_offset = 0; chunk_offset < size;
     chunk_offset += MIN_CHUNK_SIZE)
{
  size_t slab_index = chunk_offset / slab_size;
  uintptr_t ras_i = Pagemap::Entry::encode(remote, sizeclass, slab_index);
  typename Pagemap::Entry t_i(meta, ras_i);
  Pagemap::concretePagemap.set(address_cast(p) + chunk_offset, t_i);
}
```

Only the `META_BOUNDARY_BIT` in `meta` is preserved across this
write: `MetaEntryBase::operator=` (`metadata.h:235-242`)
explicitly preserves the target's boundary bit and otherwise
overwrites both `meta` (modulo that bit) and `remote_and_sizeclass`
in full. Any prior backend-owned state in the old `ras` is gone
once the frontend claims the chunk (in `claim_for_backend`,
`metadata.h:313-317`, which resets `ras` to
`REMOTE_BACKEND_MARKER`), so the frontend's per-chunk write
overwriting `ras` from that pristine `REMOTE_BACKEND_MARKER`-only
state to the encoded `(remote, sc, offset)` is exactly the
expected ownership transition.

### Backend bits relocate automatically

`MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT` is derived from
`REMOTE_BACKEND_MARKER`; since the marker moves up by `OFFSET_BITS`,
the backend's `RED_BIT`, `VARIANT_SHIFT`, `LARGE_SIZE_SHIFT`
(`largearenarange.h:50-67`) auto-shift up by the same amount.
Verify the existing
`static_assert((MAX_SIZE_BITS - MIN_SIZE_BITS) + LARGE_SIZE_SHIFT
<= bits::BITS, ...)` still holds. For the default config:
- `MAX_SIZE_BITS = bits::BITS - 1 = 63`
- `MIN_CHUNK_BITS = 14`, so the large size field needs
  `MAX_SIZE_BITS - MIN_CHUNK_BITS = 49` bits.
- Pre-Phase-14: `BACKEND_LAYOUT_FIRST_FREE_BIT = SIZECLASS_BITS = 8`,
  so `LARGE_SIZE_SHIFT ≈ 9` → `49 + 9 = 58 ≤ 64`. ✓
- Phase 14: `BACKEND_LAYOUT_FIRST_FREE_BIT = SIZECLASS_BITS +
  OFFSET_BITS = 11`, so `LARGE_SIZE_SHIFT ≈ 12` → `49 + 12 = 61 ≤
  64`. ✓ (Three bits of headroom remain; OFFSET_BITS = 4 — the
  `INTERMEDIATE_BITS = 3` config — would still pass.)

### Pre-existing pagemap bug (still fixed in prep commit `1144eab4`)

Same as before: `FlatPagemap::get_mut<true>` double-base-adjust on
`PALNoAlloc`. Fix unrelated to Phase 14 layout choice.

### Consumers that MUST be updated in Phase 14

Phase 14 is incomplete until every caller of the
sizeclass-table-only `start_of_object` / `is_start_of_object` /
`remaining_bytes` on a **user-supplied** potentially-large pointer
is offset-aware.

The offset support is pushed into the inner helpers in
`sizeclasstable.h` themselves: `start_of_object`, `index_in_object`,
`remaining_bytes`, and `is_start_of_object` take a mandatory
*combined* `size_t` index parameter (sizeclass + offset packed into
the low `COMBINED_BITS` of `ras`). Callers must explicitly pass
either `sc.raw()` (when local context proves the address is in the
allocation's first slab — offset implicitly 0) or
`entry.get_offset_and_sizeclass()` (from the address's pagemap entry).
Removing default arguments forces every call site to make a
deliberate choice and prevents a future Phase 15 caller from
accidentally inheriting offset = 0 when it should consult the
pagemap.

Inside each helper, the formula uses a single
`sizeclass_metadata.start(combined)` lookup — the `start_` table is
indexed by `COMBINED_REP_SIZE` rows so the combined index lands
directly in a precomputed row. `offset_bytes` collapses to 0 for the
offset = 0 rows, which today are the only rows reached from
front-end allocation paths. This keeps `globalalloc.h` and
`corealloc.h` branch-free at the call site and avoids duplicating
the slab-mask / slab-size arithmetic across files.

- `globalalloc.h:138-144` (`remaining_bytes`): reads the metaentry,
  then unconditionally calls
  `snmalloc::remaining_bytes(entry.get_offset_and_sizeclass(), p)`.
  No small/large dispatch.
- `globalalloc.h:158-167` (`index_in_object`): same pattern.
- `bounds_checks.h:101` memcpy gate: calls `remaining_bytes(...)`.
  Moves to the offset-aware version transitively via the inner-helper
  rewrite — no source change here, and no extra branch on the
  bounds-check fast path.

Audit of all `is_start_of_object` call sites (verified against the
post-Phase-13 tree via `grep -rn is_start_of_object src/snmalloc`):

| File:line | Sizeclass source | Pointer source | Action |
|---|---|---|---|
| `corealloc.h:41` (`DefaultConts::success`) | requested-size→sc | allocator-output base | **Keep** — slab-mask check on the allocator's own freshly-returned base is tight enough; pass `sc.raw()`. |
| `override/new.cc:40` (`handler::Base::success`) | requested-size→sc | allocator-output base | **Keep** — same rationale as above; pass `sc.raw()`. |
| `corealloc.h:536` (`dealloc_local_object_meta`) | `entry.get_sizeclass()` | **user input** | **Update** — pass `entry.get_offset_and_sizeclass()`; the helper folds the offset check internally. |
| `corealloc.h:1084` (`dealloc_local_object`) | `entry.get_sizeclass()` | **user input** | **Update** — same: pass `entry.get_offset_and_sizeclass()`. |
| `corealloc.h:1258` | `from_small_class(...)` | small allocation | **Keep** — small-only path; pass `sc.raw()`. |
| `corealloc.h:1438` | `from_small_class(...)` | small allocation | **Keep** — small-only path; pass `sc.raw()`. |

Additionally, `slab_index` itself has two call sites outside the
`start_of_object` family:

- `globalalloc.h:231` (`remaining_bytes` wrapper, large-class
  arm): calls `slab_index(entry.get_sizeclass(), address_cast(p))`.
- `globalalloc.h:260` (`index_in_object` wrapper, large-class
  arm): same shape.

After the helper-signature refactor these two wrappers fold into
the new `start_of_object(combined, addr)` path entirely (the
combined-index version of `remaining_bytes`/`index_in_object`
calls `start_of_object` internally, which itself dispatches to
`slab_index_via`). Neither wrapper calls `slab_index` directly
post-refactor.

The "Keep" rows on allocator-output base pointers are safe because
the allocator itself always returns the allocation base, which by
construction is slab-aligned (`addr & info.slab_mask == 0`) *and*
allocation-start (offset == 0 in pagemap, so combined ==
`sc.raw()`). The old `is_start_of_object(sc, addr)` test reduces to
`(addr & info.slab_mask) == 0`, which holds for all such bases
both today and after Phase 15.

The dealloc-API consumers (rows 3 and 4) get the offset folded
inside the combined index because for non-pow2 large in Phase 15
every natural-alignment slab boundary *inside* the allocation would
satisfy the old `slab_mask`-only check; the precomputed
`offset_bytes` in the combined row distinguishes the actual
allocation base. These call sites remain gated by
`snmalloc_check_client(mitigations(sanity_checks), ...)`, so the
additional comparison is dead in release/non-checked builds.

`slab_index` for large: irrelevant — large allocations are a single
"object" of size `sizeclass_full_to_size(sc)`, not a slab of
multiple. The refactored `start_of_object` uses
`addr - alloc_start` (offset within the *allocation*, not the slab)
as the dividend, which is 0 for any in-range large pointer.

### Backend changes

- `backend.h:131-156` (alloc_chunk small/large dispatch): replace
  the single `set_metaentry(p, size, t)` call with the small/large
  dispatch described in "Backend write in `alloc_chunk`" above.
  Phase 14 keeps `alloc_chunk`'s `bits::is_pow2(size)` assertion
  (Phase 15 relaxes it). This is fine: today only pow2 large
  allocations reach this site, so `slab_size == size` and offset
  is always 0; the entries written by the new large path are
  bit-identical to the entries written by the old uniform path.
- `backend.h:172-196` (dealloc_chunk): constructs
  `Entry t(nullptr, 0)`, calls `claim_for_backend()`, then
  `set_metaentry(p, size, t)`. The `Entry(nullptr, 0)`
  constructor's `ras = 0` clears both the sizeclass and offset
  fields. `claim_for_backend()` (`metadata.h:313-317`) sets `ras`
  to `REMOTE_BACKEND_MARKER` and only the boundary bit on `meta` is
  preserved. The subsequent `set_metaentry` writes the
  cleared-ras `Entry` to every pagemap cell in the range. No
  further change is needed: the offset is meaningful only while
  the chunk is owned by the frontend.

### `RemoteAllocator` alignment

`REMOTE_MIN_ALIGN` bumps from 512 B to 4096 B (default config:
`COMBINED_REP_SIZE = 2048`, doubled for the marker, so
`max(CACHELINE, 2048) << 1 = 4096`).

`RemoteAllocator` (`remoteallocator.h:292-310`) gets its alignment
from its `FreeListMPSCQ<key_global>` member (`freelist_queue.h`),
which is declared `alignas(REMOTE_MIN_ALIGN)`. So bumping
`REMOTE_MIN_ALIGN` automatically widens `alignof(RemoteAllocator)`
to 4096 with no source change to `RemoteAllocator` itself.

Verifications (do during step 2):

1. `sizeof(RemoteAllocator)` does not blow up. The structure is a
   small fixed-size queue head plus padding; rounding up to a
   4096-B alignment unit only consumes extra padding in
   surrounding containers (allocators, pool slots), not inside
   `RemoteAllocator`.
2. `CommonConfig::unused_remote` (`commonconfig.h:119-120`) — a
   static `RemoteAllocator` — inherits the new alignment from
   `RemoteAllocator`'s natural alignof. Confirm it still compiles
   and the linker honours the alignment (compilers do; some older
   linkers cap `.bss` alignment, but 4096 is the page size, so it
   is universally supported).
3. Per-allocator-pool storage: the pool allocates `Allocator<Config>`
   instances; each `Allocator` contains a `RemoteAllocator`
   (transitively), and the pool's metadata-allocation path is
   already aligned to `alignof(Allocator)` via the backend's
   metadata allocator. Confirm via inspection that
   `Pool<Allocator>::acquire` honours `alignof(Allocator)` after
   the bump.
4. `unused_remote_address`-style runtime checks (any assertion that
   `(uintptr_t)remote & (REMOTE_MIN_ALIGN - 1) == 0`) — grep for
   `REMOTE_MIN_ALIGN` to find them and confirm they pass with the
   bumped value.

## Implementation steps

Each step must produce a testable result before moving to the next.
Steps are ordered so that earlier steps' tests don't depend on
later steps' code.

### Step 0: Revert the current (meta-based) Phase-14 implementation

The current working tree carries a partial, meta-word-based
Phase 14 (`META_OFFSET_BITS`, `META_OFFSET_SHIFT`,
`META_OFFSET_MASK`, `META_FRONTEND_RESERVED_MASK`, `set_offset` /
`get_offset` on `FrontendMetaEntry`, `alignas(...)` on
`FrontendSlabMetadata`, branchless three-parameter
`start_of_object(sc, addr, slab_offset)` / `index_in_object` /
`remaining_bytes` / `is_start_of_object`, three-parameter wrapper
calls in `globalalloc.h` / `corealloc.h` / `override/new.cc` /
`test/func/release-rounding/rounding.cc`, and the small/large
dispatch in `backend.h::alloc_chunk`). The new design replaces all
of this. Revert these files to the pre-Phase-14 head (commit
`1144eab4`), keeping only:
- The new test scaffolding in `src/test/func/memory/memory.cc`
  (`test_large_alloc_pointer_recovery`) and
  `src/test/func/large_offset/large_offset.cc` — to be updated for
  the combined-index API in steps 4 and 6.

**Gate**: clean build, full ctest suite passes (this is the
pre-Phase-14 head with two test additions that will be updated
later — the additions either compile and pass or are temporarily
gated out until step 4).

### Step 1: Constants + table widening (no behaviour change)

> **Implementation note**: the as-shipped design splits the metadata
> table into `start_` / `align_` / `slab_` rather than widening the
> single `fast_` table described below. See Step 7 Outcome for the
> rationale (perf gate). The constants and `(sc, offset)`
> initialisation described here apply to `start_`.

Changes:
- `sizeclasstable.h`: add `OFFSET_BITS`, `COMBINED_BITS`,
  `COMBINED_REP_SIZE`. Add `offset_bytes` column to
  `sizeclass_data_fast`. Widen `fast_` to `COMBINED_REP_SIZE`.
  Initialise every `(sc, offset)` cell — non-zero rows duplicate
  the `(sc, 0)` row's fields except `offset_bytes = offset *
  slab_size`. Add new overload `fast(size_t combined)`. Keep
  `fast(sizeclass_t)` unchanged.
- Add `static_assert(max_large_slab_index() < (1 <<
  OFFSET_BITS))`.

**Gate**: clean build. All existing tests still pass — nothing
reads `fast(combined)` yet, and the offset = 0 rows of the widened
table are byte-identical to today's rows for callers that index
via `sc.raw()` (whose value lies in `[0, SIZECLASS_REP_SIZE)`).

### Step 1.5: Per-word backend-reserved mask + lower BIN/RANGE bit positions

Motivation: today's `BACKEND_RESERVED_MASK = (REMOTE_BACKEND_MARKER
<< 1) - 1` applies symmetrically to both `meta` (Word::One) and
`ras` (Word::Two). That is overly conservative: in backend mode,
the only invariants are
- `meta` must preserve `META_BOUNDARY_BIT` (bit 0) across the
  ownership transition (frontend reads it to detect PAL
  boundaries), and
- `ras` must keep `REMOTE_BACKEND_MARKER` set while backend-owns
  (frontend reads bit MARKER to detect ownership).

Everything else on both words is free for the backend. Today's
unified mask forces `RED + VARIANT` (which live on `meta`) up to
`BACKEND_LAYOUT_FIRST_FREE_BIT`, i.e., just above the marker
position. After Step 2 moves the marker from bit 8 to bit 11,
those positions become bits 12, 13, 14 — and bit 14 collides with
the `MIN_CHUNK_BITS = 14` unit-address packing in the backend's
buddy-tree pointer storage, tripping the
`BIN_META_MASK < UNIT_SIZE` assertion in
`largearenarange.h:72`.

Changes:

- `metadata.h`:
  - Replace `BACKEND_RESERVED_MASK` with two per-word constants:
    - `BACKEND_RESERVED_MASK_WORD_ONE = META_BOUNDARY_BIT`
    - `BACKEND_RESERVED_MASK_WORD_TWO = (REMOTE_BACKEND_MARKER <<
      1) - 1` (the old value — unchanged in behaviour for `ras`).
  - Make `is_backend_allowed_value(Word w, uintptr_t v)` use the
    right mask per `w`.
  - Change `BackendStateWordRef` to carry the relevant mask (or
    its `Word` identity) so its `get()` and `operator=` use the
    correct per-word mask. The simplest mechanical change is to
    pass the mask into the `BackendStateWordRef` constructor and
    store it as a member; `get_backend_word(Word w)` selects the
    right mask at the call site.
- `largearenarange.h`:
  - Move `RED_BIT_POS` and `VARIANT_SHIFT` down to start at bit 1
    (just above `META_BOUNDARY_BIT`). `RED_BIT_POS = 1`,
    `VARIANT_SHIFT = 2`. `BIN_META_MASK = (1<<1) | (3<<2) = 14`.
  - Move `LARGE_SIZE_SHIFT` to bit 1 too (it stores the large
    chunk count on `Word::One` of unit 2 — same word, same
    relaxed reservation).
  - The `is_backend_allowed_value(Word::Two, RED_BIT)` assert at
    line 75 — RANGE_META_MASK applied to Word::Two of unit 1
    stores bit 1 in the left-child mask region. Bit 1 ≠ bit
    MARKER (= 11 in new layout or 8 today), so the marker bit
    is not disturbed. Verify the per-word mask check passes for
    Word::Two with bit 1 (it should: the new Word::Two mask still
    forbids the backend from writing the marker bit, but bit 1 is
    not the marker).
  - **Note**: After this step, `Word::Two`'s relaxed mask still
    requires the backend not to disturb the marker. Today's
    Word::Two mask was bits 0..MARKER, which forbade *any* bits
    in that range. The relaxed mask forbids only the marker bit
    itself. So the backend can now write low bits of `ras`
    (sizeclass/offset positions) — those are zero in backend mode
    (cleared by `claim_for_backend()`) and overwritten on
    ownership transition, so no real change.

**Gate**: clean build. Full ctest suite passes. The marker has
NOT moved yet (still at SIZECLASS_REP_SIZE), so the layout
change is invisible to allocation behaviour; only the
relaxation of asserts and the lowered bit positions for
RED/VARIANT/LARGE_SIZE_SHIFT differ. Run a focused build to
re-trigger the static_asserts in `largearenarange.h` and
confirm they all pass.

### Step 2: Marker move + ras encoding (no offset writers yet)

Changes:
- `metadata.h`: change `REMOTE_BACKEND_MARKER` from
  `SIZECLASS_REP_SIZE` to `COMBINED_REP_SIZE`. Define
  `SIZECLASS_MASK = SIZECLASS_REP_SIZE - 1` and
  `COMBINED_MASK = COMBINED_REP_SIZE - 1`. Update
  `get_sizeclass()` to mask with `SIZECLASS_MASK` explicitly.
  Add `get_offset_and_sizeclass()`. Extend `encode(remote, sc)` to
  `encode(remote, sc, size_t offset = 0)`; assert
  `offset < (1 << OFFSET_BITS)`.
- Verify alignment chain (RemoteAllocator alignment section
  above). If any check fails, fix before continuing.

**Gate**: clean build (the size-budget `static_assert` in
`largearenarange.h` is the compile-time guard for the marker
shift). All existing tests still pass — every `ras` write still
encodes with `offset = 0` (the new default arg), so every
combined value still equals `sc.raw()`.

### Step 3: Refactor `slab_index` into `slab_index_via`

Changes:
- `sizeclasstable.h`: introduce
  `slab_index_via(sizeclass_data_fast const& meta, address_t
  addr)` carrying the existing body (mask, 64-bit reciprocal-mul,
  32-bit fallback). Make `slab_index(sizeclass_t, addr)` a
  one-line wrapper over `slab_index_via`.

**Gate**: clean build. All existing tests still pass — pure
refactor.

### Step 4: Switch helpers to combined index

Changes:
- `sizeclasstable.h`: change `start_of_object`, `index_in_object`,
  `remaining_bytes`, `is_start_of_object` to take a single
  `size_t combined` parameter. Body uses `fast(combined)` and
  reads `meta.offset_bytes`; recovery is
  `(addr & ~slab_mask) - offset_bytes`. Mark `index_in_object`
  and `remaining_bytes` `SNMALLOC_FAST_PATH`.
- Update all call sites per the audit table above:
  - `globalalloc.h:138-144`, `:158-167`: pass
    `entry.get_offset_and_sizeclass()`.
  - `globalalloc.h:231,260` (`slab_index` direct callers): fold
    into the new `start_of_object`-based path (large arm of
    `remaining_bytes` / `index_in_object` now goes through
    `start_of_object(combined, addr)` and no longer calls
    `slab_index` directly).
  - `corealloc.h:41`, `override/new.cc:40`,
    `corealloc.h:1258`, `corealloc.h:1438`: pass
    `sc.raw()`.
  - `corealloc.h:536`, `corealloc.h:1084`: pass
    `entry.get_offset_and_sizeclass()`.
  - `src/test/func/release-rounding/rounding.cc`: pass
    `sc.raw()`.

**Gate**: clean build. Full ctest suite passes. All combined
values are still `sc.raw()` because no offset writer exists yet
(step 5).

### Step 5: Backend `alloc_chunk` writes per-chunk offset

Changes:
- `backend.h::alloc_chunk` (~lines 131-156 today): keep the
  `slab_size >= size` fast path using `set_metaentry` (offset = 0
  for every chunk). Add the multi-slab-tile branch (currently
  dormant — only reached after Phase 15) that loops over chunks
  and writes `ras_i = encode(remote, sc, slab_index)` via
  `concretePagemap.set`.

**Gate**: clean build. Full ctest suite passes — every Phase-14
allocation today is single-slab-tile, so the new branch is
dormant.

### Step 6: Targeted test for the per-chunk offset write

Add `src/test/func/large_offset/large_offset.cc` per the
"Targeted test" subsection of "Final acceptance gates" below;
this exercises the multi-slab-tile write path by calling
`Config::Backend::alloc_chunk` directly with a synthetic
non-pow2 sizeclass.

**Gate**: the new test passes; full suite still passes.

### Step 7: Performance gate

Run `perf-external_pointer` and `perf-large_alloc` on
`build-rel-base` vs `build-rel-p14`, 10× medians, per
`.github/skills/building_and_testing.md`. Compare against the
baseline-noise band measured pre-Phase-14.

**Gate**: `perf-external_pointer` and `perf-large_alloc` within
noise of baseline (no statistically significant regression).
Disassemble `__malloc_start_pointer` to confirm: one `ras`-word
load, mask + table lookup with `offset_bytes`, no `meta`-word
load on the recovery path, no `imul`.

**Outcome**: gate met after splitting the sizeclass metadata table
into three by purpose, plus an offset-aware branch in
`start_of_object`.

1. Three tables, replacing the previous `fast_`/`slow_` pair:
   - `start_` (4 × size_t = 32 B/row, indexed by
     `offset_and_sizeclass_t`): `size`, `slab_mask`, `div_mult`,
     `offset_bytes`. Power-of-two stride keeps the
     `__malloc_start_pointer` index calc to a single `ubfiz #5`,
     matching the baseline shape.
   - `align_` (2 × size_t = 16 B/row, indexed by `sizeclass_t`):
     `slab_mask` (duplicated), `mod_zero_mult`.
     `is_start_of_object` reads both fields from one row instead of
     straddling two tables; cold in `-fast` builds.
   - `slab_` (2 × uint16 = 4 B/row, indexed by `sizeclass_t`):
     `capacity`, `waking`. Slab init thresholds; cold.
2. `start_of_object` branches on `osc.offset() == 0` (testable from
   bits already loaded in the `ras` word, before any metadata-table
   access). The common arm skips the `offset_bytes` field load and
   the offset-shift arithmetic; the slow arm handles non-pow2 large
   interior chunks. Branch fully predicted on small-allocation
   workloads.

Without these refinements `perf-external_pointer-fast` regressed by
~24% (median ~360 ms vs baseline ~290 ms). With them, median
~290 ms — within noise of baseline. `perf-singlethread-check`
(exercises `is_start_of_object` on every dealloc) is also within
noise: identical 9-instruction codegen, now reading from the
narrower `align_` rows (4-per-cache-line vs the baseline's
2-per-cache-line).

## Final acceptance gates

1. **Build**: clean build passes. The new `static_assert` in
   `sizeclasstable.h` (max large slab index < `1 << OFFSET_BITS`)
   guards the OFFSET_BITS choice. The size-budget assert in
   `largearenarange.h` (`(MAX_SIZE_BITS - MIN_SIZE_BITS) +
   LARGE_SIZE_SHIFT <= bits::BITS`) guards the upward shift of
   backend bits.
2. **Full ctest suite**: all existing tests pass. Front-end still
   issues pow2 large requests, so for every materialised large
   allocation `info.align == size` and offset is always 0 — the
   combined index for every entry equals `sc.raw()`, indexing the
   offset = 0 row of `start_`, which is bit-identical to the
   pre-split row layout.
3. **`src/test/func/release-rounding/rounding.cc`** continues to
   pass — small path unchanged; large path uses offset = 0 always.
4. **Extend `src/test/func/memory/memory.cc`** with a
   `large_alloc_pointer_recovery` test (public-API path):
   - Allocate several large sizes via the public API. For each
     allocation `p` of requested size `S_req`, the actual reservation
     in Phase 14 is `S_res = bits::next_pow2(S_req)` (front-end is
     still pow2-only). For each:
     - For every chunk offset `k * MIN_CHUNK_SIZE` for
       `k = 0..S_res/MIN_CHUNK_SIZE - 1`, assert
       `remaining_bytes(p + k * MIN_CHUNK_SIZE) == S_res - k *
       MIN_CHUNK_SIZE`. The public `remaining_bytes` routes through
       `index_in_object<Config>` and therefore consumes the
       combined index from the pagemap entry; any miscalculation
       in `offset_bytes` would produce a wrong residual.
     - For every interior address `q = p + j` with `j ∈ {0, 1,
       S_res/2, S_res-1}`, assert
       `address_cast(snmalloc::external_pointer<snmalloc::Start>(
       reinterpret_cast<void*>(q))) == p` (offset-aware public API,
       which uses `index_in_object` → pagemap entry → combined
       index → `offset_bytes` subtraction).
5. **New test or extension** to exercise the non-zero offset write
   path directly (Phase 14 is otherwise un-tested with non-zero
   offsets, because the front-end is still pow2-only). Path:
   - Add a test in `src/test/func/large_offset/large_offset.cc`
     that calls `Config::Backend::alloc_chunk` directly. The test
     obtains a `LocalState&` from a constructed `snmalloc::Allocator`
     via its public `get_backend_local_state()` accessor
     (`corealloc.h:378`).
   - Sizeclass selection: pick a non-pow2 large `sc` via
     `sizeclass_t::from_raw(raw)` for a raw index whose
     `sizeclass_metadata` entry has non-pow2 `size` but a smaller
     `slab_mask` (= `info.align - 1`). These entries are
     table-populated in Phase 13 and unreachable from the public
     allocation API, but they are usable here because
     `alloc_chunk`'s sizeclass argument is only consulted in the
     pagemap write loop (which is what we want to exercise).
   - Size argument: `alloc_chunk` asserts `bits::is_pow2(size)`
     (`backend.h:95`). Pass `bits::next_pow2(sizeclass_full_to_size(sc))`
     so the assert holds. This is *larger* than the sizeclass's
     `size`, but the pagemap write loop iterates over the
     passed-in pow2 region, computing per-chunk offsets via
     `chunk_offset / slab_size` (where `slab_size =
     sizeclass_full_to_slab_size(sc) < size`). Non-zero offsets
     are therefore written for all chunks past the first slab.
   - `ras` argument: construct via
     `Config::PagemapEntry::encode(nullptr, sc)` (see
     `metadata.h:211-219`), which matches how the front end builds
     `ras` in `corealloc.h:723-728`. Avoids hard-coding the bit
     layout in the test. The per-chunk `alloc_chunk` loop re-encodes
     `ras` per chunk with the appropriate offset.
   - Capability handling: `alloc_chunk` returns
     `capptr::Chunk<void>` (`backend.h:89-93`). Use
     `address_cast(chunk)` for pagemap/start-of-object checks.
     Before calling `dealloc_chunk`, convert via
     `capptr_chunk_is_alloc(capptr_to_user_address_control(chunk))`
     to get the `capptr::Alloc<void>` it expects.
   - Verify:
     - For each chunk in the pow2 region:
       - `Config::Backend::get_metaentry(address_cast(chunk) +
         k * MIN_CHUNK_SIZE).get_offset_and_sizeclass()` decomposes
         as `sc.raw() | (expected_slab_idx << SIZECLASS_BITS)`
         where `expected_slab_idx = (k * MIN_CHUNK_SIZE) /
         sizeclass_full_to_slab_size(sc)`.
       - The same entry's `get_sizeclass()` (low-bits-only mask)
         still returns `sc`.
     - `address_cast(snmalloc::external_pointer<snmalloc::Start,
       Config>(reinterpret_cast<void*>(address_cast(chunk) +
       interior_offset))) == address_cast(chunk)` for a sample of
       interior addresses spanning multiple slabs (one address
       per slab boundary, plus mid-slab). `external_pointer<Start>`
       routes through `index_in_object<Config>` which consults the
       pagemap entry's combined index and the precomputed
       `offset_bytes`.
   - Then `Config::Backend::dealloc_chunk` with the *same* pow2
     size, and verify all chunks' offsets are cleared
     (`get_offset_and_sizeclass() == 0`) — the dealloc path
     constructs `Entry(nullptr, 0)`, whose `ras = 0` clears the
     combined-index field entirely.
6. **Backend-bit-preservation test**: with the synthetic-sizeclass
   test from (5) in place, allocate a region whose pow2 size spans
   a PAL-allocation boundary so the backend has set bits in `meta`
   and (after the move) the upper bits of `ras`. Verify the
   boundary bit and other backend-owned bits survive the per-chunk
   frontend write loop. (This is implicitly already covered by the
   existing ctest suite — every multi-PAL-chunk allocation today
   already does this, just without per-chunk offset writes — but
   the explicit large_offset test makes the guarantee local.)

## Risks

1. **`RemoteAllocator` alignment bump.** `REMOTE_MIN_ALIGN` rises
   from 512 B to 4096 B. Mitigation: verify the structure size and
   pool-storage alignment annotations before changing the
   constant; bump pool alignment if needed. Caught at runtime by
   the existing `snmalloc_check_client` assertions on `ras`
   pointer-bit-extraction, and by misaligned-pointer crashes in
   message-passing.
2. **Backend bit budget.** `MAX_SIZE_BITS - MIN_SIZE_BITS +
   LARGE_SIZE_SHIFT <= bits::BITS` (the assert in
   `largearenarange.h:68-70`). With `LARGE_SIZE_SHIFT`
   auto-shifted up by `OFFSET_BITS`, default config goes from ~44
   to ~47 bits used, still ≤ 64. The assert is the gate.
3. **Combined-index table size.** The combined-index `start_` table
   holds `1 << OFFSET_BITS` × `sizeof(sizeclass_data_start)` more
   rows than the original sizeclass-indexed table. Default: 8 × 32 B
   × `SIZECLASS_REP_SIZE` ≈ 64 KB. Acceptable for an L2-resident
   metadata table; if `INTERMEDIATE_BITS` is raised to 3
   (`OFFSET_BITS = 4`) the table grows to ~128 KB — also
   acceptable.
4. **Encode-time offset overflow.** `encode(remote, sc, offset)`
   asserts `offset < (1 << OFFSET_BITS)`. The `alloc_chunk` loop
   bounds `slab_index` to `size / slab_size`, which is bounded by
   the worst-case slab count for the chosen sizeclass — the same
   bound the `static_assert` on `OFFSET_BITS` enforces. Caught at
   build time by `static_assert`, at runtime by the encode assert.
5. **Combined-index masks elsewhere.** Anywhere that previously
   masked `ras` by `SIZECLASS_REP_SIZE - 1` (or equivalent) to
   extract a sizeclass needs an audit: does it want pure sizeclass
   (`SIZECLASS_MASK`) or combined (`COMBINED_MASK`)? Grep for
   `SIZECLASS_REP_SIZE`, `(0xff)` style masks on `ras`, and
   `get_sizeclass()` callers. Convert each deliberately. The
   primary risk site is the backend's claim/release flow, which is
   already gated on the marker bit and so unaffected.

## Out of scope

- Front-end requesting non-pow2 large sizes (Phase 15).
- Per-chunk offset for small allocations (small uses slab_mask
  recovery, no per-chunk offset needed).
- Configs where `slab_size < MIN_CHUNK_SIZE` (multiple logical
  slabs per pagemap entry). The default `INTERMEDIATE_BITS = 2`
  config does not hit this. Deferred to a future phase if needed.

## Performance characterisation

Goal: the layout-aware design should bring `perf-external_pointer`
back to baseline (or within noise). The two costs the previous,
`meta`-word-based, Phase 14 design carried —

- one extra 8-byte load of the `meta` word per `external_pointer`
  query, just to extract the offset; and
- one `imul` for `offset * slab_size` on the critical path —

are both eliminated:

- The combined index is the same `ras` word already loaded for the
  sizeclass; masking with `COMBINED_MASK` is a single `and`-with-imm.
- `offset_bytes` is a table column; the subtraction is a load + a
  sub, with no multiplication.

`perf-large_alloc` is unchanged from the prior fix (single-slab-tile
fast path keeps `set_metaentry` as before; the per-chunk loop is
dormant until Phase 15). `perf-singlethread` and `perf-memcpy` were
within noise before and should remain so.

Measure with five-run medians on `build-rel-base` (commit
`1144eab4`) vs `build-rel-p14` (head + Phase 14 layout-aware), per
the perf workflow in `.github/skills/building_and_testing.md`. If
`perf-external_pointer` is not within noise of baseline,
disassemble the new `__malloc_start_pointer` to confirm the load
count matches baseline (one 8-byte load of the pagemap byte, no
`meta` word load, no `imul`).

# Pre-Phase-15: compile-time aligned dealloc overload

## Goal

Fix a pre-existing latent bug in the compile-time templated alloc /
dealloc API. This is independent of Phase 15 and is committed as a
sibling commit before Phase 15 begins.

## The bug

`globalalloc.h:341-356` `alloc<size, Conts, align>` applies
`aligned_size(align, size)` internally:
```
constexpr size_t sz = aligned_size(align, size);
… alloc(sz);
```

`globalalloc.h:394-399` `dealloc<size>(p)` does not — it passes the
raw `size` to `check_size`:
```
template<size_t size>
SNMALLOC_FAST_PATH_INLINE void dealloc(void* p)
{
  check_size(p, size);
  …
}
```

When the alignment-driven upgrade pushes the alloc into a different
sizeclass than `size` itself, `check_size` fires. Concretely today
(pre-Phase-15), with `S = 33 KiB`, `A = 128 KiB`:

- `alloc<33 KiB, Uninit, 128 KiB>()` → `aligned_size(128 KiB, 33 KiB)
  = 128 KiB` → pagemap `sc(128 KiB)`.
- `dealloc<33 KiB>(p)` → `check_size(p, 33 KiB)` →
  `size_to_sizeclass_full(33 KiB) = sc(40 KiB)` (sc(64 KiB) once
  Phase 15 lands).
- Mismatch — `check_size` fires under `mitigations(sanity_checks)`.
  Verified on `main` with a manual reproducer:
  `Dealloc rounded size mismatch: 0xa000 != 0x20000`.

The bug exists in `main` today; it does not require Phase 15. Phase
15 lowers the threshold (more (A, S) pairs cross a sizeclass
boundary) but does not introduce the asymmetry.

## Fix

Merge `dealloc<size>` into a single template with `align` defaulted
to 1, so the same body handles both calling forms:
```
template<size_t size, size_t align = 1>
SNMALLOC_FAST_PATH_INLINE void dealloc(void* p)
{
  constexpr size_t sz = aligned_size(align, size);
  check_size(p, sz);
  ThreadAlloc::get().dealloc<ThreadAlloc::CheckInit>(p);
}
```
`aligned_size(1, size) == size` for all `size`, so existing
single-argument `dealloc<size>(p)` callers are bit-equivalent to
their previous behaviour.

To make `aligned_size` reachable from the test library header (which
deliberately avoids pulling in the full runtime sizeclass tables),
move its definition from `sizeclasstable.h` to `sizeclassstatic.h`.
The function is a pure compile-time-friendly utility — it depends
only on `is_small_sizeclass`, `bits::is_pow2`, and the SNMALLOC_*
macros, all of which are already available in `sizeclassstatic.h`.
Consumers of `aligned_size` previously included via `sizeclasstable.h`
still pick it up transitively through the existing include chain
(`pal.h` → `ds_core.h` → `sizeclassstatic.h`).

Apply the same merge in the test library:
- `template<size_t size, size_t align = 1> void dealloc(void* p)`
  replaces the previous `template<size_t size>` testlib overload.
- `template<size_t size, ZeroMem, size_t align = 1> void* alloc()`
  replaces the previous two-parameter testlib `alloc`. The body
  computes `sz = aligned_size(align, size)` and routes to the
  small/large path based on `sz`.

## Test

`src/test/func/aligned_dealloc/aligned_dealloc.cc`, listed in
`TESTLIB_ONLY_TESTS` so it is compiled once and linked against both
testlib flavours.

- Includes `test/snmalloc_testlib.h` only — exercises the public
  templated `alloc<size, ZeroMem, align>` / `dealloc<size, align>`
  surface through the testlib layering.
- The canonical reproducer `(S = 33 KiB, A = 128 KiB)` fires the bug
  on `main` under the `check` flavour. Confirmed by hand before the
  fix.
- Additional `(S, A)` pairs cover a small-to-large alignment upgrade,
  a wider gap, the `align == size` baseline, and a small natural
  alignment case.

## Gate

1. Build clean.
2. New test passes under both `fast` and `check`.
3. Full ctest suite green.
4. Pre-commit review loop.
5. Commit approval.

After this commit lands, Phase 15 begins on top of it.

# Phase 15: Front-end requests non-pow2 large allocations

## Goal

Flip the front-end so that large allocations request exactly the
sizeclass-encoded size (chunk-multiple, exp+mantissa-rounded),
instead of always the next power of two. This is the long-running
goal of the refactor: the backend (`LargeArenaRange`) has
supported arbitrary chunk-multiple sizes since Phase 10–12, the
sizeclass encoding has supported non-pow2 large since Phase 13,
and the per-chunk offset machinery has supported pointer recovery
since Phase 14.

Effect: a request for e.g. 70 KiB on the default config
(`INTERMEDIATE_BITS = 2`) currently reserves 128 KiB (next pow2);
after Phase 15 it reserves 80 KiB (the next exp+mantissa class,
saving ~37.5%). A request for 96 KiB + 1 byte currently reserves
128 KiB; after Phase 15 it reserves 112 KiB. Sizes that already
land on a class boundary (e.g. 80 KiB, 96 KiB) reserve exactly
their requested size where today they reserve the next pow2. Net
effect across workloads is a reduction of large-allocation
footprint up to ~33% for sizes that fall mid-exponent.

## Why now

Phase 14 added the per-chunk offset write in `Backend::alloc_chunk`,
the three-table sizeclass metadata split (`start_` / `align_` /
`slab_`), and the offset==0 fast-path branch in `start_of_object`.
All of this is dormant on the front-end today because
`large_size_to_chunk_size(size) = next_pow2(size)` means every
materialised large allocation has `offset = 0` in every chunk. The
Phase 14 `large_offset` test reaches the per-chunk path via the
public *backend* API to confirm the dormant code is correct; Phase
15 is what makes the front-end actually exercise it.

## Pre-flight verification

Before implementing, confirm these Phase 14 facts (all true today —
listed so reviewers can re-check):

- `bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(v)`
  ceil-encodes (`v = v - 1; …`), so passing the raw size (not
  `next_pow2(size)`) maps to the smallest enclosing sizeclass.
- `Backend::alloc_chunk` currently asserts
  `bits::is_pow2(size)`. The Phase 14 pagemap loop advances by
  `slab_size = sizeclass_full_to_slab_size(sizeclass)`, so the
  correct precondition is `size >= slab_size` *and*
  `(size & (slab_size - 1)) == 0`. Both already hold by
  construction for front-end calls because
  `size = sizeclass_full_to_size(sc)` and `slab_size = size & -size`
  is the largest pow2 divisor of `size`; the loop terminates
  exactly at `size`. We will tighten/relax the assert to match.
- The Phase 14 assert that `ras`'s offset bits are zero on entry
  to `alloc_chunk` continues to hold: front-end calls
  `PagemapEntry::encode(remote, sc)` with default `offset = 0`.
- `ArenaBins::carve` returns a base aligned to
  `info.align = size & -size` (the largest pow2 divisor of size,
  set in the bin-table ctor at `arenabins.h:742`). For a
  96 KiB request that is 32 KiB = `slab_size` =
  `sizeclass_full_to_slab_size(sc)` — exactly what
  `start_of_object`'s `addr & ~slab_mask` requires.
- `globalalloc::remaining_bytes` / `index_in_object` already route
  through `entry.get_offset_and_sizeclass()` (committed in Phase
  14's API cleanup), so they will pick up non-zero offsets
  automatically once the front-end produces them.

## Changes

### `src/snmalloc/ds/sizeclasstable.h`

- `size_to_sizeclass_full(size)`: large branch calls
  `to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(size)`
  directly. The encoding's ceil semantic selects the smallest
  sizeclass whose size is `>= size`.
- `large_size_to_chunk_size` is removed. After the change above it
  would just be `sizeclass_full_to_size(size_to_sizeclass_full(size))`,
  which is exactly what `round_size` returns on the large branch; the
  one in-tree caller (`corealloc.h` large path) is hoisted to use
  `sizeclass_full_to_size(sc)` directly with a single `sc` lookup, so
  the wrapper carries no remaining work.
- `round_size(size)`: large branch returns
  `sizeclass_full_to_size(size_to_sizeclass_full(size))`. This is
  correctness-critical because `DefaultConts::success` in
  `corealloc.h:34-47` uses `round_size` to determine the zeroing
  range for `calloc`. Without it `calloc` would zero beyond the
  actual reservation.
- `compute_max_large_slab_index` tightens its bound to
  `meta.size / slab_size - 1` (the actual worst case the runtime
  loop writes). The previous `next_pow2(meta.size) / slab_size - 1`
  overestimates now that no caller reserves `next_pow2(size)`.
- Doc-comments on `size_to_sizeclass_full` and `round_size` describe
  the exp+mantissa rounding.

### `src/snmalloc/backend/backend.h`

- `alloc_chunk` precondition: the slab-tile invariant
  ```
  const size_t slab_size = sizeclass_full_to_slab_size(sizeclass);
  SNMALLOC_ASSERT(size >= slab_size);
  SNMALLOC_ASSERT((size & (slab_size - 1)) == 0);
  ```
  matches the pagemap loop's stride exactly and is the minimum
  required for the per-chunk write to terminate at `size`. The
  previous duplicate `size >= slab_size` assert inside the loop is
  consolidated.
- The offset-bits-zero assert on `ras` stays — the front-end uses
  `encode(remote, sc)` with default offset 0.
- Loop comment describes `size` as a multiple of `slab_size` with
  `size >= slab_size`.

### `src/snmalloc/global/globalalloc.h`

No change. The runtime sized-dealloc check is correct because every
legitimate caller pre-applies `aligned_size`:

- Unaligned `sized_dealloc(p, S)`: alloc was `malloc(S)`, which goes
  through `size_to_sizeclass_full(S)`; the dealloc check evaluates
  the same function on the same `S`. Same sizeclass.
- Aligned `sized_dealloc(p, S, A)` (line 401): computes
  `aligned_size(A, S)` *before* calling `check_size`.
- `rust.cc:33` and `rust.cc:51`: both apply `aligned_size` before
  the 2-arg `dealloc(ptr, size)` path.
- `jemalloc_compat::sdallocx`: ignores the size argument.

A 2-arg `sized_dealloc(p, S)` after `aligned_alloc(A, S)` with
`aligned_size(A, S) > S` would mismatch — but that is a client bug:
the client should use the 3-arg form for aligned allocations.

The compile-time `alloc<size, Conts, align>` / `dealloc<size>`
asymmetry is being fixed in the **pre-Phase-15 sibling commit**
(see the "Pre-Phase-15: compile-time aligned dealloc overload"
section below). Phase 15 does not touch `globalalloc.h`.

### `src/snmalloc/mem/corealloc.h`

- Large-alloc handler at lines 723-728 currently invokes
  `size_to_sizeclass_full(size)` three times and
  `large_size_to_chunk_size(size)` once. Hoist into locals so the
  table lookups happen once:
  ```
  const auto sc        = size_to_sizeclass_full(size);
  const size_t chunk_sz = sizeclass_full_to_size(sc);
  auto [chunk, meta] = Config::Backend::alloc_chunk(
    self->get_backend_local_state(),
    chunk_sz,
    PagemapEntry::encode(self->public_state(), sc),
    sc);
  ```
  - Phase 15 still leaves the large path through the same handler;
    the hoist removes duplicated work on the large-allocation path
    rather than changing any small-allocation hot loop.

### `src/snmalloc/backend_helpers/smallbuddyrange.h:232` and similar

- `alloc_range_with_leftover` uses `bits::next_pow2(size)` to size
  its parent request. This range serves the *meta-data* allocator,
  not the user object range — meta_size is always pow2 (line 203
  of `backend.h` already calls `next_pow2(sizeof(SlabMetadata) +
  extra_bytes)`). No change needed; verify by inspection that the
  call site is not on the user-large path and note the conclusion
  in the commit.

### Tests

- The existing `src/test/func/large_offset/large_offset.cc` test
  exercises the per-chunk path via the *backend* API. Phase 15
  flips the *front-end* to do the same. The test's header
  comment (lines 5-9) currently says "currently only issues pow2
  large requests" and that `alloc_chunk` "asserts pow2"; both
  become false after Phase 15. Update the comment to describe
  this test as the *low-level* / *backend-API* counterpart of the
  new front-end test.

- Add a sibling test `src/test/func/large_offset_frontend/` that
  exercises a *bounded* set of representative large sizeclasses
  (smallest non-pow2 large class, two mid-range classes spanning
  different exponents, one near `MAX_LARGE_SIZECLASS_SIZE` only if
  the total allocation is well under the available test-time
  address budget — cap at a few MiB per allocation). For each
  selected sizeclass `sc` where
  `sizeclass_full_to_size(sc) != sizeclass_full_to_slab_size(sc)`:
  - Call `malloc(sizeclass_full_to_size(sc))`, save `p`. Assert
    `is_start_of_object<Start>(p)`.
  - For every chunk offset `j * MIN_CHUNK_SIZE` with
    `j ∈ [1, size_full / MIN_CHUNK_SIZE)`, assert
    `external_pointer<Start>(p + j * MIN_CHUNK_SIZE) == p` and
    `remaining_bytes(p + j * MIN_CHUNK_SIZE) == size_full - j *
    MIN_CHUNK_SIZE`.
  - Assert `malloc_usable_size(p) == size_full` (the new actual
    reservation, not `next_pow2(size_full)`).
  - Free, then re-allocate and confirm address re-use behaves
    sanely.
  - Also allocate a *non-boundary* request between adjacent class
    sizes (e.g. `malloc(size_full - 1)` for a non-pow2 class,
    `malloc(prev_class + 1)`) and assert `malloc_usable_size(p)`
    equals `size_full` — this is what proves the raw request maps
    to the smallest enclosing class.
  - Pure table-level properties (every large sizeclass round-trips
    through `size_to_sizeclass_full` ∘ `sizeclass_full_to_size`)
    can be checked without allocating; loop over the full large
    range there.

- `src/test/func/sizeclass/sizeclass.cc` lines 160-175 currently
  assert that a non-pow2 large size strictly between adjacent
  pow2 rounds to the next pow2. Phase 15 changes this: a non-pow2
  size now rounds to the next exp+mantissa class. Compute the
  expected value independently of the function under test — scan
  the representable large classes (e.g. iterate sizeclasses 0 ..
  `NUM_LARGE_CLASSES`) and pick the smallest `sizeclass_full_to_size(sc) >= mid`.
  Then assert `size_to_sizeclass_full(mid)` equals that sizeclass
  and `sizeclass_full_to_size(size_to_sizeclass_full(mid))` equals
  the independently-computed class size. Update the comment
  ("pow2 rounding still in force") accordingly. The surrounding
  `b == ENCODED_ADDRESS_BITS` bound logic stays.

  **Add a deterministic `round_size` regression gate alongside.**
  For each representable large sizeclass `sc` with size `S =
  sizeclass_full_to_size(sc)`, and `S_prev` the previous class
  size, assert:
  - `round_size(S) == S`
  - `round_size(S_prev + 1) == S` (i.e. the request is rounded
    to the smallest enclosing class, not blown up to the next
    pow2).
  - `large_size_to_chunk_size(S_prev + 1) == round_size(S_prev + 1)`
    (the chunk-size and round-size views agree).

  This is the primary `round_size` gate. If `round_size` is left
  as `next_pow2`, these assertions fail deterministically — unlike
  the calloc zeroing smoke test below, which may not fault when
  `memset` overruns into backend free range.

- `src/test/func/release-rounding/rounding.cc` lines 86-127
  exercise pow2 large sizes end-to-end via
  `index_in_object`/`is_start_of_object`. Phase 15 does not
  change behaviour for pow2 sizes (they still round to themselves),
  so this loop continues to pass unchanged. Optionally extend
  the loop with a non-pow2 case (e.g. `mid = S + (S >> 2)`) to
  exercise the new front-end-materialised non-pow2 classes.

- `src/test/func/malloc/malloc.cc:82-87` uses
  `natural_alignment(size)` symbolically. Because
  `natural_alignment` derives from `round_size`, the test
  auto-tracks Phase 15: a 96 KiB alloc now reports 32 KiB
  alignment (today: 128 KiB). No code change in the test, but
  cross-check that no test elsewhere hard-codes "pow2 large
  alignment".

- `src/test/func/statistics/` (and any other test asserting
  per-sizeclass alloc counts): verify the assertion model does
  not assume pow2 large counts. Inspection-only first; update
  only if tests fail.

- **Calloc zeroing correctness smoke test.** The existing calloc
  tests (`memory.cc::test_calloc_16M`, `test_calloc` loop in
  `malloc.cc`) mostly use sizes that round to a pow2 reservation
  even today, so they would not catch `round_size` being left as
  `next_pow2` after Phase 15. Add a test in
  `src/test/func/memory/memory.cc` that calls `calloc(1, S)` for
  a non-pow2 large class size `S` and asserts
  `malloc_usable_size(p) == S` and that every byte in `[p, p + S)`
  is zero. This is a smoke test only — the deterministic gate for
  the `round_size` regression lives in `sizeclass.cc` (above)
  because a `memset` overshoot into backend free range may not
  fault and would not be caught by zeroing the visible range.

## Test gates

1. **Build**: clean build passes. The `static_assert` chain from
   Phase 14 is unchanged — `compute_max_large_slab_index` in
   `sizeclasstable.h:419-437` still uses
   `bits::next_pow2_const(meta.size)`, which is *conservative*
   under Phase 15 (the front-end now reserves at most that much,
   often less), so the budget bound continues to hold.
2. **Full ctest suite**: all 88 existing tests pass after
   expectation updates in `sizeclass.cc`. Tests exercising large
   allocations now allocate exp+mantissa-rounded chunk sizes;
   reservation footprint shrinks; functional results unchanged.
3. **New `large_offset_frontend` test** passes — per-chunk offsets
   are now produced by the front-end and recovered by
   `external_pointer` / `remaining_bytes`.
4. **`perf-external_pointer-fast`**: median within noise of the
   Phase 14 baseline (~290 ms on the dev machine). The hot path
   for small allocations is unchanged; the only change in
   instruction count comes from `__malloc_start_pointer` for
   non-pow2 large allocations, which now exercises the slow arm of
   the `offset == 0` branch added in Phase 14 — but only for
   genuinely non-pow2 allocations, of which the benchmark has
   none.
5. **`perf-singlethread-check`**: within noise.
6. **Memory footprint**: a synthetic benchmark allocating
   `malloc(96 KiB)` × N reports peak RSS lower by ~25% vs the
   pre-Phase-15 baseline. (Optional diagnostic; not a gate.)

## Risks

1. **`calloc` zeroing range overshoot**. Mitigated by updating
   `round_size` for large. Verify by inspecting
   `corealloc.h:34-47` (`DefaultConts::success`) — must zero
   exactly the reservation size returned by `round_size`. The new
   non-pow2 calloc test in `memory.cc` is the regression gate.
2. **External clients assuming pow2-aligned large allocations.**
   `natural_alignment` automatically reports the reduced
   alignment, but any external code that hard-codes "large allocs
   are pow2-aligned" silently breaks. Document in the commit
   message; consider a release note if there is a CHANGELOG.
3. **`aligned_alloc` overflow at extreme sizes.** `aligned_size`
   already handles SIZE_MAX overflow; behaviour unchanged.
4. **Performance regression on the front-end alloc path.**
   `next_pow2(size)` is replaced by `to_exp_mant(size)` plus a
   table lookup. Both are constant-time and small; perf gate
   confirms no regression.
5. **External pagemap / fixed-region builds.** The fixed-region
   tests (`src/test/func/fixed_region/`,
   `src/test/func/external_pagemap/`) construct allocations via
   different paths. Re-run them in the full suite.
6. **Statistics counters.** `func-statistics` checks per-sizeclass
   counts. Verify the test doesn't hard-code "every large is
   pow2".

## Out of scope

- Reducing `INTERMEDIATE_BITS` to gain bits in the sizeclass tag
  (Phase 13 chose the existing value).
- Generalising small allocations (already exp+mantissa).
- Any change to `alloc_range` / `dealloc_range` of arbitrary
  byte-multiples — front-end always rounds via the sizeclass
  encoding before reaching the backend.
- Removing the offset==0 fast-path branch in `start_of_object`.
  After Phase 15 the slow arm is reachable from the front-end, but
  the branch is fully predicted on small-allocation workloads
  (which dominate the benchmark) and the slow arm's cost is small.

## Implementation order (every step has a test gate)

1. **Front-end flip + `alloc_chunk` precondition + frontend test
   in a single commit.** This is one atomic refactor: the
   precondition cannot be relaxed safely until the front-end has
   reasons to call with non-pow2 sizes, and the front-end flip
   cannot be exercised end-to-end without the precondition
   relaxation. Files touched in this commit:
   - `src/snmalloc/ds/sizeclasstable.h`: drop `next_pow2` from
     `size_to_sizeclass_full`; rewrite `large_size_to_chunk_size`
     and `round_size` per the "Changes" section; update doc
     comments.
   - `src/snmalloc/backend/backend.h`: replace
     `alloc_chunk`'s `is_pow2(size)` precondition with the
     slab-tile invariant; rewrite the surrounding comment.
   - `src/snmalloc/mem/corealloc.h`: hoist the duplicated
     `size_to_sizeclass_full(size)` / `large_size_to_chunk_size`
     calls in the large-alloc path (lines 723-728) into locals.
   - `src/test/func/large_offset_frontend/`: new test (the
     gate). Covers per-chunk pagemap recovery and non-boundary
     requests.
   - `src/test/func/large_offset/large_offset.cc`: update header
     comment now that the backend-API and front-end exercise the
     same path.
   - `src/test/func/sizeclass/sizeclass.cc`: update the
     non-pow2-rounds-to-next-pow2 expectation at lines 160-175.
   - `src/test/func/memory/memory.cc`: add non-pow2-large calloc
     test (the `round_size` regression gate).
   Gate: full ctest suite passes including
   `large_offset_frontend` and the new calloc test.

2. **Perf gate** (per the perf-gate protocol from Phase 14):
   measure `perf-external_pointer-fast` and
   `perf-singlethread-check` against the Phase-14 baseline (~290
   ms / ~580 ms median); 5 runs × 3 reps each; report median +
   range. If a regression is found, root-cause via perf annotate
   before committing — do not paper over with workarounds.

3. **Mandatory pre-commit review loop** before the commit.

# Review plan for Phases 13–15

Per claude.md "Mandatory review checkpoints":

1. After this plan is written (now), run the rubber-duck review
   pass on Phases 13–15 — read the plan + existing
   `sizeclasstable.h`, `metadata.h`, `corealloc.h`,
   `backend.h`, and confirm:
   - Assumptions about bit availability in `FrontendMetaEntry`
     (especially `alignof(SlabMetadata)`) are correct.
   - No phase has a hidden cross-phase dependency that breaks the
     "each phase ends with passing tests" invariant.
   - The SIZECLASS_BITS widening doesn't break MetaEntry encoding.
   - Tests proposed for each phase actually gate the right
     invariants.
2. Address findings; present revised plan for explicit user
   approval before any code changes.
3. After implementation of each phase, run the build/test subagent
   per `.github/skills/building_and_testing.md`.
4. After all three phases land, pre-PR review (mandatory checkpoint).
