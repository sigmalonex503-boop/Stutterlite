// StutterLite — experimental LeviLauncher native mod.
//
// What this does, in plain terms:
//  1. "Skip decorative blocks" (OFF by default): while the game is building the
//     visible shape of a loaded area, blocks like tall grass / ferns / flowers /
//     seagrass are skipped instead of drawn. Less work per loaded area while you
//     move around or raise render distance. The blocks themselves are untouched
//     in the world, only their outline is not drawn, while this is turned on.
//  2. "Thread monitor" (ON by default, log-only): counts how many background
//     threads the game creates and prints a summary to the Android log every
//     few seconds. It does NOT change anything about those threads. This is a
//     first step to find out whether thread activity lines up with the
//     stutter moments, before we try changing anything.
//
// Safety notes:
//  - If the block signature below does not match this game build, feature 1
//    silently stays unavailable (logged once) instead of crashing anything.
//  - Feature 2 never touches thread priorities; it only counts and logs.
//  - Both features can be turned on/off from the in-game Mod Menu.
//
// Minecraft Bedrock target: 1.26.4x (tested signature family from community
// mods for this build range; NOT guaranteed to match your exact game build —
// see the project notes).

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#define SL_TAG "StutterLite"
#define SL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, SL_TAG, __VA_ARGS__)
#define SL_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SL_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Minimal declarations for the LeviLauncher "preloader" mod API (pl::...).
// These are interface declarations only (no implementation) — the real code
// lives in libpreloader.so, already loaded by the launcher at runtime. This
// is the same approach every native LeviLauncher mod uses to talk to it.
// ---------------------------------------------------------------------------
namespace pl::memory {
using FuncPtr = void*;
enum class HookPriority : int { Highest = 0, High = 100, Normal = 200, Low = 300, Lowest = 400 };
// Finds a function inside a loaded library by a wildcard byte pattern
// (bytes written as hex, "??" or "?" meaning "any byte here").
uintptr_t resolveSignature(std::string_view signature, std::string_view moduleName);
// Redirects calls to `target` into `detour`; the original function pointer
// is written into *originalFunc so the detour can still call it.
int hook(FuncPtr target, FuncPtr detour, FuncPtr* originalFunc,
         HookPriority priority = HookPriority::Normal);
bool unhook(FuncPtr target, FuncPtr detour);
}  // namespace pl::memory

namespace pl::modmenu {
enum class ConfigType { Toggle, SliderInt, SliderFloat, Radio, Color, Keybind, Text, Button };
struct ConfigEntry {
    std::string key;
    std::string displayName;
    ConfigType type{};
    std::string defaultValue;
    std::string minValue;
    std::string maxValue;
    std::string dependsOn;
};
struct ModuleInfo {
    std::string moduleId;
    std::string displayName;
    std::string description;
    std::string modId;
    bool defaultEnabled{};
    bool hideInHudEditor{};
    std::vector<ConfigEntry> configs;
    std::function<void(std::string_view, bool)> onToggle;
    std::function<void(std::string_view, std::string_view, std::string_view)> onConfigChanged;
    std::function<void(std::string_view, std::string_view, bool)> onKeybind;
};
bool registerModule(const ModuleInfo& info);
void unregisterModule(std::string_view moduleId);
}  // namespace pl::modmenu

// ABI the launcher expects every native mod's entry point to return.
struct ModRegistration {
    void* instance{};
    bool (*load)(void*, void*){};
    bool (*enable)(void*, void*){};
    bool (*disable)(void*, void*){};
    bool (*unload)(void*, void*){};
};

