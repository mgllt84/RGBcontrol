#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <shellapi.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wincrypt.h>

#include "openrgb.hpp"
#include "audio_loopback.hpp"
#include "screen_capture.hpp"
#include "plugin_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {
constexpr UINT WM_SCAN_COMPLETE = WM_APP + 1;
constexpr UINT WM_ASYNC_STATUS = WM_APP + 2;
constexpr UINT WM_UPDATE_COMPLETE = WM_APP + 3;
constexpr UINT WM_FAN_COMPLETE = WM_APP + 4;
constexpr UINT WM_TRAY_ICON = WM_APP + 5;
constexpr UINT WM_CERTIFICATION_COMPLETE = WM_APP + 6;
constexpr UINT_PTR APP_TIMER = 7;
constexpr UINT TRAY_SHOW = 4101;
constexpr UINT TRAY_PROFILE_FIRST = 4110;
constexpr UINT TRAY_EXIT = 4199;
constexpr int kHeaderHeight = 68;
constexpr int kSidebarWidth = 204;
constexpr wchar_t kAppVersion[] = L"0.16.18";
constexpr wchar_t kOfficialUpdateManifestUrl[] =
    L"https://github.com/mgllt84/RGBcontrol/releases/latest/download/RGBCcontrol-update.ini";
constexpr double kLaunchDurationMs = 2750.0;

enum class Page { Dashboard, Effects, Profiles, Fans, Diagnostics, Settings, Devices, DuckyAssistant, Compatibility, Gamepads };
enum class Language { French, English, German, Chinese };
enum class FanProfile { Auto, Quiet, Balanced, Performance, Custom };
enum class Action {
    None, NavDashboard, NavDevices, NavGamepads, NavEffects, NavProfiles, NavFans, NavDiagnostics, NavSettings, Donate, Refresh,
    HeaderStatus, HeaderUpdate, Minimize, Maximize, Close,
    DeviceOpenEffects, DeviceOpenFans, DeviceOpenDiagnostics, DeviceOpenDuckyAssistant, DeviceOpenCompatibility,
    ToggleRgb, PickColor, SetColor, ApplyColor, Brightness,
    SelectEffect, EffectSpeed, EffectIntensity, ApplyEffect, AmbientMonitor, AmbientZones, AmbientSaturation,
    FanSlider, FanApply, FanAuto, FanProfileAuto, FanProfileQuiet, FanProfileBalanced,
    FanProfilePerformance, FanAdmin, FanCurveToggle, FanCurvePreset, FanCurvePoint,
    ProfileSave, ProfileLoad, ProfileDelete, ProfileExport, ProfileImport, DiagnosticCopy,
    ToggleStartup, ToggleMinimizeToTray, ScheduleToggle, ScheduleDayProfile, ScheduleNightProfile,
    ScheduleDayHourDown, ScheduleDayHourUp, ScheduleNightHourDown, ScheduleNightHourUp,
    SelectLanguage, Update, SelectAccent, SelectScanInterval, SelectEffectQuality,
    ToggleStartupQuiet, ToggleCloseToTray, ToggleRememberPage, ToggleReduceMotion, ResetPreferences,
    PickerBackdrop, PickerWheel, PickerBrightness, PickerPreset, PickerCancel, PickerApply,
    DuckyBack, DuckyMode, DuckyPrevious, DuckyNext, DuckySync, DuckyOpenManual,
    DuckyCalibrationNextMode, DuckyCalibrationRestart, CompatibilityBack, CompatibilityRun,
    CertificationStart, CertificationYes, CertificationNo,
    GamepadTogglePlayerLeds, GamepadApplyColor, GamepadUseNative, GamepadUseXbox,
    GamepadRotate, GamepadResetView
};

struct HitTarget {
    RectF rect;
    Action action = Action::None;
    int index = -1;
    std::uint32_t value = 0;
};

struct DeferredTextCommand {
    std::wstring value;
    RectF rect;
    float size = 0;
    Color color;
    FontStyle style = FontStyleRegular;
    StringAlignment horizontal = StringAlignmentNear;
    StringAlignment vertical = StringAlignmentNear;
    bool wrapped = false;
};

struct FanDevice {
    std::string encodedId;
    std::wstring name;
    std::wstring hardware;
    double rpm = -1;
    double percent = -1;
    int minimum = 30;
    int desired = 30;
    bool controllable = false;
    bool manual = false;
};

struct ScanResult {
    std::vector<RgbDevice> rgb;
    std::vector<PluginProviderStatus> pluginProviders;
    std::vector<FanDevice> fans;
    double cpuTemperature = -1;
    double gpuTemperature = -1;
    bool ducky = false;
    bool openRgbReady = false;
    int rejectedPlugins = 0;
    std::wstring error;
};

struct AppProfile {
    bool saved = false;
    std::uint32_t color = 0x7C5CFF;
    int brightness = 80;
    int effect = 0;
    int effectSpeed = 55;
    int effectIntensity = 80;
    int ambientSaturation = 68;
    int ambientMonitor = 0;
    bool ambientZones = true;
    FanProfile fanProfile = FanProfile::Auto;
    bool fanCurveEnabled = false;
    std::array<int, 4> curveSpeeds{30, 45, 70, 100};
    std::wstring selectedDevices;
    std::wstring fanValues;
};

struct EffectInfo {
    const wchar_t* labels[4];
    const wchar_t* internal;
    const wchar_t* descriptions[4];
    std::uint32_t color;
};

struct DuckyModeInfo {
    const wchar_t* labels[4];
    const wchar_t* descriptions[4];
    int matchingEffect;
    bool adjustableColor;
};

const EffectInfo kEffects[] = {
    {{L"Couleur fixe", L"Static color", L"Feste Farbe", L"固定颜色"}, L"Static", {L"Sobre et uniforme", L"Clean and uniform", L"Klar und gleichmäßig", L"简洁均匀"}, 0x9A7CFF},
    {{L"Respiration", L"Breathing", L"Atmen", L"呼吸"}, L"Breathing", {L"Pulsation douce", L"Soft pulse", L"Sanftes Pulsieren", L"柔和脉动"}, 0xFF73AC},
    {{L"Arc-en-ciel", L"Rainbow", L"Regenbogen", L"彩虹"}, L"Rainbow", {L"Toutes les couleurs", L"All colors", L"Alle Farben", L"所有颜色"}, 0x36D8FF},
    {{L"Vague", L"Wave", L"Welle", L"波浪"}, L"Wave", {L"Mouvement fluide", L"Smooth movement", L"Fließende Bewegung", L"流畅运动"}, 0x3DE1C2},
    {{L"Cycle spectral", L"Spectrum cycle", L"Spektrumzyklus", L"光谱循环"}, L"Spectrum Cycle", {L"Transition continue", L"Continuous transition", L"Fließender Übergang", L"连续过渡"}, 0xFFC95C},
    {{L"Stroboscope", L"Strobe", L"Stroboskop", L"频闪"}, L"Strobe", {L"Flash dynamique", L"Dynamic flash", L"Dynamischer Blitz", L"动态闪光"}, 0xFFFFFF},
    {{L"Aléatoire", L"Random", L"Zufällig", L"随机"}, L"Random", {L"Surprise permanente", L"Always surprising", L"Immer überraschend", L"持续惊喜"}, 0xFF8271},
    {{L"Musique", L"Music", L"Musik", L"音乐"}, L"Music", {L"Réaction au son", L"Reacts to sound", L"Reagiert auf Klang", L"随声音变化"}, 0xD779FF},
    {{L"Néon", L"Neon", L"Neon", L"霓虹"}, L"Neon", {L"Lueur électrique", L"Electric glow", L"Elektrisches Leuchten", L"电光效果"}, 0x35F2E2},
    {{L"Eau", L"Water", L"Wasser", L"水波"}, L"Water", {L"Reflets apaisants", L"Calming reflections", L"Ruhige Reflexe", L"舒缓倒影"}, 0x459EFF},
    {{L"Scanner", L"Scanner", L"Scanner", L"扫描"}, L"Scan", {L"Balayage lumineux", L"Light sweep", L"Lichtlauf", L"灯光扫描"}, 0x7FFF90},
    {{L"Empilement", L"Stack", L"Stapel", L"堆叠"}, L"Stack", {L"Construction progressive", L"Progressive build", L"Schrittweiser Aufbau", L"渐进构建"}, 0xFF9B54},
    {{L"Dégradé fluide", L"Smooth gradient", L"Sanfter Verlauf", L"平滑渐变"}, L"Gradient", {L"Couleurs sans coupure", L"Seamless colors", L"Farben ohne Sprung", L"无缝色彩过渡"}, 0xA66BFF},
    {{L"Ambilight", L"Ambilight", L"Ambilight", L"屏幕氛围灯"}, L"Ambilight", {L"Couleurs de l'écran", L"Screen colors", L"Bildschirmfarben", L"跟随屏幕颜色"}, 0x43A7FF}
};

static constexpr std::uint32_t kAccentChoices[] = {
    0x7C5CFF, 0x2F8CFF, 0x00CDB4, 0xF05B9D, 0xF05262, 0xFF9B42, 0x50C878, 0xE6EAF2
};

const DuckyModeInfo kDuckyModes[] = {
    {{L"Couleur fixe", L"Static color", L"Feste Farbe", L"固定颜色"},
     {L"Éclairage uniforme", L"Uniform lighting", L"Gleichmäßige Beleuchtung", L"均匀灯光"}, 0, true},
    {{L"Respiration", L"Breathing", L"Atmen", L"呼吸"},
     {L"Pulsation douce", L"Soft pulse", L"Sanftes Pulsieren", L"柔和脉动"}, 1, true},
    {{L"Cycle couleurs", L"Color cycle", L"Farbzyklus", L"颜色循环"},
     {L"Le spectre entier", L"The full spectrum", L"Das ganze Spektrum", L"完整光谱"}, 4, false},
    {{L"Vague", L"Wave", L"Welle", L"波浪"},
     {L"Déplacement latéral", L"Sideways motion", L"Seitliche Bewegung", L"横向移动"}, 3, false},
    {{L"Réactif", L"Reactive", L"Reaktiv", L"响应"},
     {L"Réagit à la frappe", L"Reacts to typing", L"Reagiert auf Tastendruck", L"响应按键"}, 10, true},
    {{L"Éteint", L"Lights off", L"Beleuchtung aus", L"关闭灯光"},
     {L"Aucun rétroéclairage", L"No backlighting", L"Keine Beleuchtung", L"无背光"}, 0, false}
};

HWND g_window = nullptr;
ULONG_PTR g_gdiplusToken = 0;
std::unique_ptr<Image> g_logo;
std::unique_ptr<Image> g_dualSenseImage;
std::unique_ptr<Bitmap> g_dualSensePreview;
struct DualSenseMeshVertex {
    float x = 0;
    float y = 0;
    float z = 0;
    float u = 0;
    float v = 0;
    float nx = 0;
    float ny = 0;
    float nz = 1;
    float tx = 1;
    float ty = 0;
    float tz = 0;
    float bx = 0;
    float by = 1;
    float bz = 0;
    // Connected-surface identifier from the Collada topology. It lets the
    // live renderer move a button cap without translating the shell around it.
    std::uint32_t component = UINT32_MAX;
};
struct DualSenseMeshTriangle {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint8_t material = 0;
};
struct DualSenseMesh {
    std::vector<DualSenseMeshVertex> vertices;
    std::vector<DualSenseMeshTriangle> triangles;
    float centerX = 0;
    float centerY = 0;
    float centerZ = 0;
    float scale = 1;
    bool loaded = false;
};
struct DualSenseTexture {
    std::vector<BYTE> pixels;
    int width = 0;
    int height = 0;
    bool loaded = false;
};
DualSenseMesh g_dualSenseMesh;
struct DualSenseLightAnchor { float x = 0.0f, y = 0.0f, z = 0.0f; };
std::array<DualSenseLightAnchor, 6> g_dualSenseTouchpadLeftEdge{{
    {-0.36f, 0.55f, 0.31f}, {-0.39f, 0.48f, 0.315f}, {-0.395f, 0.40f, 0.32f},
    {-0.375f, 0.32f, 0.324f}, {-0.34f, 0.24f, 0.327f}, {-0.30f, 0.19f, 0.329f}
}};
std::array<DualSenseLightAnchor, 6> g_dualSenseTouchpadRightEdge{{
    {0.36f, 0.55f, 0.31f}, {0.39f, 0.48f, 0.315f}, {0.395f, 0.40f, 0.32f},
    {0.375f, 0.32f, 0.324f}, {0.34f, 0.24f, 0.327f}, {0.30f, 0.19f, 0.329f}
}};
std::array<std::uint32_t, 15> g_dualSenseControlComponents{};
std::array<std::uint32_t, 4> g_dualSenseDpadComponents{};
std::array<std::vector<std::uint32_t>, 15> g_dualSenseControlComponentGroups;
std::array<std::vector<std::uint32_t>, 4> g_dualSenseDpadComponentGroups;
std::set<std::uint32_t> g_dualSenseDynamicComponents;
std::vector<DualSenseMeshTriangle> g_dualSenseDynamicTriangles;
std::array<DualSenseTexture, 3> g_dualSenseTextures;
std::array<DualSenseTexture, 3> g_dualSenseNormalTextures;
std::array<DualSenseTexture, 3> g_dualSenseRoughnessTextures;
std::array<DualSenseTexture, 3> g_dualSenseMetallicTextures;
std::unique_ptr<Bitmap> g_dualSenseModelCache;
std::unique_ptr<Bitmap> g_dualSenseControlCache;
std::unique_ptr<Bitmap> g_dualSenseMovingControlCache;
float g_dualSenseModelCacheYaw = 999.0f;
float g_dualSenseModelCachePitch = 999.0f;
int g_dualSenseModelCacheWidth = 0;
int g_dualSenseModelCacheHeight = 0;
std::vector<float> g_dualSenseModelDepthCache;
float g_dualSenseControlCacheYaw = 999.0f;
float g_dualSenseControlCachePitch = 999.0f;
int g_dualSenseControlCacheWidth = 0;
int g_dualSenseControlCacheHeight = 0;
std::uint64_t g_dualSenseControlCacheInputSignature = UINT64_MAX;
float g_dualSenseMovingControlCacheYaw = 999.0f;
float g_dualSenseMovingControlCachePitch = 999.0f;
int g_dualSenseMovingControlCacheWidth = 0;
int g_dualSenseMovingControlCacheHeight = 0;
std::uint64_t g_dualSenseMovingControlCacheInputSignature = UINT64_MAX;
std::unique_ptr<Bitmap> g_gamepadPageCache;
int g_gamepadPageCacheWidth = 0;
int g_gamepadPageCacheHeight = 0;
float g_gamepadPageCacheScroll = -1.0f;
bool g_gamepadFastPaintRequested = false;
ULONGLONG g_gamepadLastFullPaintAt = 0;
float g_gamepadYaw = 0.0f;
float g_gamepadPitch = 0.12f;
int g_gamepadLastMouseX = 0;
int g_gamepadLastMouseY = 0;
bool g_gamepadDragging = false;
RectF g_gamepadModelRect;
fs::path g_appDirectory;
Page g_page = Page::Dashboard;
Language g_language = Language::French;
FanProfile g_fanProfile = FanProfile::Auto;
std::vector<HitTarget> g_hits;
std::vector<DeferredTextCommand> g_deferredText;
bool g_collectOpaqueText = false;
std::vector<RgbDevice> g_rgbDevices;
PluginEngine g_pluginEngine;
std::vector<PluginProviderStatus> g_pluginProviders;
int g_rejectedPlugins = 0;
std::vector<FanDevice> g_fans;
std::set<std::string> g_modifiedFans;
std::set<std::string> g_busyFans;
std::wstring g_status = L"Initialisation de RGBCcontrol...";
std::wstring g_fanStatus = L"Recherche des ventilateurs...";
double g_cpuTemperature = -1;
double g_gpuTemperature = -1;
bool g_duckyDetected = false;
int g_duckyMode = 0;
int g_duckyAssistantStep = 0;
int g_duckyGuideStage = 0;
int g_duckyCalibrationPhase = 0;
int g_duckyCalibrationAttempt = 0;
bool g_duckyModeConfirmed = false;
std::wstring g_duckyStatus;
struct DualSenseLiveState {
    bool seen = false;
    bool edge = false;
    bool bluetooth = false;
    std::array<bool, 16> buttons{};
    std::uint8_t leftX = 128;
    std::uint8_t leftY = 128;
    std::uint8_t rightX = 128;
    std::uint8_t rightY = 128;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    int dpad = 8;
    ULONGLONG lastInputAt = 0;
};
DualSenseLiveState g_dualSenseLive;
struct DualSenseVisualAxes {
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    ULONGLONG updatedAt = 0;
    bool initialized = false;
};
DualSenseVisualAxes g_dualSenseVisualAxes;
bool g_dualSensePlayerLedsEnabled = true;
bool g_captureSuppressDualSenseLightOverlays = false;
bool g_gamepadRawInputReady = false;
ULONGLONG g_lastGamepadFrameAt = 0;
enum class XboxBridgeStatus { Off, Starting, Ready, Error };
bool g_xboxModeEnabled = false;
XboxBridgeStatus g_xboxBridgeStatus = XboxBridgeStatus::Off;
std::wstring g_xboxBridgeMessage;
HANDLE g_xboxBridgeProcess = nullptr;
HANDLE g_xboxBridgeInput = nullptr;
ULONGLONG g_xboxBridgeStartedAt = 0;
bool parseDualSenseInputReport(const BYTE* report, std::size_t size, bool bluetooth, bool edge,
                               DualSenseLiveState& state);
bool updateDualSenseVisualAxes(ULONGLONG now, bool snap = false);
bool g_openRgbReady = false;
bool g_isAdministrator = false;
std::uint32_t g_baseColor = 0x7C5CFF;
int g_brightness = 80;
int g_selectedEffect = 0;
std::atomic<int> g_effectSpeed{55};
std::atomic<int> g_effectIntensity{80};
std::atomic<bool> g_effectActive{false};
std::atomic<int> g_activeEffectIndex{0};
ULONGLONG g_effectStartedAt = 0;
std::atomic<double> g_audioVolume{0.0};
std::atomic<double> g_audioBass{0.0};
std::atomic<double> g_audioMid{0.0};
std::atomic<double> g_audioTreble{0.0};
std::atomic<bool> g_audioCaptureReady{false};
std::atomic<bool> g_audioSignal{false};
std::vector<ScreenMonitorInfo> g_screenMonitors;
int g_ambientMonitorIndex = 0;
std::atomic<int> g_ambientSaturation{68};
std::atomic<bool> g_ambientZones{true};
std::atomic<bool> g_ambientCaptureReady{false};
std::atomic<double> g_ambientFps{0.0};
std::array<std::atomic<std::uint32_t>, 4> g_ambientColors{};
std::atomic<std::uint32_t> g_ambientAverage{0};
std::atomic<bool> g_compatibilityRunning{false};
std::atomic<bool> g_compatibilityFinished{false};
std::atomic<int> g_compatibilityProgress{0};
std::atomic<int> g_compatibilityStep{0};
std::atomic<bool> g_compatOpenRgb{false};
std::atomic<bool> g_compatAudio{false};
std::atomic<bool> g_compatScreen{false};
std::atomic<bool> g_compatHardware{false};
std::atomic<bool> g_running{true};
std::atomic<bool> g_scanInFlight{false};
std::thread g_scanThread;
std::thread g_effectThread;
std::thread g_compatibilityThread;
std::thread g_certificationThread;
std::vector<std::thread> g_fanThreads;
std::vector<std::thread> g_pluginThreads;
HANDLE g_ownedOpenRgbProcess = nullptr;
std::mutex g_serverMutex;
std::mutex g_hardwareMutex;
HANDLE g_hardwareProcess = nullptr;
HANDLE g_hardwareInput = nullptr;
HANDLE g_hardwareOutput = nullptr;
std::string g_hardwareBuffer;
ULONGLONG g_lastScan = 0;
float g_scrollOffset = 0;
float g_maxScroll = 0;
RectF g_scrollThumb;
bool g_scrollDragging = false;
float g_scrollDragOrigin = 0;
float g_scrollOffsetOrigin = 0;
Action g_dragAction = Action::None;
int g_dragIndex = -1;
Action g_hoverAction = Action::None;
int g_hoverIndex = -1;
ULONGLONG g_hoverStartedAt = 0;
ULONGLONG g_launchAnimationStartedAt = 0;
ULONGLONG g_lastAmbientFrameAt = 0;
ULONGLONG g_lastEffectUiFrameAt = 0;
bool g_launchAnimationFinished = true;
double g_launchPreviewMs = -1.0;
int g_headerHealthState = -1;
ULONGLONG g_headerStatusChangedAt = 0;
int g_pendingFanProfile = -1;
std::string g_pendingFanId;
int g_pendingFanValue = 30;
bool g_pendingFanAutomatic = false;
bool g_colorPickerOpen = false;
double g_pickerHue = 255.0;
double g_pickerSaturation = 0.64;
int g_pickerBrightness = 100;
std::atomic<bool> g_updateInFlight{false};
std::wstring g_updateStatus;
fs::path g_downloadedUpdate;
bool g_justUpdated = false;
std::array<AppProfile, 3> g_profiles;
int g_activeProfile = -1;
std::wstring g_profileStatus;
bool g_fanCurveEnabled = false;
int g_curvePreset = 1;
std::array<int, 4> g_curveSpeeds{30, 45, 70, 100};
int g_curveTarget = -1;
bool g_pendingFanCurve = false;
int g_pendingSavedProfile = -1;
std::wstring g_diagnosticStatus;
NOTIFYICONDATAW g_trayIcon{};
bool g_trayAdded = false;
bool g_startHidden = false;
bool g_startupEnabled = false;
bool g_minimizeToTray = true;
bool g_startupQuiet = true;
bool g_closeToTray = false;
bool g_rememberLastPage = true;
bool g_reduceMotion = false;
int g_accentPreset = 0;
int g_detectionIntervalSeconds = 5;
std::atomic<int> g_effectQuality{1};
bool g_scheduleEnabled = false;
int g_scheduleDayHour = 8;
int g_scheduleNightHour = 22;
int g_scheduleDayProfile = 0;
int g_scheduleNightProfile = 2;
int g_lastScheduledProfile = -1;
ULONGLONG g_lastScheduleCheck = 0;
bool g_hasCompletedScan = false;
std::wstring g_scheduleStatus;
HDEVNOTIFY g_deviceNotification = nullptr;
bool g_hotplugMonitoring = false;
bool g_hotplugScanPending = false;
bool g_hotplugScanActive = false;
ULONGLONG g_hotplugEventAt = 0;
std::wstring g_hotplugStatus;
int g_certificationDevice = -1;
int g_certificationStage = 0;
int g_certificationOrderAttempt = 0;
int g_certificationBaseOrder = 0;
int g_certificationTransportAttempt = 0;
int g_certificationRefreshAttempt = 0;
bool g_certificationMotionTest = false;
bool g_certificationApplying = false;
bool g_certificationAwaitingAnswer = false;
std::wstring g_certificationStatus;

struct UpdateResult {
    std::wstring status;
    fs::path installer;
};

struct FanCommandResult {
    std::string id;
    std::wstring status;
    bool success = false;
    bool automatic = false;
};

struct CertificationCommandResult {
    int device = -1;
    int stage = 0;
    bool motion = false;
    bool success = false;
    std::wstring message;
};

enum class LocalCertification { Untested, Passed, Failed };

const wchar_t* colorOrderName(RgbColorOrder order) {
    static const wchar_t* names[] = {L"RGB", L"RBG", L"GRB", L"GBR", L"BRG", L"BGR"};
    return names[std::clamp(static_cast<int>(order), 0, 5)];
}

const wchar_t* transportName(RgbFrameTransport transport) {
    if (transport == RgbFrameTransport::AtomicDevice) return L"Atomic";
    if (transport == RgbFrameTransport::PerZone) return L"Per-zone";
    return L"Auto";
}

const wchar_t* localized(const wchar_t* french, const wchar_t* english, const wchar_t* german, const wchar_t* chinese) {
    switch (g_language) {
        case Language::English: return english;
        case Language::German: return german;
        case Language::Chinese: return chinese;
        default: return french;
    }
}

const wchar_t* localizedFor(Language language, const wchar_t* french, const wchar_t* english,
                            const wchar_t* german, const wchar_t* chinese) {
    switch (language) {
        case Language::English: return english;
        case Language::German: return german;
        case Language::Chinese: return chinese;
        default: return french;
    }
}

// The supplied DualSense asset is a Collada scene.  The native client does
// not depend on a heavyweight 3D runtime, so we keep a compact, sampled mesh
// and render it with GDI+.  Sampling the dense CAD export keeps the view
// responsive while retaining the complete silhouette, including the rear.
void parseMeshNumbers(const std::string& xml, std::size_t begin, std::size_t end,
                      std::vector<float>& values) {
    const char* cursor = xml.data() + begin;
    const char* limit = xml.data() + end;
    while (cursor < limit) {
        while (cursor < limit && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) ++cursor;
        if (cursor >= limit) break;
        char* next = nullptr;
        const float value = std::strtof(cursor, &next);
        if (next == cursor || next > limit) {
            ++cursor;
            continue;
        }
        values.push_back(value);
        cursor = next;
    }
}

void parseMeshIntegers(const std::string& xml, std::size_t begin, std::size_t end,
                       std::vector<int>& values) {
    const char* cursor = xml.data() + begin;
    const char* limit = xml.data() + end;
    while (cursor < limit) {
        while (cursor < limit && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) ++cursor;
        if (cursor >= limit) break;
        char* next = nullptr;
        const long value = std::strtol(cursor, &next, 10);
        if (next == cursor || next > limit) {
            ++cursor;
            continue;
        }
        values.push_back(static_cast<int>(value));
        cursor = next;
    }
}

bool parseMeshArray(const std::string& xml, const std::string& id, std::vector<float>& values) {
    const std::string marker = "<float_array id=\"" + id + "\"";
    const std::size_t open = xml.find(marker);
    if (open == std::string::npos) return false;
    const std::size_t begin = xml.find('>', open);
    const std::size_t end = xml.find("</float_array>", begin == std::string::npos ? open : begin);
    if (begin == std::string::npos || end == std::string::npos) return false;
    parseMeshNumbers(xml, begin + 1, end, values);
    return !values.empty();
}

bool loadDualSenseTexture(const fs::path& path, DualSenseTexture& texture) {
    texture = {};
    std::unique_ptr<Image> source(Image::FromFile(path.c_str()));
    if (!source || source->GetLastStatus() != Ok || source->GetWidth() == 0 || source->GetHeight() == 0) return false;
    const int longest = std::max(source->GetWidth(), source->GetHeight());
    const float ratio = longest > 1024 ? 1024.0f / static_cast<float>(longest) : 1.0f;
    texture.width = std::max(1, static_cast<int>(std::lround(source->GetWidth() * ratio)));
    texture.height = std::max(1, static_cast<int>(std::lround(source->GetHeight() * ratio)));
    Bitmap bitmap(texture.width, texture.height, PixelFormat32bppARGB);
    if (bitmap.GetLastStatus() != Ok) return false;
    Graphics bitmapGraphics(&bitmap);
    bitmapGraphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    bitmapGraphics.DrawImage(source.get(), RectF(0, 0, static_cast<REAL>(texture.width), static_cast<REAL>(texture.height)));
    Rect lockRect(0, 0, texture.width, texture.height);
    BitmapData data{};
    if (bitmap.LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return false;
    texture.pixels.resize(static_cast<std::size_t>(texture.width) * static_cast<std::size_t>(texture.height) * 4);
    const int stride = std::abs(data.Stride);
    for (int y = 0; y < texture.height; ++y) {
        const BYTE* sourceRow = static_cast<const BYTE*>(data.Scan0) + static_cast<std::size_t>(y) * stride;
        std::memcpy(texture.pixels.data() + static_cast<std::size_t>(y) * texture.width * 4,
                    sourceRow, static_cast<std::size_t>(texture.width) * 4);
    }
    bitmap.UnlockBits(&data);
    texture.loaded = true;
    return true;
}

bool loadDualSenseMesh(const fs::path& path) {
    g_dualSenseMesh = {};
    g_dualSenseControlComponents.fill(UINT32_MAX);
    g_dualSenseDpadComponents.fill(UINT32_MAX);
    for (auto& components : g_dualSenseControlComponentGroups) components.clear();
    for (auto& components : g_dualSenseDpadComponentGroups) components.clear();
    g_dualSenseDynamicComponents.clear();
    g_dualSenseDynamicTriangles.clear();
    g_dualSenseModelDepthCache.clear();
    g_dualSenseControlCache.reset();
    g_dualSenseControlCacheInputSignature = UINT64_MAX;
    g_dualSenseMovingControlCache.reset();
    g_dualSenseMovingControlCacheInputSignature = UINT64_MAX;
    g_dualSenseTextures = {};
    g_dualSenseNormalTextures = {};
    g_dualSenseRoughnessTextures = {};
    g_dualSenseMetallicTextures = {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) return false;
    std::string xml(static_cast<std::size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    file.read(xml.data(), size);
    if (!file) return false;

    // Keep every source face.  The model is rendered once into a cache for
    // each angle, so the dense CAD export stays watertight without costing a
    // frame of work while the page is idle.
    constexpr std::size_t kTargetTrianglesPerPart = 60000;
    std::vector<std::uint32_t> componentParent;
    auto findComponent = [&](std::uint32_t index) {
        std::uint32_t root = index;
        while (root < componentParent.size() && componentParent[root] != root) root = componentParent[root];
        while (index < componentParent.size() && componentParent[index] != index) {
            const std::uint32_t next = componentParent[index];
            componentParent[index] = root;
            index = next;
        }
        return root;
    };
    auto unionComponents = [&](std::uint32_t first, std::uint32_t second) {
        const std::uint32_t rootFirst = findComponent(first);
        const std::uint32_t rootSecond = findComponent(second);
        if (rootFirst != rootSecond && rootSecond < componentParent.size()) componentParent[rootSecond] = rootFirst;
    };
    for (int part = 0; part < 3; ++part) {
        const std::string geometryMarker = "<geometry id=\"meshId" + std::to_string(part) + "\"";
        const std::size_t geometryStart = xml.find(geometryMarker);
        if (geometryStart == std::string::npos) continue;
        const std::size_t geometryEnd = xml.find("</geometry>", geometryStart);
        if (geometryEnd == std::string::npos) continue;
        const std::string positionId = "meshId" + std::to_string(part) + "-positions-array";
        const std::string texcoordId = "meshId" + std::to_string(part) + "-tex0-array";
        const std::string normalId = "meshId" + std::to_string(part) + "-normals-array";
        const std::string tangentId = "meshId" + std::to_string(part) + "-tangents-array";
        const std::string bitangentId = "meshId" + std::to_string(part) + "-bitangents-array";
        std::vector<float> positions;
        std::vector<float> texcoords;
        std::vector<float> normals;
        std::vector<float> tangents;
        std::vector<float> bitangents;
        if (!parseMeshArray(xml, positionId, positions)) continue;
        parseMeshArray(xml, texcoordId, texcoords);
        parseMeshArray(xml, normalId, normals);
        parseMeshArray(xml, tangentId, tangents);
        parseMeshArray(xml, bitangentId, bitangents);
        if (positions.size() < 9) continue;
        const std::uint32_t vertexOffset = static_cast<std::uint32_t>(g_dualSenseMesh.vertices.size());
        const std::size_t vertexCount = positions.size() / 3;
        g_dualSenseMesh.vertices.reserve(g_dualSenseMesh.vertices.size() + vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i) {
            DualSenseMeshVertex vertex;
            vertex.x = positions[i * 3];
            vertex.y = positions[i * 3 + 1];
            vertex.z = positions[i * 3 + 2];
            if (texcoords.size() >= i * 2 + 2) {
                vertex.u = texcoords[i * 2];
                vertex.v = texcoords[i * 2 + 1];
            }
            if (normals.size() >= i * 3 + 3) {
                vertex.nx = normals[i * 3];
                vertex.ny = normals[i * 3 + 1];
                vertex.nz = normals[i * 3 + 2];
            }
            if (tangents.size() >= i * 3 + 3) {
                vertex.tx = tangents[i * 3];
                vertex.ty = tangents[i * 3 + 1];
                vertex.tz = tangents[i * 3 + 2];
            }
            if (bitangents.size() >= i * 3 + 3) {
                vertex.bx = bitangents[i * 3];
                vertex.by = bitangents[i * 3 + 1];
                vertex.bz = bitangents[i * 3 + 2];
            }
            g_dualSenseMesh.vertices.push_back(vertex);
        }
        componentParent.resize(g_dualSenseMesh.vertices.size());
        for (std::uint32_t index = vertexOffset; index < componentParent.size(); ++index) componentParent[index] = index;

        const std::size_t polylistStart = xml.find("<polylist", geometryStart);
        if (polylistStart == std::string::npos || polylistStart >= geometryEnd) continue;
        const std::size_t polylistEnd = xml.find("</polylist>", polylistStart);
        if (polylistEnd == std::string::npos || polylistEnd > geometryEnd) continue;
        int stride = 1;
        std::size_t input = polylistStart;
        while ((input = xml.find("<input", input)) != std::string::npos && input < polylistEnd) {
            const std::size_t inputEnd = xml.find('>', input);
            if (inputEnd == std::string::npos || inputEnd > polylistEnd) break;
            const std::size_t offsetPos = xml.find("offset=\"", input);
            if (offsetPos != std::string::npos && offsetPos < inputEnd) {
                const int offset = std::atoi(xml.c_str() + offsetPos + 8);
                stride = std::max(stride, offset + 1);
            }
            input = inputEnd + 1;
        }
        const std::size_t vcountTag = xml.find("<vcount>", polylistStart);
        const std::size_t vcountEnd = xml.find("</vcount>", vcountTag == std::string::npos ? polylistStart : vcountTag);
        const std::size_t pTag = xml.find("<p>", polylistStart);
        const std::size_t pEnd = xml.find("</p>", pTag == std::string::npos ? polylistStart : pTag);
        if (vcountTag == std::string::npos || vcountEnd == std::string::npos || pTag == std::string::npos || pEnd == std::string::npos) continue;
        std::vector<int> faceSizes;
        std::vector<int> indices;
        parseMeshIntegers(xml, vcountTag + 8, vcountEnd, faceSizes);
        parseMeshIntegers(xml, pTag + 3, pEnd, indices);
        const std::size_t sampleStep = std::max<std::size_t>(1, (faceSizes.size() + kTargetTrianglesPerPart - 1) / kTargetTrianglesPerPart);
        std::size_t cursor = 0;
        for (std::size_t face = 0; face < faceSizes.size(); ++face) {
            const int corners = std::max(0, faceSizes[face]);
            std::vector<std::uint32_t> faceVertices;
            faceVertices.reserve(static_cast<std::size_t>(corners));
            for (int corner = 0; corner < corners; ++corner) {
                if (cursor + static_cast<std::size_t>(stride) > indices.size()) break;
                const int index = indices[cursor];
                cursor += static_cast<std::size_t>(stride);
                if (index >= 0 && static_cast<std::size_t>(index) < vertexCount) {
                    faceVertices.push_back(vertexOffset + static_cast<std::uint32_t>(index));
                }
            }
            if (faceVertices.size() >= 3) {
                for (std::size_t corner = 1; corner < faceVertices.size(); ++corner) {
                    unionComponents(faceVertices[0], faceVertices[corner]);
                }
            }
            if (faceVertices.size() < 3 || face % sampleStep != 0) continue;
            for (std::size_t corner = 1; corner + 1 < faceVertices.size(); ++corner) {
                g_dualSenseMesh.triangles.push_back({faceVertices[0], faceVertices[corner], faceVertices[corner + 1], static_cast<std::uint8_t>(part)});
            }
        }
    }
    if (g_dualSenseMesh.vertices.empty() || g_dualSenseMesh.triangles.empty()) {
        g_dualSenseMesh = {};
        return false;
    }
    for (std::size_t index = 0; index < g_dualSenseMesh.vertices.size(); ++index) {
        g_dualSenseMesh.vertices[index].component = findComponent(static_cast<std::uint32_t>(index));
    }
    auto nearestControlComponent = [&](float x, float y, float radius, float minimumZ) {
        struct Score {
            int totalCount = 0;
            int nearbyCount = 0;
            double sumX = 0.0;
            double sumY = 0.0;
            double sumZ = 0.0;
            float highestZ = -std::numeric_limits<float>::infinity();
        };
        std::unordered_map<std::uint32_t, Score> candidates;
        for (const DualSenseMeshVertex& vertex : g_dualSenseMesh.vertices) {
            if (vertex.component == UINT32_MAX) continue;
            Score& score = candidates[vertex.component];
            ++score.totalCount;
            score.sumX += vertex.x;
            score.sumY += vertex.y;
            score.sumZ += vertex.z;
            score.highestZ = std::max(score.highestZ, vertex.z);
            if (vertex.z < minimumZ) continue;
            const float dx = vertex.x - x;
            const float dy = vertex.y - y;
            if (dx * dx + dy * dy >= radius * radius) continue;
            ++score.nearbyCount;
        }
        std::uint32_t selected = UINT32_MAX;
        float bestDistance = std::numeric_limits<float>::infinity();
        float bestHighestZ = -std::numeric_limits<float>::infinity();
        for (const auto& [component, score] : candidates) {
            if (score.nearbyCount == 0 || score.totalCount == 0) continue;
            const float centerX = static_cast<float>(score.sumX / score.totalCount);
            const float centerY = static_cast<float>(score.sumY / score.totalCount);
            const float dx = centerX - x;
            const float dy = centerY - y;
            const float distance = dx * dx + dy * dy;
            // Several caps contain nested material layers with the same
            // centroid.  In that case choose the outermost/highest layer.
            if (distance < bestDistance - 0.000001f ||
                (std::abs(distance - bestDistance) <= 0.000001f && score.highestZ > bestHighestZ)) {
                selected = component;
                bestDistance = distance;
                bestHighestZ = score.highestZ;
            }
        }
        return selected;
    };
    // Components are selected from the actual CAD topology, so these masks
    // remain correct if the model is resampled at a different triangle rate.
    // Anchors are measured in the CAD model coordinate system.  Keeping them
    // at the cap centroids (instead of using screen-space guesses) means that
    // the same component is animated at every camera angle and resolution.
    g_dualSenseControlComponents[3] = nearestControlComponent(0.622f, 0.423f, 0.085f, 0.285f);
    g_dualSenseControlComponents[2] = nearestControlComponent(0.771f, 0.276f, 0.085f, 0.255f);
    g_dualSenseControlComponents[1] = nearestControlComponent(0.622f, 0.129f, 0.085f, 0.275f);
    g_dualSenseControlComponents[0] = nearestControlComponent(0.476f, 0.276f, 0.085f, 0.295f);
    g_dualSenseControlComponents[10] = nearestControlComponent(-0.318f, -0.001f, 0.14f, 0.32f);
    g_dualSenseControlComponents[11] = nearestControlComponent(0.318f, -0.001f, 0.14f, 0.32f);
    // The CAD export names the two shoulder layers in the opposite order to
    // the physical DualSense.  The raised rear cap is L2/R2; the lower/front
    // cap is L1/R1.  Bind the HID inputs to the physical pieces users see.
    g_dualSenseControlComponents[4] = nearestControlComponent(-0.624f, 0.599f, 0.19f, 0.04f);
    g_dualSenseControlComponents[5] = nearestControlComponent(0.624f, 0.599f, 0.19f, 0.04f);
    g_dualSenseControlComponents[6] = nearestControlComponent(-0.598f, 0.556f, 0.19f, -0.20f);
    g_dualSenseControlComponents[7] = nearestControlComponent(0.598f, 0.556f, 0.19f, -0.20f);
    g_dualSenseControlComponents[8] = nearestControlComponent(-0.467f, 0.496f, 0.09f, 0.245f);
    g_dualSenseControlComponents[9] = nearestControlComponent(0.467f, 0.496f, 0.09f, 0.245f);
    g_dualSenseControlComponents[12] = nearestControlComponent(0.0f, -0.10f, 0.08f, 0.18f);
    g_dualSenseControlComponents[14] = nearestControlComponent(0.0f, -0.17f, 0.07f, 0.13f);
    g_dualSenseControlComponents[13] = nearestControlComponent(0.0f, 0.405f, 0.39f, 0.292f);
    g_dualSenseDpadComponents[0] = nearestControlComponent(-0.620f, 0.374f, 0.085f, 0.275f);
    g_dualSenseDpadComponents[1] = nearestControlComponent(-0.529f, 0.274f, 0.085f, 0.275f);
    g_dualSenseDpadComponents[2] = nearestControlComponent(-0.626f, 0.179f, 0.085f, 0.275f);
    g_dualSenseDpadComponents[3] = nearestControlComponent(-0.721f, 0.280f, 0.085f, 0.275f);

    // Start with the main cap selected above, then attach only the small
    // stacked layers that physically belong to it. The CAD export separates
    // these layers even though they are one moving control on the real pad.
    for (std::size_t index = 0; index < g_dualSenseControlComponents.size(); ++index) {
        if (g_dualSenseControlComponents[index] != UINT32_MAX) {
            g_dualSenseControlComponentGroups[index] = {g_dualSenseControlComponents[index]};
        }
    }

    struct ComponentGroupStats {
        int count = 0;
        double sumX = 0.0, sumY = 0.0;
        float highestZ = -std::numeric_limits<float>::infinity();
    };
    std::unordered_map<std::uint32_t, ComponentGroupStats> groupStats;
    for (const DualSenseMeshVertex& vertex : g_dualSenseMesh.vertices) {
        ComponentGroupStats& stats = groupStats[vertex.component];
        ++stats.count;
        stats.sumX += vertex.x;
        stats.sumY += vertex.y;
        stats.highestZ = std::max(stats.highestZ, vertex.z);
    }
    auto addCenteredLayers = [&](std::vector<std::uint32_t>& group, float x, float y, float radius,
                                 float minimumHighestZ, float maximumHighestZ) {
        for (const auto& [component, stats] : groupStats) {
            if (stats.count <= 0 || stats.highestZ < minimumHighestZ || stats.highestZ > maximumHighestZ) continue;
            const float centerX = static_cast<float>(stats.sumX / stats.count);
            const float centerY = static_cast<float>(stats.sumY / stats.count);
            const float dx = centerX - x;
            const float dy = centerY - y;
            if (dx * dx + dy * dy <= radius * radius) group.push_back(component);
        }
        std::sort(group.begin(), group.end());
        group.erase(std::unique(group.begin(), group.end()), group.end());
    };

    // L2/R2 contain a deep shell plus a thin upper lip. Move both together,
    // while leaving the independent L1/R1 bumper in place.
    addCenteredLayers(g_dualSenseControlComponentGroups[6], -0.615f, 0.594f, 0.020f, -0.25f, 0.025f);
    addCenteredLayers(g_dualSenseControlComponentGroups[6], -0.619f, 0.533f, 0.018f, -0.01f, 0.025f);
    addCenteredLayers(g_dualSenseControlComponentGroups[7], 0.615f, 0.594f, 0.020f, -0.25f, 0.025f);
    addCenteredLayers(g_dualSenseControlComponentGroups[7], 0.619f, 0.533f, 0.018f, -0.01f, 0.025f);

    const float dpadX[] = {-0.620f, -0.529f, -0.626f, -0.721f};
    const float dpadY[] = {0.374f, 0.274f, 0.179f, 0.280f};
    for (std::size_t index = 0; index < g_dualSenseDpadComponents.size(); ++index) {
        if (g_dualSenseDpadComponents[index] != UINT32_MAX) {
            g_dualSenseDpadComponentGroups[index] = {g_dualSenseDpadComponents[index]};
            // Every direction has several coincident material shells. Moving
            // the complete stack makes the east/right arrow visibly depress.
            addCenteredLayers(g_dualSenseDpadComponentGroups[index], dpadX[index], dpadY[index],
                              0.014f, 0.24f, 0.34f);
        }
    }

    // Measure the light path from the actual touchpad boundary. This keeps the
    // emissive ribbons on the seam at every scale instead of relying on hand-
    // tuned screen coordinates.
    const float edgeSamples[] = {0.55f, 0.48f, 0.40f, 0.32f, 0.24f, 0.19f};
    const std::uint32_t touchpadComponent = g_dualSenseControlComponents[13];
    for (std::size_t sample = 0; sample < std::size(edgeSamples); ++sample) {
        const float targetY = edgeSamples[sample];
        const DualSenseMeshVertex* leftEdge = nullptr;
        const DualSenseMeshVertex* rightEdge = nullptr;
        for (const DualSenseMeshVertex& vertex : g_dualSenseMesh.vertices) {
            if (vertex.component != touchpadComponent || std::abs(vertex.y - targetY) > 0.018f || vertex.z < 0.20f) continue;
            if (!leftEdge || vertex.x < leftEdge->x) leftEdge = &vertex;
            if (!rightEdge || vertex.x > rightEdge->x) rightEdge = &vertex;
        }
        if (leftEdge && rightEdge) {
            constexpr float inset = 0.004f;
            constexpr float lift = 0.004f;
            g_dualSenseTouchpadLeftEdge[sample] = {leftEdge->x + inset, targetY, leftEdge->z + lift};
            g_dualSenseTouchpadRightEdge[sample] = {rightEdge->x - inset, targetY, rightEdge->z + lift};
        }
    }
    for (const auto& components : g_dualSenseControlComponentGroups) {
        g_dualSenseDynamicComponents.insert(components.begin(), components.end());
    }
    for (const auto& components : g_dualSenseDpadComponentGroups) {
        g_dualSenseDynamicComponents.insert(components.begin(), components.end());
    }
    for (const DualSenseMeshTriangle& triangle : g_dualSenseMesh.triangles) {
        if (triangle.a < g_dualSenseMesh.vertices.size() &&
            g_dualSenseDynamicComponents.contains(g_dualSenseMesh.vertices[triangle.a].component)) {
            g_dualSenseDynamicTriangles.push_back(triangle);
        }
    }
    float minX = g_dualSenseMesh.vertices.front().x, maxX = minX;
    float minY = g_dualSenseMesh.vertices.front().y, maxY = minY;
    float minZ = g_dualSenseMesh.vertices.front().z, maxZ = minZ;
    for (const DualSenseMeshVertex& vertex : g_dualSenseMesh.vertices) {
        minX = std::min(minX, vertex.x); maxX = std::max(maxX, vertex.x);
        minY = std::min(minY, vertex.y); maxY = std::max(maxY, vertex.y);
        minZ = std::min(minZ, vertex.z); maxZ = std::max(maxZ, vertex.z);
    }
    g_dualSenseMesh.centerX = (minX + maxX) * 0.5f;
    g_dualSenseMesh.centerY = (minY + maxY) * 0.5f;
    g_dualSenseMesh.centerZ = (minZ + maxZ) * 0.5f;
    const float spanX = std::max(0.1f, maxX - minX);
    const float spanY = std::max(0.1f, maxY - minY);
    g_dualSenseMesh.scale = 1.0f / std::max(spanX, spanY);
    g_dualSenseMesh.loaded = true;
    const fs::path textureRoot = path.parent_path() / L"textures";
    loadDualSenseTexture(textureRoot / L"1011_albedo.jpg", g_dualSenseTextures[0]);
    loadDualSenseTexture(textureRoot / L"1001_albedo.jpg", g_dualSenseTextures[1]);
    loadDualSenseTexture(textureRoot / L"1002_albedo.jpg", g_dualSenseTextures[2]);
    loadDualSenseTexture(textureRoot / L"1011_normal.png", g_dualSenseNormalTextures[0]);
    loadDualSenseTexture(textureRoot / L"1001_normal.png", g_dualSenseNormalTextures[1]);
    loadDualSenseTexture(textureRoot / L"1002_normal.png", g_dualSenseNormalTextures[2]);
    loadDualSenseTexture(textureRoot / L"1011_roughness.jpg", g_dualSenseRoughnessTextures[0]);
    loadDualSenseTexture(textureRoot / L"1001_roughness.jpg", g_dualSenseRoughnessTextures[1]);
    loadDualSenseTexture(textureRoot / L"1002_roughness.jpg", g_dualSenseRoughnessTextures[2]);
    loadDualSenseTexture(textureRoot / L"1011_metallic.jpg", g_dualSenseMetallicTextures[0]);
    loadDualSenseTexture(textureRoot / L"1001_metallic.jpg", g_dualSenseMetallicTextures[1]);
    loadDualSenseTexture(textureRoot / L"1002_metallic.jpg", g_dualSenseMetallicTextures[2]);
    return true;
}

const wchar_t* effectLabel(int index) {
    return kEffects[index].labels[static_cast<int>(g_language)];
}

const wchar_t* effectDescription(int index) {
    return kEffects[index].descriptions[static_cast<int>(g_language)];
}

const wchar_t* duckyModeLabel(int index) {
    return kDuckyModes[index].labels[static_cast<int>(g_language)];
}

const wchar_t* duckyModeDescription(int index) {
    return kDuckyModes[index].descriptions[static_cast<int>(g_language)];
}

const wchar_t* duckyFirmwareModeName(int index) {
    static const wchar_t* names[] = {
        L"100% Full Backlit", L"Breathe", L"Color Cycle", L"Wave", L"Reactive", L"Backlit OFF"
    };
    return names[std::clamp(index, 0, static_cast<int>(std::size(names)) - 1)];
}

bool duckyModeUsesSpeed(int index) {
    return index != 0 && index != static_cast<int>(std::size(kDuckyModes)) - 1;
}

int duckyGuideStageCount() {
    return 1 + (kDuckyModes[g_duckyMode].adjustableColor ? 1 : 0) + (duckyModeUsesSpeed(g_duckyMode) ? 1 : 0);
}

int duckyGuideKind() {
    if (g_duckyGuideStage <= 0) return 0;
    if (kDuckyModes[g_duckyMode].adjustableColor && g_duckyGuideStage == 1) return 1;
    return 2;
}

std::wstring localizedDeviceType(const std::wstring& type) {
    static const wchar_t* source[] = {
        L"Carte mere", L"Memoire", L"GPU", L"Refroidissement", L"Ruban LED", L"Clavier", L"Souris", L"Tapis",
        L"Casque", L"Support casque", L"Manette", L"Luminaire", L"Enceinte", L"Virtuel", L"Stockage", L"Boitier",
        L"Microphone", L"Accessoire", L"Pave numerique", L"Portable", L"Ecran", L"Inconnu"
    };
    static const wchar_t* translations[4][22] = {
        {L"Carte mère", L"Mémoire", L"GPU", L"Refroidissement", L"Ruban LED", L"Clavier", L"Souris", L"Tapis",
         L"Casque", L"Support casque", L"Manette", L"Luminaire", L"Enceinte", L"Virtuel", L"Stockage", L"Boîtier",
         L"Microphone", L"Accessoire", L"Pavé numérique", L"Portable", L"Écran", L"Inconnu"},
        {L"Motherboard", L"Memory", L"GPU", L"Cooling", L"LED strip", L"Keyboard", L"Mouse", L"Mouse mat",
         L"Headset", L"Headset stand", L"Controller", L"Light", L"Speaker", L"Virtual", L"Storage", L"Case",
         L"Microphone", L"Accessory", L"Keypad", L"Laptop", L"Display", L"Unknown"},
        {L"Mainboard", L"Speicher", L"GPU", L"Kühlung", L"LED-Streifen", L"Tastatur", L"Maus", L"Mauspad",
         L"Headset", L"Headset-Halter", L"Controller", L"Leuchte", L"Lautsprecher", L"Virtuell", L"Speicherlaufwerk", L"Gehäuse",
         L"Mikrofon", L"Zubehör", L"Ziffernblock", L"Laptop", L"Bildschirm", L"Unbekannt"},
        {L"主板", L"内存", L"GPU", L"散热设备", L"LED 灯带", L"键盘", L"鼠标", L"鼠标垫", L"耳机", L"耳机支架",
         L"手柄", L"灯具", L"扬声器", L"虚拟设备", L"存储设备", L"机箱", L"麦克风", L"配件", L"数字键盘", L"笔记本电脑", L"显示器", L"未知"}
    };
    for (std::size_t index = 0; index < std::size(source); ++index) {
        if (type == source[index]) return translations[static_cast<int>(g_language)][index];
    }
    return type;
}

void loadLanguage() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RGBCcontrol", L"Language", RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS && value <= 3) {
        g_language = static_cast<Language>(value);
    }
}

void saveLanguage() {
    DWORD value = static_cast<DWORD>(g_language);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\RGBCcontrol", L"Language", REG_DWORD, &value, sizeof(value));
}

fs::path profileStorePath(bool createDirectory = false) {
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    fs::path directory = length > 0 && length < buffer.size() ? fs::path(buffer.data()) / L"RGBCcontrol" : g_appDirectory;
    if (createDirectory) {
        std::error_code error;
        fs::create_directories(directory, error);
    }
    return directory / L"profiles.ini";
}

void writeIniInteger(const fs::path& path, const std::wstring& section, const wchar_t* key, int value) {
    const std::wstring textValue = std::to_wstring(value);
    WritePrivateProfileStringW(section.c_str(), key, textValue.c_str(), path.c_str());
}

int readIniInteger(const fs::path& path, const std::wstring& section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section.c_str(), key, fallback, path.c_str()));
}

std::wstring readIniText(const fs::path& path, const std::wstring& section, const wchar_t* key) {
    std::vector<wchar_t> buffer(16384);
    GetPrivateProfileStringW(section.c_str(), key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

fs::path certificationStorePath(bool createDirectory = true) {
    return profileStorePath(createDirectory).parent_path() / L"certifications.ini";
}

std::wstring certificationFingerprint(const RgbDevice& device) {
    std::wstring identity = device.providerId + L"|" + device.vendor + L"|" + device.name + L"|" +
        device.type + L"|" + device.description + L"|" + std::to_wstring(device.zones) + L"|" +
        std::to_wstring(device.leds);
    for (const std::wstring& mode : device.modes) identity += L"|" + mode;
    std::transform(identity.begin(), identity.end(), identity.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    std::uint64_t hash = 1469598103934665603ULL;
    for (wchar_t character : identity) {
        const std::uint32_t value = static_cast<std::uint32_t>(character);
        hash ^= value & 0xffu; hash *= 1099511628211ULL;
        hash ^= (value >> 8) & 0xffu; hash *= 1099511628211ULL;
    }
    std::wostringstream output;
    output << L"Device_" << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    return output.str();
}

LocalCertification localCertification(const RgbDevice& device) {
    const fs::path path = certificationStorePath(false);
    const std::wstring section = certificationFingerprint(device);
    const int value = readIniInteger(path, section, L"Status", 0);
    if (value == 1 && readIniInteger(path, section, L"CalibrationVersion", 0) != 1) return LocalCertification::Untested;
    return value == 1 ? LocalCertification::Passed : value == 2 ? LocalCertification::Failed : LocalCertification::Untested;
}

void saveLocalCertification(const RgbDevice& device, bool passed) {
    const fs::path path = certificationStorePath(true);
    const std::wstring section = certificationFingerprint(device);
    writeIniInteger(path, section, L"Status", passed ? 1 : 2);
    writeIniInteger(path, section, L"CalibrationVersion", 1);
    WritePrivateProfileStringW(section.c_str(), L"Name", device.name.c_str(), path.c_str());
    WritePrivateProfileStringW(section.c_str(), L"Vendor", device.vendor.c_str(), path.c_str());
    WritePrivateProfileStringW(section.c_str(), L"Provider", device.providerId.c_str(), path.c_str());
    writeIniInteger(path, section, L"Transport", static_cast<int>(device.frameTransport));
    writeIniInteger(path, section, L"ColorOrder", static_cast<int>(device.colorOrder));
    writeIniInteger(path, section, L"FrameIntervalMs", std::clamp(device.frameIntervalMs, 20, 200));
    writeIniInteger(path, section, L"PreferredLeds", std::clamp(device.preferredLeds, 1, 4096));
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t date[32]{};
    swprintf(date, std::size(date), L"%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    WritePrivateProfileStringW(section.c_str(), L"Date", date, path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

void loadLocalCalibration(RgbDevice& device) {
    const fs::path path = certificationStorePath(false);
    const std::wstring section = certificationFingerprint(device);
    const int defaultTransport = OpenRgbClient::prefersAtomicFrames(device)
        ? static_cast<int>(RgbFrameTransport::AtomicDevice) : static_cast<int>(RgbFrameTransport::PerZone);
    device.frameTransport = static_cast<RgbFrameTransport>(std::clamp(
        readIniInteger(path, section, L"Transport", defaultTransport), 0, 2));
    device.colorOrder = static_cast<RgbColorOrder>(std::clamp(
        readIniInteger(path, section, L"ColorOrder", 0), 0, 5));
    device.frameIntervalMs = std::clamp(readIniInteger(path, section, L"FrameIntervalMs", 40), 20, 200);
    device.preferredLeds = std::clamp(readIniInteger(path, section, L"PreferredLeds", 120), 1, 4096);
}

std::wstring serializedSelectedDevices() {
    std::wstring result;
    for (const RgbDevice& device : g_rgbDevices) {
        if (!device.selected) continue;
        if (!result.empty()) result += L"||";
        result += device.vendor + L"\t" + device.name;
    }
    return result;
}

std::wstring serializedFanValues() {
    std::wstring result;
    for (const FanDevice& fan : g_fans) {
        if (!fan.controllable) continue;
        if (!result.empty()) result += L"||";
        result.append(fan.encodedId.begin(), fan.encodedId.end());
        result += L"=" + std::to_wstring(fan.desired);
    }
    return result;
}

int serializedFanValue(const std::wstring& list, const std::string& encodedId) {
    const std::wstring prefix(encodedId.begin(), encodedId.end());
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = list.find(L"||", start);
        const std::wstring entry = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (entry.starts_with(prefix + L"=")) return std::clamp(_wtoi(entry.c_str() + prefix.size() + 1), 0, 100);
        if (end == std::wstring::npos) break;
        start = end + 2;
    }
    return -1;
}

bool serializedListContains(const std::wstring& list, const RgbDevice& device) {
    const std::wstring key = device.vendor + L"\t" + device.name;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = list.find(L"||", start);
        if (list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start) == key) return true;
        if (end == std::wstring::npos) break;
        start = end + 2;
    }
    return false;
}

AppProfile captureCurrentProfile() {
    AppProfile profile;
    profile.saved = true;
    profile.color = g_baseColor;
    profile.brightness = g_brightness;
    profile.effect = g_selectedEffect;
    profile.effectSpeed = g_effectSpeed.load();
    profile.effectIntensity = g_effectIntensity.load();
    profile.ambientSaturation = g_ambientSaturation.load();
    profile.ambientMonitor = g_ambientMonitorIndex;
    profile.ambientZones = g_ambientZones.load();
    profile.fanProfile = g_fanProfile;
    profile.fanCurveEnabled = g_fanCurveEnabled;
    profile.curveSpeeds = g_curveSpeeds;
    profile.selectedDevices = serializedSelectedDevices();
    profile.fanValues = serializedFanValues();
    return profile;
}

void writeProfileToIni(const fs::path& path, const std::wstring& section, const AppProfile& profile) {
    writeIniInteger(path, section, L"Saved", profile.saved ? 1 : 0);
    writeIniInteger(path, section, L"Color", static_cast<int>(profile.color));
    writeIniInteger(path, section, L"Brightness", profile.brightness);
    writeIniInteger(path, section, L"Effect", profile.effect);
    writeIniInteger(path, section, L"EffectSpeed", profile.effectSpeed);
    writeIniInteger(path, section, L"EffectIntensity", profile.effectIntensity);
    writeIniInteger(path, section, L"AmbientSaturation", profile.ambientSaturation);
    writeIniInteger(path, section, L"AmbientMonitor", profile.ambientMonitor);
    writeIniInteger(path, section, L"AmbientZones", profile.ambientZones ? 1 : 0);
    writeIniInteger(path, section, L"FanProfile", static_cast<int>(profile.fanProfile));
    writeIniInteger(path, section, L"FanCurveEnabled", profile.fanCurveEnabled ? 1 : 0);
    for (int index = 0; index < 4; ++index) {
        const std::wstring key = L"Curve" + std::to_wstring(index);
        writeIniInteger(path, section, key.c_str(), profile.curveSpeeds[index]);
    }
    WritePrivateProfileStringW(section.c_str(), L"SelectedDevices", profile.selectedDevices.c_str(), path.c_str());
    WritePrivateProfileStringW(section.c_str(), L"FanValues", profile.fanValues.c_str(), path.c_str());
}

AppProfile readProfileFromIni(const fs::path& path, const std::wstring& section) {
    AppProfile profile;
    profile.saved = readIniInteger(path, section, L"Saved", 0) != 0;
    profile.color = static_cast<std::uint32_t>(std::clamp(readIniInteger(path, section, L"Color", 0x7C5CFF), 0, 0xFFFFFF));
    profile.brightness = std::clamp(readIniInteger(path, section, L"Brightness", 80), 0, 100);
    profile.effect = std::clamp(readIniInteger(path, section, L"Effect", 0), 0, static_cast<int>(std::size(kEffects)) - 1);
    profile.effectSpeed = std::clamp(readIniInteger(path, section, L"EffectSpeed", 55), 0, 100);
    profile.effectIntensity = std::clamp(readIniInteger(path, section, L"EffectIntensity", 80), 0, 100);
    profile.ambientSaturation = std::clamp(readIniInteger(path, section, L"AmbientSaturation", 68), 0, 100);
    profile.ambientMonitor = std::max(0, readIniInteger(path, section, L"AmbientMonitor", 0));
    profile.ambientZones = readIniInteger(path, section, L"AmbientZones", 1) != 0;
    profile.fanProfile = static_cast<FanProfile>(std::clamp(readIniInteger(path, section, L"FanProfile", 0), 0, 4));
    profile.fanCurveEnabled = readIniInteger(path, section, L"FanCurveEnabled", 0) != 0;
    for (int index = 0; index < 4; ++index) {
        const std::wstring key = L"Curve" + std::to_wstring(index);
        profile.curveSpeeds[index] = std::clamp(readIniInteger(path, section, key.c_str(), profile.curveSpeeds[index]), 30, 100);
        if (index > 0) profile.curveSpeeds[index] = std::max(profile.curveSpeeds[index], profile.curveSpeeds[index - 1]);
    }
    profile.selectedDevices = readIniText(path, section, L"SelectedDevices");
    profile.fanValues = readIniText(path, section, L"FanValues");
    return profile;
}

void saveProfileSlot(int index) {
    if (index < 0 || index >= static_cast<int>(g_profiles.size())) return;
    g_profiles[index] = captureCurrentProfile();
    const fs::path path = profileStorePath(true);
    writeProfileToIni(path, L"Profile" + std::to_wstring(index + 1), g_profiles[index]);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    g_activeProfile = index;
    g_lastScheduledProfile = -1;
    g_profileStatus = localized(L"Profil enregistré avec toute la configuration.", L"Profile saved with the full configuration.",
                                L"Profil mit der gesamten Konfiguration gespeichert.", L"已保存包含完整配置的模式。");
}

void deleteProfileSlot(int index) {
    if (index < 0 || index >= static_cast<int>(g_profiles.size())) return;
    g_profiles[index] = AppProfile{};
    const fs::path path = profileStorePath(true);
    const std::wstring section = L"Profile" + std::to_wstring(index + 1);
    WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, path.c_str());
    if (g_activeProfile == index) g_activeProfile = -1;
    g_lastScheduledProfile = -1;
    g_profileStatus = localized(L"Profil supprimé.", L"Profile deleted.", L"Profil gelöscht.", L"模式已删除。");
}

void loadStoredProfiles() {
    const fs::path path = profileStorePath(false);
    for (int index = 0; index < static_cast<int>(g_profiles.size()); ++index) {
        g_profiles[index] = readProfileFromIni(path, L"Profile" + std::to_wstring(index + 1));
    }
    AppProfile curve = readProfileFromIni(path, L"CurveSettings");
    if (curve.saved) {
        g_curveSpeeds = curve.curveSpeeds;
        if (g_curveSpeeds == std::array<int, 4>{30, 35, 55, 80}) g_curvePreset = 0;
        else if (g_curveSpeeds == std::array<int, 4>{30, 45, 70, 100}) g_curvePreset = 1;
        else if (g_curveSpeeds == std::array<int, 4>{45, 60, 85, 100}) g_curvePreset = 2;
        else g_curvePreset = -1;
    }
}

void saveCurveSettings() {
    AppProfile curve = captureCurrentProfile();
    curve.saved = true;
    const fs::path path = profileStorePath(true);
    writeProfileToIni(path, L"CurveSettings", curve);
}

bool startupRegistryEnabled() {
    std::vector<wchar_t> value(32768);
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"RGBCcontrol",
                        RRF_RT_REG_SZ, nullptr, value.data(), &bytes) == ERROR_SUCCESS;
}

void setStartupEnabled(bool enabled) {
    if (enabled) {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const std::wstring command = L"\"" + std::wstring(executable) + L"\" --startup";
        const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"RGBCcontrol",
                                               REG_SZ, command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        g_startupEnabled = status == ERROR_SUCCESS;
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"RGBCcontrol");
        g_startupEnabled = false;
    }
}

void saveAutomationSettings() {
    const fs::path path = profileStorePath(true);
    const std::wstring section = L"Automation";
    writeIniInteger(path, section, L"MinimizeToTray", g_minimizeToTray ? 1 : 0);
    writeIniInteger(path, section, L"StartupQuiet", g_startupQuiet ? 1 : 0);
    writeIniInteger(path, section, L"CloseToTray", g_closeToTray ? 1 : 0);
    writeIniInteger(path, section, L"RememberLastPage", g_rememberLastPage ? 1 : 0);
    writeIniInteger(path, section, L"LastPage", static_cast<int>(g_page));
    writeIniInteger(path, section, L"ReduceMotion", g_reduceMotion ? 1 : 0);
    writeIniInteger(path, section, L"AccentPreset", g_accentPreset);
    writeIniInteger(path, section, L"DetectionInterval", g_detectionIntervalSeconds);
    writeIniInteger(path, section, L"EffectQuality", g_effectQuality.load());
    writeIniInteger(path, section, L"DuckyMode", g_duckyMode);
    writeIniInteger(path, section, L"DualSensePlayerLeds", g_dualSensePlayerLedsEnabled ? 1 : 0);
    writeIniInteger(path, section, L"DualSenseXboxMode", g_xboxModeEnabled ? 1 : 0);
    writeIniInteger(path, section, L"ScheduleEnabled", g_scheduleEnabled ? 1 : 0);
    writeIniInteger(path, section, L"DayHour", g_scheduleDayHour);
    writeIniInteger(path, section, L"NightHour", g_scheduleNightHour);
    writeIniInteger(path, section, L"DayProfile", g_scheduleDayProfile);
    writeIniInteger(path, section, L"NightProfile", g_scheduleNightProfile);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

void loadAutomationSettings() {
    const fs::path path = profileStorePath(false);
    const std::wstring section = L"Automation";
    g_startupEnabled = startupRegistryEnabled();
    g_minimizeToTray = readIniInteger(path, section, L"MinimizeToTray", 1) != 0;
    g_startupQuiet = readIniInteger(path, section, L"StartupQuiet", 1) != 0;
    g_closeToTray = readIniInteger(path, section, L"CloseToTray", 0) != 0;
    g_rememberLastPage = readIniInteger(path, section, L"RememberLastPage", 1) != 0;
    g_reduceMotion = readIniInteger(path, section, L"ReduceMotion", 0) != 0;
    g_accentPreset = std::clamp(readIniInteger(path, section, L"AccentPreset", 0), 0,
                                static_cast<int>(std::size(kAccentChoices)) - 1);
    const int storedInterval = readIniInteger(path, section, L"DetectionInterval", 5);
    g_detectionIntervalSeconds = storedInterval == 15 || storedInterval == 30 || storedInterval == 60 ? storedInterval : 5;
    g_effectQuality = std::clamp(readIniInteger(path, section, L"EffectQuality", 1), 0, 2);
    g_duckyMode = std::clamp(readIniInteger(path, section, L"DuckyMode", 0), 0, static_cast<int>(std::size(kDuckyModes)) - 1);
    g_dualSensePlayerLedsEnabled = readIniInteger(path, section, L"DualSensePlayerLeds", 1) != 0;
    g_xboxModeEnabled = readIniInteger(path, section, L"DualSenseXboxMode", 0) != 0;
    if (g_rememberLastPage) {
        g_page = static_cast<Page>(std::clamp(readIniInteger(path, section, L"LastPage", 0), 0, 9));
    }
    g_scheduleEnabled = readIniInteger(path, section, L"ScheduleEnabled", 0) != 0;
    g_scheduleDayHour = std::clamp(readIniInteger(path, section, L"DayHour", 8), 0, 23);
    g_scheduleNightHour = std::clamp(readIniInteger(path, section, L"NightHour", 22), 0, 23);
    g_scheduleDayProfile = std::clamp(readIniInteger(path, section, L"DayProfile", 0), 0, 2);
    g_scheduleNightProfile = std::clamp(readIniInteger(path, section, L"NightProfile", 2), 0, 2);
}

bool selectProfileFile(bool save, fs::path& path) {
    wchar_t filename[MAX_PATH] = L"RGBCcontrol-Profile.rgbcprofile";
    const wchar_t filter[] = L"Profil RGBCcontrol (*.rgbcprofile)\0*.rgbcprofile\0Tous les fichiers (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"rgbcprofile";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!accepted) return false;
    path = filename;
    return true;
}

std::wstring readIniValue(const fs::path& path, const wchar_t* key) {
    std::vector<wchar_t> buffer(4096);
    GetPrivateProfileStringW(L"RGBCcontrol", key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

std::vector<int> versionParts(const std::wstring& version) {
    std::vector<int> result;
    int current = 0;
    bool hasDigits = false;
    for (wchar_t character : version) {
        if (character >= L'0' && character <= L'9') {
            current = std::min(100000, current * 10 + static_cast<int>(character - L'0'));
            hasDigits = true;
        } else if (hasDigits) {
            result.push_back(current);
            current = 0;
            hasDigits = false;
        }
    }
    if (hasDigits) result.push_back(current);
    return result;
}

bool isNewerVersion(const std::wstring& candidate) {
    std::vector<int> remote = versionParts(candidate);
    std::vector<int> local = versionParts(kAppVersion);
    const std::size_t count = std::max(remote.size(), local.size());
    remote.resize(count);
    local.resize(count);
    return remote > local;
}

std::wstring sha256File(const fs::path& path) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (provider) CryptReleaseContext(provider, 0);
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    std::vector<BYTE> buffer(64 * 1024);
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        std::streamsize count = stream.gcount();
        if (count > 0 && !CryptHashData(hash, buffer.data(), static_cast<DWORD>(count), 0)) break;
    }
    BYTE digest[32]{};
    DWORD size = sizeof(digest);
    std::wstring result;
    if (stream.eof() && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0)) {
        std::wostringstream value;
        value << std::hex << std::setfill(L'0');
        for (DWORD index = 0; index < size; ++index) value << std::setw(2) << static_cast<int>(digest[index]);
        result = value.str();
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return result;
}

void postUpdateResult(std::wstring status, fs::path installer = {}) {
    auto* result = new UpdateResult{std::move(status), std::move(installer)};
    if (!PostMessageW(g_window, WM_UPDATE_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

void startUpdateCheck() {
    if (g_updateInFlight.exchange(true)) return;
    const Language language = g_language;
    g_downloadedUpdate.clear();
    g_updateStatus = localizedFor(language, L"Vérification de la dernière version...", L"Checking for the latest version...",
                                  L"Die neueste Version wird gesucht...", L"正在检查最新版本…");
    InvalidateRect(g_window, nullptr, FALSE);
    const fs::path appDirectory = g_appDirectory;
    std::thread([language, appDirectory] {
        const fs::path channel = appDirectory / L"update-channel.ini";
        std::wstring manifestUrl = readIniValue(channel, L"manifest_url");
        if (manifestUrl.empty()) manifestUrl = kOfficialUpdateManifestUrl;
        if (manifestUrl.empty()) {
            postUpdateResult(localizedFor(language,
                L"Le canal de publication n'est pas encore configuré.",
                L"The release channel has not been configured yet.",
                L"Der Veröffentlichungskanal ist noch nicht konfiguriert.",
                L"发布通道尚未配置。"));
            return;
        }
        if (manifestUrl.rfind(L"https://", 0) != 0) {
            postUpdateResult(localizedFor(language, L"Le canal de mise à jour doit utiliser HTTPS.", L"The update channel must use HTTPS.",
                                          L"Der Update-Kanal muss HTTPS verwenden.", L"更新通道必须使用 HTTPS。"));
            return;
        }
        wchar_t temporaryDirectory[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporaryDirectory)) {
            postUpdateResult(localizedFor(language, L"Dossier temporaire inaccessible.", L"Temporary folder is unavailable.",
                                          L"Temporärer Ordner ist nicht verfügbar.", L"无法访问临时文件夹。"));
            return;
        }
        fs::path manifest = fs::path(temporaryDirectory) / L"RGBCcontrol-update.ini";
        DeleteFileW(manifest.c_str());
        if (FAILED(URLDownloadToFileW(nullptr, manifestUrl.c_str(), manifest.c_str(), 0, nullptr))) {
            postUpdateResult(localizedFor(language, L"Impossible de joindre le serveur de mise à jour.", L"The update server could not be reached.",
                                          L"Der Update-Server ist nicht erreichbar.", L"无法连接更新服务器。"));
            return;
        }
        const std::wstring version = readIniValue(manifest, L"version");
        const std::wstring installerUrl = readIniValue(manifest, L"installer_url");
        std::wstring expectedHash = readIniValue(manifest, L"sha256");
        DeleteFileW(manifest.c_str());
        std::transform(expectedHash.begin(), expectedHash.end(), expectedHash.begin(), ::towlower);
        if (version.empty() || installerUrl.rfind(L"https://", 0) != 0 || expectedHash.size() != 64) {
            postUpdateResult(localizedFor(language, L"Le manifeste de mise à jour est invalide.", L"The update manifest is invalid.",
                                          L"Das Update-Manifest ist ungültig.", L"更新清单无效。"));
            return;
        }
        if (!isNewerVersion(version)) {
            postUpdateResult(localizedFor(language, L"RGBCcontrol est déjà à jour.", L"RGBCcontrol is already up to date.",
                                          L"RGBCcontrol ist bereits aktuell.", L"RGBCcontrol 已是最新版本。"));
            return;
        }
        fs::path installer = fs::path(temporaryDirectory) / (L"RGBCcontrol-Setup-" + version + L".exe");
        DeleteFileW(installer.c_str());
        if (FAILED(URLDownloadToFileW(nullptr, installerUrl.c_str(), installer.c_str(), 0, nullptr))) {
            postUpdateResult(localizedFor(language, L"Le téléchargement de la mise à jour a échoué.", L"The update download failed.",
                                          L"Der Download des Updates ist fehlgeschlagen.", L"更新下载失败。"));
            return;
        }
        if (sha256File(installer) != expectedHash) {
            DeleteFileW(installer.c_str());
            postUpdateResult(localizedFor(language, L"Mise à jour refusée : signature SHA-256 incorrecte.", L"Update rejected: invalid SHA-256 signature.",
                                          L"Update abgelehnt: ungültige SHA-256-Signatur.", L"更新被拒绝：SHA-256 校验失败。"));
            return;
        }
        postUpdateResult(localizedFor(language, L"Mise à jour téléchargée et vérifiée. Prête à installer.",
                                      L"Update downloaded and verified. Ready to install.",
                                      L"Update heruntergeladen und geprüft. Installationsbereit.",
                                      L"更新已下载并验证，可以安装。"), installer);
    }).detach();
}

bool launchSilentUpdate(const fs::path& installer) {
    if (installer.empty() || !fs::exists(installer)) return false;
    const fs::path directory = installer.parent_path();
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = g_window;
    execute.lpVerb = L"open";
    execute.lpFile = installer.c_str();
    execute.lpParameters = L"/S";
    execute.lpDirectory = directory.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) return false;
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return true;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring quote(const fs::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

Color rgbColor(std::uint32_t rgb, BYTE alpha = 255) {
    return Color(alpha, static_cast<BYTE>((rgb >> 16) & 255), static_cast<BYTE>((rgb >> 8) & 255), static_cast<BYTE>(rgb & 255));
}

std::uint32_t activeAccentRgb() {
    return kAccentChoices[std::clamp(g_accentPreset, 0, static_cast<int>(std::size(kAccentChoices)) - 1)];
}

Color accentColor(BYTE alpha = 255) {
    return rgbColor(activeAccentRgb(), alpha);
}

Color accentButtonText() {
    const std::uint32_t color = activeAccentRgb();
    const int luminance = static_cast<int>(((color >> 16) & 255) * 299 + ((color >> 8) & 255) * 587 +
                                           (color & 255) * 114) / 1000;
    return luminance >= 175 ? Color(255, 18, 23, 33) : Color(255, 246, 244, 252);
}

Color accentTint(double amount, BYTE alpha = 255) {
    amount = std::clamp(amount, 0.0, 1.0);
    const std::uint32_t source = activeAccentRgb();
    auto tint = [amount](std::uint32_t channel) {
        return static_cast<std::uint32_t>(std::lround(channel + (255.0 - channel) * amount));
    };
    return Color(alpha, static_cast<BYTE>(tint((source >> 16) & 255)),
                 static_cast<BYTE>(tint((source >> 8) & 255)), static_cast<BYTE>(tint(source & 255)));
}

std::uint32_t hsvColor(double hue, double saturation, double value) {
    hue = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0);
    saturation = std::clamp(saturation, 0.0, 1.0);
    value = std::clamp(value, 0.0, 1.0);
    const double chroma = value * saturation;
    const double second = chroma * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
    const double match = value - chroma;
    double red = 0, green = 0, blue = 0;
    if (hue < 60) { red = chroma; green = second; }
    else if (hue < 120) { red = second; green = chroma; }
    else if (hue < 180) { green = chroma; blue = second; }
    else if (hue < 240) { green = second; blue = chroma; }
    else if (hue < 300) { red = second; blue = chroma; }
    else { red = chroma; blue = second; }
    return (static_cast<std::uint32_t>(std::lround((red + match) * 255.0)) << 16) |
           (static_cast<std::uint32_t>(std::lround((green + match) * 255.0)) << 8) |
           static_cast<std::uint32_t>(std::lround((blue + match) * 255.0));
}

void rgbToHsv(std::uint32_t rgb, double& hue, double& saturation, int& brightness) {
    const double red = ((rgb >> 16) & 255) / 255.0;
    const double green = ((rgb >> 8) & 255) / 255.0;
    const double blue = (rgb & 255) / 255.0;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;
    if (delta <= 0.000001) hue = 0;
    else if (maximum == red) hue = 60.0 * std::fmod((green - blue) / delta, 6.0);
    else if (maximum == green) hue = 60.0 * ((blue - red) / delta + 2.0);
    else hue = 60.0 * ((red - green) / delta + 4.0);
    if (hue < 0) hue += 360.0;
    saturation = maximum <= 0.000001 ? 0 : delta / maximum;
    brightness = static_cast<int>(std::lround(maximum * 100.0));
}

std::uint32_t scaleColor(std::uint32_t rgb, double factor) {
    factor = std::clamp(factor, 0.0, 1.0);
    auto scale = [factor](std::uint32_t channel) { return static_cast<std::uint32_t>(std::lround(channel * factor)); };
    return (scale((rgb >> 16) & 255) << 16) | (scale((rgb >> 8) & 255) << 8) | scale(rgb & 255);
}

double gradientDegreesPerSecond(int speed) {
    const double normalized = std::clamp(speed, 0, 100) / 100.0;
    // 120 seconds per revolution at minimum, 8 seconds at maximum. The curve
    // offers useful precision at low speed without making the upper half feel
    // unresponsive.
    return 3.0 + 42.0 * std::pow(normalized, 1.25);
}

std::uint32_t gamepadLightingPreviewRgb(ULONGLONG now) {
    const double brightness = std::clamp(g_brightness / 100.0, 0.0, 1.0);
    const int effect = g_activeEffectIndex.load();
    if (!g_effectActive.load() || effect <= 0 || effect >= static_cast<int>(std::size(kEffects))) {
        return scaleColor(g_baseColor, brightness);
    }

    // Keep this curve byte-for-byte compatible with the standalone DualSense
    // lighting worker. The 3D preview must describe the packet being sent to
    // the real lightbar, not an approximation of it.
    const double amount = brightness * std::clamp(g_effectIntensity.load() / 100.0, 0.0, 1.0);
    const double elapsed = g_effectStartedAt == 0 || now <= g_effectStartedAt
        ? 0.0
        : (now - g_effectStartedAt) / 1000.0;
    const double speed = std::clamp(g_effectSpeed.load(), 0, 100);
    const double rate = 0.30 + speed / 38.0;
    const double phase = elapsed * rate;
    double baseHue = 0.0, baseSaturation = 0.0;
    int baseBrightness = 0;
    rgbToHsv(g_baseColor, baseHue, baseSaturation, baseBrightness);
    const std::wstring mode = kEffects[effect].internal;

    if (mode == L"Rainbow" || mode == L"Spectrum Cycle" || mode == L"Gradient") {
        return hsvColor(baseHue + phase * (mode == L"Gradient" ? 42.0 : 78.0), 0.98, amount);
    }
    if (mode == L"Breathing") {
        const double pulse = 0.12 + 0.88 * (std::sin(phase * 2.2) + 1.0) * 0.5;
        return scaleColor(g_baseColor, amount * pulse);
    }
    if (mode == L"Wave") {
        const double pulse = 0.58 + 0.42 * std::sin(phase * 2.0) * std::sin(phase * 2.0);
        return hsvColor(baseHue + std::sin(phase * 1.6) * 90.0, 0.94, amount * pulse);
    }
    if (mode == L"Strobe") {
        return scaleColor(g_baseColor, (static_cast<int>(std::floor(phase * 5.0)) & 1) ? 0.0 : amount);
    }
    if (mode == L"Random") {
        const double step = std::floor(phase * 1.8);
        return hsvColor(std::fmod(step * 137.507764, 360.0), 0.95, amount);
    }
    if (mode == L"Music") {
        const double pulse = 0.28 + 0.72 * std::abs(std::sin(phase * 3.1) * std::sin(phase * 0.83));
        return hsvColor(baseHue + std::sin(phase) * 24.0, 0.90, amount * pulse);
    }
    if (mode == L"Ambilight") {
        return hsvColor(baseHue + phase * 25.0, 0.82, amount);
    }
    if (mode == L"Neon") {
        return hsvColor(286.0 + std::sin(phase * 1.9) * 58.0, 0.92, amount);
    }
    if (mode == L"Water") {
        return hsvColor(195.0 + std::sin(phase * 1.7) * 24.0, 0.88,
                        amount * (0.62 + 0.38 * std::sin(phase * 2.1) * std::sin(phase * 2.1)));
    }
    if (mode == L"Scan") {
        const double pulse = 0.18 + 0.82 * std::abs(std::sin(phase * 2.5));
        return scaleColor(g_baseColor, amount * pulse);
    }
    if (mode == L"Stack") {
        const double pulse = std::fmod(phase * 0.45, 1.0);
        return scaleColor(g_baseColor, amount * pulse);
    }
    return scaleColor(g_baseColor, amount);
}

std::wstring hexColor(std::uint32_t rgb) {
    std::wostringstream text;
    text << L"#" << std::uppercase << std::hex << std::setw(6) << std::setfill(L'0') << (rgb & 0xffffff);
    return text.str();
}

void roundedPath(GraphicsPath& path, const RectF& rect, float radius) {
    float diameter = radius * 2.0f;
    if (diameter > rect.Width) diameter = rect.Width;
    if (diameter > rect.Height) diameter = rect.Height;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

void fillRound(Graphics& graphics, const RectF& rect, float radius, Color color) {
    GraphicsPath path;
    roundedPath(path, rect, radius);
    // Aurora Studio design system. The legacy pages deliberately keep using a
    // small set of surface colours; translating them here gives every screen
    // the same calm hierarchy without duplicating visual code in each page.
    const bool primarySurface = color.GetA() == 255 && color.GetR() == 28 &&
                                color.GetG() == 32 && color.GetB() == 45;
    const bool secondarySurface = color.GetA() == 255 &&
        ((color.GetR() == 23 && color.GetG() == 27 && color.GetB() == 39) ||
         (color.GetR() == 24 && color.GetG() == 28 && color.GetB() == 40) ||
         (color.GetR() == 22 && color.GetG() == 26 && color.GetB() == 37));
    if (primarySurface || secondarySurface) {
        GraphicsPath shadowPath;
        roundedPath(shadowPath, RectF(rect.X, rect.Y + 7.0f, rect.Width, rect.Height), radius);
        SolidBrush shadow(Color(primarySurface ? 66 : 42, 0, 0, 0));
        graphics.FillPath(&shadow, &shadowPath);
        LinearGradientBrush glass(PointF(rect.X, rect.Y), PointF(rect.X, rect.GetBottom()),
                                  primarySurface ? Color(250, 27, 32, 46) : Color(248, 22, 26, 38),
                                  primarySurface ? Color(250, 18, 22, 33) : Color(248, 15, 19, 29));
        graphics.FillPath(&glass, &path);
        Pen topLight(primarySurface ? Color(28, 255, 255, 255) : Color(18, 255, 255, 255), 1.0f);
        graphics.DrawPath(&topLight, &path);
        return;
    }
    SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void strokeRound(Graphics& graphics, const RectF& rect, float radius, Color color, float width = 1.0f) {
    GraphicsPath path;
    roundedPath(path, rect, radius);
    Pen pen(color, width);
    graphics.DrawPath(&pen, &path);
}

std::uint32_t cosinePaletteColor(double progress, std::uint32_t baseRgb, double intensity = 1.0) {
    double baseHue = 0, saturation = 0;
    int brightness = 0;
    rgbToHsv(baseRgb, baseHue, saturation, brightness);
    constexpr double tau = 6.28318530717958647692;
    progress += baseHue / 360.0;
    const double red = 0.58 + 0.42 * std::cos(tau * progress);
    const double green = 0.58 + 0.42 * std::cos(tau * (progress - 1.0 / 3.0));
    const double blue = 0.58 + 0.42 * std::cos(tau * (progress - 2.0 / 3.0));
    auto channel = [intensity](double value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value * intensity, 0.0, 1.0) * 255.0));
    };
    return (channel(red) << 16) | (channel(green) << 8) | channel(blue);
}

void fillSeamlessGradient(Graphics& graphics, const RectF& rect, float radius) {
    constexpr int stopCount = 7;
    Color colors[stopCount];
    REAL positions[stopCount];
    for (int index = 0; index < stopCount; ++index) {
        positions[index] = index / static_cast<REAL>(stopCount - 1);
        colors[index] = rgbColor(cosinePaletteColor(positions[index], g_baseColor));
    }
    LinearGradientBrush brush(PointF(rect.X, rect.Y), PointF(rect.GetRight(), rect.Y), colors[0], colors[stopCount - 1]);
    brush.SetInterpolationColors(colors, positions, stopCount);
    GraphicsPath path;
    roundedPath(path, rect, radius);
    graphics.FillPath(&brush, &path);
}

const wchar_t* interfaceFontName(bool display) {
    if (g_language == Language::Chinese) return L"Microsoft YaHei UI";
    static const wchar_t* displayFont = []() -> const wchar_t* {
        FontFamily variable(L"Segoe UI Variable Display");
        if (variable.GetLastStatus() == Ok) return L"Segoe UI Variable Display";
        FontFamily bahnschrift(L"Bahnschrift");
        if (bahnschrift.GetLastStatus() == Ok) return L"Bahnschrift";
        return L"Segoe UI";
    }();
    static const wchar_t* textFont = []() -> const wchar_t* {
        FontFamily variable(L"Segoe UI Variable Text");
        return variable.GetLastStatus() == Ok ? L"Segoe UI Variable Text" : L"Segoe UI";
    }();
    return display ? displayFont : textFont;
}

Font* cachedInterfaceFont(float size, FontStyle style) {
    struct Entry {
        int sizeTenths = 0;
        int style = 0;
        std::wstring family;
        std::unique_ptr<Font> font;
    };
    // Process-lifetime cache: GDI+ is shut down before static destructors run,
    // so keeping these few font handles alive avoids destroying them too late.
    static auto* fonts = new std::vector<Entry>();
    const bool display = size >= 17.0f || (style == FontStyleBold && size >= 14.0f);
    const std::wstring familyName = interfaceFontName(display);
    const int sizeTenths = static_cast<int>(std::lround(size * 10.0f));
    const int styleValue = static_cast<int>(style);
    auto found = std::find_if(fonts->begin(), fonts->end(), [&](const Entry& entry) {
        return entry.sizeTenths == sizeTenths && entry.style == styleValue && entry.family == familyName;
    });
    if (found != fonts->end()) return found->font.get();
    FontFamily family(familyName.c_str());
    Entry entry;
    entry.sizeTenths = sizeTenths;
    entry.style = styleValue;
    entry.family = familyName;
    entry.font = std::make_unique<Font>(&family, size, style, UnitPixel);
    fonts->push_back(std::move(entry));
    return fonts->back().font.get();
}

HFONT cachedNativeFont(float size, FontStyle style) {
    struct Entry {
        int size = 0;
        int style = 0;
        std::wstring family;
        HFONT font = nullptr;
    };
    static auto* fonts = new std::vector<Entry>();
    const bool display = size >= 17.0f || (style == FontStyleBold && size >= 14.0f);
    const std::wstring familyName = interfaceFontName(display);
    const int pixelSize = std::max(1, static_cast<int>(std::lround(size)));
    const int styleValue = static_cast<int>(style);
    auto found = std::find_if(fonts->begin(), fonts->end(), [&](const Entry& entry) {
        return entry.size == pixelSize && entry.style == styleValue && entry.family == familyName;
    });
    if (found != fonts->end()) return found->font;
    Entry entry;
    entry.size = pixelSize;
    entry.style = styleValue;
    entry.family = familyName;
    entry.font = CreateFontW(-pixelSize, 0, 0, 0, (styleValue & FontStyleBold) ? FW_BOLD : FW_NORMAL,
                             (styleValue & FontStyleItalic) != 0, (styleValue & FontStyleUnderline) != 0,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, familyName.c_str());
    fonts->push_back(std::move(entry));
    return fonts->back().font;
}

void drawNativeText(HDC target, const DeferredTextCommand& command) {
    HFONT font = cachedNativeFont(command.size, command.style);
    if (!target || !font) return;
    const HGDIOBJ previous = SelectObject(target, font);
    SetTextColor(target, RGB(command.color.GetR(), command.color.GetG(), command.color.GetB()));
    RECT bounds{
        static_cast<LONG>(std::floor(command.rect.X)), static_cast<LONG>(std::floor(command.rect.Y)),
        static_cast<LONG>(std::ceil(command.rect.GetRight())), static_cast<LONG>(std::ceil(command.rect.GetBottom()))
    };
    UINT flags = DT_NOPREFIX | (command.wrapped ? (DT_WORDBREAK | DT_END_ELLIPSIS)
                                                : (DT_SINGLELINE | DT_END_ELLIPSIS));
    if (command.horizontal == StringAlignmentCenter) flags |= DT_CENTER;
    else if (command.horizontal == StringAlignmentFar) flags |= DT_RIGHT;
    else flags |= DT_LEFT;
    if (!command.wrapped) {
        if (command.vertical == StringAlignmentCenter) flags |= DT_VCENTER;
        else if (command.vertical == StringAlignmentFar) flags |= DT_BOTTOM;
        else flags |= DT_TOP;
    }
    DrawTextW(target, command.value.c_str(), static_cast<int>(command.value.size()), &bounds, flags);
    SelectObject(target, previous);
}

void flushDeferredText(Graphics& graphics, const RectF* clipBounds = nullptr) {
    if (g_deferredText.empty()) return;
    HDC target = graphics.GetHDC();
    if (target) {
        const int savedState = SaveDC(target);
        if (clipBounds) {
            IntersectClipRect(target,
                              static_cast<int>(std::floor(clipBounds->X)),
                              static_cast<int>(std::floor(clipBounds->Y)),
                              static_cast<int>(std::ceil(clipBounds->GetRight())),
                              static_cast<int>(std::ceil(clipBounds->GetBottom())));
        }
        SetBkMode(target, TRANSPARENT);
        for (const DeferredTextCommand& command : g_deferredText) drawNativeText(target, command);
        if (savedState != 0) RestoreDC(target, savedState);
        graphics.ReleaseHDC(target);
    }
    g_deferredText.clear();
}

void drawCachedText(Graphics& graphics, const std::wstring& value, const RectF& rect, float size,
                    Color color, FontStyle style, StringAlignment horizontal,
                    StringAlignment vertical, bool wrapped) {
    if (value.empty() || rect.Width <= 0.0f || rect.Height <= 0.0f) return;
    if (color.GetA() == 255) {
        DeferredTextCommand command{value, rect, size, color, style, horizontal, vertical, wrapped};
        if (g_collectOpaqueText) {
            g_deferredText.push_back(std::move(command));
            return;
        }
        HDC target = graphics.GetHDC();
        if (target) {
            SetBkMode(target, TRANSPARENT);
            drawNativeText(target, command);
            graphics.ReleaseHDC(target);
            return;
        }
    }
    const int width = std::max(1, static_cast<int>(std::ceil(rect.Width)));
    const int height = std::max(1, static_cast<int>(std::ceil(rect.Height)));
    std::wstring key = value;
    key += L'\x1f' + std::wstring(interfaceFontName(size >= 17.0f || (style == FontStyleBold && size >= 14.0f)));
    key += L'\x1f' + std::to_wstring(static_cast<int>(std::lround(size * 10.0f)));
    key += L'\x1f' + std::to_wstring(static_cast<int>(style));
    key += L'\x1f' + std::to_wstring(color.GetValue());
    key += L'\x1f' + std::to_wstring(width) + L'x' + std::to_wstring(height);
    key += L'\x1f' + std::to_wstring(static_cast<int>(horizontal));
    key += L'\x1f' + std::to_wstring(static_cast<int>(vertical));
    key += wrapped ? L"\x1fW" : L"\x1fN";

    // Text is the most expensive part of GDI+ on software-rendered Windows
    // sessions. Rasterizing each unique label once makes subsequent frames a
    // cheap bitmap composition while preserving the exact modern typography.
    static auto* cache = new std::unordered_map<std::wstring, std::unique_ptr<Bitmap>>();
    auto found = cache->find(key);
    if (found == cache->end()) {
        if (cache->size() > 1400) cache->clear();
        auto image = std::make_unique<Bitmap>(width, height, PixelFormat32bppPARGB);
        Graphics raster(image.get());
        raster.Clear(Color(0, 0, 0, 0));
        raster.SetSmoothingMode(SmoothingModeAntiAlias);
        raster.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        Font* font = cachedInterfaceFont(size, style);
        SolidBrush brush(color);
        StringFormat format;
        format.SetAlignment(horizontal);
        format.SetLineAlignment(vertical);
        format.SetTrimming(wrapped ? StringTrimmingEllipsisWord : StringTrimmingEllipsisCharacter);
        if (!wrapped) format.SetFormatFlags(StringFormatFlagsNoWrap);
        raster.DrawString(value.c_str(), static_cast<INT>(value.size()), font,
                          RectF(0, 0, static_cast<REAL>(width), static_cast<REAL>(height)), &format, &brush);
        found = cache->emplace(std::move(key), std::move(image)).first;
    }
    graphics.DrawImage(found->second.get(), PointF(rect.X, rect.Y));
}

void text(Graphics& graphics, const std::wstring& value, const RectF& rect, float size,
          Color color, FontStyle style = FontStyleRegular, StringAlignment horizontal = StringAlignmentNear,
          StringAlignment vertical = StringAlignmentNear) {
    drawCachedText(graphics, value, rect, size, color, style, horizontal, vertical, false);
}

void textWrapped(Graphics& graphics, const std::wstring& value, const RectF& rect, float size,
                 Color color, FontStyle style = FontStyleRegular, StringAlignment vertical = StringAlignmentNear) {
    drawCachedText(graphics, value, rect, size, color, style, StringAlignmentNear, vertical, true);
}

void addHit(const RectF& rect, Action action, int index = -1, std::uint32_t value = 0) {
    g_hits.push_back({rect, action, index, value});
}

bool contains(const RectF& rect, float x, float y) {
    return x >= rect.X && y >= rect.Y && x <= rect.GetRight() && y <= rect.GetBottom();
}

std::wstring base64Decode(const std::string& input) {
    DWORD byteCount = 0;
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64, nullptr, &byteCount, nullptr, nullptr)) return {};
    std::string decoded(byteCount, '\0');
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(decoded.data()), &byteCount, nullptr, nullptr)) return {};
    decoded.resize(byteCount);
    return widen(decoded);
}

std::vector<std::string> split(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        std::size_t position = line.find(separator, start);
        fields.push_back(line.substr(start, position == std::string::npos ? std::string::npos : position - start));
        if (position == std::string::npos) break;
        start = position + 1;
    }
    return fields;
}

bool runProcess(const fs::path& executable, const std::wstring& arguments, bool wait, DWORD* exitCode = nullptr) {
    std::wstring command = quote(executable) + (arguments.empty() ? L"" : L" " + arguments);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                  nullptr, executable.parent_path().c_str(), &startup, &process);
    if (!created) return false;
    if (wait) {
        WaitForSingleObject(process.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        if (exitCode) *exitCode = code;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

fs::path openRgbExecutable() {
    fs::path expected = g_appDirectory / L"OpenRGB" / L"OpenRGB Windows 64-bit" / L"OpenRGB.exe";
    if (fs::exists(expected)) return expected;
    fs::path root = g_appDirectory / L"OpenRGB";
    std::error_code error;
    if (fs::exists(root)) {
        for (const auto& entry : fs::recursive_directory_iterator(root, error)) {
            if (entry.path().filename() == L"OpenRGB.exe") return entry.path();
        }
    }
    return {};
}

fs::path bridgeExecutable() {
    return g_appDirectory / L"HardwareMonitor" / L"RGBCcontrol.HardwareBridge.exe";
}

fs::path xboxBridgeExecutable() {
    return g_appDirectory / L"GamepadBridge" / L"RGBCcontrol.GamepadBridge.exe";
}

void closeXboxBridgeHandles() {
    if (g_xboxBridgeInput) { CloseHandle(g_xboxBridgeInput); g_xboxBridgeInput = nullptr; }
    if (g_xboxBridgeProcess) { CloseHandle(g_xboxBridgeProcess); g_xboxBridgeProcess = nullptr; }
}

void stopXboxBridge() {
    if (g_xboxBridgeInput) {
        static constexpr char stopCommand[] = "STOP\n";
        DWORD written = 0;
        WriteFile(g_xboxBridgeInput, stopCommand, sizeof(stopCommand) - 1, &written, nullptr);
        CloseHandle(g_xboxBridgeInput);
        g_xboxBridgeInput = nullptr;
    }
    if (g_xboxBridgeProcess) {
        if (WaitForSingleObject(g_xboxBridgeProcess, 1800) == WAIT_TIMEOUT) {
            TerminateProcess(g_xboxBridgeProcess, 0);
            WaitForSingleObject(g_xboxBridgeProcess, 500);
        }
        CloseHandle(g_xboxBridgeProcess);
        g_xboxBridgeProcess = nullptr;
    }
    g_xboxModeEnabled = false;
    g_xboxBridgeStatus = XboxBridgeStatus::Off;
    g_xboxBridgeMessage.clear();
}

void failXboxBridge(const std::wstring& message) {
    if (g_xboxBridgeInput) { CloseHandle(g_xboxBridgeInput); g_xboxBridgeInput = nullptr; }
    if (g_xboxBridgeProcess) {
        WaitForSingleObject(g_xboxBridgeProcess, 300);
        CloseHandle(g_xboxBridgeProcess);
        g_xboxBridgeProcess = nullptr;
    }
    g_xboxBridgeStatus = XboxBridgeStatus::Error;
    g_xboxBridgeMessage = message;
}

bool startXboxBridge() {
    if (g_xboxBridgeProcess && WaitForSingleObject(g_xboxBridgeProcess, 0) == WAIT_TIMEOUT) return true;
    closeXboxBridgeHandles();
    const fs::path executable = xboxBridgeExecutable();
    if (!fs::exists(executable)) {
        g_xboxBridgeStatus = XboxBridgeStatus::Error;
        g_xboxBridgeMessage = localized(L"Composant Xbox absent.", L"Xbox component is missing.",
                                        L"Xbox-Komponente fehlt.", L"缺少 Xbox 组件。");
        return false;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childInput = nullptr;
    HANDLE parentInput = nullptr;
    if (!CreatePipe(&childInput, &parentInput, &security, 0)) {
        g_xboxBridgeStatus = XboxBridgeStatus::Error;
        g_xboxBridgeMessage = localized(L"Impossible d'ouvrir le canal XInput.", L"Could not open the XInput channel.",
                                        L"XInput-Kanal konnte nicht geöffnet werden.", L"无法打开 XInput 通道。");
        return false;
    }
    SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = childInput;
    startup.hStdOutput = nullOutput != INVALID_HANDLE_VALUE ? nullOutput : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = startup.hStdOutput;
    PROCESS_INFORMATION process{};
    std::wstring command = quote(executable);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, g_appDirectory.c_str(), &startup, &process);
    CloseHandle(childInput);
    if (nullOutput != INVALID_HANDLE_VALUE) CloseHandle(nullOutput);
    if (!created) {
        CloseHandle(parentInput);
        g_xboxBridgeStatus = XboxBridgeStatus::Error;
        g_xboxBridgeMessage = localized(L"Le mode Xbox n'a pas pu démarrer.", L"Xbox mode could not start.",
                                        L"Xbox-Modus konnte nicht gestartet werden.", L"Xbox 模式无法启动。");
        return false;
    }
    CloseHandle(process.hThread);
    g_xboxBridgeProcess = process.hProcess;
    g_xboxBridgeInput = parentInput;
    g_xboxBridgeStartedAt = GetTickCount64();
    g_xboxBridgeStatus = XboxBridgeStatus::Starting;
    g_xboxBridgeMessage.clear();
    g_xboxModeEnabled = true;
    return true;
}

bool pollXboxBridge() {
    if (!g_xboxModeEnabled || !g_xboxBridgeProcess) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(g_xboxBridgeProcess, &exitCode) || exitCode != STILL_ACTIVE) {
        failXboxBridge(localized(L"Activation refusée. Relance RGBCcontrol en administrateur.",
                                 L"Activation failed. Restart RGBCcontrol as administrator.",
                                 L"Aktivierung fehlgeschlagen. RGBCcontrol als Administrator neu starten.",
                                 L"激活失败。请以管理员身份重新启动 RGBCcontrol。"));
        return true;
    }
    if (g_xboxBridgeStatus == XboxBridgeStatus::Starting && GetTickCount64() - g_xboxBridgeStartedAt >= 1300) {
        g_xboxBridgeStatus = XboxBridgeStatus::Ready;
        return true;
    }
    return false;
}

void sendXboxState(const DualSenseLiveState& state) {
    if (!g_xboxModeEnabled || !g_xboxBridgeInput || !g_xboxBridgeProcess) return;
    if (pollXboxBridge() && g_xboxBridgeStatus == XboxBridgeStatus::Error) return;
    // Do not fill the pipe while HIDMaestro is still installing its driver;
    // the first live report after READY will immediately refresh the state.
    if (g_xboxBridgeStatus != XboxBridgeStatus::Ready) return;
    unsigned int buttons = 0;
    for (int index = 0; index < 15; ++index) if (state.buttons[index]) buttons |= 1u << index;
    std::ostringstream command;
    command << "STATE " << buttons << ' ' << state.dpad << ' '
            << static_cast<int>(state.leftX) << ' ' << static_cast<int>(state.leftY) << ' '
            << static_cast<int>(state.rightX) << ' ' << static_cast<int>(state.rightY) << ' '
            << static_cast<int>(state.leftTrigger) << ' ' << static_cast<int>(state.rightTrigger) << '\n';
    const std::string bytes = command.str();
    DWORD written = 0;
    if (!WriteFile(g_xboxBridgeInput, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size()) {
        failXboxBridge(localized(L"La connexion XInput s'est interrompue.", L"The XInput connection was interrupted.",
                                 L"Die XInput-Verbindung wurde unterbrochen.", L"XInput 连接已中断。"));
    }
}

void closeHardwareHandles() {
    if (g_hardwareInput) { CloseHandle(g_hardwareInput); g_hardwareInput = nullptr; }
    if (g_hardwareOutput) { CloseHandle(g_hardwareOutput); g_hardwareOutput = nullptr; }
    if (g_hardwareProcess) { CloseHandle(g_hardwareProcess); g_hardwareProcess = nullptr; }
    g_hardwareBuffer.clear();
}

bool ensureHardwareBridgeLocked() {
    if (g_hardwareProcess && WaitForSingleObject(g_hardwareProcess, 0) == WAIT_TIMEOUT && g_hardwareInput && g_hardwareOutput) return true;
    closeHardwareHandles();
    const fs::path executable = bridgeExecutable();
    if (!fs::exists(executable)) return false;

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childInput = nullptr;
    HANDLE parentInput = nullptr;
    HANDLE parentOutput = nullptr;
    HANDLE childOutput = nullptr;
    if (!CreatePipe(&childInput, &parentInput, &security, 0)) return false;
    if (!CreatePipe(&parentOutput, &childOutput, &security, 0)) {
        CloseHandle(childInput); CloseHandle(parentInput); return false;
    }
    SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOutput, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quote(executable) + L" server";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childInput;
    startup.hStdOutput = childOutput;
    startup.hStdError = childOutput;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, executable.parent_path().c_str(), &startup, &process);
    CloseHandle(childInput);
    CloseHandle(childOutput);
    if (!created) {
        CloseHandle(parentInput);
        CloseHandle(parentOutput);
        return false;
    }
    CloseHandle(process.hThread);
    g_hardwareProcess = process.hProcess;
    g_hardwareInput = parentInput;
    g_hardwareOutput = parentOutput;
    return true;
}

bool hardwareCommand(const std::string& command, std::string* output = nullptr, DWORD* exitCode = nullptr) {
    std::lock_guard<std::mutex> lock(g_hardwareMutex);
    if (!ensureHardwareBridgeLocked()) return false;
    const std::string request = command + "\n";
    DWORD written = 0;
    if (!WriteFile(g_hardwareInput, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) || written != request.size()) {
        closeHardwareHandles();
        return false;
    }
    std::string response;
    while (true) {
        std::size_t newline = g_hardwareBuffer.find('\n');
        if (newline != std::string::npos) {
            std::string line = g_hardwareBuffer.substr(0, newline);
            g_hardwareBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("RESULT\t", 0) == 0) {
                DWORD result = 1;
                try { result = static_cast<DWORD>(std::stoul(line.substr(7))); } catch (...) { }
                if (output) *output = std::move(response);
                if (exitCode) *exitCode = result;
                return true;
            }
            response += line + "\n";
            continue;
        }
        char buffer[4096];
        DWORD count = 0;
        if (!ReadFile(g_hardwareOutput, buffer, sizeof(buffer), &count, nullptr) || count == 0) {
            closeHardwareHandles();
            return false;
        }
        g_hardwareBuffer.append(buffer, count);
    }
}

void shutdownHardwareBridge() {
    if (g_hardwareProcess && g_hardwareInput && g_hardwareOutput) {
        DWORD ignored = 1;
        hardwareCommand("quit", nullptr, &ignored);
    }
    std::lock_guard<std::mutex> lock(g_hardwareMutex);
    if (g_hardwareProcess) WaitForSingleObject(g_hardwareProcess, 3000);
    closeHardwareHandles();
}

bool ensureOpenRgbServer() {
    std::lock_guard<std::mutex> lock(g_serverMutex);
    OpenRgbClient client;
    if (client.canConnect()) return true;
    fs::path executable = openRgbExecutable();
    if (executable.empty()) return false;
    std::wstring command = quote(executable) + L" --server --server-host 127.0.0.1";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, executable.parent_path().c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    g_ownedOpenRgbProcess = process.hProcess;
    for (int attempt = 0; attempt < 30 && g_running; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (client.canConnect()) return true;
    }
    return false;
}

std::wstring rgbConflictProcess() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring found;
    static const wchar_t* writers[] = {
        L"signalrgb", L"lightingservice", L"asrpolychromergb", L"polychromesync",
        L"rgbfusion", L"ledkeeper", L"mysticlight", L"icue"
    };
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring executable = entry.szExeFile;
            std::wstring lowerExecutable = executable;
            std::transform(lowerExecutable.begin(), lowerExecutable.end(), lowerExecutable.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(towlower(character));
            });
            for (const wchar_t* writer : writers) {
                if (lowerExecutable.find(writer) != std::wstring::npos) {
                    found = executable;
                    break;
                }
            }
            if (!found.empty()) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::wstring rgbConflictMessage(const std::wstring& process) {
    return localized(L"Un autre moteur RGB est actif (", L"Another RGB engine is active (",
                     L"Eine andere RGB-Engine ist aktiv (", L"另一个 RGB 引擎正在运行（") + process +
           localized(L"). Ferme-le complètement depuis la zone de notification, puis réessaie.",
                     L"). Quit it completely from the tray, then try again.",
                     L"). Beende ihn vollständig im Infobereich und versuche es erneut.",
                     L"）。请从托盘中完全退出后重试。");
}

bool hasDuckyKeyboard() {
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
        wchar_t buffer[4096]{};
        DWORD type = 0, required = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, &type,
                                               reinterpret_cast<PBYTE>(buffer), sizeof(buffer), &required)) continue;
        std::wstring ids(buffer);
        std::transform(ids.begin(), ids.end(), ids.begin(), [](wchar_t value) { return static_cast<wchar_t>(towupper(value)); });
        // DKON1861ST in its normal firmware mode.  VID 0416 is shared by many
        // generic Holtek keyboards, so broad PID guesses caused false Ducky
        // cards on computers that did not own one.
        if (ids.find(L"VID_0416&PID_0123") != std::wstring::npos) {
            found = true;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

void parseFans(const std::string& output, ScanResult& result) {
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields = split(line, '\t');
        if (fields.size() >= 3 && fields[0] == "TEMP") {
            try {
                double value = std::stod(fields[2]);
                if (fields[1] == "CPU") result.cpuTemperature = value;
                if (fields[1] == "GPU") result.gpuTemperature = value;
            } catch (...) { }
        } else if (fields.size() >= 8 && fields[0] == "FAN") {
            FanDevice fan;
            fan.encodedId = fields[1];
            fan.name = base64Decode(fields[2]);
            fan.hardware = base64Decode(fields[3]);
            try { fan.rpm = std::stod(fields[4]); } catch (...) { fan.rpm = -1; }
            try { fan.percent = std::stod(fields[5]); } catch (...) { fan.percent = -1; }
            try { fan.minimum = std::stoi(fields[6]); } catch (...) { fan.minimum = 30; }
            fan.controllable = fields[7] == "1";
            fan.desired = fan.percent >= fan.minimum ? static_cast<int>(std::lround(fan.percent)) : fan.minimum;
            result.fans.push_back(std::move(fan));
        }
    }
}

void postStatus(const std::wstring& status) {
    if (!g_running || !g_window) return;
    PostMessageW(g_window, WM_ASYNC_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(status)));
}

void startScan(bool force = false) {
    // Keep the exact device index/fingerprint stable during the three-step
    // visual certification. Hot-plug events remain queued and are processed
    // as soon as the test ends.
    if (g_certificationDevice >= 0 || g_certificationApplying || g_certificationAwaitingAnswer) return;
    if (g_scanInFlight.exchange(true)) return;
    if (g_scanThread.joinable()) g_scanThread.join();
    g_lastScan = GetTickCount64();
    const Language language = g_language;
    g_status = force ? localized(L"Analyse matérielle complète...", L"Full hardware scan...", L"Vollständige Hardware-Suche...", L"正在全面扫描硬件…")
                     : localized(L"Détection automatique...", L"Automatic detection...", L"Automatische Erkennung...", L"正在自动检测…");
    InvalidateRect(g_window, nullptr, FALSE);
    g_scanThread = std::thread([force, language] {
        std::unique_ptr<ScanResult> result(new ScanResult());
        try {
            result->openRgbReady = ensureOpenRgbServer();
            if (result->openRgbReady) result->rgb = OpenRgbClient().scan(force);
        } catch (const std::exception& error) {
            result->error = L"OpenRGB : " + widen(error.what());
        }
        // Native plugins are scanned even when OpenRGB is unavailable.  A
        // successful plugin probe owns the device; an optional manifest rule
        // can hide the less capable OpenRGB duplicate.
        PluginScanResult pluginScan = g_pluginEngine.scan();
        auto scanLower = [](std::wstring value) {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(towlower(character));
            });
            return value;
        };
        for (const std::wstring& pattern : pluginScan.supersededOpenRgbNames) {
            const std::wstring wanted = scanLower(pattern);
            result->rgb.erase(std::remove_if(result->rgb.begin(), result->rgb.end(), [&](const RgbDevice& device) {
                return device.providerId == L"openrgb" && scanLower(device.name).find(wanted) != std::wstring::npos;
            }), result->rgb.end());
        }
        result->rgb.insert(result->rgb.end(), std::make_move_iterator(pluginScan.devices.begin()),
                           std::make_move_iterator(pluginScan.devices.end()));
        result->pluginProviders = std::move(pluginScan.providers);
        result->rejectedPlugins = pluginScan.rejectedPlugins;
        result->ducky = hasDuckyKeyboard();
        fs::path bridge = bridgeExecutable();
        if (fs::exists(bridge)) {
            DWORD exitCode = 1;
            std::string output;
            bool started = hardwareCommand("scan", &output, &exitCode);
            if (started && exitCode == 0) parseFans(output, *result);
            else if (result->error.empty()) result->error = localizedFor(language,
                L"Capteurs de ventilation indisponibles en accès standard.", L"Fan sensors are unavailable with standard access.",
                L"Lüftersensoren sind mit Standardzugriff nicht verfügbar.", L"标准权限下无法访问风扇传感器。");
        } else if (result->error.empty()) {
            result->error = localizedFor(language, L"Pont de détection des ventilateurs absent.", L"The fan detection bridge is missing.",
                                         L"Die Lüfter-Erkennungskomponente fehlt.", L"缺少风扇检测组件。");
        }
        g_scanInFlight = false;
        if (g_running && g_window) PostMessageW(g_window, WM_SCAN_COMPLETE, 0, reinterpret_cast<LPARAM>(result.release()));
    });
}

std::wstring openRgbArguments(const std::vector<RgbDevice>& devices, const std::wstring& mode, std::uint32_t color,
                              bool includeColor = true) {
    std::wostringstream arguments;
    arguments << L"--client 127.0.0.1:6742";
    for (const RgbDevice& device : devices) {
        if (!device.selected || device.providerId != L"openrgb") continue;
        // OpenRGB's command-line selector is the numeric SDK device index.
        // Passing the friendly name can silently select no controller on some
        // releases, leaving the previous Direct frame (often black) visible.
        arguments << L" --device " << device.index << L" --mode \"" << mode << L"\"";
        if (includeColor) arguments << L" --color \"" << hexColor(color).substr(1) << L"\"";
    }
    return arguments.str();
}

std::wstring lowerCase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

std::wstring advertisedMode(const RgbDevice& device, const std::wstring& wanted, bool allowContains = false) {
    const std::wstring key = lowerCase(wanted);
    for (const std::wstring& mode : device.modes) {
        if (lowerCase(mode) == key) return mode;
    }
    if (allowContains) {
        for (const std::wstring& mode : device.modes) {
            if (lowerCase(mode).find(key) != std::wstring::npos) return mode;
        }
    }
    return {};
}

std::wstring staticModeFor(const RgbDevice& device) {
    for (const wchar_t* candidate : {L"Static", L"Solid", L"Fixed", L"Constant"}) {
        std::wstring mode = advertisedMode(device, candidate, true);
        if (!mode.empty()) return mode;
    }
    return {};
}

bool runOpenRgbMode(const fs::path& executable, const RgbDevice& device, const std::wstring& mode,
                    std::uint32_t color, bool includeColor) {
    if (executable.empty() || mode.empty()) return false;
    DWORD exitCode = 1;
    return runProcess(executable, openRgbArguments({device}, mode, color, includeColor), true, &exitCode) && exitCode == 0;
}

bool applyColorToDevice(RgbDevice device, std::uint32_t color, std::wstring& failure, int* initializedZones = nullptr) {
    if (PluginEngine::isPluginDevice(device)) {
        PluginCommandResult result = g_pluginEngine.applyStatic(device, color, 100);
        if (!result.success) failure = result.message;
        return result.success;
    }
    if (device.providerId != L"openrgb") {
        failure = L"Fournisseur inconnu";
        return false;
    }
    if (!ensureOpenRgbServer()) {
        failure = L"OpenRGB indisponible";
        return false;
    }
    int resized = 0;
    // Keep the resized geometry returned by initializeEmptyZones for the
    // following frame.  A named vector avoids a device-wide black Direct frame
    // on multi-zone motherboards.
    std::vector<RgbDevice> prepared{device};
    try { resized = OpenRgbClient().initializeEmptyZones(prepared); }
    catch (...) { }
    if (!prepared.empty()) device = prepared.front();
    if (initializedZones) *initializedZones += resized;

    const fs::path executable = openRgbExecutable();
    bool applied = false;
    if (localCertification(device) == LocalCertification::Passed) {
        std::wstring directMode = advertisedMode(device, L"Direct");
        if (directMode.empty() && device.modes.empty()) directMode = L"Direct";
        if (!directMode.empty() && runOpenRgbMode(executable, device, directMode, color, false)) {
            try {
                OpenRgbClient().sendEffectFrame({device}, L"Static", 0.0, color, 1.0);
                return true;
            } catch (...) { }
        }
    }
    std::wstring staticMode = staticModeFor(device);
    if (!staticMode.empty()) applied = runOpenRgbMode(executable, device, staticMode, color, true);
    if (!applied && device.modes.empty()) applied = runOpenRgbMode(executable, device, L"Static", color, true);
    if (applied) {
        try { OpenRgbClient().sendEffectFrame({device}, L"Static", 0.0, color, 1.0); }
        catch (...) { }
    }
    if (!applied) {
        std::wstring directMode = advertisedMode(device, L"Direct");
        if (directMode.empty() && device.modes.empty()) directMode = L"Direct";
        if (!directMode.empty() && runOpenRgbMode(executable, device, directMode, color, false)) {
            try {
                OpenRgbClient().sendEffectFrame({device}, L"Static", 0.0, color, 1.0);
                applied = true;
            } catch (...) { }
        }
    }
    if (!applied) failure = L"Aucun mode couleur accepté";
    return applied;
}

bool isDualSenseDevice(const RgbDevice& device) {
    return device.providerId == L"plugin:com.sony.dualsense" ||
           device.providerId == L"plugin:com.sony.dualsense-edge";
}

int dualSenseDeviceCount() {
    return static_cast<int>(std::count_if(g_rgbDevices.begin(), g_rgbDevices.end(), [](const RgbDevice& device) {
        return isDualSenseDevice(device);
    }));
}

void applyDualSenseColor() {
    std::vector<RgbDevice> devices;
    for (RgbDevice& device : g_rgbDevices) {
        if (!isDualSenseDevice(device)) continue;
        device.auxiliaryLedMask = g_dualSensePlayerLedsEnabled ? 0x1f : 0;
        devices.push_back(device);
    }
    if (devices.empty()) {
        g_status = localized(L"Aucune DualSense contrôlable n'est connectée.", L"No controllable DualSense is connected.",
                             L"Kein steuerbarer DualSense-Controller ist verbunden.", L"未连接可控制的 DualSense。");
        return;
    }
    const std::uint32_t color = scaleColor(g_baseColor, g_brightness / 100.0);
    const Language language = g_language;
    g_status = localized(L"Mise à jour des lumières de la manette…", L"Updating controller lights…",
                         L"Controller-Beleuchtung wird aktualisiert…", L"正在更新手柄灯光…");
    g_pluginThreads.emplace_back([devices = std::move(devices), color, language] {
        int applied = 0;
        std::wstring firstFailure;
        for (const RgbDevice& device : devices) {
            std::wstring failure;
            if (applyColorToDevice(device, color, failure)) ++applied;
            else if (firstFailure.empty()) firstFailure = failure;
        }
        postStatus(applied == static_cast<int>(devices.size())
            ? localizedFor(language, L"Lumières DualSense mises à jour.", L"DualSense lights updated.",
                           L"DualSense-Beleuchtung aktualisiert.", L"DualSense 灯光已更新。")
            : localizedFor(language, L"La manette a refusé la commande : ", L"The controller rejected the command: ",
                           L"Der Controller hat den Befehl abgelehnt: ", L"手柄拒绝了命令：") + firstFailure);
    });
}

int selectedRgbCount() {
    return static_cast<int>(std::count_if(g_rgbDevices.begin(), g_rgbDevices.end(), [](const RgbDevice& device) { return device.selected; }));
}

void applyStaticColor() {
    const std::wstring conflict = rgbConflictProcess();
    if (!conflict.empty()) {
        g_status = rgbConflictMessage(conflict);
        InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
    if (selectedRgbCount() == 0) {
        g_status = localized(L"Sélectionne au moins un appareil compatible.", L"Select at least one compatible device.",
                             L"Wähle mindestens ein kompatibles Gerät aus.", L"请至少选择一个兼容设备。");
        InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
    g_effectActive = false;
    g_activeEffectIndex = 0;
    if (g_effectThread.joinable()) g_effectThread.join();
    std::vector<RgbDevice> devices = g_rgbDevices;
    std::uint32_t color = scaleColor(g_baseColor, g_brightness / 100.0);
    g_status = localized(L"Application de la couleur...", L"Applying color...", L"Farbe wird angewendet...", L"正在应用颜色…");
    InvalidateRect(g_window, nullptr, FALSE);
    const Language language = g_language;
    std::thread([devices, color, language]() mutable {
        int initializedZones = 0;
        int appliedCount = 0;
        int selectedCount = 0;
        std::wstring firstFailure;
        for (const RgbDevice& device : devices) {
            if (!device.selected) continue;
            ++selectedCount;
            std::wstring failure;
            const bool applied = applyColorToDevice(device, color, failure, &initializedZones);
            if (applied) ++appliedCount;
            else if (firstFailure.empty()) firstFailure = device.name + (failure.empty() ? L"" : L" · " + failure);
        }
        if (appliedCount == selectedCount && selectedCount > 0) {
            const std::wstring zoneNote = initializedZones > 0
                ? L" · " + std::to_wstring(initializedZones) + localizedFor(language,
                    L" sortie(s) ARGB initialisée(s)", L" ARGB output(s) initialized",
                    L" ARGB-Ausgang/Ausgänge initialisiert", L" 个 ARGB 输出已初始化")
                : L"";
            postStatus(localizedFor(language, L"Commande couleur envoyée à chaque appareil.", L"Color command sent to every device.",
                                    L"Farbbefehl an jedes Gerät gesendet.", L"颜色命令已发送到每个设备。") + zoneNote);
        } else if (appliedCount > 0) {
            postStatus(std::to_wstring(appliedCount) + L" / " + std::to_wstring(selectedCount) +
                       localizedFor(language, L" appareil(s) appliqué(s) · incompatible : ", L" device(s) applied · incompatible: ",
                                    L" Gerät(e) angewendet · inkompatibel: ", L" 个设备已应用 · 不兼容：") + firstFailure);
        } else {
            postStatus(localizedFor(language, L"Aucun mode couleur compatible pour : ", L"No compatible color mode for: ",
                                    L"Kein kompatibler Farbmodus für: ", L"没有兼容的颜色模式：") + firstFailure);
        }
    }).detach();
}

void restoreAfterCertification(const RgbDevice& device) {
    const std::uint32_t color = scaleColor(g_baseColor, g_brightness / 100.0);
    g_pluginThreads.emplace_back([device, color] {
        std::wstring ignored;
        applyColorToDevice(device, color, ignored);
    });
}

RgbFrameTransport certificationTransportFor(const RgbDevice& device) {
    const bool atomicFirst = OpenRgbClient::prefersAtomicFrames(device);
    if (g_certificationTransportAttempt == 0) {
        return atomicFirst ? RgbFrameTransport::AtomicDevice : RgbFrameTransport::PerZone;
    }
    return atomicFirst ? RgbFrameTransport::PerZone : RgbFrameTransport::AtomicDevice;
}

RgbColorOrder certificationColorOrder() {
    return static_cast<RgbColorOrder>((g_certificationBaseOrder + g_certificationOrderAttempt) % 6);
}

int certificationFrameInterval() {
    static constexpr int intervals[] = {25, 40, 70, 100};
    return intervals[std::clamp(g_certificationRefreshAttempt, 0, 3)];
}

RgbDevice configuredCertificationDevice(int deviceIndex) {
    RgbDevice device = g_rgbDevices[deviceIndex];
    device.selected = true;
    if (!PluginEngine::isPluginDevice(device)) {
        device.frameTransport = certificationTransportFor(device);
        device.colorOrder = certificationColorOrder();
        device.frameIntervalMs = certificationFrameInterval();
    }
    return device;
}

bool prepareCalibrationDevice(RgbDevice& device, std::wstring& failure) {
    if (PluginEngine::isPluginDevice(device)) return true;
    if (device.providerId != L"openrgb" || !ensureOpenRgbServer()) {
        failure = localized(L"Moteur OpenRGB indisponible", L"OpenRGB engine unavailable",
                            L"OpenRGB-Engine nicht verfügbar", L"OpenRGB 引擎不可用");
        return false;
    }
    std::vector<RgbDevice> prepared{device};
    try { OpenRgbClient().initializeEmptyZones(prepared, device.preferredLeds); }
    catch (...) { }
    if (!prepared.empty()) device = prepared.front();
    std::wstring directMode = advertisedMode(device, L"Direct");
    if (directMode.empty() && device.modes.empty()) directMode = L"Direct";
    if (directMode.empty()) {
        failure = localized(L"Ce contrôleur ne propose pas le mode Direct.", L"This controller does not provide Direct mode.",
                            L"Dieser Controller bietet keinen Direct-Modus.", L"此控制器不提供直接模式。");
        return false;
    }
    if (!runOpenRgbMode(openRgbExecutable(), device, directMode, 0, false)) {
        failure = localized(L"Le mode Direct a été refusé.", L"Direct mode was rejected.",
                            L"Der Direct-Modus wurde abgelehnt.", L"直接模式被拒绝。");
        return false;
    }
    return true;
}

bool runCalibrationTest(RgbDevice device, bool motion, std::uint32_t color, std::wstring& failure) {
    if (PluginEngine::isPluginDevice(device)) {
        PluginCommandResult result = motion && hasCapability(device.capabilities, DeviceCapability::Effects)
            ? g_pluginEngine.applyEffect(device, L"Gradient", g_baseColor, g_brightness, 55, 80)
            : g_pluginEngine.applyStatic(device, color, 100);
        if (!result.success) failure = result.message;
        return result.success;
    }
    if (!prepareCalibrationDevice(device, failure)) return false;
    try {
        OpenRgbClient client;
        if (!motion) {
            client.sendEffectFrame({device}, L"Static", 0.0, color, 1.0);
            return true;
        }
        const int interval = certificationFrameInterval();
        const int frameCount = std::max(12, 1700 / interval);
        double phase = 0.0;
        for (int frame = 0; frame < frameCount && g_running; ++frame) {
            client.sendEffectFrame({device}, L"Gradient", phase, g_baseColor, 0.82);
            phase = std::fmod(phase + interval * 0.026, 360.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
        return g_running;
    } catch (const std::exception& error) {
        failure = widen(error.what());
        return false;
    }
}

void finishCertificationFailure(const std::wstring& reason) {
    if (g_certificationDevice < 0 || g_certificationDevice >= static_cast<int>(g_rgbDevices.size())) return;
    const RgbDevice device = g_rgbDevices[g_certificationDevice];
    saveLocalCertification(device, false);
    g_certificationApplying = false;
    g_certificationAwaitingAnswer = false;
    g_certificationMotionTest = false;
    g_certificationDevice = -1;
    g_certificationStatus = reason;
    restoreAfterCertification(device);
}

void startCertificationStage(int deviceIndex) {
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(g_rgbDevices.size()) || g_certificationApplying) return;
    const std::wstring conflict = rgbConflictProcess();
    if (!conflict.empty()) {
        g_certificationStatus = rgbConflictMessage(conflict);
        g_certificationDevice = -1;
        g_certificationApplying = false;
        g_certificationAwaitingAnswer = false;
        g_certificationMotionTest = false;
        return;
    }
    g_effectActive = false;
    if (g_effectThread.joinable()) g_effectThread.join();
    if (g_certificationThread.joinable()) g_certificationThread.join();
    g_certificationDevice = deviceIndex;
    g_certificationStage = std::clamp(g_certificationStage, 0, 2);
    g_certificationApplying = true;
    g_certificationAwaitingAnswer = false;
    const std::uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF};
    const RgbDevice device = configuredCertificationDevice(deviceIndex);
    const int stage = g_certificationStage;
    const bool motion = g_certificationMotionTest;
    g_certificationStatus = motion
        ? localized(L"Test de fluidité pendant deux secondes…", L"Testing smoothness for two seconds…",
                    L"Flüssigkeitstest für zwei Sekunden…", L"正在进行两秒流畅度测试…")
        : localized(L"Envoi de la couleur de test…", L"Sending the test color…",
                    L"Testfarbe wird gesendet…", L"正在发送测试颜色…");
    InvalidateRect(g_window, nullptr, FALSE);
    g_certificationThread = std::thread([device, deviceIndex, stage, motion, color = colors[stage]] {
        std::unique_ptr<CertificationCommandResult> result(new CertificationCommandResult());
        result->device = deviceIndex;
        result->stage = stage;
        result->motion = motion;
        result->success = runCalibrationTest(device, motion, color, result->message);
        if (!g_window || !PostMessageW(g_window, WM_CERTIFICATION_COMPLETE, 0,
                                       reinterpret_cast<LPARAM>(result.get()))) return;
        result.release();
    });
}

void beginCertification(int deviceIndex) {
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(g_rgbDevices.size())) return;
    g_certificationStage = 0;
    g_certificationOrderAttempt = 0;
    g_certificationBaseOrder = static_cast<int>(g_rgbDevices[deviceIndex].colorOrder);
    g_certificationTransportAttempt = 0;
    g_certificationRefreshAttempt = 0;
    g_certificationMotionTest = false;
    startCertificationStage(deviceIndex);
}

void retryCertificationCandidate(bool motionFailure) {
    if (g_certificationDevice < 0 || g_certificationDevice >= static_cast<int>(g_rgbDevices.size())) return;
    if (PluginEngine::isPluginDevice(g_rgbDevices[g_certificationDevice])) {
        finishCertificationFailure(localized(L"Le plugin n'a pas reproduit le résultat attendu.",
                                              L"The plugin did not reproduce the expected result.",
                                              L"Das Plugin hat das erwartete Ergebnis nicht reproduziert.",
                                              L"插件未呈现预期结果。"));
        return;
    }
    if (motionFailure) {
        if (++g_certificationRefreshAttempt < 4) {
            startCertificationStage(g_certificationDevice);
            return;
        }
        g_certificationRefreshAttempt = 0;
        if (++g_certificationTransportAttempt < 2) {
            g_certificationOrderAttempt = 0;
            g_certificationStage = 0;
            g_certificationMotionTest = false;
            startCertificationStage(g_certificationDevice);
            return;
        }
    } else {
        g_certificationStage = 0;
        if (++g_certificationOrderAttempt < 6) {
            startCertificationStage(g_certificationDevice);
            return;
        }
        g_certificationOrderAttempt = 0;
        if (++g_certificationTransportAttempt < 2) {
            startCertificationStage(g_certificationDevice);
            return;
        }
    }
    finishCertificationFailure(localized(L"Aucune combinaison sûre n'a été validée sur cet appareil.",
                                          L"No safe combination was validated on this device.",
                                          L"Für dieses Gerät wurde keine sichere Kombination bestätigt.",
                                          L"此设备没有通过任何安全组合验证。"));
}

void answerCertification(bool visible) {
    if (!g_certificationAwaitingAnswer || g_certificationDevice < 0 ||
        g_certificationDevice >= static_cast<int>(g_rgbDevices.size())) return;
    g_certificationAwaitingAnswer = false;
    if (!visible) {
        retryCertificationCandidate(g_certificationMotionTest);
        return;
    }
    if (g_certificationMotionTest) {
        RgbDevice& device = g_rgbDevices[g_certificationDevice];
        if (!PluginEngine::isPluginDevice(device)) {
            device.frameTransport = certificationTransportFor(device);
            device.colorOrder = certificationColorOrder();
            device.frameIntervalMs = certificationFrameInterval();
        }
        saveLocalCertification(device, true);
        g_certificationStatus = localized(L"Auto-calibration réussie et enregistrée pour cet appareil.",
                                          L"Auto-calibration passed and was saved for this device.",
                                          L"Autokalibrierung erfolgreich und für dieses Gerät gespeichert.",
                                          L"自动校准成功并已为此设备保存。");
        g_certificationMotionTest = false;
        g_certificationDevice = -1;
        restoreAfterCertification(device);
        return;
    }
    if (g_certificationStage < 2) {
        ++g_certificationStage;
        startCertificationStage(g_certificationDevice);
        return;
    }
    if (PluginEngine::isPluginDevice(g_rgbDevices[g_certificationDevice])) {
        RgbDevice device = g_rgbDevices[g_certificationDevice];
        saveLocalCertification(device, true);
        g_certificationStatus = localized(L"Validation du plugin réussie et enregistrée.", L"Plugin validation passed and was saved.",
                                          L"Plugin-Validierung erfolgreich gespeichert.", L"插件验证通过并已保存。");
        g_certificationDevice = -1;
        restoreAfterCertification(device);
        return;
    }
    g_certificationMotionTest = true;
    g_certificationRefreshAttempt = 0;
    startCertificationStage(g_certificationDevice);
}

void startEffect() {
    const std::wstring conflict = rgbConflictProcess();
    if (!conflict.empty()) {
        g_status = rgbConflictMessage(conflict);
        InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
    if (selectedRgbCount() == 0) {
        g_status = localized(L"Sélectionne au moins un appareil compatible.", L"Select at least one compatible device.",
                             L"Wähle mindestens ein kompatibles Gerät aus.", L"请至少选择一个兼容设备。");
        InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
    if (g_selectedEffect == 0) { applyStaticColor(); return; }
    g_effectActive = false;
    if (g_effectThread.joinable()) g_effectThread.join();
    std::vector<RgbDevice> devices = g_rgbDevices;
    std::wstring mode = kEffects[g_selectedEffect].internal;
    std::uint32_t baseColor = g_baseColor;
    const bool musicMode = mode == L"Music";
    const bool ambientMode = mode == L"Ambilight";
    if (ambientMode) {
        g_screenMonitors = AmbientScreenCapture::enumerateMonitors();
        if (g_screenMonitors.empty()) {
            g_status = localized(L"Aucun écran Windows n'a été détecté.", L"No Windows display was detected.",
                                 L"Kein Windows-Bildschirm erkannt.", L"未检测到 Windows 显示器。");
            InvalidateRect(g_window, nullptr, FALSE);
            return;
        }
        g_ambientMonitorIndex = std::clamp(g_ambientMonitorIndex, 0, static_cast<int>(g_screenMonitors.size()) - 1);
    }
    const int ambientMonitor = g_ambientMonitorIndex;
    g_audioCaptureReady = false;
    g_audioSignal = false;
    g_audioVolume = g_audioBass = g_audioMid = g_audioTreble = 0.0;
    g_ambientCaptureReady = false;
    g_ambientFps = 0.0;
    g_ambientAverage = 0;
    for (auto& color : g_ambientColors) color = 0;
    g_effectActive = true;
    g_activeEffectIndex = g_selectedEffect;
    g_effectStartedAt = GetTickCount64();
    g_status = musicMode
        ? localized(L"Connexion à la sortie audio Windows...", L"Connecting to Windows audio output...",
                    L"Verbindung mit der Windows-Audioausgabe...", L"正在连接 Windows 音频输出…")
        : ambientMode
            ? localized(L"Connexion à l'écran Windows...", L"Connecting to the Windows display...",
                        L"Verbindung mit dem Windows-Bildschirm...", L"正在连接 Windows 显示器…")
        : std::wstring(localized(L"Effet ", L"Effect ", L"Effekt ", L"效果 ")) + effectLabel(g_selectedEffect) +
              localized(L" actif.", L" is active.", L" ist aktiv.", L" 已启用。");
    InvalidateRect(g_window, nullptr, FALSE);
    const Language language = g_language;
    g_effectThread = std::thread([devices, mode, baseColor, language, ambientMonitor]() mutable {
        std::vector<RgbDevice> openRgbSelected;
        int pluginApplied = 0;
        int pluginFailures = 0;
        std::wstring firstPluginFailure;
        for (const RgbDevice& device : devices) {
            if (!device.selected) continue;
            if (PluginEngine::isPluginDevice(device)) {
                PluginCommandResult result = hasCapability(device.capabilities, DeviceCapability::Effects)
                    ? g_pluginEngine.applyEffect(device, mode, baseColor, g_brightness,
                                                 g_effectSpeed.load(), g_effectIntensity.load())
                    : g_pluginEngine.applyStatic(device, scaleColor(baseColor, g_brightness / 100.0), 100);
                if (result.success) ++pluginApplied;
                else {
                    ++pluginFailures;
                    if (firstPluginFailure.empty()) firstPluginFailure = device.name + L" · " + result.message;
                }
            } else if (device.providerId == L"openrgb") {
                openRgbSelected.push_back(device);
            }
        }
        if (openRgbSelected.empty()) {
            postStatus(pluginApplied > 0
                ? std::to_wstring(pluginApplied) + localizedFor(language,
                    L" appareil(s) piloté(s) par plugins RGBCcontrol.", L" device(s) driven by RGBCcontrol plugins.",
                    L" Gerät(e) durch RGBCcontrol-Plugins gesteuert.", L" 个设备由 RGBCcontrol 插件驱动。")
                : localizedFor(language, L"Aucun plugin n'a accepté cet effet : ", L"No plugin accepted this effect: ",
                    L"Kein Plugin hat diesen Effekt akzeptiert: ", L"没有插件接受此效果：") + firstPluginFailure);
            // Process plugins such as the DualSense driver own their animation
            // loop after a successful command. Keep the UI in the active state;
            // applying another effect, a static color, or closing the app sends
            // the corresponding replacement/stop command.
            if (pluginApplied == 0) g_effectActive = false;
            return;
        }
        devices = std::move(openRgbSelected);
        if (!ensureOpenRgbServer()) {
            postStatus(pluginApplied > 0
                ? localizedFor(language, L"Plugins appliqués, mais OpenRGB est indisponible.", L"Plugins applied, but OpenRGB is unavailable.",
                    L"Plugins angewendet, aber OpenRGB ist nicht verfügbar.", L"插件已应用，但 OpenRGB 不可用。")
                : localizedFor(language, L"OpenRGB est indisponible.", L"OpenRGB is unavailable.",
                    L"OpenRGB ist nicht verfügbar.", L"OpenRGB 不可用。"));
            g_effectActive = false;
            return;
        }
        try { OpenRgbClient().initializeEmptyZones(devices); }
        catch (...) { }
        fs::path executable = openRgbExecutable();
        std::vector<RgbDevice> directDevices;
        int hardwareFallbacks = 0;
        int incompatibleDevices = 0;
        for (const RgbDevice& device : devices) {
            if (!device.selected) continue;
            std::wstring directMode = advertisedMode(device, L"Direct");
            if (directMode.empty() && device.modes.empty()) directMode = L"Direct";
            if (!directMode.empty() && runOpenRgbMode(executable, device, directMode, baseColor, false)) {
                directDevices.push_back(device);
                continue;
            }

            // No software Direct mode: keep this controller lit with its own
            // closest hardware mode instead of aborting/blanking every device.
            std::wstring hardwareMode = advertisedMode(device, mode);
            if (hardwareMode.empty()) hardwareMode = staticModeFor(device);
            if (!hardwareMode.empty() && runOpenRgbMode(executable, device, hardwareMode, baseColor, true)) ++hardwareFallbacks;
            else ++incompatibleDevices;
        }
        devices = std::move(directDevices);
        if (devices.empty()) {
            const int hardwareTotal = hardwareFallbacks + pluginApplied;
            postStatus(hardwareTotal > 0
                ? std::to_wstring(hardwareTotal) + localizedFor(language,
                    L" appareil(s) utilisent un effet matériel ou un plugin.", L" device(s) use a hardware effect or plugin.",
                    L" Gerät(e) verwenden einen Hardware-Effekt oder ein Plugin.", L" 个设备使用硬件效果或插件。")
                : localizedFor(language, L"Aucun appareil sélectionné ne propose de mode d'éclairage compatible.",
                    L"No selected device provides a compatible lighting mode.",
                    L"Kein ausgewähltes Gerät bietet einen kompatiblen Beleuchtungsmodus.",
                    L"所选设备均不提供兼容的灯光模式。") +
                    (firstPluginFailure.empty() ? L"" : L" " + firstPluginFailure));
            g_effectActive = false;
            return;
        }
        if (hardwareFallbacks > 0 || incompatibleDevices > 0 || pluginApplied > 0 || pluginFailures > 0) {
            postStatus(std::to_wstring(devices.size()) + localizedFor(language,
                L" appareil(s) synchronisé(s) en direct · ", L" device(s) synchronized live · ",
                L" Gerät(e) live synchronisiert · ", L" 个设备实时同步 · ") +
                std::to_wstring(hardwareFallbacks + pluginApplied) + localizedFor(language,
                L" en effet matériel/plugin.", L" using a hardware effect/plugin.", L" mit Hardware-Effekt/Plugin.", L" 个使用硬件灯效/插件。"));
        }
        int calibratedFrameFloor = 20;
        for (const RgbDevice& device : devices) calibratedFrameFloor = std::max(calibratedFrameFloor, device.frameIntervalMs);
        const bool musicMode = mode == L"Music";
        const bool ambientMode = mode == L"Ambilight";
        std::unique_ptr<LoopbackAudioAnalyzer> audio;
        std::unique_ptr<AmbientScreenCapture> screen;
        if (musicMode) {
            audio = std::make_unique<LoopbackAudioAnalyzer>();
            std::wstring audioError;
            if (!audio->start(audioError)) {
                postStatus(std::wstring(localizedFor(language,
                    L"Impossible d'écouter la sortie audio Windows : ", L"Could not listen to Windows audio output: ",
                    L"Windows-Audioausgabe konnte nicht erfasst werden: ", L"无法监听 Windows 音频输出：")) + audioError);
                g_effectActive = false;
                return;
            }
            g_audioCaptureReady = true;
            postStatus(localizedFor(language, L"Musique réelle active · écoute de la sortie Windows.",
                                    L"Real music mode active · listening to Windows output.",
                                    L"Echter Musikmodus aktiv · Windows-Ausgabe wird analysiert.",
                                     L"真实音乐模式已启用 · 正在监听 Windows 输出。"));
        }
        if (ambientMode) {
            screen = std::make_unique<AmbientScreenCapture>();
            std::wstring screenError;
            const int quality = g_effectQuality.load();
            const int sampleWidth = quality == 0 ? 32 : quality == 2 ? 96 : 64;
            const int sampleHeight = quality == 0 ? 18 : quality == 2 ? 54 : 36;
            if (!screen->start(ambientMonitor, sampleWidth, sampleHeight, screenError)) {
                postStatus(std::wstring(localizedFor(language,
                    L"Impossible d'analyser l'écran : ", L"Could not analyze the display: ",
                    L"Bildschirm konnte nicht analysiert werden: ", L"无法分析屏幕：")) + screenError);
                g_effectActive = false;
                return;
            }
            g_ambientCaptureReady = true;
            postStatus(localizedFor(language, L"Ambilight actif · les couleurs de l'écran sont analysées.",
                                    L"Ambilight active · screen colors are being analyzed.",
                                    L"Ambilight aktiv · Bildschirmfarben werden analysiert.",
                                    L"屏幕氛围灯已启用 · 正在分析屏幕颜色。"));
        }
        double phase = 0;
        int errors = 0;
        const bool seamlessGradient = mode == L"Gradient";
        auto gradientLastFrame = std::chrono::steady_clock::now();
        while (g_running && g_effectActive) {
            try {
                if (seamlessGradient) {
                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed = std::clamp(std::chrono::duration<double>(now - gradientLastFrame).count(), 0.0, 0.20);
                    gradientLastFrame = now;
                    // Integrating speed preserves the current color when the
                    // slider moves. Recomputing elapsed*time by the new speed
                    // caused the gradient to jump to an unrelated color.
                    phase = std::fmod(phase + elapsed * gradientDegreesPerSecond(g_effectSpeed.load()), 360.0);
                }
                AudioBands bands;
                AmbientFrame ambient;
                if (musicMode) {
                    std::wstring audioError;
                    if (!audio->sample(bands, audioError)) {
                        postStatus(std::wstring(localizedFor(language,
                            L"Mode Musique arrêté : la sortie audio a changé. Relance l'effet. ",
                            L"Music mode stopped: the audio output changed. Start the effect again. ",
                            L"Musikmodus gestoppt: Die Audioausgabe hat sich geändert. Effekt neu starten. ",
                            L"音乐模式已停止：音频输出已更改。请重新启动效果。")) + audioError);
                        g_effectActive = false;
                        break;
                    }
                    const double sensitivity = std::pow(4.0, (g_effectSpeed.load() - 50) / 50.0);
                    auto adjusted = [sensitivity](double level) { return std::clamp(level * sensitivity, 0.0, 1.0); };
                    bands.volume = adjusted(bands.volume);
                    bands.bass = adjusted(bands.bass);
                    bands.mid = adjusted(bands.mid);
                    bands.treble = adjusted(bands.treble);
                    g_audioVolume = bands.volume;
                    g_audioBass = bands.bass;
                    g_audioMid = bands.mid;
                    g_audioTreble = bands.treble;
                    g_audioSignal = bands.signal;
                }
                if (ambientMode) {
                    std::wstring screenError;
                    if (!screen->sample(ambient, g_ambientSaturation.load(), g_effectSpeed.load(), g_ambientZones.load(), screenError)) {
                        postStatus(std::wstring(localizedFor(language,
                            L"Ambilight arrêté : ", L"Ambilight stopped: ", L"Ambilight gestoppt: ", L"屏幕氛围灯已停止：")) + screenError);
                        g_effectActive = false;
                        break;
                    }
                    for (int zone = 0; zone < 4; ++zone) g_ambientColors[zone] = ambient.zones[zone];
                    g_ambientAverage = ambient.average;
                    g_ambientFps = ambient.fps;
                }
                OpenRgbClient().sendEffectFrame(devices, mode, phase, baseColor, g_effectIntensity.load() / 100.0,
                                                bands.bass, bands.mid, bands.treble, bands.volume,
                                                ambient.zones, ambient.average);
                if (!seamlessGradient && !ambientMode) phase += 0.16 + g_effectSpeed.load() / 115.0;
                errors = 0;
            } catch (...) {
                if (++errors >= 3) {
                    postStatus(localizedFor(language, L"Animation arrêtée : connexion OpenRGB interrompue.", L"Animation stopped: OpenRGB connection lost.",
                                                  L"Animation gestoppt: OpenRGB-Verbindung unterbrochen.", L"动画已停止：OpenRGB 连接中断。"));
                    g_effectActive = false;
                    break;
                }
            }
            const int quality = g_effectQuality.load();
            int interval = ambientMode ? (quality == 0 ? 66 : quality == 2 ? 25 : 36)
                           : musicMode ? (quality == 0 ? 58 : quality == 2 ? 24 : 36)
                           : seamlessGradient ? (quality == 0 ? 70 : quality == 2 ? 25 : 40)
                                             : std::max(70, 500 - g_effectSpeed.load() * 4);
            if (!musicMode && !ambientMode && !seamlessGradient && quality == 0) interval = std::max(120, interval * 3 / 2);
            if (!musicMode && !ambientMode && !seamlessGradient && quality == 2) interval = std::max(40, interval * 2 / 3);
            interval = std::max(interval, calibratedFrameFloor);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
        g_audioCaptureReady = false;
        g_audioSignal = false;
        g_audioVolume = g_audioBass = g_audioMid = g_audioTreble = 0.0;
        g_ambientCaptureReady = false;
        g_ambientFps = 0.0;
    });
}

void startCompatibilityAudit() {
    if (g_compatibilityRunning) return;
    if (g_compatibilityThread.joinable()) g_compatibilityThread.join();
    g_compatibilityRunning = true;
    g_compatibilityFinished = false;
    g_compatibilityProgress = 4;
    g_compatibilityStep = 0;
    g_compatOpenRgb = g_compatAudio = g_compatScreen = g_compatHardware = false;
    g_compatibilityThread = std::thread([] {
        auto advance = [](int step, int progress) {
            g_compatibilityStep = step;
            g_compatibilityProgress = progress;
            if (g_window) InvalidateRect(g_window, nullptr, FALSE);
        };
        advance(0, 12);
        std::this_thread::sleep_for(std::chrono::milliseconds(130));
        if (!g_running) { g_compatibilityRunning = false; return; }

        advance(1, 28);
        g_compatOpenRgb = OpenRgbClient().canConnect(900);
        if (!g_compatOpenRgb && g_running) {
            ensureOpenRgbServer();
            g_compatOpenRgb = OpenRgbClient().canConnect(900);
        }

        advance(2, 50);
        {
            LoopbackAudioAnalyzer audio;
            std::wstring error;
            g_compatAudio = audio.start(error);
            if (g_compatAudio) {
                std::this_thread::sleep_for(std::chrono::milliseconds(35));
                AudioBands bands;
                g_compatAudio = audio.sample(bands, error);
            }
        }
        if (!g_running) { g_compatibilityRunning = false; return; }

        advance(3, 72);
        {
            const auto monitors = AmbientScreenCapture::enumerateMonitors();
            if (!monitors.empty()) {
                AmbientScreenCapture capture;
                AmbientFrame frame;
                std::wstring error;
                g_compatScreen = capture.start(0, 32, 18, error) && capture.sample(frame, 60, 70, true, error) && frame.valid;
            }
        }

        advance(4, 90);
        g_compatHardware = fs::exists(bridgeExecutable());
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        g_compatibilityProgress = 100;
        g_compatibilityStep = 5;
        g_compatibilityFinished = true;
        g_compatibilityRunning = false;
        if (g_window) InvalidateRect(g_window, nullptr, FALSE);
    });
}

void syncDuckyCompanions() {
    if (!g_duckyModeConfirmed) {
        g_duckyStatus = localized(L"Confirme d'abord le rendu du clavier dans l'étape 2.",
                                  L"Confirm the keyboard look in step 2 first.",
                                  L"Bestätige zuerst den Tastatur-Look in Schritt 2.",
                                  L"请先在第 2 步确认键盘效果。");
        return;
    }
    if (selectedRgbCount() == 0) {
        g_duckyStatus = localized(L"Aucun autre appareil RGB n'est inclus dans la synchronisation.",
                                  L"No other RGB device is included in synchronization.",
                                  L"Kein anderes RGB-Gerät ist in der Synchronisierung enthalten.",
                                  L"同步中未包含其他 RGB 设备。");
        return;
    }
    const DuckyModeInfo& mode = kDuckyModes[g_duckyMode];
    g_selectedEffect = mode.matchingEffect;
    g_activeProfile = -1;
    if (g_duckyMode == static_cast<int>(std::size(kDuckyModes)) - 1) {
        const std::uint32_t savedColor = g_baseColor;
        g_baseColor = 0;
        applyStaticColor();
        g_baseColor = savedColor;
    } else {
        startEffect();
    }
    g_duckyStatus = localized(L"Équivalent lancé sur les appareils compatibles. Termine les raccourcis sur le Ducky.",
                              L"Matching effect started on compatible devices. Finish the shortcuts on the Ducky.",
                              L"Passender Effekt auf kompatiblen Geräten gestartet. Führe die Tastenkürzel am Ducky aus.",
                              L"已在兼容设备上启动匹配效果。请在 Ducky 上完成快捷键操作。");
}

void changeFan(int index, bool automatic) {
    if (index < 0 || index >= static_cast<int>(g_fans.size()) || !g_fans[index].controllable) return;
    FanDevice& fan = g_fans[index];
    std::string id = fan.encodedId;
    if (g_busyFans.contains(id)) return;
    if (!g_isAdministrator) {
        g_fanStatus = localized(L"Autorisation administrateur demandée...", L"Requesting administrator access...",
                                L"Administratorzugriff wird angefordert...", L"正在请求管理员权限…");
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring parameters = automatic ? L"--elevated --fan-auto \"" : L"--elevated --fan-set \"";
        parameters.append(id.begin(), id.end());
        parameters += L"\"";
        if (!automatic) parameters += L" " + std::to_wstring(std::max(fan.minimum, fan.desired));
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"runas", executable, parameters.c_str(), g_appDirectory.c_str(), SW_SHOWNORMAL)) > 32) {
            PostMessageW(g_window, WM_CLOSE, 0, 0);
        } else {
            g_fanStatus = localized(L"Le contrôle nécessite le mode avancé.", L"Fan control requires advanced mode.",
                                    L"Die Lüftersteuerung benötigt den erweiterten Modus.", L"风扇控制需要高级模式。");
        }
        return;
    }
    int value = std::max(fan.minimum, fan.desired);
    std::wstring name = fan.name;
    const Language language = g_language;
    g_busyFans.insert(id);
    g_lastScan = GetTickCount64();
    if (automatic) {
        g_fanStatus = name + localized(L" repasse en mode automatique...", L" is returning to automatic mode...", L" wechselt zurück in den Automatikmodus...", L" 正在恢复自动模式…");
    } else {
        g_fanStatus = name + localized(L" : application de ", L": applying ", L": Anwenden von ", L"：正在应用 ") + std::to_wstring(value) + L" %...";
    }
    InvalidateRect(g_window, nullptr, FALSE);
    g_fanThreads.emplace_back([id, value, automatic, name, language] {
        DWORD exitCode = 1;
        const std::string command = automatic ? "auto " + id : "set " + id + " " + std::to_string(value);
        const bool started = hardwareCommand(command, nullptr, &exitCode);
        const bool success = started && exitCode == 0;
        std::wstring status = success
            ? name + (automatic
                ? localizedFor(language, L" est en mode automatique.", L" is in automatic mode.", L" ist im Automatikmodus.", L" 已进入自动模式。")
                : localizedFor(language, L" a été réglé avec succès.", L" was adjusted successfully.", L" wurde erfolgreich eingestellt.", L" 已成功调整。"))
            : std::wstring(localizedFor(language, L"Impossible de modifier ", L"Could not adjust ", L"Konnte nicht angepasst werden: ", L"无法调整 ")) + name + L".";
        auto* result = new FanCommandResult{id, std::move(status), success, automatic};
        if (!g_window || !PostMessageW(g_window, WM_FAN_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
    });
}

FanProfile fanProfileFromAction(Action profile) {
    if (profile == Action::FanProfileAuto) return FanProfile::Auto;
    if (profile == Action::FanProfileQuiet) return FanProfile::Quiet;
    if (profile == Action::FanProfileBalanced) return FanProfile::Balanced;
    if (profile == Action::FanProfilePerformance) return FanProfile::Performance;
    return FanProfile::Custom;
}

int fanProfileArgument(Action profile) {
    if (profile == Action::FanProfileAuto) return 0;
    if (profile == Action::FanProfileQuiet) return 1;
    if (profile == Action::FanProfileBalanced) return 2;
    if (profile == Action::FanProfilePerformance) return 3;
    return -1;
}

Action fanProfileAction(int profile) {
    if (profile == 0) return Action::FanProfileAuto;
    if (profile == 1) return Action::FanProfileQuiet;
    if (profile == 2) return Action::FanProfileBalanced;
    if (profile == 3) return Action::FanProfilePerformance;
    return Action::None;
}

void applyFanProfile(Action profile) {
    const int profileArgument = fanProfileArgument(profile);
    if (profileArgument < 0) return;
    g_fanCurveEnabled = false;
    g_curveTarget = -1;
    g_fanProfile = fanProfileFromAction(profile);
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
    if (g_fans.empty()) {
        g_fanStatus = localized(L"Aucun ventilateur contrôlable n'a été détecté.", L"No controllable fan was detected.",
                                L"Es wurde kein steuerbarer Lüfter erkannt.", L"未检测到可控风扇。");
        return;
    }
    if (!g_isAdministrator) {
        g_fanStatus = localized(L"Autorisation administrateur demandée...", L"Requesting administrator access...",
                                L"Administratorzugriff wird angefordert...", L"正在请求管理员权限…");
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const std::wstring parameters = L"--elevated --fan-profile " + std::to_wstring(profileArgument);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"runas", executable, parameters.c_str(), g_appDirectory.c_str(), SW_SHOWNORMAL)) > 32) {
            PostMessageW(g_window, WM_CLOSE, 0, 0);
        } else {
            g_fanStatus = localized(L"Le profil n'a pas été appliqué : autorisation refusée.", L"The profile was not applied: permission denied.",
                                    L"Das Profil wurde nicht angewendet: Zugriff verweigert.", L"未应用模式：权限被拒绝。");
        }
        return;
    }
    for (int index = 0; index < static_cast<int>(g_fans.size()); ++index) {
        if (!g_fans[index].controllable) continue;
        if (profile == Action::FanProfileAuto) {
            changeFan(index, true);
        } else {
            int requested = profile == Action::FanProfileQuiet ? 35 : profile == Action::FanProfileBalanced ? 50 : 75;
            g_fans[index].desired = std::max(g_fans[index].minimum, requested);
            changeFan(index, false);
        }
    }
}

bool hasControllableFans() {
    return std::any_of(g_fans.begin(), g_fans.end(), [](const FanDevice& fan) { return fan.controllable; });
}

bool relaunchElevated(const std::wstring& arguments) {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const std::wstring parameters = L"--elevated " + arguments;
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"runas", executable, parameters.c_str(), g_appDirectory.c_str(), SW_SHOWNORMAL)) <= 32) return false;
    PostMessageW(g_window, WM_CLOSE, 0, 0);
    return true;
}

int fanCurveTarget(double temperature) {
    constexpr std::array<double, 4> temperatures{35.0, 50.0, 70.0, 85.0};
    if (temperature <= temperatures.front()) return g_curveSpeeds.front();
    if (temperature >= temperatures.back()) return g_curveSpeeds.back();
    for (int index = 1; index < 4; ++index) {
        if (temperature > temperatures[index]) continue;
        const double ratio = (temperature - temperatures[index - 1]) / (temperatures[index] - temperatures[index - 1]);
        return static_cast<int>(std::lround(g_curveSpeeds[index - 1] + ratio * (g_curveSpeeds[index] - g_curveSpeeds[index - 1])));
    }
    return g_curveSpeeds.back();
}

void applyFanCurve(bool force = false) {
    if (!g_fanCurveEnabled || !hasControllableFans()) return;
    if (!g_isAdministrator) {
        g_fanStatus = localized(L"Courbe prête : active le mode avancé pour contrôler les ventilateurs.",
                                L"Curve ready: enable advanced mode to control the fans.",
                                L"Kurve bereit: Aktiviere den erweiterten Modus zur Lüftersteuerung.",
                                L"曲线已就绪：请启用高级模式以控制风扇。");
        return;
    }
    const double temperature = std::max(g_cpuTemperature, g_gpuTemperature);
    if (temperature < 0) {
        g_fanStatus = localized(L"Courbe en attente d'une température valide.", L"Curve waiting for a valid temperature.",
                                L"Kurve wartet auf einen gültigen Temperaturwert.", L"曲线正在等待有效温度。");
        return;
    }
    const int target = fanCurveTarget(temperature);
    g_curveTarget = target;
    for (int index = 0; index < static_cast<int>(g_fans.size()); ++index) {
        FanDevice& fan = g_fans[index];
        if (!fan.controllable || g_busyFans.contains(fan.encodedId)) continue;
        const int safeTarget = std::max(fan.minimum, target);
        if (!force && fan.manual && std::abs(fan.desired - safeTarget) < 2) continue;
        fan.desired = safeTarget;
        changeFan(index, false);
    }
}

void setFanCurveEnabled(bool enabled) {
    if (enabled && !g_isAdministrator && hasControllableFans()) {
        saveCurveSettings();
        g_fanStatus = localized(L"Autorisation demandée pour activer la courbe...", L"Requesting permission to enable the curve...",
                                L"Berechtigung zum Aktivieren der Kurve wird angefordert...", L"正在请求启用曲线的权限…");
        if (!relaunchElevated(L"--fan-curve")) {
            g_fanStatus = localized(L"La courbe n'a pas été activée : autorisation refusée.", L"The curve was not enabled: permission denied.",
                                    L"Die Kurve wurde nicht aktiviert: Zugriff verweigert.", L"曲线未启用：权限被拒绝。");
        }
        return;
    }
    g_fanCurveEnabled = enabled;
    g_fanProfile = enabled ? FanProfile::Custom : FanProfile::Auto;
    g_curveTarget = -1;
    saveCurveSettings();
    if (enabled) {
        g_fanStatus = localized(L"Courbe intelligente activée.", L"Smart fan curve enabled.", L"Intelligente Lüfterkurve aktiviert.", L"智能风扇曲线已启用。");
        applyFanCurve(true);
    } else if (g_isAdministrator) {
        for (int index = 0; index < static_cast<int>(g_fans.size()); ++index) {
            if (g_fans[index].controllable) changeFan(index, true);
        }
    }
}

void applyProfileState(const AppProfile& profile) {
    g_baseColor = profile.color;
    g_brightness = profile.brightness;
    g_selectedEffect = profile.effect;
    g_effectSpeed = profile.effectSpeed;
    g_effectIntensity = profile.effectIntensity;
    g_ambientSaturation = profile.ambientSaturation;
    g_ambientMonitorIndex = g_screenMonitors.empty() ? 0 : std::clamp(profile.ambientMonitor, 0, static_cast<int>(g_screenMonitors.size()) - 1);
    g_ambientZones = profile.ambientZones;
    g_fanProfile = profile.fanProfile;
    g_fanCurveEnabled = profile.fanCurveEnabled;
    g_curveSpeeds = profile.curveSpeeds;
    g_curveTarget = -1;
    for (RgbDevice& device : g_rgbDevices) device.selected = serializedListContains(profile.selectedDevices, device);
    for (FanDevice& fan : g_fans) {
        const int value = serializedFanValue(profile.fanValues, fan.encodedId);
        if (value >= 0) fan.desired = std::max(fan.minimum, value);
    }
}

void applyProfileSlot(int index, bool allowElevation = true) {
    if (index < 0 || index >= static_cast<int>(g_profiles.size()) || !g_profiles[index].saved) return;
    applyProfileState(g_profiles[index]);
    g_activeProfile = index;
    bool fullyApplied = true;
    const bool needsFanAccess = hasControllableFans() && (g_fanCurveEnabled || g_fanProfile != FanProfile::Custom || !g_profiles[index].fanValues.empty());
    if (!g_isAdministrator && needsFanAccess) {
        if (allowElevation) {
            g_profileStatus = localized(L"Ouverture du profil en mode administrateur...", L"Opening the profile with administrator access...",
                                        L"Profil wird mit Administratorrechten geöffnet...", L"正在以管理员权限打开模式…");
            if (relaunchElevated(L"--load-profile " + std::to_wstring(index))) return;
        }
        g_profileStatus = localized(L"Le RGB sera appliqué, mais la ventilation exige une autorisation.",
                                    L"RGB will be applied, but fan control requires permission.",
                                    L"RGB wird angewendet, die Lüftersteuerung benötigt jedoch eine Berechtigung.",
                                    L"RGB 将被应用，但风扇控制需要权限。");
        g_fanCurveEnabled = false;
        fullyApplied = false;
    }
    if (g_selectedEffect == 0) applyStaticColor(); else startEffect();
    if (g_isAdministrator) {
        if (g_fanCurveEnabled) applyFanCurve(true);
        else if (g_fanProfile != FanProfile::Custom) applyFanProfile(fanProfileAction(static_cast<int>(g_fanProfile)));
        else {
            for (int fanIndex = 0; fanIndex < static_cast<int>(g_fans.size()); ++fanIndex) {
                if (serializedFanValue(g_profiles[index].fanValues, g_fans[fanIndex].encodedId) >= 0) changeFan(fanIndex, false);
            }
        }
    }
    if (fullyApplied) {
        g_profileStatus = localized(L"Profil appliqué au setup.", L"Profile applied to the setup.",
                                    L"Profil auf das Setup angewendet.", L"模式已应用到设备。");
    }
}

void exportCurrentProfile() {
    fs::path path;
    if (!selectProfileFile(true, path)) return;
    AppProfile profile = captureCurrentProfile();
    writeProfileToIni(path, L"RGBCcontrolProfile", profile);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    g_profileStatus = localized(L"Profil exporté avec succès.", L"Profile exported successfully.",
                                L"Profil erfolgreich exportiert.", L"模式已成功导出。");
}

void importProfile() {
    fs::path path;
    if (!selectProfileFile(false, path)) return;
    AppProfile profile = readProfileFromIni(path, L"RGBCcontrolProfile");
    if (!profile.saved) {
        g_profileStatus = localized(L"Ce fichier n'est pas un profil RGBCcontrol valide.", L"This is not a valid RGBCcontrol profile.",
                                    L"Diese Datei ist kein gültiges RGBCcontrol-Profil.", L"该文件不是有效的 RGBCcontrol 模式。");
        return;
    }
    int slot = g_activeProfile;
    if (slot < 0) {
        auto empty = std::find_if(g_profiles.begin(), g_profiles.end(), [](const AppProfile& candidate) { return !candidate.saved; });
        slot = empty == g_profiles.end() ? 0 : static_cast<int>(std::distance(g_profiles.begin(), empty));
    }
    g_profiles[slot] = profile;
    writeProfileToIni(profileStorePath(true), L"Profile" + std::to_wstring(slot + 1), profile);
    const fs::path store = profileStorePath(false);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, store.c_str());
    g_profileStatus = localized(L"Profil importé. Application en cours...", L"Profile imported. Applying now...",
                                L"Profil importiert. Es wird angewendet...", L"模式已导入，正在应用…");
    applyProfileSlot(slot);
}

bool scheduleUsesDayProfile(int hour) {
    if (g_scheduleDayHour == g_scheduleNightHour) return true;
    if (g_scheduleDayHour < g_scheduleNightHour) return hour >= g_scheduleDayHour && hour < g_scheduleNightHour;
    return hour >= g_scheduleDayHour || hour < g_scheduleNightHour;
}

void evaluateSchedule(bool force = false) {
    if (!g_scheduleEnabled || !g_hasCompletedScan) return;
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    const bool daytime = scheduleUsesDayProfile(localTime.wHour);
    const int slot = daytime ? g_scheduleDayProfile : g_scheduleNightProfile;
    if (!force && slot == g_lastScheduledProfile) return;
    g_lastScheduledProfile = slot;
    if (slot < 0 || slot >= static_cast<int>(g_profiles.size()) || !g_profiles[slot].saved) {
        g_scheduleStatus = localized(L"Planification en attente : le profil choisi est vide.",
                                     L"Schedule waiting: the selected profile is empty.",
                                     L"Zeitplan wartet: Das gewählte Profil ist leer.",
                                     L"计划等待中：所选模式为空。");
        return;
    }
    applyProfileSlot(slot, false);
    g_scheduleStatus = daytime
        ? localized(L"Profil de jour appliqué automatiquement.", L"Day profile applied automatically.",
                    L"Tagesprofil automatisch angewendet.", L"已自动应用日间模式。")
        : localized(L"Profil de nuit appliqué automatiquement.", L"Night profile applied automatically.",
                    L"Nachtprofil automatisch angewendet.", L"已自动应用夜间模式。");
}

const wchar_t* profileName(int index);

void addTrayIcon(HWND window) {
    if (g_trayAdded) return;
    g_trayIcon = {};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = window;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAY_ICON;
    g_trayIcon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    lstrcpynW(g_trayIcon.szTip, L"RGBCcontrol", static_cast<int>(std::size(g_trayIcon.szTip)));
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_trayIcon) != FALSE;
}

void removeTrayIcon() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayAdded = false;
}

void showMainWindow() {
    ShowWindow(g_window, SW_RESTORE);
    SetForegroundWindow(g_window);
}

void showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, TRAY_SHOW, localized(L"Ouvrir RGBCcontrol", L"Open RGBCcontrol", L"RGBCcontrol öffnen", L"打开 RGBCcontrol"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (int index = 0; index < static_cast<int>(g_profiles.size()); ++index) {
        UINT flags = MF_STRING;
        if (!g_profiles[index].saved) flags |= MF_GRAYED;
        if (g_activeProfile == index) flags |= MF_CHECKED;
        const std::wstring label = std::wstring(localized(L"Profil ", L"Profile ", L"Profil ", L"模式 ")) + profileName(index);
        AppendMenuW(menu, flags, TRAY_PROFILE_FIRST + index, label.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TRAY_EXIT, localized(L"Quitter", L"Exit", L"Beenden", L"退出"));
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window, nullptr);
    DestroyMenu(menu);
    if (command == TRAY_SHOW) showMainWindow();
    else if (command >= TRAY_PROFILE_FIRST && command < TRAY_PROFILE_FIRST + g_profiles.size()) applyProfileSlot(static_cast<int>(command - TRAY_PROFILE_FIRST));
    else if (command == TRAY_EXIT) DestroyWindow(g_window);
}

bool isAdministrator() {
    BOOL administrator = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &administrator);
        FreeSid(group);
    }
    return administrator != FALSE;
}

float activeHoverProgress(Action action);

void drawButton(Graphics& graphics, const RectF& rect, const std::wstring& label, bool accent, Action action,
                int index = -1, bool enabled = true) {
    const bool hovered = enabled && g_hoverAction == action && g_hoverIndex == index;
    const float hoverProgress = hovered ? activeHoverProgress(action) : 0.0f;
    const float radius = std::min(11.0f, rect.Height / 2.0f);
    RectF visual(rect.X, rect.Y - hoverProgress * 0.7f, rect.Width, rect.Height);
    GraphicsPath path;
    roundedPath(path, visual, radius);
    GraphicsPath shadowPath;
    roundedPath(shadowPath, RectF(visual.X, visual.Y + 4, visual.Width, visual.Height), radius);
    SolidBrush shadow(Color(static_cast<BYTE>(accent && enabled ? 38 + 22 * hoverProgress : 25), 0, 0, 0));
    graphics.FillPath(&shadow, &shadowPath);
    const Color fill = !enabled ? Color(255, 24, 28, 39)
        : accent ? accentColor()
                 : (hovered ? Color(255, 39, 46, 64) : Color(252, 27, 32, 45));
    const Color border = !enabled ? Color(255, 38, 43, 57)
        : accent ? (hovered ? accentTint(0.62) : accentTint(0.24))
                 : (hovered ? Color(255, 68, 77, 101) : Color(255, 43, 50, 68));
    if (accent && enabled) {
        LinearGradientBrush gradient(PointF(visual.X, visual.Y), PointF(visual.GetRight(), visual.Y),
                                     accentColor(), accentTint(hovered ? 0.34 : 0.18));
        graphics.FillPath(&gradient, &path);
        if (hovered) {
            Pen halo(accentColor(static_cast<BYTE>(45 + 45 * hoverProgress)), 3.0f);
            graphics.DrawPath(&halo, &path);
        }
    } else {
        SolidBrush brush(fill);
        graphics.FillPath(&brush, &path);
    }
    strokeRound(graphics, visual, radius, border, hovered ? 1.35f : 1.0f);
    if (enabled) {
        Pen highlight(Color(accent ? 38 : 22, 255, 255, 255), 1.0f);
        graphics.DrawLine(&highlight, visual.X + radius, visual.Y + 1.0f,
                          visual.GetRight() - radius, visual.Y + 1.0f);
    }
    text(graphics, label, visual, 11, enabled ? (accent ? accentButtonText() : Color(255, 235, 238, 247)) : Color(255, 101, 109, 129),
         FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    if (enabled) addHit(rect, action, index);
}

void drawSlider(Graphics& graphics, const RectF& rect, int value, int minimum, Action action, int index = -1) {
    RectF track(rect.X, rect.Y + rect.Height / 2 - 3, rect.Width, 6);
    fillRound(graphics, track, 3, Color(255, 12, 15, 23));
    strokeRound(graphics, track, 3, Color(255, 40, 47, 65));
    float ratio = (value - minimum) / static_cast<float>(std::max(1, 100 - minimum));
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    RectF filled(track.X, track.Y, std::max(6.0f, track.Width * ratio), track.Height);
    GraphicsPath progressPath;
    roundedPath(progressPath, filled, 3.0f);
    LinearGradientBrush progress(PointF(filled.X, filled.Y), PointF(filled.GetRight(), filled.Y),
                                 accentColor(), accentTint(0.34));
    graphics.FillPath(&progress, &progressPath);
    float thumbX = track.X + track.Width * ratio;
    SolidBrush glow(accentColor(52));
    graphics.FillEllipse(&glow, RectF(thumbX - 11.0f, track.Y + track.Height / 2 - 11.0f, 22.0f, 22.0f));
    SolidBrush thumb(Color(255, 244, 246, 252));
    graphics.FillEllipse(&thumb, RectF(thumbX - 6.5f, track.Y + track.Height / 2 - 6.5f, 13.0f, 13.0f));
    Pen border(accentColor(), 2.2f);
    graphics.DrawEllipse(&border, RectF(thumbX - 7.5f, track.Y + track.Height / 2 - 7.5f, 15.0f, 15.0f));
    addHit(RectF(rect.X, rect.Y - 4, rect.Width, rect.Height + 8), action, index, static_cast<std::uint32_t>(minimum));
}

enum class HeaderHealth { Scanning, Ready, Limited, Error };

bool hasActivePluginDevice() {
    return std::any_of(g_rgbDevices.begin(), g_rgbDevices.end(), [](const RgbDevice& device) {
        return PluginEngine::isPluginDevice(device) && hasCapability(device.capabilities, DeviceCapability::Lighting);
    });
}

HeaderHealth currentHeaderHealth() {
    const int totalDevices = static_cast<int>(g_rgbDevices.size()) + (g_duckyDetected ? 1 : 0);
    if (g_scanInFlight.load() || !g_hasCompletedScan) return HeaderHealth::Scanning;
    if ((g_openRgbReady || hasActivePluginDevice()) && totalDevices > 0) return HeaderHealth::Ready;
    if (totalDevices > 0 || g_openRgbReady) return HeaderHealth::Limited;
    return HeaderHealth::Error;
}

float activeHoverProgress(Action action) {
    if (g_hoverAction != action) return 0.0f;
    if (g_reduceMotion || g_hoverStartedAt == 0) return 1.0f;
    return std::clamp(static_cast<float>(GetTickCount64() - g_hoverStartedAt) / 150.0f, 0.0f, 1.0f);
}

RectF baseLogoSourceCrop(Image* image) {
    if (!image) return RectF();
    const float width = static_cast<float>(image->GetWidth());
    const float height = static_cast<float>(image->GetHeight());
    const float side = std::min(width, height) * 0.82f;
    return RectF((width - side) / 2.0f, (height - side) / 2.0f, side, side);
}

void drawBaseLogoRound(Graphics& graphics, const RectF& destination, double opacity = 1.0) {
    if (!g_logo || g_logo->GetLastStatus() != Ok || opacity <= 0.0) return;
    // Decode and crop the 1280 px source only once. Re-filtering that source on
    // every splash frame made the otherwise light animation feel sluggish.
    static Bitmap* cachedLogo = nullptr;
    if (!cachedLogo) {
        constexpr int cacheSize = 384;
        cachedLogo = new Bitmap(cacheSize, cacheSize, PixelFormat32bppPARGB);
        Graphics cache(cachedLogo);
        cache.Clear(Color(0, 0, 0, 0));
        cache.SetSmoothingMode(SmoothingModeHighQuality);
        cache.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        cache.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        cache.SetCompositingQuality(CompositingQualityHighQuality);
        GraphicsPath cacheClip;
        cacheClip.AddEllipse(RectF(0, 0, cacheSize, cacheSize));
        cache.SetClip(&cacheClip, CombineModeIntersect);
        const RectF source = baseLogoSourceCrop(g_logo.get());
        cache.DrawImage(g_logo.get(), RectF(0, 0, cacheSize, cacheSize), source.X, source.Y,
                        source.Width, source.Height, UnitPixel);
    }

    const GraphicsState state = graphics.Save();
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighSpeed);
    if (opacity >= 0.995) {
        graphics.DrawImage(cachedLogo, destination);
    } else {
        ColorMatrix matrix = {{
            {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, static_cast<float>(std::clamp(opacity, 0.0, 1.0)), 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f, 1.0f}
        }};
        ImageAttributes attributes;
        attributes.SetColorMatrix(&matrix, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
        graphics.DrawImage(cachedLogo, destination, 0.0f, 0.0f,
                           static_cast<REAL>(cachedLogo->GetWidth()), static_cast<REAL>(cachedLogo->GetHeight()),
                           UnitPixel, &attributes);
    }
    graphics.Restore(state);
}

void drawHeader(Graphics& graphics, int width) {
    LinearGradientBrush header(PointF(0, 0), PointF(static_cast<REAL>(width), static_cast<REAL>(kHeaderHeight)),
                               Color(250, 9, 12, 20), Color(246, 15, 18, 29));
    graphics.FillRectangle(&header, 0, 0, width, kHeaderHeight);
    Pen line(Color(178, 35, 41, 57));
    graphics.DrawLine(&line, 0, kHeaderHeight - 1, width, kHeaderHeight - 1);
    SolidBrush logoGlow(accentColor(26));
    graphics.FillEllipse(&logoGlow, RectF(14, 8, 50, 50));
    const RectF logoBounds(20, 14, 38, 38);
    if (g_logo && g_logo->GetLastStatus() == Ok) {
        static Bitmap* roundLogo = nullptr;
        if (!roundLogo) {
            // Supersampling keeps the detailed setup legible in the 40 px header mark.
            roundLogo = new Bitmap(80, 80, PixelFormat32bppPARGB);
            Graphics thumbnail(roundLogo);
            thumbnail.Clear(Color(0, 0, 0, 0));
            drawBaseLogoRound(thumbnail, RectF(0, 0, 80, 80));
        }
        const GraphicsState logoState = graphics.Save();
        // The source is already high-resolution; bilinear sampling keeps the
        // live page responsive while remaining crisp at controller-card size.
        graphics.SetInterpolationMode(InterpolationModeBilinear);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
        graphics.DrawImage(roundLogo, logoBounds);
        graphics.Restore(logoState);
    }
    Pen logoRing(accentTint(0.24), 1.3f);
    graphics.DrawEllipse(&logoRing, logoBounds);
    Pen logoHighlight(Color(88, 255, 255, 255), 1.0f);
    graphics.DrawArc(&logoHighlight, RectF(22, 16, 34, 34), 205.0f, 105.0f);
    text(graphics, L"RGBCcontrol", RectF(70, 11, 166, 25), 17, Color(255, 246, 247, 251), FontStyleBold,
         StringAlignmentNear, StringAlignmentCenter);
    text(graphics, L"CONTROL STUDIO", RectF(71, 36, 145, 15), 8, accentTint(0.38), FontStyleBold,
         StringAlignmentNear, StringAlignmentCenter);

    RectF controls(static_cast<float>(width - 164), 12, 148, 44);
    fillRound(graphics, controls, 13, Color(228, 18, 22, 33));
    strokeRound(graphics, controls, 13, Color(255, 39, 46, 63));

    const bool updateReady = !g_downloadedUpdate.empty();
    constexpr float statusWidth = 268.0f;
    const float statusRight = controls.X - 12.0f - (updateReady ? 54.0f : 0.0f);
    RectF statusPill(statusRight - statusWidth, 16, statusWidth, 36);
    const HeaderHealth health = currentHeaderHealth();
    const int healthState = static_cast<int>(health);
    const ULONGLONG now = GetTickCount64();
    if (healthState != g_headerHealthState) {
        g_headerHealthState = healthState;
        g_headerStatusChangedAt = now;
    }

    Color statusBackground;
    Color statusBorder;
    Color statusForeground;
    std::wstring statusText;
    const int totalDevices = static_cast<int>(g_rgbDevices.size()) + (g_duckyDetected ? 1 : 0);
    const wchar_t* deviceWord = totalDevices == 1
        ? localized(L" appareil", L" device", L" Gerät", L" 台设备")
        : localized(L" appareils", L" devices", L" Geräte", L" 台设备");
    if (health == HeaderHealth::Ready) {
        statusBackground = Color(255, 15, 47, 39);
        statusBorder = Color(255, 30, 91, 73);
        statusForeground = Color(255, 111, 229, 184);
        statusText = std::to_wstring(totalDevices) + deviceWord +
                     (hasActivePluginDevice()
                         ? localized(L" · moteurs RGB actifs", L" · RGB engines active", L" · RGB-Engines aktiv", L" · RGB 引擎已启用")
                         : localized(L" · OpenRGB actif", L" · OpenRGB active", L" · OpenRGB aktiv", L" · OpenRGB 已启用"));
    } else if (health == HeaderHealth::Limited) {
        statusBackground = Color(255, 54, 42, 24);
        statusBorder = Color(255, 106, 76, 31);
        statusForeground = Color(255, 247, 191, 99);
        statusText = std::to_wstring(totalDevices) + deviceWord +
                     localized(L" · mode limité", L" · limited mode", L" · eingeschränkt", L" · 受限模式");
    } else if (health == HeaderHealth::Error) {
        statusBackground = Color(255, 59, 28, 36);
        statusBorder = Color(255, 112, 45, 60);
        statusForeground = Color(255, 255, 126, 143);
        statusText = localized(L"Aucun appareil · hors ligne", L"No devices · offline", L"Keine Geräte · offline", L"无设备 · 离线");
    } else {
        statusBackground = Color(255, 25, 36, 57);
        statusBorder = Color(255, 55, 73, 109);
        statusForeground = Color(255, 151, 181, 243);
        statusText = localized(L"Détection automatique…", L"Automatic detection…", L"Automatische Erkennung…", L"正在自动检测…");
    }
    fillRound(graphics, statusPill, 12, statusBackground);
    const float statusHover = activeHoverProgress(Action::HeaderStatus);
    if (statusHover > 0) {
        fillRound(graphics, statusPill, 12, accentColor(static_cast<BYTE>(46.0f * statusHover)));
    }
    strokeRound(graphics, statusPill, 12, statusBorder, statusHover > 0 ? 1.5f : 1.0f);

    const double statusAge = g_headerStatusChangedAt == 0 ? 2000.0 : static_cast<double>(now - g_headerStatusChangedAt);
    const double transitionPulse = g_reduceMotion ? 0.0 : std::max(0.0, 1.0 - statusAge / 1300.0);
    const double scanPulse = !g_reduceMotion && health == HeaderHealth::Scanning ? 0.5 + 0.5 * std::sin(now / 180.0) : 0.0;
    const BYTE glowAlpha = static_cast<BYTE>(35.0 + 80.0 * std::max(transitionPulse, scanPulse));
    SolidBrush statusGlow(Color(glowAlpha, statusForeground.GetR(), statusForeground.GetG(), statusForeground.GetB()));
    graphics.FillEllipse(&statusGlow, RectF(statusPill.X + 11, statusPill.Y + 10, 16, 16));
    SolidBrush statusDot(statusForeground);
    graphics.FillEllipse(&statusDot, RectF(statusPill.X + 16, statusPill.Y + 15, 6, 6));
    text(graphics, statusText, RectF(statusPill.X + 34, statusPill.Y, statusPill.Width - 45, statusPill.Height),
         10, statusForeground, FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
    addHit(statusPill, Action::HeaderStatus);

    if (updateReady) {
        RectF updatePill(controls.X - 58, 16, 46, 36);
        const float updateHover = activeHoverProgress(Action::HeaderUpdate);
        fillRound(graphics, updatePill, 12, Color(255, 35, 38, 54));
        fillRound(graphics, updatePill, 12, accentColor(updateHover > 0 ? 210 : 72));
        strokeRound(graphics, updatePill, 12, updateHover > 0 ? accentTint(0.56) : accentColor(),
                    updateHover > 0 ? 1.5f : 1.0f);
        const wchar_t* badge = localized(L"MAJ", L"NEW", L"NEU", L"新");
        text(graphics, badge, updatePill, 9, Color(255, 232, 225, 255), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        addHit(updatePill, Action::HeaderUpdate);
    }

    RectF minimize(controls.X + 7, controls.Y + 5, 40, 34);
    RectF maximize(controls.X + 54, controls.Y + 5, 40, 34);
    RectF close(controls.X + 101, controls.Y + 5, 40, 34);
    const bool minimizeHovered = g_hoverAction == Action::Minimize;
    const bool maximizeHovered = g_hoverAction == Action::Maximize;
    const bool closeHovered = g_hoverAction == Action::Close;
    if (minimizeHovered) fillRound(graphics, minimize, 9, Color(static_cast<BYTE>(80 + 80 * activeHoverProgress(Action::Minimize)), 57, 63, 87));
    if (maximizeHovered) fillRound(graphics, maximize, 9, Color(static_cast<BYTE>(80 + 80 * activeHoverProgress(Action::Maximize)), 57, 63, 87));
    if (closeHovered) {
        const float closeHover = activeHoverProgress(Action::Close);
        fillRound(graphics, close, 9, Color(static_cast<BYTE>(145 + 110 * closeHover), 154, 45, 66));
        strokeRound(graphics, close, 9, Color(255, 225, 84, 105));
    }

    const Color normalGlyph(255, 197, 204, 222);
    const Color activeGlyph(255, 245, 242, 255);
    Pen minimizePen(minimizeHovered ? activeGlyph : normalGlyph, 1.8f);
    const float minimizeCenterX = minimize.X + minimize.Width / 2.0f;
    const float minimizeCenterY = minimize.Y + minimize.Height / 2.0f;
    graphics.DrawLine(&minimizePen, minimizeCenterX - 6.0f, minimizeCenterY + 4.0f,
                      minimizeCenterX + 6.0f, minimizeCenterY + 4.0f);

    Pen maximizePen(maximizeHovered ? activeGlyph : normalGlyph, 1.6f);
    const float maximizeCenterX = maximize.X + maximize.Width / 2.0f;
    const float maximizeCenterY = maximize.Y + maximize.Height / 2.0f;
    if (g_window && IsZoomed(g_window)) {
        graphics.DrawRectangle(&maximizePen, RectF(maximizeCenterX - 4.0f, maximizeCenterY - 6.0f, 11.0f, 10.0f));
        SolidBrush buttonBackground(maximizeHovered ? Color(255, 46, 51, 71) : Color(255, 22, 27, 40));
        graphics.FillRectangle(&buttonBackground, RectF(maximizeCenterX - 7.0f, maximizeCenterY - 2.0f, 11.0f, 10.0f));
        graphics.DrawRectangle(&maximizePen, RectF(maximizeCenterX - 7.0f, maximizeCenterY - 2.0f, 11.0f, 10.0f));
    } else {
        graphics.DrawRectangle(&maximizePen, RectF(maximizeCenterX - 6.0f, maximizeCenterY - 6.0f, 12.0f, 12.0f));
    }

    Pen closePen(closeHovered ? Color::White : normalGlyph, 1.9f);
    const float closeCenterX = close.X + close.Width / 2.0f;
    const float closeCenterY = close.Y + close.Height / 2.0f;
    graphics.DrawLine(&closePen, closeCenterX - 5.0f, closeCenterY - 5.0f,
                      closeCenterX + 5.0f, closeCenterY + 5.0f);
    graphics.DrawLine(&closePen, closeCenterX + 5.0f, closeCenterY - 5.0f,
                      closeCenterX - 5.0f, closeCenterY + 5.0f);
    addHit(minimize, Action::Minimize); addHit(maximize, Action::Maximize); addHit(close, Action::Close);
}

void drawHeaderTooltip(Graphics& graphics, int width) {
    if (g_hoverAction == Action::None || (g_hoverStartedAt != 0 && GetTickCount64() - g_hoverStartedAt < 360)) return;
    std::wstring label;
    float anchor = 0;
    float tooltipWidth = 0;
    const float controlsX = static_cast<float>(width - 164);
    switch (g_hoverAction) {
        case Action::HeaderStatus:
            label = localized(L"Ouvrir le diagnostic matériel", L"Open hardware diagnostics", L"Hardwarediagnose öffnen", L"打开硬件诊断");
            anchor = controlsX - (!g_downloadedUpdate.empty() ? 200.0f : 146.0f);
            tooltipWidth = 205;
            break;
        case Action::HeaderUpdate:
            label = localized(L"Mise à jour prête", L"Update ready", L"Update bereit", L"更新已就绪");
            anchor = controlsX - 35.0f;
            tooltipWidth = 132;
            break;
        case Action::Minimize:
            label = localized(L"Réduire", L"Minimize", L"Minimieren", L"最小化");
            anchor = controlsX + 27.0f;
            tooltipWidth = 82;
            break;
        case Action::Maximize:
            label = IsZoomed(g_window)
                ? localized(L"Restaurer", L"Restore", L"Wiederherstellen", L"还原")
                : localized(L"Agrandir", L"Maximize", L"Maximieren", L"最大化");
            anchor = controlsX + 74.0f;
            tooltipWidth = 96;
            break;
        case Action::Close:
            label = g_closeToTray
                ? localized(L"Masquer dans la zone", L"Hide in tray", L"In Infobereich ausblenden", L"隐藏到托盘")
                : localized(L"Fermer", L"Close", L"Schließen", L"关闭");
            anchor = controlsX + 121.0f;
            tooltipWidth = g_closeToTray ? 138.0f : 76.0f;
            break;
        default: return;
    }
    const float tooltipX = std::clamp(anchor - tooltipWidth / 2.0f, 8.0f, static_cast<float>(width) - tooltipWidth - 8.0f);
    RectF shadow(tooltipX + 2, 69, tooltipWidth, 30);
    fillRound(graphics, shadow, 8, Color(80, 0, 0, 0));
    RectF tooltip(tooltipX, 66, tooltipWidth, 30);
    fillRound(graphics, tooltip, 8, Color(255, 28, 33, 47));
    strokeRound(graphics, tooltip, 8, Color(255, 63, 71, 94));
    PointF pointer[] = {PointF(anchor - 5, 66), PointF(anchor + 5, 66), PointF(anchor, 61)};
    SolidBrush pointerBrush(Color(255, 28, 33, 47));
    graphics.FillPolygon(&pointerBrush, pointer, 3);
    text(graphics, label, tooltip, 10, Color(255, 229, 232, 241), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
}

void drawNavigationIcon(Graphics& graphics, int iconIndex, const RectF& bounds, const Color& color) {
    const float cx = bounds.X + bounds.Width / 2.0f;
    const float cy = bounds.Y + bounds.Height / 2.0f;
    Pen pen(color, 1.8f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(color);

    switch (iconIndex) {
        case 0: { // Tableau de bord
            const float size = 5.5f;
            fillRound(graphics, RectF(cx - 7, cy - 7, size, size), 1.8f, color);
            fillRound(graphics, RectF(cx + 1.5f, cy - 7, size, size), 1.8f, color);
            fillRound(graphics, RectF(cx - 7, cy + 1.5f, size, size), 1.8f, color);
            fillRound(graphics, RectF(cx + 1.5f, cy + 1.5f, size, size), 1.8f, color);
            break;
        }
        case 1: { // Appareils
            RectF screen(cx - 8, cy - 7, 16, 11);
            strokeRound(graphics, screen, 2.5f, color, 1.8f);
            graphics.DrawLine(&pen, cx, cy + 4, cx, cy + 7);
            graphics.DrawLine(&pen, cx - 4, cy + 7, cx + 4, cy + 7);
            break;
        }
        case 2: { // Manette
            GraphicsPath pad;
            pad.StartFigure();
            pad.AddBezier(PointF(cx - 3, cy - 5), PointF(cx - 8, cy - 5), PointF(cx - 10, cy - 1), PointF(cx - 9, cy + 5));
            pad.AddBezier(PointF(cx - 9, cy + 5), PointF(cx - 8, cy + 8), PointF(cx - 5, cy + 7), PointF(cx - 3, cy + 3));
            pad.AddLine(PointF(cx - 3, cy + 3), PointF(cx + 3, cy + 3));
            pad.AddBezier(PointF(cx + 3, cy + 3), PointF(cx + 5, cy + 7), PointF(cx + 8, cy + 8), PointF(cx + 9, cy + 5));
            pad.AddBezier(PointF(cx + 9, cy + 5), PointF(cx + 10, cy - 1), PointF(cx + 8, cy - 5), PointF(cx + 3, cy - 5));
            pad.CloseFigure();
            graphics.DrawPath(&pen, &pad);
            graphics.DrawLine(&pen, cx - 6.5f, cy - 1, cx - 2.5f, cy - 1);
            graphics.DrawLine(&pen, cx - 4.5f, cy - 3, cx - 4.5f, cy + 1);
            graphics.FillEllipse(&brush, cx + 3, cy - 3, 2.3f, 2.3f);
            graphics.FillEllipse(&brush, cx + 6, cy, 2.3f, 2.3f);
            break;
        }
        case 3: { // Effets lumineux
            graphics.DrawEllipse(&pen, cx - 4.5f, cy - 7.0f, 9.0f, 9.0f);
            graphics.DrawLine(&pen, cx - 3.5f, cy + 3, cx + 3.5f, cy + 3);
            graphics.DrawLine(&pen, cx - 2.5f, cy + 6, cx + 2.5f, cy + 6);
            graphics.DrawLine(&pen, cx, cy - 10, cx, cy - 8.5f);
            graphics.DrawLine(&pen, cx - 8.5f, cy - 5.5f, cx - 7, cy - 4.5f);
            graphics.DrawLine(&pen, cx + 8.5f, cy - 5.5f, cx + 7, cy - 4.5f);
            break;
        }
        case 4: { // Profils
            graphics.FillEllipse(&brush, cx - 3.2f, cy - 7, 6.4f, 6.4f);
            GraphicsPath shoulders;
            shoulders.AddArc(cx - 8.0f, cy - 1.0f, 16.0f, 13.0f, 200.0f, 140.0f);
            graphics.DrawPath(&pen, &shoulders);
            break;
        }
        case 5: { // Ventilation
            graphics.DrawEllipse(&pen, cx - 8.5f, cy - 8.5f, 17.0f, 17.0f);
            for (int blade = 0; blade < 3; ++blade) {
                GraphicsState state = graphics.Save();
                graphics.TranslateTransform(cx, cy);
                graphics.RotateTransform(static_cast<REAL>(blade * 120));
                GraphicsPath shape;
                shape.StartFigure();
                shape.AddBezier(PointF(-1.1f, -2), PointF(-2.4f, -7.8f), PointF(1.7f, -9.1f), PointF(4.8f, -6.7f));
                shape.AddBezier(PointF(4.8f, -6.7f), PointF(6.4f, -4.1f), PointF(3.3f, -1.0f), PointF(1.2f, 0.2f));
                shape.CloseFigure();
                graphics.FillPath(&brush, &shape);
                graphics.Restore(state);
            }
            SolidBrush hub(Color(color.GetA(), color.GetR(), color.GetG(), color.GetB()));
            graphics.FillEllipse(&hub, cx - 2.2f, cy - 2.2f, 4.4f, 4.4f);
            break;
        }
        case 6: { // Diagnostic
            PointF pulse[] = {
                PointF(cx - 9, cy + 1), PointF(cx - 5, cy + 1), PointF(cx - 2, cy - 5),
                PointF(cx + 1, cy + 6), PointF(cx + 4, cy - 2), PointF(cx + 6, cy + 1), PointF(cx + 9, cy + 1)
            };
            graphics.DrawLines(&pen, pulse, static_cast<INT>(std::size(pulse)));
            break;
        }
        default: { // Paramètres
            graphics.DrawEllipse(&pen, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
            graphics.DrawEllipse(&pen, cx - 2.2f, cy - 2.2f, 4.4f, 4.4f);
            constexpr float pi = 3.14159265358979323846f;
            for (int tooth = 0; tooth < 8; ++tooth) {
                const float angle = tooth * pi / 4.0f;
                graphics.DrawLine(&pen,
                    cx + std::cos(angle) * 6.5f, cy + std::sin(angle) * 6.5f,
                    cx + std::cos(angle) * 9.0f, cy + std::sin(angle) * 9.0f);
            }
            break;
        }
    }
}

void drawNavigation(Graphics& graphics, int height) {
    LinearGradientBrush sidebar(PointF(0, static_cast<REAL>(kHeaderHeight)), PointF(static_cast<REAL>(kSidebarWidth), static_cast<REAL>(height)),
                                Color(248, 11, 14, 23), Color(244, 14, 18, 28));
    graphics.FillRectangle(&sidebar, 0, kHeaderHeight, kSidebarWidth, height - kHeaderHeight);
    Pen line(Color(188, 33, 39, 55));
    graphics.DrawLine(&line, kSidebarWidth - 1, kHeaderHeight, kSidebarWidth - 1, height);
    text(graphics, localized(L"NAVIGATION", L"NAVIGATION", L"NAVIGATION", L"导航"), RectF(20, 91, 150, 18), 9,
         Color(255, 101, 112, 136), FontStyleBold);
    struct Nav { const wchar_t* label; Page page; Action action; } navs[] = {
        {localized(L"Tableau de bord", L"Dashboard", L"Übersicht", L"控制面板"), Page::Dashboard, Action::NavDashboard},
        {localized(L"Appareils", L"Devices", L"Geräte", L"设备"), Page::Devices, Action::NavDevices},
        {localized(L"Manette", L"Controller", L"Controller", L"手柄"), Page::Gamepads, Action::NavGamepads},
        {localized(L"Effets", L"Effects", L"Effekte", L"灯光效果"), Page::Effects, Action::NavEffects},
        {localized(L"Profils", L"Profiles", L"Profile", L"模式"), Page::Profiles, Action::NavProfiles},
        {localized(L"Ventilation", L"Cooling", L"Lüfter", L"风扇控制"), Page::Fans, Action::NavFans},
        {localized(L"Diagnostic", L"Diagnostics", L"Diagnose", L"诊断"), Page::Diagnostics, Action::NavDiagnostics},
        {localized(L"Paramètres", L"Settings", L"Einstellungen", L"设置"), Page::Settings, Action::NavSettings}
    };
    float y = 118;
    int navIndex = 0;
    for (const Nav& nav : navs) {
        RectF rect(12, y, static_cast<float>(kSidebarWidth - 24), 44);
        const bool active = g_page == nav.page ||
                            (nav.page == Page::Devices && (g_page == Page::DuckyAssistant || g_page == Page::Compatibility));
        const bool hovered = g_hoverAction == nav.action;
        if (active) {
            GraphicsPath activePath;
            roundedPath(activePath, rect, 12);
            LinearGradientBrush activeBrush(PointF(rect.X, rect.Y), PointF(rect.GetRight(), rect.GetBottom()),
                                            accentColor(52), accentColor(22));
            graphics.FillPath(&activeBrush, &activePath);
            strokeRound(graphics, rect, 12, accentColor(98), 1.0f);
            fillRound(graphics, RectF(rect.X + 3, rect.Y + 11, 3, rect.Height - 22), 2, accentTint(0.44));
        } else if (hovered) {
            fillRound(graphics, rect, 12, Color(178, 28, 34, 48));
        }
        RectF icon(rect.X + 12, rect.Y + 8, 28, 28);
        fillRound(graphics, icon, 9, active ? accentColor(66) : Color(255, 25, 30, 43));
        strokeRound(graphics, icon, 9, active ? accentColor(112) : Color(255, 42, 49, 67));
        drawNavigationIcon(graphics, navIndex, icon,
                           active ? accentTint(0.62) : Color(255, 135, 146, 170));
        text(graphics, nav.label, RectF(rect.X + 51, rect.Y, rect.Width - 59, rect.Height), 12,
             active ? Color(255, 242, 239, 252) : Color(255, 163, 172, 193),
             active ? FontStyleBold : FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
        addHit(rect, nav.action);
        y += 52;
        ++navIndex;
    }
    RectF donate(16, static_cast<float>(height - 126), static_cast<float>(kSidebarWidth - 32), 40);
    fillRound(graphics, donate, 11, Color(255, 31, 24, 36));
    strokeRound(graphics, donate, 11, Color(255, 78, 48, 70));
    text(graphics, localized(L"Soutenir le projet", L"Support the project", L"Projekt unterstützen", L"支持项目"), donate,
         11, Color(255, 237, 194, 215), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    addHit(donate, Action::Donate);
    RectF safe(16, static_cast<float>(height - 74), static_cast<float>(kSidebarWidth - 32), 52);
    fillRound(graphics, safe, 12, Color(255, 14, 34, 30));
    strokeRound(graphics, safe, 12, Color(255, 27, 69, 58));
    SolidBrush dot(Color(255, 85, 217, 162));
    graphics.FillEllipse(&dot, RectF(safe.X + 13.0f, safe.Y + 13.0f, 7.0f, 7.0f));
    text(graphics, localized(L"CONTRÔLE LOCAL", L"LOCAL CONTROL", L"LOKALE STEUERUNG", L"本地控制"),
         RectF(safe.X + 28, safe.Y + 7, safe.Width - 39, 17), 8, Color(255, 112, 224, 179), FontStyleBold);
    text(graphics, localized(L"Aucune donnée en ligne", L"No cloud data", L"Keine Cloud-Daten", L"无云端数据"),
         RectF(safe.X + 13, safe.Y + 29, safe.Width - 26, 14), 9, Color(255, 133, 169, 158));
}

void drawPageIntro(Graphics& graphics, float x, float y, float titleWidth,
                   const std::wstring& eyebrow, const std::wstring& title) {
    fillRound(graphics, RectF(x, y + 2.0f, 3.0f, 14.0f), 2.0f, accentColor());
    text(graphics, eyebrow, RectF(x + 13.0f, y, std::max(0.0f, titleWidth - 13.0f), 18.0f),
         9, accentTint(0.42), FontStyleBold);
    text(graphics, title, RectF(x, y + 22.0f, titleWidth, 38.0f), 27,
         Color(255, 244, 246, 251), FontStyleBold);
}

void drawDashboard(Graphics& graphics, int width, int height, float originY) {
    float x = kSidebarWidth + 32.0f;
    float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, 580,
                  localized(L"ACCUEIL", L"HOME", L"START", L"主页"),
                  localized(L"Ton setup, simplement.", L"Your setup, made simple.", L"Dein Setup, ganz einfach.", L"轻松掌控整套设备。"));
    drawButton(graphics, RectF(width - 164.0f, originY + 10, 126, 40), localized(L"Actualiser", L"Refresh", L"Aktualisieren", L"刷新"), false, Action::Refresh);
    float y = originY + 86;
    int totalDevices = static_cast<int>(g_rgbDevices.size()) + (g_duckyDetected ? 1 : 0);
    text(graphics, std::wstring(localized(L"Appareils détectés  ", L"Devices found  ", L"Erkannte Geräte  ", L"已检测设备  ")) + std::to_wstring(totalDevices), RectF(x, y, 360, 22), 13, Color(255, 220, 224, 234), FontStyleBold);
    text(graphics, g_status, RectF(width - 480.0f, y, 440, 22), 10, Color(255, 117, 205, 180), FontStyleRegular, StringAlignmentFar);
    y += 34;
    const float cardWidth = std::max(220.0f, (available - 24.0f) / 3.0f);
    int column = 0;
    for (int index = 0; index < static_cast<int>(g_rgbDevices.size()); ++index) {
        const RgbDevice& device = g_rgbDevices[index];
        const std::wstring deviceType = localizedDeviceType(device.type);
        float cardX = x + column * (cardWidth + 12);
        RectF card(cardX, y, cardWidth, 106);
        fillRound(graphics, card, 17, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 17, device.selected ? accentColor() : Color(255, 42, 48, 64));
        fillRound(graphics, RectF(card.X + 16, card.Y + 17, 42, 42), 13, Color(255, 39, 61, 75));
        text(graphics, deviceType.substr(0, std::min<std::size_t>(2, deviceType.size())), RectF(card.X + 16, card.Y + 17, 42, 42), 11,
             Color(255, 105, 221, 199), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, device.name, RectF(card.X + 70, card.Y + 16, card.Width - 86, 20), 13, Color(255, 237, 239, 246), FontStyleBold);
        text(graphics, deviceType + L" · " + std::to_wstring(device.zones) + localized(L" zone(s)", L" zone(s)", L" Zone(n)", L" 个区域"), RectF(card.X + 70, card.Y + 40, card.Width - 86, 18), 10, Color(255, 135, 144, 165));
        RectF selector(card.X + 16, card.Y + 70, card.Width - 32, 24);
        fillRound(graphics, selector, 8, device.selected ? Color(255, 39, 42, 59) : Color(255, 34, 38, 52));
        if (device.selected) fillRound(graphics, selector, 8, accentColor(34));
        text(graphics, device.selected
                 ? localized(L"Inclus · cliquer pour exclure", L"Included · click to exclude", L"Aktiv · zum Ausschließen klicken", L"已包含 · 点击排除")
                 : localized(L"Exclu · cliquer pour inclure", L"Excluded · click to include", L"Aus · zum Einschließen klicken", L"已排除 · 点击包含"), selector, 10,
             device.selected ? Color(255, 209, 198, 255) : Color(255, 135, 144, 165), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        addHit(card, Action::ToggleRgb, index);
        if (++column == 3) { column = 0; y += 118; }
    }
    if (g_duckyDetected) {
        float cardX = x + column * (cardWidth + 12);
        RectF card(cardX, y, cardWidth, 106);
        fillRound(graphics, card, 17, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 17, Color(255, 42, 48, 64));
        text(graphics, L"Ducky One 2 Mini", RectF(card.X + 18, card.Y + 17, card.Width - 36, 20), 13, Color(255, 237, 239, 246), FontStyleBold);
        text(graphics, std::wstring(L"DKON1861ST · ") + localized(L"firmware officiel", L"official firmware", L"offizielle Firmware", L"官方固件"), RectF(card.X + 18, card.Y + 42, card.Width - 36, 18), 10, Color(255, 135, 144, 165));
        fillRound(graphics, RectF(card.X + 16, card.Y + 70, card.Width - 32, 24), 8, Color(255, 61, 48, 27));
        text(graphics, localized(L"Ouvrir l'assistant sécurisé", L"Open safe assistant", L"Sicheren Assistenten öffnen", L"打开安全助手"), RectF(card.X + 16, card.Y + 70, card.Width - 32, 24), 10, Color(255, 240, 188, 99), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        addHit(card, Action::DeviceOpenDuckyAssistant);
        if (++column == 3) { column = 0; y += 118; }
    }
    if (column != 0 || (g_rgbDevices.empty() && !g_duckyDetected)) y += 118;
    RectF colorCard(x, y + 8, available * 0.63f, 324);
    fillRound(graphics, colorCard, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, colorCard, 18, Color(255, 39, 45, 61));
    text(graphics, localized(L"Couleur principale", L"Main color", L"Hauptfarbe", L"主颜色"), RectF(colorCard.X + 22, colorCard.Y + 20, 260, 24), 17, Color::White, FontStyleBold);
    text(graphics, localized(L"Appareils OpenRGB marqués Inclus", L"OpenRGB devices marked Included", L"Als aktiv markierte OpenRGB-Geräte", L"已包含的 OpenRGB 设备"), RectF(colorCard.X + 22, colorCard.Y + 47, colorCard.Width - 44, 18), 10, Color(255, 142, 150, 171));
    RectF preview(colorCard.X + 22, colorCard.Y + 82, 78, 78);
    fillRound(graphics, preview, 22, rgbColor(g_baseColor));
    addHit(preview, Action::PickColor);
    text(graphics, localized(L"Valeur hexadécimale", L"Hex value", L"Hexadezimalwert", L"十六进制值"), RectF(colorCard.X + 120, colorCard.Y + 88, 230, 18), 10, Color(255, 142, 150, 171));
    fillRound(graphics, RectF(colorCard.X + 120, colorCard.Y + 112, colorCard.Width - 142, 42), 9, Color(255, 17, 20, 29));
    text(graphics, hexColor(g_baseColor), RectF(colorCard.X + 133, colorCard.Y + 112, colorCard.Width - 155, 42), 14, Color::White, FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
    text(graphics, localized(L"Luminosité", L"Brightness", L"Helligkeit", L"亮度"), RectF(colorCard.X + 22, colorCard.Y + 174, 150, 20), 12, Color(255, 220, 224, 234));
    text(graphics, std::to_wstring(g_brightness) + L" %", RectF(colorCard.GetRight() - 100, colorCard.Y + 174, 78, 20), 12, Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
    drawSlider(graphics, RectF(colorCard.X + 22, colorCard.Y + 198, colorCard.Width - 44, 30), g_brightness, 0, Action::Brightness);
    const std::uint32_t swatches[] = {0x7C5CFF, 0x149CFF, 0x00D69E, 0xFFB83D, 0xFF4F70, 0xFFFFFF};
    const float applyWidth = std::clamp(colorCard.Width * 0.40f, 176.0f, 208.0f);
    const RectF applyRect(colorCard.GetRight() - applyWidth - 22, colorCard.Y + 252, applyWidth, 44);
    const float paletteWidth = applyRect.X - (colorCard.X + 22) - 14;
    const float swatchSize = 30.0f;
    const float swatchGap = std::clamp((paletteWidth - swatchSize * static_cast<float>(std::size(swatches))) /
                                           static_cast<float>(std::size(swatches) - 1),
                                       6.0f, 12.0f);
    float swatchX = colorCard.X + 22;
    for (std::uint32_t swatch : swatches) {
        RectF swatchRect(swatchX, colorCard.Y + 259, swatchSize, swatchSize);
        fillRound(graphics, swatchRect, 9, rgbColor(swatch));
        strokeRound(graphics, swatchRect, 9, swatch == g_baseColor ? Color::White : Color(255, 83, 96, 120), swatch == g_baseColor ? 2.0f : 1.0f);
        addHit(swatchRect, Action::SetColor, -1, swatch);
        swatchX += swatchSize + swatchGap;
    }
    drawButton(graphics, applyRect, localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true, Action::ApplyColor);

    RectF activity(colorCard.GetRight() + 18, colorCard.Y, available - colorCard.Width - 18, colorCard.Height);
    fillRound(graphics, activity, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, activity, 18, Color(255, 39, 45, 61));
    text(graphics, localized(L"État du moteur", L"Engine status", L"Engine-Status", L"引擎状态"), RectF(activity.X + 20, activity.Y + 20, activity.Width - 40, 24), 16, Color::White, FontStyleBold);
    const bool rgbEngineReady = g_openRgbReady || hasActivePluginDevice();
    fillRound(graphics, RectF(activity.X + 20, activity.Y + 61, activity.Width - 40, 58), 13, rgbEngineReady ? Color(255, 16, 52, 43) : Color(255, 56, 42, 25));
    text(graphics, rgbEngineReady ? localized(L"MOTEUR RGB ACTIF", L"RGB ENGINE ACTIVE", L"RGB-ENGINE AKTIV", L"RGB 引擎已启用")
                                  : localized(L"MOTEUR RGB EN ATTENTE", L"RGB ENGINE WAITING", L"RGB-ENGINE WARTET", L"RGB 引擎等待中"),
         RectF(activity.X + 34, activity.Y + 61, activity.Width - 68, 58), 11,
         rgbEngineReady ? Color(255, 103, 225, 178) : Color(255, 255, 190, 120), FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
    text(graphics, localized(L"Application native C++", L"Native C++ application", L"Native C++-Anwendung", L"原生 C++ 应用"), RectF(activity.X + 20, activity.Y + 142, activity.Width - 40, 20), 10, Color(255, 128, 137, 158));
    textWrapped(graphics, g_status, RectF(activity.X + 20, activity.Y + 170, activity.Width - 40, 92), 11, Color(255, 213, 217, 228));
    g_maxScroll = std::max(0.0f, colorCard.GetBottom() + g_scrollOffset + 16 - height);
}

std::vector<DeviceProviderDescriptor> currentProviderCatalog() {
    const bool hardwareBridgeAvailable = fs::exists(bridgeExecutable());
    const int pluginReady = static_cast<int>(std::count_if(g_pluginProviders.begin(), g_pluginProviders.end(),
        [](const PluginProviderStatus& provider) { return provider.valid; }));
    int pluginDevices = 0;
    for (const PluginProviderStatus& provider : g_pluginProviders) pluginDevices += provider.matchedDevices;
    return {
        {L"openrgb", L"OpenRGB", L"SDK local TCP",
         DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::Effects |
             DeviceCapability::Zones | DeviceCapability::PerLed,
         g_openRgbReady, g_hasCompletedScan && !g_openRgbReady},
        {L"hardware", localized(L"Capteurs matériels", L"Hardware sensors", L"Hardware-Sensoren", L"硬件传感器"),
         L"LibreHardwareMonitor",
         DeviceCapability::Discovery | DeviceCapability::Sensors | DeviceCapability::FanRead | DeviceCapability::FanControl,
         hardwareBridgeAvailable, hardwareBridgeAvailable && g_hasCompletedScan && g_fans.empty()},
        {L"hotplug", L"Windows Hot-plug", L"WM_DEVICECHANGE",
         DeviceCapability::Discovery | DeviceCapability::HotPlug,
         g_hotplugMonitoring, !g_hotplugMonitoring},
        {L"plugins", localized(L"Plugins RGBCcontrol", L"RGBCcontrol plugins", L"RGBCcontrol-Plugins", L"RGBCcontrol 插件"),
         std::to_wstring(pluginReady) + localized(L" pilote(s) · ", L" driver(s) · ", L" Treiber · ", L" 个驱动 · ") +
             std::to_wstring(pluginDevices) + localized(L" appareil(s)", L" device(s)", L" Gerät(e)", L" 个设备"),
         DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::Effects |
             DeviceCapability::Zones | DeviceCapability::PerLed | DeviceCapability::LocalOnly,
         true, g_rejectedPlugins > 0}
    };
}

void drawDevices(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    const int rgbCount = static_cast<int>(g_rgbDevices.size()) + (g_duckyDetected ? 1 : 0);
    const int totalCount = rgbCount + static_cast<int>(g_fans.size());
    drawPageIntro(graphics, x, originY, available - 350,
                  localized(L"APPAREILS", L"DEVICES", L"GERÄTE", L"设备"),
                  localized(L"Tous tes appareils. Un seul endroit.", L"Every device. One place.",
                            L"Alle Geräte. Ein Ort.", L"所有设备，尽在一处。"));
    drawButton(graphics, RectF(width - 376.0f, originY + 10, 178, 42),
               localized(L"Tester la compatibilité", L"Test compatibility", L"Kompatibilität testen", L"测试兼容性"),
               true, Action::DeviceOpenCompatibility);
    drawButton(graphics, RectF(width - 188.0f, originY + 10, 150, 42),
               localized(L"Nouvelle analyse", L"Scan again", L"Neu suchen", L"重新扫描"), false, Action::Refresh);
    text(graphics, std::to_wstring(totalCount) + localized(L" éléments inventoriés", L" items inventoried", L" Elemente erfasst", L" 个项目已清点"),
         RectF(x, originY + 62, 290, 20), 11, Color(255, 132, 142, 165));
    const std::wstring detectionState = !g_hotplugStatus.empty() ? g_hotplugStatus : g_hotplugMonitoring
             ? localized(L"Détection instantanée active", L"Instant detection active", L"Soforterkennung aktiv", L"即时检测已启用")
             : localized(L"Détection périodique active", L"Periodic detection active", L"Periodische Erkennung aktiv", L"定期检测已启用");
    text(graphics, detectionState,
         RectF(width - 330.0f, originY + 62, 292, 20), 10,
         g_hotplugMonitoring ? Color(255, 105, 221, 178) : Color(255, 241, 183, 104), FontStyleBold, StringAlignmentFar);

    float y = originY + 96;
    const std::vector<DeviceProviderDescriptor> providers = currentProviderCatalog();
    const float providerGap = 10.0f;
    const int providerColumns = width < 1120 ? 2 : 4;
    const float providerWidth = (available - providerGap * (providerColumns - 1)) / providerColumns;
    for (int index = 0; index < static_cast<int>(providers.size()); ++index) {
        const DeviceProviderDescriptor& provider = providers[index];
        const int row = index / providerColumns;
        const int column = index % providerColumns;
        RectF card(x + column * (providerWidth + providerGap), y + row * 118, providerWidth, 108);
        const bool providerReady = provider.available && !provider.limited;
        fillRound(graphics, card, 16, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 16, providerReady ? Color(255, 44, 91, 76) : Color(255, 91, 62, 50));
        fillRound(graphics, RectF(card.X + 14, card.Y + 14, 38, 38), 12,
                  providerReady ? Color(255, 23, 61, 51) : Color(255, 58, 42, 32));
        const wchar_t* providerCode = provider.id == L"openrgb" ? L"OR" : provider.id == L"hardware" ? L"HW" :
                                      provider.id == L"hotplug" ? L"HP" : L"PL";
        text(graphics, providerCode, RectF(card.X + 14, card.Y + 14, 38, 38), 9,
             providerReady ? Color(255, 102, 226, 181) : Color(255, 242, 181, 105),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, provider.name, RectF(card.X + 62, card.Y + 13, card.Width - 76, 20), 12, Color::White, FontStyleBold);
        text(graphics, provider.transport, RectF(card.X + 62, card.Y + 35, card.Width - 76, 17), 8, Color(255, 126, 136, 158));
        const wchar_t* capabilityText = provider.id == L"openrgb"
            ? localized(L"RGB · Effets · Zones", L"RGB · Effects · Zones", L"RGB · Effekte · Zonen", L"RGB · 灯效 · 分区")
            : provider.id == L"hardware"
                ? localized(L"Capteurs · Ventilateurs", L"Sensors · Fans", L"Sensoren · Lüfter", L"传感器 · 风扇")
                : provider.id == L"hotplug"
                    ? localized(L"Branchement · Retrait", L"Connect · Disconnect", L"Anschluss · Entfernen", L"连接 · 移除")
                    : localized(L"Pilotes · Marques", L"Drivers · Brands", L"Treiber · Marken", L"驱动 · 品牌");
        text(graphics, capabilityText, RectF(card.X + 14, card.Y + 62, card.Width - 28, 16), 8, Color(255, 150, 159, 179));
        RectF status(card.X + 14, card.Y + 82, card.Width - 28, 18);
        fillRound(graphics, status, 7, providerReady ? Color(255, 17, 52, 43) : Color(255, 58, 43, 27));
        text(graphics,
             providerReady ? localized(L"PRÊT", L"READY", L"BEREIT", L"就绪")
                           : localized(L"À VÉRIFIER", L"CHECK", L"PRÜFEN", L"需检查"),
             status, 8, providerReady ? Color(255, 104, 225, 179) : Color(255, 243, 188, 102),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        addHit(card, Action::DeviceOpenDiagnostics);
    }

    y += static_cast<float>(((providers.size() + providerColumns - 1) / providerColumns) * 118 + 16);
    text(graphics, localized(L"Éclairage et périphériques", L"Lighting and peripherals", L"Beleuchtung und Peripherie", L"灯光和外设"),
         RectF(x, y, 360, 23), 15, Color::White, FontStyleBold);
    text(graphics, std::to_wstring(rgbCount), RectF(width - 92.0f, y, 54, 23), 11, accentTint(0.38), FontStyleBold, StringAlignmentFar);
    y += 34;
    const float itemGap = 12.0f;
    const float itemWidth = (available - itemGap) / 2.0f;
    int column = 0;
    auto nextItemCard = [&]() {
        RectF card(x + column * (itemWidth + itemGap), y, itemWidth, 132);
        if (++column == 2) { column = 0; y += 144; }
        return card;
    };
    for (int index = 0; index < static_cast<int>(g_rgbDevices.size()); ++index) {
        const RgbDevice& device = g_rgbDevices[index];
        RectF card = nextItemCard();
        fillRound(graphics, card, 16, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 16, device.selected ? accentColor() : Color(255, 43, 49, 65), device.selected ? 1.5f : 1.0f);
        fillRound(graphics, RectF(card.X + 15, card.Y + 15, 44, 44), 13, Color(255, 34, 54, 68));
        const std::wstring type = localizedDeviceType(device.type);
        text(graphics, type.substr(0, std::min<std::size_t>(2, type.size())), RectF(card.X + 15, card.Y + 15, 44, 44), 10,
             Color(255, 99, 219, 199), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, device.name, RectF(card.X + 72, card.Y + 14, card.Width - 87, 20), 13, Color::White, FontStyleBold);
        const std::wstring providerLabel = PluginEngine::isPluginDevice(device) ? L" · Plugin RGBCcontrol" : L" · OpenRGB";
        text(graphics, (device.vendor.empty() ? L"Inconnu" : device.vendor) + providerLabel,
             RectF(card.X + 72, card.Y + 36, card.Width - 87, 17), 9, Color(255, 132, 142, 164));
        const std::wstring capabilities = L"RGB  ·  " + std::to_wstring(device.zones) + localized(L" zone(s)  ·  ", L" zone(s)  ·  ", L" Zone(n)  ·  ", L" 个分区  ·  ") +
                                          std::to_wstring(device.leds) + L" LED";
        text(graphics, capabilities, RectF(card.X + 15, card.Y + 68, card.Width - 30, 18), 9, Color(255, 159, 168, 187));
        RectF selector(card.X + 15, card.Y + 94, card.Width - 30, 25);
        fillRound(graphics, selector, 8, device.selected ? Color(255, 37, 42, 58) : Color(255, 21, 25, 36));
        if (device.selected) fillRound(graphics, selector, 8, accentColor(42));
        text(graphics, device.selected
                 ? localized(L"Inclus dans la synchronisation", L"Included in synchronization", L"In Synchronisierung enthalten", L"已包含在同步中")
                 : localized(L"Exclu · cliquer pour inclure", L"Excluded · click to include", L"Ausgeschlossen · zum Aktivieren klicken", L"已排除 · 点击包含"),
             selector, 9, device.selected ? accentTint(0.56) : Color(255, 137, 146, 166), FontStyleBold,
             StringAlignmentCenter, StringAlignmentCenter);
        addHit(card, Action::DeviceOpenEffects);
        addHit(selector, Action::ToggleRgb, index);
    }
    if (g_duckyDetected) {
        RectF card = nextItemCard();
        fillRound(graphics, card, 16, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 16, Color(255, 104, 75, 31));
        fillRound(graphics, RectF(card.X + 15, card.Y + 15, 44, 44), 13, Color(255, 60, 47, 27));
        text(graphics, L"DK", RectF(card.X + 15, card.Y + 15, 44, 44), 10, Color(255, 242, 190, 102),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, L"Ducky One 2 Mini", RectF(card.X + 72, card.Y + 14, card.Width - 87, 20), 13, Color::White, FontStyleBold);
        text(graphics, L"DKON1861ST · firmware officiel", RectF(card.X + 72, card.Y + 36, card.Width - 87, 17), 9, Color(255, 132, 142, 164));
        text(graphics, localized(L"Détecté · RGB local sécurisé", L"Detected · safe local RGB", L"Erkannt · sicheres lokales RGB", L"已检测 · 安全本地 RGB"),
             RectF(card.X + 15, card.Y + 68, card.Width - 30, 18), 9, Color(255, 235, 185, 99));
        RectF safe(card.X + 15, card.Y + 94, card.Width - 30, 25);
        fillRound(graphics, safe, 8, Color(255, 55, 45, 27));
        text(graphics, localized(L"Ouvrir l'assistant Ducky sécurisé", L"Open the safe Ducky assistant", L"Sicheren Ducky-Assistenten öffnen", L"打开安全 Ducky 助手"),
             safe, 9, Color(255, 241, 192, 107), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        addHit(card, Action::DeviceOpenDuckyAssistant);
    }
    if (column != 0) y += 144;
    if (rgbCount == 0) {
        RectF empty(x, y, available, 76);
        fillRound(graphics, empty, 15, Color(255, 25, 29, 41));
        strokeRound(graphics, empty, 15, Color(255, 50, 56, 72));
        text(graphics, localized(L"Aucun contrôleur RGB détecté pour le moment.", L"No RGB controller detected yet.",
                                 L"Noch kein RGB-Controller erkannt.", L"尚未检测到 RGB 控制器。"),
             empty, 11, Color(255, 151, 160, 180), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
        y += 88;
    }

    text(graphics, localized(L"Ventilation et capteurs", L"Cooling and sensors", L"Kühlung und Sensoren", L"散热和传感器"),
         RectF(x, y, 360, 23), 15, Color::White, FontStyleBold);
    drawButton(graphics, RectF(width - 190.0f, y - 7, 152, 36),
               localized(L"Ouvrir ventilation", L"Open cooling", L"Kühlung öffnen", L"打开散热控制"), false, Action::DeviceOpenFans);
    y += 35;
    column = 0;
    for (const FanDevice& fan : g_fans) {
        RectF card(x + column * (itemWidth + itemGap), y, itemWidth, 98);
        fillRound(graphics, card, 15, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 15, fan.controllable ? Color(255, 49, 91, 78) : Color(255, 49, 55, 72));
        text(graphics, fan.name, RectF(card.X + 16, card.Y + 13, card.Width - 32, 20), 12, Color::White, FontStyleBold);
        text(graphics, fan.hardware, RectF(card.X + 16, card.Y + 35, card.Width - 32, 17), 8, Color(255, 126, 136, 158));
        const std::wstring fanReading = fan.rpm >= 0 ? std::to_wstring(static_cast<int>(std::lround(fan.rpm))) + L" RPM" : L"RPM --";
        text(graphics, fanReading, RectF(card.X + 16, card.Y + 63, 130, 19), 10, Color(255, 204, 211, 225), FontStyleBold);
        RectF capability(card.GetRight() - 132, card.Y + 61, 116, 23);
        fillRound(graphics, capability, 8, fan.controllable ? Color(255, 17, 52, 43) : Color(255, 38, 42, 55));
        text(graphics, fan.controllable ? localized(L"CONTRÔLABLE", L"CONTROLLABLE", L"STEUERBAR", L"可控制")
                                        : localized(L"LECTURE SEULE", L"READ ONLY", L"NUR LESEN", L"只读"),
             capability, 8, fan.controllable ? Color(255, 104, 225, 179) : Color(255, 151, 160, 180),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        addHit(card, Action::DeviceOpenFans);
        if (++column == 2) { column = 0; y += 110; }
    }
    if (column != 0) y += 110;
    if (g_fans.empty()) {
        RectF empty(x, y, available, 76);
        fillRound(graphics, empty, 15, Color(255, 25, 29, 41));
        strokeRound(graphics, empty, 15, Color(255, 50, 56, 72));
        text(graphics, localized(L"Aucun canal de ventilation exposé par Windows.", L"No fan channel is exposed by Windows.",
                                 L"Windows stellt keinen Lüfterkanal bereit.", L"Windows 未公开风扇通道。"),
             empty, 11, Color(255, 151, 160, 180), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
        y += 88;
    }
    g_maxScroll = std::max(0.0f, y + g_scrollOffset + 18 - height);
}

struct DualSenseRenderPoint {
    float x = 0;
    float y = 0;
    float z = 0;
};

DualSenseRenderPoint rotateDualSensePoint(const DualSenseMeshVertex& vertex) {
    const float cy = std::cos(g_gamepadYaw);
    const float sy = std::sin(g_gamepadYaw);
    const float cp = std::cos(g_gamepadPitch);
    const float sp = std::sin(g_gamepadPitch);
    const float x = vertex.x - g_dualSenseMesh.centerX;
    const float y = vertex.y - g_dualSenseMesh.centerY;
    const float z = vertex.z - g_dualSenseMesh.centerZ;
    const float yawX = cy * x + sy * z;
    const float yawZ = -sy * x + cy * z;
    return {yawX, cp * y - sp * yawZ, sp * y + cp * yawZ};
}

DualSenseRenderPoint rotateDualSenseDirection(float x, float y, float z) {
    const float cy = std::cos(g_gamepadYaw);
    const float sy = std::sin(g_gamepadYaw);
    const float cp = std::cos(g_gamepadPitch);
    const float sp = std::sin(g_gamepadPitch);
    const float yawX = cy * x + sy * z;
    const float yawZ = -sy * x + cy * z;
    return {yawX, cp * y - sp * yawZ, sp * y + cp * yawZ};
}

std::pair<float, float> normalizedDualSenseStick(std::uint8_t rawX, std::uint8_t rawY) {
    float x = (static_cast<float>(rawX) - 127.5f) / 127.5f;
    float y = (static_cast<float>(rawY) - 127.5f) / 127.5f;
    const float magnitude = std::sqrt(x * x + y * y);
    constexpr float kDeadZone = 0.075f;
    if (magnitude <= kDeadZone) return {0.0f, 0.0f};
    const float rescaled = std::clamp((magnitude - kDeadZone) / (1.0f - kDeadZone), 0.0f, 1.0f);
    const float factor = rescaled / magnitude;
    return {x * factor, y * factor};
}

bool updateDualSenseVisualAxes(ULONGLONG now, bool snap) {
    auto left = normalizedDualSenseStick(g_dualSenseLive.leftX, g_dualSenseLive.leftY);
    auto right = normalizedDualSenseStick(g_dualSenseLive.rightX, g_dualSenseLive.rightY);
    if (!g_dualSenseLive.seen || now - g_dualSenseLive.lastInputAt >= 2500) {
        left = {0.0f, 0.0f};
        right = {0.0f, 0.0f};
    }

    if (!g_dualSenseVisualAxes.initialized || snap || g_reduceMotion) {
        const bool changed = !g_dualSenseVisualAxes.initialized ||
            std::abs(g_dualSenseVisualAxes.leftX - left.first) > 0.0005f ||
            std::abs(g_dualSenseVisualAxes.leftY - left.second) > 0.0005f ||
            std::abs(g_dualSenseVisualAxes.rightX - right.first) > 0.0005f ||
            std::abs(g_dualSenseVisualAxes.rightY - right.second) > 0.0005f;
        g_dualSenseVisualAxes.leftX = left.first;
        g_dualSenseVisualAxes.leftY = left.second;
        g_dualSenseVisualAxes.rightX = right.first;
        g_dualSenseVisualAxes.rightY = right.second;
        g_dualSenseVisualAxes.updatedAt = now;
        g_dualSenseVisualAxes.initialized = true;
        return changed;
    }

    const float elapsedMs = static_cast<float>(std::clamp<ULONGLONG>(now - g_dualSenseVisualAxes.updatedAt, 1, 50));
    g_dualSenseVisualAxes.updatedAt = now;
    // A 24 ms critically damped response removes HID jitter while keeping the
    // visual knob practically latency-free at the 60 Hz input cadence. The
    // final epsilon snap prevents long-tail drift when the stick is released.
    const float blend = 1.0f - std::exp(-elapsedMs / 24.0f);
    bool changed = false;
    auto approach = [&](float& current, float target) {
        const float before = current;
        current += (target - current) * blend;
        if (std::abs(target - current) < 0.0015f) current = target;
        changed = changed || std::abs(current - before) > 0.00035f;
    };
    approach(g_dualSenseVisualAxes.leftX, left.first);
    approach(g_dualSenseVisualAxes.leftY, left.second);
    approach(g_dualSenseVisualAxes.rightX, right.first);
    approach(g_dualSenseVisualAxes.rightY, right.second);
    return changed;
}

std::uint64_t dualSenseModelInputSignature(bool liveInput) {
    if (!liveInput) return 0;
    std::uint64_t signature = static_cast<std::uint64_t>(g_dualSenseLive.dpad & 0x0f);
    for (std::size_t index = 0; index < 15; ++index) {
        if (g_dualSenseLive.buttons[index]) signature |= 1ull << (4 + index);
    }
    auto axisByte = [](float value) {
        return static_cast<std::uint64_t>(std::clamp(
            static_cast<int>(std::lround((std::clamp(value, -1.0f, 1.0f) + 1.0f) * 127.5f)), 0, 255));
    };
    signature |= axisByte(g_dualSenseVisualAxes.leftX) << 20;
    signature |= axisByte(g_dualSenseVisualAxes.leftY) << 28;
    signature |= axisByte(g_dualSenseVisualAxes.rightX) << 36;
    signature |= axisByte(g_dualSenseVisualAxes.rightY) << 44;
    signature |= static_cast<std::uint64_t>(g_dualSenseLive.leftTrigger / 4) << 52;
    signature |= static_cast<std::uint64_t>(g_dualSenseLive.rightTrigger / 4) << 58;
    return signature;
}

struct DualSenseDpadDirections {
    bool up = false;
    bool right = false;
    bool down = false;
    bool left = false;
};

DualSenseDpadDirections dualSenseDpadDirections(int dpad) {
    // DualSense reports its hat clockwise: 0=N, 2=E, 4=S, 6=W and 8=neutral.
    // Keeping this mapping in one place prevents the right direction from
    // disappearing between input decoding, the cache mask and the animation.
    return {
        dpad == 0 || dpad == 1 || dpad == 7,
        dpad == 1 || dpad == 2 || dpad == 3,
        dpad == 3 || dpad == 4 || dpad == 5,
        dpad == 5 || dpad == 6 || dpad == 7
    };
}

std::uint32_t dualSenseActiveControlMask(bool liveInput) {
    if (!liveInput) return 0;
    std::uint32_t mask = 0;
    for (std::size_t index = 0; index < 15; ++index) {
        if (g_dualSenseLive.buttons[index]) mask |= 1u << index;
    }
    const float leftMagnitude = std::hypot(g_dualSenseVisualAxes.leftX, g_dualSenseVisualAxes.leftY);
    const float rightMagnitude = std::hypot(g_dualSenseVisualAxes.rightX, g_dualSenseVisualAxes.rightY);
    if (leftMagnitude > 0.002f) mask |= 1u << 10;
    if (rightMagnitude > 0.002f) mask |= 1u << 11;
    if (g_dualSenseLive.leftTrigger > 1) mask |= 1u << 6;
    if (g_dualSenseLive.rightTrigger > 1) mask |= 1u << 7;
    const DualSenseDpadDirections directions = dualSenseDpadDirections(g_dualSenseLive.dpad);
    if (directions.up) mask |= 1u << 15;
    if (directions.right) mask |= 1u << 16;
    if (directions.down) mask |= 1u << 17;
    if (directions.left) mask |= 1u << 18;
    return mask;
}

std::set<std::uint32_t> dualSenseComponentsForMask(std::uint32_t mask) {
    std::set<std::uint32_t> components;
    for (std::size_t index = 0; index < g_dualSenseControlComponentGroups.size(); ++index) {
        if ((mask & (1u << index)) == 0) continue;
        components.insert(g_dualSenseControlComponentGroups[index].begin(),
                          g_dualSenseControlComponentGroups[index].end());
    }
    for (std::size_t index = 0; index < g_dualSenseDpadComponentGroups.size(); ++index) {
        if ((mask & (1u << (15 + index))) == 0) continue;
        components.insert(g_dualSenseDpadComponentGroups[index].begin(),
                          g_dualSenseDpadComponentGroups[index].end());
    }
    return components;
}

float dualSenseButtonPressAt(const DualSenseMeshVertex& vertex, bool liveInput) {
    if (!liveInput) return 0.0f;
    auto inside = [&](float x, float y, float radius) {
        const float dx = vertex.x - x;
        const float dy = vertex.y - y;
        return dx * dx + dy * dy <= radius * radius;
    };
    auto onControl = [&](const std::vector<std::uint32_t>& components, float x, float y, float radius) {
        return !components.empty()
            ? std::binary_search(components.begin(), components.end(), vertex.component)
            : inside(x, y, radius);
    };
    // Highlight the real connected component rather than a circle projected
    // over the shell.  This keeps the feedback exactly on the physical cap at
    // every camera angle and prevents adjacent keys from flashing.
    if ((g_dualSenseLive.buttons[4] && onControl(g_dualSenseControlComponentGroups[4], -0.624f, 0.599f, 0.18f)) ||
        (g_dualSenseLive.buttons[5] && onControl(g_dualSenseControlComponentGroups[5], 0.624f, 0.599f, 0.18f))) {
        return 1.0f;
    }
    if (onControl(g_dualSenseControlComponentGroups[6], -0.598f, 0.556f, 0.18f)) {
        return g_dualSenseLive.leftTrigger / 255.0f;
    }
    if (onControl(g_dualSenseControlComponentGroups[7], 0.598f, 0.556f, 0.18f)) {
        return g_dualSenseLive.rightTrigger / 255.0f;
    }
    const DualSenseDpadDirections directions = dualSenseDpadDirections(g_dualSenseLive.dpad);
    if ((g_dualSenseLive.buttons[3] && onControl(g_dualSenseControlComponentGroups[3], 0.622f, 0.423f, 0.066f)) ||
        (g_dualSenseLive.buttons[2] && onControl(g_dualSenseControlComponentGroups[2], 0.771f, 0.276f, 0.066f)) ||
        (g_dualSenseLive.buttons[1] && onControl(g_dualSenseControlComponentGroups[1], 0.622f, 0.129f, 0.066f)) ||
        (g_dualSenseLive.buttons[0] && onControl(g_dualSenseControlComponentGroups[0], 0.476f, 0.276f, 0.066f)) ||
        (directions.up && onControl(g_dualSenseDpadComponentGroups[0], -0.620f, 0.374f, 0.060f)) ||
        (directions.right && onControl(g_dualSenseDpadComponentGroups[1], -0.529f, 0.274f, 0.060f)) ||
        (directions.down && onControl(g_dualSenseDpadComponentGroups[2], -0.626f, 0.179f, 0.060f)) ||
        (directions.left && onControl(g_dualSenseDpadComponentGroups[3], -0.721f, 0.280f, 0.060f)) ||
        (g_dualSenseLive.buttons[10] && onControl(g_dualSenseControlComponentGroups[10], -0.318f, -0.001f, 0.100f)) ||
        (g_dualSenseLive.buttons[11] && onControl(g_dualSenseControlComponentGroups[11], 0.318f, -0.001f, 0.100f)) ||
        (g_dualSenseLive.buttons[8] && onControl(g_dualSenseControlComponentGroups[8], -0.467f, 0.496f, 0.050f)) ||
        (g_dualSenseLive.buttons[9] && onControl(g_dualSenseControlComponentGroups[9], 0.467f, 0.496f, 0.050f)) ||
        (g_dualSenseLive.buttons[12] && onControl(g_dualSenseControlComponentGroups[12], 0.0f, -0.10f, 0.055f)) ||
        (g_dualSenseLive.buttons[14] && onControl(g_dualSenseControlComponentGroups[14], 0.0f, -0.17f, 0.050f))) {
        return 1.0f;
    }
    if (g_dualSenseLive.buttons[13] &&
        onControl(g_dualSenseControlComponentGroups[13], 0.0f, 0.405f, 0.39f)) {
        return 1.0f;
    }
    return 0.0f;
}

DualSenseMeshVertex animateDualSenseVertex(const DualSenseMeshVertex& source, bool liveInput) {
    DualSenseMeshVertex vertex = source;
    if (!liveInput) return vertex;
    // When the CAD topology exposes a separate control, translate that entire
    // component rigidly.  A radial mask is only kept as a fallback for unusual
    // exports.  Rigid motion avoids stretching the cap or the surrounding
    // shell while the controller is animated.
    auto influence = [&](float x, float y, float radius, float zMin, float normalMin,
                         const std::vector<std::uint32_t>& components) {
        if (!components.empty()) {
            return std::binary_search(components.begin(), components.end(), source.component) ? 1.0f : 0.0f;
        }
        if (source.z < zMin || source.nz < normalMin) return 0.0f;
        const float dx = source.x - x;
        const float dy = source.y - y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance >= radius) return 0.0f;
        const float radial = 1.0f - distance / radius;
        const float radialSmooth = radial * radial * (3.0f - 2.0f * radial);
        const float height = std::clamp((source.z - zMin) / 0.045f, 0.0f, 1.0f);
        return radialSmooth * height;
    };
    auto activeInfluence = [&](bool active, float x, float y, float radius, float zMin,
                               float normalMin, const std::vector<std::uint32_t>& components) {
        return active ? influence(x, y, radius, zMin, normalMin, components) : 0.0f;
    };

    // Stick caps: only the raised knob moves. The surrounding well and shell
    // stay fixed, while the already smoothed axes make the motion continuous.
    const float leftStick = influence(-0.318f, -0.001f, 0.12f, 0.32f, 0.10f, g_dualSenseControlComponentGroups[10]);
    const float rightStick = influence(0.318f, -0.001f, 0.12f, 0.32f, 0.10f, g_dualSenseControlComponentGroups[11]);
    vertex.x += g_dualSenseVisualAxes.leftX * leftStick * 0.050f;
    vertex.y -= g_dualSenseVisualAxes.leftY * leftStick * 0.050f;
    vertex.x += g_dualSenseVisualAxes.rightX * rightStick * 0.050f;
    vertex.y -= g_dualSenseVisualAxes.rightY * rightStick * 0.050f;

    float facePress = 0.0f;
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[3], 0.622f, 0.423f, 0.070f, 0.285f, -0.35f, g_dualSenseControlComponentGroups[3]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[2], 0.771f, 0.276f, 0.070f, 0.255f, -0.35f, g_dualSenseControlComponentGroups[2]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[1], 0.622f, 0.129f, 0.070f, 0.275f, -0.35f, g_dualSenseControlComponentGroups[1]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[0], 0.476f, 0.276f, 0.070f, 0.295f, -0.35f, g_dualSenseControlComponentGroups[0]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[4], -0.624f, 0.599f, 0.18f, 0.04f, -1.0f, g_dualSenseControlComponentGroups[4]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[5], 0.624f, 0.599f, 0.18f, 0.04f, -1.0f, g_dualSenseControlComponentGroups[5]));

    const DualSenseDpadDirections directions = dualSenseDpadDirections(g_dualSenseLive.dpad);
    float dpadPress = 0.0f;
    dpadPress = std::max(dpadPress, activeInfluence(directions.up, -0.620f, 0.374f, 0.070f, 0.275f, -0.35f, g_dualSenseDpadComponentGroups[0]));
    dpadPress = std::max(dpadPress, activeInfluence(directions.right, -0.529f, 0.274f, 0.070f, 0.275f, -0.35f, g_dualSenseDpadComponentGroups[1]));
    dpadPress = std::max(dpadPress, activeInfluence(directions.down, -0.626f, 0.179f, 0.070f, 0.275f, -0.35f, g_dualSenseDpadComponentGroups[2]));
    dpadPress = std::max(dpadPress, activeInfluence(directions.left, -0.721f, 0.280f, 0.070f, 0.275f, -0.35f, g_dualSenseDpadComponentGroups[3]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[8], -0.467f, 0.496f, 0.060f, 0.245f, -0.35f, g_dualSenseControlComponentGroups[8]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[9], 0.467f, 0.496f, 0.060f, 0.245f, -0.35f, g_dualSenseControlComponentGroups[9]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[12], 0.0f, -0.10f, 0.060f, 0.18f, -0.35f, g_dualSenseControlComponentGroups[12]));
    facePress = std::max(facePress, activeInfluence(g_dualSenseLive.buttons[14], 0.0f, -0.17f, 0.055f, 0.13f, -0.35f, g_dualSenseControlComponentGroups[14]));

    const float touchpadPress = g_dualSenseLive.buttons[13] &&
                                std::binary_search(g_dualSenseControlComponentGroups[13].begin(),
                                                   g_dualSenseControlComponentGroups[13].end(),
                                                   source.component) ? 1.0f : 0.0f;
    // Face keys travel only a few millimetres. Keeping the depth change small
    // prevents the shell behind a cap from winning the depth test and removes
    // the grey ``cut-out'' artefact seen on the touchpad.
    vertex.z -= facePress * 0.008f;
    // The directional pad has a deeper mechanical travel than the face keys.
    // Its complete material stack moves rigidly, so the press is visible
    // without stretching the surrounding shell.
    vertex.z -= dpadPress * 0.016f;
    // The touchpad moves as one rigid plate, along the same depth axis as the
    // real click mechanism.
    vertex.z -= touchpadPress * 0.0050f;

    // L2/R2 have a longer travel and a slightly forward/downward arc, but the
    // same compact influence keeps their caps separate from the shoulders.
    const float leftTrigger = g_dualSenseLive.leftTrigger / 255.0f;
    const float rightTrigger = g_dualSenseLive.rightTrigger / 255.0f;
    const float triggerWeight = std::max(
        leftTrigger * influence(-0.598f, 0.556f, 0.19f, -0.20f, -1.0f, g_dualSenseControlComponentGroups[6]),
        rightTrigger * influence(0.598f, 0.556f, 0.19f, -0.20f, -1.0f, g_dualSenseControlComponentGroups[7]));
    vertex.y -= triggerWeight * 0.018f;
    vertex.z -= triggerWeight * 0.010f;
    return vertex;
}

enum class DualSenseRasterPass { Full, StaticBody, DynamicControls };

void drawDualSenseRasterMesh(Graphics& target, const RectF& modelRect, bool liveInput,
                             DualSenseRasterPass pass = DualSenseRasterPass::Full,
                             std::vector<float>* depthOutput = nullptr,
                             const std::vector<float>* staticDepth = nullptr,
                             const std::set<std::uint32_t>* selectedComponents = nullptr,
                             bool excludeSelectedComponents = false) {
    const int width = std::max(1, static_cast<int>(std::lround(modelRect.Width)));
    const int height = std::max(1, static_cast<int>(std::lround(modelRect.Height)));
    Bitmap raster(width, height, PixelFormat32bppARGB);
    if (raster.GetLastStatus() != Ok) return;
    {
        Graphics background(&raster);
        if (pass == DualSenseRasterPass::DynamicControls) {
            background.Clear(Color(0, 0, 0, 0));
        } else {
            SolidBrush backdrop(Color(255, 20, 24, 36));
            background.FillRectangle(&backdrop, RectF(0, 0, static_cast<REAL>(width), static_cast<REAL>(height)));
            SolidBrush shadow(Color(85, 0, 0, 0));
            background.FillEllipse(&shadow, RectF(width * 0.17f, height - 45.0f, width * 0.66f, 34.0f));
        }
    }

    Rect lockRect(0, 0, width, height);
    BitmapData bitmapData{};
    if (raster.LockBits(&lockRect, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &bitmapData) != Ok) return;
    std::vector<float> depthBuffer(static_cast<std::size_t>(width) * height,
                                   -std::numeric_limits<float>::infinity());
    if (staticDepth && staticDepth->size() == depthBuffer.size()) depthBuffer = *staticDepth;
    const float cx = width * 0.5f;
    const float cy = height * 0.51f;
    const float scale = std::min(width * 0.47f, height * 0.73f);
    auto project = [&](const DualSenseRenderPoint& point) {
        const float perspective = 1.0f / std::max(0.66f, 1.0f + point.z * 0.12f);
        return PointF(cx + point.x * scale * perspective, cy - point.y * scale * perspective);
    };
    auto sampleMap = [&](const DualSenseTexture& texture, float u, float v,
                         BYTE fallbackRed, BYTE fallbackGreen, BYTE fallbackBlue,
                         BYTE& red, BYTE& green, BYTE& blue) {
        if (!texture.loaded || texture.pixels.empty() || texture.width <= 0 || texture.height <= 0) {
            red = fallbackRed; green = fallbackGreen; blue = fallbackBlue;
            return;
        }
        const float wrappedU = u - std::floor(u);
        const float wrappedV = v - std::floor(v);
        const float textureX = wrappedU * (texture.width - 1);
        const float textureY = (1.0f - wrappedV) * (texture.height - 1);
        const int x0 = std::clamp(static_cast<int>(std::floor(textureX)), 0, texture.width - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(textureY)), 0, texture.height - 1);
        const int x1 = std::min(x0 + 1, texture.width - 1);
        const int y1 = std::min(y0 + 1, texture.height - 1);
        const float tx = textureX - x0;
        const float ty = textureY - y0;
        auto channel = [&](int x, int y, int offset) {
            return static_cast<float>(texture.pixels[(static_cast<std::size_t>(y) * texture.width + x) * 4 + offset]);
        };
        auto bilinear = [&](int offset) {
            const float top = channel(x0, y0, offset) * (1.0f - tx) + channel(x1, y0, offset) * tx;
            const float bottom = channel(x0, y1, offset) * (1.0f - tx) + channel(x1, y1, offset) * tx;
            return static_cast<BYTE>(std::clamp(top * (1.0f - ty) + bottom * ty, 0.0f, 255.0f));
        };
        blue = bilinear(0); green = bilinear(1); red = bilinear(2);
    };
    auto sampleTexture = [&](std::uint8_t material, float u, float v, BYTE& red, BYTE& green, BYTE& blue) {
        static constexpr BYTE fallback[3][3] = {
            {33, 37, 51}, {232, 235, 243}, {170, 175, 186}
        };
        const int index = std::min<int>(material, 2);
        sampleMap(g_dualSenseTextures[index], u, v,
                  fallback[index][0], fallback[index][1], fallback[index][2], red, green, blue);
    };
    auto lineDistance = [](float px, float py, float ax, float ay, float bx, float by) {
        const float abX = bx - ax;
        const float abY = by - ay;
        const float lengthSquared = abX * abX + abY * abY;
        const float amount = lengthSquared > 0.000001f
            ? std::clamp(((px - ax) * abX + (py - ay) * abY) / lengthSquared, 0.0f, 1.0f)
            : 0.0f;
        const float dx = px - (ax + abX * amount);
        const float dy = py - (ay + abY * amount);
        return std::sqrt(dx * dx + dy * dy);
    };
    auto faceSymbolStrength = [&](float modelX, float modelY, float modelZ) {
        if (modelZ < 0.27f) return 0.0f;
        auto stroke = [](float distance) {
            return std::clamp((0.115f - distance) / 0.045f, 0.0f, 1.0f);
        };
        auto local = [&](float centerX, float centerY, float& x, float& y) {
            x = (modelX - centerX) / 0.068f;
            y = (modelY - centerY) / 0.068f;
            return x * x + y * y < 0.78f;
        };
        float x = 0, y = 0;
        if (local(0.622f, 0.423f, x, y)) {
            return std::max({stroke(lineDistance(x, y, 0.0f, 0.48f, -0.43f, -0.34f)),
                             stroke(lineDistance(x, y, -0.43f, -0.34f, 0.43f, -0.34f)),
                             stroke(lineDistance(x, y, 0.43f, -0.34f, 0.0f, 0.48f))});
        }
        if (local(0.768f, 0.275f, x, y)) {
            return stroke(std::abs(std::sqrt(x * x + y * y) - 0.43f));
        }
        if (local(0.622f, 0.129f, x, y)) {
            return std::max(stroke(lineDistance(x, y, -0.36f, -0.36f, 0.36f, 0.36f)),
                            stroke(lineDistance(x, y, -0.36f, 0.36f, 0.36f, -0.36f)));
        }
        if (local(0.476f, 0.278f, x, y)) {
            return std::max({stroke(lineDistance(x, y, -0.38f, -0.38f, 0.38f, -0.38f)),
                             stroke(lineDistance(x, y, 0.38f, -0.38f, 0.38f, 0.38f)),
                             stroke(lineDistance(x, y, 0.38f, 0.38f, -0.38f, 0.38f)),
                             stroke(lineDistance(x, y, -0.38f, 0.38f, -0.38f, -0.38f))});
        }
        return 0.0f;
    };

    const DualSenseRenderPoint light{0.35f, 0.65f, 1.0f};
    const std::vector<DualSenseMeshTriangle>& renderTriangles =
        pass == DualSenseRasterPass::DynamicControls ? g_dualSenseDynamicTriangles : g_dualSenseMesh.triangles;
    for (const DualSenseMeshTriangle& triangle : renderTriangles) {
        if (triangle.a >= g_dualSenseMesh.vertices.size() || triangle.b >= g_dualSenseMesh.vertices.size() ||
            triangle.c >= g_dualSenseMesh.vertices.size()) continue;
        const DualSenseMeshVertex sourceA = g_dualSenseMesh.vertices[triangle.a];
        const DualSenseMeshVertex sourceB = g_dualSenseMesh.vertices[triangle.b];
        const DualSenseMeshVertex sourceC = g_dualSenseMesh.vertices[triangle.c];
        const bool dynamicComponent = g_dualSenseDynamicComponents.contains(sourceA.component);
        if ((pass == DualSenseRasterPass::StaticBody && dynamicComponent) ||
            (pass == DualSenseRasterPass::DynamicControls && !dynamicComponent)) continue;
        if (pass == DualSenseRasterPass::DynamicControls && selectedComponents) {
            const bool selected = selectedComponents->contains(sourceA.component);
            if ((!excludeSelectedComponents && !selected) || (excludeSelectedComponents && selected)) continue;
        }
        const DualSenseMeshVertex animatedA = animateDualSenseVertex(sourceA, liveInput);
        const DualSenseMeshVertex animatedB = animateDualSenseVertex(sourceB, liveInput);
        const DualSenseMeshVertex animatedC = animateDualSenseVertex(sourceC, liveInput);
        const DualSenseRenderPoint a = rotateDualSensePoint(animatedA);
        const DualSenseRenderPoint b = rotateDualSensePoint(animatedB);
        const DualSenseRenderPoint c = rotateDualSensePoint(animatedC);
        const PointF pa = project(a), pb = project(b), pc = project(c);
        const float inverseWa = 1.0f / std::max(0.66f, 1.0f + a.z * 0.12f);
        const float inverseWb = 1.0f / std::max(0.66f, 1.0f + b.z * 0.12f);
        const float inverseWc = 1.0f / std::max(0.66f, 1.0f + c.z * 0.12f);
        const float denominator = (pb.Y - pc.Y) * (pa.X - pc.X) + (pc.X - pb.X) * (pa.Y - pc.Y);
        if (std::abs(denominator) < 0.00001f) continue;
        const int minX = std::clamp(static_cast<int>(std::floor(std::min({pa.X, pb.X, pc.X}))), 0, width - 1);
        const int maxX = std::clamp(static_cast<int>(std::ceil(std::max({pa.X, pb.X, pc.X}))), 0, width - 1);
        const int minY = std::clamp(static_cast<int>(std::floor(std::min({pa.Y, pb.Y, pc.Y}))), 0, height - 1);
        const int maxY = std::clamp(static_cast<int>(std::ceil(std::max({pa.Y, pb.Y, pc.Y}))), 0, height - 1);
        if (minX > maxX || minY > maxY) continue;

        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;
        const float normalLength = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float fallbackNormalX = normalLength > 0.00001f ? nx / normalLength : 0.0f;
        const float fallbackNormalY = normalLength > 0.00001f ? ny / normalLength : 0.0f;
        const float fallbackNormalZ = normalLength > 0.00001f ? nz / normalLength : 1.0f;
        const float pressA = dualSenseButtonPressAt(sourceA, liveInput);
        const float pressB = dualSenseButtonPressAt(sourceB, liveInput);
        const float pressC = dualSenseButtonPressAt(sourceC, liveInput);

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float px = x + 0.5f;
                const float py = y + 0.5f;
                const float wa = ((pb.Y - pc.Y) * (px - pc.X) + (pc.X - pb.X) * (py - pc.Y)) / denominator;
                const float wb = ((pc.Y - pa.Y) * (px - pc.X) + (pa.X - pc.X) * (py - pc.Y)) / denominator;
                const float wc = 1.0f - wa - wb;
                if (wa < -0.0001f || wb < -0.0001f || wc < -0.0001f) continue;
                const float perspectiveSum = wa * inverseWa + wb * inverseWb + wc * inverseWc;
                if (perspectiveSum <= 0.000001f) continue;
                const float ca = wa * inverseWa / perspectiveSum;
                const float cb = wb * inverseWb / perspectiveSum;
                const float cc = wc * inverseWc / perspectiveSum;
                const float depth = ca * a.z + cb * b.z + cc * c.z;
                const std::size_t pixelIndex = static_cast<std::size_t>(y) * width + x;
                if (depth <= depthBuffer[pixelIndex]) continue;
                depthBuffer[pixelIndex] = depth;
                BYTE red = 0, green = 0, blue = 0;
                const float textureU = ca * sourceA.u + cb * sourceB.u + cc * sourceC.u;
                const float textureV = ca * sourceA.v + cb * sourceB.v + cc * sourceC.v;
                sampleTexture(triangle.material,
                              textureU, textureV,
                              red, green, blue);
                const float modelX = ca * animatedA.x + cb * animatedB.x + cc * animatedC.x;
                const float modelY = ca * animatedA.y + cb * animatedB.y + cc * animatedC.y;
                const float modelZ = ca * animatedA.z + cb * animatedB.z + cc * animatedC.z;
                const float symbol = faceSymbolStrength(modelX, modelY, modelZ);
                if (symbol > 0.0f) {
                    red = static_cast<BYTE>(red * (1.0f - symbol) + 100.0f * symbol);
                    green = static_cast<BYTE>(green * (1.0f - symbol) + 104.0f * symbol);
                    blue = static_cast<BYTE>(blue * (1.0f - symbol) + 114.0f * symbol);
                }
                const int materialIndex = std::min<int>(triangle.material, 2);
                BYTE normalRed = 128, normalGreen = 128, normalBlue = 255;
                sampleMap(g_dualSenseNormalTextures[materialIndex], textureU, textureV,
                          128, 128, 255, normalRed, normalGreen, normalBlue);
                float tangentNormalX = normalRed / 127.5f - 1.0f;
                float tangentNormalY = normalGreen / 127.5f - 1.0f;
                float tangentNormalZ = normalBlue / 127.5f - 1.0f;
                const float mappedLength = std::sqrt(tangentNormalX * tangentNormalX +
                                                     tangentNormalY * tangentNormalY +
                                                     tangentNormalZ * tangentNormalZ);
                if (mappedLength > 0.00001f) {
                    tangentNormalX /= mappedLength;
                    tangentNormalY /= mappedLength;
                    tangentNormalZ /= mappedLength;
                }
                const float normalX = ca * sourceA.nx + cb * sourceB.nx + cc * sourceC.nx;
                const float normalY = ca * sourceA.ny + cb * sourceB.ny + cc * sourceC.ny;
                const float normalZ = ca * sourceA.nz + cb * sourceB.nz + cc * sourceC.nz;
                const float tangentX = ca * sourceA.tx + cb * sourceB.tx + cc * sourceC.tx;
                const float tangentY = ca * sourceA.ty + cb * sourceB.ty + cc * sourceC.ty;
                const float tangentZ = ca * sourceA.tz + cb * sourceB.tz + cc * sourceC.tz;
                const float bitangentX = ca * sourceA.bx + cb * sourceB.bx + cc * sourceC.bx;
                const float bitangentY = ca * sourceA.by + cb * sourceB.by + cc * sourceC.by;
                const float bitangentZ = ca * sourceA.bz + cb * sourceB.bz + cc * sourceC.bz;
                DualSenseRenderPoint shadedNormal = rotateDualSenseDirection(
                    tangentX * tangentNormalX + bitangentX * tangentNormalY + normalX * tangentNormalZ,
                    tangentY * tangentNormalX + bitangentY * tangentNormalY + normalY * tangentNormalZ,
                    tangentZ * tangentNormalX + bitangentZ * tangentNormalY + normalZ * tangentNormalZ);
                float shadedLength = std::sqrt(shadedNormal.x * shadedNormal.x + shadedNormal.y * shadedNormal.y +
                                               shadedNormal.z * shadedNormal.z);
                if (shadedLength <= 0.00001f) {
                    shadedNormal = {fallbackNormalX, fallbackNormalY, fallbackNormalZ};
                    shadedLength = 1.0f;
                }
                shadedNormal.x /= shadedLength;
                shadedNormal.y /= shadedLength;
                shadedNormal.z /= shadedLength;

                BYTE roughRed = 180, roughGreen = 180, roughBlue = 180;
                BYTE metalRed = 0, metalGreen = 0, metalBlue = 0;
                sampleMap(g_dualSenseRoughnessTextures[materialIndex], textureU, textureV,
                          180, 180, 180, roughRed, roughGreen, roughBlue);
                sampleMap(g_dualSenseMetallicTextures[materialIndex], textureU, textureV,
                          0, 0, 0, metalRed, metalGreen, metalBlue);
                const float roughness = std::clamp(roughRed / 255.0f, 0.06f, 1.0f);
                const float metallic = std::clamp(metalRed / 255.0f, 0.0f, 1.0f);
                const float diffuse = std::max(0.0f, shadedNormal.x * light.x + shadedNormal.y * light.y +
                                                       shadedNormal.z * light.z) /
                                      std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
                constexpr float halfX = 0.115f;
                constexpr float halfY = 0.215f;
                constexpr float halfZ = 0.970f;
                const float specularPower = 6.0f + (1.0f - roughness) * 74.0f;
                const float specular = std::pow(std::max(0.0f, shadedNormal.x * halfX + shadedNormal.y * halfY +
                                                                shadedNormal.z * halfZ), specularPower) *
                                       ((1.0f - roughness) * 0.42f + metallic * 0.34f);
                const float lighting = 0.67f + diffuse * 0.38f;
                const float pressedShade = 1.0f - (ca * pressA + cb * pressB + cc * pressC) * 0.20f;
                BYTE* row = bitmapData.Stride >= 0
                    ? static_cast<BYTE*>(bitmapData.Scan0) + static_cast<std::size_t>(y) * bitmapData.Stride
                    : static_cast<BYTE*>(bitmapData.Scan0) + static_cast<std::size_t>(height - 1 - y) * (-bitmapData.Stride);
                BYTE* pixel = row + static_cast<std::size_t>(x) * 4;
                pixel[0] = static_cast<BYTE>(std::clamp(blue * lighting * pressedShade + 255.0f * specular, 0.0f, 255.0f));
                pixel[1] = static_cast<BYTE>(std::clamp(green * lighting * pressedShade + 255.0f * specular, 0.0f, 255.0f));
                pixel[2] = static_cast<BYTE>(std::clamp(red * lighting * pressedShade + 255.0f * specular, 0.0f, 255.0f));
                pixel[3] = 255;
            }
        }
    }
    if (depthOutput) *depthOutput = depthBuffer;
    raster.UnlockBits(&bitmapData);
    const InterpolationMode previousInterpolation = target.GetInterpolationMode();
    // During a drag this bitmap is rebuilt for each camera step. Bilinear
    // filtering keeps the preview responsive; the idle path still uses the
    // sharper bicubic pass below the model cache.
    target.SetInterpolationMode(g_gamepadDragging ? InterpolationModeBilinear : InterpolationModeHighQualityBicubic);
    target.DrawImage(&raster, modelRect);
    target.SetInterpolationMode(previousInterpolation);
}

void drawDualSenseModel(Graphics& graphics, const RectF& modelRect, bool liveInput, bool lightingReady) {
    if (!g_dualSenseMesh.loaded || g_dualSenseMesh.vertices.empty()) return;
    const SmoothingMode oldSmoothing = graphics.GetSmoothingMode();
    // The shell is static while the controller is used, so rasterize it only
    // when the camera angle or output size changes.  Buttons and sticks are a
    // separate lightweight 3D pass.  This removes the old 100k-triangle rebuild
    // from every joystick packet while preserving the original CAD quality.
    const float cacheAngleStep = g_gamepadDragging ? 0.085f : 0.045f;
    const float cacheYaw = std::round(g_gamepadYaw / cacheAngleStep) * cacheAngleStep;
    const float cachePitch = std::round(g_gamepadPitch / cacheAngleStep) * cacheAngleStep;
    const float modelSupersampling = g_gamepadDragging ? 0.86f : 1.10f;
    const int cacheWidth = std::max(1, static_cast<int>(std::lround(modelRect.Width * modelSupersampling)));
    const int cacheHeight = std::max(1, static_cast<int>(std::lround(modelRect.Height * modelSupersampling)));
    const bool cacheValid = g_dualSenseModelCache && g_dualSenseModelCacheWidth == cacheWidth &&
                            g_dualSenseModelCacheHeight == cacheHeight &&
                            std::abs(g_dualSenseModelCacheYaw - cacheYaw) < 0.0001f &&
                            std::abs(g_dualSenseModelCachePitch - cachePitch) < 0.0001f &&
                            g_dualSenseModelDepthCache.size() == static_cast<std::size_t>(cacheWidth) * cacheHeight;
    if (!cacheValid) {
        g_dualSenseModelCache = std::make_unique<Bitmap>(cacheWidth, cacheHeight, PixelFormat32bppPARGB);
        g_dualSenseModelDepthCache.clear();
        g_dualSenseControlCache.reset();
        g_dualSenseControlCacheInputSignature = UINT64_MAX;
        g_dualSenseMovingControlCache.reset();
        g_dualSenseMovingControlCacheInputSignature = UINT64_MAX;
        if (g_dualSenseModelCache && g_dualSenseModelCache->GetLastStatus() == Ok) {
            Graphics cacheGraphics(g_dualSenseModelCache.get());
            const float savedYaw = g_gamepadYaw;
            const float savedPitch = g_gamepadPitch;
            g_gamepadYaw = cacheYaw;
            g_gamepadPitch = cachePitch;
            drawDualSenseRasterMesh(cacheGraphics,
                                    RectF(0, 0, static_cast<REAL>(cacheWidth), static_cast<REAL>(cacheHeight)),
                                    false, DualSenseRasterPass::StaticBody, &g_dualSenseModelDepthCache);
            g_gamepadYaw = savedYaw;
            g_gamepadPitch = savedPitch;
            g_dualSenseModelCacheWidth = cacheWidth;
            g_dualSenseModelCacheHeight = cacheHeight;
            g_dualSenseModelCacheYaw = cacheYaw;
            g_dualSenseModelCachePitch = cachePitch;
        } else {
            g_dualSenseModelCache.reset();
        }
    }
    const InterpolationMode previousInterpolation = graphics.GetInterpolationMode();
    const PixelOffsetMode previousPixelOffset = graphics.GetPixelOffsetMode();
    graphics.SetInterpolationMode(InterpolationModeBilinear);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    if (g_dualSenseModelCache && g_dualSenseModelCache->GetLastStatus() == Ok) {
        graphics.DrawImage(g_dualSenseModelCache.get(), modelRect);
    }
    // Use the cache's exact quantized camera for controls, depth and lights so
    // none of those layers can drift while the user rotates the controller.
    const float savedOverlayYaw = g_gamepadYaw;
    const float savedOverlayPitch = g_gamepadPitch;
    g_gamepadYaw = cacheYaw;
    g_gamepadPitch = cachePitch;
    const std::uint32_t activeControlMask = dualSenseActiveControlMask(liveInput);
    const std::set<std::uint32_t> movingComponents = dualSenseComponentsForMask(activeControlMask);
    const bool controlCacheValid = g_dualSenseControlCache &&
                                   g_dualSenseControlCacheWidth == cacheWidth &&
                                   g_dualSenseControlCacheHeight == cacheHeight &&
                                   std::abs(g_dualSenseControlCacheYaw - cacheYaw) < 0.0001f &&
                                   std::abs(g_dualSenseControlCachePitch - cachePitch) < 0.0001f &&
                                   g_dualSenseControlCacheInputSignature == activeControlMask;
    if (!controlCacheValid) {
        g_dualSenseControlCache = std::make_unique<Bitmap>(cacheWidth, cacheHeight, PixelFormat32bppPARGB);
        if (g_dualSenseControlCache && g_dualSenseControlCache->GetLastStatus() == Ok) {
            Graphics dynamicGraphics(g_dualSenseControlCache.get());
            dynamicGraphics.Clear(Color(0, 0, 0, 0));
            drawDualSenseRasterMesh(dynamicGraphics,
                                    RectF(0, 0, static_cast<REAL>(cacheWidth), static_cast<REAL>(cacheHeight)),
                                    false, DualSenseRasterPass::DynamicControls, nullptr,
                                    &g_dualSenseModelDepthCache, &movingComponents, true);
            g_dualSenseControlCacheWidth = cacheWidth;
            g_dualSenseControlCacheHeight = cacheHeight;
            g_dualSenseControlCacheYaw = cacheYaw;
            g_dualSenseControlCachePitch = cachePitch;
            g_dualSenseControlCacheInputSignature = activeControlMask;
        } else {
            g_dualSenseControlCache.reset();
        }
    }
    if (g_dualSenseControlCache && g_dualSenseControlCache->GetLastStatus() == Ok) {
        graphics.DrawImage(g_dualSenseControlCache.get(), modelRect);
    }
    const std::uint64_t inputSignature = dualSenseModelInputSignature(liveInput);
    const bool movingCacheValid = g_dualSenseMovingControlCache &&
                                  g_dualSenseMovingControlCacheWidth == cacheWidth &&
                                  g_dualSenseMovingControlCacheHeight == cacheHeight &&
                                  std::abs(g_dualSenseMovingControlCacheYaw - cacheYaw) < 0.0001f &&
                                  std::abs(g_dualSenseMovingControlCachePitch - cachePitch) < 0.0001f &&
                                  g_dualSenseMovingControlCacheInputSignature == inputSignature;
    if (!movingCacheValid) {
        g_dualSenseMovingControlCache = std::make_unique<Bitmap>(cacheWidth, cacheHeight, PixelFormat32bppPARGB);
        if (g_dualSenseMovingControlCache && g_dualSenseMovingControlCache->GetLastStatus() == Ok) {
            Graphics movingGraphics(g_dualSenseMovingControlCache.get());
            movingGraphics.Clear(Color(0, 0, 0, 0));
            if (!movingComponents.empty()) {
                drawDualSenseRasterMesh(movingGraphics,
                                        RectF(0, 0, static_cast<REAL>(cacheWidth), static_cast<REAL>(cacheHeight)),
                                        liveInput, DualSenseRasterPass::DynamicControls, nullptr,
                                        &g_dualSenseModelDepthCache, &movingComponents, false);
            }
            g_dualSenseMovingControlCacheWidth = cacheWidth;
            g_dualSenseMovingControlCacheHeight = cacheHeight;
            g_dualSenseMovingControlCacheYaw = cacheYaw;
            g_dualSenseMovingControlCachePitch = cachePitch;
            g_dualSenseMovingControlCacheInputSignature = inputSignature;
        } else {
            g_dualSenseMovingControlCache.reset();
        }
    }
    if (g_dualSenseMovingControlCache && g_dualSenseMovingControlCache->GetLastStatus() == Ok) {
        graphics.DrawImage(g_dualSenseMovingControlCache.get(), modelRect);
    }
    graphics.SetInterpolationMode(previousInterpolation);
    graphics.SetPixelOffsetMode(previousPixelOffset);

    const float cx = modelRect.X + modelRect.Width * 0.5f;
    const float cy = modelRect.Y + modelRect.Height * 0.51f;
    const float scale = std::min(modelRect.Width * 0.47f, modelRect.Height * 0.73f);
    auto project = [&](const DualSenseRenderPoint& point) {
        // A shallow perspective makes the rear and side edges readable while
        // keeping the controller stable at every window size.
        const float perspective = 1.0f / std::max(0.66f, 1.0f + point.z * 0.12f);
        return PointF(cx + point.x * scale * perspective,
                      cy - point.y * scale * perspective);
    };

    // The supplied CAD asset has no dedicated LED material. Add thin emissive
    // ribbons in model space instead of painting screen-space strokes over the
    // controller. Their vertices now share the model's rotation, perspective
    // and front-face visibility, so the light physically follows the touchpad
    // seam and disappears when the controller is viewed from the rear.
    const std::uint32_t litRgb = gamepadLightingPreviewRgb(GetTickCount64());
    const Color lightColor(lightingReady ? 255 : 170,
                          static_cast<BYTE>((litRgb >> 16) & 0xff),
                          static_cast<BYTE>((litRgb >> 8) & 0xff),
                          static_cast<BYTE>(litRgb & 0xff));
    auto modelPoint = [&](float x, float y, float z) {
        DualSenseMeshVertex vertex;
        vertex.x = x + g_dualSenseMesh.centerX;
        vertex.y = y + g_dualSenseMesh.centerY;
        vertex.z = z + g_dualSenseMesh.centerZ;
        return project(rotateDualSensePoint(vertex));
    };
    const DualSenseRenderPoint frontNormal = rotateDualSenseDirection(0.0f, 0.0f, 1.0f);
    const float frontVisibility = std::clamp((frontNormal.z - 0.03f) / 0.94f, 0.0f, 1.0f);
    const float touchpadLightDepthOffset = liveInput && g_dualSenseLive.buttons[13] ? -0.0050f : 0.0f;
    auto curvePoint = [&](bool left, float t) {
        const auto& anchors = left ? g_dualSenseTouchpadLeftEdge : g_dualSenseTouchpadRightEdge;
        const float scaled = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(anchors.size() - 1);
        const std::size_t first = std::min<std::size_t>(static_cast<std::size_t>(scaled), anchors.size() - 2);
        const std::size_t second = first + 1;
        const float amount = scaled - static_cast<float>(first);
        return DualSenseLightAnchor{
            anchors[first].x + (anchors[second].x - anchors[first].x) * amount,
            anchors[first].y + (anchors[second].y - anchors[first].y) * amount,
            anchors[first].z + (anchors[second].z - anchors[first].z) * amount + touchpadLightDepthOffset
        };
    };
    auto drawRibbon = [&](bool left, float width, Color color) {
        if (frontVisibility <= 0.01f || color.GetA() == 0) return;
        constexpr int segments = 18;
        std::array<PointF, (segments + 1) * 2> polygon{};
        for (int index = 0; index <= segments; ++index) {
            const float t = index / static_cast<float>(segments);
            const DualSenseLightAnchor center = curvePoint(left, t);
            const DualSenseLightAnchor before = curvePoint(left, std::max(0.0f, t - 0.01f));
            const DualSenseLightAnchor after = curvePoint(left, std::min(1.0f, t + 0.01f));
            const float tangentX = after.x - before.x;
            const float tangentY = after.y - before.y;
            const float length = std::max(0.00001f, std::sqrt(tangentX * tangentX + tangentY * tangentY));
            const float normalX = -tangentY / length;
            const float normalY = tangentX / length;
            const float half = width * 0.5f;
            polygon[index] = modelPoint(center.x + normalX * half, center.y + normalY * half, center.z);
            polygon[polygon.size() - 1 - index] =
                modelPoint(center.x - normalX * half, center.y - normalY * half, center.z);
        }
        GraphicsPath ribbon;
        ribbon.AddPolygon(polygon.data(), static_cast<INT>(polygon.size()));
        SolidBrush brush(Color(static_cast<BYTE>(std::lround(color.GetA() * frontVisibility)),
                               color.GetR(), color.GetG(), color.GetB()));
        graphics.FillPath(&brush, &ribbon);
    };

    // Keep the two white lower strips in model space. They sit directly under
    // the touchpad edge, span its width and follow every camera rotation or
    // touchpad click instead of looking like floating UI elements.
    auto drawLowerLightWindow = [&](float centerX, float halfLength, float radius, Color color) {
        if (frontVisibility <= 0.01f || color.GetA() == 0) return;
        constexpr int capSegments = 8;
        std::array<PointF, capSegments * 2 + 2> polygon{};
        constexpr float pi = 3.14159265358979323846f;
        const float centerY = 0.208f;
        const float centerZ = 0.333f + touchpadLightDepthOffset;
        int point = 0;
        for (int index = 0; index <= capSegments; ++index) {
            const float angle = -pi * 0.5f + pi * index / capSegments;
            polygon[point++] = modelPoint(centerX + halfLength + std::cos(angle) * radius,
                                          centerY + std::sin(angle) * radius, centerZ);
        }
        for (int index = 0; index <= capSegments; ++index) {
            const float angle = pi * 0.5f + pi * index / capSegments;
            polygon[point++] = modelPoint(centerX - halfLength + std::cos(angle) * radius,
                                          centerY + std::sin(angle) * radius, centerZ);
        }
        SolidBrush brush(Color(static_cast<BYTE>(std::lround(color.GetA() * frontVisibility)),
                               color.GetR(), color.GetG(), color.GetB()));
        graphics.FillPolygon(&brush, polygon.data(), point);
    };

    if (frontVisibility > 0.01f && !g_captureSuppressDualSenseLightOverlays) {
        const Color channel(215, 12, 15, 23);
        if (lightingReady) {
            const Color glow(54, lightColor.GetR(), lightColor.GetG(), lightColor.GetB());
            const Color softCore(228, lightColor.GetR(), lightColor.GetG(), lightColor.GetB());
            drawRibbon(true, 0.032f, glow);
            drawRibbon(false, 0.032f, glow);
            drawRibbon(true, 0.023f, channel);
            drawRibbon(false, 0.023f, channel);
            drawRibbon(true, 0.014f, softCore);
            drawRibbon(false, 0.014f, softCore);
            drawRibbon(true, 0.006f, lightColor);
            drawRibbon(false, 0.006f, lightColor);

        } else {
            drawRibbon(true, 0.023f, channel);
            drawRibbon(false, 0.023f, channel);
            const Color unlit(220, 52, 60, 77);
            drawRibbon(true, 0.010f, unlit);
            drawRibbon(false, 0.010f, unlit);
        }

        // These two lower windows are the cool-white player indicators, not
        // part of the RGB lightbar. Their state follows the dedicated toggle
        // and never inherits the selected colour or animated effect.
        const bool lowerIndicatorsOn = lightingReady && g_dualSensePlayerLedsEnabled;
        if (lowerIndicatorsOn) {
            const Color whiteGlow(62, 194, 214, 255);
            const Color whiteEmitter(255, 239, 245, 255);
            drawLowerLightWindow(-0.153f, 0.128f, 0.012f, whiteGlow);
            drawLowerLightWindow(0.153f, 0.128f, 0.012f, whiteGlow);
            drawLowerLightWindow(-0.153f, 0.126f, 0.0050f, whiteEmitter);
            drawLowerLightWindow(0.153f, 0.126f, 0.0050f, whiteEmitter);
        } else {
            const Color unlitIndicator(220, 44, 50, 64);
            drawLowerLightWindow(-0.153f, 0.126f, 0.0050f, unlitIndicator);
            drawLowerLightWindow(0.153f, 0.126f, 0.0050f, unlitIndicator);
        }
    }

    // Sticks and buttons are deformed in the cached mesh itself. Nothing is
    // painted over the physical controls, so the supplied model stays intact.
    g_gamepadYaw = savedOverlayYaw;
    g_gamepadPitch = savedOverlayPitch;
    graphics.SetSmoothingMode(oldSmoothing);
}

void drawGamepads(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    const ULONGLONG now = GetTickCount64();
    const bool liveInput = g_dualSenseLive.seen && now - g_dualSenseLive.lastInputAt < 2500;
    const int lightingCount = dualSenseDeviceCount();
    const bool lightingReady = lightingCount > 0;
    const bool connected = liveInput || lightingReady;

    drawPageIntro(graphics, x, originY, available - 170,
                  localized(L"MANETTE", L"CONTROLLER", L"CONTROLLER", L"手柄"),
                  localized(L"Ta manette, en direct.", L"Your controller, live.",
                            L"Dein Controller, live.", L"实时查看你的手柄。"));
    drawButton(graphics, RectF(width - 186.0f, originY + 9, 148, 42),
               localized(L"Rechercher", L"Scan", L"Suchen", L"扫描"), false, Action::Refresh);

    float y = originY + 78;
    RectF connection(x, y, available, 78);
    fillRound(graphics, connection, 17, Color(248, 27, 32, 45));
    strokeRound(graphics, connection, 17, connected ? Color(255, 43, 108, 84) : Color(255, 54, 61, 80));
    fillRound(graphics, RectF(connection.X + 17, connection.Y + 15, 48, 48), 15,
              connected ? Color(255, 24, 66, 54) : Color(255, 36, 41, 55));
    text(graphics, L"PS", RectF(connection.X + 17, connection.Y + 15, 48, 48), 10,
         connected ? Color(255, 101, 230, 181) : Color(255, 129, 139, 160),
         FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    const std::wstring controllerName = g_dualSenseLive.edge ? L"DualSense Edge" : L"DualSense";
    text(graphics, connected ? controllerName : localized(L"Aucune DualSense connectée", L"No DualSense connected",
                                                           L"Kein DualSense verbunden", L"未连接 DualSense"),
         RectF(connection.X + 80, connection.Y + 15, connection.Width - 410, 21), 13, Color::White, FontStyleBold);
    std::wstring transport = liveInput
        ? (g_dualSenseLive.bluetooth ? L"Bluetooth" : L"USB")
        : localized(L"Branche la manette en USB ou Bluetooth", L"Connect the controller through USB or Bluetooth",
                    L"Controller per USB oder Bluetooth verbinden", L"通过 USB 或蓝牙连接手柄");
    text(graphics, transport, RectF(connection.X + 80, connection.Y + 39, connection.Width - 410, 18), 9,
         Color(255, 137, 147, 168));

    auto statusPill = [&](const RectF& rect, bool ready, const wchar_t* readyText, const wchar_t* waitingText) {
        fillRound(graphics, rect, 10, ready ? Color(255, 17, 56, 45) : Color(255, 37, 42, 56));
        strokeRound(graphics, rect, 10, ready ? Color(255, 42, 103, 79) : Color(255, 54, 61, 79));
        fillRound(graphics, RectF(rect.X + 11, rect.Y + rect.Height / 2 - 3, 6, 6), 3,
                  ready ? Color(255, 87, 225, 170) : Color(255, 112, 122, 143));
        text(graphics, ready ? readyText : waitingText, RectF(rect.X + 23, rect.Y, rect.Width - 31, rect.Height), 8,
             ready ? Color(255, 104, 228, 181) : Color(255, 149, 158, 178), FontStyleBold,
             StringAlignmentNear, StringAlignmentCenter);
    };
    statusPill(RectF(connection.GetRight() - 326, connection.Y + 21, 150, 36), liveInput,
               localized(L"ENTRÉE EN DIRECT", L"LIVE INPUT", L"LIVE-EINGABE", L"实时输入"),
               localized(L"ENTRÉE EN ATTENTE", L"INPUT WAITING", L"EINGABE WARTET", L"等待输入"));
    statusPill(RectF(connection.GetRight() - 166, connection.Y + 21, 148, 36), lightingReady,
               localized(L"ÉCLAIRAGE PRÊT", L"LIGHTING READY", L"LICHT BEREIT", L"灯光就绪"),
               localized(L"RGB INDISPONIBLE", L"RGB UNAVAILABLE", L"RGB NICHT BEREIT", L"RGB 不可用"));

    y += 94;
    RectF liveCard(x, y, available, 430);
    fillRound(graphics, liveCard, 19, Color(248, 27, 32, 45));
    strokeRound(graphics, liveCard, 19, Color(255, 43, 49, 65));
    text(graphics, localized(L"Touches utilisées", L"Live controls", L"Live-Steuerung", L"实时按键"),
         RectF(liveCard.X + 20, liveCard.Y + 16, 260, 22), 14, Color::White, FontStyleBold);
    text(graphics, liveInput
             ? localized(L"Chaque pression apparaît instantanément.", L"Every press appears instantly.",
                         L"Jeder Tastendruck erscheint sofort.", L"每次按键都会即时显示。")
             : localized(L"Appuie sur une touche pour démarrer l'affichage.", L"Press a button to start the display.",
                         L"Drücke eine Taste, um die Anzeige zu starten.", L"按下任意按键开始显示。"),
         RectF(liveCard.X + 20, liveCard.Y + 40, 410, 18), 9, Color(255, 131, 141, 162));

    // Two aligned columns keep the controller and its telemetry balanced at
    // every supported window width. Both columns share the same top/bottom
    // edges instead of relying on unrelated magic offsets.
    const float contentX = liveCard.X + 20.0f;
    const float contentY = liveCard.Y + 62.0f;
    const float contentWidth = liveCard.Width - 40.0f;
    const float columnGap = 24.0f;
    const float infoW = std::clamp(contentWidth * 0.34f, 230.0f, 330.0f);
    const float modelWidth = std::max(330.0f, contentWidth - infoW - columnGap);
    const float modelHeight = 330.0f;
    const float infoX = contentX + modelWidth + columnGap;
    RectF infoPanel(infoX, contentY, infoW, modelHeight);
    fillRound(graphics, infoPanel, 14, Color(255, 22, 27, 39));
    strokeRound(graphics, infoPanel, 14, Color(255, 44, 51, 68));
    const std::uint32_t litRgb = gamepadLightingPreviewRgb(now);
    const Color lightColor(255, static_cast<BYTE>((litRgb >> 16) & 0xff),
                           static_cast<BYTE>((litRgb >> 8) & 0xff), static_cast<BYTE>(litRgb & 0xff));
    if (g_dualSenseMesh.loaded) {
        const RectF modelRect(contentX, contentY, modelWidth, modelHeight);
        g_gamepadModelRect = modelRect;
        drawDualSenseModel(graphics, modelRect, liveInput, lightingReady);
        text(graphics, localized(L"Glisser pour tourner · double-clic pour réinitialiser",
                                 L"Drag to rotate · double-click to reset",
                                 L"Ziehen zum Drehen · Doppelklick zum Zurücksetzen",
                                 L"拖动旋转 · 双击重置视图"),
             RectF(modelRect.X, modelRect.GetBottom() + 5, modelRect.Width, 18), 8,
             Color(255, 123, 135, 158), FontStyleRegular, StringAlignmentCenter);
    } else {
    if (!g_dualSenseImage || g_dualSenseImage->GetLastStatus() != Ok) {
    const float padX = liveCard.X + 32;
    const float padY = liveCard.Y + 72;
    const float padW = std::min(500.0f, liveCard.Width * 0.59f);
    const float padH = 205.0f;
    GraphicsPath body;
    body.StartFigure();
    body.AddBezier(PointF(padX + 95, padY + 16), PointF(padX + 45, padY + 12),
                   PointF(padX + 24, padY + 53), PointF(padX + 14, padY + 142));
    body.AddBezier(PointF(padX + 14, padY + 142), PointF(padX + 9, padY + 192),
                   PointF(padX + 54, padY + 208), PointF(padX + 91, padY + 158));
    body.AddBezier(PointF(padX + 91, padY + 158), PointF(padX + 121, padY + 127),
                   PointF(padX + padW - 121, padY + 127), PointF(padX + padW - 91, padY + 158));
    body.AddBezier(PointF(padX + padW - 91, padY + 158), PointF(padX + padW - 54, padY + 208),
                   PointF(padX + padW - 9, padY + 192), PointF(padX + padW - 14, padY + 142));
    body.AddBezier(PointF(padX + padW - 14, padY + 142), PointF(padX + padW - 24, padY + 53),
                   PointF(padX + padW - 45, padY + 12), PointF(padX + padW - 95, padY + 16));
    body.AddBezier(PointF(padX + padW - 95, padY + 16), PointF(padX + padW * 0.64f, padY + 25),
                   PointF(padX + padW * 0.36f, padY + 25), PointF(padX + 95, padY + 16));
    body.CloseFigure();
    // A recognisable DualSense shell: pearl body, dark central spine and
    // controller-shaped handles. Live highlights are layered on top.
    LinearGradientBrush bodyBrush(PointF(padX, padY), PointF(padX + padW, padY + padH),
                                  Color(255, 242, 244, 249), Color(255, 159, 166, 181));
    graphics.FillPath(&bodyBrush, &body);
    Pen bodyBorder(liveInput ? accentColor(175) : Color(255, 105, 113, 131), 1.5f);
    graphics.DrawPath(&bodyBorder, &body);

    GraphicsPath centerPanel;
    centerPanel.StartFigure();
    centerPanel.AddBezier(PointF(padX + padW / 2 - 79, padY + 61), PointF(padX + padW / 2 - 77, padY + 105),
                          PointF(padX + padW / 2 - 64, padY + 163), PointF(padX + padW / 2 - 34, padY + 188));
    centerPanel.AddBezier(PointF(padX + padW / 2 - 34, padY + 188), PointF(padX + padW / 2 - 12, padY + 197),
                          PointF(padX + padW / 2 + 12, padY + 197), PointF(padX + padW / 2 + 34, padY + 188));
    centerPanel.AddBezier(PointF(padX + padW / 2 + 34, padY + 188), PointF(padX + padW / 2 + 64, padY + 163),
                          PointF(padX + padW / 2 + 77, padY + 105), PointF(padX + padW / 2 + 79, padY + 61));
    centerPanel.CloseFigure();
    SolidBrush centerBrush(Color(255, 17, 20, 29));
    graphics.FillPath(&centerBrush, &centerPanel);

    RectF leftShoulder(padX + 55, padY + 10, 79, 17);
    RectF rightShoulder(padX + padW - 134, padY + 10, 79, 17);
    fillRound(graphics, leftShoulder, 8, g_dualSenseLive.buttons[4] && liveInput ? accentColor() : Color(255, 41, 46, 59));
    fillRound(graphics, rightShoulder, 8, g_dualSenseLive.buttons[5] && liveInput ? accentColor() : Color(255, 41, 46, 59));
    text(graphics, L"L1", leftShoulder, 7, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    text(graphics, L"R1", rightShoulder, 7, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);

    const std::uint32_t litRgb = gamepadLightingPreviewRgb(now);
    Color lightColor(255, static_cast<BYTE>((litRgb >> 16) & 0xff), static_cast<BYTE>((litRgb >> 8) & 0xff), static_cast<BYTE>(litRgb & 0xff));
    Pen lightbar(lightingReady ? lightColor : Color(255, 67, 76, 98), 4.0f);
    graphics.DrawArc(&lightbar, RectF(padX + padW / 2 - 83, padY + 25, 166, 94), 197, 146);

    RectF touchpad(padX + padW / 2 - 67, padY + 31, 134, 66);
    fillRound(graphics, touchpad, 12, g_dualSenseLive.buttons[13] && liveInput ? accentColor(115) : Color(255, 24, 28, 38));
    strokeRound(graphics, touchpad, 11, g_dualSenseLive.buttons[13] && liveInput ? accentTint(0.45) : Color(255, 65, 73, 93));
    text(graphics, L"TOUCHPAD", touchpad, 7, Color(255, 111, 122, 145), FontStyleBold,
         StringAlignmentCenter, StringAlignmentCenter);
    for (int indicator = 0; indicator < 5; ++indicator) {
        const float indicatorX = padX + padW / 2 - 20.0f + indicator * 10.0f;
        SolidBrush indicatorGlow(g_dualSensePlayerLedsEnabled && lightingReady ? Color(70, 220, 230, 255) : Color(0, 0, 0, 0));
        SolidBrush indicatorFill(g_dualSensePlayerLedsEnabled && lightingReady ? Color(255, 231, 237, 255)
                                                                              : Color(255, 63, 70, 88));
        if (g_dualSensePlayerLedsEnabled && lightingReady) {
            graphics.FillEllipse(&indicatorGlow, RectF(indicatorX - 3.5f, padY + 102.5f, 7, 7));
        }
        graphics.FillEllipse(&indicatorFill, RectF(indicatorX - 2, padY + 104, 4, 4));
    }
    fillRound(graphics, RectF(padX + padW / 2 - 91, padY + 58, 11, 4), 2,
              g_dualSenseLive.buttons[8] && liveInput ? accentColor() : Color(255, 64, 70, 84));
    fillRound(graphics, RectF(padX + padW / 2 + 80, padY + 58, 11, 4), 2,
              g_dualSenseLive.buttons[9] && liveInput ? accentColor() : Color(255, 64, 70, 84));

    auto controlButton = [&](float cx, float cy, float diameter, bool active, const std::wstring& label, float fontSize = 11.0f) {
        SolidBrush glow(active && liveInput ? accentColor(55) : Color(0, 0, 0, 0));
        if (active && liveInput) graphics.FillEllipse(&glow, RectF(cx - diameter * 0.72f, cy - diameter * 0.72f, diameter * 1.44f, diameter * 1.44f));
        SolidBrush fill(active && liveInput ? accentColor() : Color(255, 31, 36, 49));
        graphics.FillEllipse(&fill, RectF(cx - diameter / 2, cy - diameter / 2, diameter, diameter));
        Pen outline(active && liveInput ? accentTint(0.60) : Color(255, 78, 87, 108), 1.2f);
        graphics.DrawEllipse(&outline, RectF(cx - diameter / 2, cy - diameter / 2, diameter, diameter));
        text(graphics, label, RectF(cx - diameter / 2, cy - diameter / 2, diameter, diameter), fontSize,
             active && liveInput ? accentButtonText() : Color(255, 206, 212, 226), FontStyleBold,
             StringAlignmentCenter, StringAlignmentCenter);
    };
    const float dpadX = padX + 101, dpadY = padY + 100;
    const int dp = liveInput ? g_dualSenseLive.dpad : 8;
    controlButton(dpadX, dpadY - 23, 25, dp == 0 || dp == 1 || dp == 7, L"↑", 13);
    controlButton(dpadX + 23, dpadY, 25, dp == 1 || dp == 2 || dp == 3, L"→", 13);
    controlButton(dpadX, dpadY + 23, 25, dp == 3 || dp == 4 || dp == 5, L"↓", 13);
    controlButton(dpadX - 23, dpadY, 25, dp == 5 || dp == 6 || dp == 7, L"←", 13);
    const float faceX = padX + padW - 101, faceY = padY + 100;
    controlButton(faceX, faceY - 25, 26, g_dualSenseLive.buttons[3], L"△", 12);
    controlButton(faceX + 25, faceY, 26, g_dualSenseLive.buttons[2], L"○", 12);
    controlButton(faceX, faceY + 25, 26, g_dualSenseLive.buttons[1], L"×", 12);
    controlButton(faceX - 25, faceY, 26, g_dualSenseLive.buttons[0], L"□", 12);

    auto stick = [&](float cx, float cy, std::uint8_t rawX, std::uint8_t rawY, bool pressed) {
        SolidBrush well(Color(255, 15, 18, 27));
        graphics.FillEllipse(&well, RectF(cx - 23, cy - 23, 46, 46));
        Pen rim(pressed && liveInput ? accentColor() : Color(255, 66, 74, 95), 1.5f);
        graphics.DrawEllipse(&rim, RectF(cx - 23, cy - 23, 46, 46));
        const float ox = liveInput ? (static_cast<int>(rawX) - 128) / 127.0f * 9.0f : 0.0f;
        const float oy = liveInput ? (static_cast<int>(rawY) - 128) / 127.0f * 9.0f : 0.0f;
        SolidBrush knob(pressed && liveInput ? accentColor() : Color(255, 70, 78, 98));
        graphics.FillEllipse(&knob, RectF(cx + ox - 13, cy + oy - 13, 26, 26));
    };
    stick(padX + padW / 2 - 55, padY + 145, g_dualSenseLive.leftX, g_dualSenseLive.leftY, g_dualSenseLive.buttons[10]);
    stick(padX + padW / 2 + 55, padY + 145, g_dualSenseLive.rightX, g_dualSenseLive.rightY, g_dualSenseLive.buttons[11]);
    controlButton(padX + padW / 2, padY + 128, 24, g_dualSenseLive.buttons[12], L"PS", 7);

    }
    const float padX = liveCard.X + 20;
    const float padY = liveCard.Y + 62;
    const float padW = std::min(520.0f, liveCard.Width * 0.61f);
    const int dp = liveInput ? g_dualSenseLive.dpad : 8;
    const std::uint32_t litRgb = gamepadLightingPreviewRgb(now);
    Color lightColor(255, static_cast<BYTE>((litRgb >> 16) & 0xff), static_cast<BYTE>((litRgb >> 8) & 0xff), static_cast<BYTE>(litRgb & 0xff));
    // Paint the real DualSense product silhouette over the legacy vector
    // fallback, then add only live interaction and lighting layers. Keeping the
    // photograph separate from the overlays makes every RGB color accurate.
    constexpr float dualSenseSourceWidth = 1360.0f;
    constexpr float dualSenseSourceTop = 145.0f;
    constexpr float dualSenseSourceHeight = 930.0f;
    const float photoH = padW * dualSenseSourceHeight / dualSenseSourceWidth;
    auto photoPoint = [&](float imageX, float imageY) {
        return PointF(padX + imageX / dualSenseSourceWidth * padW,
                      padY + (imageY - dualSenseSourceTop) / dualSenseSourceHeight * photoH);
    };
    if (g_dualSensePreview && g_dualSensePreview->GetLastStatus() == Ok) {
        SolidBrush photoBackdrop(Color(255, 27, 32, 45));
        graphics.FillRectangle(&photoBackdrop, RectF(padX - 6, padY - 10, padW + 12, photoH + 20));
        const InterpolationMode oldInterpolation = graphics.GetInterpolationMode();
        const PixelOffsetMode oldPixelOffset = graphics.GetPixelOffsetMode();
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        graphics.DrawImage(g_dualSensePreview.get(), RectF(padX, padY, padW, photoH),
                           0.0f, 0.0f, 520.0f, 356.0f, UnitPixel);
        graphics.SetInterpolationMode(oldInterpolation);
        graphics.SetPixelOffsetMode(oldPixelOffset);
    }

    auto drawLightStrip = [&](bool left, const Pen& pen) {
        const PointF p0 = photoPoint(left ? 407.0f : 953.0f, 216.0f);
        const PointF p1 = photoPoint(left ? 418.0f : 942.0f, 280.0f);
        const PointF p2 = photoPoint(left ? 433.0f : 927.0f, 398.0f);
        const PointF p3 = photoPoint(left ? 466.0f : 894.0f, 457.0f);
        GraphicsPath strip;
        strip.StartFigure();
        strip.AddBezier(p0, p1, p2, p3);
        graphics.DrawPath(&pen, &strip);
    };
    const Color previewColor = lightingReady ? lightColor : Color(255, 67, 76, 98);
    Pen photoLightGlow(Color(88, previewColor.GetR(), previewColor.GetG(), previewColor.GetB()), 12.0f);
    photoLightGlow.SetStartCap(LineCapRound);
    photoLightGlow.SetEndCap(LineCapRound);
    Pen photoLightCore(previewColor, 4.8f);
    photoLightCore.SetStartCap(LineCapRound);
    photoLightCore.SetEndCap(LineCapRound);
    drawLightStrip(true, photoLightGlow);
    drawLightStrip(false, photoLightGlow);
    drawLightStrip(true, photoLightCore);
    drawLightStrip(false, photoLightCore);

    auto photoGlow = [&](float imageX, float imageY, float imageRadius, bool active) {
        if (!active || !liveInput) return;
        const PointF center = photoPoint(imageX, imageY);
        const float radius = imageRadius / dualSenseSourceWidth * padW;
        SolidBrush glow(accentColor(58));
        Pen rim(accentColor(), 2.1f);
        graphics.FillEllipse(&glow, RectF(center.X - radius, center.Y - radius, radius * 2, radius * 2));
        graphics.DrawEllipse(&rim, RectF(center.X - radius * 0.78f, center.Y - radius * 0.78f,
                                         radius * 1.56f, radius * 1.56f));
    };
    photoGlow(267, 337, 45, dp == 0 || dp == 1 || dp == 7);
    photoGlow(350, 420, 45, dp == 1 || dp == 2 || dp == 3);
    photoGlow(267, 502, 45, dp == 3 || dp == 4 || dp == 5);
    photoGlow(184, 420, 45, dp == 5 || dp == 6 || dp == 7);
    photoGlow(1092, 320, 42, g_dualSenseLive.buttons[3]);
    photoGlow(1185, 418, 42, g_dualSenseLive.buttons[2]);
    photoGlow(1092, 510, 42, g_dualSenseLive.buttons[1]);
    photoGlow(995, 418, 42, g_dualSenseLive.buttons[0]);
    photoGlow(368, 269, 34, g_dualSenseLive.buttons[8]);
    photoGlow(992, 269, 34, g_dualSenseLive.buttons[9]);
    photoGlow(680, 585, 38, g_dualSenseLive.buttons[12]);

    if (g_dualSenseLive.buttons[13] && liveInput) {
        const PointF touchTopLeft = photoPoint(428, 183);
        const PointF touchBottomRight = photoPoint(932, 460);
        RectF touchRect(touchTopLeft.X, touchTopLeft.Y,
                        touchBottomRight.X - touchTopLeft.X, touchBottomRight.Y - touchTopLeft.Y);
        fillRound(graphics, touchRect, 18, accentColor(30));
        strokeRound(graphics, touchRect, 18, accentColor(210), 2.0f);
    }

    auto photoStick = [&](float imageX, float imageY, std::uint8_t rawX, std::uint8_t rawY, bool pressed) {
        const PointF center = photoPoint(imageX, imageY);
        if (pressed && liveInput) {
            SolidBrush glow(accentColor(52));
            graphics.FillEllipse(&glow, RectF(center.X - 27, center.Y - 27, 54, 54));
        }
        if (!liveInput || (std::abs(static_cast<int>(rawX) - 128) <= 12 &&
                           std::abs(static_cast<int>(rawY) - 128) <= 12)) return;
        const float ox = (static_cast<int>(rawX) - 128) / 127.0f * 9.0f;
        const float oy = (static_cast<int>(rawY) - 128) / 127.0f * 9.0f;
        Pen direction(accentColor(), 3.0f);
        direction.SetEndCap(LineCapRound);
        graphics.DrawLine(&direction, center, PointF(center.X + ox, center.Y + oy));
    };
    photoStick(466, 603, g_dualSenseLive.leftX, g_dualSenseLive.leftY, g_dualSenseLive.buttons[10]);
    photoStick(894, 603, g_dualSenseLive.rightX, g_dualSenseLive.rightY, g_dualSenseLive.buttons[11]);

    for (int indicator = 0; indicator < 5; ++indicator) {
        const PointF led = photoPoint(638.0f + indicator * 21.0f, 493.0f);
        const bool enabled = g_dualSensePlayerLedsEnabled && lightingReady;
        if (enabled) {
            SolidBrush ledGlow(Color(78, 215, 229, 255));
            graphics.FillEllipse(&ledGlow, RectF(led.X - 5, led.Y - 5, 10, 10));
        }
        SolidBrush ledFill(enabled ? Color(255, 238, 243, 255) : Color(255, 45, 51, 65));
        graphics.FillEllipse(&ledFill, RectF(led.X - 2.1f, led.Y - 2.1f, 4.2f, 4.2f));
    }

    }
    g_gamepadModelRect = RectF(contentX, contentY, modelWidth, modelHeight);
    const float telemetryX = infoPanel.X + 16.0f;
    const float telemetryW = infoPanel.Width - 32.0f;
    text(graphics, localized(L"Enfoncement des gâchettes", L"Trigger pressure", L"Trigger-Druck", L"扳机压力"),
         RectF(telemetryX, infoPanel.Y + 14, telemetryW, 18), 9, Color(255, 137, 147, 168), FontStyleBold);
    auto triggerMeter = [&](float meterY, const wchar_t* label, std::uint8_t amount, bool active) {
        text(graphics, label, RectF(telemetryX, meterY, 30, 18), 9,
             active && liveInput ? accentTint(0.50) : Color(255, 183, 190, 207), FontStyleBold);
        RectF track(telemetryX + 34, meterY + 5, std::max(30.0f, telemetryW - 74), 8);
        fillRound(graphics, track, 4, Color(255, 13, 16, 24));
        const float ratio = liveInput ? amount / 255.0f : 0.0f;
        if (ratio > 0.01f) fillRound(graphics, RectF(track.X, track.Y, track.Width * ratio, track.Height), 4, accentColor());
        text(graphics, std::to_wstring(static_cast<int>(std::lround(ratio * 100))) + L" %",
             RectF(track.GetRight() + 5, meterY, 38, 18), 8, Color(255, 157, 166, 186), FontStyleBold,
             StringAlignmentFar);
    };
    triggerMeter(infoPanel.Y + 40, L"L2", g_dualSenseLive.leftTrigger, g_dualSenseLive.buttons[6]);
    triggerMeter(infoPanel.Y + 68, L"R2", g_dualSenseLive.rightTrigger, g_dualSenseLive.buttons[7]);

    static const wchar_t* buttonNames[] = {L"Carré", L"Croix", L"Rond", L"Triangle", L"L1", L"R1", L"L2", L"R2",
                                           L"Créer", L"Options", L"L3", L"R3", L"PS", L"Pavé tactile", L"Micro"};
    std::wstring pressed;
    auto appendPressed = [&](const std::wstring& name) {
        if (!pressed.empty()) pressed += L"  ·  ";
        pressed += name;
    };
    if (liveInput) {
        for (int index = 0; index < 15; ++index) {
            if (!g_dualSenseLive.buttons[index]) continue;
            appendPressed(buttonNames[index]);
        }
        const DualSenseDpadDirections directions = dualSenseDpadDirections(g_dualSenseLive.dpad);
        if (directions.up) appendPressed(localized(L"Flèche haut", L"D-pad up", L"Steuerkreuz oben", L"方向键上"));
        if (directions.right) appendPressed(localized(L"Flèche droite", L"D-pad right", L"Steuerkreuz rechts", L"方向键右"));
        if (directions.down) appendPressed(localized(L"Flèche bas", L"D-pad down", L"Steuerkreuz unten", L"方向键下"));
        if (directions.left) appendPressed(localized(L"Flèche gauche", L"D-pad left", L"Steuerkreuz links", L"方向键左"));
    }
    if (pressed.empty()) pressed = localized(L"Aucune touche enfoncée", L"No button pressed",
                                             L"Keine Taste gedrückt", L"未按下任何按键");
    text(graphics, localized(L"Touches actives", L"Active buttons", L"Aktive Tasten", L"当前按键"),
         RectF(telemetryX, infoPanel.Y + 113, telemetryW, 18), 9, Color(255, 137, 147, 168), FontStyleBold);
    RectF pressedBox(telemetryX, infoPanel.Y + 136, telemetryW, 82);
    fillRound(graphics, pressedBox, 11, Color(255, 19, 23, 33));
    strokeRound(graphics, pressedBox, 11, Color(255, 45, 52, 69));
    textWrapped(graphics, pressed, RectF(pressedBox.X + 12, pressedBox.Y + 10, pressedBox.Width - 24, pressedBox.Height - 20),
                9, liveInput ? Color(255, 224, 228, 238) : Color(255, 125, 135, 156));

    y += 446;
    RectF lighting(x, y, available, 154);
    fillRound(graphics, lighting, 19, Color(248, 27, 32, 45));
    strokeRound(graphics, lighting, 19, Color(255, 43, 49, 65));
    text(graphics, localized(L"Éclairage DualSense", L"DualSense lighting", L"DualSense-Beleuchtung", L"DualSense 灯光"),
         RectF(lighting.X + 20, lighting.Y + 16, 260, 22), 14, Color::White, FontStyleBold);
    text(graphics, localized(L"Les bandes latérales utilisent la couleur RGB. La lumière blanche sous le pavé reste indépendante.",
                             L"The side bars use the RGB color. The white light under the touchpad remains independent.",
                             L"Die Seitenleisten verwenden die RGB-Farbe. Das weiße Licht unter dem Touchpad bleibt unabhängig.",
                             L"侧灯带使用 RGB 颜色。触控板下方的白光保持独立。"),
         RectF(lighting.X + 20, lighting.Y + 42, lighting.Width - 40, 18), 9, Color(255, 129, 140, 161));
    RectF swatch(lighting.X + 20, lighting.Y + 78, 56, 56);
    fillRound(graphics, swatch, 15, lightColor);
    strokeRound(graphics, swatch, 15, accentTint(0.42), 1.4f);
    drawButton(graphics, RectF(lighting.X + 88, lighting.Y + 86, 166, 42),
               localized(L"Choisir la couleur", L"Choose color", L"Farbe wählen", L"选择颜色"),
               false, Action::PickColor);

    RectF ledToggle(lighting.X + 274, lighting.Y + 78, 235, 56);
    fillRound(graphics, ledToggle, 14, g_dualSensePlayerLedsEnabled ? Color(255, 24, 62, 51) : Color(255, 31, 36, 49));
    strokeRound(graphics, ledToggle, 14, g_dualSensePlayerLedsEnabled ? Color(255, 45, 112, 86) : Color(255, 55, 63, 82));
    text(graphics, localized(L"Lumière blanche", L"White light", L"Weißes Licht", L"白色灯光"),
         RectF(ledToggle.X + 15, ledToggle.Y + 8, 144, 18), 9, Color(255, 225, 229, 239), FontStyleBold);
    text(graphics, g_dualSensePlayerLedsEnabled ? localized(L"Activés", L"Enabled", L"Aktiviert", L"已开启")
                                                : localized(L"Désactivés", L"Disabled", L"Deaktiviert", L"已关闭"),
         RectF(ledToggle.X + 15, ledToggle.Y + 29, 144, 16), 8,
         g_dualSensePlayerLedsEnabled ? Color(255, 104, 225, 179) : Color(255, 145, 154, 175), FontStyleBold);
    RectF switchTrack(ledToggle.GetRight() - 59, ledToggle.Y + 17, 43, 22);
    fillRound(graphics, switchTrack, 11, g_dualSensePlayerLedsEnabled ? accentColor() : Color(255, 61, 69, 88));
    SolidBrush switchKnob(Color::White);
    graphics.FillEllipse(&switchKnob, RectF(switchTrack.X + (g_dualSensePlayerLedsEnabled ? 23 : 3), switchTrack.Y + 3, 16, 16));
    addHit(ledToggle, Action::GamepadTogglePlayerLeds);

    drawButton(graphics, RectF(lighting.GetRight() - 204, lighting.Y + 86, 184, 42),
               localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true,
               Action::GamepadApplyColor, -1, lightingReady);

    y = lighting.GetBottom() + 16;
    RectF compatibility(x, y, available, 168);
    fillRound(graphics, compatibility, 19, Color(248, 27, 32, 45));
    strokeRound(graphics, compatibility, 19,
                g_xboxBridgeStatus == XboxBridgeStatus::Ready ? Color(255, 42, 105, 82) : Color(255, 43, 49, 65));
    text(graphics, localized(L"Compatibilité avec les jeux", L"Game compatibility", L"Spielekompatibilität", L"游戏兼容性"),
         RectF(compatibility.X + 20, compatibility.Y + 16, 320, 22), 14, Color::White, FontStyleBold);
    text(graphics, localized(L"Choisis le comportement de la DualSense. Le mode Xbox ajoute XInput pour les jeux qui ne comprennent pas les manettes PlayStation.",
                             L"Choose how the DualSense behaves. Xbox mode adds XInput for games that do not understand PlayStation controllers.",
                             L"Wähle das Verhalten der DualSense. Der Xbox-Modus ergänzt XInput für Spiele ohne PlayStation-Controller-Unterstützung.",
                             L"选择 DualSense 的工作方式。Xbox 模式为不支持 PlayStation 手柄的游戏添加 XInput。"),
         RectF(compatibility.X + 20, compatibility.Y + 42, compatibility.Width - 40, 22), 9, Color(255, 129, 140, 161));

    auto modeChoice = [&](const RectF& rect, bool selected, const wchar_t* icon,
                          const wchar_t* title, const wchar_t* detail, Action action) {
        fillRound(graphics, rect, 14, selected ? Color(255, 44, 36, 78) : Color(255, 31, 36, 49));
        strokeRound(graphics, rect, 14, selected ? accentColor() : Color(255, 54, 62, 81), selected ? 1.8f : 1.0f);
        fillRound(graphics, RectF(rect.X + 11, rect.Y + 11, 34, 34), 10,
                  selected ? accentColor(75) : Color(255, 42, 48, 63));
        text(graphics, icon, RectF(rect.X + 11, rect.Y + 11, 34, 34), 9,
             selected ? Color::White : Color(255, 160, 170, 191), FontStyleBold,
             StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, title, RectF(rect.X + 56, rect.Y + 8, rect.Width - 66, 19), 10,
             Color::White, FontStyleBold);
        text(graphics, detail, RectF(rect.X + 56, rect.Y + 29, rect.Width - 66, 16), 7.5f,
             selected ? accentTint(0.52) : Color(255, 128, 138, 158));
        addHit(rect, action);
    };
    const float choicesWidth = std::min(420.0f, compatibility.Width * 0.57f);
    const float choiceGap = 10.0f;
    const float choiceWidth = (choicesWidth - choiceGap) / 2.0f;
    RectF nativeChoice(compatibility.X + 20, compatibility.Y + 84, choiceWidth, 58);
    RectF xboxChoice(nativeChoice.GetRight() + choiceGap, nativeChoice.Y, choiceWidth, 58);
    modeChoice(nativeChoice, !g_xboxModeEnabled, L"PS",
               localized(L"DualSense native", L"Native DualSense", L"Native DualSense", L"原生 DualSense"),
               localized(L"Fonctions PlayStation", L"PlayStation features", L"PlayStation-Funktionen", L"PlayStation 功能"),
               Action::GamepadUseNative);
    modeChoice(xboxChoice, g_xboxModeEnabled, L"X",
               localized(L"Manette Xbox 360", L"Xbox 360 controller", L"Xbox-360-Controller", L"Xbox 360 手柄"),
               localized(L"Compatibilité XInput", L"XInput compatibility", L"XInput-Kompatibilität", L"XInput 兼容"),
               Action::GamepadUseXbox);

    RectF bridgeStatus(compatibility.X + 20 + choicesWidth + 18, compatibility.Y + 84,
                       compatibility.Width - choicesWidth - 58, 58);
    const bool bridgeReady = g_xboxBridgeStatus == XboxBridgeStatus::Ready;
    const bool bridgeStarting = g_xboxBridgeStatus == XboxBridgeStatus::Starting;
    const bool bridgeError = g_xboxBridgeStatus == XboxBridgeStatus::Error;
    fillRound(graphics, bridgeStatus, 14, bridgeReady ? Color(255, 19, 58, 47)
                                      : bridgeError ? Color(255, 61, 31, 40) : Color(255, 31, 36, 49));
    strokeRound(graphics, bridgeStatus, 14, bridgeReady ? Color(255, 44, 111, 85)
                                        : bridgeError ? Color(255, 123, 55, 68) : Color(255, 54, 62, 81));
    const std::wstring bridgeTitle = bridgeReady
        ? localized(L"XINPUT PRÊT", L"XINPUT READY", L"XINPUT BEREIT", L"XINPUT 已就绪")
        : bridgeStarting
            ? localized(L"ACTIVATION…", L"STARTING…", L"AKTIVIERUNG…", L"正在激活…")
            : bridgeError
                ? localized(L"ACTION REQUISE", L"ACTION REQUIRED", L"AKTION ERFORDERLICH", L"需要操作")
                : localized(L"MODE NATIF", L"NATIVE MODE", L"NATIVER MODUS", L"原生模式");
    const std::wstring bridgeDetail = bridgeError && !g_xboxBridgeMessage.empty()
        ? g_xboxBridgeMessage
        : bridgeReady
            ? localized(L"Une manette Xbox virtuelle est visible dans les jeux.", L"A virtual Xbox controller is visible to games.",
                        L"Ein virtueller Xbox-Controller ist in Spielen sichtbar.", L"游戏现在可以看到虚拟 Xbox 手柄。")
            : localized(L"Touchpad, gyroscope et gâchettes adaptatives restent natifs.",
                        L"Touchpad, gyro and adaptive triggers remain native.",
                        L"Touchpad, Gyro und adaptive Trigger bleiben nativ.",
                        L"触控板、陀螺仪和自适应扳机仍保持原生。" );
    text(graphics, bridgeTitle, RectF(bridgeStatus.X + 13, bridgeStatus.Y + 8, bridgeStatus.Width - 26, 17), 8,
         bridgeReady ? Color(255, 101, 229, 180) : bridgeError ? Color(255, 255, 139, 154) : Color(255, 181, 190, 208),
         FontStyleBold);
    textWrapped(graphics, bridgeDetail, RectF(bridgeStatus.X + 13, bridgeStatus.Y + 27, bridgeStatus.Width - 26, 25),
                7.2f, Color(255, 139, 149, 170));

    g_maxScroll = std::max(0.0f, compatibility.GetBottom() + g_scrollOffset + 18 - height);
}

void drawGamepadFastOverlay(Graphics& graphics, int width, int height) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    const float originY = static_cast<float>(kHeaderHeight + 24) - g_scrollOffset;
    const float liveCardY = originY + 78.0f + 94.0f;
    const float contentX = x + 20.0f;
    const float contentY = liveCardY + 62.0f;
    const float contentWidth = available - 40.0f;
    const float columnGap = 24.0f;
    const float infoW = std::clamp(contentWidth * 0.34f, 230.0f, 330.0f);
    const float modelWidth = std::max(330.0f, contentWidth - infoW - columnGap);
    const RectF modelRect(contentX, contentY, modelWidth, 330.0f);
    const ULONGLONG now = GetTickCount64();
    const bool liveInput = g_dualSenseLive.seen && now - g_dualSenseLive.lastInputAt < 2500;
    const bool lightingReady = dualSenseDeviceCount() > 0;
    const GraphicsState state = graphics.Save();
    graphics.SetClip(RectF(static_cast<REAL>(kSidebarWidth), static_cast<REAL>(kHeaderHeight),
                           static_cast<REAL>(std::max(0, width - kSidebarWidth)),
                           static_cast<REAL>(std::max(0, height - kHeaderHeight))), CombineModeReplace);
    drawDualSenseModel(graphics, modelRect, liveInput, lightingReady);
    graphics.Restore(state);
    g_gamepadModelRect = modelRect;
}

void drawCompatibility(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, available - 150,
                  localized(L"COMPATIBILITÉ", L"COMPATIBILITY", L"KOMPATIBILITÄT", L"兼容性"),
                  localized(L"Teste ton PC, étape par étape.", L"Test your PC, step by step.",
                            L"Teste deinen PC Schritt für Schritt.", L"逐步测试你的电脑。"));
    drawButton(graphics, RectF(width - 170.0f, originY + 8, 132, 42),
               localized(L"Retour", L"Back", L"Zurück", L"返回"), false, Action::CompatibilityBack, -1,
               g_certificationDevice < 0 && !g_certificationApplying && !g_certificationAwaitingAnswer);
    text(graphics, localized(L"L'analyse est en lecture seule. L'auto-calibration n'écrit sur un appareil qu'après ton clic.",
                             L"The analysis is read-only. Auto-calibration writes to a device only after your click.",
                             L"Die Analyse ist schreibgeschützt. Die Autokalibrierung schreibt erst nach deinem Klick.",
                             L"分析仅进行读取。自动校准只会在你点击后写入设备。"),
         RectF(x, originY + 62, available, 20), 10, Color(255, 136, 146, 167));

    float y = originY + 98;
    RectF progressCard(x, y, available, 94);
    fillRound(graphics, progressCard, 17, Color(255, 28, 32, 45));
    strokeRound(graphics, progressCard, 17, g_compatibilityFinished ? Color(255, 42, 99, 79) : Color(255, 43, 49, 65));
    const std::wstring progressTitle = g_compatibilityRunning
        ? localized(L"Analyse en cours…", L"Analysis in progress…", L"Analyse läuft…", L"正在分析…")
        : g_compatibilityFinished
            ? localized(L"Analyse terminée", L"Analysis complete", L"Analyse abgeschlossen", L"分析完成")
            : localized(L"Prêt pour le contrôle complet", L"Ready for the full check", L"Bereit für die vollständige Prüfung", L"已准备进行完整检查");
    text(graphics, progressTitle, RectF(progressCard.X + 20, progressCard.Y + 15, 360, 20), 13, Color::White, FontStyleBold);
    text(graphics, std::to_wstring(g_compatibilityProgress.load()) + L" %",
         RectF(progressCard.GetRight() - 90, progressCard.Y + 15, 70, 20), 10, accentTint(0.48), FontStyleBold, StringAlignmentFar);
    RectF progressTrack(progressCard.X + 20, progressCard.Y + 48, progressCard.Width - 220, 10);
    fillRound(graphics, progressTrack, 5, Color(255, 14, 18, 27));
    const float progressWidth = progressTrack.Width * g_compatibilityProgress.load() / 100.0f;
    if (progressWidth > 1) fillRound(graphics, RectF(progressTrack.X, progressTrack.Y, progressWidth, progressTrack.Height), 5, accentColor());
    drawButton(graphics, RectF(progressCard.GetRight() - 180, progressCard.Y + 42, 160, 38),
               g_compatibilityRunning ? localized(L"Test en cours", L"Testing", L"Test läuft", L"测试中")
                                      : g_compatibilityFinished ? localized(L"Relancer le test", L"Run again", L"Erneut testen", L"重新测试")
                                                                : localized(L"Lancer le test", L"Start test", L"Test starten", L"开始测试"),
               true, Action::CompatibilityRun, -1, !g_compatibilityRunning);

    y += 112;
    const float gap = 12;
    const float cardWidth = (available - gap) / 2.0f;
    struct CheckView { const wchar_t* code; const wchar_t* title; const wchar_t* detail; bool passed; int step; } checks[] = {
        {L"RGB", localized(L"Éclairage OpenRGB", L"OpenRGB lighting", L"OpenRGB-Beleuchtung", L"OpenRGB 灯光"),
         localized(L"Moteur et inventaire · écriture à confirmer", L"Engine and inventory · writing to confirm", L"Engine und Inventar · Schreiben noch bestätigen", L"引擎与设备清单 · 写入待确认"), g_compatOpenRgb.load(), 1},
        {L"AU", localized(L"Audio Windows", L"Windows audio", L"Windows-Audio", L"Windows 音频"),
         localized(L"Musique réelle sans microphone", L"Real music without a microphone", L"Echte Musik ohne Mikrofon", L"无需麦克风的音乐模式"), g_compatAudio.load(), 2},
        {L"AM", localized(L"Capture Ambilight", L"Ambilight capture", L"Ambilight-Aufnahme", L"屏幕氛围灯捕获"),
         localized(L"Écrans et couleurs par zones", L"Displays and colors by zone", L"Bildschirme und Zonenfarben", L"显示器和分区颜色"), g_compatScreen.load(), 3},
        {L"FN", localized(L"Ventilation et capteurs", L"Cooling and sensors", L"Kühlung und Sensoren", L"风扇和传感器"),
         localized(L"Pont matériel et contrôle protégé", L"Hardware bridge and protected control", L"Hardware-Brücke und geschützte Steuerung", L"硬件桥接和保护控制"), g_compatHardware.load(), 4}
    };
    for (int index = 0; index < 4; ++index) {
        const int row = index / 2, column = index % 2;
        const CheckView& check = checks[index];
        RectF card(x + column * (cardWidth + gap), y + row * 108, cardWidth, 96);
        const bool reached = g_compatibilityFinished || g_compatibilityStep.load() > check.step;
        const bool active = g_compatibilityRunning && g_compatibilityStep.load() == check.step;
        fillRound(graphics, card, 16, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 16, reached ? (check.passed ? Color(255, 42, 105, 82) : Color(255, 112, 78, 44))
                                                   : active ? accentColor() : Color(255, 43, 49, 65));
        fillRound(graphics, RectF(card.X + 15, card.Y + 16, 42, 42), 12,
                  reached ? (check.passed ? Color(255, 18, 57, 46) : Color(255, 63, 44, 27)) : Color(255, 34, 39, 54));
        text(graphics, check.code, RectF(card.X + 15, card.Y + 16, 42, 42), 9,
             reached ? (check.passed ? Color(255, 103, 226, 179) : Color(255, 245, 184, 98)) : Color(255, 150, 159, 179),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, check.title, RectF(card.X + 70, card.Y + 13, card.Width - 180, 20), 12, Color::White, FontStyleBold);
        text(graphics, check.detail, RectF(card.X + 70, card.Y + 38, card.Width - 85, 17), 8, Color(255, 130, 140, 161));
        std::wstring result = active ? localized(L"ANALYSE…", L"TESTING…", L"ANALYSE…", L"分析中…")
            : reached ? (check.passed ? (index == 0
                                          ? localized(L"DÉTECTÉ", L"DETECTED", L"ERKANNT", L"已检测")
                                          : localized(L"COMPATIBLE", L"COMPATIBLE", L"KOMPATIBEL", L"兼容"))
                                      : localized(L"LIMITÉ", L"LIMITED", L"EINGESCHRÄNKT", L"受限"))
                      : localized(L"À TESTER", L"TO TEST", L"ZU TESTEN", L"待测试");
        RectF badge(card.GetRight() - 108, card.Y + 13, 92, 24);
        fillRound(graphics, badge, 8, reached ? (check.passed ? Color(255, 17, 52, 43) : Color(255, 59, 42, 27)) : Color(255, 35, 40, 55));
        text(graphics, result, badge, 8, reached ? (check.passed ? Color(255, 103, 226, 179) : Color(255, 245, 184, 98))
                                                : Color(255, 144, 154, 175), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    }

    y += 226;
    const bool certificationBusy = g_certificationDevice >= 0 || g_certificationApplying || g_certificationAwaitingAnswer;
    const float certificationHeight = certificationBusy ? 166.0f : 118.0f;
    RectF certificationCard(x, y, available, certificationHeight);
    fillRound(graphics, certificationCard, 17, Color(255, 27, 31, 44));
    strokeRound(graphics, certificationCard, 17, certificationBusy ? accentTint(0.48) : Color(255, 43, 49, 65));
    text(graphics, localized(L"Auto-calibration matérielle", L"Hardware auto-calibration", L"Hardware-Autokalibrierung", L"硬件自动校准"),
         RectF(certificationCard.X + 20, certificationCard.Y + 16, 310, 22), 14, Color::White, FontStyleBold);
    text(graphics, localized(L"Teste automatiquement le transport, l'ordre RGB et la fluidité, puis mémorise la combinaison validée.",
                             L"Automatically tests transport, RGB order and smoothness, then saves the validated combination.",
                             L"Testet Transport, RGB-Reihenfolge und Flüssigkeit automatisch und speichert die bestätigte Kombination.",
                             L"自动测试传输方式、RGB 顺序和流畅度，然后保存通过验证的组合。"),
         RectF(certificationCard.X + 20, certificationCard.Y + 43, certificationCard.Width - 250, 34),
         9, Color(255, 137, 147, 168));
    const std::uint32_t certificationColors[] = {0xFF3B47, 0x36D98B, 0x3B8DFF, 0x9A7CFF};
    for (int stage = 0; stage < 4; ++stage) {
        RectF step(certificationCard.GetRight() - 220 + stage * 48, certificationCard.Y + 19, 34, 34);
        const bool completed = certificationBusy && (stage < g_certificationStage || (g_certificationMotionTest && stage < 3));
        const bool current = certificationBusy && (g_certificationMotionTest ? stage == 3 : stage == g_certificationStage);
        fillRound(graphics, step, 11, rgbColor(certificationColors[stage], completed || current ? 255 : 105));
        strokeRound(graphics, step, 11, current ? Color::White : Color(255, 62, 69, 88), current ? 2.0f : 1.0f);
        if (completed) text(graphics, L"✓", step, 11, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    }
    if (certificationBusy) {
        std::wstring testedName;
        if (g_certificationDevice >= 0 && g_certificationDevice < static_cast<int>(g_rgbDevices.size())) {
            const RgbDevice candidate = configuredCertificationDevice(g_certificationDevice);
            testedName = candidate.name;
            if (!PluginEngine::isPluginDevice(candidate)) {
                testedName += L" · " + std::wstring(colorOrderName(candidate.colorOrder)) + L" · " +
                    transportName(candidate.frameTransport) + L" · " + std::to_wstring(candidate.frameIntervalMs) + L" ms";
            }
        }
        text(graphics, testedName, RectF(certificationCard.X + 20, certificationCard.Y + 82, certificationCard.Width - 390, 22),
             11, Color::White, FontStyleBold);
        text(graphics, g_certificationStatus, RectF(certificationCard.X + 20, certificationCard.Y + 108, certificationCard.Width - 390, 25),
             9, g_certificationApplying ? accentTint(0.48) : Color(255, 178, 188, 207));
        if (g_certificationAwaitingAnswer) {
            drawButton(graphics, RectF(certificationCard.GetRight() - 354, certificationCard.Y + 91, 214, 43),
                       g_certificationMotionTest
                           ? localized(L"Oui, animation stable", L"Yes, stable animation", L"Ja, stabile Animation", L"是的，动画稳定")
                           : localized(L"Oui, toutes les zones", L"Yes, all zones", L"Ja, alle Zonen", L"是的，所有区域"),
                       true, Action::CertificationYes);
            drawButton(graphics, RectF(certificationCard.GetRight() - 128, certificationCard.Y + 91, 108, 43),
                       g_certificationMotionTest ? localized(L"Ça clignote", L"It flickers", L"Es flimmert", L"有闪烁")
                                                 : localized(L"Non", L"No", L"Nein", L"否"),
                       false, Action::CertificationNo);
        } else {
            text(graphics, localized(L"ENVOI EN COURS…", L"SENDING…", L"WIRD GESENDET…", L"正在发送…"),
                 RectF(certificationCard.GetRight() - 260, certificationCard.Y + 96, 240, 24), 9,
                 accentTint(0.50), FontStyleBold, StringAlignmentFar, StringAlignmentCenter);
        }
    } else {
        const std::wstring instruction = g_certificationStatus.empty()
            ? localized(L"Utilise « Tester » sur un appareil ci-dessous.", L"Use “Test” on a device below.",
                        L"Nutze unten „Testen“ für ein Gerät.", L"请点击下方设备的“测试”。")
            : g_certificationStatus;
        text(graphics, instruction, RectF(certificationCard.X + 20, certificationCard.Y + 84, certificationCard.Width - 40, 20),
             9, Color(255, 164, 175, 195), FontStyleBold);
    }
    y = certificationCard.GetBottom() + 20;
    if (g_duckyDetected) {
        RectF ducky(x, y, available, 76);
        fillRound(graphics, ducky, 15, Color(255, 27, 31, 44));
        strokeRound(graphics, ducky, 15, Color(255, 91, 70, 38));
        fillRound(graphics, RectF(ducky.X + 15, ducky.Y + 14, 44, 44), 12, Color(255, 61, 47, 27));
        text(graphics, L"DK", RectF(ducky.X + 15, ducky.Y + 14, 44, 44), 9, Color(255, 242, 190, 102),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, L"Ducky One 2 Mini · DKON1861ST", RectF(ducky.X + 72, ducky.Y + 13, ducky.Width - 250, 20), 12, Color::White, FontStyleBold);
        text(graphics, localized(L"Détection USB + assistant local sécurisé · aucun protocole HID expérimental",
                                 L"USB detection + safe local assistant · no experimental HID protocol",
                                 L"USB-Erkennung + sicherer lokaler Assistent · kein experimentelles HID-Protokoll",
                                 L"USB 检测 + 安全本地助手 · 不使用实验性 HID 协议"),
             RectF(ducky.X + 72, ducky.Y + 39, ducky.Width - 250, 18), 9, Color(255, 143, 152, 173));
        RectF duckyBadge(ducky.GetRight() - 168, ducky.Y + 24, 150, 27);
        fillRound(graphics, duckyBadge, 9, Color(255, 56, 45, 26));
        text(graphics, localized(L"MODE COMPAGNON", L"COMPANION MODE", L"BEGLEITMODUS", L"伴随模式"),
             duckyBadge, 8, Color(255, 243, 191, 102), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        y = ducky.GetBottom() + 20;
    }
    text(graphics, localized(L"Résultat par appareil", L"Result by device", L"Ergebnis nach Gerät", L"各设备结果"),
         RectF(x, y, 340, 23), 15, Color::White, FontStyleBold);
    text(graphics, localized(L"Chaque élément garde ses propres capacités et réglages.", L"Each item keeps its own capabilities and settings.",
                             L"Jedes Gerät behält seine eigenen Fähigkeiten und Einstellungen.", L"每个设备都有独立功能和设置。"),
         RectF(width - 490.0f, y + 2, 452, 20), 9, Color(255, 129, 139, 160), FontStyleRegular, StringAlignmentFar);
    y += 34;
    auto deviceResult = [&](const std::wstring& name, const std::wstring& detail, const std::wstring& badgeText,
                            bool full, int certificationIndex = -1) {
        RectF row(x, y, available, 64);
        fillRound(graphics, row, 13, Color(255, 25, 29, 41));
        strokeRound(graphics, row, 13, full ? Color(255, 39, 83, 69) : Color(255, 78, 64, 42));
        const float reserved = certificationIndex >= 0 ? 390.0f : 210.0f;
        text(graphics, name, RectF(row.X + 16, row.Y + 10, row.Width - reserved, 18), 11, Color::White, FontStyleBold);
        text(graphics, detail, RectF(row.X + 16, row.Y + 34, row.Width - reserved, 15), 8, Color(255, 132, 142, 163));
        RectF badge(certificationIndex >= 0 ? row.GetRight() - 370 : row.GetRight() - 172,
                    row.Y + 18, certificationIndex >= 0 ? 196.0f : 154.0f, 28);
        fillRound(graphics, badge, 8, full ? Color(255, 17, 52, 43) : Color(255, 57, 45, 28));
        text(graphics, badgeText, badge, 8, full ? Color(255, 103, 226, 179) : Color(255, 244, 188, 101),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        if (certificationIndex >= 0) {
            drawButton(graphics, RectF(row.GetRight() - 160, row.Y + 11, 142, 42),
                       full ? localized(L"Re-tester", L"Test again", L"Erneut testen", L"重新测试")
                            : localized(L"Tester", L"Test", L"Testen", L"测试"),
                       !full, Action::CertificationStart, certificationIndex, !certificationBusy);
        }
        y += 72;
    };
    for (int deviceIndex = 0; deviceIndex < static_cast<int>(g_rgbDevices.size()); ++deviceIndex) {
        const RgbDevice& device = g_rgbDevices[deviceIndex];
        const LocalCertification certification = localCertification(device);
        const bool certified = certification == LocalCertification::Passed;
        std::wstring detail = (device.vendor.empty() ? L"OpenRGB" : device.vendor) + L" · " +
            std::to_wstring(device.zones) + localized(L" zone(s) · ", L" zone(s) · ", L" Zone(n) · ", L" 个分区 · ") +
            std::to_wstring(device.leds) + L" LED";
        if (certified && device.providerId == L"openrgb") {
            detail += L" · " + std::wstring(colorOrderName(device.colorOrder)) + L" · " +
                      transportName(device.frameTransport) + L" · " + std::to_wstring(device.frameIntervalMs) + L" ms";
        }
        deviceResult(device.name, detail,
                     certified ? localized(L"CERTIFIÉ SUR CE PC", L"CERTIFIED ON THIS PC", L"AUF DIESEM PC ZERTIFIZIERT", L"已在此电脑上认证")
                     : certification == LocalCertification::Failed
                         ? localized(L"ÉCHEC CONFIRMÉ", L"CONFIRMED FAILURE", L"FEHLER BESTÄTIGT", L"已确认失败")
                         : localized(L"TEST REQUIS", L"TEST REQUIRED", L"TEST ERFORDERLICH", L"需要测试"),
                     certified, deviceIndex);
    }
    for (const FanDevice& fan : g_fans) {
        const std::wstring reading = fan.rpm >= 0 ? std::to_wstring(static_cast<int>(std::lround(fan.rpm))) + L" RPM" : L"RPM --";
        deviceResult(fan.name, fan.hardware + L" · " + reading,
                     fan.controllable ? localized(L"CONTRÔLABLE", L"CONTROLLABLE", L"STEUERBAR", L"可控制")
                                      : localized(L"LECTURE SEULE", L"READ ONLY", L"NUR LESEN", L"只读"), fan.controllable);
    }
    if (g_rgbDevices.empty() && g_fans.empty()) {
        deviceResult(localized(L"Aucun élément détaillé", L"No detailed item", L"Kein detailliertes Gerät", L"没有设备详情"),
                     localized(L"Lance d'abord une nouvelle analyse depuis la page Appareils.",
                               L"Run a new scan from the Devices page first.",
                               L"Starte zuerst eine neue Suche auf der Geräteseite.", L"请先从设备页面重新扫描。"),
                     localized(L"À ANALYSER", L"TO SCAN", L"ZU SUCHEN", L"待扫描"), false);
    }
    g_maxScroll = std::max(0.0f, y + g_scrollOffset + 18 - height);
}

void drawDuckyAssistant(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, available - 160,
                  localized(L"ASSISTANT DUCKY", L"DUCKY ASSISTANT", L"DUCKY-ASSISTENT", L"DUCKY 助手"),
                  localized(L"Ton Ducky, simplement.", L"Your Ducky, made simple.",
                            L"Dein Ducky, ganz einfach.", L"轻松设置你的 Ducky。"));
    drawButton(graphics, RectF(width - 170.0f, originY + 8, 132, 42),
               localized(L"Retour", L"Back", L"Zurück", L"返回"), false, Action::DuckyBack);
    text(graphics,
         localized(L"Le clavier reste piloté par ses raccourcis officiels ; RGBCcontrol harmonise les autres appareils.",
                   L"The keyboard stays under its official shortcuts; RGBCcontrol matches the other devices.",
                   L"Die Tastatur bleibt unter ihren offiziellen Kürzeln; RGBCcontrol gleicht die anderen Geräte an.",
                   L"键盘仍使用官方快捷键；RGBCcontrol 会匹配其他设备。"),
         RectF(x, originY + 62, available, 22), 11, Color(255, 139, 149, 170));

    const float progressY = originY + 94;
    const wchar_t* progressLabels[] = {
        localized(L"1  STYLE", L"1  STYLE", L"1  STIL", L"1  样式"),
        localized(L"2  CLAVIER", L"2  KEYBOARD", L"2  TASTATUR", L"2  键盘"),
        localized(L"3  SYNCHRO", L"3  SYNC", L"3  SYNC", L"3  同步")
    };
    const float progressGap = 10.0f;
    const float progressWidth = (available - progressGap * 2) / 3.0f;
    for (int index = 0; index < 3; ++index) {
        const bool active = index == g_duckyAssistantStep;
        const bool complete = index < g_duckyAssistantStep;
        RectF step(x + index * (progressWidth + progressGap), progressY, progressWidth, 35);
        fillRound(graphics, step, 11, active ? accentColor(54) : complete ? Color(255, 20, 51, 43) : Color(255, 24, 28, 40));
        strokeRound(graphics, step, 11, active ? accentColor() : complete ? Color(255, 44, 105, 84) : Color(255, 43, 49, 65));
        text(graphics, progressLabels[index], step, 9,
             active ? accentTint(0.62) : complete ? Color(255, 104, 225, 179) : Color(255, 125, 135, 157),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    }

    const float cardY = progressY + 49;
    RectF panel(x, cardY, available, 356);
    fillRound(graphics, panel, 19, Color(255, 28, 32, 45));
    strokeRound(graphics, panel, 19, Color(255, 42, 48, 64));
    const float summaryWidth = std::clamp(available * 0.29f, 205.0f, 258.0f);
    RectF summary(panel.X + 18, panel.Y + 18, summaryWidth, panel.Height - 36);
    fillRound(graphics, summary, 16, Color(255, 20, 24, 35));
    strokeRound(graphics, summary, 16, g_duckyDetected ? Color(255, 79, 69, 44) : Color(255, 66, 70, 84));
    fillRound(graphics, RectF(summary.X + 16, summary.Y + 16, 48, 48), 14, Color(255, 61, 48, 28));
    text(graphics, L"DK", RectF(summary.X + 16, summary.Y + 16, 48, 48), 11, Color(255, 244, 194, 106),
         FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    text(graphics, L"Ducky One 2 Mini", RectF(summary.X + 76, summary.Y + 17, summary.Width - 90, 20), 12, Color::White, FontStyleBold);
    text(graphics, L"DKON1861ST", RectF(summary.X + 76, summary.Y + 40, summary.Width - 90, 17), 9, Color(255, 133, 143, 164));
    RectF deviceState(summary.X + 16, summary.Y + 79, summary.Width - 32, 26);
    fillRound(graphics, deviceState, 8, g_duckyDetected ? Color(255, 16, 52, 43) : Color(255, 52, 42, 27));
    text(graphics, g_duckyDetected
             ? localized(L"●  DÉTECTÉ", L"●  DETECTED", L"●  ERKANNT", L"●  已检测")
             : localized(L"●  NON CONNECTÉ", L"●  NOT CONNECTED", L"●  NICHT VERBUNDEN", L"●  未连接"),
         deviceState, 9, g_duckyDetected ? Color(255, 103, 225, 178) : Color(255, 242, 189, 104),
         FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    text(graphics, g_duckyModeConfirmed
             ? localized(L"CIBLE  ✓ CONFIRMÉE", L"TARGET  ✓ CONFIRMED", L"ZIEL  ✓ BESTÄTIGT", L"目标  ✓ 已确认")
             : localized(L"CIBLE", L"TARGET", L"ZIEL", L"目标"),
         RectF(summary.X + 16, summary.Y + 126, summary.Width - 32, 16), 8,
         g_duckyModeConfirmed ? Color(255, 103, 225, 178) : Color(255, 123, 133, 155), FontStyleBold);
    RectF colorPreview(summary.X + 16, summary.Y + 151, summary.Width - 32, 72);
    if (g_duckyMode == static_cast<int>(std::size(kDuckyModes)) - 1) fillRound(graphics, colorPreview, 15, Color(255, 8, 10, 15));
    else if (!kDuckyModes[g_duckyMode].adjustableColor) fillSeamlessGradient(graphics, colorPreview, 15);
    else fillRound(graphics, colorPreview, 15, rgbColor(g_baseColor));
    strokeRound(graphics, colorPreview, 15, Color(130, 255, 255, 255));
    text(graphics, duckyModeLabel(g_duckyMode), RectF(colorPreview.X + 12, colorPreview.Y + 13, colorPreview.Width - 24, 20), 12,
         Color::White, FontStyleBold, StringAlignmentCenter);
    text(graphics, kDuckyModes[g_duckyMode].adjustableColor ? hexColor(g_baseColor) : duckyModeDescription(g_duckyMode),
         RectF(colorPreview.X + 12, colorPreview.Y + 40, colorPreview.Width - 24, 18), 9, Color(230, 255, 255, 255),
         FontStyleRegular, StringAlignmentCenter);
    fillRound(graphics, RectF(summary.X + 16, summary.GetBottom() - 55, summary.Width - 32, 39), 11, Color(255, 31, 37, 52));
    textWrapped(graphics,
                localized(L"Bouclier actif : aucune commande HID n'est envoyée.", L"Shield active: no HID command is sent.",
                          L"Schutz aktiv: Es wird kein HID-Befehl gesendet.", L"保护已启用：不发送 HID 命令。"),
                RectF(summary.X + 28, summary.GetBottom() - 48, summary.Width - 56, 28), 8, Color(255, 147, 194, 176));

    const float contentX = summary.GetRight() + 18;
    const float contentWidth = panel.GetRight() - 18 - contentX;
    const float contentY = panel.Y + 18;
    if (g_duckyAssistantStep == 0) {
        text(graphics, localized(L"Choisis le rendu", L"Choose the look", L"Wähle den Look", L"选择外观"),
             RectF(contentX, contentY, contentWidth, 24), 17, Color::White, FontStyleBold);
        text(graphics, localized(L"Nous trouverons l'effet OpenRGB le plus proche.", L"We will find the closest OpenRGB effect.",
                                 L"Wir wählen den ähnlichsten OpenRGB-Effekt.", L"我们会选择最接近的 OpenRGB 效果。"),
             RectF(contentX, contentY + 27, contentWidth, 18), 9, Color(255, 135, 145, 166));
        const float gap = 10.0f;
        const float tileWidth = (contentWidth - gap) / 2.0f;
        for (int index = 0; index < static_cast<int>(std::size(kDuckyModes)); ++index) {
            const int row = index / 2;
            const int column = index % 2;
            RectF tile(contentX + column * (tileWidth + gap), contentY + 58 + row * 73, tileWidth, 63);
            const bool selected = index == g_duckyMode;
            fillRound(graphics, tile, 13, selected ? Color(255, 39, 42, 59) : Color(255, 22, 26, 37));
            if (selected) fillRound(graphics, tile, 13, accentColor(34));
            strokeRound(graphics, tile, 13, selected ? accentColor() : Color(255, 43, 49, 65), selected ? 1.5f : 1.0f);
            SolidBrush dot(index == static_cast<int>(std::size(kDuckyModes)) - 1 ? Color(255, 8, 10, 15)
                                                                                : rgbColor(kEffects[kDuckyModes[index].matchingEffect].color));
            graphics.FillEllipse(&dot, RectF(tile.X + 13, tile.Y + 15, 32, 32));
            text(graphics, duckyModeLabel(index), RectF(tile.X + 56, tile.Y + 11, tile.Width - 68, 19), 10,
                 Color::White, FontStyleBold);
            text(graphics, duckyModeDescription(index), RectF(tile.X + 56, tile.Y + 32, tile.Width - 68, 17), 8,
                 Color(255, 137, 147, 168));
            addHit(tile, Action::DuckyMode, index);
        }
        if (kDuckyModes[g_duckyMode].adjustableColor) {
            drawButton(graphics, RectF(contentX, contentY + 276, 154, 34),
                       std::wstring(localized(L"Ma couleur  ", L"My color  ", L"Meine Farbe  ", L"我的颜色  ")) + hexColor(g_baseColor),
                       false, Action::PickColor);
            const std::uint32_t presets[] = {0x7C5CFF, 0x149CFF, 0x00D69E, 0xFFD34F, 0xFF4F70, 0xFFFFFF};
            float presetX = contentX + 168;
            for (std::uint32_t preset : presets) {
                RectF presetRect(presetX, contentY + 278, 30, 30);
                fillRound(graphics, presetRect, 9, rgbColor(preset));
                strokeRound(graphics, presetRect, 9, preset == g_baseColor ? Color::White : Color(255, 75, 82, 101),
                            preset == g_baseColor ? 2.0f : 1.0f);
                addHit(presetRect, Action::SetColor, -1, preset);
                presetX += 39;
            }
        } else {
            RectF automatic(contentX, contentY + 276, contentWidth, 34);
            fillRound(graphics, automatic, 10, Color(255, 22, 38, 43));
            text(graphics, localized(L"✨ Les couleurs bougent automatiquement", L"✨ Colors move automatically",
                                     L"✨ Farben bewegen sich automatisch", L"✨ 颜色自动变化"),
                 automatic, 9, Color(255, 142, 210, 197), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        }
    } else if (g_duckyAssistantStep == 1) {
        const int guideKind = duckyGuideKind();
        const int guideCount = duckyGuideStageCount();
        text(graphics, localized(L"Regarde, puis reproduis", L"Watch, then copy", L"Ansehen und nachmachen", L"看一看，然后跟着做"),
             RectF(contentX, contentY, contentWidth - 90, 24), 17, Color::White, FontStyleBold);
        text(graphics, std::to_wstring(g_duckyGuideStage + 1) + L" / " + std::to_wstring(guideCount),
             RectF(contentX + contentWidth - 80, contentY, 80, 24), 10, accentTint(0.58), FontStyleBold,
             StringAlignmentFar, StringAlignmentCenter);
        for (int index = 0; index < guideCount; ++index) {
            RectF dot(contentX + index * 34.0f, contentY + 34, 26, 5);
            fillRound(graphics, dot, 3, index <= g_duckyGuideStage ? accentColor() : Color(255, 47, 53, 69));
        }

        RectF playground(contentX, contentY + 53, contentWidth, 251);
        fillRound(graphics, playground, 17, Color(255, 18, 22, 32));
        strokeRound(graphics, playground, 17, Color(255, 46, 53, 70));
        auto keycap = [&](float left, float top, float keyWidth, const std::wstring& label, bool highlighted = false) {
            RectF shadow(left + 2, top + 4, keyWidth, 54);
            fillRound(graphics, shadow, 12, Color(170, 4, 6, 10));
            RectF key(left, top, keyWidth, 54);
            fillRound(graphics, key, 12, highlighted ? accentColor() : Color(255, 39, 45, 61));
            strokeRound(graphics, key, 12, highlighted ? accentTint(0.62) : Color(255, 77, 85, 107), 1.4f);
            text(graphics, label, key, 13, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        };
        auto plus = [&](float left, float top) {
            text(graphics, L"+", RectF(left, top, 24, 54), 18, Color(255, 119, 129, 151), FontStyleBold,
                 StringAlignmentCenter, StringAlignmentCenter);
        };

        if (guideKind == 0) {
            const bool lightsOff = g_duckyMode == static_cast<int>(std::size(kDuckyModes)) - 1;
            const float totalWidth = 84 + 24 + 84 + 24 + 64;
            const float keysX = playground.X + (playground.Width - totalWidth) / 2.0f;
            if (g_duckyCalibrationPhase == 0) {
                text(graphics, localized(L"D'abord, un point de départ simple", L"First, set a simple starting point",
                                         L"Zuerst einen klaren Startpunkt setzen", L"先设置一个简单的起点"),
                     RectF(playground.X + 18, playground.Y + 16, playground.Width - 36, 25), 15, Color::White, FontStyleBold,
                     StringAlignmentCenter);
                keycap(keysX, playground.Y + 57, 84, L"Fn");
                plus(keysX + 84, playground.Y + 57);
                keycap(keysX + 108, playground.Y + 57, 84, L"Alt");
                plus(keysX + 192, playground.Y + 57);
                keycap(keysX + 216, playground.Y + 57, 64, L"T", true);
                RectF target(playground.X + 18, playground.Y + 130, playground.Width - 36, 50);
                fillRound(graphics, target, 13, Color(255, 7, 9, 14));
                strokeRound(graphics, target, 13, Color(255, 54, 61, 78));
                text(graphics, localized(L"Maintiens les 3 touches pendant 3 secondes", L"Hold all 3 keys for 3 seconds",
                                         L"Alle 3 Tasten 3 Sekunden halten", L"按住这 3 个键 3 秒"),
                     target, 10, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
                text(graphics, lightsOff
                         ? localized(L"Quand tout est éteint, ton objectif est déjà atteint.", L"When everything is off, your target is already reached.",
                                     L"Wenn alles aus ist, ist dein Ziel bereits erreicht.", L"全部熄灭后，目标就完成了。")
                         : localized(L"Quand le clavier est noir, confirme avec le bouton ci-dessous.", L"When the keyboard is dark, confirm with the button below.",
                                     L"Wenn die Tastatur dunkel ist, unten bestätigen.", L"键盘熄灭后，用下方按钮确认。"),
                     RectF(playground.X + 18, playground.Y + 194, playground.Width - 36, 22), 9, Color(255, 139, 149, 170),
                     FontStyleRegular, StringAlignmentCenter);
            } else if (g_duckyCalibrationPhase == 1) {
                text(graphics, std::wstring(localized(L"Compare avec : ", L"Compare with: ", L"Vergleiche mit: ", L"对比：")) + duckyModeLabel(g_duckyMode),
                     RectF(playground.X + 18, playground.Y + 14, playground.Width - 130, 25), 15, Color::White, FontStyleBold);
                text(graphics, std::wstring(localized(L"Essai ", L"Try ", L"Versuch ", L"尝试 ")) + std::to_wstring(g_duckyCalibrationAttempt),
                     RectF(playground.GetRight() - 112, playground.Y + 15, 94, 22), 9, accentTint(0.58), FontStyleBold,
                     StringAlignmentFar, StringAlignmentCenter);
                RectF visual(playground.X + 18, playground.Y + 50, playground.Width - 36, 62);
                if (kDuckyModes[g_duckyMode].adjustableColor) fillRound(graphics, visual, 14, rgbColor(g_baseColor));
                else fillSeamlessGradient(graphics, visual, 14);
                strokeRound(graphics, visual, 14, Color(150, 255, 255, 255));
                text(graphics, std::wstring(duckyModeLabel(g_duckyMode)) + L"  ·  " + duckyFirmwareModeName(g_duckyMode),
                     visual, 12, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
                RectF shortcut(playground.X + 18, playground.Y + 127, playground.Width - 36, 47);
                fillRound(graphics, shortcut, 12, Color(255, 32, 37, 52));
                text(graphics, L"Fn + Alt + T", RectF(shortcut.X + 14, shortcut.Y, 124, shortcut.Height), 12, Color::White,
                     FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
                text(graphics, localized(L"Appuie 1 fois sur le clavier", L"Press once on the keyboard",
                                         L"Einmal auf der Tastatur drücken", L"在键盘上按 1 次"),
                     RectF(shortcut.X + 144, shortcut.Y, shortcut.Width - 158, shortcut.Height), 9, Color(255, 150, 160, 180),
                     FontStyleRegular, StringAlignmentFar, StringAlignmentCenter);
                drawButton(graphics, RectF(playground.X + 18, playground.Y + 190, playground.Width - 36, 39),
                           localized(L"Pas encore — essayer le mode suivant", L"Not yet — try the next mode",
                                     L"Noch nicht — nächsten Modus versuchen", L"还没有——尝试下一个模式"),
                           false, Action::DuckyCalibrationNextMode);
            } else {
                SolidBrush glow(Color(55, 70, 224, 166));
                graphics.FillEllipse(&glow, RectF(playground.X + playground.Width / 2 - 48, playground.Y + 25, 96, 96));
                SolidBrush success(Color(255, 63, 214, 155));
                graphics.FillEllipse(&success, RectF(playground.X + playground.Width / 2 - 32, playground.Y + 41, 64, 64));
                text(graphics, L"✓", RectF(playground.X + playground.Width / 2 - 32, playground.Y + 41, 64, 64), 28,
                     Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
                text(graphics, localized(L"Bon mode confirmé !", L"Correct mode confirmed!", L"Richtiger Modus bestätigt!", L"正确模式已确认！"),
                     RectF(playground.X + 18, playground.Y + 122, playground.Width - 36, 28), 17, Color::White, FontStyleBold,
                     StringAlignmentCenter, StringAlignmentCenter);
                text(graphics, std::wstring(duckyModeLabel(g_duckyMode)) + L"  ·  " + duckyFirmwareModeName(g_duckyMode),
                     RectF(playground.X + 18, playground.Y + 157, playground.Width - 36, 24), 11, Color(255, 110, 225, 181),
                     FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
                drawButton(graphics, RectF(playground.X + playground.Width / 2 - 92, playground.Y + 197, 184, 35),
                           localized(L"Recommencer", L"Start again", L"Neu starten", L"重新开始"), false,
                           Action::DuckyCalibrationRestart);
            }
        } else if (guideKind == 1) {
            const int redLevel = static_cast<int>(std::lround(((g_baseColor >> 16) & 0xFF) * 10.0 / 255.0));
            const int greenLevel = static_cast<int>(std::lround(((g_baseColor >> 8) & 0xFF) * 10.0 / 255.0));
            const int blueLevel = static_cast<int>(std::lround((g_baseColor & 0xFF) * 10.0 / 255.0));
            text(graphics, localized(L"Compose ta couleur", L"Build your color", L"Mische deine Farbe", L"调出你的颜色"),
                 RectF(playground.X + 18, playground.Y + 15, playground.Width - 36, 24), 15, Color::White, FontStyleBold,
                 StringAlignmentCenter);
            RectF reset(playground.X + 18, playground.Y + 50, playground.Width - 36, 42);
            fillRound(graphics, reset, 11, Color(255, 31, 36, 50));
            text(graphics, L"1   Fn + Alt + V", RectF(reset.X + 14, reset.Y, 138, reset.Height), 11, Color::White, FontStyleBold,
                 StringAlignmentNear, StringAlignmentCenter);
            text(graphics, localized(L"Remise à zéro", L"Reset colors", L"Farben zurücksetzen", L"重置颜色"),
                 RectF(reset.X + 155, reset.Y, reset.Width - 169, reset.Height), 9, Color(255, 144, 154, 175),
                 FontStyleRegular, StringAlignmentFar, StringAlignmentCenter);
            text(graphics, localized(L"2   Garde Fn + Alt enfoncés, puis appuie :", L"2   Keep Fn + Alt held, then press:",
                                     L"2   Fn + Alt halten, dann drücken:", L"2   按住 Fn + Alt，然后按："),
                 RectF(playground.X + 18, playground.Y + 106, playground.Width - 36, 20), 10, Color(255, 212, 216, 228), FontStyleBold);
            const wchar_t channelKeys[] = {L'Z', L'X', L'C'};
            const int channelLevels[] = {redLevel, greenLevel, blueLevel};
            const Color channelColors[] = {Color(255, 255, 91, 113), Color(255, 51, 218, 163), Color(255, 58, 155, 255)};
            const float channelGap = 9.0f;
            const float channelWidth = (playground.Width - 36 - channelGap * 2) / 3.0f;
            for (int index = 0; index < 3; ++index) {
                RectF channel(playground.X + 18 + index * (channelWidth + channelGap), playground.Y + 137, channelWidth, 55);
                fillRound(graphics, channel, 12, Color(255, 28, 33, 46));
                strokeRound(graphics, channel, 12, channelColors[index]);
                text(graphics, std::wstring(1, channelKeys[index]), RectF(channel.X + 12, channel.Y, 30, channel.Height), 15,
                     channelColors[index], FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
                text(graphics, L"× " + std::to_wstring(channelLevels[index]), RectF(channel.X + 45, channel.Y, channel.Width - 57, channel.Height),
                     14, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
            }
            RectF wanted(playground.X + 18, playground.Y + 204, playground.Width - 36, 30);
            fillRound(graphics, wanted, 10, rgbColor(g_baseColor));
            text(graphics, std::wstring(localized(L"Résultat visé  ", L"Target result  ", L"Zielfarbe  ", L"目标结果  ")) + hexColor(g_baseColor),
                 wanted, 10, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        } else {
            text(graphics, localized(L"Ajuste la vitesse à l'œil", L"Match the speed by eye", L"Geschwindigkeit visuell anpassen", L"目测调整速度"),
                 RectF(playground.X + 18, playground.Y + 16, playground.Width - 36, 25), 15, Color::White, FontStyleBold,
                 StringAlignmentCenter);
            const float pairWidth = std::min(370.0f, playground.Width - 40);
            const float pairX = playground.X + (playground.Width - pairWidth) / 2.0f;
            RectF slower(pairX, playground.Y + 58, (pairWidth - 12) / 2.0f, 82);
            RectF faster(slower.GetRight() + 12, slower.Y, slower.Width, slower.Height);
            fillRound(graphics, slower, 14, Color(255, 30, 36, 50));
            fillRound(graphics, faster, 14, Color(255, 30, 36, 50));
            strokeRound(graphics, slower, 14, Color(255, 77, 86, 108));
            strokeRound(graphics, faster, 14, accentColor());
            text(graphics, L"Fn + Alt + J", RectF(slower.X, slower.Y + 10, slower.Width, 27), 13, Color::White, FontStyleBold,
                 StringAlignmentCenter, StringAlignmentCenter);
            text(graphics, localized(L"Plus lent", L"Slower", L"Langsamer", L"更慢"), RectF(slower.X, slower.Y + 44, slower.Width, 22), 9,
                 Color(255, 143, 153, 174), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
            text(graphics, L"Fn + Alt + L", RectF(faster.X, faster.Y + 10, faster.Width, 27), 13, Color::White, FontStyleBold,
                 StringAlignmentCenter, StringAlignmentCenter);
            text(graphics, localized(L"Plus rapide", L"Faster", L"Schneller", L"更快"), RectF(faster.X, faster.Y + 44, faster.Width, 22), 9,
                 accentTint(0.60), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
            text(graphics, localized(L"Appuie sur J ou L jusqu'à ce que les animations avancent ensemble.",
                                     L"Press J or L until the animations move together.",
                                     L"J oder L drücken, bis die Animationen gemeinsam laufen.",
                                     L"按 J 或 L，直到动画同步。"),
                 RectF(playground.X + 24, playground.Y + 160, playground.Width - 48, 42), 10, Color(255, 160, 169, 188),
                 FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
            fillSeamlessGradient(graphics, RectF(playground.X + 55, playground.Y + 215, playground.Width - 110, 8), 4);
        }
    } else {
        text(graphics, localized(L"Lance la synchronisation", L"Start synchronization", L"Synchronisierung starten", L"开始同步"),
             RectF(contentX, contentY, contentWidth, 24), 17, Color::White, FontStyleBold);
        textWrapped(graphics,
                    localized(L"RGBCcontrol applique maintenant l'équivalent aux appareils compatibles. Le Ducky reste protégé et conserve ses réglages en mémoire interne.",
                              L"RGBCcontrol now applies the matching effect to compatible devices. The Ducky stays protected and keeps its settings in onboard memory.",
                              L"RGBCcontrol wendet jetzt den passenden Effekt auf kompatible Geräte an. Das Ducky bleibt geschützt und speichert seine Einstellungen intern.",
                              L"RGBCcontrol 现在将匹配效果应用于兼容设备。Ducky 仍受保护，并在内置存储中保留设置。"),
                    RectF(contentX, contentY + 35, contentWidth, 54), 10, Color(255, 143, 153, 174));
        RectF keyboardCard(contentX, contentY + 103, contentWidth, 73);
        fillRound(graphics, keyboardCard, 14, Color(255, 55, 45, 27));
        text(graphics, g_duckyModeConfirmed
                 ? localized(L"DUCKY · MODE CONFIRMÉ", L"DUCKY · MODE CONFIRMED", L"DUCKY · MODUS BESTÄTIGT", L"DUCKY · 模式已确认")
                 : localized(L"DUCKY · À CONFIRMER", L"DUCKY · NEEDS CONFIRMATION", L"DUCKY · NOCH BESTÄTIGEN", L"DUCKY · 待确认"),
             RectF(keyboardCard.X + 15, keyboardCard.Y + 12, keyboardCard.Width - 30, 17), 8,
             g_duckyModeConfirmed ? Color(255, 104, 225, 179) : Color(255, 242, 190, 104), FontStyleBold);
        text(graphics, std::wstring(duckyModeLabel(g_duckyMode)) + L"  ·  " + duckyFirmwareModeName(g_duckyMode),
             RectF(keyboardCard.X + 15, keyboardCard.Y + 37, keyboardCard.Width - 30, 20), 11, Color::White, FontStyleBold);
        RectF syncCard(contentX, contentY + 187, contentWidth, 91);
        fillRound(graphics, syncCard, 14, Color(255, 20, 38, 43));
        strokeRound(graphics, syncCard, 14, Color(255, 42, 79, 79));
        text(graphics, std::to_wstring(selectedRgbCount()) + localized(L" appareil(s) OpenRGB inclus", L" OpenRGB device(s) included",
                                                                       L" OpenRGB-Gerät(e) enthalten", L" 个 OpenRGB 设备已包含"),
             RectF(syncCard.X + 15, syncCard.Y + 12, syncCard.Width - 214, 20), 10, Color(255, 172, 215, 206), FontStyleBold);
        text(graphics, g_duckyStatus.empty()
                 ? localized(L"Prêt à appliquer l'équivalent visuel.", L"Ready to apply the visual match.",
                             L"Bereit für den passenden visuellen Effekt.", L"已准备应用匹配效果。")
                 : g_duckyStatus,
             RectF(syncCard.X + 15, syncCard.Y + 39, syncCard.Width - 214, 36), 8, Color(255, 130, 159, 158));
        drawButton(graphics, RectF(syncCard.GetRight() - 184, syncCard.Y + 24, 166, 43),
                   localized(L"Synchroniser", L"Synchronize", L"Synchronisieren", L"同步"), true, Action::DuckySync,
                   -1, selectedRgbCount() > 0 && g_duckyModeConfirmed);
    }

    const float controlsY = panel.GetBottom() + 14;
    if (g_duckyAssistantStep > 0) {
        drawButton(graphics, RectF(x, controlsY, 132, 40), localized(L"Précédent", L"Previous", L"Zurück", L"上一步"),
                   false, Action::DuckyPrevious);
    }
    if (g_duckyAssistantStep < 2) {
        const int guideKind = duckyGuideKind();
        const wchar_t* nextLabel = g_duckyAssistantStep == 0
            ? localized(L"C'est parti !", L"Let's go!", L"Los geht's!", L"开始吧！")
            : guideKind == 0
                ? g_duckyCalibrationPhase == 0
                    ? localized(L"Le clavier est éteint", L"The keyboard is dark", L"Die Tastatur ist dunkel", L"键盘已熄灭")
                    : g_duckyCalibrationPhase == 1
                        ? localized(L"Oui, c'est le bon mode", L"Yes, this is the right mode", L"Ja, das ist der richtige Modus", L"是的，这是正确模式")
                        : localized(L"Continuer", L"Continue", L"Weiter", L"继续")
                : guideKind == 1
                    ? localized(L"La couleur est bonne", L"The color looks right", L"Die Farbe stimmt", L"颜色对了")
                    : localized(L"La vitesse me va", L"The speed looks right", L"Die Geschwindigkeit stimmt", L"速度对了");
        drawButton(graphics, RectF(width - 242.0f, controlsY, 204, 40), nextLabel, true, Action::DuckyNext);
    } else {
        drawButton(graphics, RectF(width - 238.0f, controlsY, 200, 40),
                   localized(L"Manuel officiel", L"Official manual", L"Offizielles Handbuch", L"官方手册"),
                   false, Action::DuckyOpenManual);
    }
    RectF safety(x, controlsY + 55, available, 60);
    fillRound(graphics, safety, 14, Color(255, 17, 43, 36));
    strokeRound(graphics, safety, 14, Color(255, 31, 81, 65));
    text(graphics, localized(L"SÉCURITÉ", L"SAFETY", L"SICHERHEIT", L"安全"), RectF(safety.X + 16, safety.Y + 10, 100, 17), 8,
         Color(255, 103, 225, 178), FontStyleBold);
    textWrapped(graphics,
                localized(L"Cet assistant n'installe aucun firmware et n'ouvre aucune interface HID du clavier.",
                          L"This assistant installs no firmware and opens no keyboard HID interface.",
                          L"Dieser Assistent installiert keine Firmware und öffnet keine HID-Schnittstelle der Tastatur.",
                          L"此助手不安装固件，也不打开键盘 HID 接口。"),
                RectF(safety.X + 16, safety.Y + 31, safety.Width - 32, 22), 9, Color(255, 158, 197, 182));
    g_maxScroll = std::max(0.0f, safety.GetBottom() + g_scrollOffset + 18 - height);
}

void drawAudioMeter(Graphics& graphics, const RectF& rect, const std::wstring& label, double level, const Color& color) {
    level = std::clamp(level, 0.0, 1.0);
    text(graphics, label, RectF(rect.X, rect.Y, 74, rect.Height), 9, Color(255, 178, 185, 204), FontStyleBold,
         StringAlignmentNear, StringAlignmentCenter);
    RectF track(rect.X + 76, rect.Y + rect.Height / 2 - 4, rect.Width - 119, 8);
    fillRound(graphics, track, 4, Color(255, 13, 17, 25));
    strokeRound(graphics, track, 4, Color(255, 44, 51, 69));
    const float filledWidth = track.Width * static_cast<float>(level);
    if (filledWidth > 1.0f) {
        fillRound(graphics, RectF(track.X, track.Y, filledWidth, track.Height), 4, color);
        SolidBrush glow(Color(72, color.GetR(), color.GetG(), color.GetB()));
        graphics.FillEllipse(&glow, RectF(track.X + filledWidth - 8, track.Y - 4, 16, 16));
    }
    text(graphics, std::to_wstring(static_cast<int>(std::lround(level * 100.0))) + L" %",
         RectF(rect.GetRight() - 39, rect.Y, 39, rect.Height), 9, Color(255, 223, 226, 237), FontStyleBold,
         StringAlignmentFar, StringAlignmentCenter);
}

void drawEffects(Graphics& graphics, int width, int height, float originY) {
    float x = kSidebarWidth + 32.0f;
    float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, 620,
                  localized(L"STUDIO RGB", L"RGB STUDIO", L"RGB-STUDIO", L"RGB 工作室"),
                  localized(L"Crée ton ambiance.", L"Create your atmosphere.",
                            L"Gestalte deine Atmosphäre.", L"打造你的灯光氛围。"));
    text(graphics, std::to_wstring(selectedRgbCount()) + localized(L" appareil(s) sélectionné(s)", L" device(s) selected", L" Gerät(e) ausgewählt", L" 个设备已选择"), RectF(width - 350.0f, originY + 27, 310, 24), 11, Color(255, 117, 205, 180), FontStyleBold, StringAlignmentFar);
    float y = originY + 83;
    RectF selected(x, y, available, 82);
    LinearGradientBrush gradient(PointF(selected.X, selected.Y), PointF(selected.GetRight(), selected.GetBottom()), Color(255, 34, 28, 57), Color(255, 16, 39, 52));
    GraphicsPath selectedPath;
    roundedPath(selectedPath, selected, 18);
    graphics.FillPath(&gradient, &selectedPath);
    strokeRound(graphics, selected, 18, Color(255, 52, 48, 82));
    text(graphics, localized(L"EFFET SÉLECTIONNÉ", L"SELECTED EFFECT", L"AUSGEWÄHLTER EFFEKT", L"已选效果"), RectF(selected.X + 20, selected.Y + 11, 260, 16), 9, Color(255, 132, 116, 214), FontStyleBold);
    text(graphics, effectLabel(g_selectedEffect), RectF(selected.X + 20, selected.Y + 31, 300, 25), 19, Color::White, FontStyleBold);
    text(graphics, effectDescription(g_selectedEffect), RectF(selected.X + 20, selected.Y + 58, 420, 16), 9, Color(255, 156, 164, 183));
    RectF selectedPreview(selected.GetRight() - 90, selected.Y + 15, 70, 52);
    const bool musicSelected = std::wstring(kEffects[g_selectedEffect].internal) == L"Music";
    const bool ambientSelected = std::wstring(kEffects[g_selectedEffect].internal) == L"Ambilight";
    if (std::wstring(kEffects[g_selectedEffect].internal) == L"Gradient") {
        fillSeamlessGradient(graphics, selectedPreview, 14);
    } else if (musicSelected) {
        fillRound(graphics, selectedPreview, 14, Color(255, 19, 23, 35));
        const double levels[] = {g_audioBass.load(), g_audioMid.load(), g_audioTreble.load()};
        const Color colors[] = {Color(255, 255, 91, 140), accentColor(), Color(255, 54, 218, 208)};
        for (int band = 0; band < 3; ++band) {
            const float meterHeight = 12.0f + static_cast<float>(levels[band]) * 23.0f;
            fillRound(graphics, RectF(selectedPreview.X + 13 + band * 17, selectedPreview.GetBottom() - 9 - meterHeight,
                                      10, meterHeight), 5, colors[band]);
        }
        strokeRound(graphics, selectedPreview, 14, accentTint(0.35));
    } else if (ambientSelected) {
        fillRound(graphics, selectedPreview, 14, Color(255, 15, 19, 29));
        const Color left = rgbColor(g_ambientColors[0].load() ? g_ambientColors[0].load() : 0x7C5CFF);
        const Color top = rgbColor(g_ambientColors[1].load() ? g_ambientColors[1].load() : 0x2F8CFF);
        const Color right = rgbColor(g_ambientColors[2].load() ? g_ambientColors[2].load() : 0x00CDB4);
        const Color bottom = rgbColor(g_ambientColors[3].load() ? g_ambientColors[3].load() : 0xF05B9D);
        fillRound(graphics, RectF(selectedPreview.X + 7, selectedPreview.Y + 7, 6, selectedPreview.Height - 14), 3, left);
        fillRound(graphics, RectF(selectedPreview.X + 12, selectedPreview.Y + 7, selectedPreview.Width - 24, 6), 3, top);
        fillRound(graphics, RectF(selectedPreview.GetRight() - 13, selectedPreview.Y + 7, 6, selectedPreview.Height - 14), 3, right);
        fillRound(graphics, RectF(selectedPreview.X + 12, selectedPreview.GetBottom() - 13, selectedPreview.Width - 24, 6), 3, bottom);
        strokeRound(graphics, selectedPreview, 14, accentTint(0.35));
    } else {
        fillRound(graphics, selectedPreview, 14, rgbColor(kEffects[g_selectedEffect].color));
    }
    y += 98;
    text(graphics, localized(L"Bibliothèque d'effets", L"Effects library", L"Effektbibliothek", L"效果库"), RectF(x, y, 280, 20), 13, Color(255, 220, 224, 234), FontStyleBold);
    y += 24;
    constexpr int effectColumns = 5;
    const int effectRows = (static_cast<int>(std::size(kEffects)) + effectColumns - 1) / effectColumns;
    float tileWidth = (available - 40) / static_cast<float>(effectColumns);
    for (int index = 0; index < static_cast<int>(std::size(kEffects)); ++index) {
        int row = index / effectColumns;
        int column = index % effectColumns;
        RectF tile(x + column * (tileWidth + 10), y + row * 86, tileWidth, 80);
        fillRound(graphics, tile, 15, index == g_selectedEffect ? Color(255, 43, 37, 68) : Color(255, 26, 30, 42));
        strokeRound(graphics, tile, 15, index == g_selectedEffect ? Color(255, 128, 104, 232) : Color(255, 44, 50, 66));
        if (std::wstring(kEffects[index].internal) == L"Gradient") {
            fillSeamlessGradient(graphics, RectF(tile.X + 15, tile.Y + 12, 44, 11), 5);
        } else {
            SolidBrush dot(rgbColor(kEffects[index].color));
            graphics.FillEllipse(&dot, RectF(tile.X + 15.0f, tile.Y + 12.0f, 11.0f, 11.0f));
        }
        text(graphics, effectLabel(index), RectF(tile.X + 15, tile.Y + 31, tile.Width - 30, 19), 11, Color(255, 230, 233, 241), FontStyleBold);
        text(graphics, effectDescription(index), RectF(tile.X + 15, tile.Y + 55, tile.Width - 30, 16), 8, Color(255, 121, 129, 151));
        addHit(tile, Action::SelectEffect, index);
    }
    y += effectRows * 86 + 8;
    RectF settings(x, y, available, ambientSelected ? 188.0f : musicSelected ? 176.0f : 100.0f);
    fillRound(graphics, settings, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, settings, 18, Color(255, 39, 45, 61));
    if (musicSelected) {
        const float dividerX = settings.X + settings.Width * 0.56f;
        text(graphics, L"AUDIO WINDOWS · WASAPI", RectF(settings.X + 20, settings.Y + 13, 230, 18), 9,
             Color(255, 106, 224, 193), FontStyleBold);
        const std::wstring signalLabel = g_audioCaptureReady
            ? (g_audioSignal ? localized(L"● SIGNAL DÉTECTÉ", L"● SIGNAL DETECTED", L"● SIGNAL ERKANNT", L"● 检测到信号")
                             : localized(L"● EN ATTENTE DE SON", L"● WAITING FOR AUDIO", L"● WARTE AUF TON", L"● 等待声音"))
            : localized(L"○ PRÊT À ÉCOUTER", L"○ READY TO LISTEN", L"○ BEREIT", L"○ 准备监听");
        text(graphics, signalLabel, RectF(dividerX - 190, settings.Y + 13, 170, 18), 8,
             g_audioSignal ? Color(255, 102, 226, 176) : Color(255, 139, 149, 171), FontStyleBold, StringAlignmentFar);
        const float meterWidth = dividerX - settings.X - 40;
        drawAudioMeter(graphics, RectF(settings.X + 20, settings.Y + 39, meterWidth, 22),
                       localized(L"BASSES", L"BASS", L"BÄSSE", L"低音"), g_audioBass.load(), Color(255, 255, 91, 140));
        drawAudioMeter(graphics, RectF(settings.X + 20, settings.Y + 67, meterWidth, 22),
                       localized(L"MÉDIUMS", L"MIDS", L"MITTEN", L"中音"), g_audioMid.load(), accentColor());
        drawAudioMeter(graphics, RectF(settings.X + 20, settings.Y + 95, meterWidth, 22),
                       localized(L"AIGUS", L"TREBLE", L"HÖHEN", L"高音"), g_audioTreble.load(), Color(255, 54, 218, 208));
        text(graphics, g_duckyDetected
            ? localized(L"Sortie du PC · aucun microphone · Ducky en mode compagnon",
                        L"PC output · no microphone · Ducky companion mode",
                        L"PC-Ausgabe · kein Mikrofon · Ducky-Begleitmodus",
                        L"电脑输出 · 无需麦克风 · Ducky 伴随模式")
            : localized(L"Sortie du PC · aucun microphone",
                        L"PC output · no microphone",
                        L"PC-Ausgabe · kein Mikrofon",
                        L"电脑输出 · 无需麦克风"),
             RectF(settings.X + 20, settings.Y + 127, meterWidth, 17), 8, Color(255, 126, 136, 158));
        text(graphics, g_status, RectF(settings.X + 20, settings.Y + 149, meterWidth, 17), 8,
             g_effectActive ? Color(255, 112, 220, 177) : Color(255, 127, 136, 157));

        const float controlX = dividerX + 20;
        const float controlWidth = settings.GetRight() - controlX - 20;
        text(graphics, localized(L"Sensibilité", L"Sensitivity", L"Empfindlichkeit", L"灵敏度"),
             RectF(controlX, settings.Y + 13, 150, 18), 10, Color::White, FontStyleBold);
        text(graphics, std::to_wstring(g_effectSpeed.load()) + L" %", RectF(controlX, settings.Y + 13, controlWidth, 18),
             9, Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
        drawSlider(graphics, RectF(controlX, settings.Y + 34, controlWidth, 22), g_effectSpeed.load(), 0, Action::EffectSpeed);
        text(graphics, localized(L"Luminosité", L"Brightness", L"Helligkeit", L"亮度"),
             RectF(controlX, settings.Y + 64, 150, 18), 10, Color::White, FontStyleBold);
        text(graphics, std::to_wstring(g_effectIntensity.load()) + L" %", RectF(controlX, settings.Y + 64, controlWidth, 18),
             9, Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
        drawSlider(graphics, RectF(controlX, settings.Y + 85, controlWidth, 22), g_effectIntensity.load(), 0, Action::EffectIntensity);
        drawButton(graphics, RectF(controlX, settings.Y + 122, controlWidth, 40),
                   localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true, Action::ApplyEffect);
    } else if (ambientSelected) {
        const float dividerX = settings.X + settings.Width * 0.55f;
        const float previewWidth = dividerX - settings.X - 40;
        text(graphics, localized(L"APERÇU DE L'ÉCRAN", L"SCREEN PREVIEW", L"BILDSCHIRMVORSCHAU", L"屏幕预览"),
             RectF(settings.X + 20, settings.Y + 12, 180, 18), 9, Color(255, 106, 224, 193), FontStyleBold);
        const std::wstring captureState = g_ambientCaptureReady
            ? std::to_wstring(static_cast<int>(std::lround(g_ambientFps.load()))) + L" FPS"
            : localized(L"PRÊT", L"READY", L"BEREIT", L"就绪");
        text(graphics, captureState, RectF(dividerX - 110, settings.Y + 12, 90, 18), 8,
             g_ambientCaptureReady ? Color(255, 104, 225, 179) : Color(255, 139, 149, 171), FontStyleBold, StringAlignmentFar);
        RectF screenPreview(settings.X + 20, settings.Y + 37, previewWidth, 82);
        fillRound(graphics, screenPreview, 12, Color(255, 11, 14, 22));
        const std::uint32_t fallback[] = {0x7C5CFF, 0x2F8CFF, 0x00CDB4, 0xF05B9D};
        std::uint32_t zoneRgb[4]{};
        for (int zone = 0; zone < 4; ++zone) zoneRgb[zone] = g_ambientColors[zone].load() ? g_ambientColors[zone].load() : fallback[zone];
        fillRound(graphics, RectF(screenPreview.X + 6, screenPreview.Y + 6, 8, screenPreview.Height - 12), 4, rgbColor(zoneRgb[0]));
        fillRound(graphics, RectF(screenPreview.X + 13, screenPreview.Y + 6, screenPreview.Width - 26, 8), 4, rgbColor(zoneRgb[1]));
        fillRound(graphics, RectF(screenPreview.GetRight() - 14, screenPreview.Y + 6, 8, screenPreview.Height - 12), 4, rgbColor(zoneRgb[2]));
        fillRound(graphics, RectF(screenPreview.X + 13, screenPreview.GetBottom() - 14, screenPreview.Width - 26, 8), 4, rgbColor(zoneRgb[3]));
        text(graphics, localized(L"Les bords deviennent la lumière", L"Edges become light", L"Bildränder werden Licht", L"屏幕边缘化为灯光"),
             RectF(screenPreview.X + 24, screenPreview.Y + 18, screenPreview.Width - 48, 46), 10, Color(255, 198, 204, 220),
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);

        std::wstring monitorLabel = localized(L"Écran principal", L"Primary display", L"Hauptbildschirm", L"主显示器");
        if (!g_screenMonitors.empty()) {
            const int selectedMonitor = std::clamp(g_ambientMonitorIndex, 0, static_cast<int>(g_screenMonitors.size()) - 1);
            monitorLabel = std::wstring(localized(L"Écran ", L"Display ", L"Bildschirm ", L"显示器 ")) +
                           std::to_wstring(selectedMonitor + 1) +
                           (g_screenMonitors[selectedMonitor].primary ? localized(L" · principal", L" · primary", L" · primär", L" · 主屏") : L"");
        }
        const float leftButtonWidth = (previewWidth - 8) * 0.58f;
        drawButton(graphics, RectF(settings.X + 20, settings.Y + 132, leftButtonWidth, 38), monitorLabel,
                   false, Action::AmbientMonitor);
        drawButton(graphics, RectF(settings.X + 28 + leftButtonWidth, settings.Y + 132, previewWidth - leftButtonWidth - 8, 38),
                   g_ambientZones ? localized(L"4 zones", L"4 zones", L"4 Zonen", L"4 分区")
                                  : localized(L"Moyenne", L"Average", L"Mittelwert", L"平均色"),
                   g_ambientZones, Action::AmbientZones);

        const float controlX = dividerX + 20;
        const float controlWidth = settings.GetRight() - controlX - 20;
        auto ambientSlider = [&](float labelY, const wchar_t* label, int value, Action action) {
            text(graphics, label, RectF(controlX, settings.Y + labelY, 150, 18), 9, Color::White, FontStyleBold);
            text(graphics, std::to_wstring(value) + L" %", RectF(controlX, settings.Y + labelY, controlWidth, 18), 8,
                 Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
            drawSlider(graphics, RectF(controlX, settings.Y + labelY + 18, controlWidth, 20), value, 0, action);
        };
        ambientSlider(10, localized(L"Réactivité", L"Reactivity", L"Reaktion", L"响应速度"), g_effectSpeed.load(), Action::EffectSpeed);
        ambientSlider(51, localized(L"Saturation", L"Saturation", L"Sättigung", L"饱和度"), g_ambientSaturation.load(), Action::AmbientSaturation);
        ambientSlider(92, localized(L"Luminosité", L"Brightness", L"Helligkeit", L"亮度"), g_effectIntensity.load(), Action::EffectIntensity);
        drawButton(graphics, RectF(controlX, settings.Y + 139, controlWidth, 36),
                   localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true, Action::ApplyEffect);
    } else {
        text(graphics, localized(L"Vitesse", L"Speed", L"Geschwindigkeit", L"速度"), RectF(settings.X + 20, settings.Y + 13, 150, 20), 11, Color::White, FontStyleBold);
        text(graphics, std::to_wstring(g_effectSpeed.load()) + L" %", RectF(settings.X + available / 2 - 90, settings.Y + 13, 70, 20), 10, Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
        drawSlider(graphics, RectF(settings.X + 20, settings.Y + 36, available / 2 - 40, 26), g_effectSpeed.load(), 0, Action::EffectSpeed);
        text(graphics, localized(L"Intensité", L"Intensity", L"Intensität", L"强度"), RectF(settings.X + available / 2 + 10, settings.Y + 13, 120, 20), 11, Color::White, FontStyleBold);
        text(graphics, std::to_wstring(g_effectIntensity.load()) + L" %", RectF(settings.GetRight() - 240, settings.Y + 13, 70, 20), 10, Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
        drawSlider(graphics, RectF(settings.X + available / 2 + 10, settings.Y + 36, available / 2 - 210, 26), g_effectIntensity.load(), 0, Action::EffectIntensity);
        drawButton(graphics, RectF(settings.GetRight() - 158, settings.Y + 27, 138, 40),
                   localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true, Action::ApplyEffect);
        text(graphics, g_status, RectF(settings.X + 20, settings.Y + 74, settings.Width - 40, 18), 9,
             g_effectActive ? Color(255, 112, 220, 177) : Color(255, 127, 136, 157));
    }
    g_maxScroll = std::max(0.0f, settings.GetBottom() + g_scrollOffset + 18 - height);
}

void temperatureCard(Graphics& graphics, const RectF& rect, const wchar_t* title, double value, const wchar_t* subtitle) {
    fillRound(graphics, rect, 17, Color(255, 28, 32, 45));
    strokeRound(graphics, rect, 17, Color(255, 39, 45, 61));
    text(graphics, title, RectF(rect.X + 17, rect.Y + 15, rect.Width - 34, 16), 9, Color(255, 117, 128, 151), FontStyleBold);
    std::wstring reading = value > 0 ? std::to_wstring(static_cast<int>(std::lround(value))) + L" °C" : L"-- °C";
    text(graphics, reading, RectF(rect.X + 17, rect.Y + 38, rect.Width - 34, 30), 23, Color::White, FontStyleBold);
    text(graphics, subtitle, RectF(rect.X + 17, rect.Y + 74, rect.Width - 34, 16), 9, Color(255, 127, 136, 157));
}

void drawFans(Graphics& graphics, int width, int height, float originY) {
    float x = kSidebarWidth + 32.0f;
    float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, std::max(300.0f, available - 300.0f),
                  localized(L"VENTILATION", L"COOLING", L"LÜFTER", L"风扇控制"),
                  localized(L"Silence ou performance.", L"Quiet or cool, on demand.",
                            L"Leise oder leistungsstark.", L"静音或高性能。"));
    drawButton(graphics, RectF(width - 290.0f, originY + 12, 120, 38), localized(L"Analyser", L"Scan", L"Suchen", L"扫描"), false, Action::Refresh);
    drawButton(graphics, RectF(width - 160.0f, originY + 12, 122, 38),
               g_isAdministrator ? localized(L"Accès complet", L"Full access", L"Vollzugriff", L"完整权限")
                                 : localized(L"Mode avancé", L"Advanced", L"Erweitert", L"高级模式"),
               false, Action::FanAdmin, -1, !g_isAdministrator);
    float y = originY + 82;
    float tempWidth = (available - 24) / 3.0f;
    temperatureCard(graphics, RectF(x, y, tempWidth, 104), localized(L"PROCESSEUR", L"PROCESSOR", L"PROZESSOR", L"处理器"), g_cpuTemperature,
                    localized(L"Température CPU", L"CPU temperature", L"CPU-Temperatur", L"CPU 温度"));
    temperatureCard(graphics, RectF(x + tempWidth + 12, y, tempWidth, 104), localized(L"CARTE GRAPHIQUE", L"GRAPHICS CARD", L"GRAFIKKARTE", L"显卡"), g_gpuTemperature,
                    localized(L"Température GPU", L"GPU temperature", L"GPU-Temperatur", L"GPU 温度"));
    RectF detection(x + (tempWidth + 12) * 2, y, tempWidth, 104);
    fillRound(graphics, detection, 17, Color(255, 28, 32, 45));
    strokeRound(graphics, detection, 17, Color(255, 39, 45, 61));
    text(graphics, localized(L"DÉTECTION AUTOMATIQUE", L"AUTOMATIC DETECTION", L"AUTOMATISCHE ERKENNUNG", L"自动检测"), RectF(detection.X + 17, detection.Y + 15, detection.Width - 34, 16), 9, Color(255, 117, 128, 151), FontStyleBold);
    text(graphics, std::to_wstring(g_fans.size()) + localized(L" ventilateur(s)", L" fan(s)", L" Lüfter", L" 个风扇"), RectF(detection.X + 17, detection.Y + 41, detection.Width - 34, 24), 15, Color::White, FontStyleBold);
    text(graphics, g_isAdministrator ? localized(L"Accès administrateur", L"Administrator access", L"Administratorzugriff", L"管理员权限")
                                     : localized(L"Accès standard", L"Standard access", L"Standardzugriff", L"标准权限"),
         RectF(detection.X + 17, detection.Y + 73, detection.Width - 34, 16), 9, Color(255, 117, 205, 180));
    y += 122;
    RectF profiles(x, y, available, 76);
    fillRound(graphics, profiles, 16, Color(255, 23, 27, 39));
    strokeRound(graphics, profiles, 16, Color(255, 43, 49, 65));
    text(graphics, localized(L"Profils rapides", L"Quick profiles", L"Schnellprofile", L"快捷模式"), RectF(profiles.X + 18, profiles.Y + 16, 190, 20), 13, Color::White, FontStyleBold);
    text(graphics, localized(L"Action explicite sur tous les canaux", L"Applies to every controllable channel", L"Gilt für alle steuerbaren Kanäle", L"应用到所有可控通道"), RectF(profiles.X + 18, profiles.Y + 42, 260, 16), 9, Color(255, 127, 136, 157));
    float bx = profiles.GetRight() - 450;
    drawButton(graphics, RectF(bx, profiles.Y + 19, 100, 38), localized(L"Auto", L"Auto", L"Auto", L"自动"),
               g_fanProfile == FanProfile::Auto, Action::FanProfileAuto);
    drawButton(graphics, RectF(bx + 108, profiles.Y + 19, 100, 38), localized(L"Silencieux", L"Quiet", L"Leise", L"静音"),
               g_fanProfile == FanProfile::Quiet, Action::FanProfileQuiet);
    drawButton(graphics, RectF(bx + 216, profiles.Y + 19, 100, 38), localized(L"Équilibré", L"Balanced", L"Ausgeglichen", L"均衡"),
               g_fanProfile == FanProfile::Balanced, Action::FanProfileBalanced);
    drawButton(graphics, RectF(bx + 324, profiles.Y + 19, 108, 38), localized(L"Performance", L"Performance", L"Leistung", L"性能"),
               g_fanProfile == FanProfile::Performance, Action::FanProfilePerformance);
    y += 98;

    RectF curveCard(x, y, available, 202);
    fillRound(graphics, curveCard, 18, Color(255, 23, 27, 39));
    strokeRound(graphics, curveCard, 18, g_fanCurveEnabled ? Color(255, 76, 151, 132) : Color(255, 43, 49, 65), g_fanCurveEnabled ? 1.4f : 1.0f);
    text(graphics, localized(L"Courbe intelligente", L"Smart fan curve", L"Intelligente Lüfterkurve", L"智能风扇曲线"),
         RectF(curveCard.X + 18, curveCard.Y + 15, 260, 22), 14, Color::White, FontStyleBold);
    text(graphics, localized(L"La température la plus haute du CPU ou du GPU pilote tous les canaux.",
                             L"The highest CPU or GPU temperature drives every channel.",
                             L"Die höchste CPU- oder GPU-Temperatur steuert alle Kanäle.",
                             L"CPU 或 GPU 的最高温度将控制所有通道。"),
         RectF(curveCard.X + 18, curveCard.Y + 38, curveCard.Width - 220, 18), 9, Color(255, 127, 136, 157));
    drawButton(graphics, RectF(curveCard.GetRight() - 174, curveCard.Y + 14, 156, 40),
               g_fanCurveEnabled ? localized(L"Désactiver", L"Disable", L"Deaktivieren", L"禁用")
                                 : localized(L"Activer la courbe", L"Enable curve", L"Kurve aktivieren", L"启用曲线"),
               g_fanCurveEnabled, Action::FanCurveToggle);

    const RectF graph(curveCard.X + 28, curveCard.Y + 76, curveCard.Width - 300, 100);
    fillRound(graphics, graph, 12, Color(255, 15, 19, 28));
    Pen grid(Color(255, 42, 48, 64), 1.0f);
    for (int line = 1; line < 4; ++line) {
        const float gy = graph.Y + graph.Height * line / 4.0f;
        graphics.DrawLine(&grid, graph.X + 10, gy, graph.GetRight() - 10, gy);
    }
    constexpr std::array<int, 4> curveTemperatures{35, 50, 70, 85};
    std::array<PointF, 4> points{};
    for (int index = 0; index < 4; ++index) {
        const float ratioX = (curveTemperatures[index] - 35) / 50.0f;
        const float ratioY = (g_curveSpeeds[index] - 30) / 70.0f;
        points[index] = PointF(graph.X + 16 + ratioX * (graph.Width - 32), graph.GetBottom() - 14 - ratioY * (graph.Height - 28));
    }
    Pen curveLine(Color(255, 119, 96, 242), 3.0f);
    graphics.DrawLines(&curveLine, points.data(), static_cast<INT>(points.size()));
    for (int index = 0; index < 4; ++index) {
        SolidBrush glow(Color(75, 139, 108, 255));
        graphics.FillEllipse(&glow, RectF(points[index].X - 10, points[index].Y - 10, 20, 20));
        SolidBrush point(Color(255, 151, 126, 255));
        graphics.FillEllipse(&point, RectF(points[index].X - 6, points[index].Y - 6, 12, 12));
        Pen pointBorder(Color::White, 1.5f);
        graphics.DrawEllipse(&pointBorder, RectF(points[index].X - 6, points[index].Y - 6, 12, 12));
        text(graphics, std::to_wstring(g_curveSpeeds[index]) + L"%", RectF(points[index].X - 25, points[index].Y - 28, 50, 16),
             8, Color(255, 213, 206, 255), FontStyleBold, StringAlignmentCenter);
        text(graphics, std::to_wstring(curveTemperatures[index]) + L"°", RectF(points[index].X - 22, graph.GetBottom() + 3, 44, 15),
             8, Color(255, 117, 128, 151), FontStyleBold, StringAlignmentCenter);
        addHit(RectF(points[index].X - 18, graph.Y - 4, 36, graph.Height + 8), Action::FanCurvePoint, index);
    }

    const float controlsX = graph.GetRight() + 24;
    text(graphics, localized(L"Réponse", L"Response", L"Reaktion", L"响应"), RectF(controlsX, curveCard.Y + 76, 190, 17),
         9, Color(255, 117, 128, 151), FontStyleBold);
    const float presetWidth = (curveCard.GetRight() - 18 - controlsX - 12) / 3.0f;
    drawButton(graphics, RectF(controlsX, curveCard.Y + 98, presetWidth, 34), localized(L"Calme", L"Quiet", L"Leise", L"静音"),
               g_curvePreset == 0, Action::FanCurvePreset, 0);
    drawButton(graphics, RectF(controlsX + presetWidth + 6, curveCard.Y + 98, presetWidth, 34), localized(L"Équilibre", L"Balanced", L"Balance", L"均衡"),
               g_curvePreset == 1, Action::FanCurvePreset, 1);
    drawButton(graphics, RectF(controlsX + (presetWidth + 6) * 2, curveCard.Y + 98, presetWidth, 34), localized(L"Frais", L"Cool", L"Kühl", L"冷却"),
               g_curvePreset == 2, Action::FanCurvePreset, 2);
    const double hottest = std::max(g_cpuTemperature, g_gpuTemperature);
    const std::wstring curveReading = hottest >= 0
        ? std::to_wstring(static_cast<int>(std::lround(hottest))) + L" °C  →  " + std::to_wstring(fanCurveTarget(hottest)) + L" %"
        : L"-- °C  →  -- %";
    text(graphics, curveReading, RectF(controlsX, curveCard.Y + 148, curveCard.GetRight() - controlsX - 18, 24),
         14, g_fanCurveEnabled ? Color(255, 111, 224, 181) : Color(255, 189, 195, 210), FontStyleBold, StringAlignmentCenter);
    y += 224;

    text(graphics, localized(L"Ventilateurs détectés", L"Detected fans", L"Erkannte Lüfter", L"已检测风扇"), RectF(x, y, 260, 20), 13, Color(255, 220, 224, 234), FontStyleBold);
    text(graphics, g_fanStatus, RectF(width - 500.0f, y, 460, 20), 9, Color(255, 127, 205, 180), FontStyleRegular, StringAlignmentFar);
    y += 29;
    if (g_fans.empty()) {
        RectF empty(x, y, available, 88);
        fillRound(graphics, empty, 16, Color(255, 23, 27, 37));
        strokeRound(graphics, empty, 16, Color(255, 43, 49, 65));
        textWrapped(graphics, localized(L"Aucun canal exposé. Le mode avancé peut révéler les prises de la carte mère.",
                                        L"No channel is exposed. Advanced mode may reveal motherboard headers.",
                                        L"Kein Kanal verfügbar. Der erweiterte Modus kann Mainboard-Anschlüsse erkennen.",
                                        L"未发现可用通道。高级模式可能检测到主板接口。"),
                    RectF(empty.X + 20, empty.Y, empty.Width - 40, empty.Height), 11, Color(255, 142, 150, 171), FontStyleRegular, StringAlignmentCenter);
        y += 102;
    } else {
        float cardWidth = (available - 24) / 3.0f;
        for (int index = 0; index < static_cast<int>(g_fans.size()); ++index) {
            FanDevice& fan = g_fans[index];
            const bool busy = g_busyFans.contains(fan.encodedId);
            int column = index % 3;
            int row = index / 3;
            RectF card(x + column * (cardWidth + 12), y + row * 226, cardWidth, 212);
            fillRound(graphics, card, 17, Color(255, 28, 32, 45));
            strokeRound(graphics, card, 17, fan.manual ? Color(255, 99, 81, 174) : Color(255, 42, 48, 64));
            text(graphics, fan.name, RectF(card.X + 17, card.Y + 15, card.Width - 90, 20), 13, Color::White, FontStyleBold);
            text(graphics, busy ? localized(L"EN COURS", L"WORKING", L"AKTIV", L"处理中")
                                : fan.manual ? localized(L"MANUEL", L"MANUAL", L"MANUELL", L"手动")
                                      : (fan.controllable ? L"AUTO" : localized(L"LECTURE", L"READ ONLY", L"NUR LESEN", L"只读")), RectF(card.GetRight() - 92, card.Y + 16, 75, 16), 9,
                 busy || fan.manual ? Color(255, 200, 187, 255) : Color(255, 114, 225, 179), FontStyleBold, StringAlignmentFar);
            text(graphics, fan.hardware, RectF(card.X + 17, card.Y + 39, card.Width - 34, 17), 9, Color(255, 127, 136, 157));
            std::wstring rpm = fan.rpm >= 0 ? std::to_wstring(static_cast<int>(std::lround(fan.rpm))) + L" RPM"
                                            : localized(L"RPM indisponible", L"RPM unavailable", L"Drehzahl unbekannt", L"转速不可用");
            text(graphics, rpm, RectF(card.X + 17, card.Y + 67, card.Width - 34, 24), 17, Color::White, FontStyleBold);
            if (fan.controllable) {
                text(graphics, std::to_wstring(fan.desired) + L" %", RectF(card.GetRight() - 80, card.Y + 69, 62, 20), 10, Color(255, 200, 187, 255), FontStyleBold, StringAlignmentFar);
                drawSlider(graphics, RectF(card.X + 17, card.Y + 99, card.Width - 34, 28), fan.desired, fan.minimum, Action::FanSlider, index);
                text(graphics, std::wstring(localized(L"Minimum sûr : ", L"Safe minimum: ", L"Sicheres Minimum: ", L"安全下限：")) + std::to_wstring(fan.minimum) + L" %", RectF(card.X + 17, card.Y + 130, card.Width - 34, 16), 9, Color(255, 111, 120, 142));
                drawButton(graphics, RectF(card.X + 17, card.Y + 159, (card.Width - 44) / 2, 36),
                           busy ? L"..." : localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true, Action::FanApply, index, !busy);
                drawButton(graphics, RectF(card.X + 27 + (card.Width - 44) / 2, card.Y + 159, (card.Width - 44) / 2, 36),
                           localized(L"Auto", L"Auto", L"Auto", L"自动"), false, Action::FanAuto, index, !busy);
            } else {
                textWrapped(graphics, localized(L"Ce capteur n'expose pas de commande logicielle.", L"This sensor exposes no software control.",
                                                L"Dieser Sensor bietet keine Softwaresteuerung.", L"此传感器不支持软件控制。"),
                            RectF(card.X + 17, card.Y + 108, card.Width - 34, 46), 10, Color(255, 142, 150, 171));
            }
        }
        y += static_cast<float>((g_fans.size() + 2) / 3) * 226;
    }
    RectF warning(x, y + 4, available, 48);
    fillRound(graphics, warning, 12, Color(255, 36, 31, 22));
    strokeRound(graphics, warning, 12, Color(255, 80, 68, 42));
    text(graphics, localized(L"Sécurité : minimum 30 % (50 % pour une pompe). Le mode Auto est restauré à la fermeture.",
                             L"Safety: 30% minimum (50% for a pump). Auto mode is restored when the app closes.",
                             L"Sicherheit: mindestens 30 % (50 % bei Pumpen). Auto wird beim Beenden wiederhergestellt.",
                             L"安全：风扇最低 30%（水泵 50%），关闭应用时恢复自动模式。"),
         RectF(warning.X + 15, warning.Y, warning.Width - 30, warning.Height), 9, Color(255, 200, 174, 114), FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
    g_maxScroll = std::max(0.0f, warning.GetBottom() + g_scrollOffset + 24 - height);
}

const wchar_t* profileName(int index) {
    if (index == 0) return localized(L"Gaming", L"Gaming", L"Gaming", L"游戏");
    if (index == 1) return localized(L"Silence", L"Quiet", L"Leise", L"静音");
    return localized(L"Nuit", L"Night", L"Nacht", L"夜间");
}

const wchar_t* fanProfileLabel(FanProfile profile) {
    if (profile == FanProfile::Auto) return localized(L"Automatique", L"Automatic", L"Automatisch", L"自动");
    if (profile == FanProfile::Quiet) return localized(L"Silencieux", L"Quiet", L"Leise", L"静音");
    if (profile == FanProfile::Balanced) return localized(L"Équilibré", L"Balanced", L"Ausgeglichen", L"均衡");
    if (profile == FanProfile::Performance) return localized(L"Performance", L"Performance", L"Leistung", L"性能");
    return localized(L"Personnalisé", L"Custom", L"Benutzerdefiniert", L"自定义");
}

void drawProfiles(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, available,
                  localized(L"PROFILS", L"PROFILES", L"PROFILE", L"模式"),
                  localized(L"Change de style en un clic.", L"Change the mood in one click.",
                            L"Wechsle den Stil mit einem Klick.", L"一键切换灯光风格。"));
    text(graphics, localized(L"Chaque profil conserve le RGB, l'effet, les appareils inclus et la ventilation.",
                             L"Each profile keeps RGB, effect, included devices and fan settings.",
                             L"Jedes Profil speichert RGB, Effekt, Geräte und Lüftereinstellungen.",
                             L"每个模式都会保存 RGB、效果、设备和风扇设置。"),
         RectF(x, originY + 61, available, 20), 11, Color(255, 137, 146, 167));

    float y = originY + 104;
    const float gap = 12.0f;
    const float cardWidth = (available - gap * 2) / 3.0f;
    for (int index = 0; index < 3; ++index) {
        const AppProfile& profile = g_profiles[index];
        const bool active = g_activeProfile == index;
        RectF card(x + index * (cardWidth + gap), y, cardWidth, 250);
        fillRound(graphics, card, 18, active ? Color(255, 36, 31, 57) : Color(255, 28, 32, 45));
        strokeRound(graphics, card, 18, active ? Color(255, 128, 104, 232) : Color(255, 43, 49, 65), active ? 1.6f : 1.0f);
        fillRound(graphics, RectF(card.X + 18, card.Y + 18, 48, 48), 14, profile.saved ? rgbColor(profile.color) : Color(255, 38, 43, 58));
        text(graphics, std::to_wstring(index + 1), RectF(card.X + 18, card.Y + 18, 48, 48), 13, Color::White,
             FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, profileName(index), RectF(card.X + 80, card.Y + 18, card.Width - 98, 24), 16, Color::White, FontStyleBold);
        text(graphics, profile.saved ? localized(L"PRÊT", L"READY", L"BEREIT", L"就绪") : localized(L"EMPLACEMENT VIDE", L"EMPTY SLOT", L"LEERER PLATZ", L"空位"),
             RectF(card.X + 80, card.Y + 46, card.Width - 98, 16), 9,
             profile.saved ? Color(255, 105, 221, 178) : Color(255, 128, 137, 158), FontStyleBold);
        if (profile.saved) {
            text(graphics, localized(L"Couleur", L"Color", L"Farbe", L"颜色"), RectF(card.X + 18, card.Y + 88, 90, 16), 9, Color(255, 116, 126, 148));
            text(graphics, hexColor(profile.color), RectF(card.GetRight() - 116, card.Y + 86, 98, 18), 10, Color(255, 225, 228, 237), FontStyleBold, StringAlignmentFar);
            text(graphics, localized(L"Effet", L"Effect", L"Effekt", L"效果"), RectF(card.X + 18, card.Y + 116, 90, 16), 9, Color(255, 116, 126, 148));
            text(graphics, kEffects[profile.effect].labels[static_cast<int>(g_language)], RectF(card.X + 92, card.Y + 114, card.Width - 110, 18), 10, Color(255, 225, 228, 237), FontStyleBold, StringAlignmentFar);
            text(graphics, localized(L"Ventilation", L"Cooling", L"Lüfter", L"风扇"), RectF(card.X + 18, card.Y + 144, 100, 16), 9, Color(255, 116, 126, 148));
            const std::wstring cooling = profile.fanCurveEnabled ? localized(L"Courbe intelligente", L"Smart curve", L"Intelligente Kurve", L"智能曲线") : fanProfileLabel(profile.fanProfile);
            text(graphics, cooling, RectF(card.X + 112, card.Y + 142, card.Width - 130, 18), 10, Color(255, 225, 228, 237), FontStyleBold, StringAlignmentFar);
        } else {
            textWrapped(graphics, localized(L"Configure ton setup puis enregistre son état ici.", L"Configure your setup, then save its state here.",
                                            L"Konfiguriere dein Setup und speichere es hier.", L"配置设备后在此保存。"),
                        RectF(card.X + 22, card.Y + 92, card.Width - 44, 70), 11, Color(255, 137, 146, 167), FontStyleRegular, StringAlignmentCenter);
        }
        drawButton(graphics, RectF(card.X + 18, card.GetBottom() - 56, (card.Width - 46) / 2, 38),
                   localized(L"Enregistrer", L"Save current", L"Speichern", L"保存"), false, Action::ProfileSave, index);
        drawButton(graphics, RectF(card.X + 28 + (card.Width - 46) / 2, card.GetBottom() - 56, (card.Width - 46) / 2, 38),
                   localized(L"Appliquer", L"Apply", L"Anwenden", L"应用"), true, Action::ProfileLoad, index, profile.saved);
        if (profile.saved) {
            RectF remove(card.GetRight() - 40, card.Y + 14, 24, 24);
            text(graphics, L"×", remove, 17, Color(255, 172, 179, 196), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
            addHit(remove, Action::ProfileDelete, index);
        }
    }

    y += 270;
    RectF transfer(x, y, available, 118);
    fillRound(graphics, transfer, 17, Color(255, 23, 27, 39));
    strokeRound(graphics, transfer, 17, Color(255, 43, 49, 65));
    text(graphics, localized(L"Sauvegarde portable", L"Portable backup", L"Portable Sicherung", L"便携备份"), RectF(transfer.X + 20, transfer.Y + 17, 280, 22), 14, Color::White, FontStyleBold);
    textWrapped(graphics, localized(L"Exporte la configuration actuelle dans un fichier, puis restaure-la sur ce PC ou un autre.",
                                    L"Export the current configuration to a file, then restore it on this PC or another one.",
                                    L"Exportiere die aktuelle Konfiguration und stelle sie auf diesem oder einem anderen PC wieder her.",
                                    L"将当前配置导出为文件，然后在本机或其他电脑上恢复。"),
                RectF(transfer.X + 20, transfer.Y + 46, transfer.Width - 440, 48), 10, Color(255, 137, 146, 167));
    drawButton(graphics, RectF(transfer.GetRight() - 400, transfer.Y + 38, 184, 44), localized(L"Exporter", L"Export", L"Exportieren", L"导出"),
               false, Action::ProfileExport);
    drawButton(graphics, RectF(transfer.GetRight() - 204, transfer.Y + 38, 184, 44), localized(L"Importer", L"Import", L"Importieren", L"导入"),
               true, Action::ProfileImport);
    const std::wstring status = g_profileStatus.empty()
        ? localized(L"Tes profils sont conservés après la fermeture de l'application.", L"Your profiles remain saved after the app closes.",
                    L"Deine Profile bleiben nach dem Beenden gespeichert.", L"关闭应用后模式仍会保留。")
        : g_profileStatus;
    text(graphics, status, RectF(x, transfer.GetBottom() + 15, available, 20), 10, Color(255, 110, 217, 177), FontStyleBold);
    g_maxScroll = std::max(0.0f, transfer.GetBottom() + g_scrollOffset + 52 - height);
}

std::wstring diagnosticReport() {
    std::wostringstream report;
    report << L"RGBCcontrol " << kAppVersion << L"\r\n";
    report << L"OpenRGB: " << (g_openRgbReady ? L"OK" : L"Unavailable") << L"\r\n";
    report << L"Provider API: " << kDeviceProviderApiVersion << L"\r\n";
    report << L"Plugin engine: out-of-process + SHA-256 verification\r\n";
    report << L"Plugin manifests: " << g_pluginProviders.size() << L" | rejected=" << g_rejectedPlugins << L"\r\n";
    for (const PluginProviderStatus& provider : g_pluginProviders) {
        report << L"  - plugin " << provider.id << L" | " << provider.name << L" | " << provider.transport
               << L" | valid=" << (provider.valid ? L"yes" : L"no")
               << L" | devices=" << provider.matchedDevices << L" | " << provider.state << L"\r\n";
    }
    report << L"Windows hot-plug: " << (g_hotplugMonitoring ? L"Active" : L"Unavailable") << L"\r\n";
    report << L"Fallback scan interval: " << g_detectionIntervalSeconds << L" seconds\r\n";
    report << L"Administrator: " << (g_isAdministrator ? L"Yes" : L"No") << L"\r\n";
    const std::wstring rgbConflict = rgbConflictProcess();
    report << L"Competing RGB engine: " << (rgbConflict.empty() ? L"No" : rgbConflict) << L"\r\n";
    report << L"Empty ARGB zone policy: initialize to 120 LEDs within controller limits\r\n";
    report << L"RGB certification: local visual confirmation of red + green + blue for the exact device fingerprint\r\n";
    report << L"RGB controllers: " << g_rgbDevices.size() << L"\r\n";
    for (const RgbDevice& device : g_rgbDevices) {
        const LocalCertification certification = localCertification(device);
        report << L"  - " << device.name << L" | " << device.vendor << L" | " << device.type
               << L" | provider=" << device.providerId << L" | zones=" << device.zones << L" | leds=" << device.leds
               << L" | local_validation=" << (certification == LocalCertification::Passed ? L"PASSED"
                                                : certification == LocalCertification::Failed ? L"FAILED" : L"UNTESTED");
        if (device.providerId == L"openrgb") {
            const RgbFrameTransport transport = device.frameTransport == RgbFrameTransport::Automatic
                ? (OpenRgbClient::prefersAtomicFrames(device) ? RgbFrameTransport::AtomicDevice : RgbFrameTransport::PerZone)
                : device.frameTransport;
            report << L" | frame_transport=" << (transport == RgbFrameTransport::AtomicDevice ? L"atomic-device" : L"per-zone")
                   << L" | color_order=" << colorOrderName(device.colorOrder)
                   << L" | frame_interval_ms=" << device.frameIntervalMs
                   << L" | preferred_leds=" << device.preferredLeds;
        }
        report << L"\r\n";
        if (!device.modes.empty()) {
            report << L"    modes=";
            for (std::size_t mode = 0; mode < device.modes.size(); ++mode) {
                if (mode) report << L", ";
                report << device.modes[mode];
            }
            report << L"\r\n";
        }
        for (std::size_t zone = 0; zone < device.zoneDetails.size(); ++zone) {
            const RgbZone& detail = device.zoneDetails[zone];
            report << L"    zone[" << zone << L"]=" << detail.name
                   << L" | leds=" << detail.leds << L" | min=" << detail.minimumLeds
                   << L" | max=" << detail.maximumLeds << L" | flags=0x"
                   << std::hex << std::uppercase << detail.flags << std::dec << L"\r\n";
        }
    }
    if (g_duckyDetected) report << L"  - Ducky One 2 Mini DKON1861ST | local firmware RGB only\r\n";
    report << L"Fans: " << g_fans.size() << L"\r\n";
    for (const FanDevice& fan : g_fans) {
        report << L"  - " << fan.name << L" | " << fan.hardware << L" | "
               << (fan.controllable ? L"controllable" : L"read-only") << L" | rpm=" << static_cast<int>(std::lround(fan.rpm)) << L"\r\n";
    }
    report << L"CPU=" << static_cast<int>(std::lround(g_cpuTemperature)) << L" C | GPU=" << static_cast<int>(std::lround(g_gpuTemperature)) << L" C\r\n";
    return report.str();
}

void copyDiagnosticReport() {
    const std::wstring report = diagnosticReport();
    if (!OpenClipboard(g_window)) return;
    EmptyClipboard();
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, (report.size() + 1) * sizeof(wchar_t));
    if (storage) {
        void* memory = GlobalLock(storage);
        if (memory) {
            std::memcpy(memory, report.c_str(), (report.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(storage);
            if (!SetClipboardData(CF_UNICODETEXT, storage)) GlobalFree(storage);
        } else {
            GlobalFree(storage);
        }
    }
    CloseClipboard();
    g_diagnosticStatus = localized(L"Rapport copié dans le presse-papiers.", L"Report copied to the clipboard.",
                                   L"Bericht in die Zwischenablage kopiert.", L"报告已复制到剪贴板。");
}

void drawDiagnostics(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, available - 300,
                  localized(L"DIAGNOSTIC", L"DIAGNOSTICS", L"DIAGNOSE", L"诊断"),
                  localized(L"Vérifie que tout fonctionne.", L"Make sure everything works.",
                            L"Prüfe, ob alles funktioniert.", L"确认所有设备正常工作。"));
    drawButton(graphics, RectF(width - 340.0f, originY + 12, 142, 40), localized(L"Actualiser", L"Refresh", L"Aktualisieren", L"刷新"), false, Action::Refresh);
    drawButton(graphics, RectF(width - 188.0f, originY + 12, 150, 40), localized(L"Copier le rapport", L"Copy report", L"Bericht kopieren", L"复制报告"), true, Action::DiagnosticCopy);

    float y = originY + 86;
    const int controllableFans = static_cast<int>(std::count_if(g_fans.begin(), g_fans.end(), [](const FanDevice& fan) { return fan.controllable; }));
    const std::array<std::wstring, 3> summaries{
        std::to_wstring(g_rgbDevices.size()) + localized(L" contrôleur(s) RGB", L" RGB controller(s)", L" RGB-Controller", L" 个 RGB 控制器"),
        std::to_wstring(controllableFans) + L" / " + std::to_wstring(g_fans.size()) + localized(L" ventilateurs", L" fans", L" Lüfter", L" 个风扇"),
        (g_openRgbReady || hasActivePluginDevice())
            ? localized(L"Moteurs RGB prêts", L"RGB engines ready", L"RGB-Engines bereit", L"RGB 引擎已就绪")
            : localized(L"Moteurs RGB indisponibles", L"RGB engines unavailable", L"RGB-Engines nicht verfügbar", L"RGB 引擎不可用")
    };
    const float summaryWidth = (available - 24) / 3.0f;
    for (int index = 0; index < 3; ++index) {
        RectF card(x + index * (summaryWidth + 12), y, summaryWidth, 84);
        fillRound(graphics, card, 16, Color(255, 28, 32, 45));
        strokeRound(graphics, card, 16, Color(255, 43, 49, 65));
        text(graphics, summaries[index], RectF(card.X + 17, card.Y + 18, card.Width - 34, 24), 14, Color::White, FontStyleBold);
        text(graphics, index == 0 ? L"OpenRGB + Plugins" : index == 1 ? L"LibreHardwareMonitor" : localized(L"Contrôle local", L"Local control", L"Lokale Steuerung", L"本地控制"),
             RectF(card.X + 17, card.Y + 50, card.Width - 34, 16), 9,
             index == 2 && !(g_openRgbReady || hasActivePluginDevice()) ? Color(255, 241, 183, 104) : Color(255, 105, 221, 178));
    }

    y += 106;
    text(graphics, localized(L"Périphériques RGB", L"RGB devices", L"RGB-Geräte", L"RGB 设备"), RectF(x, y, 260, 20), 13, Color(255, 220, 224, 234), FontStyleBold);
    y += 28;
    auto diagnosticRow = [&](const std::wstring& name, const std::wstring& detail, const std::wstring& state, bool ready) {
        RectF row(x, y, available, 68);
        fillRound(graphics, row, 14, Color(255, 24, 28, 40));
        strokeRound(graphics, row, 14, Color(255, 42, 48, 64));
        SolidBrush dot(ready ? Color(255, 82, 220, 164) : Color(255, 240, 180, 93));
        graphics.FillEllipse(&dot, RectF(row.X + 18, row.Y + 29, 9, 9));
        text(graphics, name, RectF(row.X + 41, row.Y + 12, row.Width * 0.44f, 21), 12, Color::White, FontStyleBold);
        text(graphics, detail, RectF(row.X + 41, row.Y + 37, row.Width * 0.55f, 17), 9, Color(255, 126, 136, 158));
        text(graphics, state, RectF(row.GetRight() - 310, row.Y + 12, 290, 44), 10,
             ready ? Color(255, 105, 221, 178) : Color(255, 241, 183, 104), FontStyleBold, StringAlignmentFar, StringAlignmentCenter);
        y += 76;
    };
    if (g_rgbDevices.empty() && !g_duckyDetected) {
        diagnosticRow(localized(L"Aucun appareil RGB", L"No RGB device", L"Kein RGB-Gerät", L"无 RGB 设备"),
                      localized(L"Relance une analyse après connexion.", L"Run a new scan after connecting it.", L"Nach dem Anschließen erneut suchen.", L"连接后重新扫描。"),
                      localized(L"NON DÉTECTÉ", L"NOT DETECTED", L"NICHT ERKANNT", L"未检测"), false);
    }
    for (const RgbDevice& device : g_rgbDevices) {
        const LocalCertification certification = localCertification(device);
        const bool certified = certification == LocalCertification::Passed;
        diagnosticRow(device.name, device.vendor + L" · " + localizedDeviceType(device.type) + L" · " + std::to_wstring(device.zones) + localized(L" zone(s)", L" zone(s)", L" Zone(n)", L" 个区域"),
                      certified
                          ? localized(L"CERTIFIÉ SUR CE PC", L"CERTIFIED ON THIS PC", L"AUF DIESEM PC ZERTIFIZIERT", L"已在此电脑上认证")
                          : certification == LocalCertification::Failed
                              ? localized(L"ÉCHEC RGB CONFIRMÉ", L"CONFIRMED RGB FAILURE", L"RGB-FEHLER BESTÄTIGT", L"已确认 RGB 失败")
                              : PluginEngine::isPluginDevice(device)
                                  ? localized(L"PLUGIN VALIDÉ · TEST RGB REQUIS", L"VERIFIED PLUGIN · RGB TEST REQUIRED", L"PLUGIN GEPRÜFT · RGB-TEST NÖTIG", L"插件已验证 · 需要 RGB 测试")
                                  : localized(L"DÉTECTÉ · TEST RGB REQUIS", L"DETECTED · RGB TEST REQUIRED", L"ERKANNT · RGB-TEST NÖTIG", L"已检测 · 需要 RGB 测试"), certified);
    }
    if (g_duckyDetected) {
        diagnosticRow(L"Ducky One 2 Mini", L"DKON1861ST · " + std::wstring(localized(L"firmware officiel conservé", L"official firmware preserved", L"offizielle Firmware bleibt erhalten", L"保留官方固件")),
                      localized(L"DÉTECTÉ · RGB LOCAL", L"DETECTED · LOCAL RGB", L"ERKANNT · LOKALES RGB", L"已检测 · 本地 RGB"), false);
    }

    y += 6;
    text(graphics, localized(L"Ventilation et capteurs", L"Fans and sensors", L"Lüfter und Sensoren", L"风扇和传感器"), RectF(x, y, 280, 20), 13, Color(255, 220, 224, 234), FontStyleBold);
    y += 28;
    if (g_fans.empty()) {
        diagnosticRow(localized(L"Aucun canal de ventilation", L"No fan channel", L"Kein Lüfterkanal", L"无风扇通道"),
                      localized(L"Le BIOS ou le contrôleur peut ne pas exposer ses capteurs.", L"The BIOS or controller may not expose its sensors.",
                                L"BIOS oder Controller stellen eventuell keine Sensoren bereit.", L"BIOS 或控制器可能未公开传感器。"),
                      localized(L"NON DÉTECTÉ", L"NOT DETECTED", L"NICHT ERKANNT", L"未检测"), false);
    }
    for (const FanDevice& fan : g_fans) {
        const std::wstring rpm = fan.rpm >= 0 ? std::to_wstring(static_cast<int>(std::lround(fan.rpm))) + L" RPM" : L"RPM --";
        diagnosticRow(fan.name, fan.hardware + L" · " + rpm,
                      fan.controllable ? localized(L"CONTRÔLABLE", L"CONTROLLABLE", L"STEUERBAR", L"可控")
                                       : localized(L"LECTURE SEULE", L"READ ONLY", L"NUR LESEN", L"只读"), fan.controllable);
    }
    if (!g_diagnosticStatus.empty()) {
        text(graphics, g_diagnosticStatus, RectF(x, y + 3, available, 20), 10, Color(255, 105, 221, 178), FontStyleBold);
        y += 28;
    }
    g_maxScroll = std::max(0.0f, y + g_scrollOffset + 20 - height);
}

void drawSettings(Graphics& graphics, int width, int height, float originY) {
    const float x = kSidebarWidth + 32.0f;
    const float available = width - x - 32.0f;
    drawPageIntro(graphics, x, originY, available,
                  localized(L"PARAMÈTRES", L"SETTINGS", L"EINSTELLUNGEN", L"设置"),
                  localized(L"Une application à ton image.", L"Make the app feel like yours.",
                            L"Eine App, die zu dir passt.", L"打造属于你的应用。"));
    text(graphics, localized(L"Tous les réglages sont enregistrés automatiquement pour cet utilisateur.",
                             L"All settings are saved automatically for this user.",
                             L"Alle Einstellungen werden automatisch für diesen Benutzer gespeichert.",
                             L"所有设置都会自动为当前用户保存。"),
         RectF(x, originY + 61, available, 20), 11, Color(255, 137, 146, 167));

    float y = originY + 102;
    RectF languageCard(x, y, available, 174);
    fillRound(graphics, languageCard, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, languageCard, 18, Color(255, 39, 45, 61));
    text(graphics, localized(L"Langue de l'application", L"Application language", L"Sprache der Anwendung", L"应用语言"),
         RectF(languageCard.X + 20, languageCard.Y + 17, available - 40, 22), 15, Color::White, FontStyleBold);
    text(graphics, localized(L"Choisis la langue utilisée dans toute l'interface.", L"Choose the language used throughout the interface.",
                             L"Wähle die Sprache der gesamten Oberfläche.", L"选择整个界面使用的语言。"),
         RectF(languageCard.X + 20, languageCard.Y + 42, available - 40, 18), 10, Color(255, 137, 146, 167));

    const wchar_t* names[] = {L"Français", L"English", L"Deutsch", L"简体中文"};
    const wchar_t* codes[] = {L"FR", L"EN", L"DE", L"中文"};
    const float optionGap = 10.0f;
    const float optionWidth = (available - 40 - optionGap * 3) / 4.0f;
    for (int index = 0; index < 4; ++index) {
        const bool selected = index == static_cast<int>(g_language);
        RectF option(languageCard.X + 20 + index * (optionWidth + optionGap), languageCard.Y + 76, optionWidth, 76);
        fillRound(graphics, option, 13, selected ? Color(255, 45, 38, 72) : Color(255, 22, 26, 37));
        strokeRound(graphics, option, 13, selected ? accentColor() : Color(255, 43, 49, 65), selected ? 1.5f : 1.0f);
        fillRound(graphics, RectF(option.X + 12, option.Y + 17, 38, 38), 11, selected ? accentColor() : Color(255, 34, 40, 55));
        text(graphics, codes[index], RectF(option.X + 12, option.Y + 17, 38, 38), index == 3 ? 10 : 11,
             Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        text(graphics, names[index], RectF(option.X + 61, option.Y + 17, option.Width - 72, 38), 12,
             selected ? Color::White : Color(255, 196, 202, 216), selected ? FontStyleBold : FontStyleRegular,
             StringAlignmentNear, StringAlignmentCenter);
        addHit(option, Action::SelectLanguage, index);
    }

    y = languageCard.GetBottom() + 18;
    RectF updateCard(x, y, available, 174);
    fillRound(graphics, updateCard, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, updateCard, 18, Color(255, 39, 45, 61));
    text(graphics, localized(L"Mises à jour", L"Updates", L"Updates", L"软件更新"),
         RectF(updateCard.X + 20, updateCard.Y + 18, 300, 24), 16, Color::White, FontStyleBold);
    text(graphics, std::wstring(localized(L"Version installée : ", L"Installed version: ", L"Installierte Version: ", L"当前版本：")) + kAppVersion,
         RectF(updateCard.X + 20, updateCard.Y + 47, 360, 19), 10, Color(255, 143, 152, 174));
    const std::wstring updateMessage = g_updateStatus.empty()
        ? localized(L"Recherche et téléchargement sécurisés via le canal officiel.",
                    L"Secure checking and downloading through the official channel.",
                    L"Sichere Suche und Downloads über den offiziellen Kanal.",
                    L"通过官方通道安全检查并下载更新。")
        : g_updateStatus;
    textWrapped(graphics, updateMessage, RectF(updateCard.X + 20, updateCard.Y + 80, updateCard.Width - 260, 62),
                11, g_downloadedUpdate.empty() ? Color(255, 173, 180, 197) : Color(255, 103, 225, 178));
    const std::wstring updateLabel = g_updateInFlight
        ? localized(L"Mise à jour en cours...", L"Updating...", L"Update läuft...", L"正在更新…")
        : !g_downloadedUpdate.empty()
            ? localized(L"Relancer l'installation", L"Retry installation", L"Installation wiederholen", L"重试安装")
            : localized(L"Mettre à jour", L"Update now", L"Jetzt aktualisieren", L"立即更新");
    drawButton(graphics, RectF(updateCard.GetRight() - 220, updateCard.Y + 80, 200, 46), updateLabel, true,
               Action::Update, -1, !g_updateInFlight);
    text(graphics, localized(L"Les fichiers sont vérifiés en SHA-256 avant installation.",
                             L"Files are SHA-256 verified before installation.",
                             L"Dateien werden vor der Installation mit SHA-256 geprüft.",
                             L"安装前会进行 SHA-256 校验。"),
         RectF(updateCard.X + 20, updateCard.GetBottom() - 26, updateCard.Width - 40, 17), 9, Color(255, 111, 120, 142));

    y = updateCard.GetBottom() + 18;
    RectF preferenceCard(x, y, available, 360);
    fillRound(graphics, preferenceCard, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, preferenceCard, 18, Color(255, 39, 45, 61));
    text(graphics, localized(L"Personnalisation avancée", L"Advanced customization", L"Erweiterte Anpassung", L"高级自定义"),
         RectF(preferenceCard.X + 20, preferenceCard.Y + 17, 330, 23), 15, Color::White, FontStyleBold);
    text(graphics, localized(L"Adapte l'interface, la détection et les performances à ton PC.",
                             L"Tune the interface, detection and performance for your PC.",
                             L"Passe Oberfläche, Erkennung und Leistung an deinen PC an.",
                             L"根据你的电脑调整界面、检测和性能。"),
         RectF(preferenceCard.X + 20, preferenceCard.Y + 42, preferenceCard.Width - 220, 18), 9, Color(255, 126, 136, 158));
    drawButton(graphics, RectF(preferenceCard.GetRight() - 164, preferenceCard.Y + 15, 144, 38),
               localized(L"Réinitialiser", L"Reset", L"Zurücksetzen", L"重置"), false, Action::ResetPreferences);

    const float optionX = preferenceCard.X + 188;
    const float optionArea = preferenceCard.GetRight() - optionX - 20;
    auto preferenceLabel = [&](float rowY, const wchar_t* label, const wchar_t* detail) {
        text(graphics, label, RectF(preferenceCard.X + 20, rowY + 2, 158, 18), 10, Color::White, FontStyleBold);
        text(graphics, detail, RectF(preferenceCard.X + 20, rowY + 22, 158, 16), 8, Color(255, 119, 129, 151));
    };

    float rowY = preferenceCard.Y + 70;
    preferenceLabel(rowY,
                    localized(L"Couleur d'accent", L"Accent color", L"Akzentfarbe", L"强调色"),
                    localized(L"Contrôles et sélections", L"Controls and selections", L"Steuerungen und Auswahl", L"控件和选择"));
    const wchar_t* accentNames[] = {
        localized(L"Violet", L"Purple", L"Violett", L"紫色"), localized(L"Bleu", L"Blue", L"Blau", L"蓝色"),
        localized(L"Cyan", L"Teal", L"Türkis", L"青色"), localized(L"Rose", L"Pink", L"Rosa", L"粉色"),
        localized(L"Rouge", L"Red", L"Rot", L"红色"), localized(L"Orange", L"Orange", L"Orange", L"橙色"),
        localized(L"Vert", L"Green", L"Grün", L"绿色"), localized(L"Blanc", L"White", L"Weiß", L"白色")
    };
    const float fourChoiceWidth = (optionArea - 24) / 4.0f;
    for (int index = 0; index < static_cast<int>(std::size(kAccentChoices)); ++index) {
        const int row = index / 4;
        const int column = index % 4;
        RectF option(optionX + column * (fourChoiceWidth + 8), rowY + row * 46, fourChoiceWidth, 40);
        fillRound(graphics, option, 11, index == g_accentPreset ? Color(255, 38, 43, 60) : Color(255, 21, 25, 36));
        strokeRound(graphics, option, 11, index == g_accentPreset ? rgbColor(kAccentChoices[index]) : Color(255, 43, 49, 65),
                    index == g_accentPreset ? 1.6f : 1.0f);
        SolidBrush swatch(rgbColor(kAccentChoices[index]));
        graphics.FillEllipse(&swatch, RectF(option.X + 11, option.Y + 10, 20, 20));
        text(graphics, accentNames[index], RectF(option.X + 39, option.Y, option.Width - 47, option.Height), 9,
             Color(255, 229, 232, 241), index == g_accentPreset ? FontStyleBold : FontStyleRegular,
             StringAlignmentNear, StringAlignmentCenter);
        addHit(option, Action::SelectAccent, index);
    }

    rowY += 104;
    preferenceLabel(rowY,
                    localized(L"Détection matérielle", L"Hardware detection", L"Hardware-Erkennung", L"硬件检测"),
                    localized(L"Recherche les changements", L"Looks for hardware changes", L"Sucht nach Änderungen", L"检测硬件变化"));
    const int scanIntervals[] = {5, 15, 30, 60};
    for (int index = 0; index < 4; ++index) {
        const std::wstring label = std::to_wstring(scanIntervals[index]) + L" s";
        drawButton(graphics, RectF(optionX + index * (fourChoiceWidth + 8), rowY, fourChoiceWidth, 42), label,
                   g_detectionIntervalSeconds == scanIntervals[index], Action::SelectScanInterval, index);
    }

    rowY += 58;
    preferenceLabel(rowY,
                    localized(L"Fluidité des effets", L"Effect smoothness", L"Effekt-Flüssigkeit", L"灯效流畅度"),
                    localized(L"Ajuste l'usage du processeur", L"Adjusts CPU usage", L"Passt die CPU-Last an", L"调整处理器占用"));
    const wchar_t* qualityNames[] = {
        localized(L"Éco", L"Eco", L"Eco", L"节能"),
        localized(L"Équilibré", L"Balanced", L"Ausgewogen", L"平衡"),
        localized(L"Ultra fluide", L"Ultra smooth", L"Sehr flüssig", L"超流畅")
    };
    const float threeChoiceWidth = (optionArea - 16) / 3.0f;
    for (int index = 0; index < 3; ++index) {
        drawButton(graphics, RectF(optionX + index * (threeChoiceWidth + 8), rowY, threeChoiceWidth, 42), qualityNames[index],
                   g_effectQuality.load() == index, Action::SelectEffectQuality, index);
    }

    rowY += 58;
    preferenceLabel(rowY,
                    localized(L"Confort visuel", L"Visual comfort", L"Visueller Komfort", L"视觉舒适度"),
                    localized(L"Comportement de l'interface", L"Interface behavior", L"Verhalten der Oberfläche", L"界面行为"));
    auto compactToggle = [&](const RectF& option, const wchar_t* label, bool enabled, Action action) {
        fillRound(graphics, option, 11, enabled ? Color(255, 36, 41, 58) : Color(255, 21, 25, 36));
        strokeRound(graphics, option, 11, enabled ? accentColor() : Color(255, 43, 49, 65));
        text(graphics, label, RectF(option.X + 13, option.Y, option.Width - 70, option.Height), 9, Color(255, 226, 229, 238),
             enabled ? FontStyleBold : FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
        RectF toggle(option.GetRight() - 52, option.Y + 9, 40, 24);
        fillRound(graphics, toggle, 12, enabled ? accentColor() : Color(255, 43, 48, 63));
        SolidBrush knob(Color::White);
        graphics.FillEllipse(&knob, RectF(enabled ? toggle.GetRight() - 20 : toggle.X + 4, toggle.Y + 4, 16, 16));
        addHit(option, action);
    };
    const float toggleWidth = (optionArea - 8) / 2.0f;
    compactToggle(RectF(optionX, rowY, toggleWidth, 42),
                  localized(L"Mémoriser la page", L"Remember last page", L"Letzte Seite merken", L"记住上次页面"),
                  g_rememberLastPage, Action::ToggleRememberPage);
    compactToggle(RectF(optionX + toggleWidth + 8, rowY, toggleWidth, 42),
                  localized(L"Réduire les animations", L"Reduce animations", L"Animationen reduzieren", L"减少动画"),
                  g_reduceMotion, Action::ToggleReduceMotion);

    y = preferenceCard.GetBottom() + 18;
    RectF windowsCard(x, y, available, 200);
    fillRound(graphics, windowsCard, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, windowsCard, 18, Color(255, 39, 45, 61));
    text(graphics, localized(L"Intégration Windows", L"Windows integration", L"Windows-Integration", L"Windows 集成"),
         RectF(windowsCard.X + 20, windowsCard.Y + 17, 300, 22), 15, Color::White, FontStyleBold);
    const float integrationWidth = (windowsCard.Width - 52) / 2.0f;
    RectF startupOption(windowsCard.X + 20, windowsCard.Y + 53, integrationWidth, 58);
    RectF trayOption(startupOption.GetRight() + 12, startupOption.Y, integrationWidth, startupOption.Height);
    RectF quietOption(startupOption.X, startupOption.GetBottom() + 10, integrationWidth, 58);
    RectF closeOption(trayOption.X, trayOption.GetBottom() + 10, integrationWidth, 58);
    auto integrationOption = [&](const RectF& option, const wchar_t* label, const wchar_t* detail, bool enabled, Action action) {
        fillRound(graphics, option, 13, enabled ? Color(255, 37, 42, 61) : Color(255, 22, 26, 37));
        strokeRound(graphics, option, 13, enabled ? accentColor() : Color(255, 43, 49, 65));
        text(graphics, label, RectF(option.X + 16, option.Y + 9, option.Width - 92, 20), 11, Color::White, FontStyleBold);
        text(graphics, detail, RectF(option.X + 16, option.Y + 31, option.Width - 92, 16), 8, Color(255, 126, 136, 158));
        fillRound(graphics, RectF(option.GetRight() - 66, option.Y + 15, 48, 28), 14,
                  enabled ? accentColor() : Color(255, 43, 48, 63));
        SolidBrush knob(Color::White);
        graphics.FillEllipse(&knob, RectF(enabled ? option.GetRight() - 42 : option.GetRight() - 62, option.Y + 19, 20, 20));
        addHit(option, action);
    };
    integrationOption(startupOption,
                      localized(L"Démarrer avec Windows", L"Start with Windows", L"Mit Windows starten", L"随 Windows 启动"),
                      localized(L"Ouverture discrète", L"Starts quietly", L"Startet im Hintergrund", L"静默启动"),
                      g_startupEnabled, Action::ToggleStartup);
    integrationOption(trayOption,
                      localized(L"Réduire dans la zone", L"Minimize to tray", L"In Infobereich minimieren", L"最小化到托盘"),
                      localized(L"Le bouton Réduire masque la fenêtre", L"Minimize hides the window", L"Minimieren blendet das Fenster aus", L"最小化时隐藏窗口"),
                      g_minimizeToTray, Action::ToggleMinimizeToTray);
    integrationOption(quietOption,
                      localized(L"Démarrage discret", L"Quiet startup", L"Stiller Start", L"静默启动"),
                      localized(L"Masque la fenêtre au démarrage Windows", L"Hides the window at Windows startup", L"Blendet das Fenster beim Windows-Start aus", L"Windows 启动时隐藏窗口"),
                      g_startupQuiet, Action::ToggleStartupQuiet);
    integrationOption(closeOption,
                      localized(L"Fermer dans la zone", L"Close to tray", L"In Infobereich schließen", L"关闭到托盘"),
                      localized(L"La croix garde RGBCcontrol actif", L"Close keeps RGBCcontrol running", L"Schließen lässt RGBCcontrol aktiv", L"关闭后继续运行"),
                      g_closeToTray, Action::ToggleCloseToTray);

    y = windowsCard.GetBottom() + 18;
    RectF scheduleCard(x, y, available, 246);
    fillRound(graphics, scheduleCard, 18, Color(255, 28, 32, 45));
    strokeRound(graphics, scheduleCard, 18, g_scheduleEnabled ? Color(255, 79, 129, 114) : Color(255, 39, 45, 61), g_scheduleEnabled ? 1.4f : 1.0f);
    text(graphics, localized(L"Planification jour / nuit", L"Day / night schedule", L"Tag-/Nacht-Zeitplan", L"日间 / 夜间计划"),
         RectF(scheduleCard.X + 20, scheduleCard.Y + 17, 340, 23), 15, Color::White, FontStyleBold);
    text(graphics, localized(L"RGBCcontrol applique automatiquement un profil selon l'heure locale.",
                             L"RGBCcontrol automatically applies a profile using local time.",
                             L"RGBCcontrol wendet anhand der Ortszeit automatisch ein Profil an.",
                             L"RGBCcontrol 会根据本地时间自动应用模式。"),
         RectF(scheduleCard.X + 20, scheduleCard.Y + 43, scheduleCard.Width - 230, 18), 9, Color(255, 126, 136, 158));
    drawButton(graphics, RectF(scheduleCard.GetRight() - 180, scheduleCard.Y + 15, 160, 40),
               g_scheduleEnabled ? localized(L"Désactiver", L"Disable", L"Deaktivieren", L"禁用")
                                 : localized(L"Activer", L"Enable", L"Aktivieren", L"启用"),
               g_scheduleEnabled, Action::ScheduleToggle);

    const float schedulePanelWidth = (scheduleCard.Width - 52) / 2.0f;
    auto schedulePanel = [&](const RectF& panel, bool day) {
        const int hour = day ? g_scheduleDayHour : g_scheduleNightHour;
        const int selectedProfile = day ? g_scheduleDayProfile : g_scheduleNightProfile;
        fillRound(graphics, panel, 14, Color(255, 21, 25, 36));
        strokeRound(graphics, panel, 14, Color(255, 43, 49, 65));
        text(graphics, day ? localized(L"JOUR", L"DAY", L"TAG", L"日间") : localized(L"NUIT", L"NIGHT", L"NACHT", L"夜间"),
             RectF(panel.X + 16, panel.Y + 12, 90, 17), 9, day ? Color(255, 242, 190, 102) : accentTint(0.36), FontStyleBold);
        text(graphics, localized(L"À partir de", L"From", L"Ab", L"开始时间"), RectF(panel.X + 16, panel.Y + 36, 100, 18), 9, Color(255, 126, 136, 158));
        drawButton(graphics, RectF(panel.GetRight() - 176, panel.Y + 31, 38, 34), L"−", false,
                   day ? Action::ScheduleDayHourDown : Action::ScheduleNightHourDown);
        fillRound(graphics, RectF(panel.GetRight() - 130, panel.Y + 31, 76, 34), 9, Color(255, 30, 35, 49));
        const std::wstring time = (hour < 10 ? L"0" : L"") + std::to_wstring(hour) + L":00";
        text(graphics, time, RectF(panel.GetRight() - 130, panel.Y + 31, 76, 34), 11, Color::White, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        drawButton(graphics, RectF(panel.GetRight() - 46, panel.Y + 31, 30, 34), L"+", false,
                   day ? Action::ScheduleDayHourUp : Action::ScheduleNightHourUp);
        text(graphics, localized(L"Profil appliqué", L"Applied profile", L"Angewendetes Profil", L"应用模式"),
             RectF(panel.X + 16, panel.Y + 82, 130, 17), 9, Color(255, 126, 136, 158));
        const float profileButtonWidth = (panel.Width - 44) / 3.0f;
        for (int index = 0; index < 3; ++index) {
            drawButton(graphics, RectF(panel.X + 16 + index * (profileButtonWidth + 6), panel.Y + 105, profileButtonWidth, 35),
                       profileName(index), selectedProfile == index,
                       day ? Action::ScheduleDayProfile : Action::ScheduleNightProfile, index);
        }
    };
    schedulePanel(RectF(scheduleCard.X + 20, scheduleCard.Y + 76, schedulePanelWidth, 154), true);
    schedulePanel(RectF(scheduleCard.X + 32 + schedulePanelWidth, scheduleCard.Y + 76, schedulePanelWidth, 154), false);
    const std::wstring scheduleMessage = g_scheduleStatus.empty()
        ? localized(L"Les changements sont enregistrés automatiquement.", L"Changes are saved automatically.",
                    L"Änderungen werden automatisch gespeichert.", L"更改会自动保存。")
        : g_scheduleStatus;
    text(graphics, scheduleMessage, RectF(x, scheduleCard.GetBottom() + 12, available, 20), 9,
         g_scheduleEnabled ? Color(255, 105, 221, 178) : Color(255, 126, 136, 158), FontStyleBold);
    g_maxScroll = std::max(0.0f, scheduleCard.GetBottom() + g_scrollOffset + 48 - height);
}

void drawColorPicker(Graphics& graphics, int width, int height) {
    SolidBrush backdrop(Color(188, 5, 7, 12));
    graphics.FillRectangle(&backdrop, 0, 0, width, height);
    addHit(RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), Action::PickerBackdrop);

    const float modalWidth = std::min(680.0f, width - 40.0f);
    const float modalHeight = std::min(440.0f, height - 36.0f);
    const RectF modal((width - modalWidth) / 2.0f, (height - modalHeight) / 2.0f, modalWidth, modalHeight);
    fillRound(graphics, RectF(modal.X - 8, modal.Y + 10, modal.Width + 16, modal.Height + 8), 24, Color(95, 0, 0, 0));
    fillRound(graphics, modal, 22, Color(255, 20, 24, 35));
    strokeRound(graphics, modal, 22, accentColor(150), 1.2f);
    text(graphics, localized(L"Choisir une couleur", L"Choose a color", L"Farbe auswählen", L"选择颜色"),
         RectF(modal.X + 28, modal.Y + 21, 360, 28), 20, Color::White, FontStyleBold);
    text(graphics, localized(L"Mélange précisément la teinte et la saturation.", L"Precisely blend hue and saturation.",
                             L"Farbton und Sättigung präzise mischen.", L"精确混合色相与饱和度。"),
         RectF(modal.X + 28, modal.Y + 50, 430, 18), 10, Color(255, 143, 152, 174));
    const RectF closeButton(modal.GetRight() - 52, modal.Y + 17, 32, 32);
    fillRound(graphics, closeButton, 10, Color(255, 31, 36, 49));
    text(graphics, L"×", closeButton, 20, Color(255, 189, 195, 210), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
    addHit(closeButton, Action::PickerCancel);

    constexpr int wheelPixels = 240;
    const RectF wheelRect(modal.X + 34, modal.Y + 86, static_cast<float>(wheelPixels), static_cast<float>(wheelPixels));
    Bitmap wheel(wheelPixels, wheelPixels, PixelFormat32bppARGB);
    BitmapData data{};
    Rect pixelBounds(0, 0, wheelPixels, wheelPixels);
    if (wheel.LockBits(&pixelBounds, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok) {
        const double radius = wheelPixels / 2.0 - 2.0;
        const double center = (wheelPixels - 1) / 2.0;
        for (int py = 0; py < wheelPixels; ++py) {
            auto* row = reinterpret_cast<std::uint32_t*>(static_cast<BYTE*>(data.Scan0) + py * data.Stride);
            for (int px = 0; px < wheelPixels; ++px) {
                const double dx = px - center;
                const double dy = py - center;
                const double distance = std::sqrt(dx * dx + dy * dy);
                if (distance > radius) { row[px] = 0; continue; }
                double hue = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                if (hue < 0) hue += 360.0;
                const double edge = std::clamp((radius - distance) / 1.5, 0.0, 1.0);
                const std::uint32_t rgb = hsvColor(hue, distance / radius, 1.0);
                row[px] = (static_cast<std::uint32_t>(std::lround(edge * 255.0)) << 24) | rgb;
            }
        }
        wheel.UnlockBits(&data);
    }
    SolidBrush wheelGlow(accentColor(60));
    graphics.FillEllipse(&wheelGlow, RectF(wheelRect.X - 8, wheelRect.Y - 8, wheelRect.Width + 16, wheelRect.Height + 16));
    graphics.DrawImage(&wheel, wheelRect);
    Pen wheelBorder(Color(255, 74, 80, 101), 1.5f);
    graphics.DrawEllipse(&wheelBorder, wheelRect);
    const float radius = wheelRect.Width / 2.0f - 3.0f;
    const double angle = g_pickerHue * 3.14159265358979323846 / 180.0;
    const float markerX = wheelRect.X + wheelRect.Width / 2.0f + static_cast<float>(std::cos(angle) * g_pickerSaturation * radius);
    const float markerY = wheelRect.Y + wheelRect.Height / 2.0f + static_cast<float>(std::sin(angle) * g_pickerSaturation * radius);
    SolidBrush markerShadow(Color(130, 0, 0, 0));
    graphics.FillEllipse(&markerShadow, RectF(markerX - 9, markerY - 8, 20, 20));
    Pen markerOuter(Color::White, 3.0f);
    graphics.DrawEllipse(&markerOuter, RectF(markerX - 8, markerY - 8, 16, 16));
    Pen markerInner(Color(210, 20, 22, 30), 1.5f);
    graphics.DrawEllipse(&markerInner, RectF(markerX - 5, markerY - 5, 10, 10));
    addHit(wheelRect, Action::PickerWheel);
    text(graphics, localized(L"Glisse dans la roue pour mélanger les couleurs", L"Drag inside the wheel to blend colors",
                             L"In der Farbfläche ziehen, um Farben zu mischen", L"在色轮中拖动以混合颜色"),
         RectF(wheelRect.X - 2, wheelRect.GetBottom() + 12, wheelRect.Width + 4, 32), 9, Color(255, 126, 135, 156),
         FontStyleRegular, StringAlignmentCenter);

    const float panelX = modal.X + 316;
    const float panelWidth = modal.GetRight() - panelX - 28;
    const std::uint32_t selectedColor = hsvColor(g_pickerHue, g_pickerSaturation, g_pickerBrightness / 100.0);
    text(graphics, localized(L"APERÇU", L"PREVIEW", L"VORSCHAU", L"预览"), RectF(panelX, modal.Y + 86, 100, 16), 9,
         Color(255, 124, 111, 215), FontStyleBold);
    const RectF preview(panelX, modal.Y + 108, panelWidth, 62);
    fillRound(graphics, preview, 15, rgbColor(selectedColor));
    strokeRound(graphics, preview, 15, Color(150, 255, 255, 255));
    fillRound(graphics, RectF(preview.X + 12, preview.Y + 14, 112, 34), 9, Color(150, 12, 15, 23));
    text(graphics, hexColor(selectedColor), RectF(preview.X + 12, preview.Y + 14, 112, 34), 13, Color::White,
         FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);

    const std::wstring hueText = std::to_wstring(static_cast<int>(std::lround(g_pickerHue))) + L"°";
    const std::wstring saturationText = std::to_wstring(static_cast<int>(std::lround(g_pickerSaturation * 100.0))) + L" %";
    const float chipWidth = (panelWidth - 16) / 3.0f;
    const wchar_t* chipLabels[] = {L"H", L"S", L"V"};
    const std::wstring chipValues[] = {hueText, saturationText, std::to_wstring(g_pickerBrightness) + L" %"};
    for (int index = 0; index < 3; ++index) {
        RectF chip(panelX + index * (chipWidth + 8), modal.Y + 181, chipWidth, 42);
        fillRound(graphics, chip, 10, Color(255, 27, 32, 45));
        text(graphics, chipLabels[index], RectF(chip.X + 10, chip.Y, 18, chip.Height), 9, Color(255, 128, 137, 158),
             FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
        text(graphics, chipValues[index], RectF(chip.X + 27, chip.Y, chip.Width - 37, chip.Height), 10, Color::White,
             FontStyleBold, StringAlignmentFar, StringAlignmentCenter);
    }

    text(graphics, localized(L"Luminosité", L"Brightness", L"Helligkeit", L"亮度"), RectF(panelX, modal.Y + 238, 150, 18),
         11, Color(255, 220, 224, 234), FontStyleBold);
    const RectF valueRect(panelX, modal.Y + 260, panelWidth, 26);
    const RectF valueTrack(valueRect.X, valueRect.Y + 9, valueRect.Width, 8);
    GraphicsPath valuePath;
    roundedPath(valuePath, valueTrack, 4);
    LinearGradientBrush valueGradient(PointF(valueTrack.X, valueTrack.Y), PointF(valueTrack.GetRight(), valueTrack.Y),
                                      Color(255, 5, 6, 9), rgbColor(hsvColor(g_pickerHue, g_pickerSaturation, 1.0)));
    graphics.FillPath(&valueGradient, &valuePath);
    strokeRound(graphics, valueTrack, 4, Color(255, 68, 74, 94));
    const float valueX = valueTrack.X + valueTrack.Width * g_pickerBrightness / 100.0f;
    SolidBrush valueThumb(rgbColor(selectedColor));
    graphics.FillEllipse(&valueThumb, RectF(valueX - 8, valueTrack.Y - 4, 16, 16));
    Pen valueBorder(Color::White, 2.0f);
    graphics.DrawEllipse(&valueBorder, RectF(valueX - 8, valueTrack.Y - 4, 16, 16));
    addHit(RectF(valueRect.X, valueRect.Y - 4, valueRect.Width, valueRect.Height + 8), Action::PickerBrightness);

    text(graphics, localized(L"RACCOURCIS", L"PRESETS", L"FAVORITEN", L"预设"), RectF(panelX, modal.Y + 298, 130, 16), 9,
         Color(255, 124, 111, 215), FontStyleBold);
    const std::uint32_t presets[] = {0x7C5CFF, 0x149CFF, 0x00D69E, 0xFFD34F, 0xFF4F70, 0xFF6B35, 0xD75CFF, 0xFFFFFF};
    const float presetGap = (panelWidth - 8 * 28.0f) / 7.0f;
    float presetX = panelX;
    for (std::uint32_t preset : presets) {
        RectF presetRect(presetX, modal.Y + 322, 28, 28);
        fillRound(graphics, presetRect, 8, rgbColor(preset));
        strokeRound(graphics, presetRect, 8, preset == selectedColor ? Color::White : Color(255, 76, 84, 105), preset == selectedColor ? 2.0f : 1.0f);
        addHit(presetRect, Action::PickerPreset, -1, preset);
        presetX += 28 + presetGap;
    }

    const float buttonY = modal.GetBottom() - 58;
    drawButton(graphics, RectF(panelX, buttonY, (panelWidth - 10) / 2, 42), localized(L"Annuler", L"Cancel", L"Abbrechen", L"取消"),
               false, Action::PickerCancel);
    drawButton(graphics, RectF(panelX + (panelWidth - 10) / 2 + 10, buttonY, (panelWidth - 10) / 2, 42),
               localized(L"Utiliser", L"Use color", L"Übernehmen", L"使用颜色"), true, Action::PickerApply);
}

void drawScrollbar(Graphics& graphics, int width, int height) {
    if (g_maxScroll <= 0) { g_scrollThumb = RectF(); return; }
    const float trackTop = kHeaderHeight + 12.0f;
    const float trackHeight = height - trackTop - 12.0f;
    RectF track(width - 10.0f, trackTop, 5, trackHeight);
    fillRound(graphics, track, 3, Color(190, 17, 21, 31));
    float viewport = height - kHeaderHeight;
    float content = viewport + g_maxScroll;
    float thumbHeight = std::max(48.0f, trackHeight * viewport / content);
    float ratio = g_maxScroll > 0 ? g_scrollOffset / g_maxScroll : 0;
    float thumbY = trackTop + (trackHeight - thumbHeight) * ratio;
    g_scrollThumb = RectF(width - 10.0f, thumbY, 5, thumbHeight);
    fillRound(graphics, g_scrollThumb, 3, g_scrollDragging ? accentTint(0.42) : accentColor(205));
}

double smoothStep(double edge0, double edge1, double value) {
    if (edge0 == edge1) return value >= edge1 ? 1.0 : 0.0;
    const double normalized = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

BYTE scaledAlpha(int alpha, double opacity) {
    return static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(alpha * opacity)), 0, 255));
}

void drawFastGlow(Graphics& graphics, const RectF& bounds, Color color) {
    LinearGradientBrush glow(PointF(bounds.X, bounds.Y), PointF(bounds.GetRight(), bounds.GetBottom()),
                             Color(0, color.GetR(), color.GetG(), color.GetB()),
                             Color(0, color.GetR(), color.GetG(), color.GetB()));
    Color colors[5] = {
        Color(0, color.GetR(), color.GetG(), color.GetB()),
        Color(static_cast<BYTE>(color.GetA() / 2), color.GetR(), color.GetG(), color.GetB()),
        color,
        Color(static_cast<BYTE>(color.GetA() / 2), color.GetR(), color.GetG(), color.GetB()),
        Color(0, color.GetR(), color.GetG(), color.GetB())
    };
    REAL positions[5] = {0.0f, 0.24f, 0.5f, 0.76f, 1.0f};
    glow.SetInterpolationColors(colors, positions, 5);
    graphics.FillEllipse(&glow, bounds);
}

void renderAnimatedBackgroundFrame(Graphics& graphics, int width, int height) {
    LinearGradientBrush base(PointF(0, 0), PointF(static_cast<REAL>(width), static_cast<REAL>(height)),
                             Color(255, 7, 9, 15), Color(255, 12, 15, 24));
    constexpr INT stopCount = 5;
    Color colors[stopCount] = {
        Color(255, 7, 9, 15), Color(255, 10, 13, 22), Color(255, 12, 15, 25),
        Color(255, 9, 13, 22), Color(255, 6, 8, 14)
    };
    REAL positions[stopCount] = {0.0f, 0.28f, 0.54f, 0.78f, 1.0f};
    base.SetInterpolationColors(colors, positions, stopCount);
    graphics.FillRectangle(&base, 0, 0, width, height);

    // Two wide auroras replace the old grid and large competing colour blobs.
    // They preserve the RGB identity while leaving the controls visually quiet.
    const float accentX = width * 0.72f;
    drawFastGlow(graphics, RectF(accentX - 390.0f, -250.0f, 780.0f, 520.0f), accentColor(27));
    const float cyanX = width * 0.34f;
    const float cyanY = height * 0.96f;
    drawFastGlow(graphics, RectF(cyanX - 340.0f, cyanY - 220.0f, 680.0f, 440.0f), Color(19, 27, 203, 190));

    SolidBrush constellation(Color(22, 151, 167, 202));
    for (float y = 126.0f; y < height; y += 118.0f) {
        for (float x = static_cast<float>(kSidebarWidth + 52); x < width; x += 142.0f) {
            graphics.FillEllipse(&constellation, RectF(x, y, 1.4f, 1.4f));
        }
    }
}

void drawBackgroundMotion(Graphics& graphics, int width, int height) {
    if (g_reduceMotion) return;
    const double now = g_launchPreviewMs >= 0.0 ? g_launchPreviewMs : static_cast<double>(GetTickCount64());
    const double phase = now / 1000.0;
    for (int index = 0; index < 9; ++index) {
        const double speed = 4.0 + (index % 4) * 0.85;
        const float x = static_cast<float>(std::fmod(index * 173.0 + phase * speed + width * 2.0, std::max(1, width)));
        const float y = static_cast<float>(std::fmod(index * 127.0 + 14.0 * std::sin(phase * 0.18 + index), std::max(1, height)));
        const float radius = 0.65f + (index % 3) * 0.32f;
        SolidBrush particle(index % 3 == 0 ? accentColor(36) : Color(22, 190, 207, 235));
        graphics.FillEllipse(&particle, RectF(x, y, radius * 2, radius * 2));
    }
}

void drawAnimatedBackground(Graphics& graphics, int width, int height) {
    renderAnimatedBackgroundFrame(graphics, width, height);
    drawBackgroundMotion(graphics, width, height);
}

void drawLaunchBackdrop(Graphics& graphics, int width, int height, double opacity) {
    SolidBrush curtain(Color(scaledAlpha(252, opacity), 5, 7, 13));
    graphics.FillRectangle(&curtain, 0, 0, width, height);

    const float centerX = width / 2.0f;
    const float centerY = height / 2.0f - 48.0f;
    drawFastGlow(graphics, RectF(centerX - 330, centerY - 285, 660, 570),
                 accentColor(scaledAlpha(20, opacity)));
    drawFastGlow(graphics, RectF(centerX - 205, centerY - 225, 410, 450),
                 Color(scaledAlpha(12, opacity), 29, 220, 208));

    // Very light cinema bars focus the eye without putting the logo in a box.
    SolidBrush upperShade(Color(scaledAlpha(72, opacity), 0, 0, 0));
    graphics.FillRectangle(&upperShade, 0, 0, width, std::max(0, height / 10));
    graphics.FillRectangle(&upperShade, 0, height - std::max(0, height / 11), width, std::max(0, height / 11));
}

void drawLaunchAnimation(Graphics& graphics, int width, int height, double elapsedMs, double opacity) {
    if (opacity <= 0.0) return;
    const double progress = std::clamp(elapsedMs / kLaunchDurationMs, 0.0, 1.0);
    const float centerX = width / 2.0f;
    const float centerY = height / 2.0f - 74.0f;
    drawLaunchBackdrop(graphics, width, height, opacity);

    const Color spectrum[] = {
        Color(255, 255, 63, 116), Color(255, 255, 170, 42), Color(255, 223, 236, 58),
        Color(255, 47, 226, 157), Color(255, 32, 211, 224), Color(255, 47, 142, 255),
        Color(255, 128, 92, 255), Color(255, 225, 62, 216)
    };

    // Thin RGB light ribbons meet in the centre before the emblem is drawn.
    const double ribbonLife = smoothStep(0.01, 0.22, progress) * (1.0 - smoothStep(0.39, 0.58, progress));
    if (ribbonLife > 0.0) {
        for (int index = 0; index < 9; ++index) {
            const double ribbon = smoothStep(0.01 + index * 0.012, 0.16 + index * 0.012, progress) * ribbonLife;
            const float offset = (index - 4) * 19.0f;
            const float lean = (index - 4) * 2.7f;
            const float halfWidth = 3.0f + (index % 3) * 1.6f;
            const float top = centerY - 250.0f + static_cast<float>((1.0 - ribbon) * 70.0);
            const float bottom = centerY + 250.0f - static_cast<float>((1.0 - ribbon) * 70.0);
            const Color base = spectrum[index % static_cast<int>(std::size(spectrum))];
            LinearGradientBrush ribbonBrush(PointF(centerX, top), PointF(centerX, bottom),
                                            Color(0, base.GetR(), base.GetG(), base.GetB()),
                                            Color(0, base.GetR(), base.GetG(), base.GetB()));
            Color ribbonColors[5] = {
                Color(0, base.GetR(), base.GetG(), base.GetB()),
                Color(scaledAlpha(42, opacity * ribbon), base.GetR(), base.GetG(), base.GetB()),
                Color(scaledAlpha(185, opacity * ribbon), base.GetR(), base.GetG(), base.GetB()),
                Color(scaledAlpha(42, opacity * ribbon), base.GetR(), base.GetG(), base.GetB()),
                Color(0, base.GetR(), base.GetG(), base.GetB())
            };
            REAL ribbonStops[5] = {0.0f, 0.28f, 0.5f, 0.72f, 1.0f};
            ribbonBrush.SetInterpolationColors(ribbonColors, ribbonStops, 5);
            PointF ribbonShape[4] = {
                PointF(centerX + offset - halfWidth, top), PointF(centerX + offset + halfWidth, top),
                PointF(centerX + offset + lean + halfWidth, bottom), PointF(centerX + offset + lean - halfWidth, bottom)
            };
            graphics.FillPolygon(&ribbonBrush, ribbonShape, 4);
        }
    }

    const double emblemBuild = smoothStep(0.12, 0.49, progress);
    const double emblemSettle = smoothStep(0.31, 0.55, progress);
    const float emblemScale = static_cast<float>(0.82 + 0.18 * emblemSettle);
    const float logoSize = static_cast<float>(std::clamp(std::min(width, height) * 0.34, 220.0, 262.0)) * emblemScale;
    RectF orbit(centerX - logoSize / 2.0f, centerY - logoSize / 2.0f, logoSize, logoSize);

    drawFastGlow(graphics, RectF(centerX - logoSize * 0.68f, centerY - logoSize * 0.68f,
                                 logoSize * 1.36f, logoSize * 1.36f),
                 accentColor(scaledAlpha(38, opacity * emblemBuild)));

    // Keep the original RGBCcontrol artwork, but crop away its opaque square
    // corners and reveal the high-resolution source inside a clean circle.
    if (g_logo && g_logo->GetLastStatus() == Ok) {
        const double logoReveal = smoothStep(0.14, 0.48, progress);
        const GraphicsState revealState = graphics.Save();
        const float revealWidth = orbit.Width * static_cast<float>(logoReveal);
        graphics.SetClip(RectF(centerX - revealWidth / 2.0f, orbit.Y, revealWidth, orbit.Height), CombineModeIntersect);
        drawBaseLogoRound(graphics, orbit, opacity * smoothStep(0.10, 0.32, progress));
        graphics.Restore(revealState);
    } else {
        SolidBrush fallback(accentColor(scaledAlpha(220, opacity * emblemBuild)));
        graphics.FillEllipse(&fallback, orbit);
    }

    RectF halo(orbit.X - 7.0f, orbit.Y - 7.0f, orbit.Width + 14.0f, orbit.Height + 14.0f);
    Pen haloGuide(Color(scaledAlpha(52, opacity * emblemBuild), 157, 170, 205), 1.0f);
    graphics.DrawEllipse(&haloGuide, halo);
    for (int index = 0; index < 4; ++index) {
        const double segment = smoothStep(0.17 + index * 0.048, 0.38 + index * 0.048, progress);
        if (segment <= 0.0) continue;
        const Color base = spectrum[index * 2];
        Pen arc(Color(scaledAlpha(225, opacity * segment), base.GetR(), base.GetG(), base.GetB()), 3.2f);
        arc.SetStartCap(LineCapRound);
        arc.SetEndCap(LineCapRound);
        graphics.DrawArc(&arc, halo, -82.0f + index * 90.0f, static_cast<REAL>(50.0 * segment));
    }

    const double sweep = smoothStep(0.43, 0.59, progress) * (1.0 - smoothStep(0.63, 0.75, progress));
    if (sweep > 0.0) {
        const float sweepX = orbit.X + orbit.Width * static_cast<float>(smoothStep(0.45, 0.70, progress));
        const GraphicsState state = graphics.Save();
        GraphicsPath clipPath;
        clipPath.AddEllipse(orbit);
        graphics.SetClip(&clipPath, CombineModeIntersect);
        LinearGradientBrush sweepBrush(PointF(sweepX - 34, centerY), PointF(sweepX + 34, centerY),
                                       Color(0, 255, 255, 255), Color(0, 255, 255, 255));
        Color sweepColors[3] = {Color(0, 255, 255, 255), Color(scaledAlpha(120, opacity * sweep), 255, 255, 255), Color(0, 255, 255, 255)};
        REAL sweepStops[3] = {0.0f, 0.5f, 1.0f};
        sweepBrush.SetInterpolationColors(sweepColors, sweepStops, 3);
        graphics.FillRectangle(&sweepBrush, RectF(sweepX - 34.0f, orbit.Y, 68.0f, orbit.Height));
        graphics.Restore(state);
    }

    const double titleReveal = smoothStep(0.54, 0.73, progress);
    const float titleY = orbit.GetBottom() + 27.0f - static_cast<float>(7.0 * titleReveal);
    text(graphics, L"RGBCcontrol", RectF(centerX - 260, titleY, 520, 44), 31,
         Color(scaledAlpha(255, opacity * titleReveal), 245, 247, 255), FontStyleBold,
         StringAlignmentCenter, StringAlignmentCenter);
    const std::wstring tagline = localized(L"TON SETUP. TON AMBIANCE.", L"YOUR SETUP. YOUR LIGHT.",
                                           L"DEIN SETUP. DEIN LICHT.", L"你的设备，你的光效。 ");
    text(graphics, tagline, RectF(centerX - 260, titleY + 44, 520, 24), 10,
         Color(scaledAlpha(210, opacity * titleReveal), 164, 177, 207), FontStyleBold,
         StringAlignmentCenter, StringAlignmentCenter);

    const float lineWidth = 190.0f * static_cast<float>(smoothStep(0.62, 0.82, progress));
    if (lineWidth > 1.0f) {
        LinearGradientBrush lineBrush(PointF(centerX - lineWidth / 2, titleY + 76), PointF(centerX + lineWidth / 2, titleY + 76),
                                      Color(0, 124, 92, 255), Color(scaledAlpha(255, opacity), 55, 211, 211));
        Color lineColors[3] = {Color(0, 124, 92, 255), accentColor(scaledAlpha(255, opacity)), Color(0, 55, 211, 211)};
        REAL lineStops[3] = {0.0f, 0.5f, 1.0f};
        lineBrush.SetInterpolationColors(lineColors, lineStops, 3);
        Pen linePen(&lineBrush, 2.0f);
        graphics.DrawLine(&linePen, centerX - lineWidth / 2, titleY + 76, centerX + lineWidth / 2, titleY + 76);
    }
}

bool launchAnimationActive() {
    if (g_launchPreviewMs >= 0.0) return true;
    return !g_launchAnimationFinished && g_launchAnimationStartedAt != 0 && !g_reduceMotion;
}

double launchElapsedMs() {
    return g_launchPreviewMs >= 0.0 ? g_launchPreviewMs
                                   : static_cast<double>(GetTickCount64() - g_launchAnimationStartedAt);
}

void drawApplicationScene(Graphics& graphics, int width, int height) {
    drawAnimatedBackground(graphics, width, height);
    drawHeader(graphics, width);
    drawNavigation(graphics, height);
    // Header/sidebar text must be emitted before the scrolling clip is active.
    flushDeferredText(graphics);
    const RectF contentClip(static_cast<REAL>(kSidebarWidth), static_cast<REAL>(kHeaderHeight),
                            static_cast<REAL>(std::max(0, width - kSidebarWidth)),
                            static_cast<REAL>(std::max(0, height - kHeaderHeight)));
    graphics.SetClip(contentClip, CombineModeReplace);
    const std::size_t firstScrollableHit = g_hits.size();
    float originY = static_cast<float>(kHeaderHeight + 24) - g_scrollOffset;
    if (g_page == Page::Dashboard) drawDashboard(graphics, width, height, originY);
    else if (g_page == Page::Devices) drawDevices(graphics, width, height, originY);
    else if (g_page == Page::Gamepads) drawGamepads(graphics, width, height, originY);
    else if (g_page == Page::DuckyAssistant) drawDuckyAssistant(graphics, width, height, originY);
    else if (g_page == Page::Compatibility) drawCompatibility(graphics, width, height, originY);
    else if (g_page == Page::Effects) drawEffects(graphics, width, height, originY);
    else if (g_page == Page::Profiles) drawProfiles(graphics, width, height, originY);
    else if (g_page == Page::Fans) drawFans(graphics, width, height, originY);
    else if (g_page == Page::Diagnostics) drawDiagnostics(graphics, width, height, originY);
    else drawSettings(graphics, width, height, originY);
    g_scrollOffset = std::clamp(g_scrollOffset, 0.0f, g_maxScroll);

    // Text is batched for speed, so flush it while the content clip still
    // exists. Otherwise scrolled labels are drawn later over the fixed header.
    flushDeferredText(graphics, &contentClip);
    for (std::size_t index = firstScrollableHit; index < g_hits.size();) {
        RectF& rect = g_hits[index].rect;
        const float left = std::max(rect.X, contentClip.X);
        const float top = std::max(rect.Y, contentClip.Y);
        const float right = std::min(rect.GetRight(), contentClip.GetRight());
        const float bottom = std::min(rect.GetBottom(), contentClip.GetBottom());
        if (right <= left || bottom <= top) {
            g_hits.erase(g_hits.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            rect = RectF(left, top, right - left, bottom - top);
            ++index;
        }
    }
    drawScrollbar(graphics, width, height);
    graphics.ResetClip();
    drawHeaderTooltip(graphics, width);
    if (g_colorPickerOpen) {
        flushDeferredText(graphics);
        drawColorPicker(graphics, width, height);
    }
}

void renderScene(Graphics& graphics, int width, int height) {
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(InterpolationModeBilinear);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    graphics.SetCompositingQuality(CompositingQualityHighSpeed);
    g_hits.clear();
    g_deferredText.clear();
    g_collectOpaqueText = true;
    const bool launching = launchAnimationActive();
    const double elapsed = launching ? launchElapsedMs() : kLaunchDurationMs;
    const double revealInterface = smoothStep(kLaunchDurationMs * 0.80, kLaunchDurationMs, elapsed);
    if (!launching || revealInterface > 0.001) drawApplicationScene(graphics, width, height);
    else drawAnimatedBackground(graphics, width, height);
    flushDeferredText(graphics);
    if (launching) drawLaunchAnimation(graphics, width, height, elapsed, 1.0 - revealInterface);
    flushDeferredText(graphics);
    g_collectOpaqueText = false;

    Pen outer(accentColor(30), 1.0f);
    graphics.DrawRectangle(&outer, RectF(0.5f, 0.5f, std::max(0.0f, width - 1.0f), std::max(0.0f, height - 1.0f)));
}

void paint(HWND window) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    int width = std::max(1L, client.right);
    int height = std::max(1L, client.bottom);
    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    Graphics graphics(&buffer);
    const ULONGLONG now = GetTickCount64();
    const bool fastGamepadFrame = g_gamepadFastPaintRequested && g_page == Page::Gamepads &&
                                  !g_colorPickerOpen && !launchAnimationActive() &&
                                  g_gamepadPageCache && g_gamepadPageCache->GetLastStatus() == Ok &&
                                  g_gamepadPageCacheWidth == width && g_gamepadPageCacheHeight == height &&
                                  std::abs(g_gamepadPageCacheScroll - g_scrollOffset) < 0.01f &&
                                  now - g_gamepadLastFullPaintAt < 120;
    g_gamepadFastPaintRequested = false;
    if (fastGamepadFrame) {
        graphics.SetInterpolationMode(InterpolationModeNearestNeighbor);
        graphics.DrawImage(g_gamepadPageCache.get(), 0, 0, width, height);
        graphics.SetInterpolationMode(InterpolationModeBilinear);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        drawGamepadFastOverlay(graphics, width, height);
    } else {
        renderScene(graphics, width, height);
        if (g_page == Page::Gamepads && !g_colorPickerOpen && !launchAnimationActive()) {
            g_gamepadPageCache = std::make_unique<Bitmap>(width, height, PixelFormat32bppPARGB);
            if (g_gamepadPageCache && g_gamepadPageCache->GetLastStatus() == Ok) {
                Graphics pageGraphics(g_gamepadPageCache.get());
                pageGraphics.SetCompositingMode(CompositingModeSourceCopy);
                pageGraphics.DrawImage(&buffer, 0, 0, width, height);
                g_gamepadPageCacheWidth = width;
                g_gamepadPageCacheHeight = height;
                g_gamepadPageCacheScroll = g_scrollOffset;
                g_gamepadLastFullPaintAt = now;
            } else {
                g_gamepadPageCache.reset();
            }
        }
    }
    Graphics screen(target);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(window, &paint);
}

CLSID pngEncoder() {
    UINT count = 0, bytes = 0;
    GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> storage(bytes);
    ImageCodecInfo* encoders = reinterpret_cast<ImageCodecInfo*>(storage.data());
    GetImageEncoders(count, bytes, encoders);
    for (UINT index = 0; index < count; ++index) {
        if (encoders[index].MimeType && std::wcscmp(encoders[index].MimeType, L"image/png") == 0) return encoders[index].Clsid;
    }
    return CLSID{};
}

bool savePageCapture(Page page, int width, int height, const fs::path& destination, float scrollOffset = 0) {
    g_page = page;
    g_scrollOffset = scrollOffset;
    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    Graphics graphics(&buffer);
    renderScene(graphics, width, height);
    CLSID encoder = pngEncoder();
    return buffer.Save(destination.c_str(), &encoder, nullptr) == Ok;
}

bool saveLaunchCapture(int width, int height, const fs::path& destination, double elapsedMs) {
    const double previousPreview = g_launchPreviewMs;
    g_launchPreviewMs = elapsedMs;
    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    Graphics graphics(&buffer);
    renderScene(graphics, width, height);
    CLSID encoder = pngEncoder();
    const bool saved = buffer.Save(destination.c_str(), &encoder, nullptr) == Ok;
    g_launchPreviewMs = previousPreview;
    return saved;
}

void seedCaptureData() {
    g_rgbDevices = {
        {0, L"ASRock A620AM-HVS", L"ASRock", L"Polychrome USB", L"Carte mere", 4, 241, true, L"openrgb",
         DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::Effects | DeviceCapability::Zones | DeviceCapability::PerLed,
         {L"Static", L"Direct", L"Rainbow"}, {}},
        {1, L"Ruban LED USB", L"Action", L"Controleur USB", L"Ruban LED", 1, 60, true, L"openrgb",
         DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::Effects | DeviceCapability::Zones | DeviceCapability::PerLed,
         {L"Static", L"Direct"}, {}}
    };
    g_duckyDetected = true;
    g_openRgbReady = true;
    g_hasCompletedScan = true;
    g_hotplugMonitoring = true;
    g_hotplugStatus = localized(L"Inventaire actualisé automatiquement.", L"Inventory updated automatically.",
                                L"Inventar automatisch aktualisiert.", L"设备清单已自动更新。");
    g_status = localized(L"Tous les contrôleurs compatibles sont prêts.", L"All compatible controllers are ready.",
                         L"Alle kompatiblen Controller sind bereit.", L"所有兼容控制器均已就绪。");
    g_cpuTemperature = 53;
    g_gpuTemperature = 43;
    g_fans.clear();
    for (int index = 0; index < 7; ++index) {
        FanDevice fan;
        fan.encodedId = "capture" + std::to_string(index);
        fan.name = index < 2 ? L"GPU Fan " + std::to_wstring(index + 1) : L"Chassis Fan " + std::to_wstring(index - 1);
        fan.hardware = index < 2 ? L"NVIDIA GeForce RTX 5060" : L"Carte mere compatible";
        fan.rpm = 820 + index * 115;
        fan.percent = 42;
        fan.minimum = 30;
        fan.desired = 42 + index;
        fan.controllable = true;
        fan.manual = index == 2;
        g_fans.push_back(std::move(fan));
    }
    g_fanStatus = localized(L"7 détectés · 7 contrôlables", L"7 detected · 7 controllable",
                            L"7 erkannt · 7 steuerbar", L"已检测 7 个 · 可控制 7 个");
    g_fanCurveEnabled = false;
    g_curvePreset = 1;
    g_curveSpeeds = {30, 45, 70, 100};
    g_screenMonitors = {{L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, true},
                        {L"\\\\.\\DISPLAY2", 1920, 0, 2560, 1440, false}};
    g_ambientMonitorIndex = 0;
    g_ambientSaturation = 68;
    g_ambientZones = true;
    g_profiles = {};
    g_profiles[0] = captureCurrentProfile();
    g_profiles[0].effect = 2;
    g_profiles[0].fanProfile = FanProfile::Performance;
    g_profiles[1] = captureCurrentProfile();
    g_profiles[1].brightness = 45;
    g_profiles[1].effect = 1;
    g_profiles[1].fanCurveEnabled = true;
    g_profiles[1].curveSpeeds = {30, 35, 55, 80};
    g_activeProfile = 0;
}

int runRenderBenchmark(const fs::path& destination) {
    seedCaptureData();
    g_launchAnimationFinished = true;
    g_launchPreviewMs = -1.0;
    g_reduceMotion = false;
    constexpr int width = 1180;
    constexpr int height = 760;
    constexpr int frames = 30;
    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    Graphics graphics(&buffer);
    renderScene(graphics, width, height); // Warm caches and font families.
    auto measure = [&](auto&& operation) {
        const auto started = std::chrono::steady_clock::now();
        for (int frame = 0; frame < frames; ++frame) operation();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };
    const double backgroundMs = measure([&] { drawAnimatedBackground(graphics, width, height); });
    const double headerMs = measure([&] { drawHeader(graphics, width); });
    const double navigationMs = measure([&] { drawNavigation(graphics, height); });
    const double dashboardMs = measure([&] { drawDashboard(graphics, width, height, 88.0f); });
    const double elapsedMs = measure([&] { renderScene(graphics, width, height); });
    const double averageMs = elapsedMs / frames;
    g_page = Page::Effects;
    const double effectsMs = measure([&] { renderScene(graphics, width, height); });
    g_page = Page::Gamepads;
    const double gamepadsMs = measure([&] { renderScene(graphics, width, height); });
    g_dualSenseLive = {};
    g_dualSenseLive.seen = true;
    g_dualSenseLive.lastInputAt = GetTickCount64();
    g_dualSenseVisualAxes.initialized = true;
    int motionFrame = 0;
    const double gamepadsMotionMs = measure([&] {
        const float phase = static_cast<float>(motionFrame++) / frames * 6.28318530718f;
        g_dualSenseVisualAxes.leftX = std::sin(phase) * 0.92f;
        g_dualSenseVisualAxes.leftY = std::cos(phase) * 0.92f;
        g_dualSenseVisualAxes.rightX = std::sin(phase + 1.2f) * 0.78f;
        g_dualSenseVisualAxes.rightY = std::cos(phase + 1.2f) * 0.78f;
        renderScene(graphics, width, height);
    });
    motionFrame = 0;
    const double gamepadsFastMotionMs = measure([&] {
        const float phase = static_cast<float>(motionFrame++) / frames * 6.28318530718f;
        g_dualSenseVisualAxes.leftX = std::sin(phase) * 0.92f;
        g_dualSenseVisualAxes.leftY = std::cos(phase) * 0.92f;
        g_dualSenseVisualAxes.rightX = std::sin(phase + 1.2f) * 0.78f;
        g_dualSenseVisualAxes.rightY = std::cos(phase + 1.2f) * 0.78f;
        drawGamepadFastOverlay(graphics, width, height);
    });
    g_dualSenseLive = {};
    g_dualSenseVisualAxes = {};
    g_page = Page::Dashboard;
    g_launchPreviewMs = 820.0;
    const double launchBuildMs = measure([&] { renderScene(graphics, width, height); });
    g_launchPreviewMs = 1800.0;
    const double launchLogoMs = measure([&] { renderScene(graphics, width, height); });
    g_launchPreviewMs = -1.0;
    g_launchAnimationFinished = true;
    const double effectsAverageMs = effectsMs / frames;
    const double gamepadsAverageMs = gamepadsMs / frames;
    const double gamepadsMotionAverageMs = gamepadsMotionMs / frames;
    const double gamepadsFastMotionAverageMs = gamepadsFastMotionMs / frames;
    const double launchBuildAverageMs = launchBuildMs / frames;
    const double launchLogoAverageMs = launchLogoMs / frames;
    std::ofstream report(destination, std::ios::trunc);
    if (!report) return 24;
    report << std::fixed << std::setprecision(2)
           << "frames=" << frames << "\n"
           << "total_ms=" << elapsedMs << "\n"
           << "average_ms=" << averageMs << "\n"
           << "estimated_fps=" << (averageMs > 0.0 ? 1000.0 / averageMs : 0.0) << "\n"
           << "background_ms=" << backgroundMs / frames << "\n"
           << "header_ms=" << headerMs / frames << "\n"
           << "navigation_ms=" << navigationMs / frames << "\n"
           << "dashboard_ms=" << dashboardMs / frames << "\n"
           << "effects_average_ms=" << effectsAverageMs << "\n"
           << "effects_estimated_fps=" << (effectsAverageMs > 0.0 ? 1000.0 / effectsAverageMs : 0.0) << "\n"
           << "gamepads_average_ms=" << gamepadsAverageMs << "\n"
           << "gamepads_estimated_fps=" << (gamepadsAverageMs > 0.0 ? 1000.0 / gamepadsAverageMs : 0.0) << "\n"
           << "gamepads_motion_average_ms=" << gamepadsMotionAverageMs << "\n"
           << "gamepads_motion_estimated_fps=" << (gamepadsMotionAverageMs > 0.0 ? 1000.0 / gamepadsMotionAverageMs : 0.0) << "\n"
           << "gamepads_fast_motion_average_ms=" << gamepadsFastMotionAverageMs << "\n"
           << "gamepads_fast_motion_estimated_fps=" << (gamepadsFastMotionAverageMs > 0.0 ? 1000.0 / gamepadsFastMotionAverageMs : 0.0) << "\n"
           << "launch_build_average_ms=" << launchBuildAverageMs << "\n"
           << "launch_build_estimated_fps=" << (launchBuildAverageMs > 0.0 ? 1000.0 / launchBuildAverageMs : 0.0) << "\n"
           << "launch_logo_average_ms=" << launchLogoAverageMs << "\n"
           << "launch_logo_estimated_fps=" << (launchLogoAverageMs > 0.0 ? 1000.0 / launchLogoAverageMs : 0.0) << "\n";
    return averageMs <= 33.34 && effectsAverageMs <= 33.34 && gamepadsAverageMs <= 33.34 &&
           gamepadsMotionAverageMs <= 33.34 &&
           gamepadsFastMotionAverageMs <= 16.67 &&
           launchBuildAverageMs <= 33.34 && launchLogoAverageMs <= 33.34 ? 0 : 25;
}

int runSelfTests(const fs::path& destination) {
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) return 20;
    seedCaptureData();
    const AppProfile original = captureCurrentProfile();
    const fs::path testFile = destination / L"profile-roundtrip.rgbcprofile";
    writeProfileToIni(testFile, L"RGBCcontrolProfile", original);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, testFile.c_str());
    const AppProfile restored = readProfileFromIni(testFile, L"RGBCcontrolProfile");
    const bool roundTrip = restored.saved && restored.color == original.color && restored.brightness == original.brightness &&
                           restored.effect == original.effect && restored.effectSpeed == original.effectSpeed &&
                           restored.effectIntensity == original.effectIntensity &&
                           restored.ambientSaturation == original.ambientSaturation &&
                           restored.ambientMonitor == original.ambientMonitor && restored.ambientZones == original.ambientZones &&
                           restored.fanProfile == original.fanProfile &&
                           restored.curveSpeeds == original.curveSpeeds && restored.selectedDevices == original.selectedDevices &&
                           restored.fanValues == original.fanValues;
    g_curveSpeeds = {30, 45, 70, 100};
    const bool interpolation = fanCurveTarget(35) == 30 && fanCurveTarget(50) == 45 &&
                               fanCurveTarget(60) == 58 && fanCurveTarget(85) == 100;
    g_scheduleDayHour = 8;
    g_scheduleNightHour = 22;
    const bool schedule = scheduleUsesDayProfile(8) && scheduleUsesDayProfile(21) &&
                          !scheduleUsesDayProfile(22) && !scheduleUsesDayProfile(2);
    g_duckyMode = 0;
    g_duckyGuideStage = 1;
    const bool duckyStaticGuide = duckyGuideStageCount() == 2 && duckyGuideKind() == 1;
    g_duckyMode = 1;
    g_duckyGuideStage = 2;
    const bool duckyAnimatedGuide = duckyGuideStageCount() == 3 && duckyGuideKind() == 2;
    g_duckyMode = static_cast<int>(std::size(kDuckyModes)) - 1;
    g_duckyGuideStage = 0;
    const bool duckyOffGuide = duckyGuideStageCount() == 1 && duckyGuideKind() == 0;
    const bool realMusicEffect = std::wstring(kEffects[7].internal) == L"Music" &&
                                 std::wstring(kEffects[7].labels[0]) == L"Musique";
    const bool realAmbilightEffect = std::wstring(kEffects[std::size(kEffects) - 1].internal) == L"Ambilight";
    const bool extendedAccentPalette = std::size(kAccentChoices) == 8;
    const std::wstring directArguments = openRgbArguments(g_rgbDevices, L"Direct", 0x123456, false);
    const std::wstring staticArguments = openRgbArguments(g_rgbDevices, L"Static", 0x123456, true);
    const bool numericOpenRgbTargets = directArguments.find(L"--device 0") != std::wstring::npos &&
                                       directArguments.find(L"ASRock") == std::wstring::npos &&
                                       directArguments.find(L"--color") == std::wstring::npos &&
                                       staticArguments.find(L"123456") != std::wstring::npos;
    const bool perDeviceModes = staticModeFor(g_rgbDevices[0]) == L"Static" &&
                                advertisedMode(g_rgbDevices[0], L"Direct") == L"Direct" &&
                                advertisedMode(g_rgbDevices[0], L"Rainbow") == L"Rainbow";
    const bool gradientSpeedMapping = std::abs(gradientDegreesPerSecond(0) - 3.0) < 0.001 &&
                                      std::abs(gradientDegreesPerSecond(100) - 45.0) < 0.001 &&
                                      gradientDegreesPerSecond(20) < gradientDegreesPerSecond(50) &&
                                      gradientDegreesPerSecond(50) < gradientDegreesPerSecond(80);
    bool gradientStaysSaturated = true;
    for (int phase = 0; phase < 360; ++phase) {
        const std::uint32_t color = OpenRgbClient::gradientColor(static_cast<double>(phase), 0.37, 0x7C5CFF, 1.0);
        const int red = static_cast<int>((color >> 16) & 255);
        const int green = static_cast<int>((color >> 8) & 255);
        const int blue = static_cast<int>(color & 255);
        gradientStaysSaturated = gradientStaysSaturated && std::min({red, green, blue}) <= 6 &&
                                 std::max({red, green, blue}) >= 254;
    }
    const bool gradientHasNoSeam = OpenRgbClient::gradientColor(0.0, 0.0, 0x7C5CFF, 0.8) ==
                                       OpenRgbClient::gradientColor(0.0, 1.0, 0x7C5CFF, 0.8) &&
                                   OpenRgbClient::gradientColor(0.0, 0.4, 0x7C5CFF, 0.8) ==
                                       OpenRgbClient::gradientColor(360.0, 0.4, 0x7C5CFF, 0.8);
    RgbDevice asusTransport = g_rgbDevices[0];
    asusTransport.name = L"ASUS TUF GAMING B650-PLUS WIFI";
    asusTransport.vendor = L"ASUS";
    asusTransport.description = L"Aura USB Controller";
    const bool controllerTransportProfiles = OpenRgbClient::prefersAtomicFrames(g_rgbDevices[0]) &&
                                             !OpenRgbClient::prefersAtomicFrames(asusTransport);
    const bool colorOrderPermutations = OpenRgbClient::orderedColor(0x123456, RgbColorOrder::RGB) == 0x123456 &&
                                        OpenRgbClient::orderedColor(0x123456, RgbColorOrder::RBG) == 0x125634 &&
                                        OpenRgbClient::orderedColor(0x123456, RgbColorOrder::GRB) == 0x341256 &&
                                        OpenRgbClient::orderedColor(0x123456, RgbColorOrder::GBR) == 0x345612 &&
                                        OpenRgbClient::orderedColor(0x123456, RgbColorOrder::BRG) == 0x561234 &&
                                        OpenRgbClient::orderedColor(0x123456, RgbColorOrder::BGR) == 0x563412;
    RgbDevice changedIdentity = g_rgbDevices[0];
    changedIdentity.description += L" revision-2";
    const bool certificationIdentity = certificationFingerprint(g_rgbDevices[0]) == certificationFingerprint(g_rgbDevices[0]) &&
                                       certificationFingerprint(g_rgbDevices[0]) != certificationFingerprint(g_rgbDevices[1]) &&
                                       certificationFingerprint(g_rgbDevices[0]) != certificationFingerprint(changedIdentity);
    std::array<BYTE, 64> dualSenseUsb{};
    dualSenseUsb[0] = 0x01;
    dualSenseUsb[1] = 150;
    dualSenseUsb[2] = 90;
    dualSenseUsb[3] = 200;
    dualSenseUsb[4] = 128;
    dualSenseUsb[5] = 81;
    dualSenseUsb[6] = 143;
    dualSenseUsb[7] = 0x9b; // A real USB sequence counter must never be decoded as buttons.
    dualSenseUsb[8] = 0x21; // D-pad up + Cross.
    dualSenseUsb[9] = 0x05; // L1 + L2.
    dualSenseUsb[10] = 0x02; // Touchpad.
    DualSenseLiveState parsedUsb;
    const bool dualSenseUsbInput = parseDualSenseInputReport(dualSenseUsb.data(), dualSenseUsb.size(), false, false, parsedUsb) &&
                                     parsedUsb.seen && !parsedUsb.bluetooth && parsedUsb.buttons[1] && parsedUsb.buttons[4] &&
                                     parsedUsb.buttons[6] && parsedUsb.buttons[13] && parsedUsb.dpad == 1 &&
                                     parsedUsb.leftTrigger == 81 && parsedUsb.rightTrigger == 143 &&
                                     std::count(parsedUsb.buttons.begin(), parsedUsb.buttons.end(), true) == 4;
    std::array<BYTE, 10> dualSenseBluetooth{};
    dualSenseBluetooth[0] = 0x01;
    dualSenseBluetooth[1] = 128;
    dualSenseBluetooth[2] = 128;
    dualSenseBluetooth[3] = 128;
    dualSenseBluetooth[4] = 128;
    dualSenseBluetooth[5] = 0x48; // Neutral D-pad + Circle.
    dualSenseBluetooth[6] = 0x22; // R1 + Options.
    dualSenseBluetooth[7] = 0x05; // PS + Mute.
    dualSenseBluetooth[8] = 40;
    dualSenseBluetooth[9] = 220;
    DualSenseLiveState parsedBluetooth;
    const bool dualSenseBluetoothInput = parseDualSenseInputReport(dualSenseBluetooth.data(), dualSenseBluetooth.size(), true, true, parsedBluetooth) &&
                                           parsedBluetooth.bluetooth && parsedBluetooth.edge && parsedBluetooth.buttons[2] &&
                                           parsedBluetooth.buttons[5] && parsedBluetooth.buttons[9] && parsedBluetooth.buttons[12] &&
                                           parsedBluetooth.buttons[14] && parsedBluetooth.leftTrigger == 40 && parsedBluetooth.rightTrigger == 220;
    std::array<BYTE, 78> dualSenseBluetoothEnhanced{};
    dualSenseBluetoothEnhanced[0] = 0x31;
    dualSenseBluetoothEnhanced[1] = 0x07; // Sequence/tag.
    dualSenseBluetoothEnhanced[2] = 130;
    dualSenseBluetoothEnhanced[3] = 126;
    dualSenseBluetoothEnhanced[4] = 210;
    dualSenseBluetoothEnhanced[5] = 128;
    dualSenseBluetoothEnhanced[6] = 64;
    dualSenseBluetoothEnhanced[7] = 192;
    dualSenseBluetoothEnhanced[9] = 0x82; // D-pad right + Triangle.
    dualSenseBluetoothEnhanced[10] = 0x50; // Create + L3.
    dualSenseBluetoothEnhanced[11] = 0x04; // Mute.
    DualSenseLiveState parsedBluetoothEnhanced;
    const bool dualSenseBluetoothEnhancedInput =
        parseDualSenseInputReport(dualSenseBluetoothEnhanced.data(), dualSenseBluetoothEnhanced.size(), true, false,
                                  parsedBluetoothEnhanced) &&
        parsedBluetoothEnhanced.bluetooth && !parsedBluetoothEnhanced.edge && parsedBluetoothEnhanced.buttons[3] &&
        parsedBluetoothEnhanced.buttons[8] && parsedBluetoothEnhanced.buttons[10] && parsedBluetoothEnhanced.buttons[14] &&
        parsedBluetoothEnhanced.dpad == 2 && parsedBluetoothEnhanced.leftTrigger == 64 &&
        parsedBluetoothEnhanced.rightTrigger == 192;
    std::array<BYTE, 9> dualSenseBluetoothBody{};
    dualSenseBluetoothBody[0] = 120;
    dualSenseBluetoothBody[1] = 136;
    dualSenseBluetoothBody[2] = 132;
    dualSenseBluetoothBody[3] = 124;
    dualSenseBluetoothBody[4] = 0x61; // D-pad up + Circle.
    dualSenseBluetoothBody[5] = 0x04; // L2 digital.
    dualSenseBluetoothBody[6] = 0x01; // PS.
    dualSenseBluetoothBody[7] = 52;
    dualSenseBluetoothBody[8] = 208;
    DualSenseLiveState parsedBluetoothBody;
    const bool dualSenseBluetoothBodyInput =
        parseDualSenseInputReport(dualSenseBluetoothBody.data(), dualSenseBluetoothBody.size(), true, false,
                                  parsedBluetoothBody) && parsedBluetoothBody.bluetooth && parsedBluetoothBody.buttons[2] &&
        parsedBluetoothBody.buttons[6] && parsedBluetoothBody.buttons[12] && parsedBluetoothBody.dpad == 1 &&
        parsedBluetoothBody.leftTrigger == 52 && parsedBluetoothBody.rightTrigger == 208;
    std::array<BYTE, 77> dualSenseBluetoothEnhancedBody{};
    dualSenseBluetoothEnhancedBody[0] = 0x19; // Sequence/tag, report ID stripped.
    dualSenseBluetoothEnhancedBody[1] = 141;
    dualSenseBluetoothEnhancedBody[2] = 119;
    dualSenseBluetoothEnhancedBody[3] = 201;
    dualSenseBluetoothEnhancedBody[4] = 101;
    dualSenseBluetoothEnhancedBody[5] = 88;
    dualSenseBluetoothEnhancedBody[6] = 177;
    dualSenseBluetoothEnhancedBody[8] = 0x44; // D-pad down + Circle.
    dualSenseBluetoothEnhancedBody[9] = 0x82; // R1 + R3.
    dualSenseBluetoothEnhancedBody[10] = 0x01; // PS.
    DualSenseLiveState parsedBluetoothEnhancedBody;
    const bool dualSenseBluetoothEnhancedBodyInput =
        parseDualSenseInputReport(dualSenseBluetoothEnhancedBody.data(), dualSenseBluetoothEnhancedBody.size(), true,
                                  false, parsedBluetoothEnhancedBody) && parsedBluetoothEnhancedBody.buttons[2] &&
        parsedBluetoothEnhancedBody.buttons[5] && parsedBluetoothEnhancedBody.buttons[11] &&
        parsedBluetoothEnhancedBody.buttons[12] && parsedBluetoothEnhancedBody.dpad == 4 &&
        parsedBluetoothEnhancedBody.leftX == 141 && parsedBluetoothEnhancedBody.leftY == 119 &&
        parsedBluetoothEnhancedBody.leftTrigger == 88 && parsedBluetoothEnhancedBody.rightTrigger == 177;
    // Windows may deliver the compact BT report in a buffer padded to the
    // controller's 64-byte USB report size. This specifically guards against
    // the regression where L1/R1 and L2/R2 were read from the USB offsets.
    std::array<BYTE, 64> dualSenseBluetoothPadded{};
    dualSenseBluetoothPadded[0] = 0x01;
    dualSenseBluetoothPadded[1] = 128;
    dualSenseBluetoothPadded[2] = 128;
    dualSenseBluetoothPadded[3] = 128;
    dualSenseBluetoothPadded[4] = 128;
    dualSenseBluetoothPadded[5] = 0x20; // Cross.
    dualSenseBluetoothPadded[6] = 0x03; // L1 + R1.
    dualSenseBluetoothPadded[7] = 0x00;
    dualSenseBluetoothPadded[8] = 71;
    dualSenseBluetoothPadded[9] = 189;
    DualSenseLiveState parsedBluetoothPadded;
    const bool dualSenseBluetoothPaddedInput =
        parseDualSenseInputReport(dualSenseBluetoothPadded.data(), dualSenseBluetoothPadded.size(), true, false,
                                  parsedBluetoothPadded) && parsedBluetoothPadded.bluetooth &&
        parsedBluetoothPadded.buttons[1] && parsedBluetoothPadded.buttons[4] && parsedBluetoothPadded.buttons[5] &&
        parsedBluetoothPadded.leftTrigger == 71 && parsedBluetoothPadded.rightTrigger == 189;
    std::set<std::uint32_t> dualSenseComponentIds;
    bool dualSenseComponentsReady = g_dualSenseMesh.loaded;
    for (std::uint32_t component : g_dualSenseControlComponents) {
        dualSenseComponentsReady = dualSenseComponentsReady && component != UINT32_MAX;
        if (component != UINT32_MAX) dualSenseComponentIds.insert(component);
    }
    for (std::uint32_t component : g_dualSenseDpadComponents) {
        dualSenseComponentsReady = dualSenseComponentsReady && component != UINT32_MAX;
        if (component != UINT32_MAX) dualSenseComponentIds.insert(component);
    }
    // Every visible input must own a different connected part.  This catches
    // the regression where L1/R1, Create/Options or a face button accidentally
    // selected the shell and made another region move.
    dualSenseComponentsReady = dualSenseComponentsReady &&
                               dualSenseComponentIds.size() ==
                                   g_dualSenseControlComponents.size() + g_dualSenseDpadComponents.size();
    // Keep a compact topology report beside the self-test output. It makes
    // layered trigger caps and button pieces diagnosable without modifying or
    // probing the user's HID device.
    struct ComponentTestStats {
        int count = 0;
        double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
        float minX = std::numeric_limits<float>::infinity();
        float maxX = -std::numeric_limits<float>::infinity();
        float minY = std::numeric_limits<float>::infinity();
        float maxY = -std::numeric_limits<float>::infinity();
        float minZ = std::numeric_limits<float>::infinity();
        float maxZ = -std::numeric_limits<float>::infinity();
    };
    std::unordered_map<std::uint32_t, ComponentTestStats> componentTestStats;
    for (const DualSenseMeshVertex& vertex : g_dualSenseMesh.vertices) {
        ComponentTestStats& stats = componentTestStats[vertex.component];
        ++stats.count;
        stats.sumX += vertex.x; stats.sumY += vertex.y; stats.sumZ += vertex.z;
        stats.minX = std::min(stats.minX, vertex.x); stats.maxX = std::max(stats.maxX, vertex.x);
        stats.minY = std::min(stats.minY, vertex.y); stats.maxY = std::max(stats.maxY, vertex.y);
        stats.minZ = std::min(stats.minZ, vertex.z); stats.maxZ = std::max(stats.maxZ, vertex.z);
    }
    std::ofstream topology(destination / L"dualsense-topology.tsv", std::ios::trunc);
    if (topology) {
        topology << "component\tcount\tcx\tcy\tcz\tminx\tmaxx\tminy\tmaxy\tminz\tmaxz\tbindings\n";
        for (const auto& [component, stats] : componentTestStats) {
            if (stats.count <= 0) continue;
            const double centerX = stats.sumX / stats.count;
            const double centerY = stats.sumY / stats.count;
            const bool shoulder = std::abs(std::abs(centerX) - 0.61) < 0.24 && centerY > 0.42;
            const bool dpad = centerX < -0.42 && centerX > -0.82 && centerY > 0.12 && centerY < 0.46;
            const bool touchpad = component == g_dualSenseControlComponents[13];
            const bool touchpadSeam = std::abs(centerX) > 0.24 && std::abs(centerX) < 0.48 &&
                                      centerY > 0.14 && centerY < 0.58 && stats.maxZ > 0.25f;
            if (!shoulder && !dpad && !touchpad && !touchpadSeam) continue;
            std::string bindings;
            for (std::size_t index = 0; index < g_dualSenseControlComponents.size(); ++index) {
                if (g_dualSenseControlComponents[index] == component) bindings += " C" + std::to_string(index);
            }
            for (std::size_t index = 0; index < g_dualSenseDpadComponents.size(); ++index) {
                if (g_dualSenseDpadComponents[index] == component) bindings += " D" + std::to_string(index);
            }
            topology << component << '\t' << stats.count << '\t' << centerX << '\t'
                     << centerY << '\t' << stats.sumZ / stats.count << '\t'
                     << stats.minX << '\t' << stats.maxX << '\t' << stats.minY << '\t'
                     << stats.maxY << '\t' << stats.minZ << '\t' << stats.maxZ << '\t'
                     << bindings << '\n';
        }
    }
    const DualSenseLiveState savedDualSenseLive = g_dualSenseLive;
    g_dualSenseLive = {};
    g_dualSenseLive.seen = true;
    g_dualSenseLive.dpad = 2; // East / physical right arrow.
    g_dualSenseLive.lastInputAt = GetTickCount64();
    const std::uint32_t rightDpadMask = dualSenseActiveControlMask(true);
    bool rightDpadPress = false;
    bool rightDpadMoves = false;
    for (const DualSenseMeshVertex& vertex : g_dualSenseMesh.vertices) {
        if (vertex.component != g_dualSenseDpadComponents[1]) continue;
        rightDpadPress = dualSenseButtonPressAt(vertex, true) > 0.5f;
        rightDpadMoves = animateDualSenseVertex(vertex, true).z < vertex.z - 0.001f;
        break;
    }
    const bool dualSenseDpadRight = (rightDpadMask & (1u << 16)) != 0 &&
                                    (rightDpadMask & (1u << 15)) == 0 &&
                                    (rightDpadMask & (1u << 17)) == 0 &&
                                    (rightDpadMask & (1u << 18)) == 0 &&
                                    rightDpadPress && rightDpadMoves;
    const bool layeredDualSenseControls = g_dualSenseControlComponentGroups[6].size() >= 3 &&
                                          g_dualSenseControlComponentGroups[7].size() >= 3 &&
                                          std::all_of(g_dualSenseDpadComponentGroups.begin(),
                                                      g_dualSenseDpadComponentGroups.end(),
                                                      [](const auto& group) { return group.size() >= 6; });
    const bool measuredTouchpadLights = std::all_of(g_dualSenseTouchpadLeftEdge.begin(),
                                                    g_dualSenseTouchpadLeftEdge.end(),
                                                    [](const DualSenseLightAnchor& point) {
                                                        return point.x < -0.20f && point.z > 0.20f;
                                                    }) &&
                                        std::all_of(g_dualSenseTouchpadRightEdge.begin(),
                                                    g_dualSenseTouchpadRightEdge.end(),
                                                    [](const DualSenseLightAnchor& point) {
                                                        return point.x > 0.20f && point.z > 0.20f;
                                                    });
    g_dualSenseLive = savedDualSenseLive;
    const std::uint32_t savedBaseColor = g_baseColor;
    const int savedBrightness = g_brightness;
    const bool savedEffectActive = g_effectActive.load();
    const int savedActiveEffect = g_activeEffectIndex.load();
    const ULONGLONG savedEffectStartedAt = g_effectStartedAt;
    g_baseColor = 0x7C5CFF;
    g_brightness = 80;
    g_effectActive = true;
    g_activeEffectIndex = 12; // Smooth gradient.
    g_effectStartedAt = 1000;
    const std::uint32_t previewFirst = gamepadLightingPreviewRgb(2000);
    const std::uint32_t previewSecond = gamepadLightingPreviewRgb(4500);
    const bool gamepadAnimatedLightingPreview = previewFirst != previewSecond;
    g_baseColor = savedBaseColor;
    g_brightness = savedBrightness;
    g_effectActive = savedEffectActive;
    g_activeEffectIndex = savedActiveEffect;
    g_effectStartedAt = savedEffectStartedAt;
    fs::remove(testFile, error);
    return roundTrip && interpolation && schedule && duckyStaticGuide && duckyAnimatedGuide && duckyOffGuide &&
           realMusicEffect && realAmbilightEffect && extendedAccentPalette && numericOpenRgbTargets && perDeviceModes &&
           gradientSpeedMapping && gradientStaysSaturated && gradientHasNoSeam && controllerTransportProfiles &&
           colorOrderPermutations && dualSenseUsbInput && dualSenseBluetoothInput && dualSenseBluetoothEnhancedInput &&
           dualSenseBluetoothBodyInput && dualSenseBluetoothEnhancedBodyInput && dualSenseBluetoothPaddedInput &&
           dualSenseComponentsReady && dualSenseDpadRight && layeredDualSenseControls && measuredTouchpadLights &&
           gamepadAnimatedLightingPreview && certificationIdentity ? 0 : 21;
}

int createInterfaceCaptures(const fs::path& destination) {
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) return 10;
    seedCaptureData();
    bool ok = true;
    ok = saveLaunchCapture(1180, 760, destination / L"launch-build.png", 820.0) && ok;
    ok = saveLaunchCapture(1180, 760, destination / L"launch-logo.png", 1800.0) && ok;
    ok = saveLaunchCapture(1180, 760, destination / L"launch-reveal.png", 2820.0) && ok;
    ok = savePageCapture(Page::Dashboard, 1180, 760, destination / L"dashboard.png") && ok;
    ok = savePageCapture(Page::Devices, 1180, 760, destination / L"devices.png") && ok;
    RgbDevice dualSenseCapture;
    dualSenseCapture.index = 2;
    dualSenseCapture.name = L"Sony DualSense Wireless Controller";
    dualSenseCapture.vendor = L"Sony";
    dualSenseCapture.description = L"Bluetooth HID";
    dualSenseCapture.type = L"Manette";
    dualSenseCapture.zones = 2;
    dualSenseCapture.leds = 7;
    dualSenseCapture.providerId = L"plugin:com.sony.dualsense";
    dualSenseCapture.capabilities = DeviceCapability::Discovery | DeviceCapability::Lighting | DeviceCapability::HotPlug | DeviceCapability::Zones;
    dualSenseCapture.modes = {L"Static"};
    dualSenseCapture.auxiliaryLedMask = 0x1f;
    g_rgbDevices.push_back(dualSenseCapture);
    g_dualSensePlayerLedsEnabled = true;
    g_dualSenseLive = {};
    g_dualSenseLive.seen = true;
    g_dualSenseLive.bluetooth = true;
    g_dualSenseLive.lastInputAt = GetTickCount64();
    g_dualSenseLive.leftX = 92;
    g_dualSenseLive.leftY = 172;
    g_dualSenseLive.rightX = 184;
    g_dualSenseLive.rightY = 107;
    g_dualSenseLive.leftTrigger = 84;
    g_dualSenseLive.rightTrigger = 201;
    g_dualSenseLive.dpad = 2; // Capture the repaired physical right arrow by itself.
    g_dualSenseLive.buttons[1] = true;
    g_dualSenseLive.buttons[4] = true;
    g_dualSenseLive.buttons[7] = true;
    g_dualSenseLive.buttons[13] = true;
    updateDualSenseVisualAxes(GetTickCount64(), true);
    ok = savePageCapture(Page::Gamepads, 1180, 760, destination / L"gamepads.png") && ok;
    g_captureSuppressDualSenseLightOverlays = true;
    ok = savePageCapture(Page::Gamepads, 1180, 760, destination / L"gamepads-model-geometry.png") && ok;
    g_captureSuppressDualSenseLightOverlays = false;
    ok = savePageCapture(Page::Gamepads, 1020, 680, destination / L"gamepads-small.png") && ok;
    const float captureYaw = g_gamepadYaw;
    const float capturePitch = g_gamepadPitch;
    g_gamepadYaw = 0.72f;
    g_gamepadPitch = 0.18f;
    g_dualSenseModelCache.reset();
    g_dualSenseControlCache.reset();
    g_dualSenseMovingControlCache.reset();
    ok = savePageCapture(Page::Gamepads, 1180, 760, destination / L"gamepads-angled.png") && ok;
    g_gamepadYaw = 3.14159f;
    g_gamepadPitch = 0.12f;
    g_dualSenseModelCache.reset();
    g_dualSenseControlCache.reset();
    g_dualSenseMovingControlCache.reset();
    ok = savePageCapture(Page::Gamepads, 1180, 760, destination / L"gamepads-rear.png") && ok;
    g_gamepadYaw = captureYaw;
    g_gamepadPitch = capturePitch;
    g_dualSenseModelCache.reset();
    g_dualSenseControlCache.reset();
    g_dualSenseMovingControlCache.reset();
    rgbToHsv(g_baseColor, g_pickerHue, g_pickerSaturation, g_pickerBrightness);
    g_colorPickerOpen = true;
    ok = savePageCapture(Page::Gamepads, 1180, 760, destination / L"gamepads-color-picker.png") && ok;
    g_colorPickerOpen = false;
    g_rgbDevices.pop_back();
    g_dualSenseLive = {};
    g_dualSenseVisualAxes = {};
    g_duckyAssistantStep = 0;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-style.png") && ok;
    g_duckyAssistantStep = 1;
    g_duckyCalibrationPhase = 0;
    g_duckyCalibrationAttempt = 0;
    g_duckyModeConfirmed = false;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-keyboard.png") && ok;
    g_duckyCalibrationPhase = 1;
    g_duckyCalibrationAttempt = 3;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-search.png") && ok;
    g_duckyCalibrationPhase = 2;
    g_duckyModeConfirmed = true;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-confirmed.png") && ok;
    g_duckyGuideStage = 1;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-color.png") && ok;
    g_duckyMode = 1;
    g_duckyGuideStage = 2;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-speed.png") && ok;
    g_duckyMode = 0;
    g_duckyGuideStage = 0;
    g_duckyAssistantStep = 2;
    ok = savePageCapture(Page::DuckyAssistant, 1180, 760, destination / L"ducky-assistant-sync.png") && ok;
    g_duckyAssistantStep = 0;
    ok = savePageCapture(Page::Effects, 1180, 760, destination / L"effects.png") && ok;
    g_selectedEffect = 7;
    g_effectActive = true;
    g_audioCaptureReady = true;
    g_audioSignal = true;
    g_audioVolume = 0.74;
    g_audioBass = 0.82;
    g_audioMid = 0.48;
    g_audioTreble = 0.67;
    g_status = localized(L"Musique réelle active · écoute de la sortie Windows.",
                         L"Real music mode active · listening to Windows output.",
                         L"Echter Musikmodus aktiv · Windows-Ausgabe wird analysiert.",
                         L"真实音乐模式已启用 · 正在监听 Windows 输出。");
    ok = savePageCapture(Page::Effects, 1180, 760, destination / L"music-reactive.png") && ok;
    g_effectActive = false;
    g_audioCaptureReady = false;
    g_audioSignal = false;
    g_audioVolume = g_audioBass = g_audioMid = g_audioTreble = 0.0;
    g_selectedEffect = 0;
    g_selectedEffect = static_cast<int>(std::size(kEffects)) - 1;
    g_effectActive = true;
    g_ambientCaptureReady = true;
    g_ambientFps = 28.0;
    g_ambientColors[0] = 0xB24CFF;
    g_ambientColors[1] = 0x3B8CFF;
    g_ambientColors[2] = 0x20D9C0;
    g_ambientColors[3] = 0xFF5F8F;
    g_ambientAverage = 0x6B86D8;
    g_status = localized(L"Ambilight actif · les couleurs de l'écran sont analysées.",
                         L"Ambilight active · screen colors are being analyzed.",
                         L"Ambilight aktiv · Bildschirmfarben werden analysiert.",
                         L"屏幕氛围灯已启用 · 正在分析屏幕颜色。");
    ok = savePageCapture(Page::Effects, 1180, 760, destination / L"ambilight.png") && ok;
    g_effectActive = false;
    g_ambientCaptureReady = false;
    g_ambientFps = 0.0;
    g_selectedEffect = 0;
    g_compatibilityFinished = true;
    g_compatibilityProgress = 100;
    g_compatibilityStep = 5;
    g_compatOpenRgb = g_compatAudio = g_compatScreen = g_compatHardware = true;
    ok = savePageCapture(Page::Compatibility, 1180, 760, destination / L"compatibility.png") && ok;
    g_certificationDevice = 0;
    g_certificationStage = 1;
    g_certificationApplying = false;
    g_certificationAwaitingAnswer = true;
    g_certificationStatus = localized(L"Toutes les zones attendues affichent-elles du vert ?", L"Do all expected zones show green?",
                                      L"Zeigen alle erwarteten Zonen Grün?", L"所有预期区域都显示绿色吗？");
    ok = savePageCapture(Page::Compatibility, 1180, 760, destination / L"compatibility-color-validation.png") && ok;
    g_certificationStage = 2;
    g_certificationMotionTest = true;
    g_certificationRefreshAttempt = 1;
    g_certificationStatus = localized(L"L'animation est-elle fluide, stable et sans clignotement ?",
                                      L"Is the animation smooth, stable and flicker-free?",
                                      L"Ist die Animation flüssig, stabil und flimmerfrei?",
                                      L"动画是否流畅、稳定且无闪烁？");
    ok = savePageCapture(Page::Compatibility, 1180, 760, destination / L"compatibility-animation-validation.png") && ok;
    g_certificationDevice = -1;
    g_certificationStage = 0;
    g_certificationMotionTest = false;
    g_certificationRefreshAttempt = 0;
    g_certificationAwaitingAnswer = false;
    g_certificationStatus.clear();
    ok = savePageCapture(Page::Compatibility, 1180, 760, destination / L"compatibility-devices.png", 330) && ok;
    g_duckyDetected = false;
    ok = savePageCapture(Page::Compatibility, 1180, 760, destination / L"compatibility-without-ducky.png") && ok;
    g_duckyDetected = true;
    ok = savePageCapture(Page::Profiles, 1180, 760, destination / L"profiles.png") && ok;
    ok = savePageCapture(Page::Fans, 1180, 760, destination / L"fans.png") && ok;
    ok = savePageCapture(Page::Fans, 1180, 760, destination / L"fans-scrolled.png", 260.0f) && ok;
    g_fanProfile = FanProfile::Quiet;
    ok = savePageCapture(Page::Fans, 1180, 760, destination / L"fans-quiet.png") && ok;
    g_fanProfile = FanProfile::Auto;
    ok = savePageCapture(Page::Diagnostics, 1180, 760, destination / L"diagnostics.png") && ok;
    ok = savePageCapture(Page::Settings, 1180, 760, destination / L"settings.png") && ok;
    g_startupEnabled = true;
    g_minimizeToTray = true;
    g_scheduleEnabled = true;
    ok = savePageCapture(Page::Settings, 1180, 760, destination / L"settings-automation.png", 600) && ok;
    g_startupEnabled = false;
    g_scheduleEnabled = false;
    g_accentPreset = 5;
    g_detectionIntervalSeconds = 15;
    g_effectQuality = 2;
    g_closeToTray = true;
    g_reduceMotion = true;
    ok = savePageCapture(Page::Settings, 1180, 760, destination / L"settings-customization.png", 480) && ok;
    g_accentPreset = 0;
    g_detectionIntervalSeconds = 5;
    g_effectQuality = 1;
    g_closeToTray = false;
    g_reduceMotion = false;
    g_selectedEffect = 12;
    ok = savePageCapture(Page::Effects, 1180, 760, destination / L"gradient-effect.png") && ok;
    g_selectedEffect = 0;
    rgbToHsv(g_baseColor, g_pickerHue, g_pickerSaturation, g_pickerBrightness);
    g_colorPickerOpen = true;
    ok = savePageCapture(Page::Dashboard, 1180, 760, destination / L"color-picker.png") && ok;
    g_colorPickerOpen = false;
    ok = savePageCapture(Page::Dashboard, 1020, 680, destination / L"dashboard-small.png") && ok;
    ok = savePageCapture(Page::Devices, 1020, 680, destination / L"devices-small.png") && ok;
    g_duckyAssistantStep = 1;
    g_duckyGuideStage = 0;
    g_duckyCalibrationPhase = 1;
    g_duckyCalibrationAttempt = 2;
    g_duckyModeConfirmed = false;
    ok = savePageCapture(Page::DuckyAssistant, 1020, 680, destination / L"ducky-assistant-search-small.png") && ok;
    g_duckyGuideStage = 1;
    g_duckyCalibrationPhase = 2;
    g_duckyModeConfirmed = true;
    ok = savePageCapture(Page::DuckyAssistant, 1020, 680, destination / L"ducky-assistant-small.png") && ok;
    g_duckyAssistantStep = 0;
    g_duckyGuideStage = 0;
    g_duckyCalibrationPhase = 0;
    g_duckyCalibrationAttempt = 0;
    g_duckyModeConfirmed = false;
    g_hoverAction = Action::Close;
    g_hoverStartedAt = 0;
    ok = savePageCapture(Page::Dashboard, 1020, 680, destination / L"window-controls-close-hover.png") && ok;
    g_hoverAction = Action::None;
    g_downloadedUpdate = destination / L"RGBCcontrol-Setup-new.exe";
    ok = savePageCapture(Page::Dashboard, 1020, 680, destination / L"header-update-ready.png") && ok;
    g_downloadedUpdate.clear();
    ok = savePageCapture(Page::Effects, 1020, 680, destination / L"effects-small.png") && ok;
    g_selectedEffect = 7;
    g_effectActive = true;
    g_audioCaptureReady = true;
    g_audioSignal = true;
    g_audioVolume = 0.74;
    g_audioBass = 0.82;
    g_audioMid = 0.48;
    g_audioTreble = 0.67;
    g_status = localized(L"Musique réelle active · écoute de la sortie Windows.",
                         L"Real music mode active · listening to Windows output.",
                         L"Echter Musikmodus aktiv · Windows-Ausgabe wird analysiert.",
                         L"真实音乐模式已启用 · 正在监听 Windows 输出。");
    ok = savePageCapture(Page::Effects, 1020, 680, destination / L"music-reactive-small.png", 76) && ok;
    g_effectActive = false;
    g_audioCaptureReady = false;
    g_audioSignal = false;
    g_audioVolume = g_audioBass = g_audioMid = g_audioTreble = 0.0;
    g_selectedEffect = 0;
    g_selectedEffect = static_cast<int>(std::size(kEffects)) - 1;
    g_effectActive = true;
    g_ambientCaptureReady = true;
    g_ambientFps = 28.0;
    g_ambientColors[0] = 0xB24CFF;
    g_ambientColors[1] = 0x3B8CFF;
    g_ambientColors[2] = 0x20D9C0;
    g_ambientColors[3] = 0xFF5F8F;
    g_ambientAverage = 0x6B86D8;
    ok = savePageCapture(Page::Effects, 1020, 680, destination / L"ambilight-small.png", 86) && ok;
    g_effectActive = false;
    g_ambientCaptureReady = false;
    g_selectedEffect = 0;
    ok = savePageCapture(Page::Compatibility, 1020, 680, destination / L"compatibility-small.png") && ok;
    ok = savePageCapture(Page::Profiles, 1020, 680, destination / L"profiles-small.png") && ok;
    ok = savePageCapture(Page::Fans, 1020, 680, destination / L"fans-small.png") && ok;
    ok = savePageCapture(Page::Diagnostics, 1020, 680, destination / L"diagnostics-small.png") && ok;
    ok = savePageCapture(Page::Settings, 1020, 680, destination / L"settings-small.png") && ok;
    g_startupEnabled = true;
    g_minimizeToTray = true;
    g_scheduleEnabled = true;
    ok = savePageCapture(Page::Settings, 1020, 680, destination / L"settings-automation-small.png", 700) && ok;
    g_startupEnabled = false;
    g_scheduleEnabled = false;
    g_selectedEffect = 12;
    ok = savePageCapture(Page::Effects, 1020, 680, destination / L"gradient-effect-small.png") && ok;
    g_selectedEffect = 0;
    rgbToHsv(g_baseColor, g_pickerHue, g_pickerSaturation, g_pickerBrightness);
    g_colorPickerOpen = true;
    ok = savePageCapture(Page::Dashboard, 1020, 680, destination / L"color-picker-small.png") && ok;
    g_colorPickerOpen = false;
    return ok ? 0 : 11;
}

void setSliderValue(Action action, int index, float mouseX) {
    for (const HitTarget& hit : g_hits) {
        if (hit.action != action || hit.index != index) continue;
        int minimum = static_cast<int>(hit.value);
        float ratio = std::clamp((mouseX - hit.rect.X) / hit.rect.Width, 0.0f, 1.0f);
        int value = static_cast<int>(std::lround(minimum + ratio * (100 - minimum)));
        if (action == Action::Brightness) g_brightness = value;
        else if (action == Action::EffectSpeed) g_effectSpeed = value;
        else if (action == Action::EffectIntensity) g_effectIntensity = value;
        else if (action == Action::AmbientSaturation) g_ambientSaturation = value;
        else if (action == Action::FanSlider && index >= 0 && index < static_cast<int>(g_fans.size())) {
            g_fans[index].desired = value;
            g_fanProfile = FanProfile::Custom;
            g_fanCurveEnabled = false;
            g_curveTarget = -1;
        }
        g_activeProfile = -1;
        InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
}

void setFanCurvePoint(int index, float mouseY) {
    if (index < 0 || index >= static_cast<int>(g_curveSpeeds.size())) return;
    for (const HitTarget& hit : g_hits) {
        if (hit.action != Action::FanCurvePoint || hit.index != index) continue;
        const float graphTop = hit.rect.Y + 4.0f;
        const float graphHeight = hit.rect.Height - 8.0f;
        const float usableTop = graphTop + 14.0f;
        const float usableBottom = graphTop + graphHeight - 14.0f;
        const float ratio = std::clamp((usableBottom - mouseY) / std::max(1.0f, usableBottom - usableTop), 0.0f, 1.0f);
        int value = static_cast<int>(std::lround(30.0f + ratio * 70.0f));
        if (index > 0) value = std::max(value, g_curveSpeeds[index - 1]);
        if (index + 1 < static_cast<int>(g_curveSpeeds.size())) value = std::min(value, g_curveSpeeds[index + 1]);
        g_curveSpeeds[index] = value;
        g_curvePreset = -1;
        g_activeProfile = -1;
        g_curveTarget = -1;
        InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
}

void chooseColor() {
    rgbToHsv(g_baseColor, g_pickerHue, g_pickerSaturation, g_pickerBrightness);
    g_colorPickerOpen = true;
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
}

bool pickerAction(Action action) {
    return action == Action::PickerBackdrop || action == Action::PickerWheel || action == Action::PickerBrightness ||
           action == Action::PickerPreset || action == Action::PickerCancel || action == Action::PickerApply;
}

void updatePickerWheel(float mouseX, float mouseY) {
    for (auto iterator = g_hits.rbegin(); iterator != g_hits.rend(); ++iterator) {
        if (iterator->action != Action::PickerWheel) continue;
        const float centerX = iterator->rect.X + iterator->rect.Width / 2.0f;
        const float centerY = iterator->rect.Y + iterator->rect.Height / 2.0f;
        const double dx = mouseX - centerX;
        const double dy = mouseY - centerY;
        const double radius = iterator->rect.Width / 2.0 - 3.0;
        g_pickerSaturation = std::clamp(std::sqrt(dx * dx + dy * dy) / radius, 0.0, 1.0);
        g_pickerHue = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
        if (g_pickerHue < 0) g_pickerHue += 360.0;
        if (g_window) InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
}

void updatePickerBrightness(float mouseX) {
    for (auto iterator = g_hits.rbegin(); iterator != g_hits.rend(); ++iterator) {
        if (iterator->action != Action::PickerBrightness) continue;
        const float ratio = std::clamp((mouseX - iterator->rect.X) / iterator->rect.Width, 0.0f, 1.0f);
        g_pickerBrightness = static_cast<int>(std::lround(ratio * 100.0f));
        if (g_window) InvalidateRect(g_window, nullptr, FALSE);
        return;
    }
}

void handleAction(const HitTarget& hit, float mouseX, float mouseY) {
    switch (hit.action) {
        case Action::NavDashboard: g_page = Page::Dashboard; g_scrollOffset = 0; break;
        case Action::NavDevices: g_page = Page::Devices; g_scrollOffset = 0; break;
        case Action::NavGamepads: g_page = Page::Gamepads; g_scrollOffset = 0; break;
        case Action::NavEffects: g_page = Page::Effects; g_scrollOffset = 0; break;
        case Action::NavProfiles: g_page = Page::Profiles; g_scrollOffset = 0; break;
        case Action::NavFans: g_page = Page::Fans; g_scrollOffset = 0; break;
        case Action::NavDiagnostics: g_page = Page::Diagnostics; g_scrollOffset = 0; break;
        case Action::NavSettings: g_page = Page::Settings; g_scrollOffset = 0; break;
        case Action::Donate: MessageBoxW(g_window,
            localized(L"Le bouton est prêt. Ton lien PayPal sera ajouté plus tard.", L"The button is ready. Your PayPal link will be added later.",
                      L"Die Schaltfläche ist bereit. Dein PayPal-Link wird später ergänzt.", L"按钮已就绪，稍后可添加 PayPal 链接。"),
            L"RGBCcontrol", MB_OK | MB_ICONINFORMATION); break;
        case Action::Refresh: startScan(true); break;
        case Action::HeaderStatus: g_page = Page::Diagnostics; g_scrollOffset = 0; break;
        case Action::HeaderUpdate: g_page = Page::Settings; g_scrollOffset = 0; break;
        case Action::DeviceOpenEffects: g_page = Page::Effects; g_scrollOffset = 0; break;
        case Action::DeviceOpenFans: g_page = Page::Fans; g_scrollOffset = 0; break;
        case Action::DeviceOpenDiagnostics: g_page = Page::Diagnostics; g_scrollOffset = 0; break;
        case Action::DeviceOpenDuckyAssistant:
            if (!g_duckyDetected) break;
            g_page = Page::DuckyAssistant;
            g_duckyAssistantStep = 0;
            g_duckyGuideStage = 0;
            g_duckyCalibrationPhase = 0;
            g_duckyCalibrationAttempt = 0;
            g_duckyModeConfirmed = false;
            g_scrollOffset = 0;
            break;
        case Action::DeviceOpenCompatibility:
            g_page = Page::Compatibility;
            g_scrollOffset = 0;
            break;
        case Action::CompatibilityBack:
            if (g_certificationDevice < 0 && !g_certificationApplying && !g_certificationAwaitingAnswer) {
                g_page = Page::Devices;
                g_scrollOffset = 0;
            }
            break;
        case Action::CompatibilityRun: startCompatibilityAudit(); break;
        case Action::CertificationStart: beginCertification(hit.index); break;
        case Action::CertificationYes: answerCertification(true); break;
        case Action::CertificationNo: answerCertification(false); break;
        case Action::GamepadResetView:
            g_gamepadYaw = 0.0f;
            g_gamepadPitch = 0.12f;
            break;
        case Action::GamepadTogglePlayerLeds:
            g_dualSensePlayerLedsEnabled = !g_dualSensePlayerLedsEnabled;
            saveAutomationSettings();
            applyDualSenseColor();
            break;
        case Action::GamepadApplyColor: applyDualSenseColor(); break;
        case Action::GamepadUseNative:
            if (g_xboxModeEnabled || g_xboxBridgeProcess) stopXboxBridge();
            saveAutomationSettings();
            break;
        case Action::GamepadUseXbox:
            if (g_xboxBridgeStatus == XboxBridgeStatus::Ready || g_xboxBridgeStatus == XboxBridgeStatus::Starting) break;
            if (!g_isAdministrator) {
                const int answer = MessageBoxW(g_window,
                    localized(L"La première activation installe le contrôleur virtuel Windows. Relancer RGBCcontrol en administrateur ?",
                              L"The first activation installs the Windows virtual controller. Restart RGBCcontrol as administrator?",
                              L"Bei der ersten Aktivierung wird der virtuelle Windows-Controller installiert. RGBCcontrol als Administrator neu starten?",
                              L"首次激活会安装 Windows 虚拟控制器。是否以管理员身份重新启动 RGBCcontrol？"),
                    L"RGBCcontrol · XInput", MB_YESNO | MB_ICONQUESTION);
                if (answer == IDYES) {
                    wchar_t executable[MAX_PATH]{};
                    GetModuleFileNameW(nullptr, executable, MAX_PATH);
                    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"runas", executable, L"--enable-xbox",
                                                               g_appDirectory.c_str(), SW_SHOWNORMAL)) > 32) {
                        DestroyWindow(g_window);
                        return;
                    }
                }
                break;
            }
            g_xboxModeEnabled = true;
            closeXboxBridgeHandles();
            if (startXboxBridge() && g_dualSenseLive.seen) sendXboxState(g_dualSenseLive);
            saveAutomationSettings();
            break;
        case Action::DuckyBack: g_page = Page::Devices; g_scrollOffset = 0; break;
        case Action::DuckyMode:
            if (hit.index >= 0 && hit.index < static_cast<int>(std::size(kDuckyModes))) {
                g_duckyMode = hit.index;
                g_duckyGuideStage = 0;
                g_duckyCalibrationPhase = 0;
                g_duckyCalibrationAttempt = 0;
                g_duckyModeConfirmed = false;
                g_duckyStatus.clear();
                saveAutomationSettings();
            }
            break;
        case Action::DuckyPrevious:
            if (g_duckyAssistantStep == 1 && duckyGuideKind() == 0 && g_duckyCalibrationPhase > 0) {
                if (g_duckyCalibrationPhase == 2) g_duckyModeConfirmed = false;
                --g_duckyCalibrationPhase;
                if (g_duckyCalibrationPhase == 0) g_duckyCalibrationAttempt = 0;
            } else if (g_duckyAssistantStep == 1 && g_duckyGuideStage > 0) {
                --g_duckyGuideStage;
            } else if (g_duckyAssistantStep == 2) {
                g_duckyAssistantStep = 1;
                g_duckyGuideStage = duckyGuideStageCount() - 1;
            } else {
                g_duckyAssistantStep = std::max(0, g_duckyAssistantStep - 1);
                g_duckyGuideStage = 0;
            }
            g_scrollOffset = 0;
            break;
        case Action::DuckyNext:
            if (g_duckyAssistantStep == 0) {
                g_duckyAssistantStep = 1;
                g_duckyGuideStage = 0;
                g_duckyCalibrationPhase = 0;
                g_duckyCalibrationAttempt = 0;
                g_duckyModeConfirmed = false;
            } else if (g_duckyAssistantStep == 1) {
                if (duckyGuideKind() == 0 && g_duckyCalibrationPhase == 0) {
                    if (g_duckyMode == static_cast<int>(std::size(kDuckyModes)) - 1) {
                        g_duckyCalibrationPhase = 2;
                        g_duckyModeConfirmed = true;
                    } else {
                        g_duckyCalibrationPhase = 1;
                        g_duckyCalibrationAttempt = 1;
                    }
                } else if (duckyGuideKind() == 0 && g_duckyCalibrationPhase == 1) {
                    g_duckyCalibrationPhase = 2;
                    g_duckyModeConfirmed = true;
                } else if (g_duckyGuideStage + 1 < duckyGuideStageCount()) {
                    ++g_duckyGuideStage;
                } else {
                    g_duckyAssistantStep = 2;
                }
            } else {
                g_duckyAssistantStep = 2;
            }
            g_scrollOffset = 0;
            break;
        case Action::DuckyCalibrationNextMode:
            if (g_duckyAssistantStep == 1 && duckyGuideKind() == 0 && g_duckyCalibrationPhase == 1) {
                g_duckyCalibrationAttempt = g_duckyCalibrationAttempt >= 12 ? 1 : g_duckyCalibrationAttempt + 1;
            }
            break;
        case Action::DuckyCalibrationRestart:
            g_duckyCalibrationPhase = 0;
            g_duckyCalibrationAttempt = 0;
            g_duckyModeConfirmed = false;
            break;
        case Action::DuckySync: syncDuckyCompanions(); break;
        case Action::DuckyOpenManual:
            ShellExecuteW(g_window, L"open",
                          L"https://cdn.shopify.com/s/files/1/0728/4382/1295/files/Ducky_One2_mini_usermanual-DKON1861ST.pdf?v=1745574595",
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Action::Minimize:
            if (g_minimizeToTray && g_trayAdded) ShowWindow(g_window, SW_HIDE);
            else ShowWindow(g_window, SW_MINIMIZE);
            break;
        case Action::Maximize: ShowWindow(g_window, IsZoomed(g_window) ? SW_RESTORE : SW_MAXIMIZE); break;
        case Action::Close: SendMessageW(g_window, WM_CLOSE, 0, 0); return;
        case Action::ToggleRgb:
            if (hit.index >= 0 && hit.index < static_cast<int>(g_rgbDevices.size())) {
                g_rgbDevices[hit.index].selected = !g_rgbDevices[hit.index].selected;
                if (!g_rgbDevices[hit.index].selected && PluginEngine::isPluginDevice(g_rgbDevices[hit.index])) {
                    const RgbDevice device = g_rgbDevices[hit.index];
                    g_pluginThreads.emplace_back([device] { g_pluginEngine.stop(device); });
                }
                g_activeProfile = -1;
            }
            break;
        case Action::PickColor: chooseColor(); break;
        case Action::SetColor: g_baseColor = hit.value; g_activeProfile = -1; break;
        case Action::ApplyColor: applyStaticColor(); break;
        case Action::Brightness:
        case Action::EffectSpeed:
        case Action::EffectIntensity:
        case Action::AmbientSaturation:
        case Action::FanSlider:
            g_dragAction = hit.action; g_dragIndex = hit.index; setSliderValue(hit.action, hit.index, mouseX); break;
        case Action::SelectEffect: if (hit.index >= 0 && hit.index < static_cast<int>(std::size(kEffects))) { g_selectedEffect = hit.index; g_activeProfile = -1; } break;
        case Action::ApplyEffect: startEffect(); break;
        case Action::AmbientMonitor:
            g_screenMonitors = AmbientScreenCapture::enumerateMonitors();
            if (!g_screenMonitors.empty()) {
                g_ambientMonitorIndex = (g_ambientMonitorIndex + 1) % static_cast<int>(g_screenMonitors.size());
                g_activeProfile = -1;
                if (g_effectActive && std::wstring(kEffects[g_selectedEffect].internal) == L"Ambilight") startEffect();
            }
            break;
        case Action::AmbientZones:
            g_ambientZones = !g_ambientZones.load();
            g_activeProfile = -1;
            break;
        case Action::FanApply: g_activeProfile = -1; g_fanCurveEnabled = false; g_fanProfile = FanProfile::Custom; changeFan(hit.index, false); break;
        case Action::FanAuto: g_activeProfile = -1; g_fanCurveEnabled = false; g_fanProfile = FanProfile::Custom; changeFan(hit.index, true); break;
        case Action::FanProfileAuto:
        case Action::FanProfileQuiet:
        case Action::FanProfileBalanced:
        case Action::FanProfilePerformance: g_activeProfile = -1; applyFanProfile(hit.action); break;
        case Action::FanCurveToggle:
            g_activeProfile = -1;
            setFanCurveEnabled(!g_fanCurveEnabled);
            break;
        case Action::FanCurvePreset:
            if (hit.index == 0) g_curveSpeeds = {30, 35, 55, 80};
            else if (hit.index == 1) g_curveSpeeds = {30, 45, 70, 100};
            else if (hit.index == 2) g_curveSpeeds = {45, 60, 85, 100};
            if (hit.index >= 0 && hit.index <= 2) {
                g_curvePreset = hit.index;
                g_activeProfile = -1;
                saveCurveSettings();
                if (g_fanCurveEnabled) applyFanCurve(true);
            }
            break;
        case Action::FanCurvePoint:
            g_dragAction = Action::FanCurvePoint;
            g_dragIndex = hit.index;
            setFanCurvePoint(hit.index, mouseY);
            break;
        case Action::ProfileSave: saveProfileSlot(hit.index); break;
        case Action::ProfileLoad: applyProfileSlot(hit.index); break;
        case Action::ProfileDelete:
            if (MessageBoxW(g_window, localized(L"Supprimer ce profil enregistré ?", L"Delete this saved profile?",
                                                L"Dieses gespeicherte Profil löschen?", L"删除此已保存模式？"),
                            L"RGBCcontrol", MB_YESNO | MB_ICONQUESTION) == IDYES) deleteProfileSlot(hit.index);
            break;
        case Action::ProfileExport: exportCurrentProfile(); break;
        case Action::ProfileImport: importProfile(); break;
        case Action::DiagnosticCopy: copyDiagnosticReport(); break;
        case Action::ToggleStartup:
            setStartupEnabled(!g_startupEnabled);
            break;
        case Action::ToggleMinimizeToTray:
            g_minimizeToTray = !g_minimizeToTray;
            saveAutomationSettings();
            break;
        case Action::ToggleStartupQuiet:
            g_startupQuiet = !g_startupQuiet;
            saveAutomationSettings();
            break;
        case Action::ToggleCloseToTray:
            g_closeToTray = !g_closeToTray;
            saveAutomationSettings();
            break;
        case Action::ToggleRememberPage:
            g_rememberLastPage = !g_rememberLastPage;
            saveAutomationSettings();
            break;
        case Action::ToggleReduceMotion:
            g_reduceMotion = !g_reduceMotion;
            saveAutomationSettings();
            break;
        case Action::SelectAccent:
            if (hit.index >= 0 && hit.index < static_cast<int>(std::size(kAccentChoices))) {
                g_accentPreset = hit.index;
                saveAutomationSettings();
            }
            break;
        case Action::SelectScanInterval: {
            const int intervals[] = {5, 15, 30, 60};
            if (hit.index >= 0 && hit.index < static_cast<int>(std::size(intervals))) {
                g_detectionIntervalSeconds = intervals[hit.index];
                g_lastScan = GetTickCount64();
                saveAutomationSettings();
            }
            break;
        }
        case Action::SelectEffectQuality:
            if (hit.index >= 0 && hit.index <= 2) {
                g_effectQuality = hit.index;
                saveAutomationSettings();
            }
            break;
        case Action::ResetPreferences:
            g_accentPreset = 0;
            g_detectionIntervalSeconds = 5;
            g_effectQuality = 1;
            g_startupQuiet = true;
            g_minimizeToTray = true;
            g_closeToTray = false;
            g_rememberLastPage = true;
            g_reduceMotion = false;
            g_dualSensePlayerLedsEnabled = true;
            saveAutomationSettings();
            break;
        case Action::ScheduleToggle:
            g_scheduleEnabled = !g_scheduleEnabled;
            g_lastScheduledProfile = -1;
            saveAutomationSettings();
            if (g_scheduleEnabled) evaluateSchedule(true);
            else g_scheduleStatus = localized(L"Planification désactivée.", L"Schedule disabled.", L"Zeitplan deaktiviert.", L"计划已禁用。");
            break;
        case Action::ScheduleDayProfile:
        case Action::ScheduleNightProfile:
            if (hit.index >= 0 && hit.index < 3) {
                if (hit.action == Action::ScheduleDayProfile) g_scheduleDayProfile = hit.index;
                else g_scheduleNightProfile = hit.index;
                g_lastScheduledProfile = -1;
                saveAutomationSettings();
                if (g_scheduleEnabled) evaluateSchedule(true);
            }
            break;
        case Action::ScheduleDayHourDown:
        case Action::ScheduleDayHourUp:
        case Action::ScheduleNightHourDown:
        case Action::ScheduleNightHourUp: {
            int* hour = (hit.action == Action::ScheduleDayHourDown || hit.action == Action::ScheduleDayHourUp)
                ? &g_scheduleDayHour : &g_scheduleNightHour;
            const int direction = (hit.action == Action::ScheduleDayHourUp || hit.action == Action::ScheduleNightHourUp) ? 1 : -1;
            *hour = (*hour + direction + 24) % 24;
            g_lastScheduledProfile = -1;
            saveAutomationSettings();
            if (g_scheduleEnabled) evaluateSchedule(true);
            break;
        }
        case Action::FanAdmin: {
            wchar_t executable[MAX_PATH]{};
            GetModuleFileNameW(nullptr, executable, MAX_PATH);
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"runas", executable, L"--elevated", g_appDirectory.c_str(), SW_SHOWNORMAL)) > 32) DestroyWindow(g_window);
            break;
        }
        case Action::SelectLanguage:
            if (hit.index >= 0 && hit.index <= 3) {
                g_language = static_cast<Language>(hit.index);
                saveLanguage();
                g_updateStatus = localized(L"Langue enregistrée.", L"Language saved.", L"Sprache gespeichert.", L"语言已保存。");
                g_status = localized(L"Interface en français.", L"Interface switched to English.", L"Oberfläche auf Deutsch umgestellt.", L"界面已切换为简体中文。");
                g_fanStatus = localized(L"Détection automatique active.", L"Automatic detection is active.", L"Automatische Erkennung ist aktiv.", L"自动检测已启用。");
            }
            break;
        case Action::Update:
            if (!g_downloadedUpdate.empty() && fs::exists(g_downloadedUpdate)) {
                g_updateStatus = localized(L"Installation automatique et redémarrage...", L"Automatic installation and restart...",
                                           L"Automatische Installation und Neustart...", L"正在自动安装并重启…");
                UpdateWindow(g_window);
                if (launchSilentUpdate(g_downloadedUpdate)) {
                    DestroyWindow(g_window);
                    return;
                }
                g_updateStatus = localized(L"Impossible de lancer la mise à jour automatique.", L"The automatic update could not be started.",
                                           L"Das automatische Update konnte nicht gestartet werden.", L"无法启动自动更新。");
            } else {
                startUpdateCheck();
            }
            break;
        case Action::PickerWheel:
            g_dragAction = Action::PickerWheel;
            updatePickerWheel(mouseX, mouseY);
            break;
        case Action::PickerBrightness:
            g_dragAction = Action::PickerBrightness;
            updatePickerBrightness(mouseX);
            break;
        case Action::PickerPreset:
            rgbToHsv(hit.value, g_pickerHue, g_pickerSaturation, g_pickerBrightness);
            break;
        case Action::PickerCancel:
            g_colorPickerOpen = false;
            g_dragAction = Action::None;
            break;
        case Action::PickerApply:
            g_baseColor = hsvColor(g_pickerHue, g_pickerSaturation, g_pickerBrightness / 100.0);
            g_activeProfile = -1;
            g_colorPickerOpen = false;
            g_dragAction = Action::None;
            break;
        case Action::PickerBackdrop:
            break;
        default: break;
    }
    InvalidateRect(g_window, nullptr, FALSE);
}

void resetModifiedFans() {
    for (const std::string& id : g_modifiedFans) {
        DWORD exitCode = 1;
        hardwareCommand("auto " + id, nullptr, &exitCode);
    }
    g_modifiedFans.clear();
}

bool parseDualSenseInputReport(const BYTE* report, std::size_t size, bool bluetooth, bool edge,
                               DualSenseLiveState& state) {
    if (!report || size < 7) return false;
    std::size_t start = 0;
    bool enhanced = false;
    if (size == 63) {
        // HID report-ID stripping is also used for the full report. Check the
        // body lengths before looking at byte zero, because an axis can itself
        // legitimately contain the value 0x01.
        start = 0;
        enhanced = true;
    } else if (size == 77) {
        // A Bluetooth 0x31 body without the report ID still begins with its
        // sequence/tag byte. The common stick/trigger state starts one byte
        // later, unlike a stripped USB report.
        start = 1;
        enhanced = true;
    } else if (report[0] == 0x31 && size >= 12) {
        // Bluetooth report 0x31 keeps both the report ID and a sequence/tag
        // byte in front of the common state. Reading from byte 1 makes the
        // changing sequence counter look like a joystick moving by itself and
        // shifts every trigger/button by one byte.
        start = 2;
        enhanced = true;
    } else if (report[0] == 0x01) {
        start = 1;
        // USB report 0x01 is normally 64 bytes and places L2/R2 at bytes 5/6,
        // then padding at byte 7 and buttons at bytes 8..10. Bluetooth's
        // compact report is 10 bytes and places buttons at 5..7 and triggers
        // at 8..9. Windows may pad that compact Bluetooth report, so its
        // transport decides the layout. Never inspect byte 7 for this choice:
        // on the official 64-byte USB report byte 7 is a changing sequence
        // counter. Treating that counter as a layout marker shifted all button
        // offsets and made the preview press many controls by itself.
        enhanced = !bluetooth && size >= 64;
    } else if (size == 9) {
        // Some HID stacks strip the report ID before delivering the compact
        // Bluetooth payload. Keep accepting that form for older Windows HID
        // filters and controller bridge software.
        start = 0;
        enhanced = false;
    } else {
        return false;
    }

    const std::size_t buttonsOffset = start + (enhanced ? 7 : 4);
    const std::size_t triggerOffset = start + (enhanced ? 4 : 7);
    if (buttonsOffset + 2 >= size || triggerOffset + 1 >= size || start + 3 >= size) return false;
    state.edge = edge;
    state.bluetooth = bluetooth;
    state.leftX = report[start + 0];
    state.leftY = report[start + 1];
    state.rightX = report[start + 2];
    state.rightY = report[start + 3];
    state.leftTrigger = report[triggerOffset + 0];
    state.rightTrigger = report[triggerOffset + 1];
    const BYTE buttons0 = report[buttonsOffset + 0];
    const BYTE buttons1 = report[buttonsOffset + 1];
    const BYTE buttons2 = report[buttonsOffset + 2];
    state.dpad = buttons0 & 0x0f;
    if (state.dpad > 8) state.dpad = 8;
    state.buttons.fill(false);
    state.buttons[0] = (buttons0 & 0x10) != 0; // Square
    state.buttons[1] = (buttons0 & 0x20) != 0; // Cross
    state.buttons[2] = (buttons0 & 0x40) != 0; // Circle
    state.buttons[3] = (buttons0 & 0x80) != 0; // Triangle
    state.buttons[4] = (buttons1 & 0x01) != 0; // L1
    state.buttons[5] = (buttons1 & 0x02) != 0; // R1
    state.buttons[6] = (buttons1 & 0x04) != 0; // L2 digital
    state.buttons[7] = (buttons1 & 0x08) != 0; // R2 digital
    state.buttons[8] = (buttons1 & 0x10) != 0; // Create
    state.buttons[9] = (buttons1 & 0x20) != 0; // Options
    state.buttons[10] = (buttons1 & 0x40) != 0; // L3
    state.buttons[11] = (buttons1 & 0x80) != 0; // R3
    state.buttons[12] = (buttons2 & 0x01) != 0; // PS
    state.buttons[13] = (buttons2 & 0x02) != 0; // Touchpad
    state.buttons[14] = (buttons2 & 0x04) != 0; // Mute
    state.seen = true;
    state.lastInputAt = GetTickCount64();
    return true;
}

void handleDualSenseRawInput(HRAWINPUT input) {
    UINT bytes = 0;
    if (GetRawInputData(input, RID_INPUT, nullptr, &bytes, sizeof(RAWINPUTHEADER)) != 0 || bytes < sizeof(RAWINPUT)) return;
    std::vector<BYTE> storage(bytes);
    if (GetRawInputData(input, RID_INPUT, storage.data(), &bytes, sizeof(RAWINPUTHEADER)) != bytes) return;
    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(storage.data());
    if (raw->header.dwType != RIM_TYPEHID) return;

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT infoSize = sizeof(info);
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICEINFO, &info, &infoSize) == static_cast<UINT>(-1) ||
        info.hid.dwVendorId != 0x054c || (info.hid.dwProductId != 0x0ce6 && info.hid.dwProductId != 0x0df2)) return;

    UINT nameLength = 0;
    GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, nullptr, &nameLength);
    std::wstring deviceName;
    if (nameLength > 0) {
        std::vector<wchar_t> name(nameLength + 1, L'\0');
        UINT requested = nameLength;
        if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, name.data(), &requested) != static_cast<UINT>(-1)) {
            deviceName.assign(name.data());
        }
    }
    std::wstring upperName = deviceName;
    std::transform(upperName.begin(), upperName.end(), upperName.begin(), towupper);
    const bool bluetooth = upperName.find(L"BTH") != std::wstring::npos || upperName.find(L"VID&0002") != std::wstring::npos;
    const bool edge = info.hid.dwProductId == 0x0df2;

    const RAWHID& hid = raw->data.hid;
    if (hid.dwSizeHid == 0 || hid.dwCount == 0) return;
    bool parsed = false;
    for (DWORD index = 0; index < hid.dwCount; ++index) {
        const BYTE* report = hid.bRawData + index * hid.dwSizeHid;
        parsed = parseDualSenseInputReport(report, hid.dwSizeHid, bluetooth, edge, g_dualSenseLive) || parsed;
    }
    if (parsed && !g_dualSenseVisualAxes.initialized) updateDualSenseVisualAxes(GetTickCount64(), true);
    if (parsed && g_xboxModeEnabled) sendXboxState(g_dualSenseLive);
    if (parsed && g_page == Page::Gamepads) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastGamepadFrameAt >= 16) {
            g_lastGamepadFrameAt = now;
            g_gamepadFastPaintRequested = true;
            InvalidateRect(g_window, nullptr, FALSE);
        }
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            g_launchAnimationStartedAt = GetTickCount64();
            g_launchAnimationFinished = g_startHidden || g_reduceMotion || g_justUpdated;
            g_lastAmbientFrameAt = g_launchAnimationStartedAt;
            g_lastEffectUiFrameAt = g_launchAnimationStartedAt;
            addTrayIcon(window);
            {
                DEV_BROADCAST_DEVICEINTERFACE_W filter{};
                filter.dbcc_size = sizeof(filter);
                filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
                g_deviceNotification = RegisterDeviceNotificationW(window, &filter,
                    DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
                g_hotplugMonitoring = g_deviceNotification != nullptr;
            }
            {
                // Listen only to controller collections. Registering the
                // complete Generic Desktop page also captures the mouse and
                // keyboard and can flood the UI with raw-input messages.
                RAWINPUTDEVICE devices[3]{};
                constexpr USHORT usages[] = {0x04, 0x05, 0x08}; // Joystick, Game Pad, Multi-axis Controller.
                for (int index = 0; index < 3; ++index) {
                    devices[index].usUsagePage = 0x01;
                    devices[index].usUsage = usages[index];
                    devices[index].dwFlags = RIDEV_INPUTSINK;
                    devices[index].hwndTarget = window;
                }
                g_gamepadRawInputReady = RegisterRawInputDevices(devices, 3, sizeof(RAWINPUTDEVICE)) == TRUE;
            }
            SetTimer(window, APP_TIMER, 16, nullptr);
            startScan(false);
            return 0;
        case WM_PAINT: paint(window); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_INPUT:
            handleDualSenseRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT) {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
                g_hotplugScanPending = true;
                g_hotplugEventAt = GetTickCount64();
                g_lastScan = g_hotplugEventAt;
                g_hotplugStatus = localized(L"Changement matériel détecté…", L"Hardware change detected…",
                                            L"Hardware-Änderung erkannt…", L"检测到硬件变化…");
                g_status = g_hotplugStatus;
                InvalidateRect(window, nullptr, FALSE);
            }
            return TRUE;
        case WM_TIMER:
            if (wParam == APP_TIMER) {
                const ULONGLONG now = GetTickCount64();
                if (pollXboxBridge()) InvalidateRect(window, nullptr, FALSE);
                const bool certificationBusy = g_certificationDevice >= 0 || g_certificationApplying || g_certificationAwaitingAnswer;
                if (!certificationBusy && g_hotplugScanPending && !g_scanInFlight && now - g_hotplugEventAt >= 750) {
                    g_hotplugScanPending = false;
                    g_hotplugScanActive = true;
                    startScan(true);
                } else if (!certificationBusy && !g_hotplugScanPending && !g_scanInFlight &&
                           now - g_lastScan >= static_cast<ULONGLONG>(g_detectionIntervalSeconds) * 1000ULL) {
                    startScan(false);
                }
                const bool headerAnimating = !g_reduceMotion && (g_scanInFlight.load() ||
                    (g_headerStatusChangedAt != 0 && now - g_headerStatusChangedAt < 1400) ||
                    (g_hoverAction != Action::None && g_hoverStartedAt != 0 && now - g_hoverStartedAt < 420));
                bool splashAnimating = launchAnimationActive();
                if (splashAnimating && launchElapsedMs() >= kLaunchDurationMs) {
                    g_launchAnimationFinished = true;
                    splashAnimating = false;
                }
                // The ambient motion runs at 30 FPS. Live meters are also capped
                // at 30 FPS instead of forcing a wasteful full redraw every 16 ms.
                const bool ambientAnimating = !g_reduceMotion && IsWindowVisible(window) && now - g_lastAmbientFrameAt >= 33;
                if (ambientAnimating) g_lastAmbientFrameAt = now;
                bool liveEffectMeters = false;
                if (g_effectActive && g_page == Page::Effects && g_selectedEffect >= 0 &&
                    g_selectedEffect < static_cast<int>(std::size(kEffects))) {
                    const std::wstring activeMode = kEffects[g_selectedEffect].internal;
                    liveEffectMeters = activeMode == L"Music" || activeMode == L"Ambilight";
                }
                const bool liveUiAnimating = (liveEffectMeters || g_compatibilityRunning) &&
                                             now - g_lastEffectUiFrameAt >= 33;
                if (liveUiAnimating) g_lastEffectUiFrameAt = now;
                const bool gamepadAxesAnimating = g_page == Page::Gamepads && updateDualSenseVisualAxes(now);
                // A drag is also an animation even when no HID packet is
                // arriving. Let the 16 ms timer own those repaints so a
                // high-rate WM_MOUSEMOVE stream cannot queue hundreds of
                // expensive model renders and make the cursor feel sticky.
                const bool gamepadLightingAnimating = g_page == Page::Gamepads && !g_reduceMotion &&
                    g_effectActive.load() && g_activeEffectIndex.load() > 0;
                const bool gamepadAnimating = g_page == Page::Gamepads &&
                    (((g_gamepadDragging || gamepadAxesAnimating) && now - g_lastGamepadFrameAt >= 16) ||
                     (gamepadLightingAnimating && now - g_lastGamepadFrameAt >= 33));
                if (gamepadAnimating) g_lastGamepadFrameAt = now;
                if (gamepadAnimating) g_gamepadFastPaintRequested = true;
                if (liveUiAnimating || headerAnimating || splashAnimating || ambientAnimating || gamepadAnimating) {
                    InvalidateRect(window, nullptr, FALSE);
                }
                if (GetTickCount64() - g_lastScheduleCheck >= 30000) {
                    g_lastScheduleCheck = GetTickCount64();
                    evaluateSchedule(false);
                }
            }
            return 0;
        case WM_SCAN_COMPLETE: {
            std::unique_ptr<ScanResult> result(reinterpret_cast<ScanResult*>(lParam));
            if (!result) return 0;
            for (RgbDevice& fresh : result->rgb) {
                auto old = std::find_if(g_rgbDevices.begin(), g_rgbDevices.end(), [&](const RgbDevice& device) { return device.name == fresh.name && device.vendor == fresh.vendor; });
                if (old != g_rgbDevices.end()) fresh.selected = old->selected;
                if (isDualSenseDevice(fresh)) fresh.auxiliaryLedMask = g_dualSensePlayerLedsEnabled ? 0x1f : 0;
                loadLocalCalibration(fresh);
            }
            for (FanDevice& fresh : result->fans) {
                auto old = std::find_if(g_fans.begin(), g_fans.end(), [&](const FanDevice& fan) { return fan.encodedId == fresh.encodedId; });
                if (old != g_fans.end()) {
                    fresh.desired = old->desired;
                    fresh.manual = old->manual;
                }
            }
            g_rgbDevices = std::move(result->rgb);
            g_pluginProviders = std::move(result->pluginProviders);
            g_rejectedPlugins = result->rejectedPlugins;
            g_fans = std::move(result->fans);
            g_cpuTemperature = result->cpuTemperature;
            g_gpuTemperature = result->gpuTemperature;
            g_duckyDetected = result->ducky;
            if (!g_duckyDetected && g_page == Page::DuckyAssistant) {
                g_page = Page::Devices;
                g_scrollOffset = 0;
            }
            g_openRgbReady = result->openRgbReady;
            g_hasCompletedScan = true;
            if (g_hotplugScanActive) {
                g_hotplugStatus = localized(L"Inventaire actualisé automatiquement.", L"Inventory updated automatically.",
                                            L"Inventar automatisch aktualisiert.", L"设备清单已自动更新。");
                g_hotplugScanActive = false;
            }
            int controllable = static_cast<int>(std::count_if(g_fans.begin(), g_fans.end(), [](const FanDevice& fan) { return fan.controllable; }));
            g_fanStatus = std::to_wstring(g_fans.size()) + localized(L" détecté(s) · ", L" detected · ", L" erkannt · ", L" 个已检测 · ") +
                          std::to_wstring(controllable) + localized(L" contrôlable(s)", L" controllable", L" steuerbar", L" 个可控制");
            int pluginDeviceCount = static_cast<int>(std::count_if(g_rgbDevices.begin(), g_rgbDevices.end(),
                [](const RgbDevice& device) { return PluginEngine::isPluginDevice(device); }));
            g_status = result->error.empty()
                ? std::to_wstring(g_rgbDevices.size()) + localized(L" contrôleur(s) RGB détecté(s) · ", L" RGB controller(s) detected · ",
                                                                    L" RGB-Controller erkannt · ", L" 个 RGB 控制器已检测 · ") +
                  std::to_wstring(pluginDeviceCount) + localized(L" via plugin(s).", L" through plugin(s).",
                                                               L" über Plugin(s).", L" 个通过插件。")
                : result->error;
            const int pendingProfile = g_pendingFanProfile;
            const std::string pendingFanId = g_pendingFanId;
            const int pendingFanValue = g_pendingFanValue;
            const bool pendingFanAutomatic = g_pendingFanAutomatic;
            const bool pendingFanCurve = g_pendingFanCurve;
            const int pendingSavedProfile = g_pendingSavedProfile;
            g_pendingFanProfile = -1;
            g_pendingFanId.clear();
            g_pendingFanCurve = false;
            g_pendingSavedProfile = -1;
            if (pendingSavedProfile >= 0) {
                applyProfileSlot(pendingSavedProfile);
            } else if (pendingFanCurve) {
                setFanCurveEnabled(true);
            } else if (pendingProfile >= 0) {
                applyFanProfile(fanProfileAction(pendingProfile));
            } else if (!pendingFanId.empty()) {
                auto fan = std::find_if(g_fans.begin(), g_fans.end(), [&](const FanDevice& candidate) { return candidate.encodedId == pendingFanId; });
                if (fan != g_fans.end()) {
                    const int index = static_cast<int>(std::distance(g_fans.begin(), fan));
                    g_fanProfile = FanProfile::Custom;
                    if (!pendingFanAutomatic) fan->desired = std::clamp(pendingFanValue, fan->minimum, 100);
                    changeFan(index, pendingFanAutomatic);
                } else {
                    g_fanStatus = localized(L"Le ventilateur demandé n'a pas été retrouvé.", L"The requested fan was not found.",
                                            L"Der angeforderte Lüfter wurde nicht gefunden.", L"未找到所请求的风扇。");
                }
            } else if (g_fanCurveEnabled) {
                applyFanCurve(false);
            }
            if (pendingSavedProfile < 0 && !pendingFanCurve && pendingProfile < 0 && pendingFanId.empty()) evaluateSchedule(false);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_ASYNC_STATUS: {
            std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lParam));
            if (status) { g_status = *status; g_fanStatus = *status; InvalidateRect(window, nullptr, FALSE); }
            return 0;
        }
        case WM_UPDATE_COMPLETE: {
            std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lParam));
            g_updateInFlight = false;
            if (result) {
                g_updateStatus = std::move(result->status);
                g_downloadedUpdate = std::move(result->installer);
                if (!g_downloadedUpdate.empty() && fs::exists(g_downloadedUpdate)) {
                    g_updateStatus = localized(L"Mise à jour vérifiée. Installation automatique...",
                                               L"Update verified. Installing automatically...",
                                               L"Update geprüft. Automatische Installation...",
                                               L"更新已验证，正在自动安装…");
                    InvalidateRect(window, nullptr, FALSE);
                    UpdateWindow(window);
                    if (launchSilentUpdate(g_downloadedUpdate)) {
                        DestroyWindow(window);
                        return 0;
                    }
                    g_updateStatus = localized(L"Le téléchargement est prêt, mais le redémarrage automatique a échoué. Clique pour réessayer.",
                                               L"The download is ready, but automatic restart failed. Click to retry.",
                                               L"Der Download ist bereit, aber der automatische Neustart ist fehlgeschlagen. Zum Wiederholen klicken.",
                                               L"下载已就绪，但自动重启失败。请点击重试。");
                }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_FAN_COMPLETE: {
            std::unique_ptr<FanCommandResult> result(reinterpret_cast<FanCommandResult*>(lParam));
            if (!result) return 0;
            g_busyFans.erase(result->id);
            auto fan = std::find_if(g_fans.begin(), g_fans.end(), [&](const FanDevice& candidate) { return candidate.encodedId == result->id; });
            if (result->success && fan != g_fans.end()) {
                fan->manual = !result->automatic;
                if (result->automatic) g_modifiedFans.erase(result->id);
                else g_modifiedFans.insert(result->id);
            }
            g_status = result->status;
            g_fanStatus = result->status;
            g_lastScan = GetTickCount64();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_CERTIFICATION_COMPLETE: {
            std::unique_ptr<CertificationCommandResult> result(reinterpret_cast<CertificationCommandResult*>(lParam));
            if (g_certificationThread.joinable()) g_certificationThread.join();
            g_certificationApplying = false;
            if (!result || result->device != g_certificationDevice || result->stage != g_certificationStage ||
                result->motion != g_certificationMotionTest ||
                g_certificationDevice < 0 || g_certificationDevice >= static_cast<int>(g_rgbDevices.size())) {
                g_certificationAwaitingAnswer = false;
                g_certificationDevice = -1;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (!result->success) {
                finishCertificationFailure(localized(L"Commande refusée : ", L"Command rejected: ",
                                                    L"Befehl abgelehnt: ", L"命令被拒绝：") +
                    (result->message.empty() ? localized(L"aucun mode compatible", L"no compatible mode",
                                                        L"kein kompatibler Modus", L"没有兼容模式") : result->message));
            } else {
                g_certificationAwaitingAnswer = true;
                if (result->motion) {
                    g_certificationStatus = localized(L"L'animation est-elle fluide, stable et sans clignotement ?",
                                                      L"Is the animation smooth, stable and flicker-free?",
                                                      L"Ist die Animation flüssig, stabil und flimmerfrei?",
                                                      L"动画是否流畅、稳定且无闪烁？");
                } else {
                    static const wchar_t* french[] = {L"rouge", L"vert", L"bleu"};
                    static const wchar_t* english[] = {L"red", L"green", L"blue"};
                    static const wchar_t* german[] = {L"Rot", L"Grün", L"Blau"};
                    static const wchar_t* chinese[] = {L"红色", L"绿色", L"蓝色"};
                    g_certificationStatus = localized(L"Toutes les zones attendues affichent-elles du ", L"Do all expected zones show ",
                                                      L"Zeigen alle erwarteten Zonen ", L"所有预期区域都显示") +
                        std::wstring(g_language == Language::French ? french[g_certificationStage]
                                     : g_language == Language::English ? english[g_certificationStage]
                                     : g_language == Language::German ? german[g_certificationStage]
                                     : chinese[g_certificationStage]) +
                        localized(L" ?", L"?", L"?", L"吗？");
                }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_TRAY_ICON:
            if (lParam == WM_LBUTTONDBLCLK) showMainWindow();
            else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) showTrayMenu();
            return 0;
        case WM_LBUTTONDOWN: {
            if (launchAnimationActive()) {
                g_launchAnimationFinished = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            float x = static_cast<float>(GET_X_LPARAM(lParam));
            float y = static_cast<float>(GET_Y_LPARAM(lParam));
            // Modal controls always own the pointer. Previously the 3D model
            // intercepted clicks through the color picker and started rotating
            // while the user was trying to move inside the color wheel.
            if (g_colorPickerOpen) {
                for (auto iterator = g_hits.rbegin(); iterator != g_hits.rend(); ++iterator) {
                    if (pickerAction(iterator->action) && contains(iterator->rect, x, y)) {
                        handleAction(*iterator, x, y);
                        SetCapture(window);
                        return 0;
                    }
                }
                return 0;
            }
            if (g_page == Page::Gamepads && g_dualSenseMesh.loaded && contains(g_gamepadModelRect, x, y)) {
                g_dragAction = Action::GamepadRotate;
                g_gamepadDragging = true;
                g_gamepadLastMouseX = static_cast<int>(x);
                g_gamepadLastMouseY = static_cast<int>(y);
                SetCapture(window);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return 0;
            }
            if (g_maxScroll > 0 && contains(g_scrollThumb, x, y)) {
                g_scrollDragging = true; g_scrollDragOrigin = y; g_scrollOffsetOrigin = g_scrollOffset; SetCapture(window); return 0;
            }
            for (auto iterator = g_hits.rbegin(); iterator != g_hits.rend(); ++iterator) {
                bool headerAction = iterator->action == Action::HeaderStatus || iterator->action == Action::HeaderUpdate ||
                                    iterator->action == Action::Minimize || iterator->action == Action::Maximize || iterator->action == Action::Close;
                bool navigationAction = iterator->action == Action::NavDashboard || iterator->action == Action::NavDevices || iterator->action == Action::NavGamepads || iterator->action == Action::NavEffects ||
                                        iterator->action == Action::NavProfiles || iterator->action == Action::NavFans ||
                                        iterator->action == Action::NavDiagnostics || iterator->action == Action::NavSettings ||
                                        iterator->action == Action::Donate;
                if (y < kHeaderHeight && !headerAction) continue;
                if (y >= kHeaderHeight && x < kSidebarWidth && !navigationAction) continue;
                if (y >= kHeaderHeight && x >= kSidebarWidth && (headerAction || navigationAction)) continue;
                if (contains(iterator->rect, x, y)) { handleAction(*iterator, x, y); SetCapture(window); return 0; }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            float x = static_cast<float>(GET_X_LPARAM(lParam));
            float y = static_cast<float>(GET_Y_LPARAM(lParam));
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            if (g_scrollDragging) {
                RECT client{}; GetClientRect(window, &client);
                float track = std::max(1.0f, client.bottom - kHeaderHeight - 20.0f - g_scrollThumb.Height);
                g_scrollOffset = std::clamp(g_scrollOffsetOrigin + (y - g_scrollDragOrigin) * g_maxScroll / track, 0.0f, g_maxScroll);
                InvalidateRect(window, nullptr, FALSE);
            } else if (g_dragAction == Action::GamepadRotate) {
                const float deltaX = x - static_cast<float>(g_gamepadLastMouseX);
                const float deltaY = y - static_cast<float>(g_gamepadLastMouseY);
                g_gamepadLastMouseX = static_cast<int>(x);
                g_gamepadLastMouseY = static_cast<int>(y);
                g_gamepadYaw = std::clamp(g_gamepadYaw + deltaX * 0.012f, -3.14159f, 3.14159f);
                g_gamepadPitch = std::clamp(g_gamepadPitch + deltaY * 0.010f, -1.15f, 1.15f);
                // The 16 ms timer owns rotation repaints. Avoid queuing a
                // full model render for every high-rate mouse message.
            } else if (g_dragAction == Action::PickerWheel) updatePickerWheel(x, y);
            else if (g_dragAction == Action::PickerBrightness) updatePickerBrightness(x);
            else if (g_dragAction == Action::FanCurvePoint) setFanCurvePoint(g_dragIndex, y);
            else if (g_dragAction != Action::None) setSliderValue(g_dragAction, g_dragIndex, x);
            else {
                Action hovered = Action::None;
                int hoveredIndex = -1;
                for (auto iterator = g_hits.rbegin(); iterator != g_hits.rend(); ++iterator) {
                    if (g_colorPickerOpen && !pickerAction(iterator->action)) continue;
                    if (contains(iterator->rect, x, y)) { hovered = iterator->action; hoveredIndex = iterator->index; break; }
                }
                if (hovered != g_hoverAction || hoveredIndex != g_hoverIndex) {
                    g_hoverAction = hovered;
                    g_hoverIndex = hoveredIndex;
                    g_hoverStartedAt = GetTickCount64();
                    InvalidateRect(window, nullptr, FALSE);
                }
                SetCursor(LoadCursorW(nullptr, hovered != Action::None && hovered != Action::PickerBackdrop ? IDC_HAND : IDC_ARROW));
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hoverAction = Action::None;
            g_hoverIndex = -1;
            g_hoverStartedAt = 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const bool curveChanged = g_dragAction == Action::FanCurvePoint;
            const bool gamepadRotate = g_dragAction == Action::GamepadRotate;
            if (gamepadRotate) g_gamepadDragging = false;
            g_scrollDragging = false; g_dragAction = Action::None; g_dragIndex = -1; ReleaseCapture();
            if (gamepadRotate) InvalidateRect(window, nullptr, FALSE);
            if (curveChanged) {
                saveCurveSettings();
                if (g_fanCurveEnabled) applyFanCurve(true);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (g_page == Page::Gamepads && g_dualSenseMesh.loaded && contains(g_gamepadModelRect,
                                                                                static_cast<float>(GET_X_LPARAM(lParam)),
                                                                                static_cast<float>(GET_Y_LPARAM(lParam)))) {
                g_gamepadYaw = 0.0f;
                g_gamepadPitch = 0.12f;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (g_colorPickerOpen) return 0;
            g_scrollOffset = std::clamp(g_scrollOffset - GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 62.0f, 0.0f, g_maxScroll);
            InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_KEYDOWN:
            if (launchAnimationActive() && (wParam == VK_ESCAPE || wParam == VK_SPACE || wParam == VK_RETURN)) {
                g_launchAnimationFinished = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (g_colorPickerOpen && wParam == VK_ESCAPE) {
                g_colorPickerOpen = false;
                g_dragAction = Action::None;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (g_colorPickerOpen && wParam == VK_RETURN) {
                g_baseColor = hsvColor(g_pickerHue, g_pickerSaturation, g_pickerBrightness / 100.0);
                g_colorPickerOpen = false;
                g_dragAction = Action::None;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        case WM_NCHITTEST: {
            LRESULT base = DefWindowProcW(window, message, wParam, lParam);
            if (base != HTCLIENT) return base;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            RECT client{}; GetClientRect(window, &client);
            const bool updateReady = !g_downloadedUpdate.empty();
            const float controlsLeft = static_cast<float>(client.right - 164);
            const float interactiveLeft = controlsLeft - 12.0f - 268.0f - (updateReady ? 54.0f : 0.0f);
            if (point.y < kHeaderHeight && point.x < static_cast<int>(interactiveLeft - 8.0f)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 1020;
            info->ptMinTrackSize.y = 680;
            return 0;
        }
        case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_CLOSE:
            if (g_closeToTray && g_trayAdded) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            saveAutomationSettings();
            KillTimer(window, APP_TIMER);
            stopXboxBridge();
            if (g_deviceNotification) {
                UnregisterDeviceNotification(g_deviceNotification);
                g_deviceNotification = nullptr;
            }
            g_hotplugMonitoring = false;
            removeTrayIcon();
            g_running = false;
            g_effectActive = false;
            if (g_effectThread.joinable()) g_effectThread.join();
            if (g_compatibilityThread.joinable()) g_compatibilityThread.join();
            if (g_scanThread.joinable()) g_scanThread.join();
            if (g_certificationThread.joinable()) g_certificationThread.join();
            for (std::thread& thread : g_fanThreads) if (thread.joinable()) thread.join();
            for (std::thread& thread : g_pluginThreads) if (thread.joinable()) thread.join();
            for (const RgbDevice& device : g_rgbDevices) {
                if (PluginEngine::isPluginDevice(device)) g_pluginEngine.stop(device);
            }
            resetModifiedFans();
            shutdownHardwareBridge();
            if (g_ownedOpenRgbProcess) {
                TerminateProcess(g_ownedOpenRgbProcess, 0);
                CloseHandle(g_ownedOpenRgbProcess);
                g_ownedOpenRgbProcess = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(window, message, wParam, lParam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDPIAware();
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    g_appDirectory = fs::path(executable).parent_path();
    g_pluginEngine.setRoot(g_appDirectory / L"Plugins");
    g_isAdministrator = isAdministrator();
    loadLanguage();
    g_status = localized(L"Initialisation de RGBCcontrol...", L"Starting RGBCcontrol...", L"RGBCcontrol wird gestartet...", L"正在启动 RGBCcontrol…");
    g_fanStatus = localized(L"Recherche des ventilateurs...", L"Searching for fans...", L"Lüfter werden gesucht...", L"正在搜索风扇…");

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    GdiplusStartupInput gdiplusInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
    // Load the supplied DualSense 3D model once at startup.  The sampled mesh
    // is kept in memory so dragging the controller never touches the disk.
    loadDualSenseMesh(g_appDirectory / L"assets" / L"dualsense.dae");
    fs::path logoPath = g_appDirectory / L"assets" / L"setup-rgb-logo.png";
    if (fs::exists(logoPath)) {
        g_logo.reset(Image::FromFile(logoPath.c_str()));
        if (g_logo && g_logo->GetLastStatus() == Ok) {
            Bitmap warmup(2, 2, PixelFormat32bppPARGB);
            Graphics warmupGraphics(&warmup);
            drawBaseLogoRound(warmupGraphics, RectF(0, 0, 2, 2));
        }
    }
    fs::path dualSenseImagePath = g_appDirectory / L"assets" / L"dualsense-controller.png";
    if (fs::exists(dualSenseImagePath)) {
        g_dualSenseImage.reset(Image::FromFile(dualSenseImagePath.c_str()));
    if (!g_dualSenseImage || g_dualSenseImage->GetLastStatus() != Ok) {
        g_dualSensePreview.reset();
        g_dualSenseImage.reset();
    } else {
        // Pre-scale once at startup; painting a small premultiplied bitmap is
        // far cheaper than resampling the 1360px source on every WM_PAINT.
        g_dualSensePreview = std::make_unique<Bitmap>(520, 356, PixelFormat32bppPARGB);
        Graphics previewGraphics(g_dualSensePreview.get());
        previewGraphics.SetCompositingMode(CompositingModeSourceCopy);
        previewGraphics.Clear(Color(0, 0, 0, 0));
        previewGraphics.SetCompositingMode(CompositingModeSourceOver);
        previewGraphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        previewGraphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        previewGraphics.DrawImage(g_dualSenseImage.get(), RectF(0, 0, 520, 356),
                                  0.0f, 145.0f, 1360.0f, 930.0f, UnitPixel);
        if (g_dualSensePreview->GetLastStatus() != Ok) g_dualSensePreview.reset();
    }
    }
    g_screenMonitors = AmbientScreenCapture::enumerateMonitors();

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount >= 2 && std::wstring(arguments[1]) == L"--audio-test") {
        LoopbackAudioAnalyzer analyzer;
        std::wstring audioError;
        bool ok = analyzer.start(audioError);
        for (int attempt = 0; ok && attempt < 12; ++attempt) {
            AudioBands bands;
            Sleep(25);
            ok = analyzer.sample(bands, audioError);
        }
        LocalFree(arguments);
        g_dualSenseModelCache.reset();
        g_dualSenseControlCache.reset();
        g_dualSenseMovingControlCache.reset();
        g_gamepadPageCache.reset();
        g_dualSensePreview.reset();
        g_dualSenseImage.reset();
        g_logo.reset();
        GdiplusShutdown(g_gdiplusToken);
        WSACleanup();
        return ok ? 0 : 22;
    }
    if (arguments && argumentCount >= 2 && std::wstring(arguments[1]) == L"--screen-test") {
        AmbientScreenCapture capture;
        AmbientFrame frame;
        std::wstring screenError;
        const bool ok = !g_screenMonitors.empty() && capture.start(0, 32, 18, screenError) &&
                        capture.sample(frame, 68, 70, true, screenError) && frame.valid;
        LocalFree(arguments);
        g_dualSenseModelCache.reset();
        g_dualSenseControlCache.reset();
        g_dualSenseMovingControlCache.reset();
        g_gamepadPageCache.reset();
        g_dualSensePreview.reset();
        g_dualSenseImage.reset();
        g_logo.reset();
        GdiplusShutdown(g_gdiplusToken);
        WSACleanup();
        return ok ? 0 : 23;
    }
    if (arguments && argumentCount >= 3 && std::wstring(arguments[1]) == L"--render-benchmark") {
        const int result = runRenderBenchmark(arguments[2]);
        LocalFree(arguments);
        g_dualSenseModelCache.reset();
        g_dualSenseControlCache.reset();
        g_dualSenseMovingControlCache.reset();
        g_gamepadPageCache.reset();
        g_dualSensePreview.reset();
        g_dualSenseImage.reset();
        g_logo.reset();
        GdiplusShutdown(g_gdiplusToken);
        WSACleanup();
        return result;
    }
    if (arguments && argumentCount >= 3 && std::wstring(arguments[1]) == L"--capture-all") {
        if (argumentCount >= 5 && std::wstring(arguments[3]) == L"--language") {
            int language = _wtoi(arguments[4]);
            if (language >= 0 && language <= 3) g_language = static_cast<Language>(language);
        }
        int result = createInterfaceCaptures(arguments[2]);
        LocalFree(arguments);
        g_dualSenseModelCache.reset();
        g_dualSenseControlCache.reset();
        g_dualSenseMovingControlCache.reset();
        g_gamepadPageCache.reset();
        g_dualSensePreview.reset();
        g_dualSenseImage.reset();
        g_logo.reset();
        GdiplusShutdown(g_gdiplusToken);
        WSACleanup();
        return result;
    }
    if (arguments && argumentCount >= 3 && std::wstring(arguments[1]) == L"--self-test") {
        int result = runSelfTests(arguments[2]);
        LocalFree(arguments);
        g_dualSenseModelCache.reset();
        g_dualSenseControlCache.reset();
        g_dualSenseMovingControlCache.reset();
        g_gamepadPageCache.reset();
        g_dualSensePreview.reset();
        g_dualSenseImage.reset();
        g_logo.reset();
        GdiplusShutdown(g_gdiplusToken);
        WSACleanup();
        return result;
    }
    loadStoredProfiles();
    loadAutomationSettings();
    if (arguments) {
        for (int index = 1; index < argumentCount; ++index) {
            const std::wstring argument = arguments[index];
            if (argument == L"--fan-profile" && index + 1 < argumentCount) {
                const int profile = _wtoi(arguments[++index]);
                if (profile >= 0 && profile <= 3) g_pendingFanProfile = profile;
            } else if (argument == L"--fan-set" && index + 2 < argumentCount) {
                const std::wstring id = arguments[++index];
                g_pendingFanId.assign(id.begin(), id.end());
                g_pendingFanValue = std::clamp(_wtoi(arguments[++index]), 0, 100);
                g_pendingFanAutomatic = false;
            } else if (argument == L"--fan-auto" && index + 1 < argumentCount) {
                const std::wstring id = arguments[++index];
                g_pendingFanId.assign(id.begin(), id.end());
                g_pendingFanAutomatic = true;
            } else if (argument == L"--fan-curve") {
                g_pendingFanCurve = true;
            } else if (argument == L"--load-profile" && index + 1 < argumentCount) {
                const int profile = _wtoi(arguments[++index]);
                if (profile >= 0 && profile < static_cast<int>(g_profiles.size())) g_pendingSavedProfile = profile;
            } else if (argument == L"--startup") {
                g_startHidden = g_startupQuiet;
            } else if (argument == L"--updated") {
                g_justUpdated = true;
                g_updateStatus = localized(L"Mise à jour installée avec succès.", L"Update installed successfully.",
                                           L"Update erfolgreich installiert.", L"更新安装成功。");
            } else if (argument == L"--enable-xbox") {
                g_xboxModeEnabled = true;
                g_page = Page::Gamepads;
            }
        }
    }
    if (arguments) LocalFree(arguments);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = L"RGBCcontrolNativeWindow";
    RegisterClassExW(&windowClass);

    g_window = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, L"RGBCcontrol",
                               WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, instance, nullptr);
    if (!g_window) return 2;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark));
    int corner = 2;
    DwmSetWindowAttribute(g_window, 33, &corner, sizeof(corner));
    COLORREF border = RGB(49, 55, 72);
    DwmSetWindowAttribute(g_window, 34, &border, sizeof(border));
    if (g_xboxModeEnabled) startXboxBridge();
    if (!g_startHidden) {
        ShowWindow(g_window, showCommand);
        UpdateWindow(g_window);
    } else {
        ShowWindow(g_window, SW_HIDE);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_dualSenseModelCache.reset();
    g_dualSenseControlCache.reset();
    g_dualSenseMovingControlCache.reset();
    g_gamepadPageCache.reset();
    g_dualSensePreview.reset();
    g_dualSenseImage.reset();
    g_logo.reset();
    GdiplusShutdown(g_gdiplusToken);
    WSACleanup();
    return static_cast<int>(message.wParam);
}
