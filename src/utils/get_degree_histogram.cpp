#include <fstream>
#include <map>

#include "graph.h"

typedef uint32_t NodeId;
typedef uint64_t EdgeId;
#ifdef FLOAT
typedef float EdgeTy;
#else
typedef uint32_t EdgeTy;
#endif

int main(int argc, char *argv[]) {
  if (argc == 1) {
    fprintf(stderr,
            "Usage: %s [-i input_file] [-s]\n"
            "Options:\n"
            "\t-i,\tinput file path\n"
            "\t-s,\tsymmetrized input graph\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }
  char c;
  char const *input_path = nullptr;
  bool symmetrized = false;
  while ((c = getopt(argc, argv, "i:s")) != -1) {
    switch (c) {
      case 'i':
        input_path = optarg;
        break;
      case 's':
        symmetrized = true;
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

  std::map<int, int> mp;
  for (size_t i = 0; i < G.n; i++) {
    size_t deg = G.offsets[i + 1] - G.offsets[i];
    mp[deg]++;
  }
  std::ofstream ofs("histogram/" + graphname + ".tsv");
  for (auto [deg, cnt] : mp) {
    ofs << deg << '\t' << cnt << '\n';
  }
  ofs.close();
  return 0;
}
