#include "RetroArt.h"
#include "CartridgeArt.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <tuple>
#include <utility>

namespace rack::retro
{
namespace
{
using juce::Colour;
namespace Colours = juce::Colours;
using juce::ColourGradient;
using juce::Path;
using Point = juce::Point<float>;
using juce::MathConstants;

constexpr float twoPi = MathConstants<float>::twoPi;

juce::Font withTypeface(const juce::Typeface::Ptr& typeface, float size)
{
    return typeface != nullptr ? juce::Font(juce::FontOptions(typeface).withHeight(size))
                               : juce::Font(juce::FontOptions(size));
}

struct Theme
{
    Colour face, inspector;
};

// Panel colours follow the original hand-painted finishes.
constexpr std::array<juce::uint32, 11> faceColours {
    0xffc8b38c, 0xff8a3a2c, 0xff9c7040, 0xff44515b, 0xffc2581c, 0xff4e3444, 0xff5a613d, 0xff3c5a5f, 0xff2d2b52,
    0xff2b2e33, 0xff1f4a3b };
constexpr std::array<juce::uint32, 11> inspectorColours {
    0xff4c443f, 0xff4d393a, 0xff4a3c31, 0xff384044, 0xff4f3e30, 0xff403742, 0xff3e4238, 0xff2f4044, 0xff2c2a42,
    0xff2a2d31, 0xff233a32 };
// The VHS sleeve's rainbow, red through blue.
constexpr std::array<juce::uint32, 5> vhsRainbow { 0xffe8362f, 0xfff28c1c, 0xfff5cf1d, 0xff3fb24c, 0xff2c7fd6 };
const Colour stretchAmber { 0xffffb347 };
// The Grain card's gold metal flake and its screen's phosphor.
const Colour grainGold { 0xffe6c15a };
const Colour grainMint { 0xff8ff5c4 };

// Cards with a quick selector keep a header strip for it; the others give the whole face to their controls.
bool hasHeader(size_t effect)
{
    return effect == 0 || effect == 4 || effect == 5 || effect == 7 || effect == 8 || effect == 9 || effect == 10;
}

// Card layouts are written for controls between y = 44 and 226. Cards without a header stretch that range
// up to y = 16, so their controls and ornaments grow into the space.
float stretchedY(size_t effect, float y)
{
    constexpr float top = 44.f, bottom = 226.f, headerlessTop = 16.f;
    if (hasHeader(effect) || y < top)
        return y;
    return headerlessTop + (y - top) * (bottom - headerlessTop) / (bottom - top);
}

size_t safeEffect(size_t effect)
{
    constexpr size_t graphiteBlue = 3;
    return effect < faceColours.size() ? effect : graphiteBlue;
}

const juce::Image& grainImage()
{
    const juce::SharedResourcePointer<Grain> grain;
    return grain->image;
}

void grain(juce::Graphics& g, const Path& shape, float strength)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setTiledImageFill(grainImage(), 0, 0, strength);
    g.fillPath(shape);
}

Path rounded(AreaF area, float radius)
{
    Path path;
    path.addRoundedRectangle(area, radius);
    return path;
}

Point polar(Point centre, float radius, float angle)
{
    return { centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle) };
}

void verticalFill(juce::Graphics& g, AreaF area, Colour top, Colour bottom)
{
    g.setGradientFill(ColourGradient(top, area.getX(), area.getY(), bottom, area.getX(), area.getBottom(), false));
}

void topHighlight(juce::Graphics& g, AreaF area, float radius, float alpha)
{
    g.setColour(Colours::white.withAlpha(alpha));
    g.drawLine(area.getX() + radius, area.getY() + 1.f, area.getRight() - radius, area.getY() + 1.f, 1.f);
}

// Height of the side wall of a raised part.
float wall(AreaF area)
{
    return juce::jlimit(1.f, 5.f, area.getHeight() * .07f);
}

// Raised part lit from the top left: contact shadow, a visible side wall and a bevelled, convex face.
void raised(juce::Graphics& g, AreaF area, float radius, Colour top, Colour bottom, float highlight = .12f)
{
    const auto face = area.withTrimmedBottom(wall(area));
    g.setColour(Colours::black.withAlpha(.18f));
    g.fillRoundedRectangle(area.translated(2.f, 3.5f).expanded(1.5f), radius + 1.5f);
    g.setColour(Colours::black.withAlpha(.35f));
    g.fillRoundedRectangle(area.translated(1.f, 1.5f), radius);

    verticalFill(g, area, bottom.darker(.35f), bottom.darker(1.1f));
    g.fillRoundedRectangle(area, radius);

    verticalFill(g, face, top, bottom);
    g.fillRoundedRectangle(face, radius);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(highlight * .45f), 0, face.getY(),
                                     Colours::transparentWhite, 0, face.getY() + face.getHeight() * .55f, false));
    g.fillRoundedRectangle(face.reduced(1.f), radius);

    // Chamfer: lit along the top-left edge, shaded along the bottom-right edge.
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(juce::jmin(.7f, highlight * 2.4f)), face.getX(), face.getY(),
                                     Colours::black.withAlpha(.3f), face.getX() + face.getWidth() * .3f, face.getBottom(), false));
    g.drawRoundedRectangle(face.reduced(1.f), juce::jmax(0.f, radius - 1.f), 1.3f);
    g.setColour(Colours::black.withAlpha(.9f));
    g.drawRoundedRectangle(area.reduced(.5f), radius, 1.f);
}

// Sunken well: deep inner shadow under the top and left edges and a lit lower lip.
void recess(juce::Graphics& g, AreaF area, float radius, Colour fill, float lip = .08f)
{
    g.setColour(Colours::white.withAlpha(lip));
    g.drawLine(area.getX() + radius, area.getBottom() + .7f, area.getRight() - radius, area.getBottom() + .7f, 1.2f);
    g.setColour(fill);
    g.fillRoundedRectangle(area, radius);
    const auto depth = juce::jmin(13.f, area.getHeight() * .5f);
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(.78f), 0, area.getY(),
                                     Colours::transparentBlack, 0, area.getY() + depth, false));
    g.fillRoundedRectangle(area, radius);
    const auto side = juce::jmin(depth * .6f, area.getWidth() * .3f);
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(.42f), area.getX(), 0,
                                     Colours::transparentBlack, area.getX() + side, 0, false));
    g.fillRoundedRectangle(area, radius);
    g.setGradientFill(ColourGradient(Colours::transparentBlack, area.getRight() - side * .6f, 0,
                                     Colours::black.withAlpha(.22f), area.getRight(), 0, false));
    g.fillRoundedRectangle(area, radius);
    g.setGradientFill(ColourGradient(Colours::transparentWhite, 0, area.getBottom() - depth * .6f,
                                     Colours::white.withAlpha(.04f), 0, area.getBottom(), false));
    g.fillRoundedRectangle(area, radius);
    g.setColour(Colours::black.withAlpha(.9f));
    g.drawRoundedRectangle(area.reduced(.5f), radius, 1.f);
}

void screw(juce::Graphics& g, Point centre, float radius, float turn = .5f)
{
    const AreaF head(centre.x - radius, centre.y - radius, radius * 2, radius * 2);
    g.setColour(Colours::black.withAlpha(.5f));
    g.fillEllipse(head.translated(0, radius * .25f).expanded(radius * .15f));
    g.setGradientFill(ColourGradient(Colour(0xffb8bcc0), head.getX(), head.getY(),
                                     Colour(0xff2f3337), head.getRight(), head.getBottom(), false));
    g.fillEllipse(head);
    g.setColour(Colours::black.withAlpha(.75f));
    g.drawEllipse(head, .8f);
    g.setColour(Colour(0xff111315));
    g.drawLine(juce::Line<float>(polar(centre, radius * .72f, turn), polar(centre, radius * .72f, turn + MathConstants<float>::pi)),
               juce::jmax(1.f, radius * .32f));
}

void grille(juce::Graphics& g, AreaF area, int lines)
{
    const auto pitch = area.getHeight() / static_cast<float>(lines);
    for (int i = 0; i < lines; ++i)
    {
        const auto y = area.getY() + pitch * (i + .5f);
        g.setColour(Colours::black.withAlpha(.75f));
        g.fillRoundedRectangle(area.getX(), y - pitch * .28f, area.getWidth(), pitch * .5f, pitch * .25f);
        g.setColour(Colours::white.withAlpha(.13f));
        g.drawLine(area.getX() + 1, y + pitch * .3f, area.getRight() - 1, y + pitch * .3f, .7f);
    }
}

// Three slanted bands from the 70s and 80s tape packaging.
Path stripePath(AreaF band, float slant, size_t i)
{
    const auto width = band.getWidth() / 3.f;
    const auto x = band.getX() + width * static_cast<float>(i);
    Path stripe;
    stripe.startNewSubPath(x + slant, band.getY());
    stripe.lineTo(x + width + slant + .6f, band.getY());
    stripe.lineTo(x + width + .6f, band.getBottom());
    stripe.lineTo(x, band.getBottom());
    stripe.closeSubPath();
    return stripe;
}

const std::array<Colour, 3>& stripeColours()
{
    static const std::array<Colour, 3> colours { palette::stripeYellow, palette::stripeOrange, palette::stripeRed };
    return colours;
}

void stripes(juce::Graphics& g, AreaF band, float slant, float shadowAlpha = 0)
{
    for (size_t i = 0; i < 3; ++i)
    {
        g.setColour(shadowAlpha > 0 ? Colours::black.withAlpha(shadowAlpha) : stripeColours()[i]);
        g.fillPath(stripePath(band, slant, i));
    }
}

void italicText(juce::Graphics& g, const juce::String& text, const juce::Font& typeface, AreaF area,
                juce::Justification justification, Colour colour)
{
    juce::GlyphArrangement glyphs;
    glyphs.addFittedText(typeface, text, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                         justification, 1, .7f);
    Path outline;
    glyphs.createPath(outline);
    const auto centre = area.getCentre();
    outline.applyTransform(juce::AffineTransform::translation(-centre.x, -centre.y)
                               .sheared(-.2f, 0)
                               .translated(centre.x, centre.y));
    g.setColour(colour);
    g.fillPath(outline);
}

void centredText(juce::Graphics& g, const juce::String& text, const juce::Font& typeface, AreaF area, Colour colour)
{
    g.setColour(colour);
    g.setFont(typeface);
    g.drawFittedText(text, area.toNearestInt(), juce::Justification::centred, 1, .8f);
}

AreaF fit(AreaF available, float aspect)
{
    const auto width = juce::jmin(available.getWidth(), available.getHeight() * aspect);
    return available.withSizeKeepingCentre(width, width / aspect);
}

// ---------------------------------------------------------------- knobs
// Studio-lit hardware seen from straight above; the lamp sits above the upper-left corner.

AreaF circle(Point centre, float radius)
{
    return { centre.x - radius, centre.y - radius, radius * 2, radius * 2 };
}

// Dark ring where a part meets the panel. The long cast shadow is painted beneath the whole tile (knobShadow).
void contactShadow(juce::Graphics& g, Point centre, float radius)
{
    ColourGradient contact(Colours::black.withAlpha(.75f), centre.x, centre.y, Colours::transparentBlack,
                           centre.x + radius * 1.16f, centre.y, true);
    contact.addColour(.85, Colours::black.withAlpha(.65f));
    g.setGradientFill(contact);
    g.fillEllipse(circle(centre.translated(radius * .02f, radius * .05f), radius * 1.16f));
}

// Narrow chamfer around a round edge: catches the lamp on the upper left, falls into shade on the lower right.
void chamfer(juce::Graphics& g, Point centre, float radius, Colour base)
{
    g.setGradientFill(ColourGradient(base.interpolatedWith(Colours::white, .3f), centre.x - radius * .7f, centre.y - radius * .7f,
                                     base.darker(.9f), centre.x + radius * .7f, centre.y + radius * .7f, false));
    g.fillEllipse(circle(centre, radius));
    g.setColour(Colours::black.withAlpha(.5f));
    g.drawEllipse(circle(centre, radius - .35f), .7f);
}

// Matte moulded or anodised face with a broad, faint reflection of the lamp.
void matte(juce::Graphics& g, Point centre, float radius, Colour base, bool dished = false)
{
    const auto lit = base.interpolatedWith(Colours::white, .08f), shade = base.darker(.3f);
    g.setGradientFill(ColourGradient(dished ? shade : lit, centre.x - radius * .55f, centre.y - radius * .65f,
                                     dished ? lit : shade, centre.x + radius * .6f, centre.y + radius * .8f, false));
    g.fillEllipse(circle(centre, radius));
    const auto spot = centre.translated(-radius * .3f, -radius * .38f);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(dished ? .02f : .07f), spot.x, spot.y,
                                     Colours::transparentWhite, spot.x + radius * .9f, spot.y, true));
    g.fillEllipse(circle(centre, radius));
}

