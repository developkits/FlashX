/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <libgen.h>
#include <malloc.h>

#include <unordered_map>

#include "safs_file.h"
#include "io_request.h"
#include "io_interface.h"
#include "in_mem_io.h"

#include "EM_vector.h"
#include "matrix_config.h"
#include "mem_worker_thread.h"
#include "local_vec_store.h"
#include "matrix_store.h"

namespace fm
{

namespace detail
{

static safs::file_io_factory::shared_ptr create_temp_file(size_t num_bytes)
{
	char *tmp = tempnam(".", "vec");
	std::string tmp_name = basename(tmp);
	safs::safs_file f(safs::get_sys_RAID_conf(), tmp_name);
	assert(!f.exist());
	bool ret = f.create_file(num_bytes);
	assert(ret);
	safs::file_io_factory::shared_ptr factory
		= safs::create_io_factory(tmp_name, safs::REMOTE_ACCESS);
	free(tmp);
	return factory;
}

EM_vec_store::ptr EM_vec_store::cast(vec_store::ptr vec)
{
	if (vec->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error) << "Can't cast an in-mem vector to EM_vec_store";
		return EM_vec_store::ptr();
	}
	return std::static_pointer_cast<EM_vec_store>(vec);
}

EM_vec_store::const_ptr EM_vec_store::cast(vec_store::const_ptr vec)
{
	if (vec->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error) << "Can't cast an in-mem vector to EM_vec_store";
		return EM_vec_store::const_ptr();
	}
	return std::static_pointer_cast<const EM_vec_store>(vec);
}

EM_vec_store::EM_vec_store(size_t length, const scalar_type &type): vec_store(
		length, type, false)
{
	factory = create_temp_file(length * type.get_size());
	int ret = pthread_key_create(&io_key, NULL);
	assert(ret == 0);
	pthread_spin_init(&io_lock, PTHREAD_PROCESS_PRIVATE);
}

EM_vec_store::~EM_vec_store()
{
	pthread_spin_destroy(&io_lock);
	pthread_key_delete(io_key);
	thread_ios.clear();
	if (factory) {
		std::string file_name = factory->get_name();
		factory = NULL;
		safs::safs_file f(safs::get_sys_RAID_conf(), file_name);
		assert(f.exist());
		f.delete_file();
	}
}

bool EM_vec_store::resize(size_t length)
{
	// TODO
	assert(0);
	return false;
}

bool EM_vec_store::append(
		std::vector<vec_store::const_ptr>::const_iterator vec_it,
		std::vector<vec_store::const_ptr>::const_iterator vec_end)
{
	assert(0);
}

bool EM_vec_store::append(const vec_store &vec)
{
	assert(0);
}

vec_store::ptr EM_vec_store::deep_copy() const
{
	assert(0);
}

vec_store::ptr EM_vec_store::shallow_copy()
{
	assert(0);
}

vec_store::const_ptr EM_vec_store::shallow_copy() const
{
	assert(0);
}

size_t EM_vec_store::get_portion_size() const
{
	return matrix_conf.get_anchor_gap_size() / get_entry_size();
}

local_vec_store::ptr EM_vec_store::get_portion_async(off_t start,
		size_t size, portion_compute::ptr compute) const
{
	safs::io_interface &io = get_curr_io();
	local_buf_vec_store::ptr buf(new local_buf_vec_store(start, size,
				get_type(), -1));
	off_t off = get_byte_off(start);
	safs::data_loc_t loc(io.get_file_id(), off);
	safs::io_request req(buf->get_raw_arr(), loc,
			buf->get_length() * buf->get_entry_size(), READ);
	static_cast<portion_callback &>(io.get_callback()).add(req, compute);
	io.access(&req, 1);
	// TODO I might want to flush requests later.
	io.flush_requests();
	return buf;
}

void EM_vec_store::write_portion(local_vec_store::const_ptr store, off_t off)
{
	off_t start = off;
	if (start < 0)
		start = store->get_global_start();
	assert(start >= 0);

	safs::io_interface &io = get_curr_io();
	off_t off_in_bytes = get_byte_off(start);
	safs::data_loc_t loc(io.get_file_id(), off_in_bytes);
	safs::io_request req(const_cast<char *>(store->get_raw_arr()), loc,
			store->get_length() * store->get_entry_size(), WRITE);
	portion_compute::ptr compute(new portion_write_complete(store));
	static_cast<portion_callback &>(io.get_callback()).add(req, compute);
	io.access(&req, 1);
	// TODO I might want to flush requests later.
	io.flush_requests();
}

void EM_vec_store::reset_data()
{
	assert(0);
}

vec_store::ptr EM_vec_store::sort_with_index()
{
	assert(0);
}

