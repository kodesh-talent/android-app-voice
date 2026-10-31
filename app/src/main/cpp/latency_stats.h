// Latency statistics: monotonic clocks plus percentile rollups per probe mode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vcore {

// Microseconds from a monotonic clock (immune to wall-clock changes).
int64_t nowMicros();
int64_t nowMillis();

class Rollup {
public:
    void add(double valueMs);
    void clear();

    size_t count() const { return samples_.size(); }
    double last() const;
    double average() const;
    double minimum() const;
    double maximum() const;
    // |percentile| in [0, 1]; 0.5 is the median, 0.95 the p95.
    double percentile(double percentile) const;
    double stddev() const;
    // Jitter: mean absolute successive difference between samples.
    double jitter() const;

    std::string toJson() const;

private:
    std::vector<double> samples_;
};

}  // namespace vcore
