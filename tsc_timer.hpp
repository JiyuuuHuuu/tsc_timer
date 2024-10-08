/**
 * wrapper for tscn (https://github.com/MengRao/tscns) 
 */

#ifndef TSC_TIMER_
#define TSC_TIMER_

#include <thread>
#include <numa.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <functional>
#include <chrono>

#include <stdexcept>

#define PT_CALIBRATE_PERIOD_MS 1000

/***************************************************************************/
/*
MIT License

Copyright (c) 2022 Meng Rao <raomeng1@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifdef _MSC_VER
#include <intrin.h>
#endif

class TSCNS
{
public:
    static const int64_t NsPerSec = 1000000000;

    void init(int64_t init_calibrate_ns = 20000000, int64_t calibrate_interval_ns = 3 * NsPerSec) {
        calibate_interval_ns_ = calibrate_interval_ns;
        int64_t base_tsc, base_ns;
        syncTime(base_tsc, base_ns);
        int64_t expire_ns = base_ns + init_calibrate_ns;
        while (rdsysns() < expire_ns) std::this_thread::yield();
        int64_t delayed_tsc, delayed_ns;
        syncTime(delayed_tsc, delayed_ns);
        double init_ns_per_tsc = (double)(delayed_ns - base_ns) / (double)(delayed_tsc - base_tsc);
        saveParam(base_tsc, base_ns, base_ns, init_ns_per_tsc);
    }

    void calibrate() {
        if (rdtsc() < next_calibrate_tsc_) return;
        int64_t tsc, ns;
        syncTime(tsc, ns);
        int64_t calulated_ns = tsc2ns(tsc);
        int64_t ns_err = calulated_ns - ns;
        int64_t expected_err_at_next_calibration =
            ns_err + (ns_err - base_ns_err_) * calibate_interval_ns_ / (ns - base_ns_ + base_ns_err_);
        double new_ns_per_tsc =
            ns_per_tsc_ * (1.0 - (double)expected_err_at_next_calibration / (double)calibate_interval_ns_);
        saveParam(tsc, calulated_ns, ns, new_ns_per_tsc);
    }

    static inline int64_t rdtsc() {
#ifdef _MSC_VER
        return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__) || defined(__amd64__)
        return (int64_t)__builtin_ia32_rdtsc();
#else
        return rdsysns();
#endif
    }

    inline int64_t tsc2ns(int64_t tsc) const {
        while (true) {
            uint32_t before_seq = param_seq_.load(std::memory_order_acquire) & static_cast<uint32_t>(~1);
            std::atomic_signal_fence(std::memory_order_acq_rel);
            int64_t ns = base_ns_ + (int64_t)((double)(tsc - base_tsc_) * ns_per_tsc_);
            std::atomic_signal_fence(std::memory_order_acq_rel);
            uint32_t after_seq = param_seq_.load(std::memory_order_acquire);
            if (before_seq == after_seq) return ns;
        }
    }

    inline int64_t rdns() const { return tsc2ns(rdtsc()); }

    static inline int64_t rdsysns() {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    }

    double getTscGhz() const { return 1.0 / ns_per_tsc_; }

    // Linux kernel sync time by finding the first trial with tsc diff < 50000
    // We try several times and return the one with the mininum tsc diff.
    // Note that MSVC has a 100ns resolution clock, so we need to combine those ns with the same
    // value, and drop the first and the last value as they may not scan a full 100ns range
    static void syncTime(int64_t& tsc_out, int64_t& ns_out) {
#ifdef _MSC_VER
        const int N = 15;
#else
        const int N = 3;
#endif
        int64_t tsc[N + 1];
        int64_t ns[N + 1];

        tsc[0] = rdtsc();
        for (int i = 1; i <= N; i++) {
            ns[i] = rdsysns();
            tsc[i] = rdtsc();
        }

#ifdef _MSC_VER
        int j = 1;
        for (int i = 2; i <= N; i++) {
            if (ns[i] == ns[i - 1]) continue;
            tsc[j - 1] = tsc[i - 1];
            ns[j++] = ns[i];
        }
        j--;
#else
        int j = N + 1;
#endif

        int best = 1;
        for (int i = 2; i < j; i++) {
            if (tsc[i] - tsc[i - 1] < tsc[best] - tsc[best - 1]) best = i;
        }
        tsc_out = (tsc[best] + tsc[best - 1]) >> 1;
        ns_out = ns[best];
    }

    void saveParam(int64_t base_tsc, int64_t base_ns, int64_t sys_ns, double new_ns_per_tsc) {
        base_ns_err_ = base_ns - sys_ns;
        next_calibrate_tsc_ = base_tsc + (int64_t)((double)(calibate_interval_ns_ - 1000) / new_ns_per_tsc);
        uint32_t seq = param_seq_.load(std::memory_order_relaxed);
        param_seq_.store(++seq, std::memory_order_release);
        std::atomic_signal_fence(std::memory_order_acq_rel);
        base_tsc_ = base_tsc;
        base_ns_ = base_ns;
        ns_per_tsc_ = new_ns_per_tsc;
        std::atomic_signal_fence(std::memory_order_acq_rel);
        param_seq_.store(++seq, std::memory_order_release);
    }

    alignas(64) std::atomic<uint32_t> param_seq_ = 0;
    double ns_per_tsc_;
    int64_t base_tsc_;
    int64_t base_ns_;
    int64_t calibate_interval_ns_;
    int64_t base_ns_err_;
    int64_t next_calibrate_tsc_;
};
/***************************************************************************/