// Light caught by the rounded rim on the side facing the lamp.
void edgeLight(juce::Graphics& g, Point centre, float radius, float strength = 1.f)
{
    Path arc;
    arc.addCentredArc(centre.x, centre.y, radius - .8f, radius - .8f, 0, -2.5f, -.35f, true);
    g.setColour(Colours::white.withAlpha(.22f * strength));
    g.strokePath(arc, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    Path glint;
    glint.addCentredArc(centre.x, centre.y, radius - .8f, radius - .8f, 0, -1.75f, -1.05f, true);
    g.setColour(Colours::white.withAlpha(.3f * strength));
    g.strokePath(glint, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// Turned metal: a smooth conical sheen with two opposite streaks and fine lathe rings.
void turned(juce::Graphics& g, Point centre, float radius, Colour dark, Colour light, float contrast = 1.f)
{
    const auto disc = circle(centre, radius);
    constexpr int wedges = 180;
    constexpr float step = twoPi / wedges;
    constexpr float streak = -.78f; // Upper left, towards the lamp.
    g.setColour(dark.interpolatedWith(light, .4f));
    g.fillEllipse(disc);
    for (int i = 0; i < wedges; ++i)
    {
        const auto a = step * static_cast<float>(i);
        const auto mid = a + step * .5f;
        auto t = .5f + .5f * std::cos(2.f * (mid - streak));
        t = std::pow(t, 2.4f);
        const auto ripple = .5f + .5f * std::cos(6.f * mid + 1.3f);
        const auto value = juce::jlimit(0.f, 1.f, .22f + contrast * (.66f * t + .1f * ripple - .1f));
        Path wedge;
        wedge.addPieSegment(disc, a, a + step * 1.7f, 0);
        g.setColour(dark.interpolatedWith(light, value));
        g.fillPath(wedge);
    }
    for (float ring = radius - .9f; ring > 1.2f; ring -= 1.15f)
    {
        g.setColour((static_cast<int>(ring * 2.f) % 2 == 0 ? Colours::white : Colours::black).withAlpha(.035f));
        g.drawEllipse(circle(centre, ring), .5f);
    }
    g.setGradientFill(ColourGradient(Colours::transparentBlack, centre.x, centre.y,
                                     Colours::black.withAlpha(.2f), centre.x + radius, centre.y, true));
    g.fillEllipse(disc);
}

// Shadow a raised cap throws onto the part beneath it.
void capShadow(juce::Graphics& g, Point centre, float radius)
{
    const auto s = centre.translated(radius * .12f, radius * .18f);
    ColourGradient shade(Colours::black.withAlpha(.65f), s.x, s.y, Colours::transparentBlack, s.x + radius * 1.35f, s.y, true);
    shade.addColour(.7, Colours::black.withAlpha(.4f));
    g.setGradientFill(shade);
    g.fillEllipse(circle(s, radius * 1.35f));
}

// Thin painted index line, slightly sunk into the surface.
void indexLine(juce::Graphics& g, Point centre, float angle, float from, float to, float width, Colour colour)
{
    Path line;
    line.startNewSubPath(polar(centre, from, angle));
    line.lineTo(polar(centre, to, angle));
    const juce::PathStrokeType stroke(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour(Colours::black.withAlpha(.35f));
    g.strokePath(line, stroke, juce::AffineTransform::translation(.4f, .6f));
    g.setColour(colour);
    g.strokePath(line, stroke);
}

void indexDot(juce::Graphics& g, Point centre, float angle, float distance, float size, Colour colour)
{
    const auto at = polar(centre, distance, angle);
    g.setColour(Colours::black.withAlpha(.35f));
    g.fillEllipse(circle(at.translated(.4f, .6f), size));
    g.setColour(colour);
    g.fillEllipse(circle(at, size));
    g.setColour(Colours::white.withAlpha(.5f));
    g.fillEllipse(circle(at.translated(-size * .3f, -size * .3f), size * .35f));
}

void dome(juce::Graphics& g, Point centre, float radius, Colour light, Colour dark)
{
    g.setGradientFill(ColourGradient(light, centre.x - radius * .3f, centre.y - radius * .5f,
                                     dark, centre.x + radius * .45f, centre.y + radius, true));
    g.fillEllipse(circle(centre, radius));
}

// Polished dome: tight highlight towards the lamp, dark horizon band and light bounced up from the panel.
void chromeDome(juce::Graphics& g, Point centre, float radius)
{
    const auto hot = centre.translated(-radius * .35f, -radius * .4f);
    ColourGradient metal(Colour(0xfffdfdfd), hot.x, hot.y, Colour(0xffb9bcc0), hot.x + radius * 1.6f, hot.y, true);
    metal.addColour(.12, Colour(0xffe9ebed));
    metal.addColour(.45, Colour(0xff8c9095));
    metal.addColour(.72, Colour(0xff3f4245));
    metal.addColour(.88, Colour(0xff8d9196));
    g.setGradientFill(metal);
    g.fillEllipse(circle(centre, radius));
}

// ---------------------------------------------------------------- cartridge label art

void labelArt(juce::Graphics& g, AreaF a, size_t effect)
{
    const auto at = [a](float x, float y) { return Point(a.getX() + a.getWidth() * x, a.getY() + a.getHeight() * y); };
    const auto h = a.getHeight();
    const auto stroke = [&g](const Path& path, float width, Colour colour)
    {
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(width, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    };
    const auto band = [&](float y0, float y1, float thickness, Colour colour)
    {
        Path p;
        p.startNewSubPath(at(-.1f, y0));
        p.lineTo(at(1.1f, y1));
        stroke(p, thickness * h, colour);
    };

    switch (effect)
    {
        case 0: // Tape: sunset bands and two reels.
        {
            verticalFill(g, a, Colour(0xfffde3bd), Colour(0xfffbb978));
            g.fillRect(a);
            band(.98f, .40f, .10f, Colour(0xfff2580b));
            band(1.12f, .54f, .09f, Colour(0xfffc8700));
            band(.84f, .26f, .05f, Colour(0xfff6a03c));
            band(1.26f, .68f, .08f, Colour(0xffc94a12));
            for (const auto& [x, y, w] : { std::tuple { .36f, .36f, .40f }, std::tuple { .72f, .60f, .34f } })
            {
                const AreaF reel(at(x - w * .5f, y - w * .42f), at(x + w * .5f, y + w * .42f));
                g.setColour(Colour(0xffc94a12).withAlpha(.6f));
                g.drawEllipse(reel.expanded(1.5f), h * .085f);
                g.setColour(Colour(0xffffe6c6));
                g.drawEllipse(reel, h * .06f);
            }
            break;
        }
        case 1: // Tremolo: green oscilloscope.
        {
            g.setColour(Colour(0xff0b1515));
            g.fillRect(a);
            for (auto y : { .2f, .8f })
            {
                Path rail;
                rail.startNewSubPath(at(0, y));
                rail.lineTo(at(1, y));
                stroke(rail, h * .035f, Colour(0xff2f7f5c));
            }
            for (int layer = 0; layer < 2; ++layer)
            {
                Path wave;
                const auto offset = layer * .07f;
                for (int i = 0; i <= 12; ++i)
                {
                    const auto p = at(-.04f + i * .09f, (i % 2 == 0 ? .34f : .66f) + offset);
                    i == 0 ? wave.startNewSubPath(p) : wave.lineTo(p);
                }
                stroke(wave, h * .05f, layer == 0 ? Colour(0xff6be0a6) : Colour(0xff2b6c50));
            }
            break;
        }
        case 2: // Saturation: purple S-curve bands.
        {
            g.setColour(Colour(0xfff9d8ae));
            g.fillRect(a);
            const std::array<Colour, 4> colours { Colour(0xffc9a0dc), Colour(0xff9b49c0), Colour(0xff7a1aa0), Colour(0xff4e0a6e) };
            for (size_t k = 0; k < colours.size(); ++k)
            {
                Path curve;
                const auto lift = k * .11f;
                for (int i = 0; i <= 40; ++i)
                {
                    const auto x = -.05f + i * 1.1f / 40.f;
                    const auto t = juce::jlimit(0.f, 1.f, (x - .25f) / .45f);
                    const auto s = t * t * (3 - 2 * t);
                    const auto p = at(x, .86f - lift - .4f * s);
                    i == 0 ? curve.startNewSubPath(p) : curve.lineTo(p);
                }
                stroke(curve, h * .1f, colours[k]);
            }
            break;
        }
        case 3: // Resampler: stepped square wave.
        {
            g.setColour(Colour(0xff131717));
            g.fillRect(a);
            for (int layer = 1; layer >= 0; --layer)
            {
                Path steps;
                for (int i = 0; i <= 9; ++i)
                {
                    const auto y = (i % 2 == 0 ? .58f : .36f) + layer * .1f;
                    const auto x0 = -.05f + i * .12f;
                    i == 0 ? steps.startNewSubPath(at(x0, y)) : steps.lineTo(at(x0, y));
                    steps.lineTo(at(x0 + .12f, y));
                }
                stroke(steps, h * .075f, layer == 0 ? Colour(0xfffbc502) : Colour(0xff9f630e));
            }
            break;
        }
        case 4: // Chorus: doubled cyan traces.
        {
            verticalFill(g, a, Colour(0xff0a5cab), Colour(0xff032a58));
            g.fillRect(a);
            const std::array<Colour, 3> colours { Colour(0xfff3e6d0), Colour(0xff00c4eb), Colour(0xff7fe3f5) };
            for (size_t k = 0; k < colours.size(); ++k)
            {
                const auto o = k * .14f;
                Path trace;
                trace.startNewSubPath(at(-.1f, .62f + o));
                trace.lineTo(at(.32f + o * .4f, .62f + o));
                trace.lineTo(at(.52f + o * .4f, .30f + o));
                trace.lineTo(at(1.1f, .30f + o));
                stroke(trace, h * .055f, colours[k]);
            }
            break;
        }
        case 5: // Delay: receding echoes.
        {
            verticalFill(g, a, Colour(0xff0d46d8), Colour(0xff031f6c));
            g.fillRect(a);
            for (int i = 0; i < 9; ++i)
            {
                const auto x = .04f + i * .11f;
                const auto top = .16f + i * .025f;
                const auto bottom = .62f - i * .03f;
                Path dash;
                dash.startNewSubPath(at(x + .05f, top));
                dash.lineTo(at(x + .1f, top));
                dash.lineTo(at(x + .05f, bottom));
                dash.lineTo(at(x, bottom));
                dash.closeSubPath();
                g.setColour(i % 2 == 0 ? Colour(0xfff7731c) : Colour(0xffffad42));
                g.fillPath(dash);
            }
            band(.92f, .78f, .05f, Colour(0xff4f7bff));
            break;
        }
        case 6: // Reverse: chevrons running backwards.
        {
            g.setColour(Colour(0xfffbe4d0));
            g.fillRect(a);
            for (int i = -1; i < 6; ++i)
            {
                const auto x = .02f + i * .2f;
                Path chevron;
                chevron.startNewSubPath(at(x + .16f, .12f));
                chevron.lineTo(at(x, .5f));
                chevron.lineTo(at(x + .16f, .88f));
                stroke(chevron, a.getWidth() * .06f, i % 2 == 0 ? Colour(0xffe9505a) : Colour(0xff9a1418));
            }
            break;
        }
        case 8: // Stretch: a wave whose cycles grow longer as time slows down, over the LIVE rail.
        {
            verticalFill(g, a, Colour(0xff201e44), Colour(0xff0c0b1f));
            g.fillRect(a);
            Path rail;
            rail.startNewSubPath(at(-.1f, .86f));
            rail.lineTo(at(1.1f, .86f));
            stroke(rail, h * .035f, Colour(0xff5a55a6));
            for (int layer = 1; layer >= 0; --layer)
            {
                Path wave;
                for (int i = 0; i <= 160; ++i)
                {
                    const auto x = -.05f + static_cast<float>(i) * 1.1f / 160.f;
                    const auto t = juce::jmax(0.f, x);
                    const auto cycles = 5.f * std::log1p(4.f * t) / std::log1p(4.f);
                    const auto p = at(x, .46f + .27f * std::sin(cycles * MathConstants<float>::twoPi) + layer * .07f);
                    i == 0 ? wave.startNewSubPath(p) : wave.lineTo(p);
                }
                stroke(wave, h * .06f, layer == 0 ? stretchAmber : Colour(0xff7b55d6));
            }
            break;
        }
        case 9: // VHS: a black sleeve with the rainbow running across it and video scan lines.
        {
            verticalFill(g, a, Colour(0xff15161a), Colour(0xff050506));
            g.fillRect(a);
            for (size_t k = 0; k < vhsRainbow.size(); ++k)
                band(.95f - static_cast<float>(k) * .11f, .2f - static_cast<float>(k) * .11f, .1f, Colour(vhsRainbow[k]));
            g.setColour(Colours::black.withAlpha(.25f));
            for (auto y = a.getY(); y < a.getBottom(); y += 2.f)
                g.fillRect(a.getX(), y, a.getWidth(), .8f);
            break;
        }
        case 10: // Grain: a cloud of particles of every size, densest in the middle, drifting off to the right.
        {
            verticalFill(g, a, Colour(0xff163a2e), Colour(0xff07140f));
            g.fillRect(a);
            juce::Random scatter(0x6a41);
            for (int i = 0; i < 70; ++i)
            {
                const auto spread = scatter.nextFloat() + scatter.nextFloat() - 1.f;
                const auto x = juce::jlimit(-.05f, 1.05f, .45f + .48f * spread + .25f * scatter.nextFloat() * scatter.nextFloat());
                const auto y = .5f + .45f * (scatter.nextFloat() + scatter.nextFloat() - 1.f);
                const auto size = h * (.03f + .11f * scatter.nextFloat() * scatter.nextFloat());
                const auto colour = i % 5 == 0 ? palette::cream : i % 2 == 0 ? grainGold : grainMint;
                g.setColour(colour.withAlpha(.25f));
                g.fillEllipse(circle(at(x, y), size * 1.9f));
                g.setColour(colour.withAlpha(.55f + .4f * scatter.nextFloat()));
                g.fillEllipse(circle(at(x, y), size));
            }
            break;
        }
        default: // Reverb: nested rooms fading into the distance.
        {
            g.setColour(Colour(0xfffcd7ae));
            g.fillRect(a);
            const std::array<Colour, 5> colours { Colour(0xfff19bb7), Colour(0xffc2307a), Colour(0xff932489),
                                                  Colour(0xff4b1f6e), Colour(0xff0d0a33) };
            for (size_t k = 0; k < colours.size(); ++k)
            {
                const auto inset = static_cast<float>(k + 1);
                const AreaF room(at(.1f + inset * .07f, .1f + inset * .1f), at(1.4f, 1.6f));
                g.setColour(colours[k]);
                g.fillRoundedRectangle(room, h * .28f);
            }
            break;
        }
    }
}

const char* effectName(size_t effect)
{
    static constexpr std::array<const char*, 11> names {
        "Tape", "Tremolo", "Saturation", "Resampler", "Chorus", "Delay", "Reverse", "Reverb", "Stretch", "VHS", "Grain" };
    return names[safeEffect(effect)];
}

// Grey injection-moulded shell shared by seated and shelf cartridges.
void shell(juce::Graphics& g, AreaF area, float topFraction)
{
    const auto radius = area.getHeight() * .07f;
    raised(g, area, radius, Colour(0xff9a9ea3), Colour(0xff45494d), .3f);
    grain(g, rounded(area, radius), .5f);

    const auto top = area.withHeight(area.getHeight() * topFraction);
    const auto window = AreaF(area.getWidth() * .4f, top.getHeight() * .38f)
                            .withCentre({ area.getCentreX(), top.getY() + top.getHeight() * .45f });
    recess(g, window, window.getHeight() * .5f, Colour(0xff121314), .18f);
    const auto vents = top.getHeight() * .5f;
    grille(g, AreaF(area.getX() + area.getWidth() * .06f, window.getY(), area.getWidth() * .2f, vents), 4);
    grille(g, AreaF(area.getRight() - area.getWidth() * .26f, window.getY(), area.getWidth() * .2f, vents), 4);
}

void label(juce::Graphics& g, AreaF window, size_t effect)
{
    const auto radius = window.getHeight() * .06f;
    g.setColour(Colour(0xff121315));
    g.fillRoundedRectangle(window.expanded(2.f), radius + 2.f);
    g.setColour(Colours::white.withAlpha(.18f));
    g.drawLine(window.getX() + radius, window.getBottom() + 2.2f, window.getRight() - radius, window.getBottom() + 2.2f, 1.f);
    const juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(rounded(window, radius));
    labelArt(g, window, safeEffect(effect));
    grain(g, rounded(window, radius), .35f);

    // Printed paper behind a clear plastic window: inner shadow, top reflection and a diagonal glint.
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(.45f), 0, window.getY(),
                                     Colours::transparentBlack, 0, window.getY() + 6.f, false));
    g.fillRect(window);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(.2f), 0, window.getY(),
                                     Colours::transparentWhite, 0, window.getY() + window.getHeight() * .4f, false));
    g.fillRect(window);
    Path glint;
    glint.startNewSubPath(window.getX() + window.getWidth() * .58f, window.getY());
    glint.lineTo(window.getX() + window.getWidth() * .74f, window.getY());
    glint.lineTo(window.getX() + window.getWidth() * .52f, window.getBottom());
    glint.lineTo(window.getX() + window.getWidth() * .36f, window.getBottom());
    glint.closeSubPath();
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(.16f), 0, window.getY(),
                                     Colours::white.withAlpha(.02f), 0, window.getBottom(), false));
    g.fillPath(glint);
    g.setGradientFill(ColourGradient(Colours::transparentBlack, 0, window.getBottom() - window.getHeight() * .25f,
                                     Colours::black.withAlpha(.25f), 0, window.getBottom(), false));
    g.fillRect(window);
}
}

Grain::Grain() : image(juce::Image::ARGB, 128, 128, true)
{
    juce::Random random(0x5eed);
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto shade = static_cast<juce::uint8>(random.nextBool() ? 255 : 0);
            image.setPixelAt(x, y, Colour::fromRGBA(shade, shade, shade, static_cast<juce::uint8>(random.nextInt(26))));
        }
}

juce::Font font(float size, bool bold)
{
    static const auto medium = juce::Typeface::createSystemTypefaceFor(
        CartridgeArt::PilcrowRoundedMedium_otf, CartridgeArt::PilcrowRoundedMedium_otfSize);
    static const auto semibold = juce::Typeface::createSystemTypefaceFor(
        CartridgeArt::PilcrowRoundedSemibold_otf, CartridgeArt::PilcrowRoundedSemibold_otfSize);
    return withTypeface(bold ? semibold : medium, size);
}

juce::Font headingFont(float size)
{
    static const auto face = juce::Typeface::createSystemTypefaceFor(
        CartridgeArt::TechnorSemibold_otf, CartridgeArt::TechnorSemibold_otfSize);
    return withTypeface(face, size);
}

juce::Font readoutFont(float size)
{
    static const auto face = juce::Typeface::createSystemTypefaceFor(
        CartridgeArt::AzeretMonoRegular_otf, CartridgeArt::AzeretMonoRegular_otfSize);
    return withTypeface(face, size);
}

float keyAspect(Key key)
{
    switch (key)
    {
        case Key::generate: return 169.f / 49.f;
        case Key::previous:
        case Key::next: return 58.f / 46.f;
        case Key::options: return 58.f / 49.f;
        default: return 1.f;
    }
}

// ---------------------------------------------------------------- chassis

void deck(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat();
    verticalFill(g, bounds, Colour(0xff0d1114), Colour(0xff07090b));
    g.fillRect(bounds);
    grain(g, rounded(bounds, 0), .6f);

    const auto skirt = bounds.withTop(bounds.getBottom() - 24);
    verticalFill(g, skirt, Colour(0xff1d2124), Colour(0xff0a0c0e));
    g.fillRect(skirt);
    topHighlight(g, skirt, 0, .08f);
    const auto vents = skirt.withSizeKeepingCentre(220, 8);
    for (int i = 0; i < 16; ++i)
    {
        const AreaF slot(vents.getX() + i * 14.f, vents.getY(), 9, vents.getHeight());
        recess(g, slot, 2, Colour(0xff050607), .06f);
    }
}

namespace
{
// The MELTIT badge: a glowing oval CRT tube in the header's left corner.
AreaF logoOval(Area area)
{
    const auto bounds = area.toFloat();
    const auto scale = bounds.getWidth() / 1160.f;
    const auto height = bounds.getHeight() - 4.f;
    return { bounds.getX() + 100 * scale, bounds.getCentreY() - height * .5f, height * 2.9f, height };
}

// The wordmark set in the heading font, then bent so every letter fills the oval from top to bottom.
Path logoPath(AreaF oval)
{
    juce::GlyphArrangement glyphs;
    glyphs.addFittedText(headingFont(100.f), "MELTIT", 0, 0, 1000, 100, juce::Justification::centred, 1, 1.f);
    Path text;
    glyphs.createPath(text);
    const auto source = text.getBounds();
    const auto centre = oval.getCentre();
    const auto rx = oval.getWidth() * .5f, ry = oval.getHeight() * .5f;
    const auto warp = [&](float x, float y)
    {
        const auto u = (x - source.getX()) / source.getWidth();
        const auto v = (y - source.getY()) / source.getHeight();
        const auto px = oval.getX() + oval.getWidth() * (.07f + .86f * u);
        const auto nx = juce::jlimit(-1.f, 1.f, (px - centre.x) / rx);
        const auto half = ry * std::sqrt(1.f - nx * nx) * .93f;
        return Point(px, centre.y - half + 2 * half * v);
    };

    Path bent;
    for (juce::PathFlatteningIterator it(text, {}, .05f); it.next();)
    {
        if (it.subPathIndex == 0)
            bent.startNewSubPath(warp(it.x1, it.y1));
        // Long straight edges are split so they follow the curve too.
        const auto steps = juce::jmax(1, juce::roundToInt(Point(it.x1, it.y1).getDistanceFrom({ it.x2, it.y2 }) / 2.f));
        for (int i = 1; i <= steps; ++i)
        {
            const auto t = (float) i / (float) steps;
            bent.lineTo(warp(it.x1 + (it.x2 - it.x1) * t, it.y1 + (it.y2 - it.y1) * t));
        }
        if (it.closesSubPath)
            bent.closeSubPath();
    }
    bent.setUsingNonZeroWinding(text.isUsingNonZeroWinding());
    return bent;
}
}

