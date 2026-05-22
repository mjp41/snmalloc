# User Plan

We need to refactor the backend buddy allocator to use the more general concept in IDEA.md, which uses a more general concept of sizeclasses than powers of two to avoid internal fragmentation.

The design will use the Red-Black tree that currently underlies the buddy allocator, but in a different shape: two parallel trees instead of one-per-exponent.

Each block will be part of two structures:

* [Bin] A red-black tree of all blocks held by this BackendArena, in the same bin, ordered by address.
* [Range] A red-black tree of all blocks held by this BackendArena, ordered by address.

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

### Build BackendArena

This should use two RB-trees.

It should support adding and removing blocks.

There should be unit tests that check that it is functioning correctly.

There should be a runtime checked invariant that
* the system is maximally consolidated, and
* the system is consistent between the two RB-trees.

### Build BackendArenaRange

This should wrap the BackendArena using the snmalloc Range approach that is used in the current backend pipelines.

### Update backend to use BackendArenaRange

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

# Implementation plan: BackendArena phase

## Scope of this phase

This plan covers **only** the `BackendArena` data structure and its standalone
unit tests. The following are explicitly deferred to follow-up plans, each of
which will become its own PLAN.md revision:

- `BackendArenaRange` — wrapping `BackendArena` behind snmalloc's Range API.
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

Each `BackendArena` instance owns:

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
3. `carve(block, n_chunks)` splits into pre-pad / aligned request /
   post-pad. Re-add any non-empty pre/post via `add_block` (which
   classifies the remainder via `bitmap.add(remainder)`).

The bin classification and per-sc search masks (`start_word`,
`first_mask`, `second_mask`) are precomputed at `constexpr` time
directly from the size-class structure (no runtime tables beyond the
bitmap of non-empty bins).

### Range: single tree across all blocks (with min-size exception)

A single RBTree per `BackendArena` orders all *non-min-size* free blocks by
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
**variant tag** that tells `BackendArena` how to interpret the other
entries in the block:

| Variant     | Value | Block size     | Alignment      | Pagemap entries used by BackendArena                                   |
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
The variant tag is only meaningful for entries `BackendArena` reaches via
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
`BackendArena`'s own trees. **No pagemap probing.** The pagemap is shared
across `BackendArena` instances (e.g. thread-local + global), and reading
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
adjacency" exactly when it is reachable from one of this `BackendArena`'s
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
`BackendArena`.

### Invariants (debug-only, runtime-checked)

The `BackendArena::invariant()` method checks:

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
is currently power-of-two only. `backend_arena_bins.h` defines the
chunk-unit size-class scheme using the snmalloc size-class formula
`S = 2^e + m · 2^(e − B)` applied at **chunk-count exponents starting
from zero**. Low-exponent special cases (chunk counts 1, 2, 3, …) follow
the same pattern as `bits::from_exp_mant` in
`src/snmalloc/ds_core/sizeclassstatic.h`: at small exponents the mantissa
space is degenerate, handled by enumeration.

The public API of `BackendArenaBins<B>` — the integration contract
`BackendArena` builds on — is intentionally narrow:

- `struct range_t { size_t base; size_t size; }` — a chunk-count range
  used to describe free blocks and carved sub-ranges.
- `struct carve_t { range_t pre, req, post; }` — output of a carving
  operation; either of `pre`/`post` may have `size == 0` (absent).
- `static carve_t carve(range_t block, size_t n_chunks)` — given a
  free block and an allocation request, split into pre-pad / aligned
  request / post-pad. Pure function; does not touch the bitmap.
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
only via the friend struct `BackendArenaBinsTestAccess<B>` (defined in
the test translation unit, see Phase 1) so unit tests can exercise
them directly; production code outside this header does not depend on
them.

**Free blocks may have arbitrary chunk counts**, including non-class
sizes that arise from carving (e.g. a 9-chunk prefix at `B=2`). The
private `bin_index` operates on arbitrary `(address, size)` pairs and
classifies into a bin by the block's servable set; the public `Bitmap`
exposes this only through `add(range_t)`, which returns the bin id.
Exact size classes appear only on the request side; free blocks store
their precise chunk count where needed (`Large` variant).

