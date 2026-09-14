#pragma once

#include "device_provider.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct RgbZone {
    std::wstring name;
    std::uint32_t type = 0;
    std::uint32_t minimumLeds = 0;
    std::uint32_t maximumLeds = 0;
    std::uint32_t leds = 0;
    std::uint32_t flags = 0;
};

enum class RgbFrameTransport : int { Automatic = 0, AtomicDevice = 1, PerZone = 2 };
enum class RgbColorOrder : int { RGB = 0, RBG = 1, GRB = 2, GBR = 3, BRG = 4, BGR = 5 };

struct RgbDevice {
    int index = 0;
    std::wstring name;
    std::wstring vendor;
    std::wstring description;
    std::wstring type;
    int zones = 0;
    int leds = 1;
    bool selected = true;
    std::wstring providerId = L"openrgb";
    DeviceCapability capabilities = DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::Effects;
    // Exact mode names advertised by this controller.  OpenRGB devices do not
    // all implement Static and Direct, so control decisions must be per-device.
    std::vector<std::wstring> modes;
    // Complete zone geometry is needed for controllers (notably motherboards)
    // whose device-wide color vector does not represent every physical output.
    std::vector<RgbZone> zoneDetails;
    // Learned locally by the guided hardware calibration. Defaults keep the
    // controller-specific safe heuristic until the user validates a profile.
    RgbFrameTransport frameTransport = RgbFrameTransport::Automatic;
    RgbColorOrder colorOrder = RgbColorOrder::RGB;
    int frameIntervalMs = 40;
    int preferredLeds = 120;
    // Optional auxiliary monochrome LED mask exposed by process plugins.
    // -1 means unsupported; DualSense uses bits 0..4 for player indicators.
    int auxiliaryLedMask = -1;
};

class OpenRgbClient {
public:
    static std::uint32_t gradientColor(double phaseDegrees, double position,
                                       std::uint32_t baseRgb, double intensity);
    static std::uint32_t orderedColor(std::uint32_t rgb, RgbColorOrder order);
    static bool prefersAtomicFrames(const RgbDevice& device);
    std::vector<RgbDevice> scan(bool rescan = false) const;
    int initializeEmptyZones(std::vector<RgbDevice>& devices, int preferredLeds = 120) const;
    void sendEffectFrame(const std::vector<RgbDevice>& devices,
                         const std::wstring& mode,
                         double phase,
                         std::uint32_t baseRgb,
                         double intensity,
                         double audioBass = 0.0,
                         double audioMid = 0.0,
                         double audioTreble = 0.0,
                         double audioVolume = 0.0,
                         std::array<std::uint32_t, 4> ambientColors = {},
                         std::uint32_t ambientAverage = 0) const;
    bool canConnect(int timeoutMs = 500) const;
};
