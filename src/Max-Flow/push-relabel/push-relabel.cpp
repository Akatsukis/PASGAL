// Driver for the push-relabel max-flow algorithms in this directory:
//   -a serial   : serial Goldberg-Tarjan FIFO push-relabel  (mf::SerialPushRelabel)
//   -a prsn     : parallel sync nondeterministic w/ "win" rule, ESA'15 (mf::PRSyncNondet)
//
// Edge layout matches the residual graph emitted by ../reverse_edges.h:
// every Edge carries a single residual-capacity field `.w` plus a `.rev`
// index pointing to its mirror arc.  Flow on a forward arc i is implicitly
// arcs[i.rev].w (the mirror's residual = the flow already routed forward).

#include "../../graph.h"
#include "../reverse_edges.h"
#include "prs.h"
#include "prsn.h"
#include "serial_pr.h"

#include <getopt.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <type_traits>

#include "parlay/internal/get_time.h"
#include "parlay/parallel.h"
#include "parlay/utilities.h"

using std::cout;
using std::endl;
using std::ofstream;
using std::string;

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
  FlowTy w;            // residual capacity in this arc's direction
  EdgeId rev;          // index of the mirror arc
  FlowEdge() = default;
  FlowEdge(NodeId _v, FlowTy _w) : BaseEdge<NodeId>(_v), w(_w) {}
  bool operator<(const FlowEdge &rhs) const {
    return this->v < rhs.v || (this->v == rhs.v && w < rhs.w);
  }
  bool operator==(const FlowEdge &rhs) const {
    return this->v == rhs.v && w == rhs.w;
  }
};

static_assert(sizeof(FlowEdge<FlowTy>) == 12 || sizeof(FlowEdge<FlowTy>) == 16,
              "FlowEdge has unexpected size");

template <class Algo, class Graph, class NodeId = typename Graph::NodeId>
void run(Algo &algo, [[maybe_unused]] Graph &G, NodeId s, NodeId t,
         const string &algo_label) {
  cout << "source " << s << ", target " << t << endl;
  double total_time = 0;
  FlowTy max_flow = 0;
  for (int i = 0; i <= NUM_ROUND; i++) {
    parlay::internal::timer tm;
    max_flow = algo.max_flow(s, t);
    tm.stop();
    cout << std::fixed << std::setprecision(6);
    if (i == 0) {
      cout << "Warmup Round: " << tm.total_time() << endl;
    } else {
      cout << "Round " << i << ": " << tm.total_time() << endl;
      total_time += tm.total_time();
    }
    cout << "Max flow: " << max_flow << endl;
  }
  double average_time = total_time / NUM_ROUND;
  cout << std::fixed << std::setprecision(6)
       << "Average time: " << average_time << endl;

  ofstream ofs(algo_label + ".tsv", std::ios_base::app);
  ofs << s << '\t' << t << '\t' << max_flow << '\t' << average_time << '\n';
}

template <class Algo, class Graph>
void run(Algo &algo, Graph &G, const string &algo_label) {
  using NodeId = typename Graph::NodeId;
  for (int v = 0; v < NUM_SRC; v++) {
    NodeId s = parlay::hash32(v) % G.n;
    NodeId t = parlay::hash32(v + 1) % G.n;
    run(algo, G, s, t, algo_label);
  }
}

int main(int argc, char *argv[]) {
  if (argc == 1) {
    cout << "Usage: " << argv[0]
         << " [-i input_file] [-s] [-r src] [-t sink] [-a algorithm]\n"
         << "Options:\n"
         << "\t-i,\tinput file path\n"
         << "\t-s,\tsymmetrized input graph\n"
         << "\t-r,\tsource node id (0-based)\n"
         << "\t-t,\tsink   node id (0-based)\n"
         << "\t-a,\talgorithm: serial | prs | prsn  (default: prsn)\n"
         << "\t        \tserial = single-thread Goldberg-Tarjan FIFO PR\n"
         << "\t        \tprs    = parallel sync deterministic baseline (ESA'15 §3)\n"
         << "\t        \tprsn   = parallel sync nondeterministic + win rule (ESA'15 §3.1)\n";
    return 0;
  }
  char c;
  char const *input_path = nullptr;
  bool symmetrized = false;
  uint32_t source = UINT_MAX;
  uint32_t target = UINT_MAX;
  string algorithm = "prsn";
  while ((c = getopt(argc, argv, "i:sr:t:a:")) != -1) {
    switch (c) {
      case 'i': input_path = optarg; break;
      case 's': symmetrized = true; break;
      case 'r': source = atol(optarg); break;
      case 't': target = atol(optarg); break;
      case 'a': algorithm = optarg; break;
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
  G = generate_reverse_edges(G);

  cout << "Running on " << input_path << ": |V|=" << G.n << ", |E|=" << G.m
       << ", num_src=" << NUM_SRC << ", num_round=" << NUM_ROUND << endl;
  cout << "Using algorithm: " << algorithm << endl;

  using GraphType = Graph<NodeId, EdgeId, FlowEdge<FlowTy>>;

  if (algorithm == "prsn") {
    auto solver = mf::PRSyncNondet<GraphType>(G);
    if (source == UINT_MAX || target == UINT_MAX) run(solver, G, "prsn");
    else run(solver, G, source, target, "prsn");
  } else if (algorithm == "prs") {
    auto solver = mf::PRSync<GraphType>(G);
    if (source == UINT_MAX || target == UINT_MAX) run(solver, G, "prs");
    else run(solver, G, source, target, "prs");
  } else if (algorithm == "serial") {
    auto solver = mf::SerialPushRelabel<GraphType>(G);
    if (source == UINT_MAX || target == UINT_MAX) run(solver, G, "serial");
    else run(solver, G, source, target, "serial");
  } else {
    std::cerr << "Error: Unknown algorithm '" << algorithm
              << "'. Available: serial, prs, prsn" << std::endl;
    return 1;
  }
  return 0;
}
