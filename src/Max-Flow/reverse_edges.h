// Shared utility: generate_reverse_edges
//
// For each arc in the input graph, adds a residual reverse arc with capacity 0
// (or equal capacity, when the input is marked symmetrized) and a `rev` index
// linking each forward arc to its residual counterpart.
//
// Requirements on the Graph type:
//   - Graph::Edge has fields `.v`, `.w`, and `.rev`
//     (push-relabel uses a `union { FlowTy w; FlowTy cap; }`, so `.w` works
//     uniformly across dinic and push-relabel edge types).
//   - Graph has `.n`, `.m`, `.offsets`, `.edges`, `.symmetrized`.

#pragma once

#include <tuple>

#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"

template <class Graph>
Graph generate_reverse_edges(const Graph &G) {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;
  using Edge = typename Graph::Edge;
  using FlowTy = typename Edge::FlowTy;

  parlay::sequence<std::tuple<NodeId, NodeId, EdgeId>> edgelist(G.m * 2);
  parlay::parallel_for(0, G.n, [&](NodeId u) {
    parlay::parallel_for(G.offsets[u], G.offsets[u + 1], [&](EdgeId i) {
      // forward edge
      std::get<0>(edgelist[i * 2]) = u;
      std::get<1>(edgelist[i * 2]) = G.edges[i].v;
      std::get<2>(edgelist[i * 2]) = i * 2;
      // reverse edge
      std::get<0>(edgelist[i * 2 + 1]) = G.edges[i].v;
      std::get<1>(edgelist[i * 2 + 1]) = u;
      std::get<2>(edgelist[i * 2 + 1]) = i * 2 + 1;
    });
  });
  parlay::sort_inplace(parlay::make_slice(edgelist), [&](auto &a, auto &b) {
    return std::get<0>(a) < std::get<0>(b) ||
           (std::get<0>(a) == std::get<0>(b) &&
            std::get<1>(a) < std::get<1>(b));
  });

  Graph G_rev;
  G_rev.n = G.n;
  G_rev.m = G.m * 2;
  G_rev.symmetrized = G.symmetrized;
  G_rev.offsets = parlay::sequence<EdgeId>(G.n + 1, G.m * 2);
  G_rev.edges = parlay::sequence<Edge>(G.m * 2);
  parlay::parallel_for(0, G.m * 2, [&](EdgeId i) {
    EdgeId id = std::get<2>(edgelist[i]);
    G_rev.edges[i].v = std::get<1>(edgelist[i]);
    // Symmetrized inputs mean each (u,v,c) represents an undirected edge;
    // both directions carry the same capacity.  Otherwise reverse arcs start
    // with capacity 0.
    if (G.symmetrized) {
      G_rev.edges[i].w = G.edges[id / 2].w;
    } else {
      G_rev.edges[i].w = (id & 1) ? FlowTy(0) : G.edges[id / 2].w;
    }
    if (i == 0 || std::get<0>(edgelist[i]) != std::get<0>(edgelist[i - 1])) {
      G_rev.offsets[std::get<0>(edgelist[i])] = i;
    }
  });
  parlay::scan_inclusive_inplace(
      parlay::make_slice(G_rev.offsets.rbegin(), G_rev.offsets.rend()),
      parlay::minm<EdgeId>());

  parlay::sequence<std::tuple<uint32_t, uint32_t>> rev_id(G.m);
  parlay::parallel_for(0, G_rev.n, [&](NodeId u) {
    parlay::parallel_for(G_rev.offsets[u], G_rev.offsets[u + 1], [&](EdgeId i) {
      EdgeId id = std::get<2>(edgelist[i]);
      if (id & 1) {
        std::get<1>(rev_id[id / 2]) = i;
      } else {
        std::get<0>(rev_id[id / 2]) = i;
      }
    });
  });

  parlay::parallel_for(0, G.m * 2, [&](EdgeId i) {
    EdgeId id = std::get<2>(edgelist[i]);
    if (id & 1) {
      G_rev.edges[i].rev = std::get<0>(rev_id[id / 2]);
    } else {
      G_rev.edges[i].rev = std::get<1>(rev_id[id / 2]);
    }
  });

  // validate
  parlay::parallel_for(0, G_rev.n, [&](NodeId u) {
    parlay::parallel_for(G_rev.offsets[u], G_rev.offsets[u + 1], [&](EdgeId i) {
      FlowTy w = G_rev.edges[i].w;
      EdgeId rev = G_rev.edges[i].rev;
      assert(G_rev.edges[rev].v == u);
      if (!G.symmetrized && w) {
        assert(G_rev.edges[rev].w == 0);
      }
    });
  });
  return G_rev;
}
