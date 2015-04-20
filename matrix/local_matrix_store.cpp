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

#include "local_matrix_store.h"
#include "bulk_operate.h"

namespace fm
{

namespace detail
{

void local_row_matrix_store::reset_data()
{
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
	// If the store has data stored contiguously.
	if (get_raw_arr())
		memset(get_raw_arr(), 0, nrow * ncol * get_entry_size());
	else {
		for (size_t i = 0; i < nrow; i++)
			memset(get_row(i), 0, ncol * get_entry_size());
	}
}

void local_row_matrix_store::set_data(const set_operate &op)
{
	assert(op.get_type() == get_type());
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
	for (size_t i = 0; i < nrow; i++)
		op.set(get_row(i), ncol, get_global_start_row() + i,
				get_global_start_col());
}

void local_col_matrix_store::reset_data()
{
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
	// If the store has data stored contiguously.
	if (get_raw_arr())
		memset(get_raw_arr(), 0, nrow * ncol * get_entry_size());
	else {
		for (size_t i = 0; i < ncol; i++)
			memset(get_col(i), 0, nrow * get_entry_size());
	}
}

void local_col_matrix_store::set_data(const set_operate &op)
{
	assert(op.get_type() == get_type());
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
	for (size_t i = 0; i < ncol; i++)
		op.set(get_col(i), nrow, get_global_start_row(),
				get_global_start_col() + i);
}

namespace
{

const size_t SUB_CHUNK_SIZE = 1024;

/*
 * This class contains the information of a submatrix in the original matrix.
 * This is used to improve CPU cache hits.
 */
class sub_matrix_info
{
	size_t start_row;
	size_t start_col;
	size_t nrow;
	size_t ncol;
public:
	sub_matrix_info(size_t start_row, size_t nrow, size_t start_col,
			size_t ncol) {
		this->start_row = start_row;
		this->start_col = start_col;
		this->nrow = nrow;
		this->ncol = ncol;
	}

	size_t get_num_rows() const {
		return nrow;
	}

	size_t get_num_cols() const {
		return ncol;
	}

	size_t get_start_row() const {
		return start_row;
	}

	size_t get_start_col() const {
		return start_col;
	}
};

/*
 * This class contains the information of a submatrix in the a column-wise matrix.
 * It is mainly used in inner product.
 */
class sub_col_matrix_info: public sub_matrix_info
{
	const local_col_matrix_store &m;
public:
	sub_col_matrix_info(size_t start_row, size_t nrow, size_t start_col,
			size_t ncol, const local_col_matrix_store &_m): sub_matrix_info(
				start_row, nrow, start_col, ncol), m(_m) {
		assert(start_row + nrow <= m.get_num_rows());
		assert(start_col + ncol <= m.get_num_cols());
	}

	const char *get_col(size_t col) const {
		return m.get_col(get_start_col() + col)
			+ get_start_row() * m.get_entry_size();
	}
};

/*
 * This class contains the information of a submatrix in the a row-wise matrix.
 * It is mainly used in inner product.
 */
class sub_row_matrix_info: public sub_matrix_info
{
	const local_row_matrix_store &m;
public:
	sub_row_matrix_info(size_t start_row, size_t nrow, size_t start_col,
			size_t ncol, const local_row_matrix_store &_m): sub_matrix_info(
				start_row, nrow, start_col, ncol), m(_m) {
		assert(start_row + nrow <= m.get_num_rows());
		assert(start_col + ncol <= m.get_num_cols());
	}

