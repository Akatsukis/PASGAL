// PRSyncNondet (parallel push-relabel, sync, nondeterministic w/ "win" rule)
//
// Faithful re-implementation of Listing 1.1 + Listing 1.2 from:
//   N. Baumstark, G. E. Blelloch, J. Shun.
//   "Efficient Implementation of a Synchronous Parallel Push-Relabel
//    Algorithm." ESA 2015, LNCS 9294, pp. 106-117.
//   PDF: https://www.cs.cmu.edu/~guyb/papers/BBS15.pdf
//
// Edge invariants (matches reverse_edges.h):
//   `Edge::w` is the residual capacity of the arc in its own direction.
//   `Edge::rev` is the index of the mirror arc.
//   A push of δ on arc i:  arcs[i].w -= δ; arcs[arcs[i].rev].w += δ.
//
// Key per-iteration phases (working set = currently-active vertices):
//   1. process: every active v in parallel runs its discharge loop on a
//      LOCAL copy of d(v) and e(v).  Pushes commit to:
//        - arc.w  (forward residual, decremented in place)
//        - arc.rev.w (reverse residual, incremented in place)
//        - added_excess[w] (atomic accumulator for neighbor's excess)
//      A "win rule" arbitrates between two adjacent active vertices that
//      both want to act on the shared admissible arc, ensuring at most one
//      side commits per iteration.
//   2. apply phase 1: for every v in working set, commit
//        d(v) := d'(v),  e(v) += added_excess[v],  added_excess[v] := 0.
//   3. build new working set: concat per-vertex `discovered` lists.
//   4. apply phase 2: for newly-discovered vertices that were NOT in the
//      old working set, commit added_excess too.
//
// Periodic global relabel (parallel BFS from sink in the residual graph)
// resets stale labels — its frequency is governed by ALPHA, BETA, FREQ.

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
class PRSyncNondet {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;
  using Edge   = typename Graph::Edge;
  using FlowTy = typename Edge::FlowTy;

  Graph& G;
  size_t n;
  NodeId source = 0, sink = 0;

  // Per-vertex state.
  parlay::sequence<NodeId>            d;             // distance label (committed)
  parlay::sequence<NodeId>            d_prime;       // tentative new label
  parlay::sequence<FlowTy>            excess;        // excess (committed)
  parlay::sequence<std::atomic<FlowTy>> added_excess; // pushes received this round
  parlay::sequence<std::atomic<int>>  is_discovered; // 0/1 for de-dup of discoveries
  parlay::sequence<size_t>            work_per_v;    // work counter per vertex

  // Per-vertex per-iteration "discovered" buffers.  Resized to vertex degree.
  // (capacity = out-degree + 1 covers all neighbors plus optional self-add.)
  parlay::sequence<std::vector<NodeId>> discovered;

  parlay::sequence<NodeId> working_set;
  parlay::sequence<NodeId> next_working_set;

  // Save a copy of initial residuals so successive max_flow calls start clean.
  parlay::sequence<FlowTy> initial_w;

  size_t work_since_gr = 0;

  // Constants from the paper / hi_pr.
  static constexpr size_t ALPHA = 6;
  static constexpr size_t BETA  = 12;
  static constexpr double FREQ  = 0.5;

