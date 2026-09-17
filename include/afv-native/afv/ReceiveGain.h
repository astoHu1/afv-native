#ifndef AFV_NATIVE_RECEIVE_GAIN_H
#define AFV_NATIVE_RECEIVE_GAIN_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace afv_native { namespace afv { namespace detail {

// State belongs to the playback thread, under RadioSimulation's radio/stream
// locks. The frame duration is 20 ms. No heap allocation or logging in DSP.
struct ReceiveGain {
    static constexpr float targetRms = 0.12f;
    static constexpr float noiseGate = 0.00316228f; // -50 dBFS
    float gain = 1.0f;
    float rms = 0.0f;
    unsigned silentFrames = 0;

    void reset() { *this = ReceiveGain{}; }

    void process(float* samples, std::size_t count, bool enabled, float strength) {
        double energy = 0;
        float peak = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(samples[i])) samples[i] = 0;
            energy += static_cast<double>(samples[i]) * samples[i];
            peak = std::max(peak, std::fabs(samples[i]));
        }
        const float measured = count ? static_cast<float>(std::sqrt(energy / count)) : 0;
        if (!enabled || strength <= 0) { reset(); return; }
        float target = gain;
        if (measured >= noiseGate) {
            silentFrames = 0;
            // Follow rising levels faster; retain an estimate through syllables.
            rms += (measured - rms) * (measured > rms ? 0.4f : 0.06f);
            float correction = std::clamp(targetRms / std::max(rms, noiseGate), 0.25f, 4.0f);
            if (peak > 0) correction = std::min(correction, 0.90f / peak);
            target = 1.0f + strength * (correction - 1.0f);
        } else if (++silentFrames >= 25) {
            rms = 0;
            target = 1.0f;
        }
        const float next = gain + (target - gain) * (target < gain ? 0.35f : 0.05f);
        for (std::size_t i = 0; i < count; ++i) {
            // Never raise sub-gate background noise, even after quiet speech.
            const float ramp = gain + (next - gain) * static_cast<float>(i + 1) / count;
            samples[i] *= measured < noiseGate ? std::min(1.0f, ramp) : ramp;
        }
        gain = next;
    }
};

struct OutputGain {
    float gain = 1.0f;
    // Inspectable offline/debug metrics; no realtime I/O.
    float inputRms = 0.0f;
    float outputRms = 0.0f;
    float targetGain = 1.0f;
    unsigned activeStreams = 0;
    std::uint64_t limitedSamples = 0;

    void reset() { *this = OutputGain{}; }

    void process(float* samples, std::size_t count, unsigned streams, bool enabled, float strength) {
        activeStreams = streams;
        targetGain = enabled && strength > 0 && streams > 1
            ? std::pow(static_cast<float>(streams), -0.5f * strength) : 1.0f;
        if (!enabled || strength <= 0 || streams == 0) gain = 1.0f;
        const float next = gain + (targetGain - gain) * (targetGain < gain ? 0.35f : 0.08f);
        double inputEnergy = 0, outputEnergy = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const float input = std::isfinite(samples[i]) ? samples[i] : 0;
            inputEnergy += static_cast<double>(input) * input;
            const float scaled = input * (gain + (next - gain) * static_cast<float>(i + 1) / count);
            // Transparent below full scale, including when balancing is disabled.
            // All speech and effects reach this final output safety limiter.
            if (std::fabs(scaled) > 1.0f) ++limitedSamples;
            samples[i] = std::clamp(scaled, -1.0f, 1.0f);
            outputEnergy += static_cast<double>(samples[i]) * samples[i];
        }
        gain = next;
        inputRms = count ? static_cast<float>(std::sqrt(inputEnergy / count)) : 0;
        outputRms = count ? static_cast<float>(std::sqrt(outputEnergy / count)) : 0;
    }
};

}}}
#endif
