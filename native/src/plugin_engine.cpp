#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <wincrypt.h>

#include "plugin_engine.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {
constexpr wchar_t kManifestSection[] = L"RGBCPlugin";
constexpr int kPluginApiVersion = 2;

struct WindowsUsbDevice {
    std::wstring instanceId;
    std::wstring hardwareIds;
    std::wstring name;
    std::wstring vendor;
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

std::wstring upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towupper(character));
    });
    return value;
}

std::wstring trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring iniText(const fs::path& path, const wchar_t* key, const wchar_t* fallback = L"") {
    std::vector<wchar_t> buffer(8192);
    GetPrivateProfileStringW(kManifestSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return trim(buffer.data());
}

int iniInteger(const fs::path& path, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(kManifestSection, key, fallback, path.c_str()));
}

bool safeIdentifier(const std::wstring& value) {
    if (value.empty() || value.size() > 96) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t character) {
        return iswalnum(character) || character == L'.' || character == L'-' || character == L'_';
    });
}

bool parseWord16(const std::wstring& text, std::uint16_t& result) {
    if (text.empty()) return false;
    try {
        std::size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 0);
        if (consumed != text.size() || value > 0xffffUL) return false;
        result = static_cast<std::uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::wstring> split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> values;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(separator, start);
        std::wstring value = trim(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!value.empty()) values.push_back(std::move(value));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return values;
}

DeviceCapability parseCapabilities(const std::wstring& text) {
    DeviceCapability value = DeviceCapability::Discovery | DeviceCapability::LocalOnly;
    std::wstring normalized = lower(text);
    std::replace(normalized.begin(), normalized.end(), L';', L',');
    for (const std::wstring& item : split(normalized, L',')) {
        if (item == L"lighting") value |= DeviceCapability::Lighting;
        else if (item == L"effects") value |= DeviceCapability::Effects;
        else if (item == L"zones") value |= DeviceCapability::Zones;
        else if (item == L"perled") value |= DeviceCapability::PerLed;
        else if (item == L"sensors") value |= DeviceCapability::Sensors;
        else if (item == L"fanread") value |= DeviceCapability::FanRead;
        else if (item == L"fancontrol") value |= DeviceCapability::FanControl;
        else if (item == L"hotplug") value |= DeviceCapability::HotPlug;
    }
    return value;
}

std::wstring sha256File(const fs::path& path) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (provider) CryptReleaseContext(provider, 0);
        return {};
    }
    bool valid = true;
    std::ifstream stream(path, std::ios::binary);
    std::array<BYTE, 64 * 1024> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0 && !CryptHashData(hash, buffer.data(), static_cast<DWORD>(count), 0)) {
            valid = false;
            break;
        }
    }
    BYTE digest[32]{};
    DWORD digestSize = sizeof(digest);
    std::wstring result;
    if (valid && stream.eof() && CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0)) {
        std::wostringstream output;
        output << std::hex << std::setfill(L'0');
        for (DWORD index = 0; index < digestSize; ++index) output << std::setw(2) << static_cast<int>(digest[index]);
        result = output.str();
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return result;
}

bool pathInside(const fs::path& child, const fs::path& root) {
    std::error_code error;
    const fs::path canonicalChild = fs::weakly_canonical(child, error);
    if (error) return false;
    const fs::path canonicalRoot = fs::weakly_canonical(root, error);
    if (error) return false;
    std::wstring childText = lower(canonicalChild.wstring());
    std::wstring rootText = lower(canonicalRoot.wstring());
    if (!rootText.ends_with(L"\\")) rootText += L"\\";
    return childText.starts_with(rootText);
}

