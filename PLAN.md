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
only via the friend struct `BackendArenaBinsTestAccess<B>` (defined in
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

`BackendArena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>` takes **byte-size
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

- `BackendArena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>`:
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

#### PagemapRep

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

Templated on `Pagemap`, `MIN_SIZE_BITS`, and `MAX_SIZE_BITS` (mirroring
`Buddy`'s shape). `MIN_SIZE_BITS` is the log2 of the pagemap stride
(snmalloc's `MIN_CHUNK_BITS` when wired through `BackendArenaRange`);
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

    using PagemapRepT = PagemapRep<Pagemap, MIN_CHUNK_BITS, MAX_SIZE_BITS>;
    BackendArena<PagemapRepT, MIN_CHUNK_BITS, MAX_SIZE_BITS> arena;
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
non-Buddy ranges accept any chunk-aligned size, and `BackendArenaRange`
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

- `Aligned = true`: BackendArena's carving ensures that a request of
  size `n` (power-of-two, chunk-aligned) is placed at an `n`-aligned
  address within the source block. For non-power-of-two requests, the
  bin scheme's alignment rules still hold (alignment matches the
  lowest set bit of the size class).
- `ConcurrencySafe = false`: same as `LargeBuddyRange`.
- `ChunkBounds = capptr::bounds::Arena`: same as `LargeBuddyRange`.

### MAX_SIZE_BITS = BITS - 1 (global range)

The global `LargeBuddyRange` uses `MAX_SIZE_BITS = BITS - 1`, meaning
the buddy can hold up to half the address space. For BackendArenaRange:
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
   (red bit at bit 8 to match the PagemapRep layout).
7. MockRep keeps top-level `get_variant`/`set_variant`/large-size
   accessors and adds `can_consolidate(uintptr_t) → true`.
8. New test: verify that a MockRep variant with `can_consolidate`
   returning false at a specific address prevents consolidation across
   that boundary. Test both predecessor and successor merges being
   independently blocked.

**Test gate**: all existing BackendArena tests pass unchanged; new
boundary test passes.

### Phase 10: PagemapRep + BackendArenaRange + tests

**Status**: implemented and tested. Committed in `9c1ca745`.

> **Note**: the design notes below were written before Phase 10d
> (bytes-throughout). The as-built code uses byte sizes everywhere
> at the arena/range API and a unified `parent_dealloc(uintptr_t,
> size_t)` helper in place of the old `dealloc_overflow` /
> `parent_dealloc_range` pair. See the Phase 10d section for the
> current shape. Where the notes below say `size_chunks`, the
> implementation uses bytes; where they say `dealloc_overflow`, the
> implementation uses `parent_dealloc`.

**Phase 10b refactor (also implemented):** `BackendArena` and `PagemapRep`
were both retemplated to mirror `Buddy`'s 3-parameter shape:

- `template<typename Rep, size_t MIN_SIZE_BITS, size_t MAX_SIZE_BITS> class BackendArena`
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
- `BackendArenaRange::Type` wires snmalloc's `MIN_CHUNK_BITS` as
  `MIN_SIZE_BITS` for both PagemapRep and BackendArena:
  `PagemapRep<Pagemap, MIN_CHUNK_BITS, MAX_SIZE_BITS>` and
  `BackendArena<PagemapRepT, MIN_CHUNK_BITS, MAX_SIZE_BITS>`.

New file: `src/snmalloc/backend_helpers/backend_arena_range.h`

1. `PagemapRep<Pagemap, MIN_SIZE_BITS, MAX_SIZE_BITS>` — full Rep
   implementation using pagemap entries as described above, with all
   static assertions.
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

**Test gate**: BackendArenaRange tests pass; existing tests unaffected.

### Phase 11: Final review

Per `claude.md` mandatory review checkpoints:

- Spawn a fresh-context reviewer on the full diff (Phases 9–10).
- Address findings, loop until clean.

**Test gate**: full ctest run passes; reviewer reports no issues.

### Phase 10d: Bytes throughout (replace chunk-count internal API)

**Goal**: drop the `size_chunks` / chunk-count internal convention from
`BackendArena` and `PagemapRep` so byte sizes (multiples of UNIT_SIZE)
flow end-to-end, removing the `<< MIN_CHUNK_BITS` conversion dance at
the BackendArenaRange ↔ BackendArena boundary and the matching reverse
shifts inside the range wrapper.

**Substep 1 (DONE)**: generalise `BackendArenaBins` on a new
`MIN_SIZE_BITS` template parameter so its `range_t.size`, carve
arguments, and `max_supported_size()` are byte sizes (multiples of
`UNIT_SIZE = 1 << MIN_SIZE_BITS`). Renames inside Bins:
`size_chunks → size`, `align_chunks → align`, `max_supported_chunks
→ max_supported_size`. Tests cover `MIN_SIZE_BITS ∈ {0, 4, 14}`.

**Substep 2 (DONE)**: flip `BackendArena`, `PagemapRep`, and
`BackendArenaRange` to bytes throughout:
- `BackendArena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>` now uses
  `BackendArenaBins<B, MIN_SIZE_BITS>`; `add_block` / `remove_block`
  take/return bytes; `addr_to_chunk` / `chunk_to_addr` / `CHUNKS_BITS`
  deleted; `variant_of(size, addr)` works in byte units with
  parity from `(addr >> MIN_SIZE_BITS) & 1`.
- `remove_block(size)` returns a scalar `addr_t` (0 = failure). The
  size in the returned pair was tautological (always equal to the
  requested `size` on success).
- `PagemapRep::get_large_size` / `set_large_size` (renamed from
  `*_chunks`) take and return bytes; internal storage still scales
  by `MIN_SIZE_BITS` so the shifted field fits a pagemap word.
- `BackendArenaRange::add_range` / `dealloc_range` /
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
   via `>> 8`. Guarded by `static_assert((MAX_SIZE_BITS - MIN_CHUNK_BITS) + 8 <= bits::BITS)`.

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

7. **Overflow forwarding** — `add_block` overflow may produce non-
   power-of-two sizes (consolidated blocks from multiple PAL allocs).
   `dealloc_overflow` forwards the overflow directly to the parent's
   `dealloc_range`; no power-of-two decomposition is needed because
   `BackendArenaRange` (which is what replaces `LargeBuddyRange` in
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
  required because `BackendArenaRange` itself accepts arbitrary
  chunk-multiple sizes and replaces `LargeBuddyRange` in the pipeline.
  (Rubber-duck finding #2 superseded by Option B refactor.)
- Handle visibility / layering: original plan promoted bit-layout
  constants and a `BackendArenaWordRef` proxy to namespace scope so
  the in-tree header and tests could share them. Subsequent review
  observed that this broke the Buddy/`BuddyChunkRep`/`BuddyInplaceRep`
  layering: the data structure should be representation-agnostic.
  Resolved by making `BackendArena` carry no bit-layout state and
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

No in-tree code path is changed in this phase: the existing
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
  contract. Oversize inputs (`size_chunks >= 2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)`) bypass
  `BackendArena` entirely — the wrapping `BackendArenaRange` layer
  handles them before calling `add_block`, and `add_block` asserts
  `size_chunks < 2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)`. The only overflow case is
  consolidation growing a coalesced block to exactly
  `2^(MAX_SIZE_BITS - MIN_CHUNK_BITS)` (the consolidated range is returned, neighbours
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
- `BackendArena<Rep, MIN_SIZE_BITS, MAX_SIZE_BITS>` uses byte-size
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

---

# Phase 12: Update backend to use BackendArenaRange

## Status: implementation complete, awaiting commit approval

Substitution implemented and tested in the working tree (uncommitted on
top of `9c1ca745`). `BackendArena::add_block` had a latent
out-of-region pagemap-probe bug in its successor-min branch that
became reachable once `BackendArenaRange` started serving fixed-region
allocations; fixed in this phase (see "Issue found during Phase 12
test run" below). Full ctest suite passes (86/86).

Diff: 6 files, 183/45 +/- (PLAN.md, both pipeline range headers,
`backend_arena.h`, `backend_arena_bins.h`, `backend_arena.cc`).

## Goal

Replace every `LargeBuddyRange` instantiation in the range
pipelines with `BackendArenaRange`. After this phase, snmalloc uses
the BackendArena bin-tree allocator instead of the power-of-two buddy
for all large-range management. The `LargeBuddyRange` and
`BuddyChunkRep` classes are **not deleted** — they remain available
for alternative configurations and external embedders. Only the
default pipeline wiring changes.

## Scope

- Modify `standard_range.h` — replace all `LargeBuddyRange` with
  `BackendArenaRange` (same template parameters).
- Modify `meta_protected_range.h` — replace all `LargeBuddyRange`
  with `BackendArenaRange` (same template parameters).
- **No other source files change.** `BackendArenaRange` is already a
  drop-in replacement: same template signature, same `Type<Parent>`
  shape, same `alloc_range`/`dealloc_range` API, same `Aligned`,
  `ConcurrencySafe`, and `ChunkBounds` constants.

## Pre-conditions

- Phase 10 (BackendArenaRange) is committed and all its tests pass
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
→ `BackendArenaRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, MinSizeBits>`

- `MAX_SIZE_BITS = bits::BITS - 1` → global-range mode (no parent
  dealloc). `BackendArenaRange` handles this identically.
- `MIN_REFILL_SIZE_BITS = MinSizeBits` (Windows: 16, otherwise PAL-
  dependent). `BackendArenaRange` passes this through.
- Parent is `Base` (PalRange + PagemapRegisterRange chain). Parent is
  **unaligned** on PALs without `AlignedAllocation` (e.g. Linux mmap)
  and aligned otherwise. `BackendArenaRange::refill` currently still
  carries the aligned/unaligned dual path inherited from
  `LargeBuddyRange`; collapsing this into a single path is deferred to
  Phase 13.

**2. LargeObjectRange (local cache)**
```cpp
LargeBuddyRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>
```
→ `BackendArenaRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>`

- `MAX_SIZE_BITS = LocalCacheSizeBits = 21` (2 MiB). Non-global mode.
  Overflow goes to parent.
- `BackendArenaRange::parent_dealloc` forwards directly to parent
  without decomposition (single block returned by
  `BackendArena::add_block` when consolidation reaches the arena-scale
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
→ `BackendArenaRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap>`

- `MIN_REFILL_SIZE_BITS = 0` (default). Global-range mode.

**5. CentralMetaRange**
```cpp
LargeBuddyRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, page_size_bits>
```
→ `BackendArenaRange<GlobalCacheSizeBits, bits::BITS - 1, Pagemap, page_size_bits>`

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
→ Replace `LargeBuddyRange` with `BackendArenaRange` inside the
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
→ `BackendArenaRange<LocalCacheSizeBits, LocalCacheSizeBits, Pagemap, page_size_bits>`

- Same shape as standard_range.h #2.

**8. MetaRange (local)**
```cpp
LargeBuddyRange<LocalCacheSizeBits - SubRangeRatioBits, bits::BITS - 1, Pagemap>
```
→ `BackendArenaRange<LocalCacheSizeBits - SubRangeRatioBits, bits::BITS - 1, Pagemap>`

- `REFILL_SIZE_BITS = 21 - 6 = 15`. Global-range mode.
  `MIN_REFILL_SIZE_BITS = 0`.

## Implementation

The change is a mechanical text substitution — replace the string
`LargeBuddyRange` with `BackendArenaRange` in both files. No
template parameters, no API calls, no structural changes.

### Step 1: Replace LargeBuddyRange → BackendArenaRange

In `src/snmalloc/backend/standard_range.h`:
- 2 instantiations of `LargeBuddyRange<` (GlobalR, LargeObjectRange).

In `src/snmalloc/backend/meta_protected_range.h`:
- 6 instantiations of `LargeBuddyRange<` (GlobalR, CentralObjectRange,
  CentralMetaRange, the `conditional_t` huge-page cache,
  ObjectRange, MetaRange).

### Step 2: Verify include paths

Both files include `"../backend/backend.h"` which includes
`"../backend_helpers/backend_helpers.h"` which already includes
`"backend_arena_range.h"`. **No new includes needed.**

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
when `BackendArena::add_block` was called with a block whose
`succ_addr = addr + size` sat one chunk past the registered pagemap
range (the last 8 MiB of a 256 MiB FixedRange). The bug shape matches
the `buddy.h:90-93` comment exactly: `can_consolidate` reads the
pagemap entry at `succ_addr`, and that read is only safe once a
tree-membership test has confirmed the address is in our region.

**Fix.** In `BackendArena::add_block`, the successor-min branch was
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
production. A new test `test_block_at_arena_top_edge` adds a block
whose `succ_addr` sits one past the arena's pagemap; without the
reorder this test reproduces the original failure.

This unification also subsumed the previous `BoundaryMockRep` and its
`boundary_addrs` global `std::set`: the four boundary tests
(`test_boundary_blocks_predecessor`, `test_boundary_blocks_successor`,
`test_boundary_partial`, `test_boundary_blocks_min_predecessor`) now
run on `Arena<K>` and set `mock_store[mock_index(addr)].boundary = true`
instead. Net −35 lines in `backend_arena.cc`.

A leftover `throw "..."` in `backend_arena_bins.h:807` (used as a
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
the intent of collapsing `BackendArenaRange::refill`'s two-path
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

The BackendArena refactor (Phases 1–12) ends with Phase 12. No Phase 13.

## Risks

1. **BackendArenaRange behaviour differences.** The bin-tree allocator
   returns blocks with different internal fragmentation characteristics
   than the power-of-two buddy. Functionally, the caller always gets
   at least the requested size (power-of-two), so correctness is
   maintained. The arena may produce different carving patterns, but
   `alloc_range` always returns exactly the requested size.

2. **Overflow behaviour.** `LargeBuddyRange::dealloc_overflow` returns
   a single block of exactly `1 << MAX_SIZE_BITS`.
   `BackendArenaRange::parent_dealloc` forwards a single block of the
   consolidated size directly to the parent. The size can be any
   chunk multiple up to `2^MAX_SIZE_BITS`, not just power-of-two, but
   the parent (now itself a `BackendArenaRange` or pass-through layer)
   accepts arbitrary chunk-multiple sizes.

3. **`FixedRangeConfig` uses `StandardLocalState`.** The fixed-region
   configuration pushes memory directly into `GlobalR.dealloc_range`.
   This works with `BackendArenaRange` because `dealloc_range` has the
   same signature and contract.

4. **Pagemap metadata footprint.** `BackendArenaRange` uses up to
   three pagemap entries per free block (`backend_arena_range.h:12-17`)
   — one at the base, one at `base + UNIT_SIZE`, one at
   `base + 2*UNIT_SIZE`. `LargeBuddyRange`'s `BuddyChunkRep` only
   touched the base entry. Pagemap registration covers every
   `MIN_CHUNK_SIZE` stride for the full reserved address range
   (`pagemap.h:60-65`), so this is safe in the in-tree pipeline, but
   external embedders with custom Pagemap implementations should
   verify their pagemap entries cover the per-unit stride.

## Resolved during plan review

- `backend_arena_range.h` was missing `#include "empty_range.h"` for
  its `EmptyRange<>` default template parameter. Fixed pre-commit.
  (Rubber-duck finding #2.)
- The `conditional_t` huge-page path in `meta_protected_range.h` may
  not be instantiated on default builds. CI tests multiple PAL
  configurations. Risk acknowledged but no custom build added — the
  conditional branch is structurally identical to other
  `BackendArenaRange` uses and shares the same template. (Rubber-duck
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

No production behaviour changes yet: the front-end still calls
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

- `backend_arena_range.h:42-50`: `RED_BIT_POS = 8`,
  `VARIANT_SHIFT = 9`, `LARGE_SIZE_SHIFT = 8`. Today these sit at
  bits 8/9-10/8. After Phase 13 they shift to bits 9/10-11/9. The
  `static_assert(Entry::is_backend_allowed_value(...))` at
  `backend_arena_range.h:64-66` catches any miss at compile time.
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
  `backend_arena_bins.h:741`). For pow2 sizes, `info.align == size`,
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
   * `backend_arena_range.h` and `largebuddyrange.h` to derive
   * RED_BIT_POS, VARIANT_SHIFT, and LARGE_SIZE_SHIFT.
   */
  static constexpr size_t BACKEND_LAYOUT_FIRST_FREE_BIT =
    bits::next_pow2_bits_const(REMOTE_BACKEND_MARKER) + 1;
  ```
  The `+1` reserves `REMOTE_BACKEND_MARKER`'s own bit (it lives at
  `next_pow2_bits_const(REMOTE_BACKEND_MARKER)`).

### `src/snmalloc/backend_helpers/backend_arena_range.h` and `src/snmalloc/backend_helpers/largebuddyrange.h`

- Replace hard-coded `RED_BIT_POS = 8`, `VARIANT_SHIFT = 9`,
  `LARGE_SIZE_SHIFT = 8` in `backend_arena_range.h` with
  derivations from the new public
  `MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT`:
  `RED_BIT_POS = MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT;`
  `LARGE_SIZE_SHIFT = MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT;`
  `VARIANT_SHIFT = MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT + 1;`
  (the `+1` reserves the RED bit).
- `backend_arena_range.h:64-66` `static_assert` continues to enforce
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
   `backend_arena_range.h:64-66` catch any bit-layout mismatch.
2. **Full ctest suite**: all existing tests pass (no behaviour
   regression — front-end still issues pow2 large requests, so
   non-pow2 large sizeclasses exist in tables but are unreachable
   from the API).
3. **BackendArena unit tests** (`test_backend_arena`) continue to
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
   `backend_arena_range.h:64-66`.
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

# Phase 14: Per-chunk pagemap offset (slab-granular)

## Goal

Add a per-chunk "slab offset within allocation" field to
`FrontendMetaEntry`, written by a new `set_metaentry_large` path,
and use it in `start_of_object` / `is_start_of_object` so that the
start of a large allocation can be recovered from any address
within it — independent of the allocation's alignment. This unlocks
Phase 15.

After Phase 14, the start-finding code uses the per-chunk offset
for large allocations and continues to use `slab_mask` for small.
With the front-end still issuing pow2 large requests (Phase 15
changes that), every materialised large allocation has
`info.align == size` so `slab_mask = size - 1` covers the whole
allocation with offset always 0 — exactly today's behaviour.

## Why now

- Phase 15 introduces non-pow2 reservations. The existing
  `addr & ~slab_mask` answer is wrong for non-pow2 sizes/alignments.
- Per-chunk offset is the mechanism PLAN.md (lines 65-71) already
  identified. Phase 14 implements that mechanism with offset = 0
  semantics matching the existing pow2 path — so it can land
  without changing observable behaviour.

## Design

### Slab granularity, not chunk granularity

The offset records "which slab within the allocation does this
chunk belong to", in units of the per-sizeclass `slab_size`. The
recovery formula (matching PLAN.md lines 65-71) is:

```
start = (addr & ~slab_mask) - offset * slab_size
```

where `slab_size = info.align` (the natural alignment from
`backend_arena_bins.h:741`, `info.align = size & (~size + 1)`,
i.e. the lowest set bit of `size`), and `slab_mask = slab_size - 1`.
Both are per-sizeclass, stored in `sizeclass_data_fast` as
`slab_mask` (already there, value changes per Phase 13).

**Offset width.** With `INTERMEDIATE_BITS = 2`, a large sizeclass
of size `S = (4+M) * 2^(E-2)` for `M ∈ {0,1,2,3}` has:

| M | size factor | info.align (lowest set bit) | slabs (size/align) |
|---|-------------|------------------------------|---------------------|
| 0 | 4           | 2^E (= size)                 | 1                   |
| 1 | 5           | 2^(E-2)                      | 5                   |
| 2 | 6           | 2^(E-1)                      | 3                   |
| 3 | 7           | 2^(E-2)                      | 7                   |

Worst case `2^(M+1) - 1` slabs (M = `INTERMEDIATE_BITS`): for M=2,
the table above shows 7 slabs. Generalising: `OFFSET_BITS = M + 1`
gives the needed `2^(M+1)` distinct values. A `static_assert` in
`metadata.h` guards the bound:
`static_assert((1 << OFFSET_BITS) > max_slabs_in_largest_class)`.

(With natural alignment, the allocation incurs no address-space
waste beyond what alignment already implies. With computed
`OFFSET_BITS = INTERMEDIATE_BITS + 1`, we accept the extra
`meta`-word bit consumption to keep allocations at natural
alignment.)

### Offset is a frontend concept (layering)

Per user clarification: the offset is owned by the frontend (used
to recover start-of-object from an interior pointer); the boundary
bit is owned by the backend (used to mark PAL-allocation boundaries
for the buddy allocator).

Both bits happen to live in the `meta` word of the pagemap entry,
but they are conceptually disjoint:

- Offset accessors live on `FrontendMetaEntry`, not on
  `MetaEntryBase`. The boundary bit machinery
  (`MetaEntryBase::set_boundary`, `clear_boundary_bit`, `is_boundary`)
  must not clobber offset bits — and currently doesn't, because
  it only `|=` / `&= ~` the single boundary bit at position 0.
- The frontend's `set_metaentry_large` packs offset into `meta`
  and must preserve the boundary bit. **Key observation**:
  `MetaEntryBase::operator=` at `metadata.h:162-169` *already*
  preserves the target's boundary bit on assignment. So writing a
  freshly-constructed `Entry t_i(meta, ras)` (with offset and
  boundary both zero), calling `set_offset(slab_index)` on it (RMW
  that touches only OFFSET bits — boundary on `t_i` is still 0),
  then `concretePagemap.set(addr, t_i)` (which assigns via
  `operator=`) leaves the pagemap entry's pre-existing boundary
  bit intact. No manual boundary-preservation logic is needed.
- `FrontendMetaEntry::get_slab_metadata()` (currently at
  `metadata.h:739-740` masks `meta & ~META_BOUNDARY_BIT`) must
  also mask the offset bits. The simplest way: extend the existing
  mask constant. Define `META_FRONTEND_RESERVED_MASK =
  META_BOUNDARY_BIT | (((1 << OFFSET_BITS) - 1) << OFFSET_SHIFT)`
  and mask with that everywhere `get_slab_metadata` needs the
  pointer.

### Where the offset lives in the `meta` word

Bits `1..OFFSET_BITS` of `meta` (with `OFFSET_SHIFT = 1`):

- Bit 0: `META_BOUNDARY_BIT` (backend-owned).
- Bits `1..OFFSET_BITS`: offset (frontend-owned, large-only).
- Bits `(1 + OFFSET_BITS)..`: `SlabMetadata*` payload (natural
  pointer alignment).

This requires `alignof(SlabMetadata) >= (1 << (1 + OFFSET_BITS))`.
For default `INTERMEDIATE_BITS=2`, `OFFSET_BITS=3`, the requirement
is `alignof(SlabMetadata) >= 16`. Inspect `SlabMetadata` at the top
of Phase 14; if alignment is insufficient, add
`alignas(1 << (1 + OFFSET_BITS))` (= `alignas(16)` for default) to
`SlabMetadata`. Cost: a few bytes of padding per slab metadata
record — negligible.

### Accessors

Add to `FrontendMetaEntry`:

- `static constexpr size_t OFFSET_BITS = INTERMEDIATE_BITS + 1;`
  (derives from `INTERMEDIATE_BITS` because the worst-case slab
  count for a non-pow2 large class with M mantissa bits is
  `2^M + (2^M - 1) = 2^(M+1) - 1`. For default `INTERMEDIATE_BITS=2`
  this gives `OFFSET_BITS = 3` (max offset 7, matching a worst case
  of 7 slabs). For `INTERMEDIATE_BITS=3` (config option) it gives
  `OFFSET_BITS = 4` (max offset 15, matching a worst case of 15
  slabs).)
- `static constexpr size_t OFFSET_SHIFT = 1;` (immediately above
  the boundary bit)
- `static constexpr address_t OFFSET_MASK =
   ((1 << OFFSET_BITS) - 1) << OFFSET_SHIFT;`
- `void set_offset(size_t slab_offset)`: read-modify-write of
  `meta`, preserving boundary bit and `SlabMetadata*` payload.
  Asserts `slab_offset < (1 << OFFSET_BITS)`.
- `size_t get_offset() const`: reads `(meta & OFFSET_MASK) >>
  OFFSET_SHIFT`.

Update the existing pointer mask: define
`META_FRONTEND_RESERVED_MASK = META_BOUNDARY_BIT | OFFSET_MASK`,
update `get_slab_metadata()` to mask `meta & ~META_FRONTEND_RESERVED_MASK`.

A `static_assert(alignof(SlabMetadata) >= (1 << (OFFSET_BITS +
OFFSET_SHIFT)))` enforces the pointer-alignment requirement at
compile time. For the default config this requires
`alignof(SlabMetadata) >= 16`. Verify the current value and add
`alignas(16)` (or computed `alignas(1 << (OFFSET_BITS+OFFSET_SHIFT))`)
to `FrontendSlabMetadata` if needed.

### `Pagemap::set_metaentry` (split into small vs large)

The existing `set_metaentry` (writes uniform entries per chunk in
a range) is a static member of `BasicPagemap` in
`backend_helpers/pagemap.h:56-66`, which uses
`concretePagemap.set(...)` to reach the underlying `FlatPagemap`.
The new `set_metaentry_large` is added as a static member alongside
it.

`FrontendMetaEntry` deletes its copy constructor (`metadata.h:754`),
so we cannot use `Entry t_i = t;` and modify per chunk. Instead,
reconstruct each per-chunk entry from its components:

```cpp
// In BasicPagemap, alongside set_metaentry:
static void set_metaentry_large(
  address_t p,
  size_t size,
  size_t slab_size,
  SlabMetadata* meta,
  uintptr_t remote_and_sizeclass)
{
  // slab_size = info.align of this sizeclass.
  // size      = total allocation size (== sizeclass-encoded size).
  for (size_t chunk_offset = 0; chunk_offset < size;
       chunk_offset += MIN_CHUNK_SIZE)
  {
    size_t slab_index = chunk_offset / slab_size;
    Entry t_i(meta, remote_and_sizeclass);  // meta low bits = 0
    t_i.set_offset(slab_index);             // RMW; touches only OFFSET bits
    concretePagemap.set(p + chunk_offset, t_i);
  }
}
```

**Boundary-bit preservation**: `MetaEntryBase::operator=` at
`metadata.h:162-169` already preserves the *target's* boundary bit
when copy-assigning from `other`. `FlatPagemap::set` uses `=` to
write entries. Therefore: the freshly-constructed `t_i` carries
`boundary = 0`, but when it is assigned into the pagemap slot, the
slot's pre-existing boundary bit (set earlier by the backend's
`register_range`) is preserved by `operator=`. No manual
boundary-preservation logic is needed in this loop.

**Backend call site** (`backend.h:131-132`): dispatch on
`sizeclass.is_small()`. Small path keeps existing
`Pagemap::set_metaentry(p, size, t)`. Large path:
`Pagemap::set_metaentry_large(p, size,
                              sizeclass_data_fast(sc).slab_mask + 1,
                              meta, ras);`
where `meta` and `ras` are the `SlabMetadata*` and
`remote_and_sizeclass` values currently passed to the `Entry t(meta,
ras)` construction at `backend.h:131`.

### `start_of_object` / `is_start_of_object`

The current `start_of_object` lives in `sizeclasstable.h` with no
Pagemap access. After Phase 14, the large case needs the per-chunk
offset — which lives in the pagemap.

Split the function: keep `sizeclasstable.h`'s `start_of_object` as
the small-case implementation (rename internally to
`start_of_object_small` if helpful), and add a Config-aware wrapper
in `globalalloc.h` (or `mem/start_of_object.h`):

```cpp
template<typename Config>
inline address_t start_of_object(address_t addr) {
  // Use the existing public BackendAllocator accessor (see
  // backend.h:197) instead of reaching for `Config::Backend::Pagemap`
  // directly — `Pagemap` is a template parameter of `BackendAllocator`,
  // not a publicly exposed nested type. The public
  // `get_metaentry<bool potentially_out_of_range>(addr)` static
  // wraps the Pagemap access.
  auto& entry = Config::Backend::template get_metaentry<false>(addr);
  auto sc = entry.get_sizeclass();
  if (sc.is_small()) {
    auto info = sizeclass_data_fast(sc);
    return start_of_object_small(info, addr);
  }
  // Large: PLAN.md (65-71) recovery.
  auto info = sizeclass_data_fast(sc);
  size_t slab_size = info.slab_mask + 1;
  return (addr & ~info.slab_mask) - entry.get_offset() * slab_size;
}
```

### Consumers that MUST be rewritten in Phase 14

Phase 14 is incomplete until every caller of the
sizeclass-table-only `start_of_object` / `is_start_of_object` /
`remaining_bytes` on a potentially-large pointer is moved to the
Config-aware wrapper:

- `globalalloc.h:137-144` (`remaining_bytes`): currently calls
  `snmalloc::remaining_bytes(sizeclass, p)` which has no pagemap
  offset access. Replace with the Config-aware path that consults
  the pagemap entry for offset and computes
  `start + sizeclass_full_to_size(sc) - addr`.
- `globalalloc.h:145-220` (`index_in_object`, `external_pointer`):
  similarly rewrite to consult the pagemap.
- `corealloc.h` deallocation-sanity checks: `is_start_of_object`
  is used in dealloc paths to assert the caller is passing a
  valid base pointer. Grep `is_start_of_object` and `start_of_object`
  across `corealloc.h` (verified candidates at
  `corealloc.h:534-537` and `corealloc.h:1080-1083` per
  rubber-duck review). Each call site that may receive a large
  allocation's pointer must use the Config-aware variant. Without
  this update, after Phase 15 a `dealloc` of a non-pow2 large
  allocation could miss the start-of-object check entirely (every
  natural-alignment slab boundary inside the allocation would
  satisfy the old `slab_mask`-only check).
- `bounds_checks.h` memcpy gate (line 99-103): calls
  `remaining_bytes(...)`. Moves to the Config-aware version
  transitively via the `globalalloc.h::remaining_bytes` rewrite.

`is_start_of_object` analogue: for small, today's formula; for
large, `(addr & info.slab_mask) == 0 && entry.get_offset() == 0`.

`slab_index` for large: irrelevant — large allocations are a single
"object" of size `sizeclass_full_to_size(sc)`, not a slab of
multiple. Existing callers gated by `sc.is_small()` already avoid
calling `slab_index` for large.

### Backend changes

- `backend.h:131-132`: at the `set_metaentry` call site after a
  large `alloc_chunk`, dispatch on small vs large as above. Phase
  14 keeps `alloc_chunk`'s `bits::is_pow2(size)` assertion (Phase
  15 relaxes it). This is fine: today only pow2 large allocations
  reach this site, so `slab_size == size` and offset is always 0.
- `backend.h:169` (dealloc): writes backend-claim entries via the
  backend Rep's word setters; those don't touch frontend bits.
  No change.

## Test gates

1. **Build**: clean build passes.
2. **Full ctest suite**: all existing tests pass. Front-end still
   issues pow2 large requests, so for every materialised large
   allocation `info.align == size` and offset is always 0 — the
   new `set_metaentry_large` path produces the same `get_slab_metadata()`
   answer as before. Existing `start_of_object` answers (via
   `slab_mask`) match the new offset-based answers for pow2-aligned
   allocations.
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
       `Pagemap::get_metaentry(p + k * MIN_CHUNK_SIZE).get_offset()
       == 0` (since the reservation is pow2 and `slab_size ==
       reservation_size` for pow2 large classes, all chunks live in
       the single slab and have offset 0).
     - For every interior address `q = p + j` with `j ∈ {0, 1,
       S_res/2, S_res-1}`, assert `start_of_object<Config>(q) == p`.
5. **New test or extension** to exercise the non-zero offset write
   path directly (Phase 14 is otherwise un-tested with non-zero
   offsets, because the front-end is still pow2-only). Two options:
   - (a) Add an internal-API test in
     `src/test/func/large_offset/large_offset.cc` (or extend
     `memory.cc`) that calls `BasicPagemap::set_metaentry_large`
     directly on a freshly-allocated chunk-multiple range with a
     synthetic non-pow2 sizeclass (one already populated in the
     table in Phase 13). Then verify:
     - `get_metaentry(p + k * MIN_CHUNK_SIZE).get_offset() == k *
       MIN_CHUNK_SIZE / slab_size` for each chunk.
     - `start_of_object<Config>(p + interior_addr) == p` for a
       sample of interior addresses across all slabs.
   - (b) Defer non-zero offset coverage to Phase 15 explicitly and
     accept that Phase 14's gate is "no regressions on
     pow2-allocation paths".
   The plan picks (a) — Phase 14 must be independently testable.
6. **Boundary-bit-preservation test**: in an existing test that
   exercises PAL-allocation boundaries (or a new minimal one), set
   the boundary bit on a chunk via the backend path, then call
   `set_offset(3)` on the frontend side, then read both — both
   round-trip without clobbering each other.

## Risks

1. **`alignof(SlabMetadata)` insufficient.** Required alignment is
   `1 << (1 + OFFSET_BITS)` — 16 bytes for default config. If
   inspection shows alignment is smaller (likely 8 today), add
   `alignas(1 << (1 + OFFSET_BITS))`. Caught at compile time by the
   new `static_assert`.
2. **`get_slab_metadata` mask update missed somewhere.** Grep for
   `META_BOUNDARY_BIT` and `meta &` to find every site that
   masks the meta word for a pointer. Convert each to the new
   `META_FRONTEND_RESERVED_MASK`.
3. **Offset-bit positions overlap with backend bits when the entry
   is backend-claimed.** Not a real risk: when the backend writes
   its claim, the entry's `meta` is owned by the backend Rep
   (different layout). Frontend reads `get_offset()` only on
   frontend-claimed entries.
4. **Boundary bit not preserved during `set_offset`.** Mitigation:
   implement `set_offset` as RMW preserving all bits except the
   offset field. Test case: set boundary, set offset, read offset,
   read boundary — both round-trip.

## Out of scope

- Front-end requesting non-pow2 large sizes (Phase 15).
- Per-chunk offset for small allocations (small uses slab_mask
  recovery, no per-chunk offset needed).
- Multi-byte offset (`OFFSET_BITS = INTERMEDIATE_BITS + 1` bits,
  fits cleanly in `meta` low bits).

# Phase 15: Front-end requests non-pow2 large allocations

## Goal

Flip the front-end so that large allocations request exactly the
sizeclass-encoded size (chunk-multiple, exp+mantissa-rounded),
instead of always the next power of two. This is the long-running
goal of the refactor: the backend (`BackendArenaRange`) has
supported arbitrary chunk-multiple sizes since Phase 10–12, the
sizeclass encoding has supported non-pow2 large since Phase 13,
and the per-chunk offset machinery has supported pointer recovery
since Phase 14.

## Changes

### `src/snmalloc/ds/sizeclasstable.h`

- `large_size_to_chunk_size(size)`: replace
  `bits::next_pow2(size)` with the rounded sizeclass-derived size:
  `sizeclass_full_to_size(size_to_sizeclass_full(size))`. Now
  rounds to exp+mantissa boundaries (matching Phase 13 encoding).
- `round_size(size)` for large (lines 478-501): currently returns
  `bits::next_pow2(size)`. Update to match `large_size_to_chunk_size`:
  `return sizeclass_full_to_size(size_to_sizeclass_full(size));`
  This is critical because `DefaultConts::success` in
  `corealloc.h:34-47` uses `round_size` to determine the zeroing
  range for `calloc`. Without this update, `calloc` would zero
  beyond the actual reservation. The two functions converge to
  the same value now that the front-end's chunk-size request
  matches the round-size.
- Update the comments on both functions to describe the new
  rounding behaviour (no "next pow2"; "exp+mantissa rounded").

### `src/snmalloc/backend/backend.h`

- `alloc_chunk` (line 89-95):
  `SNMALLOC_ASSERT(bits::is_pow2(size))` → relaxed to
  `SNMALLOC_ASSERT((size & (MIN_CHUNK_SIZE - 1)) == 0)`.
  (Already permissible per Phase 14; tighten only if Phase 14
  did not relax it.)
- `meta_size = bits::next_pow2(sizeof(SlabMetadata) + extra_bytes);`
  unchanged — that's metadata-array size, not allocation size.

### `src/snmalloc/mem/corealloc.h`

- Verify line 1576 (and any other `next_pow2(round_sizeof)` site)
  — read context and update to match the new rounding scheme if
  it's on the large-allocation path.
- The dealloc-large path was already migrated in Phase 13 to
  `sizeclass_full_to_size(entry.get_sizeclass())` — no further
  change needed.
- The front-end large-alloc path (corealloc.h:703-727) uses
  `large_size_to_chunk_size` — automatically picks up the new
  behaviour.

### `src/snmalloc/mem/smallbuddyrange.h:232`

- `auto rsize = bits::next_pow2(size);` inside
  `alloc_range_with_leftover` is used only by the meta-data range
  (and arguably the small object path). Read context to determine
  scope. Likely no change in Phase 15; Phase 15 only touches large
  object allocations. If a change is required, include it here;
  if not, document the decision.

## Test gates

1. **Build**: clean build passes.
2. **Full ctest suite**: all existing tests pass. Existing tests
   that exercise large allocations now allocate chunk-multiples,
   not pow2 sizes. Reservation footprint shrinks; functional
   results are unchanged.
3. **Extend `src/test/func/memcpy/func-memcpy.cc`** with a
   non-pow2 large case:
   - For sizes `S` strictly between adjacent pow2 (e.g. `S =
     1.5 * MAX_SMALL_SIZECLASS_SIZE`), call `malloc(S)`. Verify:
     - `memcpy(p + sizeclass_full_to_size(sc) - 1, src, 1)` succeeds.
     - `memcpy(p + sizeclass_full_to_size(sc), src, 1)` traps (in
       the bounds-checking variant).
     - **Prerequisite**: Phase 14 must have already replaced
       `globalalloc.h::remaining_bytes` with the Config-aware
       pagemap-offset path. Without that prerequisite, this test
       does not exercise the offset path. (Verify by inspection:
       confirm the new `remaining_bytes` consults
       `entry.get_offset()`, not just `start_of_object_small`.)
4. **Extend existing `test/func/memory/memory.cc`** with a
   non-pow2 pointer-recovery case (mirroring the Phase 14 test but
   on front-end-issued non-pow2 allocations):
   - For sizes `S` strictly between adjacent pow2 in the large
     range, call `malloc(S)`, save `p`. Compute
     `S_rounded = sizeclass_full_to_size(size_to_sizeclass_full(S))`.
   - For every interior address `q = p + j` with `j ∈ {0, 1,
     MIN_CHUNK_SIZE, S_rounded / 2, S_rounded - 1}`, assert
     `external_pointer<Start>(q) == p`.
   - Assert `is_start_of_object(p)` is true; `is_start_of_object(p
     + 1)` is false; `is_start_of_object(p + MIN_CHUNK_SIZE)` is
     false (every interior chunk has offset != 0).
   - Assert reservation footprint matches `S_rounded /
     MIN_CHUNK_SIZE` chunks (NOT `next_pow2(S) / MIN_CHUNK_SIZE`).
5. **Extend `src/test/func/release-rounding/rounding.cc`** to cover
   non-pow2 large sizeclasses now that they're materialised
   end-to-end.
6. **Existing memory-stress tests** (e.g. `external_pointer.cc`)
   continue to pass.

## Risks

1. **Existing tests assume pow2 reservation footprint.** Grep tests
   for `next_pow2`, `pow2`, and any size-arithmetic over allocations
   returned from `malloc`. Likely small handful; convert each to
   `sizeclass_full_to_size` or to a less assumption-laden check.
2. **`calloc` zeroing range.** Mitigated by updating `round_size`
   for large (item above). Verify by inspecting
   `corealloc.h:34-47` (`DefaultConts::success`) — it should now
   zero exactly the reservation size.
3. **`SlabMetadata` reuse boundary.** The current
   `slab_metadata == &slab_metadata` assertion in `dealloc_chunk`
   relies on every chunk in the allocation pointing to the same
   `SlabMetadata`. Phase 14's per-chunk-offset path keeps the
   `meta` field's pointer bits unchanged across chunks (only
   offset differs), so the assertion continues to hold after the
   Phase 14 mask update. Verify by re-reading the assertion site.
4. **`remaining_bytes` overflow for very large allocations.**
   After Phase 15, the rounded size can be just below the next
   pow2, which is still bounded by `2^MAX_address_bits` — no
   arithmetic overflow. Verify with a max-size allocation test.
5. **Performance.** Front-end alloc path: `next_pow2` is replaced
   by an exp+mantissa table lookup. Dealloc-large already moved
   to a table lookup in Phase 13. Net neutral.

## Out of scope

- Reducing `INTERMEDIATE_BITS` to gain bits in the sizeclass tag
  (Phase 13 already chose 2 = the existing value).
- Generalising small allocations (already exp+mantissa).
- Any change to `alloc_range` / `dealloc_range` of arbitrary
  byte-multiples — front-end always rounds via the sizeclass
  encoding.

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
