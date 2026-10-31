package com.xeno.voicelatency;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Aggregate statistics over a batch of millisecond samples. */
public final class StatSummary {

    public final int count;
    public final double avg;
    public final double min;
    public final double max;
    public final double p50;
    public final double p90;
    public final double p95;
    public final double stddev;
    public final double jitter;

    private StatSummary(int count, double avg, double min, double max, double p50, double p90,
                        double p95, double stddev, double jitter) {
        this.count = count;
        this.avg = avg;
        this.min = min;
        this.max = max;
        this.p50 = p50;
        this.p90 = p90;
        this.p95 = p95;
        this.stddev = stddev;
        this.jitter = jitter;
    }

    public static StatSummary empty() {
        return new StatSummary(0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    /** Computes the summary from millisecond samples. */
    public static StatSummary of(List<Double> samplesMs) {
        if (samplesMs == null || samplesMs.isEmpty()) {
            return empty();
        }
        List<Double> values = new ArrayList<>(samplesMs);
        Collections.sort(values);
        final int n = values.size();

        double sum = 0;
        for (double value : values) {
            sum += value;
        }
        final double mean = sum / n;

        double variance = 0;
        for (double value : values) {
            variance += (value - mean) * (value - mean);
        }
        final double sd = n > 1 ? Math.sqrt(variance / n) : 0;

        double jitterSum = 0;
        for (int i = 1; i < n; i++) {
            jitterSum += Math.abs(values.get(i) - values.get(i - 1));
        }
        final double jitter = n > 1 ? jitterSum / (n - 1) : 0;

        return new StatSummary(n, mean, values.get(0), values.get(n - 1),
                percentile(values, 0.50), percentile(values, 0.90), percentile(values, 0.95),
                sd, jitter);
    }

    private static double percentile(List<Double> sorted, double percentile) {
        if (sorted.isEmpty()) {
            return 0;
        }
        final double position = Math.min(1.0, Math.max(0.0, percentile)) * (sorted.size() - 1);
        final int lower = (int) Math.floor(position);
        final int upper = (int) Math.ceil(position);
        final double weight = position - lower;
        return sorted.get(lower) * (1 - weight) + sorted.get(upper) * weight;
    }

    public boolean isEmpty() {
        return count == 0;
    }

    public String formatAvg() {
        return count == 0 ? "—" : String.format(Locale.US, "%.1f ms", avg);
    }

    public String formatP95() {
        return count == 0 ? "—" : String.format(Locale.US, "%.1f ms", p95);
    }

    public String formatRange() {
        if (count == 0) {
            return "—";
        }
        return String.format(Locale.US, "%.1f – %.1f ms", min, max);
    }

    public String formatJitter() {
        return count == 0 ? "—" : String.format(Locale.US, "%.1f ms", jitter);
    }
}
