#include "afv-native/Client.h"
#include "afv-native/Log.h"
#include "afv-native/afv/RemoteVoiceSource.h"
#include "afv-native/afv/VoiceCompressionSink.h"
#include "afv-native/afv/dto/voice_server/Heartbeat.h"
#include "afv-native/cryptodto/UDPChannel.h"
#include "afv-native/cryptodto/dto/ChannelConfig.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace afv_native;
using namespace std::chrono_literals;
#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)

// No audio hardware, Qt resource loading, API requests or live server connections.
std::vector<std::pair<std::string, bool>> deviceStarts;
std::shared_ptr<audio::AudioDevice> audio::AudioDevice::makeDevice(
        const std::string& name, const std::string&, Api, bool split)
{
    deviceStarts.emplace_back(name, split);
    return nullptr;
}

struct LogGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    int calls = 0;
};
void blockingLogger(const char*, const char*, int, const char*, void* ref)
{
    auto& gate = *static_cast<LogGate*>(ref);
    std::unique_lock<std::mutex> lock(gate.mutex);
    ++gate.calls;
    gate.entered = true;
    gate.cv.notify_all();
    gate.cv.wait(lock, [&] { return gate.release; });
}
void testLoggerDrain(bool hex)
{
    LogGate gate;
    setLogger(blockingLogger, &gate);
    std::thread writer([&] {
        if (hex) { const unsigned char bytes[2] = {1, 2}; LOGDUMPHEX("test", bytes, 2); }
        else { LOG("test", "%s %d", "message", 42); }
    });
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        CHECK(gate.cv.wait_for(lock, 2s, [&] { return gate.entered; }));
    }
    std::promise<void> clearing;
    auto cleared = std::async(std::launch::async, [&] {
        clearing.set_value();
        setLogger(nullptr, nullptr);
    });
    clearing.get_future().wait();
    CHECK(cleared.wait_for(50ms) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.cv.notify_all();
    writer.join();
    CHECK(cleared.wait_for(2s) == std::future_status::ready);
    cleared.get();
    LOG("test", "after clear");
    const unsigned char byte = 3;
    LOGDUMPHEX("test", &byte, 1);
    CHECK(gate.calls == 1);
}
std::atomic<int> nullContextCalls{0};
void nullContextLogger(const char*, const char*, int, const char*, void* ref)
{
    CHECK(ref == nullptr);
    ++nullContextCalls;
    // Also verify replacement from inside the active callback.
    setLogger(nullptr, nullptr);
}
struct LogContext { int tag; std::atomic<int> calls{0}; };
void firstLogger(const char*, const char*, int, const char*, void* ref)
{
    auto& context = *static_cast<LogContext*>(ref);
    CHECK(context.tag == 1);
    ++context.calls;
}
void secondLogger(const char*, const char*, int, const char*, void* ref)
{
    auto& context = *static_cast<LogContext*>(ref);
    CHECK(context.tag == 2);
    ++context.calls;
}
void testLoggerReplacement()
{
    setLogger(nullContextLogger, nullptr);
    LOG("test", "null context is valid");
    CHECK(nullContextCalls == 1);
    LogContext one{1}, two{2};
    std::atomic<bool> done{false};
    std::thread writer([&] { while (!done) LOG("test", "replacement"); });
    for (int i = 0; i < 500; ++i) {
        setLogger(firstLogger, &one);
        setLogger(secondLogger, &two);
        setLogger(nullptr, nullptr);
    }
    done = true;
    writer.join();
}

