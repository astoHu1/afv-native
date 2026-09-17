#define MINIAUDIO_IMPLEMENTATION
#include "MiniAudioAudioDevice.h"

#include <algorithm>
#include <memory>
#include <cstring>

using namespace afv_native::audio;
using namespace std;

void logger(void* pUserData, ma_uint32 logLevel, const char* message)
{
    (void)pUserData;
    std::string msg(message);
    msg.erase(std::remove(msg.begin(), msg.end(), '\n'), msg.cend());
    LOG("MiniAudioAudioDevice", "%s: %s", ma_log_level_to_string(logLevel), msg.c_str());
}

MiniAudioAudioDevice::MiniAudioAudioDevice(
        const std::string& userStreamName,
        const std::string& deviceName,
        AudioDevice::Api audioApi,
        bool splitChannels) :
    AudioDevice(),
    mUserStreamName(userStreamName),
    mDeviceName(deviceName),
    mContextInitialized(false),
    mDeviceInitialized(false),
    mSplitChannels(splitChannels)
{
}

bool MiniAudioAudioDevice::initContext()
{
    if (mContextInitialized)
        return true;

    ma_context_config contextConfig = ma_context_config_init();
    contextConfig.threadPriority = ma_thread_priority_normal;
    contextConfig.jack.pClientName = "xpilot";
    contextConfig.pulse.pApplicationName = "xpilot";
    ma_result result = ma_context_init(NULL, 0, &contextConfig, &context);
    if(result == MA_SUCCESS) {
        mContextInitialized = true;
        ma_log_register_callback(ma_context_get_log(&context), ma_log_callback_init(logger, NULL));
        LOG("MiniAudioAudioDevice", "Context initialized. Audio Backend: %s", ma_get_backend_name(context.backend));
    }
    else {
        LOG("MiniAudioAudioDevice", "Error initializing context: %s", ma_result_description(result));
    }
    return mContextInitialized;
}

MiniAudioAudioDevice::~MiniAudioAudioDevice()
{
    close();
}

bool MiniAudioAudioDevice::openOutput()
{
    std::lock_guard<std::mutex> lifecycleGuard(mLifecycleLock);
    return initDevice(ma_device_type_playback);
}

bool MiniAudioAudioDevice::openInput()
{
    std::lock_guard<std::mutex> lifecycleGuard(mLifecycleLock);
    return initDevice(ma_device_type_capture);
}

void MiniAudioAudioDevice::close()
{
    std::lock_guard<std::mutex> lifecycleGuard(mLifecycleLock);
    closeDevice();
    if (mContextInitialized) {
        ma_context_uninit(&context);
        mContextInitialized = false;
    }
}

void MiniAudioAudioDevice::closeDevice()
{
    if (mDeviceInitialized) {
        // These calls wait for callbacks. Never hold a source/sink lock here.
        ma_device_stop(&audioDevice);
        ma_device_uninit(&audioDevice);
        mDeviceInitialized = false;
    }
    {
        std::lock_guard<std::mutex> sourceGuard(mSourcePtrLock);
        mFrameAdapter.resetPlayback();
    }
    {
        std::lock_guard<std::mutex> sinkGuard(mSinkPtrLock);
        mFrameAdapter.resetCapture();
    }
}

void MiniAudioAudioDevice::setSource(std::shared_ptr<ISampleSource> newSrc)
{
    // Match the callback lock so return also fences calls to the old source.
    // Release the old shared_ptr on this control thread, outside the lock.
    std::lock_guard<std::mutex> sourceGuard(mSourcePtrLock);
    mSource.swap(newSrc);
    mFrameAdapter.resetPlayback();
}

void MiniAudioAudioDevice::setSink(std::shared_ptr<ISampleSink> newSink)
{
    std::lock_guard<std::mutex> sinkGuard(mSinkPtrLock);
    mSink.swap(newSink);
    mFrameAdapter.resetCapture();
}

