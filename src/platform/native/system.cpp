// The machine: start-up, the console's language and clock (the PS2's OSD settings), memory, time, the I/O processor, and the
// native build's settings and log
#include "graphics/renderthread.h"
#include "graphics/window.h"
#include "native.h"
#include "platform/io.h"
#include "platform/memory.h"
#include "platform/sound.h"
#include "platform/system.h"
#include "platform/time.h"
#include "retail/libc.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------------------------------------- settings and log
namespace
{
Native::Settings g_Settings = {nullptr, true, false};
std::string g_DiscImageSetting;

std::string SavedDiscImage()
{
    std::ifstream file(Native::SettingsFolder() + "/disc_image.txt");
    std::string path;
    std::getline(file, path);
    return path;
}

// local.json's "disc_image" (the decomp's own settings file, in the working folder)
std::string DiscImageFromLocalJson()
{
    std::ifstream file("local.json");
    if (!file)
    {
        return {};
    }

    std::stringstream text;
    text << file.rdbuf();
    std::string json = text.str();
    size_t key = json.find("\"disc_image\"");
    if (key == std::string::npos)
    {
        return {};
    }

    size_t open = json.find('"', json.find(':', key) + 1);
    size_t close = json.find('"', open + 1);
    return open == std::string::npos || close == std::string::npos ? std::string() : json.substr(open + 1, close - open - 1);
}
}

const Native::Settings& Native::GetSettings()
{
    return g_Settings;
}

void Native::ParseArguments(int& argc, char** argv)
{
    int kept = 1;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--iso") == 0 && i + 1 < argc)
        {
            g_DiscImageSetting = argv[++i];
        }
        else if (std::strcmp(argv[i], "--quiet-stubs") == 0)
        {
            g_Settings.logStubs = false;
        }
        else if (std::strcmp(argv[i], "--snapshots") == 0 && i + 1 < argc)
        {
            Native::SetSnapshotFolder(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            Native::SetQuickStart(true);
        }
        else if (std::strcmp(argv[i], "--headless") == 0)
        {
            g_Settings.headless = true;
        }
        else
        {
            argv[kept++] = argv[i];
        }
    }

    argc = kept;
    argv[argc] = nullptr;
    if (g_DiscImageSetting.empty())
    {
        const char* environment = std::getenv("TWINSANITY_ISO");
        g_DiscImageSetting = environment != nullptr ? environment : DiscImageFromLocalJson();
    }

    if (g_DiscImageSetting.empty())
    {
        g_DiscImageSetting = SavedDiscImage();
    }

    g_Settings.discImage = g_DiscImageSetting.empty() ? nullptr : g_DiscImageSetting.c_str();
}

// The disc image picked before (or now): the app keeps it in its settings (macOS: ~/Library/Application Support/Crash
// Twinsanity/disc_image.txt; elsewhere $XDG_CONFIG_HOME or ~/.config/crash-twinsanity/), and asks for it with the system's file
// dialog the first time it's launched without one
std::string Native::SettingsFolder()
{
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    return std::string(home != nullptr ? home : ".") + "/Library/Application Support/Crash Twinsanity";
#else
    const char* config = std::getenv("XDG_CONFIG_HOME");
    return config != nullptr ? std::string(config) + "/crash-twinsanity"
                             : std::string(home != nullptr ? home : ".") + "/.config/crash-twinsanity";
#endif
}

std::string Native::PickDiscImage()
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        return {};
    }

    struct Pick
    {
        bool done = false;
        std::string path;
    } pick;
    static const SDL_DialogFileFilter filters[] = {{"PS2 disc image", "iso;bin;img"}, {"Any file", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* user, const char* const* files, int)
        {
            auto* result = static_cast<Pick*>(user);
            if (files != nullptr && files[0] != nullptr)
            {
                result->path = files[0];
            }
            result->done = true;
        },
        &pick, nullptr, filters, 2, nullptr, false);
    while (!pick.done)
    {
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 50);
    }

    if (!pick.path.empty())
    {
        std::filesystem::create_directories(SettingsFolder());
        std::ofstream(SettingsFolder() + "/disc_image.txt") << pick.path << '\n';
    }

    return pick.path;
}

