#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Sony DualSense lighting transport for RGBCcontrol Plugin API v2.
// The report layout follows the public DualSense HID definitions documented
// by SDL and the Linux hid-playstation driver. Only LED control bits are set:
// rumble, audio, microphone and adaptive-trigger fields remain untouched.

namespace {
constexpr USHORT kSonyVendor = 0x054c;
constexpr USHORT kDualSenseProduct = 0x0ce6;
constexpr USHORT kDualSenseEdgeProduct = 0x0df2;

struct HidTarget {
    HANDLE handle = INVALID_HANDLE_VALUE;
    HIDD_ATTRIBUTES attributes{};
    HIDP_CAPS capabilities{};
    std::wstring instanceId;
    bool bluetooth = false;

    ~HidTarget() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }

    HidTarget() = default;
    HidTarget(const HidTarget&) = delete;
    HidTarget& operator=(const HidTarget&) = delete;
};

std::wstring upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towupper(character));
    });
    return value;
}

std::wstring argumentValue(int argc, wchar_t** argv, const wchar_t* key) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wstring(argv[index]) == key) return argv[index + 1];
    }
    return {};
}

bool supportedProduct(USHORT product) {
    return product == kDualSenseProduct || product == kDualSenseEdgeProduct;
}

bool interfaceInstanceId(HDEVINFO devices, SP_DEVINFO_DATA& info, std::wstring& result) {
    std::vector<wchar_t> buffer(4096);
    if (!SetupDiGetDeviceInstanceIdW(devices, &info, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)) return false;
    result.assign(buffer.data());
    return true;
}

bool openTarget(const std::wstring& requestedInstance, HidTarget& target, std::wstring& error) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devices = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        error = L"HID enumeration unavailable";
        return false;
    }

    const std::wstring wanted = upper(requestedInstance);
    bool foundMatchingNode = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index, &interfaceData)) break;

        DWORD required = 0;
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, &info);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> detailStorage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required, nullptr, &info)) continue;

        std::wstring instance;
        if (!interfaceInstanceId(devices, info, instance)) continue;
        if (!wanted.empty() && upper(instance) != wanted) continue;
        foundMatchingNode = true;

        HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(handle, &attributes) || attributes.VendorID != kSonyVendor ||
            !supportedProduct(attributes.ProductID)) {
            CloseHandle(handle);
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS capabilities{};
        if (!HidD_GetPreparsedData(handle, &preparsed)) {
            CloseHandle(handle);
            continue;
        }
        const NTSTATUS capsStatus = HidP_GetCaps(preparsed, &capabilities);
        HidD_FreePreparsedData(preparsed);
        if (capsStatus != HIDP_STATUS_SUCCESS || capabilities.UsagePage != 0x01 || capabilities.Usage != 0x05) {
            CloseHandle(handle);
            continue;
        }

        target.handle = handle;
        target.attributes = attributes;
        target.capabilities = capabilities;
        target.instanceId = instance;
        const std::wstring normalized = upper(instance);
        target.bluetooth = capabilities.OutputReportByteLength >= 78 ||
                           normalized.find(L"BTH") != std::wstring::npos ||
                           normalized.find(L"VID&0002") != std::wstring::npos;
        SetupDiDestroyDeviceInfoList(devices);
        return true;
    }
    SetupDiDestroyDeviceInfoList(devices);
    error = foundMatchingNode ? L"DualSense HID interface is busy" : L"DualSense gamepad HID interface not found";
    return false;
}

std::uint32_t crc32Byte(std::uint32_t crc, std::uint8_t byte) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    return crc;
}

std::uint32_t bluetoothCrc(const std::vector<std::uint8_t>& report) {
    std::uint32_t crc = 0xffffffffu;
    crc = crc32Byte(crc, 0xa2);
    for (std::size_t index = 0; index + 4 < report.size(); ++index) crc = crc32Byte(crc, report[index]);
    return ~crc;
}

