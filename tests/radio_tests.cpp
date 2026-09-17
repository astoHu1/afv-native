// Offline integration tests: real RadioSimulation, Opus, Speex, filters and
// mixer. Link the test target's dummy EffectResources constructor instead of
// EffectResources.cpp (which loads Qt/WAV resources). This file owns main().
#include "afv-native/afv/RadioSimulation.h"
#include "afv-native/Log.h"

#include <event2/event.h>
#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace afv = afv_native::afv;
namespace audio = afv_native::audio;
constexpr std::size_t frameSize = audio::frameSizeSamples;
constexpr unsigned com1 = 118000000, com2 = 121000000;
constexpr double pi = 3.14159265358979323846;
using Mono = std::array<float, frameSize>;
using Samples = std::array<float, 2 * frameSize>;

void require(bool ok, const char* expression, int line) {
    if (!ok) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
// Unlike assert(), these checks and their operands execute in Release builds.
#define REQUIRE(expression) require(bool(expression), #expression, __LINE__)

bool near(double actual, double expected, double tolerance = 0.00001) {
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

double rms(const Samples& samples, unsigned channel = 0, unsigned channels = 1) {
    double energy = 0;
    for (std::size_t i = 0; i < frameSize; ++i) {
        const double sample = samples[i * channels + channel];
        energy += sample * sample;
    }
    return std::sqrt(energy / frameSize);
}

class MemorySamples final : public audio::ISampleStorage {
    mutable Mono samples;
public:
    explicit MemorySamples(float level) { samples.fill(level); }
    audio::SampleType* data() const override { return samples.data(); }
    std::size_t lengthInSamples() const override { return samples.size(); }
};

std::shared_ptr<afv::EffectResources> makeResources() {
    auto resources = std::make_shared<afv::EffectResources>();
    auto silence = std::make_shared<MemorySamples>(0.0f);
    resources->mClick = silence;
    resources->mCrackle = silence;
    resources->mAcBus = silence;
    resources->mVhfWhiteNoise = silence;
    resources->mHfWhiteNoise = silence;
    return resources;
}

// Only read metrics under the same locks as production. No DSP substitution,
// synthetic cached frames, or private state writes are needed for this fixture.
class InspectableRadio final : public afv::RadioSimulation {
public:
    InspectableRadio(event_base* base, std::shared_ptr<afv::EffectResources> resources)
        : RadioSimulation(base, std::move(resources), nullptr, 2) {}

    afv::detail::OutputGain output(bool headset, unsigned channel = 0) {
        std::lock_guard<std::mutex> lock(mRadioStateLock);
        return (headset ? mHeadsetState : mSpeakerState)->outputGain[channel];
    }
    afv::detail::ReceiveGain receive(const std::string& callsign, bool headset = true) {
        std::lock_guard<std::mutex> lock(mStreamMapLock);
        return (headset ? mHeadsetIncomingStreams : mSpeakerIncomingStreams).at(callsign).receiveGain;
    }
    std::size_t streams(bool headset) {
        std::lock_guard<std::mutex> lock(mStreamMapLock);
        return (headset ? mHeadsetIncomingStreams : mSpeakerIncomingStreams).size();
    }
    float strength() {
        std::lock_guard<std::mutex> lock(mRadioStateLock);
        return mAutoOutputGainStrength;
    }
};

struct Fixture {
    // Reverse destruction order keeps the base alive until its timer is freed.
    std::unique_ptr<event_base, decltype(&event_base_free)> base{event_base_new(), event_base_free};
    std::shared_ptr<afv::EffectResources> resources = makeResources();
    std::shared_ptr<InspectableRadio> radio;
    bool split = false;

    Fixture() {
        REQUIRE(base != nullptr);
        radio = std::make_shared<InspectableRadio>(base.get(), resources);
        // These are sample-source adapters, not hardware devices.
        radio->setupDevices(nullptr);
        radio->setFrequency(0, com1);
        radio->setFrequency(1, com2);
        radio->setEnableOutputEffects(false);
        radio->setAutoOutputGain(true);
        radio->setAutoOutputGainStrength(1.0f); // UI strength 100%.
        // Never dispatch the event base, including in the concurrency test.
    }
    void setSplit(bool value) { radio->setSplitAudioChannels(value); split = value; }

    Samples read(bool headset, bool checkShape = true) {
        constexpr float sentinel = 12345.0f;
        std::array<float, 2 * frameSize + 4> guarded;
        guarded.fill(sentinel);
        auto* destination = guarded.data() + 2;
        REQUIRE(radio->getAudioFrame(destination, headset) == audio::SourceStatus::OK);
        REQUIRE(guarded[0] == sentinel && guarded[1] == sentinel);
        REQUIRE(guarded[2 * frameSize + 2] == sentinel && guarded[2 * frameSize + 3] == sentinel);
        Samples result{};
        const auto written = checkShape ? frameSize * (split ? 2 : 1) : 2 * frameSize;
        for (std::size_t i = 0; i < written; ++i) {
            // Concurrent split toggles may leave the unused mono tail untouched.
            if (!checkShape && i >= frameSize && destination[i] == sentinel) continue;
            REQUIRE(std::isfinite(destination[i]) && std::fabs(destination[i]) <= 1.0f);
            result[i] = destination[i];
        }
        if (checkShape && !split)
            for (std::size_t i = frameSize; i < 2 * frameSize; ++i) REQUIRE(destination[i] == sentinel);
        return result;
    }
};

class Voice {
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder{nullptr, opus_encoder_destroy};
    std::uint64_t sampleOffset = 0;
    std::uint32_t sequence = 0;
public:
    std::string callsign;
    unsigned frequency;
    float amplitude;
    double tone;

    Voice(std::string name, unsigned frequency, float amplitude, double tone = 700)
        : callsign(std::move(name)), frequency(frequency), amplitude(amplitude), tone(tone) {
        int status = OPUS_OK;
        encoder.reset(opus_encoder_create(audio::sampleRateHz, 1, OPUS_APPLICATION_VOIP, &status));
        REQUIRE(status == OPUS_OK && encoder != nullptr);
        REQUIRE(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(32000)) == OPUS_OK);
        REQUIRE(opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(3)) == OPUS_OK);
        REQUIRE(opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(0)) == OPUS_OK);
    }

    afv::dto::AudioRxOnTransceivers packet(bool last = false) {
        Mono samples{};
        for (std::size_t i = 0; i < frameSize; ++i)
            samples[i] = amplitude * std::sin(2 * pi * tone * (sampleOffset + i) / audio::sampleRateHz);
        sampleOffset += frameSize;
        std::array<unsigned char, 4000> encoded{};
        const int size = opus_encode_float(encoder.get(), samples.data(), frameSize, encoded.data(), encoded.size());
        REQUIRE(size > 0);
        afv::dto::AudioRxOnTransceivers packet{};
        packet.Callsign = callsign;
        packet.SequenceCounter = sequence++;
        packet.Audio.assign(encoded.begin(), encoded.begin() + size);
        packet.LastPacket = last;
        afv::dto::RxTransceiver transceiver{};
        transceiver.ID = frequency == com2 ? 1 : 0;
        transceiver.Frequency = frequency;
        transceiver.DistanceRatio = 1.0f;
        packet.Transceivers.push_back(transceiver);
        return packet;
    }
};

