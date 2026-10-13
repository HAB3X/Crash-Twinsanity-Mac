// Twinsanity Crash's animation slots (his game object's animations, which his scripts and controllers play) and the clips shown
// for them, by the Wrath of Cortex pack's names (characters.cpp; native/CHARACTERS.md, "The mapping"); other packs name their
// clips the same where they have them, and "a|b" falls back to b when a pack has no a. The slots were found with
// $TWIN_CHARACTER_TRACE and the sweep's scripted input ($TWIN_SWEEP_INPUT) on the beach. Loop plays the clip round (a stepping
// clip at the pace the character's speed over the ground asks), Once plays it through and holds its last frame, Fit stretches it
// over Twinsanity's animation; turns spins the model round its up axis that many times over Twinsanity's animation. A slot not
// listed shows the default
constexpr SlotClip g_DefaultClip = {-1, "b/idle", Loop, 1.0f, 0.0f};

constexpr SlotClip g_SlotClips[] = {
    {8, "b/idle", Loop, 1.0f, 0.0f},                  // standing (1.40 s, looped)
    {9, "a/tiptoe|a/walk", Loop, 1.0f, 0.0f},         // creeping, the stick pushed a little (0.48 s, looped)
    {10, "a/walk", Loop, 1.0f, 0.0f},                 // walking (0.76 s, looped)
    {11, "a/sprint|a/run", Loop, 1.0f, 0.0f},         // running (0.56 s, looped): Twinsanity's run is about twice as fast over
                                                      // the ground as WoC's run, whose legs would whirl; its sprint's stride is
                                                      // closer
    {15, "d/cptrspin|a/spin", Fit, 1.0f, 3.0f},       // the spin attack (0.72 s): WoC's own spin is a stretched pose under a
                                                      // tornado model, so the arms-out spin of its copter pack, turned three
                                                      // times
    {16, "f/fall", Loop, 1.0f, 0.0f},                 // falling (0.56 s)
    {19, "b/jump", Fit, 1.0f, 0.0f},                  // a jump from standing (0.66 s)
    {20, "a/runjump|b/jump", Fit, 1.0f, 0.0f},        // a jump while running (0.66 s): WoC's somersault
    {21, "a/starjump|b/jump", Fit, 1.0f, 0.0f},       // the double jump (0.43 s)
    {22, "a/starjump|b/jump", Fit, 1.0f, 0.0f},       // the double jump, the other (0.47 s)
    {29, "a/land", Once, 1.0f, 0.0f},                 // landing still (1.40 s)
    {30, "a/run", Loop, 1.0f, 0.0f},                  // landing on the run (0.92 s): WoC lands straight into its run
    {31, "a/crchdwn|b/idle", Fit, 1.0f, 0.0f},        // crouching down
    {32, "a/crchidle|b/idle", Loop, 1.0f, 0.0f},      // crouched (1.24 s, looped)
    {34, "a/crawl|a/walk", Loop, 1.0f, 0.0f},         // crawling (1.12 s, looped)
    {35, "a/crchstnd|b/idle", Fit, 1.0f, 0.0f},       // standing up from the crouch (0.10 s)
    {36, "a/slidattk|a/run", Once, 1.0f, 0.0f},       // the slide's start (0.15 s)
    {38, "a/slidattk|a/run", Once, 1.0f, 0.0f},       // sliding (0.64 s)
    {97, "b/scratch|b/itch", Fit, 1.0f, 0.0f},        // standing a while: the fidgets (1.24 s)
    {98, "e/pickear|b/itch", Fit, 1.0f, 0.0f},        // (4.68 s)
    {99, "b/sigh|b/itch", Fit, 1.0f, 0.0f},           // (2.04 s)
};
