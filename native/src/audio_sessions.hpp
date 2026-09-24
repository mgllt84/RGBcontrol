#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AudioSessionInfo {
    std::uint32_t processId = 0;
    std::wstring executable;
    std::wstring displayName;
    float volume = 1.0f;
    bool muted = false;
    bool active = false;
    bool systemSounds = false;
};

struct AudioEndpointInfo {
    float volume = 1.0f;
    bool muted = false;
};

bool enumerateAudioSessions(std::vector<AudioSessionInfo>& sessions,
                            AudioEndpointInfo& endpoint,
                            std::wstring& error);
bool setAudioSessionVolume(std::uint32_t processId, float volume, std::wstring& error);
bool setAudioSessionMute(std::uint32_t processId, bool muted, std::wstring& error);
bool setMasterAudioVolume(float volume, std::wstring& error);
bool setMasterAudioMute(bool muted, std::wstring& error);