safs::io_interface::ptr EM_vec_store::create_io()
{
	thread *t = thread::get_curr_thread();
	assert(t);
	pthread_spin_lock(&io_lock);
	auto it = thread_ios.find(t);
	if (it == thread_ios.end()) {
		safs::io_interface::ptr io = safs::create_io(factory, t);
		io->set_callback(portion_callback::ptr(new portion_callback()));
		thread_ios.insert(std::pair<thread *, safs::io_interface::ptr>(t, io));
		pthread_setspecific(io_key, io.get());
		pthread_spin_unlock(&io_lock);
		return io;
	}
	else {
		safs::io_interface::ptr io = it->second;
		pthread_spin_unlock(&io_lock);
		return io;
	}
}

void EM_vec_store::destroy_ios()
{
	pthread_key_delete(io_key);
	int ret = pthread_key_create(&io_key, NULL);
	assert(ret == 0);
	thread_ios.clear();
}

safs::io_interface &EM_vec_store::get_curr_io() const
{
	void *io_addr = pthread_getspecific(io_key);
	assert(io_addr);
	return *(safs::io_interface *) io_addr;
}

class EM_vec_dispatcher: public task_dispatcher
{
	const EM_vec_store &store;
	off_t portion_idx;
	pthread_spinlock_t lock;
	size_t portion_size;
public:
	EM_vec_dispatcher(const EM_vec_store &_store,
			size_t portion_size = 0): store(_store) {
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
		portion_idx = 0;
		if (portion_size == 0)
			this->portion_size = store.get_portion_size();
		else
			this->portion_size = portion_size;
	}

	virtual bool issue_task() {
		pthread_spin_lock(&lock);
		off_t global_start = portion_idx * portion_size;
		if ((size_t) global_start >= store.get_length()) {
			pthread_spin_unlock(&lock);
			return false;
		}
		size_t length = std::min(portion_size, store.get_length() - global_start);
		portion_idx++;
		pthread_spin_unlock(&lock);
		create_vec_task(global_start, length, store);
		return true;
	}

	virtual void create_vec_task(off_t global_start, size_t length,
			const EM_vec_store &from_vec) = 0;
};

///////////////////////////// Sort the vector /////////////////////////////////

namespace EM_sort_detail
{

anchor_prio_queue::anchor_prio_queue(const std::vector<local_buf_vec_store::ptr> &anchor_vals)
{
	for (size_t i = 0; i < anchor_vals.size(); i++) {
		anchor_struct anchor;
		anchor.local_anchors = anchor_vals[i];
		anchor.id = i;
		anchor.curr_off = 0;
		const scalar_type &type = anchor_vals.front()->get_type();
		anchor.gt = type.get_basic_ops().get_op(basic_ops::op_idx::GT);;
		queue.push(anchor);
	}
	const scalar_type &type = anchor_vals.front()->get_type();
	anchor_gap_size = matrix_conf.get_anchor_gap_size() / type.get_size();
	sort_buf_size = matrix_conf.get_sort_buf_size() / type.get_size();
}

off_t anchor_prio_queue::get_anchor_off(const anchor_struct &anchor) const
{
	return anchor.id * sort_buf_size + anchor.curr_off * anchor_gap_size;
}

scalar_variable::ptr anchor_prio_queue::get_min_frontier() const
{
	if (queue.empty())
		return scalar_variable::ptr();
	else {
		local_buf_vec_store::const_ptr local_anchors = queue.top().local_anchors;
		const scalar_type &type = local_anchors->get_type();
		scalar_variable::ptr var = type.create_scalar();
		assert((size_t) queue.top().curr_off < local_anchors->get_length());
		var->set_raw(local_anchors->get(queue.top().curr_off),
				type.get_size());
		return var;
	}
}

/*
 * Here we pop a set of chunks of data whose values are the potentially
 * the smallest.
 */
std::vector<off_t> anchor_prio_queue::pop(size_t size)
{
	std::vector<off_t> chunks;
	long remaining_size = size;
	while (remaining_size > 0 && !queue.empty()) {
		anchor_struct anchor = queue.top();
		off_t off = get_anchor_off(anchor);
		chunks.push_back(off);
		remaining_size -= anchor_gap_size;
		queue.pop();

		// If there are still anchors left in the partition, we should
		// update it and put it back to the priority.
		anchor.curr_off++;
		if (anchor.local_anchors->get_length() > (size_t) anchor.curr_off)
			queue.push(anchor);
	}
	return chunks;
}

sort_portion_summary::sort_portion_summary(const scalar_type &type,
		size_t num_sort_bufs)
{
	size_t entry_size = type.get_size();
	anchor_gap_size = matrix_conf.get_anchor_gap_size() / entry_size;
	sort_buf_size = matrix_conf.get_sort_buf_size() / entry_size;
	anchor_vals.resize(num_sort_bufs);
}

void sort_portion_summary::add_portion(local_buf_vec_store::const_ptr sorted_buf)
{
	std::vector<off_t> idxs;
	for (size_t i = 0; i < sorted_buf->get_length(); i += anchor_gap_size)
		idxs.push_back(i);
	//		assert(idxs.back() < sorted_buf->get_length());
	//		if ((size_t) idxs.back() != sorted_buf->get_length() - 1)
	//			idxs.push_back(sorted_buf->get_length() - 1);
	//		assert(idxs.back() < sorted_buf->get_length());
	off_t idx = sorted_buf->get_global_start() / sort_buf_size;
	assert(anchor_vals[idx] == NULL);
	anchor_vals[idx] = sorted_buf->get(idxs);
	if ((size_t) idx == anchor_vals.size() - 1)
		assert(sorted_buf->get_length() <= sort_buf_size);
	else
		assert(sorted_buf->get_length() == sort_buf_size);
}

anchor_prio_queue::ptr sort_portion_summary::get_prio_queue() const
{
	return anchor_prio_queue::ptr(new anchor_prio_queue(anchor_vals));
}

void EM_vec_sort_compute::run(char *buf, size_t size)
{
	num_completed++;
	if (num_completed == portions.size()) {
		// Sort each portion in parallel.
		// Here we rely on OpenMP to sort the data in the buffer in parallel.
		local_buf_vec_store::ptr sort_buf = portions.front();
		std::vector<off_t> orig_offs(sort_buf->get_length());
		sort_buf->get_type().get_sorter().sort_with_index(
				sort_buf->get_raw_arr(), orig_offs.data(),
				sort_buf->get_length(), false);
		summary.add_portion(sort_buf);

		// Write the sorting result to disks.
		to_vecs.front()->write_portion(sort_buf);
		for (size_t i = 1; i < portions.size(); i++) {
			local_vec_store::ptr shuffle_buf = portions[i]->get(orig_offs);
			assert(shuffle_buf->is_sorted());
			to_vecs[i]->write_portion(shuffle_buf,
					portions[i]->get_global_start());
		}
	}
}

class EM_vec_sort_dispatcher: public EM_vec_dispatcher
{
	std::shared_ptr<sort_portion_summary> summary;
	std::vector<EM_vec_store::const_ptr> from_vecs;
	std::vector<EM_vec_store::ptr> to_vecs;
public:
	typedef std::shared_ptr<EM_vec_sort_dispatcher> ptr;

