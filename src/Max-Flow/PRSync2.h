#include <queue>
#include <limits>
#include <algorithm>

#include "graph.h"
#include "hashbag.h"
#include "parlay/delayed_sequence.h"
#include "parlay/sequence.h"

using namespace std;
using namespace parlay;

template <class Graph>
class PRSync2 {
	using NodeId = typename Graph::NodeId;
	using EdgeId = typename Graph::EdgeId;
	using Edge = typename Graph::Edge;
	using FlowTy = typename Edge::FlowTy;

	// cannot use const Graph& because e.w is modified
	int n;
	NodeId source, sink;
	Graph& G;
	hashbag<NodeId> bag;
	sequence<bool> in_frontier;
	sequence<NodeId> frontier;
	sequence<int> heights;
	sequence<FlowTy> excess;
	
	sequence<FlowTy> enew;
	sequence<int> hnew;

	static constexpr size_t ALPHA = 6;
  	static constexpr size_t BETA = 12;
	const float GLOBAL_UPDATE_FREQ = 3.0;

	size_t work_threshold;
	size_t work_counter = 0;

	public:
	PRSync2() = delete;
	PRSync2(Graph& _G) : G(_G), bag(G.n) {
		n = G.n;
		in_frontier = sequence<bool>(G.n);
		frontier = sequence<NodeId>::uninitialized(G.n);
		heights = sequence<int>::uninitialized(G.n);
		excess = sequence<FlowTy>::uninitialized(G.n);
		
		enew = sequence<FlowTy>::uninitialized(G.n);
		hnew = sequence<int>::uninitialized(G.n);

		work_threshold = ALPHA * n + G.m / 2;
	}

	void add_to_bag(NodeId u) {
		if (u != source && u != sink &&
			compare_and_swap(&in_frontier[u], false, true)) {
			bag.insert(u);
		}
	}

	inline bool need_global_relabel()
	{
		return work_counter * GLOBAL_UPDATE_FREQ > work_threshold;
	}

	void global_relabel()
	{
		cout<<"global relabel\n";
		
		parallel_for(0, G.n, [&](size_t i) {
      		if (i != sink)
        		heights[i] = n;
    	});

		// Sequential BFS for correctness
		sequence<NodeId> current_level(G.n);
		sequence<NodeId> next_level(G.n);
		
		heights[sink] = 0;
		current_level[0] = sink;
		size_t current_size = 1;
		
		int distance = 0;
		while (current_size > 0) 
		{
			size_t next_size = 0;
			
			// Process current level
			for (size_t i = 0; i < current_size; i++) 
			{
				NodeId u = current_level[i];
				
				// Look at incoming edges (reverse edges in residual graph)
				for (size_t j = G.offsets[u]; j < G.offsets[u + 1]; j++) 
				{
					auto& e = G.edges[j];
					NodeId v = e.v;
					
					// Check if reverse edge has capacity and v is not yet labeled
					auto& rev_e = G.edges[e.rev];

					if (rev_e.w > 0 && heights[v] == n && v != source) {
						heights[v] = distance + 1;
						next_level[next_size++] = v;
					}
				}
			}
			
			// Swap levels
			for (size_t i = 0; i < next_size; i++) {
				current_level[i] = next_level[i];
			}
			current_size = next_size;
			distance++;
		}
		
		// Reset work counter
		work_counter = 0;
	}

	void init() {
		parallel_for(0, G.n, [&](size_t i) {
			heights[i] = 0;
			excess[i] = 0;
			enew[i] = 0;
			hnew[i] = 0;
		});

		heights[source] = n;
		parallel_for(G.offsets[source], G.offsets[source + 1], [&](size_t i) {
			auto& e = G.edges[i];
			if (e.w) {
				G.edges[e.rev].w += e.w;
				excess[e.v] += e.w;
				e.w = 0;
				add_to_bag(e.v);
			}
		});
	}

	void push_flow(NodeId u)
	{
		size_t local_work_done = 0;

		for (size_t i = G.offsets[u]; i < G.offsets[u + 1]; i++) 
		{
			if(excess[u] == 0)
				break;
			
			auto& e = G.edges[i];

			if (e.w > 0 && heights[u] == heights[e.v] + 1) 
			{
				FlowTy value = min(e.w, excess[u]);

				excess[u] -= value;
				write_add(&enew[e.v], value);

				e.w -= value;
				G.edges[e.rev].w += value;

				if(value) add_to_bag(e.v);

				local_work_done += BETA;
			}
		}

		write_add(&work_counter, local_work_done); //push work
	}

	int compute_new_label(NodeId u)
	{
		int ans = std::numeric_limits<int>::max();
		for (size_t i = G.offsets[u]; i < G.offsets[u + 1]; i++) 
		{
			auto& e = G.edges[i];
			if (e.w > 0)
				ans = min(ans, heights[e.v]);
		}

		return ans+1;
	}

	FlowTy max_flow(NodeId s, NodeId t) {
		source = s;
		sink = t;
		int round = 0;

		if (source == sink)
			return 0;
		
		init();
		
		while(true) 
		{
			size_t frontier_size = bag.pack_into(frontier);
			if (frontier_size == 0) break;

			parallel_for(0, frontier_size,
							[&](size_t i) { in_frontier[frontier[i]] = false; });
			
			if(round % 100000 == 0)
			{
				printf("Iteration %d: frontier size: %zu\n", round, frontier_size);
				cout<<enew[sink]<<'\n';
			}


			if(need_global_relabel())
				global_relabel();
				

			//step 4 - updating excess values 
			//(for vertices that were not in the frontier before)
			parallel_for(0, frontier_size, [&](size_t i) {
				excess[frontier[i]] += enew[frontier[i]];
				enew[frontier[i]] = 0;
			});

			//step 1 - pushing flow
			parallel_for(0, frontier_size, [&](size_t i) {
				push_flow(frontier[i]);
			});

			//step 2 - compute new labels
			parallel_for(0, frontier_size, [&](size_t i) {
				NodeId u = frontier[i];

				write_add(&work_counter, BETA + (G.offsets[u + 1] - G.offsets[u])); //edge scan work
				if(excess[u] > 0)
				{
					hnew[u] = compute_new_label(u);
					write_add(&work_counter, BETA + (G.offsets[u + 1] - G.offsets[u])); //relabel work
					add_to_bag(u);
				}
			});

			//step 3 & 4 - updating excess values
			parallel_for(0, frontier_size, [&](size_t i) {
				if(excess[frontier[i]] > 0)
					heights[frontier[i]] = hnew[frontier[i]];

				excess[frontier[i]] += enew[frontier[i]];
				enew[frontier[i]] = 0;
			});

			round++;
		}

		return enew[sink];
	} 
};