std::vector<std::uint8_t> makeReport(bool bluetooth, std::size_t requestedLength,
                                     std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                                     bool restore, std::uint8_t playerLedMask = 0x1f) {
    const std::size_t minimum = bluetooth ? 78u : 48u;
    std::vector<std::uint8_t> report(std::max(minimum, requestedLength), 0);
    std::size_t effectsOffset = 1;
    if (bluetooth) {
        report.resize(78);
        report[0] = 0x31;
        report[1] = 0x00; // Sequence and tag.
        report[2] = 0x10; // DualSense Bluetooth output magic value.
        effectsOffset = 3;
    } else {
        report[0] = 0x02;
    }

    if (restore) {
        report[effectsOffset + 1] = 0x08; // Release/reset LED control to the controller.
    } else {
        report[effectsOffset + 1] = 0x14; // RGB lightbar + five player indicators.
        report[effectsOffset + 43] = static_cast<std::uint8_t>((playerLedMask & 0x1f) | 0x20);
        report[effectsOffset + 44] = red;
        report[effectsOffset + 45] = green;
        report[effectsOffset + 46] = blue;
    }

    if (bluetooth) {
        const std::uint32_t crc = bluetoothCrc(report);
        const std::size_t offset = report.size() - 4;
        report[offset] = static_cast<std::uint8_t>(crc);
        report[offset + 1] = static_cast<std::uint8_t>(crc >> 8);
        report[offset + 2] = static_cast<std::uint8_t>(crc >> 16);
        report[offset + 3] = static_cast<std::uint8_t>(crc >> 24);
    }
    return report;
}

bool writeReport(HidTarget& target, std::vector<std::uint8_t>& report, std::wstring& error) {
    DWORD written = 0;
    if (WriteFile(target.handle, report.data(), static_cast<DWORD>(report.size()), &written, nullptr) &&
        written == report.size()) return true;
    if (HidD_SetOutputReport(target.handle, report.data(), static_cast<ULONG>(report.size()))) return true;
    error = L"DualSense LED report rejected (Windows error " + std::to_wstring(GetLastError()) + L")";
    return false;
}

bool parseColor(const std::wstring& text, std::uint8_t& red, std::uint8_t& green, std::uint8_t& blue) {
    if (text.size() != 6) return false;
    try {
        std::size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 16);
        if (consumed != text.size() || value > 0xffffffUL) return false;
        red = static_cast<std::uint8_t>(value >> 16);
        green = static_cast<std::uint8_t>(value >> 8);
        blue = static_cast<std::uint8_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

int clampBrightness(const std::wstring& text) {
    try { return std::clamp(std::stoi(text), 0, 100); }
    catch (...) { return 100; }
}

std::uint8_t playerLedMask(const std::wstring& text) {
    if (text.empty()) return 0x1f;
    try { return static_cast<std::uint8_t>(std::clamp(std::stoi(text), 0, 0x1f)); }
    catch (...) { return 0x1f; }
}

int integerArgument(int argc, wchar_t** argv, const wchar_t* key, int fallback) {
    try { return std::stoi(argumentValue(argc, argv, key)); }
    catch (...) { return fallback; }
}

std::wstring quoted(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(character);
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring effectStopEventName(const std::wstring& instance) {
    // Stable FNV-1a keeps separate controllers independent across processes.
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : upper(instance)) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= 1099511628211ull;
    }
    std::wostringstream name;
    name << L"Local\\RGBCcontrol.DualSense.Effect." << std::hex << hash;
    return name.str();
}

void stopEffectWorker(const std::wstring& instance, DWORD settleMs = 0) {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, effectStopEventName(instance).c_str());
    if (event) {
        SetEvent(event);
        CloseHandle(event);
        if (settleMs > 0) Sleep(settleMs);
    }
}

double hueOf(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    const double r = red / 255.0, g = green / 255.0, b = blue / 255.0;
    const double maximum = std::max({r, g, b});
    const double minimum = std::min({r, g, b});
    const double delta = maximum - minimum;
    if (delta <= 0.000001) return 0.0;
    double hue = maximum == r ? 60.0 * std::fmod((g - b) / delta, 6.0)
               : maximum == g ? 60.0 * ((b - r) / delta + 2.0)
                              : 60.0 * ((r - g) / delta + 4.0);
    return hue < 0.0 ? hue + 360.0 : hue;
}