struct Levels {
    double headset[2]{};
    double speaker[2]{};
    std::uint64_t limitedDuringMeasurement[2]{};
};

Levels run(Fixture& fixture, std::vector<Voice>& voices, int frames = 300) {
    Levels levels;
    std::uint64_t limitedBeforeMeasurement[2]{};
    const int measuredFrames = std::min(60, frames);
    const unsigned channels = fixture.split ? 2 : 1;
    for (int frame = 0; frame < frames; ++frame) {
        if (frame == frames - measuredFrames)
            for (unsigned channel = 0; channel < channels; ++channel)
                limitedBeforeMeasurement[channel] = fixture.radio->output(true, channel).limitedSamples;
        for (auto& voice : voices) fixture.radio->rxVoicePacket(voice.packet());
        const auto headset = fixture.read(true);
        const auto speaker = fixture.read(false); // Consume both independent jitter buffers.
        if (frame >= frames - measuredFrames) {
            for (unsigned channel = 0; channel < channels; ++channel) {
                levels.headset[channel] += std::pow(rms(headset, channel, channels), 2);
                levels.speaker[channel] += std::pow(rms(speaker, channel, channels), 2);
            }
        }
    }
    for (unsigned channel = 0; channel < channels; ++channel) {
        levels.headset[channel] = std::sqrt(levels.headset[channel] / measuredFrames);
        levels.speaker[channel] = std::sqrt(levels.speaker[channel] / measuredFrames);
        levels.limitedDuringMeasurement[channel] = fixture.radio->output(true, channel).limitedSamples
            - limitedBeforeMeasurement[channel];
    }
    return levels;
}