std::wstring property(HDEVINFO devices, SP_DEVINFO_DATA& info, DWORD propertyId) {
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(devices, &info, propertyId, &type, nullptr, 0, &required);
    if (required == 0) return {};
    std::vector<BYTE> bytes(required + sizeof(wchar_t) * 2, 0);
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, propertyId, &type, bytes.data(),
                                           static_cast<DWORD>(bytes.size()), &required)) return {};
    const wchar_t* values = reinterpret_cast<const wchar_t*>(bytes.data());
    std::wstring result;
    if (type == REG_MULTI_SZ) {
        while (*values) {
            if (!result.empty()) result += L";";
            result += values;
            values += wcslen(values) + 1;
        }
    } else {
        result = values;
    }
    return result;
}

std::vector<WindowsUsbDevice> enumerateUsbDevices() {
    std::vector<WindowsUsbDevice> result;
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) return result;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
        WindowsUsbDevice device;
        device.hardwareIds = upper(property(devices, info, SPDRP_HARDWAREID));
        const bool usbIdentifiers = device.hardwareIds.find(L"VID_") != std::wstring::npos &&
                                    device.hardwareIds.find(L"PID_") != std::wstring::npos;
        const bool bluetoothIdentifiers = device.hardwareIds.find(L"VID&0002") != std::wstring::npos &&
                                          device.hardwareIds.find(L"_PID&") != std::wstring::npos;
        if (!usbIdentifiers && !bluetoothIdentifiers) continue;
        std::vector<wchar_t> instance(4096);
        if (!SetupDiGetDeviceInstanceIdW(devices, &info, instance.data(), static_cast<DWORD>(instance.size()), nullptr)) continue;
        device.instanceId = instance.data();
        device.name = property(devices, info, SPDRP_FRIENDLYNAME);
        if (device.name.empty()) device.name = property(devices, info, SPDRP_DEVICEDESC);
        device.vendor = property(devices, info, SPDRP_MFG);
        result.push_back(std::move(device));
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'"');
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            escaped.append(slashes * 2 + 1, L'\\');
            escaped.push_back(L'"');
            slashes = 0;
            continue;
        }
        escaped.append(slashes, L'\\');
        slashes = 0;
        escaped.push_back(character);
    }
    escaped.append(slashes * 2, L'\\');
    escaped.push_back(L'"');
    return escaped;
}

std::map<std::wstring, std::wstring> parseResponseMetadata(const std::wstring& output) {
    std::map<std::wstring, std::wstring> result;
    std::wistringstream lines(output);
    std::wstring line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (first) { first = false; continue; }
        const std::size_t equals = line.find(L'=');
        if (equals != std::wstring::npos) result[upper(trim(line.substr(0, equals)))] = trim(line.substr(equals + 1));
    }
    return result;
}

int metadataInteger(const std::map<std::wstring, std::wstring>& metadata, const wchar_t* key, int fallback) {
    const auto value = metadata.find(key);
    if (value == metadata.end()) return fallback;
    try { return std::stoi(value->second); } catch (...) { return fallback; }
}
} // namespace

void PluginEngine::setRoot(fs::path root) {
    std::lock_guard<std::mutex> lock(mutex_);
    root_ = std::move(root);
    manifests_.clear();
}

bool PluginEngine::isPluginDevice(const RgbDevice& device) {
    return device.providerId.starts_with(L"plugin:");
}