struct FrameSink : afv::ICompressedFrameSink {
    std::atomic<int> calls{0};
    afv::VoiceCompressionSink* resetFromCallback = nullptr;
    void processCompressedFrame(std::vector<unsigned char> data) override
    {
        CHECK(!data.empty());
        ++calls;
        if (resetFromCallback) resetFromCallback->reset();
    }
};
void testEncoder()
{
    FrameSink output;
    afv::VoiceCompressionSink sink(output);
    std::array<float, audio::frameSizeSamples> samples{};
    sink.close();
    sink.close();
    sink.putAudioFrame(samples.data());
    CHECK(output.calls == 0);
    CHECK(sink.open() == OPUS_OK);
    output.resetFromCallback = &sink;
    sink.putAudioFrame(samples.data());
    CHECK(output.calls == 1);
    output.resetFromCallback = nullptr;
    std::thread capture([&] { for (int i = 0; i < 1500; ++i) sink.putAudioFrame(samples.data()); });
    std::thread configure([&] {
        for (int i = 0; i < 400; ++i) {
            sink.close();
            sink.reset();
            CHECK(sink.open() == OPUS_OK);
        }
    });
    capture.join();
    configure.join();
    sink.putAudioFrame(samples.data());
    CHECK(output.calls > 1);
}
std::vector<unsigned char> opusFrame(int frameSize = audio::frameSizeSamples)
{
    int status;
    auto* encoder = opus_encoder_create(audio::sampleRateHz, 1, OPUS_APPLICATION_VOIP, &status);
    CHECK(status == OPUS_OK);
    std::array<float, audio::frameSizeSamples> samples{};
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = 0.1f * std::sin(i * 0.03f);
    std::vector<unsigned char> bytes(4096);
    int length = opus_encode_float(encoder, samples.data(), frameSize, bytes.data(), bytes.size());
    CHECK(length > 0);
    bytes.resize(length);
    opus_encoder_destroy(encoder);
    return bytes;
}
struct BrokenSource : afv::RemoteVoiceSource {
    void removeDecoder() { opus_decoder_destroy(mDecoder); mDecoder = nullptr; }
};
void testStream()
{
    afv::RemoteVoiceSource source;
    CHECK(!source.isActive());
    CHECK(source.getLastActivityTime() == 0);
    afv::dto::IAudio packet{};
    packet.Audio = opusFrame();
    packet.SequenceCounter = 5000;
    packet.LastPacket = true;
    source.appendAudioDTO(packet);
    CHECK(source.isActive());
    CHECK(source.getLastActivityTime() > 0);
    std::array<float, audio::frameSizeSamples> samples{};
    CHECK(source.getAudioFrame(samples.data()) == audio::SourceStatus::OK);
    // A high sequence must terminate on the next missing frame, not timeout.
    CHECK(source.getAudioFrame(samples.data()) == audio::SourceStatus::Closed);
    CHECK(std::all_of(samples.begin(), samples.end(), [](float s) { return s == 0; }));
    source.flush();
    CHECK(!source.isActive());
    packet.SequenceCounter = UINT32_MAX;
    source.appendAudioDTO(packet);
    CHECK(source.getAudioFrame(samples.data()) == audio::SourceStatus::OK);
    CHECK(source.getAudioFrame(samples.data()) == audio::SourceStatus::Closed);
    source.flush();
    packet.SequenceCounter = 100;
    packet.LastPacket = false;
    packet.Audio = opusFrame(audio::frameSizeSamples / 2);
    source.appendAudioDTO(packet);
    samples.fill(99);
    CHECK(source.getAudioFrame(samples.data()) == audio::SourceStatus::OK);
    CHECK(std::all_of(samples.begin() + audio::frameSizeSamples / 2, samples.end(), [](float s) { return s == 0; }));
    BrokenSource broken;
    broken.removeDecoder();
    broken.appendAudioDTO(packet);
    broken.flush();
    samples.fill(99);
    CHECK(broken.getAudioFrame(samples.data()) == audio::SourceStatus::Error);
    CHECK(std::all_of(samples.begin(), samples.end(), [](float s) { return s == 0; }));
    packet.Audio = opusFrame();
    std::thread producer([&] {
        for (int i = 0; i < 2000; ++i) {
            packet.SequenceCounter = i;
            packet.LastPacket = i % 40 == 39;
            source.appendAudioDTO(packet);
        }
    });
    std::thread consumer([&] {
        for (int i = 0; i < 2000; ++i) {
            source.getAudioFrame(samples.data());
            for (float s : samples) CHECK(std::isfinite(s));
        }
    });
    std::thread maintenance([&] {
        for (int i = 0; i < 1000; ++i) {
            source.isActive();
            source.getLastActivityTime();
            source.flush();
        }
    });
    producer.join();
    consumer.join();
    maintenance.join();
}