void Native::Log(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::fputs("[native] ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
}

void Native::Fatal(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::fputs("[native] fatal: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    std::exit(1);
}

// ---------------------------------------------------------------------------------------------------------- the system
void Platform::System::Initialise()
{
    const char* image = Native::GetSettings().discImage;
    static std::string picked;
    if (image == nullptr)
    {
        picked = Native::PickDiscImage();
        image = picked.empty() ? nullptr : picked.c_str();
    }

    if (image == nullptr)
    {
        Native::Fatal("no disc image: give --iso PATH (your PAL Crash Twinsanity image), set TWINSANITY_ISO, or put "
                      "\"disc_image\" in local.json");
    }

    if (!Native::Disc::Open(image))
    {
        Native::Fatal("can't read %s as a disc image", image);
    }

    Native::Disc::Entry executable;
    if (!Native::Disc::Find("SLES_525.68", &executable))
    {
        Native::Fatal("%s isn't the PAL Crash Twinsanity disc (no SLES_525.68 on it)", image);
    }

    Native::Log("disc image %s", image);
    Native::LogMovieLibraries();

    // The window the game is shown in (4:3 like the PS2's TV picture); the graphics make their context on it
    if (!Native::GetSettings().headless)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        {
            Native::Fatal("SDL's video didn't start: %s", SDL_GetError());
        }

        SDL_Window* window = SDL_CreateWindow("Crash Twinsanity", 1280, 960,
                                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window == nullptr)
        {
            Native::Fatal("the window couldn't be made: %s", SDL_GetError());
        }

        NativeGraphicsAttachWindow(window);
    }
}

void Platform::System::StartServices()
{
    Platform::Sound::InitialiseRemote();
}

void Platform::System::Exit(s32 status)
{
    // The render thread's work done and its context let go before SDL goes
    NativeGraphics::StopRenderThread();
    SDL_Quit();
    std::exit(status);
}

// The console's language is the machine's: the user's first preferred language the game has, English otherwise (the PS2's
// OSD setting, which the game goes by the same way)
Platform::System::ConsoleLanguage Platform::System::Language()
{
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    ConsoleLanguage language = LanguageEnglish;
    for (int i = 0; locales != nullptr && i < count; i++)
    {
        const char* code = locales[i]->language;
        ConsoleLanguage found = std::strcmp(code, "en") == 0   ? LanguageEnglish
                                : std::strcmp(code, "fr") == 0 ? LanguageFrench
                                : std::strcmp(code, "es") == 0 ? LanguageSpanish
                                : std::strcmp(code, "de") == 0 ? LanguageGerman
                                : std::strcmp(code, "it") == 0 ? LanguageItalian
                                : std::strcmp(code, "nl") == 0 ? LanguageDutch
                                : std::strcmp(code, "pt") == 0 ? LanguagePortuguese
                                                               : LanguageJapanese;
        if (found != LanguageJapanese)
        {
            language = found;
            break;
        }
    }

    SDL_free(locales);
    return language;
}

Platform::System::DateTime Platform::System::LocalTime()
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return {static_cast<u16>(local.tm_year + 1900), static_cast<u8>(local.tm_mon + 1), static_cast<u8>(local.tm_mday),
            static_cast<u8>(local.tm_hour),       static_cast<u8>(local.tm_min),     static_cast<u8>(local.tm_sec)};
}

// ---------------------------------------------------------------------------------------------------------- memory
namespace
{
// The PS2 gave the game's pools what its 32 MB left past the executable and the C library's reserve (about 28 MB). Natively
// structs that hold pointers are bigger, so the pools get twice that (native/NATIVE.md)
constexpr u32 Ps2PoolSpace = 0x2000000 - (0x3DDA00 + 0x2800);
constexpr u32 NativePoolScale = 4;
}

u32 Platform::Memory::PoolSpace()
{
    return Ps2PoolSpace * NativePoolScale;
}

void* Platform::Memory::AllocatePool(u32 size)
{
    return RetailLibc::Malloc(size);
}

void Platform::Memory::Synchronise()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void Platform::Memory::BeforeDeviceWrite(void*, u32)
{
}

void Platform::Memory::WriteBackCache()
{
}

// The retail C library's malloc and free (Sony's dlmalloc on sbrk on the PS2): the host's, 16 byte aligned like the PS2's
// quadwords. The game's own heap manager (game/memory.cpp) works inside what AllocatePool gives it, as on the PS2
void* RetailLibc::Malloc(u32 size)
{
    void* memory = nullptr;
    return posix_memalign(&memory, 16, size != 0 ? size : 16) == 0 ? memory : nullptr;
}

void RetailLibc::Free(void* memory)
{
    std::free(memory);
}

void* RetailLibc::Sbrk(s32)
{
    // Only the PS2's test runs move the break (AllocatePool): the native build has none
    return reinterpret_cast<void*>(-1);
}

// ---------------------------------------------------------------------------------------------------------- time
namespace
{
std::chrono::steady_clock::time_point g_Start;
}

void Platform::Time::Initialise()
{
    g_Start = std::chrono::steady_clock::now();
}

// The PS2's timer 0 at the bus clock / 256 (576000 a second)
u64 Platform::Time::Ticks()
{
    auto elapsed = std::chrono::steady_clock::now() - g_Start;
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) * TicksPerSecond /
           1000000000ull;
}

// ---------------------------------------------------------------------------------------------------------- the I/O processor
// Its modules and heap: the native build's services need none of it
void Platform::Io::Initialise()
{
}

void Platform::Io::Restart(const char*)
{
}

s32 Platform::Io::LoadDriver(const char*)
{
    return 0;
}

void Platform::Io::InitialiseHeap()
{
}

void* Platform::Io::AllocateHeap(s32 size)
{
    return std::malloc(static_cast<size_t>(size));
}

s32 Platform::Io::FreeHeap(void* address)
{
    std::free(address);
    return 0;
}

// The PS2 side's test hook (src/platform/ps2/pads.cpp): buttons held for tools/run_pcsx2.py's --press. Nothing sets it natively
extern "C" volatile u32 g_TestPadButtons = 0;

// The retail C library's way out and the debugger's console (deci2's kputs), which the game's printf goes to a line at a time
extern "C"
{
    [[noreturn]] void ProgramExit(int status)
    {
        Platform::System::Exit(status);
    }

    int kputs(char* text)
    {
        std::fputs(text, stdout);
        std::fflush(stdout);
        return 0;
    }
}
