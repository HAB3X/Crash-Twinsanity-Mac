// The level sweep (testing, native/LEVEL_SWEEP.md): $TWIN_SWEEP_LEVEL=Levels\Earth\Hub\Beach starts the game in that chunk the way
// the game's own command-line start does (the travellers' tales logo's branch: the progress's start chunk, a saved game's entry,
// the save controller taking the progress, the level loaded), then plays it with scripted input for $TWIN_SWEEP_SECONDS (30) of
// play and exits 0. The states it went through are logged ("sweep:"); it exits 3 when play hasn't started in $TWIN_SWEEP_TIMEOUT
// (180) seconds. The input: the left stick turned round a little each cycle, cross and square now and then, any held button
// skipping the cutscenes ("hold any button to skip": cross is held 1 s each cycle)
#include "ui/bindings.h"

#include "native.h"

#include "game/gamecontroller.h"
#include "game/string.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace NativeUi
{
namespace
{
using Clock = std::chrono::steady_clock;

bool g_Read = false;
std::string g_Level;
f32 g_Seconds = 30.0f;
f32 g_Timeout = 180.0f;
bool g_Warped = false;
u32 g_LastState = ~0u;
Clock::time_point g_Start;
Clock::time_point g_PlayStart;
bool g_Playing = false;
f32 g_Played = 0.0f;
Clock::time_point g_LastFrame;
u32 g_Frames = 0;

void ReadSettings()
{
    if (g_Read)
    {
        return;
    }

    g_Read = true;
    const char* level = std::getenv("TWIN_SWEEP_LEVEL");
    g_Level = level != nullptr ? level : "";
    if (const char* seconds = std::getenv("TWIN_SWEEP_SECONDS"))
    {
        g_Seconds = static_cast<f32>(std::atof(seconds));
    }

    if (const char* timeout = std::getenv("TWIN_SWEEP_TIMEOUT"))
    {
        g_Timeout = static_cast<f32>(std::atof(timeout));
    }

    g_Start = Clock::now();
    g_LastFrame = g_Start;
}

[[noreturn]] void Finish(int status, const char* why)
{
    Native::Log("sweep: %s: %s (%.1f s played, %u frames)", g_Level.c_str(), why, g_Played, g_Frames);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(status);
}
}

bool SweepActive()
{
    ReadSettings();
    return !g_Level.empty();
}

void SweepFrame()
{
    if (!SweepActive())
    {
        return;
    }

    GameController* controller = G_GameController;
    if (controller == nullptr)
    {
        return;
    }

    Clock::time_point now = Clock::now();
    f32 delta = std::chrono::duration<f32>(now - g_LastFrame).count();
    g_LastFrame = now;
    u32 state = controller->State();
    if (state != g_LastState)
    {
        Native::Log("sweep: state %u at %.1f s", state, std::chrono::duration<f32>(now - g_Start).count());
        g_LastState = state;
    }

    // At the first logo: the level, as the command line's start chunk
    if (!g_Warped && (state == GameController::StateVivendiLogo || state == GameController::StateTravellersTalesLogo))
    {
        g_Warped = true;
        StringAssign(&controller->progress.startChunk, g_Level.c_str());
        controller->states.entry = EntrySaved;
        controller->saveController.Take(&controller->progress, controller->chunkManager, nullptr);
        controller->SetNextState(GameController::StateLoadingLevel);
        Native::Log("sweep: loading %s", g_Level.c_str());
        return;
    }

    // A chunk with no character's instance in it (a hub's part or a tunnel, only ever entered from a neighbour, whose links bring
    // the characters): play would start with none (FollowCameraRig::Restart through a null vtable)
    if (controller->NextState() == GameController::StateStartingPlay)
    {
        bool any = false;
        for (u32 character = 0; character < GameProgress::Characters; character++)
        {
            any = any || controller->progress.Instance(character) != nullptr;
        }

        if (!any)
        {
            Finish(4, "no character in the chunk");
        }
    }

    // Play counted while the game runs (playing, watching a cutscene or a movie)
    bool running = state == GameController::StatePlaying || state == GameController::StateWatching ||
                   state == GameController::StateMovie;
    if (running)
    {
        if (!g_Playing)
        {
            g_Playing = true;
            g_PlayStart = now;
        }

        g_Frames++;
        g_Played += std::min(delta, 0.25f);
        controller->hudDelay = 0;
        if (g_Played >= g_Seconds)
        {
            Finish(0, "ran");
        }
    }
    else if (state == GameController::StatePaused)
    {
        // Out of a pause the scripted start may have made
        controller->SetNextState(GameController::StatePlaying);
    }

    if (!g_Playing && std::chrono::duration<f32>(now - g_Start).count() > g_Timeout)
    {
        char why[64];
        std::snprintf(why, sizeof(why), "never played (state %u)", state);
        Finish(3, why);
    }
}

void SweepInput(BoundPad* pad)
{
    if (!SweepActive() || !g_Playing)
    {
        return;
    }

    // A 4 s cycle: the stick pushed in a direction that turns 100 degrees a cycle, cross held for the first second (a jump, and
    // the cutscenes' skip), square tapped at 2.5 s
    f32 t = g_Played;
    s32 cycle = static_cast<s32>(t / 4.0f);
    f32 inCycle = t - static_cast<f32>(cycle) * 4.0f;
    f32 angle = static_cast<f32>(cycle) * 1.745f;
    pad->leftX = static_cast<u8>(128.0f + std::sin(angle) * 127.0f);
    pad->leftY = static_cast<u8>(128.0f - std::cos(angle) * 127.0f);
    if (inCycle < 1.0f)
    {
        pad->buttons |= PadBitCross;
    }

    if (inCycle > 2.5f && inCycle < 2.7f)
    {
        pad->buttons |= PadBitSquare;
    }
}
}
