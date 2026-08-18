# Vendored dependencies

## `unordered_dense.h`

[`ankerl::unordered_dense`](https://github.com/martinus/unordered_dense) v4.5.0, MIT
licensed, vendored as the single header its author ships. It appears in this repo only as
a **reference line on graphs** — maplab does not use it and does not aim to beat it.

It is vendored rather than fetched because it is one self-contained header with no build
system worth invoking, and because a benchmark baseline that silently changes version
between runs makes the numbers in RESULTS.md unreproducible.

`absl::flat_hash_map` is the other reference implementation. It is *not* vendored: it is a
much heavier dependency, so it is fetched on demand and off by default. Enable with
`-DMAPLAB_WITH_ABSEIL=ON`.

`std::unordered_map` needs no vendoring and is always present on the graphs, as the
explained strawman rather than as a target — see DESIGN.md §11 for why the standard
forbids implementing it as a flat table.
