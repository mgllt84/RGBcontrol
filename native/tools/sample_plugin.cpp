#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

int wmain(int argc, wchar_t** argv) {
    std::wstring command;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wstring(argv[index]) == L"--command") command = argv[index + 1];
    }
    if (command != L"probe" && command != L"static" && command != L"effect" && command != L"stop") {
        std::cerr << "RGBCPLUGIN/2\tERROR\nUnsupported command\n";
        return 2;
    }
    std::cout << "RGBCPLUGIN/2\tOK\n";
    if (command == L"probe") {
        std::cout << "NAME=RGBCcontrol protocol test\n"
                     "VENDOR=RGBCcontrol\n"
                     "TYPE=Virtual test device\n"
                     "ZONES=2\n"
                     "LEDS=12\n";
    }
    return 0;
}
