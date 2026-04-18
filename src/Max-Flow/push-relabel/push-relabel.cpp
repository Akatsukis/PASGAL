#include "push-relabel.h"

#include <quill/Quill.h>

#include <queue>
#include <string>
#include <type_traits>

#include "PRSyncNondetWin_optimized.h"
#include "graph.h"
#include "../reverse_edges.h"

using namespace std;
using namespace parlay;
using namespace Basic;

typedef uint32_t NodeId;
typedef uint32_t EdgeId;
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
  union {
    FlowTy w;
    FlowTy cap;
  };
  FlowTy flow;
  EdgeId rev;
  FlowEdge() = default;
  FlowEdge(NodeId _v, FlowTy _flow, FlowTy _cap)
      : BaseEdge<NodeId>(_v), flow(_flow), cap(_cap) {}
  bool operator<(const FlowEdge &rhs) const {
    return this->v < rhs.v || (this->v == rhs.v && flow < rhs.flow);
  }
  bool operator==(const FlowEdge &rhs) const {
    return this->v == rhs.v && flow == rhs.flow;
  }
};

static_assert(alignof(FlowEdge<FlowTy>) == 4, "FlowEdge alignment is wrong");
static_assert(sizeof(FlowEdge<FlowTy>) == 16, "FlowEdge size is wrong");

template <class Algo, class Graph, class NodeId = typename Graph::NodeId>
void run(Algo &algo, [[maybe_unused]] Graph &G, NodeId s, NodeId t) {
  cout << "source " << s << ", target " << t << endl;
  double total_time = 0;
  FlowTy max_flow = 0;
  for (int i = 0; i <= NUM_ROUND; i++) {
    parlay::parallel_for(0, G.m, [&](EdgeId j) { G.edges[j].flow = 0; });
    internal::timer tm;
    max_flow = algo.max_flow(s, t);
    tm.stop();
    if (i == 0) {
      cout << std::fixed << std::setprecision(6)
           << "Warmup Round: " << tm.total_time() << endl;
    } else {
      cout << std::fixed << std::setprecision(6) << "Round " << i << ": "
           << tm.total_time() << endl;
      total_time += tm.total_time();
    }
    cout << "Max flow: " << max_flow << endl;
  }
  double average_time = total_time / NUM_ROUND;
  cout << std::fixed << std::setprecision(6) << "Average time: " << average_time
       << endl;

  ofstream ofs("push-relabel.tsv", ios_base::app);
  ofs << s << '\t' << t << '\t' << max_flow << '\t' << average_time << '\n';
  ofs.close();
}

template <class Algo, class Graph>
void run(Algo &algo, Graph &G) {
  using NodeId = typename Graph::NodeId;
  for (int v = 0; v < NUM_SRC; v++) {
    NodeId s = hash32(v) % G.n;
    NodeId t = hash32(v + 1) % G.n;
    run(algo, G, s, t);
  }
}

int main(int argc, char *argv[]) {
  quill::start();

  if (argc == 1) {
    cout << "Usage: " << argv[0] << " [-i input_file] [-s] [-a algorithm]\n"
         << "Options:\n"
         << "\t-i,\tinput file path\n"
         << "\t-s,\tsymmetrized input graph\n"
         << "\t-a,\talgorithm (default: basic, options: basic, nondet)\n";
    return 0;
  }
  char c;
  char const *input_path = nullptr;
  bool symmetrized = false;
  uint32_t source = UINT_MAX;
  uint32_t target = UINT_MAX;
  string algorithm = "basic";
  while ((c = getopt(argc, argv, "i:sr:t:a:")) != -1) {
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
        target = atol(optarg);
        break;
      case 'a':
        algorithm = optarg;
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

  cout << "Using algorithm: " << algorithm << endl;

  using GraphType = Graph<NodeId, EdgeId, FlowEdge<FlowTy>>;

  if (algorithm == "nondet") {
    auto solver = PRSyncNondetWin<GraphType>(G);
    if (source == UINT_MAX || target == UINT_MAX) {
      run(solver, G);
    } else {
      run(solver, G, source, target);
    }
  } else if (algorithm == "basic") {
    auto solver = PushRelabel<GraphType>(G);
    if (source == UINT_MAX || target == UINT_MAX) {
      run(solver, G);
    } else {
      run(solver, G, source, target);
    }
  } else {
    std::cerr << "Error: Unknown algorithm '" << algorithm
              << "'. Available: basic, nondet" << std::endl;
    return 1;
  }
  return 0;
}
