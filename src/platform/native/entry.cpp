// The native build's entry: its own arguments taken out, the retail data's pointers fixed up, then the game's Main as the
// PS2's entry calls it
#include "native.h"
#include "platform/saves.h"
#include "platform/system.h"
#include "ui/nativeui.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>
#include <string_view>

extern "C"
{
    int Main(u32 argc, char** argv);
    // build/native/data.cpp (native/tools/convert_data.py)
    void NativeApplyDataFixups();
}

namespace Native
{
void StartLowMemory();
}

namespace
{
// --selftest-lowmemory: the retail code's kinds of low accesses done once each, which must go on with the PS2's results
int SelfTestLowMemory()
{
    g_NativeLowMemory[0x10] = 0x5A;
    g_NativeLowMemory[0x18] = 0x80;
    volatile u8* null = nullptr;
    u8 byte = null[0x10];
    s8 signedByte = static_cast<s8>(null[0x18]);
    volatile u32* words = reinterpret_cast<volatile u32*>(static_cast<uiptr>(0x40));
    words[1] = 0x12345678;
    u32 word = words[1];
    volatile u64* pair = reinterpret_cast<volatile u64*>(static_cast<uiptr>(0x80));
    pair[0] = 1;
    pair[1] = 2;
    u64 sum = pair[0] + pair[1];
    volatile float* f = reinterpret_cast<volatile float*>(static_cast<uiptr>(0x100));
    f[0] = 1.5f;
    float back = f[0];
    bool ok = byte == 0x5A && signedByte == -128 && word == 0x12345678 && sum == 3 && back == 1.5f;
    Native::Log("low memory self-test: %s (byte %#x, signed %d, word %#x, sum %llu, float %g)", ok ? "passed" : "FAILED", byte,
                signedByte, word, static_cast<unsigned long long>(sum), back);
    return ok ? 0 : 1;
}
}

// --selftest-saves: the game's own save manager (src/platform/ps2/saves.cpp) through the native memory card: a save made, a file
// written, found and read back, then the test's save removed
int SelfTestSaves()
{
    using namespace Platform::Saves;
    auto run = [](Operation operation)
    {
        StorageInfo info;
        for (int frames = 0; frames < 100; frames++)
        {
            if (Update(0, 0, &info) == Operation::None && GetResult(operation).succeeded != -1)
            {
                return GetResult(operation);
            }
        }
        return Result{-1, -1, -1};
    };
    StorageInfo info{};
    bool ready = Initialise("BE", "SLES-52568");
    for (int i = 0; i < 3; i++)
    {
        Update(0, 0, &info);
    }

    const char* save = "SELFTEST";
    const char* file = "TEST.DAT";
    char data[300];
    for (u32 i = 0; i < sizeof(data); i++)
    {
        data[i] = static_cast<char>(i * 7);
    }

    bool created = CreateSave(0, 0, save) && run(Operation::CreateSave).succeeded == 1;
    bool written = Write(0, 0, save, file, data, sizeof(data)) && run(Operation::Write).succeeded == 1;
    s32 size = -2;
    bool found = FindFile(0, 0, file, save, &size, nullptr) && run(Operation::FindFile).succeeded == 1;
    char back[300] = {};
    bool read = Read(0, 0, save, file, back, sizeof(back)) && run(Operation::Read).succeeded == 1;
    bool same = std::memcmp(data, back, sizeof(data)) == 0;
    std::filesystem::remove_all(std::filesystem::path(Native::SettingsFolder()) / "memorycard" / "BESLES-52568SELFTEST");
    bool ok = ready && info.type == StorageMemoryCard && created && written && found && size == sizeof(data) && read && same;
    Native::Log("saves self-test: %s (card type %d, %d KB free, created %d, written %d, found %d size %d, read %d, same %d)",
                ok ? "passed" : "FAILED", info.type, info.freeKilobytes, created, written, found, size, read, same);
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    Native::ParseArguments(argc, argv);
    if (Native::GetSettings().headless)
    {
        // A headless run (the tests) shows nothing: no Dock icon, no menu bar, never the active app
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    }
    Native::StartLowMemory();
    for (int i = 1; i < argc; i++)
    {
        if (std::string_view(argv[i]) == "--selftest-lowmemory")
        {
            return SelfTestLowMemory();
        }
        if (std::string_view(argv[i]) == "--selftest-saves")
        {
            return SelfTestSaves();
        }
        // --selftest-ui, --dump-texts (src/platform/native/ui/commands.cpp)
        if (int status = 0; NativeUi::RunCommand(argv[i], &status))
        {
            return status;
        }
    }

    NativeApplyDataFixups();
    Platform::System::Exit(Main(static_cast<u32>(argc), argv));
}
