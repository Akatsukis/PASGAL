#include <fstream>
#include <map>

#include "graph.h"
#include "testlib.h"

typedef uint32_t NodeId;
typedef uint64_t EdgeId;
#ifdef FLOAT
typedef float EdgeTy;
#else
typedef uint32_t EdgeTy;
#endif

template <class Graph>
void generate_weight_uniform(Graph &G) {
  size_t m = G.m;
  constexpr int BITS = 8;
  constexpr int MAX_WEIGHT = 1 << BITS;
  for (size_t i = 0; i < m; i++) {
    if constexpr (std::is_same_v<EdgeTy, uint32_t>) {
      G.edges[i].w = rnd.next(1, MAX_WEIGHT);
    } else {
      G.edges[i].w = rnd.next(1.0);
    }
  }
}

template <class Graph>
void generate_weight_skewed(Graph &G) {
  size_t m = G.m;
  constexpr int BITS = 8;
  for (size_t i = 0; i < m; i++) {
    int b = rnd.next(1, BITS);
    G.edges[i].w = rnd.next(1, (1 << b) - 1);
  }
}

int main(int argc, char *argv[]) {
  if (argc == 1) {
    fprintf(stderr,
            "Usage: %s [-i input_file] [-s] [-m mode]\n"
            "Options:\n"
            "\t-i,\tinput file path\n"
            "\t-s,\tsymmetrized input graph\n"
            "\t-m,\tuniform[mode=0], skewed[mode=1]\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }
  char c;
  char const *input_path = nullptr;
  bool symmetrized = false;
  int mode = 0;
  while ((c = getopt(argc, argv, "i:s:m")) != -1) {
    switch (c) {
      case 'i':
        input_path = optarg;
        break;
      case 's':
        symmetrized = true;
        break;
      case 'm':
        mode = atoi(optarg);
        break;
    }
  }

  printf("Reading graph...\n");
  Graph<NodeId, EdgeId, EdgeTy> G;
  G.symmetrized = symmetrized;
  G.read_graph(input_path);

  std::string graphname;
  std::size_t pos1 = std::string(input_path).find_last_of("/\\");
  std::size_t pos2 = std::string(input_path).find_last_of(".");
  if (pos1 == std::string::npos) {
    graphname = std::string(input_path).substr(pos2 + 1);
  } else {
    graphname = std::string(input_path).substr(pos1 + 1, pos2 - pos1 - 1);
  }
  printf("Graph: %s |V|: %zu, |E|: %zu\n", graphname.c_str(), G.n, G.m);

  if (mode == 0) {
    generate_weight_uniform(G);
  } else {
    generate_weight_skewed(G);
  }

  std::string output_name = graphname + ".adj";
  G.write_pbbs_format(output_name.c_str());
  return 0;
}
