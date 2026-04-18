// PRSync — synchronous parallel push-relabel "simple" baseline
//
// Baumstark, Blelloch, Shun.  ESA 2015, §3 (the version BEFORE the win-rule
// optimisation introduced in §3.1 / `prsn`).  Distinguishing properties:
//
//   - At most ONE relabel per vertex per iteration (no inner re-discharge
//     loop).  Heights are read-only for the duration of the iteration.
//   - Because heights are stable across the whole iteration, at any moment
//     only one direction of any edge is admissible — no two adjacent active
//     vertices can ever try to push on the same arc → no race → no win rule.
//   - Output is deterministic in work and in result regardless of thread
//     count.  Useful as a cross-check for the nondeterministic `prsn`.
//
// Edge invariants identical to prsn / serial_pr:
//   `Edge::w`   = residual capacity in this arc's direction.
//   `Edge::rev` = index of mirror arc.
//   Push of δ on arc i:  arcs[i].w -= δ;  arcs[arcs[i].rev].w += δ.

#pragma once

#include <atomic>
#include <cassert>
#include <limits>
#include <vector>

#include "graph.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"

namespace mf {

template <class Graph>
class PRSync {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;
  using Edge   = typename Graph::Edge;
  using FlowTy = typename Edge::FlowTy;

  Graph& G;
  size_t n;
  NodeId source = 0, sink = 0;

  parlay::sequence<NodeId> d;             // committed labels
  parlay::sequence<NodeId> d_new;         // tentative labels for this iter
  parlay::sequence<FlowTy> excess;
  parlay::sequence<std::atomic<FlowTy>> added_excess;
  parlay::sequence<std::atomic<int>>   is_discovered;
  parlay::sequence<std::vector<NodeId>> discovered;

  parlay::sequence<NodeId> working_set;
  parlay::sequence<NodeId> next_working_set;

  parlay::sequence<FlowTy> initial_w;
  size_t work_since_gr = 0;

  static constexpr size_t ALPHA = 6;
  static constexpr double FREQ  = 0.5;

  // ---------------------------------------------------------------------------
  void global_relabel() {
    parlay::parallel_for(0, n, [&](size_t i) {
      d[i] = n;
      is_discovered[i].store(0);
    });
    d[sink] = 0;

    parlay::sequence<NodeId> frontier;
    frontier.push_back(sink);
    while (!frontier.empty()) {
      parlay::sequence<NodeId> next;
      for (NodeId u : frontier) {
        for (EdgeId i = G.offsets[u]; i < G.offsets[u + 1]; ++i) {
          NodeId v = G.edges[i].v;
          if (v == source || d[v] != n) continue;
          if (G.edges[G.edges[i].rev].w > 0) {  // residual cap of v->u
            d[v] = d[u] + 1;
            next.push_back(v);
          }
        }
      }
      frontier = std::move(next);
    }
    d[source] = n;
    work_since_gr = 0;
  }

  // ---------------------------------------------------------------------------
  // process(v): single-pass discharge.
  //
  // Reads:  d[*], excess[*]  (start-of-iter snapshot — neither mutates here)
  // Writes: G.edges[*].w     (race-free: only one direction of any edge is
  //                           admissible under the start-of-iter heights)
  //         added_excess[*]  (atomic fetch-and-add)
  //         d_new[v]         (tentative label, applied later)
  //         is_discovered[*] (CAS dedup)
  //         discovered[v]    (this thread's own buffer)
  void process(NodeId v) {
    discovered[v].clear();

    const NodeId d_v   = d[v];
    const FlowTy e_orig = excess[v];
    FlowTy       e_local = e_orig;
    NodeId       new_label = n;

    for (EdgeId i = G.offsets[v]; i < G.offsets[v + 1]; ++i) {
      Edge& edge = G.edges[i];
      if (edge.w <= 0) continue;
      NodeId w = edge.v;
      NodeId d_w = d[w];

      if (e_local > 0 && d_v == d_w + 1) {
        // admissible push (heights stable → no race possible)
        FlowTy delta = std::min<FlowTy>(edge.w, e_local);
        edge.w -= delta;
        G.edges[edge.rev].w += delta;
        e_local -= delta;
        added_excess[w].fetch_add(delta, std::memory_order_relaxed);
        if (w != sink) {
          int expected = 0;
          if (is_discovered[w].compare_exchange_strong(
                  expected, 1, std::memory_order_acq_rel)) {
            discovered[v].push_back(w);
          }
        }
      } else if (d_w >= d_v) {
        // candidate for relabel
        if (d_w + 1 < new_label) new_label = d_w + 1;
      }
    }

    // At most one relabel per vertex per iteration: only relabel if we still
    // have excess after the push pass.
    if (e_local > 0 && new_label > d_v) {
      d_new[v] = new_label;            // commits at apply phase
    } else {
      d_new[v] = d_v;
    }

    // Self-add the excess delta (negative if we pushed flow out).
    if (e_local != e_orig) {
      added_excess[v].fetch_add(e_local - e_orig, std::memory_order_relaxed);
    }

    // Self-discover so apply phase will commit our excess delta and we
    // appear in the next working set if we still have excess + finite label.
    if (e_local > 0) {
      int expected = 0;
      if (is_discovered[v].compare_exchange_strong(
              expected, 1, std::memory_order_acq_rel)) {
        discovered[v].push_back(v);
      }
    }
  }

