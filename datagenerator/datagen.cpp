#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <string>
#include <limits>

using namespace std;

struct tuple_t
{
	uint32_t m_key;
	uint32_t m_value;
};

#define RAND_RANGE(N) ((float)rand() / ((float)RAND_MAX + 1) * (N))

void generate_key_relation (
	tuple_t* data,
	uint32_t num_tuples,
	uint32_t distribution,
	uint32_t key_offset,
	uint32_t value_offset)
{

	for(uint32_t i = 0; i < num_tuples; i++)
	{
		if (distribution == 0) {
			data[i].m_key = i + key_offset;
		}
		else if (distribution == 1) {
			data[i].m_key = RAND_RANGE(num_tuples);
		}
		data[i].m_value = value_offset - i;
	}
	for (uint32_t i = num_tuples - 1; i > 0; i--) // Shuffle
	{
		uint32_t j = RAND_RANGE(i);
		tuple_t temp = data[i];
		data[i] = data[j];
		data[j] = temp;
	}
}

int main (int argc, char** argv) {

	uint32_t num_tuples = 1024;
	// 0: linear, 1 random
	uint32_t distribution = 0;
	uint32_t key_offset = 0;
	uint32_t value_seed = 12;

	if (argc != 5) {
		cout << "usage: ./datagen <num_tuples> <distribution> <key_offset> <value_seed>" << endl;
		return 1;
	}
	num_tuples = atoi(argv[1]);
	distribution = atoi(argv[2]);
	key_offset = atoi(argv[3]);
	value_seed = atoi(argv[4]);

	srand(value_seed);

	tuple_t* data = (tuple_t*)malloc(num_tuples*sizeof(tuple_t));

	uint32_t value_offset = 0xFFFFFFFF >> value_seed;
	generate_key_relation(data, num_tuples, distribution, key_offset, value_offset);

	string filename;
	if (distribution == 0) {
		filename = "linear_";
	}
	else if (distribution == 1) {
		filename = "random_";
	}

	filename = filename + to_string(num_tuples) + \
					"_" + to_string(key_offset) + \
					"_" + to_string(value_seed) + \
					".tbl";

	FILE* f;
	f = fopen(filename.c_str(), "w");
	for (uint32_t i = 0; i < num_tuples; i++) {
		fprintf(f, "%d,%d\n", data[i].m_key, data[i].m_value);
	}
	fclose(f);

	return 0;
}