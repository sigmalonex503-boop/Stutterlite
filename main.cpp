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
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
// ---------------------------------------------------------------------------
// Feature 3: report fewer CPU cores to the game (experimental).
//
// Why: several real Bedrock/Java performance mods (Volt, StutterFix) get
// their gains by limiting how many worker threads the game spins up for
// background work like chunk building — fewer threads fighting over the
// same CPU cores means less contention on the main thread. The game (and
// the C++ runtime underneath it) decides how many worker threads to create
// by asking the OS "how many CPU cores are online?" via the standard
// sysconf(_SC_NPROCESSORS_ONLN) call. We hook that single, simple,
// version-independent OS function (same hooking style already proven to
// work by the GL-hooking mods on this device) and make it return a smaller
// number than the real core count — nothing else about the device changes,
// we just answer that one question differently.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Feature 5: shader program binary cache.
//
// The first time any GLSL shader program is used, the driver has to compile
// and link it — that compile step is a known, well-documented cause of a
// one-time hitch (not just on this game; it's a classic PC/console cause of
// stutter too). This works whether the shader is one of the game's own
// built-in ones or from a third-party shader patch — it doesn't care where
// the shader came from, only that compiling it is slow and re-compiling the
// exact same thing every single launch is wasted work.
//
// How: hook three small, standard OpenGL ES entry points (glShaderSource,
// glAttachShader, glLinkProgram) via the same dlsym + pl::memory::hook
// pattern already used elsewhere in this file — no GL internals are
// touched beyond calling the real functions. We fingerprint each program by
// the combined source text of the shaders attached to it, and keep a
// compiled-binary cache file per fingerprint in the app's own private cache
// folder. On a cache hit we try glProgramBinary instead of a real link; if
// that fails for any reason we fall straight back to the real link, so a
// stale/incompatible cache entry can never break rendering — it just
// costs the same one-time compile it would have anyway.
// ---------------------------------------------------------------------------
namespace feature_shadercache {

using GlShaderSourceFn = void (*)(unsigned, int, const char* const*, const int*);
using GlAttachShaderFn = void (*)(unsigned, unsigned);
using GlLinkProgramFn = void (*)(unsigned);
using GlGetProgramivFn = void (*)(unsigned, unsigned, int*);
using GlGetProgramBinaryFn = void (*)(unsigned, int, int*, unsigned*, void*);
using GlProgramBinaryFn = void (*)(unsigned, unsigned, const void*, int);

constexpr unsigned GL_LINK_STATUS = 0x8B82;
constexpr unsigned GL_PROGRAM_BINARY_LENGTH = 0x8741;

std::atomic_bool gEnabled{true};
std::atomic_bool gInstalled{false};
std::atomic<uint32_t> gHits{0};
std::atomic<uint32_t> gMisses{0};

GlShaderSourceFn gOrigShaderSource{nullptr};
GlAttachShaderFn gOrigAttachShader{nullptr};
GlLinkProgramFn gOrigLinkProgram{nullptr};
GlGetProgramivFn gGetProgramiv{nullptr};
GlGetProgramBinaryFn gGetProgramBinary{nullptr};
GlProgramBinaryFn gProgramBinary{nullptr};

std::mutex gMutex;
std::unordered_map<unsigned, uint64_t> gShaderHash;   // shader id -> source hash
std::unordered_map<unsigned, uint64_t> gProgramKey;   // program id -> combined key
std::string gCacheDir;

uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL) {
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Finds (or creates) this app's own private cache directory — always
// writable by the app itself, no storage permission needed, nothing shared
// with other apps. Reads the process name from /proc/self/cmdline rather
// than guessing a package name.
bool resolveCacheDir() {
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return false;
    char name[256] = {0};
    size_t n = fread(name, 1, sizeof(name) - 1, f);
    fclose(f);
    if (n == 0) return false;
    std::string pkg(name);  // cmdline is NUL-separated; first token is the process name
    if (pkg.empty()) return false;
    std::string dir = "/data/data/" + pkg + "/cache/stutterlite_shadercache";
    mkdir(("/data/data/" + pkg + "/cache").c_str(), 0700);
    mkdir(dir.c_str(), 0700);
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0 || !(st.st_mode & S_IFDIR)) return false;
    gCacheDir = dir;
    return true;
}

std::string cachePathFor(uint64_t key) {
    char buf[32];
    snprintf(buf, sizeof(buf), "/%016llx.bin", (unsigned long long)key);
    return gCacheDir + buf;
}