std::array<std::uint8_t, 3> hsv(double hue, double saturation, double value) {
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) hue += 360.0;
    saturation = std::clamp(saturation, 0.0, 1.0);
    value = std::clamp(value, 0.0, 1.0);
    const double chroma = value * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
    const double m = value - chroma;
    double r = 0.0, g = 0.0, b = 0.0;
    if (hue < 60.0) { r = chroma; g = x; }
    else if (hue < 120.0) { r = x; g = chroma; }
    else if (hue < 180.0) { g = chroma; b = x; }
    else if (hue < 240.0) { g = x; b = chroma; }
    else if (hue < 300.0) { r = x; b = chroma; }
    else { r = chroma; b = x; }
    return {
        static_cast<std::uint8_t>(std::lround((r + m) * 255.0)),
        static_cast<std::uint8_t>(std::lround((g + m) * 255.0)),
        static_cast<std::uint8_t>(std::lround((b + m) * 255.0))
    };
}

std::array<std::uint8_t, 3> effectColor(const std::wstring& mode, double elapsed,
                                         int speed, int intensity,
                                         std::uint8_t baseRed, std::uint8_t baseGreen, std::uint8_t baseBlue) {
    const double amount = std::clamp(intensity, 0, 100) / 100.0;
    const double rate = 0.30 + std::clamp(speed, 0, 100) / 38.0;
    const double phase = elapsed * rate;
    const double baseHue = hueOf(baseRed, baseGreen, baseBlue);
    std::wstring key = upper(mode);
    if (key == L"RAINBOW" || key == L"SPECTRUM CYCLE" || key == L"GRADIENT") {
        return hsv(baseHue + phase * (key == L"GRADIENT" ? 42.0 : 78.0), 0.98, amount);
    }
    if (key == L"BREATHING") {
        const double pulse = 0.12 + 0.88 * (std::sin(phase * 2.2) + 1.0) * 0.5;
        return {static_cast<std::uint8_t>(baseRed * amount * pulse),
                static_cast<std::uint8_t>(baseGreen * amount * pulse),
                static_cast<std::uint8_t>(baseBlue * amount * pulse)};
    }
    if (key == L"STROBE") {
        const double pulse = (static_cast<int>(std::floor(phase * 5.0)) & 1) == 0 ? amount : 0.0;
        return {static_cast<std::uint8_t>(baseRed * pulse), static_cast<std::uint8_t>(baseGreen * pulse),
                static_cast<std::uint8_t>(baseBlue * pulse)};
    }
    if (key == L"RANDOM") {
        const double step = std::floor(phase * 1.8);
        return hsv(std::fmod(step * 137.507764, 360.0), 0.95, amount);
    }
    if (key == L"WATER") return hsv(195.0 + std::sin(phase * 1.7) * 24.0, 0.88, amount * (0.62 + 0.38 * std::sin(phase * 2.1) * std::sin(phase * 2.1)));
    if (key == L"NEON") return hsv(286.0 + std::sin(phase * 1.9) * 58.0, 0.92, amount);
    if (key == L"SCAN" || key == L"STACK") {
        const double pulse = key == L"SCAN" ? 0.18 + 0.82 * std::abs(std::sin(phase * 2.5))
                                              : std::fmod(phase * 0.45, 1.0);
        return {static_cast<std::uint8_t>(baseRed * amount * pulse),
                static_cast<std::uint8_t>(baseGreen * amount * pulse),
                static_cast<std::uint8_t>(baseBlue * amount * pulse)};
    }
    if (key == L"MUSIC") {
        const double pulse = 0.28 + 0.72 * std::abs(std::sin(phase * 3.1) * std::sin(phase * 0.83));
        return hsv(baseHue + std::sin(phase) * 24.0, 0.9, amount * pulse);
    }
    if (key == L"AMBI LIGHT" || key == L"AMBILIGHT") return hsv(baseHue + phase * 25.0, 0.82, amount);
    if (key == L"WAVE") return hsv(baseHue + std::sin(phase * 1.6) * 90.0, 0.94, amount * (0.58 + 0.42 * std::sin(phase * 2.0) * std::sin(phase * 2.0)));
    return {static_cast<std::uint8_t>(baseRed * amount), static_cast<std::uint8_t>(baseGreen * amount),
            static_cast<std::uint8_t>(baseBlue * amount)};
}