// ---------------------------------------------------------------------------
// Feature 1: skip decorative blocks while the world builds visible geometry.
// ---------------------------------------------------------------------------
namespace feature_skip {

// The function we hook decides whether one block gets added to the drawn
// shape of its loaded area. Signature and parameter layout come from public
// LeviLauncher mod sources that target Minecraft Bedrock 1.26.4x.
struct BlockPos { int32_t x{}, y{}, z{}; };
using BlockFn = bool (*)(void*, void*, void*, const BlockPos*, bool);

constexpr char kBlockSig[] =
    "FF 83 ?? D1 FD 7B 14 A9 FC 6F 15 A9 FA 67 16 A9 F8 5F 17 A9 F6 57 18 A9 "
    "F4 4F 19 A9 FD 03 05 91 5A D0 3B D5 F7 03 04 2A F4 03 03 AA ?? ?? 40 F9 "
    "F6 03 02 AA F3 03 00 AA F5 03 01 AA";

std::atomic_bool gAvailable{false};
std::atomic_bool gEnabled{false};
BlockFn gOriginal{nullptr};
void* gTarget{nullptr};

// Decorative, no-collision blocks only. Deliberately conservative: nothing
// here affects movement, redstone, farming or world data — only whether the
// block's own shape gets drawn while this feature is turned on.
const std::vector<std::string_view>& skipSuffixes() {
    static const std::vector<std::string_view> list = {
        "short_grass", "tall_grass", "fern", "large_fern",
        "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet",
        "red_tulip", "orange_tulip", "white_tulip", "pink_tulip",
        "oxeye_daisy", "cornflower", "lily_of_the_valley", "wither_rose",
        "sunflower", "lilac", "rose_bush", "peony", "seagrass",
    };
    return list;
}

struct RawStringView { const char* data{}; size_t size{}; bool valid{}; };

// Reads a libc++ std::string in-place (handles both the short and long
// string layouts). This mirrors the standard libc++ string ABI used by the
// Android NDK build of the game, not anything specific to one mod.
RawStringView readLibcppString(const void* obj) {
    if (!obj) return {};
    const auto* p = reinterpret_cast<const unsigned char*>(obj);
    if ((p[0] & 1u) == 0u) {
        const size_t n = size_t(p[0] >> 1u);
        if (n > 22u) return {};
        return {reinterpret_cast<const char*>(p + 1), n, true};
    }
    size_t n{};
    const char* d{};
    std::memcpy(&n, p + 8, sizeof(n));
    std::memcpy(&d, p + 16, sizeof(d));
    if (!d || n == 0 || n > 128u) return {};
    return {d, n, true};
}

bool endsWithAscii(const RawStringView& s, std::string_view suffix) {
    if (!s.valid || suffix.empty() || s.size < suffix.size()) return false;
    return std::string_view(s.data, s.size).substr(s.size - suffix.size()) == suffix;
}

bool isDecorative(void* block) {
    if (!block) return false;
    const auto* b = reinterpret_cast<const unsigned char*>(block);
    void* blockType{};
    std::memcpy(&blockType, b + 0x68, sizeof(blockType));
    if (!blockType) return false;
    const auto* bt = reinterpret_cast<const unsigned char*>(blockType);
    const auto full = readLibcppString(bt + 0xD0);
    const auto desc = readLibcppString(bt + 0x8);
    for (auto suf : skipSuffixes()) {
        if (endsWithAscii(full, suf) || endsWithAscii(desc, suf)) return true;
    }
    return false;
}

bool detour(void* self, void* tess, void* block, const BlockPos* pos, bool flag) {
    if (gEnabled.load(std::memory_order_relaxed) && pos && isDecorative(block)) {
        return false;  // nothing drawn for this block this time
    }
    return gOriginal(self, tess, block, pos, flag);
}

bool install() {
    uintptr_t addr = pl::memory::resolveSignature(kBlockSig, "libminecraftpe.so");
    if (!addr || (addr & 3u)) {
        SL_LOGE("skip-decorative: signature not found on this game build — feature unavailable");
        return false;
    }
    gTarget = reinterpret_cast<void*>(addr);
    if (pl::memory::hook(gTarget, reinterpret_cast<void*>(&detour),
                          reinterpret_cast<void**>(&gOriginal)) != 0) {
        SL_LOGE("skip-decorative: hook install failed");
        gTarget = nullptr;
        return false;
    }
    gAvailable.store(true, std::memory_order_release);
    SL_LOGI("skip-decorative: ready (target=%p)", gTarget);
    return true;
}

void remove() {
    if (gTarget && gOriginal) {
        pl::memory::unhook(gTarget, reinterpret_cast<void*>(&detour));
    }
    gTarget = nullptr;
    gOriginal = nullptr;
    gAvailable.store(false, std::memory_order_release);
}

}  // namespace feature_skip