void PluginEngine::reloadLocked(PluginScanResult& result) {
    manifests_.clear();
    std::error_code error;
    if (root_.empty()) return;
    fs::create_directories(root_, error);
    if (error) return;

    std::vector<fs::path> manifestPaths;
    for (const auto& entry : fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, error)) {
        if (error) { error.clear(); continue; }
        if (entry.is_regular_file() && lower(entry.path().extension().wstring()) == L".rgbcplugin") manifestPaths.push_back(entry.path());
    }
    std::sort(manifestPaths.begin(), manifestPaths.end());

    for (const fs::path& path : manifestPaths) {
        if (iniInteger(path, L"Enabled", 1) == 0) continue;
        Manifest manifest;
        manifest.manifestPath = path;
        manifest.id = iniText(path, L"Id");
        manifest.name = iniText(path, L"Name", L"Unnamed plugin");
        manifest.publisher = iniText(path, L"Publisher", L"Community");
        manifest.transport = lower(iniText(path, L"Transport", L"hid"));
        manifest.executableName = iniText(path, L"Executable");
        manifest.sha256 = lower(iniText(path, L"Sha256"));
        manifest.deviceName = iniText(path, L"DeviceName");
        manifest.deviceType = iniText(path, L"DeviceType", L"Périphérique RGB");
        manifest.modes = iniText(path, L"Modes", L"Static");
        manifest.supersedesOpenRgb = iniText(path, L"SupersedesOpenRGB");
        manifest.capabilities = parseCapabilities(iniText(path, L"Capabilities", L"lighting"));
        manifest.zones = std::clamp(iniInteger(path, L"Zones", 1), 1, 1024);
        manifest.leds = std::clamp(iniInteger(path, L"Leds", 1), 1, 100000);
        manifest.timeoutMs = std::clamp(iniInteger(path, L"TimeoutMs", 5000), 250, 30000);
        const int api = iniInteger(path, L"Api", 0);
        const bool vidOk = parseWord16(iniText(path, L"Vid"), manifest.vendorId);
        const bool pidOk = parseWord16(iniText(path, L"Pid"), manifest.productId);

        if (api != kPluginApiVersion) manifest.error = L"API incompatible";
        else if (!safeIdentifier(manifest.id)) manifest.error = L"Identifiant invalide";
        else if (!vidOk || !pidOk) manifest.error = L"VID/PID invalide";
        else if (manifest.executableName.empty() || fs::path(manifest.executableName).is_absolute()) manifest.error = L"Exécutable invalide";
        else {
            manifest.executablePath = path.parent_path() / manifest.executableName;
            if (!pathInside(manifest.executablePath, root_)) manifest.error = L"Chemin refusé";
            else {
                error.clear();
                if (lower(manifest.executablePath.extension().wstring()) != L".exe" || !fs::is_regular_file(manifest.executablePath, error)) manifest.error = L"Exécutable absent";
            }
            if (manifest.error.empty()) {
                if (manifest.sha256.size() != 64 || !std::all_of(manifest.sha256.begin(), manifest.sha256.end(), [](wchar_t character) { return iswxdigit(character); })) manifest.error = L"Empreinte SHA-256 invalide";
                else if (sha256File(manifest.executablePath) != manifest.sha256) manifest.error = L"Empreinte SHA-256 incorrecte";
                else manifest.valid = true;
            }
        }

        PluginProviderStatus status;
        status.id = manifest.id.empty() ? path.stem().wstring() : manifest.id;
        status.name = manifest.name;
        status.publisher = manifest.publisher;
        status.transport = manifest.transport;
        status.capabilities = manifest.capabilities;
        status.valid = manifest.valid;
        status.state = manifest.valid ? L"Validé par SHA-256" : manifest.error;
        if (!manifest.valid) ++result.rejectedPlugins;
        manifests_.push_back(std::move(manifest));
        result.providers.push_back(std::move(status));
    }
}

