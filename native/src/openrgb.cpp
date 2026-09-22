#include "openrgb.hpp"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
constexpr std::uint32_t kProtocol = 5;
constexpr std::size_t kMaxPacket = 8u * 1024u * 1024u;

struct SocketGuard {
    SOCKET value = INVALID_SOCKET;
    ~SocketGuard() { if (value != INVALID_SOCKET) closesocket(value); }
    void reset(SOCKET replacement = INVALID_SOCKET) {
        if (value != INVALID_SOCKET) closesocket(value);
        value = replacement;
    }
};

struct Packet {
    std::uint32_t device = 0;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> data;
};

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset + 2 > bytes.size()) throw std::runtime_error("OpenRGB: paquet tronque");
    std::uint16_t result = static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
    offset += 2;
    return result;
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("OpenRGB: paquet tronque");
    std::uint32_t result = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return result;
}

void skip(const std::vector<std::uint8_t>& bytes, std::size_t& offset, std::size_t count) {
    if (offset + count > bytes.size()) throw std::runtime_error("OpenRGB: paquet tronque");
    offset += count;
}

std::string readString(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    std::uint16_t length = readU16(bytes, offset);
    if (offset + length > bytes.size()) throw std::runtime_error("OpenRGB: chaine tronquee");
    std::string result(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    while (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

bool sendAll(SOCKET socket, const std::uint8_t* bytes, std::size_t count) {
    while (count > 0) {
        int sent = send(socket, reinterpret_cast<const char*>(bytes), static_cast<int>(std::min<std::size_t>(count, 1u << 20)), 0);
        if (sent <= 0) return false;
        bytes += sent;
        count -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool receiveAll(SOCKET socket, std::uint8_t* bytes, std::size_t count) {
    while (count > 0) {
        int received = recv(socket, reinterpret_cast<char*>(bytes), static_cast<int>(std::min<std::size_t>(count, 1u << 20)), 0);
        if (received <= 0) return false;
        bytes += received;
        count -= static_cast<std::size_t>(received);
    }
    return true;
}

SOCKET connectLocal(int timeoutMs) {
    SocketGuard holder;
    holder.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (holder.value == INVALID_SOCKET) throw std::runtime_error("OpenRGB: socket indisponible");

    u_long nonBlocking = 1;
    ioctlsocket(holder.value, FIONBIO, &nonBlocking);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6742);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    int result = connect(holder.value, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) throw std::runtime_error("OpenRGB: connexion refusee");

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(holder.value, &writeSet);
    timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    if (select(0, nullptr, &writeSet, nullptr, &timeout) <= 0) throw std::runtime_error("OpenRGB: delai de connexion depasse");
    int socketError = 0;
    int optionLength = sizeof(socketError);
    getsockopt(holder.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &optionLength);
    if (socketError != 0) throw std::runtime_error("OpenRGB: serveur absent");

    nonBlocking = 0;
    ioctlsocket(holder.value, FIONBIO, &nonBlocking);
    DWORD ioTimeout = 2500;
    setsockopt(holder.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
    setsockopt(holder.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
    SOCKET resultSocket = holder.value;
    holder.value = INVALID_SOCKET;
    return resultSocket;
}

void sendPacket(SOCKET socket, std::uint32_t device, std::uint32_t id, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> header;
    header.reserve(16);
    header.insert(header.end(), {'O', 'R', 'G', 'B'});
    appendU32(header, device);
    appendU32(header, id);
    appendU32(header, static_cast<std::uint32_t>(data.size()));
    if (!sendAll(socket, header.data(), header.size()) || (!data.empty() && !sendAll(socket, data.data(), data.size()))) {
        throw std::runtime_error("OpenRGB: envoi interrompu");
    }
}

Packet receivePacket(SOCKET socket) {
    std::uint8_t rawHeader[16]{};
    if (!receiveAll(socket, rawHeader, sizeof(rawHeader))) throw std::runtime_error("OpenRGB: connexion fermee");
    if (std::memcmp(rawHeader, "ORGB", 4) != 0) throw std::runtime_error("OpenRGB: en-tete invalide");
    Packet packet;
    std::memcpy(&packet.device, rawHeader + 4, 4);
    std::memcpy(&packet.id, rawHeader + 8, 4);
    std::uint32_t size = 0;
    std::memcpy(&size, rawHeader + 12, 4);
    if (size > kMaxPacket) throw std::runtime_error("OpenRGB: paquet trop volumineux");
    packet.data.resize(size);
    if (size && !receiveAll(socket, packet.data.data(), size)) throw std::runtime_error("OpenRGB: paquet incomplet");
    return packet;
}

Packet receiveMatching(SOCKET socket, std::uint32_t id) {
    for (int attempt = 0; attempt < 256; ++attempt) {
        Packet packet = receivePacket(socket);
        if (packet.id == id) return packet;
    }
    throw std::runtime_error("OpenRGB: reponse inattendue");
}

std::vector<std::uint8_t> u32Data(std::uint32_t value) {
    std::vector<std::uint8_t> result;
    appendU32(result, value);
    return result;
}

std::uint32_t hsv(double hue, double saturation, double value) {
    hue = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0);
    saturation = std::clamp(saturation, 0.0, 1.0);
    value = std::clamp(value, 0.0, 1.0);
    const double c = value * saturation;
    const double x = c * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
    const double m = value - c;
    double r = 0, g = 0, b = 0;
    if (hue < 60) { r = c; g = x; }
    else if (hue < 120) { r = x; g = c; }
    else if (hue < 180) { g = c; b = x; }
    else if (hue < 240) { g = x; b = c; }
    else if (hue < 300) { r = x; b = c; }
    else { r = c; b = x; }
    return (static_cast<std::uint32_t>(std::lround((r + m) * 255.0)) << 16) |
           (static_cast<std::uint32_t>(std::lround((g + m) * 255.0)) << 8) |
           static_cast<std::uint32_t>(std::lround((b + m) * 255.0));
}

std::uint32_t scaleRgb(std::uint32_t rgb, double factor) {
    factor = std::clamp(factor, 0.0, 1.0);
    auto scale = [factor](std::uint32_t channel) { return static_cast<std::uint32_t>(std::lround(channel * factor)); };
    return (scale((rgb >> 16) & 255) << 16) | (scale((rgb >> 8) & 255) << 8) | scale(rgb & 255);
}

std::uint32_t blendRgb(std::uint32_t from, std::uint32_t to, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    amount = amount * amount * (3.0 - 2.0 * amount);
    auto blend = [amount](std::uint32_t left, std::uint32_t right) {
        return static_cast<std::uint32_t>(std::lround(left + (static_cast<double>(right) - left) * amount));
    };
    return (blend((from >> 16) & 255, (to >> 16) & 255) << 16) |
           (blend((from >> 8) & 255, (to >> 8) & 255) << 8) |
           blend(from & 255, to & 255);
}

double rgbHue(std::uint32_t rgb) {
    const double red = ((rgb >> 16) & 255) / 255.0;
    const double green = ((rgb >> 8) & 255) / 255.0;
    const double blue = (rgb & 255) / 255.0;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;
    if (delta <= 0.000001) return 0;
    double hue = maximum == red ? 60.0 * std::fmod((green - blue) / delta, 6.0)
               : maximum == green ? 60.0 * ((blue - red) / delta + 2.0)
                                  : 60.0 * ((red - green) / delta + 4.0);
    return hue < 0 ? hue + 360.0 : hue;
}

std::uint32_t smoothGradient(double phaseDegrees, double position, std::uint32_t baseRgb, double intensity) {
    // Keep the gradient highly saturated. The previous cosine palette left a
    // non-zero floor in all three channels; controllers with coarse channel
    // resolution often collapsed those pastel values to white, making the
    // animation look like a white/blue blink. HSV is continuous around 360°,
    // keeps one channel genuinely low and preserves the spatial seam.
    const double hue = rgbHue(baseRgb) + phaseDegrees + std::clamp(position, 0.0, 1.0) * 360.0;
    return hsv(hue, 0.98, std::clamp(intensity, 0.0, 1.0));
}

std::uint32_t reorderRgb(std::uint32_t rgb, RgbColorOrder order) {
    const std::uint32_t r = (rgb >> 16) & 255;
    const std::uint32_t g = (rgb >> 8) & 255;
    const std::uint32_t b = rgb & 255;
    switch (order) {
        case RgbColorOrder::RBG: return (r << 16) | (b << 8) | g;
        case RgbColorOrder::GRB: return (g << 16) | (r << 8) | b;
        case RgbColorOrder::GBR: return (g << 16) | (b << 8) | r;
        case RgbColorOrder::BRG: return (b << 16) | (r << 8) | g;
        case RgbColorOrder::BGR: return (b << 16) | (g << 8) | r;
        default: return rgb;
    }
}

std::vector<std::uint8_t> colorPacket(const std::vector<std::uint32_t>& colors, RgbColorOrder order) {
    const std::size_t count = std::clamp<std::size_t>(colors.size(), 1, 4096);
    std::vector<std::uint8_t> packet;
    packet.reserve(6 + count * 4);
    appendU32(packet, static_cast<std::uint32_t>(6 + count * 4));
    appendU16(packet, static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t rgb = OpenRgbClient::orderedColor(colors[i], order);
        const std::uint32_t packed = ((rgb >> 16) & 255) | (rgb & 0x00ff00) | ((rgb & 255) << 16);
        appendU32(packet, packed);
    }
    return packet;
}

std::vector<std::uint8_t> zoneColorPacket(std::uint32_t zoneIndex,
                                          const std::vector<std::uint32_t>& colors,
                                          RgbColorOrder order) {
    const std::size_t count = std::clamp<std::size_t>(colors.size(), 1, 4096);
    std::vector<std::uint8_t> packet;
    packet.reserve(10 + count * 4);
    appendU32(packet, static_cast<std::uint32_t>(10 + count * 4));
    appendU32(packet, zoneIndex);
    appendU16(packet, static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t rgb = OpenRgbClient::orderedColor(colors[i], order);
        const std::uint32_t packed = ((rgb >> 16) & 255) | (rgb & 0x00ff00) | ((rgb & 255) << 16);
        appendU32(packet, packed);
    }
    return packet;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}
} // namespace

std::uint32_t OpenRgbClient::gradientColor(double phaseDegrees, double position,
                                           std::uint32_t baseRgb, double intensity) {
    return smoothGradient(phaseDegrees, position, baseRgb, intensity);
}

std::uint32_t OpenRgbClient::orderedColor(std::uint32_t rgb, RgbColorOrder order) {
    return reorderRgb(rgb, order);
}

bool OpenRgbClient::prefersAtomicFrames(const RgbDevice& device) {
    const std::wstring identity = lower(device.vendor + L" " + device.name + L" " + device.description);
    // ASRock Polychrome exposes several zones but commits them through one USB
    // controller. Sending UPDATEZONELEDS four times makes some firmwares show
    // each partial state in turn (commonly white/blue). A complete 1050 frame
    // is applied as one coherent transaction. Other multi-output boards retain
    // zone packets, which are required for independently sized empty headers.
    return identity.find(L"asrock") != std::wstring::npos || identity.find(L"polychrome") != std::wstring::npos;
}

bool OpenRgbClient::canConnect(int timeoutMs) const {
    try {
        SocketGuard socket{ connectLocal(timeoutMs) };
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<RgbDevice> OpenRgbClient::scan(bool rescan) const {
    SocketGuard socket{ connectLocal(2500) };
    sendPacket(socket.value, 0, 40, u32Data(kProtocol));
    Packet versionPacket = receiveMatching(socket.value, 40);
    std::size_t versionOffset = 0;
    std::uint32_t version = versionPacket.data.size() >= 4 ? std::min(kProtocol, readU32(versionPacket.data, versionOffset)) : 0;
    std::string client = "RGBCcontrol Native";
    std::vector<std::uint8_t> clientBytes(client.begin(), client.end());
    clientBytes.push_back(0);
    sendPacket(socket.value, 0, 50, clientBytes);
    if (rescan && version >= 5) sendPacket(socket.value, 0, 140, {});
    sendPacket(socket.value, 0, 0, {});
    Packet countPacket = receiveMatching(socket.value, 0);
    std::size_t countOffset = 0;
    std::uint32_t count = countPacket.data.size() >= 4 ? readU32(countPacket.data, countOffset) : 0;

    static const wchar_t* typeNames[] = {
        L"Carte mere", L"Memoire", L"GPU", L"Refroidissement", L"Ruban LED", L"Clavier",
        L"Souris", L"Tapis", L"Casque", L"Support casque", L"Manette", L"Luminaire",
        L"Enceinte", L"Virtuel", L"Stockage", L"Boitier", L"Microphone", L"Accessoire",
        L"Pave numerique", L"Portable", L"Ecran", L"Inconnu"
    };
    std::vector<RgbDevice> result;
    for (std::uint32_t index = 0; index < count; ++index) {
        sendPacket(socket.value, index, 1, version ? u32Data(version) : std::vector<std::uint8_t>{});
        Packet packet = receiveMatching(socket.value, 1);
        std::size_t offset = 4;
        if (packet.data.size() < offset + 6) continue;
        int type = static_cast<int>(readU32(packet.data, offset));
        std::string name = readString(packet.data, offset);
        std::string vendor = version >= 1 ? readString(packet.data, offset) : std::string{};
        std::string description = readString(packet.data, offset);
        int zones = 0;
        int leds = 1;
        std::vector<std::wstring> modes;
        std::vector<RgbZone> zoneDetails;
        try {
            readString(packet.data, offset);
            readString(packet.data, offset);
            readString(packet.data, offset);
            std::uint16_t modeCount = readU16(packet.data, offset);
            skip(packet.data, offset, 4);
            for (std::uint16_t mode = 0; mode < modeCount; ++mode) {
                modes.push_back(widen(readString(packet.data, offset)));
                skip(packet.data, offset, 48);
                std::uint16_t modeColors = readU16(packet.data, offset);
                skip(packet.data, offset, static_cast<std::size_t>(modeColors) * 4);
            }
            std::uint16_t zoneCount = readU16(packet.data, offset);
            zones = zoneCount;
            for (std::uint16_t zone = 0; zone < zoneCount; ++zone) {
                RgbZone detail;
                detail.name = widen(readString(packet.data, offset));
                detail.type = readU32(packet.data, offset);
                detail.minimumLeds = readU32(packet.data, offset);
                detail.maximumLeds = readU32(packet.data, offset);
                detail.leds = readU32(packet.data, offset);
                std::uint16_t matrixSize = readU16(packet.data, offset);
                skip(packet.data, offset, matrixSize);
                std::uint16_t segmentCount = readU16(packet.data, offset);
                for (std::uint16_t segment = 0; segment < segmentCount; ++segment) {
                    readString(packet.data, offset);
                    skip(packet.data, offset, 12);
                }
                detail.flags = readU32(packet.data, offset);
                zoneDetails.push_back(std::move(detail));
            }
            leds = std::max(1, static_cast<int>(readU16(packet.data, offset)));
        } catch (...) {
            zones = 0;
            leds = 1;
            zoneDetails.clear();
        }
        RgbDevice device;
        device.index = static_cast<int>(index);
        device.name = name.empty() ? L"Appareil RGB" : widen(name);
        device.vendor = widen(vendor);
        device.description = widen(description);
        device.type = type >= 0 && type < static_cast<int>(std::size(typeNames)) ? typeNames[type] : L"Inconnu";
        device.zones = zones;
        device.leds = leds;
        device.modes = std::move(modes);
        device.zoneDetails = std::move(zoneDetails);
        if (zones > 0) device.capabilities |= DeviceCapability::Zones;
        if (leds > 1) device.capabilities |= DeviceCapability::PerLed;
        result.push_back(std::move(device));
    }
    return result;
}

int OpenRgbClient::initializeEmptyZones(std::vector<RgbDevice>& devices, int preferredLeds) const {
    SocketGuard socket{ connectLocal(900) };
    sendPacket(socket.value, 0, 40, u32Data(kProtocol));
    receiveMatching(socket.value, 40);
    const std::string client = "RGBCcontrol Zone Setup";
    std::vector<std::uint8_t> clientBytes(client.begin(), client.end());
    clientBytes.push_back(0);
    sendPacket(socket.value, 0, 50, clientBytes);

    int initialized = 0;
    for (RgbDevice& device : devices) {
        if (!device.selected) continue;
        for (std::size_t zoneIndex = 0; zoneIndex < device.zoneDetails.size(); ++zoneIndex) {
            RgbZone& zone = device.zoneDetails[zoneIndex];
            if (zone.leds != 0 || zone.maximumLeds == 0 || zone.maximumLeds < zone.minimumLeds) continue;

            // Bits 0 and 1 are OpenRGB's two manually-resizable-size flags.
            // Older controllers did not always set them consistently, so a
            // genuine min/max range is accepted as a compatibility fallback.
            const bool resizable = (zone.flags & 0x3u) != 0 || zone.maximumLeds > zone.minimumLeds;
            if (!resizable) continue;
            const int configuredLeds = device.preferredLeds > 0 ? device.preferredLeds : preferredLeds;
            const std::uint32_t wanted = static_cast<std::uint32_t>(std::max(1, configuredLeds));
            const std::uint32_t target = std::clamp(wanted, zone.minimumLeds, zone.maximumLeds);
            if (target == 0) continue;

            std::vector<std::uint8_t> resize;
            resize.reserve(8);
            appendU32(resize, static_cast<std::uint32_t>(zoneIndex));
            appendU32(resize, target);
            sendPacket(socket.value, static_cast<std::uint32_t>(device.index), 1000, resize);
            zone.leds = target;
            ++initialized;
        }

        int total = 0;
        for (const RgbZone& zone : device.zoneDetails) {
            int count = static_cast<int>(std::min<std::uint32_t>(zone.leds, 4096));
            if ((zone.flags & 0x1u) != 0 && count > 0) count = 1;
            total += count;
        }
        if (total > 0) device.leds = std::clamp(total, 1, 4096);
    }
    if (initialized > 0) std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return initialized;
}

void OpenRgbClient::sendEffectFrame(const std::vector<RgbDevice>& devices,
                                    const std::wstring& mode,
                                    double phase,
                                    std::uint32_t baseRgb,
                                    double intensity,
                                    double audioBass,
                                    double audioMid,
                                    double audioTreble,
                                    double audioVolume,
                                    std::array<std::uint32_t, 4> ambientColors,
                                    std::uint32_t ambientAverage) const {
    // Animated effects keep one SDK connection per worker thread. Reopening a
    // TCP session for every frame caused visible stalls and eventually socket
    // exhaustion on long-running gradients. A failed stream is discarded so
    // the next frame can reconnect cleanly.
    thread_local SocketGuard socket;
    try {
        if (socket.value == INVALID_SOCKET) {
            socket.value = connectLocal(700);
            sendPacket(socket.value, 0, 40, u32Data(kProtocol));
            receiveMatching(socket.value, 40);
            const std::string client = "RGBCcontrol Effects";
            std::vector<std::uint8_t> clientBytes(client.begin(), client.end());
            clientBytes.push_back(0);
            sendPacket(socket.value, 0, 50, clientBytes);
        }
        const std::wstring key = lower(mode);
        auto frameColorCount = [](const RgbDevice& device) {
            int zoneTotal = 0;
            for (const RgbZone& zone : device.zoneDetails) {
                int count = static_cast<int>(std::min<std::uint32_t>(zone.leds, 4096));
                if ((zone.flags & 0x1u) != 0 && count > 0) count = 1;
                zoneTotal += count;
            }
            return device.zoneDetails.size() > 1 && zoneTotal > 0 ? zoneTotal : std::clamp(device.leds, 1, 4096);
        };
        int globalColorTotal = 0;
        for (const RgbDevice& device : devices) {
            if (device.selected) globalColorTotal += frameColorCount(device);
        }
        globalColorTotal = std::max(1, globalColorTotal);
        int deviceFirstColor = 0;
        for (const RgbDevice& device : devices) {
        if (!device.selected) continue;
        auto makeColors = [&](int count, int first, int total) {
            count = std::clamp(count, 1, 4096);
            total = std::max(1, total);
            std::vector<std::uint32_t> colors(static_cast<std::size_t>(count), baseRgb);
            for (int led = 0; led < count; ++led) {
                const int globalLed = first + led;
                const double position = total <= 1 ? 0.0 : static_cast<double>(globalLed) / (total - 1);
                std::uint32_t color = baseRgb;
                if (key == L"rainbow") color = hsv(phase * 55.0 + position * 360.0, 1.0, intensity);
                else if (key == L"spectrum cycle") color = hsv(phase * 60.0, 1.0, intensity);
                else if (key == L"breathing") color = scaleRgb(baseRgb, intensity * (0.18 + 0.82 * ((std::sin(phase * 2.0) + 1.0) / 2.0)));
                else if (key == L"wave") color = hsv(phase * 42.0 + position * 140.0, 0.92, intensity * (0.30 + 0.70 * ((std::sin(position * 12.566 - phase * 2.2) + 1.0) / 2.0)));
                else if (key == L"strobe") color = scaleRgb(baseRgb, static_cast<int>(std::floor(phase * 2.2)) % 2 == 0 ? intensity : 0.0);
                else if (key == L"random") color = hsv(std::fmod(globalLed * 97.0 + std::floor(phase * 2.0) * 71.0, 360.0), 0.95, intensity);
                else if (key == L"music") {
                    const double levels[] = {audioBass, audioMid, audioTreble};
                    const double hueOffsets[] = {-32.0, 8.0, 92.0};
                    int band = 0;
                    if (total <= 1) {
                        band = audioMid > audioBass ? 1 : 0;
                        if (audioTreble > levels[band]) band = 2;
                    } else {
                        band = std::min(2, static_cast<int>(position * 3.0));
                    }
                    const double response = std::clamp(std::max(levels[band], audioVolume * 0.32), 0.0, 1.0);
                    const double pulse = 0.86 + 0.14 * std::sin(phase * 2.0 + position * 6.283185307179586);
                    color = hsv(rgbHue(baseRgb) + hueOffsets[band], 0.92,
                                intensity * std::clamp(0.025 + response * pulse, 0.0, 1.0));
                }
                else if (key == L"ambilight") {
                    if (total <= 1) {
                        color = scaleRgb(ambientAverage, intensity);
                    } else {
                        const double cycle = position * 4.0;
                        const int segment = static_cast<int>(std::floor(cycle)) % 4;
                        const int next = (segment + 1) % 4;
                        color = scaleRgb(blendRgb(ambientColors[segment], ambientColors[next], cycle - std::floor(cycle)), intensity);
                    }
                }
                else if (key == L"neon") color = hsv(285.0 + 65.0 * std::sin(phase + position * 5.0), 0.9, intensity * (0.55 + 0.45 * std::abs(std::sin(phase * 1.7 + position * 4.0))));
                else if (key == L"water") color = hsv(185.0 + 28.0 * std::sin(phase + position * 8.0), 0.85, intensity * (0.45 + 0.55 * ((std::sin(phase * 1.4 - position * 10.0) + 1.0) / 2.0)));
                else if (key == L"scan") {
                    const double distance = std::abs(position - ((std::sin(phase) + 1.0) / 2.0));
                    color = scaleRgb(baseRgb, intensity * std::max(0.05, 1.0 - distance * 8.0));
                } else if (key == L"stack") {
                    const double fill = std::fmod(phase * 0.16, 1.15);
                    color = position <= fill ? hsv(position * 220.0 + phase * 20.0, 0.9, intensity) : scaleRgb(baseRgb, 0.03);
                } else if (key == L"gradient") {
                    color = gradientColor(phase, position, baseRgb, intensity);
                } else color = scaleRgb(baseRgb, intensity);
                colors[static_cast<std::size_t>(led)] = color;
            }
            return colors;
        };

        // A device-wide update is ambiguous on multi-output motherboard
        // controllers. OpenRGB exposes the physical outputs as zones, so send
        // packet 1051 to every non-empty zone. Effects-only zones intentionally
        // receive one color even if their configured hardware size is larger.
        std::vector<int> zoneCounts;
        int totalZoneColors = 0;
        for (const RgbZone& zone : device.zoneDetails) {
            int count = static_cast<int>(std::min<std::uint32_t>(zone.leds, 4096));
            if ((zone.flags & 0x1u) != 0 && count > 0) count = 1;
            zoneCounts.push_back(count);
            totalZoneColors += count;
        }
        const bool hasUsableZones = device.zoneDetails.size() > 1 && totalZoneColors > 0;
        const bool useZonePackets = hasUsableZones && (device.frameTransport == RgbFrameTransport::PerZone ||
            (device.frameTransport == RgbFrameTransport::Automatic && !prefersAtomicFrames(device)));
        if (useZonePackets) {
            int first = 0;
            for (std::size_t zone = 0; zone < device.zoneDetails.size(); ++zone) {
                const int count = zoneCounts[zone];
                if (count <= 0) continue;
                sendPacket(socket.value, static_cast<std::uint32_t>(device.index), 1051,
                           zoneColorPacket(static_cast<std::uint32_t>(zone),
                                           makeColors(count, deviceFirstColor + first, globalColorTotal), device.colorOrder));
                first += count;
            }
        } else {
            const int count = std::clamp(device.leds, 1, 4096);
            sendPacket(socket.value, static_cast<std::uint32_t>(device.index), 1050,
                       colorPacket(makeColors(count, deviceFirstColor, globalColorTotal), device.colorOrder));
        }
        deviceFirstColor += frameColorCount(device);
        }
    } catch (...) {
        socket.reset();
        throw;
    }
}