void detourShaderSource(unsigned shader, int count, const char* const* strings, const int* lengths) {
    gOrigShaderSource(shader, count, strings, lengths);
    if (!gEnabled.load(std::memory_order_relaxed)) return;
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < count; ++i) {
        if (!strings || !strings[i]) continue;
        size_t len = (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : std::strlen(strings[i]);
        h = fnv1a(strings[i], len, h);
    }
    std::lock_guard<std::mutex> lock(gMutex);
    gShaderHash[shader] = h;
}

void detourAttachShader(unsigned program, unsigned shader) {
    gOrigAttachShader(program, shader);
    if (!gEnabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gShaderHash.find(shader);
    uint64_t sh = (it != gShaderHash.end()) ? it->second : 0;
    uint64_t& key = gProgramKey[program];
    key = fnv1a(&sh, sizeof(sh), key ? key : 1469598103934665603ULL);
}

void detourLinkProgram(unsigned program) {
    if (!gEnabled.load(std::memory_order_relaxed) || gCacheDir.empty()) {
        gOrigLinkProgram(program);
        return;
    }
    uint64_t key;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gProgramKey.find(program);
        key = (it != gProgramKey.end()) ? it->second : 0;
    }
    if (key == 0 || !gGetProgramBinary || !gProgramBinary || !gGetProgramiv) {
        gOrigLinkProgram(program);
        return;
    }

    std::string path = cachePathFor(key);
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        unsigned format = 0;
        fread(&format, sizeof(format), 1, f);
        fseek(f, 0, SEEK_END);
        long total = ftell(f);
        long dataLen = total - (long)sizeof(format);
        if (dataLen > 0) {
            fseek(f, sizeof(format), SEEK_SET);
            std::vector<unsigned char> buf(dataLen);
            fread(buf.data(), 1, dataLen, f);
            fclose(f);
            gProgramBinary(program, format, buf.data(), (int)dataLen);
            int ok = 0;
            gGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (ok) {
                gHits.fetch_add(1, std::memory_order_relaxed);
                return;  // cache hit — skipped a real shader compile+link
            }
            // Cached binary didn't take (driver/version mismatch) — fall
            // through to a real link below, same as a normal cache miss.
        } else {
            fclose(f);
        }
    }

    gOrigLinkProgram(program);
    gMisses.fetch_add(1, std::memory_order_relaxed);

    int ok = 0;
    gGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) return;
    int binLen = 0;
    gGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binLen);
    if (binLen <= 0) return;
    std::vector<unsigned char> buf(binLen);
    int actualLen = 0;
    unsigned format = 0;
    gGetProgramBinary(program, binLen, &actualLen, &format, buf.data());
    if (actualLen <= 0) return;
    FILE* out = fopen(path.c_str(), "wb");
    if (!out) return;
    fwrite(&format, sizeof(format), 1, out);
    fwrite(buf.data(), 1, actualLen, out);
    fclose(out);
}

bool install() {
    if (!resolveCacheDir()) {
        SL_LOGE("shader-cache: could not find a private cache directory — feature unavailable");
        return false;
    }
    void* src = dlsym(RTLD_DEFAULT, "glShaderSource");
    void* att = dlsym(RTLD_DEFAULT, "glAttachShader");
    void* link = dlsym(RTLD_DEFAULT, "glLinkProgram");
    gGetProgramiv = reinterpret_cast<GlGetProgramivFn>(dlsym(RTLD_DEFAULT, "glGetProgramiv"));
    gGetProgramBinary = reinterpret_cast<GlGetProgramBinaryFn>(dlsym(RTLD_DEFAULT, "glGetProgramBinary"));
    gProgramBinary = reinterpret_cast<GlProgramBinaryFn>(dlsym(RTLD_DEFAULT, "glProgramBinary"));
    if (!src || !att || !link || !gGetProgramiv || !gGetProgramBinary || !gProgramBinary) {
        SL_LOGE("shader-cache: required GL entry points not found — feature unavailable");
        return false;
    }
    bool ok = true;
    ok &= pl::memory::hook(src, reinterpret_cast<void*>(&detourShaderSource),
                            reinterpret_cast<void**>(&gOrigShaderSource)) == 0;
    ok &= pl::memory::hook(att, reinterpret_cast<void*>(&detourAttachShader),
                            reinterpret_cast<void**>(&gOrigAttachShader)) == 0;
    ok &= pl::memory::hook(link, reinterpret_cast<void*>(&detourLinkProgram),
                            reinterpret_cast<void**>(&gOrigLinkProgram)) == 0;
    if (!ok) {
        SL_LOGE("shader-cache: one or more hooks failed to install");
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    SL_LOGI("shader-cache: ready (cache dir=%s)", gCacheDir.c_str());
    return true;
}

void remove() {
    if (gOrigShaderSource) {
        pl::memory::unhook(reinterpret_cast<void*>(dlsym(RTLD_DEFAULT, "glShaderSource")),
                            reinterpret_cast<void*>(&detourShaderSource));
    }
    if (gOrigAttachShader) {
        pl::memory::unhook(reinterpret_cast<void*>(dlsym(RTLD_DEFAULT, "glAttachShader")),
                            reinterpret_cast<void*>(&detourAttachShader));
    }
    if (gOrigLinkProgram) {
        pl::memory::unhook(reinterpret_cast<void*>(dlsym(RTLD_DEFAULT, "glLinkProgram")),
                            reinterpret_cast<void*>(&detourLinkProgram));
    }
    gOrigShaderSource = nullptr;
    gOrigAttachShader = nullptr;
    gOrigLinkProgram = nullptr;
    gInstalled.store(false, std::memory_order_release);
}

}  // namespace feature_shadercache

