// Character packs drawn (characters.h, native/CHARACTERS.md). A pack (native/tools/woc_character.py) has a skinned model (its
// materials' vertexes with four joints and weights each, its triangles), its textures (RGBA) and its animations (every joint's
// skinning matrix a frame). It's drawn as the PS2 renderer draws: its triangles skinned, lit and projected here, then sent to the
// GS in the frame's chain (a CALL in the player's skin's bucket of a chain of our own: VIF1 DIRECT of GIF packets), its textures
// uploaded into the renderer's slots of GS memory (ClaimTextureSlot) like the game's. It shares the frame's depth buffer, its
// view and its lights; nothing goes past the emulated hardware.

#include "characters.h"

#include "native.h"
#include "ui/nativeui.h"
#include "renderer/gsregs.h"
#include "renderer/renderer.h"

#include "game/animation.h"
#include "game/characters.h"
#include "game/clock.h"
#include "game/controllers.h"
#include "game/gamecontroller.h"
#include "game/instances.h"
#include "game/lights.h"
#include "game/objectnode.h"
#include "game/objects.h"
#include "game/view.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace
{
namespace fs = std::filesystem;

constexpr u32 PackVersion = 1;
// The WoC animations' frames a second
constexpr f32 ClipRate = 30.0f;
// How long a new clip blends in from the pose the last one left (seconds)
constexpr f32 BlendSeconds = 0.12f;

struct alignas(16) Quad
{
    u32 w[4];
};

struct PackVertex
{
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    u8 joints[4];
    f32 weights[4];
};
static_assert(sizeof(PackVertex) == 52);

struct PackMaterial
{
    s32 texture;
    bool blends;
    f32 colour[4];
    std::vector<PackVertex> vertices;
    std::vector<u16> triangles;
};

// A pack's texture as the GS takes it: 32 bit pixels (alpha 0x80 opaque) in an IMAGE transfer, and a Texture of the renderer's
// for its slot (only the slot, its width and its size powers are used)
constexpr u32 MostTextureSide = 512;
// A ring slot's size (renderer/textures.cpp's 64 KB slots); a texture past it takes the large slot
constexpr size_t SlotBytes = 0x10000;

struct PackTexture
{
    u32 width;
    u32 height;
    std::vector<Quad> pixels;
    std::unique_ptr<Texture> slotOwner;
};

// A skinning matrix: four rows of three (row vectors: x, y and z axes, then the translation)
struct Affine
{
    f32 m[4][3];
};

struct PackClip
{
    std::string name;
    u32 frames;
    f32 rate;
    std::vector<Affine> matrices;
    // How fast the ground goes by under its feet (model units a second; 0 for a clip that doesn't step): two steps a cycle, each
    // as long as a foot's travel along the model's forward axis
    f32 stride = 0.0f;
};

struct Pack
{
    std::string folder;
    u32 jointCount = 0;
    f32 low[3];
    f32 high[3];
    std::vector<PackTexture> textures;
    std::vector<PackMaterial> materials;
    std::vector<PackClip> clips;
    // Each joint's place in the bind pose (model space): the point its skinning matrices turn about
    std::vector<std::array<f32, 3>> pivots;
    std::map<std::string, u32> clipIndex;
};

template <typename T>
bool Take(const std::vector<u8>& data, size_t& at, T* out, size_t count = 1)
{
    if (at + sizeof(T) * count > data.size())
    {
        return false;
    }

    std::memcpy(out, data.data() + at, sizeof(T) * count);
    at += sizeof(T) * count;
    return true;
}

fs::path CharactersFolder()
{
    return fs::path(Native::SettingsFolder()) / "characters";
}

// The feet: the two joints lowest in the bind pose
void StrideOf(const Pack& pack, PackClip& clip)
{
    if (pack.pivots.size() < 2 || clip.frames < 8)
    {
        return;
    }

    u32 feet[2] = {0, 1};
    for (u32 j = 0; j < pack.pivots.size(); j++)
    {
        if (pack.pivots[j][1] < pack.pivots[feet[0]][1])
        {
            feet[1] = feet[0];
            feet[0] = j;
        }
        else if (j != feet[0] && pack.pivots[j][1] < pack.pivots[feet[1]][1])
        {
            feet[1] = j;
        }
    }

    f32 longest = 0.0f;
    for (u32 foot : feet)
    {
        const f32* p = pack.pivots[foot].data();
        f32 low = 1e9f;
        f32 high = -1e9f;
        for (u32 f = 0; f < clip.frames; f++)
        {
            const Affine& m = clip.matrices[f * pack.jointCount + foot];
            f32 z = p[0] * m.m[0][2] + p[1] * m.m[1][2] + p[2] * m.m[2][2] + m.m[3][2];
            low = std::min(low, z);
            high = std::max(high, z);
        }

        longest = std::max(longest, high - low);
    }

    f32 seconds = static_cast<f32>(clip.frames) / clip.rate;
    clip.stride = longest > 0.1f ? 2.0f * longest / seconds : 0.0f;
}

std::unique_ptr<Pack> LoadPack(const std::string& folder)
{
    fs::path path = CharactersFolder() / folder / "pack.bin";
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        Native::Log("characters: no pack at %s", path.c_str());
        return nullptr;
    }

    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto pack = std::make_unique<Pack>();
    pack->folder = folder;
    size_t at = 0;
    char magic[4];
    u32 header[5];
    if (!Take(data, at, magic, 4) || std::memcmp(magic, "TWCP", 4) != 0 || !Take(data, at, header, 5) ||
        header[0] != PackVersion)
    {
        Native::Log("characters: %s isn't a version %u pack", path.c_str(), PackVersion);
        return nullptr;
    }

    pack->jointCount = header[1];
    u32 textureCount = header[2];
    u32 materialCount = header[3];
    u32 clipCount = header[4];
    Take(data, at, pack->low, 3);
    Take(data, at, pack->high, 3);
    for (u32 j = 0; j < pack->jointCount && 64 + (j + 1) * 84 <= data.size(); j++)
    {
        // The inverse bind matrix (4 rows of 3, after the name and the parent): the joint's place is where it takes to the
        // origin, minus its translation turned back (its rotation's inverse is its transpose)
        f32 m[12];
        std::memcpy(m, data.data() + 64 + j * 84 + 36, sizeof(m));
        std::array<f32, 3> place{};
        for (u32 c = 0; c < 3; c++)
        {
            place[c] = -(m[9] * m[c * 3 + 0] + m[10] * m[c * 3 + 1] + m[11] * m[c * 3 + 2]);
        }

        pack->pivots.push_back(place);
    }

    at = 64 + static_cast<size_t>(pack->jointCount) * 84;
    bool good = at <= data.size();
    for (u32 i = 0; good && i < textureCount; i++)
    {
        PackTexture texture;
        u16 size[2];
        good = Take(data, at, size, 2);
        texture.width = size[0];
        texture.height = size[1];
        size_t bytes = static_cast<size_t>(texture.width) * texture.height * 4;
        // Powers of two from 8 to 512 a side: up to 64 KB in one of the renderer's ring slots of GS memory, bigger ones (up to 1 MB,
        // 512 x 512) in its large textures' slot (1.25 MB)
        good = good && at + bytes <= data.size() && std::has_single_bit(texture.width) && std::has_single_bit(texture.height) &&
               texture.width >= 8 && texture.height >= 8 && texture.width <= MostTextureSide && texture.height <= MostTextureSide;
        if (!good)
        {
            break;
        }

        texture.pixels.resize((bytes + 15) / 16);
        auto* pixels = reinterpret_cast<u8*>(texture.pixels.data());
        std::memcpy(pixels, data.data() + at, bytes);
        for (size_t p = 3; p < bytes; p += 4)
        {
            pixels[p] = static_cast<u8>((pixels[p] + 1) / 2);
        }

        at += bytes;
        texture.slotOwner = std::make_unique<Texture>();
        std::memset(texture.slotOwner.get(), 0, sizeof(Texture));
        texture.slotOwner->levels = 1;
        texture.slotOwner->widthPower = static_cast<u16>(std::countr_zero(texture.width));
        texture.slotOwner->heightPower = static_cast<u16>(std::countr_zero(texture.height));
        texture.slotOwner->levelWidths[0] = std::max(1u, texture.width / 64);
        pack->textures.push_back(std::move(texture));
    }

    for (u32 i = 0; good && i < materialCount; i++)
    {
        PackMaterial material;
        u32 flags = 0;
        u32 counts[2] = {};
        good = Take(data, at, &material.texture) && Take(data, at, &flags) && Take(data, at, material.colour, 4) &&
               Take(data, at, counts, 2);
        material.blends = (flags & 1) != 0;
        material.vertices.resize(counts[0]);
        material.triangles.resize(static_cast<size_t>(counts[1]) * 3);
        good = good && Take(data, at, material.vertices.data(), counts[0]) &&
               Take(data, at, material.triangles.data(), material.triangles.size());
        for (u16 index : material.triangles)
        {
            good = good && index < counts[0];
        }

        for (const PackVertex& vertex : material.vertices)
        {
            for (u8 joint : vertex.joints)
            {
                good = good && joint < pack->jointCount;
            }
        }

        good = good && material.texture < static_cast<s32>(pack->textures.size());
        pack->materials.push_back(std::move(material));
    }

    for (u32 i = 0; good && i < clipCount; i++)
    {
        PackClip clip;
        char name[32];
        good = Take(data, at, name, 32) && Take(data, at, &clip.frames) && Take(data, at, &clip.rate) && clip.frames != 0;
        name[31] = 0;
        clip.name = name;
        clip.matrices.resize(static_cast<size_t>(clip.frames) * pack->jointCount);
        good = good && Take(data, at, clip.matrices.data(), clip.matrices.size());
        pack->clipIndex[clip.name] = static_cast<u32>(pack->clips.size());
        StrideOf(*pack, clip);
        pack->clips.push_back(std::move(clip));
    }

    if (!good || pack->clips.empty())
    {
        Native::Log("characters: %s is damaged", path.c_str());
        return nullptr;
    }

    Native::Log("characters: %s loaded: %u joints, %zu materials, %zu textures, %zu animations", folder.c_str(), pack->jointCount,
                pack->materials.size(), pack->textures.size(), pack->clips.size());
    return pack;
}

