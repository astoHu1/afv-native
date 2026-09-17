// Standalone, offline DSP checks. REQUIRE deliberately remains active under NDEBUG.
#include "afv-native/afv/ReceiveGain.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using afv_native::afv::detail::OutputGain;
using afv_native::afv::detail::ReceiveGain;
using Frame = std::array<float, 960>;

void require(bool ok, const char* expression, int line) {
    if (!ok) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define REQUIRE(expression) require(bool(expression), #expression, __LINE__)

bool near(float actual, float expected, float tolerance = 0.00001f) {
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

Frame constant(float amplitude) {
    Frame frame;
    frame.fill(amplitude);
    return frame;
}

Frame settle(ReceiveGain& gain, float amplitude, float strength = 1.0f, int frames = 400) {
    Frame frame{};
    for (int i = 0; i < frames; ++i) {
        frame = constant(amplitude);
        gain.process(frame.data(), frame.size(), true, strength);
        REQUIRE(std::isfinite(gain.gain));
        REQUIRE(gain.gain >= 0.2499f && gain.gain <= 4.0001f);
    }
    return frame;
}

void convergenceAndStrength() {
    // Inputs straddle the target, including both correction caps.
    for (float amplitude : {0.004f, 0.04f, 0.12f, 0.48f, 0.8f}) {
        for (float strength : {0.0f, 0.25f, 0.6f, 1.0f}) {
            ReceiveGain gain;
            const auto frame = settle(gain, amplitude, strength);
            const float correction = std::clamp(ReceiveGain::targetRms / amplitude, 0.25f, 4.0f);
            const float expected = 1.0f + strength * (correction - 1.0f);
            REQUIRE(near(gain.gain, expected, 0.0001f));
            REQUIRE(near(frame.back(), amplitude * expected, 0.0001f));
        }
    }
    ReceiveGain quiet, loud;
    const auto quietFrame = settle(quiet, 0.04f);
    const auto loudFrame = settle(loud, 0.48f);
    REQUIRE(near(quietFrame.back(), 0.12f, 0.0001f));
    REQUIRE(near(loudFrame.back(), 0.12f, 0.0001f));
}

void attackReleaseAndRamp() {
    ReceiveGain gain;
    settle(gain, 0.04f);
    const float before = gain.gain;
    auto loud = constant(0.48f);
    gain.process(loud.data(), loud.size(), true, 1.0f);
    REQUIRE(gain.gain < before);
    REQUIRE(loud.front() > loud.back());
    // The gain changes across a frame instead of stepping at its first sample.
    REQUIRE(std::fabs(loud.front() - 0.48f * before) < 0.002f);
    for (std::size_t i = 1; i < loud.size(); ++i)
        REQUIRE(loud[i] <= loud[i - 1] && loud[i - 1] - loud[i] < 0.002f);
    settle(gain, 0.48f, 1.0f, 20);
    REQUIRE(gain.gain < 0.30f);

    const float attenuated = gain.gain;
    const auto recovering = settle(gain, 0.04f, 1.0f, 20);
    REQUIRE(gain.gain > attenuated);
    REQUIRE(gain.gain < 1.5f); // Raising quiet speech is deliberately slower.
    REQUIRE(recovering.front() <= recovering.back());
    settle(gain, 0.04f);
    REQUIRE(near(gain.gain, 3.0f, 0.0001f));
}

void noiseAndSilence() {
    ReceiveGain gain;
    settle(gain, 0.04f);
    const float boosted = gain.gain;
    for (unsigned i = 0; i < 24; ++i) {
        auto noise = constant(ReceiveGain::noiseGate * 0.5f);
        const auto original = noise;
        gain.process(noise.data(), noise.size(), true, 1.0f);
        REQUIRE(noise == original); // Previously boosted speech must not raise background noise.
        REQUIRE(near(gain.gain, boosted));
    }
    REQUIRE(gain.silentFrames == 24);
    REQUIRE(gain.rms > 0.03f); // Hold the speech estimate through short pauses.
    auto silence = constant(0.0f);
    gain.process(silence.data(), silence.size(), true, 1.0f);
    REQUIRE(gain.silentFrames == 25);
    REQUIRE(gain.rms == 0.0f);
    REQUIRE(gain.gain < boosted && gain.gain > 1.0f);
    settle(gain, 0.0f);
    REQUIRE(near(gain.gain, 1.0f, 0.0001f));

    auto speech = constant(0.04f);
    gain.process(speech.data(), speech.size(), true, 1.0f);
    REQUIRE(gain.silentFrames == 0);
    REQUIRE(gain.gain > 1.0f);
    ReceiveGain boundary;
    auto atGate = constant(ReceiveGain::noiseGate);
    boundary.process(atGate.data(), atGate.size(), true, 1.0f);
    REQUIRE(boundary.silentFrames == 0);
    REQUIRE(atGate.back() > ReceiveGain::noiseGate);
}

void bypassResetAndFinite() {
    for (bool enabled : {false, true}) {
        ReceiveGain gain;
        settle(gain, 0.04f);
        Frame frame{};
        for (std::size_t i = 0; i < frame.size(); ++i)
            frame[i] = (i % 2 ? -1.0f : 1.0f) * float(i) / frame.size();
        const auto original = frame;
        gain.process(frame.data(), frame.size(), enabled, enabled ? 0.0f : 1.0f);
        REQUIRE(frame == original);
        REQUIRE(gain.gain == 1.0f && gain.rms == 0.0f && gain.silentFrames == 0);
    }
    for (bool enabled : {false, true}) {
        ReceiveGain gain;
        auto frame = constant(0.1f);
        frame[0] = std::numeric_limits<float>::quiet_NaN();
        frame[1] = std::numeric_limits<float>::infinity();
        frame[2] = -std::numeric_limits<float>::infinity();
        gain.process(frame.data(), frame.size(), enabled, 1.0f);
        REQUIRE(frame[0] == 0 && frame[1] == 0 && frame[2] == 0);
        for (float sample : frame) REQUIRE(std::isfinite(sample));
        REQUIRE(std::isfinite(gain.gain) && std::isfinite(gain.rms));
        gain.process(nullptr, 0, enabled, 1.0f);
        REQUIRE(std::isfinite(gain.gain));
        gain.reset();
        REQUIRE(gain.gain == 1.0f && gain.rms == 0.0f && gain.silentFrames == 0);
    }
    // ReceiveGain is not itself a hard limiter: OutputGain is responsible for
    // the safety bound after speech, overlap tones, and recorded effects mix.
    ReceiveGain gain;
    auto huge = constant(std::numeric_limits<float>::max());
    gain.process(huge.data(), huge.size(), true, 1.0f);
    OutputGain output;
    output.process(huge.data(), huge.size(), 1, true, 1.0f);
    for (float sample : huge) REQUIRE(std::isfinite(sample) && std::fabs(sample) <= 1.0f);
}

void outputCountsAndStrength() {
    for (unsigned streams : {0u, 1u, 2u, 4u, 8u, 32u}) {
        for (float strength : {0.0f, 0.25f, 0.6f, 1.0f}) {
            OutputGain gain;
            Frame frame{};
            for (int i = 0; i < 300; ++i) {
                frame = constant(0.5f);
                gain.process(frame.data(), frame.size(), streams, true, strength);
            }
            const float expected = streams > 1 ? std::pow(float(streams), -0.5f * strength) : 1.0f;
            REQUIRE(near(gain.gain, expected));
            REQUIRE(near(gain.targetGain, expected));
            REQUIRE(gain.activeStreams == streams);
            REQUIRE(near(gain.inputRms, 0.5f));
            REQUIRE(near(gain.outputRms, 0.5f * expected));
            REQUIRE(near(frame.front(), frame.back()));
            REQUIRE(gain.limitedSamples == 0);
        }
    }
    OutputGain gain;
    auto frame = constant(0.5f);
    gain.process(frame.data(), frame.size(), 4, true, 1.0f);
    const float attack = 1.0f - gain.gain;
    REQUIRE(frame.front() > frame.back());
    frame = constant(0.5f);
    const float before = gain.gain;
    gain.process(frame.data(), frame.size(), 1, true, 1.0f);
    REQUIRE(gain.gain > before && gain.gain - before < attack);
    REQUIRE(frame.front() < frame.back());
    frame = constant(0.0f);
    gain.process(frame.data(), frame.size(), 0, true, 1.0f);
    REQUIRE(gain.gain == 1.0f && gain.outputRms == 0.0f);
}

void finalLimiterAndPassthrough() {
    for (bool enabled : {false, true}) {
        for (float strength : {0.0f, 1.0f}) {
            OutputGain gain;
            auto warmup = constant(0.5f);
            gain.process(warmup.data(), warmup.size(), 4, true, 1.0f);
            std::array<float, 11> frame{{-4, -1, -0.25f, -0.0f, 0.25f, 1, 4,
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(), 0}};
            gain.process(frame.data(), frame.size(), 1, enabled, strength);
            for (float sample : frame) REQUIRE(std::isfinite(sample) && std::fabs(sample) <= 1);
            REQUIRE(frame[0] == -1 && frame[6] == 1);
            REQUIRE(frame[7] == 0 && frame[8] == 0 && frame[9] == 0);
            REQUIRE(gain.limitedSamples == 2);
            REQUIRE(std::isfinite(gain.inputRms) && std::isfinite(gain.outputRms));
            if (!enabled || strength == 0) {
                REQUIRE(frame[1] == -1 && frame[2] == -0.25f);
                REQUIRE(frame[4] == 0.25f && frame[5] == 1);
                REQUIRE(gain.gain == 1.0f);
            }
            gain.reset();
            REQUIRE(gain.gain == 1 && gain.targetGain == 1);
            REQUIRE(gain.activeStreams == 0 && gain.limitedSamples == 0);
            REQUIRE(gain.inputRms == 0 && gain.outputRms == 0);
            gain.process(nullptr, 0, 0, enabled, strength);
            REQUIRE(gain.inputRms == 0 && gain.outputRms == 0);
        }
    }
}
} // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"receive convergence / strength endpoints", convergenceAndStrength},
        {"receive attack / release / sample ramp", attackReleaseAndRamp},
        {"noise gate / silence / recovery", noiseAndSilence},
        {"bypass / reset / finite samples", bypassResetAndFinite},
        {"output stream counts / strength / recovery", outputCountsAndStrength},
        {"final limiter / transparent bypass", finalLimiterAndPassthrough},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << e.what() << '\n';
        }
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
