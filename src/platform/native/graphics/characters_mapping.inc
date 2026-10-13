// Twinsanity Crash's animation slots (his game object's animations, which his scripts and controllers play) and the Wrath of
// Cortex clips shown for them (characters.cpp; native/CHARACTERS.md, "The mapping"). The slots were found with
// $TWIN_CHARACTER_TRACE and the sweep's scripted input ($TWIN_SWEEP_INPUT) on the beach. Loop plays the clip round at its own
// speed (times speed), Once plays it through and holds its last frame, Fit stretches it over Twinsanity's animation; turns spins
// the model round its up axis that many times over Twinsanity's animation (WoC's spin is a pose the game turns, with its
// tornado drawn over). A slot not listed shows the default
constexpr SlotClip g_DefaultClip = {-1, "b/idle", Loop, 1.0f, 0.0f};

constexpr SlotClip g_SlotClips[] = {
    {8, "b/idle", Loop, 1.0f, 0.0f},          // standing (1.40 s, looped)
    {10, "a/walk", Loop, 1.0f, 0.0f},         // walking (0.76 s, looped)
    {11, "a/run", Loop, 1.0f, 0.0f},          // running (0.56 s, looped)
    {15, "a/spin", Fit, 1.0f, 3.0f},          // the spin attack (0.72 s)
    {16, "f/fall", Loop, 1.0f, 0.0f},         // falling (0.56 s)
    {19, "b/jump", Fit, 1.0f, 0.0f},          // a jump from standing (0.66 s)
    {20, "a/runjump", Fit, 1.0f, 0.0f},       // a jump while running (0.66 s)
    {22, "a/starjump", Fit, 1.0f, 0.0f},      // the double jump (0.47 s)
    {29, "a/land", Once, 1.0f, 0.0f},         // landing still (1.40 s)
    {30, "a/land", Once, 1.5f, 0.0f},         // landing on the run (0.92 s)
    {31, "a/crchdwn", Fit, 1.0f, 0.0f},       // crouching down
    {32, "a/crchidle", Loop, 1.0f, 0.0f},     // crouched (1.24 s, looped)
    {34, "a/crawl", Loop, 1.0f, 0.0f},        // crawling (1.12 s, looped)
    {35, "a/crchstnd", Fit, 1.0f, 0.0f},      // standing up from the crouch (0.10 s)
    {36, "a/slidattk", Once, 1.0f, 0.0f},     // the slide's start (0.15 s)
    {38, "a/slidattk", Once, 1.0f, 0.0f},     // sliding (0.64 s)
};
