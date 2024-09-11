#include <iostream>
#include <chrono>
#include <vector>
#include <numa.h>
#include <cmath>
#include <numeric>

#include "tsc_timer.hpp"

#define NODE 1

tsc_timer pt = tsc_timer(NODE);

int main() {
    numa_run_on_node(NODE);
    std::vector<int64_t> start_time;
    std::vector<int64_t> end_time;

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count() < 10) {
        start_time.push_back(pt.current_cpu());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        end_time.push_back(pt.current_cpu());
    }

    pt.ns_duration_vector(start_time, end_time);
    
    try {
        auto [mean, std_dev] = mean_and_std(start_time);
        std::cout << "Mean: " << mean << std::endl;
        std::cout << "Standard Deviation: " << std_dev << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}