### Exponent / bin-count bounds

`BackendArena<Rep, MIN_CHUNKS_BITS, MAX_CHUNKS_BITS>` takes
**chunk-count exponent** bounds. `MIN_CHUNKS_BITS = 0` (1 chunk). The
upper bound is **exclusive**, matching `Buddy<..., MIN, MAX>`'s
semantics. The total number of bins is
`(MAX_CHUNKS_BITS - MIN_CHUNKS_BITS) * BINS_PER_EXP` plus the
degenerate-low-exponent bins. Static assertions encode the exclusive
semantics; tests exercise minimum, just-below-max, and exact-max sizes
(the last triggers overflow back to the parent, mirroring `Buddy`).

The chunk-unit bin scheme is independent of `sizeclass_t::as_large()`
for now. The "Generalise the large size classes" follow-up plan will
reconcile the front-end large size classes with this scheme.

### Multiple instances

All state lives in pagemap-backed nodes and in per-instance roots/bitmaps;
no global state. Multiple `BackendArena` instances can coexist (thread-local
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
     compiler attributes, no C++ STL in production, `SNMALLOC_*`
     macros, etc.).

  Address findings, re-spawn a fresh-context reviewer, loop until
  a reviewer reports no issues. Disputes with reviewer findings
  escalate to the user, not resolved unilaterally.

Phases 0 and 6 are exempted from the review gate: Phase 0 adds no
code; Phase 6 is test-only over already-reviewed production code.
Phase 7 is the final mandatory review per `claude.md`.

### Phase 0: Baseline

Per `claude.md` "Baseline the checkout before starting work": run a clean
Debug build and the full test suite via the testing subagent protocol in
`.github/skills/building_and_testing.md`. Record the results. If the baseline is
broken, stop and report — do not start implementation on a broken base.

**Test gate**: full ctest run completes; record pass/fail status of each
test for later comparison.

### Phase 1: BackendArenaBins — bin scheme, per-sc tables, and bitmap

Add `src/snmalloc/backend_helpers/backend_arena_bins.h` defining
`BackendArenaBins<INTERMEDIATE_BITS>`: the chunk-unit size-class
scheme, two per-sc rodata tables, the free-block classifier, and the
nested non-empty-bins bitmap that the allocation fast path scans.

#### Public surface — the integration contract

- `struct range_t { size_t base; size_t size; }` — chunk-count range.
- `struct carve_t { range_t pre; range_t req; range_t post; }` — output
  of a carving operation; `pre` and/or `post` may have `size == 0`.
- `static SNMALLOC_FAST_PATH carve_t carve(range_t block, size_t n_chunks)`
  — split a free `block` into pre-pad, aligned request, post-pad.
  Pure. **Preconditions** (asserted):
  `n_chunks >= 1 && n_chunks <= max_supported_chunks()`,
  `block.size > 0`, and `block` is servable for `n_chunks` (the caller
  has already used `Bitmap::find_for_request`).
- `static constexpr size_t max_supported_chunks()` — upper bound on
  legal `n_chunks`; used for assertions.
