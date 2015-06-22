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

#include <boost/format.hpp>

#include "NUMA_dense_matrix.h"
#include "mem_worker_thread.h"
#include "local_matrix_store.h"

namespace fm
{

namespace detail
{

NUMA_matrix_store::ptr NUMA_matrix_store::cast(matrix_store::ptr store)
{
	mem_matrix_store::ptr mem_store = mem_matrix_store::cast(store);
	if (mem_store == NULL)
		return NUMA_matrix_store::ptr();
	if (mem_store->get_num_nodes() < 0) {
		BOOST_LOG_TRIVIAL(error)
			<< "cast to NUMA matrix: the matrix isn't stored in NUMA memory";
		return NUMA_matrix_store::ptr();
	}
	return std::static_pointer_cast<NUMA_matrix_store>(store);
}

NUMA_matrix_store::const_ptr NUMA_matrix_store::cast(matrix_store::const_ptr store)
{
	mem_matrix_store::const_ptr mem_store = mem_matrix_store::cast(store);
	if (mem_store == NULL)
		return NUMA_matrix_store::const_ptr();
	if (mem_store->get_num_nodes() < 0) {
		BOOST_LOG_TRIVIAL(error)
			<< "cast to NUMA matrix: the matrix isn't stored in NUMA memory";
		return NUMA_matrix_store::const_ptr();
	}
	return std::static_pointer_cast<const NUMA_matrix_store>(store);
}

NUMA_matrix_store::ptr NUMA_matrix_store::create(size_t nrow, size_t ncol,
		int num_nodes, matrix_layout_t layout, const scalar_type &type)
{
	if (layout == matrix_layout_t::L_ROW) {
		if (nrow > ncol)
			return NUMA_row_tall_matrix_store::create(nrow, ncol, num_nodes,
					type);
		else
			return NUMA_row_wide_matrix_store::create(nrow, ncol, num_nodes,
					type);
	}
	else {
		if (nrow > ncol)
			return NUMA_col_tall_matrix_store::create(nrow, ncol, num_nodes,
					type);
		else
			return NUMA_col_wide_matrix_store::create(nrow, ncol, num_nodes,
					type);
	}
}

NUMA_row_tall_matrix_store::NUMA_row_tall_matrix_store(size_t nrow, size_t ncol,
		int num_nodes, const scalar_type &type): NUMA_row_matrix_store(nrow, ncol,
			type), mapper(num_nodes)
{
	data.resize(num_nodes);
	std::vector<size_t> local_lens = mapper.cal_local_lengths(nrow);
	for (int node_id = 0; node_id < num_nodes; node_id++)
		data[node_id] = detail::raw_data_array(
				local_lens[node_id] * ncol * get_entry_size(), node_id);
}

char *NUMA_row_tall_matrix_store::get_row(size_t row_idx)
{
	auto phy_loc = mapper.map2physical(row_idx);
	return data[phy_loc.first].get_raw()
		+ phy_loc.second * get_num_cols() * get_entry_size();
}

const char *NUMA_row_tall_matrix_store::get_row(size_t row_idx) const
{
	auto phy_loc = mapper.map2physical(row_idx);
	return data[phy_loc.first].get_raw()
		+ phy_loc.second * get_num_cols() * get_entry_size();
}

const char *NUMA_row_tall_matrix_store::get_rows(size_t row_start,
		size_t row_end) const
{
	if (mapper.get_logical_range_id(row_start)
			!= mapper.get_logical_range_id(row_end - 1)) {
		BOOST_LOG_TRIVIAL(error) << boost::format(
				"[%1%, %2%) isn't in the same range") % row_start % row_end;
		return NULL;
	}
	return get_row(row_start);
}

char *NUMA_row_tall_matrix_store::get_rows(size_t row_start, size_t row_end)
{
	if (mapper.get_logical_range_id(row_start)
			!= mapper.get_logical_range_id(row_end - 1)) {
		BOOST_LOG_TRIVIAL(error) << boost::format(
				"[%1%, %2%) isn't in the same range") % row_start % row_end;
		return NULL;
	}
	return (char *) get_row(row_start);
}

NUMA_col_tall_matrix_store::NUMA_col_tall_matrix_store(size_t nrow,
		size_t ncol, int num_nodes, const scalar_type &type): NUMA_col_matrix_store(
			nrow, ncol, type)
{
	data.resize(ncol);
	for (size_t i = 0; i < ncol; i++)
		data[i] = NUMA_vec_store::create(nrow, num_nodes, type);
}

matrix_store::const_ptr NUMA_row_tall_matrix_store::transpose() const
{
	return NUMA_col_wide_matrix_store::create_transpose(*this);
}

matrix_store::const_ptr NUMA_col_tall_matrix_store::transpose() const
{
	return NUMA_row_wide_matrix_store::create_transpose(*this);
}

matrix_store::const_ptr NUMA_row_wide_matrix_store::transpose() const
{
	return matrix_store::const_ptr(new NUMA_col_tall_matrix_store(store));
}

matrix_store::const_ptr NUMA_col_wide_matrix_store::transpose() const
{
	return matrix_store::const_ptr(new NUMA_row_tall_matrix_store(store));
}

local_matrix_store::const_ptr NUMA_row_tall_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows,
		size_t num_cols) const
{
	if (start_row + num_rows > get_num_rows())
		return local_matrix_store::const_ptr();
	// We have to retrieve the entire rows.
	if (num_cols != get_num_cols() || start_col != 0)
		return local_matrix_store::const_ptr();
	// The retrieved rows have to be stored contiguously.
	// range size has to be 2^n.
	size_t chunk_size = get_portion_size().first;
	if (ROUND(start_row, chunk_size)
			!= ROUND(start_row + num_rows - 1, chunk_size))
		return local_matrix_store::const_ptr();
	auto phy_loc = mapper.map2physical(start_row);
	return local_matrix_store::const_ptr(new local_cref_contig_row_matrix_store(
				get_row(start_row), start_row, start_col, num_rows, num_cols,
				get_type(), phy_loc.first));
}

local_matrix_store::ptr NUMA_row_tall_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows,
		size_t num_cols)
{
	if (start_row + num_rows > get_num_rows())
		return local_matrix_store::ptr();
	// We have to retrieve the entire rows.
	if (num_cols != get_num_cols() || start_col != 0)
		return local_matrix_store::ptr();
	// The retrieved rows have to be stored contiguously.
	// range size has to be 2^n.
	size_t chunk_size = get_portion_size().first;
	if (ROUND(start_row, chunk_size)
			!= ROUND(start_row + num_rows - 1, chunk_size))
		return local_matrix_store::ptr();
	auto phy_loc = mapper.map2physical(start_row);
	return local_matrix_store::ptr(new local_ref_contig_row_matrix_store(
				get_row(start_row), start_row, start_col, num_rows, num_cols,
				get_type(), phy_loc.first));
}

