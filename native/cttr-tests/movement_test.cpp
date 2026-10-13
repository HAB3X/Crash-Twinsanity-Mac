// Checks of the Tag Team Racing movement model (src/platform/native/cttrmovement.cpp) against the retail numbers:
//
//   c++ -std=c++20 -I src/platform/native native/cttr-tests/movement_test.cpp src/platform/native/cttrmovement.cpp -o /tmp/mt
//   /tmp/mt

#include "cttrmovement.h"

#include <cmath>
#include <cstdio>

using namespace CttrMovement;

namespace
{
int g_Failures = 0;

void Check(bool condition, const char* what, float value)
{
    std::printf("%s %s (%.4f)\n", condition ? "ok  " : "FAIL", what, value);
    g_Failures += condition ? 0 : 1;
}

constexpr float Dt = 1.0f / 60.0f;
} // namespace

int main()
{
    // Running: from standing to the top speed of 12 in about a quarter second (48 a second net), then held there
    {
        State state;
        Input input;
        input.move = {0.0f, 0.0f, 1.0f};
        float time = 0.0f;
        while (Speed(state) < 11.99f && time < 2.0f)
        {
            Step(state, input, Dt, true);
            time += Dt;
        }
        Check(std::fabs(time - 0.25f) < 0.03f, "reaches the top speed in 0.25 s", time);
        for (int i = 0; i < 60; i++)
        {
            Step(state, input, Dt, true);
        }
        Check(std::fabs(Speed(state) - 12.0f) < 0.01f, "holds 12", Speed(state));
        input.move = {};
        time = 0.0f;
        while (Speed(state) > 0.0f && time < 2.0f)
        {
            Step(state, input, Dt, true);
            time += Dt;
        }
        Check(std::fabs(time - 0.25f) < 0.03f, "stops in 0.25 s", time);
    }

    // A jump: 4.1 up in 0.4 s, stopping at the top, then a cubic fall held to 30 a second
    {
        State state;
        Input jump;
        jump.jump = true;
        float height = 0.0f, top = 0.0f, topTime = 0.0f, time = 0.0f;
        height += Step(state, jump, Dt, true).y;
        time += Dt;
        Input none;
        float fastest = 0.0f;
        while (time < 2.0f)
        {
            float dy = Step(state, none, Dt, false).y;
            height += dy;
            time += Dt;
            if (height > top)
            {
                top = height;
                topTime = time;
            }
            fastest = std::fmin(fastest, dy / Dt);
        }
        Check(std::fabs(top - 4.1f) < 0.01f, "a jump rises 4.1", top);
        Check(std::fabs(topTime - 0.4f) < 0.02f, "its top at 0.4 s", topTime);
        Check(std::fabs(fastest + 30.0f) < 0.01f, "falls at 30 a second at most", fastest);
        Check(IsFalling(state), "falling after the top", 0.0f);
        Land(state);
        Check(state.mode == Mode::Ground, "landed", 0.0f);
    }

    // A double jump at the top: 2.1 more, once
    {
        State state;
        Input jump;
        jump.jump = true;
        Input none;
        float height = Step(state, jump, Dt, true).y;
        for (int i = 0; i < 23; i++)
        {
            height += Step(state, none, Dt, false).y;
        }
        float before = height;
        height += Step(state, jump, Dt, false).y;
        float top = height;
        for (int i = 0; i < 40; i++)
        {
            height += Step(state, none, Dt, false).y;
            top = std::fmax(top, height);
        }
        Check(std::fabs(top - before - 2.1f) < 0.05f, "a double jump rises 2.1 more", top - before);
        float again = height;
        height += Step(state, jump, Dt, false).y;
        Check(height <= again, "only once", height - again);
    }

    // A belly flop: the rest of the rise, a 0.3 s hold (sped up 1.3 times), then a faster fall
    {
        State state;
        Input jump;
        jump.jump = true;
        Input flop;
        flop.bellyFlop = true;
        Input none;
        Step(state, jump, Dt, true);
        Step(state, flop, Dt, false);
        bool held = false;
        for (int i = 0; i < 60; i++)
        {
            Step(state, none, Dt, false);
            held = held || state.mode == Mode::BellyFlopHold;
        }
        Check(held, "holds before the fall", 0.0f);
        Check(IsBellyFlopping(state), "belly flopping as it falls", 0.0f);
    }

    std::printf("%d failure%s\n", g_Failures, g_Failures == 1 ? "" : "s");
    return g_Failures == 0 ? 0 : 1;
}
