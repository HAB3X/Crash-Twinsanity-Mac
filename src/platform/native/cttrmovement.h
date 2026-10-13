#pragma once

// Crash Tag Team Racing's on-foot movement, reimplemented from the retail PS2 executable (SLUS_211.91; our own decompilation,
// ~/Documents/decomps/cttr-ours/NOTES.md): the "Original" physics for the Tag Team Racing character pack. Collision stays the
// host's: it says when the character stands on something, the model says how he moves.
//
// The retail object (the on-foot mover, constructed at 0x21E3E0, updated at 0x21E750) moves in two parts:
// - Across the ground (0x22C460): a velocity that speeds up towards the stick and slows down by itself, capped at a top speed;
//   half as strong in the air.
// - Up and down (0x22CDF0 starts an arc, 0x22C2D8 steps it): no gravity as such, but cubic arcs per vertical mode. A jump rises
//   its height along 1 - (1 - t)^3 over its time, so it stops at the top; a fall drops along t^3 (and keeps going: t passes 1),
//   held to the terminal speed. When an arc ends, the mode's next one starts (0x22CAF0, the table at 0x559128: a fall).

#include <cstdint>

namespace CttrMovement
{
struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// The vertical modes (the mover's +0xD8 bits 16-19). Retail's 5 (hanging from a ledge), 6 and 7 come with its ledge code,
// which the host's own takes the place of
enum class Mode : uint8_t
{
    Ground = 0,
    Falling = 1,
    Jumping = 2,
    DoubleJumping = 3,
    BellyFlopHold = 4,
};

constexpr int ArcModes = 5;

// A vertical mode's arc: its time (negative: it counts up, the fall's cubic) and its height (0x5124A8 and 0x5124C0)
struct ArcSpec
{
    float duration;
    float height;
};

struct Tuning
{
    // 0x21E3E0: the horizontal integrator's top speed (12) and its speeding-up and slowing-down times (1 / 0.25 each)
    float topSpeed = 12.0f;
    float speedUpTime = 0.25f;
    float slowDownTime = 0.25f;
    // 0x21E750: the integrator's two strengths, 1 on the ground, 0.5 in the air
    float airControl = 0.5f;
    // 0x21E750: a frame's fall is at most dt * 30
    float terminalFall = 30.0f;
    // 0x5124A8, 0x5124C0
    ArcSpec arcs[ArcModes] = {{0.0f, 0.0f}, {-0.4f, -4.1f}, {0.4f, 4.1f}, {0.4f, 2.1f}, {0.3f, 0.0f}};
    // 0x21E750: a belly flop's arcs run 1.3 times as fast (0x22CF70 with 0x3FA66666)
    float bellyFlopRate = 1.3f;
    // Retail's units to the host's (CTTR's on-foot Crash is 2.58 units tall: Twinsanity's is 1.95)
    float scale = 1.0f;
};

// An arc (the mover's +0x34): its direction (the height as a vector), its time t, the rate t moves at, the curve's last value
struct Arc
{
    Vec3 direction;
    float t = 0.0f;
    float rate = 0.0f;
    float previous = 0.0f;
};

struct State
{
    Vec3 velocity;
    Mode mode = Mode::Ground;
    Mode next = Mode::Ground;
    Arc arc;
    bool doubleJumpUsed = false;
    bool bellyFlop = false;
    bool jumpRequested = false;
};

struct Input
{
    // The wanted direction across the ground, its length the stick's push (0 to 1)
    Vec3 move;
    bool jump = false;
    bool bellyFlop = false;
};

// One step: the displacement to try (the host collides it); `onGround` says whether the host has the character standing
Vec3 Step(State& state, const Input& input, float dt, bool onGround, const Tuning& tuning = {});

// The host found ground under a falling character: the landing (the arcs and the double jump reset)
void Land(State& state);

// The host lost the ground under a standing character: the fall starts
void WalkOff(State& state, const Tuning& tuning = {});

// What the fight tree's conditions ask (0x22CD30, 0x22CCE8, 0x220AC0)
bool IsJumping(const State& state);
bool IsFalling(const State& state);
bool IsBellyFlopping(const State& state);
float Speed(const State& state);
} // namespace CttrMovement