namespace
{
juce::Image blurred(const juce::Image& source, int shrink, float radius)
{
    juce::Image small(juce::Image::ARGB, juce::jmax(1, source.getWidth() / shrink), juce::jmax(1, source.getHeight() / shrink), true, juce::SoftwareImageType());
    {
        juce::Graphics s(small);
        s.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        s.drawImage(source, small.getBounds().toFloat());
    }
    juce::Image soft(juce::Image::ARGB, small.getWidth(), small.getHeight(), true, juce::SoftwareImageType());
    juce::ImageConvolutionKernel kernel(2 * juce::roundToInt(radius * 1.5f) + 1);
    kernel.createGaussianBlur(radius);
    kernel.applyToImage(soft, small, small.getBounds());
    juce::Image full(juce::Image::ARGB, source.getWidth(), source.getHeight(), true, juce::SoftwareImageType());
    juce::Graphics f(full);
    f.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    f.drawImage(soft, full.getBounds().toFloat());
    return full;
}

// MELTIT as a picture on a cathode-ray tube: the signal is softened like a scanning beam, broken up by an
// aperture grille of red, green and blue phosphor columns and faint scanlines, then left to bloom through the
// dark gaps where it is brightest, all behind curved glass.
juce::Image crtLogo(float tubeWidth, float tubeHeight, float margin)
{
    const AreaF tube(margin, margin, tubeWidth, tubeHeight);
    const auto width = juce::roundToInt(tubeWidth + 2 * margin), height = juce::roundToInt(tubeHeight + 2 * margin);
    const auto centre = tube.getCentre();
    const auto text = logoPath(tube);

    juce::Image signal(juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    {
        juce::Graphics p(signal);
        p.setColour(Colours::black);
        p.fillEllipse(tube);
        ColourGradient core(Colour(0xfffff3a0), centre.x, centre.y,
                            Colour(0xffff9a10).withAlpha(0.f), centre.x + tubeHeight * .6f, centre.y, true);
        core.addColour(.35, Colour(0xffffd830));
        core.addColour(.75, Colour(0xffffa818).withAlpha(.6f));
        p.setGradientFill(core);
        p.fillEllipse(tube.withSizeKeepingCentre(tubeHeight * 1.25f, tubeHeight * .9f));

        // A black keyline keeps each letter apart from the core and its neighbours.
        p.setColour(Colours::black);
        p.strokePath(text, juce::PathStrokeType(tubeHeight * .06f, juce::PathStrokeType::curved));
        p.setColour(Colour(0xffff5a12));
        p.fillPath(text);
    }

    const auto beam = blurred(signal, 2, .8f);
    const auto bloom = blurred(signal, 4, 2.5f);

    juce::Image screen(juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    {
        const juce::Image::BitmapData out(screen, juce::Image::BitmapData::writeOnly);
        const juce::Image::BitmapData in(beam, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData halo(bloom, juce::Image::BitmapData::readOnly);
        const auto grille = tubeHeight / 17.f;
        const auto scan = tubeHeight / 24.f;
        const auto rx = tubeWidth * .5f, ry = tubeHeight * .5f;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto px = (float) x + .5f, py = (float) y + .5f;
                const auto nx = (px - centre.x) / rx, ny = (py - centre.y) / ry;
                const auto r2 = nx * nx + ny * ny;
                const auto glow = halo.getPixelColour(x, y);
                if (r2 > 1.f)
                {
                    // Light spilling past the glass onto the panel.
                    const auto fade = juce::jlimit(0.f, 1.f, 1.f - (std::sqrt(r2) - 1.f) * 2.2f);
                    out.setPixelColour(x, y, Colour(0xffff6a1c).withAlpha(glow.getBrightness() * .4f * fade));
                    continue;
                }
                const auto colour = in.getPixelColour(x, y);
                const auto column = std::fmod(px, grille) / grille;
                const auto phosphor = (int) (column * 3.f);
                const auto edge = std::abs(std::fmod(column * 3.f, 1.f) - .5f) * 2.f;
                const auto shape = 1.f - .55f * edge * edge;
                const auto line = .72f + .28f * std::sin(py / scan * twoPi) * .5f + .14f;
                const float gain[3] { phosphor == 0 ? 1.f : .5f, phosphor == 1 ? 1.f : .5f, phosphor == 2 ? 1.f : .5f };
                const auto vignette = 1.f - .55f * std::pow(r2, 2.f);
                // Only the brightest light blooms across the grille's dark gaps.
                const auto lit = [&](float c, float g, float h)
                { return juce::jlimit(0.f, 1.f, (c * g * shape * line * 1.85f + juce::jmax(0.f, h - .3f) * .6f) * vignette + .02f); };
                out.setPixelColour(x, y, Colour::fromFloatRGBA(lit(colour.getFloatRed(), gain[0], glow.getFloatRed()),
                                                               lit(colour.getFloatGreen(), gain[1], glow.getFloatGreen()),
                                                               lit(colour.getFloatBlue(), gain[2], glow.getFloatBlue()), 1.f));
            }
    }

    juce::Graphics c(screen);
    // Curved glass: the lamp's reflection at top-left, a soft rim shadow, and the dark bezel.
    Path reflection;
    reflection.addEllipse(tube.reduced(tubeHeight * .08f).withTrimmedBottom(tubeHeight * .45f).withTrimmedRight(tubeWidth * .35f));
    c.setGradientFill(ColourGradient(Colours::white.withAlpha(.16f), tube.getX() + tubeWidth * .2f, tube.getY(),
                                     Colours::transparentWhite, tube.getX() + tubeWidth * .3f, centre.y, false));
    c.fillPath(reflection);
    c.setColour(Colour(0xff050505));
    c.drawEllipse(tube.reduced(1.5f), tubeHeight * .04f);
    c.setColour(Colours::white.withAlpha(.12f));
    c.drawEllipse(tube.reduced(tubeHeight * .045f), 1.f);
    return screen;
}
}

float headerLogoRight(Area area)
{
    return logoOval(area).getRight() + 6.f;
}

void header(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat();
    verticalFill(g, bounds, Colour(0xff272b2f), Colour(0xff121518));
    g.fillRect(bounds);
    grain(g, rounded(bounds, 0), .7f);
    for (float y = bounds.getY() + 2; y < bounds.getBottom(); y += 2)
    {
        g.setColour(Colours::white.withAlpha(.012f));
        g.drawHorizontalLine(juce::roundToInt(y), bounds.getX(), bounds.getRight());
    }
    topHighlight(g, bounds, 0, .1f);
    g.setColour(Colours::black);
    g.drawHorizontalLine(area.getBottom() - 1, bounds.getX(), bounds.getRight());

    const auto scale = bounds.getWidth() / 1160.f;
    const AreaF stripeBand(bounds.getX() + 12 * scale, bounds.getY() + 5, 58 * scale, bounds.getHeight() - 10);
    stripes(g, stripeBand.translated(1.5f, 2.5f), 20 * scale, .7f);
    stripes(g, stripeBand, 20 * scale);

}

namespace
{
AreaF stripeBand(Area area)
{
    const auto bounds = area.toFloat();
    const auto scale = bounds.getWidth() / 1160.f;
    return { bounds.getX() + 12 * scale, bounds.getY() + 5, 58 * scale, bounds.getHeight() - 10 };
}

// A stripe's bloom: a soft halo of overlapping strokes in its own colour, spilling onto the panel and neighbours.
void stripeHalo(juce::Graphics& g, const Path& stripe, Colour colour, float glow)
{
    if (glow <= 0.f)
        return;
    const auto halo = colour.brighter(.3f);
    for (int k = 7; k > 0; --k)
    {
        g.setColour(halo.withAlpha(glow * .085f));
        g.strokePath(stripe, juce::PathStrokeType((float) k * 2.6f, juce::PathStrokeType::curved));
    }
}

// The stripe's face, lit like a neon tube: the hue stays saturated and only the middle runs hot.
void stripeFace(juce::Graphics& g, const Path& stripe, Colour colour, float glow)
{
    g.setColour(glow <= 0.f ? colour : colour.withMultipliedBrightness(1.f + .35f * glow).withMultipliedSaturation(1.f + .1f * glow));
    g.fillPath(stripe);
    if (glow <= 0.f)
        return;
    const auto box = stripe.getBounds();
    ColourGradient hot(colour.brighter(.9f).withAlpha(glow * .75f), box.getCentreX(), box.getCentreY(),
                       colour.withAlpha(0.f), box.getCentreX() + box.getHeight() * .6f, box.getCentreY(), true);
    g.setGradientFill(hot);
    g.fillPath(stripe);
}

const juce::Image& tubePicture(AreaF oval, AreaF frame, float margin)
{
    // The tube is painted once at four times the size and scaled down, so the phosphor grille stays smooth.
    constexpr float detail = 4.f;
    static juce::Image cached;
    static AreaF cachedFor;
    if (cached.isNull() || cachedFor != frame)
    {
        cachedFor = frame;
        cached = crtLogo(oval.getWidth() * detail, oval.getHeight() * detail, margin * detail);
    }
    return cached;
}
}

Area headerBadgeArea(Area area)
{
    const auto oval = logoOval(area);
    return stripeBand(area).getUnion(oval.expanded(oval.getHeight() * .35f)).expanded(16.f)
        .getSmallestIntegerContainer().getIntersection(area);
}

void headerBadge(juce::Graphics& g, Area area, const BadgeState& state)
{
    juce::Graphics::ScopedSaveState outer(g);
    g.reduceClipRegion(area);
    const auto scale = area.toFloat().getWidth() / 1160.f;
    const auto band = stripeBand(area);
    for (size_t i = 0; i < 3; ++i)
        stripeHalo(g, stripePath(band, 20 * scale, i), stripeColours()[i], juce::jlimit(0.f, 1.f, state.stripes[i]));
    for (size_t i = 0; i < 3; ++i)
        stripeFace(g, stripePath(band, 20 * scale, i), stripeColours()[i], juce::jlimit(0.f, 1.f, state.stripes[i]));

    const auto oval = logoOval(area);
    const auto margin = oval.getHeight() * .35f;
    const auto frame = oval.expanded(margin);
    const auto& picture = tubePicture(oval, frame, margin);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    juce::Random random(state.seed);

    g.drawImage(picture, frame);
    Path screen;
    screen.addEllipse(oval.reduced(oval.getHeight() * .05f));
    if (state.glitch > 0.f)
    {
        // A tear: behind the glass the picture slips sideways in a few horizontal bands, as when the sync is lost.
        constexpr int bands = 9;
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(screen);
        g.setColour(Colour(0xff070302));
        g.fillPath(screen);
        for (int i = 0; i < bands; ++i)
        {
            const auto top = oval.getY() + oval.getHeight() * (float) i / bands;
            const AreaF slice(frame.getX(), top, frame.getWidth(), oval.getHeight() / bands + .5f);
            const auto shift = (random.nextFloat() * 2 - 1) * state.glitch * oval.getHeight() * .22f
                               * (random.nextFloat() < .55f ? 1.f : .15f);
            juce::Graphics::ScopedSaveState band(g);
            g.reduceClipRegion(slice.getSmallestIntegerContainer());
            g.drawImage(picture, frame.translated(shift, 0));
        }
    }

    juce::Graphics::ScopedSaveState inner(g);
    g.reduceClipRegion(screen);

    // The bright band of an old set rolling slowly down the screen.
    const auto bandHeight = oval.getHeight() * .45f;
    const auto y = oval.getY() - bandHeight + (oval.getHeight() + bandHeight) * state.roll;
    ColourGradient roll(Colour(0xffffd9a0).withAlpha(0.f), 0, y, Colour(0xffffd9a0).withAlpha(0.f), 0, y + bandHeight, false);
    roll.addColour(.5, Colour(0xffffd9a0).withAlpha(.13f));
    g.setGradientFill(roll);
    g.fillRect(oval.withY(y).withHeight(bandHeight));

    // Mains hum: a darker bar drifting the other way, and the whole picture dipping now and then.
    const auto hum = oval.getY() + oval.getHeight() * (1.f - state.roll);
    ColourGradient bar(Colours::black.withAlpha(0.f), 0, hum - 7, Colours::black.withAlpha(0.f), 0, hum + 7, false);
    bar.addColour(.5, Colours::black.withAlpha(.16f));
    g.setGradientFill(bar);
    g.fillRect(oval.withY(hum - 7).withHeight(14));
    if (state.flicker > 0.f)
    {
        g.setColour(Colours::black.withAlpha(juce::jlimit(0.f, .6f, state.flicker)));
        g.fillEllipse(oval);
    }

    if (state.glitch > 0.f)
    {
        // Snow and bright retrace lines while the picture is torn.
        for (int i = 0; i < 3; ++i)
        {
            const auto line = oval.getY() + random.nextFloat() * oval.getHeight();
            g.setColour(Colour(0xffffe6c0).withAlpha(state.glitch * (.2f + .3f * random.nextFloat())));
            g.fillRect(AreaF(oval.getX(), line, oval.getWidth(), .7f + random.nextFloat()));
        }
        for (int i = 0; i < 60; ++i)
        {
            g.setColour(Colours::white.withAlpha(state.glitch * random.nextFloat() * .5f));
            g.fillRect(AreaF(oval.getX() + random.nextFloat() * oval.getWidth(),
                             oval.getY() + random.nextFloat() * oval.getHeight(), .8f, .8f));
        }
    }
}

void rackFrame(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat();
    raised(g, bounds, 10, Colour(0xff2a2f33), Colour(0xff15181b), .14f);
    grain(g, rounded(bounds, 10), .55f);
    recess(g, bounds.reduced(6), 7, Colour(0xff111417), .1f);
    for (auto x : { bounds.getX() + 15.f, bounds.getRight() - 15.f })
        for (auto y : { bounds.getY() + 3.5f, bounds.getBottom() - 3.5f })
            screw(g, { x, y }, 2.2f, x * .1f);
}

void shelf(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat();
    raised(g, bounds, 10, Colour(0xff2b3034), Colour(0xff15181b), .14f);
    grain(g, rounded(bounds, 10), .55f);
    recess(g, bounds.reduced(9, 8), 7, Colour(0xff060708), .1f);
}

AreaF signalLine(Area area)
{
    const auto bounds = area.toFloat();
    const auto sx = bounds.getWidth() / 1671.f;
    return { bounds.getX() + 128 * sx, bounds.getY(), (1345.f - 128.f) * sx, bounds.getHeight() };
}

void chainChip(juce::Graphics& g, AreaF area, size_t effect, const juce::String& text, bool on, bool hot)
{
    const auto radius = area.getHeight() * .32f;
    if (effect >= faceColours.size())
    {
        // An empty slot: a dashed outline sitting on the line.
        Path outline;
        outline.addRoundedRectangle(area.reduced(1.f), radius);
        Path dashed;
        const float dashes[] { 3.f, 3.f };
        juce::PathStrokeType(1.f).createDashedStroke(dashed, outline, dashes, 2);
        g.setColour(Colour(0xff14181b));
        g.fillRoundedRectangle(area.reduced(1.f), radius);
        g.setColour(palette::cream.withAlpha(hot ? .8f : .45f));
        g.fillPath(dashed);
        g.setFont(font(area.getHeight() * .5f, true));
        g.drawText(text, area, juce::Justification::centred);
        return;
    }

    const Colour colour(faceColours[effect]);
    g.setColour(Colours::black.withAlpha(.55f));
    g.fillRoundedRectangle(area.translated(0, 1.5f), radius);
    raised(g, area, radius, (hot ? colour.brighter(.35f) : colour.brighter(.12f)), colour.darker(.3f), .3f);
    if (hot)
    {
        g.setColour(palette::cream.withAlpha(.8f));
        g.drawRoundedRectangle(area.reduced(.5f), radius, 1.2f);
    }

    const auto lampSize = area.getHeight() * .36f;
    const auto lamp = AreaF(lampSize, lampSize).withCentre({ area.getX() + area.getHeight() * .48f, area.getCentreY() });
    if (on)
    {
        g.setColour(palette::ledOn.withAlpha(.35f));
        g.fillEllipse(lamp.expanded(2.5f));
    }
    g.setColour(on ? palette::ledOn : Colour(0xff1b2422));
    g.fillEllipse(lamp);
    g.setColour(Colours::white.withAlpha(on ? .55f : .12f));
    g.fillEllipse(lamp.reduced(lampSize * .3f).translated(-lampSize * .12f, -lampSize * .12f));

    // Light faces take dark ink, dark faces take cream.
    const auto ink = colour.getPerceivedBrightness() > .55f ? palette::metalInk : palette::cream;
    const auto room = area.withTrimmedLeft(area.getHeight() * .9f).withTrimmedRight(area.getHeight() * .3f);
    auto size = area.getHeight() * .62f;
    while (size > 7.f && juce::GlyphArrangement::getStringWidth(font(size, true), text) > room.getWidth())
        size -= .5f;
    g.setFont(font(size, true));
    g.setColour(ink.withAlpha(on ? 1.f : .6f));
    g.drawText(text, room, juce::Justification::centred, true);
}

void signal(juce::Graphics& g, Area area, const std::array<float, 2>& peaks)
{
    const auto bounds = area.toFloat();
    verticalFill(g, bounds, Colour(0xff14181b), Colour(0xff0e1113));
    g.fillRect(bounds);
    g.setColour(Colours::black);
    g.drawHorizontalLine(area.getBottom() - 1, bounds.getX(), bounds.getRight());

    const auto sx = bounds.getWidth() / 1671.f;
    const auto mid = bounds.getCentreY();
    g.setColour(palette::cream);
    g.setFont(font(bounds.getHeight() * .5f, true));
    g.drawText("Input", AreaF(bounds.getX() + 28 * sx, bounds.getY(), 120 * sx, bounds.getHeight()).toNearestInt(),
               juce::Justification::centredLeft);
    g.drawText("Output", AreaF(bounds.getRight() - 160 * sx, bounds.getY(), 129 * sx, bounds.getHeight()).toNearestInt(),
               juce::Justification::centredRight);

    const auto start = bounds.getX() + 128 * sx, end = bounds.getX() + 1345 * sx;
    for (const auto& [spread, alpha] : { std::pair { 7.f, .04f }, std::pair { 4.f, .07f } })
    {
        g.setColour(palette::cream.withAlpha(alpha));
        g.fillRoundedRectangle(start - spread, mid - spread, end - start + spread * 2, spread * 2, spread);
    }
    g.setColour(Colours::black.withAlpha(.6f));
    g.fillRoundedRectangle(start, mid, end - start, 3.5f, 1.5f);
    g.setColour(palette::cream);
    g.fillRoundedRectangle(start, mid - 1.5f, end - start - 2, 3.f, 1.5f);
    Path head;
    head.startNewSubPath(end - 9, mid - 6);
    head.lineTo(end, mid);
    head.lineTo(end - 9, mid + 6);
    g.strokePath(head, juce::PathStrokeType(2.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));

    g.setFont(font(bounds.getHeight() * .3f, true));
    for (size_t channel = 0; channel < peaks.size(); ++channel)
    {
        const auto y = bounds.getY() + (10.f + channel * 18.f) * bounds.getHeight() / 50.f;
        const auto rowHeight = 11.f * bounds.getHeight() / 50.f;
        g.setColour(palette::cream.withAlpha(.85f));
        g.drawText(channel == 0 ? "L" : "R", AreaF(bounds.getX() + 1368 * sx, y, 18 * sx, rowHeight).toNearestInt(),
                   juce::Justification::centred);
        const auto db = juce::Decibels::gainToDecibels(peaks[channel], -54.f);
        const auto lit = juce::jlimit(0, 12, juce::roundToInt((db + 54.f) / 4.5f));
        for (int step = 0; step < 12; ++step)
        {
            const AreaF led(bounds.getX() + (1393.f + step * 10.4f) * sx, y, 7.f * sx, rowHeight);
            const auto on = step < lit;
            const auto hue = step >= 11 ? palette::stripeRed : step >= 9 ? palette::amber : palette::ledOn;
            if (on)
            {
                g.setColour(hue.withAlpha(.14f));
                g.fillRoundedRectangle(led.expanded(3.5f), 3.f);
                g.setColour(hue.withAlpha(.3f));
                g.fillRoundedRectangle(led.expanded(1.5f), 2.f);
            }
            g.setColour(on ? hue : Colour(0xff343a39));
            g.fillRoundedRectangle(led, 1.f);
            g.setColour(Colours::white.withAlpha(on ? .45f : .06f));
            g.fillRect(led.withHeight(led.getHeight() * .3f).reduced(1.f, 0));
        }
    }
}

void presetHousing(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat();
    raised(g, bounds, 6, Colour(0xff2a2e32), Colour(0xff131619), .12f);
    recess(g, bounds.reduced(40, 4), 4, Colour(0xff0a0d0f), .1f);
}

// ---------------------------------------------------------------- slots

void dock(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat();
    raised(g, bounds, 9, Colour(0xff32373b), Colour(0xff15181b), .16f);
    grain(g, rounded(bounds, 9), .55f);
    screw(g, { bounds.getX() + 15, bounds.getY() + 16 }, 4.2f, .6f);
    grille(g, AreaF(bounds.getCentreX() - bounds.getWidth() * .2f, bounds.getY() + 9,
                    bounds.getWidth() * .4f, 17), 4);

    const auto cavity = bounds.reduced(10, 0).withTrimmedTop(31).withTrimmedBottom(5);
    recess(g, cavity, 6, Colour(0xff08090b), .12f);
    for (auto x : { cavity.getX() + 1.5f, cavity.getRight() - 3.5f })
    {
        g.setGradientFill(ColourGradient(Colour(0xff4a5056), 0, cavity.getY(), Colour(0xff111315), 0,
                                         cavity.getBottom(), false));
        g.fillRect(x, cavity.getY() + 6, 2.f, cavity.getHeight() - 12);
    }
}

void connector(juce::Graphics& g, Area area, float opacity)
{
    const auto bounds = area.toFloat();
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    const auto rod = bounds.withSizeKeepingCentre(bounds.getWidth() - 10, bounds.getHeight() * .32f);
    verticalFill(g, rod, Colour(0xffeef0f1), Colour(0xff5d6267));
    g.fillRoundedRectangle(rod, rod.getHeight() * .5f);
    g.setColour(Colours::black.withAlpha(.6f));
    g.drawRoundedRectangle(rod, rod.getHeight() * .5f, .8f);
    for (auto x : { bounds.getX(), bounds.getRight() - 12.f })
    {
        const AreaF cap(x, bounds.getY() + bounds.getHeight() * .18f, 12, bounds.getHeight() * .64f);
        raised(g, cap, 2, Colour(0xffb2b6ba), Colour(0xff474b50), .4f);
    }
}

void port(juce::Graphics& g, Area area, bool pads, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    const auto bar = fit(area.toFloat(), 6.f);
    raised(g, bar, bar.getHeight() * .5f, Colour(0xff2b2f33), Colour(0xff121416), .12f);
    if (!pads)
    {
        screw(g, { bar.getX() + bar.getHeight() * .55f, bar.getCentreY() }, bar.getHeight() * .22f);
        screw(g, { bar.getRight() - bar.getHeight() * .55f, bar.getCentreY() }, bar.getHeight() * .22f);
    }
    const auto contacts = bar.reduced(bar.getWidth() * .2f, 0);
    for (int i = 0; i < 4; ++i)
    {
        const auto x = contacts.getX() + contacts.getWidth() * (i + .5f) / 4.f;
        const auto size = bar.getHeight() * (pads ? .34f : .38f);
        const auto contact = pads ? AreaF(size * 2.2f, size).withCentre({ x, bar.getCentreY() })
                                  : AreaF(size, size).withCentre({ x, bar.getCentreY() });
        g.setGradientFill(ColourGradient(Colour(0xfff6d37a), contact.getX(), contact.getY(),
                                         Colour(0xff7d5617), contact.getRight(), contact.getBottom(), false));
        pads ? g.fillRect(contact) : g.fillEllipse(contact);
    }
}

void addFrame(juce::Graphics& g, Area area, float opacity)
{
    const auto bounds = area.toFloat().reduced(1.5f);
    recess(g, bounds, 8, Colour(0xff121517), .1f);
    g.setColour(palette::amber.withAlpha(.35f + .6f * (opacity - .9f) * 10.f));
    g.drawRoundedRectangle(bounds.reduced(2), 6, 2.f);
    const auto centre = bounds.getCentre();
    g.setColour(palette::cream.withAlpha(opacity));
    g.fillRoundedRectangle(AreaF(26, 3.5f).withCentre(centre), 1.5f);
    g.fillRoundedRectangle(AreaF(3.5f, 26).withCentre(centre), 1.5f);
}

// ---------------------------------------------------------------- panels and cartridges

// ---------------------------------------------------------------- effect cards
// Every card is its own little stompbox: a finish, a header treatment and ornaments placed
// around that card's control layout (see cardLayout).
namespace
{
void bevelEdge(juce::Graphics& g, AreaF face, AreaF whole, float radius, float highlight)
{
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(juce::jmin(.7f, highlight * 2.4f)), face.getX(), face.getY(),
                                     Colours::black.withAlpha(.3f), face.getX() + face.getWidth() * .3f, face.getBottom(), false));
    g.drawRoundedRectangle(face.reduced(1.f), juce::jmax(0.f, radius - 1.f), 1.3f);
    g.setColour(Colours::black.withAlpha(.9f));
    g.drawRoundedRectangle(whole.reduced(.5f), radius, 1.f);
}

void brushedBand(juce::Graphics& g, AreaF band, Colour top, Colour bottom, juce::Random& random)
{
    verticalFill(g, band, top, bottom);
    g.fillRect(band);
    for (auto y = band.getY(); y < band.getBottom(); y += 1.f)
    {
        g.setColour((random.nextBool() ? Colours::white : Colours::black).withAlpha(.02f + random.nextFloat() * .06f));
        g.fillRect(band.getX(), y, band.getWidth(), 1.f);
    }
}

// Hammered enamel: overlapping shallow dimples, each lit from the upper left.
void hammertone(juce::Graphics& g, AreaF area, juce::Random& random, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const auto x = area.getX() + random.nextFloat() * area.getWidth();
        const auto y = area.getY() + random.nextFloat() * area.getHeight();
        const auto r = 3.f + random.nextFloat() * 6.f;
        g.setGradientFill(ColourGradient(Colours::black.withAlpha(.11f), x - r, y - r,
                                         Colours::white.withAlpha(.09f), x + r, y + r, false));
        g.fillEllipse(x - r, y - r, r * 2, r * 2);
    }
}

