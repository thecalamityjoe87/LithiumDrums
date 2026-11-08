// LithiumIconGen: writes the app icon and the installers' artwork, all drawn
// from the same logo code as the header (src/ui/Logo.cpp).
//
//   LithiumIconGen <output-folder>
//
// It writes:
//   icon-<n>.png                     the app icon at 16 to 1024 px
//   installer-side.png, -2x.png      the Windows wizard's welcome/finish panel (164 x 314)
//   installer-small.png, -2x.png     the Windows wizard's top-right picture (55 x 55)
//   installer-mac-background.png     the macOS Installer's background (logo bottom left)
//
// scripts/make-icons.sh runs it and converts the results into the .ico and
// .bmp files the installers need. Its output is committed, so the build only
// compiles this tool; run the script by hand after changing the logo.

#include "LithiumFonts.h"
#include "ui/Logo.h"
#include "ui/Theme.h"

#include <iostream>

using namespace lithium;

namespace
{
    bool writePng (const juce::Image& image, const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        return out.openedOk() && png.writeImageToStream (image, out);
    }

    juce::Font font (const char* data, int size, float height)
    {
        return juce::Font (juce::FontOptions (juce::Typeface::createSystemTypefaceFor (data, (size_t) size)).withPointHeight (height));
    }

    // Letter-spaced text, centred on x (the header's wordmark style).
    void drawTracked (juce::Graphics& g, const juce::String& text, const juce::Font& f, float tracking, float centreX, float baseline)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (f, text, 0.0f, baseline);

        const auto extra = tracking * f.getHeight();
        const auto width = glyphs.getBoundingBox (0, -1, true).getWidth() + extra * (float) (text.length() - 1);
        auto x = centreX - width * 0.5f;

        for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
        {
            auto glyph = glyphs.getGlyph (i);
            glyph.moveBy (x - glyph.getLeft(), 0.0f);
            glyph.draw (g);
            x += glyph.getRight() - glyph.getLeft() + extra;
        }
    }

    // The Windows wizard's tall picture: the mark over the wordmark.
    juce::Image sidePanel (float scale)
    {
        using namespace theme;
        const auto w = (int) std::round (164 * scale), h = (int) std::round (314 * scale);
        juce::Image image (juce::Image::RGB, w, h, true);
        juce::Graphics g (image);

        g.setGradientFill (juce::ColourGradient (colours::raisedHigh, 0.0f, 0.0f, colours::background, 0.0f, (float) h, false));
        g.fillAll();

        const auto mark = 92.0f * scale;
        const auto centre = juce::Point<float> ((float) w * 0.5f, (float) h * 0.36f);
        logo::drawMark (g, juce::Rectangle<float> (mark, mark).withCentre (centre), colours::accent,
                        colours::raisedHigh.interpolatedWith (colours::background, 0.36f));

        using namespace LithiumFonts;
        const auto baseline = centre.y + mark * 0.5f + 46.0f * scale;
        g.setColour (colours::text);
        drawTracked (g, "LITHIUM", font (BarlowCondensedBold_ttf, BarlowCondensedBold_ttfSize, 27.0f * scale), 0.14f, (float) w * 0.5f, baseline);
        g.setColour (colours::textMuted);
        drawTracked (g, "DRUMS", font (BarlowCondensedSemiBold_ttf, BarlowCondensedSemiBold_ttfSize, 27.0f * scale), 0.14f, (float) w * 0.5f, baseline + 27.0f * scale);
        g.setFont (font (BarlowRegular_ttf, BarlowRegular_ttfSize, 12.0f * scale));
        g.drawText ("Calamity Audio", juce::Rectangle<float> (0.0f, (float) h - 34.0f * scale, (float) w, 16.0f * scale),
                    juce::Justification::centred);
        return image;
    }

    // The macOS Installer draws its background under the left-hand step
    // list; the icon sits at the bottom left of it.
    juce::Image macBackground()
    {
        juce::Image image (juce::Image::ARGB, 620, 418, true);
        juce::Graphics g (image);
        g.drawImageAt (logo::renderIcon (128), 46, 418 - 128 - 34);
        return image;
    }
}

int main (int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: LithiumIconGen <output-folder>" << std::endl;
        return 1;
    }

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    folder.createDirectory();
    bool ok = true;

    for (const auto size : { 16, 24, 32, 48, 64, 128, 256, 512, 1024 })
        ok &= writePng (logo::renderIcon (size), folder.getChildFile ("icon-" + juce::String (size) + ".png"));

    ok &= writePng (sidePanel (1.0f), folder.getChildFile ("installer-side.png"));
    ok &= writePng (sidePanel (2.0f), folder.getChildFile ("installer-side-2x.png"));
    ok &= writePng (logo::renderIcon (55), folder.getChildFile ("installer-small.png"));
    ok &= writePng (logo::renderIcon (110), folder.getChildFile ("installer-small-2x.png"));
    ok &= writePng (macBackground(), folder.getChildFile ("installer-mac-background.png"));

    std::cout << (ok ? "Wrote the icons to " : "Couldn't write all the icons to ") << folder.getFullPathName() << std::endl;
    return ok ? 0 : 1;
}