// ---------------------------------------------------------------------------
// Feature 2: thread monitor (counts only, changes nothing).
// ---------------------------------------------------------------------------
namespace feature_threads {

using CreateFn = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

std::atomic_bool gEnabled{true};
std::atomic_bool gInstalled{false};
std::atomic<uint64_t> gTotalCreated{0};
std::atomic<uint64_t> gSinceReport{0};
CreateFn gOriginalCreate{nullptr};
void* gTarget{nullptr};
pthread_t gReporter{};
std::atomic_bool gReporterRunning{false};

int detourCreate(pthread_t* out, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    gTotalCreated.fetch_add(1, std::memory_order_relaxed);
    gSinceReport.fetch_add(1, std::memory_order_relaxed);
    return gOriginalCreate(out, attr, start, arg);
}

void* reporterMain(void*) {
    while (gReporterRunning.load(std::memory_order_relaxed)) {
        struct timespec ts {5, 0};
        nanosleep(&ts, nullptr);
        if (!gEnabled.load(std::memory_order_relaxed)) continue;
        uint64_t since = gSinceReport.exchange(0, std::memory_order_relaxed);
        uint64_t total = gTotalCreated.load(std::memory_order_relaxed);
        SL_LOGI("thread-monitor: +%llu new threads in the last 5s (total since enable: %llu)",
                (unsigned long long)since, (unsigned long long)total);
    }
    return nullptr;
}

bool install() {
    void* sym = dlsym(RTLD_DEFAULT, "pthread_create");
    if (!sym) {
        SL_LOGE("thread-monitor: pthread_create symbol not found — feature unavailable");
        return false;
    }
    gTarget = sym;
    if (pl::memory::hook(gTarget, reinterpret_cast<void*>(&detourCreate),
                          reinterpret_cast<void**>(&gOriginalCreate)) != 0) {
        SL_LOGE("thread-monitor: hook install failed");
        gTarget = nullptr;
        return false;
    }
    gReporterRunning.store(true, std::memory_order_relaxed);
    if (pthread_create(&gReporter, nullptr, &reporterMain, nullptr) != 0) {
        SL_LOGE("thread-monitor: could not start its own reporting thread");
        gReporterRunning.store(false, std::memory_order_relaxed);
    }
    gInstalled.store(true, std::memory_order_release);
    SL_LOGI("thread-monitor: ready (observe-only, logs every 5s)");
    return true;
}

void remove() {
    gReporterRunning.store(false, std::memory_order_relaxed);
    if (gTarget && gOriginalCreate) {
        pl::memory::unhook(gTarget, reinterpret_cast<void*>(&detourCreate));
    }
    gTarget = nullptr;
    gOriginalCreate = nullptr;
    gInstalled.store(false, std::memory_order_release);
}

}  // namespace feature_threads