struct FakeDevice : audio::AudioDevice {
    int closes = 0;
    std::function<void()> onClose;
    std::weak_ptr<const afv::RadioSimulation> simulation;
    explicit FakeDevice(std::shared_ptr<const afv::RadioSimulation> sim) : simulation(sim) {}
    bool openInput() override { return true; }
    bool openOutput() override { return true; }
    void close() override { CHECK(!simulation.expired()); if (onClose) onClose(); ++closes; }
};
struct InspectableClient : Client {
    InspectableClient(event_base* base, unsigned count) : Client(base, count, "offline-test", "http://127.0.0.1") {}
    size_t radioCount() const { return mRadioState.size(); }
    int frequency(unsigned index) const { return mRadioState.at(index).mNextFreq; }
    void devices(const std::shared_ptr<FakeDevice>& microphone, const std::shared_ptr<FakeDevice>& headset,
                 const std::shared_ptr<FakeDevice>& speaker)
    { mMicrophoneDevice = microphone; mHeadsetDevice = headset; mSpeakerDevice = speaker; }
};
void testClient()
{
    auto* base = event_base_new();
    CHECK(base);
    for (unsigned count : {0u, 1u, 3u}) {
        auto client = std::make_unique<InspectableClient>(base, count);
        CHECK(client->radioCount() == count);
        if (count) { client->setRadioState(count - 1, 123450000); CHECK(client->frequency(count - 1) == 123450000); }
        for (unsigned invalid : {count, UINT32_MAX}) {
            client->setRadioState(invalid, 123450000);
            client->setRadioGain(invalid, 1);
            client->setTxRadio(invalid);
            client->setOnHeadset(invalid, false);
            CHECK(!client->getRxActive(invalid));
            CHECK(!client->getTxActive(invalid));
        }
        auto mic = std::make_shared<FakeDevice>(client->getRadioSimulation());
        auto headset = std::make_shared<FakeDevice>(client->getRadioSimulation());
        auto speaker = std::make_shared<FakeDevice>(client->getRadioSimulation());
        client->devices(mic, headset, speaker);
        client.reset();
        CHECK(mic->closes == 1 && headset->closes == 1 && speaker->closes == 1);
    }
    {
        InspectableClient client(base, 2);
        auto mic = std::make_shared<FakeDevice>(client.getRadioSimulation());
        auto speaker = std::make_shared<FakeDevice>(client.getRadioSimulation());
        auto checkOldWidth = [&] {
            std::array<float, audio::frameSizeSamples * 2> frame;
            frame.fill(99);
            auto radio = std::const_pointer_cast<afv::RadioSimulation>(client.getRadioSimulation());
            CHECK(radio->getAudioFrame(frame.data(), false) == audio::SourceStatus::OK);
            CHECK(frame[audio::frameSizeSamples] == 99);
        };
        mic->onClose = speaker->onClose = checkOldWidth;
        client.devices(mic, nullptr, speaker);
        deviceStarts.clear();
        client.setSplitAudioChannels(true);
        CHECK(mic->closes == 1 && speaker->closes == 1);
        CHECK(deviceStarts.size() == 2);
        CHECK(deviceStarts[0] == std::make_pair(std::string("afv::microphone"), true));
        CHECK(deviceStarts[1] == std::make_pair(std::string("afv::speaker"), true));
        client.setSplitAudioChannels(true);
        CHECK(deviceStarts.size() == 2);
    }
    {
        InspectableClient client(base, 2);
        auto mic = std::make_shared<FakeDevice>(client.getRadioSimulation());
        std::promise<void> entered, release;
        auto released = release.get_future();
        mic->onClose = [&] { entered.set_value(); released.wait(); };
        client.devices(mic, nullptr, nullptr);
        auto stopped = std::async(std::launch::async, [&] { client.stopAudio(); });
        CHECK(entered.get_future().wait_for(2s) == std::future_status::ready);
        CHECK(stopped.wait_for(50ms) == std::future_status::timeout);
        release.set_value();
        CHECK(stopped.wait_for(2s) == std::future_status::ready);
        stopped.get();
        CHECK(!client.getMicrophoneDevice());
        client.stopAudio();
        CHECK(mic->closes == 1);
    }
    event_base_free(base);
}

void testUdpSendClose()
{
    // Only loopback is used, and the event loop is never dispatched.
    evutil_socket_t receiver = ::socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(receiver != static_cast<evutil_socket_t>(-1));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    CHECK(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    auto* base = event_base_new();
    CHECK(base);
    {
        cryptodto::UDPChannel channel(base);
        channel.setAddress("127.0.0.1:" + std::to_string(ntohs(address.sin_port)));
        cryptodto::dto::ChannelConfig config;
        config.ChannelTag = "offline";
        channel.setChannelConfig(config);
        CHECK(channel.open());
        afv::dto::Heartbeat packet("OFFLINE");
        std::atomic<bool> done{false};
        std::atomic<int> sent{0};
        std::thread sender([&] {
            while (!done) { channel.sendDto(packet); ++sent; }
        });
        while (sent < 20) std::this_thread::yield();
        for (int i = 0; i < 100; ++i) {
            channel.close();
            CHECK(!channel.isOpen());
            config.ChannelTag = "offline-" + std::to_string(i);
            std::fill(std::begin(config.AeadTransmitKey), std::end(config.AeadTransmitKey), i);
            channel.setChannelConfig(config);
            CHECK(channel.open());
        }
        done = true;
        sender.join();
        channel.close();
        channel.close();
    }
    event_base_free(base);
    evutil_closesocket(receiver);
}

int main()
{
#ifdef _WIN32
    WSADATA winsock;
    CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
#endif
    setLogger(nullptr, nullptr);
    testLoggerDrain(false);
    testLoggerDrain(true);
    testLoggerReplacement();
    testEncoder();
    testStream();
    testClient();
    testUdpSendClose();
#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "native lifecycle tests passed\n";
}
