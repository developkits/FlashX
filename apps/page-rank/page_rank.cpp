/**
 * Copyright 2013 Disa Mhembere
 *
 * This file is part of FlashGraph.
 *
 * FlashGraph is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FlashGraph is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FlashGraph.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include <limits>

#include "thread.h"
#include "io_interface.h"
#include "container.h"
#include "concurrency.h"

#include "vertex_index.h"
#include "graph_engine.h"
#include "graph_config.h"

float DAMPING_FACTOR = 0.85;
float TOLERANCE = 1.0E-2; 
int num_iters = INT_MAX;

class pgrank_vertex: public compute_vertex
{
  float curr_itr_pr; // Current iteration's page rank
  vsize_t num_out_edges;

public:
	pgrank_vertex() {
		num_out_edges = 0;
	}

  pgrank_vertex(vertex_id_t id, const vertex_index *index1): 
        compute_vertex(id, index1) {
    this->curr_itr_pr = 1 - DAMPING_FACTOR; // Must be this
	directed_vertex_index *index = (directed_vertex_index *) index1;
	num_out_edges = index->get_num_out_edges(id);
  }

  vsize_t get_num_out_edges() const {
	  return num_out_edges;
  }

  vsize_t get_num_in_edges() const {
	  return get_num_edges() - num_out_edges;
  }

  float get_curr_itr_pr() const{
    return curr_itr_pr;
  }

  void run(graph_engine &graph) { 
	// We perform pagerank for at most `num_iters' iterations.
	if (graph.get_curr_level() >= num_iters)
		return;
    vertex_id_t id = get_id();
    request_vertices(&id, 1); // put my edgelist in page cache
  };

	void run(graph_engine &graph, const page_vertex &vertex);

	void run_on_message(graph_engine &,
/* Only serves to activate on the next iteration */
			const vertex_message &msg) { }; 
};

void pgrank_vertex::run(graph_engine &graph, const page_vertex &vertex) {
  // Gather
  float accum = 0;
  page_byte_array::const_iterator<vertex_id_t> end_it
    = vertex.get_neigh_end(IN_EDGE);
  
  for (page_byte_array::const_iterator<vertex_id_t> it
      = vertex.get_neigh_begin(IN_EDGE); it != end_it; ++it) {
    vertex_id_t id = *it;
    pgrank_vertex& v = (pgrank_vertex&) graph.get_vertex(id);
    // Notice I want this iteration's pagerank
    accum += (v.get_curr_itr_pr()/v.get_num_out_edges()); 
  }   

  // Apply
  float last_change = 0;
  if (get_num_in_edges() > 0) {
    float new_pr = ((1 - DAMPING_FACTOR)) + (DAMPING_FACTOR*(accum));
    last_change = new_pr - curr_itr_pr;
    curr_itr_pr = new_pr;
  }   
  
  // Scatter (activate your out-neighbors ... if you have any :) 
  if ( std::fabs( last_change ) > TOLERANCE ) {
    page_byte_array::const_iterator<vertex_id_t> end_it
      = vertex.get_neigh_end(OUT_EDGE);
    stack_array<vertex_id_t, 1024> dest_buf(vertex.get_num_edges(OUT_EDGE));
    int num_dests = 0;
    for (page_byte_array::const_iterator<vertex_id_t> it
        = vertex.get_neigh_begin(OUT_EDGE); it != end_it; ++it) {
      vertex_id_t id = *it;
      dest_buf[num_dests++] = id; 
    }   

    if (num_dests > 0) {
      graph.activate_vertices(dest_buf.data(), num_dests) ;
    }   
  }
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

void print_usage()
{
	fprintf(stderr,
			"page-rank [options] conf_file graph_file index_file damping_factor\n");
	fprintf(stderr, "-c confs: add more configurations to the system\n");
	fprintf(stderr, "-p: preload the graph\n");
	fprintf(stderr, "-i num: specify the maximal number of iterations\n");
	graph_conf.print_help();
	params.print_help();
}

int main(int argc, char *argv[])
{
	int opt;
	std::string confs;
	int num_opts = 0;
	bool preload = false;
	while ((opt = getopt(argc, argv, "c:pi:")) != -1) {
		num_opts++;
		switch (opt) {
			case 'c':
				confs = optarg;
				num_opts++;
				break;
			case 'p':
				preload = true;
				break;
			case 'i':
				num_iters = atoi(optarg);
				num_opts++;
				break;
			default:
				print_usage();
		}
	}
	argv += 1 + num_opts;
	argc -= 1 + num_opts;

	if (argc < 4) {
		print_usage();
		exit(-1);
	}

	std::string conf_file = argv[0];
	std::string graph_file = argv[1];
	std::string index_file = argv[2];
	DAMPING_FACTOR = atof(argv[3]);

  if (DAMPING_FACTOR < 0 || DAMPING_FACTOR > 1) {
    fprintf(stderr, "Damping factor must be between 0 and 1 inclusive\n");
    exit(-1);
  }

	config_map configs(conf_file);
	configs.add_options(confs);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	graph_index *index = NUMA_graph_index<pgrank_vertex>::create(index_file,
			graph_conf.get_num_threads(), params.get_num_nodes());
	graph_engine *graph = graph_engine::create(graph_conf.get_num_threads(),
			params.get_num_nodes(), graph_file, index);
	if (preload)
		graph->preload_graph();
	printf("Pagerank (at maximal %d iterations) starting\n", num_iters);
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());


	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start_all(); 
  graph->wait4complete();
	gettimeofday(&end, NULL);

	NUMA_graph_index<pgrank_vertex>::const_iterator it
		= ((NUMA_graph_index<pgrank_vertex> *) index)->begin();
	NUMA_graph_index<pgrank_vertex>::const_iterator end_it
		= ((NUMA_graph_index<pgrank_vertex> *) index)->end();
  
#if 0
	for (; it != end_it; ++it) {
		const pgrank_vertex &v = (const pgrank_vertex &) *it;
    printf("%d:%f\n", v.get_id()+1, v.get_curr_itr_pr());
	}
#endif

#if 1
	float total = 0;
  vsize_t count = 0;

	for (; it != end_it; ++it) {
		const pgrank_vertex &v = (const pgrank_vertex &) *it;
    total += v.get_curr_itr_pr();
    count++;
	}

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph_engine::destroy(graph);
	destroy_io_system();
  
  printf("The %d vertices have page rank sum: %f\n in %f seconds\n", 
      count, total, time_diff(start, end));
#endif
}
