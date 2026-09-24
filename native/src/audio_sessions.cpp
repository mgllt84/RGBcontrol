#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

#include "audio_sessions.hpp"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <map>
#include <utility>

namespace {
template <typename T>
void releaseCom(T*& value) {
    if (value) value->Release();
    value = nullptr;
}

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
    bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

std::wstring hrText(const wchar_t* prefix, HRESULT value) {
    wchar_t code[32]{};
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(value));
    return std::wstring(prefix) + L" (" + code + L")";
}

std::wstring processExecutable(DWORD processId) {
    if (processId == 0) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    const bool found = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!found || size == 0) return {};
    std::wstring full(path, size);
    const std::size_t separator = full.find_last_of(L"\\/");
    return separator == std::wstring::npos ? full : full.substr(separator + 1);
}

std::wstring friendlyName(const std::wstring& executable) {
    if (executable.empty()) return {};
    std::wstring name = executable;
    const std::size_t extension = name.find_last_of(L'.');
    if (extension != std::wstring::npos) name.resize(extension);
    if (!name.empty()) name[0] = static_cast<wchar_t>(std::towupper(name[0]));
    return name;
}

bool openDefaultEndpoint(IMMDeviceEnumerator*& enumerator, IMMDevice*& device, std::wstring& error) {
    HRESULT result = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                      IID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
    if (FAILED(result)) {
        error = hrText(L"Le service audio Windows est inaccessible", result);
        return false;
    }
    result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(result)) {
        error = hrText(L"Aucune sortie audio Windows active", result);
        return false;
    }
    return true;
}

template <typename Callback>
bool forEachSessionVolume(std::uint32_t processId, Callback callback, std::wstring& error) {
    ComScope com;
    if (!com.ready()) {
        error = hrText(L"Initialisation audio impossible", com.result);
        return false;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    if (!openDefaultEndpoint(enumerator, device, error)) {
        releaseCom(device);
        releaseCom(enumerator);
        return false;
    }
    IAudioSessionManager2* manager = nullptr;
    HRESULT result = device->Activate(IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&manager));
    IAudioSessionEnumerator* sessions = nullptr;
    if (SUCCEEDED(result)) result = manager->GetSessionEnumerator(&sessions);
    int count = 0;
    if (SUCCEEDED(result)) result = sessions->GetCount(&count);
    bool matched = false;
    for (int index = 0; SUCCEEDED(result) && index < count; ++index) {
        IAudioSessionControl* control = nullptr;
        IAudioSessionControl2* control2 = nullptr;
        ISimpleAudioVolume* volume = nullptr;
        if (SUCCEEDED(sessions->GetSession(index, &control)) &&
            SUCCEEDED(control->QueryInterface(IID_IAudioSessionControl2, reinterpret_cast<void**>(&control2)))) {
            DWORD pid = 0;
            control2->GetProcessId(&pid);
            if (pid == processId &&
                SUCCEEDED(control->QueryInterface(IID_ISimpleAudioVolume, reinterpret_cast<void**>(&volume)))) {
                matched = true;
                const HRESULT applied = callback(volume);
                if (FAILED(applied) && SUCCEEDED(result)) result = applied;
            }
        }
        releaseCom(volume);
        releaseCom(control2);
        releaseCom(control);
    }
    releaseCom(sessions);
    releaseCom(manager);
    releaseCom(device);
    releaseCom(enumerator);
    if (FAILED(result)) {
        error = hrText(L"Windows a refusé le réglage audio", result);
        return false;
    }
    if (!matched) {
        error = L"Cette application ne possède plus de session audio active.";
        return false;
    }
    error.clear();
    return true;
}

template <typename Callback>
bool withEndpointVolume(Callback callback, std::wstring& error) {
    ComScope com;
    if (!com.ready()) {
        error = hrText(L"Initialisation audio impossible", com.result);
        return false;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    if (!openDefaultEndpoint(enumerator, device, error)) {
        releaseCom(device);
        releaseCom(enumerator);
        return false;
    }
    IAudioEndpointVolume* volume = nullptr;
    HRESULT result = device->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&volume));
    if (SUCCEEDED(result)) result = callback(volume);
    releaseCom(volume);
    releaseCom(device);
    releaseCom(enumerator);
    if (FAILED(result)) {
        error = hrText(L"Windows a refusé le volume principal", result);
        return false;
    }
    error.clear();
    return true;
}
} // namespace