	EM_vec_sort_dispatcher(const std::vector<EM_vec_store::const_ptr> &from_vecs,
			const std::vector<EM_vec_store::ptr> &to_vecs);

	const sort_portion_summary &get_sort_summary() const {
		return *summary;
	}

	virtual void create_vec_task(off_t global_start,
			size_t length, const EM_vec_store &from_vec);
};

EM_vec_sort_dispatcher::EM_vec_sort_dispatcher(
		const std::vector<EM_vec_store::const_ptr> &from_vecs,
		const std::vector<EM_vec_store::ptr> &to_vecs): EM_vec_dispatcher(
			*from_vecs.front(),
			// We want to have a larger buffer for sorting.
			matrix_conf.get_sort_buf_size() / from_vecs.front()->get_entry_size())
{
	EM_vec_store::const_ptr sort_vec = from_vecs.front();
	size_t sort_buf_size
		= matrix_conf.get_sort_buf_size() / sort_vec->get_entry_size();
	size_t num_sort_bufs
		= ceil(((double) sort_vec->get_length()) / sort_buf_size);
	summary = std::shared_ptr<sort_portion_summary>(
			new sort_portion_summary(sort_vec->get_type(), num_sort_bufs));
	this->from_vecs = from_vecs;
	this->to_vecs = to_vecs;
}

void EM_vec_sort_dispatcher::create_vec_task(off_t global_start,
			size_t length, const EM_vec_store &from_vec)
{
	EM_vec_sort_compute *sort_compute = new EM_vec_sort_compute(to_vecs,
			*summary);
	portion_compute::ptr compute(sort_compute);
	std::vector<local_vec_store::ptr> from_portions(from_vecs.size());
	for (size_t i = 0; i < from_portions.size(); i++)
		from_portions[i] = from_vecs[i]->get_portion_async(
				global_start, length, compute);
	sort_compute->set_bufs(from_portions);
}

/////////////////// Merge portions ///////////////////////

class merge_writer
{
	size_t local_buf_size;	// In the number of elements
	off_t merge_end;		// In the number of bytes
	local_buf_vec_store::ptr buf;
	size_t data_size_in_buf;		// In the number of elements
	EM_vec_store::ptr to_vec;
public:
	merge_writer(EM_vec_store::ptr vec) {
		this->local_buf_size
			= matrix_conf.get_write_io_buf_size() / vec->get_type().get_size();
		this->to_vec = vec;
		merge_end = 0;
		buf = local_buf_vec_store::ptr(new local_buf_vec_store(
					-1, local_buf_size, to_vec->get_type(), -1));
		data_size_in_buf = 0;
	}