local_matrix_store::const_ptr NUMA_row_tall_matrix_store::get_portion(
		size_t id) const
{
	size_t chunk_size = get_portion_size().first;
	size_t start_row = id * chunk_size;
	size_t start_col = 0;
	size_t num_rows = std::min(get_num_rows() - start_row, chunk_size);
	size_t num_cols = get_num_cols();
	auto phy_loc = mapper.map2physical(start_row);
	return local_matrix_store::const_ptr(new local_cref_contig_row_matrix_store(
				get_row(start_row), start_row, start_col, num_rows, num_cols,
				get_type(), phy_loc.first));
}

local_matrix_store::ptr NUMA_row_tall_matrix_store::get_portion(size_t id)
{
	size_t chunk_size = get_portion_size().first;
	size_t start_row = id * chunk_size;
	size_t start_col = 0;
	size_t num_rows = std::min(get_num_rows() - start_row, chunk_size);
	size_t num_cols = get_num_cols();
	auto phy_loc = mapper.map2physical(start_row);
	return local_matrix_store::ptr(new local_ref_contig_row_matrix_store(
				get_row(start_row), start_row, start_col, num_rows, num_cols,
				get_type(), phy_loc.first));
}

local_matrix_store::const_ptr NUMA_col_tall_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows,
		size_t num_cols) const
{
	if (start_row + num_rows > get_num_rows())
		return local_matrix_store::const_ptr();
	// We have to retrieve the entire rows.
	if (num_cols != get_num_cols() || start_col != 0)
		return local_matrix_store::const_ptr();
	// The retrieved rows have to be stored contiguously.
	// range size has to be 2^n.
	size_t chunk_size = get_portion_size().first;
	if (ROUND(start_row, chunk_size)
			!= ROUND(start_row + num_rows - 1, chunk_size))
		return local_matrix_store::const_ptr();

	int node_id = data.front()->get_node_id(start_row);
	std::vector<const char *> cols(num_cols);
	for (size_t i = 0; i < num_cols; i++) {
		cols[i] = data[i + start_col]->get_sub_arr(start_row,
				start_row + num_rows);
		assert(node_id == data[i + start_col]->get_node_id(start_row));
	}
	return local_matrix_store::const_ptr(new local_cref_col_matrix_store(
				cols, start_row, start_col, num_rows, num_cols, get_type(),
				node_id));
}