std::string g_Choice;
std::unique_ptr<Pack> g_Pack;
std::string g_FailedFolder;

// A pack loaded (one at a time: the one chosen, or the one the options preview)
Pack* PackFor(const std::string& choice)
{
    if (choice.empty())
    {
        return nullptr;
    }

    if (g_Pack != nullptr && g_Pack->folder == choice)
    {
        return g_Pack.get();
    }

    if (g_FailedFolder == choice)
    {
        return nullptr;
    }

    g_Pack = LoadPack(choice);
    if (g_Pack == nullptr)
    {
        g_FailedFolder = choice;
    }

    return g_Pack.get();
}

Pack* ChosenPack()
{
    return PackFor(NativeCharacters::Choice());
}

// ---------------------------------------------------------------------------------------------------------------------------
// Which animation: Twinsanity's playing one (the root's newest main status, by weight) found among the object's slots

// What a Twinsanity animation slot of Crash's object shows as (native/CHARACTERS.md, "The mapping"): the WoC clip, how it plays
// (looped at its own speed, once and held, or fitted to the length of Twinsanity's), and its speed
enum ClipTiming : u8
{
    Loop,
    Once,
    Fit,
};

struct SlotClip
{
    s16 slot;
    const char* clip;
    ClipTiming timing;
    f32 speed;
    f32 turns;
};

#include "characters_mapping.inc"

struct Playing
{
    s32 slot = -2;
    const AnimationStatus* status = nullptr;
    s32 statusTime = 0;
    s32 began = 0;
    s32 now = 0;
    bool clocked = false;
    // The character's place last frame (the world's units) and its smoothed speed over the ground (the pack's units)
    f32 place[3] = {};
    bool hasPlace = false;
    f32 groundSpeed = 0.0f;
    u32 clip = 0;
    ClipTiming timing = Loop;
    f32 speed = 1.0f;
    f32 turns = 0.0f;
    f32 time = 0.0f;
    // The turn about the up axis this frame (degrees)
    f32 yaw = 0.0f;
    // The pose blended from as a new clip starts, and how far in (1: done)
    std::vector<Affine> from;
    f32 blend = 1.0f;
    std::vector<Affine> pose;
};

Playing g_Playing;

const AnimationStatus* NewestStatus(const OgiAnimator* animator)
{
    if (animator == nullptr || animator->root == nullptr || animator->root->chain == nullptr)
    {
        return nullptr;
    }

    const AnimationStatus* best = nullptr;
    for (const AnimationStatus* status = animator->root->chain->main; status != nullptr; status = status->next)
    {
        if (status->main != nullptr && (best == nullptr || status->weight >= best->weight))
        {
            best = status;
        }
    }

    return best;
}

const GameAnimation* AnimationOf(const AnimationStatus* status)
{
    if (status == nullptr || status->main == nullptr || status->main->information == nullptr)
    {
        return nullptr;
    }

    auto* information = reinterpret_cast<const u8*>(status->main->information);
    return reinterpret_cast<const GameAnimation*>(information - offsetof(GameAnimation, main));
}

s32 SlotOf(const InstanceContext* instance, const GameAnimation* animation)
{
    if (animation == nullptr)
    {
        return -1;
    }

    auto* node = static_cast<ObjectNode*>(GetGameNode(const_cast<NodeList*>(&instance->nodes), NodeObject));
    const GameObject* object = node != nullptr ? node->object : nullptr;
    if (object == nullptr)
    {
        return -1;
    }

    for (u32 slot = 0; slot < static_cast<u32>(object->animations.count); slot++)
    {
        if ((object->animations.items[slot] & ResourceIndexMask) == (animation->id & ResourceIndexMask))
        {
            return static_cast<s32>(slot);
        }
    }

    return -1;
}

const SlotClip* MappingOf(s32 slot)
{
    for (const SlotClip& entry : g_SlotClips)
    {
        if (entry.slot == slot)
        {
            return &entry;
        }
    }

    return &g_DefaultClip;
}

// The first of a mapping's clips ("a|b": b when the pack has no a) the pack has, else the default's, else its first
u32 ClipNamed(const Pack& pack, const char* names)
{
    std::string list = names;
    for (size_t at = 0; at <= list.size();)
    {
        size_t end = list.find('|', at);
        end = end == std::string::npos ? list.size() : end;
        auto found = pack.clipIndex.find(list.substr(at, end - at));
        if (found != pack.clipIndex.end())
        {
            return found->second;
        }

        at = end + 1;
    }

    auto found = pack.clipIndex.find(g_DefaultClip.clip);
    return found != pack.clipIndex.end() ? found->second : 0;
}

// A skinning matrix as a rotation (a unit quaternion), the scales of its axes and its translation
struct Parts
{
    f32 q[4];
    f32 scale[3];
    f32 move[3];
};

