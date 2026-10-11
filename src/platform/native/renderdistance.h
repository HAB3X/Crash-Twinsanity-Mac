#pragma once

// The render distance option (OptionRenderDistance, the Display tab): how far the game draws. Three things bound it, all scaled
// here: each model's LOD cut-off (`LodMeshAt`: past its farthest distance a model isn't drawn: the grass and small things that
// pop in as the player gets close), the camera lens's far plane in the projection (view.cpp) and in the culling frustum
// (culling.cpp). 0 is the default, "unlimited" (no LOD cut-off, the far plane four times the game's); 3 is retail's
#include "common.h"

#include <atomic>

namespace Native
{
inline std::atomic<u32> g_RenderDistance{0};

inline void SetRenderDistance(u32 choice)
{
    g_RenderDistance.store(choice > 4 ? 4 : choice, std::memory_order_relaxed);
}

// The far plane's and the LODs' cut-off's scale (1: retail's)
inline f32 RenderDistanceScale()
{
    static constexpr f32 Scales[5] = {4.0f, 3.0f, 2.0f, 1.0f, 0.6f};
    return Scales[g_RenderDistance.load(std::memory_order_relaxed)];
}

// No LOD cut-off at all
inline bool RenderDistanceUnlimited()
{
    return g_RenderDistance.load(std::memory_order_relaxed) == 0;
}
}