	const char *get_row(size_t row) const {
		return m.get_row(get_start_row() + row) + get_start_col() * m.get_entry_size();
	}
};

/*
 * In this case, the left matrix is row-major and wide. The right matrix is tall and its data
 * is stored column-wise.
 */
void inner_prod_row_wide(const local_row_matrix_store &m1,
		const local_col_matrix_store &m2, const bulk_operate &left_op,
		const bulk_operate &right_op, local_row_matrix_store &res)
{
	size_t ncol = m1.get_num_cols();
	size_t nrow = m1.get_num_rows();
	char *tmp_res = (char *) malloc(SUB_CHUNK_SIZE * left_op.output_entry_size());
	char *tmp_res2 = (char *) malloc(res.get_num_cols() * res.get_entry_size());
	for (size_t k = 0; k < ncol; k += SUB_CHUNK_SIZE) {
		size_t sub_ncol = std::min(SUB_CHUNK_SIZE, ncol - k);
		sub_row_matrix_info sub_left(0, nrow, k, sub_ncol, m1);
		sub_col_matrix_info sub_right(k, sub_ncol, 0, m2.get_num_cols(), m2);
		for (size_t i = 0; i < sub_left.get_num_rows(); i++) {
			for (size_t j = 0; j < sub_right.get_num_cols(); j++) {
				left_op.runAA(sub_ncol, sub_left.get_row(i),
						sub_right.get_col(j), tmp_res);
				right_op.runA(sub_ncol, tmp_res,
						tmp_res2 + res.get_entry_size() * j);
			}
			// This is fine because we assume the input type of the right operator
			// should be the same as the type of the output matrix.
			right_op.runAA(sub_right.get_num_cols(), tmp_res2, res.get_row(i),
					res.get_row(i));
		}
	}
	free(tmp_res);
	free(tmp_res2);
}

/*
 * In this case, the left matrix is tall and is stored in row major. I assume
 * the right matrix is small and is stored in column major. We don't need to consider
 * the case that the right matrix is wide because the product would
 * be too large to be stored in any storage media.
 */
void inner_prod_row_tall(const local_row_matrix_store &m1,
		const local_col_matrix_store &m2, const bulk_operate &left_op,
		const bulk_operate &right_op, local_row_matrix_store &res)
{
	size_t ncol = m1.get_num_cols();
	size_t nrow = m1.get_num_rows();
	char *tmp_res = (char *) malloc(ncol * res.get_entry_size());
	for (size_t i = 0; i < nrow; i++) {
		for (size_t j = 0; j < m2.get_num_cols(); j++) {
			left_op.runAA(ncol, m1.get_row(i), m2.get_col(j), tmp_res);
			right_op.runA(ncol, tmp_res, res.get(i, j));
		}
	}
	free(tmp_res);
}

/*
 * In this case, the left matrix is tall and stored in column major. I assume
 * the right matrix is small and don't care its format.
 */
void inner_prod_col_tall(const local_col_matrix_store &m1,
		const local_matrix_store &m2, const bulk_operate &left_op,
		const bulk_operate &right_op, local_col_matrix_store &res)
{
	size_t ncol = m1.get_num_cols();
	size_t nrow = m1.get_num_rows();
	char *tmp_res = (char *) malloc(SUB_CHUNK_SIZE * res.get_entry_size());
	// We further break the local matrix into small matrices to increase
	// CPU cache hits.
	for (size_t k = 0; k < nrow; k += SUB_CHUNK_SIZE) {
		sub_col_matrix_info subm(k, std::min(SUB_CHUNK_SIZE, nrow - k),
				0, ncol, m1);
		for (size_t i = 0; i < ncol; i++) {
			for (size_t j = 0; j < m2.get_num_cols(); j++) {
				left_op.runAE(subm.get_num_rows(), subm.get_col(i),
						m2.get(i, j), tmp_res);
				char *store_col = res.get_col(j) + k * res.get_entry_size();
				right_op.runAA(subm.get_num_rows(), tmp_res, store_col,
						store_col);
			}
		}
	}
	free(tmp_res);
}

}

void aggregate(const local_matrix_store &store, const bulk_operate &op, char *res)
{
	size_t output_size = op.output_entry_size();
	size_t ncol = store.get_num_cols();
	size_t nrow = store.get_num_rows();
	// If the store has data stored contiguously.
	if (store.get_raw_arr())
		op.runA(ncol * nrow, store.get_raw_arr(), res);
	// For row-major matrix.
	else if (store.store_layout() == matrix_layout_t::L_ROW) {
		const local_row_matrix_store &row_store = (const local_row_matrix_store &) store;
		std::unique_ptr<char []> raw_arr(new char[output_size * nrow]);
		for (size_t i = 0; i < nrow; i++)
			op.runA(ncol, row_store.get_row(i), raw_arr.get() + output_size * i);
		op.runA(nrow, raw_arr.get(), res);
	}
	else {
		assert(store.store_layout() == matrix_layout_t::L_COL);
		const local_col_matrix_store &col_store = (const local_col_matrix_store &) store;
		std::unique_ptr<char []> raw_arr(new char[output_size * ncol]);
		for (size_t i = 0; i < ncol; i++)
			op.runA(nrow, col_store.get_col(i), raw_arr.get() + output_size * i);
		op.runA(ncol, raw_arr.get(), res);
	}
}

void mapply2(const local_matrix_store &m1, const local_matrix_store &m2,
			const bulk_operate &op, local_matrix_store &res)
{
	assert(m1.store_layout() == m2.store_layout()
			&& m1.store_layout() == res.store_layout());
	size_t ncol = m1.get_num_cols();
	size_t nrow = m1.get_num_rows();
	// If the store has data stored contiguously.
	if (m1.get_raw_arr() && m2.get_raw_arr() && res.get_raw_arr())
		op.runAA(ncol * nrow, m1.get_raw_arr(), m2.get_raw_arr(),
				res.get_raw_arr());
	else if (m1.store_layout() == matrix_layout_t::L_ROW) {
		const local_row_matrix_store &row_m1 = (const local_row_matrix_store &) m1;
		const local_row_matrix_store &row_m2 = (const local_row_matrix_store &) m2;
		local_row_matrix_store &row_res = (local_row_matrix_store &) res;
		for (size_t i = 0; i < nrow; i++)
			op.runAA(ncol, row_m1.get_row(i), row_m2.get_row(i),
					row_res.get_row(i));
	}
	else {
		assert(m1.store_layout() == matrix_layout_t::L_COL);
		const local_col_matrix_store &col_m1 = (const local_col_matrix_store &) m1;
		const local_col_matrix_store &col_m2 = (const local_col_matrix_store &) m2;
		local_col_matrix_store &col_res = (local_col_matrix_store &) res;
		for (size_t i = 0; i < ncol; i++)
			op.runAA(nrow, col_m1.get_col(i), col_m2.get_col(i),
					col_res.get_col(i));
	}
}

void sapply(const local_matrix_store &store, const bulk_uoperate &op,
		local_matrix_store &res)
{
	assert(res.store_layout() == store.store_layout());
	size_t ncol = store.get_num_cols();
	size_t nrow = store.get_num_rows();
	// If the store has data stored contiguously.
	if (store.get_raw_arr() && res.get_raw_arr())
		op.runA(ncol * nrow, store.get_raw_arr(), res.get_raw_arr());
	else if (store.store_layout() == matrix_layout_t::L_ROW) {
		const local_row_matrix_store &row_store
			= (const local_row_matrix_store &) store;
		local_row_matrix_store &row_res = (local_row_matrix_store &) res;
		for (size_t i = 0; i < nrow; i++)
			op.runA(ncol, row_store.get_row(i), row_res.get_row(i));
	}
	else {
		assert(store.store_layout() == matrix_layout_t::L_COL);
		const local_col_matrix_store &col_store
			= (const local_col_matrix_store &) store;
		local_col_matrix_store &col_res = (local_col_matrix_store &) res;
		for (size_t i = 0; i < ncol; i++)
			op.runA(nrow, col_store.get_col(i), col_res.get_col(i));
	}
}

void inner_prod(const local_matrix_store &m1, const local_matrix_store &m2,
		const bulk_operate &left_op, const bulk_operate &right_op,
		local_matrix_store &res)
{
	if (m1.store_layout() == matrix_layout_t::L_ROW) {
		assert(m2.store_layout() == matrix_layout_t::L_COL);
		assert(res.store_layout() == matrix_layout_t::L_ROW);
		if (m1.is_wide())
			inner_prod_row_wide((const local_row_matrix_store &) m1,
					(const local_col_matrix_store &) m2, left_op, right_op,
					(local_row_matrix_store &) res);
		else
			inner_prod_row_tall((const local_row_matrix_store &) m1,
					(const local_col_matrix_store &) m2, left_op, right_op,
					(local_row_matrix_store &) res);
	}
	else {
		assert(!m1.is_wide());
		assert(res.store_layout() == matrix_layout_t::L_COL);
		inner_prod_col_tall((const local_col_matrix_store &) m1, m2, left_op, right_op,
				(local_col_matrix_store &) res);
	}
}

}

}
