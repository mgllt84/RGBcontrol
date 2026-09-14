#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include "audio_loopback.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <complex>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kFftSize = 2048;

template <typename T>
void releaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring hresultText(const wchar_t* operation, HRESULT result) {
    std::wostringstream value;
    value << operation << L" (0x" << std::hex << std::uppercase << static_cast<unsigned long>(result) << L")";
    return value.str();
}

void fft(std::vector<std::complex<double>>& values) {
    const std::size_t count = values.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        std::size_t bit = count >> 1;
        while (reversed & bit) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed) std::swap(values[index], values[reversed]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < count; offset += length) {
            std::complex<double> rotation(1.0, 0.0);
            for (std::size_t index = 0; index < length / 2; ++index) {
                const std::complex<double> even = values[offset + index];
                const std::complex<double> odd = values[offset + index + length / 2] * rotation;
                values[offset + index] = even + odd;
                values[offset + index + length / 2] = even - odd;
                rotation *= step;
            }
        }
    }
}

double dbLevel(double amplitude, double floorDb = -58.0) {
    const double decibels = 20.0 * std::log10(std::max(amplitude, 1e-8));
    return std::clamp((decibels - floorDb) / -floorDb, 0.0, 1.0);
}

double smoothLevel(double previous, double target) {
    const double factor = target > previous ? 0.58 : 0.13;
    return previous + (target - previous) * factor;
}
} // namespace

struct LoopbackAudioAnalyzer::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    bool comInitialized = false;
    bool started = false;
    bool floatingPoint = false;
    WORD channels = 0;
    WORD bitsPerSample = 0;
    WORD blockAlign = 0;
    DWORD sampleRate = 0;
    std::vector<float> mono;
    AudioBands smoothed;

    ~Impl() {
        if (client && started) client->Stop();
        releaseCom(capture);
        releaseCom(client);
        releaseCom(device);
        releaseCom(enumerator);
        if (comInitialized) CoUninitialize();
    }

    float decode(const BYTE* bytes) const {
        if (floatingPoint && bitsPerSample == 32) {
            float value = 0.0f;
            std::memcpy(&value, bytes, sizeof(value));
            return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
        }
        if (bitsPerSample == 16) {
            std::int16_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value / 32768.0f;
        }
        if (bitsPerSample == 24) {
            std::int32_t value = static_cast<std::int32_t>(bytes[0]) |
                                 (static_cast<std::int32_t>(bytes[1]) << 8) |
                                 (static_cast<std::int32_t>(bytes[2]) << 16);
            if (value & 0x00800000) value |= static_cast<std::int32_t>(0xFF000000);
            return value / 8388608.0f;
        }
        if (bitsPerSample == 32) {
            std::int32_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return static_cast<float>(value / 2147483648.0);
        }
        return 0.0f;
    }

    AudioBands analyze() {
        AudioBands target;
        if (mono.empty() || sampleRate == 0) return target;

        const std::size_t rmsCount = std::min<std::size_t>(mono.size(), kFftSize);
        double squares = 0.0;
        for (std::size_t index = mono.size() - rmsCount; index < mono.size(); ++index) {
            squares += static_cast<double>(mono[index]) * mono[index];
        }
        target.volume = dbLevel(std::sqrt(squares / std::max<std::size_t>(1, rmsCount)), -55.0);

        if (mono.size() >= kFftSize) {
            std::vector<std::complex<double>> spectrum(kFftSize);
            const std::size_t start = mono.size() - kFftSize;
            for (std::size_t index = 0; index < kFftSize; ++index) {
                const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * index / (kFftSize - 1));
                spectrum[index] = std::complex<double>(mono[start + index] * window, 0.0);
            }
            fft(spectrum);
            auto band = [&](double fromHz, double toHz, double boost) {
                const std::size_t first = std::max<std::size_t>(1, static_cast<std::size_t>(fromHz * kFftSize / sampleRate));
                const std::size_t last = std::min<std::size_t>(kFftSize / 2 - 1, static_cast<std::size_t>(toHz * kFftSize / sampleRate));
                double peak = 0.0;
                double energy = 0.0;
                std::size_t bins = 0;
                for (std::size_t bin = first; bin <= last; ++bin) {
                    const double magnitude = std::abs(spectrum[bin]) / (kFftSize * 0.25);
                    peak = std::max(peak, magnitude);
                    energy += magnitude * magnitude;
                    ++bins;
                }
                const double average = bins ? std::sqrt(energy / bins) : 0.0;
                return dbLevel(std::max(peak * 0.70, average * 2.2) * boost);
            };
            target.bass = band(38.0, 250.0, 1.22);
            target.mid = band(250.0, 2200.0, 1.18);
            target.treble = band(2200.0, std::min(15000.0, sampleRate * 0.46), 1.42);
        } else {
            target.bass = target.mid = target.treble = target.volume;
        }
        target.signal = target.volume > 0.025;
        return target;
    }
};