Parts Split(const Affine& a)
{
    Parts parts;
    f32 r[3][3];
    for (u32 row = 0; row < 3; row++)
    {
        f32 length = std::sqrt(a.m[row][0] * a.m[row][0] + a.m[row][1] * a.m[row][1] + a.m[row][2] * a.m[row][2]);
        parts.scale[row] = length;
        for (u32 c = 0; c < 3; c++)
        {
            r[row][c] = length > 1e-8f ? a.m[row][c] / length : (row == c ? 1.0f : 0.0f);
        }

        parts.move[row] = a.m[3][row];
    }

    // The rotation's quaternion (row vectors: r is the transpose of the column form)
    f32 trace = r[0][0] + r[1][1] + r[2][2];
    f32* q = parts.q;
    if (trace > 0.0f)
    {
        f32 k = 0.5f / std::sqrt(trace + 1.0f);
        q[3] = 0.25f / k;
        q[0] = (r[1][2] - r[2][1]) * k;
        q[1] = (r[2][0] - r[0][2]) * k;
        q[2] = (r[0][1] - r[1][0]) * k;
    }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
    {
        f32 k = 2.0f * std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]);
        q[3] = (r[1][2] - r[2][1]) / k;
        q[0] = 0.25f * k;
        q[1] = (r[1][0] + r[0][1]) / k;
        q[2] = (r[2][0] + r[0][2]) / k;
    }
    else if (r[1][1] > r[2][2])
    {
        f32 k = 2.0f * std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]);
        q[3] = (r[2][0] - r[0][2]) / k;
        q[0] = (r[1][0] + r[0][1]) / k;
        q[1] = 0.25f * k;
        q[2] = (r[2][1] + r[1][2]) / k;
    }
    else
    {
        f32 k = 2.0f * std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]);
        q[3] = (r[0][1] - r[1][0]) / k;
        q[0] = (r[2][0] + r[0][2]) / k;
        q[1] = (r[2][1] + r[1][2]) / k;
        q[2] = 0.25f * k;
    }

    return parts;
}

