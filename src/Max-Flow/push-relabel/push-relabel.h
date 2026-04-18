#include <parlay/delayed_sequence.h>
#include <parlay/sequence.h>
#include <quill/Quill.h>

#include <queue>

#include "graph.h"
#include "hashbag.h"

using namespace std;
using namespace parlay;

namespace Basic {

template <class Graph>
class PushRelabel {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;
  using Edge = typename Graph::Edge;
  using FlowTy = typename Edge::FlowTy;

  // cannot use const Graph& because e.w is modified
  size_t n;
  NodeId source, sink;
  Graph& G;
  hashbag<NodeId> bag;
  sequence<std::atomic<bool>> in_frontier;
  sequence<NodeId> frontier;
  sequence<NodeId> heights;
  sequence<NodeId> new_heights;
  sequence<FlowTy> excess;
  sequence<std::atomic<FlowTy>> added_excess;

 public:
  PushRelabel() = delete;
  PushRelabel(Graph& _G) : G(_G), bag(G.n) {
    n = G.n;
    in_frontier = sequence<std::atomic<bool>>(G.n);
    frontier = sequence<NodeId>::uninitialized(G.n);
    heights = sequence<NodeId>::uninitialized(G.n);
    new_heights = sequence<NodeId>::uninitialized(G.n);
    excess = sequence<FlowTy>::uninitialized(G.n);
    added_excess = sequence<std::atomic<FlowTy>>(G.n);
  }

  FlowTy get_residual(const Edge& e) {
    if (e.cap) {
      return e.cap - e.flow;
    } else {
      return G.edges[e.rev].flow;
    }
  }

  void add_to_bag(NodeId u) {
    if (compare_and_swap(&in_frontier[u], false, true)) {
      bag.insert(u);
    }
  }

  void init(NodeId s) {
    parallel_for(0, G.n, [&](size_t i) {
      heights[i] = 0;
      excess[i] = 0;
      added_excess[i] = 0;
      in_frontier[i] = false;
    });
    heights[s] = n;
    parallel_for(G.offsets[s], G.offsets[s + 1], [&](size_t i) {
      auto& e = G.edges[i];
      if (e.cap) {
        e.flow = e.cap;
        excess[e.v] = e.cap;
        add_to_bag(e.v);
      }
    });
  }

  void global_relabel() {
    parallel_for(0, n, [&](size_t i) { heights[i] = n; });
    heights[sink] = 0;
    queue<NodeId> q;
    q.push(sink);
    while (!q.empty()) {
      NodeId u = q.front();
      q.pop();
      for (size_t i = G.offsets[u]; i < G.offsets[u + 1]; i++) {
        auto& e = G.edges[i];
        if (e.v != source && get_residual(e) > 0) {
          if (e.v != sink && heights[e.v] == n) {
            heights[e.v] = heights[u] + 1;
            q.push(e.v);
            add_to_bag(e.v);
          }
        }
      }
    }
  }

  void push(NodeId u) {
    for (size_t i = G.offsets[u]; i < G.offsets[u + 1]; i++) {
      if (excess[u] == 0) {
        break;
      }
      auto& e = G.edges[i];
      if (heights[u] == heights[e.v] + 1 && get_residual(e) > 0) {
        FlowTy delta = min(get_residual(e), excess[u]);
        e.flow += delta;
        excess[u] -= delta;
        added_excess[e.v].fetch_add(delta);
        if (e.v != sink) {
          add_to_bag(e.v);
        }
      }
    }
  }

  void relabel(NodeId u) {
    if (excess[u] > 0) {
      new_heights[u] = n;
      for (size_t i = G.offsets[u]; i < G.offsets[u + 1]; i++) {
        const auto& e = G.edges[i];
        if (get_residual(e) > 0) {
          new_heights[u] = min(new_heights[u], heights[e.v]);
        }
      }
    } else {
      new_heights[u] = heights[u];
    }
  }

  FlowTy max_flow(NodeId s, NodeId t) {
    source = s;
    sink = t;
    if (source == sink) {
      return 0;
    }
    init(source);
    int round = 0;

    size_t frontier_size = bag.pack_into(frontier);
    parallel_for(0, frontier_size,
                 [&](size_t i) { in_frontier[frontier[i]] = false; });
    while (frontier_size) {
      LOG_INFO("Round {}: frontier size: {}", round++, frontier_size);

      // phase 1 (push)
      parallel_for(0, frontier_size, [&](size_t i) {
        NodeId u = frontier[i];
        push(u);
      });

      // phase 2 (relabel)
      parallel_for(0, frontier_size, [&](size_t i) {
        NodeId u = frontier[i];
        if (excess[u] > 0) {
          relabel(u);
        }
      });

      // phase 3 (apply updates)
      parallel_for(0, frontier_size, [&](size_t i) {
        NodeId u = frontier[i];
        heights[u] = new_heights[u];
        added_excess[u] = 0;
      });

      frontier_size = bag.pack_into(frontier);
      parallel_for(0, frontier_size, [&](size_t i) {
        NodeId u = frontier[i];
        excess[u] += added_excess[u];
        added_excess[u] = 0;
        in_frontier[frontier[i]] = false;
      });

      if (!frontier_size) {
        global_relabel();
        frontier_size = bag.pack_into(frontier);
      }
    }
    return excess[sink];
  }
};

}  // namespace Basic