  // ---------------------------------------------------------------------------
  // Global relabel: parallel reverse BFS from sink in the residual graph
  // (Listing 1.2 of the paper, simplified to a sequential layer-by-layer BFS
  //  for clarity — for typical graphs this is fast).
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
          if (v == source) continue;
          if (d[v] != n) continue;
          // Residual cap of v -> u = w of mirror arc.
          if (G.edges[G.edges[i].rev].w > 0) {
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
  // Process one active vertex (Listing 1.1 inner block, lines 26-66).
  //
  // Reads:  d[*], excess[*]  (start-of-iteration snapshot — neither is mutated
  //         during process by anyone)
  // Writes: G.edges[i].w  (arc residuals — race-free thanks to the win rule:
  //         at any moment only one direction of an edge is admissible for an
  //         active vertex)
  //         added_excess[w] via fetch_add  (atomic accumulator)
  //         d_prime[v]      (tentative label, applied after process finishes)
  //         is_discovered[w] via CAS       (per-vertex de-dup flag)
  //         discovered[v]   (this thread's own buffer — no cross-thread access)
  //         work_per_v[v]
  void process(NodeId v) {
    discovered[v].clear();
    work_per_v[v] = 0;

    NodeId d_v_old = d[v];          // committed label, stable during this round
    NodeId d_local = d_v_old;       // d'(v)
    FlowTy e_local = excess[v];     // local copy of e(v)
    const FlowTy e_orig = e_local;

    while (e_local > 0) {
      NodeId new_label = n;
      bool skipped = false;

      for (EdgeId i = G.offsets[v]; i < G.offsets[v + 1]; ++i) {
        if (e_local == 0) break;
        Edge& edge = G.edges[i];
        NodeId w = edge.v;
        FlowTy res = edge.w;          // current residual (may have been
                                      // mutated by *this thread* on prior
                                      // iterations of this very loop)

        bool admissible = (d_local == d[w] + 1);

        // "Win rule": if w is also active in this round, we may not act on
        // the shared admissible arc unless we win the race.  Uses the
        // committed labels d(v), d(w), NOT the tentative d_local.
        if (excess[w] > 0) {
          NodeId d_w = d[w];
          bool win = (d_v_old == d_w + 1) ||
                     (d_v_old <  static_cast<NodeId>(d_w - 1)) ||
                     (d_v_old == d_w && v < w);
          if (admissible && !win) {
            skipped = true;
            continue;
          }
        }

        if (admissible && res > 0) {
          FlowTy delta = std::min<FlowTy>(res, e_local);
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
        }

        // Update relabel candidate (paper line 56-57).
        if (res > 0 && d[w] >= d_local) {
          if (d[w] + 1 < new_label) new_label = d[w] + 1;
        }
      }

      if (e_local == 0 || skipped) break;
      d_local = new_label;
      work_per_v[v] += (G.offsets[v + 1] - G.offsets[v]) + BETA;
      if (d_local == n) break;
    }

    d_prime[v] = d_local;
    if (e_local != e_orig) {
      added_excess[v].fetch_add(e_local - e_orig, std::memory_order_relaxed);
    }

    // Self-discover so the apply phase will commit our excess delta.
    if (e_local > 0) {
      int expected = 0;
      if (is_discovered[v].compare_exchange_strong(expected, 1,
                                                    std::memory_order_acq_rel)) {
        discovered[v].push_back(v);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Concatenate every per-vertex `discovered` list into a flat sequence of
  // candidate vertices.  Used both for draining added_excess (no filter — we
  // must commit pushes even to vertices that just got relabeled to n) and
  // for building the next working set (with d < n filter applied separately).
  parlay::sequence<NodeId> all_discovered() {
    parlay::sequence<NodeId> out;
    for (NodeId v : working_set) {
      for (NodeId w : discovered[v]) out.push_back(w);
    }
    return out;
  }

 public:
  explicit PRSyncNondet(Graph& g) : G(g), n(g.n) {
    d             = parlay::sequence<NodeId>(n);
    d_prime       = parlay::sequence<NodeId>(n);
    excess        = parlay::sequence<FlowTy>(n);
    added_excess  = parlay::sequence<std::atomic<FlowTy>>(n);
    is_discovered = parlay::sequence<std::atomic<int>>(n);
    work_per_v    = parlay::sequence<size_t>(n);
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

    // Restore initial residuals (for repeatable runs).
    parlay::parallel_for(0, G.edges.size(),
                         [&](size_t i) { G.edges[i].w = initial_w[i]; });

    parlay::parallel_for(0, n, [&](size_t i) {
      d[i] = 0;
      d_prime[i] = 0;
      excess[i] = 0;
      added_excess[i].store(0);
      is_discovered[i].store(0);
    });
    d[source] = n;

    // Saturate source-adjacent arcs (paper lines 11-14).
    parlay::parallel_for(G.offsets[source], G.offsets[source + 1], [&](EdgeId i) {
      Edge& edge = G.edges[i];
      FlowTy c = edge.w;
      if (c > 0) {
        edge.w = 0;
        G.edges[edge.rev].w += c;
        // excess[source-neighbor] += c.  Multiple input arcs to the same
        // neighbor are already merged by reverse_edges.h, so this is a
        // single-writer update.
        excess[edge.v] = c;
      }
    });

    // Initial working set = source's neighbors with positive excess.
    working_set = parlay::sequence<NodeId>();
    for (EdgeId i = G.offsets[source]; i < G.offsets[source + 1]; ++i) {
      NodeId v = G.edges[i].v;
      if (v != source && v != sink && excess[v] > 0) {
        // No need to dedup: reverse_edges.h merged any parallel arcs.
        working_set.push_back(v);
      }
    }

    work_since_gr = std::numeric_limits<size_t>::max();  // force initial GR

    while (true) {
      // Periodic global relabel.
      if (FREQ * static_cast<double>(work_since_gr) >
          static_cast<double>(ALPHA * n + G.m)) {
        global_relabel();
        // Rebuild working set: every vertex with positive excess and finite
        // label that isn't source/sink.
        working_set = parlay::sequence<NodeId>();
        for (NodeId v = 0; v < n; ++v) {
          if (v != source && v != sink && excess[v] > 0 && d[v] < n) {
            working_set.push_back(v);
          }
        }
      }

      // Filter out vertices with d(v) >= n (paper line 22).
      working_set = parlay::filter(working_set,
                                   [&](NodeId v) { return d[v] < n; });
      if (working_set.empty()) break;

      // ------------------------- Process phase -------------------------
      parlay::parallel_for(0, working_set.size(),
                           [&](size_t i) { process(working_set[i]); });

      // Aggregate work counter.
      size_t round_work = 0;
      for (NodeId v : working_set) round_work += work_per_v[v];
      work_since_gr += round_work;

      // ----------------- Apply phase 1 (working set members) ------------
      // For every v that was processed: commit label, drain accumulated
      // added_excess into excess.
      parlay::parallel_for(0, working_set.size(), [&](size_t i) {
        NodeId v = working_set[i];
        d[v] = d_prime[v];
        FlowTy delta = added_excess[v].exchange(0, std::memory_order_acq_rel);
        excess[v] += delta;
        is_discovered[v].store(0, std::memory_order_release);
      });

      // ----------------- Apply phase 2 (every discovered vertex) ----------
      // Drain added_excess for ALL vertices that received a push this round,
      // even those at d == n (which won't appear in the next working set).
      // Skipping them would leave their `added_excess` un-applied — those
      // pushes would never increment the recipient's `excess`, breaking
      // flow conservation for any subsequent path that needs to reverse
      // through them.
      auto disc = all_discovered();
      parlay::parallel_for(0, disc.size(), [&](size_t i) {
        NodeId v = disc[i];
        if (v == source || v == sink) return;
        FlowTy delta = added_excess[v].exchange(0, std::memory_order_acq_rel);
        excess[v] += delta;
        is_discovered[v].store(0, std::memory_order_release);
      });

      // Sink is intentionally excluded from the working set / discovered
      // lists, but pushes to it land in added_excess[sink].  Drain so
      // excess[sink] (= total flow value) stays up to date.
      excess[sink] += added_excess[sink].exchange(0, std::memory_order_acq_rel);

      // ------------------ Build next working set --------------------------
      // Filter discovered vertices keeping only d(v) < n and excluding
      // source/sink.  Note: a vertex can appear multiple times if multiple
      // old-WS vertices discovered it — that's fine, no harm in processing
      // a node twice in a round (idempotent for d<n check).
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