Affine Join(const Parts& parts)
{
    const f32* q = parts.q;
    f32 x = q[0], y = q[1], z = q[2], w = q[3];
    f32 r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
                   {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
                   {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
    Affine a;
    for (u32 row = 0; row < 3; row++)
    {
        for (u32 c = 0; c < 3; c++)
        {
            a.m[row][c] = r[row][c] * parts.scale[row];
        }

        a.m[3][row] = parts.move[row];
    }

    return a;
}

// Between two skinning matrices: their rotations the shorter way round (normalised), their scales and translations straight.
// A straight blend of the matrices shrinks the model where joints turn fast (a somersault's half a turn in a frame or two)
void Lerp(const Affine& a, const Affine& b, f32 share, Affine* out, const f32* pivot = nullptr)
{
    Parts from = Split(a);
    Parts to = Split(b);
    // About the joint's own place: where it is moves straight, so a joint turning fast keeps its place in the chain
    f32 at[2][3] = {};
    if (pivot != nullptr)
    {
        for (u32 c = 0; c < 3; c++)
        {
            at[0][c] = pivot[0] * a.m[0][c] + pivot[1] * a.m[1][c] + pivot[2] * a.m[2][c] + a.m[3][c];
            at[1][c] = pivot[0] * b.m[0][c] + pivot[1] * b.m[1][c] + pivot[2] * b.m[2][c] + b.m[3][c];
        }
    }

    f32 dot = from.q[0] * to.q[0] + from.q[1] * to.q[1] + from.q[2] * to.q[2] + from.q[3] * to.q[3];
    f32 sign = dot < 0.0f ? -1.0f : 1.0f;
    Parts mixed;
    f32 length = 0.0f;
    for (u32 k = 0; k < 4; k++)
    {
        mixed.q[k] = from.q[k] + (to.q[k] * sign - from.q[k]) * share;
        length += mixed.q[k] * mixed.q[k];
    }

    length = std::sqrt(length);
    for (u32 k = 0; k < 4; k++)
    {
        mixed.q[k] = length > 1e-8f ? mixed.q[k] / length : (k == 3 ? 1.0f : 0.0f);
    }

    for (u32 k = 0; k < 3; k++)
    {
        mixed.scale[k] = from.scale[k] + (to.scale[k] - from.scale[k]) * share;
        mixed.move[k] = from.move[k] + (to.move[k] - from.move[k]) * share;
    }

    *out = Join(mixed);
    if (pivot != nullptr)
    {
        for (u32 c = 0; c < 3; c++)
        {
            f32 turned = pivot[0] * out->m[0][c] + pivot[1] * out->m[1][c] + pivot[2] * out->m[2][c];
            out->m[3][c] = at[0][c] + (at[1][c] - at[0][c]) * share - turned;
        }
    }
}

// The clip's pose at a time (frames, from 0): between its frames, a looped one wrapping round
void ClipPose(const Pack& pack, const PackClip& clip, f32 time, bool loops, std::vector<Affine>& out)
{
    f32 last = static_cast<f32>(clip.frames - 1);
    if (loops && clip.frames > 1)
    {
        time = std::fmod(time, static_cast<f32>(clip.frames));
        if (time < 0.0f)
        {
            time += static_cast<f32>(clip.frames);
        }
    }
    else
    {
        time = std::clamp(time, 0.0f, last);
    }

    u32 first = static_cast<u32>(time);
    u32 second = first + 1 < clip.frames ? first + 1 : (loops ? 0 : first);
    f32 share = time - static_cast<f32>(first);
    out.resize(pack.jointCount);
    for (u32 j = 0; j < pack.jointCount; j++)
    {
        Lerp(clip.matrices[first * pack.jointCount + j], clip.matrices[second * pack.jointCount + j], share, &out[j],
             j < pack.pivots.size() ? pack.pivots[j].data() : nullptr);
    }
}

// $TWIN_CHARACTER_TRACE: each change of Twinsanity's animation logged (the frame, the slot, its animation's ID, length in
// seconds and whether it loops, the clip shown)
void Trace(const AnimationStatus* status, s32 slot, const GameAnimation* animation, const char* clip)
{
    static const bool tracing = std::getenv("TWIN_CHARACTER_TRACE") != nullptr;
    if (tracing)
    {
        static const auto start = std::chrono::steady_clock::now();
        f32 seconds = std::chrono::duration<f32>(std::chrono::steady_clock::now() - start).count();
        Native::Log("characters: %.2f s, frame %u, snapshot %u: Twinsanity slot %d (animation %u, %.2f s%s) shown as %s", seconds, g_RenderedFrames, Native::SnapshotsTaken(), slot,
                    animation != nullptr ? animation->id : 0,
                    status != nullptr ? static_cast<f32>(status->length) / g_ClockUnitsPerSecond : 0.0f,
                    status != nullptr && status->bits.loops ? ", looped" : "", clip);
    }
}

// The pose this frame: the clip for Twinsanity's animation, moved on by the game's time (nothing moves while it's paused)
// The pose this frame. groundSpeed: how fast the character goes over the ground in the pack's units a second (a stepping clip
// plays at the pace its feet keep up with it, within a third and three times its own)
const std::vector<Affine>& PoseFor(const Pack& pack, const InstanceContext* instance, const OgiAnimator* animator,
                                   const f32* place, f32 scale)
{
    Playing& playing = g_Playing;
    const AnimationStatus* status = NewestStatus(animator);
    const GameAnimation* animation = AnimationOf(status);
    s32 slot = SlotOf(instance, animation);
    // The character's clock now (a started status's start plus its time into it: the same for all of the character's
    // statuses, a loop's wrap moving both), its advance since the last frame the time the clip moves on by. Twinsanity may
    // start a new status for the same animation, so statuses aren't followed one by one
    f32 elapsed = 0.0f;
    bool clocked = status != nullptr && status->bits.started != 0;
    s32 now = clocked ? status->began + status->time : playing.now;
    if (clocked && playing.clocked && now > playing.now)
    {
        elapsed = std::min(static_cast<f32>(now - playing.now) / g_ClockUnitsPerSecond, 0.25f);
    }

    // A new slot, or the same one started again (a second jump) unless it loops
    bool restarted = status != nullptr && clocked && status->began != playing.began && status->time < playing.statusTime &&
                     MappingOf(slot)->timing != Loop;
    bool fresh = slot != playing.slot || restarted;
    playing.status = status;
    playing.statusTime = status != nullptr ? status->time : 0;
    playing.began = status != nullptr ? status->began : 0;
    playing.clocked = clocked || playing.clocked;
    playing.now = now;
    if (fresh)
    {
        const SlotClip* mapping = MappingOf(slot);
        u32 clip = ClipNamed(pack, mapping->clip);
        Trace(status, slot, animation, pack.clips[clip].name.c_str());
        // The same clip going on (a loop, or the slide's two slots) keeps its place
        bool continues = clip == playing.clip && mapping->timing != Fit && playing.timing == mapping->timing;
        playing.from = playing.pose;
        playing.blend = playing.from.empty() ? 1.0f : 0.0f;
        playing.slot = slot;
        playing.clip = clip;
        playing.timing = mapping->timing;
        playing.speed = mapping->speed;
        playing.turns = mapping->turns;
        if (!continues)
        {
            playing.time = 0.0f;
        }
    }

    const PackClip& clip = pack.clips[playing.clip];
    f32 share = status != nullptr && status->length > 0
                    ? std::clamp(static_cast<f32>(status->time) / static_cast<f32>(status->length), 0.0f, 1.0f)
                    : 0.0f;
    playing.yaw = playing.turns * 360.0f * share;
    if (playing.timing == Fit)
    {
        playing.time = share * static_cast<f32>(clip.frames - 1);
    }
    else
    {
        f32 pace = playing.speed;
        if (playing.timing == Loop && clip.stride > 0.0f)
        {
            if (elapsed > 0.0f && playing.hasPlace)
            {
                f32 dx = place[0] - playing.place[0];
                f32 dz = place[2] - playing.place[2];
                f32 speed = std::sqrt(dx * dx + dz * dz) / elapsed / std::max(scale, 1e-3f);
                // Smoothed: a frame's step is uneven
                playing.groundSpeed += (speed - playing.groundSpeed) * std::min(1.0f, elapsed * 8.0f);
            }

            pace = std::clamp(playing.groundSpeed / clip.stride, 0.33f, 2.5f) * playing.speed;
            static const bool tracing = std::getenv("TWIN_CHARACTER_TRACE") != nullptr;
            if (tracing && g_RenderedFrames % 30 == 0)
            {
                Native::Log("characters: %s: ground %.2f, its stride %.2f a second, pace %.2f", clip.name.c_str(),
                            playing.groundSpeed, clip.stride, pace);
            }
        }

        playing.time += elapsed * clip.rate * pace;
    }

    for (u32 k = 0; k < 3; k++)
    {
        playing.place[k] = place[k];
    }

    playing.hasPlace = true;
    ClipPose(pack, clip, playing.time, playing.timing == Loop, playing.pose);
    if (playing.blend < 1.0f && playing.from.size() == playing.pose.size())
    {
        playing.blend = std::min(1.0f, playing.blend + elapsed / BlendSeconds);
        for (u32 j = 0; j < pack.jointCount; j++)
        {
            Lerp(playing.from[j], playing.pose[j], playing.blend, &playing.pose[j],
                 j < pack.pivots.size() ? pack.pivots[j].data() : nullptr);
        }
    }

    return playing.pose;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The packets

// The chain of our own the buckets CALL: two buffers, a frame apart (the frame's chain is sent at its end)
std::vector<Quad> g_Chains[2];
// Quadwords: a chain's room (reserved once: CALLed memory mustn't move), and the most a draw takes (every texture uploaded and
// every triangle clipped into a few)
constexpr size_t ChainRoom = 0x80000;
constexpr size_t DrawRoom = 0x30000;
u32 g_ChainFrame = ~0u;
u32 g_ChainIndex = 0;

struct ClipVertex
{
    f32 clip[4];
    f32 uv[2];
    f32 colour[3];
};

ClipVertex Mix(const ClipVertex& a, const ClipVertex& b, f32 t)
{
    ClipVertex out;
    for (u32 i = 0; i < 4; i++)
    {
        out.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
    }

    for (u32 i = 0; i < 2; i++)
    {
        out.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    }

    for (u32 i = 0; i < 3; i++)
    {
        out.colour[i] = a.colour[i] + (b.colour[i] - a.colour[i]) * t;
    }

    return out;
}

// A triangle clipped to the view (near and far planes, the guard band's sides) in clip space: the polygon's corners
u32 ClipTriangle(const ClipVertex* triangle, ClipVertex* out, f32 guardX, f32 guardY)
{
    ClipVertex buffers[2][12];
    u32 count = 3;
    std::copy(triangle, triangle + 3, buffers[0]);
    u32 current = 0;
    // Each plane's distance: inside when >= 0
    auto distance = [&](const ClipVertex& v, u32 plane) {
        switch (plane)
        {
        case 0:
            return v.clip[2] + v.clip[3];
        case 1:
            return v.clip[3] - v.clip[2];
        case 2:
            return guardX * v.clip[3] - v.clip[0];
        case 3:
            return guardX * v.clip[3] + v.clip[0];
        case 4:
            return guardY * v.clip[3] - v.clip[1];
        default:
            return guardY * v.clip[3] + v.clip[1];
        }
    };

    for (u32 plane = 0; plane < 6 && count >= 3; plane++)
    {
        const ClipVertex* in = buffers[current];
        ClipVertex* next = buffers[current ^ 1];
        u32 kept = 0;
        for (u32 i = 0; i < count && kept < 11; i++)
        {
            const ClipVertex& a = in[i];
            const ClipVertex& b = in[(i + 1) % count];
            f32 da = distance(a, plane);
            f32 db = distance(b, plane);
            if (da >= 0.0f)
            {
                next[kept++] = a;
            }

            if ((da >= 0.0f) != (db >= 0.0f))
            {
                next[kept++] = Mix(a, b, da / (da - db));
            }
        }

        count = kept;
        current ^= 1;
    }

    std::copy(buffers[current], buffers[current] + count, out);
    return count >= 3 ? count : 0;
}

struct ChainWriter
{
    std::vector<Quad>& chain;

    size_t Here() const
    {
        return chain.size();
    }

    Quad& Add()
    {
        chain.push_back({});
        return chain.back();
    }

    // A CNT tag of VIF1's DIRECT (after FLUSHA) whose quadwords are filled in after it; Close sets the count
    size_t Open()
    {
        chain.push_back({});
        return chain.size() - 1;
    }

    void Close(size_t tag)
    {
        u32 quadwords = static_cast<u32>(chain.size() - tag - 1);
        chain[tag].w[0] = CountTag | quadwords;
        chain[tag].w[1] = 0;
        chain[tag].w[2] = VifFlushA;
        chain[tag].w[3] = VifDirect | quadwords;
    }

    void Write64(u64 low, u64 high)
    {
        Quad& q = Add();
        std::memcpy(&q.w[0], &low, 8);
        std::memcpy(&q.w[2], &high, 8);
    }

    void Register(u64 value, u64 address)
    {
        Write64(value, address);
    }
};

void UploadPixels(ChainWriter& writer, const PackTexture& texture, u32 gsAddress)
{
    size_t tag = writer.Open();
    writer.Write64(AddressDataTag(4), GifAddressData);
    GS_BITBLTBUF blit = {};
    blit.dest_addr = gsAddress;
    blit.dest_width = texture.slotOwner->levelWidths[0];
    blit.dest_pixmode = GS_PIXMODE_32;
    writer.Register(std::bit_cast<u64>(blit), GsBitBltBuf);
    writer.Register(0, GsTrxPos);
    writer.Register(static_cast<u64>(texture.width) | static_cast<u64>(texture.height) << 32, GsTrxReg);
    writer.Register(0, GsTrxDir);
    writer.Close(tag);
    // The pixels where they are (REFs, not copied into the chain), in IMAGE packets the GIF's tag can count (the path keeps its
    // place between VIF1's DIRECTs)
    constexpr size_t Chunk = 0x4000;
    for (size_t at = 0; at < texture.pixels.size(); at += Chunk)
    {
        u32 quadwords = static_cast<u32>(std::min(Chunk, texture.pixels.size() - at));
        Quad& image = writer.Add();
        image.w[0] = CountTag | 1;
        image.w[1] = 0;
        image.w[2] = 0;
        image.w[3] = VifDirect | 1;
        writer.Write64(ImageDataTag(quadwords), 0);
        Quad& reference = writer.Add();
        reference.w[0] = ReferenceTag | quadwords;
        reference.w[1] = Address(&texture.pixels[at]);
        reference.w[2] = 0;
        reference.w[3] = VifDirect | quadwords;
    }
}

void MaterialRegisters(ChainWriter& writer, const PackMaterial& material, const PackTexture* texture, u32 gsAddress)
{
    size_t tag = writer.Open();
    constexpr u32 Writes = 10;
    writer.Write64(AddressDataTag(Writes), GifAddressData);
    writer.Register(0, GsTexFlush);
    GS_TEX0 tex0 = {};
    if (texture != nullptr)
    {
        tex0.tb_addr = gsAddress;
        tex0.tb_width = texture->slotOwner->levelWidths[0];
        tex0.psm = GS_TEX_32;
        tex0.tex_width = texture->slotOwner->widthPower;
        tex0.tex_height = texture->slotOwner->heightPower;
        tex0.tex_cc = 1;
    }

    writer.Register(std::bit_cast<u64>(tex0), GsTex0);
    GS_TEX1 tex1 = {};
    tex1.mmag = GS_TEX_LINEAR;
    tex1.mmin = GS_TEX_LINEAR;
    writer.Register(std::bit_cast<u64>(tex1), GsTex1);
    writer.Register(0, GsClamp);
    writer.Register(AttributesFromPrmode, GsPrmodeCont);
    GS_PRMODE mode = {};
    mode.iip = 1;
    mode.tme = texture != nullptr ? 1 : 0;
    mode.abe = material.blends ? 1 : 0;
    writer.Register(std::bit_cast<u64>(mode), GsPrmode);
    writer.Register(std::bit_cast<u64>(GS_ALPHA{.a = 0, .b = 1, .c = 0, .d = 1}), GsAlpha);
    GS_TEST test = {};
    test.atest_enable = 1;
    test.atest_method = GS_ALPHA_GREATER;
    test.atest_reference = material.blends ? 0x04 : 0x20;
    test.atest_fail_method = GS_ALPHA_NO_UPDATE;
    test.ztest_enable = 1;
    test.ztest_method = GS_ZBUFF_GEQUAL;
    writer.Register(std::bit_cast<u64>(test), GsTest);
    writer.Register(g_DepthBufferPage | ZbufZ32Format, GsZbuf);
    writer.Register(1, GsFba);
    writer.Close(tag);
}

struct Placement
{
    // Model to world (the pack's space turned and scaled into Twinsanity's, then the instance's matrix)
    Matrix4x4 world;
    f32 normalScale;
};

f32 g_TurnDegrees = 180.0f;

// The pack's size made the Twinsanity character's (its model's height; $TWIN_CHARACTER_SCALE sets it)
f32 ScaleOf(const Pack& pack, const GameOGI* ogi)
{
    f32 packHeight = pack.high[1] - pack.low[1];
    f32 height = ogi != nullptr ? ogi->bounds.max.y - ogi->bounds.min.y : 0.0f;
    f32 scale = packHeight > 0.0f && height > 0.0f ? height / packHeight : 1.0f;
    static const char* scaleSetting = std::getenv("TWIN_CHARACTER_SCALE");
    if (scaleSetting != nullptr)
    {
        scale = static_cast<f32>(std::atof(scaleSetting));
    }

    return scale;
}

// The pack's model space (WoC's: Y up, facing -Z, Crash about 1 tall) into the instance's: turned about Y, scaled to the
// Twinsanity character's height
Placement Place(const Pack& pack, const GameOGI* ogi, const Matrix4x4* matrix)
{
    static const char* turn = std::getenv("TWIN_CHARACTER_TURN");
    if (turn != nullptr)
    {
        g_TurnDegrees = static_cast<f32>(std::atof(turn));
    }

    f32 scale = ScaleOf(pack, ogi);

    f32 radians = (g_TurnDegrees + g_Playing.yaw) * 3.14159265f / 180.0f;
    f32 c = std::cos(radians) * scale;
    f32 s = std::sin(radians) * scale;
    Matrix4x4 turnScale = {};
    turnScale.m[0][0] = c;
    turnScale.m[0][2] = -s;
    turnScale.m[1][1] = scale;
    turnScale.m[2][0] = s;
    turnScale.m[2][2] = c;
    turnScale.m[3][3] = 1.0f;
    Placement placement;
    for (u32 r = 0; r < 4; r++)
    {
        for (u32 col = 0; col < 4; col++)
        {
            f32 sum = 0.0f;
            for (u32 k = 0; k < 4; k++)
            {
                sum += turnScale.m[r][k] * matrix->m[k][col];
            }

            placement.world.m[r][col] = sum;
        }
    }

    placement.normalScale = 1.0f;
    return placement;
}

void TransformPoint(const f32* p, const Matrix4x4& m, f32* out)
{
    for (u32 c = 0; c < 4; c++)
    {
        out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
    }
}

void SkinVertex(const PackVertex& v, const std::vector<Affine>& pose, f32* position, f32* normal)
{
    for (u32 k = 0; k < 3; k++)
    {
        position[k] = 0.0f;
        normal[k] = 0.0f;
    }

    for (u32 i = 0; i < 4; i++)
    {
        f32 w = v.weights[i];
        if (w == 0.0f)
        {
            continue;
        }

        const Affine& m = pose[v.joints[i]];
        for (u32 k = 0; k < 3; k++)
        {
            position[k] += w * (v.position[0] * m.m[0][k] + v.position[1] * m.m[1][k] + v.position[2] * m.m[2][k] + m.m[3][k]);
            normal[k] += w * (v.normal[0] * m.m[0][k] + v.normal[1] * m.m[1][k] + v.normal[2] * m.m[2][k]);
        }
    }
}

// The lit colour of a normal in the world: the ambient light and the three strongest lights, times the material's colour.
// Twinsanity's textures are stored at half intensity and its lights' sums double them (2: the texture at full); the packs'
// textures are at full intensity, so the sum is halved
constexpr f32 LightScale = 0.5f;

void Light(const f32* normal, const ChunkLights* lights, const f32* materialColour, f32* out)
{
    f32 length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    f32 n[3] = {0.0f, 1.0f, 0.0f};
    if (length > 1e-6f)
    {
        for (u32 k = 0; k < 3; k++)
        {
            n[k] = normal[k] / length;
        }
    }

    f32 sum[3] = {1.0f, 1.0f, 1.0f};
    if (lights != nullptr)
    {
        for (u32 k = 0; k < 3; k++)
        {
            sum[k] = (&lights->ambient.x)[k];
        }

        for (u32 i = 0; i < ChunkLights::Strongest; i++)
        {
            const Vector4& d = lights->directions[i];
            f32 facing = -(n[0] * d.x + n[1] * d.y + n[2] * d.z);
            if (facing > 0.0f)
            {
                for (u32 k = 0; k < 3; k++)
                {
                    sum[k] += facing * (&lights->colours[i].x)[k];
                }
            }
        }
    }

    for (u32 k = 0; k < 3; k++)
    {
        out[k] = sum[k] * LightScale * materialColour[k];
    }
}

u32 g_DrawnFrame = ~0u;

void DrawPack(Pack& pack, const InstanceContext* instance, const GameOGI* ogi, const OgiAnimator* animator,
              const Matrix4x4* matrix, const ChunkLights* lights, u32 bucketIndex)
{
    const std::vector<Affine>& pose = PoseFor(pack, instance, animator, &matrix->m[3][0], ScaleOf(pack, ogi));
    Placement placement = Place(pack, ogi, matrix);
    Matrix4x4 toClip = g_RenderView->toClip;
    const Matrix4x4& toScreen = g_RenderTarget->clipToScreen;
    f32 guardX = g_RenderView->clip.x > 0.0f ? 1.0f / g_RenderView->clip.x : 5.0f;
    f32 guardY = g_RenderView->clip.y > 0.0f ? 1.0f / g_RenderView->clip.y : 5.0f;

    if (g_ChainFrame != g_RenderedFrames)
    {
        g_ChainFrame = g_RenderedFrames;
        g_ChainIndex ^= 1;
        g_Chains[g_ChainIndex].clear();
    }

    std::vector<Quad>& chain = g_Chains[g_ChainIndex];
    chain.reserve(ChainRoom);
    if (chain.size() + DrawRoom > ChainRoom)
    {
        return;
    }

    size_t start = chain.size();
    ChainWriter writer{chain};

    std::vector<ClipVertex> corners;
    for (u32 pass = 0; pass < 2; pass++)
    {
        for (const PackMaterial& material : pack.materials)
        {
            if (material.blends != (pass == 1) || material.triangles.empty())
            {
                continue;
            }

            PackTexture* texture = material.texture >= 0 ? &pack.textures[material.texture] : nullptr;
            u32 gsAddress = 0;
            if (texture != nullptr)
            {
                bool upload = false;
                bool large = texture->pixels.size() * sizeof(Quad) > SlotBytes;
                TextureSlot* slot = ClaimTextureSlot(texture->slotOwner.get(), bucketIndex, large, &upload);
                if (slot == nullptr)
                {
                    continue;
                }

                gsAddress = slot->gsAddress;
                if (upload)
                {
                    UploadPixels(writer, *texture, gsAddress);
                }
            }

            MaterialRegisters(writer, material, texture, gsAddress);
            corners.resize(material.vertices.size());
            for (size_t i = 0; i < material.vertices.size(); i++)
            {
                const PackVertex& v = material.vertices[i];
                f32 position[3];
                f32 normal[3];
                SkinVertex(v, pose, position, normal);
                f32 world[4];
                TransformPoint(position, placement.world, world);
                f32 worldNormal[3];
                for (u32 k = 0; k < 3; k++)
                {
                    worldNormal[k] = normal[0] * placement.world.m[0][k] + normal[1] * placement.world.m[1][k] +
                                     normal[2] * placement.world.m[2][k];
                }

                ClipVertex& out = corners[i];
                TransformPoint(world, toClip, out.clip);
                out.uv[0] = v.uv[0];
                out.uv[1] = v.uv[1];
                Light(worldNormal, lights, material.colour, out.colour);
            }

            // The triangles: a GIF tag of PACKED ST, RGBAQ and XYZ2 loops (a triangle list), up to a few thousand vertexes a tag
            size_t tag = writer.Open();
            size_t gifAt = writer.Here();
            writer.Add();
            u32 vertexes = 0;
            auto emit = [&](const ClipVertex& c) {
                f32 screen[4];
                for (u32 col = 0; col < 4; col++)
                {
                    screen[col] = c.clip[0] * toScreen.m[0][col] + c.clip[1] * toScreen.m[1][col] +
                                  c.clip[2] * toScreen.m[2][col] + c.clip[3] * toScreen.m[3][col];
                }

                f32 q = 1.0f / screen[3];
                Quad& st = writer.Add();
                f32 s = c.uv[0] * q;
                f32 t = c.uv[1] * q;
                std::memcpy(&st.w[0], &s, 4);
                std::memcpy(&st.w[1], &t, 4);
                std::memcpy(&st.w[2], &q, 4);
                Quad& rgba = writer.Add();
                for (u32 k = 0; k < 3; k++)
                {
                    rgba.w[k] = static_cast<u32>(std::clamp(c.colour[k] * 128.0f, 0.0f, 255.0f));
                }

                rgba.w[3] = 0x80;
                Quad& xyz = writer.Add();
                xyz.w[0] = static_cast<u32>(std::clamp(screen[0] * q, 0.0f, 65535.0f));
                xyz.w[1] = static_cast<u32>(std::clamp(screen[1] * q, 0.0f, 65535.0f));
                xyz.w[2] = static_cast<u32>(std::clamp(screen[2] * q, 0.0f, 4294967040.0f));
                vertexes++;
            };

            for (size_t t = 0; t + 2 < material.triangles.size(); t += 3)
            {
                ClipVertex triangle[3] = {corners[material.triangles[t]], corners[material.triangles[t + 1]],
                                          corners[material.triangles[t + 2]]};
                ClipVertex polygon[12];
                u32 count = ClipTriangle(triangle, polygon, guardX, guardY);
                for (u32 i = 1; i + 1 < count; i++)
                {
                    emit(polygon[0]);
                    emit(polygon[i]);
                    emit(polygon[i + 1]);
                }
            }

            GifTag gif = {};
            gif.loops = vertexes;
            gif.endOfPacket = 1;
            gif.setsPrim = 1;
            gif.prim = GS_PRIM_TRI;
            gif.format = GifPacked;
            gif.registerCount = 3;
            u64 descriptors = GifDescriptors(GifSt, GifRgbaq, GifXyz2);
            std::memcpy(&chain[gifAt].w[0], &gif.value, 8);
            std::memcpy(&chain[gifAt].w[2], &descriptors, 8);
            if (vertexes == 0)
            {
                chain.resize(tag);
                continue;
            }

            writer.Close(tag);
        }
    }

    Quad& ret = writer.Add();
    ret.w[0] = ReturnTag;

    RenderBucket& bucket = g_FrameBuckets.buckets[bucketIndex];
    auto* packet = reinterpret_cast<u32*>(BeginPacket(bucket));
    packet[0] = CallTag;
    packet[1] = Address(&chain[start]);
    packet[2] = 0;
    packet[3] = 0;
    EndPacket(bucket, reinterpret_cast<u8*>(packet + 4));
    // The GS's settings changed under the bucket's next material: it sets its own again
    bucket.lastCall = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The options' preview: the pack drawn on the CPU by a small rasteriser (perspective-correct texture coordinates, bilinear
// texels, a depth buffer, the blended materials after the rest), lit by a warm key, a cool fill and a rim

struct ShowcaseStep
{
    const char* clip;
    f32 seconds;
    bool loops;
};

// Idle, a run, idle again and a fidget, round and round, each blending into the next
constexpr ShowcaseStep Showcase[] = {
    {"b/idle", 3.2f, true}, {"a/run|a/walk", 2.4f, true}, {"b/idle", 1.6f, true},
    {"e/taunt|b/itch|b/idle", 2.5f, false}, {"b/idle", 1.6f, true}, {"b/itch|b/idle", 2.4f, false},
};

void ShowcasePose(const Pack& pack, f32 seconds, std::vector<Affine>& pose)
{
    f32 total = 0.0f;
    for (const ShowcaseStep& step : Showcase)
    {
        total += step.seconds;
    }

    f32 t = std::fmod(std::max(seconds, 0.0f), total);
    u32 index = 0;
    while (t >= Showcase[index].seconds)
    {
        t -= Showcase[index].seconds;
        index = (index + 1) % std::size(Showcase);
    }

    auto clipPose = [&](u32 step, f32 time, std::vector<Affine>& out) {
        const PackClip& clip = pack.clips[ClipNamed(pack, Showcase[step].clip)];
        ClipPose(pack, clip, time * clip.rate, Showcase[step].loops, out);
    };

    clipPose(index, t, pose);
    constexpr f32 Blend = BlendSeconds * 2.0f;
    if (t < Blend)
    {
        u32 before = (index + static_cast<u32>(std::size(Showcase)) - 1) % static_cast<u32>(std::size(Showcase));
        std::vector<Affine> from;
        clipPose(before, Showcase[before].seconds + t, from);
        f32 share = t / Blend;
        share = share * share * (3.0f - 2.0f * share);
        for (u32 j = 0; j < pack.jointCount; j++)
        {
            Lerp(from[j], pose[j], share, &pose[j], j < pack.pivots.size() ? pack.pivots[j].data() : nullptr);
        }
    }
}

// How far the showcase reaches: the lowest and highest points, and the farthest from the up axis
struct ShowcaseExtent
{
    f32 low = 0.0f;
    f32 high = 1.0f;
    f32 reach = 0.5f;
};

const ShowcaseExtent& ExtentOf(const Pack& pack)
{
    static std::map<std::string, ShowcaseExtent> extents;
    auto found = extents.find(pack.folder);
    if (found != extents.end())
    {
        return found->second;
    }

    ShowcaseExtent extent;
    extent.low = 1e9f;
    extent.high = -1e9f;
    extent.reach = 0.0f;
    std::vector<Affine> pose;
    for (const ShowcaseStep& step : Showcase)
    {
        const PackClip& clip = pack.clips[ClipNamed(pack, step.clip)];
        for (u32 frame = 0; frame < clip.frames; frame += 2)
        {
            ClipPose(pack, clip, static_cast<f32>(frame), false, pose);
            for (const PackMaterial& material : pack.materials)
            {
                for (const PackVertex& vertex : material.vertices)
                {
                    f32 p[3];
                    f32 n[3];
                    SkinVertex(vertex, pose, p, n);
                    extent.low = std::min(extent.low, p[1]);
                    extent.high = std::max(extent.high, p[1]);
                    extent.reach = std::max(extent.reach, std::sqrt(p[0] * p[0] + p[2] * p[2]));
                }
            }
        }
    }

    if (extent.low > extent.high)
    {
        extent = ShowcaseExtent{};
    }

    return extents.emplace(pack.folder, extent).first->second;
}

struct PreviewVertex
{
    f32 x, y, z;
    f32 u, v;
    f32 shade[3];
};

// Bilinear and wrapping, from the pixels as the GS has them (alpha 0x80 opaque)
void Texel(const PackTexture& texture, f32 u, f32 v, f32* out)
{
    const auto* pixels = reinterpret_cast<const u8*>(texture.pixels.data());
    f32 x = u * static_cast<f32>(texture.width) - 0.5f;
    f32 y = v * static_cast<f32>(texture.height) - 0.5f;
    f32 fx = std::floor(x);
    f32 fy = std::floor(y);
    f32 ax = x - fx;
    f32 ay = y - fy;
    s32 x0 = static_cast<s32>(fx);
    s32 y0 = static_cast<s32>(fy);
    for (u32 k = 0; k < 4; k++)
    {
        out[k] = 0.0f;
    }

    for (u32 corner = 0; corner < 4; corner++)
    {
        s32 cx = (x0 + static_cast<s32>(corner & 1)) & static_cast<s32>(texture.width - 1);
        s32 cy = (y0 + static_cast<s32>(corner >> 1)) & static_cast<s32>(texture.height - 1);
        f32 weight = ((corner & 1) ? ax : 1.0f - ax) * ((corner >> 1) ? ay : 1.0f - ay);
        const u8* p = pixels + (static_cast<size_t>(cy) * texture.width + static_cast<size_t>(cx)) * 4;
        out[0] += weight * p[0] / 255.0f;
        out[1] += weight * p[1] / 255.0f;
        out[2] += weight * p[2] / 255.0f;
        out[3] += weight * std::min(1.0f, p[3] / 128.0f);
    }
}

void RenderPack(const Pack& pack, f32 seconds, f32 yaw, u32 width, u32 height, std::vector<u32>& pixels)
{
    std::vector<Affine> pose;
    ShowcasePose(pack, seconds, pose);
    std::vector<f32> colour(static_cast<size_t>(width) * height * 4, 0.0f);
    std::vector<f32> depth(static_cast<size_t>(width) * height, 1e30f);
    // A 26 degree view fitting everything the showcase does (its height, and its reach round the up axis at any turn) with a
    // margin, the camera in front of it (the packs' models face -Z)
    const ShowcaseExtent& extent = ExtentOf(pack);
    f32 modelHeight = std::max(0.1f, extent.high - extent.low);
    f32 centreY = extent.low + modelHeight * 0.5f;
    f32 tangent = std::tan(13.0f * 3.14159265f / 180.0f);
    f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    f32 distance = std::max(modelHeight * 0.56f, extent.reach * 1.15f / aspect) / tangent;
    f32 focal = static_cast<f32>(height) * 0.5f / tangent;
    f32 cx = static_cast<f32>(width) * 0.5f;
    f32 cy = static_cast<f32>(height) * 0.5f;
    f32 radians = yaw * 3.14159265f / 180.0f;
    f32 cosYaw = std::cos(radians);
    f32 sinYaw = std::sin(radians);
    // The lights' directions in the camera's space (x right, y up, z away from the eye)
    const f32 key[3] = {-0.45f, -0.65f, 0.62f};
    const f32 fill[3] = {0.7f, -0.1f, 0.7f};

    // The ground's shadow: a soft dark ellipse under the feet
    f32 groundY = cy - (extent.low - centreY) * focal / distance;
    f32 radiusX = modelHeight * 0.3f * focal / distance;
    f32 radiusY = radiusX * 0.2f;
    for (u32 y = 0; y < height; y++)
    {
        for (u32 x = 0; x < width; x++)
        {
            f32 dx = (static_cast<f32>(x) - cx) / radiusX;
            f32 dy = (static_cast<f32>(y) - groundY) / radiusY;
            f32 r = dx * dx + dy * dy;
            if (r < 1.0f)
            {
                f32* c = &colour[(static_cast<size_t>(y) * width + x) * 4];
                c[3] = 0.42f * (1.0f - r) * (1.0f - r);
                c[0] = 0.02f * c[3];
                c[1] = 0.06f * c[3];
                c[2] = 0.16f * c[3];
            }
        }
    }

    for (u32 pass = 0; pass < 2; pass++)
    {
        for (const PackMaterial& material : pack.materials)
        {
            if (material.blends != (pass == 1))
            {
                continue;
            }

            const PackTexture* texture = material.texture >= 0 ? &pack.textures[material.texture] : nullptr;
            std::vector<PreviewVertex> corners(material.vertices.size());
            for (size_t i = 0; i < material.vertices.size(); i++)
            {
                f32 p[3];
                f32 n[3];
                SkinVertex(material.vertices[i], pose, p, n);
                // Turned about Y, then the camera's: x right is the model's -x (it faces us down its -Z)
                f32 x = p[0] * cosYaw - p[2] * sinYaw;
                f32 z = p[0] * sinYaw + p[2] * cosYaw;
                f32 nx = n[0] * cosYaw - n[2] * sinYaw;
                f32 nz = n[0] * sinYaw + n[2] * cosYaw;
                f32 ny = n[1];
                f32 length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 1e-6f)
                {
                    nx /= length;
                    ny /= length;
                    nz /= length;
                }

                PreviewVertex& out = corners[i];
                out.z = z + distance;
                out.x = cx - x * focal / out.z;
                out.y = cy - (p[1] - centreY) * focal / out.z;
                out.u = material.vertices[i].uv[0];
                out.v = material.vertices[i].uv[1];
                f32 normal[3] = {-nx, ny, nz};
                f32 keyLight = std::max(0.0f, -(normal[0] * key[0] + normal[1] * key[1] + normal[2] * key[2]));
                f32 fillLight = std::max(0.0f, -(normal[0] * fill[0] + normal[1] * fill[1] + normal[2] * fill[2]));
                f32 rim = std::pow(1.0f - std::fabs(normal[2]), 3.0f) * 0.4f;
                f32 ambient = 0.42f;
                out.shade[0] = (ambient + keyLight * 0.75f + fillLight * 0.16f) * material.colour[0] + rim * 0.7f;
                out.shade[1] = (ambient + keyLight * 0.70f + fillLight * 0.20f) * material.colour[1] + rim * 0.85f;
                out.shade[2] = (ambient + keyLight * 0.62f + fillLight * 0.28f) * material.colour[2] + rim;
            }

            for (size_t t = 0; t + 2 < material.triangles.size(); t += 3)
            {
                const PreviewVertex& a = corners[material.triangles[t]];
                const PreviewVertex& b = corners[material.triangles[t + 1]];
                const PreviewVertex& c = corners[material.triangles[t + 2]];
                if (a.z < 0.05f || b.z < 0.05f || c.z < 0.05f)
                {
                    continue;
                }

                f32 area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
                if (std::fabs(area) < 1e-6f)
                {
                    continue;
                }

                s32 left = std::max(0, static_cast<s32>(std::floor(std::min({a.x, b.x, c.x}))));
                s32 right = std::min(static_cast<s32>(width) - 1, static_cast<s32>(std::ceil(std::max({a.x, b.x, c.x}))));
                s32 top = std::max(0, static_cast<s32>(std::floor(std::min({a.y, b.y, c.y}))));
                s32 bottom = std::min(static_cast<s32>(height) - 1, static_cast<s32>(std::ceil(std::max({a.y, b.y, c.y}))));
                f32 inverse[3] = {1.0f / a.z, 1.0f / b.z, 1.0f / c.z};
                for (s32 y = top; y <= bottom; y++)
                {
                    f32 py = static_cast<f32>(y) + 0.5f;
                    for (s32 x = left; x <= right; x++)
                    {
                        f32 px = static_cast<f32>(x) + 0.5f;
                        f32 w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
                        f32 w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
                        f32 w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                        {
                            continue;
                        }

                        f32 z = 1.0f / (w0 * inverse[0] + w1 * inverse[1] + w2 * inverse[2]);
                        size_t at = static_cast<size_t>(y) * width + static_cast<size_t>(x);
                        if (z >= depth[at])
                        {
                            continue;
                        }

                        f32 p0 = w0 * inverse[0] * z;
                        f32 p1 = w1 * inverse[1] * z;
                        f32 p2 = w2 * inverse[2] * z;
                        f32 texel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                        if (texture != nullptr)
                        {
                            Texel(*texture, p0 * a.u + p1 * b.u + p2 * c.u, p0 * a.v + p1 * b.v + p2 * c.v, texel);
                        }

                        f32 alpha = texel[3];
                        if (alpha < (material.blends ? 0.02f : 0.3f))
                        {
                            continue;
                        }

                        if (!material.blends)
                        {
                            alpha = 1.0f;
                            depth[at] = z;
                        }

                        // Premultiplied over what's there
                        f32* out = &colour[at * 4];
                        for (u32 k = 0; k < 3; k++)
                        {
                            f32 lit = std::min(1.0f, texel[k] * (p0 * a.shade[k] + p1 * b.shade[k] + p2 * c.shade[k]));
                            out[k] = lit * alpha + out[k] * (1.0f - alpha);
                        }

                        out[3] = alpha + out[3] * (1.0f - alpha);
                    }
                }
            }
        }
    }

    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++)
    {
        const f32* c = &colour[i * 4];
        f32 a = std::clamp(c[3], 0.0f, 1.0f);
        auto channel = [&](f32 v) {
            return static_cast<u32>(std::clamp(a > 0.0f ? v / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        pixels[i] = channel(c[0]) | channel(c[1]) << 8 | channel(c[2]) << 16 | static_cast<u32>(a * 255.0f + 0.5f) << 24;
    }
}
}

namespace NativeCharacters
{
bool RenderPreview(const std::string& folder, f32 seconds, f32 yaw, u32 width, u32 height, std::vector<u32>& pixels)
{
    Pack* pack = PackFor(folder);
    if (pack == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    RenderPack(*pack, seconds, yaw, width, height, pixels);
    return true;
}

std::vector<std::string> Installed()
{
    std::vector<std::string> found;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(CharactersFolder(), error))
    {
        if (entry.is_directory() && fs::exists(entry.path() / "pack.bin"))
        {
            found.push_back(entry.path().filename().string());
        }
    }

    std::sort(found.begin(), found.end());
    return found;
}

const std::string& Choice()
{
    // $TWIN_CHARACTER (tests) over the option
    if (const char* setting = std::getenv("TWIN_CHARACTER"))
    {
        g_Choice = setting;
    }
    else
    {
        g_Choice = NativeUi::CharacterChoice();
    }

    return g_Choice;
}

void SetChoice(const std::string& name)
{
    NativeUi::SetCharacterChoice(name);
}

std::string DisplayName(const std::string& folder)
{
    if (folder.empty())
    {
        return "Twinsanity Crash";
    }

    // The pack's own name (name.txt's first line), when it has one
    std::ifstream file(CharactersFolder() / folder / "name.txt");
    std::string line;
    if (file && std::getline(file, line) && !line.empty())
    {
        return line;
    }

    // Else its folder's: the game's prefix as its name, the rest in words ("cttr_crash_japan_caveman": "Tag Team Racing Crash:
    // Japan Caveman")
    struct Prefix
    {
        const char* folder;
        const char* name;
    };
    constexpr Prefix Prefixes[] = {{"woc_crash", "Wrath of Cortex Crash"}, {"cttr_crash", "Tag Team Racing Crash"}};
    std::string rest = folder;
    std::string name;
    for (const Prefix& prefix : Prefixes)
    {
        std::string start = prefix.folder;
        if (rest.rfind(start, 0) == 0 && (rest.size() == start.size() || rest[start.size()] == '_'))
        {
            name = prefix.name;
            rest = rest.size() > start.size() ? rest.substr(start.size() + 1) : "";
            break;
        }
    }

    std::string words;
    bool capital = true;
    for (char c : rest)
    {
        if (c == '_')
        {
            words += ' ';
            capital = true;
            continue;
        }

        words += capital ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        capital = false;
    }

    if (name.empty())
    {
        return words;
    }

    return words.empty() ? name : name + ": " + words;
}

bool DrawInstead(InstanceContext* instance, GameOGI* ogi, OgiAnimator* animator, const Matrix4x4* matrix, const ChunkLights* lights,
                 u32)
{
    GameController* controller = G_GameController;
    if (controller == nullptr || instance == nullptr || instance != controller->progress.Instance(CharacterCrash))
    {
        return false;
    }

    Pack* pack = ChosenPack();
    if (pack == nullptr || ogi == nullptr || ogi->hasSkin == 0 || ogi->skin == nullptr || g_RenderView == nullptr ||
        g_RenderTarget == nullptr)
    {
        return false;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Native::Log("characters: drawing %s for Crash (OGI %u: %u joints, bounds y %.3f to %.3f)", pack->folder.c_str(), ogi->id,
                    ogi->jointCount, ogi->bounds.min.y, ogi->bounds.max.y);
        if (lights != nullptr)
        {
            Native::Log("characters: lights: ambient %.3f %.3f %.3f, first %.3f %.3f %.3f from %.3f %.3f %.3f",
                        lights->ambient.x, lights->ambient.y, lights->ambient.z, lights->colours[0].x, lights->colours[0].y,
                        lights->colours[0].z, lights->directions[0].x, lights->directions[0].y, lights->directions[0].z);
        }
    }

    u32 bucket = ogi->skin->count > 0 ? ogi->skin->materials[0]->material->bucket : BucketOpaque;
    DrawPack(*pack, instance, ogi, animator, matrix, lights, bucket);
    return true;
}
}