static std::map<int, ma_device_info> cachedInputDevices;
std::map<int, ma_device_info> MiniAudioAudioDevice::getCompatibleInputDevices()
{
    static std::mutex cacheLock;
    std::lock_guard<std::mutex> cacheGuard(cacheLock);
    std::map<int, ma_device_info> deviceList;

    ma_device_info* devices;
    ma_uint32 deviceCount;
    ma_context maContext{};

    ma_result result = ma_context_init(NULL, 0, NULL, &maContext);
    if(result == MA_SUCCESS) {
        result = ma_context_get_devices(&maContext, NULL, NULL, &devices, &deviceCount);
        if(result == MA_SUCCESS) {
            // Check if there's a change in the available devices
            bool devicesChanged = false;
            if (cachedInputDevices.size() != deviceCount) {
                devicesChanged = true;
            } else {
                for (ma_uint32 i = 0; i < deviceCount; i++) {
                    if (cachedInputDevices.find(i) == cachedInputDevices.end() ||
                        memcmp(&cachedInputDevices[i].id, &devices[i].id, sizeof(ma_device_id)) != 0) {
                        devicesChanged = true;
                        break;
                    }
                }
            }

            if (devicesChanged) {
                LOG("MiniAudioAudioDevice", "Input devices have changed: %d devices found.", deviceCount);
                for(ma_uint32 i = 0; i < deviceCount; i++) {
                    deviceList.emplace(i, devices[i]);

                    // log detailed device info
                    {
                        ma_device_info detailedDeviceInfo;
                        result = ma_context_get_device_info(&maContext, ma_device_type_capture, &devices[i].id, &detailedDeviceInfo);
                        if(result == MA_SUCCESS)
                        {
                            LOG("MiniAudioAudioDevice", "Input: %s (Default: %s, Format Count: %d)",
                                devices[i].name,
                                detailedDeviceInfo.isDefault ? "Yes" : "No",
                                detailedDeviceInfo.nativeDataFormatCount);

                            ma_uint32 iFormat;
                            for(iFormat = 0; iFormat < detailedDeviceInfo.nativeDataFormatCount; ++iFormat)
                            {
                                LOG("MiniAudioAudioDevice", "   --> Format: %s, Channels: %d, Sample Rate: %d",
                                    ma_get_format_name(detailedDeviceInfo.nativeDataFormats[iFormat].format),
                                    detailedDeviceInfo.nativeDataFormats[iFormat].channels,
                                    detailedDeviceInfo.nativeDataFormats[iFormat].sampleRate);
                            }
                        }
                        else {
                            LOG("MiniAudioAudioDevice", "Error getting input device info: %s", ma_result_description(result));
                        }
                    }
                }

                // Update the cache
                cachedInputDevices = deviceList;
            } else {
                deviceList = cachedInputDevices;
            }
        }
        else {
            LOG("MiniAudioAudioDevice", "Error querying input devices: %s", ma_result_description(result));
        }
        ma_context_uninit(&maContext);
    }
    else {
        LOG("MiniAudioAudioDevice", "Error initializing input device context: %s", ma_result_description(result));
    }

    return deviceList;
}

static std::map<int, ma_device_info> cachedOutputDevices;
std::map<int, ma_device_info> MiniAudioAudioDevice::getCompatibleOutputDevices()
{
    static std::mutex cacheLock;
    std::lock_guard<std::mutex> cacheGuard(cacheLock);
    std::map<int, ma_device_info> deviceList;

    ma_device_info* devices;
    ma_uint32 deviceCount;
    ma_context maContext{};

    ma_result result = ma_context_init(NULL, 0, NULL, &maContext);
    if(result == MA_SUCCESS) {
        result = ma_context_get_devices(&maContext, &devices, &deviceCount, NULL, NULL);
        if(result == MA_SUCCESS) {
            // Check if there's a change in the available devices
            bool devicesChanged = false;
            if (cachedOutputDevices.size() != deviceCount) {
                devicesChanged = true;
            } else {
                for (ma_uint32 i = 0; i < deviceCount; i++) {
                    if (cachedOutputDevices.find(i) == cachedOutputDevices.end() ||
                        memcmp(&cachedOutputDevices[i].id, &devices[i].id, sizeof(ma_device_id)) != 0) {
                        devicesChanged = true;
                        break;
                    }
                }
            }

            if (devicesChanged) {
                LOG("MiniAudioAudioDevice", "Output devices have changed: %d devices found.", deviceCount);
                for(ma_uint32 i = 0; i < deviceCount; i++) {
                    deviceList.emplace(i, devices[i]);

                    // log detailed device info
                    {
                        ma_device_info detailedDeviceInfo;
                        result = ma_context_get_device_info(&maContext, ma_device_type_playback, &devices[i].id, &detailedDeviceInfo);
                        if(result == MA_SUCCESS)
                        {
                            LOG("MiniAudioAudioDevice", "Output: %s (Default: %s, Format Count: %d)",
                                devices[i].name,
                                detailedDeviceInfo.isDefault ? "Yes" : "No",
                                detailedDeviceInfo.nativeDataFormatCount);

                            ma_uint32 iFormat;
                            for(iFormat = 0; iFormat < detailedDeviceInfo.nativeDataFormatCount; ++iFormat)
                            {
                                LOG("MiniAudioAudioDevice", "   --> Format: %s, Channels: %d, Sample Rate: %d",
                                    ma_get_format_name(detailedDeviceInfo.nativeDataFormats[iFormat].format),
                                    detailedDeviceInfo.nativeDataFormats[iFormat].channels,
                                    detailedDeviceInfo.nativeDataFormats[iFormat].sampleRate);
                            }
                        }
                        else {
                            LOG("MiniAudioAudioDevice", "Error getting output device info: %s", ma_result_description(result));
                        }
                    }
                }

                // Update the cache
                cachedOutputDevices = deviceList;
            } else {
                deviceList = cachedOutputDevices;
            }
        }
        else {
            LOG("MiniAudioAudioDevice", "Error querying output devices: %s", ma_result_description(result));
        }
        ma_context_uninit(&maContext);
    }
    else {
        LOG("MiniAudioAudioDevice", "Error initializing output device context: %s", ma_result_description(result));
    }

    return deviceList;
}