void emptyAndDeviceLifetime() {
    Fixture fixture;
    for (bool split : {false, true}) {
        fixture.setSplit(split);
        for (bool headset : {false, true}) {
            const auto frame = fixture.read(headset);
            for (float sample : frame) REQUIRE(sample == 0);
            const auto gain = fixture.radio->output(headset);
            REQUIRE(gain.activeStreams == 0 && gain.gain == 1 && gain.outputRms == 0);
        }
    }
    REQUIRE(!fixture.radio->getRxActive(0) && !fixture.radio->getRxActive(1));
    auto adapter = fixture.radio->headsetDevice();
    fixture.radio.reset();
    Samples frame{};
    REQUIRE(adapter->getAudioFrame(frame.data()) == audio::SourceStatus::Closed);
}

void loudnessConvergence() {
    for (unsigned count : {1u, 2u, 4u, 8u}) {
        double levels[2]{};
        for (unsigned loud = 0; loud < 2; ++loud) {
            Fixture fixture;
            std::vector<Voice> voices;
            for (unsigned i = 0; i < count; ++i)
                voices.emplace_back("VOICE" + std::to_string(i), com1, loud ? 0.5f : 0.05f, 550 + 250 * i);
            const auto result = run(fixture, voices);
            levels[loud] = result.headset[0];
            const auto gain = fixture.radio->output(true);
            REQUIRE(gain.activeStreams == count);
            REQUIRE(near(gain.gain, 1.0 / std::sqrt(double(count)), 0.0001));
            // Startup attack may legitimately hit the limiter. Settled speech
            // must converge without clipping, including the real overlap tone.
            REQUIRE(result.limitedDuringMeasurement[0] == 0);
            REQUIRE(result.speaker[0] == 0);
            REQUIRE(result.headset[0] > 0.10 && result.headset[0] < 0.21);
            for (const auto& voice : voices) {
                const auto receive = fixture.radio->receive(voice.callsign);
                REQUIRE(near(receive.rms * receive.gain, 0.12, 0.008));
            }
        }
        // Same-frequency overlaps include the real 180Hz blocking tone even
        // with effects bypassed; quiet/loud runs must still converge together.
        REQUIRE(near(levels[0] / levels[1], 1.0, 0.12));
        std::cout << "  " << count << " streams: quiet RMS=" << levels[0]
                  << ", loud RMS=" << levels[1] << '\n';
    }
}

void outputRouting() {
    // One quiet COM1 talker and one loud COM2 talker: merged output balances
    // both streams; split ears and separate devices each balance one stream.
    for (int routing = 0; routing < 4; ++routing) {
        Fixture fixture;
        fixture.setSplit(routing == 1 || routing == 3);
        if (routing >= 2) fixture.radio->setOnHeadset(1, false);
        std::vector<Voice> voices;
        voices.emplace_back("QUIET", com1, 0.05f, 650);
        voices.emplace_back("LOUD", com2, 0.5f, 1300);
        const auto levels = run(fixture, voices);
        if (routing == 0) {
            REQUIRE(fixture.radio->output(true).activeStreams == 2);
            REQUIRE(near(fixture.radio->output(true).gain, std::sqrt(0.5), 0.0001));
            REQUIRE(near(levels.headset[0], 0.12, 0.012));
            REQUIRE(levels.speaker[0] == 0);
        } else if (routing == 1) {
            for (unsigned ear : {0u, 1u}) {
                REQUIRE(fixture.radio->output(true, ear).activeStreams == 1);
                REQUIRE(fixture.radio->output(true, ear).gain == 1);
                REQUIRE(near(levels.headset[ear], 0.12, 0.012));
                REQUIRE(levels.speaker[ear] == 0);
            }
        } else {
            const unsigned speakerChannel = fixture.split ? 1 : 0;
            REQUIRE(fixture.radio->output(true).activeStreams == 1);
            REQUIRE(fixture.radio->output(false, speakerChannel).activeStreams == 1);
            REQUIRE(fixture.radio->output(true).gain == 1);
            REQUIRE(fixture.radio->output(false, speakerChannel).gain == 1);
            REQUIRE(near(levels.headset[0], 0.12, 0.012));
            REQUIRE(near(levels.speaker[speakerChannel], 0.12, 0.012));
            if (fixture.split) REQUIRE(levels.headset[1] == 0 && levels.speaker[0] == 0);
        }
    }
}