// ---------------------------------------------------------------------------
// Feature 6: world-file prefetching (I/O warm-up).
//
// Everything else in this file targets the CPU. This one targets a
// different, separate cause of stutter: reading the world's save data off
// the phone's own storage. Bedrock worlds are stored as a small database
// (a handful of .ldb files in the world's "db" folder), not as one file per
// loaded area, so "prefetch the chunks near the player" isn't something we
// can target individually without understanding that database's internal
// format. What we CAN do safely and generically: once we see the game open
// a file inside a world's db folder for the first time, ask the OS to start
// pulling that whole folder into its page cache in the background
// (posix_fadvise WILLNEED) — read-only, best-effort, so later real reads
// from the game are more likely to already be in RAM instead of waiting on
// storage.
// ---------------------------------------------------------------------------
namespace feature_prefetch {

using OpenFn = int (*)(const char*, int, ...);

std::atomic_bool gEnabled{true};
std::atomic_bool gInstalled{false};
std::atomic_bool gTriggeredOnce{false};
std::atomic<uint32_t> gFilesWarmed{0};
OpenFn gOriginal{nullptr};
void* gTarget{nullptr};

std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? "" : path.substr(0, pos);
}

void* warmupThread(void* arg) {
    std::string* dirPtr = reinterpret_cast<std::string*>(arg);
    std::string dir = *dirPtr;
    delete dirPtr;

    DIR* d = opendir(dir.c_str());
    if (!d) return nullptr;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string path = dir + "/" + ent->d_name;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);
        feature_prefetch::gFilesWarmed.fetch_add(1, std::memory_order_relaxed);
    }
    closedir(d);
    SL_LOGI("prefetch: warmed up world db folder (%s)", dir.c_str());
    return nullptr;
}

int detour(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    int fd = gOriginal(path, flags, mode);

    if (gEnabled.load(std::memory_order_relaxed) && !gTriggeredOnce.load(std::memory_order_relaxed) &&
        path && std::strstr(path, "/minecraftWorlds/") && std::strstr(path, "/db/")) {
        gTriggeredOnce.store(true, std::memory_order_relaxed);
        std::string dir = dirOf(path);
        if (!dir.empty()) {
            auto* heapDir = new std::string(dir);
            pthread_t t;
            if (pthread_create(&t, nullptr, &warmupThread, heapDir) == 0) {
                pthread_detach(t);
            } else {
                delete heapDir;
            }
        }
    }
    return fd;
}

bool install() {
    void* sym = dlsym(RTLD_DEFAULT, "open");
    if (!sym) {
        SL_LOGE("prefetch: open() symbol not found — feature unavailable");
        return false;
    }
    gTarget = sym;
    if (pl::memory::hook(gTarget, reinterpret_cast<void*>(&detour),
                          reinterpret_cast<void**>(&gOriginal)) != 0) {
        SL_LOGE("prefetch: hook install failed");
        gTarget = nullptr;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    SL_LOGI("prefetch: ready (watching for a world's db folder to open)");
    return true;
}

void remove() {
    if (gTarget && gOriginal) {
        pl::memory::unhook(gTarget, reinterpret_cast<void*>(&detour));
    }
    gTarget = nullptr;
    gOriginal = nullptr;
    gTriggeredOnce.store(false, std::memory_order_relaxed);
    gInstalled.store(false, std::memory_order_release);
}

}  // namespace feature_prefetch