local_matrix_store::ptr NUMA_col_tall_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows,
		size_t num_cols)
{
	if (start_row + num_rows > get_num_rows())
		return local_matrix_store::ptr();
	// We have to retrieve the entire rows.
	if (num_cols != get_num_cols() || start_col != 0)
		return local_matrix_store::ptr();
	// The retrieved rows have to be stored contiguously.
	// range size has to be 2^n.
	size_t chunk_size = get_portion_size().first;
	if (ROUND(start_row, chunk_size)
			!= ROUND(start_row + num_rows - 1, chunk_size))
		return local_matrix_store::ptr();

	int node_id = data.front()->get_node_id(start_row);
	std::vector<char *> cols(num_cols);
	for (size_t i = 0; i < num_cols; i++) {
		cols[i] = data[i + start_col]->get_sub_arr(start_row,
				start_row + num_rows);
		assert(node_id == data[i + start_col]->get_node_id(start_row));
	}
	return local_matrix_store::ptr(new local_ref_col_matrix_store(
				cols, start_row, start_col, num_rows, num_cols, get_type(),
				node_id));
}

local_matrix_store::const_ptr NUMA_col_tall_matrix_store::get_portion(
		size_t id) const
{
	assert(!data.empty());
	size_t chunk_size = get_portion_size().first;
	size_t start_row = id * chunk_size;
	size_t start_col = 0;
	size_t num_rows = std::min(get_num_rows() - start_row, chunk_size);
	size_t num_cols = get_num_cols();
	assert(!data.empty());
	int node_id = data.front()->get_node_id(start_row);
	std::vector<const char *> cols(num_cols);
	for (size_t i = 0; i < num_cols; i++) {
		cols[i] = data[i + start_col]->get_sub_arr(start_row,
				start_row + num_rows);
		assert(node_id == data[i + start_col]->get_node_id(start_row));
	}
	return local_matrix_store::const_ptr(new local_cref_col_matrix_store(
				cols, start_row, start_col, num_rows, num_cols, get_type(),
				node_id));
}

local_matrix_store::ptr NUMA_col_tall_matrix_store::get_portion(size_t id)
{
	assert(!data.empty());
	size_t chunk_size = get_portion_size().first;
	size_t start_row = id * chunk_size;
	size_t start_col = 0;
	size_t num_rows = std::min(get_num_rows() - start_row, chunk_size);
	size_t num_cols = get_num_cols();
	assert(!data.empty());
	int node_id = data.front()->get_node_id(start_row);
	std::vector<char *> cols(num_cols);
	for (size_t i = 0; i < num_cols; i++) {
		cols[i] = data[i + start_col]->get_sub_arr(start_row,
				start_row + num_rows);
		assert(node_id == data[i + start_col]->get_node_id(start_row));
	}
	return local_matrix_store::ptr(new local_ref_col_matrix_store(
				cols, start_row, start_col, num_rows, num_cols, get_type(),
				node_id));
}

local_matrix_store::const_ptr NUMA_row_wide_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows, size_t num_cols) const
{
	return local_matrix_store::cast(store.get_portion(start_col, start_row,
				num_cols, num_rows)->transpose());
}

local_matrix_store::ptr NUMA_row_wide_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows, size_t num_cols)
{
	return local_matrix_store::cast(store.get_portion(start_col, start_row,
				num_cols, num_rows)->transpose());
}

local_matrix_store::const_ptr NUMA_col_wide_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows, size_t num_cols) const
{
	return local_matrix_store::cast(store.get_portion(start_col, start_row,
				num_cols, num_rows)->transpose());
}

local_matrix_store::ptr NUMA_col_wide_matrix_store::get_portion(
		size_t start_row, size_t start_col, size_t num_rows, size_t num_cols)
{
	return local_matrix_store::cast(store.get_portion(start_col, start_row,
				num_cols, num_rows)->transpose());
}

local_matrix_store::const_ptr NUMA_row_wide_matrix_store::get_portion(
		size_t id) const
{
	return local_matrix_store::cast(store.get_portion(id)->transpose());
}

local_matrix_store::ptr NUMA_row_wide_matrix_store::get_portion(size_t id)
{
	return local_matrix_store::cast(store.get_portion(id)->transpose());
}

local_matrix_store::const_ptr NUMA_col_wide_matrix_store::get_portion(
		size_t id) const
{
	return local_matrix_store::cast(store.get_portion(id)->transpose());
}

local_matrix_store::ptr NUMA_col_wide_matrix_store::get_portion(size_t id)
{
	return local_matrix_store::cast(store.get_portion(id)->transpose());
}