LoopbackAudioAnalyzer::LoopbackAudioAnalyzer() : impl_(std::make_unique<Impl>()) {}
LoopbackAudioAnalyzer::~LoopbackAudioAnalyzer() = default;

bool LoopbackAudioAnalyzer::start(std::wstring& error) {
    error.clear();
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        error = hresultText(L"CoInitializeEx", result);
        return false;
    }
    impl_->comInitialized = SUCCEEDED(result);

    result = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                              reinterpret_cast<void**>(&impl_->enumerator));
    if (FAILED(result)) { error = hresultText(L"MMDeviceEnumerator", result); return false; }
    result = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
    if (FAILED(result)) { error = hresultText(L"DefaultAudioEndpoint", result); return false; }
    result = impl_->device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&impl_->client));
    if (FAILED(result)) { error = hresultText(L"IAudioClient", result); return false; }

    WAVEFORMATEX* format = nullptr;
    result = impl_->client->GetMixFormat(&format);
    if (FAILED(result) || !format) { error = hresultText(L"GetMixFormat", result); return false; }
    impl_->channels = format->nChannels;
    impl_->bitsPerSample = format->wBitsPerSample;
    impl_->blockAlign = format->nBlockAlign;
    impl_->sampleRate = format->nSamplesPerSec;
    impl_->floatingPoint = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        impl_->floatingPoint = extensible->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    const bool supported = impl_->channels > 0 && impl_->blockAlign > 0 &&
                           (impl_->bitsPerSample == 16 || impl_->bitsPerSample == 24 || impl_->bitsPerSample == 32);
    if (!supported) {
        CoTaskMemFree(format);
        error = L"Unsupported Windows audio format";
        return false;
    }

    result = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       1000000, 0, format, nullptr);
    CoTaskMemFree(format);
    if (FAILED(result)) { error = hresultText(L"AudioClient.Initialize", result); return false; }
    result = impl_->client->GetService(IID_IAudioCaptureClient, reinterpret_cast<void**>(&impl_->capture));
    if (FAILED(result)) { error = hresultText(L"IAudioCaptureClient", result); return false; }
    result = impl_->client->Start();
    if (FAILED(result)) { error = hresultText(L"AudioClient.Start", result); return false; }
    impl_->started = true;
    impl_->mono.reserve(kFftSize * 2);
    return true;
}

bool LoopbackAudioAnalyzer::sample(AudioBands& bands, std::wstring& error) {
    error.clear();
    if (!impl_->capture || !impl_->started) {
        error = L"Audio analyzer is not started";
        return false;
    }
    UINT32 packetFrames = 0;
    HRESULT result = impl_->capture->GetNextPacketSize(&packetFrames);
    if (FAILED(result)) { error = hresultText(L"GetNextPacketSize", result); return false; }
    while (packetFrames > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        result = impl_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(result)) { error = hresultText(L"AudioCapture.GetBuffer", result); return false; }
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
        const int bytesPerSample = impl_->bitsPerSample / 8;
        for (UINT32 frame = 0; frame < frames; ++frame) {
            float mono = 0.0f;
            if (!silent) {
                const BYTE* frameBytes = data + static_cast<std::size_t>(frame) * impl_->blockAlign;
                for (WORD channel = 0; channel < impl_->channels; ++channel) {
                    mono += impl_->decode(frameBytes + static_cast<std::size_t>(channel) * bytesPerSample);
                }
                mono /= impl_->channels;
            }
            impl_->mono.push_back(mono);
        }
        impl_->capture->ReleaseBuffer(frames);
        result = impl_->capture->GetNextPacketSize(&packetFrames);
        if (FAILED(result)) { error = hresultText(L"GetNextPacketSize", result); return false; }
    }
    if (impl_->mono.size() > kFftSize * 3) {
        impl_->mono.erase(impl_->mono.begin(), impl_->mono.end() - static_cast<std::ptrdiff_t>(kFftSize * 2));
    }

    AudioBands target = impl_->analyze();
    impl_->smoothed.volume = smoothLevel(impl_->smoothed.volume, target.volume);
    impl_->smoothed.bass = smoothLevel(impl_->smoothed.bass, target.bass);
    impl_->smoothed.mid = smoothLevel(impl_->smoothed.mid, target.mid);
    impl_->smoothed.treble = smoothLevel(impl_->smoothed.treble, target.treble);
    impl_->smoothed.signal = target.signal || impl_->smoothed.volume > 0.035;
    bands = impl_->smoothed;
    return true;
}
