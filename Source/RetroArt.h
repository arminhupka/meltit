#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>

// Procedural (vector) artwork for the MELTIT editor. Everything is drawn with juce::Graphics
// so the interface stays sharp at any host scale and needs no embedded bitmaps.
namespace rack::retro
{
using Area = juce::Rectangle<int>;
using AreaF = juce::Rectangle<float>;

juce::Font font(float size, bool bold = false);
juce::Font headingFont(float size);
juce::Font readoutFont(float size);

namespace palette
{
inline const juce::Colour cream { 0xfff6ddb4 };
inline const juce::Colour charcoal { 0xff182427 };
inline const juce::Colour metalInk { 0xff162631 };
inline const juce::Colour amber { 0xfff0a818 };
inline const juce::Colour ledOn { 0xff25dfa6 };
inline const juce::Colour stripeYellow { 0xfffbb714 };
inline const juce::Colour stripeOrange { 0xfff37021 };
inline const juce::Colour stripeRed { 0xffd52a1e };
}

// Shared film-grain texture. Hold a juce::SharedResourcePointer<Grain> while drawing so it is built once.
struct Grain
{
    Grain();
    juce::Image image;
};

enum class Key
{
    generate, previous, next, options, controls, close, toggleOff, toggleOn, inspectorClose
};

float keyAspect(Key key);

void deck(juce::Graphics&, Area);
void header(juce::Graphics&, Area);
// Right edge of the MELTIT badge drawn by headerBadge() in the same header area.
float headerLogoRight(Area);

// One frame of the header's animated badge: the three VHS stripes and the MELTIT picture tube.
struct BadgeState
{
    std::array<float, 3> stripes {}; // how brightly each stripe is lit, 0 when idle
    float roll = 0;                  // position of the bright band rolling down the tube, 0 to 1
    float flicker = 0;               // how far the tube's brightness dips this frame, 0 to 1
    float glitch = 0;                // strength of a picture tear, 0 when the picture is stable
    int seed = 0;                    // varies the tear and the snow from frame to frame
};

// The part of the header the badge (with its glow) covers, and the badge itself drawn over header().
Area headerBadgeArea(Area);
void headerBadge(juce::Graphics&, Area, const BadgeState&);
void rackFrame(juce::Graphics&, Area);
void shelf(juce::Graphics&, Area);
void signal(juce::Graphics&, Area, const std::array<float, 2>& peaks);
// Where the signal line runs across the bar drawn by signal(), from just after "Input" to its arrow head.
AreaF signalLine(Area);
// One effect in the chain shown on the signal line: a small plate in the card's finish with a power lamp.
// `effect` past the last effect draws an empty slot.
void chainChip(juce::Graphics&, AreaF, size_t effect, const juce::String& text, bool on, bool hot);
void presetHousing(juce::Graphics&, Area);

void dock(juce::Graphics&, Area);
void connector(juce::Graphics&, Area, float opacity);
void port(juce::Graphics&, Area, bool pads, float opacity);
void addFrame(juce::Graphics&, Area, float opacity);

void face(juce::Graphics&, Area, size_t effect, float dividerPosition);
// The cassette window on the Tape card, its spools turned by the given angles; `card` is the card's bounds.
AreaF tapeWindowBounds(Area card);
void tapeWindow(juce::Graphics&, Area card, float leftTurn, float rightTurn);
// Card title in that card's own lettering: engraved, script, valve glow, LCD, label tape...
void title(juce::Graphics&, Area, const juce::String& text, size_t effect, float preferred = 17.5f);
// Printed scale around a large knob.
void knobScale(juce::Graphics&, juce::Point<float> centre, float radius, float start, float end, juce::Colour ink);

// Where the four featured controls sit on a 240 x 242 effect card. Hero tiles get a large knob.
struct CardTile
{
    Area bounds;
    bool hero;
};
std::array<CardTile, 4> cardLayout(size_t effect);
// A display screen set into the card face, in the same 240 x 242 frame; empty for cards without one.
Area cardScreen(size_t effect);
// Cards with a quick selector keep a header strip for it; the others give their whole face to the controls.
bool cardHasHeader(size_t effect);
void inspectorPlate(juce::Graphics&, Area, size_t effect);
void seatedCartridge(juce::Graphics&, Area, size_t effect, float opacity);
void shelfCartridge(juce::Graphics&, Area, size_t effect, float opacity);
void knob(juce::Graphics&, juce::Point<float> centre, float diameter, float angle, size_t effect);
// Long soft shadow a knob throws onto the panel; paint it before the knob, from the component beneath it.
void knobShadow(juce::Graphics&, juce::Point<float> centre, float diameter);
// Shadow cast by the dock lip onto a seated cartridge.
void seatShadow(juce::Graphics&, Area cavity);
// Electric current crackling round a frame, played when a cartridge powers up. Two arcs leave the top centre
// and run down both sides; `reach` (0 to 1) is how far they have got, `intensity` scales the whole effect and
// `seed` picks this frame's jitter.
void electricFrame(juce::Graphics&, AreaF frame, float reach, float intensity, juce::uint32 seed);
// Blue light the charged frame throws onto the rack around it; paint it beneath the panel.
void electricBloom(juce::Graphics&, AreaF frame, float intensity);

void key(juce::Graphics&, Key, Area, float opacity);
void lamp(juce::Graphics&, Area, bool on, float opacity);
void valueWell(juce::Graphics&, Area, bool compact);
void selector(juce::Graphics&, Area, bool focused, int chevronWidth, float opacity);
void inspectorSwitch(juce::Graphics&, Area, bool on, float opacity);
void scrollTrack(juce::Graphics&, Area, bool thumb, float opacity);
// A small monitor's glass laid over a screen's content: scan lines, darker corners and a sheen.
void crtGlass(juce::Graphics&, AreaF screen, float radius);
// A line drawn on a phosphor screen, with its bloom.
void crtTrace(juce::Graphics&, const juce::Path& trace, juce::Colour colour, float width);
// VHS tracking trouble over a screen: snow, rolling noise bands with colour fringes and the odd torn line.
// `intensity` runs from 0 (clean) to 1; `seconds` animates it and `seed` keeps screens apart.
void trackingNoise(juce::Graphics&, AreaF screen, float radius, float intensity, double seconds, juce::uint32 seed);
// The on-screen display of a video recorder on the VHS card: PLAY or STOP, the tape speed, a tracking meter and
// the tape counter (seconds) on a blue screen, with tracking noise from `tracking` and `wear` (0 to 1).
void vcrScreen(juce::Graphics&, AreaF screen, const juce::String& speed, bool on, bool playing, double counter,
               float tracking, float wear, double seconds, juce::uint32 seed);
// Glowing VCR-style digital readout: caption and unit on the left, the value in display digits on the right and
// a level ladder lit up to `proportion` (0 to 1) along the bottom,
// or out from its centre when `bipolar`. `lit` brightens it while being adjusted.
// `brightness` (1 is steady) dims or flares the tube for flicker. `bloom` keeps the blurred glow between paints;
// give each readout its own.
struct VfdBloom
{
    juce::String key;
    juce::Image wide, mid, tight;
};
void vfdReadout(juce::Graphics&, Area, const juce::String& caption, const juce::String& value,
                const juce::String& unit, float proportion, bool lit, bool bipolar, float brightness, VfdBloom& bloom);

// Popup menus in the hardware finish: a bevelled metal panel, lit rows, a lamp for the selected entry.
void menuPanel(juce::Graphics&, Area);
void menuItem(juce::Graphics&, Area, const juce::String& text, bool separator, bool enabled, bool highlighted,
              bool ticked, bool hasSubMenu);
// In-app notice plate with an amber warning lamp; `glow` (0 to 1) pulses the lamp.
void notice(juce::Graphics&, Area, const juce::String& heading, const juce::String& message, float glow);
}
