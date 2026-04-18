// Serial Goldberg-Tarjan FIFO push-relabel with periodic global relabel.
//
// Reference implementation used for cross-checking the parallel PRSyncNondet
// (see prsn.h).  Single-threaded by design — no atomics, no race conditions.
//
// Edge invariants (matches the residual graph produced by reverse_edges.h):
//   - `Edge::w`   is the residual capacity of the arc in its own direction.
//   - `Edge::rev` is the index of the mirror arc.
//   - A push of δ on arc i:  arcs[i].w -= δ; arcs[arcs[i].rev].w += δ.

#pragma once

#include <cassert>
#include <queue>
#include <limits>

#include "graph.h"
#include "parlay/sequence.h"

namespace mf {

template <class Graph>
class SerialPushRelabel {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;
  using Edge   = typename Graph::Edge;
  using FlowTy = typename Edge::FlowTy;

  Graph& G;
  size_t n;
  NodeId source = 0, sink = 0;

  parlay::sequence<NodeId> h;       // distance label
  parlay::sequence<FlowTy> e;       // excess
  parlay::sequence<char>   in_q;
  std::queue<NodeId>       fifo;

  size_t work_since_gr = 0;
  static constexpr size_t ALPHA = 6;

  // Save initial residuals so successive max_flow calls start from a fresh
  // residual graph (the algorithm mutates `G.edges[*].w` in place).
  parlay::sequence<FlowTy> initial_w;

  void enqueue(NodeId v) {
    if (v == source || v == sink) return;
    if (e[v] <= 0) return;
    if (in_q[v]) return;
    in_q[v] = 1;
    fifo.push(v);
  }

  void global_relabel() {
    // BFS from sink in the residual graph (reversed sense): a node v is
    // visited iff it can still send flow toward the sink along positive
    // residual arcs.  d(v) := BFS distance from sink, d(sink) := 0,
    // unreachable nodes get d(v) = n.
    std::fill(h.begin(), h.end(), static_cast<NodeId>(n));
    h[sink] = 0;
    std::queue<NodeId> bq;
    bq.push(sink);
    while (!bq.empty()) {
      NodeId u = bq.front(); bq.pop();
      for (EdgeId i = G.offsets[u]; i < G.offsets[u + 1]; ++i) {
        NodeId v = G.edges[i].v;
        if (v == source || h[v] != static_cast<NodeId>(n)) continue;
        // Residual cap of v -> u = w of the mirror arc (which lives in v's
        // adjacency).  arcs[arcs[i].rev] IS that mirror.
        if (G.edges[G.edges[i].rev].w > 0) {
          h[v] = h[u] + 1;
          bq.push(v);
        }
      }
    }
    h[source] = n;
    work_since_gr = 0;
  }

  void discharge(NodeId u) {
    while (e[u] > 0 && h[u] < n) {
      NodeId min_neighbor_h = n;

      for (EdgeId i = G.offsets[u]; i < G.offsets[u + 1]; ++i) {
        Edge& edge = G.edges[i];
        if (edge.w <= 0) continue;
        NodeId v = edge.v;
        if (h[u] == h[v] + 1) {
          // admissible push
          FlowTy delta = std::min<FlowTy>(edge.w, e[u]);
          edge.w -= delta;
          G.edges[edge.rev].w += delta;
          e[u] -= delta;
          e[v] += delta;
          enqueue(v);
          if (e[u] == 0) return;
        } else if (h[v] + 1 < min_neighbor_h) {
          min_neighbor_h = h[v] + 1;
        }
      }

      if (e[u] == 0) return;
      // No admissible arc → relabel.
      if (min_neighbor_h >= n) {       // disconnected from sink
        h[u] = n;
        return;
      }
      h[u] = min_neighbor_h;
      work_since_gr += (G.offsets[u + 1] - G.offsets[u]);
    }
  }

 public:
  explicit SerialPushRelabel(Graph& g) : G(g), n(g.n) {
    h    = parlay::sequence<NodeId>(n);
    e    = parlay::sequence<FlowTy>(n);
    in_q = parlay::sequence<char>(n);
    initial_w = parlay::sequence<FlowTy>(G.edges.size());
    for (size_t i = 0; i < G.edges.size(); ++i) initial_w[i] = G.edges[i].w;
  }

  FlowTy max_flow(NodeId s, NodeId t) {
    if (s == t) return 0;
    source = s; sink = t;

    // Restore residual graph in case max_flow is called multiple times.
    for (size_t i = 0; i < G.edges.size(); ++i) G.edges[i].w = initial_w[i];

    std::fill(e.begin(), e.end(), FlowTy(0));
    std::fill(in_q.begin(), in_q.end(), 0);
    while (!fifo.empty()) fifo.pop();

    global_relabel();
    h[source] = n;

    // Saturate source-adjacent arcs.
    for (EdgeId i = G.offsets[source]; i < G.offsets[source + 1]; ++i) {
      Edge& edge = G.edges[i];
      FlowTy c = edge.w;
      if (c > 0) {
        edge.w = 0;
        G.edges[edge.rev].w += c;
        e[edge.v] += c;
        enqueue(edge.v);
      }
    }

    while (!fifo.empty()) {
      if (work_since_gr > ALPHA * n + G.m) {
        global_relabel();
        h[source] = n;
        // Rebuild queue with active vertices (those with positive excess and
        // valid label).
        while (!fifo.empty()) { in_q[fifo.front()] = 0; fifo.pop(); }
        for (NodeId v = 0; v < n; ++v) {
          if (v != source && v != sink && e[v] > 0 && h[v] < n)
            enqueue(v);
        }
        if (fifo.empty()) break;
      }
      NodeId u = fifo.front(); fifo.pop();
      in_q[u] = 0;
      discharge(u);
    }
    return e[sink];
  }
};

}  // namespace mf
