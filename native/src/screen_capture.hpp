#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ScreenMonitorInfo {
    std::wstring deviceName;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
};

struct AmbientFrame {
    std::array<std::uint32_t, 4> zones{0, 0, 0, 0}; // gauche, haut, droite, bas
    std::uint32_t average = 0;
    double fps = 0.0;
    bool valid = false;
};

class AmbientScreenCapture {
public:
    AmbientScreenCapture();
    ~AmbientScreenCapture();
    AmbientScreenCapture(const AmbientScreenCapture&) = delete;
    AmbientScreenCapture& operator=(const AmbientScreenCapture&) = delete;

    static std::vector<ScreenMonitorInfo> enumerateMonitors();
    bool start(int monitorIndex, int sampleWidth, int sampleHeight, std::wstring& error);
    bool sample(AmbientFrame& frame, int saturation, int reactivity, bool useZones, std::wstring& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