	void flush_buffer_data() {
		buf->resize(data_size_in_buf);
		const scalar_type &type = buf->get_type();
		assert(merge_end % type.get_size() == 0);
		to_vec->write_portion(buf, merge_end / type.get_size());
		merge_end += data_size_in_buf * type.get_size();

		// The data is written asynchronously, we need to allocate a new buffer.
		buf = local_buf_vec_store::ptr(new local_buf_vec_store(
					-1, local_buf_size, type, -1));
		data_size_in_buf = 0;
	}

	void append(local_vec_store::ptr data) {
		// In the number of elements.
		off_t off_in_new_data = 0;
		size_t new_data_size = data->get_length();

		while (new_data_size > 0) {
			size_t copy_data_size = std::min(new_data_size,
					// The size of the available space in the buffer.
					buf->get_length() - data_size_in_buf);
			// We always copy the data to the local buffer and write
			// the local buffer to disks. The reason of doing so is to
			// make sure the size and offset of data written to disks
			// are aligned to the I/O block size.
			// TODO maybe we should remove this extra memory copy.
			memcpy(buf->get(data_size_in_buf), data->get(off_in_new_data),
					copy_data_size * buf->get_entry_size());
			data_size_in_buf += copy_data_size;
			// Update the amount of data in the incoming data buffer.
			off_in_new_data += copy_data_size;
			new_data_size -= copy_data_size;

			// If the buffer is full, we need to write it out first.
			if (data_size_in_buf == buf->get_length())
				flush_buffer_data();
		}
	}
};

class EM_vec_merge_dispatcher: public task_dispatcher
{
	std::vector<EM_vec_store::const_ptr> from_vecs;
	std::vector<local_buf_vec_store::ptr> prev_leftovers;
	anchor_prio_queue::ptr anchors;
	size_t sort_buf_size;
	std::vector<merge_writer> writers;
public:
	EM_vec_merge_dispatcher(const std::vector<EM_vec_store::const_ptr> &from_vecs,
			const std::vector<EM_vec_store::ptr> &to_vecs,
			anchor_prio_queue::ptr anchors);
	void set_prev_leftovers(
			const std::vector<local_buf_vec_store::ptr> &prev_leftovers) {
		this->prev_leftovers = prev_leftovers;
	}

	merge_writer &get_merge_writer(int idx) {
		return writers[idx];
	}

	const anchor_prio_queue &get_anchors() const {
		return *anchors;
	}

