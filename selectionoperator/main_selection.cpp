#include "selection.hpp"

#define DO_VERIFY

int main(int argc, char *argv[]) {

    unsigned num_values = 1024;
    int lower = 0;
    int upper = 0;
    unsigned num_reps = 10;
    unsigned num_engines = 1;
    bool print = false;
    if (argc != 7) {
        cout << "Usage: ./testbench <num_values> <lower> <upper> <num_reps> <num_engines> <print>" << endl;
        return 1;
    }
    num_values = atoi(argv[1]);
    lower = atoi(argv[2]);
    upper = atoi(argv[3]);
    num_reps = atoi(argv[4]);
    num_engines = atoi(argv[5]);
    print = atoi(argv[6]) == 1;
    cout << "num_values: " << num_values << endl;
    cout << "lower: " << lower << endl;
    cout << "upper: " << upper << endl;
    cout << "num_reps: " << num_reps << endl;
    cout << "num_engines: " << num_engines << endl;

    srand(3);

    // Input
    column<int> in_column(num_values);
    in_column.populate_int_column(num_values, 'u', '-');
    in_column.set_partitions(num_engines);

    vector<selection*> selection_insts;
    for (unsigned e = 0; e < num_engines; e++) {
        selection* temp = new selection(e, &in_column, lower, upper);
        selection_insts.push_back(temp);
    }

    vector<thread*> sw_threads;
    double start = get_time();
    for (selection* s: selection_insts) {
        thread* temp_thread = new thread(&selection::execute_selection_sw, s, num_reps);
        sw_threads.push_back(temp_thread);
    }

    for (thread* t: sw_threads) {
        t->join();
    }
    double end = get_time();

    uint32_t sw_num_matches = 0;
    column<uint32_t> sw_out_column(num_values);
    for (selection* s: selection_insts) {
        sw_num_matches += s->append_results(&sw_out_column, sw_num_matches);
    }

    if (print) {
        sw_out_column.print();
    }

    cout << "-----------> sw_num_matches: " << sw_num_matches << endl;
    cout << "total SW time: " << end-start << endl;


    return 0;
}