bool enumerateAudioSessions(std::vector<AudioSessionInfo>& output,
                            AudioEndpointInfo& endpoint,
                            std::wstring& error) {
    output.clear();
    ComScope com;
    if (!com.ready()) {
        error = hrText(L"Initialisation audio impossible", com.result);
        return false;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    if (!openDefaultEndpoint(enumerator, device, error)) {
        releaseCom(device);
        releaseCom(enumerator);
        return false;
    }

    IAudioEndpointVolume* endpointVolume = nullptr;
    HRESULT result = device->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&endpointVolume));
    if (SUCCEEDED(result)) {
        float scalar = 1.0f;
        BOOL muted = FALSE;
        endpointVolume->GetMasterVolumeLevelScalar(&scalar);
        endpointVolume->GetMute(&muted);
        endpoint.volume = std::clamp(scalar, 0.0f, 1.0f);
        endpoint.muted = muted != FALSE;
    }
    releaseCom(endpointVolume);

    IAudioSessionManager2* manager = nullptr;
    result = device->Activate(IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&manager));
    IAudioSessionEnumerator* sessionEnumerator = nullptr;
    if (SUCCEEDED(result)) result = manager->GetSessionEnumerator(&sessionEnumerator);
    int count = 0;
    if (SUCCEEDED(result)) result = sessionEnumerator->GetCount(&count);

    std::map<std::uint32_t, AudioSessionInfo> grouped;
    for (int index = 0; SUCCEEDED(result) && index < count; ++index) {
        IAudioSessionControl* control = nullptr;
        IAudioSessionControl2* control2 = nullptr;
        ISimpleAudioVolume* volume = nullptr;
        if (FAILED(sessionEnumerator->GetSession(index, &control)) ||
            FAILED(control->QueryInterface(IID_IAudioSessionControl2, reinterpret_cast<void**>(&control2))) ||
            FAILED(control->QueryInterface(IID_ISimpleAudioVolume, reinterpret_cast<void**>(&volume)))) {
            releaseCom(volume);
            releaseCom(control2);
            releaseCom(control);
            continue;
        }

        DWORD processId = 0;
        AudioSessionState state = AudioSessionStateInactive;
        float scalar = 1.0f;
        BOOL muted = FALSE;
        LPWSTR display = nullptr;
        control2->GetProcessId(&processId);
        control->GetState(&state);
        control->GetDisplayName(&display);
        volume->GetMasterVolume(&scalar);
        volume->GetMute(&muted);
        const bool system = control2->IsSystemSoundsSession() == S_OK;

        const std::uint32_t key = system ? 0 : static_cast<std::uint32_t>(processId);
        auto found = grouped.find(key);
        if (found == grouped.end()) {
            AudioSessionInfo info;
            info.processId = key;
            info.systemSounds = system;
            info.executable = system ? L"SystemSounds" : processExecutable(processId);
            info.displayName = display && *display ? display : friendlyName(info.executable);
            if (info.displayName.empty()) info.displayName = system ? L"Sons système" : L"Application audio";
            info.volume = std::clamp(scalar, 0.0f, 1.0f);
            info.muted = muted != FALSE;
            info.active = state == AudioSessionStateActive;
            grouped.emplace(key, std::move(info));
        } else {
            found->second.active = found->second.active || state == AudioSessionStateActive;
            found->second.volume = std::max(found->second.volume, std::clamp(scalar, 0.0f, 1.0f));
            found->second.muted = found->second.muted && muted != FALSE;
        }
        if (display) CoTaskMemFree(display);
        releaseCom(volume);
        releaseCom(control2);
        releaseCom(control);
    }

    releaseCom(sessionEnumerator);
    releaseCom(manager);
    releaseCom(device);
    releaseCom(enumerator);
    if (FAILED(result)) {
        error = hrText(L"Lecture des applications audio impossible", result);
        return false;
    }
    for (auto& entry : grouped) output.push_back(std::move(entry.second));
    std::sort(output.begin(), output.end(), [](const AudioSessionInfo& left, const AudioSessionInfo& right) {
        if (left.active != right.active) return left.active > right.active;
        std::wstring a = left.displayName;
        std::wstring b = right.displayName;
        std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return a < b;
    });
    error.clear();
    return true;
}

bool setAudioSessionVolume(std::uint32_t processId, float volume, std::wstring& error) {
    const float scalar = std::clamp(volume, 0.0f, 1.0f);
    return forEachSessionVolume(processId, [scalar](ISimpleAudioVolume* session) {
        return session->SetMasterVolume(scalar, nullptr);
    }, error);
}

bool setAudioSessionMute(std::uint32_t processId, bool muted, std::wstring& error) {
    return forEachSessionVolume(processId, [muted](ISimpleAudioVolume* session) {
        return session->SetMute(muted ? TRUE : FALSE, nullptr);
    }, error);
}

bool setMasterAudioVolume(float volume, std::wstring& error) {
    const float scalar = std::clamp(volume, 0.0f, 1.0f);
    return withEndpointVolume([scalar](IAudioEndpointVolume* endpoint) {
        return endpoint->SetMasterVolumeLevelScalar(scalar, nullptr);
    }, error);
}

bool setMasterAudioMute(bool muted, std::wstring& error) {
    return withEndpointVolume([muted](IAudioEndpointVolume* endpoint) {
        return endpoint->SetMute(muted ? TRUE : FALSE, nullptr);
    }, error);
}
