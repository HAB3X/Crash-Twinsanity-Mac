// Crash Tag Team Racing's on-foot movement (cttrmovement.h): the retail functions' logic, from our own decompilation of
// SLUS_211.91, written again in plain C++. The addresses are the retail ones.

#include "cttrmovement.h"

#include <algorithm>
#include <cmath>

namespace CttrMovement
{
namespace
{
// 0x559128: the mode an arc's end leads to (a fall after any jump, nothing after standing)
constexpr Mode NextMode[ArcModes] = {Mode::Ground, Mode::Falling, Mode::Falling, Mode::Falling, Mode::Falling};

float Sign(float value)
{
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

float Length(const Vec3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// 0x22CDF0: an arc of `height` over `duration` seconds. A positive duration counts t down from 1 (a rise that slows to a
// stop), a negative one counts it up from 0 (a fall that speeds up); 0 is no arc
void StartArc(Arc& arc, float duration, float height)
{
    arc.direction = {0.0f, height, 0.0f};
    if (duration == 0.0f)
    {
        arc.t = 0.0f;
        arc.rate = 0.0f;
    }
    else
    {
        arc.rate = 1.0f / -duration;
        arc.t = (Sign(arc.rate) - 1.0f) * -0.5f;
    }
    arc.previous = 0.0f;
}

// 0x22C2D8: the arc moved on by dt; the displacement added to `out`. Its curve is t^3 * s + (1 - s) / 2 for the rate's sign s
// (t^3 going up, 1 - t^3 coming down). Returns the time left over when a counting-down arc has ended (its rate then 0)
float StepArc(Arc& arc, float dt, Vec3& out)
{
    float overshoot = 0.0f;
    if (arc.rate == 0.0f)
    {
        return 0.0f;
    }

    arc.t += dt * arc.rate;
    if (arc.t < 0.0f)
    {
        overshoot = std::fabs(arc.t / arc.rate);
        arc.t = 0.0f;
    }

    float s = Sign(arc.rate);
    float value = arc.t * arc.t * arc.t * s + (s - 1.0f) * -0.5f;
    float step = value - arc.previous;
    arc.previous = value;
    out.x += arc.direction.x * step;
    out.y += arc.direction.y * step;
    out.z += arc.direction.z * step;
    if (overshoot > 0.0f)
    {
        arc.rate = 0.0f;
    }

    return overshoot;
}

// 0x22CAF0: a new vertical mode, its arc started from the tables (a mode it's already in is left alone)
void SetMode(State& state, Mode mode, const Tuning& tuning)
{
    if (mode == state.mode)
    {
        return;
    }

    state.mode = mode;
    int index = static_cast<int>(mode);
    if (index < ArcModes)
    {
        StartArc(state.arc, tuning.arcs[index].duration, tuning.arcs[index].height * tuning.scale);
        state.next = NextMode[index];
    }
}

// 0x22C460: the velocity across the ground. It loses `topSpeed / slowDownTime` a second by itself, gains the stick times
// `topSpeed * (1 / speedUpTime + 1 / slowDownTime)`, both times the ground or air strength, and is held to the top speed
Vec3 IntegrateHorizontal(State& state, const Vec3& move, float dt, float strength, const Tuning& tuning)
{
    float top = tuning.topSpeed * tuning.scale;
    float slowDown = 1.0f / tuning.slowDownTime;
    float speedUp = 1.0f / tuning.speedUpTime;
    Vec3& v = state.velocity;
    float speed = Length(v);
    if (speed > 0.0f)
    {
        float slower = speed - dt * slowDown * top * strength;
        if (slower < 0.0f)
        {
            v = {};
        }
        else
        {
            float k = slower / speed;
            v = {v.x * k, v.y * k, v.z * k};
        }
    }

    float push = strength * top * dt * (speedUp + slowDown);
    v = {move.x * push + v.x, move.y * push + v.y, move.z * push + v.z};
    speed = Length(v);
    if (speed > top)
    {
        float k = top / speed;
        v = {v.x * k, v.y * k, v.z * k};
    }

    return {v.x * dt, v.y * dt, v.z * dt};
}
} // namespace

Vec3 Step(State& state, const Input& input, float dt, bool onGround, const Tuning& tuning)
{
    // The requests (0x2208C0 asks for a jump, 0x220B00 for a belly flop), taken by the update (0x21E750)
    if (input.jump)
    {
        state.jumpRequested = true;
    }

    if (input.bellyFlop && !state.bellyFlop &&
        (state.mode == Mode::Jumping || state.mode == Mode::DoubleJumping || state.mode == Mode::Falling))
    {
        state.bellyFlop = true;
        state.next = Mode::BellyFlopHold;
    }

    if (state.jumpRequested)
    {
        state.jumpRequested = false;
        if (state.mode == Mode::Ground)
        {
            if (onGround)
            {
                SetMode(state, Mode::Jumping, tuning);
            }
        }
        else if (!state.bellyFlop && !state.doubleJumpUsed &&
                 (state.mode == Mode::Jumping || (state.mode == Mode::Falling && state.arc.t <= 1.0f)))
        {
            // A double jump from the rise, or from a fall's first part (its t still under 1)
            SetMode(state, Mode::DoubleJumping, tuning);
            state.doubleJumpUsed = true;
        }
    }

    // Across the ground: full strength standing, half in the air (0x22D0C8/0x22D0D0 with 1 or 0.5)
    float strength = state.mode == Mode::Ground ? 1.0f : tuning.airControl;
    Vec3 move = input.move;
    float push = std::max(std::fabs(move.x), std::fabs(move.z));
    float length = std::sqrt(move.x * move.x + move.z * move.z);
    if (length > 0.0f)
    {
        move = {move.x / length * push, 0.0f, move.z / length * push};
    }
    Vec3 out = IntegrateHorizontal(state, move, dt, strength, tuning);

    // Up and down
    Vec3 vertical;
    if (state.mode != Mode::Ground)
    {
        if (state.mode == Mode::Falling && state.bellyFlop && state.next != Mode::Falling)
        {
            SetMode(state, state.next, tuning);
            state.arc.rate *= tuning.bellyFlopRate;
        }

        float overshoot = StepArc(state.arc, dt, vertical);
        if (overshoot > 0.0f)
        {
            SetMode(state, state.next, tuning);
            if (state.bellyFlop)
            {
                state.arc.rate *= tuning.bellyFlopRate;
            }
            StepArc(state.arc, overshoot, vertical);
        }
    }

    // In the air, a frame's fall is held to the terminal speed
    if (!onGround)
    {
        vertical.y = std::max(vertical.y, -tuning.terminalFall * tuning.scale * dt);
    }

    out.y += vertical.y;
    return out;
}

void Land(State& state)
{
    state.mode = Mode::Ground;
    state.next = Mode::Ground;
    state.arc = {};
    state.doubleJumpUsed = false;
    state.bellyFlop = false;
}

void WalkOff(State& state, const Tuning& tuning)
{
    if (state.mode == Mode::Ground)
    {
        SetMode(state, Mode::Falling, tuning);
    }
}

bool IsJumping(const State& state)
{
    return state.mode == Mode::Jumping || state.mode == Mode::DoubleJumping;
}

bool IsFalling(const State& state)
{
    return state.mode == Mode::Falling;
}

bool IsBellyFlopping(const State& state)
{
    return state.bellyFlop && (state.mode == Mode::BellyFlopHold || state.mode == Mode::Falling);
}

float Speed(const State& state)
{
    return std::sqrt(state.velocity.x * state.velocity.x + state.velocity.z * state.velocity.z);
}
} // namespace CttrMovement