void sameCallsignOnBothRadios() {
    Fixture fixture;
    fixture.setSplit(true);
    Voice voice("DUAL", com1, 0.05f);
    for (int i = 0; i < 200; ++i) {
        auto packet = voice.packet();
        auto second = packet.Transceivers.front();
        second.Frequency = com2;
        second.ID = 1;
        packet.Transceivers.push_back(second);
        fixture.radio->rxVoicePacket(packet);
        const auto frame = fixture.read(true);
        fixture.read(false);
        for (std::size_t sample = 0; sample < frameSize; ++sample)
            REQUIRE(frame[2 * sample] == frame[2 * sample + 1]);
    }
    REQUIRE(fixture.radio->streams(true) == 1);
    REQUIRE(fixture.radio->output(true, 0).activeStreams == 1);
    REQUIRE(fixture.radio->output(true, 1).activeStreams == 1);
    REQUIRE(near(fixture.radio->receive("DUAL").rms * fixture.radio->receive("DUAL").gain, 0.12, 0.008));
}

void bypassAndStrengthBounds() {
    Fixture balanced, bypass, zero, zeroTransition;
    bypass.radio->setAutoOutputGain(false);
    zero.radio->setAutoOutputGainStrength(0);
    Voice voice("BYPASS", com1, 0.05f);
    Voice second("BYPASS2", com2, 0.05f, 1200);
    for (int i = 0; i < 240; ++i) {
        // Disable at 100% after convergence; changing only strength to zero
        // must likewise discard all accumulated receive and output gain.
        if (i == 180) {
            balanced.radio->setAutoOutputGain(false);
            zeroTransition.radio->setAutoOutputGainStrength(0);
        }
        const auto packet = voice.packet();
        const auto secondPacket = second.packet();
        for (auto* fixture : {&balanced, &bypass, &zero, &zeroTransition}) {
            fixture->radio->rxVoicePacket(packet);
            fixture->radio->rxVoicePacket(secondPacket);
        }
        const auto reference = bypass.read(true);
        REQUIRE(zero.read(true) == reference);
        const auto output = balanced.read(true);
        const auto strengthOutput = zeroTransition.read(true);
        if (i >= 180) {
            REQUIRE(output == reference && strengthOutput == reference);
            REQUIRE(zeroTransition.radio->receive("BYPASS").gain == 1);
            REQUIRE(zeroTransition.radio->output(true).gain == 1);
        }
        if (i == 170) {
            REQUIRE(rms(output) > 2.0 * rms(reference));
            REQUIRE(zeroTransition.radio->output(true).gain < 0.8);
        }
        for (auto* fixture : {&balanced, &bypass, &zero, &zeroTransition}) fixture->read(false);
    }
    for (float value : {-10.0f, 0.0f, 0.4f, 1.0f, 100.0f,
                        std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
        zero.radio->setAutoOutputGainStrength(value);
        const float expected = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0;
        REQUIRE(zero.radio->strength() == expected);
    }
}

void mutedRadioDoesNotDuckOtherRadio() {
    Fixture reference, muted;
    muted.radio->setGain(1, 0);
    Voice audible("AUDIBLE", com1, 0.05f, 650);
    Voice inaudible("MUTED", com2, 0.5f, 1300);
    for (int i = 0; i < 220; ++i) {
        const auto packet = audible.packet();
        reference.radio->rxVoicePacket(packet);
        muted.radio->rxVoicePacket(packet);
        muted.radio->rxVoicePacket(inaudible.packet());
        REQUIRE(muted.read(true) == reference.read(true));
        muted.read(false);
        reference.read(false);
    }
    REQUIRE(muted.radio->getRxActive(1)); // The muted COM still has an active DTO stream.
    REQUIRE(muted.radio->output(true).activeStreams == 1);
    REQUIRE(muted.radio->output(true).gain == 1);
    REQUIRE(near(muted.radio->output(true).outputRms, 0.12, 0.012));
}

void idleComChangesPreserveActiveCom() {
    Fixture reference, changed;
    Voice first("ACTIVE0", com1, 0.05f, 650);
    Voice second("ACTIVE1", com1, 0.05f, 1300);
    for (int i = 0; i < 240; ++i) {
        if (i == 180 || i == 200) {
            const auto before = changed.radio->receive("ACTIVE0");
            const auto output = changed.radio->output(true);
            REQUIRE(before.gain > 2.5 && output.gain < 0.8);
            REQUIRE(!changed.radio->getRxActive(1));
            if (i == 180) changed.radio->setFrequency(1, com2 + 25000);
            else changed.radio->setOnHeadset(1, false);
            REQUIRE(changed.radio->receive("ACTIVE0").gain == before.gain);
            REQUIRE(changed.radio->receive("ACTIVE0").rms == before.rms);
            REQUIRE(changed.radio->output(true).gain == output.gain);
            REQUIRE(changed.radio->output(true).targetGain == output.targetGain);
        }
        const auto firstPacket = first.packet();
        const auto secondPacket = second.packet();
        for (auto* fixture : {&reference, &changed}) {
            fixture->radio->rxVoicePacket(firstPacket);
            fixture->radio->rxVoicePacket(secondPacket);
        }
        // No loudness transient, lost filter history, or gain restart on COM1.
        REQUIRE(changed.read(true) == reference.read(true));
        changed.read(false);
        reference.read(false);
    }
}

void resetFrequencyAndRouting() {
    struct Change {
        const char* name;
        bool scoped;
        bool movesDevice;
        std::function<void(Fixture&)> apply;
    };
    const Change changes[] = {
        {"frequency", true, false, [](Fixture& f) { f.radio->setFrequency(0, com1 + 25000); }},
        {"device routing", true, true, [](Fixture& f) { f.radio->setOnHeadset(0, false); }},
        {"split routing", false, false, [](Fixture& f) { f.setSplit(true); }},
        {"disable gain", false, false, [](Fixture& f) { f.radio->setAutoOutputGain(false); }},
        {"reset", false, false, [](Fixture& f) { f.radio->reset(); }},
    };
    for (const auto& change : changes) {
        Fixture fixture;
        std::vector<Voice> voices;
        voices.emplace_back("RESET0", com1, 0.05f);
        voices.emplace_back("RESET1", com2, 0.05f, 1200);
        run(fixture, voices, 200);
        REQUIRE(fixture.radio->receive("RESET0").gain > 2.5);
        REQUIRE(fixture.radio->receive("RESET0", false).gain > 2.5);
        REQUIRE(fixture.radio->output(true).gain < 0.8);
        // Idempotent updates must not restart a converged controller.
        const float before = fixture.radio->receive("RESET0").gain;
        fixture.radio->setFrequency(0, com1);
        fixture.radio->setOnHeadset(0, true);
        fixture.radio->setSplitAudioChannels(false);
        fixture.radio->setAutoOutputGain(true);
        REQUIRE(fixture.radio->receive("RESET0").gain == before);
        const auto otherHeadset = fixture.radio->receive("RESET1");
        const auto otherSpeaker = fixture.radio->receive("RESET1", false);
        const auto firstSpeaker = fixture.radio->receive("RESET0", false);
        change.apply(fixture);
        for (bool headset : {false, true}) {
            for (unsigned channel : {0u, 1u}) {
                const auto output = fixture.radio->output(headset, channel);
                REQUIRE(output.gain == 1 && output.targetGain == 1);
                REQUIRE(output.activeStreams == 0 && output.limitedSamples == 0);
            }
            if (fixture.radio->streams(headset)) {
                const auto receive = fixture.radio->receive("RESET0", headset);
                if (!change.scoped || change.movesDevice || headset) {
                    REQUIRE(receive.gain == 1 && receive.rms == 0 && receive.silentFrames == 0);
                } else {
                    REQUIRE(receive.gain == firstSpeaker.gain && receive.rms == firstSpeaker.rms);
                }
                const auto other = fixture.radio->receive("RESET1", headset);
                if (change.scoped) {
                    const auto previous = headset ? otherHeadset : otherSpeaker;
                    REQUIRE(other.gain == previous.gain && other.rms == previous.rms);
                } else {
                    REQUIRE(other.gain == 1 && other.rms == 0);
                }
            }
        }
        std::cout << "  reset gains after " << change.name << '\n';
    }

    Fixture fixture;
    std::vector<Voice> voices;
    voices.emplace_back("RETUNE", com1, 0.1f);
    REQUIRE(run(fixture, voices, 50).headset[0] > 0.04);
    fixture.radio->setFrequency(0, com1 + 25000);
    REQUIRE(run(fixture, voices, 30).headset[0] == 0);
    REQUIRE(!fixture.radio->getRxActive(0));
    voices.front().frequency = com1 + 25000;
    REQUIRE(run(fixture, voices, 100).headset[0] > 0.08);
    fixture.radio->setOnHeadset(0, false);
    const auto routed = run(fixture, voices, 100);
    REQUIRE(routed.headset[0] == 0 && routed.speaker[0] > 0.08);
    fixture.radio->setPtt(true);
    REQUIRE(fixture.radio->getTxActive(0));
    fixture.radio->reset();
    REQUIRE(!fixture.radio->getTxActive(0));
    REQUIRE(fixture.radio->streams(true) == 0 && fixture.radio->streams(false) == 0);
    REQUIRE(rms(fixture.read(true)) == 0 && rms(fixture.read(false)) == 0);
    REQUIRE(!fixture.radio->getRxActive(0));
}

void endOfTransmission() {
    Fixture fixture;
    std::vector<Voice> voices;
    voices.emplace_back("ENDING", com1, 0.05f);
    run(fixture, voices, 180);
    fixture.radio->rxVoicePacket(voices.front().packet(true));
    fixture.read(true);
    fixture.read(false);
    for (int i = 0; i < afv::frameTimeOut + 5; ++i) {
        fixture.read(true);
        fixture.read(false);
    }
    REQUIRE(!fixture.radio->getRxActive(0));
    REQUIRE(fixture.radio->output(true).activeStreams == 0);
    REQUIRE(fixture.radio->receive("ENDING").gain == 1);
    REQUIRE(fixture.radio->receive("ENDING").rms == 0);
    REQUIRE(rms(fixture.read(true)) == 0);
}

void effectsAndFinalLimiter() {
    for (bool enabled : {false, true}) {
        Fixture fixture;
        fixture.radio->setAutoOutputGain(enabled);
        fixture.radio->setEnableOutputEffects(true);
        // Recorded VHF noise alone exceeds full scale after the earlier speech
        // limiter. This proves limiting happens AFTER effects are mixed in.
        fixture.resources->mVhfWhiteNoise = std::make_shared<MemorySamples>(16.0f);
        std::vector<Voice> voices;
        voices.emplace_back("EFFECT", com1, 0.0f);
        const auto levels = run(fixture, voices, 40);
        REQUIRE(levels.headset[0] > 0.99);
        const auto output = fixture.radio->output(true);
        REQUIRE(output.inputRms > 2.0f && output.outputRms <= 1.0f);
        REQUIRE(output.limitedSamples >= frameSize);
    }
    // Clicks are emitted even with zero active speech streams.
    Fixture click;
    click.resources->mClick = std::make_shared<MemorySamples>(-4.0f);
    Voice ending("CLICK", com1, 0.1f);
    click.radio->rxVoicePacket(ending.packet(true));
    click.read(true);
    click.read(false);
    bool heardClick = false;
    for (int i = 0; i < afv::frameTimeOut + 5; ++i) {
        const auto frame = click.read(true);
        click.read(false);
        const auto output = click.radio->output(true);
        if (output.activeStreams == 0 && output.limitedSamples > 0) {
            heardClick = true;
            REQUIRE(output.gain == 1);
            REQUIRE(frame[0] == -1);
            break;
        }
    }
    REQUIRE(heardClick);

    Fixture unsafeEffect;
    unsafeEffect.radio->setEnableOutputEffects(true);
    unsafeEffect.resources->mVhfWhiteNoise = std::make_shared<MemorySamples>(std::numeric_limits<float>::infinity());
    std::vector<Voice> voices;
    voices.emplace_back("FINITE", com1, 0.2f);
    run(unsafeEffect, voices, 20); // read() checks every output sample is finite and bounded.

    Fixture hot;
    hot.setSplit(true);
    hot.radio->setAutoOutputGain(false);
    hot.radio->setGain(0, 50);
    hot.radio->setGain(1, 50);
    std::vector<Voice> hotVoices;
    hotVoices.emplace_back("HOT0", com1, 0.5f);
    hotVoices.emplace_back("HOT1", com2, 0.5f, 1200);
    run(hot, hotVoices, 30);
    REQUIRE(hot.radio->output(true, 0).limitedSamples > 0);
    REQUIRE(hot.radio->output(true, 1).limitedSamples > 0);
}

void captureMetersAndBounds() {
    Fixture fixture;
    Mono input;
    input.fill(0.2f);
    fixture.radio->setEnableInputFilters(false);
    for (int i = 0; i < 20; ++i) fixture.radio->putAudioFrame(input.data());
    REQUIRE(fixture.radio->getVu() > 0.5 && fixture.radio->getVu() < 0.8);
    REQUIRE(fixture.radio->getPeak() > 0.5 && fixture.radio->getPeak() <= 1);
    input[0] = std::numeric_limits<float>::quiet_NaN();
    input[1] = std::numeric_limits<float>::infinity();
    input[2] = -std::numeric_limits<float>::infinity();
    input[3] = 2;
    input[4] = -2;
    fixture.radio->setMicrophoneVolume(std::numeric_limits<float>::infinity());
    fixture.radio->putAudioFrame(input.data());
    REQUIRE(fixture.radio->getPeak() == 1);
    fixture.radio->setMicrophoneVolume(-1);
    for (int i = 0; i < 20; ++i) fixture.radio->putAudioFrame(input.data());
    REQUIRE(fixture.radio->getVu() == 0 && fixture.radio->getPeak() == 0);
    fixture.radio->setGain(100, 3);
    fixture.radio->setFrequency(100, com1);
    fixture.radio->setOnHeadset(100, false);
    fixture.radio->setTxRadio(100);
    REQUIRE(!fixture.radio->getRxActive(100) && !fixture.radio->getTxActive(100));
    fixture.radio->setGain(0, std::numeric_limits<float>::quiet_NaN());
    fixture.radio->setGain(0, std::numeric_limits<float>::infinity());
    fixture.radio->setGain(0, -1);
    std::vector<Voice> voices;
    voices.emplace_back("MUTED", com1, 0.5f);
    REQUIRE(run(fixture, voices, 25).headset[0] == 0);
}

void concurrentCallbacksAndControls() {
    Fixture fixture;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::atomic<unsigned> completed{0};
    std::mutex failureLock;
    std::exception_ptr failure;
    std::vector<std::thread> workers;
    constexpr int iterations = 320;
    auto launch = [&](std::function<void(int)> action) {
        workers.emplace_back([&, action] {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            try {
                for (int i = 0; i < iterations; ++i) { action(i); std::this_thread::yield(); }
                completed.fetch_add(1);
            } catch (...) {
                std::lock_guard<std::mutex> lock(failureLock);
                if (!failure) failure = std::current_exception();
            }
        });
    };
    Voice incoming("THREAD", com1, 0.1f);
    launch([&](int i) {
        incoming.frequency = i % 2 ? com1 : com2;
        fixture.radio->rxVoicePacket(incoming.packet(i % 19 == 0));
    });
    launch([&](int) { fixture.read(true, false); });
    launch([&](int) { fixture.read(false, false); });
    Mono microphone;
    microphone.fill(0.2f);
    microphone[0] = std::numeric_limits<float>::quiet_NaN();
    launch([&](int) { fixture.radio->putAudioFrame(microphone.data()); });
    launch([&](int i) {
        fixture.radio->setAutoOutputGain(i % 3 != 0);
        fixture.radio->setAutoOutputGainStrength(float(i % 13 - 1) / 10);
        fixture.radio->setFrequency(i % 2, i % 2 ? com2 : com1);
        fixture.radio->setOnHeadset(i % 2, i % 3 != 0);
        fixture.radio->setSplitAudioChannels(i % 2 != 0);
        fixture.radio->setGain(i % 2, float(i % 5));
        fixture.radio->setMicrophoneVolume(float(i % 4));
        fixture.radio->setPtt(i % 2 != 0);
        fixture.radio->setTxRadio(i % 2);
        fixture.radio->setCallsign("OFFLINE");
        fixture.radio->setEnableOutputEffects(i % 2 != 0);
        fixture.radio->setEnableHfSquelch(i % 2 != 0);
        fixture.radio->setUDPChannel(nullptr);
    });
    launch([&](int i) {
        fixture.radio->setEnableInputFilters(i % 2 != 0);
        (void)fixture.radio->getEnableInputFilters();
        const double vu = fixture.radio->getVu(), peak = fixture.radio->getPeak();
        REQUIRE(std::isfinite(vu) && vu >= 0 && vu <= 1);
        REQUIRE(std::isfinite(peak) && peak >= 0 && peak <= 1);
        (void)fixture.radio->getRxActive(i % 2);
        (void)fixture.radio->getTxActive(i % 2);
    });
    launch([&](int) { fixture.radio->reset(); });

    // Bound lock-order regressions even when run without CTest's timeout. The
    // watchdog never touches libevent and returns the process nonzero on a hang.
    std::mutex watchdogLock;
    std::condition_variable watchdogWake;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(watchdogLock);
        if (!watchdogWake.wait_for(lock, std::chrono::seconds(30), [&] { return finished; })) {
            std::cerr << "FAIL concurrent callbacks timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
    });
    while (ready.load() != workers.size()) std::this_thread::yield();
    start.store(true);
    for (auto& worker : workers) worker.join();
    {
        std::lock_guard<std::mutex> lock(watchdogLock);
        finished = true;
    }
    watchdogWake.notify_one();
    watchdog.join();
    if (failure) std::rethrow_exception(failure);
    REQUIRE(completed.load() == workers.size());
    fixture.radio->setEnableInputFilters(false);
    REQUIRE(!fixture.radio->getEnableInputFilters());
    fixture.radio->reset();
    fixture.setSplit(false);
    fixture.radio->setEnableOutputEffects(false);
    fixture.radio->setOnHeadset(0, true);
    fixture.radio->setOnHeadset(1, true);
    fixture.radio->setGain(0, 1);
    fixture.radio->setAutoOutputGain(true);
    fixture.radio->setAutoOutputGainStrength(1);
    std::vector<Voice> recovery;
    recovery.emplace_back("RECOVERY", com1, 0.1f);
    REQUIRE(near(run(fixture, recovery, 180).headset[0], 0.12, 0.012));
}
} // namespace

int main() {
    afv_native::setLogger(nullptr, nullptr);
    const std::pair<const char*, void (*)()> tests[] = {
        {"zero streams / output lifetime", emptyAndDeviceLifetime},
        {"1/2/4/8 quiet and loud Opus streams", loudnessConvergence},
        {"COM1+COM2 merged / split / headset / speaker", outputRouting},
        {"single callsign decoded once for both radios", sameCallsignOnBothRadios},
        {"strength zero / disabled at 100% / strength bounds", bypassAndStrengthBounds},
        {"muted COM does not attenuate audible COM", mutedRadioDoesNotDuckOtherRadio},
        {"idle COM2 retune / reroute preserves active COM1", idleComChangesPreserveActiveCom},
        {"reset / retune / routing invalidates gain", resetFrequencyAndRouting},
        {"last packet clears receive gain", endOfTransmission},
        {"effects / click / final limiter / finite output", effectsAndFinalLimiter},
        {"capture meters / invalid values / bounds", captureMetersAndBounds},
        {"concurrent RX / capture / playback / setters / reset", concurrentCallbacksAndControls},
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