void crossHatch(juce::Graphics& g, AreaF area, float spacing, float alpha)
{
    g.setColour(Colours::black.withAlpha(alpha));
    for (auto d = -area.getHeight(); d < area.getWidth(); d += spacing)
    {
        g.drawLine(area.getX() + d, area.getBottom(), area.getX() + d + area.getHeight(), area.getY(), .6f);
        g.drawLine(area.getX() + d, area.getY(), area.getX() + d + area.getHeight(), area.getBottom(), .6f);
    }
}

void dotGrid(juce::Graphics& g, AreaF area, float spacing)
{
    for (auto y = area.getY(); y < area.getBottom(); y += spacing)
        for (auto x = area.getX(); x < area.getRight(); x += spacing)
        {
            g.setColour(Colours::black.withAlpha(.32f));
            g.fillEllipse(x, y, 1.8f, 1.8f);
            g.setColour(Colours::white.withAlpha(.07f));
            g.fillRect(x + .6f, y + 1.8f, 1.2f, .6f);
        }
}

void chromeRule(juce::Graphics& g, float x0, float x1, float y, float thickness = 3.f)
{
    const AreaF strip(x0, y - thickness * .5f, x1 - x0, thickness);
    ColourGradient metal(Colour(0xfff7f8f9), 0, strip.getY(), Colour(0xff5d6166), 0, strip.getBottom(), false);
    metal.addColour(.45, Colour(0xffb9bdc2));
    g.setGradientFill(metal);
    g.fillRect(strip);
    g.setColour(Colours::black.withAlpha(.5f));
    g.drawLine(x0, strip.getBottom() + .5f, x1, strip.getBottom() + .5f, .9f);
}

void engravedRule(juce::Graphics& g, float x0, float x1, float y)
{
    g.setColour(Colours::black.withAlpha(.55f));
    g.drawLine(x0, y, x1, y, 1.f);
    g.setColour(Colours::white.withAlpha(.22f));
    g.drawLine(x0, y + 1, x1, y + 1, 1.f);
}

// Metal-flake paint: the base colour with a sparkle of gold and white flakes catching the lamp.
void metalFlake(juce::Graphics& g, AreaF area, Colour base, juce::Random& random, int count)
{
    verticalFill(g, area, base.brighter(.1f), base.darker(.35f));
    g.fillRect(area);
    for (int i = 0; i < count; ++i)
    {
        const Point p { area.getX() + random.nextFloat() * area.getWidth(), area.getY() + random.nextFloat() * area.getHeight() };
        const auto bright = random.nextFloat();
        g.setColour((bright > .82f ? Colours::white : grainGold).withAlpha(.08f + .3f * bright * bright));
        g.fillRect(AreaF(p.x, p.y, .9f + random.nextFloat() * .8f, .9f + random.nextFloat() * .8f));
    }
}

// A gold inlay line with its shadow below.
void goldRule(juce::Graphics& g, float x0, float x1, float y)
{
    g.setColour(Colours::black.withAlpha(.55f));
    g.drawLine(x0, y + 1.2f, x1, y + 1.2f, 1.f);
    g.setGradientFill(ColourGradient(grainGold.darker(.4f), x0, y, grainGold.brighter(.25f), (x0 + x1) * .5f, y, false));
    g.drawLine(x0, y, x1, y, 1.6f);
}

// Particles thinning out from left to right along a band, like a cloud of grains drifting off.
void particleTrail(juce::Graphics& g, AreaF band, juce::Random& random)
{
    for (int i = 0; i < 46; ++i)
    {
        const auto u = std::pow(random.nextFloat(), 1.7f);
        const Point p { band.getX() + u * band.getWidth(), band.getY() + random.nextFloat() * band.getHeight() };
        const auto size = (1.f - u) * 1.6f + .6f;
        g.setColour(grainGold.withAlpha(.85f - .6f * u));
        g.fillEllipse(circle(p, size));
    }
}

void rivet(juce::Graphics& g, Point centre, float radius)
{
    g.setColour(Colours::black.withAlpha(.45f));
    g.fillEllipse(circle(centre.translated(.5f, .9f), radius + .4f));
    g.setGradientFill(ColourGradient(Colour(0xffd9dcdf), centre.x - radius * .5f, centre.y - radius * .6f,
                                     Colour(0xff4a4e52), centre.x + radius * .6f, centre.y + radius, true));
    g.fillEllipse(circle(centre, radius));
    g.setColour(Colours::black.withAlpha(.6f));
    g.drawEllipse(circle(centre, radius), .6f);
}

// A cassette spool seen through a smoked window, its hub turned by `turn` radians.
void spool(juce::Graphics& g, Point centre, float radius, float turn = 0)
{
    g.setGradientFill(ColourGradient(Colour(0xff6b4b36), centre.x, centre.y, Colour(0xff24180f), centre.x + radius, centre.y, true));
    g.fillEllipse(circle(centre, radius));
    g.setColour(Colours::white.withAlpha(.14f));
    g.drawEllipse(circle(centre, radius), .8f);
    g.setColour(Colour(0xffe9e4da));
    g.fillEllipse(circle(centre, radius * .46f));
    g.setColour(Colour(0xff2a2522));
    for (int k = 0; k < 6; ++k)
        g.fillEllipse(circle(polar(centre, radius * .33f, turn + k * MathConstants<float>::pi / 3.f), radius * .07f));
    g.fillEllipse(circle(centre, radius * .16f));
    // The drive teeth inside the hub show which way it turns.
    g.setColour(Colour(0xffe9e4da));
    for (int k = 0; k < 3; ++k)
    {
        const auto a = turn + k * MathConstants<float>::twoPi / 3.f;
        g.drawLine(juce::Line<float>(polar(centre, radius * .05f, a), polar(centre, radius * .15f, a)), radius * .06f);
    }
}
}

AreaF tapeWindowBounds(Area card)
{
    const auto whole = card.toFloat().reduced(.5f);
    const auto sx = whole.getWidth() / 240.f, sy = whole.getHeight() / 242.f;
    return { whole.getX() + 89 * sx, whole.getY() + 152 * sy, 62 * sx, 66 * sy };
}

void tapeWindow(juce::Graphics& g, Area card, float leftTurn, float rightTurn)
{
    const auto window = tapeWindowBounds(card);
    const auto s = window.getWidth() / 62.f;
    recess(g, window, 6, Colour(0xff17130f), .3f);
    const Point left { window.getX() + 16 * s, window.getY() + 24 * window.getHeight() / 66.f };
    const Point right { window.getX() + 46 * s, left.y };
    const auto reelRadius = 12.f * s;
    spool(g, left, reelRadius, leftTurn);
    spool(g, right, reelRadius, rightTurn);
    g.setColour(Colour(0xff3b2a1f));
    g.drawLine(left.x, left.y + reelRadius, right.x, right.y + reelRadius, 1.6f);
    g.setColour(palette::cream.withAlpha(.75f));
    g.setFont(readoutFont(8.5f));
    g.drawText("C-60", AreaF(window.getX(), window.getBottom() - 15, window.getWidth(), 12).toNearestInt(),
               juce::Justification::centred);
    Path glint;
    glint.startNewSubPath(window.getX() + window.getWidth() * .55f, window.getY());
    glint.lineTo(window.getX() + window.getWidth() * .8f, window.getY());
    glint.lineTo(window.getX() + window.getWidth() * .35f, window.getBottom());
    glint.lineTo(window.getX() + window.getWidth() * .1f, window.getBottom());
    glint.closeSubPath();
    g.setColour(Colours::white.withAlpha(.07f));
    g.fillPath(glint);
}

