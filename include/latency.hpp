#pragma once

#include <cstdint>
#include <ctime>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>


inline uint64_t now_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}


class LatencyHistogram {
public:
    void reserve(std::size_t n) { samples_.reserve(n); }

    void record(uint64_t latency_ns) noexcept {
        samples_.push_back(latency_ns);
    }

    void sort() {
        std::sort(samples_.begin(), samples_.end());
        sorted_ = true;
    }

    [[nodiscard]] uint64_t percentile(double p) const {
        if (!sorted_) throw std::logic_error("call sort() first");
        if (samples_.empty()) return 0;
        const std::size_t idx =
            static_cast<std::size_t>(std::ceil(p / 100.0 * samples_.size())) - 1;
        return samples_[std::min(idx, samples_.size() - 1)];
    }

    [[nodiscard]] double mean() const {
        if (samples_.empty()) return 0.0;
        const double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
        return sum / static_cast<double>(samples_.size());
    }

    [[nodiscard]] uint64_t min_ns() const {
        if (samples_.empty()) return 0;
        return *std::min_element(samples_.begin(), samples_.end());
    }

    [[nodiscard]] uint64_t max_ns() const {
        if (samples_.empty()) return 0;
        return *std::max_element(samples_.begin(), samples_.end());
    }

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }

    // Print an ASCII histogram with `buckets` equal-width bins
    [[nodiscard]] std::string ascii_histogram(std::size_t buckets = 20) const {
        if (samples_.empty()) return "(no samples)\n";
        const uint64_t lo = samples_.front();  // assumes sorted
        const uint64_t hi = samples_.back();
        if (lo == hi) return "(all samples identical)\n";

        const double width = static_cast<double>(hi - lo) / buckets;
        std::vector<std::size_t> counts(buckets, 0);
        for (uint64_t s : samples_) {
            std::size_t b = static_cast<std::size_t>((s - lo) / width);
            if (b >= buckets) b = buckets - 1;
            ++counts[b];
        }
        const std::size_t maxc = *std::max_element(counts.begin(), counts.end());
        const std::size_t bar_width = 50;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0);
        for (std::size_t i = 0; i < buckets; ++i) {
            const uint64_t bin_lo = lo + static_cast<uint64_t>(i * width);
            const uint64_t bin_hi = lo + static_cast<uint64_t>((i + 1) * width);
            const std::size_t bar = maxc ? (counts[i] * bar_width / maxc) : 0;
            oss << std::setw(7) << bin_lo << "-" << std::setw(7) << bin_hi
                << " ns | " << std::string(bar, '#') << " (" << counts[i] << ")\n";
        }
        return oss.str();
    }

private:
    std::vector<uint64_t> samples_;
    bool sorted_ = false;
};