	virtual bool issue_task();
};

EM_vec_merge_compute::EM_vec_merge_compute(
		const std::vector<local_buf_vec_store::ptr> &prev_leftovers,
		EM_vec_merge_dispatcher &_dispatcher): dispatcher(_dispatcher)
{
	stores.resize(prev_leftovers.size());
	for (size_t i = 0; i < prev_leftovers.size(); i++) {
		// If there is a leftover for a vector from the previous merge,
		// all vectors should have the same number of leftover elements.
		if (prev_leftovers[0]) {
			assert(prev_leftovers[i]);
			assert(prev_leftovers[0]->get_length()
					== prev_leftovers[i]->get_length());
		}
		if (prev_leftovers[i])
			this->stores[i].push_back(prev_leftovers[i]);
	}
	num_completed = 0;
}

void EM_vec_merge_compute::set_bufs(const std::vector<merge_set_t> &bufs)
{
	assert(bufs.size() == stores.size());
	num_expected = 0;
	for (size_t i = 0; i < bufs.size(); i++) {
		// If all vectors should have the same number of buffers to merge.
		assert(bufs[0].size() == bufs[i].size());
		num_expected += bufs[i].size();
		this->stores[i].insert(this->stores[i].end(), bufs[i].begin(),
				bufs[i].end());
	}
}

EM_vec_merge_dispatcher::EM_vec_merge_dispatcher(
		const std::vector<EM_vec_store::const_ptr> &from_vecs,
		const std::vector<EM_vec_store::ptr> &to_vecs,
		anchor_prio_queue::ptr anchors)
{
	this->from_vecs = from_vecs;
	assert(from_vecs.size() == to_vecs.size());
	for (size_t i = 0; i < from_vecs.size(); i++)
		assert(from_vecs[i]->get_type() == to_vecs[i]->get_type());
	this->anchors = anchors;
	for (size_t i = 0; i < to_vecs.size(); i++)
		writers.emplace_back(to_vecs[i]);
	sort_buf_size
		= matrix_conf.get_sort_buf_size() / from_vecs[0]->get_entry_size();
	prev_leftovers.resize(from_vecs.size());
}

bool EM_vec_merge_dispatcher::issue_task()
{
	typedef std::vector<local_buf_vec_store::const_ptr> merge_set_t;
	size_t leftover = 0;
	assert(!prev_leftovers.empty());
	if (prev_leftovers[0])
		leftover = prev_leftovers[0]->get_length();
	assert(sort_buf_size > leftover);
	std::vector<off_t> anchor_locs = anchors->pop(sort_buf_size - leftover);
	// If there isn't any data to merge and there isn't leftover from
	// the previous merge.
	if (anchor_locs.empty() && prev_leftovers[0] == NULL) {
		assert(anchors->get_min_frontier() == NULL);
		assert(leftover == 0);
		// If there is nothing left to merge and there isn't leftover, we still
		// need to flush the buffered data.
		for (size_t i = 0; i < writers.size(); i++)
			writers[i].flush_buffer_data();
		return false;
	}
	else {
		size_t anchor_gap_size
			= matrix_conf.get_anchor_gap_size() / from_vecs[0]->get_type().get_size();
		// Merge the anchors.
		std::vector<std::pair<off_t, size_t> > data_locs;
		std::sort(anchor_locs.begin(), anchor_locs.end());
		for (size_t i = 0; i < anchor_locs.size(); i++) {
			size_t num_eles = std::min(anchor_gap_size,
					from_vecs[0]->get_length() - anchor_locs[i]);
			size_t off = anchor_locs[i];
			// If the anchors are contiguous, we merge them.
			while (i + 1 < anchor_locs.size()
					&& (size_t) anchor_locs[i + 1] == anchor_locs[i] + anchor_gap_size) {
				i++;
				num_eles += std::min(anchor_gap_size,
						from_vecs[0]->get_length() - anchor_locs[i]);
			}
			data_locs.push_back(std::pair<off_t, size_t>(off, num_eles));
		}

		// In this case, we need to read some data from the disks first and then
		// merge with the data left from the previous merge.
		if (!data_locs.empty()) {
			EM_vec_merge_compute *_compute = new EM_vec_merge_compute(
					prev_leftovers, *this);
			portion_compute::ptr compute(_compute);
			std::vector<merge_set_t> merge_sets(from_vecs.size());
			for (size_t j = 0; j < from_vecs.size(); j++) {
				merge_set_t portions(data_locs.size());
				for (size_t i = 0; i < data_locs.size(); i++)
					portions[i] = from_vecs[j]->get_portion_async(
							data_locs[i].first, data_locs[i].second, compute);
				merge_sets[j] = portions;
			}
			_compute->set_bufs(merge_sets);
		}
		// In this case, we don't need to read data from disks any more.
		// We only need to write the data left from the previous merge.
		else {
			for (size_t i = 0; i < writers.size(); i++) {
				if (prev_leftovers[i])
					writers[i].append(prev_leftovers[i]);
				// This is the last write. we should flush everything to disks.
				writers[i].flush_buffer_data();
				// No more leftover.
				prev_leftovers[i] = NULL;
			}
		}
		return true;
	}
}

void EM_vec_merge_compute::run(char *buf, size_t size)
{
	num_completed++;
	// If all data in the buffers is ready, we should merge all the buffers.
	if (num_completed == num_expected) {
		assert(stores.size() > 0);
		merge_set_t &merge_bufs = stores[0];
		// Find the min values among the last elements in the buffers.
		const scalar_type &type = merge_bufs.front()->get_type();
		scalar_variable::ptr min_val
			= dispatcher.get_anchors().get_min_frontier();

		// Breaks the local buffers into two parts. The first part is to
		// merge with others; we have to keep the second part for further
		// merging.
		std::vector<std::pair<const char *, const char *> > merge_data(
				merge_bufs.size());
		std::vector<std::pair<const char *, const char *> > leftovers(
				merge_bufs.size());
		std::vector<size_t> merge_sizes(merge_bufs.size());
		size_t leftover_size = 0;
		size_t merge_size = 0;
		for (size_t i = 0; i < merge_bufs.size(); i++) {
			const char *start = merge_bufs[i]->get_raw_arr();
			const char *end = merge_bufs[i]->get_raw_arr()
				+ merge_bufs[i]->get_length() * merge_bufs[i]->get_entry_size();
			off_t leftover_start;
			if (min_val != NULL)
				leftover_start = type.get_stl_algs().lower_bound(
						start, end, min_val->get_raw());
			else
				leftover_start = merge_bufs[i]->get_length();
			merge_sizes[i] = leftover_start;
			merge_size += leftover_start;
			leftover_size += (merge_bufs[i]->get_length() - leftover_start);
			merge_data[i] = std::pair<const char *, const char *>(
					merge_bufs[i]->get(0), merge_bufs[i]->get(leftover_start));
			leftovers[i] = std::pair<const char *, const char *>(
					merge_bufs[i]->get(leftover_start),
					merge_bufs[i]->get(merge_bufs[i]->get_length()));
		}

		// Here we rely on OpenMP to merge the data in the buffer in parallel.
		local_buf_vec_store::ptr merge_res(new local_buf_vec_store(-1,
					merge_size, type, -1));
		std::vector<std::pair<int, off_t> > merge_index(merge_size);
		type.get_sorter().merge_with_index(merge_data,
				merge_res->get_raw_arr(), merge_size, merge_index);
		// Write the merge result to disks.
		dispatcher.get_merge_writer(0).append(merge_res);
		merge_res = NULL;

		std::vector<std::pair<int, off_t> > leftover_merge_index(leftover_size);
		std::vector<local_buf_vec_store::ptr> leftover_bufs(stores.size());
		if (leftover_size > 0) {
			// Keep the leftover and merge them into a single buffer.
			local_buf_vec_store::ptr leftover_buf = local_buf_vec_store::ptr(
					new local_buf_vec_store(-1, leftover_size, type, -1));
			type.get_sorter().merge_with_index(leftovers,
					leftover_buf->get_raw_arr(), leftover_size,
					leftover_merge_index);
			leftover_bufs[0] = leftover_buf;
		}

		// Merge the remaining vectors accordingly.
		for (size_t i = 1; i < stores.size(); i++) {
			std::vector<std::pair<const char *, const char *> > merge_data(
					merge_sizes.size());
			std::vector<std::pair<const char *, const char *> > leftovers(
					merge_sizes.size());

			merge_set_t &set = stores[i];
			for (size_t i = 0; i < set.size(); i++) {
				off_t leftover_start = merge_sizes[i];
				merge_data[i] = std::pair<const char *, const char *>(
						set[i]->get(0), set[i]->get(leftover_start));
				leftovers[i] = std::pair<const char *, const char *>(
						set[i]->get(leftover_start),
						set[i]->get(set[i]->get_length()));
			}

			// Merge the part that can be merged.
			const scalar_type &type = set.front()->get_type();
			merge_res = local_buf_vec_store::ptr(new local_buf_vec_store(-1,
						merge_size, type, -1));
			type.get_sorter().merge(merge_data, merge_index,
					merge_res->get_raw_arr(), merge_size);
			dispatcher.get_merge_writer(i).append(merge_res);

			// Keep the leftover and merge them into a single buffer.
			local_buf_vec_store::ptr leftover_buf = local_buf_vec_store::ptr(
					new local_buf_vec_store(-1, leftover_size, type, -1));
			type.get_sorter().merge(leftovers, leftover_merge_index,
					leftover_buf->get_raw_arr(), leftover_size);
			leftover_bufs[i] = leftover_buf;
		}

		dispatcher.set_prev_leftovers(leftover_bufs);
	}
}

}

