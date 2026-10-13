#pragma once

// Character packs (native/CHARACTERS.md): another game's character, converted on the user's machine from their own disc
// (native/tools/woc_character.py), drawn in place of the player's model. Twinsanity's own character keeps playing underneath
// (movement, physics, collision, scripts): only the picture changes, its animation picked from what Twinsanity's plays.

#include "common.h"

#include <string>
#include <vector>

struct ChunkLights;
struct GameOGI;
struct InstanceContext;
struct Matrix4x4;
struct OgiAnimator;

namespace NativeCharacters
{
// The packs installed (folders of the settings folder's characters/ with a pack.bin), by folder name
std::vector<std::string> Installed();
// The character chosen: "" Twinsanity's own, else a pack's folder name ($TWIN_CHARACTER wins over the option). Loaded when first
// drawn; a pack that doesn't load is logged once and Twinsanity's own is drawn
const std::string& Choice();
void SetChoice(const std::string& name);
// A pack's name to show (its folder's name made readable when the pack has none)
std::string DisplayName(const std::string& folder);

// The options' preview (ui/settingsview.cpp): a pack's character drawn on the CPU into RGBA pixels (red the low byte, a clear
// background) as large as the picture holds, playing its showcase (idle, run, a spin, a taunt) at a time in seconds, turned by
// yaw degrees. False without the pack
bool RenderPreview(const std::string& folder, f32 seconds, f32 yaw, u32 width, u32 height, std::vector<u32>& pixels);

// Draws the chosen pack's character for the instance, at its matrix in the world and in its lights, when the instance is the
// player's Crash: whether it did (the model's own drawing is skipped then)
bool DrawInstead(InstanceContext* instance, GameOGI* ogi, OgiAnimator* animator, const Matrix4x4* matrix, const ChunkLights* lights,
                 u32 mode);
}
