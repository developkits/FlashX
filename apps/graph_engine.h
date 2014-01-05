#ifndef __GRAPH_ENGINE_H__
#define __GRAPH_ENGINE_H__

/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "thread.h"
#include "container.h"
#include "concurrency.h"

#include "vertex.h"
#include "vertex_index.h"
#include "trace_logger.h"

class graph_engine;

class vertex_message
{
};


class compute_vertex: public in_mem_vertex_info
{
	atomic_flags<long> activated_levels;
public:
	compute_vertex(vertex_id_t id, off_t off, int size): in_mem_vertex_info(
			id, off, size) {
	}

	/**
	 * This is an atomic operation. We can only activate a vertex in a level
	 * higher than the level where it was activated. i.e., the level of
	 * a vertex can only increase monotonically.
	 * Return true if the vertex is activated in the level successfully.
	 */
	bool activate_in(int level) {
		// This implementation is fast, but it's kind of tricky.
		// We can have only 64 levels at most. Maybe it's enough for
		// most graph algorithms and graphs.
		assert(level < activated_levels.get_num_tot_flags());
		return !activated_levels.set_flag(level);
	}

	bool is_activated(int level) const {
		return activated_levels.test_flag(level);
	}

	virtual bool has_required_vertices() const {
		return false;
	}

	virtual vertex_id_t get_next_required_vertex() {
		assert(0);
		return -1;
	}

	/**
	 * Run user's code when the adjacency list of the vertex is read
	 * from disks.
	 */
	virtual void run(graph_engine &graph, const page_vertex *vertex) = 0;

	/**
	 * Run user's code when the adjacency lists of its neighbors are read
	 * from disks.
	 */
	virtual void run_on_neighbors(graph_engine &graph,
			const page_vertex *vertices[], int num) = 0;

	/**
	 * Run user's code when the vertex receives messages from other.
	 */
	virtual void run_on_messages(graph_engine &,
			const vertex_message *msgs[], int num) = 0;
};

class graph_index
{
public:
	virtual compute_vertex &get_vertex(vertex_id_t id) = 0;

	virtual vertex_id_t get_max_vertex_id() const = 0;

	virtual vertex_id_t get_min_vertex_id() const = 0;

	virtual size_t get_num_vertices() const = 0;

	virtual size_t get_all_vertices(std::vector<vertex_id_t> &vec) const = 0;
};

template<class vertex_type>
class graph_index_impl: public graph_index
{
	// This is an index to the vertex array blow. Some vertices don't have
	// edges so they don't appear in the vertex array blow.
	std::vector<long> in_mem_index;
	// This contains the vertices with edges.
	std::vector<vertex_type> vertices;
	
	graph_index_impl(const std::string &index_file, bool directed) {
		int min_vertex_size;
		if (directed)
			min_vertex_size = sizeof(ext_mem_directed_vertex);
		else
			min_vertex_size = sizeof(ext_mem_undirected_vertex);

		vertex_index *indices = vertex_index::load(index_file);
		in_mem_index.resize(indices->get_num_vertices(), -1);
		size_t num_vertices = in_mem_index.size();
		// Count the number of non-empty vertices.
		// When we know the number of non-empty vertices in advance,
		// we can reduce memory footprint.
		size_t num_non_empty = 0;
		for (size_t i = 0; i < num_vertices; i++) {
			if (indices->get_vertex_size(i) > min_vertex_size)
				num_non_empty++;
		}
		vertices.resize(num_non_empty);

		size_t non_empty_idx = 0;
		for (size_t i = 0; i < num_vertices; i++) {
			off_t off = indices->get_vertex_off(i);
			int size = indices->get_vertex_size(i);
			// We ignore the vertices without edges.
			if (size > min_vertex_size) {
				in_mem_index[i] = non_empty_idx;
				vertices[non_empty_idx++] = vertex_type(i, off, size);
			}
		}
		assert(non_empty_idx == num_non_empty);
		printf("There are %ld vertices and %ld non-empty, vertex array capacity: %ld\n",
				num_vertices, vertices.size(), vertices.capacity());
		vertex_index::destroy(indices);
	}
public:
	static graph_index *create(const std::string &index_file, bool directed) {
		return new graph_index_impl<vertex_type>(index_file, directed);
	}

