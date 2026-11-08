// "Install Lithium Drums": the graphical installer in the Linux download.
//
// One window: what Lithium Drums is, a row per thing to install (standalone
// app, VST3, CLAP, LithiumImport) with where it goes, Install (or Update /
// Reinstall when it's already there) and Remove. The work itself is in
// Installation.cpp.
//
// It installs the files sitting next to it in payload/ (the package script
// puts them there), into the user's home folder: no root, no password.
//
// Without a display, or for scripts, the same thing runs from the command line:
//   "Install Lithium Drums" --install [standalone,vst3,clap,tools]
//   "Install Lithium Drums" --remove
// LITHIUM_INSTALLER_PAYLOAD overrides where the payload is (for testing).

#include "Installation.h"
#include "ui/LithiumLookAndFeel.h"
#include "ui/Logo.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <thread>

using namespace lithium;
using namespace lithium::installer;

namespace
{
    juce::File payloadFolder()
    {
        if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_INSTALLER_PAYLOAD", {}); env.isNotEmpty())
            return juce::File (env);

        return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("payload");
    }

    juce::StringArray defaultComponents()
    {
        juce::StringArray ids;

        for (const auto& c : components())
            if (c.selectedByDefault)
                ids.add (c.id);

        return ids;
    }

    //==========================================================================
    /** One thing to install: a tick box, its name and what it's for, and
        where it goes. The whole row toggles. */
    class ComponentRow final : public juce::Button
    {
    public:
        explicit ComponentRow (Part c) : juce::Button (c.name), component (std::move (c))
        {
            setClickingTogglesState (true);
            setToggleState (component.selectedByDefault, juce::dontSendNotification);
            setWantsKeyboardFocus (true);
        }

        const Part component;

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            using namespace theme;
            auto area = getLocalBounds().toFloat();
            const auto on = getToggleState();

            g.setColour (over && isEnabled() ? colours::raisedHigh : colours::raised);
            g.fillRoundedRectangle (area, cornerRadius);
            g.setColour (on ? colours::accent.withAlpha (0.5f) : colours::border);
            g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);

            if (hasKeyboardFocus (false))
                LithiumLookAndFeel::drawFocusRing (g, area, cornerRadius);

            area.reduce (14.0f, 0.0f);
            const auto box = area.removeFromLeft (18.0f).withSizeKeepingCentre (18.0f, 18.0f);
            g.setColour (on ? colours::accent : colours::borderStrong);
            on ? g.fillRoundedRectangle (box, 4.0f) : g.drawRoundedRectangle (box.reduced (0.75f), 4.0f, 1.5f);