void face(juce::Graphics& g, Area area, size_t effect, float dividerPosition)
{
    static constexpr std::array<float, 11> corners { 8.f, 14.f, 12.f, 5.f, 24.f, 10.f, 6.f, 16.f, 11.f, 7.f, 13.f };
    const auto e = safeEffect(effect);
    const auto whole = area.toFloat().reduced(.5f);
    const auto sx = whole.getWidth() / 240.f, sy = whole.getHeight() / 242.f;
    const auto X = [&](float x) { return whole.getX() + x * sx; };
    const auto Y = [&](float y) { return whole.getY() + stretchedY(e, y) * sy; };
    const auto radius = corners[e];
    const Colour colour(faceColours[e]);
    raised(g, whole, radius, colour.brighter(.12f), colour.darker(.22f), .25f);

    const auto face = whole.withTrimmedBottom(wall(whole));
    const auto shape = rounded(face, radius);
    // Without a selector the header shrinks to a thin trim in the card's header finish.
    const auto split = hasHeader(e) ? whole.getY() + whole.getHeight() * dividerPosition : whole.getY() + 11.f * sy;
    const auto header = face.withBottom(split);
    const auto body = face.withTop(split);
    juce::Random random(static_cast<juce::int64>(0x5eed00 + e));

    {
        const juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(shape);
        switch (e)
        {
            case 0: // Tape: studio deck in cream enamel with a brushed header and a cassette window.
            {
                brushedBand(g, header, Colour(0xffeef0f2), Colour(0xffa3a7ac), random);
                engravedRule(g, face.getX(), face.getRight(), split);
                g.setColour(Colour(0xff3a2f25).withAlpha(.4f));
                g.drawRoundedRectangle(AreaF(X(4), split + 4, face.getWidth() - 8 * sx, face.getBottom() - split - 8), 5, 1);
                tapeWindow(g, area, 0, 0);
                break;
            }
            case 1: // Tremolo: red hammertone with a black header and engraved waves.
            {
                hammertone(g, body, random, 170);
                verticalFill(g, header, Colour(0xff2c2c2e), Colour(0xff111112));
                g.fillRect(header);
                chromeRule(g, face.getX(), face.getRight(), split);
                for (const auto& [from, to] : { std::pair { 12.f, 76.f }, std::pair { 164.f, 228.f } })
                {
                    Path wave;
                    for (auto x = from; x <= to; x += 1.f)
                    {
                        const Point p { X(x), Y(204) + 7.f * sy * std::sin((x - from) / (to - from) * twoPi * 2.f) };
                        x == from ? wave.startNewSubPath(p) : wave.lineTo(p);
                    }
                    g.setColour(Colours::black.withAlpha(.35f));
                    g.strokePath(wave, juce::PathStrokeType(1.6f), juce::AffineTransform::translation(0, 1));
                    g.setColour(palette::cream.withAlpha(.55f));
                    g.strokePath(wave, juce::PathStrokeType(1.4f));
                }
                for (auto p : { Point(X(12), Y(52)), Point(X(228), Y(52)), Point(X(12), Y(229)), Point(X(228), Y(229)) })
                    screw(g, p, 3.f, p.x * .07f);
                break;
            }
            case 2: // Saturation: valve amp with grille-cloth weave, black glass and gold piping.
            {
                crossHatch(g, body, 4.f, .06f);
                verticalFill(g, header, Colour(0xff241a11), Colour(0xff0a0705));
                g.fillRect(header);
                g.setGradientFill(ColourGradient(Colours::white.withAlpha(.09f), 0, header.getY(),
                                                 Colours::transparentWhite, 0, header.getCentreY(), false));
                g.fillRect(header);
                const Colour gold(0xffd6a35c);
                g.setColour(gold);
                g.drawLine(face.getX(), split, face.getRight(), split, 1.8f);
                g.setColour(gold.withAlpha(.6f));
                g.drawLine(X(132), split + 10, X(132), face.getBottom() - 10, 1.1f);
                g.drawLine(face.getX() + 10, face.getBottom() - 5, face.getRight() - 10, face.getBottom() - 5, 1.2f);
                break;
            }
            case 3: // Resampler: perforated lab box with bit lamps beside the rate knob.
            {
                dotGrid(g, body.reduced(6 * sx, 5 * sy), 6.f * sx);
                verticalFill(g, header, Colour(0xff222a30), Colour(0xff12161a));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                static constexpr std::array<juce::uint8, 4> bits { 0b10110100, 0b01101001, 0b11010010, 0b00111010 };
                for (int column = 0; column < 4; ++column)
                {
                    const auto x = X(column < 2 ? 22.f + column * 14.f : 192.f + (column - 2) * 14.f);
                    for (int row = 0; row < 8; ++row)
                    {
                        const auto lit = (bits[static_cast<size_t>(column)] >> row & 1) != 0;
                        const auto cell = AreaF(5 * sx, 5 * sx).withCentre({ x, Y(146.f + row * 11.f) });
                        if (lit)
                        {
                            g.setColour(Colour(0xff7dff9c).withAlpha(.22f));
                            g.fillRoundedRectangle(cell.expanded(2.5f), 2);
                        }
                        g.setColour(lit ? Colour(0xff8dffaa) : Colour(0xff1d2a24));
                        g.fillRoundedRectangle(cell, 1);
                        g.setColour(Colours::black.withAlpha(.6f));
                        g.drawRoundedRectangle(cell, 1, .6f);
                    }
                }
                break;
            }
            case 4: // Chorus: glossy 70s orange, black capsule header, rings and racing stripes.
            {
                g.setGradientFill(ColourGradient(Colours::white.withAlpha(.24f), 0, face.getY(),
                                                 Colours::transparentWhite, 0, face.getY() + face.getHeight() * .5f, false));
                g.fillRect(face);
                const auto capsule = header.reduced(5 * sx, 0).withTrimmedTop(6).withTrimmedBottom(3);
                g.setColour(Colour(0xff17130f));
                g.fillRoundedRectangle(capsule, capsule.getHeight() * .5f);
                g.setColour(Colours::white.withAlpha(.12f));
                g.drawRoundedRectangle(capsule.reduced(.5f), capsule.getHeight() * .5f, .8f);
                const Point centre { X(120), Y(135) };
                for (const auto& [r, c, w] : { std::tuple { 50.f, Colour(0xff0a6fa8), 1.5f }, std::tuple { 57.f, Colour(0xff19c3e6), 2.2f },
                                               std::tuple { 63.f, palette::cream, 1.4f } })
                {
                    g.setColour(c.withAlpha(.4f));
                    g.drawEllipse(circle(centre, r * sx), w);
                }
                const std::array<Colour, 3> colours { Colour(0xff0a6fa8), Colour(0xff19c3e6), palette::cream };
                for (size_t k = 0; k < colours.size(); ++k)
                {
                    const auto o = 3.5f + static_cast<float>(k) * 3.5f;
                    const auto bend = juce::jmax(4.f, radius - o + 2.f);
                    Path stripe;
                    stripe.startNewSubPath(X(166), face.getBottom() - o);
                    stripe.lineTo(face.getRight() - o - bend, face.getBottom() - o);
                    stripe.quadraticTo(face.getRight() - o, face.getBottom() - o, face.getRight() - o, face.getBottom() - o - bend);
                    stripe.lineTo(face.getRight() - o, split + 26);
                    g.setColour(colours[k]);
                    g.strokePath(stripe, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
                }
                break;
            }
            case 5: // Delay: plum echo unit with a chrome trim and fading repeats.
            {
                verticalFill(g, header, colour.darker(.55f), colour.darker(.8f));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                g.setColour(Colours::black.withAlpha(.4f));
                g.drawRoundedRectangle(face.reduced(3.5f).translated(0, 1), radius - 3, 1.2f);
                g.setColour(Colours::white.withAlpha(.3f));
                g.drawRoundedRectangle(face.reduced(3.5f), radius - 3, 1.2f);
                for (int i = 0; i < 5; ++i)
                {
                    g.setColour(palette::cream.withAlpha(.7f - i * .12f));
                    g.fillEllipse(circle({ X(120), Y(66.f + i * 37.f) }, 3.4f - i * .5f));
                }
                for (int i = 0; i < 7; ++i)
                {
                    const auto x = X(16.f + i * 10.f);
                    const auto h = 5.f * sy;
                    const auto top = face.getBottom() - 7.5f;
                    Path dash;
                    dash.startNewSubPath(x + 3, top);
                    dash.lineTo(x + 3 + (5.f - i * .5f), top);
                    dash.lineTo(x + (5.f - i * .5f), top + h);
                    dash.lineTo(x, top + h);
                    dash.closeSubPath();
                    g.setColour(palette::stripeOrange.withAlpha(.85f - i * .11f));
                    g.fillPath(dash);
                }
                break;
            }
            case 6: // Reverse: olive field kit with rivets, backwards chevrons and a hazard band.
            {
                verticalFill(g, header, colour.darker(.45f), colour.darker(.75f));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                for (int i = 0; i < 7; ++i)
                {
                    const auto y = Y(58.f + i * 27.f);
                    rivet(g, { X(6.5f), y }, 2.2f);
                    rivet(g, { X(233.5f), y }, 2.2f);
                }
                for (const auto x0 : { 16.f, 186.f })
                    for (int k = 0; k < 2; ++k)
                    {
                        const auto x = X(x0 + k * 16.f);
                        Path chevron;
                        chevron.startNewSubPath(x + 13 * sx, Y(78));
                        chevron.lineTo(x, Y(98));
                        chevron.lineTo(x + 13 * sx, Y(118));
                        const juce::PathStrokeType pen(4.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::square);
                        g.setColour(Colours::white.withAlpha(.13f));
                        g.strokePath(chevron, pen, juce::AffineTransform::translation(0, 1.2f));
                        g.setColour(Colours::black.withAlpha(.38f));
                        g.strokePath(chevron, pen);
                    }
                const AreaF hazard(face.getX(), face.getBottom() - 7, face.getWidth(), 7);
                const juce::Graphics::ScopedSaveState band(g);
                g.reduceClipRegion(hazard.toNearestInt());
                g.setColour(Colour(0xffe8b923));
                g.fillRect(hazard);
                g.setColour(Colour(0xff161616));
                for (auto x = hazard.getX() - 10; x < hazard.getRight(); x += 12)
                {
                    Path stripe;
                    stripe.startNewSubPath(x, hazard.getBottom());
                    stripe.lineTo(x + 6, hazard.getBottom());
                    stripe.lineTo(x + 13, hazard.getY());
                    stripe.lineTo(x + 7, hazard.getY());
                    stripe.closeSubPath();
                    g.fillPath(stripe);
                }
                break;
            }
            case 8: // Stretch: indigo time unit with an amber LIVE rail; the editor's map strip fills the trim band.
                verticalFill(g, header, colour.darker(.5f), colour.darker(.8f));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                g.setColour(stretchAmber.withAlpha(.55f));
                g.fillRect(AreaF(face.getX() + 8, split + 2.5f, face.getWidth() - 16, 1.2f));
                break;
            case 9: // VHS: a video recorder front in black plastic, a silver fascia and a cassette door.
            {
                brushedBand(g, body, colour.brighter(.08f), colour.darker(.35f), random);
                brushedBand(g, header, Colour(0xffd9dce0), Colour(0xff8b9096), random);
                chromeRule(g, face.getX(), face.getRight(), split, 2.f);
                // Cassette door along the bottom, with its flap line and a REC lamp.
                const AreaF door(X(16), Y(222) - 6.f, face.getWidth() - 40 * sx, 6.f);
                recess(g, door, 2.f, Colour(0xff07080a), .18f);
                g.setColour(Colours::white.withAlpha(.08f));
                g.drawHorizontalLine(juce::roundToInt(door.getCentreY()), door.getX() + 4, door.getRight() - 4);
                const auto lamp = circle({ face.getRight() - 12.f * sx, door.getCentreY() }, 2.2f);
                g.setColour(Colour(0xffff3b30).withAlpha(.25f));
                g.fillEllipse(lamp.expanded(2.f));
                g.setColour(Colour(0xffff4a3d));
                g.fillEllipse(lamp);
                // The rainbow across the fascia's lower edge.
                for (size_t k = 0; k < vhsRainbow.size(); ++k)
                {
                    g.setColour(Colour(vhsRainbow[k]).withAlpha(.9f));
                    g.fillRect(AreaF(face.getRight() - (60.f - static_cast<float>(k) * 9.f) * sx, split - 5.f, 8.f * sx, 2.5f));
                }
                break;
            }
            case 10: // Grain: emerald metal-flake paint under a smoked header, a gold rule and particles drifting off.
            {
                metalFlake(g, body, colour, random, 900);
                verticalFill(g, header, Colour(0xff10261e), Colour(0xff050d0a));
                g.fillRect(header);
                g.setGradientFill(ColourGradient(Colours::white.withAlpha(.08f), 0, header.getY(),
                                                 Colours::transparentWhite, 0, header.getCentreY(), false));
                g.fillRect(header);
                goldRule(g, face.getX(), face.getRight(), split);
                particleTrail(g, AreaF(face.getX() + 14 * sx, face.getBottom() - 9.f, face.getWidth() - 28 * sx, 6.f), random);
                break;
            }
            default: // Reverb: two-tone surf amp with a cream header, tolex body and a spring.
            {
                crossHatch(g, body, 3.f, .045f);
                verticalFill(g, header, Colour(0xfff2e7cd), Colour(0xffd3c29e));
                g.fillRect(header);
                chromeRule(g, face.getX(), face.getRight(), split);
                const auto x = X(104);
                for (auto y = Y(160); y < Y(218); y += 3.2f)
                {
                    const AreaF loop(x - 7 * sx, y - 2.2f, 14 * sx, 4.4f);
                    g.setColour(Colours::black.withAlpha(.45f));
                    g.drawEllipse(loop.translated(0, .8f), 1.1f);
                    g.setColour(Colour(0xffd9dde0));
                    g.drawEllipse(loop, .9f);
                }
                for (const auto y : { Y(156), Y(221) })
                {
                    const AreaF clamp(x - 5 * sx, y - 2.5f, 10 * sx, 5);
                    raised(g, clamp, 1.5f, Colour(0xffc9cdd1), Colour(0xff5d6166), .4f);
                }
                chromeRule(g, face.getX(), face.getRight(), face.getBottom() - 5, 2.5f);
                break;
            }
        }
    }
    grain(g, shape, .55f);
    bevelEdge(g, face, whole, radius, .25f);
}

namespace
{
// A screen or a tape keeps a margin around its text.
float titlePadding(size_t e)
{
    return e == 3 ? 4.f : e == 6 ? 5.f : 0.f;
}

// Every title starts at the left of the title column, right after the power lamp, whether or not the
// card has a selector. A screen or a tape is sized to its text but never grows past the column.
AreaF titleColumn(AreaF bounds, size_t e)
{
    return e == 3 ? bounds.withSizeKeepingCentre(bounds.getWidth(), juce::jmin(22.f, bounds.getHeight()))
         : e == 6 ? bounds.withSizeKeepingCentre(bounds.getWidth() - 2, juce::jmin(21.f, bounds.getHeight()))
                  : bounds;
}

AreaF titleRoom(AreaF bounds, size_t e)
{
    const auto padding = titlePadding(e);
    // The italic title leans right, so it keeps a little extra room.
    return titleColumn(bounds, e).reduced(padding, padding > 0 ? 3.f : 0.f).withTrimmedRight(e == 1 ? 4.f : 0.f);
}

// One typeface and size for every card title: the largest size at which each card's name fits its own room.
float titleHeight(AreaF bounds, float preferred)
{
    auto height = preferred;
    for (size_t e = 0; e < faceColours.size(); ++e)
    {
        const auto natural = juce::GlyphArrangement::getStringWidth(headingFont(preferred), effectName(e));
        height = juce::jmin(height, preferred * titleRoom(bounds, e).getWidth() / natural);
    }
    return height;
}
}

void title(juce::Graphics& g, Area area, const juce::String& text, size_t effect, float preferred)
{
    const auto e = safeEffect(effect);
    const auto bounds = area.toFloat();
    const auto padding = titlePadding(e);
    const auto column = titleColumn(bounds, e);
    const auto room = titleRoom(bounds, e);
    // Names that are not effects, such as "Global controls", still have to fit their own room.
    const auto own = preferred * room.getWidth() / juce::jmax(1.f, juce::GlyphArrangement::getStringWidth(headingFont(preferred), text));
    const auto typeface = headingFont(juce::jmin(titleHeight(bounds, preferred), own));

    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(typeface, text, 0, 0);
    Path letters;
    glyphs.createPath(letters);
    if (e == 1)
        letters.applyTransform(juce::AffineTransform::shear(-.2f, 0));

    // Centre the capitals on the header's middle line, as the lamp and buttons are, so descenders don't lift the word.
    juce::GlyphArrangement capital;
    capital.addLineOfText(typeface, "H", 0, 0);
    Path capitalOutline;
    capital.createPath(capitalOutline);
    const auto capHeight = -capitalOutline.getBounds().getY();
    letters.applyTransform(juce::AffineTransform::translation(room.getX() - letters.getBounds().getX(),
                                                              room.getCentreY() + capHeight * .5f));
    const auto ink = letters.getBounds();
    const AreaF plate(column.getX(), column.getY(), juce::jmin(column.getWidth(), ink.getWidth() + padding * 2),
                      column.getHeight());
    const auto print = [&g, &letters](Colour colour, float dx = 0, float dy = 0)
    {
        g.setColour(colour);
        g.fillPath(letters, juce::AffineTransform::translation(dx, dy));
    };

    const juce::Graphics::ScopedSaveState state(g);
    switch (e)
    {
        case 0: // Engraved into brushed aluminium.
            print(Colours::white.withAlpha(.6f), 0, 1);
            print(Colour(0xff1f2428));
            break;
        case 1: // Italic script with a wavy underline.
        {
            print(Colours::black.withAlpha(.75f), 0, 1.5f);
            print(palette::cream);
            Path wave;
            for (auto x = ink.getX(); x <= ink.getRight(); x += 1.f)
            {
                const Point p { x, ink.getBottom() + 4.f + 1.6f * std::sin((x - ink.getX()) * .7f) };
                x == ink.getX() ? wave.startNewSubPath(p) : wave.lineTo(p);
            }
            g.setColour(palette::cream.withAlpha(.8f));
            g.strokePath(wave, juce::PathStrokeType(1.1f));
            break;
        }
        case 2: // Valve glow behind black glass.
        {
            const Colour amber(0xffffb04a);
            for (const auto& offset : { Point(-1.5f, 0), Point(1.5f, 0), Point(0, -1.5f), Point(0, 1.5f) })
                print(amber.withAlpha(.14f), offset.x, offset.y);
            print(Colour(0xffff8a1f).withAlpha(.5f), .5f, .5f);
            print(amber);
            break;
        }
        case 3: // Green LCD.
        {
            const auto screen = plate;
            recess(g, screen, 3, Colour(0xff0a1a10), .12f);
            const Colour phosphor(0xff7dff9c);
            for (const auto& offset : { Point(-1, 0), Point(1, 0), Point(0, -1), Point(0, 1) })
                print(phosphor.withAlpha(.18f), offset.x, offset.y);
            print(phosphor);
            g.reduceClipRegion(screen.toNearestInt());
            g.setColour(Colours::black.withAlpha(.22f));
            for (auto y = screen.getY(); y < screen.getBottom(); y += 2.f)
                g.fillRect(screen.getX(), y, screen.getWidth(), 1.f);
            break;
        }
        case 4: // Offset double print.
            print(Colour(0xff19c3e6), 1.6f, 1.6f);
            print(palette::cream);
            break;
        case 5: // Repeats fading to the right.
            print(palette::stripeOrange.withAlpha(.16f), 7, 0);
            print(palette::stripeOrange.withAlpha(.32f), 3.5f, 0);
            print(Colours::black.withAlpha(.6f), 0, 1.2f);
            print(palette::cream);
            break;
        case 6: // Embossed label-maker tape.
        {
            const auto tape = plate;
            g.addTransform(juce::AffineTransform::rotation(-.03f, tape.getCentreX(), tape.getCentreY()));
            g.setColour(Colours::black.withAlpha(.5f));
            g.fillRoundedRectangle(tape.translated(.8f, 1.5f), 2);
            verticalFill(g, tape, Colour(0xff2f3032), Colour(0xff0d0d0e));
            g.fillRoundedRectangle(tape, 2);
            g.setColour(Colours::white.withAlpha(.1f));
            g.fillRect(tape.withHeight(tape.getHeight() * .4f).reduced(1, 1));
            print(Colours::black.withAlpha(.85f), 0, 1);
            print(Colour(0xffeeeeea));
            break;
        }
        case 8: // Smeared backwards, as if the letters were slowing down.
            for (int i = 4; i >= 1; --i)
                print(stretchAmber.withAlpha(.06f + .04f * static_cast<float>(4 - i)), -2.2f * static_cast<float>(i), 0);
            print(Colours::black.withAlpha(.6f), 0, 1.2f);
            print(palette::cream);
            break;
        case 9: // A slightly mistracked picture: red and cyan fringes either side of the letters.
            print(Colours::black.withAlpha(.6f), 0, 1.2f);
            print(Colour(0xffff3b6a).withAlpha(.7f), -1.4f, 0);
            print(Colour(0xff3bd8ff).withAlpha(.7f), 1.4f, 0);
            print(Colour(0xfff4f6f8));
            break;
        case 10: // Cream letters breaking up into gold grains on their right.
        {
            print(Colours::black.withAlpha(.65f), 0, 1.2f);
            print(palette::cream);
            juce::Random scatter(0x617a);
            for (int i = 0; i < 26; ++i)
            {
                const auto u = scatter.nextFloat();
                const Point p { ink.getRight() + 2.f + u * 26.f, ink.getY() + scatter.nextFloat() * ink.getHeight() };
                g.setColour(grainGold.withAlpha(.9f - .75f * u));
                g.fillEllipse(circle(p, (1.f - u) * 1.3f + .45f));
            }
            break;
        }
        default: // Letterpress on cream with speed wings.
        {
            print(Colours::white.withAlpha(.65f), 0, 1);
            print(Colour(0xff17393d));
            g.reduceClipRegion(area);
            g.setColour(Colour(0xff17393d).withAlpha(.55f));
            for (int i = 0; i < 3; ++i)
            {
                const auto y = ink.getCentreY() + (i - 1) * 4.f;
                const auto length = 16.f - i * 5.f;
                g.drawLine(ink.getRight() + 5, y, ink.getRight() + 5 + length, y, 1.4f);
            }
            break;
        }
    }
}

void knobScale(juce::Graphics& g, Point centre, float radius, float start, float end, Colour ink)
{
    for (int i = 0; i <= 10; ++i)
    {
        const auto a = start + (end - start) * static_cast<float>(i) / 10.f;
        const auto major = i % 5 == 0;
        g.setColour(ink.withAlpha(major ? .85f : .55f));
        g.drawLine(juce::Line<float>(polar(centre, radius, a), polar(centre, radius + (major ? 4.5f : 3.f), a)),
                   major ? 1.5f : 1.f);
    }
}

bool cardHasHeader(size_t effect)
{
    return hasHeader(safeEffect(effect));
}

std::array<CardTile, 4> cardLayout(size_t effect)
{
    const auto e = safeEffect(effect);
    // Written for controls between y = 44 and 226; cards without a header stretch them up to y = 16.
    const auto place = [e](int x, int y, int w, int h, bool hero)
    {
        const auto top = juce::roundToInt(stretchedY(e, static_cast<float>(y)));
        const auto bottom = juce::roundToInt(stretchedY(e, static_cast<float>(y + h)));
        return CardTile { Area(x, top, w, bottom - top), hero };
    };
    const auto hero = [&place](int x, int y, int w, int h) { return place(x, y, w, h, true); };
    const auto small = [&place](int x, int y, int w, int h) { return place(x, y, w, h, false); };
    // Controls end at y = 226; the band below is reserved for each card's trim and ornaments.
    switch (e)
    {
        case 0: return { hero(6, 46, 114, 98), hero(120, 46, 114, 98), small(6, 144, 80, 82), small(154, 144, 80, 82) };
        case 1: return { small(6, 58, 64, 84), hero(70, 46, 100, 104), small(170, 58, 64, 84), small(80, 154, 80, 70) };
        case 2: return { hero(6, 46, 122, 98), small(136, 48, 98, 86), small(136, 138, 98, 86), small(18, 144, 98, 82) };
        case 3: return { hero(60, 128, 120, 98), small(6, 46, 76, 82), small(158, 46, 76, 82), small(82, 46, 76, 82) };
        case 4: return { small(82, 44, 76, 82), small(8, 94, 74, 84), small(158, 94, 74, 84), small(82, 144, 76, 82) };
        case 5: return { small(7, 44, 113, 90), small(120, 44, 113, 90), small(7, 136, 113, 90), small(120, 136, 113, 90) };
        case 6: return { hero(60, 46, 120, 98), small(6, 144, 76, 82), small(82, 144, 76, 82), small(158, 144, 76, 82) };
        // Stretch: the map screen fills the top (see cardScreen); three knobs share the row below and the
        // fourth tile is left empty.
        case 8: return { small(6, 134, 76, 84), small(82, 134, 76, 84), small(158, 134, 76, 84), CardTile {} };
        // VHS: the on-screen display fills the top, like the Stretch map, with three knobs below.
        case 9: return { small(6, 134, 76, 84), small(82, 134, 76, 84), small(158, 134, 76, 84), CardTile {} };
        // Grain: the cloud screen fills the top, with all four knobs in a row below it.
        case 10: return { small(6, 134, 57, 84), small(63, 134, 57, 84), small(120, 134, 57, 84), small(177, 134, 57, 84) };
        default: return { small(6, 46, 84, 84), hero(96, 46, 138, 98), small(6, 138, 84, 86), small(118, 144, 100, 82) };
    }
}

Area cardScreen(size_t effect)
{
    const auto e = safeEffect(effect);
    return e == 8 || e == 9 || e == 10 ? Area(8, 50, 224, 80) : Area();
}

void electricFrame(juce::Graphics& g, AreaF frame, float reach, float intensity, juce::uint32 seed)
{
    if (intensity <= .001f || reach <= 0 || frame.isEmpty())
        return;
    const auto w = frame.getWidth(), h = frame.getHeight();
    const auto perimeter = 2 * (w + h);
    // A point at arc length `d` clockwise from the top centre, with the frame's outward normal there.
    const auto along = [&frame, w, h, perimeter](float d) -> std::pair<Point, Point>
    {
        d = std::fmod(std::fmod(d, perimeter) + perimeter, perimeter);
        if (d < w * .5f)
            return { { frame.getCentreX() + d, frame.getY() }, { 0, -1 } };
        d -= w * .5f;
        if (d < h)
            return { { frame.getRight(), frame.getY() + d }, { 1, 0 } };
        d -= h;
        if (d < w)
            return { { frame.getRight() - d, frame.getBottom() }, { 0, 1 } };
        d -= w;
        if (d < h)
            return { { frame.getX(), frame.getBottom() - d }, { -1, 0 } };
        d -= h;
        return { { frame.getX() + d, frame.getY() }, { 0, -1 } };
    };

    juce::Random random(static_cast<juce::int64>(seed) * 7919 + 17);
    const auto length = perimeter * .5f * juce::jlimit(0.f, 1.f, reach);
    constexpr float step = 5.f;
    std::vector<Path> bolts;
    Path forks;
    std::vector<Point> heads;
    for (const auto direction : { 1.f, -1.f })
    {
        for (int strand = 0; strand < 3; ++strand)
        {
            Path bolt;
            for (float d = 0; d <= length; d += step)
            {
                const auto [p, normal] = along(direction * d);
                auto jitter = (random.nextFloat() - .5f) * (strand == 0 ? 6.f : strand == 1 ? 3.5f : 8.f);
                if (random.nextFloat() < .1f)
                    jitter += (random.nextFloat() - .35f) * 14.f;
                const auto q = p + normal * jitter;
                d == 0 ? bolt.startNewSubPath(q) : bolt.lineTo(q);
                if (strand == 0 && random.nextFloat() < .11f)
                {
                    // A fork crackling off the main arc, mostly out into the rack.
                    const auto tangent = (along(direction * (d + 1)).first - p) * direction;
                    const auto side = random.nextFloat() < .7f ? 1.f : -1.f;
                    auto f = q;
                    forks.startNewSubPath(f);
                    const auto segments = 3 + random.nextInt(3);
                    for (int k = 0; k < segments; ++k)
                    {
                        f = f + normal * side * (2.5f + random.nextFloat() * 5.f)
                              + tangent * direction * (random.nextFloat() * 9.f - 3.f);
                        forks.lineTo(f);
                    }
                }
            }
            bolts.push_back(std::move(bolt));
        }
        if (reach < 1.f)
            heads.push_back(along(direction * length).first);
    }

    const Colour glow(0xff2a7dff), mid(0xff5cbcff), core(0xffe8f8ff);
    const auto pen = [](float width) { return juce::PathStrokeType(width, juce::PathStrokeType::curved,
                                                                   juce::PathStrokeType::rounded); };
    // Light spills in from every lit edge, strongest once the frame has closed, when the whole face flashes.
    {
        const auto glowIn = intensity * (.35f + .65f * reach * reach);
        if (reach >= 1.f)
        {
            g.setColour(Colour(0xff3d8cff).withAlpha(.16f * intensity));
            g.fillRoundedRectangle(frame, 5);
        }
        const auto depth = juce::jmin(40.f, juce::jmin(w, h) * .3f);
        const auto edge = [&g, &glow, glowIn](AreaF band, Point from, Point to)
        {
            ColourGradient wash(glow.withAlpha(juce::jmin(1.f, .75f * glowIn)), from.x, from.y, glow.withAlpha(0.f),
                                to.x, to.y, false);
            wash.addColour(.3, glow.withAlpha(.3f * glowIn));
            g.setGradientFill(wash);
            g.fillRect(band);
        };
        edge(frame.withHeight(depth), frame.getTopLeft(), frame.getTopLeft().translated(0, depth));
        edge(frame.withTrimmedTop(h - depth), frame.getBottomLeft(), frame.getBottomLeft().translated(0, -depth));
        edge(frame.withWidth(depth), frame.getTopLeft(), frame.getTopLeft().translated(depth, 0));
        edge(frame.withTrimmedLeft(w - depth), frame.getTopRight(), frame.getTopRight().translated(-depth, 0));
        g.setColour(Colour(0xff9ad9ff).withAlpha(juce::jmin(1.f, .6f * glowIn)));
        g.drawRoundedRectangle(frame.reduced(1), 5, 3);
    }
    for (size_t i = 0; i < bolts.size(); ++i)
    {
        const auto& bolt = bolts[i];
        const auto main = i % 3 == 0;
        g.setColour(glow.withAlpha(.12f * intensity));
        g.strokePath(bolt, pen(main ? 30.f : 16.f));
        g.setColour(glow.withAlpha(.22f * intensity));
        g.strokePath(bolt, pen(main ? 16.f : 8.f));
        g.setColour(mid.withAlpha(.5f * intensity));
        g.strokePath(bolt, pen(main ? 7.f : 3.5f));
        g.setColour(mid.brighter(.4f).withAlpha((main ? .95f : .65f) * intensity));
        g.strokePath(bolt, pen(main ? 3.2f : 1.8f));
        g.setColour(core.withAlpha(intensity));
        g.strokePath(bolt, pen(main ? 1.6f : .9f));
    }
    g.setColour(glow.withAlpha(.18f * intensity));
    g.strokePath(forks, pen(9.f));
    g.setColour(mid.withAlpha(.55f * intensity));
    g.strokePath(forks, pen(3.f));
    g.setColour(core.withAlpha(.95f * intensity));
    g.strokePath(forks, pen(1.f));

    // The arriving current flares where each arc is heading.
    for (const auto head : heads)
    {
        g.setGradientFill(ColourGradient(core.withAlpha(intensity), head.x, head.y,
                                         glow.withAlpha(0.f), head.x + 22.f, head.y, true));
        g.fillEllipse(circle(head, 22.f));
    }
}

void electricBloom(juce::Graphics& g, AreaF frame, float intensity)
{
    if (intensity <= .001f || frame.isEmpty())
        return;
    const auto area = frame.toNearestInt();
    const auto level = juce::jmin(1.f, intensity);
    // Wide haze, a strong halo and a hot rim, stacked (some twice) so the light floods the rack around the card.
    const juce::DropShadow haze(Colour(0xff1b5cff).withAlpha(level), 170, {});
    haze.drawForRectangle(g, area.expanded(30));
    haze.drawForRectangle(g, area.expanded(30));
    const juce::DropShadow halo(Colour(0xff2a7dff).withAlpha(level), 80, {});
    halo.drawForRectangle(g, area.expanded(12));
    halo.drawForRectangle(g, area.expanded(12));
    halo.drawForRectangle(g, area.expanded(12));
    juce::DropShadow(Colour(0xff5fb8ff).withAlpha(level), 32, {}).drawForRectangle(g, area.expanded(5));
    juce::DropShadow(Colour(0xff5fb8ff).withAlpha(level), 32, {}).drawForRectangle(g, area.expanded(5));
    juce::DropShadow(Colour(0xffd6f0ff).withAlpha(level), 10, {}).drawForRectangle(g, area.expanded(2));
}

void inspectorPlate(juce::Graphics& g, Area area, size_t effect)
{
    // The expanded panel of a card: the same finish, header and trim as the card, spread over a larger plate.
    static constexpr std::array<float, 11> corners { 10.f, 16.f, 14.f, 7.f, 26.f, 12.f, 8.f, 18.f, 13.f, 9.f, 15.f };
    const auto e = safeEffect(effect);
    const auto whole = area.toFloat().reduced(1);
    const auto radius = corners[e];
    const Colour colour(faceColours[e]);
    raised(g, whole, radius, colour.brighter(.12f), colour.darker(.22f), .25f);

    const auto face = whole.withTrimmedBottom(wall(whole));
    const auto shape = rounded(face, radius);
    // The header band is 64 px tall from the top of the plate; the inspector centres its heading row on it.
    const auto top = area.toFloat().getY();
    const auto split = top + 64.f;
    const auto headingLine = top + 32.f;
    const auto header = face.withBottom(split);
    const auto body = face.withTop(split);
    const auto bottomBand = face.withTop(face.getBottom() - 9);
    juce::Random random(static_cast<juce::int64>(0x1a5e00 + e));

    {
        const juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(shape);
        switch (e)
        {
            case 0: // Tape: cream enamel under a brushed aluminium header.
                brushedBand(g, header, Colour(0xffeef0f2), Colour(0xffa3a7ac), random);
                engravedRule(g, face.getX(), face.getRight(), split);
                g.setColour(Colour(0xff3a2f25).withAlpha(.4f));
                g.drawRoundedRectangle(body.reduced(9, 7), 7, 1);
                break;
            case 1: // Tremolo: red hammertone under black satin, with an engraved wave.
            {
                hammertone(g, body, random, 520);
                verticalFill(g, header, Colour(0xff2c2c2e), Colour(0xff111112));
                g.fillRect(header);
                chromeRule(g, face.getX(), face.getRight(), split);
                Path wave;
                const auto from = face.getCentreX() + 40, to = face.getRight() - 22;
                for (auto x = from; x <= to; x += 1.f)
                {
                    const Point p { x, bottomBand.getCentreY() - 2.f + 3.f * std::sin((x - from) * .12f) };
                    x == from ? wave.startNewSubPath(p) : wave.lineTo(p);
                }
                g.setColour(palette::cream.withAlpha(.5f));
                g.strokePath(wave, juce::PathStrokeType(1.3f));
                break;
            }
            case 2: // Saturation: grille-cloth weave, black glass header, gold piping.
            {
                crossHatch(g, body, 4.f, .06f);
                verticalFill(g, header, Colour(0xff241a11), Colour(0xff0a0705));
                g.fillRect(header);
                g.setGradientFill(ColourGradient(Colours::white.withAlpha(.09f), 0, header.getY(),
                                                 Colours::transparentWhite, 0, header.getCentreY(), false));
                g.fillRect(header);
                const Colour gold(0xffd6a35c);
                g.setColour(gold);
                g.drawLine(face.getX(), split, face.getRight(), split, 1.8f);
                g.setColour(gold.withAlpha(.6f));
                g.drawLine(face.getX() + 12, face.getBottom() - 5, face.getRight() - 12, face.getBottom() - 5, 1.2f);
                break;
            }
            case 3: // Resampler and the shared panels: perforated lab box with a row of bit lamps.
            {
                dotGrid(g, body.reduced(10, 6), 6.f);
                verticalFill(g, header, Colour(0xff222a30), Colour(0xff12161a));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                static constexpr juce::uint16 bits = 0b1011010011010110;
                for (int i = 0; i < 16; ++i)
                {
                    const auto lit = (bits >> i & 1) != 0;
                    const auto cell = AreaF(5, 5).withCentre({ face.getRight() - 24.f - i * 9.f, bottomBand.getCentreY() - 1.f });
                    if (lit)
                    {
                        g.setColour(Colour(0xff7dff9c).withAlpha(.22f));
                        g.fillRoundedRectangle(cell.expanded(2.5f), 2);
                    }
                    g.setColour(lit ? Colour(0xff8dffaa) : Colour(0xff1d2a24));
                    g.fillRoundedRectangle(cell, 1);
                }
                break;
            }
            case 4: // Chorus: glossy orange, black capsule header and racing stripes round the corner.
            {
                g.setGradientFill(ColourGradient(Colours::white.withAlpha(.22f), 0, face.getY(),
                                                 Colours::transparentWhite, 0, face.getY() + face.getHeight() * .45f, false));
                g.fillRect(face);
                const auto capsule = header.reduced(10, 0).withTrimmedTop(headingLine - header.getY() - 25.f)
                                         .withBottom(headingLine + 25.f);
                g.setColour(Colour(0xff17130f));
                g.fillRoundedRectangle(capsule, capsule.getHeight() * .5f);
                g.setColour(Colours::white.withAlpha(.12f));
                g.drawRoundedRectangle(capsule.reduced(.5f), capsule.getHeight() * .5f, .8f);
                const std::array<Colour, 3> colours { Colour(0xff0a6fa8), Colour(0xff19c3e6), palette::cream };
                for (size_t k = 0; k < colours.size(); ++k)
                {
                    const auto o = 4.f + static_cast<float>(k) * 4.f;
                    const auto bend = juce::jmax(5.f, radius - o + 2.f);
                    Path stripe;
                    stripe.startNewSubPath(face.getCentreX(), face.getBottom() - o);
                    stripe.lineTo(face.getRight() - o - bend, face.getBottom() - o);
                    stripe.quadraticTo(face.getRight() - o, face.getBottom() - o, face.getRight() - o, face.getBottom() - o - bend);
                    stripe.lineTo(face.getRight() - o, split + 30);
                    g.setColour(colours[k]);
                    g.strokePath(stripe, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
                }
                break;
            }
            case 5: // Delay: plum echo unit with a chrome trim and fading repeats.
            {
                verticalFill(g, header, colour.darker(.55f), colour.darker(.8f));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                g.setColour(Colours::black.withAlpha(.4f));
                g.drawRoundedRectangle(face.reduced(4.5f).translated(0, 1), radius - 4, 1.3f);
                g.setColour(Colours::white.withAlpha(.3f));
                g.drawRoundedRectangle(face.reduced(4.5f), radius - 4, 1.3f);
                for (int i = 0; i < 9; ++i)
                {
                    const auto x = face.getRight() - 30.f - i * 11.f;
                    const auto width = 6.f - i * .5f;
                    Path dash;
                    dash.startNewSubPath(x + 3, bottomBand.getY());
                    dash.lineTo(x + 3 + width, bottomBand.getY());
                    dash.lineTo(x + width, bottomBand.getBottom() - 2);
                    dash.lineTo(x, bottomBand.getBottom() - 2);
                    dash.closeSubPath();
                    g.setColour(palette::stripeOrange.withAlpha(.85f - i * .09f));
                    g.fillPath(dash);
                }
                break;
            }
            case 6: // Reverse: olive field kit with rivets and a hazard band.
            {
                verticalFill(g, header, colour.darker(.45f), colour.darker(.75f));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                for (auto y = split + 16; y < face.getBottom() - 16; y += 28)
                {
                    rivet(g, { face.getX() + 7.5f, y }, 2.4f);
                    rivet(g, { face.getRight() - 7.5f, y }, 2.4f);
                }
                const auto hazard = bottomBand.withLeft(face.getCentreX());
                const juce::Graphics::ScopedSaveState band(g);
                g.reduceClipRegion(hazard.toNearestInt());
                g.setColour(Colour(0xffe8b923));
                g.fillRect(hazard);
                g.setColour(Colour(0xff161616));
                for (auto x = hazard.getX() - 10; x < hazard.getRight(); x += 12)
                {
                    Path stripe;
                    stripe.startNewSubPath(x, hazard.getBottom());
                    stripe.lineTo(x + 6, hazard.getBottom());
                    stripe.lineTo(x + 14, hazard.getY());
                    stripe.lineTo(x + 8, hazard.getY());
                    stripe.closeSubPath();
                    g.fillPath(stripe);
                }
                break;
            }
            case 8: // Stretch: indigo plate, amber LIVE rail and a spreading ruler along the bottom.
            {
                verticalFill(g, header, colour.darker(.5f), colour.darker(.8f));
                g.fillRect(header);
                engravedRule(g, face.getX(), face.getRight(), split);
                g.setColour(stretchAmber.withAlpha(.5f));
                g.fillRect(AreaF(face.getX() + 14, split + 3, face.getWidth() - 28, 1.4f));
                auto x = face.getCentreX();
                auto gap = 3.f;
                for (int n = 0; x < face.getRight() - 24.f; ++n)
                {
                    const auto major = n % 4 == 0;
                    g.setColour(palette::cream.withAlpha(major ? .7f : .4f));
                    g.fillRect(AreaF(x, bottomBand.getY() + 1, 1.2f, major ? 6.f : 3.5f));
                    x += gap;
                    gap *= 1.08f;
                }
                break;
            }
            case 9: // VHS: silver fascia over black plastic, the rainbow along the bottom.
            {
                brushedBand(g, body, colour.brighter(.08f), colour.darker(.35f), random);
                brushedBand(g, header, Colour(0xffd9dce0), Colour(0xff8b9096), random);
                chromeRule(g, face.getX(), face.getRight(), split, 2.f);
                const auto stripe = bottomBand.reduced(14, 2);
                for (size_t k = 0; k < vhsRainbow.size(); ++k)
                {
                    g.setColour(Colour(vhsRainbow[k]).withAlpha(.85f));
                    g.fillRect(stripe.withX(stripe.getX() + stripe.getWidth() * static_cast<float>(k) / 5.f)
                                   .withWidth(stripe.getWidth() / 5.f - 2.f));
                }
                break;
            }
            case 10: // Grain: emerald flake under a smoked header, a gold rule and a drift of particles.
                metalFlake(g, body, colour, random, 2600);
                verticalFill(g, header, Colour(0xff10261e), Colour(0xff050d0a));
                g.fillRect(header);
                goldRule(g, face.getX(), face.getRight(), split);
                particleTrail(g, bottomBand.reduced(14, 1), random);
                break;
            default: // Reverb: tolex body under a cream header, chrome rules.
                crossHatch(g, body, 3.f, .045f);
                verticalFill(g, header, Colour(0xfff2e7cd), Colour(0xffd3c29e));
                g.fillRect(header);
                chromeRule(g, face.getX(), face.getRight(), split);
                chromeRule(g, face.getCentreX(), face.getRight() - 14, face.getBottom() - 5, 2.5f);
                break;
        }
    }
    grain(g, shape, .55f);
    bevelEdge(g, face, whole, radius, .25f);

    const auto inset = 13.f;
    screw(g, face.getTopLeft().translated(inset, inset), 4.f, .4f);
    screw(g, face.getTopRight().translated(-inset, inset), 4.f, 1.1f);
    screw(g, face.getBottomLeft().translated(inset, -inset), 4.f, 2.1f);
    screw(g, face.getBottomRight().translated(-inset, -inset), 4.f, .9f);

    // Spec plate riveted into the header beside the close key, like the maker's badge on old gear, so it
    // never covers the controls below.
    // Right edge stops short of the close key; the plate is wide enough for the longest effect name.
    const auto badge = AreaF(236, 22).withCentre({ face.getRight() - 204.f, headingLine });
    g.setColour(Colours::black.withAlpha(.45f));
    g.fillRoundedRectangle(badge.translated(.8f, 1.6f), 3);
    verticalFill(g, badge, Colour(0xffeef0f2), Colour(0xff979ca2));
    g.fillRoundedRectangle(badge, 3);
    g.setColour(Colours::white.withAlpha(.6f));
    g.drawLine(badge.getX() + 3, badge.getY() + 1.f, badge.getRight() - 3, badge.getY() + 1.f, .8f);
    g.setColour(Colours::black.withAlpha(.25f));
    g.drawRoundedRectangle(badge.reduced(3.5f), 1.5f, .7f);
    g.setColour(Colours::black.withAlpha(.6f));
    g.drawRoundedRectangle(badge, 3, .9f);
    rivet(g, { badge.getX() + 8, badge.getCentreY() }, 2.3f);
    rivet(g, { badge.getRight() - 8, badge.getCentreY() }, 2.3f);
    const auto model = juce::String("MELTIT  ") + juce::String(effectName(e)).toUpperCase() + "  CT-0" + juce::String(e + 1);
    // Centred on the letters' own outline between the rivets, not on font metrics, so it sits level in the plate.
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(readoutFont(11.f), model, 0, 0);
    Path letters;
    glyphs.createPath(letters);
    const auto ink = letters.getBounds();
    const auto room = badge.reduced(16, 0);
    letters.applyTransform(juce::AffineTransform::translation(room.getCentreX() - ink.getCentreX(),
                                                              room.getCentreY() - ink.getCentreY()));
    g.setColour(Colours::white.withAlpha(.55f));
    g.fillPath(letters, juce::AffineTransform::translation(0, .8f));
    g.setColour(Colour(0xff23282c));
    g.fillPath(letters);
}

void seatedCartridge(juce::Graphics& g, Area area, size_t effect, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.beginTransparencyLayer(opacity);
    const auto bounds = area.toFloat();
    shell(g, bounds, .3f);
    const auto window = AreaF(bounds.getX() + bounds.getWidth() * .045f, bounds.getY() + bounds.getHeight() * .31f,
                              bounds.getWidth() * .91f, bounds.getHeight() * .62f);
    label(g, window, effect);

    // Small cartridges (the shelf) keep their name readable; seated ones are large enough not to need it.
    const auto size = juce::jmax(10.5f, window.getHeight() * .27f);
    const auto typeface = font(size, true);
    const auto textWidth = juce::GlyphArrangement::getStringWidth(typeface, effectName(effect));
    const auto tagHeight = juce::jmax(window.getHeight() * .3f, size + 2.5f);
    const AreaF tag(window.getX() + window.getWidth() * .05f, window.getBottom() - window.getHeight() * .08f - tagHeight,
                    juce::jmin(textWidth + size * 1.5f, window.getWidth() * .9f), tagHeight);
    g.setColour(Colour(0xff1a1715).withAlpha(.92f));
    g.fillRoundedRectangle(tag, tag.getHeight() * .22f);
    italicText(g, effectName(effect), typeface, tag, juce::Justification::centred, palette::cream);
    g.endTransparencyLayer();
}

void shelfCartridge(juce::Graphics& g, Area area, size_t effect, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.beginTransparencyLayer(opacity);
    const auto bounds = area.toFloat();
    shell(g, bounds, .24f);
    const auto window = AreaF(bounds.getX() + bounds.getWidth() * .07f, bounds.getY() + bounds.getHeight() * .25f,
                              bounds.getWidth() * .86f, bounds.getHeight() * .66f);
    label(g, window, effect);

    const juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(rounded(window, window.getHeight() * .06f));
    const auto band = window.withTop(window.getBottom() - window.getHeight() * .3f);
    g.setColour(Colour(0xfff7e0c0));
    g.fillRect(band);
    g.setColour(Colours::black.withAlpha(.25f));
    g.drawHorizontalLine(juce::roundToInt(band.getY()), band.getX(), band.getRight());
    const auto stripeBand = band.withLeft(band.getRight() - band.getWidth() * .3f).withTrimmedTop(band.getHeight() * .45f);
    stripes(g, stripeBand.withWidth(stripeBand.getWidth() + 4), stripeBand.getHeight());
    italicText(g, effectName(effect), font(band.getHeight() * .72f, true),
               band.withTrimmedLeft(band.getWidth() * .06f).withTrimmedRight(band.getWidth() * .3f),
               juce::Justification::centredLeft, Colour(0xff17130f));
}

void knob(juce::Graphics& g, Point c, float diameter, float angle, size_t effect)
{
    const auto r = diameter * .47f;
    const auto line = juce::jmax(1.3f, diameter * .036f);
    const Colour paint(0xffeceae4);

    switch (safeEffect(effect))
    {
        case 0: // Tape: black skirt stepped up to a turned aluminium cap.
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff2b2c2f));
            matte(g, c, r - 1.6f, Colour(0xff1c1d1f));
            edgeLight(g, c, r);
            indexLine(g, c, angle, r * .7f, r * .92f, line, paint);
            capShadow(g, c, r * .6f);
            chamfer(g, c, r * .6f, Colour(0xff232426));
            turned(g, c, r * .5f, Colour(0xff6c7075), Colour(0xfff4f5f6));
            edgeLight(g, c, r * .5f, .8f);
            break;

        case 1: // Tremolo: knurled ring around a raised cap with a white dot.
        {
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff2a2b2e));
            matte(g, c, r - 1.4f, Colour(0xff1b1c1e));
            const auto teeth = 56;
            for (int i = 0; i < teeth; ++i)
            {
                const auto a = angle + twoPi * static_cast<float>(i) / teeth;
                const auto facing = .5f + .5f * std::cos(a + .8f); // Teeth facing the lamp catch more light.
                g.setColour(i % 2 == 0 ? Colours::white.withAlpha(.05f + .12f * facing) : Colours::black.withAlpha(.45f));
                g.drawLine(juce::Line<float>(polar(c, r * .8f, a), polar(c, r - 1.4f, a)), .9f);
            }
            edgeLight(g, c, r, .8f);
            capShadow(g, c, r * .74f);
            chamfer(g, c, r * .74f, Colour(0xff303134));
            matte(g, c, r * .74f - 1.2f, Colour(0xff1e1f21));
            edgeLight(g, c, r * .74f);
            indexDot(g, c, angle, r * .5f, juce::jmax(1.6f, diameter * .05f), paint);
            break;
        }

        case 2: // Saturation: solid turned copper.
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff7a4523));
            turned(g, c, r - 1.4f, Colour(0xff4a230e), Colour(0xffffcf9f));
            edgeLight(g, c, r, 1.2f);
            indexLine(g, c, angle, r * .58f, r * .88f, line, Colour(0xff2a1406));
            break;

        case 3: // Resampler: knurled chrome dome.
        {
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff8a8e92));
            const auto teeth = 64;
            for (int i = 0; i < teeth; ++i)
            {
                const auto a = angle + twoPi * static_cast<float>(i) / teeth;
                g.setColour(i % 2 == 0 ? Colours::white.withAlpha(.45f) : Colours::black.withAlpha(.5f));
                g.drawLine(juce::Line<float>(polar(c, r * .86f, a), polar(c, r - .6f, a)), .9f);
            }
            chromeDome(g, c, r * .86f);
            indexLine(g, c, angle, r * .45f, r * .8f, line, Colour(0xff1d1f22));
            break;
        }

        case 4: // Chorus: pointer ("chicken head") knob with a chrome hub.
        {
            const auto body = r * .86f;
            const auto tip = r * 1.04f;
            const auto spread = std::acos(body / tip);
            Path shape;
            shape.addCentredArc(c.x, c.y, body, body, 0, angle + spread, angle + twoPi - spread, true);
            shape.lineTo(polar(c, tip, angle));
            shape.closeSubPath();

            contactShadow(g, c, body * 1.08f);
            const auto bounds = shape.getBounds();
            g.setGradientFill(ColourGradient(Colour(0xff4a4b4f), bounds.getX(), bounds.getY(),
                                             Colour(0xff0e0e0f), bounds.getRight(), bounds.getBottom(), false));
            g.fillPath(shape);
            const auto inset = juce::AffineTransform::scale(.9f, .9f, c.x, c.y);
            g.setGradientFill(ColourGradient(Colour(0xff2a2b2e), c.x - r * .5f, c.y - r * .6f,
                                             Colour(0xff151618), c.x + r * .6f, c.y + r * .7f, false));
            g.fillPath(shape, inset);
            g.setColour(Colours::white.withAlpha(.18f));
            g.strokePath(shape, juce::PathStrokeType(.8f), inset);

            // Recessed well with a polished hub and a centre screw.
            const auto well = r * .4f;
            g.setGradientFill(ColourGradient(Colour(0xff050506), c.x, c.y - well, Colour(0xff2a2b2e), c.x, c.y + well, false));
            g.fillEllipse(circle(c, well));
            capShadow(g, c, well * .7f);
            turned(g, c, well * .66f, Colour(0xff55595e), Colour(0xffffffff), 1.2f);
            g.setColour(Colour(0xff26282b));
            g.fillEllipse(circle(c, well * .18f));
            indexLine(g, c, angle, r * .55f, r * .88f, line, paint);
            break;
        }

        case 5: // Delay: wide skirt under a small cap, index painted on the skirt.
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff2c2d30));
            matte(g, c, r - 1.5f, Colour(0xff1d1e20));
            edgeLight(g, c, r);
            indexLine(g, c, angle, r * .5f, r * .92f, line, paint);
            capShadow(g, c, r * .56f);
            chamfer(g, c, r * .56f, Colour(0xff45474b));
            matte(g, c, r * .56f - 1.2f, Colour(0xff2d2e31));
            edgeLight(g, c, r * .56f);
            break;

        case 6: // Reverse: polished chrome bezel around a dished black face with an amber dot.
            contactShadow(g, c, r);
            turned(g, c, r, Colour(0xff2c2e31), Colour(0xffffffff), 1.35f);
            g.setColour(Colours::black.withAlpha(.6f));
            g.drawEllipse(circle(c, r - .35f), .7f);
            g.setColour(Colour(0xff0b0b0c));
            g.fillEllipse(circle(c, r * .84f));
            matte(g, c, r * .82f, Colour(0xff1e1f21), true);
            indexDot(g, c, angle, r * .66f, juce::jmax(1.5f, diameter * .045f), Colour(0xfff0a43c));
            break;

        case 8: // Stretch: knurled black skirt, amber index and a small turned brass cap.
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff26252e));
            matte(g, c, r - 1.5f, Colour(0xff17161d));
            g.setColour(Colours::white.withAlpha(.07f));
            for (int k = 0; k < 36; ++k)
            {
                const auto a = static_cast<float>(k) * MathConstants<float>::twoPi / 36.f;
                g.drawLine(juce::Line<float>(polar(c, r * .8f, a), polar(c, r - 1.8f, a)), .6f);
            }
            edgeLight(g, c, r);
            indexLine(g, c, angle, r * .62f, r * .93f, line, stretchAmber);
            capShadow(g, c, r * .5f);
            turned(g, c, r * .5f, Colour(0xff6b5320), Colour(0xffe9cf86), 1.1f);
            break;

        case 9: // VHS: a ribbed black jog wheel with a brushed silver centre and a red index.
        {
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff222428));
            matte(g, c, r - 1.4f, Colour(0xff15171a));
            for (int k = 0; k < 28; ++k)
            {
                const auto a = static_cast<float>(k) * MathConstants<float>::twoPi / 28.f;
                g.setColour(Colours::white.withAlpha(.1f));
                g.drawLine(juce::Line<float>(polar(c, r * .74f, a), polar(c, r - 1.5f, a)), .8f);
            }
            edgeLight(g, c, r);
            indexLine(g, c, angle, r * .58f, r * .92f, line, Colour(0xffff4a3d));
            capShadow(g, c, r * .5f);
            turned(g, c, r * .48f, Colour(0xff5d6166), Colour(0xffe9ecef));
            break;
        }

        case 10: // Grain: a black skirt with a cream index under a gold metal-flake cap.
        {
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff1d2522));
            matte(g, c, r - 1.5f, Colour(0xff121816));
            edgeLight(g, c, r);
            indexLine(g, c, angle, r * .6f, r * .92f, line, paint);
            capShadow(g, c, r * .52f);
            turned(g, c, r * .52f, Colour(0xff6b5320), Colour(0xfff0d68a), 1.1f);
            juce::Random flakes(0x9a17);
            for (int k = 0; k < 18; ++k)
            {
                const auto p = polar(c, r * .45f * std::sqrt(flakes.nextFloat()), flakes.nextFloat() * MathConstants<float>::twoPi);
                g.setColour(Colours::white.withAlpha(.15f + .4f * flakes.nextFloat()));
                g.fillRect(AreaF(p.x, p.y, .8f, .8f));
            }
            break;
        }

        default: // Reverb: broad teal anodised disc with a long index line.
            contactShadow(g, c, r);
            chamfer(g, c, r, Colour(0xff2a4447));
            matte(g, c, r - 1.5f, Colour(0xff1b3134));
            for (float ring = r - 2.5f; ring > 2.f; ring -= 1.2f)
            {
                g.setColour(Colours::white.withAlpha(.018f));
                g.drawEllipse(circle(c, ring), .5f);
            }
            edgeLight(g, c, r);
            indexLine(g, c, angle, r * .2f, r * .88f, line, paint);
            break;
    }
}