std::vector<EM_vec_store::ptr> sort(
		const std::vector<EM_vec_store::const_ptr> &vecs)
{
	assert(vecs.size() > 0);
	for (size_t i = 1; i < vecs.size(); i++) {
		if (vecs[i]->get_length() != vecs[0]->get_length()) {
			BOOST_LOG_TRIVIAL(error) << "Not all vectors have the same length";
			return std::vector<EM_vec_store::ptr>();
		}
	}

#if 0
	assert(matrix_conf.get_sort_buf_size() % get_entry_size() == 0);
	size_t sort_buf_size = matrix_conf.get_sort_buf_size() / get_entry_size();
	size_t portion_size = get_portion_size();
	assert(sort_buf_size >= portion_size);
	assert(sort_buf_size % portion_size == 0);
	assert(matrix_conf.get_anchor_gap_size() % get_entry_size() == 0);
	size_t anchor_gap_size = matrix_conf.get_anchor_gap_size() / get_entry_size();
	assert(anchor_gap_size >= portion_size);
	assert(anchor_gap_size % portion_size == 0);
#endif

	/*
	 * Divide the vector into multiple large parts and sort each part in parallel.
	 */
	std::vector<EM_vec_store::ptr> tmp_vecs(vecs.size());
	for (size_t i = 0; i < vecs.size(); i++)
		tmp_vecs[i] = EM_vec_store::create(vecs[i]->get_length(),
				vecs[i]->get_type());
	EM_sort_detail::EM_vec_sort_dispatcher::ptr sort_dispatcher(
			new EM_sort_detail::EM_vec_sort_dispatcher(vecs, tmp_vecs));
	io_worker_task sort_worker(sort_dispatcher, 1);
	for (size_t i = 0; i < vecs.size(); i++) {
		sort_worker.register_EM_obj(const_cast<EM_vec_store *>(vecs[i].get()));
		sort_worker.register_EM_obj(tmp_vecs[i].get());
	}
	sort_worker.run();
	for (size_t i = 0; i < vecs.size(); i++) {
		const_cast<EM_vec_store &>(*vecs[i]).destroy_ios();
		tmp_vecs[i]->destroy_ios();
	}

	/* Merge all parts.
	 * Here we assume that one level of merging is enough and we rely on
	 * OpenMP to parallelize merging.
	 */
	std::vector<EM_vec_store::ptr> out_vecs(vecs.size());
	for (size_t i = 0; i < vecs.size(); i++)
		out_vecs[i] = EM_vec_store::create(vecs[i]->get_length(),
				vecs[i]->get_type());
	std::vector<EM_vec_store::const_ptr> tmp_vecs1(tmp_vecs.begin(),
			tmp_vecs.end());
	EM_sort_detail::EM_vec_merge_dispatcher::ptr merge_dispatcher(
			new EM_sort_detail::EM_vec_merge_dispatcher(tmp_vecs1, out_vecs,
				sort_dispatcher->get_sort_summary().get_prio_queue()));
	// TODO let's not use asynchornous I/O for now.
	io_worker_task merge_worker(merge_dispatcher, 0);
	for (size_t i = 0; i < vecs.size(); i++) {
		merge_worker.register_EM_obj(tmp_vecs[i].get());
		merge_worker.register_EM_obj(out_vecs[i].get());
	}
	merge_worker.run();
	for (size_t i = 0; i < vecs.size(); i++) {
		tmp_vecs[i]->destroy_ios();
		out_vecs[i]->destroy_ios();
	}
	return out_vecs;
}

