#pragma once

#include "column.hpp"
#include <immintrin.h>

typedef union {
	struct {
		uint64_t m_input_addr;
		uint64_t m_output_addr;
		uint64_t m_status_addr;
		uint32_t m_num_lines_input;
		int m_lower;
		int m_upper;
	} reg;
	uint32_t val[32];
} select_config_t;

typedef struct {
	uint32_t num_positives;
} selection_result_t;

class selection {
private:
	column<int>* m_input;
	column<uint32_t>* m_output;

	int m_lower;
	int m_upper;
public:
	unsigned m_id;
	selection_result_t m_result;

	selection(
			unsigned id,
			column<int>* input,
			int lower,
			int upper)
	{
		m_id = id;
		m_input = input;
		m_lower = lower;
		m_upper = upper;
		m_output = new column<uint32_t>(m_input->get_num_items());
	}

	~selection() {
		delete m_output;
	}

	void execute_selection_sw(unsigned num_reps) {
		uint32_t offset = m_input->m_offset[m_id];

		int* in_base = m_input->get_base();
		uint32_t* out_base = m_output->get_base();

		for (uint32_t i = 0; i < num_reps; i++) {
			m_result.num_positives = 0;
			for (uint32_t i = 0; i < m_input->m_num_items_partition[m_id]; i++) {
				int value = in_base[offset + i];
				if (value > m_lower && value < m_upper) {
					out_base[m_result.num_positives++] = i;
				}
			}
		}
	}

	uint32_t append_results(column<uint32_t>* output, uint32_t offset) {
		uint32_t count = 0;
		for (unsigned i = 0; i < m_result.num_positives; i++) {
			if (m_output->get_item(i) != 0xFFFFFFFF) {
				output->append(m_output->get_item(i) + offset);
				count++;
			}
		}
		return count;
	}
};