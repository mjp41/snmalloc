#!/usr/bin/env python3
"""
Exhaustive analysis of servable sets for snmalloc's size classes.

For every possible free block (offset, size) in a 512-chunk arena,
compute which snmalloc size classes can be allocated from that block,
respecting the natural alignment constraint:
  align(S) = S & ~(S-1)   (largest power of 2 dividing S)

A block at offset `a` with size `n` can serve size class `S` iff
there exists an address `x` within [a, a+n-S] such that x is a
multiple of align(S):
  first_aligned = ceil(a / align(S)) * align(S)
  servable iff first_aligned + S <= a + n
"""

ARENA = 512
B = 2  # INTERMEDIATE_BITS


def gen_size_classes(max_size):
    """Generate snmalloc size classes: S = 2^e + m * 2^{e-B}."""
    classes = set()
    classes.add(1)
    classes.add(2)
    classes.add(3)
    e = 2
    while True:
        base = 1 << e
        step = 1 << (e - B)
        for m in range(1 << B):
            s = base + m * step
            if s > max_size:
                break
            classes.add(s)
        if base > max_size:
            break
        e += 1
    return sorted(classes)


def natural_align(x):
    """Largest power of 2 dividing x. For 0, return a large value."""
    if x == 0:
        return 1 << 30
    return x & (-x)