namespace feature_cores {

using SysconfFn = long (*)(int);

std::atomic_bool gEnabled{false};
std::atomic_bool gInstalled{false};
std::atomic<long> gRealCores{0};
std::atomic<long> gReportedCores{0};
// Set by feature_auto while it's actively reacting to a stutter spike; 0
// means "no override, use gReportedCores as normal." This lets the
// auto-sense system squeeze cores down further for a few seconds without
// touching the user's own saved setting.
std::atomic<long> gBoostCap{0};
SysconfFn gOriginal{nullptr};
void* gTarget{nullptr};

long detour(int name) {
    long real = gOriginal(name);
    if (name == _SC_NPROCESSORS_ONLN && gEnabled.load(std::memory_order_relaxed)) {
        long cap = gReportedCores.load(std::memory_order_relaxed);
        long boost = gBoostCap.load(std::memory_order_relaxed);
        if (boost > 0 && boost < cap) cap = boost;
        if (cap > 0 && cap < real) return cap;
    }
    return real;
}

bool install() {
    void* sym = dlsym(RTLD_DEFAULT, "sysconf");
    if (!sym) {
        SL_LOGE("cpu-cores: sysconf symbol not found — feature unavailable");
        return false;
    }
    gTarget = sym;
    if (pl::memory::hook(gTarget, reinterpret_cast<void*>(&detour),
                          reinterpret_cast<void**>(&gOriginal)) != 0) {
        SL_LOGE("cpu-cores: hook install failed");
        gTarget = nullptr;
        return false;
    }
    long real = gOriginal(_SC_NPROCESSORS_ONLN);
    gRealCores.store(real, std::memory_order_relaxed);
    // Default target: leave at least half the cores visible, minimum 2 —
    // matches StutterFix's "fewer worker threads" idea without starving the
    // game entirely. The menu lets you change this number directly.
    long target = real > 3 ? (real / 2) : real;
    if (target < 2) target = real;  // don't go below 2 on small devices
    gReportedCores.store(target, std::memory_order_relaxed);
    gInstalled.store(true, std::memory_order_release);
    SL_LOGI("cpu-cores: ready (real=%ld, default_reported=%ld)", real, target);
    return true;
}

void remove() {
    if (gTarget && gOriginal) {
        pl::memory::unhook(gTarget, reinterpret_cast<void*>(&detour));
    }
    gTarget = nullptr;
    gOriginal = nullptr;
    gInstalled.store(false, std::memory_order_release);
}

}  // namespace feature_cores