            if (on)
            {
                juce::Path tick;
                tick.startNewSubPath (box.getX() + 4.5f, box.getCentreY() + 0.5f);
                tick.lineTo (box.getX() + 7.8f, box.getBottom() - 4.8f);
                tick.lineTo (box.getRight() - 4.0f, box.getY() + 5.0f);
                g.setColour (colours::background);
                g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            area.removeFromLeft (14.0f);
            const auto where = area.removeFromRight (150.0f);
            const auto alpha = isEnabled() ? 1.0f : 0.5f;

            g.setColour (colours::text.withMultipliedAlpha (alpha));
            g.setFont (body (15.0f, Weight::semibold));
            g.drawText (component.name, area.withTrimmedTop (9.0f).withHeight (20.0f), juce::Justification::centredLeft, true);
            g.setColour (colours::textMuted.withMultipliedAlpha (alpha));
            g.setFont (body (13.0f));
            g.drawText (component.description, area.withTrimmedTop (29.0f).withHeight (18.0f), juce::Justification::centredLeft, true);
            g.setFont (mono (12.0f));
            g.drawText (component.where, where, juce::Justification::centredRight, true);
        }
    };

    //==========================================================================
    class InstallerView final : public juce::Component, private juce::Timer
    {
    public:
        InstallerView() : installation (payloadFolder(), LITHIUM_VERSION_STRING)
        {
            for (const auto& c : components())
                addAndMakeVisible (rows.add (std::make_unique<ComponentRow> (c)));

            licenseLink.setFont (theme::body (13.0f), false, juce::Justification::centredLeft);
            licenseLink.setColour (juce::HyperlinkButton::textColourId, theme::colours::accent);
            licenseLink.onClick = [this]
            {
                if (const auto file = payloadFolder().getChildFile ("LICENSE"); file.existsAsFile())
                    file.startAsProcess();
                else
                    juce::URL ("https://www.gnu.org/licenses/agpl-3.0.html").launchInDefaultBrowser();
            };
            addAndMakeVisible (licenseLink);

            installButton.setColour (juce::TextButton::buttonColourId, theme::colours::accent);
            installButton.setColour (juce::TextButton::textColourOffId, theme::colours::background);
            openButton.setColour (juce::TextButton::buttonColourId, theme::colours::accent);
            openButton.setColour (juce::TextButton::textColourOffId, theme::colours::background);

            installButton.onClick = [this] { start (true); };
            removeButton.onClick = [this] { confirmRemove(); };
            openButton.onClick = [this]
            {
                installation.standaloneApp().startAsProcess();
                closeWindow();
            };
            closeButton.onClick = [this] { closeWindow(); };

            for (auto* b : { &installButton, &removeButton, &openButton, &closeButton })
                addChildComponent (b);

            if (const auto error = installation.checkPayload(); error.isNotEmpty())
                setStatus (error, true);

            refreshExisting();
            setSize (680, 624);
        }

        ~InstallerView() override
        {
            if (worker.joinable())
                worker.join();
        }

        void paint (juce::Graphics& g) override
        {
            using namespace theme;
            g.fillAll (colours::background);

            // The header: the logo and wordmark as in the plugin, then who made it.
            g.setColour (colours::panel);
            g.fillRect (getLocalBounds().removeFromTop (headerHeight));
            g.setColour (colours::border);
            g.fillRect (0, headerHeight - 1, getWidth(), 1);

            logo::drawMark (g, { 28.0f, 22.0f, 36.0f, 36.0f }, colours::accent, colours::panel);
            const auto bold = label (26.0f, Weight::bold);
            const auto lithiumWidth = getTrackedTextWidth ("LITHIUM", bold, 0.14f);
            const auto drumsWidth = getTrackedTextWidth ("DRUMS", label (26.0f, Weight::semibold), 0.14f);
            const juce::Rectangle<float> row (80.0f, 16.0f, 400.0f, 30.0f);
            g.setColour (colours::text);
            drawTrackedText (g, "LITHIUM", bold, 0.14f, row, juce::Justification::centredLeft);
            g.setColour (colours::textMuted);
            drawTrackedText (g, "DRUMS", label (26.0f, Weight::semibold), 0.14f, row.withTrimmedLeft (lithiumWidth + 8.0f), juce::Justification::centredLeft);
            g.setFont (mono (13.0f));
            g.drawText (LITHIUM_VERSION_STRING, row.withTrimmedLeft (lithiumWidth + drumsWidth + 22.0f).withTrimmedTop (4.0f),
                        juce::Justification::centredLeft, false);
            g.setFont (body (14.0f));
            g.drawText ("by Isaac Joseph, Calamity Audio", juce::Rectangle<float> (81.0f, 46.0f, 400.0f, 20.0f), juce::Justification::centredLeft, false);

            g.setColour (colours::text);
            g.setFont (body (15.0f));
            g.drawFittedText ("Use Lithium Drums as a plugin in your DAW, or on its own as a standalone app.\nPick what to install:",
                              introArea, juce::Justification::topLeft, 2, 1.0f);

            g.setColour (colours::textMuted);
            g.setFont (body (13.0f));
            g.drawFittedText ("Factory presets are always installed. The drum kits aren't in this download: open Lithium Drums "
                              "and pick one in the kit browser, and it downloads it for you.",
                              noteArea, juce::Justification::topLeft, 2, 1.0f);
            g.drawText ("Free software under the GNU AGPLv3, with no warranty.", licenseArea, juce::Justification::centredLeft, false);

            // Progress, while working; the status line under it.
            if (working || progress > 0.0f)
            {
                auto bar = progressArea.toFloat();
                g.setColour (colours::raised);
                g.fillRoundedRectangle (bar, 3.0f);
                g.setColour (colours::accent);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress)), 3.0f);
            }

            g.setColour (statusIsError ? colours::warning : colours::text);
            g.setFont (body (14.0f));
            g.drawFittedText (status, statusArea, juce::Justification::topLeft, 3, 1.0f);
        }

        void resized() override
        {
            auto area = getLocalBounds().withTrimmedTop (headerHeight).reduced (28, 20);

            introArea = area.removeFromTop (44);
            area.removeFromTop (6);

            for (auto* row : rows)
            {
                row->setBounds (area.removeFromTop (58));
                area.removeFromTop (8);
            }

            area.removeFromTop (2);
            noteArea = area.removeFromTop (38);
            licenseArea = area.removeFromTop (22);
            const auto font = theme::body (13.0f);
            const auto textWidth = (int) std::ceil (theme::getTrackedTextWidth ("Free software under the GNU AGPLv3, with no warranty.", font, 0.0f));
            licenseLink.setBounds (licenseArea.getX() + textWidth + 8, licenseArea.getY(), 96, licenseArea.getHeight());

            auto buttons = area.removeFromBottom (40);
            area.removeFromBottom (14);
            progressArea = area.removeFromTop (16).withTrimmedTop (10);
            statusArea = area.withTrimmedTop (8);

            auto right = buttons;

            // Right to left: the main action at the right edge.
            for (auto* b : { &installButton, &closeButton, &openButton })
            {
                if (! b->isVisible())
                    continue;

                b->setBounds (right.removeFromRight (b == &openButton ? 190 : 140));
                right.removeFromRight (10);
            }

            removeButton.setBounds (buttons.removeFromLeft (120));
        }

    private:
        static constexpr int headerHeight = 80;

        void refreshExisting()
        {
            const auto existing = installation.existing();

            if (existing.installed)
            {
                for (auto* row : rows)
                    row->setToggleState (existing.components.contains (row->component.id), juce::dontSendNotification);

                installButton.setButtonText (existing.version == LITHIUM_VERSION_STRING ? "Reinstall" : "Update");

                if (! statusIsError)
                    setStatus ("Lithium Drums " + existing.version + " is installed. "
                               + (existing.version == LITHIUM_VERSION_STRING ? "Reinstalling" : "Updating") + " replaces it with what's ticked above.", false);
            }
            else
            {
                installButton.setButtonText ("Install");
            }

            installButton.setVisible (true);
            installButton.setEnabled (installation.checkPayload().isEmpty());
            removeButton.setVisible (existing.installed);
            openButton.setVisible (false);
            closeButton.setVisible (false);
            resized();
        }

        void setStatus (const juce::String& text, bool error)
        {
            status = text;
            statusIsError = error;
            repaint();
        }

        void confirmRemove()
        {
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Remove Lithium Drums?",
                "This removes the plugins, the app and its menu entry. Your kits, presets and settings stay in "
                + Installation::dataHome().getChildFile ("Lithium Drums").getFullPathName() + ".", "Remove", "Cancel", this);

            juce::AlertWindow::showAsync (options, [safe = juce::Component::SafePointer (this)] (int result)
            {
                if (safe != nullptr && result == 1)
                    safe->start (false);
            });
        }

        // Runs the install or removal on a worker thread so the window keeps
        // painting. The worker only touches the shared state below (atomics,
        // and the status string under its lock); the timer copies it into
        // the UI on the message thread.
        void start (bool install)
        {
            if (working)
                return;

            juce::StringArray ids;

            for (auto* row : rows)
                if (row->getToggleState())
                    ids.add (row->component.id);

            if (install && ids.isEmpty())
            {
                setStatus ("Tick at least one thing to install.", true);
                return;
            }

            if (worker.joinable())
                worker.join();

            working = true;
            installing = install;
            installedStandalone = ids.contains ("standalone");
            done = false;
            progress = 0.0f;
            setStatus ({}, false);

            for (auto* c : getChildren())
                c->setEnabled (false);

            worker = std::thread ([this, install, ids]
            {
                const auto report = [this] (const juce::String& s, float p)
                {
                    const juce::ScopedLock lock (workerLock);
                    workerStatus = s;
                    workerProgress = p;
                };

                const auto error = install ? installation.install (ids, report) : installation.remove (report);

                const juce::ScopedLock lock (workerLock);
                workerError = error;
                done = true;
            });

            startTimerHz (30);
        }

        void timerCallback() override
        {
            juce::String s, error;
            bool finished;
            {
                const juce::ScopedLock lock (workerLock);
                s = workerStatus;
                progress = workerProgress;
                finished = done;
                error = workerError;
            }

            if (! finished)
            {
                setStatus (s, false);
                return;
            }

            stopTimer();
            working = false;

            for (auto* c : getChildren())
                c->setEnabled (true);

            if (error.isNotEmpty())
            {
                setStatus (error, true);
                refreshExisting();
                return;
            }

            if (installing)
            {
                setStatus ("Installed. In your DAW, rescan plugins and add Lithium Drums (by Calamity Audio) to an instrument track."
                           + juce::String (installedStandalone ? " Or open the standalone app from your app menu." : ""), false);
                installButton.setVisible (false);
                removeButton.setVisible (false);
                openButton.setVisible (installedStandalone && installation.standaloneApp().existsAsFile());
                closeButton.setVisible (true);
                resized();
            }
            else
            {
                progress = 0.0f;
                statusIsError = false;
                refreshExisting();
                setStatus ("Lithium Drums is removed. Your kits, presets and settings are still in "
                           + Installation::dataHome().getChildFile ("Lithium Drums").getFullPathName() + ".", false);
            }
        }

        void closeWindow()
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        Installation installation;
        juce::OwnedArray<ComponentRow> rows;
        juce::HyperlinkButton licenseLink { "View licence", {} };
        juce::TextButton installButton { "Install" }, removeButton { "Remove" }, openButton { "Open Lithium Drums" }, closeButton { "Close" };

        juce::Rectangle<int> introArea, noteArea, licenseArea, progressArea, statusArea;
        juce::String status;
        bool statusIsError = false, working = false, installing = true, installedStandalone = false;
        float progress = 0.0f;

        std::thread worker;
        juce::CriticalSection workerLock;
        juce::String workerStatus, workerError;   // guarded by workerLock
        float workerProgress = 0.0f;              // guarded by workerLock
        bool done = false;                        // guarded by workerLock
    };

    //==========================================================================
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow() : juce::DocumentWindow ("Install Lithium Drums", theme::colours::background, closeButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new InstallerView(), true);
            setResizable (false, false);
            setIcon (logo::renderIcon (256));
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    //==========================================================================
    class InstallerApp final : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override    { return "Install Lithium Drums"; }
        const juce::String getApplicationVersion() override { return LITHIUM_VERSION_STRING; }
        // True: with false, JUCE hands a second launch's command line to the
        // running instance and exits, which would make "--install" from a
        // script silently do nothing while a window is open.
        bool moreThanOneInstanceAllowed() override           { return true; }

        void initialise (const juce::String& commandLine) override
        {
            const auto args = juce::StringArray::fromTokens (commandLine, true);

            if (args.contains ("--install") || args.contains ("--remove") || args.contains ("--help"))
            {
                setApplicationReturnValue (runCommandLine (args));
                quit();
                return;
            }

            // Made here, not as a member: its fonts need JUCE running.
            lookAndFeel = std::make_unique<LithiumLookAndFeel>();
            juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());
            window = std::make_unique<MainWindow>();
        }

        void shutdown() override
        {
            window.reset();
            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
            lookAndFeel.reset();
        }

        void systemRequestedQuit() override { quit(); }

    private:
        static int runCommandLine (const juce::StringArray& args)
        {
            if (args.contains ("--help"))
            {
                std::cout << "usage: \"Install Lithium Drums\" --install [standalone,vst3,clap,tools]\n"
                             "       \"Install Lithium Drums\" --remove\n"
                             "Run without arguments for the installer window." << std::endl;
                return 0;
            }

            Installation installation (payloadFolder(), LITHIUM_VERSION_STRING);
            const auto report = [] (const juce::String& s, float) { std::cout << s << std::endl; };
            juce::String error;

            if (args.contains ("--remove"))
            {
                error = installation.remove (report);
            }
            else
            {
                const auto list = args[args.indexOf ("--install") + 1];
                auto ids = list.isEmpty() || list.startsWith ("-") ? defaultComponents() : juce::StringArray::fromTokens (list, ",", {});
                error = installation.install (ids, report);
            }

            if (error.isNotEmpty())
                std::cerr << error << std::endl;

            return error.isEmpty() ? 0 : 1;
        }

        std::unique_ptr<LithiumLookAndFeel> lookAndFeel;
        std::unique_ptr<MainWindow> window;
    };
}

START_JUCE_APPLICATION (InstallerApp)