void knobShadow(juce::Graphics& g, Point centre, float diameter)
{
    const auto r = diameter * .47f;
    // Ambient occlusion spread wide around the base.
    const auto ambient = centre.translated(r * .12f, r * .25f);
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(.32f), ambient.x, ambient.y,
                                     Colours::transparentBlack, ambient.x + r * 1.75f, ambient.y, true));
    g.fillEllipse(circle(ambient, r * 1.75f));
    // Cast shadow: a soft copy of the knob pushed away from the lamp in the upper left.
    const auto cast = centre.translated(r * .5f, r * .62f);
    ColourGradient shade(Colours::black.withAlpha(.75f), cast.x, cast.y, Colours::transparentBlack, cast.x + r * 1.6f, cast.y, true);
    shade.addColour(.55, Colours::black.withAlpha(.68f));
    shade.addColour(.76, Colours::black.withAlpha(.32f));
    shade.addColour(.9, Colours::black.withAlpha(.09f));
    g.setGradientFill(shade);
    g.fillEllipse(circle(cast, r * 1.6f));
}

void seatShadow(juce::Graphics& g, Area cavity)
{
    const auto area = cavity.toFloat();
    const juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(cavity);
    g.setGradientFill(ColourGradient(Colours::black.withAlpha(.7f), 0, area.getY(),
                                     Colours::transparentBlack, 0, area.getY() + 12.f, false));
    g.fillRect(area);
    for (const auto& [from, to] : { std::pair { area.getX(), area.getX() + 7.f },
                                    std::pair { area.getRight(), area.getRight() - 7.f } })
    {
        g.setGradientFill(ColourGradient(Colours::black.withAlpha(.45f), from, 0, Colours::transparentBlack, to, 0, false));
        g.fillRect(area);
    }
}

