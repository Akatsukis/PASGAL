// Shared preprocessing: generate_reverse_edges
//
// Produces a residual graph from the input graph:
//
//   1. For every input arc (u, v, c) we emit the forward residual arc (u, v, c)
//      and a reverse residual arc (v, u, 0).  For symmetrized input (undirected
//      graph) the reverse arc is instead (v, u, c).
//
//   2. PARALLEL-EDGE MERGE.  Multiple input arcs between the same ordered pair
//      (u, v) are collapsed into one output arc whose capacity is the sum of
//      their individual capacities.  Likewise, when the input contains both
//      (u, v, c1) and (v, u, c2) (antiparallel pair), the residual graph gets
//      exactly one arc per direction — each acts as the other's `rev` arc and
//      carries its own forward capacity.  This keeps the residual CSR dense
//      and avoids redundant arc scans in BFS/DFS/push phases.
//
//   3. For every output arc (u, v) we set `rev` to the index of the mirror
//      arc (v, u) via a per-node binary search (adjacency is sorted by head).
//
// Requirements on the Graph type:
//   - Graph::Edge has fields `.v`, `.w`, and `.rev`  (push-relabel uses a
//     `union { FlowTy w; FlowTy cap; }`, so `.w` works uniformly).
//   - Graph has `.n`, `.m`, `.offsets`, `.edges`, `.symmetrized`.

#pragma once

#include <tuple>
#include <atomic>
#include <cassert>

#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"

template <class Graph>
Graph generate_reverse_edges(const Graph &G) {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;
  using Edge = typename Graph::Edge;
  using FlowTy = typename Edge::FlowTy;

  const EdgeId N2 = 2 * G.m;

  // --- Step 1: enumerate 2m directed arc candidates (from, to, cap). --------
  // Forward of input arc i:  (u, v, c)
  // Reverse-residual of i:   (v, u, 0)   or  (v, u, c)  if symmetrized
  parlay::sequence<std::tuple<NodeId, NodeId, FlowTy>> arcs(N2);
  parlay::parallel_for(0, G.n, [&](NodeId u) {
    parlay::parallel_for(G.offsets[u], G.offsets[u + 1], [&](EdgeId i) {
      NodeId v = G.edges[i].v;
      FlowTy c = G.edges[i].w;
      FlowTy cr = G.symmetrized ? c : FlowTy(0);
      arcs[2 * i]     = std::make_tuple(u, v, c);
      arcs[2 * i + 1] = std::make_tuple(v, u, cr);
    });
  });

  // --- Step 2: sort by (from, to) so arcs with the same key are adjacent. ---
  parlay::sort_inplace(parlay::make_slice(arcs),
                       [](const auto &a, const auto &b) {
                         if (std::get<0>(a) != std::get<0>(b))
                           return std::get<0>(a) < std::get<0>(b);
                         return std::get<1>(a) < std::get<1>(b);
                       });

  // --- Step 3: segment starts (1 at each distinct (from,to) group start). ---
  parlay::sequence<EdgeId> seg_idx(N2);
  parlay::parallel_for(0, N2, [&](EdgeId i) {
    bool is_start = (i == 0) ||
                    std::get<0>(arcs[i - 1]) != std::get<0>(arcs[i]) ||
                    std::get<1>(arcs[i - 1]) != std::get<1>(arcs[i]);
    seg_idx[i] = is_start ? 1 : 0;
  });
  // Inclusive scan so each entry holds its 1-based segment index.
  parlay::scan_inclusive_inplace(parlay::make_slice(seg_idx));
  const EdgeId m_merged = seg_idx[N2 - 1];

  // --- Step 4: build per-segment arrays (from, to, summed capacity). --------
  parlay::sequence<NodeId> arc_from(m_merged);
  parlay::sequence<NodeId> arc_to(m_merged);
  parlay::sequence<std::atomic<FlowTy>> cap_accum(m_merged);
  parlay::parallel_for(0, m_merged,
                       [&](EdgeId s) { cap_accum[s].store(FlowTy(0)); });

  parlay::parallel_for(0, N2, [&](EdgeId i) {
    EdgeId s = seg_idx[i] - 1;
    // Segment-start entry: record keys.
    bool is_start = (i == 0) ||
                    std::get<0>(arcs[i - 1]) != std::get<0>(arcs[i]) ||
                    std::get<1>(arcs[i - 1]) != std::get<1>(arcs[i]);
    if (is_start) {
      arc_from[s] = std::get<0>(arcs[i]);
      arc_to[s]   = std::get<1>(arcs[i]);
    }
    // Every entry contributes its cap to the segment sum.
    cap_accum[s].fetch_add(std::get<2>(arcs[i]));
  });

  // --- Step 5: materialize CSR. ---------------------------------------------
  Graph G_rev;
  G_rev.n = G.n;
  G_rev.m = m_merged;
  G_rev.symmetrized = G.symmetrized;
  G_rev.offsets = parlay::sequence<EdgeId>(G.n + 1, m_merged);
  G_rev.edges = parlay::sequence<Edge>(m_merged);

  parlay::parallel_for(0, m_merged, [&](EdgeId s) {
    G_rev.edges[s].v = arc_to[s];
    G_rev.edges[s].w = cap_accum[s].load();
    if (s == 0 || arc_from[s - 1] != arc_from[s]) {
      G_rev.offsets[arc_from[s]] = s;
    }
  });
  // Fill offsets for empty nodes: offsets[u] = offsets[u+1] if u has no out-arcs.
  parlay::scan_inclusive_inplace(
      parlay::make_slice(G_rev.offsets.rbegin(), G_rev.offsets.rend()),
      parlay::minm<EdgeId>());

  // --- Step 6: link `rev` via binary search in the mirror's adjacency. ------
  // Adjacency of every node is sorted ascending by head (from the sort above).
  parlay::parallel_for(0, G_rev.n, [&](NodeId u) {
    parlay::parallel_for(G_rev.offsets[u], G_rev.offsets[u + 1], [&](EdgeId i) {
      NodeId v = G_rev.edges[i].v;
      EdgeId lo = G_rev.offsets[v];
      EdgeId hi = G_rev.offsets[v + 1];
      while (lo < hi) {
        EdgeId mid = lo + (hi - lo) / 2;
        if (G_rev.edges[mid].v < u) lo = mid + 1;
        else hi = mid;
      }
      assert(lo < G_rev.offsets[v + 1] && G_rev.edges[lo].v == u);
      G_rev.edges[i].rev = lo;
    });
  });

  // --- Step 7: validate. ----------------------------------------------------
  parlay::parallel_for(0, G_rev.n, [&](NodeId u) {
    parlay::parallel_for(G_rev.offsets[u], G_rev.offsets[u + 1], [&](EdgeId i) {
      EdgeId rev = G_rev.edges[i].rev;
      assert(G_rev.edges[rev].v == u);
      assert(G_rev.edges[G_rev.edges[rev].rev].v == G_rev.edges[i].v);
    });
  });
  return G_rev;
}