def can_serve(addr, block_size, sizeclass):
    """Can a block at `addr` of `block_size` chunks serve `sizeclass`?"""
    A = natural_align(sizeclass)
    first_aligned = ((addr + A - 1) // A) * A
    return first_aligned + sizeclass <= addr + block_size


def get_exponent_mantissa(s):
    """Return (exponent, mantissa) for size class s with B=2."""
    if s == 1:
        return (0, 0)
    if s == 2:
        return (1, 0)
    if s == 3:
        return (1, 1)
    e = 2
    while True:
        base = 1 << e
        step = 1 << (e - B)
        for m in range(4):
            if base + m * step == s:
                return (e, m)
        e += 1
        if base > s * 2:
            return None


def main():
    size_classes = gen_size_classes(ARENA)
    print(f"Size classes: {size_classes}")
    print(f"Count: {len(size_classes)}")
    print()

    # Show alignment for each size class
    print("Size class alignments:")
    for sc in size_classes:
        em = get_exponent_mantissa(sc)
        print(f"  S={sc:>4d}  align={natural_align(sc):>4d}  (e={em[0]}, m={em[1]})")
    print()

    # ================================================================
    # Step 1: Compute ALL unique servable sets
    # ================================================================
    all_sets = {}  # frozenset -> list of (addr, size) examples
    for a in range(ARENA):
        for n in range(1, ARENA - a + 1):
            servable = frozenset(
                sc for sc in size_classes if can_serve(a, n, sc)
            )
            if servable not in all_sets:
                all_sets[servable] = []
            all_sets[servable].append((a, n))

    # Sort by (cardinality, max element)
    sorted_sets = sorted(
        all_sets.keys(), key=lambda s: (len(s), max(s) if s else 0)
    )

    print(f"Total unique servable sets: {len(sorted_sets)}")
    print()

    # ================================================================
    # Step 2: Show each unique servable set and its structure
    # ================================================================
    print("=" * 80)
    print("ALL UNIQUE SERVABLE SETS")
    print("=" * 80)
    for i, s in enumerate(sorted_sets):
        examples = all_sets[s][:3]
        ex_str = ", ".join(f"(a={a},n={n})" for a, n in examples)
        print(f"  #{i:>3d}  |{len(s):>3d} classes|  {sorted(s)}")
        print(f"         examples: {ex_str}")
    print()

    # ================================================================
    # Step 3: Analyse containment / subset structure
    # ================================================================
    print("=" * 80)
    print("CONTAINMENT ANALYSIS")
    print("=" * 80)
    print()
    print("For each set, what's new compared to its largest strict subset?")
    print("Incomparable pairs are sets where neither is a subset of the other.")
    print()

    for i, s in enumerate(sorted_sets):
        # Find strict subsets
        subsets = [sorted_sets[j] for j in range(len(sorted_sets)) if sorted_sets[j] < s]
        if subsets:
            biggest_subset = max(subsets, key=len)
            new = sorted(s - biggest_subset)
        else:
            new = sorted(s)

        # Find incomparable sets (same cardinality, neither subset)
        incomparable = []
        for j in range(len(sorted_sets)):
            other = sorted_sets[j]
            if other == s:
                continue
            if not (other < s) and not (other > s) and len(other) == len(s):
                incomparable.append(j)

        new_em = [(sc, get_exponent_mantissa(sc)) for sc in new]
        inc_str = f"  ** INCOMPARABLE with #{incomparable}" if incomparable else ""
        print(f"  #{i:>3d}: +{new}  {inc_str}")

    print()

    # ================================================================
    # Step 4: Group by exponent — show the 5-state structure
    # ================================================================
    print("=" * 80)
    print("PER-EXPONENT STRUCTURE")
    print("=" * 80)
    print()
    print("Within each exponent level, how many distinct states are there?")
    print("A 'state' is a distinct subset of {m=0, m=1, m=2, m=3} that")
    print("appears as the set of servable mantissas at that exponent.")
    print()

    max_exp = max(get_exponent_mantissa(sc)[0] for sc in size_classes)

    for e in range(2, max_exp + 1):
        # Size classes at this exponent
        sizes_at_e = []
        for m in range(4):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s <= ARENA:
                sizes_at_e.append((m, s))

        if not sizes_at_e:
            continue

        # For each servable set, extract which mantissas at exponent e are present
        mantissa_subsets = set()
        for s_set in sorted_sets:
            present = frozenset(
                m for m, sc in sizes_at_e if sc in s_set
            )
            if present:  # at least one mantissa servable
                mantissa_subsets.add(present)

        print(f"  Exponent e={e}: sizes {[s for _, s in sizes_at_e]}")
        print(f"    Distinct mantissa subsets: {len(mantissa_subsets)}")
        for ms in sorted(mantissa_subsets, key=lambda x: (len(x), sorted(x))):
            label = ""
            ms_sorted = sorted(ms)
            if ms_sorted == [0]:
                label = "A-only"
            elif ms_sorted == [1]:
                label = "B-only"
            elif ms_sorted == [0, 1]:
                label = "both"
            elif ms_sorted == [0, 1, 2]:
                label = "+m2"
            elif ms_sorted == [0, 1, 2, 3]:
                label = "+m3"
            else:
                label = "???"
            print(f"      mantissas {str(ms_sorted):20s}  ({label})")

        # Check for incomparable pairs
        for ms1 in mantissa_subsets:
            for ms2 in mantissa_subsets:
                if ms1 != ms2 and not ms1 < ms2 and not ms2 < ms1:
                    print(f"    ** Incomparable: {sorted(ms1)} vs {sorted(ms2)}")
        print()

    # ================================================================
    # Step 5: Show the threshold formula
    # ================================================================
    print("=" * 80)
    print("THRESHOLD ANALYSIS")
    print("=" * 80)
    print()
    print("T(S, alpha) = S + max(0, align(S) - alpha)")
    print("= minimum block size to serve S at block alignment alpha")
    print()

    for e in range(2, min(max_exp + 1, 6)):
        print(f"  Exponent e={e}:")
        for m in range(4):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s > ARENA:
                break
            a = natural_align(s)
            print(f"    m={m}: S={s:>4d}, align={a:>4d}", end="")
            # Show threshold at various block alignments
            alphas = [1, 2, 4, 1 << e]
            vals = []
            for alpha in alphas:
                t = s + max(0, a - alpha)
                vals.append(f"T(α={alpha})={t}")
            print(f"  {', '.join(vals)}")
        print()

    # ================================================================
    # Step 6: Verify the key property
    # ================================================================
    print("=" * 80)
    print("KEY PROPERTY VERIFICATION")
    print("=" * 80)
    print()
    print("Checking: servable sets are almost totally ordered.")
    print("For each exponent, there should be exactly 5 states")
    print("with exactly 1 incomparable pair ({m=0} vs {m=1}).")
    print()

    all_ok = True
    for e in range(2, max_exp + 1):
        sizes_at_e = []
        for m in range(4):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s <= ARENA:
                sizes_at_e.append((m, s))

        if len(sizes_at_e) < 4:
            continue

        mantissa_subsets = set()
        for s_set in sorted_sets:
            present = frozenset(m for m, sc in sizes_at_e if sc in s_set)
            if present:
                mantissa_subsets.add(present)

        n_states = len(mantissa_subsets)
        n_incomparable = 0
        for ms1 in mantissa_subsets:
            for ms2 in mantissa_subsets:
                if ms1 < ms2 or ms2 < ms1 or ms1 == ms2:
                    continue
                n_incomparable += 1
        n_incomparable //= 2  # each pair counted twice

        ok = (n_states == 5 and n_incomparable == 1)
        status = "OK" if ok else "FAIL"
        if not ok:
            all_ok = False
        print(f"  e={e}: {n_states} states, {n_incomparable} incomparable pairs  [{status}]")

    print()
    if all_ok:
        print("  ALL EXPONENTS HAVE EXACTLY 5 STATES WITH 1 INCOMPARABLE PAIR.")
    else:
        print("  SOME EXPONENTS DIFFER — check output above.")

    # ================================================================
    # Step 7: Show the two-bin split for m=1 with concrete examples
    # ================================================================
    print()
    print("=" * 80)
    print("THE TWO-BIN SPLIT: blocks of the same size go to different bins")
    print("=" * 80)
    print()
    print("For each exponent, m=1 blocks are split into two bins based on")
    print("whether they can also serve m=0 (the power-of-two size).")
    print()

    for e in range(2, min(max_exp + 1, 6)):
        s0 = 1 << e                       # m=0 size
        s1 = 5 * (1 << (e - B))           # m=1 size
        a0 = natural_align(s0)
        a1 = natural_align(s1)

        print(f"  Exponent e={e}: m=0 is size {s0} (align {a0}), "
              f"m=1 is size {s1} (align {a1})")

        # Find concrete blocks of size s1 that can/cannot serve s0
        bin_a_examples = []  # can serve both s0 and s1
        bin_b_examples = []  # can serve s1 but NOT s0

        for a in range(min(ARENA, 64)):
            if can_serve(a, s1, s1):
                if can_serve(a, s1, s0):
                    if len(bin_a_examples) < 3:
                        bin_a_examples.append((a, s1))
                else:
                    if len(bin_b_examples) < 3:
                        bin_b_examples.append((a, s1))

        # Show what each bin can serve
        if bin_a_examples:
            a_ex = bin_a_examples[0]
            servable = sorted(sc for sc in size_classes if can_serve(a_ex[0], a_ex[1], sc))
            ex_strs = ", ".join(f"(a={a},n={n})" for a, n in bin_a_examples)
            print(f"    Bin A (serves {s0} AND {s1}): e.g. {ex_strs}")
            print(f"      serves: {servable}")

        if bin_b_examples:
            b_ex = bin_b_examples[0]
            servable = sorted(sc for sc in size_classes if can_serve(b_ex[0], b_ex[1], sc))
            ex_strs = ", ".join(f"(a={a},n={n})" for a, n in bin_b_examples)
            print(f"    Bin B (serves {s1} but NOT {s0}): e.g. {ex_strs}")
            print(f"      serves: {servable}")
        print()


if __name__ == "__main__":
    main()
