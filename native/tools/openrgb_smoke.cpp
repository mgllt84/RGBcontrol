#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <iostream>
#include "../src/openrgb.hpp"

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    try {
        auto devices = OpenRgbClient().scan(false);
        std::cout << "devices=" << devices.size() << "\n";
        for (const auto& device : devices) {
            std::wcout << device.index << L"|" << device.name << L"|" << device.type
                       << L"|zones=" << device.zones << L"|leds=" << device.leds << L"\n";
        }
        WSACleanup();
        return devices.empty() ? 3 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        WSACleanup();
        return 1;
    }
}