// ---------------------------------------------------------------------------
// Mod Menu registration (the two toggles shown in-game).
// ---------------------------------------------------------------------------
namespace menu {

constexpr std::string_view kModId = "StutterLite";
constexpr std::string_view kModule = "stutterlite";

// Builds a one-line human-readable status so the user can open the mod menu,
// read it (or screenshot it) and know immediately whether each feature is
// actually running on their game build — no logcat app needed for this part.
std::string statusLine() {
    std::string s = "STATUS -> skip_decorative: ";
    s += feature_skip::gAvailable.load(std::memory_order_relaxed) ? "ready" : "NOT available on this game build";
    s += " | thread_monitor: ";
    s += feature_threads::gInstalled.load(std::memory_order_relaxed) ? "running" : "NOT available";
    return s;
}

bool registerMenu() {
    pl::modmenu::ModuleInfo m{};
    m.moduleId = std::string(kModule);
    m.displayName = "StutterLite";
    m.description = "Experimental stutter-reduction toggles (test one at a time)";
    m.modId = std::string(kModId);
    m.defaultEnabled = true;
    m.hideInHudEditor = true;
    const std::string status = statusLine();
    m.configs = {
        // Read-only line: open this screen after loading the game and
        // screenshot it — that single line tells you whether each feature
        // actually took effect on your installed game version.
        {"status", status, pl::modmenu::ConfigType::Text, status, "", "", ""},
        {"skip_decorative",
         feature_skip::gAvailable.load(std::memory_order_relaxed)
             ? "Skip decorative blocks (grass/flowers)"
             : "Skip decorative blocks — unavailable on this game build",
         pl::modmenu::ConfigType::Toggle, "false", "", "", ""},
        {"thread_monitor",
         feature_threads::gInstalled.load(std::memory_order_relaxed)
             ? "Thread monitor (log only, no changes)"
             : "Thread monitor — unavailable on this device",
         pl::modmenu::ConfigType::Toggle, "true", "", "", ""},
    };
    m.onToggle = [](std::string_view, bool) {};
    m.onConfigChanged = [](std::string_view, std::string_view key, std::string_view value) {
        bool on = (value == "true" || value == "1" || value == "on" || value == "yes");
        if (key == "skip_decorative") {
            feature_skip::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("skip-decorative: %s", on ? "ON" : "OFF");
        } else if (key == "thread_monitor") {
            feature_threads::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("thread-monitor: %s", on ? "ON" : "OFF");
        }
    };
    return pl::modmenu::registerModule(m);
}

void unregisterMenu() { pl::modmenu::unregisterModule(kModule); }

}  // namespace menu

// ---------------------------------------------------------------------------
// Mod lifecycle + entry point.
// ---------------------------------------------------------------------------
namespace {

class StutterLiteMod {
public:
    bool load() {
        SL_LOGI("load");
        return true;
    }

    bool enable() {
        // Each feature is independent: one failing does not stop the others.
        skipReady_ = feature_skip::install();
        threadsReady_ = feature_threads::install();
        menuReady_ = menu::registerMenu();
        if (!menuReady_) SL_LOGE("mod menu registration failed; features keep their defaults");
        SL_LOGI("enabled (skip_decorative_available=%d, thread_monitor_available=%d)",
                skipReady_, threadsReady_);
        return true;  // stay loaded even if one feature is unavailable
    }

    bool disable() {
        if (menuReady_) {
            menu::unregisterMenu();
            menuReady_ = false;
        }
        if (skipReady_) {
            feature_skip::remove();
            skipReady_ = false;
        }
        if (threadsReady_) {
            feature_threads::remove();
            threadsReady_ = false;
        }
        SL_LOGI("disabled");
        return true;
    }

    bool unload() {
        disable();
        SL_LOGI("unloaded");
        return true;
    }

private:
    bool skipReady_{false};
    bool threadsReady_{false};
    bool menuReady_{false};
};

StutterLiteMod gMod;

bool loadThunk(void*, void*) {
    try {
        return gMod.load();
    } catch (...) {
        SL_LOGE("load() threw");
        return false;
    }
}
bool enableThunk(void*, void*) {
    try {
        return gMod.enable();
    } catch (...) {
        SL_LOGE("enable() threw");
        return false;
    }
}
bool disableThunk(void*, void*) {
    try {
        return gMod.disable();
    } catch (...) {
        SL_LOGE("disable() threw");
        return false;
    }
}
bool unloadThunk(void*, void*) {
    try {
        return gMod.unload();
    } catch (...) {
        SL_LOGE("unload() threw");
        return false;
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) ModRegistration* PLGetModRegistration() {
    static ModRegistration r{&gMod, &loadThunk, &enableThunk, &disableThunk, &unloadThunk};
    return &r;
}
