#pragma once

#include "openrgb.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// External RGBCcontrol drivers are deliberately processes rather than DLLs.
// A crashing vendor adapter therefore cannot corrupt the UI process.  Every
// executable is pinned by SHA-256 in its manifest before it can be started.
struct PluginProviderStatus {
    std::wstring id;
    std::wstring name;
    std::wstring publisher;
    std::wstring transport;
    std::wstring state;
    DeviceCapability capabilities = DeviceCapability::Discovery;
    int matchedDevices = 0;
    bool valid = false;
};

struct PluginScanResult {
    std::vector<RgbDevice> devices;
    std::vector<PluginProviderStatus> providers;
    std::vector<std::wstring> supersededOpenRgbNames;
    int rejectedPlugins = 0;
};

struct PluginCommandResult {
    bool success = false;
    bool timedOut = false;
    std::wstring message;
};

class PluginEngine {
public:
    void setRoot(std::filesystem::path root);
    PluginScanResult scan();

    PluginCommandResult applyStatic(const RgbDevice& device, std::uint32_t rgb, int brightness);
    PluginCommandResult applyEffect(const RgbDevice& device, const std::wstring& mode,
                                    std::uint32_t rgb, int brightness, int speed, int intensity);
    PluginCommandResult stop(const RgbDevice& device);

    static bool isPluginDevice(const RgbDevice& device);

private:
    struct Manifest {
        std::wstring id;
        std::wstring name;
        std::wstring publisher;
        std::wstring transport;
        std::wstring executableName;
        std::filesystem::path executablePath;
        std::filesystem::path manifestPath;
        std::wstring sha256;
        std::wstring deviceName;
        std::wstring deviceType;
        std::wstring modes;
        std::wstring supersedesOpenRgb;
        DeviceCapability capabilities = DeviceCapability::Discovery;
        std::uint16_t vendorId = 0;
        std::uint16_t productId = 0;
        int zones = 1;
        int leds = 1;
        int timeoutMs = 5000;
        bool valid = false;
        std::wstring error;
    };

    std::filesystem::path root_;
    std::vector<Manifest> manifests_;
    std::mutex mutex_;

    void reloadLocked(PluginScanResult& result);
    PluginCommandResult invokeLocked(const Manifest& manifest, const std::wstring& arguments) const;
    const Manifest* manifestForLocked(const RgbDevice& device) const;
};