// ---------------------------------------------------------------- controls

void key(juce::Graphics& g, Key face, Area area, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    auto body = fit(area.toFloat(), keyAspect(face)).reduced(1.f);
    // The side wall hangs below the key's face; drop the body by half of it so the face, not the body, is centred.
    body = body.translated(0, wall(body) * .5f);
    const auto radius = juce::jmin(body.getWidth(), body.getHeight()) * .2f;
    raised(g, body, radius, Colour(0xff434446), Colour(0xff151617), .2f);
    const auto cap = body.withTrimmedBottom(wall(body));
    const auto inner = cap.reduced(cap.getHeight() * .12f);
    const auto ink = palette::cream;
    const auto stroke = juce::jmax(1.4f, cap.getHeight() * .1f);
    const juce::PathStrokeType pen(stroke, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded);
    g.setColour(ink);
    switch (face)
    {
        case Key::generate:
        {
            centredText(g, "Generate", font(cap.getHeight() * .5f, true),
                        cap.withTrimmedBottom(cap.getHeight() * .22f), ink);
            const auto bar = AreaF(cap.getWidth() * .7f, juce::jmax(2.f, cap.getHeight() * .1f))
                                 .withCentre({ cap.getCentreX(), cap.getBottom() - cap.getHeight() * .22f });
            g.setColour(palette::amber.withAlpha(.18f));
            g.fillRoundedRectangle(bar.expanded(4.f, 3.f), 4.f);
            g.setColour(palette::amber.withAlpha(.3f));
            g.fillRoundedRectangle(bar.expanded(2.f, 1.5f), 3.f);
            g.setColour(palette::amber);
            g.fillRoundedRectangle(bar, 1.5f);
            break;
        }
        case Key::previous:
        case Key::next:
        {
            const auto c = cap.getCentre();
            const auto s = cap.getHeight() * .2f;
            const auto dir = face == Key::previous ? 1.f : -1.f;
            Path chevron;
            chevron.startNewSubPath(c.x + s * .5f * dir, c.y - s);
            chevron.lineTo(c.x - s * .5f * dir, c.y);
            chevron.lineTo(c.x + s * .5f * dir, c.y + s);
            g.strokePath(chevron, pen);
            break;
        }
        case Key::options:
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse(AreaF(stroke * 1.4f, stroke * 1.4f).withCentre(cap.getCentre().translated(i * cap.getHeight() * .22f, 0)));
            break;
        case Key::controls:
            for (const auto& [y, x] : { std::pair { -.22f, .25f }, std::pair { .22f, -.2f } })
            {
                const auto rowY = inner.getCentreY() + inner.getHeight() * y;
                g.drawLine(inner.getX() + 1, rowY, inner.getRight() - 1, rowY, juce::jmax(1.f, stroke * .6f));
                g.fillRoundedRectangle(AreaF(stroke * 1.5f, stroke * 2.6f)
                                           .withCentre({ inner.getCentreX() + inner.getWidth() * x, rowY }), 1);
            }
            break;
        case Key::close:
        case Key::inspectorClose:
        {
            const auto cross = inner.reduced(inner.getWidth() * .18f);
            g.drawLine(juce::Line<float>(cross.getTopLeft(), cross.getBottomRight()), stroke);
            g.drawLine(juce::Line<float>(cross.getBottomLeft(), cross.getTopRight()), stroke);
            break;
        }
        case Key::toggleOn:
        case Key::toggleOff:
        {
            const auto on = face == Key::toggleOn;
            centredText(g, on ? "ON" : "OFF", readoutFont(cap.getHeight() * .42f),
                        cap.withTrimmedBottom(on ? cap.getHeight() * .18f : 0), ink);
            if (on)
            {
                g.setColour(palette::amber);
                g.fillRect(AreaF(cap.getWidth() * .5f, juce::jmax(1.5f, cap.getHeight() * .07f))
                               .withCentre({ cap.getCentreX(), cap.getBottom() - cap.getHeight() * .25f }));
            }
            break;
        }
    }
}

void lamp(juce::Graphics& g, Area area, bool on, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    const auto bounds = fit(area.toFloat(), 1.f);
    const auto c = bounds.getCentre();
    const auto r = bounds.getWidth() * .5f;
    // Just the round lamp in its bezel: no glow outside it, which the square button would clip into a square.
    dome(g, c, r, Colour(0xff6a6e72), Colour(0xff141618));
    g.setColour(Colours::black.withAlpha(.7f));
    g.drawEllipse(bounds.reduced(.4f), .8f);
    const auto lens = bounds.reduced(r * .28f);
    if (on)
    {
        g.setGradientFill(ColourGradient(Colour(0xffd4fff3), c.x - r * .15f, c.y - r * .2f,
                                         Colour(0xff0fa985), c.x + r * .7f, c.y + r * .7f, true));
    }
    else
        g.setGradientFill(ColourGradient(Colour(0xff3e4a48), c.x - r * .15f, c.y - r * .2f,
                                         Colour(0xff111716), c.x + r * .7f, c.y + r * .7f, true));
    g.fillEllipse(lens);
    g.setColour(Colours::white.withAlpha(on ? .7f : .2f));
    g.fillEllipse(AreaF(r * .3f, r * .22f).withCentre(c.translated(-r * .15f, -r * .25f)));
}

void valueWell(juce::Graphics& g, Area area, bool compact)
{
    const auto bounds = area.toFloat().reduced(.5f);
    if (compact)
    {
        verticalFill(g, bounds, Colour(0xffd5d0c4), Colour(0xff6f6c66));
        g.fillRoundedRectangle(bounds, 3);
        g.setColour(Colours::black.withAlpha(.8f));
        g.drawRoundedRectangle(bounds, 3, 1);
        recess(g, bounds.reduced(2), 2, Colour(0xff151618), .0f);
        return;
    }
    // The full-size panels sit on dark metal, where a bare recess vanishes: the readout gets a gunmetal bezel, the
    // dark counterpart of the compact cards' silver one, around the same blue-black window as the selectors.
    const auto radius = juce::jmin(5.f, bounds.getHeight() * .28f);
    g.setColour(Colours::white.withAlpha(.07f));
    g.drawRoundedRectangle(bounds.translated(0, 1), radius, 1);
    verticalFill(g, bounds, Colour(0xff50575e), Colour(0xff1d2024));
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(Colours::black.withAlpha(.85f));
    g.drawRoundedRectangle(bounds, radius, 1);
    g.setColour(Colours::white.withAlpha(.16f));
    g.drawLine(bounds.getX() + radius, bounds.getY() + 1.f, bounds.getRight() - radius, bounds.getY() + 1.f, .8f);
    const auto window = bounds.reduced(2.5f);
    recess(g, window, juce::jmax(1.5f, radius - 2.f), Colour(0xff0d1b24), 0.f);
    g.setColour(Colour(0xff6fb6cc).withAlpha(.14f));
    g.drawRoundedRectangle(window.reduced(1.f), juce::jmax(1.f, radius - 3.f), .8f);
}

void selector(juce::Graphics& g, Area area, bool focused, int chevronWidth, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    const auto bounds = area.toFloat().reduced(.5f);
    const auto radius = juce::jmin(5.f, bounds.getHeight() * .22f);
    recess(g, bounds, radius, focused ? Colour(0xff17323e) : Colour(0xff0d1b24), .12f);
    g.setColour(focused ? Colour(0xff6fb6cc).withAlpha(.8f) : Colour(0xff364753));
    g.drawRoundedRectangle(bounds.reduced(.5f), radius, 1);

    const auto box = bounds.withLeft(bounds.getRight() - static_cast<float>(chevronWidth)).reduced(2);
    raised(g, box, radius * .7f, Colour(0xff34393d), Colour(0xff181b1e), .12f);
    const auto c = box.getCentre();
    const auto s = box.getHeight() * .16f;
    Path chevron;
    chevron.startNewSubPath(c.x - s * 1.3f, c.y - s * .6f);
    chevron.lineTo(c.x, c.y + s * .7f);
    chevron.lineTo(c.x + s * 1.3f, c.y - s * .6f);
    g.setColour(palette::cream);
    g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
}

void inspectorSwitch(juce::Graphics& g, Area area, bool on, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    const auto bounds = area.toFloat().reduced(.5f);
    const auto radius = bounds.getHeight() * .22f;
    recess(g, bounds, radius, Colour(0xff10171b), .12f);
    auto thumb = bounds.reduced(3);
    thumb = on ? thumb.withTrimmedLeft(thumb.getWidth() * .5f) : thumb.withWidth(thumb.getWidth() * .5f);
    raised(g, thumb, radius * .8f, Colour(0xffefe6d1), Colour(0xffaaa18a), .5f);
    grain(g, rounded(thumb, radius * .8f), .4f);
}

void scrollTrack(juce::Graphics& g, Area area, bool thumb, float opacity)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.setOpacity(opacity);
    const auto bounds = area.toFloat().reduced(1);
    const auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * .5f;
    if (thumb)
    {
        const auto horizontal = bounds.getWidth() > bounds.getHeight();
        g.setGradientFill(ColourGradient(Colour(0xffe8e2d2), bounds.getX(), bounds.getY(), Colour(0xff8f897b),
                                         horizontal ? bounds.getX() : bounds.getRight(),
                                         horizontal ? bounds.getBottom() : bounds.getY(), false));
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(Colours::black.withAlpha(.7f));
        g.drawRoundedRectangle(bounds, radius, .8f);
        return;
    }
    recess(g, bounds, radius, Colour(0xff0a0d0f), .08f);
}

// ---------------------------------------------------------------- menus and notices

void menuPanel(juce::Graphics& g, Area area)
{
    const auto bounds = area.toFloat().reduced(.5f);
    constexpr float radius = 7.f;
    verticalFill(g, bounds, Colour(0xff2d3236), Colour(0xff15181b));
    g.fillRoundedRectangle(bounds, radius);
    grain(g, rounded(bounds, radius), .6f);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(.06f), 0, bounds.getY(),
                                     Colours::transparentWhite, 0, bounds.getY() + 24, false));
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(Colours::white.withAlpha(.12f));
    g.drawLine(bounds.getX() + radius, bounds.getY() + 1.2f, bounds.getRight() - radius, bounds.getY() + 1.2f, 1.f);
    g.setColour(Colours::black.withAlpha(.9f));
    g.drawRoundedRectangle(bounds, radius, 1.f);
}