  parlay::sequence<NodeId> all_discovered() {
    parlay::sequence<NodeId> out;
    for (NodeId v : working_set) {
      for (NodeId w : discovered[v]) out.push_back(w);
    }
    return out;
  }

 public:
  explicit PRSync(Graph& g) : G(g), n(g.n) {
    d             = parlay::sequence<NodeId>(n);
    d_new         = parlay::sequence<NodeId>(n);
    excess        = parlay::sequence<FlowTy>(n);
    added_excess  = parlay::sequence<std::atomic<FlowTy>>(n);
    is_discovered = parlay::sequence<std::atomic<int>>(n);
    discovered    = parlay::sequence<std::vector<NodeId>>(n);
    parlay::parallel_for(0, n, [&](size_t i) {
      discovered[i].reserve(G.offsets[i + 1] - G.offsets[i] + 1);
    });
    initial_w = parlay::sequence<FlowTy>(G.edges.size());
    for (size_t i = 0; i < G.edges.size(); ++i) initial_w[i] = G.edges[i].w;
  }

  FlowTy max_flow(NodeId s, NodeId t) {
    if (s == t) return 0;
    source = s; sink = t;

    parlay::parallel_for(0, G.edges.size(),
                         [&](size_t i) { G.edges[i].w = initial_w[i]; });
    parlay::parallel_for(0, n, [&](size_t i) {
      d[i] = 0;
      d_new[i] = 0;
      excess[i] = 0;
      added_excess[i].store(0);
      is_discovered[i].store(0);
    });
    d[source] = n;

    parlay::parallel_for(G.offsets[source], G.offsets[source + 1], [&](EdgeId i) {
      Edge& edge = G.edges[i];
      FlowTy c = edge.w;
      if (c > 0) {
        edge.w = 0;
        G.edges[edge.rev].w += c;
        excess[edge.v] = c;  // single-writer thanks to merged residual graph
      }
    });

    working_set = parlay::sequence<NodeId>();
    for (EdgeId i = G.offsets[source]; i < G.offsets[source + 1]; ++i) {
      NodeId v = G.edges[i].v;
      if (v != source && v != sink && excess[v] > 0) working_set.push_back(v);
    }

    work_since_gr = std::numeric_limits<size_t>::max();

    while (true) {
      if (FREQ * static_cast<double>(work_since_gr) >
          static_cast<double>(ALPHA * n + G.m)) {
        global_relabel();
        working_set = parlay::sequence<NodeId>();
        for (NodeId v = 0; v < n; ++v) {
          if (v != source && v != sink && excess[v] > 0 && d[v] < n) {
            working_set.push_back(v);
          }
        }
      }

      working_set = parlay::filter(working_set,
                                   [&](NodeId v) { return d[v] < n; });
      if (working_set.empty()) break;

      // Process: single-pass discharge per active vertex.
      parlay::parallel_for(0, working_set.size(),
                           [&](size_t i) { process(working_set[i]); });

      // Cheap work counter for global-relabel scheduling: count edges scanned.
      for (NodeId v : working_set) {
        work_since_gr += (G.offsets[v + 1] - G.offsets[v]);
      }

      // Apply phase 1: commit labels + drain added_excess for processed verts.
      parlay::parallel_for(0, working_set.size(), [&](size_t i) {
        NodeId v = working_set[i];
        d[v] = d_new[v];
        FlowTy delta = added_excess[v].exchange(0, std::memory_order_acq_rel);
        excess[v] += delta;
        is_discovered[v].store(0, std::memory_order_release);
      });

      // Apply phase 2: drain added_excess for ALL push recipients (including
      // vertices that just relabeled to n, which won't appear in the next WS).
      auto disc = all_discovered();
      parlay::parallel_for(0, disc.size(), [&](size_t i) {
        NodeId v = disc[i];
        if (v == source || v == sink) return;
        FlowTy delta = added_excess[v].exchange(0, std::memory_order_acq_rel);
        excess[v] += delta;
        is_discovered[v].store(0, std::memory_order_release);
      });

      // Drain sink (excluded from working set but accumulates pushed flow).
      excess[sink] += added_excess[sink].exchange(0, std::memory_order_acq_rel);

      // Build next working set from disc, filtering d(v) < n.
      next_working_set.clear();
      for (NodeId v : disc) {
        if (v != source && v != sink && d[v] < n) {
          next_working_set.push_back(v);
        }
      }
      working_set = std::move(next_working_set);
      next_working_set = parlay::sequence<NodeId>();
    }

    return excess[sink];
  }
};

}  // namespace mf