PluginCommandResult PluginEngine::invokeLocked(const Manifest& manifest, const std::wstring& arguments) const {
    PluginCommandResult result;
    if (!manifest.valid) {
        result.message = manifest.error.empty() ? L"Plugin non validé" : manifest.error;
        return result;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        result.message = L"Impossible de créer le canal du plugin";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring command = quoteArgument(manifest.executablePath.wstring()) + L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : nullInput;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, manifest.executablePath.parent_path().c_str(),
                                        &startup, &process);
    CloseHandle(writePipe);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (!created) {
        CloseHandle(readPipe);
        result.message = L"Démarrage du plugin impossible (erreur " + std::to_wstring(GetLastError()) + L")";
        return result;
    }
    CloseHandle(process.hThread);

    const ULONGLONG started = GetTickCount64();
    std::string output;
    bool finished = false;
    while (!finished) {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            if (!ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || read == 0) break;
            if (output.size() < 64 * 1024) output.append(buffer.data(), std::min<std::size_t>(read, 64 * 1024 - output.size()));
            available -= std::min(available, read);
        }
        if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) finished = true;
        else if (GetTickCount64() - started >= static_cast<ULONGLONG>(manifest.timeoutMs)) {
            TerminateProcess(process.hProcess, 124);
            WaitForSingleObject(process.hProcess, 1000);
            result.timedOut = true;
            finished = true;
        }
    }
    DWORD available = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || read == 0) break;
        if (output.size() < 64 * 1024) output.append(buffer.data(), std::min<std::size_t>(read, 64 * 1024 - output.size()));
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);

    std::wstring response = trim(widen(output));
    result.success = !result.timedOut && exitCode == 0 && response.starts_with(L"RGBCPLUGIN/2\tOK");
    if (result.timedOut) result.message = L"Le plugin n'a pas répondu dans le délai prévu";
    else if (result.success) result.message = response;
    else if (!response.empty()) result.message = response.substr(0, 500);
    else result.message = L"Le plugin a refusé la commande (code " + std::to_wstring(exitCode) + L")";
    return result;
}

PluginScanResult PluginEngine::scan() {
    std::lock_guard<std::mutex> lock(mutex_);
    PluginScanResult result;
    reloadLocked(result);
    if (manifests_.empty()) return result;
    const std::vector<WindowsUsbDevice> windowsDevices = enumerateUsbDevices();
    std::set<std::wstring> claimed;

    for (std::size_t manifestIndex = 0; manifestIndex < manifests_.size(); ++manifestIndex) {
        Manifest& manifest = manifests_[manifestIndex];
        if (!manifest.valid) continue;
        std::wostringstream usbPattern;
        usbPattern << L"VID_" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << manifest.vendorId
                   << L"&PID_" << std::setw(4) << manifest.productId;
        std::wostringstream bluetoothPattern;
        bluetoothPattern << L"VID&0002" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << manifest.vendorId
                         << L"_PID&" << std::setw(4) << manifest.productId;
        for (const WindowsUsbDevice& windowsDevice : windowsDevices) {
            if (windowsDevice.hardwareIds.find(usbPattern.str()) == std::wstring::npos &&
                windowsDevice.hardwareIds.find(bluetoothPattern.str()) == std::wstring::npos) continue;
            const std::wstring claimKey = upper(windowsDevice.instanceId);
            if (claimed.contains(claimKey)) continue;
            const std::wstring arguments = L"--rgbc-api 2 --command probe --instance " + quoteArgument(windowsDevice.instanceId);
            PluginCommandResult probe = invokeLocked(manifest, arguments);
            if (!probe.success) {
                result.providers[manifestIndex].state = L"Matériel trouvé, test refusé : " + probe.message;
                continue;
            }
            const auto metadata = parseResponseMetadata(probe.message);
            RgbDevice device;
            device.index = -1;
            const auto responseName = metadata.find(L"NAME");
            const auto responseVendor = metadata.find(L"VENDOR");
            const auto responseType = metadata.find(L"TYPE");
            device.name = responseName != metadata.end() ? responseName->second
                : !manifest.deviceName.empty() ? manifest.deviceName
                : !windowsDevice.name.empty() ? windowsDevice.name : manifest.name;
            device.vendor = responseVendor != metadata.end() ? responseVendor->second
                : !windowsDevice.vendor.empty() ? windowsDevice.vendor : manifest.publisher;
            device.description = windowsDevice.instanceId;
            device.type = responseType != metadata.end() ? responseType->second : manifest.deviceType;
            device.zones = std::clamp(metadataInteger(metadata, L"ZONES", manifest.zones), 1, 1024);
            device.leds = std::clamp(metadataInteger(metadata, L"LEDS", manifest.leds), 1, 100000);
            device.selected = true;
            device.providerId = L"plugin:" + manifest.id;
            device.capabilities = manifest.capabilities;
            device.modes = split(manifest.modes, L',');
            result.devices.push_back(std::move(device));
            claimed.insert(claimKey);
            ++result.providers[manifestIndex].matchedDevices;
        }
        if (result.providers[manifestIndex].matchedDevices > 0) {
            result.providers[manifestIndex].state = std::to_wstring(result.providers[manifestIndex].matchedDevices) + L" appareil(s) prêt(s)";
            for (const std::wstring& pattern : split(manifest.supersedesOpenRgb, L';')) result.supersededOpenRgbNames.push_back(pattern);
        } else if (result.providers[manifestIndex].state == L"Validé par SHA-256") {
            result.providers[manifestIndex].state = L"Pilote prêt · matériel absent";
        }
    }
    return result;
}