void menuItem(juce::Graphics& g, Area area, const juce::String& text, bool separator, bool enabled,
              bool highlighted, bool ticked, bool hasSubMenu)
{
    const auto bounds = area.toFloat();
    if (separator)
    {
        const auto y = bounds.getCentreY();
        g.setColour(Colours::black.withAlpha(.6f));
        g.drawLine(bounds.getX() + 10, y, bounds.getRight() - 10, y, 1.f);
        g.setColour(Colours::white.withAlpha(.1f));
        g.drawLine(bounds.getX() + 10, y + 1, bounds.getRight() - 10, y + 1, 1.f);
        return;
    }

    const auto row = bounds.reduced(3.f, 1.f);
    if (highlighted && enabled)
    {
        // Lit like a backlit legend: a warm glow with an amber index bar on the left.
        g.setGradientFill(ColourGradient(palette::amber.withAlpha(.3f), row.getX(), 0,
                                         palette::amber.withAlpha(.08f), row.getRight(), 0, false));
        g.fillRoundedRectangle(row, 4);
        g.setColour(palette::amber.withAlpha(.55f));
        g.drawRoundedRectangle(row.reduced(.5f), 4, .8f);
        g.setColour(palette::amber);
        g.fillRoundedRectangle(row.withWidth(3).reduced(0, 4), 1.5f);
    }

    // A selected entry shows a lit lamp instead of a tick.
    if (ticked)
        lamp(g, Area(10, 10).withCentre({ juce::roundToInt(bounds.getX() + 16), juce::roundToInt(bounds.getCentreY()) }),
             true, enabled ? 1.f : .5f);

    auto textArea = bounds.withTrimmedLeft(28).withTrimmedRight(hasSubMenu ? 26 : 12);
    const auto ink = !enabled ? palette::cream.withAlpha(.35f) : highlighted ? Colour(0xfffff4dd) : palette::cream;
    g.setFont(font(15.f));
    g.setColour(Colours::black.withAlpha(.5f));
    g.drawFittedText(text, textArea.translated(0, 1).toNearestInt(), juce::Justification::centredLeft, 1, .8f);
    g.setColour(ink);
    g.drawFittedText(text, textArea.toNearestInt(), juce::Justification::centredLeft, 1, .8f);

    if (hasSubMenu)
    {
        const auto c = Point(bounds.getRight() - 14, bounds.getCentreY());
        Path chevron;
        chevron.startNewSubPath(c.x - 2.5f, c.y - 4.5f);
        chevron.lineTo(c.x + 2.5f, c.y);
        chevron.lineTo(c.x - 2.5f, c.y + 4.5f);
        g.setColour(ink);
        g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }
}

void notice(juce::Graphics& g, Area area, const juce::String& heading, const juce::String& message, float glow)
{
    const auto bounds = area.toFloat().reduced(1);
    constexpr float radius = 9.f;
    raised(g, bounds, radius, Colour(0xff32373b), Colour(0xff15181b), .18f);
    const auto face = bounds.withTrimmedBottom(wall(bounds));
    grain(g, rounded(face, radius), .6f);
    for (auto p : { face.getTopLeft().translated(9, 9), face.getTopRight().translated(-9, 9),
                    face.getBottomLeft().translated(9, -9), face.getBottomRight().translated(-9, -9) })
        screw(g, p, 2.6f, p.x * .05f);

    // Amber warning lamp, pulsing.
    const auto lampCentre = Point(face.getX() + 34, face.getCentreY());
    const auto lampRadius = 13.f;
    dome(g, lampCentre, lampRadius, Colour(0xff6a6e72), Colour(0xff141618));
    g.setColour(Colours::black.withAlpha(.7f));
    g.drawEllipse(circle(lampCentre, lampRadius), .8f);
    const auto lit = Colour(0xffffb347).interpolatedWith(Colour(0xff7a4a10), 1.f - glow);
    g.setGradientFill(ColourGradient(Colour(0xfffff0c8).interpolatedWith(lit, .4f + .6f * (1.f - glow)),
                                     lampCentre.x - 3, lampCentre.y - 4, lit.darker(.5f), lampCentre.x + 9, lampCentre.y + 9, true));
    g.fillEllipse(circle(lampCentre, lampRadius * .7f));
    g.setColour(Colours::white.withAlpha(.25f + .4f * glow));
    g.fillEllipse(AreaF(6, 4).withCentre(lampCentre.translated(-3, -4)));
    g.setColour(Colour(0xff2a1a06).withAlpha(.85f));
    g.setFont(headingFont(14.f));
    g.drawText("!", circle(lampCentre, lampRadius * .7f).toNearestInt(), juce::Justification::centred);

    const auto text = face.withTrimmedLeft(62).reduced(10, 10);
    g.setFont(headingFont(17.f));
    g.setColour(Colours::black.withAlpha(.6f));
    g.drawText(heading, text.withHeight(22).translated(0, 1).toNearestInt(), juce::Justification::centredLeft);
    g.setColour(palette::cream);
    g.drawText(heading, text.withHeight(22).toNearestInt(), juce::Justification::centredLeft);
    g.setFont(font(13.5f));
    g.setColour(palette::cream.withAlpha(.82f));
    g.drawFittedText(message, text.withTrimmedTop(24).toNearestInt(), juce::Justification::topLeft, 2, .9f);
}

// ---------------------------------------------------------------- VCR display

void vfdReadout(juce::Graphics& g, Area area, const juce::String& caption, const juce::String& value,
                const juce::String& unit, float proportion, bool lit, bool bipolar, float brightness, VfdBloom& bloom)
{
    const auto bounds = area.toFloat().reduced(.5f);
    const auto radius = 4.f;
    // Smoked glass set into the panel, with the faint blue-green cast of a lit vacuum fluorescent tube.
    recess(g, bounds, radius, Colour(0xff050a0b), .12f);
    const auto glass = bounds.reduced(2.f);
    g.setGradientFill(ColourGradient(Colour(0xff0b1a1a), glass.getCentreX(), glass.getCentreY(),
                                     Colour(0xff040708), glass.getX(), glass.getY(), true));
    g.fillRoundedRectangle(glass, radius - 1.f);

    const Colour phosphor(lit ? 0xffc2fff6 : 0xff74f2df);
    const juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(rounded(glass, radius - 1.f));

    auto inner = glass.reduced(7.f, 4.f);
    const auto bar = inner.removeFromBottom(3.f);
    inner.removeFromBottom(2.f);
    const auto captionFont = font(10.5f, true);
    const auto captionText = caption.toUpperCase();
    const auto captionWidth = juce::GlyphArrangement::getStringWidth(captionFont, captionText);
    // The digits shrink to fit the room the caption and unit leave them, so long values never run into the caption.
    const auto unitWidth = unit.isEmpty() ? 0.f
                                          : 5.f + juce::GlyphArrangement::getStringWidth(font(10.5f, false), unit.toUpperCase());
    const auto digitRoom = inner.getWidth() - captionWidth - unitWidth - 6.f;
    auto digitSize = juce::jmin(20.f, inner.getHeight() * .95f);
    while (digitSize > 8.f && juce::GlyphArrangement::getStringWidth(readoutFont(digitSize), value) > digitRoom)
        digitSize -= .5f;
    const auto digits = readoutFont(digitSize);
    // The unit follows the value at the right edge ("177 %"), its baseline level with the digits'.
    const auto unitFont = font(10.5f, false);
    const auto digitArea = inner.withTrimmedRight(unitWidth);
    const auto unitDrop = ((digits.getAscent() - digits.getDescent()) - (unitFont.getAscent() - unitFont.getDescent())) * .5f;

    // Level ladder along the bottom, lit up to the setting; a bipolar setting lights out from the centre.
    constexpr int cells = 20;
    const auto pitch = bar.getWidth() / cells;
    const auto reach = juce::roundToInt(juce::jlimit(0.f, 1.f, proportion) * cells);
    const auto from = bipolar ? juce::jmin(reach, cells / 2) : 0;
    const auto to = bipolar ? juce::jmax(reach, cells / 2) : reach;
    const auto cell = [&](int i) { return AreaF(bar.getX() + i * pitch, bar.getY(), pitch - 1.5f, bar.getHeight()); };

    // Everything that is lit: the caption, the digits with their dimmer unit after them, the ladder.
    const auto glowLevel = juce::jlimit(0.f, 1.2f, brightness);
    const auto litParts = [&](juce::Graphics& target, float strength)
    {
        target.setFont(captionFont);
        target.setColour(phosphor.withMultipliedAlpha(.85f * strength));
        target.drawText(captionText, inner, juce::Justification::centredLeft);
        target.setFont(unitFont);
        target.setColour(phosphor.withMultipliedAlpha(.5f * strength));
        target.drawText(unit.toUpperCase(), inner.translated(0.f, unitDrop), juce::Justification::centredRight);
        target.setFont(digits);
        target.setColour(phosphor.withMultipliedAlpha(strength));
        target.drawText(value, digitArea, juce::Justification::centredRight);
        target.setColour(phosphor.withMultipliedAlpha(.8f * strength));
        for (int i = from; i < to; ++i)
            target.fillRect(cell(i));
    };

    // A faint wash of light from the tube behind the digits.
    g.setGradientFill(ColourGradient(phosphor.withAlpha((lit ? .12f : .07f) * glowLevel), inner.getRight() - inner.getWidth() * .2f,
                                     inner.getCentreY(), phosphor.withAlpha(0.f), inner.getX(), inner.getCentreY(), true));
    g.fillRect(glass);

    // Unlit segments behind the digits and the ladder, as on a real display.
    juce::String ghost;
    for (auto character : value)
        ghost << (character == '.' || character == ' ' ? juce::String::charToString(character) : juce::String("8"));
    g.setFont(digits);
    g.setColour(phosphor.withAlpha(.07f));
    g.drawText(ghost, digitArea, juce::Justification::centredRight);
    for (int i = 0; i < cells; ++i)
    {
        if (i >= from && i < to)
            continue;
        const auto centreMark = bipolar && (i == cells / 2 - 1 || i == cells / 2);
        g.setColour(phosphor.withAlpha(centreMark ? .22f : .08f));
        g.fillRect(cell(i));
    }

    // Bloom: the lit parts blurred wide and soft, then tight and bright, under the sharp glyphs. The blur is
    // worked out at the panel's own resolution (it is soft anyway) and kept until the reading changes, so the
    // flicker only has to fade the pictures.
    const auto box = glass.getSmallestIntegerContainer();
    const auto key = caption + "|" + unit + "|" + value + "|" + juce::String(from) + ":" + juce::String(to) + "|"
                   + juce::String(lit ? 1 : 0) + "|" + box.toString();
    if (key != bloom.key)
    {
        bloom.key = key;
        juce::Image source(juce::Image::ARGB, juce::jmax(1, box.getWidth()), juce::jmax(1, box.getHeight()), true);
        {
            juce::Graphics painted(source);
            painted.addTransform(juce::AffineTransform::translation(static_cast<float>(-box.getX()),
                                                                    static_cast<float>(-box.getY())));
            litParts(painted, 1.f);
        }
        const auto blurred = [&source](int size, float blur)
        {
            juce::Image result(juce::Image::ARGB, source.getWidth(), source.getHeight(), true);
            juce::ImageConvolutionKernel kernel(size);
            kernel.createGaussianBlur(blur);
            kernel.applyToImage(result, source, source.getBounds());
            return result;
        };
        bloom.wide = blurred(23, 9.f);
        bloom.mid = blurred(11, 3.5f);
        bloom.tight = blurred(5, 1.2f);
    }
    // The wide halo swells and sags more than the glyphs themselves, as a tube's glow does.
    const auto layer = [&g, &box](const juce::Image& image, float opacity)
    {
        g.setOpacity(juce::jlimit(0.f, 1.f, opacity));
        g.drawImageAt(image, box.getX(), box.getY());
    };
    const auto halo = glowLevel * glowLevel;
    layer(bloom.wide, (lit ? 1.f : .8f) * halo);
    layer(bloom.mid, (lit ? .9f : .7f) * glowLevel);
    layer(bloom.tight, .6f * glowLevel);
    g.setOpacity(1.f);
    litParts(g, juce::jmin(1.f, .35f + .65f * glowLevel));

    // Reflection across the top of the glass.
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(.07f), 0, glass.getY(),
                                     Colours::transparentWhite, 0, glass.getCentreY(), false));
    g.fillRoundedRectangle(glass, radius - 1.f);
}

// ---------------------------------------------------------------- CRT and video artefacts

void crtGlass(juce::Graphics& g, AreaF screen, float radius)
{
    const juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(rounded(screen, radius));
    // Scan lines: a dark line on every other row, as a small monitor shows up close.
    g.setColour(Colours::black.withAlpha(.2f));
    for (auto y = std::floor(screen.getY()) + 1.f; y < screen.getBottom(); y += 2.f)
        g.fillRect(screen.getX(), y, screen.getWidth(), .9f);
    // The tube's curved face: darker towards the corners, a soft sheen across the top.
    ColourGradient corners(Colours::transparentBlack, screen.getCentreX(), screen.getCentreY(),
                           Colours::black.withAlpha(.45f), screen.getX(), screen.getY(), true);
    corners.addColour(.6, Colours::transparentBlack);
    g.setGradientFill(corners);
    g.fillRect(screen);
    Path sheen;
    sheen.addRoundedRectangle(screen.reduced(screen.getWidth() * .04f, 2.f).withHeight(screen.getHeight() * .32f),
                              radius);
    g.setGradientFill(ColourGradient(Colours::white.withAlpha(.07f), 0, screen.getY(),
                                     Colours::transparentWhite, 0, screen.getY() + screen.getHeight() * .34f, false));
    g.fillPath(sheen);
}

void crtTrace(juce::Graphics& g, const Path& trace, Colour colour, float width)
{
    // Phosphor bloom: wide faint strokes under the line itself.
    for (const auto& [spread, alpha] : { std::pair { 7.f, .06f }, std::pair { 4.f, .12f }, std::pair { 2.2f, .22f } })
    {
        g.setColour(colour.withMultipliedAlpha(alpha));
        g.strokePath(trace, juce::PathStrokeType(width + spread, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour(colour);
    g.strokePath(trace, juce::PathStrokeType(width, juce::PathStrokeType::mitered));
}

void trackingNoise(juce::Graphics& g, AreaF screen, float radius, float intensity, double seconds, juce::uint32 seed)
{
    intensity = juce::jlimit(0.f, 1.f, intensity);
    if (intensity <= .01f || screen.isEmpty())
        return;
    const juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(rounded(screen, radius));
    const auto frame = static_cast<juce::int64>(seconds * 30.0);
    juce::Random random(frame * 7919 + seed);

    // Snow across the whole picture.
    const auto specks = juce::roundToInt(screen.getWidth() * screen.getHeight() * .012f * intensity);
    for (int i = 0; i < specks; ++i)
    {
        g.setColour(Colours::white.withAlpha(.08f + .3f * random.nextFloat() * intensity));
        g.fillRect(screen.getX() + random.nextFloat() * screen.getWidth(), screen.getY() + random.nextFloat() * screen.getHeight(),
                   1.f + 3.f * random.nextFloat(), 1.f);
    }

    // Tracking bands rolling up the screen: a torn strip of noise with colour fringes along its edges.
    const auto bands = intensity > .55f ? 2 : 1;
    for (int b = 0; b < bands; ++b)
    {
        const auto speed = .35 + .2 * b;
        const auto travel = std::fmod(seconds * speed + .43 * b + seed * .137, 1.0);
        const auto height = (3.f + 9.f * intensity) * (1.f - .35f * b);
        const auto y = screen.getBottom() + height - static_cast<float>(travel) * (screen.getHeight() + 2.f * height);
        const AreaF band(screen.getX(), y, screen.getWidth(), height);
        g.setColour(Colours::white.withAlpha(.06f + .1f * intensity));
        g.fillRect(band);
        for (auto x = band.getX(); x < band.getRight();)
        {
            const auto length = 2.f + random.nextFloat() * 16.f;
            const auto row = band.getY() + random.nextFloat() * band.getHeight();
            g.setColour(Colours::white.withAlpha((.15f + .55f * random.nextFloat()) * intensity));
            g.fillRect(x, row, length, 1.f);
            x += length + random.nextFloat() * 10.f;
        }
        g.setColour(Colour(0xffff3b6a).withAlpha(.35f * intensity));
        g.fillRect(band.getX(), band.getY() - 1.f, band.getWidth(), 1.f);
        g.setColour(Colour(0xff3bd8ff).withAlpha(.35f * intensity));
        g.fillRect(band.getX(), band.getBottom(), band.getWidth(), 1.f);
    }

    // Now and then the whole picture tears sideways for a frame.
    if (random.nextFloat() < .12f * intensity)
    {
        const auto row = screen.getY() + random.nextFloat() * screen.getHeight();
        g.setColour(Colours::black.withAlpha(.5f * intensity));
        g.fillRect(screen.getX(), row, screen.getWidth(), 2.f + 3.f * random.nextFloat());
    }
}

void vcrScreen(juce::Graphics& g, AreaF screen, const juce::String& speed, bool on, bool playing, double counter,
               float tracking, float wear, double seconds, juce::uint32 seed)
{
    const auto radius = 5.f;
    g.setColour(Colours::white.withAlpha(.1f));
    g.drawRoundedRectangle(screen.translated(0, 1), radius, 1);
    if (!on)
    {
        g.setColour(Colour(0xff08090b));
        g.fillRoundedRectangle(screen, radius);
        crtGlass(g, screen, radius);
        return;
    }
    {
        const juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(rounded(screen, radius));
        verticalFill(g, screen, Colour(0xff2547d6), Colour(0xff10279a));
        g.fillRect(screen);

        // On-screen display: white block letters with a hard black edge, as the recorder draws them.
        const auto osd = [&g](const juce::String& text, AreaF area, juce::Justification justification, float size)
        {
            g.setFont(readoutFont(size).boldened());
            g.setColour(Colours::black.withAlpha(.75f));
            g.drawText(text, area.translated(1.2f, 1.2f), justification);
            g.setColour(Colour(0xfff4f7ff));
            g.drawText(text, area, justification);
        };
        auto inner = screen.reduced(9.f, 7.f);
        auto top = inner.removeFromTop(16.f);
        auto bottom = inner.removeFromBottom(14.f);

        // PLAY with its arrow, or STOP with a square.
        const auto icon = top.removeFromLeft(12.f).withSizeKeepingCentre(9.f, 10.f);
        Path symbol;
        if (playing)
            symbol.addTriangle(icon.getTopLeft(), icon.getBottomLeft(), { icon.getRight(), icon.getCentreY() });
        else
            symbol.addRectangle(icon.reduced(.5f, 1.f));
        g.setColour(Colours::black.withAlpha(.75f));
        g.fillPath(symbol, juce::AffineTransform::translation(1.2f, 1.2f));
        g.setColour(Colour(0xfff4f7ff));
        g.fillPath(symbol);
        top.removeFromLeft(4.f);
        osd(playing ? "PLAY" : "STOP", top, juce::Justification::centredLeft, 13.f);
        osd(speed, top, juce::Justification::centredRight, 13.f);

        // Tape counter.
        const auto total = static_cast<int>(counter);
        osd(juce::String(total / 3600) + ":" + juce::String((total / 60) % 60).paddedLeft('0', 2) + ":"
                + juce::String(total % 60).paddedLeft('0', 2),
            bottom, juce::Justification::centredRight, 11.5f);

        // Tracking meter: the worse the tracking, the further the bar is off centre.
        auto meter = bottom.removeFromLeft(bottom.getWidth() * .55f);
        osd("TRK", meter.removeFromLeft(26.f), juce::Justification::centredLeft, 9.5f);
        constexpr int cells = 9;
        const auto cellWidth = meter.getWidth() / cells;
        const auto offset = juce::roundToInt(juce::jlimit(0.f, 1.f, tracking) * (cells / 2));
        for (int i = 0; i < cells; ++i)
        {
            const AreaF cell(meter.getX() + i * cellWidth + 1.f, meter.getCentreY() - 3.f, cellWidth - 2.f, 6.f);
            const auto lit = i == cells / 2 + offset || i == cells / 2 - offset;
            g.setColour(Colours::black.withAlpha(.6f));
            g.fillRect(cell.translated(1.f, 1.f));
            g.setColour(lit ? Colour(0xfff4f7ff) : Colour(0xfff4f7ff).withAlpha(.3f));
            g.fillRect(cell);
        }
    }
    // A poorly tracked, worn tape shows it on the picture; a stopped one only faintly.
    const auto trouble = (playing ? .1f + .9f * tracking * tracking : .05f) + .25f * wear;
    trackingNoise(g, screen, radius, trouble, seconds, seed);
    crtGlass(g, screen, radius);
    g.setColour(Colours::black.withAlpha(.7f));
    g.drawRoundedRectangle(screen, radius, 1);
}
}