bool MiniAudioAudioDevice::initDevice(ma_device_type type)
{
    if (mDeviceInitialized && audioDevice.type == type)
        return true;

    closeDevice();

    if(mDeviceName.empty()) {
        LOG("MiniAudioAudioDevice", "Device name is empty");
        return false; // bail early if the device name is empty
    }

    if (!initContext())
        return false;

    ma_device_id deviceId;
    const bool forInput = type == ma_device_type_capture;
    if(!getDeviceForName(mDeviceName, forInput, deviceId)) {
        LOG("MiniAudioAudioDevice", "No device found for %s", mDeviceName.c_str());
        return false; // no device found
    }

    ma_device_config cfg = ma_device_config_init(type);
    if (forInput) {
        cfg.capture.pDeviceID = &deviceId;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 1;
        cfg.capture.shareMode = ma_share_mode_shared;
    } else {
        cfg.playback.pDeviceID = &deviceId;
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = mSplitChannels ? 2 : 1;
        cfg.playback.shareMode = ma_share_mode_shared;
    }
    cfg.sampleRate = sampleRateHz;
    cfg.periodSizeInFrames = frameSizeSamples;
    cfg.pUserData = this;
    cfg.dataCallback = forInput ? maInputCallback : maOutputCallback;

    ma_result result;

    result = ma_device_init(&context, &cfg, &audioDevice);
    if(result != MA_SUCCESS) {
        LOG("MiniAudioAudioDevice", "Error initializing device: %s", ma_result_description(result));
        return false;
    }
    // Own the initialized device even if start fails; closeDevice must uninit it.
    mDeviceInitialized = true;

    result = ma_device_start(&audioDevice);
    if(result != MA_SUCCESS) {
        LOG("MiniAudioAudioDevice", "Error starting device: %s", ma_result_description(result));
        closeDevice();
        return false;
    }

    return true;
}

bool MiniAudioAudioDevice::getDeviceForName(const std::string &deviceName, bool forInput, ma_device_id &deviceId)
{
    auto allDevices = forInput ? getCompatibleInputDevices() : getCompatibleOutputDevices();

    if(!allDevices.empty()) {
        for(const auto& devicePair : allDevices) {
            if(devicePair.second.name == deviceName) {
                deviceId = devicePair.second.id;
                return true;
            }
        }
    }

    return false;
}

int MiniAudioAudioDevice::outputCallback(void *outputBuffer, unsigned int nFrames)
{
    std::lock_guard<std::mutex> sourceGuard(mSourcePtrLock);
    mFrameAdapter.playback(static_cast<float*>(outputBuffer), nFrames,
                           audioDevice.playback.channels, mSource.get());
    return 0;
}

int MiniAudioAudioDevice::inputCallback(const void *inputBuffer, unsigned int nFrames)
{
    std::lock_guard<std::mutex> sinkGuard(mSinkPtrLock);
    mFrameAdapter.capture(static_cast<const float*>(inputBuffer), nFrames,
                          audioDevice.capture.channels, mSink.get());
    return 0;
}

void MiniAudioAudioDevice::maOutputCallback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    auto device = reinterpret_cast<MiniAudioAudioDevice*>(pDevice->pUserData);
    device->outputCallback(pOutput, frameCount);
}

void MiniAudioAudioDevice::maInputCallback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    auto device = reinterpret_cast<MiniAudioAudioDevice*>(pDevice->pUserData);
    device->inputCallback(pInput, frameCount);
}

/* ========== Factory hooks ============= */

map<AudioDevice::Api, std::string> AudioDevice::getAPIs() {
    return {};
}

map<int, AudioDevice::DeviceInfo> AudioDevice::getCompatibleInputDevicesForApi(AudioDevice::Api api) {
    auto allDevices = MiniAudioAudioDevice::getCompatibleInputDevices();
    map<int, AudioDevice::DeviceInfo> returnDevices;
    for (const auto &p: allDevices) {
        returnDevices.emplace(p.first, AudioDevice::DeviceInfo(p.second.name));
    }
    return returnDevices;
}

map<int, AudioDevice::DeviceInfo> AudioDevice::getCompatibleOutputDevicesForApi(AudioDevice::Api api) {
    auto allDevices = MiniAudioAudioDevice::getCompatibleOutputDevices();
    map<int, AudioDevice::DeviceInfo> returnDevices;
    for (const auto &p: allDevices) {
        returnDevices.emplace(p.first, AudioDevice::DeviceInfo(p.second.name));
    }
    return returnDevices;
}

std::shared_ptr<AudioDevice>
AudioDevice::makeDevice(
        const std::string &userStreamName,
        const std::string &deviceName,
        AudioDevice::Api audioApi,
        bool splitChannels) {
    auto devsp = std::make_shared<MiniAudioAudioDevice>(userStreamName, deviceName, audioApi, splitChannels);
    return devsp;
}