// ---------------------------------------------------------------------------
// Feature 4: adaptive memory trimming (not just "2 lines").
//
// Android's allocator (jemalloc/scudo) keeps recently-freed memory pages
// around for reuse instead of giving them back to the OS right away — good
// for a desktop, less good on a 3.7 GB phone where another app getting
// killed for memory, or the game itself crashing with an OOM, is a real
// cost. This feature does two real things, not just a one-off call:
//
//   1. On startup, tells the allocator to be less "sticky" about holding
//      onto freed pages (mallopt M_DECAY_TIME = 0).
//   2. Runs its own background thread that actually watches the process's
//      real memory usage (reads /proc/self/status, same number the device
//      debug screen shows) every few seconds. It only forces a page-release
//      pass (mallopt M_PURGE) when memory has genuinely grown since the
//      last check — not on a blind timer — so it isn't doing pointless work
//      every tick for no reason.
//
// Everything here uses documented Bionic (Android libc) calls, not hooks or
// signatures, so this feature can't break from a game update and carries
// close to zero crash risk.
// ---------------------------------------------------------------------------
namespace feature_memory {

std::atomic_bool gEnabled{true};
std::atomic_bool gInstalled{false};
std::atomic_bool gRunning{false};
std::atomic<long> gLastRssKb{0};
std::atomic<long> gLastPurgeRssKb{0};
std::atomic<uint32_t> gPurgeCount{0};
pthread_t gWorker{};

// Threshold: only purge once RSS has grown by at least this much since the
// last purge. Keeps the feature from doing real work needlessly.
constexpr long kGrowthThresholdKb = 48 * 1024;  // 48 MB
constexpr unsigned kCheckIntervalSec = 8;

long readRssKb() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    long rss = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, " %ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

void* workerMain(void*) {
    while (gRunning.load(std::memory_order_relaxed)) {
        struct timespec ts {kCheckIntervalSec, 0};
        nanosleep(&ts, nullptr);
        if (!gEnabled.load(std::memory_order_relaxed)) continue;

        long rss = readRssKb();
        if (rss < 0) continue;
        gLastRssKb.store(rss, std::memory_order_relaxed);

        long sinceLastPurge = rss - gLastPurgeRssKb.load(std::memory_order_relaxed);
        if (sinceLastPurge >= kGrowthThresholdKb) {
            mallopt(M_PURGE, 0);
            long after = readRssKb();
            gLastPurgeRssKb.store(after >= 0 ? after : rss, std::memory_order_relaxed);
            gPurgeCount.fetch_add(1, std::memory_order_relaxed);
            SL_LOGI("memory: purged (rss %ld KB -> %ld KB, grew %ld KB since last purge)",
                    rss, after >= 0 ? after : rss, sinceLastPurge);
        }
    }
    return nullptr;
}

bool install() {
    mallopt(M_DECAY_TIME, 0);
    long rss = readRssKb();
    gLastRssKb.store(rss >= 0 ? rss : 0, std::memory_order_relaxed);
    gLastPurgeRssKb.store(rss >= 0 ? rss : 0, std::memory_order_relaxed);

    gRunning.store(true, std::memory_order_relaxed);
    if (pthread_create(&gWorker, nullptr, &workerMain, nullptr) != 0) {
        SL_LOGE("memory: could not start its own monitor thread");
        gRunning.store(false, std::memory_order_relaxed);
        gInstalled.store(true, std::memory_order_release);  // decay-time tweak alone still applies
        return true;
    }
    gInstalled.store(true, std::memory_order_release);
    SL_LOGI("memory: ready (initial rss=%ld KB, check every %us, purge threshold=%ld KB)",
            rss, kCheckIntervalSec, kGrowthThresholdKb);
    return true;
}

void remove() {
    gRunning.store(false, std::memory_order_relaxed);
    gInstalled.store(false, std::memory_order_release);
}

// Lets another feature (feature_auto) ask for an immediate purge, bypassing
// the usual growth-threshold check, for a genuine "I detected a stutter
// right now" moment rather than the normal periodic check.
void forcePurgeNow() {
    if (!gInstalled.load(std::memory_order_relaxed)) return;
    mallopt(M_PURGE, 0);
    long after = readRssKb();
    if (after >= 0) gLastRssKb.store(after, std::memory_order_relaxed);
    gLastPurgeRssKb.store(after >= 0 ? after : gLastPurgeRssKb.load(), std::memory_order_relaxed);
    gPurgeCount.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace feature_memory

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
// Feature 7: auto-sense ("reacts to real stutter, not a timer").
//
// Every other toggle in this mod is "on all the time" once you enable it —
// useful, but it means you pay whatever small cost a feature has even in
// moments it isn't needed. This feature instead watches real frame time
// (hooking eglSwapBuffers, the same proven, version-independent hook point
// several other published mods on this device already use) and only
// reacts when it sees an actual spike: a few frames in a row taking far
// longer than normal. When that happens, it briefly (a few seconds)
// squeezes the reported CPU core count further via feature_cores's boost
// override, and asks feature_memory to purge immediately instead of
// waiting for its normal periodic check — then relaxes back to your normal
// settings once frame times are calm again.
// ---------------------------------------------------------------------------
namespace feature_auto {

using SwapFn = int (*)(void*, void*);

std::atomic_bool gEnabled{true};
std::atomic_bool gInstalled{false};
std::atomic_bool gBoosting{false};
std::atomic<uint32_t> gSpikesHandled{0};
SwapFn gOriginal{nullptr};
void* gTarget{nullptr};

// A single frame over this is a likely stutter (roughly below 20 FPS for
// that one frame). Two of these close together triggers boost mode.
constexpr long kSpikeThresholdUs = 50'000;      // 50 ms
constexpr long kBoostHoldUs = 3'000'000;        // stay boosted 3s after the last spike
constexpr long kAutoCoolBoostCores = 2;         // aggressive-but-safe floor while boosting

long lastFrameUs = 0;
long lastSpikeAtUs = 0;
int consecutiveSpikes = 0;

long nowUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1'000'000L + ts.tv_nsec / 1000;
}

int detour(void* display, void* surface) {
    int ret = gOriginal(display, surface);
    if (!gEnabled.load(std::memory_order_relaxed)) return ret;

    long now = nowUs();
    if (lastFrameUs != 0) {
        long frameUs = now - lastFrameUs;
        if (frameUs >= kSpikeThresholdUs) {
            consecutiveSpikes++;
            lastSpikeAtUs = now;
            if (consecutiveSpikes >= 2 && !gBoosting.load(std::memory_order_relaxed)) {
                gBoosting.store(true, std::memory_order_relaxed);
                long realCores = feature_cores::gRealCores.load(std::memory_order_relaxed);
                long cap = std::min<long>(kAutoCoolBoostCores, realCores > 0 ? realCores : kAutoCoolBoostCores);
                feature_cores::gBoostCap.store(cap, std::memory_order_relaxed);
                feature_memory::forcePurgeNow();
                gSpikesHandled.fetch_add(1, std::memory_order_relaxed);
                SL_LOGI("auto: stutter detected (frame=%ldms) — boosting", frameUs / 1000);
            }
        } else {
            consecutiveSpikes = 0;
        }
    }
    lastFrameUs = now;

    if (gBoosting.load(std::memory_order_relaxed) && (now - lastSpikeAtUs) > kBoostHoldUs) {
        gBoosting.store(false, std::memory_order_relaxed);
        feature_cores::gBoostCap.store(0, std::memory_order_relaxed);
        SL_LOGI("auto: calm again — boost released");
    }
    return ret;
}

bool install() {
    void* sym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    if (!sym) {
        SL_LOGE("auto: eglSwapBuffers symbol not found — feature unavailable");
        return false;
    }
    gTarget = sym;
    if (pl::memory::hook(gTarget, reinterpret_cast<void*>(&detour),
                          reinterpret_cast<void**>(&gOriginal)) != 0) {
        SL_LOGE("auto: hook install failed");
        gTarget = nullptr;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    SL_LOGI("auto: ready (spike >= %ldms for 2 frames triggers a short boost)", kSpikeThresholdUs / 1000);
    return true;
}

void remove() {
    if (gTarget && gOriginal) {
        pl::memory::unhook(gTarget, reinterpret_cast<void*>(&detour));
    }
    gTarget = nullptr;
    gOriginal = nullptr;
    feature_cores::gBoostCap.store(0, std::memory_order_relaxed);
    gBoosting.store(false, std::memory_order_relaxed);
    gInstalled.store(false, std::memory_order_release);
}

}  // namespace feature_auto

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
    s += " | cpu_cores: ";
    if (feature_cores::gInstalled.load(std::memory_order_relaxed)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ready (real=%ld, reporting=%ld)",
                 feature_cores::gRealCores.load(std::memory_order_relaxed),
                 feature_cores::gReportedCores.load(std::memory_order_relaxed));
        s += buf;
    } else {
        s += "NOT available";
    }
    s += " | memory: ";
    if (feature_memory::gInstalled.load(std::memory_order_relaxed)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "rss=%ldMB purges=%u",
                 feature_memory::gLastRssKb.load(std::memory_order_relaxed) / 1024,
                 feature_memory::gPurgeCount.load(std::memory_order_relaxed));
        s += buf;
    } else {
        s += "NOT available";
    }
    s += " | shader_cache: ";
    if (feature_shadercache::gInstalled.load(std::memory_order_relaxed)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ready (hits=%u misses=%u)",
                 feature_shadercache::gHits.load(std::memory_order_relaxed),
                 feature_shadercache::gMisses.load(std::memory_order_relaxed));
        s += buf;
    } else {
        s += "NOT available";
    }
    s += " | prefetch: ";
    if (feature_prefetch::gInstalled.load(std::memory_order_relaxed)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ready (files_warmed=%u)",
                 feature_prefetch::gFilesWarmed.load(std::memory_order_relaxed));
        s += buf;
    } else {
        s += "NOT available";
    }
    s += " | auto: ";
    if (feature_auto::gInstalled.load(std::memory_order_relaxed)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ready (spikes=%u, boosting=%s)",
                 feature_auto::gSpikesHandled.load(std::memory_order_relaxed),
                 feature_auto::gBoosting.load(std::memory_order_relaxed) ? "yes" : "no");
        s += buf;
    } else {
        s += "NOT available";
    }
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
        {"limit_cpu_cores",
         feature_cores::gInstalled.load(std::memory_order_relaxed)
             ? "Report fewer CPU cores to the game (experimental)"
             : "Report fewer CPU cores — unavailable on this device",
         pl::modmenu::ConfigType::Toggle, "false", "", "", ""},
        {"reported_cores", "Cores to report (only while the toggle above is on)",
         pl::modmenu::ConfigType::SliderInt,
         std::to_string(feature_cores::gReportedCores.load(std::memory_order_relaxed)),
         "2", std::to_string(std::max<long>(2, feature_cores::gRealCores.load(std::memory_order_relaxed))),
         "limit_cpu_cores"},
        {"adaptive_memory",
         feature_memory::gInstalled.load(std::memory_order_relaxed)
             ? "Adaptive memory trim (releases freed RAM when it builds up)"
             : "Adaptive memory trim — unavailable",
         pl::modmenu::ConfigType::Toggle, "true", "", "", ""},
        {"shader_cache",
         feature_shadercache::gInstalled.load(std::memory_order_relaxed)
             ? "Shader cache (skip re-linking shaders seen before)"
             : "Shader cache — unavailable on this device",
         pl::modmenu::ConfigType::Toggle, "true", "", "", ""},
        {"prefetch",
         feature_prefetch::gInstalled.load(std::memory_order_relaxed)
             ? "World file prefetch (warm up the world's save files on load)"
             : "World file prefetch — unavailable",
         pl::modmenu::ConfigType::Toggle, "true", "", "", ""},
        {"auto_sense",
         feature_auto::gInstalled.load(std::memory_order_relaxed)
             ? "Auto-sense stutter (briefly reacts to real frame spikes)"
             : "Auto-sense stutter — unavailable on this device",
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
        } else if (key == "limit_cpu_cores") {
            feature_cores::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("cpu-cores: %s", on ? "ON" : "OFF");
        } else if (key == "adaptive_memory") {
            feature_memory::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("memory: %s", on ? "ON" : "OFF");
        } else if (key == "reported_cores") {
            long v = strtol(std::string(value).c_str(), nullptr, 10);
            if (v >= 1) {
                feature_cores::gReportedCores.store(v, std::memory_order_relaxed);
                SL_LOGI("cpu-cores: reported_cores set to %ld", v);
            }
        } else if (key == "shader_cache") {
            feature_shadercache::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("shader-cache: %s", on ? "ON" : "OFF");
        } else if (key == "prefetch") {
            feature_prefetch::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("prefetch: %s", on ? "ON" : "OFF");
        } else if (key == "auto_sense") {
            feature_auto::gEnabled.store(on, std::memory_order_relaxed);
            SL_LOGI("auto: %s", on ? "ON" : "OFF");
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
        coresReady_ = feature_cores::install();
        memoryReady_ = feature_memory::install();
        shaderCacheReady_ = feature_shadercache::install();
        prefetchReady_ = feature_prefetch::install();
        // feature_auto reads feature_cores::gRealCores and calls
        // feature_memory::forcePurgeNow(), so install it after both.
        autoReady_ = feature_auto::install();
        menuReady_ = menu::registerMenu();
        if (!menuReady_) SL_LOGE("mod menu registration failed; features keep their defaults");
        SL_LOGI("enabled (skip=%d, threads=%d, cores=%d, memory=%d, shader_cache=%d, prefetch=%d, auto=%d)",
                skipReady_, threadsReady_, coresReady_, memoryReady_, shaderCacheReady_, prefetchReady_, autoReady_);
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
        if (coresReady_) {
            feature_cores::remove();
            coresReady_ = false;
        }
        if (memoryReady_) {
            feature_memory::remove();
            memoryReady_ = false;
        }
        // auto first: it reaches into feature_cores/feature_memory, so stop
        // it before tearing those down.
        if (autoReady_) {
            feature_auto::remove();
            autoReady_ = false;
        }
        if (shaderCacheReady_) {
            feature_shadercache::remove();
            shaderCacheReady_ = false;
        }
        if (prefetchReady_) {
            feature_prefetch::remove();
            prefetchReady_ = false;
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
    bool coresReady_{false};
    bool memoryReady_{false};
    bool shaderCacheReady_{false};
    bool prefetchReady_{false};
    bool autoReady_{false};
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
