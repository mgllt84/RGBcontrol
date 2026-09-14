#pragma once

#include <cstdint>
#include <string>

// Stable capability vocabulary shared by the device centre and future provider
// adapters. A provider only advertises operations it can safely perform.
enum class DeviceCapability : std::uint32_t {
    None = 0,
    Discovery = 1u << 0,
    Lighting = 1u << 1,
    Effects = 1u << 2,
    Zones = 1u << 3,
    PerLed = 1u << 4,
    Sensors = 1u << 5,
    FanRead = 1u << 6,
    FanControl = 1u << 7,
    HotPlug = 1u << 8,
    LocalOnly = 1u << 9
};

constexpr DeviceCapability operator|(DeviceCapability left, DeviceCapability right) {
    return static_cast<DeviceCapability>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr DeviceCapability& operator|=(DeviceCapability& left, DeviceCapability right) {
    left = left | right;
    return left;
}

constexpr bool hasCapability(DeviceCapability value, DeviceCapability capability) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(capability)) != 0;
}

struct DeviceProviderDescriptor {
    std::wstring id;
    std::wstring name;
    std::wstring transport;
    DeviceCapability capabilities = DeviceCapability::Discovery;
    bool available = false;
    bool limited = false;
};

// API v2 adds verified, out-of-process device plugins.  It remains deliberately
// transport-neutral so HID, raw USB, serial and network adapters share the
// same capability contract.
constexpr std::uint32_t kDeviceProviderApiVersion = 2;