bool launchEffectWorker(int argc, wchar_t** argv, std::wstring& error) {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
        error = L"Could not locate the DualSense driver";
        return false;
    }
    std::wstring commandLine = quoted(executable) + L" --effect-worker";
    for (int index = 1; index < argc; ++index) {
        commandLine += L" " + quoted(argv[index]);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process)) {
        error = L"Could not start the DualSense effect worker (Windows error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int runEffectWorker(int argc, wchar_t** argv) {
    const std::wstring instance = argumentValue(argc, argv, L"--instance");
    std::uint8_t baseRed = 0, baseGreen = 0, baseBlue = 0;
    if (!parseColor(argumentValue(argc, argv, L"--color"), baseRed, baseGreen, baseBlue)) return 4;
    HidTarget target;
    std::wstring error;
    if (!openTarget(instance, target, error)) return 3;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, effectStopEventName(instance).c_str());
    if (!stopEvent) return 6;
    ResetEvent(stopEvent);
    if (target.bluetooth) {
        auto neutral = makeReport(true, target.capabilities.OutputReportByteLength, 0, 0, 0, true);
        writeReport(target, neutral, error);
        Sleep(12);
    }
    const std::wstring mode = argumentValue(argc, argv, L"--mode");
    const int brightness = std::clamp(integerArgument(argc, argv, L"--brightness", 100), 0, 100);
    const int speed = std::clamp(integerArgument(argc, argv, L"--speed", 55), 0, 100);
    const int intensity = std::clamp(integerArgument(argc, argv, L"--intensity", 80), 0, 100);
    const std::uint8_t indicators = playerLedMask(argumentValue(argc, argv, L"--aux-led-mask"));
    const ULONGLONG started = GetTickCount64();
    const DWORD interval = target.bluetooth ? 50 : 34;
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        auto color = effectColor(mode, (GetTickCount64() - started) / 1000.0, speed, intensity,
                                 baseRed, baseGreen, baseBlue);
        for (std::uint8_t& channel : color) {
            channel = static_cast<std::uint8_t>((static_cast<int>(channel) * brightness + 50) / 100);
        }
        auto report = makeReport(target.bluetooth, target.capabilities.OutputReportByteLength,
                                 color[0], color[1], color[2], false, indicators);
        if (!writeReport(target, report, error)) break;
        if (WaitForSingleObject(stopEvent, interval) != WAIT_TIMEOUT) break;
    }
    CloseHandle(stopEvent);
    return 0;
}

void success(const HidTarget& target, bool probe) {
    std::cout << "RGBCPLUGIN/2\tOK\n";
    if (!probe) return;
    std::cout << "NAME=" << (target.attributes.ProductID == kDualSenseEdgeProduct
                                 ? "DualSense Edge Wireless Controller"
                                 : "DualSense Wireless Controller") << "\n"
              << "VENDOR=Sony Interactive Entertainment\n"
              << "TYPE=Manette\n"
              << "ZONES=2\n"
              << "LEDS=7\n";
}

