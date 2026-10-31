#include "synth.h"

#include <cmath>
#include <cstddef>

#include "wav_codec.h"

namespace vcore {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Approximate formant frequencies for a few vowels; cycling through them gives a
// speech-like timbre instead of a bare sine tone.
constexpr double kVowelFormants[][2] = {
    {730.0, 1090.0},   // a
    {530.0, 1840.0},   // e
    {270.0, 2290.0},   // i
    {570.0, 840.0},    // o
    {300.0, 870.0},    // u
};

size_t vowelFor(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return static_cast<size_t>(uc) % (sizeof(kVowelFormants) / sizeof(kVowelFormants[0]));
}

bool isWordChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
           (uc >= '0' && uc <= '9') || uc >= 0x80;
}

}  // namespace

std::vector<int16_t> synthesizeSpeech(const std::string& text, int sampleRate) {
    const int rate = sampleRate > 0 ? sampleRate : 16000;
    std::vector<int16_t> samples;
    if (text.empty()) {
        return samples;
    }

    // Split into syllable-ish units: runs of word characters.
    struct Unit {
        size_t vowel;
        size_t length;
    };
    std::vector<Unit> units;
    size_t runStart = 0;
    bool inRun = false;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool isWord = i < text.size() && isWordChar(text[i]);
        if (isWord && !inRun) {
            runStart = i;
            inRun = true;
        } else if (!isWord && inRun) {
            const size_t runLength = i - runStart;
            // Split long runs into syllables of ~2 characters.
            size_t consumed = 0;
            while (consumed < runLength) {
                const size_t take = std::min<size_t>(2, runLength - consumed);
                units.push_back({vowelFor(text[runStart + consumed]), take});
                consumed += take;
            }
            inRun = false;
        }
    }
    if (units.empty()) {
        return samples;
    }

    const int syllableMs = 140;
    const int gapMs = 40;
    const int syllableSamples = rate * syllableMs / 1000;
    const int gapSamples = rate * gapMs / 1000;
    const int totalSamples = static_cast<int>(units.size()) *
                             (syllableSamples + gapSamples);
    samples.reserve(static_cast<size_t>(totalSamples) + 1);

    double phase = 0.0;
    double phase2 = 0.0;
    for (size_t u = 0; u < units.size(); ++u) {
        const double f1 = kVowelFormants[units[u].vowel][0];
        const double f2 = kVowelFormants[units[u].vowel][1];
        const int length = syllableSamples + static_cast<int>(units[u].length) * 12;

        for (int i = 0; i < length; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(length);
            // Attack/decay envelope keeps clicks out of the output.
            double envelope = 1.0;
            const double attack = 0.08;
            const double release = 0.25;
            if (t < attack) {
                envelope = t / attack;
            } else if (t > 1.0 - release) {
                envelope = (1.0 - t) / release;
            }
            // 120 Hz glottal buzz with two formant resonances.
            const double fundamental = 120.0 + 6.0 * std::sin(2.0 * kPi * 3.0 * t);
            phase += 2.0 * kPi * fundamental / rate;
            phase2 += 2.0 * kPi * (fundamental * 2.0) / rate;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
            if (phase2 > 2.0 * kPi) phase2 -= 2.0 * kPi;

            const double buzz = 0.55 * std::sin(phase) + 0.25 * std::sin(phase2);
            const double formant =
                0.35 * std::sin(2.0 * kPi * f1 * (i / static_cast<double>(rate))) +
                0.20 * std::sin(2.0 * kPi * f2 * (i / static_cast<double>(rate)));
            double value = (buzz + formant) * envelope * 0.45;
            value = std::max(-1.0, std::min(1.0, value));
            samples.push_back(static_cast<int16_t>(value * 32767.0));
        }
        for (int i = 0; i < gapSamples; ++i) {
            samples.push_back(0);
        }
    }
    return samples;
}

std::string synthesizeWav(const std::string& text, int sampleRate) {
    const int rate = sampleRate > 0 ? sampleRate : 16000;
    const std::vector<int16_t> pcm = synthesizeSpeech(text, rate);
    return encodePcm16Wav(reinterpret_cast<const uint8_t*>(pcm.data()),
                          pcm.size() * sizeof(int16_t), rate, 1);
}

std::vector<int16_t> resamplePcm16(const std::vector<int16_t>& input, int fromRate,
                                   int toRate) {
    if (fromRate <= 0 || toRate <= 0 || input.empty() || fromRate == toRate) {
        return input;
    }
    const size_t outputCount = static_cast<size_t>(
        (static_cast<double>(input.size()) * toRate) / fromRate);
    std::vector<int16_t> output(outputCount);
    const double step = static_cast<double>(fromRate) / static_cast<double>(toRate);
    for (size_t i = 0; i < outputCount; ++i) {
        const double source = static_cast<double>(i) * step;
        const size_t index = static_cast<size_t>(source);
        const double fraction = source - static_cast<double>(index);
        const double a = input[std::min(index, input.size() - 1)];
        const double b = input[std::min(index + 1, input.size() - 1)];
        output[i] = static_cast<int16_t>(a + (b - a) * fraction);
    }
    return output;
}

double peakAmplitude(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }
    int peak = 0;
    for (size_t i = 0; i < count; ++i) {
        const int value = samples[i] < 0 ? -static_cast<int>(samples[i])
                                         : static_cast<int>(samples[i]);
        if (value > peak) {
            peak = value;
        }
    }
    return static_cast<double>(peak) / 32768.0;
}

double rmsLevel(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }
    double accumulator = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(samples[i]) / 32768.0;
        accumulator += value * value;
    }
    return std::sqrt(accumulator / static_cast<double>(count));
}

}  // namespace vcore