void EM_vec_store::sort()
{
#if 0
	assert(matrix_conf.get_sort_buf_size() % get_entry_size() == 0);
	size_t sort_buf_size = matrix_conf.get_sort_buf_size() / get_entry_size();
	size_t portion_size = get_portion_size();
	assert(sort_buf_size >= portion_size);
	assert(sort_buf_size % portion_size == 0);
	assert(matrix_conf.get_anchor_gap_size() % get_entry_size() == 0);
	size_t anchor_gap_size = matrix_conf.get_anchor_gap_size() / get_entry_size();
	assert(anchor_gap_size >= portion_size);
	assert(anchor_gap_size % portion_size == 0);
#endif

	/*
	 * Divide the vector into multiple large parts and sort each part in parallel.
	 */
	struct empty_free {
		void operator()(EM_vec_store *) {
		}
	};
	std::vector<EM_vec_store::const_ptr> in_vecs(1);
	std::vector<EM_vec_store::ptr> out_vecs(1);
	in_vecs[0] = EM_vec_store::const_ptr(this, empty_free());
	out_vecs[0] = EM_vec_store::ptr(this, empty_free());
	EM_sort_detail::EM_vec_sort_dispatcher::ptr sort_dispatcher(
			new EM_sort_detail::EM_vec_sort_dispatcher(in_vecs, out_vecs));
	io_worker_task sort_worker(sort_dispatcher, 1);
	sort_worker.register_EM_obj(this);
	sort_worker.run();
	this->destroy_ios();

	/* Merge all parts.
	 * Here we assume that one level of merging is enough and we rely on
	 * OpenMP to parallelize merging.
	 */
	EM_vec_store::ptr tmp = EM_vec_store::create(get_length(), get_type());
	in_vecs[0] = EM_vec_store::const_ptr(this, empty_free());
	out_vecs[0] = tmp;
	EM_sort_detail::EM_vec_merge_dispatcher::ptr merge_dispatcher(
			new EM_sort_detail::EM_vec_merge_dispatcher(in_vecs, out_vecs,
				sort_dispatcher->get_sort_summary().get_prio_queue()));
	// TODO let's not use asynchornous I/O for now.
	io_worker_task merge_worker(merge_dispatcher, 0);
	merge_worker.register_EM_obj(this);
	merge_worker.register_EM_obj(tmp.get());
	merge_worker.run();
	this->destroy_ios();
	tmp->destroy_ios();

	// In the end, we points to the new file.
	factory = tmp->factory;
	tmp->factory = NULL;
}

////////////////////////// Set data of the vector ////////////////////////////

namespace
{

/*
 * This class records the summary of the array. It stores three pieces of
 * information from each portion: the first and last elements in the portion
 * and a flag that indicates whether the portion is sorted.
 * This implementation assumes that two levels to test whether an array
 * is sorted. It is enough for a very large vector.
 * Although this data structure is shared by all threads, each portion owns
 * its own elements in the shared vector, so locking isn't needed.
 */
class issorted_summary
{
	// This stores the elements in the each end of a portion.
	local_buf_vec_store::ptr ends;
	// This vector is modified by multiple threads in parallel, so we can't
	// use a boolean vector, which isn't thread-safe. We should use
	// thread-safe bitmap.
	std::vector<int> issorted;
	size_t portion_size;
public:
	issorted_summary(const EM_vec_store &vec) {
		size_t num_portions = vec.get_num_portions();
		ends = local_vec_store::ptr(new local_buf_vec_store(-1,
					num_portions * 2, vec.get_type(), -1));
		issorted.resize(num_portions);
		portion_size = vec.get_portion_size();
	}

