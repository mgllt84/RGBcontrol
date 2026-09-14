#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "screen_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

namespace {
struct MonitorCollector {
    std::vector<ScreenMonitorInfo>* result = nullptr;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* collector = reinterpret_cast<MonitorCollector*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!collector || !collector->result || !GetMonitorInfoW(monitor, &info)) return TRUE;
    ScreenMonitorInfo item;
    item.deviceName = info.szDevice;
    item.left = info.rcMonitor.left;
    item.top = info.rcMonitor.top;
    item.width = info.rcMonitor.right - info.rcMonitor.left;
    item.height = info.rcMonitor.bottom - info.rcMonitor.top;
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    collector->result->push_back(std::move(item));
    return TRUE;
}

std::uint32_t rgb(int red, int green, int blue) {
    return (static_cast<std::uint32_t>(std::clamp(red, 0, 255)) << 16) |
           (static_cast<std::uint32_t>(std::clamp(green, 0, 255)) << 8) |
           static_cast<std::uint32_t>(std::clamp(blue, 0, 255));
}

std::array<double, 3> unpack(std::uint32_t color) {
    return {static_cast<double>((color >> 16) & 0xFF), static_cast<double>((color >> 8) & 0xFF),
            static_cast<double>(color & 0xFF)};
}

std::uint32_t adjustSaturation(std::uint32_t color, int saturation) {
    const auto channels = unpack(color);
    const double gray = channels[0] * 0.2126 + channels[1] * 0.7152 + channels[2] * 0.0722;
    const double factor = std::clamp(saturation, 0, 100) / 50.0;
    return rgb(static_cast<int>(std::lround(gray + (channels[0] - gray) * factor)),
               static_cast<int>(std::lround(gray + (channels[1] - gray) * factor)),
               static_cast<int>(std::lround(gray + (channels[2] - gray) * factor)));
}
} // namespace

struct AmbientScreenCapture::Impl {
    ScreenMonitorInfo monitor;
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    bool firstFrame = true;
    std::array<std::array<double, 3>, 4> smoothed{};
    std::array<double, 3> smoothedAverage{};
    std::chrono::steady_clock::time_point lastFrame{};
    double measuredFps = 0.0;

    ~Impl() {
        if (memory && previousBitmap) SelectObject(memory, previousBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
    }
};

AmbientScreenCapture::AmbientScreenCapture() : impl_(std::make_unique<Impl>()) {}
AmbientScreenCapture::~AmbientScreenCapture() = default;

std::vector<ScreenMonitorInfo> AmbientScreenCapture::enumerateMonitors() {
    std::vector<ScreenMonitorInfo> result;
    MonitorCollector collector{&result};
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&collector));
    std::stable_sort(result.begin(), result.end(), [](const ScreenMonitorInfo& left, const ScreenMonitorInfo& right) {
        return left.primary && !right.primary;
    });
    return result;
}

bool AmbientScreenCapture::start(int monitorIndex, int sampleWidth, int sampleHeight, std::wstring& error) {
    error.clear();
    const std::vector<ScreenMonitorInfo> monitors = enumerateMonitors();
    if (monitors.empty()) { error = L"Aucun écran Windows détecté"; return false; }
    monitorIndex = std::clamp(monitorIndex, 0, static_cast<int>(monitors.size()) - 1);
    impl_->monitor = monitors[monitorIndex];
    impl_->width = std::clamp(sampleWidth, 24, 160);
    impl_->height = std::clamp(sampleHeight, 14, 90);
    impl_->screen = GetDC(nullptr);
    if (!impl_->screen) { error = L"Capture de l'écran indisponible"; return false; }
    impl_->memory = CreateCompatibleDC(impl_->screen);
    if (!impl_->memory) { error = L"Contexte de capture indisponible"; return false; }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = impl_->width;
    info.bmiHeader.biHeight = -impl_->height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    impl_->bitmap = CreateDIBSection(impl_->screen, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&impl_->pixels), nullptr, 0);
    if (!impl_->bitmap || !impl_->pixels) { error = L"Mémoire de capture indisponible"; return false; }
    impl_->previousBitmap = SelectObject(impl_->memory, impl_->bitmap);
    SetStretchBltMode(impl_->memory, COLORONCOLOR);
    impl_->lastFrame = std::chrono::steady_clock::now();
    return true;
}

