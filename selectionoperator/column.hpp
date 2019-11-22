#pragma once

#include <stdint.h>
#include <algorithm>
#include <iostream>
#include <vector>
#include <thread>
#include <sys/time.h>

using namespace std;

#define ALIGNMENT 64

static double get_time(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec*1e-6;
}

typedef struct {
	uint32_t m_id;
	int m_value;
} tuple_t;

template <typename T>
class column {
private:

    // CPU side
    T* m_base;
    uint32_t m_capacity_items;
    uint32_t m_num_items;

public:
    uint32_t m_num_partitions;
    vector<uint32_t> m_offset;
    vector<uint32_t> m_num_items_partition;


    column(uint32_t capacity_items) {
        m_num_items = 0;
        m_base = NULL;
        column_realloc(capacity_items);
        set_partitions(1);
    }

    ~column() {
        free(m_base);
    }

    void set_partitions(uint32_t num_partitions) {
        m_num_partitions = num_partitions;
        m_offset.resize(m_num_partitions);
        m_num_items_partition.resize(m_num_partitions);
        uint32_t assigned_num_items = 0;
        for (uint32_t i = 0; i < m_num_partitions; i++) {
            m_offset[i] = assigned_num_items;
            if (i == m_num_partitions-1) {
                m_num_items_partition[i] = m_num_items - assigned_num_items;
            }
            else {
                m_num_items_partition[i] = m_num_items/m_num_partitions;
            }
#ifdef COLUMN_VERBOSE
            if (m_num_partitions > 1) {
                cout << "column, set_partitions: 0x" << hex << m_offset[i] << dec << ", m_num_items_partition[" << i << "]: " << m_num_items_partition[i] << endl;
            }
#endif
            assigned_num_items += m_num_items_partition[i];
        }
    }

    void column_realloc(uint32_t new_capacity) {
        T* new_base;
        m_capacity_items = new_capacity;

        posix_memalign((void**)&new_base, ALIGNMENT, m_capacity_items*sizeof(T));
        if (m_capacity_items < m_num_items) {
            m_num_items = m_capacity_items;
        }
        for (uint32_t i = 0; i < m_num_items; i++) {
            new_base[i] = m_base[i];
        }
        free(m_base);
        m_base = new_base;
    }

    uint32_t get_num_items() {
        return m_num_items;
    }

    T* get_base() {
        return m_base;
    }

    T get_item(uint32_t index) {
        if (index < m_capacity_items) {
            return m_base[index];
        }
        else {
            return (T)0;
        }
    }

    void set_item(T value, uint32_t index) {
        if (index < m_capacity_items) {
            m_base[index] = value;
        }
    }

    void sort_items() {
        sort((uint32_t*)m_base, ((uint32_t*)m_base) + m_num_items);
    }

    void populate_int_column(uint32_t num_items, char unique, char shuffle) {
        if (num_items > m_capacity_items) {
            column_realloc(num_items);
        }

        for (uint32_t i = 0; i < num_items; i++) {
            m_base[i] = (unique == 'u') ? i : rand()%num_items;
        }
        if (shuffle == 's') {
            for (uint32_t i = 0; i < num_items; i++) {
                uint32_t index = rand()%num_items;
                uint32_t temp = m_base[i];
                m_base[i] = m_base[index];
                m_base[index] = temp;
            }
        }
        m_num_items = num_items;
    }

    void append(T value) {
        if (m_num_items >= m_capacity_items) {
            column_realloc(2*m_capacity_items);
        }
        m_base[m_num_items++] = value;
    }

    void print() {
        for (uint32_t i = 0; i < m_num_items; i++) {
            cout << i << ": " << m_base[i] << endl;
        }
    }
};