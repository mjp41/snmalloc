#!/usr/bin/env python3
"""
Analyse the "skip" structure for different INTERMEDIATE_BITS values.

Core question: when searching upward through bins for a size class,
how many bins do you need to skip (bins that serve a larger size but
not the one you want, due to alignment)?
"""

ARENA = 1024


def natural_align(x):
    if x == 0:
        return 1 << 30
    return x & (-x)


def can_serve(addr, block_size, sizeclass):
    A = natural_align(sizeclass)
    first_aligned = ((addr + A - 1) // A) * A
    return first_aligned + sizeclass <= addr + block_size


def gen_size_classes(B, max_size):
    classes = set()
    classes.add(1)
    if B >= 1:
        classes.add(2)
    if B >= 2:
        classes.add(3)
    if B >= 3:
        for s in range(4, 8):
            if s <= max_size:
                classes.add(s)
    e = B
    while True:
        base = 1 << e
        step = 1 << (e - B)
        for m in range(1 << B):
            s = base + m * step
            if s <= max_size:
                classes.add(s)
        if base > max_size:
            break
        e += 1
    return sorted(classes)


def analyse(B):
    M = 1 << B  # mantissas per exponent
    size_classes = gen_size_classes(B, ARENA)
    print(f"{'='*80}")
    print(f"INTERMEDIATE_BITS = {B}  ({M} mantissas per exponent)")
    print(f"{'='*80}")
    print(f"Size classes: {size_classes[:30]}{'...' if len(size_classes)>30 else ''}")
    print()

    # Show alignment pattern for one exponent
    e = max(B + 2, 4)  # pick an exponent where sizes aren't tiny
    print(f"  Alignment pattern at exponent e={e}:")
    sizes_at_e = []
    for m in range(M):
        step = 1 << (e - B)
        s = (1 << e) + m * step
        a = natural_align(s)
        sizes_at_e.append((m, s, a))
        print(f"    m={m}: size={s:>4d}  align={a:>4d}  (coefficient {s >> (e-B)} = {s // (1 << (e-B))})")
    print()

    # Compute all unique servable sets
    all_sets = set()
    for a in range(ARENA):
        for n in range(1, ARENA - a + 1):
            servable = frozenset(
                sc for sc in size_classes if can_serve(a, n, sc)
            )
            all_sets.add(servable)

    sorted_sets = sorted(all_sets, key=lambda s: (len(s), max(s) if s else 0))

    # Per-exponent analysis
    max_exp = 1
    for sc in size_classes:
        ee = B
        while (1 << ee) <= sc:
            ee += 1
        ee -= 1
        if ee >= B:
            max_exp = max(max_exp, ee)

    print(f"  Per-exponent mantissa state analysis:")
    print()

    for e in range(B, max_exp + 1):
        sizes_at_e = []
        for m in range(M):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s <= ARENA:
                sizes_at_e.append((m, s))

        if len(sizes_at_e) < M:
            continue

        # For each servable set, extract which mantissas at this exponent are present
        mantissa_subsets = set()
        for s_set in sorted_sets:
            present = frozenset(m for m, sc in sizes_at_e if sc in s_set)
            if present:
                mantissa_subsets.add(present)

        # Count incomparable pairs
        incomparable_pairs = []
        ms_list = sorted(mantissa_subsets, key=lambda x: (len(x), sorted(x)))
        for i, ms1 in enumerate(ms_list):
            for ms2 in ms_list[i+1:]:
                if not ms1 < ms2 and not ms2 < ms1:
                    incomparable_pairs.append((sorted(ms1), sorted(ms2)))

        print(f"  Exponent e={e}: sizes {[s for _, s in sizes_at_e]}")
        print(f"    {len(mantissa_subsets)} distinct states, {len(incomparable_pairs)} incomparable pair(s)")

        for ms in sorted(mantissa_subsets, key=lambda x: (len(x), sorted(x))):
            print(f"      {sorted(ms)}")

        if incomparable_pairs:
            for p in incomparable_pairs:
                print(f"    ** Incomparable: {p[0]} vs {p[1]}")
        print()

    # The key analysis: for each size class, which bins must be SKIPPED?
    print(f"  SKIP ANALYSIS: when searching for size S, which larger-size bins")
    print(f"  might contain blocks that can't serve S?")
    print()

    for e in range(B, min(max_exp + 1, B + 4)):
        sizes_at_e = []
        for m in range(M):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s <= ARENA:
                sizes_at_e.append((m, s))

        if len(sizes_at_e) < M:
            continue

        print(f"  Exponent e={e}:")

        for m_req, s_req in sizes_at_e:
            a_req = natural_align(s_req)

            # For each larger size at same exponent, check if it can always serve s_req
            skips = []
            for m_other, s_other in sizes_at_e:
                if s_other <= s_req:
                    continue
                # Can a block of size s_other sometimes NOT serve s_req?
                # Check: at worst alignment for s_req, does s_other still suffice?
                # T(s_req, alpha=1) = s_req + align(s_req) - 1
                # The block serves s_req if block_size >= T(s_req, block_align)
                # A block of size s_other could have any alignment
                can_always = True
                can_sometimes_not = False
                for addr in range(min(ARENA, 64)):
                    if can_serve(addr, s_other, s_other):  # valid block
                        if not can_serve(addr, s_other, s_req):
                            can_sometimes_not = True
                            break

                if can_sometimes_not:
                    skips.append((m_other, s_other))

            if skips:
                skip_str = ", ".join(f"m={m}(size {s})" for m, s in skips)
                print(f"    Requesting m={m_req} (size {s_req}, align {a_req}): "
                      f"must skip: {skip_str}")
            else:
                print(f"    Requesting m={m_req} (size {s_req}, align {a_req}): "
                      f"no skips needed")
        print()

    # Summary: how many skips total per exponent?
    print(f"  SUMMARY: skips needed per request at each exponent")
    print()

    for e in range(B, min(max_exp + 1, B + 4)):
        sizes_at_e = []
        for m in range(M):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s <= ARENA:
                sizes_at_e.append((m, s))

        if len(sizes_at_e) < M:
            continue

        total_skips = 0
        max_skips_per_request = 0

        for m_req, s_req in sizes_at_e:
            skips = 0
            for m_other, s_other in sizes_at_e:
                if s_other <= s_req:
                    continue
                for addr in range(min(ARENA, 64)):
                    if can_serve(addr, s_other, s_other):
                        if not can_serve(addr, s_other, s_req):
                            skips += 1
                            break

            total_skips += skips
            max_skips_per_request = max(max_skips_per_request, skips)

        print(f"    e={e}: max skips for any single request = {max_skips_per_request}")


def main():
    for B in [1, 2, 3, 4]:
        analyse(B)
        print()
        print()


if __name__ == "__main__":
    main()