- nested `class Bitmap` — three methods, all that production code
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
  `BackendArena` layer; bins are not size classes (multiple size
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
  BackendArenaBins exponent (sentinel at index `bits::BITS` equals
  `MAX_SC`). NOTE: this is not uniform stride — at the bottom of the
  encoding the low regime squashes multiple BackendArenaBins exponents
  into encoded-exponent 0.
- `exp_bin_base[bits::BITS + 1]` — `e * BINS_PER_EXP`, precomputed so
  `bin_index` does no runtime multiply.
- `cascade_steps[MANTISSAS_PER_EXP][MAX_CASCADE_STEPS]` — per-`m_top`
  decision lists for `bin_offset_at`.

A `static constexpr BinTable table_{}` member of `BackendArenaBins<B>`
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

Production calls on the fast path use the runtime intrinsic, not the
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
  friend struct BackendArenaBinsTestAccess<INTERMEDIATE_BITS>;

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

Friend declarations: `BackendArenaBins<B>` and its nested `Bitmap`
each carry their own `friend struct BackendArenaBinsTestAccess<...>;`
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

`BackendArenaBinsTestAccess<INTERMEDIATE_BITS>` is **forward-declared**
in `backend_arena_bins.h` (so the friend declarations can refer to it)
and **defined in the test translation unit**
`src/test/func/backend_arena_bins/backend_arena_bins.cc` (inside
`namespace snmalloc`). The production header therefore carries no
test-only members.

What the test access struct exposes (all delegating to private
internals through the friend grant):

- Re-exports of the public types and methods, for convenience.
- The bin-scheme constants `B`, `MANTISSAS_PER_EXP`, `BINS_PER_EXP`,
  `MAX_SC`.
- `using chunk_sc_t = size_t;` — raw sc id as plain `size_t`; the
  production header does NOT define a `chunk_sc_t` handle type.
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
- Production header carries no test-only surface (no `chunk_sc_t`
  handle class, no `request`, no `_const` variants, no test-only
  per-sc accessors — those live only in
  `BackendArenaBinsTestAccess` in the test cc).
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
  `BackendArena` use case (two free blocks cannot share a starting
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

### Phase 3+4: Full BackendArena data structure (atomic)

Create `src/snmalloc/backend_helpers/backend_arena.h` with:

- A `BackendArenaRep` concept describing word-level accessors over the
  three pagemap entries, the variant tag, and the large-size accessor:
  - `get_variant(addr) -> BackendArenaVariant` / `set_variant`
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

- `BackendArena<Rep, MIN_CHUNKS_BITS, MAX_CHUNKS_BITS>`:
  - `B = 2` hardcoded; `INTERMEDIATE_BITS` wiring deferred.
  - `MIN_CHUNKS_BITS == 0` only; larger min values deferred.
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

- `Bitmap::test(size_t bin_id)` added to `BackendArenaBins` (read-only
  accessor used by `invariant()`).

Modifications to existing files:
- `src/snmalloc/backend_helpers/backend_arena_bins.h`: added
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

All changes are in `backend_arena.h` and the test file.

1. **Add `OddTwo = 3`** to `BackendArenaVariant` enum.
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
   `Rep::get_variant(addr) == BackendArenaVariant::Min`. Return false
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

Instantiate two `BackendArena<MockRep>` over disjoint address ranges in
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

# Implementation plan: BackendArenaRange phase

## Scope

Build `BackendArenaRange` — a Range pipeline component that wraps
`BackendArena` behind snmalloc's Range API, suitable for replacing
`LargeBuddyRange`. This plan covers:

- Generalising BackendArena's Rep interface for pagemap compatibility.
- `PagemapRep` — adapting pagemap entries to BackendArena's Rep concept.
- `BackendArenaRange` — the Range wrapper with refill and overflow handling.
- Boundary-bit support for safe consolidation across PAL allocations.
- Unit tests for all of the above.

The pipeline integration (replacing `LargeBuddyRange` in `standard_range.h`
and `meta_protected_range.h`) is a separate step ("Update backend to use
BackendArenaRange") that follows once this plan is complete.

## Design

### Rep generalisation: representation-agnostic data structure

`BackendArena` must be representation-agnostic, mirroring how
`Buddy<>` is generic over its node `Rep` (see `buddy.h`). The
existing buddy ecosystem demonstrates the layering:

- `buddy.h` — pure data structure, no representation.
- `largebuddyrange.h` defines `BuddyChunkRep` — a pagemap-backed Rep
  (red bit at bit 8, layout chosen to coexist with the pagemap's
  reserved low bits).
- `smallbuddyrange.h` defines `BuddyInplaceRep` — an inline Rep that
  stores tree pointers in the free chunk itself (red bit at bit 0).

`BackendArena` must support the same two representation paths so it
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
`BackendArena` carries no `RED_BIT` / `VARIANT_MASK` / `META_MASK`
constants of its own.

`BackendArena` instantiates `RBTree<typename Rep::BinRep>` and
`RBTree<typename Rep::RangeRep>` directly. It never inspects the bit
layout used by the Rep.

#### PagemapRep (production)

Lives in `backend_arena_range.h`. Privately owns its bit layout:

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
`BackendArena`-based replacement for `SmallBuddyRange`.

### Boundary-bit consolidation check

On platforms where `CONSOLIDATE_PAL_ALLOCS` is false (CHERI, Windows),
the pagemap sets a boundary bit on the first chunk of each PAL allocation
to prevent consolidation across allocation boundaries
(`BuddyChunkRep::can_consolidate` checks this).

BackendArena's `add_block` consolidation must respect the same contract.
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

Templated on `Pagemap` and `MAX_CHUNKS_BITS`. The second parameter is
needed for the large-size-shift static assertion:

```
template<SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap, size_t MAX_CHUNKS_BITS>
struct PagemapRep { ... };
```

Each free block uses pagemap entries at three offsets from its base
address:

- **Chunk 0** (`addr`): Word::One / Word::Two → bin-tree node.
  Bits 9–10 of Word::One → variant tag. Bit 8 → RED_BIT. All coexist
  because `TreeRep::set` preserves `META_MASK` on writes.
- **Chunk 1** (`addr + MIN_CHUNK_SIZE`): Word::One / Word::Two →
  range-tree node (only for blocks ≥ 2 chunks).
- **Chunk 2** (`addr + 2 * MIN_CHUNK_SIZE`): Word::One → large chunk
  count (only for blocks ≥ 3 chunks). Stored as `count << 8` to avoid
  the 8 reserved low bits; recovered via `word.get() >> 8`.

**Static assertions in PagemapRep** (catch configuration errors early):

- `static_assert((VARIANT_MASK | RED_BIT) < MIN_CHUNK_SIZE)` — metadata
  bits don't collide with address bits.
- `static_assert(MetaEntryBase::is_backend_allowed_value(Word::One,
  VARIANT_MASK | RED_BIT))` — all metadata bits are in the backend-
  allowed range.
- `static_assert(MAX_CHUNKS_BITS + 8 <= bits::BITS)` — shifted large
  size fits in a pagemap word.

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

### BackendArenaRange

Outer template matches `LargeBuddyRange`'s shape so it is a drop-in
replacement in `Pipe<...>` compositions:

```
template<
  size_t REFILL_SIZE_BITS,
  size_t MAX_SIZE_BITS,
  SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
  size_t MIN_REFILL_SIZE_BITS = 0>
class BackendArenaRange
{
public:
  template<typename ParentRange = EmptyRange<>>
  class Type : public ContainsParent<ParentRange>
  {
    using ContainsParent<ParentRange>::parent;

    static constexpr size_t MAX_CHUNKS_BITS = MAX_SIZE_BITS - MIN_CHUNK_BITS;
    using PagemapRepT = PagemapRep<Pagemap, MAX_CHUNKS_BITS>;
    BackendArena<PagemapRepT, 0, MAX_CHUNKS_BITS> arena;
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
2. `SNMALLOC_ASSERT(bits::is_pow2(size))` — same assertion as
   `LargeBuddyRange`. Non-power-of-two support is deferred to
   "Update front-end" step.
3. `n_chunks = size >> MIN_CHUNK_BITS`.
4. Oversize bypass: if `n_chunks >= bits::one_at_bit(MAX_CHUNKS_BITS)`,
   delegate to `parent.alloc_range(size)` (if `ParentRange::Aligned`),
   else return `nullptr`. Same as `LargeBuddyRange`.
5. `auto [addr, actual] = arena.remove_block(n_chunks)`.
   (`actual == n_chunks` for power-of-two requests — the `is_pow2`
   assertion above guarantees this for this phase.)
6. If `addr != 0`, return
   `capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(addr))`.
7. If `addr == 0`, call `refill(size)`.

**`dealloc_range(base, size)`**:

1. `addr = base.unsafe_uintptr()`, `n_chunks = size >> MIN_CHUNK_BITS`.
2. Oversize bypass: if `n_chunks >= bits::one_at_bit(MAX_CHUNKS_BITS)`,
   delegate to `parent.dealloc_range(base, size)`. Same condition and
   SFINAE guard as `LargeBuddyRange::parent_dealloc_range`.
3. `auto [ov_addr, ov_size] = arena.add_block(addr, n_chunks)`.
4. If overflow (`ov_addr != 0`): call `dealloc_overflow(ov_addr,
   ov_size)`.

**`dealloc_overflow(addr, size_chunks)`** — matches
`LargeBuddyRange::dealloc_overflow` pattern:

Overflow from `add_block` can produce non-power-of-two sizes (e.g.,
consolidated blocks that span multiple non-aligned PAL allocations).
The parent may require power-of-two aligned inputs. So overflow is
decomposed using `range_to_pow_2_blocks<MIN_CHUNK_BITS>`:

```
void dealloc_overflow(uintptr_t addr, size_t size_chunks)
{
  auto base = capptr::Arena<void>::unsafe_from(
    reinterpret_cast<void*>(addr));
  size_t size_bytes = size_chunks << MIN_CHUNK_BITS;
  if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
  {
    range_to_pow_2_blocks<MIN_CHUNK_BITS>(
      base, size_bytes,
      [this](capptr::Arena<void> b, size_t s, bool) {
        parent.dealloc_range(b, s);
      });
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
check), add everything to the arena via `add_range` (which decomposes
into power-of-two blocks using `range_to_pow_2_blocks`), then call
`alloc_range(size)` recursively. Same logic as `LargeBuddyRange`.

Safety guards (both from `LargeBuddyRange`):
- `static_assert((REFILL_SIZE < bits::one_at_bit(MAX_SIZE_BITS)) ||
  ParentRange::Aligned)` — prevents the unaligned path from adding a
  block that violates `add_block`'s `size_chunks < 2^MAX_CHUNKS_BITS`
  precondition.
- Runtime: `SNMALLOC_ASSERT(refill_size < bits::one_at_bit(MAX_SIZE_BITS))`
  — catches the computed `refill_size` (which may be larger than
  `REFILL_SIZE` when `needed_size = 2 * size` dominates).

### Static properties

- `Aligned = true`: BackendArena's carving ensures that a request of
  size `n` (power-of-two, chunk-aligned) is placed at an `n`-aligned
  address within the source block. For non-power-of-two requests
  (future: step 4), the bin scheme's alignment rules still hold.
- `ConcurrencySafe = false`: same as `LargeBuddyRange`.
- `ChunkBounds = capptr::bounds::Arena`: same as `LargeBuddyRange`.

### MAX_SIZE_BITS = BITS - 1 (global range)

The global `LargeBuddyRange` uses `MAX_SIZE_BITS = BITS - 1`, meaning
the buddy can hold up to half the address space. For BackendArenaRange:
`MAX_CHUNKS_BITS = (BITS - 1) - MIN_CHUNK_BITS`. On 64-bit with
`MIN_CHUNK_BITS = 14`, this is 49 — the arena can hold up to 2^49
chunks. The arena's overflow path returns consolidated blocks that
reach this size, handled by `dealloc_overflow` (see above).

The `large_size_chunks` field (stored shifted by 8 in a pagemap word)
needs at most 49 bits, which fits in the 56 backend-usable bits of a
64-bit pagemap word. A `static_assert(MAX_CHUNKS_BITS + 8 <= BITS)`
in `PagemapRep` catches configurations where this would overflow.

## Phases

### Phase 9: Rep generalisation + boundary support

**Status**: implemented; staged (not committed); awaiting review.

Changes to `backend_arena.h`:

1. Delete the private `WordRef` nested struct, the `TreeRep`
   template, and all bit-layout constants
   (`RED_BIT`/`VARIANT_MASK`/`META_MASK` and `BACKEND_RESERVED_MASK`).
   `BackendArena` is now representation-agnostic, mirroring how
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
   (red bit at bit 8 to match production layout).
7. MockRep keeps top-level `get_variant`/`set_variant`/large-size
   accessors and adds `can_consolidate(uintptr_t) → true`.
8. New test: verify that a MockRep variant with `can_consolidate`
   returning false at a specific address prevents consolidation across
   that boundary. Test both predecessor and successor merges being
   independently blocked.

**Test gate**: all existing BackendArena tests pass unchanged; new
boundary test passes.

### Phase 10: PagemapRep + BackendArenaRange + tests

New file: `src/snmalloc/backend_helpers/backend_arena_range.h`

1. `PagemapRep<Pagemap>` — full Rep implementation using pagemap entries
   as described above, with all static assertions.
2. `BackendArenaRange<REFILL_SIZE_BITS, MAX_SIZE_BITS, Pagemap,
   MIN_REFILL_SIZE_BITS>` — the Range wrapper with `alloc_range`,
   `dealloc_range`, `refill`, and `dealloc_overflow`.

Modified: `src/snmalloc/backend_helpers/backend_helpers.h`

3. Add `#include "backend_arena_range.h"` so the new header is
   available through the standard include path.

New file: `src/test/func/backend_arena_range/backend_arena_range.cc`

4. Test with snmalloc's `BasicPagemap` (or a test-appropriate pagemap):
   - PagemapRep word round-trips (variant, tree words, large size).
   - BackendArenaRange `alloc_range` / `dealloc_range` smoke test with
     a simple parent range.
   - Refill: verify that allocating when the arena is empty triggers a
     parent refill and returns memory.
   - Overflow: verify that deallocating a block that triggers arena-scale
     consolidation passes the decomposed overflow to the parent via
     `range_to_pow_2_blocks`.
   - Overflow with non-power-of-two consolidated size: verify
     decomposition produces valid power-of-two blocks.
   - Boundary: verify that a boundary bit in the pagemap prevents
     consolidation of adjacent blocks from different refills (when
     `CONSOLIDATE_PAL_ALLOCS` is false).
   - Test at largest configured `MAX_SIZE_BITS` values, especially
     `MAX_SIZE_BITS == bits::BITS - 1` if feasible.

Modified: `CMakeLists.txt`

5. Register `backend_arena_range` in `TESTLIB_ONLY_TESTS`.

**Test gate**: BackendArenaRange tests pass; existing tests unaffected.

### Phase 11: Final review

Per `claude.md` mandatory review checkpoints:

- Spawn a fresh-context reviewer on the full diff (Phases 9–10).
- Address findings, loop until clean.

**Test gate**: full ctest run passes; reviewer reports no issues.

*Pipeline integration (replacing `LargeBuddyRange` in `standard_range.h`
and `meta_protected_range.h`) is a separate follow-up plan: "Update
backend to use BackendArenaRange."*

## Files added / changed (anticipated, this phase)

- Modified: `src/snmalloc/backend_helpers/backend_arena.h` —
  representation-agnostic: delete private `WordRef`, `TreeRep`, and
  all bit-layout constants (`RED_BIT`/`VARIANT_MASK`/`META_MASK`/
  reserved); use `Rep::BinRep` and `Rep::RangeRep` directly;
  `can_consolidate` check in `add_block`; invariant clauses updated.
- New: `src/snmalloc/backend_helpers/backend_arena_range.h` —
  `PagemapRep` + `BackendArenaRange`.
- Modified: `src/snmalloc/backend_helpers/backend_helpers.h` — include
  `backend_arena_range.h`.
- Modified: `src/test/func/backend_arena/backend_arena.cc` — define
  `BackendArenaWordRef` test helper at top of file; MockRep updated
  (`BackendArenaWordRef` returns, `can_consolidate`); boundary tests.
- New: `src/test/func/backend_arena_range/backend_arena_range.cc` —
  Range wrapper tests.
- Modified: `CMakeLists.txt` — register `backend_arena_range` test.

## Key design decisions

1. **Representation-agnostic data structure** — `BackendArena`
   carries no bit-layout constants. All red/variant packing decisions
   live in the user-supplied `Rep::BinRep` / `Rep::RangeRep`, matching
   how `BuddyChunkRep` and `BuddyInplaceRep` each own their own
   layouts. This is what makes a future inline Rep (to replace
   `SmallBuddyRange`) possible.

2. **PagemapRep variant in bin-tree Word::One** — PagemapRep packs
   the variant tag at bits 9–10 of Word::One alongside the red bit
   (bit 8) and child pointer (bits ≥ MIN_CHUNK_BITS). These are
   private constants inside PagemapRep, not exposed by BackendArena.

3. **Large size stored shifted** — PagemapRep stores the chunk count
   as `count << 8` to avoid the pagemap's reserved low byte; recovered
   via `>> 8`. Guarded by `static_assert(MAX_CHUNKS_BITS + 8 <= bits::BITS)`.

4. **Boundary checks in BackendArena** — not in BackendArenaRange.
   Consolidation decisions happen inside `add_block`, so the boundary
   check must be there. The Rep concept cleanly abstracts this via
   `can_consolidate`.

5. **Refill returns prefix directly** — like LargeBuddyRange, the
   first `size` bytes of a refill bypass the arena. Only the remainder
   enters the arena. This avoids unnecessary tree operations on the
   hot path.

6. **PagemapRep auto-claims entries** — `get_backend_word` calls
   `claim_for_backend()` on first access. No explicit ownership
   management needed in BackendArena or BackendArenaRange.

7. **Overflow decomposition** — `add_block` overflow may produce non-
   power-of-two sizes (consolidated blocks from multiple PAL allocs).
   `dealloc_overflow` uses `range_to_pow_2_blocks` to decompose before
   passing to parent, matching the existing pattern in
   `LargeBuddyRange::add_range`.

8. **`BackendArenaWordRef` lives in the test file** — production
   `PagemapRep` returns `BackendStateWordRef` directly (mirroring
   `BuddyChunkRep` in `largebuddyrange.h`). The test-only
   `BackendArenaWordRef` proxy is defined in
   `src/test/func/backend_arena/backend_arena.cc` and used only by
   MockRep, so production headers carry no test scaffolding.

9. **Power-of-two assertion retained** — `alloc_range` keeps
   `is_pow2(size)` for this phase. Non-power-of-two support is
   deferred to "Update front-end" step, when the bin scheme's
   alignment guarantees are verified end-to-end.

## Resolved during plan review

- Overflow handling: `add_block` can return non-power-of-two sizes when
  blocks from multiple PAL allocations consolidate. `dealloc_overflow`
  decomposes via `range_to_pow_2_blocks`. (Rubber-duck finding #2.)
- Handle visibility / layering: original plan promoted bit-layout
  constants and a `BackendArenaWordRef` proxy to namespace scope so
  production and test code could share them. Subsequent review
  observed that this broke the Buddy/`BuddyChunkRep`/`BuddyInplaceRep`
  layering: the data structure should be representation-agnostic.
  Resolved by making `BackendArena` carry no bit-layout state and
  requiring `Rep::BinRep` / `Rep::RangeRep` to own all packing
  decisions. Production `PagemapRep` keeps its layout private; the
  test `BackendArenaWordRef` lives in the test file alongside MockRep.
  (Rubber-duck finding #1, then revised after layering review.)
- Size shift overflow: `static_assert(MAX_CHUNKS_BITS + 8 <= BITS)` in
  `PagemapRep` prevents shift overflow. (Rubber-duck finding #4.)
- Unaligned refill guard: both static assert AND runtime assert copied
  from `LargeBuddyRange` to prevent `add_block` precondition violation.
  (Rubber-duck finding #6, strengthened in second review.)
- Pipeline integration (Phase 11) removed from this plan's scope —
  separate follow-up plan. (Rubber-duck finding #8.)
- `PagemapRep` templated on `MAX_CHUNKS_BITS` so the size-shift
  static_assert is in scope. (Second review finding #1.)
- `remove_block` exact-size guarantee is scoped to power-of-two
  requests only. (Second review finding #4.)

---

## Files added / changed (BackendArena phase, completed)

- New: `src/snmalloc/backend_helpers/backend_arena_bins.h` —
  `range_t`, `carve_t`, `carve`, `max_supported_chunks`, and nested
  `Bitmap` with `add` / `find_for_request` / `clear` (public surface);
  the size-class encoding (`bitmap_info_t`, `carve_info_t`, constexpr
  `BinTable`, `bitmap_info_for_request` / `carve_info_for_request`,
  `bin_index`) is private and reachable via
  `BackendArenaBinsTestAccess` (forward-declared in the header,
  defined in the test cc) for unit tests. Templated on
  `INTERMEDIATE_BITS` for testability.
- New: `src/snmalloc/backend_helpers/backend_arena.h` — the data structure,
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

No production code path is changed in this phase: the existing
`LargeBuddyRange` continues to be the active large-block allocator.

## Resolved during plan review

- One Bin tree per IDEA servable-set bin (not per size class or per
  exponent).
- Scope is the BackendArena data structure + tests only.
- The pagemap encoding carries a 2-bit **variant tag**
  (`Min` / `TwoMin` / `Large`) on the first entry of each free block.
  Tree membership — not the tag — is the source of truth for "is this
  block free?". No transient `BackendOwned` / "claimed" tag is required.
- **No pagemap probing.** All adjacency lookups are restricted to this
  `BackendArena`'s own RBTrees: non-min neighbours come from a single
  `Range.neighbours(addr_A)` walk that returns both
  `(largest < addr_A, smallest > addr_A)`; min-size neighbours come from
  `MinSizeBin.find(addr_A ± MIN_CHUNK_SIZE)`. The pagemap is never read
  at speculative addresses (concurrency hazard and no defined contract
  for pagemap entries the BackendArena does not own).
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
  contract. Oversize inputs (`size_chunks >= 2^MAX_CHUNKS_BITS`) bypass
  `BackendArena` entirely — the wrapping `BackendArenaRange` layer
  handles them before calling `add_block`, and `add_block` asserts
  `size_chunks < 2^MAX_CHUNKS_BITS`. The only overflow case is
  consolidation growing a coalesced block to exactly
  `2^MAX_CHUNKS_BITS` (the consolidated range is returned, neighbours
  having been removed first). The future `BackendArenaRange` wrapper is
  responsible for handling overflow; the standalone `BackendArena` only
  exposes the contract.
- `BackendArenaRep` is a chunk-keyed accessor concept (variant tag plus
  word/size accessors for entries 1–3). `BackendArena` builds two
  internal `RBTree`-Rep adapters (`BinRep`, `RangeRep`) over it; user
  code never sees the adapter shape.
- Backend chunk size classes are a new chunk-unit size-class scheme in
  `backend_arena_bins.h` (not bytes), independent of the
  power-of-two-only large variant of front-end `sizeclass_t`, with
  low-exponent special cases handled in the spirit of
  `bits::from_exp_mant`.
- `BackendArena<Rep, MIN_CHUNKS_BITS, MAX_CHUNKS_BITS>` uses chunk-count
  exponent bounds with **exclusive max** semantics, matching the existing
  `Buddy<..., MIN, MAX>`.
- Multi-`B` testing is via a templated bin-table generator in a single
  test binary, not via separate CMake configurations.
- Phase 5 verifies the reuse optimisation via Range-tree insert/remove
  *call counters* at the `BackendArena` layer (no `RBTree` modification).

## Still open (resolve during implementation)

- ~~Exact bit positions in the first-word pagemap encoding for the
  variant-tag field.~~ **Resolved** (Phase 3+4): bits 9–10 encode
  `BackendArenaVariant` (`VARIANT_MASK = 0x600`); bit 8 is `RED_BIT`;
  bits 0–7 are `BACKEND_RESERVED_MASK`. Documented in
  `backend_arena.h`.
- ~~Whether Bin tree roots are stored flat
  (`Array<Root, TOTAL_BINS>`) or exponent-keyed.~~ **Resolved**
  (Phase 3+4): flat `stl::Array<BinTree, Bins::Bitmap::TOTAL_BINS>`.
- Whether the future memcpy `offset` field is best placed in the second
  word of every pagemap entry, in dedicated entries, or in a side table.
  Out of scope for this phase; flagged for the memcpy-fix plan to design.
- Whether `INTERMEDIATE_BITS=4` (34 bins/exp) needs to be tested in this
  phase. Currently `B ∈ {1, 2, 3}` only.
