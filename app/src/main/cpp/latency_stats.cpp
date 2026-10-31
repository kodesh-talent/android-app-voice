#include "latency_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace vcore {

int64_t nowMicros() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

int64_t nowMillis() {
    return nowMicros() / 1000;
}

void Rollup::add(double valueMs) {
    samples_.push_back(valueMs);
}

void Rollup::clear() {
    samples_.clear();
}

double Rollup::last() const {
    return samples_.empty() ? 0.0 : samples_.back();
}

double Rollup::average() const {
    if (samples_.empty()) return 0.0;
    double sum = 0.0;
    for (double value : samples_) sum += value;
    return sum / static_cast<double>(samples_.size());
}

double Rollup::minimum() const {
    if (samples_.empty()) return 0.0;
    return *std::min_element(samples_.begin(), samples_.end());
}

double Rollup::maximum() const {
    if (samples_.empty()) return 0.0;
    return *std::max_element(samples_.begin(), samples_.end());
}

double Rollup::percentile(double percentile) const {
    if (samples_.empty()) return 0.0;
    const double clamped = std::min(1.0, std::max(0.0, percentile));
    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const double position = clamped * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

double Rollup::stddev() const {
    if (samples_.size() < 2) return 0.0;
    const double mean = average();
    double accumulator = 0.0;
    for (double value : samples_) {
        const double delta = value - mean;
        accumulator += delta * delta;
    }
    return std::sqrt(accumulator / static_cast<double>(samples_.size()));
}

double Rollup::jitter() const {
    if (samples_.size() < 2) return 0.0;
    double accumulator = 0.0;
    for (size_t i = 1; i < samples_.size(); ++i) {
        accumulator += std::fabs(samples_[i] - samples_[i - 1]);
    }
    return accumulator / static_cast<double>(samples_.size() - 1);
}

std::string Rollup::toJson() const {
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"count\":%zu,\"avg\":%.2f,\"min\":%.2f,\"max\":%.2f,"
                  "\"p50\":%.2f,\"p90\":%.2f,\"p95\":%.2f,\"stddev\":%.2f,"
                  "\"jitter\":%.2f,\"last\":%.2f}",
                  samples_.size(), average(), minimum(), maximum(), percentile(0.5),
                  percentile(0.9), percentile(0.95), stddev(), jitter(), last());
    return std::string(buffer);
}

}  // namespace vcore