class tsc_timer {
public:
    tsc_timer(int node = 0, bool bg_calibrate = true) : 
            stop_flag_(false), target_node_(node), bg_calibrate_(bg_calibrate) {
        tscns_.init();
        if (bg_calibrate_) {
          worker_thread_ = std::thread(&tsc_timer::calibrate_func_, this);
        }
    };

    ~tsc_timer() {
        if (bg_calibrate_) {
            stop_flag_.store(true);
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
    };

    int64_t current_ns() { return tscns_.rdns(); }
    int64_t current_cpu() { return tscns_.rdtsc(); }
    int64_t cpu_to_ns(int64_t tsc) { return tscns_.tsc2ns(tsc); }

    void cpu_to_ns_vector(std::vector<int64_t>& tsc) {
        for (auto& t : tsc) t = cpu_to_ns(t);
    }

    void ns_duration_vector(std::vector<int64_t>& tsc_start, std::vector<int64_t>& tsc_end) {
        if (tsc_start.size() != tsc_end.size()) {
            throw std::runtime_error("Vectors tsc_start and tsc_end must have the same size");
        }

        cpu_to_ns_vector(tsc_start);
        cpu_to_ns_vector(tsc_end);

        for (size_t i = 0; i < tsc_start.size(); ++i) {
            tsc_start[i] = tsc_end[i] - tsc_start[i];
        }
    }

private:
    void calibrate_func_() {
        numa_run_on_node(target_node_);
        while (!stop_flag_.load()) {
            tscns_.calibrate();
            std::this_thread::sleep_for(std::chrono::milliseconds(PT_CALIBRATE_PERIOD_MS));
        }
    }

    TSCNS tscns_;
    std::thread worker_thread_;
    std::atomic<bool> stop_flag_;
    int target_node_;
    bool bg_calibrate_;
};

static inline std::pair<double, double> mean_and_std(const std::vector<int64_t>& data) {
    if (data.empty()) {
        throw std::runtime_error("The input vector is empty.");
    }

    double mean = std::accumulate(data.begin(), data.end(), 0.0) / static_cast<double>(data.size());

    double variance = 0.0;
    for (const auto& value : data) {
        variance += (static_cast<double>(value) - mean) * (static_cast<double>(value) - mean);
    }
    variance /= static_cast<double>(data.size());
    double std_dev = std::sqrt(variance);

    return {mean, std_dev}; 
}

#endif