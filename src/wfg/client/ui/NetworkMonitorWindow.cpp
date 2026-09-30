/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <wfg/client/ui/NetworkMonitorWindow.h>

#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        /*  HOW MANY LINES ARE KEPT: a busy page pushing values twenty-five
            times a second fills this in a few minutes, and the oldest go. A
            monitor is for what just happened; the log is for the evening. */
        constexpr std::size_t mostLines = 20000;

        enum Column { timeColumn = 1, directionColumn, roadColumn, peerColumn, addressColumn,
                      typesColumn, valuesColumn };

        juce::String timeOf (std::int64_t wallMicros)
        {
            const auto millis = wallMicros / 1000;
            return juce::Time (millis).formatted ("%H:%M:%S") + "."
                     + juce::String (static_cast<int> (millis % 1000)).paddedLeft ('0', 3);
        }
    }

    class NetworkMonitorWindow::Content final : public juce::Component,
                                                private juce::TableListBoxModel
    {
    public:
        Content (const model::Theme& themeToUse, std::function<void (bool)> onListen)
            : theme (themeToUse), listen (std::move (onListen))
        {
            recordToggle.setToggleState (true, juce::dontSendNotification);
            recordToggle.setTooltip ("Recording while ticked. Unticked, the lines stay and nothing new"
                                     " is kept - and the engine records nothing at all.");
            recordToggle.onClick = [this] { if (listen) listen (listening()); sayCount(); };

            for (auto* toggle : { &inToggle, &outToggle, &oscToggle, &midiToggle, &pageToggle })
            {
                toggle->setToggleState (true, juce::dontSendNotification);
                toggle->onClick = [this] { readFilter(); };
            }

            inToggle.setTooltip ("Show what Go.dot received");
            outToggle.setTooltip ("Show what Go.dot sent");
            oscToggle.setTooltip ("Show OSC messages");
            midiToggle.setTooltip ("Show MIDI messages");
            pageToggle.setTooltip ("Show the web page's own traffic: its commands, and the values pushed to it");

            filterBox.setTextToShowWhenEmpty ("filter: address, value or peer",
                                              Look::colour (theme, "ink-off"));
            filterBox.onTextChange = [this] { readFilter(); };

            clearButton.setTooltip ("Forget every line shown so far");
            clearButton.onClick = [this]
            {
                lines.clear();
                shown.clear();
                firstId += kept;
                kept = 0;
                missedAtClear = missedSoFar;
                table.updateContent();
                sayCount();
            };

            exportButton.setTooltip ("Write the lines the filters show to a CSV file");
            exportButton.onClick = [this] { exportCsv(); };

            for (juce::Component* child : std::initializer_list<juce::Component*> {
                     &recordToggle, &inToggle, &outToggle, &oscToggle, &midiToggle, &pageToggle,
                     &filterBox, &clearButton, &exportButton, &countLabel, &table })
                addAndMakeVisible (child);

            auto& header = table.getHeader();
            const auto columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
            header.addColumn ("time", timeColumn, 104, 60, 200, columnFlags);
            header.addColumn ("", directionColumn, 58, 40, 90, columnFlags);
            header.addColumn ("road", roadColumn, 50, 36, 90, columnFlags);
            header.addColumn ("peer", peerColumn, 170, 60, 400, columnFlags);
            header.addColumn ("address or message", addressColumn, 250, 80, 800, columnFlags);
            header.addColumn ("types", typesColumn, 60, 30, 200, columnFlags);
            header.addColumn ("values", valuesColumn, 420, 80, 2000, columnFlags);
            header.setStretchToFitActive (false);
            header.setColour (juce::TableHeaderComponent::backgroundColourId, Look::colour (theme, "panel-high"));
            header.setColour (juce::TableHeaderComponent::highlightColourId, Look::colour (theme, "panel-high"));
            header.setColour (juce::TableHeaderComponent::textColourId, Look::colour (theme, "ink-dim"));
            header.setColour (juce::TableHeaderComponent::outlineColourId, Look::colour (theme, "rule"));

            table.setRowHeight (juce::roundToInt (20.0 * theme.type));
            table.setColour (juce::ListBox::backgroundColourId, Look::colour (theme, "panel-cue"));
            table.setColour (juce::ListBox::outlineColourId, Look::colour (theme, "rule"));
            table.setMultipleSelectionEnabled (false);

            countLabel.setJustificationType (juce::Justification::centredRight);
            countLabel.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));

            sayCount();
        }

        bool listening() const noexcept { return recordToggle.getToggleState(); }

        void add (const std::vector<monitor::Capture>& captures, std::uint64_t droppedSoFar)
        {
            missedSoFar = droppedSoFar;

            if (! listening() || captures.empty())
            {
                sayCount();
                return;
            }

            const auto atBottom = isAtBottom();

            for (const auto& capture : captures)
                for (auto& row : model::describe (capture))
                {
                    lines.push_back (std::move (row));

                    if (filter.matches (lines.back()))
                        shown.push_back (firstId + kept);

                    ++kept;
                }

            //  The oldest go, and with them their places in what is shown.
            while (lines.size() > mostLines)
            {
                lines.pop_front();
                ++firstId;
                --kept;
            }

            while (! shown.empty() && shown.front() < firstId)
                shown.pop_front();

            table.updateContent();

            if (atBottom && ! shown.empty())
                table.scrollToEnsureRowIsOnscreen (static_cast<int> (shown.size()) - 1);

            sayCount();
        }

        std::size_t lineCount() const noexcept { return lines.size(); }
        std::size_t shownCount() const noexcept { return shown.size(); }
        const model::TrafficRow& shownRow (std::size_t index) const { return rowOf (shown[index]); }

        void setFilter (const model::TrafficFilter& wanted)
        {
            for (auto [toggle, on] : { std::pair { &inToggle, wanted.in }, std::pair { &outToggle, wanted.out },
                                       std::pair { &oscToggle, wanted.osc }, std::pair { &midiToggle, wanted.midi },
                                       std::pair { &pageToggle, wanted.page } })
                toggle->setToggleState (on, juce::dontSendNotification);

            filterBox.setText (juce::String (wanted.text), juce::dontSendNotification);
            readFilter();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (theme, "panel"));
        }

        void resized() override
        {
            const auto row = juce::roundToInt (theme.row * theme.type);
            auto area = getLocalBounds().reduced (row / 3);
            auto bar = area.removeFromTop (row);

            recordToggle.setBounds (bar.removeFromLeft (row * 4));
            bar.removeFromLeft (row / 2);

            for (auto* toggle : { &inToggle, &outToggle, &oscToggle, &midiToggle })
                toggle->setBounds (bar.removeFromLeft (row * 2 + row / 2));

            pageToggle.setBounds (bar.removeFromLeft (row * 4));
            bar.removeFromLeft (row / 2);

            exportButton.setBounds (bar.removeFromRight (row * 4).reduced (0, 2));
            bar.removeFromRight (row / 4);
            clearButton.setBounds (bar.removeFromRight (row * 3).reduced (0, 2));
            bar.removeFromRight (row / 2);
            filterBox.setBounds (bar.removeFromLeft (juce::jmin (row * 10, bar.getWidth())).reduced (0, 2));

            auto foot = area.removeFromBottom (row);
            countLabel.setBounds (foot);

            area.removeFromTop (row / 4);
            table.setBounds (area);
        }

    private:
        int getNumRows() override { return static_cast<int> (shown.size()); }

        const model::TrafficRow& rowOf (std::uint64_t id) const
        {
            return lines[static_cast<std::size_t> (id - firstId)];
        }

        void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height, bool selected) override
        {
            if (selected)
                g.fillAll (Look::colour (theme, "picked").withAlpha (0.18f));

            juce::ignoreUnused (rowNumber);
            g.setColour (Look::colour (theme, "rule").withAlpha (0.4f));
            g.fillRect (0, height - 1, width, 1);
        }

        void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override
        {
            if (rowNumber < 0 || rowNumber >= static_cast<int> (shown.size()))
                return;

            const auto& row = rowOf (shown[static_cast<std::size_t> (rowNumber)]);
            const auto area = juce::Rectangle<int> (0, 0, width, height).reduced (4, 0);

            g.setFont (Look::font (theme, 12.0f));
            g.setColour (Look::colour (theme, "ink"));

            juce::String said;

            switch (columnId)
            {
                case timeColumn:
                    g.setColour (Look::colour (theme, "ink-faint"));
                    said = timeOf (row.wallMicros);
                    break;

                case directionColumn:
                    /*  THE ARROW POINTS THE WAY IT WENT, and the word says it
                        too (§4.8); the tint only agrees with them. */
                    g.setColour (Look::colour (theme, row.incoming ? "kind-media" : "kind-transport"));
                    said = juce::String::fromUTF8 (row.incoming ? "\xe2\x86\x90 in" : "out \xe2\x86\x92");
                    break;

                case roadColumn:
                    g.setColour (Look::colour (theme, "ink-dim"));
                    said = row.road;
                    break;

                case peerColumn:
                    g.setColour (Look::colour (theme, "ink-dim"));
                    said = juce::String::fromUTF8 (row.peer.c_str());
                    break;

                case addressColumn:
                    said = juce::String::fromUTF8 (row.address.c_str());
                    break;

                case typesColumn:
                    g.setColour (Look::colour (theme, "ink-faint"));
                    said = row.types;
                    break;

                case valuesColumn:
                    if (! row.problem.empty())
                    {
                        g.setColour (Look::colour (theme, "failed"));
                        said = "could not be read: " + juce::String::fromUTF8 (row.problem.c_str());
                    }
                    else
                        said = juce::String::fromUTF8 (row.arguments.c_str());
                    break;

                default:
                    break;
            }

            g.drawText (said, area, juce::Justification::centredLeft, true);
        }

        void readFilter()
        {
            model::TrafficFilter next;
            next.in = inToggle.getToggleState();
            next.out = outToggle.getToggleState();
            next.osc = oscToggle.getToggleState();
            next.midi = midiToggle.getToggleState();
            next.page = pageToggle.getToggleState();
            next.text = filterBox.getText().toStdString();

            if (next == filter)
                return;

            filter = next;
            shown.clear();

            for (std::size_t at = 0; at < lines.size(); ++at)
                if (filter.matches (lines[at]))
                    shown.push_back (firstId + at);

            table.updateContent();
            table.repaint();
            sayCount();
        }

        bool isAtBottom()
        {
            auto* viewport = table.getViewport();

            if (viewport == nullptr || viewport->getViewedComponent() == nullptr)
                return true;

            return viewport->getViewPositionY() + viewport->getViewHeight()
                     >= viewport->getViewedComponent()->getHeight() - table.getRowHeight();
        }

        void sayCount()
        {
            const auto missed = missedSoFar - missedAtClear;
            auto words = juce::String (static_cast<int> (shown.size())) + " shown of "
                           + juce::String (static_cast<int> (lines.size())) + " kept";

            if (missed > 0)
                words << "  -  " << juce::String (static_cast<juce::int64> (missed))
                      << " not seen: more arrived at once than the monitor could keep";

            if (! listening())
                words << "  -  paused";

            countLabel.setText (words, juce::dontSendNotification);
            countLabel.setColour (juce::Label::textColourId,
                                  Look::colour (theme, missed > 0 ? "stopping" : "ink-dim"));
        }

        void exportCsv()
        {
            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");

            chooser = std::make_unique<juce::FileChooser> (
                "Save the lines shown as CSV",
                juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                    .getChildFile ("network-monitor-" + stamp + ".csv"),
                "*.csv");

            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [safe = juce::Component::SafePointer<Content> (this)] (const juce::FileChooser& answered)
                                  {
                                      if (safe == nullptr)
                                          return;

                                      const auto file = answered.getResult();

                                      if (file == juce::File())
                                          return;

                                      std::vector<model::TrafficRow> rows;

                                      for (const auto id : safe->shown)
                                          rows.push_back (safe->rowOf (id));

                                      const auto csv = model::csvOf (rows, [] (std::int64_t wallMicros)
                                      {
                                          return timeOf (wallMicros).toStdString();
                                      });

                                      if (! file.replaceWithText (juce::String::fromUTF8 (csv.c_str())))
                                          safe->countLabel.setText ("could not write " + file.getFullPathName(),
                                                                    juce::dontSendNotification);
                                  });
        }

        model::Theme theme;
        std::function<void (bool)> listen;

        juce::ToggleButton recordToggle { "recording" }, inToggle { "in" }, outToggle { "out" },
                           oscToggle { "OSC" }, midiToggle { "MIDI" }, pageToggle { "web page" };
        juce::TextEditor filterBox;
        juce::TextButton clearButton { "Clear" }, exportButton { "Export CSV..." };
        juce::Label countLabel;
        juce::TableListBox table { "traffic", this };
        std::unique_ptr<juce::FileChooser> chooser;

        /*  THE LINES KEPT, and which of them the filters show - by a number
            that counts every line ever kept, so dropping the oldest does not
            renumber what is shown. */
        std::deque<model::TrafficRow> lines;
        std::deque<std::uint64_t> shown;
        std::uint64_t firstId = 0;
        std::uint64_t kept = 0;

        model::TrafficFilter filter;
        std::uint64_t missedSoFar = 0, missedAtClear = 0;
    };

    NetworkMonitorWindow::NetworkMonitorWindow (const model::Theme& theme, Actions actionsToUse)
        : DocumentWindow ("Network monitor", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          actions (std::move (actionsToUse))
    {
        content = std::make_unique<Content> (theme, [this] (bool on) { if (actions.listen) actions.listen (on); });
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (640, 300, 4000, 3000);
        setContentNonOwned (content.get(), true);
        centreWithSize (1180, 640);
    }

    NetworkMonitorWindow::~NetworkMonitorWindow()
    {
        if (actions.listen)
            actions.listen (false);

        clearContentComponent();
    }

    void NetworkMonitorWindow::add (const std::vector<monitor::Capture>& captures, std::uint64_t droppedSoFar)
    {
        content->add (captures, droppedSoFar);
    }

    void NetworkMonitorWindow::open()
    {
        setVisible (true);
        toFront (true);

        //  Opened is listening, unless it was paused when it was closed - a pause is a hand's.
        if (actions.listen)
            actions.listen (content->listening());
    }

    void NetworkMonitorWindow::closeButtonPressed()
    {
        //  Shut, it costs nothing: the engine stops recording, and the lines wait here.
        if (actions.listen)
            actions.listen (false);

        setVisible (false);
    }

    bool NetworkMonitorWindow::keyPressed (const juce::KeyPress& key)
    {
        /*  ESC IS PANIC HERE TOO, as it is in the settings window: the key an
            operator reaches for does what it always does, whichever window
            happens to have the focus (§4.4). */
        if (key != juce::KeyPress (juce::KeyPress::escapeKey))
            return false;

        if (actions.panic)
            actions.panic();

        return true;
    }

    bool NetworkMonitorWindow::listening() const noexcept
    {
        return isVisible() && content->listening();
    }

    std::size_t NetworkMonitorWindow::lineCount() const noexcept { return content->lineCount(); }
    std::size_t NetworkMonitorWindow::shownCount() const noexcept { return content->shownCount(); }
    const model::TrafficRow& NetworkMonitorWindow::shownRow (std::size_t index) const { return content->shownRow (index); }
    void NetworkMonitorWindow::setFilter (const model::TrafficFilter& filter) { content->setFilter (filter); }
}
