// --quick (testing): straight to playing the beach the way tools/run_pcsx2.py's --quick does on the PS2 build, through the game
// controller's next state alone (no input): past the logos, the title's cutscene (New Game, as the main menu asks) and the
// intro movie, and no level title card once playing. Checked once a frame (Platform::Pads::Read)
// --level CHUNK (Twinsanity Studio's play button): the game started in a level's chunk (Levels\Earth\Hub\beach) at the first
// logo, as the game's own command-line start does (the progress's start chunk, a saved game's entry, the save controller taking
// the progress, the level loaded). A chunk with no character in it (a hub's part, a tunnel: only entered from a neighbour) goes
// back to the title rather than playing with none
#include "game/gamecontroller.h"
#include "game/string.h"
#include "native.h"

#include <string>

namespace
{
bool g_Quick = false;
std::string g_StartChunk;
bool g_Started = false;

void StartChunkFrame(GameController* controller)
{
    u32 state = controller->State();
    if (!g_Started && (state == GameController::StateVivendiLogo || state == GameController::StateTravellersTalesLogo))
    {
        g_Started = true;
        StringAssign(&controller->progress.startChunk, g_StartChunk.c_str());
        controller->states.entry = EntrySaved;
        controller->saveController.Take(&controller->progress, controller->chunkManager, nullptr);
        controller->SetNextState(GameController::StateLoadingLevel);
        Native::Log("level: starting in %s", g_StartChunk.c_str());
        return;
    }

    if (g_Started && controller->NextState() == GameController::StateStartingPlay)
    {
        bool any = false;
        for (u32 character = 0; character < GameProgress::Characters; character++)
        {
            any = any || controller->progress.Instance(character) != nullptr;
        }

        if (!any)
        {
            Native::Log("level: %s has no character in it (it's entered from a neighbouring chunk): back to the title",
                        g_StartChunk.c_str());
            controller->SetNextState(GameController::StateRestartingTitle);
        }

        g_StartChunk.clear();
    }
}
}

namespace Native
{
void SetQuickStart(bool quick)
{
    g_Quick = quick;
}

void SetStartChunk(const char* chunk)
{
    g_StartChunk = chunk;
}

void QuickStartFrame()
{
    GameController* controller = G_GameController;
    if (controller != nullptr && !g_StartChunk.empty())
    {
        StartChunkFrame(controller);
        return;
    }

    if (!g_Quick || controller == nullptr)
    {
        return;
    }

    switch (controller->State())
    {
    case GameController::StateVivendiLogo:
        controller->SetNextState(GameController::StateLoadingTitle);
        break;
    case GameController::StateTitle:
        controller->SetNextState(GameController::StateNewGame);
        break;
    case GameController::StateMovie:
        controller->SetNextState(GameController::StateStartingPlay);
        break;
    case GameController::StatePlaying:
        controller->hudDelay = 0;
        break;
    default:
        break;
    }
}
}