const PluginEngine::Manifest* PluginEngine::manifestForLocked(const RgbDevice& device) const {
    if (!isPluginDevice(device)) return nullptr;
    const std::wstring id = device.providerId.substr(7);
    const auto found = std::find_if(manifests_.begin(), manifests_.end(), [&](const Manifest& manifest) {
        return manifest.valid && manifest.id == id;
    });
    return found == manifests_.end() ? nullptr : &*found;
}

PluginCommandResult PluginEngine::applyStatic(const RgbDevice& device, std::uint32_t rgb, int brightness) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Manifest* manifest = manifestForLocked(device);
    if (!manifest) return {false, false, L"Pilote introuvable"};
    if (!hasCapability(manifest->capabilities, DeviceCapability::Lighting)) return {false, false, L"Le pilote ne permet pas l'éclairage"};
    std::wostringstream color;
    color << std::uppercase << std::hex << std::setw(6) << std::setfill(L'0') << (rgb & 0xffffff);
    const std::wstring arguments = L"--rgbc-api 2 --command static --instance " + quoteArgument(device.description) +
        L" --color " + color.str() + L" --brightness " + std::to_wstring(std::clamp(brightness, 0, 100)) +
        (device.auxiliaryLedMask >= 0 ? L" --aux-led-mask " + std::to_wstring(device.auxiliaryLedMask) : L"");
    return invokeLocked(*manifest, arguments);
}

PluginCommandResult PluginEngine::applyEffect(const RgbDevice& device, const std::wstring& mode,
                                               std::uint32_t rgb, int brightness, int speed, int intensity) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Manifest* manifest = manifestForLocked(device);
    if (!manifest) return {false, false, L"Pilote introuvable"};
    if (!hasCapability(manifest->capabilities, DeviceCapability::Effects)) return {false, false, L"Le pilote ne permet pas les effets"};
    std::wostringstream color;
    color << std::uppercase << std::hex << std::setw(6) << std::setfill(L'0') << (rgb & 0xffffff);
    const std::wstring arguments = L"--rgbc-api 2 --command effect --instance " + quoteArgument(device.description) +
        L" --mode " + quoteArgument(mode) + L" --color " + color.str() +
        L" --brightness " + std::to_wstring(std::clamp(brightness, 0, 100)) +
        L" --speed " + std::to_wstring(std::clamp(speed, 0, 100)) +
        L" --intensity " + std::to_wstring(std::clamp(intensity, 0, 100)) +
        (device.auxiliaryLedMask >= 0 ? L" --aux-led-mask " + std::to_wstring(device.auxiliaryLedMask) : L"");
    return invokeLocked(*manifest, arguments);
}

PluginCommandResult PluginEngine::stop(const RgbDevice& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Manifest* manifest = manifestForLocked(device);
    if (!manifest) return {false, false, L"Pilote introuvable"};
    const std::wstring arguments = L"--rgbc-api 2 --command stop --instance " + quoteArgument(device.description);
    return invokeLocked(*manifest, arguments);
}
