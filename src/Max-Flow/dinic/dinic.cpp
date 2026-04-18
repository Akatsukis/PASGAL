#include "dinic.h"

#include <queue>
#include <type_traits>

#include "graph.h"
#include "../reverse_edges.h"

typedef uint32_t NodeId;
typedef uint64_t EdgeId;
#ifdef FLOAT
typedef float FlowTy;
#else
typedef int32_t FlowTy;
#endif

constexpr int NUM_SRC = 1;
constexpr int NUM_ROUND = 1;
constexpr int LOG2_WEIGHT = 6;
constexpr int WEIGHT_RANGE = 1 << LOG2_WEIGHT;

template <class _FlowTy = int32_t>
class FlowEdge : public BaseEdge<NodeId> {
 public:
  using FlowTy = _FlowTy;
  FlowTy w;  // capacity
  EdgeId rev;
  FlowEdge() = default;
  FlowEdge(NodeId _v, FlowTy _w) : BaseEdge<NodeId>(_v), w(_w) {}
  bool operator<(const FlowEdge &rhs) const {
    return this->v < rhs.v || (this->v == rhs.v && w < rhs.w);
  }
  bool operator==(const FlowEdge &rhs) const {
    return this->v == rhs.v && w == rhs.w;
  }
};

template <class Algo, class Graph, class NodeId = typename Graph::NodeId>
void run(Algo &algo, [[maybe_unused]] Graph &G, NodeId s, NodeId t) {
  cout << "source " << s << ", sink " << t << endl;
  double total_time = 0;
  FlowTy max_flow;
  parlay::sequence<FlowTy> flows = parlay::tabulate(G.m, [&](EdgeId i) {
    return G.edges[i].w;
  });
  for (int i = 0; i <= NUM_ROUND; i++) {
    parlay::parallel_for(0, G.m, [&](EdgeId j) {
      G.edges[j].w = flows[j];
    });
    internal::timer tm;
    max_flow = algo.max_flow(s, t);
    tm.stop();
    if (i == 0) {
      cout << "Warmup Round: " << tm.total_time() << endl;
    } else {
      cout << "Round " << i << ": " << tm.total_time() << endl;
      total_time += tm.total_time();
    }
    cout << "Max flow: " << max_flow << endl;
  }
  double average_time = total_time / NUM_ROUND;
  cout << "Average time: " << average_time << endl;

  ofstream ofs("dinic.tsv", ios_base::app);
  ofs << s << '\t' << t << '\t' << max_flow << '\t' << average_time << '\n';
  ofs.close();
}

template <class Algo, class Graph>
void run(Algo &algo, Graph &G) {
  using NodeId = typename Graph::NodeId;
  for (int v = 0; v < NUM_SRC; v++) {
    NodeId s = hash32(v) % G.n;
    NodeId t = hash32(v + NUM_SRC) % G.n;
    run(algo, G, s, t);
  }
}

int main(int argc, char *argv[]) {
  if (argc == 1) {
    cout << "Usage: " << argv[0] << " [-i input_file] [-s]\n"
         << "Options:\n"
         << "\t-i,\tinput file path\n"
         << "\t-s,\tsymmetrized input graph\n";
    return 0;
  }
  char c;
  char const *input_path = nullptr;
  bool symmetrized = false;
  uint32_t source = UINT_MAX;
  uint32_t sink = UINT_MAX;
  while ((c = getopt(argc, argv, "i:sr:t:")) != -1) {
    switch (c) {
      case 'i':
        input_path = optarg;
        break;
      case 's':
        symmetrized = true;
        break;
      case 'r':
        source = atol(optarg);
        break;
      case 't':
        sink = atol(optarg);
        break;
      default:
        std::cerr << "Error: Unknown option " << optopt << std::endl;
        abort();
    }
  }

  cout << "Reading graph..." << endl;
  Graph<NodeId, EdgeId, FlowEdge<FlowTy>> G;
  G.read_graph(input_path);
  G.symmetrized = symmetrized;
  if (!G.weighted) {
    cout << "Generating edge weights..." << endl;
    G.generate_random_weight(1, WEIGHT_RANGE);
  }
  // for (NodeId i = 0; i < G.n; i++) {
  //   for (EdgeId j = G.offsets[i]; j < G.offsets[i + 1]; j++) {
  //     printf("(%u,%u,%d)\n", i, G.edges[j].v, G.edges[j].w);
  //   }
  // }
  // printf("\n");

  G = generate_reverse_edges(G);

  cout << "Running on " << input_path << ": |V|=" << G.n << ", |E|=" << G.m
       << ", num_src=" << NUM_SRC << ", num_round=" << NUM_ROUND << endl;

  Dinic solver(G);
  if (source == UINT_MAX || sink == UINT_MAX) {
    run(solver, G);
  } else {
    run(solver, G, source, sink);
  }
  return 0;
}
