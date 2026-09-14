#include "../src/plugin_engine.hpp"

#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 10;
    PluginEngine engine;
    engine.setRoot(std::filesystem::path(argv[1]));
    const PluginScanResult scan = engine.scan();
    if (scan.providers.size() != 1 || !scan.providers.front().valid || scan.rejectedPlugins != 0) return 11;

    RgbDevice device;
    device.providerId = L"plugin:org.rgbccontrol.protocol-test";
    device.description = L"RGBCCONTROL\\PROTOCOL_TEST";
    device.capabilities = DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::Effects;
    if (!engine.applyStatic(device, 0x12abef, 73).success) return 12;
    if (!engine.applyEffect(device, L"Gradient", 0x654321, 80, 55, 90).success) return 13;
    if (!engine.stop(device).success) return 14;
    return 0;
}