bool AmbientScreenCapture::sample(AmbientFrame& frame, int saturation, int reactivity, bool useZones, std::wstring& error) {
    error.clear();
    frame = AmbientFrame{};
    if (!impl_->screen || !impl_->memory || !impl_->pixels) { error = L"Capture Ambilight non démarrée"; return false; }
    SetLastError(ERROR_SUCCESS);
    if (!StretchBlt(impl_->memory, 0, 0, impl_->width, impl_->height, impl_->screen,
                    impl_->monitor.left, impl_->monitor.top, impl_->monitor.width, impl_->monitor.height, SRCCOPY)) {
        error = L"Windows a refusé la capture de l'écran (code " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    struct Accumulator { double r = 0, g = 0, b = 0; std::uint64_t count = 0; };
    std::array<Accumulator, 4> zones{};
    Accumulator average;
    const int edgeX = std::max(1, impl_->width / 4);
    const int edgeY = std::max(1, impl_->height / 4);
    auto add = [](Accumulator& target, int red, int green, int blue) {
        target.r += red * red; target.g += green * green; target.b += blue * blue; ++target.count;
    };
    for (int y = 0; y < impl_->height; ++y) {
        for (int x = 0; x < impl_->width; ++x) {
            const unsigned char* pixel = impl_->pixels + (static_cast<std::size_t>(y) * impl_->width + x) * 4;
            const int blue = pixel[0], green = pixel[1], red = pixel[2];
            add(average, red, green, blue);
            if (x < edgeX) add(zones[0], red, green, blue);
            if (y < edgeY) add(zones[1], red, green, blue);
            if (x >= impl_->width - edgeX) add(zones[2], red, green, blue);
            if (y >= impl_->height - edgeY) add(zones[3], red, green, blue);
        }
    }
    auto finish = [&](const Accumulator& source) {
        if (!source.count) return std::uint32_t{0};
        return adjustSaturation(rgb(static_cast<int>(std::sqrt(source.r / source.count)),
                                    static_cast<int>(std::sqrt(source.g / source.count)),
                                    static_cast<int>(std::sqrt(source.b / source.count))), saturation);
    };
    const std::uint32_t rawAverage = finish(average);
    std::array<std::uint32_t, 4> rawZones{};
    for (int zone = 0; zone < 4; ++zone) rawZones[zone] = useZones ? finish(zones[zone]) : rawAverage;

    const double normalized = std::clamp(reactivity, 0, 100) / 100.0;
    const double alpha = impl_->firstFrame ? 1.0 : 0.06 + 0.88 * std::pow(normalized, 1.35);
    auto smooth = [alpha](std::array<double, 3>& previous, std::uint32_t raw) {
        const auto current = unpack(raw);
        for (int channel = 0; channel < 3; ++channel) previous[channel] += (current[channel] - previous[channel]) * alpha;
        return rgb(static_cast<int>(std::lround(previous[0])), static_cast<int>(std::lround(previous[1])),
                   static_cast<int>(std::lround(previous[2])));
    };
    for (int zone = 0; zone < 4; ++zone) frame.zones[zone] = smooth(impl_->smoothed[zone], rawZones[zone]);
    frame.average = smooth(impl_->smoothedAverage, rawAverage);
    impl_->firstFrame = false;

    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - impl_->lastFrame).count();
    impl_->lastFrame = now;
    if (elapsed > 0.0001) {
        const double instantaneous = std::min(240.0, 1.0 / elapsed);
        impl_->measuredFps = impl_->measuredFps <= 0.0 ? instantaneous : impl_->measuredFps * 0.86 + instantaneous * 0.14;
    }
    frame.fps = impl_->measuredFps;
    frame.valid = true;
    return true;
}
