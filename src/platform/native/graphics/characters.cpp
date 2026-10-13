// Character packs drawn (characters.h, native/CHARACTERS.md). A pack (native/tools/woc_character.py) has a skinned model (its
// materials' vertexes with four joints and weights each, its triangles), its textures (RGBA) and its animations (every joint's
// skinning matrix a frame). It's drawn as the PS2 renderer draws: its triangles skinned, lit and projected here, then sent to the
// GS in the frame's chain (a CALL in the player's skin's bucket of a chain of our own: VIF1 DIRECT of GIF packets), its textures
// uploaded into the renderer's slots of GS memory (ClaimTextureSlot) like the game's. It shares the frame's depth buffer, its
// view and its lights; nothing goes past the emulated hardware.

#include "characters.h"

#include "native.h"
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
#include <bit>
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
        good = good && at + bytes <= data.size() && std::has_single_bit(texture.width) && std::has_single_bit(texture.height) &&
               bytes <= 0x10000;
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
bool g_ChoiceRead = false;
std::unique_ptr<Pack> g_Pack;
std::string g_FailedFolder;

Pack* ChosenPack()
{
    const std::string& choice = NativeCharacters::Choice();
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
};

#include "characters_mapping.inc"

struct Playing
{
    s32 slot = -2;
    const AnimationStatus* status = nullptr;
    s32 statusTime = 0;
    u32 clip = 0;
    ClipTiming timing = Loop;
    f32 speed = 1.0f;
    f32 time = 0.0f;
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

u32 ClipNamed(const Pack& pack, const char* name)
{
    auto found = pack.clipIndex.find(name);
    if (found != pack.clipIndex.end())
    {
        return found->second;
    }

    found = pack.clipIndex.find(g_DefaultClip.clip);
    return found != pack.clipIndex.end() ? found->second : 0;
}

void Lerp(const Affine& a, const Affine& b, f32 share, Affine* out)
{
    for (u32 r = 0; r < 4; r++)
    {
        for (u32 c = 0; c < 3; c++)
        {
            out->m[r][c] = a.m[r][c] + (b.m[r][c] - a.m[r][c]) * share;
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
        Lerp(clip.matrices[first * pack.jointCount + j], clip.matrices[second * pack.jointCount + j], share, &out[j]);
    }
}

void Trace(const char* format, s32 slot, u32 id, const char* clip)
{
    static const bool tracing = std::getenv("TWIN_CHARACTER_TRACE") != nullptr;
    if (tracing)
    {
        Native::Log(format, slot, id, clip);
    }
}

// The pose this frame: the clip for Twinsanity's animation, moved on by the game's time (nothing moves while it's paused)
const std::vector<Affine>& PoseFor(const Pack& pack, const InstanceContext* instance, const OgiAnimator* animator)
{
    Playing& playing = g_Playing;
    const AnimationStatus* status = NewestStatus(animator);
    const GameAnimation* animation = AnimationOf(status);
    s32 slot = SlotOf(instance, animation);
    f32 elapsed = 0.0f;
    if (status != nullptr && status == playing.status && status->time > playing.statusTime)
    {
        elapsed = static_cast<f32>(status->time - playing.statusTime) / g_ClockUnitsPerSecond;
    }

    bool fresh = slot != playing.slot || (status != playing.status && status != nullptr && status->time < playing.statusTime);
    playing.status = status;
    playing.statusTime = status != nullptr ? status->time : 0;
    if (fresh)
    {
        const SlotClip* mapping = MappingOf(slot);
        u32 clip = ClipNamed(pack, mapping->clip);
        Trace("characters: Twinsanity slot %d (animation %u) shown as %s", slot, animation != nullptr ? animation->id : 0,
              pack.clips[clip].name.c_str());
        // A looped clip going on in the same clip keeps its place
        bool continues = clip == playing.clip && mapping->timing == Loop && playing.timing == Loop;
        playing.from = playing.pose;
        playing.blend = playing.from.empty() ? 1.0f : 0.0f;
        playing.slot = slot;
        playing.clip = clip;
        playing.timing = mapping->timing;
        playing.speed = mapping->speed;
        if (!continues)
        {
            playing.time = 0.0f;
        }
    }

    const PackClip& clip = pack.clips[playing.clip];
    if (playing.timing == Fit && status != nullptr && status->length > 0)
    {
        f32 share = static_cast<f32>(status->time) / static_cast<f32>(status->length);
        playing.time = std::clamp(share, 0.0f, 1.0f) * static_cast<f32>(clip.frames - 1);
    }
    else
    {
        playing.time += elapsed * clip.rate * playing.speed;
    }

    ClipPose(pack, clip, playing.time, playing.timing == Loop, playing.pose);
    if (playing.blend < 1.0f && playing.from.size() == playing.pose.size())
    {
        playing.blend = std::min(1.0f, playing.blend + elapsed / BlendSeconds);
        for (u32 j = 0; j < pack.jointCount; j++)
        {
            Lerp(playing.from[j], playing.pose[j], playing.blend, &playing.pose[j]);
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
    writer.Write64(ImageDataTag(static_cast<u32>(texture.pixels.size())), 0);
    writer.chain.insert(writer.chain.end(), texture.pixels.begin(), texture.pixels.end());
    writer.Close(tag);
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

// The pack's model space (WoC's: Y up, facing -Z, Crash about 1 tall) into the instance's: turned about Y, scaled to the
// Twinsanity character's height
Placement Place(const Pack& pack, const GameOGI* ogi, const Matrix4x4* matrix)
{
    static const char* turn = std::getenv("TWIN_CHARACTER_TURN");
    if (turn != nullptr)
    {
        g_TurnDegrees = static_cast<f32>(std::atof(turn));
    }

    f32 packHeight = pack.high[1] - pack.low[1];
    f32 height = ogi != nullptr ? ogi->bounds.max.y - ogi->bounds.min.y : 0.0f;
    f32 scale = packHeight > 0.0f && height > 0.0f ? height / packHeight : 1.0f;
    static const char* scaleSetting = std::getenv("TWIN_CHARACTER_SCALE");
    if (scaleSetting != nullptr)
    {
        scale = static_cast<f32>(std::atof(scaleSetting));
    }

    f32 radians = g_TurnDegrees * 3.14159265f / 180.0f;
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
    const std::vector<Affine>& pose = PoseFor(pack, instance, animator);
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
                TextureSlot* slot = ClaimTextureSlot(texture->slotOwner.get(), bucketIndex, &upload);
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
}

namespace NativeCharacters
{
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
    if (!g_ChoiceRead)
    {
        g_ChoiceRead = true;
        if (const char* setting = std::getenv("TWIN_CHARACTER"))
        {
            g_Choice = setting;
        }
    }

    return g_Choice;
}

void SetChoice(const std::string& name)
{
    Choice();
    if (std::getenv("TWIN_CHARACTER") == nullptr)
    {
        g_Choice = name;
    }
}

std::string DisplayName(const std::string& folder)
{
    if (folder.empty())
    {
        return "Twinsanity Crash";
    }

    if (folder == "woc_crash")
    {
        return "Wrath of Cortex Crash";
    }

    return folder;
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