	void set_portion_result(local_buf_vec_store::const_ptr store) {
		bool sorted = store->get_type().get_sorter().is_sorted(
				store->get_raw_arr(), store->get_length(), false);
		off_t portion_idx = store->get_global_start() / portion_size;
		assert(portion_idx >= 0 && (size_t) portion_idx < issorted.size());
		issorted[portion_idx] = sorted;
		// Save the elements in each end.
		assert((size_t) portion_idx * 2 + 1 < ends->get_length());
		ends->set_raw(portion_idx * 2, store->get(0));
		ends->set_raw(portion_idx * 2 + 1, store->get(store->get_length() - 1));
	}

	bool is_sorted() const {
		for (size_t i = 0; i < issorted.size(); i++)
			if (!issorted[i])
				return false;
		bool ret = ends->get_type().get_sorter().is_sorted(
				ends->get_raw_arr(), ends->get_length(), false);
		return ret;
	}
};

class EM_vec_issorted_compute: public portion_compute
{
	local_buf_vec_store::const_ptr store;
	issorted_summary &summary;
public:
	EM_vec_issorted_compute(issorted_summary &_summary): summary(_summary) {
	}

	void set_buf(local_buf_vec_store::const_ptr store) {
		this->store = store;
	}

	virtual void run(char *buf, size_t size) {
		assert(store->get_raw_arr() == buf);
		assert(store->get_length() * store->get_entry_size() == size);
		summary.set_portion_result(store);
	}
};

}

class EM_vec_issorted_dispatcher: public EM_vec_dispatcher
{
	issorted_summary summary;
public:
	typedef std::shared_ptr<EM_vec_issorted_dispatcher> ptr;

	EM_vec_issorted_dispatcher(const EM_vec_store &store): EM_vec_dispatcher(
			store), summary(store) {
	}

	const issorted_summary &get_summary() const {
		return summary;
	}

	virtual void create_vec_task(off_t global_start,
			size_t length, const EM_vec_store &from_vec) {
		EM_vec_issorted_compute *compute = new EM_vec_issorted_compute(summary);
		local_vec_store::const_ptr portion = from_vec.get_portion_async(global_start,
				length, portion_compute::ptr(compute));
		compute->set_buf(portion);
	}
};

bool EM_vec_store::is_sorted() const
{
	mem_thread_pool::ptr threads = mem_thread_pool::get_global_mem_threads();
	EM_vec_issorted_dispatcher::ptr dispatcher(
			new EM_vec_issorted_dispatcher(*this));
	for (size_t i = 0; i < threads->get_num_threads(); i++) {
		io_worker_task *task = new io_worker_task(dispatcher);
		task->register_EM_obj(const_cast<EM_vec_store *>(this));
		threads->process_task(i % threads->get_num_nodes(), task);
	}
	threads->wait4complete();
	const_cast<EM_vec_store *>(this)->destroy_ios();
	return dispatcher->get_summary().is_sorted();
}

////////////////////////// Set data of the vector ////////////////////////////

namespace
{

class EM_vec_setdata_dispatcher: public EM_vec_dispatcher
{
	const set_vec_operate &op;
	EM_vec_store &to_vec;
public:
	EM_vec_setdata_dispatcher(EM_vec_store &store,
			const set_vec_operate &_op): EM_vec_dispatcher(store), op(
				_op), to_vec(store) {
	}

	virtual void create_vec_task(off_t global_start,
			size_t length, const EM_vec_store &from_vec) {
		local_buf_vec_store::ptr buf(new local_buf_vec_store(
					global_start, length, to_vec.get_type(), -1));
		buf->set_data(op);
		to_vec.write_portion(buf);
	}
};

}

void EM_vec_store::set_data(const set_vec_operate &op)
{
	mem_thread_pool::ptr threads = mem_thread_pool::get_global_mem_threads();
	EM_vec_setdata_dispatcher::ptr dispatcher(
			new EM_vec_setdata_dispatcher(*this, op));
	for (size_t i = 0; i < threads->get_num_threads(); i++) {
		io_worker_task *task = new io_worker_task(dispatcher);
		task->register_EM_obj(this);
		threads->process_task(i % threads->get_num_nodes(), task);
	}
	threads->wait4complete();
	destroy_ios();
}

matrix_store::const_ptr EM_vec_store::conv2mat(size_t nrow, size_t ncol,
			bool byrow) const
{
	BOOST_LOG_TRIVIAL(error)
		<< "can't convert a NUMA vector to a matrix";
	return matrix_store::ptr();
}

}

}