int selfTest() {
    auto usb = makeReport(false, 48, 0x12, 0x34, 0x56, false);
    auto bluetooth = makeReport(true, 78, 0x12, 0x34, 0x56, false);
    const std::uint32_t stored = static_cast<std::uint32_t>(bluetooth[74]) |
        (static_cast<std::uint32_t>(bluetooth[75]) << 8) |
        (static_cast<std::uint32_t>(bluetooth[76]) << 16) |
        (static_cast<std::uint32_t>(bluetooth[77]) << 24);
    const auto gradientStart = effectColor(L"Gradient", 0.0, 55, 80, 0x7c, 0x5c, 0xff);
    const auto gradientLater = effectColor(L"Gradient", 1.0, 55, 80, 0x7c, 0x5c, 0xff);
    const auto strobeOn = effectColor(L"Strobe", 0.0, 55, 100, 0xff, 0x80, 0x40);
    const auto strobeOff = effectColor(L"Strobe", 0.20, 55, 100, 0xff, 0x80, 0x40);
    const bool valid = usb.size() == 48 && usb[0] == 0x02 && usb[2] == 0x14 &&
                       usb[44] == 0x3f && usb[45] == 0x12 && usb[46] == 0x34 && usb[47] == 0x56 &&
                       bluetooth.size() == 78 && bluetooth[0] == 0x31 && bluetooth[2] == 0x10 &&
                       bluetooth[4] == 0x14 && bluetooth[46] == 0x3f && bluetooth[47] == 0x12 &&
                       bluetooth[48] == 0x34 && bluetooth[49] == 0x56 && stored == bluetoothCrc(bluetooth) &&
                       gradientStart != gradientLater && strobeOn != strobeOff;
    return valid ? 0 : 10;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (argc == 2 && std::wstring(argv[1]) == L"--self-test") return selfTest();
    if (argc >= 2 && std::wstring(argv[1]) == L"--effect-worker") return runEffectWorker(argc, argv);

    const std::wstring command = argumentValue(argc, argv, L"--command");
    const std::wstring instance = argumentValue(argc, argv, L"--instance");
    if (command != L"probe" && command != L"static" && command != L"effect" && command != L"stop") {
        std::cout << "RGBCPLUGIN/2\tERROR\nUnsupported command\n";
        return 2;
    }

    HidTarget target;
    std::wstring error;
    if (!openTarget(instance, target, error)) {
        std::wcerr << L"RGBCPLUGIN/2\tERROR\n" << error << L"\n";
        return 3;
    }
    if (command == L"probe") {
        success(target, true);
        return 0;
    }

    if (command == L"effect") {
        std::uint8_t ignoredRed = 0, ignoredGreen = 0, ignoredBlue = 0;
        if (!parseColor(argumentValue(argc, argv, L"--color"), ignoredRed, ignoredGreen, ignoredBlue)) {
            std::cout << "RGBCPLUGIN/2\tERROR\nInvalid RGB color\n";
            return 4;
        }
        stopEffectWorker(instance, 90);
        if (!launchEffectWorker(argc, argv, error)) {
            std::wcerr << L"RGBCPLUGIN/2\tERROR\n" << error << L"\n";
            return 6;
        }
        success(target, false);
        return 0;
    }

    // Static colors and restore commands replace any running animation before
    // touching the HID interface, so two writers never fight over the LEDs.
    stopEffectWorker(instance, 70);

    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    const bool restore = command == L"stop";
    if (!restore && !parseColor(argumentValue(argc, argv, L"--color"), red, green, blue)) {
        std::cout << "RGBCPLUGIN/2\tERROR\nInvalid RGB color\n";
        return 4;
    }
    if (!restore) {
        const int brightness = clampBrightness(argumentValue(argc, argv, L"--brightness"));
        red = static_cast<std::uint8_t>((static_cast<int>(red) * brightness + 50) / 100);
        green = static_cast<std::uint8_t>((static_cast<int>(green) * brightness + 50) / 100);
        blue = static_cast<std::uint8_t>((static_cast<int>(blue) * brightness + 50) / 100);
    }

    // A first neutral Bluetooth report switches a controller from basic to
    // enhanced output mode without changing haptics or audio state.
    if (target.bluetooth && !restore) {
        auto neutral = makeReport(true, target.capabilities.OutputReportByteLength, 0, 0, 0, true);
        writeReport(target, neutral, error);
        Sleep(12);
    }
    const std::uint8_t indicators = playerLedMask(argumentValue(argc, argv, L"--aux-led-mask"));
    auto report = makeReport(target.bluetooth, target.capabilities.OutputReportByteLength,
                             red, green, blue, restore, indicators);
    if (!writeReport(target, report, error)) {
        std::wcerr << L"RGBCPLUGIN/2\tERROR\n" << error << L"\n";
        return 5;
    }
    success(target, false);
    return 0;
}