matrix_store::const_ptr NUMA_col_tall_matrix_store::get_cols(
		const std::vector<off_t> &idxs) const
{
	std::vector<NUMA_vec_store::ptr> wanted(idxs.size());
	for (size_t i = 0; i < wanted.size(); i++)
		wanted[i] = data[idxs[i]];
	return matrix_store::const_ptr(new NUMA_col_tall_matrix_store(wanted));
}

matrix_store::const_ptr NUMA_col_tall_matrix_store::append_cols(
			const std::vector<matrix_store::const_ptr> &mats) const
{
	std::vector<NUMA_vec_store::ptr> data(this->data.begin(), this->data.end());
	for (size_t i = 0; i < mats.size(); i++) {
		if (mats[i]->get_num_rows() != get_num_rows()) {
			BOOST_LOG_TRIVIAL(error)
				<< "can't append columns with different length";
			return matrix_store::const_ptr();
		}
		if (mats[i]->get_type() != get_type()) {
			BOOST_LOG_TRIVIAL(error)
				<< "can't append columns with different type";
			return matrix_store::const_ptr();
		}

		if (!mats[i]->is_in_mem()) {
			BOOST_LOG_TRIVIAL(error)
				<< "The columns aren't in memory";
			return matrix_store::const_ptr();
		}
		const mem_matrix_store &mem_mat
			= static_cast<const mem_matrix_store &>(*mats[i]);
		if (mem_mat.get_num_nodes() <= 0) {
			BOOST_LOG_TRIVIAL(error)
				<< "The columns aren't in NUMA memory";
			return matrix_store::const_ptr();
		}

		const NUMA_col_tall_matrix_store &numa_mat
			= dynamic_cast<const NUMA_col_tall_matrix_store &>(mem_mat);
		data.insert(data.end(), numa_mat.data.begin(), numa_mat.data.end());
	}

	return matrix_store::const_ptr(new NUMA_col_tall_matrix_store(data));
}

bool NUMA_row_tall_matrix_store::write2file(const std::string &file_name) const
{
	FILE *f = fopen(file_name.c_str(), "w");
	if (f == NULL) {
		BOOST_LOG_TRIVIAL(error)
			<< boost::format("can't open %1%: %2%") % file_name % strerror(errno);
		return false;
	}
	if (!write_header(f))
		return false;

	size_t num_portions = get_num_portions();
	size_t tot_size = 0;
	for (size_t i = 0; i < num_portions; i++) {
		local_matrix_store::const_ptr portion = get_portion(i);
		const char *data = portion->get_raw_arr();
		assert(data);
		size_t data_size = portion->get_num_rows() * portion->get_num_cols(
				) * portion->get_entry_size();
		tot_size += data_size;
		size_t ret = fwrite(data, data_size, 1, f);
		if (ret == 0) {
			BOOST_LOG_TRIVIAL(error)
				<< boost::format("can't write to %1%: %2%")
				% file_name % strerror(errno);
			fclose(f);
			return false;
		}
	}
	printf("write %ld bytes\n", tot_size);
	fclose(f);
	return true;
}

static void copy_vec(const NUMA_vec_store &vec, char *buf)
{
	size_t portion_size = vec.get_portion_size();
	for (size_t idx = 0; idx < vec.get_length(); idx += portion_size) {
		size_t local_len = std::min(portion_size, vec.get_length() - idx);
		const char *portion = vec.get_sub_arr(idx, idx + local_len);
		assert(portion);
		memcpy(buf + idx * vec.get_entry_size(), portion,
				local_len * vec.get_entry_size());
	}
}

bool NUMA_col_tall_matrix_store::write2file(const std::string &file_name) const
{
	FILE *f = fopen(file_name.c_str(), "w");
	if (f == NULL) {
		BOOST_LOG_TRIVIAL(error)
			<< boost::format("can't open %1%: %2%") % file_name % strerror(errno);
		return false;
	}
	if (!write_header(f))
		return false;

	size_t tot_size = 0;
	for (size_t i = 0; i < get_num_cols(); i++) {
		NUMA_vec_store::const_ptr col = data[i];
		size_t col_size = col->get_length() * col->get_entry_size();
		tot_size += col_size;
		std::unique_ptr<char[]> tmp(new char[col_size]);
		copy_vec(*col, tmp.get());
		size_t ret = fwrite(tmp.get(), col_size, 1, f);
		if (ret == 0) {
			BOOST_LOG_TRIVIAL(error)
				<< boost::format("can't write to %1%: %2%")
				% file_name % strerror(errno);
			fclose(f);
			return false;
		}
	}
	printf("write %ld bytes\n", tot_size);
	fclose(f);
	return true;
}

}

}