	virtual compute_vertex &get_vertex(vertex_id_t id) {
		long idx = in_mem_index[id];
		assert(idx >= 0);
		return vertices[idx];
	}

	virtual size_t get_num_vertices() const {
		return vertices.size();
	}

	virtual size_t get_all_vertices(std::vector<vertex_id_t> &vec) const {
		vec.resize(vertices.size());
		for (size_t i = 0; i < vertices.size(); i++)
			vec[i] = vertices[i].get_id();
		return vec.size();
	}

	virtual vertex_id_t get_max_vertex_id() const {
		return vertices.back().get_id();
	}

	virtual vertex_id_t get_min_vertex_id() const {
		return vertices.front().get_id();
	}
};

class vertex_collection;
class sorted_vertex_queue;

class graph_engine
{
	graph_index *vertices;

	// These are two global queues. One contains the vertices that are being
	// processed in the current level. The other contains the vertices that
	// will be processed in the next level.
	// The queue for the current level.
	sorted_vertex_queue *activated_vertices;
	// The queue for the next level.
	vertex_collection *activated_vertex_buf;
	atomic_integer level;
	volatile bool is_complete;

	// These are used for switching queues.
	pthread_mutex_t lock;
	pthread_barrier_t barrier1;
	pthread_barrier_t barrier2;

	thread *first_thread;
	std::vector<thread *> worker_threads;

	bool directed;
	edge_type required_neighbor_type;

	trace_logger *logger;

	int file_id;

protected:
	graph_engine(int num_threads, int num_nodes, const std::string &graph_file,
			graph_index *index, bool directed);
public:
	static graph_engine *create(int num_threads, int num_nodes,
			const std::string &graph_file, graph_index *index, bool directed) {
		return new graph_engine(num_threads, num_nodes, graph_file, index, directed);
	}

	compute_vertex &get_vertex(vertex_id_t id) {
		return vertices->get_vertex(id);
	}

	void start(vertex_id_t ids[], int num);
	void start_all();

	void set_required_neighbor_type(edge_type type) {
		required_neighbor_type = type;
	}

	edge_type get_required_neighbor_type() const {
		return required_neighbor_type;
	}

	/**
	 * The algorithm progresses to the next level.
	 * It returns true if no more work can progress.
	 */
	bool progress_next_level();

	/**
	 * Activate vertices that may be processed in the next level.
	 */
	void activate_vertices(vertex_id_t vertices[], int num);

	/**
	 * Get vertices to be processed in the current level.
	 */
	int get_curr_activated_vertices(vertex_id_t vertices[], int num);
	size_t get_num_curr_activated_vertices() const;

	vertex_id_t get_max_vertex_id() const {
		return vertices->get_max_vertex_id();
	}

	vertex_id_t get_min_vertex_id() const {
		return vertices->get_min_vertex_id();
	}

	void wait4complete();

	int get_num_threads() const {
		return worker_threads.size();
	}

	bool is_directed() const {
		return directed;
	}

	trace_logger *get_logger() const {
		return logger;
	}

	void cleanup() {
		if (logger)
			logger->close();
	}

	/**
	 * Get the file id where the graph data is stored.
	 */
	int get_file_id() const {
		return file_id;
	}

	void send_msg(vertex_id_t id, const vertex_message &msg) {
		const vertex_message *msgs[1];
		msgs[0] = &msg;
		// TODO This is a temporary solution.
		get_vertex(id).run_on_messages(*this, msgs, 1);
	}
};

#endif
