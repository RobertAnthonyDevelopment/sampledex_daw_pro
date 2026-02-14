#pragma once
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include "TimelineModel.h"
#include "TransportEngine.h"
#include "Track.h"
#include "Theme.h"

namespace sampledex
{
    // --- Header ---
    class TrackHeader : public juce::Component, public juce::Timer
    {
    public:
        std::function<void()> onSelect;
        std::function<void(juce::Component*, int)> onRequestPluginMenu;
        std::function<void(int)> onRequestOpenPluginEditor;
        std::function<void()> onRequestRenameTrack;
        std::function<void()> onRequestDuplicateTrack;
        std::function<void()> onRequestDeleteTrack;
        std::function<void()> onRequestMoveTrackUp;
        std::function<void()> onRequestMoveTrackDown;
        std::function<void()> onRequestOpenChannelRack;
        std::function<void()> onRequestOpenInspector;
        std::function<void()> onRequestOpenEq;
        std::function<void()> onReorderDragBegin;
        std::function<void(float)> onReorderDragMove;
        std::function<void()> onReorderDragEnd;
        std::function<void()> onStateChanged;

        TrackHeader(Track& t) : track(t)
        {
            trackName.setText(track.getTrackName(), juce::dontSendNotification);
            trackName.setColour(juce::Label::textColourId, theme::Colours::text());
            trackName.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            trackName.setJustificationType(juce::Justification::centredLeft);
            trackName.setTooltip("Track name. Double-click to rename. Drag header area to reorder.");
            trackName.setInterceptsMouseClicks(false, false);
            pluginName.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.72f));
            pluginName.setFont(juce::Font(juce::FontOptions(13.0f)));
            pluginName.setJustificationType(juce::Justification::centredLeft);
            pluginName.setText(track.getPluginSummary(), juce::dontSendNotification);
            pluginName.setTooltip("Instrument + insert chain. Left-click to open UI, right-click to load/change.");
            pluginName.setInterceptsMouseClicks(false, false);

            volumeLabel.setText("Vol", juce::dontSendNotification);
            volumeLabel.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.70f));
            volumeLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            volumeLabel.setJustificationType(juce::Justification::centredLeft);
            volumeLabel.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(volumeLabel);
            volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            volumeSlider.setRange(0.0, 1.2, 0.01);
            volumeSlider.onValueChange = [this]
            {
                track.setVolume(static_cast<float>(volumeSlider.getValue()));
                if (onStateChanged)
                    onStateChanged();
            };
            volumeSlider.setTooltip("Track volume.");
            volumeSlider.setColour(juce::Slider::trackColourId, theme::Colours::accent().withAlpha(0.78f));
            volumeSlider.setColour(juce::Slider::thumbColourId, theme::Colours::accent());
            volumeSlider.setColour(juce::Slider::backgroundColourId, theme::Colours::panel().brighter(0.14f));
            addAndMakeVisible(volumeSlider);

            sendTapBox.addItem("Pre", 1);
            sendTapBox.addItem("Post", 2);
            sendTapBox.addItem("Post-Pan", 3);
            sendTapBox.onChange = [this]
            {
                const int selectedId = sendTapBox.getSelectedId();
                const auto mode = (selectedId == 1) ? Track::SendTapMode::PreFader
                                                    : (selectedId == 3 ? Track::SendTapMode::PostPan
                                                                       : Track::SendTapMode::PostFader);
                track.setSendTapMode(mode);
                if (onStateChanged)
                    onStateChanged();
            };
            sendTapBox.setTooltip("Send tap point.");

            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
                sendTargetBox.addItem("Bus " + juce::String(bus + 1), bus + 1);
            sendTargetBox.onChange = [this]
            {
                track.setSendTargetBus(sendTargetBox.getSelectedId() - 1);
                if (onStateChanged)
                    onStateChanged();
            };
            sendTargetBox.setTooltip("Send destination bus.");

            outputTargetBox.addItem("Master", 1);
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
                outputTargetBox.addItem("Bus " + juce::String(bus + 1), bus + 2);
            outputTargetBox.onChange = [this]
            {
                const int selectedId = outputTargetBox.getSelectedId();
                if (selectedId <= 1)
                    track.routeOutputToMaster();
                else
                    track.routeOutputToBus(selectedId - 2);
                if (onStateChanged)
                    onStateChanged();
            };
            outputTargetBox.setTooltip("Track output target.");

            const auto styleRouteBox = [](juce::ComboBox& box)
            {
                box.setColour(juce::ComboBox::backgroundColourId, theme::Colours::panel().brighter(0.08f));
                box.setColour(juce::ComboBox::textColourId, theme::Colours::text());
                box.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.24f));
                box.setColour(juce::ComboBox::arrowColourId, theme::Colours::text().withAlpha(0.88f));
            };
            styleRouteBox(sendTapBox);
            styleRouteBox(sendTargetBox);
            styleRouteBox(outputTargetBox);
            addAndMakeVisible(sendTapBox);
            addAndMakeVisible(sendTargetBox);
            addAndMakeVisible(outputTargetBox);
            
            muteBtn.setButtonText("M");
            muteBtn.setClickingTogglesState(true);
            muteBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::red);
            muteBtn.onClick = [this]
            {
                track.setMute(muteBtn.getToggleState());
                if (onStateChanged) onStateChanged();
            };

            soloBtn.setButtonText("S");
            soloBtn.setClickingTogglesState(true);
            soloBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
            soloBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            soloBtn.onClick = [this]
            {
                track.setSolo(soloBtn.getToggleState());
                if (onStateChanged) onStateChanged();
            };

            armBtn.setButtonText("R");
            armBtn.setClickingTogglesState(true);
            armBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::red);
            armBtn.onClick = [this]
            {
                track.setArm(armBtn.getToggleState());
                if (onStateChanged) onStateChanged();
            };

            monitorBtn.setButtonText("I");
            monitorBtn.setClickingTogglesState(true);
            monitorBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colours::limegreen);
            monitorBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            monitorBtn.onClick = [this]
            {
                track.setInputMonitoring(monitorBtn.getToggleState());
                if (onStateChanged) onStateChanged();
            };
            
            addAndMakeVisible(trackName);
            addAndMakeVisible(pluginName);

            instrumentButton.setButtonText("INST");
            instrumentButton.setTooltip("Instrument slot. Left-click open/load, right-click load/change.");
            instrumentButton.onClick = [this]
            {
                if (track.hasInstrumentPlugin())
                {
                    if (onRequestOpenPluginEditor)
                        onRequestOpenPluginEditor(Track::instrumentSlotIndex);
                }
                else if (onRequestPluginMenu)
                {
                    onRequestPluginMenu(&instrumentButton, Track::instrumentSlotIndex);
                }
            };
            addAndMakeVisible(instrumentButton);

            eqButton.setButtonText("EQ");
            eqButton.setTooltip("Open built-in 3-band track EQ.");
            eqButton.onClick = [this]
            {
                if (onRequestOpenEq)
                    onRequestOpenEq();
            };
            addAndMakeVisible(eqButton);
            for (int i = 0; i < insertButtonCount; ++i)
            {
                auto& button = insertButtons[static_cast<size_t>(i)];
                button.setButtonText("I" + juce::String(i + 1));
                button.setTooltip("Insert " + juce::String(i + 1) + ". Left-click open/load, right-click load/change.");
                button.onClick = [this, i]
                {
                    if (track.hasPluginInSlot(i))
                    {
                        if (onRequestOpenPluginEditor)
                            onRequestOpenPluginEditor(i);
                    }
                    else if (onRequestPluginMenu)
                    {
                        onRequestPluginMenu(&insertButtons[static_cast<size_t>(i)], i);
                    }
                };
                addAndMakeVisible(button);
            }
            addAndMakeVisible(muteBtn);
            addAndMakeVisible(soloBtn);
            addAndMakeVisible(armBtn);
            addAndMakeVisible(monitorBtn);

            startTimer(90);
        }
        
        void timerCallback() override
        {
            const auto summary = track.getPluginSummary();
            if (summary != lastPluginSummary)
            {
                lastPluginSummary = summary;
                pluginName.setText(summary, juce::dontSendNotification);
            }
            const auto newTrackName = track.getTrackName();
            if (newTrackName != lastTrackName)
            {
                lastTrackName = newTrackName;
                trackName.setText(newTrackName, juce::dontSendNotification);
            }
            pluginName.setColour(juce::Label::textColourId,
                                 track.hasPlugin()
                                     ? (pluginNameHovered ? theme::Colours::accent().brighter(0.18f)
                                                          : theme::Colours::accent().withAlpha(0.9f))
                                     : theme::Colours::text().withAlpha(pluginNameHovered ? 0.92f : 0.72f));
            const bool hasInstrument = track.hasPluginInSlot(Track::instrumentSlotIndex);
            const bool hasInstrumentEditor = track.hasInstrumentPlugin();
            instrumentButton.setColour(juce::TextButton::buttonColourId,
                                       hasInstrument ? theme::Colours::accent().withAlpha(0.30f)
                                                     : juce::Colours::black.withAlpha(0.24f));
            instrumentButton.setColour(juce::TextButton::textColourOffId,
                                       hasInstrument ? juce::Colours::white
                                                     : juce::Colours::white.withAlpha(0.68f));
            const auto instrumentName = track.getPluginNameForSlot(Track::instrumentSlotIndex);
            instrumentButton.setTooltip("Instrument: " + (instrumentName.isNotEmpty() ? instrumentName : juce::String("None"))
                                        + (hasInstrumentEditor ? ". Left-click open, right-click load/change."
                                                               : ". Left-click load, right-click load/change."));
            const bool eqEnabled = track.isEqEnabled();
            eqButton.setColour(juce::TextButton::buttonColourId,
                               eqEnabled ? theme::Colours::accent().withAlpha(0.30f)
                                         : juce::Colours::black.withAlpha(0.24f));
            eqButton.setColour(juce::TextButton::textColourOffId,
                               eqEnabled ? juce::Colours::white
                                         : juce::Colours::white.withAlpha(0.68f));
            eqButton.setTooltip("Track EQ: "
                                + juce::String(eqEnabled ? "On" : "Off")
                                + " | L " + juce::String(track.getEqLowGainDb(), 1) + " dB"
                                + " M " + juce::String(track.getEqMidGainDb(), 1) + " dB"
                                + " H " + juce::String(track.getEqHighGainDb(), 1) + " dB");
            muteBtn.setToggleState(track.isMuted(), juce::dontSendNotification);
            soloBtn.setToggleState(track.isSolo(), juce::dontSendNotification);
            armBtn.setToggleState(track.isArmed(), juce::dontSendNotification);
            monitorBtn.setToggleState(track.isInputMonitoringEnabled(), juce::dontSendNotification);
            for (int i = 0; i < insertButtonCount; ++i)
            {
                const bool loaded = track.hasPluginInSlot(i);
                insertButtons[static_cast<size_t>(i)].setColour(juce::TextButton::buttonColourId,
                                                                loaded ? theme::Colours::accent().withAlpha(0.28f)
                                                                       : juce::Colours::black.withAlpha(0.24f));
                insertButtons[static_cast<size_t>(i)].setColour(juce::TextButton::textColourOffId,
                                                                loaded ? juce::Colours::white
                                                                       : juce::Colours::white.withAlpha(0.68f));
                const auto slotName = track.getPluginNameForSlot(i);
                insertButtons[static_cast<size_t>(i)].setButtonText(compactSlotLabel(i, slotName));
                insertButtons[static_cast<size_t>(i)].setTooltip(
                    "Insert " + juce::String(i + 1)
                    + (slotName.isNotEmpty() ? (": " + slotName) : ": Empty")
                    + ". Left-click open/load, right-click load/change.");
            }
            instrumentButton.setButtonText(compactInstrumentLabel(track.getPluginNameForSlot(Track::instrumentSlotIndex)));
            volumeSlider.setValue(track.getVolume(), juce::dontSendNotification);
            sendTapBox.setSelectedId(track.getSendTapMode() == Track::SendTapMode::PreFader
                                         ? 1
                                         : (track.getSendTapMode() == Track::SendTapMode::PostPan ? 3 : 2),
                                     juce::dontSendNotification);
            sendTargetBox.setSelectedId(track.getSendTargetBus() + 1, juce::dontSendNotification);
            outputTargetBox.setSelectedId(track.getOutputTargetType() == Track::OutputTargetType::Master
                                              ? 1
                                              : (track.getOutputTargetBus() + 2),
                                          juce::dontSendNotification);
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(theme::Colours::header());
            g.setColour(juce::Colours::black);
            g.drawRect(getLocalBounds(), 1);
            
            if (selected) {
                g.setColour(theme::Colours::selection());
                g.fillRect(getLocalBounds());
                g.setColour(theme::Colours::accent());
                g.fillRect(0, 0, 4, getHeight());
            }

            const juce::Rectangle<float> typeBadge(static_cast<float>(getWidth() - 50), 6.0f, 44.0f, 14.0f);
            g.setColour(theme::Colours::darker().withAlpha(0.72f));
            g.fillRoundedRectangle(typeBadge, 3.0f);
            g.setColour(theme::Colours::accent().withAlpha(0.92f));
            g.drawRoundedRectangle(typeBadge, 3.0f, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.90f));
            g.setFont(9.0f);
            g.drawFittedText(getChannelTypeShortLabel(track.getChannelType()),
                             typeBadge.toNearestInt(),
                             juce::Justification::centred,
                             1);

            float level = track.getMeterLevel();
            if (level > 0.001f) {
                g.setColour(juce::Colours::green);
                float w = (float)getWidth() * juce::jmin(1.0f, level);
                g.fillRect(0.0f, (float)getHeight()-4.0f, w, 4.0f);
            }

            if (track.isRenderTaskActive())
            {
                const float progress = juce::jlimit(0.0f, 1.0f, track.getRenderTaskProgress());
                const auto label = track.getRenderTaskLabel();
                juce::Rectangle<float> badge(static_cast<float>(getWidth() - 126), 24.0f, 120.0f, 14.0f);
                g.setColour(theme::Colours::panel().withAlpha(0.92f));
                g.fillRoundedRectangle(badge, 3.0f);
                g.setColour(theme::Colours::accent().withAlpha(0.24f));
                g.fillRoundedRectangle(badge.withWidth(badge.getWidth() * progress), 3.0f);
                g.setColour(theme::Colours::accent().withAlpha(0.92f));
                g.drawRoundedRectangle(badge, 3.0f, 1.0f);
                g.setColour(theme::Colours::text().withAlpha(0.94f));
                g.setFont(9.4f);
                g.drawFittedText(label + " " + juce::String(juce::roundToInt(progress * 100.0f)) + "%",
                                 badge.toNearestInt(),
                                 juce::Justification::centred,
                                 1);
            }

            // Track reorder drag hint (vertical in timeline).
            const auto handleArea = juce::Rectangle<float>(static_cast<float>(getWidth() - 16), 6.0f, 10.0f, 12.0f);
            g.setColour(theme::Colours::text().withAlpha(0.38f));
            for (int i = 0; i < 3; ++i)
            {
                const float y = handleArea.getY() + (static_cast<float>(i) * 4.0f);
                g.drawLine(handleArea.getX(), y, handleArea.getRight(), y, 1.0f);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(5, 3);
            const int routeHeight = juce::jlimit(34, 50, getHeight() / 3);
            const int controlsHeight = juce::jlimit(20, 32, getHeight() / 4);
            const int insertsHeight = juce::jlimit(18, 28, getHeight() / 5);
            const int volumeHeight = juce::jlimit(16, 24, getHeight() / 6);
            const int textHeight = juce::jmax(26, r.getHeight() - routeHeight - controlsHeight - insertsHeight - volumeHeight);
            const int trackNameHeight = juce::jmax(14, (textHeight * 56) / 100);
            const int pluginHeight = juce::jmax(12, textHeight - trackNameHeight);

            trackName.setBounds(r.removeFromTop(trackNameHeight));
            r.removeFromTop(1);
            pluginName.setBounds(r.removeFromTop(pluginHeight));
            r.removeFromTop(1);

            const float nameFont = juce::jlimit(14.0f, 20.0f, static_cast<float>(trackNameHeight) * 0.78f);
            const float pluginFont = juce::jlimit(12.0f, 16.0f, static_cast<float>(pluginHeight) * 0.78f);
            trackName.setFont(juce::Font(juce::FontOptions(nameFont, juce::Font::bold)));
            pluginName.setFont(juce::Font(juce::FontOptions(pluginFont)));

            auto volumeRow = r.removeFromTop(volumeHeight);
            volumeLabel.setBounds(volumeRow.removeFromLeft(30));
            volumeSlider.setBounds(volumeRow.reduced(2, 1));

            auto insertRow = r.removeFromTop(insertsHeight);
            instrumentButton.setBounds(insertRow.removeFromLeft(52).reduced(1));
            const int eqWidth = juce::jmax(34, juce::jmin(52, insertRow.getWidth() / 5));
            const int insertWidth = juce::jmax(22,
                                               (juce::jmax(0, insertRow.getWidth() - eqWidth - 2))
                                               / juce::jmax(1, insertButtonCount));
            for (int i = 0; i < insertButtonCount; ++i)
                insertButtons[static_cast<size_t>(i)].setBounds(insertRow.removeFromLeft(insertWidth).reduced(1));
            eqButton.setBounds(insertRow.removeFromLeft(eqWidth).reduced(1));
            auto row = r.removeFromTop(controlsHeight);
            int w = row.getWidth() / 4;
            muteBtn.setBounds(row.removeFromLeft(w).reduced(1));
            soloBtn.setBounds(row.removeFromLeft(w).reduced(1));
            armBtn.setBounds(row.removeFromLeft(w).reduced(1));
            monitorBtn.setBounds(row.reduced(1));

            auto routeRow = r.removeFromTop(routeHeight);
            const int outputRowHeight = juce::jmax(16, routeRow.getHeight() / 2);
            auto sendRow = routeRow.removeFromTop(outputRowHeight);
            sendTapBox.setBounds(sendRow.removeFromLeft(sendRow.getWidth() / 2).reduced(1));
            sendTargetBox.setBounds(sendRow.reduced(1));
            outputTargetBox.setBounds(routeRow.reduced(1));
        }

        void setSelected(bool s) { selected = s; repaint(); }
        void mouseDown(const juce::MouseEvent& e) override
        {
            const auto localPos = e.getEventRelativeTo(this).position;
            const bool clickedPluginName = pluginName.getBounds().toFloat().contains(localPos);
            const int insertButtonIndex = getInsertButtonIndex(e.eventComponent);
            if (onSelect)
                onSelect();

            if (e.eventComponent == &instrumentButton)
            {
                if (e.mods.isPopupMenu() || !track.hasInstrumentPlugin())
                {
                    if (onRequestPluginMenu)
                        onRequestPluginMenu(&instrumentButton, Track::instrumentSlotIndex);
                }
                else if (onRequestOpenPluginEditor)
                {
                    onRequestOpenPluginEditor(Track::instrumentSlotIndex);
                }
                return;
            }

            if (e.eventComponent == &eqButton)
            {
                if (onRequestOpenEq)
                    onRequestOpenEq();
                return;
            }

            if (insertButtonIndex >= 0)
            {
                if (e.mods.isPopupMenu() || !track.hasPluginInSlot(insertButtonIndex))
                {
                    if (onRequestPluginMenu)
                        onRequestPluginMenu(&insertButtons[static_cast<size_t>(insertButtonIndex)], insertButtonIndex);
                }
                else if (onRequestOpenPluginEditor)
                {
                    onRequestOpenPluginEditor(insertButtonIndex);
                }
                return;
            }

            if (clickedPluginName)
            {
                const int preferredSlot = track.hasPlugin() ? track.getFirstLoadedPluginSlot()
                                                             : Track::instrumentSlotIndex;
                if (e.mods.isPopupMenu() || !track.hasPlugin())
                {
                    if (onRequestPluginMenu)
                        onRequestPluginMenu(this, preferredSlot);
                }
                else if (onRequestOpenPluginEditor)
                {
                    onRequestOpenPluginEditor(preferredSlot);
                }
                return;
            }

            if (!e.mods.isPopupMenu())
            {
                dragGesturePending = true;
                dragGestureActive = false;
                if (auto* parent = getParentComponent())
                    dragStartInParent = e.getEventRelativeTo(parent).position;
                else
                    dragStartInParent = e.position;
                return;
            }

            juce::PopupMenu menu;
            juce::PopupMenu loadInsertMenu;
            juce::PopupMenu openInsertMenu;
            menu.addItem(14, "Load Instrument...");
            menu.addItem(15, "Open Instrument UI", track.hasInstrumentPlugin());
            menu.addItem(16, "Open Track EQ");
            menu.addSeparator();
            for (int i = 0; i < insertButtonCount; ++i)
            {
                const bool hasPlugin = track.hasPluginInSlot(i);
                const auto slotPluginName = track.getPluginNameForSlot(i);
                const juce::String slotName = "Insert " + juce::String(i + 1)
                                            + " (" + (slotPluginName.isNotEmpty() ? slotPluginName : juce::String("Empty")) + ")";
                loadInsertMenu.addItem(20 + i, slotName);
                openInsertMenu.addItem(30 + i, slotName, hasPlugin);
            }
            menu.addSubMenu("Load Plugin Into Insert", loadInsertMenu);
            menu.addSubMenu("Open Insert UI", openInsertMenu);
            menu.addItem(1, "Load Instrument...");
            menu.addItem(2, "Open Plugin UI", track.hasPlugin());
            menu.addSeparator();
            juce::PopupMenu sendMenu;
            sendMenu.addItem(40, "Send 0%", true, track.getSendLevel() <= 0.001f);
            sendMenu.addItem(41, "Send 25%", true, std::abs(track.getSendLevel() - 0.25f) < 0.01f);
            sendMenu.addItem(42, "Send 50%", true, std::abs(track.getSendLevel() - 0.50f) < 0.01f);
            sendMenu.addItem(43, "Send 75%", true, std::abs(track.getSendLevel() - 0.75f) < 0.01f);
            sendMenu.addItem(44, "Send 100%", true, std::abs(track.getSendLevel() - 1.00f) < 0.01f);
            menu.addSubMenu("Set Send Level", sendMenu);
            juce::PopupMenu sendModeMenu;
            const auto sendTap = track.getSendTapMode();
            sendModeMenu.addItem(45, "Pre-Fader", true, sendTap == Track::SendTapMode::PreFader);
            sendModeMenu.addItem(46, "Post-Fader", true, sendTap == Track::SendTapMode::PostFader);
            sendModeMenu.addItem(47, "Post-Pan", true, sendTap == Track::SendTapMode::PostPan);
            menu.addSubMenu("Send Tap Point", sendModeMenu);
            juce::PopupMenu sendTargetMenu;
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                sendTargetMenu.addItem(48 + bus,
                                       "Bus " + juce::String(bus + 1),
                                       true,
                                       track.getSendTargetBus() == bus);
            }
            menu.addSubMenu("Send Target", sendTargetMenu);
            juce::PopupMenu outputTargetMenu;
            const auto outputType = track.getOutputTargetType();
            outputTargetMenu.addItem(69, "Master", true, outputType == Track::OutputTargetType::Master);
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                outputTargetMenu.addItem(70 + bus,
                                         "Bus " + juce::String(bus + 1),
                                         true,
                                         outputType == Track::OutputTargetType::Bus
                                             && track.getOutputTargetBus() == bus);
            }
            menu.addSubMenu("Output Target", outputTargetMenu);
            menu.addSeparator();
            menu.addItem(3, "Toggle Mute", true, track.isMuted());
            menu.addItem(4, "Toggle Solo", true, track.isSolo());
            menu.addItem(5, "Toggle Arm", true, track.isArmed());
            menu.addItem(6, "Toggle Input Monitor", true, track.isInputMonitoringEnabled());
            menu.addSeparator();
            menu.addItem(7, "Rename Track...");
            menu.addItem(8, "Duplicate Track");
            menu.addItem(9, "Delete Track");
            menu.addItem(10, "Move Track Up");
            menu.addItem(11, "Move Track Down");
            menu.addSeparator();
            menu.addItem(12, "Open Channel Rack");
            menu.addItem(13, "Open Inspector");
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                               [this](int selectedId)
                               {
                                   if (selectedId == 14)
                                   {
                                       if (onRequestPluginMenu)
                                           onRequestPluginMenu(this, Track::instrumentSlotIndex);
                                   }
                                   else if (selectedId == 15)
                                   {
                                       if (onRequestOpenPluginEditor)
                                           onRequestOpenPluginEditor(Track::instrumentSlotIndex);
                                   }
                                   else if (selectedId == 16)
                                   {
                                       if (onRequestOpenEq)
                                           onRequestOpenEq();
                                   }
                                   else if (selectedId == 1)
                                   {
                                       if (onRequestPluginMenu)
                                           onRequestPluginMenu(this, Track::instrumentSlotIndex);
                                   }
                                   else if (selectedId >= 20 && selectedId < 20 + insertButtonCount)
                                   {
                                       const int slotIndex = selectedId - 20;
                                       if (onRequestPluginMenu)
                                           onRequestPluginMenu(this, slotIndex);
                                   }
                                   else if (selectedId >= 30 && selectedId < 30 + insertButtonCount)
                                   {
                                       const int slotIndex = selectedId - 30;
                                       if (onRequestOpenPluginEditor)
                                           onRequestOpenPluginEditor(slotIndex);
                                   }
                                   else if (selectedId == 2)
                                   {
                                       if (onRequestOpenPluginEditor)
                                           onRequestOpenPluginEditor(track.getFirstLoadedPluginSlot());
                                   }
                                   else if (selectedId >= 40 && selectedId <= 44)
                                   {
                                       static constexpr float sendPresets[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
                                       track.setSendLevel(sendPresets[selectedId - 40]);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 45 || selectedId == 46 || selectedId == 47)
                                   {
                                       const auto mode = (selectedId == 45) ? Track::SendTapMode::PreFader
                                                                            : (selectedId == 46 ? Track::SendTapMode::PostFader
                                                                                                : Track::SendTapMode::PostPan);
                                       track.setSendTapMode(mode);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId >= 48 && selectedId < 48 + Track::maxSendBuses)
                                   {
                                       track.setSendTargetBus(selectedId - 48);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 69)
                                   {
                                       track.routeOutputToMaster();
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId >= 70 && selectedId < 70 + Track::maxSendBuses)
                                   {
                                       track.routeOutputToBus(selectedId - 70);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 3)
                                   {
                                       track.setMute(!track.isMuted());
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 4)
                                   {
                                       track.setSolo(!track.isSolo());
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 5)
                                   {
                                       track.setArm(!track.isArmed());
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 6)
                                   {
                                       track.setInputMonitoring(!track.isInputMonitoringEnabled());
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 7)
                                   {
                                       if (onRequestRenameTrack)
                                           onRequestRenameTrack();
                                   }
                                   else if (selectedId == 8)
                                   {
                                       if (onRequestDuplicateTrack)
                                           onRequestDuplicateTrack();
                                   }
                                   else if (selectedId == 9)
                                   {
                                       if (onRequestDeleteTrack)
                                           onRequestDeleteTrack();
                                   }
                                   else if (selectedId == 10)
                                   {
                                       if (onRequestMoveTrackUp)
                                           onRequestMoveTrackUp();
                                   }
                                   else if (selectedId == 11)
                                   {
                                       if (onRequestMoveTrackDown)
                                           onRequestMoveTrackDown();
                                   }
                                   else if (selectedId == 12)
                                   {
                                       if (onRequestOpenChannelRack)
                                           onRequestOpenChannelRack();
                                   }
                                   else if (selectedId == 13)
                                   {
                                       if (onRequestOpenInspector)
                                           onRequestOpenInspector();
                                   }
                               });
        }

        void mouseDoubleClick(const juce::MouseEvent& e) override
        {
            const auto localPos = e.getEventRelativeTo(this).position;
            if (trackName.getBounds().toFloat().contains(localPos))
            {
                if (onRequestRenameTrack)
                    onRequestRenameTrack();
            }
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const auto localPos = e.getEventRelativeTo(this).position;
            const bool hover = pluginName.getBounds().toFloat().contains(localPos);
            if (hover != pluginNameHovered)
            {
                pluginNameHovered = hover;
                repaint();
            }

            const bool renameSurface = trackName.getBounds().toFloat().contains(localPos);
            const bool dragSurface = (e.eventComponent == this)
                                     || trackName.getBounds().toFloat().contains(localPos);
            setMouseCursor(hover ? juce::MouseCursor::PointingHandCursor
                                 : (renameSurface ? juce::MouseCursor::IBeamCursor
                                                  : (dragSurface ? juce::MouseCursor::DraggingHandCursor
                                                                 : juce::MouseCursor::NormalCursor)));
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            if (pluginNameHovered)
            {
                pluginNameHovered = false;
                repaint();
            }
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (!dragGesturePending || e.mods.isPopupMenu())
                return;

            const auto parentPos = (getParentComponent() != nullptr)
                ? e.getEventRelativeTo(getParentComponent()).position
                : e.position;
            if (!dragGestureActive)
            {
                if (parentPos.getDistanceFrom(dragStartInParent) < 2.0f)
                    return;

                dragGestureActive = true;
                if (onReorderDragBegin)
                    onReorderDragBegin();
            }

            if (onReorderDragMove)
                onReorderDragMove(parentPos.y);
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            if (dragGestureActive && onReorderDragEnd)
                onReorderDragEnd();
            dragGesturePending = false;
            dragGestureActive = false;
        }

    private:
        static juce::String getChannelTypeShortLabel(Track::ChannelType type)
        {
            switch (type)
            {
                case Track::ChannelType::Audio: return "AUD";
                case Track::ChannelType::Aux: return "AUX";
                case Track::ChannelType::Master: return "MSTR";
                case Track::ChannelType::Instrument:
                default: return "INST";
            }
        }

        static juce::String getOutputRouteShortLabel(Track::OutputTargetType type, int bus)
        {
            if (type == Track::OutputTargetType::Bus)
                return "B" + juce::String(juce::jlimit(0, Track::maxSendBuses - 1, bus) + 1);
            return "MSTR";
        }

        static juce::String compactInstrumentLabel(const juce::String& rawName)
        {
            auto label = rawName.trim();
            if (label.isEmpty())
                return "INST";
            if (label.length() > 9)
                label = label.substring(0, 8) + "...";
            return label.toUpperCase();
        }

        static juce::String compactSlotLabel(int slotIndex, const juce::String& rawName)
        {
            auto label = rawName.trim();
            if (label.isEmpty())
                return "I" + juce::String(slotIndex + 1);

            if (label.length() > 10)
                label = label.substring(0, 9) + "...";
            return label;
        }

        int getInsertButtonIndex(const juce::Component* c) const
        {
            for (int i = 0; i < insertButtonCount; ++i)
            {
                if (c == &insertButtons[static_cast<size_t>(i)])
                    return i;
            }
            return -1;
        }

        Track& track;
        bool selected = false;
        bool pluginNameHovered = false;
        bool dragGesturePending = false;
        bool dragGestureActive = false;
        juce::Point<float> dragStartInParent;
        static constexpr int insertButtonCount = 4;
        std::array<juce::TextButton, static_cast<size_t>(insertButtonCount)> insertButtons;
        juce::TextButton instrumentButton;
        juce::TextButton eqButton;
        juce::Label volumeLabel;
        juce::Slider volumeSlider;
        juce::ComboBox sendTapBox;
        juce::ComboBox sendTargetBox;
        juce::ComboBox outputTargetBox;
        juce::Label trackName, pluginName;
        juce::String lastTrackName;
        juce::String lastPluginSummary;
        juce::TextButton muteBtn, soloBtn, armBtn, monitorBtn;
    };

    // --- Timeline ---
    class TimelineComponent : public juce::Component,
                              public juce::Timer,
                              private juce::ScrollBar::Listener
    {
        enum class DragMode
        {
            none,
            move,
            resizeLeft,
            resizeRight
        };

    public:
        std::function<void(Clip*)> onClipSelected;
        std::function<void(int)> onTrackSelected;
        std::function<void(int, double, double)> onCreateMidiClip;
        std::function<void(double)> onCreateMidiTrack;
        std::function<void(int)> onDeleteClip;
        std::function<void(int, double)> onSplitClipAtBeat;
        std::function<void(int)> onDuplicateClip;
        std::function<void(int, double)> onNudgeClipBy;
        std::function<void(int)> onDeleteTrack;
        std::function<void(int, int, double, bool)> onMoveClip;
        std::function<void(int, double, double)> onResizeClip;
        std::function<void(int, int)> onReorderTracks;
        std::function<void(int)> onTrackStateChanged;
        std::function<void(int)> onRenameTrack;
        std::function<void(int)> onDuplicateTrack;
        std::function<void(int, juce::Component*, int)> onLoadPluginForTrack;
        std::function<void(int, int)> onOpenPluginEditorForTrack;
        std::function<void(int)> onMoveTrackUp;
        std::function<void(int)> onMoveTrackDown;
        std::function<void(int)> onOpenChannelRack;
        std::function<void(int)> onOpenInspector;
        std::function<void(int)> onOpenTrackEq;

        TimelineComponent(TransportEngine& t, std::vector<Clip>& c, const juce::OwnedArray<Track>& trks) 
            : transport(t), clips(c), tracks(trks)
        {
            horizontalScrollBar.addListener(this);
            verticalScrollBar.addListener(this);
            horizontalScrollBar.setAutoHide(false);
            verticalScrollBar.setAutoHide(false);
            horizontalScrollBar.setSingleStepSize(32.0);
            verticalScrollBar.setSingleStepSize(24.0);
            addAndMakeVisible(horizontalScrollBar);
            addAndMakeVisible(verticalScrollBar);
            startTimerHz(30);
        }

        void refreshHeaders()
        {
            headers.clear();
            int i = 0;
            for (auto* t : tracks)
            {
                auto* h = headers.add(new TrackHeader(*t));
                h->onSelect = [this, i] { 
                    selectTrack(i); 
                    if(onTrackSelected) onTrackSelected(i);
                };
                h->onRequestPluginMenu = [this, i](juce::Component* target, int slotIndex)
                {
                    if (onLoadPluginForTrack)
                        onLoadPluginForTrack(i, target, slotIndex);
                };
                h->onStateChanged = [this, i]
                {
                    if (onTrackStateChanged)
                        onTrackStateChanged(i);
                };
                h->onRequestOpenPluginEditor = [this, i](int slotIndex)
                {
                    if (onOpenPluginEditorForTrack)
                        onOpenPluginEditorForTrack(i, slotIndex);
                };
                h->onRequestRenameTrack = [this, i]
                {
                    if (onRenameTrack)
                        onRenameTrack(i);
                };
                h->onRequestDuplicateTrack = [this, i]
                {
                    if (onDuplicateTrack)
                        onDuplicateTrack(i);
                };
                h->onRequestDeleteTrack = [this, i]
                {
                    if (onDeleteTrack)
                        onDeleteTrack(i);
                };
                h->onRequestMoveTrackUp = [this, i]
                {
                    if (onMoveTrackUp)
                        onMoveTrackUp(i);
                };
                h->onRequestMoveTrackDown = [this, i]
                {
                    if (onMoveTrackDown)
                        onMoveTrackDown(i);
                };
                h->onRequestOpenChannelRack = [this, i]
                {
                    if (onOpenChannelRack)
                        onOpenChannelRack(i);
                };
                h->onRequestOpenInspector = [this, i]
                {
                    if (onOpenInspector)
                        onOpenInspector(i);
                };
                h->onRequestOpenEq = [this, i]
                {
                    if (onOpenTrackEq)
                        onOpenTrackEq(i);
                };
                h->onReorderDragBegin = [this, i]
                {
                    trackReorderDragging = true;
                    reorderSourceTrack = i;
                    reorderTargetTrack = i;
                    repaint();
                };
                h->onReorderDragMove = [this, i](float parentY)
                {
                    if (!trackReorderDragging || reorderSourceTrack != i)
                        return;
                    const int target = getTrackIndexForPositionY(parentY);
                    reorderTargetTrack = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), target);
                    repaint();
                };
                h->onReorderDragEnd = [this, i]
                {
                    if (!trackReorderDragging || reorderSourceTrack != i)
                        return;

                    const int source = reorderSourceTrack;
                    const int target = reorderTargetTrack;
                    trackReorderDragging = false;
                    reorderSourceTrack = -1;
                    reorderTargetTrack = -1;
                    repaint();

                    if (source >= 0 && target >= 0 && source != target && onReorderTracks)
                        onReorderTracks(source, target);
                };
                addAndMakeVisible(h);
                i++;
            }
            resized();
        }
        
        void selectTrack(int idx)
        {
            selectedTrackIndex = juce::isPositiveAndBelow(idx, tracks.size()) ? idx : -1;
            for(int i=0; i<headers.size(); ++i)
                headers[i]->setSelected(i == selectedTrackIndex);
            repaint();
        }

        void setGridStepBeats(double beats)
        {
            gridStepBeats = juce::jlimit(1.0 / 64.0, 4.0, beats);
            repaint();
        }

        double getGridStepBeats() const { return gridStepBeats; }
        void setAutoFollowPlayhead(bool shouldFollow) { autoFollowPlayhead = shouldFollow; repaint(); }
        bool isAutoFollowPlayheadEnabled() const { return autoFollowPlayhead; }
        void selectClipIndex(int idx)
        {
            selectedClipIndex = juce::isPositiveAndBelow(idx, static_cast<int>(clips.size())) ? idx : -1;
            repaint();
        }

        void zoomHorizontalBy(float factor)
        {
            pixelsPerBeat = juce::jlimit(30.0f, 420.0f, pixelsPerBeat * factor);
            repaint();
        }

        float getPixelsPerBeat() const { return pixelsPerBeat; }
        void setPixelsPerBeat(float newPixelsPerBeat)
        {
            pixelsPerBeat = juce::jlimit(30.0f, 420.0f, newPixelsPerBeat);
            clampScrollOffsets();
            repaint();
        }

        void zoomTrackHeightBy(float delta)
        {
            trackHeight = juce::jlimit(84.0f, 280.0f, trackHeight + delta);
            clampScrollOffsets();
            resized();
            repaint();
        }

        float getTrackHeight() const { return trackHeight; }
        void setTrackHeight(float newTrackHeight)
        {
            trackHeight = juce::jlimit(84.0f, 280.0f, newTrackHeight);
            clampScrollOffsets();
            resized();
            repaint();
        }

        void timerCallback() override
        {
            if (autoFollowPlayhead && transport.playing() && !dragActive && !trackReorderDragging && getWidth() > headerWidth)
            {
                const float timelineWidth = static_cast<float>(getWidth() - headerWidth);
                const float playheadWorldX = static_cast<float>(transport.getCurrentBeat() * pixelsPerBeat);
                const float playheadScreenX = static_cast<float>(headerWidth) + playheadWorldX - scrollX;
                const float leftFollowX = static_cast<float>(headerWidth) + (timelineWidth * 0.18f);
                const float rightFollowX = static_cast<float>(headerWidth) + (timelineWidth * 0.76f);

                if (playheadScreenX > rightFollowX)
                    scrollX = juce::jmax(0.0f, playheadWorldX - (timelineWidth * 0.76f));
                else if (playheadScreenX < leftFollowX)
                    scrollX = juce::jmax(0.0f, playheadWorldX - (timelineWidth * 0.18f));
            }

            clampScrollOffsets();
            repaint();
        }

        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            {
                const float previousPixelsPerBeat = pixelsPerBeat;
                const float factor = wheel.deltaY > 0.0f ? 1.1f : 0.9f;
                pixelsPerBeat = juce::jlimit(30.0f, 420.0f, pixelsPerBeat * factor);

                const float localX = e.position.x - static_cast<float>(headerWidth);
                const double beatAtCursor = (localX + scrollX) / previousPixelsPerBeat;
                scrollX = juce::jmax(0.0f, static_cast<float>(beatAtCursor * pixelsPerBeat - localX));
                clampScrollOffsets();
                repaint();
                return;
            }

            if (e.mods.isAltDown())
            {
                if (e.mods.isShiftDown())
                    zoomTrackHeightBy(wheel.deltaY * 10.0f);
                else
                    zoomHorizontalBy(wheel.deltaY > 0.0f ? 1.1f : 0.9f);
                return;
            }

            if (e.mods.isShiftDown())
            {
                const float delta = (std::abs(wheel.deltaX) > 0.0001f ? wheel.deltaX : wheel.deltaY) * 140.0f;
                scrollX = juce::jmax(0.0f, scrollX - delta);
                clampScrollOffsets();
                repaint();
                return;
            }

            const bool verticalGesture = std::abs(wheel.deltaY) >= (std::abs(wheel.deltaX) + 0.0001f);
            if (verticalGesture)
            {
                scrollY = juce::jlimit(0.0f,
                                       getMaxScrollY(),
                                       scrollY - (wheel.deltaY * 120.0f));
                resized();
                repaint();
                return;
            }

            const float delta = (std::abs(wheel.deltaX) > 0.0001f ? wheel.deltaX : wheel.deltaY) * 140.0f;
            scrollX = juce::jmax(0.0f, scrollX - delta);
            clampScrollOffsets();
            repaint();
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            if (isOnHeaderResizeHandle(e.position.x))
            {
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
                return;
            }

            if (e.position.x < static_cast<float>(headerWidth))
            {
                setMouseCursor(juce::MouseCursor::NormalCursor);
                return;
            }

            if (e.position.y < static_cast<float>(rulerHeight))
            {
                setMouseCursor(juce::MouseCursor::PointingHandCursor);
                return;
            }

            const int clipIndex = findClipIndexAtPosition(e.position);
            if (clipIndex < 0)
            {
                setMouseCursor(juce::MouseCursor::NormalCursor);
                return;
            }

            const auto mode = getResizeModeForPoint(clips[static_cast<size_t>(clipIndex)], e.position);
            if (mode == DragMode::resizeLeft || mode == DragMode::resizeRight)
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            else
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        }

        void paint(juce::Graphics& g) override
        {
            const auto fullBounds = getLocalBounds();
            auto contentBounds = fullBounds;
            const int scrollBarThickness = 12;
            contentBounds.removeFromBottom(scrollBarThickness);
            contentBounds.removeFromRight(scrollBarThickness);

            auto rightSide = contentBounds;
            rightSide.removeFromLeft(headerWidth);

            g.fillAll(theme::Colours::background());
            const auto rulerArea = rightSide.removeFromTop(rulerHeight);
            const auto trackArea = rightSide;

            const juce::Rectangle<int> hintArea(rulerArea.getRight() - 410, rulerArea.getY() + 4, 402, 16);
            g.setColour(theme::Colours::panel().withAlpha(0.68f));
            g.fillRoundedRectangle(hintArea.toFloat(), 4.0f);
            g.setColour(theme::Colours::text().withAlpha(0.60f));
            g.setFont(10.5f);
            g.drawText("Ruler: click/drag scrub, double-click sets loop  |  Alt-drag duplicates",
                       hintArea.reduced(6, 1), juce::Justification::centredLeft, true);

            g.setGradientFill(juce::ColourGradient(theme::Colours::panel().brighter(0.08f),
                                                   static_cast<float>(rulerArea.getX()),
                                                   static_cast<float>(rulerArea.getY()),
                                                   theme::Colours::darker().withAlpha(0.88f),
                                                   static_cast<float>(rulerArea.getX()),
                                                   static_cast<float>(rulerArea.getBottom()),
                                                   false));
            g.fillRect(rulerArea);
            g.setColour(theme::Colours::gridLine().withAlpha(0.70f));
            g.drawHorizontalLine(rulerArea.getBottom() - 1,
                                 static_cast<float>(rulerArea.getX()),
                                 static_cast<float>(rulerArea.getRight()));

            const auto positionInfo = transport.getCurrentPositionInfo();
            const double beatsPerBar = juce::jmax(1.0,
                                                  static_cast<double>(positionInfo.timeSigNumerator)
                                                      * (4.0 / static_cast<double>(juce::jmax(1, positionInfo.timeSigDenominator))));
            const double visibleStartBeat = juce::jmax(0.0, static_cast<double>(scrollX / pixelsPerBeat));
            const double visibleEndBeat = juce::jmax(visibleStartBeat + 1.0,
                                                     static_cast<double>((scrollX + static_cast<float>(trackArea.getWidth())) / pixelsPerBeat));
            double mediaEndBeat = 0.0;
            for (const auto& clip : clips)
                mediaEndBeat = juce::jmax(mediaEndBeat, clip.startBeat + juce::jmax(0.25, clip.lengthBeats));
            const double loopEndBeat = transport.getLoopEndBeat();
            const double gridEndBeat = juce::jmax(visibleEndBeat,
                                                  juce::jmax(mediaEndBeat, loopEndBeat)) + 16.0;
            const double gridStartBeat = juce::jmax(0.0,
                                                    std::floor(visibleStartBeat / gridStepBeats) * gridStepBeats
                                                        - (gridStepBeats * 2.0));

            for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
            {
                const float y = static_cast<float>(trackArea.getY()) + (static_cast<float>(trackIndex) * trackHeight) - scrollY;
                if (y > static_cast<float>(trackArea.getBottom()) || (y + trackHeight) < static_cast<float>(trackArea.getY()))
                    continue;

                const juce::Rectangle<float> rowArea(static_cast<float>(trackArea.getX()),
                                                     y,
                                                     static_cast<float>(trackArea.getWidth()),
                                                     trackHeight);
                g.setColour((trackIndex % 2 == 0)
                                ? theme::Colours::darker().withAlpha(0.30f)
                                : theme::Colours::panel().withAlpha(0.16f));
                g.fillRect(rowArea);
                if (trackIndex == selectedTrackIndex)
                {
                    g.setColour(theme::Colours::accent().withAlpha(0.12f));
                    g.fillRect(rowArea);
                    g.setColour(theme::Colours::accent().withAlpha(0.55f));
                    g.fillRect(rowArea.withWidth(3.0f));
                }
                g.setColour(theme::Colours::gridLine().withAlpha(0.35f));
                g.drawHorizontalLine(static_cast<int>(y + trackHeight),
                                     static_cast<float>(trackArea.getX()),
                                     static_cast<float>(trackArea.getRight()));
            }

            const auto isMultipleOf = [](double value, double step)
            {
                if (step <= 0.0)
                    return false;
                const double ratio = value / step;
                return std::abs(ratio - std::round(ratio)) < 1.0e-6;
            };

            double beat = gridStartBeat;
            for (int line = 0; line < 22000 && beat <= (gridEndBeat + 1.0e-9); ++line, beat += gridStepBeats)
            {
                const float x = static_cast<float>(trackArea.getX()) + static_cast<float>(beat * pixelsPerBeat) - scrollX;
                if (x < static_cast<float>(trackArea.getX()) - 1.0f || x > static_cast<float>(trackArea.getRight()) + 1.0f)
                    continue;

                const bool barLine = isMultipleOf(beat, beatsPerBar);
                const bool beatLine = isMultipleOf(beat, 1.0);
                g.setColour(barLine ? theme::Colours::gridLine().brighter(0.38f)
                                    : (beatLine ? theme::Colours::gridLine().brighter(0.16f)
                                                : theme::Colours::gridLine().withAlpha(0.75f)));
                g.drawLine(x, static_cast<float>(trackArea.getY()), x, static_cast<float>(trackArea.getBottom()));

                g.setColour(barLine ? theme::Colours::gridLine().withAlpha(0.90f)
                                    : theme::Colours::gridLine().withAlpha(0.55f));
                g.drawLine(x, static_cast<float>(rulerArea.getY() + (barLine ? 1 : (rulerArea.getHeight() * 0.44f))),
                           x, static_cast<float>(rulerArea.getBottom() - 1.0f),
                           barLine ? 1.4f : 1.0f);

                if (barLine)
                {
                    const int barNumber = juce::jmax(1, static_cast<int>(std::floor(beat / beatsPerBar)) + 1);
                    g.setColour(theme::Colours::text().withAlpha(0.86f));
                    g.setFont(10.6f);
                    g.drawText(juce::String(barNumber),
                               juce::roundToInt(x + 3.0f),
                               rulerArea.getY() + 2,
                               44,
                               rulerArea.getHeight() - 4,
                               juce::Justification::centredLeft,
                               true);
                }
            }

            for (int i = 0; i < static_cast<int>(clips.size()); ++i)
            {
                const auto& clip = clips[static_cast<size_t>(i)];
                const bool drawingDraggedClip = dragActive && i == draggedClipIndex;
                const double drawStartBeat = drawingDraggedClip ? dragPreviewStartBeat : clip.startBeat;
                const int drawTrackIndex = drawingDraggedClip ? dragPreviewTrackIndex : clip.trackIndex;
                const double drawLengthBeat = drawingDraggedClip ? dragPreviewLengthBeats : clip.lengthBeats;
                juce::Rectangle<float> r(
                    static_cast<float>(trackArea.getX()) + static_cast<float>(drawStartBeat * pixelsPerBeat) - scrollX,
                    static_cast<float>(trackArea.getY()) + (static_cast<float>(drawTrackIndex) * trackHeight) - scrollY + 2.0f,
                    static_cast<float>(drawLengthBeat * pixelsPerBeat),
                    trackHeight - 4.0f
                );

                if (r.getRight() <= static_cast<float>(trackArea.getX())
                    || r.getX() >= static_cast<float>(trackArea.getRight())
                    || r.getBottom() <= static_cast<float>(trackArea.getY())
                    || r.getY() >= static_cast<float>(trackArea.getBottom()))
                {
                    continue;
                }

                auto clipColour = (clip.type == ClipType::MIDI) ? theme::Colours::clipMidi() : theme::Colours::clipAudio();
                if (drawingDraggedClip && dragMoved)
                    clipColour = clipColour.brighter(0.2f).withAlpha(0.92f);
                g.setColour(clipColour);

                const float corner = juce::jlimit(3.0f, 8.0f, r.getHeight() * 0.12f);
                g.fillRoundedRectangle(r, corner);

                if (clip.type == ClipType::MIDI
                    && !clip.events.empty()
                    && r.getWidth() > 26.0f
                    && r.getHeight() > 14.0f)
                {
                    auto notesPreviewArea = r.reduced(4.0f, 4.0f);
                    notesPreviewArea.setHeight(juce::jmax(6.0f, notesPreviewArea.getHeight() - 12.0f));

                    int minNote = 127;
                    int maxNote = 0;
                    for (const auto& ev : clip.events)
                    {
                        minNote = juce::jmin(minNote, ev.noteNumber);
                        maxNote = juce::jmax(maxNote, ev.noteNumber);
                    }
                    if (minNote > maxNote)
                    {
                        minNote = 60;
                        maxNote = 72;
                    }
                    if (maxNote == minNote)
                        ++maxNote;

                    const double invClipLen = 1.0 / juce::jmax(0.0001, clip.lengthBeats);
                    const int drawStep = juce::jmax(1, static_cast<int>(clip.events.size() / 180));
                    const float noteHeight = juce::jlimit(1.6f, 6.0f, notesPreviewArea.getHeight() / 14.0f);
                    g.setColour(juce::Colours::white.withAlpha(0.32f));
                    for (int eventIndex = 0; eventIndex < static_cast<int>(clip.events.size()); eventIndex += drawStep)
                    {
                        const auto& ev = clip.events[static_cast<size_t>(eventIndex)];
                        const float xNorm = static_cast<float>(juce::jlimit(0.0, 1.0, ev.startBeat * invClipLen));
                        const float wNorm = static_cast<float>(juce::jlimit(0.0, 1.0, ev.durationBeats * invClipLen));
                        const float yNorm = static_cast<float>(juce::jlimit(0.0,
                                                                            1.0,
                                                                            (static_cast<double>(ev.noteNumber - minNote)
                                                                             / static_cast<double>(maxNote - minNote))));
                        const float drawX = notesPreviewArea.getX() + (xNorm * notesPreviewArea.getWidth());
                        const float drawW = juce::jmax(1.5f, wNorm * notesPreviewArea.getWidth());
                        const float centerY = notesPreviewArea.getBottom() - (yNorm * notesPreviewArea.getHeight());
                        juce::Rectangle<float> noteRect(drawX,
                                                        centerY - (noteHeight * 0.5f),
                                                        drawW,
                                                        noteHeight);
                        noteRect = noteRect.getIntersection(notesPreviewArea);
                        if (!noteRect.isEmpty())
                            g.fillRoundedRectangle(noteRect, noteHeight * 0.34f);
                    }
                }

                const float textWidth = r.getWidth() - 12.0f;
                const float textHeight = r.getHeight() - 6.0f;
                if (textWidth > 28.0f && textHeight > 12.0f)
                {
                    float fontSize = juce::jlimit(11.5f, 18.0f,
                                                  juce::jmin(textHeight * 0.62f, textWidth * 0.22f));
                    juce::Font clipFont(juce::FontOptions(fontSize, juce::Font::bold));
                    juce::String clipLabel = clip.name.trim();
                    if (clipLabel.isNotEmpty())
                    {
                        const int approxChars = juce::jmax(3, static_cast<int>(
                            std::floor(textWidth / juce::jmax(6.0f, fontSize * 0.58f))));
                        if (clipLabel.length() > approxChars)
                            clipLabel = clipLabel.substring(0, juce::jmax(1, approxChars - 1)) + "...";
                        g.setColour(juce::Colours::white.withAlpha(0.95f));
                        g.setFont(clipFont);
                        g.drawFittedText(clipLabel,
                                         r.reduced(6.0f, 2.0f).toNearestInt(),
                                         juce::Justification::centredLeft,
                                         1);
                    }
                }

                if (i == selectedClipIndex)
                {
                    g.setColour(theme::Colours::accent().withAlpha(0.95f));
                    g.drawRoundedRectangle(r.reduced(0.5f), corner, 2.0f);
                }
            }

            const float phX = static_cast<float>(trackArea.getX()) + static_cast<float>(transport.getCurrentBeat() * pixelsPerBeat) - scrollX;
            if (phX >= static_cast<float>(trackArea.getX()))
            {
                g.setColour(theme::Colours::playhead());
                g.drawLine(phX, static_cast<float>(rulerArea.getY()), phX, static_cast<float>(trackArea.getBottom()), 1.5f);
            }

            if (trackReorderDragging && reorderTargetTrack >= 0)
            {
                const float indicatorY = static_cast<float>(trackArea.getY()) + (static_cast<float>(reorderTargetTrack) * trackHeight) - scrollY;
                g.setColour(theme::Colours::accent().withAlpha(0.95f));
                g.fillRect(0.0f, indicatorY, static_cast<float>(getWidth()), 2.5f);
            }

            const float dividerX = static_cast<float>(headerWidth) - 0.5f;
            g.setColour(theme::Colours::gridLine().withAlpha(headerResizeDragging ? 0.95f : 0.62f));
            g.drawLine(dividerX, 0.0f, dividerX, static_cast<float>(getHeight()), headerResizeDragging ? 2.0f : 1.2f);
            g.setColour(theme::Colours::text().withAlpha(0.40f));
            g.setFont(10.0f);
            g.drawText("||", headerWidth - 10, 2, 12, 12, juce::Justification::centred);

            g.setColour(theme::Colours::text().withAlpha(0.72f));
            g.setFont(11.0f);
            g.drawText(autoFollowPlayhead ? "Follow: ON" : "Follow: OFF",
                       rulerArea.getRight() - 104, rulerArea.getY() + 2, 100, 14, juce::Justification::centredRight);
        }

        void resized() override
        {
            if (!userSizedHeaderWidth)
                headerWidth = juce::jlimit(240, 520, juce::jmax(240, getWidth() / 4));

            const int scrollBarThickness = 12;
            const int contentWidth = juce::jmax(1, getWidth() - scrollBarThickness);
            const int contentHeight = juce::jmax(1, getHeight() - scrollBarThickness);
            headerWidth = juce::jlimit(230, juce::jmax(230, contentWidth - 300), headerWidth);
            clampScrollOffsets();

            horizontalScrollBar.setBounds(0, contentHeight, contentWidth, scrollBarThickness);
            verticalScrollBar.setBounds(contentWidth, rulerHeight, scrollBarThickness, juce::jmax(0, contentHeight - rulerHeight));
            horizontalScrollBar.setVisible(contentWidth > 120);
            verticalScrollBar.setVisible(contentHeight > rulerHeight + 40);

            const float viewportWidth = juce::jmax(1.0f, static_cast<float>(contentWidth - headerWidth));
            const float visibleBeats = viewportWidth / juce::jmax(1.0f, pixelsPerBeat);
            double mediaEndBeat = 0.0;
            for (const auto& clip : clips)
                mediaEndBeat = juce::jmax(mediaEndBeat, clip.startBeat + juce::jmax(0.25, clip.lengthBeats));
            const double playheadBeat = juce::jmax(0.0, transport.getCurrentBeat());
            const double loopEndBeat = transport.getLoopEndBeat();
            const double contentBeatExtent = juce::jmax(visibleBeats,
                                                        juce::jmax(playheadBeat + visibleBeats,
                                                                   juce::jmax(loopEndBeat, mediaEndBeat + 16.0)));
            horizontalScrollBar.setRangeLimits(0.0, juce::jmax(contentBeatExtent, visibleBeats));
            horizontalScrollBar.setCurrentRange(scrollX / juce::jmax(1.0f, pixelsPerBeat), visibleBeats);

            const float viewportTracks = getTrackViewportHeight() / juce::jmax(1.0f, trackHeight);
            const float contentTracks = juce::jmax(viewportTracks, static_cast<float>(tracks.size()));
            verticalScrollBar.setRangeLimits(0.0, contentTracks);
            verticalScrollBar.setCurrentRange(scrollY / juce::jmax(1.0f, trackHeight), viewportTracks);

            const float firstTrackY = getTrackAreaTop() - scrollY;
            const int h = static_cast<int>(trackHeight);
            for (int i = 0; i < headers.size(); ++i)
            {
                auto* header = headers[i];
                const int y = juce::roundToInt(firstTrackY + (static_cast<float>(i) * trackHeight));
                header->setBounds(0, y, headerWidth, h);
                header->setVisible(header->getBottom() >= rulerHeight
                                   && header->getY() <= getHeight());
            }
        }
        
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (!e.mods.isPopupMenu() && isOnHeaderResizeHandle(e.position.x))
            {
                headerResizeDragging = true;
                userSizedHeaderWidth = true;
                headerResizeStartX = static_cast<int>(std::round(e.position.x));
                headerResizeStartWidth = headerWidth;
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
                return;
            }

            const bool inHeader = e.position.x < static_cast<float>(headerWidth);
            const bool inRuler = !inHeader && e.position.y < static_cast<float>(rulerHeight);
            const int trackIndex = getTrackIndexForPositionY(e.position.y);
            const double clickedBeatRaw = getRawBeatForPositionX(e.position.x);
            const double clickedBeat = getBeatForPositionX(e.position.x);
            dragActive = false;
            dragMoved = false;
            draggedClipIndex = -1;
            dragMode = DragMode::none;
            scrubPlayheadDrag = false;
            laneReorderPending = false;
            laneReorderActive = false;
            laneReorderSourceTrack = -1;

            if (e.mods.isPopupMenu())
            {
                showContextMenu(trackIndex, inHeader ? transport.getCurrentBeat() : clickedBeat, e.position);
                return;
            }

            if (inHeader)
                return;

            if (inRuler)
            {
                const double beat = juce::jmax(0.0, clickedBeatRaw);
                transport.setPosition(beat);
                scrubPlayheadDrag = true;
                selectedClipIndex = -1;
                if (onClipSelected)
                    onClipSelected(nullptr);
                return;
            }

            const int clipIndex = findClipIndexAtPosition(e.position);
            if (clipIndex >= 0)
            {
                auto& clip = clips[static_cast<size_t>(clipIndex)];
                selectedClipIndex = clipIndex;
                selectTrack(clip.trackIndex);
                if (onTrackSelected)
                    onTrackSelected(clip.trackIndex);
                if (onClipSelected)
                    onClipSelected(&clip);

                draggedClipIndex = clipIndex;
                draggedOriginalStartBeat = clip.startBeat;
                draggedOriginalTrackIndex = clip.trackIndex;
                draggedOriginalLengthBeats = clip.lengthBeats;
                dragAnchorBeat = clickedBeatRaw;
                dragPreviewStartBeat = draggedOriginalStartBeat;
                dragPreviewTrackIndex = draggedOriginalTrackIndex;
                dragPreviewLengthBeats = juce::jmax(gridStepBeats, draggedOriginalLengthBeats);
                dragDuplicate = e.mods.isAltDown();
                dragMode = getResizeModeForPoint(clip, e.position);
                if (dragMode != DragMode::move)
                    dragDuplicate = false;
                dragActive = true;
                return;
            }

            if (juce::isPositiveAndBelow(trackIndex, tracks.size()))
            {
                selectTrack(trackIndex);
                if (onTrackSelected)
                    onTrackSelected(trackIndex);
            }

            // Empty-lane gesture: horizontal drag scrubs playhead, vertical drag reorders tracks.
            laneReorderPending = juce::isPositiveAndBelow(trackIndex, tracks.size());
            laneReorderSourceTrack = laneReorderPending ? trackIndex : -1;
            laneGestureStart = e.position;

            timelinePanPending = true;
            timelinePanActive = false;
            timelinePanStartPos = e.position;
            timelinePanStartScrollX = scrollX;
            timelinePanStartScrollY = scrollY;

            // Playhead move on down for immediate feedback.
            const double beat = juce::jmax(0.0, clickedBeatRaw);
            transport.setPosition(beat);
            scrubPlayheadDrag = true;
            selectedClipIndex = -1;
            if (onClipSelected) onClipSelected(nullptr);
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (headerResizeDragging)
            {
                const int deltaX = static_cast<int>(std::round(e.position.x)) - headerResizeStartX;
                headerWidth = juce::jlimit(230, juce::jmax(230, getWidth() - 300), headerResizeStartWidth + deltaX);
                resized();
                repaint();
                return;
            }

            if (laneReorderPending && laneReorderSourceTrack >= 0)
            {
                const float dx = std::abs(e.position.x - laneGestureStart.x);
                const float dy = std::abs(e.position.y - laneGestureStart.y);
                if (dy > 4.0f && dy >= (dx * 0.9f))
                {
                    laneReorderPending = false;
                    scrubPlayheadDrag = false;
                    trackReorderDragging = true;
                    reorderSourceTrack = laneReorderSourceTrack;
                    reorderTargetTrack = laneReorderSourceTrack;
                    laneReorderActive = true;
                    repaint();
                }
            }

            if (timelinePanPending && !laneReorderActive)
            {
                const float dx = e.position.x - timelinePanStartPos.x;
                const float dy = e.position.y - timelinePanStartPos.y;
                if (!timelinePanActive)
                {
                    if (std::abs(dx) < 3.0f && std::abs(dy) < 3.0f)
                        return;
                    timelinePanActive = true;
                    scrubPlayheadDrag = false;
                }

                scrollX = juce::jmax(0.0f, timelinePanStartScrollX - dx);
                scrollY = juce::jmax(0.0f, timelinePanStartScrollY - dy);
                clampScrollOffsets();
                resized();
                repaint();
                return;
            }

            if (laneReorderActive && trackReorderDragging)
            {
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
                const int target = getTrackIndexForPositionY(e.position.y);
                reorderTargetTrack = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), target);
                repaint();
                return;
            }

            if (scrubPlayheadDrag && !dragActive)
            {
                transport.setPosition(juce::jmax(0.0, getRawBeatForPositionX(e.position.x)));
                return;
            }

            if (!dragActive || draggedClipIndex < 0 || draggedClipIndex >= static_cast<int>(clips.size()))
                return;

            if (tracks.isEmpty())
                return;

            const float edgeThreshold = 28.0f;
            const float leftEdge = static_cast<float>(headerWidth) + edgeThreshold;
            const float rightEdge = static_cast<float>(getWidth()) - edgeThreshold;
            const float topEdge = getTrackAreaTop() + edgeThreshold;
            const float bottomEdge = static_cast<float>(getHeight()) - edgeThreshold;
            bool scrolled = false;
            if (e.position.x < leftEdge)
            {
                scrollX = juce::jmax(0.0f, scrollX - 12.0f);
                scrolled = true;
            }
            else if (e.position.x > rightEdge)
            {
                scrollX += 12.0f;
                scrolled = true;
            }
            if (e.position.y < topEdge)
            {
                const float newScroll = juce::jmax(0.0f, scrollY - 10.0f);
                if (std::abs(newScroll - scrollY) > 0.001f)
                {
                    scrollY = newScroll;
                    scrolled = true;
                }
            }
            else if (e.position.y > bottomEdge)
            {
                const float newScroll = juce::jmin(getMaxScrollY(), scrollY + 10.0f);
                if (std::abs(newScroll - scrollY) > 0.001f)
                {
                    scrollY = newScroll;
                    scrolled = true;
                }
            }
            if (scrolled)
            {
                clampScrollOffsets();
                resized();
                repaint();
            }

            const auto applyGridSnap = [&](double beatValue)
            {
                if (e.mods.isShiftDown())
                    return juce::jmax(0.0, beatValue);
                return juce::jmax(0.0, std::round(beatValue / gridStepBeats) * gridStepBeats);
            };

            const double mouseBeatRaw = getRawBeatForPositionX(e.position.x);
            const double minLength = juce::jmax(0.0625, gridStepBeats);

            if (dragMode == DragMode::move)
            {
                const double deltaBeat = mouseBeatRaw - dragAnchorBeat;
                const double snappedStartBeat = applyGridSnap(draggedOriginalStartBeat + deltaBeat);
                const int targetTrackIndex = getTrackIndexForPositionY(e.position.y);
                const int clampedTrackIndex = juce::jlimit(0, tracks.size() - 1, targetTrackIndex);
                if (std::abs(snappedStartBeat - dragPreviewStartBeat) > 1.0e-6
                    || clampedTrackIndex != dragPreviewTrackIndex)
                {
                    dragPreviewStartBeat = snappedStartBeat;
                    dragPreviewTrackIndex = clampedTrackIndex;
                    dragMoved = true;
                    repaint();
                }
                return;
            }

            if (dragMode == DragMode::resizeLeft)
            {
                const double originalEndBeat = draggedOriginalStartBeat + draggedOriginalLengthBeats;
                const double maxStart = juce::jmax(0.0, originalEndBeat - minLength);
                const double newStart = juce::jlimit(0.0, maxStart, applyGridSnap(mouseBeatRaw));
                const double newLength = juce::jmax(minLength, originalEndBeat - newStart);
                if (std::abs(newStart - dragPreviewStartBeat) > 1.0e-6
                    || std::abs(newLength - dragPreviewLengthBeats) > 1.0e-6)
                {
                    dragPreviewStartBeat = newStart;
                    dragPreviewLengthBeats = newLength;
                    dragMoved = true;
                    repaint();
                }
                return;
            }

            if (dragMode == DragMode::resizeRight)
            {
                const double minEnd = draggedOriginalStartBeat + minLength;
                const double newEnd = juce::jmax(minEnd, applyGridSnap(mouseBeatRaw));
                const double newLength = juce::jmax(minLength, newEnd - draggedOriginalStartBeat);
                if (std::abs(newLength - dragPreviewLengthBeats) > 1.0e-6)
                {
                    dragPreviewStartBeat = draggedOriginalStartBeat;
                    dragPreviewLengthBeats = newLength;
                    dragMoved = true;
                    repaint();
                }
            }
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            if (headerResizeDragging)
            {
                headerResizeDragging = false;
                setMouseCursor(juce::MouseCursor::NormalCursor);
                repaint();
                return;
            }

            scrubPlayheadDrag = false;
            laneReorderPending = false;
            timelinePanPending = false;
            timelinePanActive = false;

            if (laneReorderActive && trackReorderDragging)
            {
                const int source = reorderSourceTrack;
                const int target = reorderTargetTrack;
                laneReorderActive = false;
                trackReorderDragging = false;
                reorderSourceTrack = -1;
                reorderTargetTrack = -1;
                repaint();

                if (source >= 0 && target >= 0 && source != target && onReorderTracks)
                    onReorderTracks(source, target);
                return;
            }

            if (!dragActive)
                return;

            if (dragMoved && juce::isPositiveAndBelow(draggedClipIndex, static_cast<int>(clips.size())))
            {
                if (dragMode == DragMode::move && onMoveClip)
                {
                    onMoveClip(draggedClipIndex,
                               dragPreviewTrackIndex,
                               dragPreviewStartBeat,
                               dragDuplicate);
                }
                else if ((dragMode == DragMode::resizeLeft || dragMode == DragMode::resizeRight) && onResizeClip)
                {
                    onResizeClip(draggedClipIndex,
                                 dragPreviewStartBeat,
                                 juce::jmax(0.0625, dragPreviewLengthBeats));
                }
                else
                {
                    auto& movedClip = clips[static_cast<size_t>(draggedClipIndex)];
                    movedClip.startBeat = dragPreviewStartBeat;
                    if (dragMode == DragMode::move)
                        movedClip.trackIndex = dragPreviewTrackIndex;
                    movedClip.lengthBeats = juce::jmax(0.0625, dragPreviewLengthBeats);
                }
            }

            dragActive = false;
            dragMoved = false;
            dragDuplicate = false;
            draggedClipIndex = -1;
            dragMode = DragMode::none;
            repaint();
        }

        void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override
        {
            if (bar == &horizontalScrollBar)
                scrollX = static_cast<float>(juce::jmax(0.0, newRangeStart) * pixelsPerBeat);
            else if (bar == &verticalScrollBar)
                scrollY = static_cast<float>(juce::jmax(0.0, newRangeStart) * trackHeight);
            else
                return;

            clampScrollOffsets();
            resized();
            repaint();
        }

        void mouseDoubleClick(const juce::MouseEvent& e) override
        {
            if (isOnHeaderResizeHandle(e.position.x))
            {
                userSizedHeaderWidth = false;
                resized();
                repaint();
                return;
            }

            if (e.mods.isPopupMenu() || e.position.x < static_cast<float>(headerWidth))
                return;

            if (e.position.y < static_cast<float>(rulerHeight))
            {
                const auto info = transport.getCurrentPositionInfo();
                const double beatsPerBar = juce::jmax(1.0,
                                                      static_cast<double>(info.timeSigNumerator)
                                                          * (4.0 / static_cast<double>(juce::jmax(1, info.timeSigDenominator))));
                const double beat = getBeatForPositionX(e.position.x);
                const double loopStart = juce::jmax(0.0, std::floor(beat / beatsPerBar) * beatsPerBar);
                const double loopEnd = loopStart + beatsPerBar;
                transport.setLoop(true, loopStart, loopEnd);
                transport.setPosition(loopStart);
                repaint();
                return;
            }

            const int trackIndex = getTrackIndexForPositionY(e.position.y);
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;

            const double beat = getBeatForPositionX(e.position.x);
            selectTrack(trackIndex);
            if (onTrackSelected)
                onTrackSelected(trackIndex);
            if (onCreateMidiClip)
                onCreateMidiClip(trackIndex, beat, 4.0);
        }

    private:
        static constexpr int rulerHeight = 26;

        float getTrackAreaTop() const
        {
            return static_cast<float>(rulerHeight);
        }

        float getTrackViewportHeight() const
        {
            constexpr float scrollBarThickness = 12.0f;
            return juce::jmax(0.0f, static_cast<float>(getHeight() - rulerHeight) - scrollBarThickness);
        }

        float getMaxScrollY() const
        {
            const float contentHeight = static_cast<float>(tracks.size()) * trackHeight;
            return juce::jmax(0.0f, contentHeight - getTrackViewportHeight());
        }

        void clampScrollOffsets()
        {
            scrollX = juce::jmax(0.0f, scrollX);
            scrollY = juce::jlimit(0.0f, getMaxScrollY(), scrollY);
        }

        bool isOnHeaderResizeHandle(float x) const
        {
            return std::abs(x - static_cast<float>(headerWidth)) <= 6.0f;
        }

        int getTrackIndexForPositionY(float y) const
        {
            if (tracks.isEmpty())
                return -1;

            const float adjustedY = y - getTrackAreaTop() + scrollY;
            if (adjustedY < 0.0f)
                return -1;

            const int idx = static_cast<int>(adjustedY / trackHeight);
            return juce::jlimit(0, tracks.size() - 1, idx);
        }

        double getBeatForPositionX(float x) const
        {
            const double rawBeat = getRawBeatForPositionX(x);
            return juce::jmax(0.0, std::round(rawBeat / gridStepBeats) * gridStepBeats);
        }

        double getRawBeatForPositionX(float x) const
        {
            const double beat = static_cast<double>((x - static_cast<float>(headerWidth) + scrollX) / pixelsPerBeat);
            return juce::jmax(0.0, beat);
        }

        int findClipIndexAtPosition(juce::Point<float> pos) const
        {
            for (int i = 0; i < static_cast<int>(clips.size()); ++i)
            {
                const auto& clip = clips[static_cast<size_t>(i)];
                auto r = getClipBounds(clip);
                if (r.contains(pos))
                    return i;
            }
            return -1;
        }

        juce::Rectangle<float> getClipBounds(const Clip& clip) const
        {
            return {
                static_cast<float>(headerWidth) + static_cast<float>(clip.startBeat * pixelsPerBeat) - scrollX,
                getTrackAreaTop() + (static_cast<float>(clip.trackIndex) * trackHeight) - scrollY + 2.0f,
                static_cast<float>(clip.lengthBeats * pixelsPerBeat),
                trackHeight - 4.0f
            };
        }

        DragMode getResizeModeForPoint(const Clip& clip, juce::Point<float> pos) const
        {
            const auto rect = getClipBounds(clip);
            if (!rect.contains(pos))
                return DragMode::none;

            // Keep short clips easy to drag/move rather than forcing resize-only interaction.
            if (rect.getWidth() < 22.0f)
                return DragMode::move;

            const float edgeWidth = juce::jlimit(4.0f, 10.0f, rect.getWidth() * 0.16f);
            if (std::abs(pos.x - rect.getX()) <= edgeWidth)
                return DragMode::resizeLeft;
            if (std::abs(pos.x - rect.getRight()) <= edgeWidth)
                return DragMode::resizeRight;
            return DragMode::move;
        }

        void showContextMenu(int trackIndex, double beat, juce::Point<float> clickPosition)
        {
            juce::PopupMenu menu;
            menu.addItem(1, "Create MIDI Track");
            if (trackIndex >= 0)
                menu.addItem(2, "Create MIDI Clip Here");
            menu.addItem(4, "Move Playhead Here");
            menu.addItem(11, "Follow Playhead", true, autoFollowPlayhead);
            if (trackIndex >= 0)
            {
                menu.addSeparator();
                menu.addItem(9, "Load Instrument...");
                menu.addItem(10, "Open Plugin UI", tracks[trackIndex]->hasPlugin());
                menu.addItem(54, "Open Track EQ");
                juce::PopupMenu loadInsertMenu;
                juce::PopupMenu openInsertMenu;
                for (int slot = 0; slot < tracks[trackIndex]->getPluginSlotCount(); ++slot)
                {
                    const bool loaded = tracks[trackIndex]->hasPluginInSlot(slot);
                    const auto pluginName = tracks[trackIndex]->getPluginNameForSlot(slot);
                    const juce::String slotName = "Insert " + juce::String(slot + 1)
                                                + " (" + (pluginName.isNotEmpty() ? pluginName : juce::String("Empty")) + ")";
                    loadInsertMenu.addItem(30 + slot, slotName);
                    openInsertMenu.addItem(34 + slot, slotName, loaded);
                }
                menu.addSubMenu("Load Plugin Into Insert", loadInsertMenu);
                menu.addSubMenu("Open Insert UI", openInsertMenu);
                menu.addItem(12, "Rename Track...");
                menu.addItem(13, "Duplicate Track");
                menu.addItem(14, "Delete Track", tracks.size() > 1);
                menu.addItem(15, "Move Track Up", trackIndex > 0);
                menu.addItem(16, "Move Track Down", trackIndex < tracks.size() - 1);
                menu.addSeparator();
                menu.addItem(52, "Open Channel Rack");
                menu.addItem(53, "Open Inspector");
                menu.addSeparator();
                juce::PopupMenu sendMenu;
                sendMenu.addItem(21, "Send 0%", true, tracks[trackIndex]->getSendLevel() <= 0.001f);
                sendMenu.addItem(22, "Send 25%", true, std::abs(tracks[trackIndex]->getSendLevel() - 0.25f) < 0.01f);
                sendMenu.addItem(23, "Send 50%", true, std::abs(tracks[trackIndex]->getSendLevel() - 0.50f) < 0.01f);
                sendMenu.addItem(24, "Send 75%", true, std::abs(tracks[trackIndex]->getSendLevel() - 0.75f) < 0.01f);
                sendMenu.addItem(25, "Send 100%", true, std::abs(tracks[trackIndex]->getSendLevel() - 1.00f) < 0.01f);
                menu.addSubMenu("Set Send Level", sendMenu);
                juce::PopupMenu sendModeMenu;
                const auto sendTap = tracks[trackIndex]->getSendTapMode();
                sendModeMenu.addItem(26, "Pre-Fader", true, sendTap == Track::SendTapMode::PreFader);
                sendModeMenu.addItem(27, "Post-Fader", true, sendTap == Track::SendTapMode::PostFader);
                sendModeMenu.addItem(28, "Post-Pan", true, sendTap == Track::SendTapMode::PostPan);
                menu.addSubMenu("Send Tap Point", sendModeMenu);
                juce::PopupMenu sendTargetMenu;
                for (int bus = 0; bus < Track::maxSendBuses; ++bus)
                {
                    sendTargetMenu.addItem(60 + bus,
                                           "Bus " + juce::String(bus + 1),
                                           true,
                                           tracks[trackIndex]->getSendTargetBus() == bus);
                }
                menu.addSubMenu("Send Target", sendTargetMenu);
                juce::PopupMenu outputTargetMenu;
                const auto outputType = tracks[trackIndex]->getOutputTargetType();
                outputTargetMenu.addItem(80, "Master", true, outputType == Track::OutputTargetType::Master);
                for (int bus = 0; bus < Track::maxSendBuses; ++bus)
                {
                    outputTargetMenu.addItem(81 + bus,
                                             "Bus " + juce::String(bus + 1),
                                             true,
                                             outputType == Track::OutputTargetType::Bus
                                                 && tracks[trackIndex]->getOutputTargetBus() == bus);
                }
                menu.addSubMenu("Output Target", outputTargetMenu);
                menu.addSeparator();
                menu.addItem(17, "Toggle Mute", true, tracks[trackIndex]->isMuted());
                menu.addItem(18, "Toggle Solo", true, tracks[trackIndex]->isSolo());
                menu.addItem(19, "Toggle Arm", true, tracks[trackIndex]->isArmed());
                menu.addItem(20, "Toggle Input Monitor", true, tracks[trackIndex]->isInputMonitoringEnabled());
            }

            const int clipIndex = findClipIndexAtPosition(clickPosition);
            if (clipIndex >= 0)
            {
                menu.addItem(3, "Delete Clip");
                menu.addItem(5, "Split Clip At Cursor");
                menu.addItem(6, "Duplicate Clip");
                menu.addItem(7, "Nudge Clip Left");
                menu.addItem(8, "Nudge Clip Right");
            }

            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(this),
                [this, trackIndex, beat, clipIndex](int selectedId)
                {
                    if (selectedId == 1)
                    {
                        if (onCreateMidiTrack)
                            onCreateMidiTrack(juce::jmax(0.0, beat));
                    }
                    else if (selectedId == 2)
                    {
                        if (trackIndex >= 0)
                        {
                            selectTrack(trackIndex);
                            if (onTrackSelected)
                                onTrackSelected(trackIndex);
                            if (onCreateMidiClip)
                                onCreateMidiClip(trackIndex, juce::jmax(0.0, beat), 4.0);
                        }
                    }
                    else if (selectedId == 3)
                    {
                        if (clipIndex >= 0 && onDeleteClip)
                            onDeleteClip(clipIndex);
                    }
                    else if (selectedId == 4)
                    {
                        transport.setPosition(juce::jmax(0.0, beat));
                        if (onClipSelected)
                            onClipSelected(nullptr);
                    }
                    else if (selectedId == 5)
                    {
                        if (clipIndex >= 0 && onSplitClipAtBeat)
                            onSplitClipAtBeat(clipIndex, juce::jmax(0.0, beat));
                    }
                    else if (selectedId == 6)
                    {
                        if (clipIndex >= 0 && onDuplicateClip)
                            onDuplicateClip(clipIndex);
                    }
                    else if (selectedId == 7)
                    {
                        if (clipIndex >= 0 && onNudgeClipBy)
                            onNudgeClipBy(clipIndex, -gridStepBeats);
                    }
                    else if (selectedId == 8)
                    {
                        if (clipIndex >= 0 && onNudgeClipBy)
                            onNudgeClipBy(clipIndex, gridStepBeats);
                    }
                    else if (selectedId == 9)
                    {
                        if (trackIndex >= 0)
                        {
                            selectTrack(trackIndex);
                            if (onTrackSelected)
                                onTrackSelected(trackIndex);
                            if (onLoadPluginForTrack)
                                onLoadPluginForTrack(trackIndex, this, Track::instrumentSlotIndex);
                        }
                    }
                    else if (selectedId == 10)
                    {
                        if (trackIndex >= 0)
                        {
                            selectTrack(trackIndex);
                            if (onTrackSelected)
                                onTrackSelected(trackIndex);
                            if (onOpenPluginEditorForTrack)
                                onOpenPluginEditorForTrack(trackIndex, tracks[trackIndex]->getFirstLoadedPluginSlot());
                        }
                    }
                    else if (selectedId == 54)
                    {
                        if (trackIndex >= 0 && onOpenTrackEq)
                            onOpenTrackEq(trackIndex);
                    }
                    else if (selectedId >= 30 && selectedId <= 33)
                    {
                        if (trackIndex >= 0)
                        {
                            const int slotIndex = selectedId - 30;
                            selectTrack(trackIndex);
                            if (onTrackSelected)
                                onTrackSelected(trackIndex);
                            if (onLoadPluginForTrack)
                                onLoadPluginForTrack(trackIndex, this, slotIndex);
                        }
                    }
                    else if (selectedId >= 34 && selectedId <= 37)
                    {
                        if (trackIndex >= 0)
                        {
                            const int slotIndex = selectedId - 34;
                            selectTrack(trackIndex);
                            if (onTrackSelected)
                                onTrackSelected(trackIndex);
                            if (onOpenPluginEditorForTrack)
                                onOpenPluginEditorForTrack(trackIndex, slotIndex);
                        }
                    }
                    else if (selectedId == 11)
                    {
                        autoFollowPlayhead = !autoFollowPlayhead;
                        repaint();
                    }
                    else if (selectedId == 12)
                    {
                        if (trackIndex >= 0 && onRenameTrack)
                            onRenameTrack(trackIndex);
                    }
                    else if (selectedId == 13)
                    {
                        if (trackIndex >= 0 && onDuplicateTrack)
                            onDuplicateTrack(trackIndex);
                    }
                    else if (selectedId == 14)
                    {
                        if (trackIndex >= 0 && onDeleteTrack)
                            onDeleteTrack(trackIndex);
                    }
                    else if (selectedId == 15)
                    {
                        if (trackIndex >= 0 && onMoveTrackUp)
                            onMoveTrackUp(trackIndex);
                    }
                    else if (selectedId == 16)
                    {
                        if (trackIndex >= 0 && onMoveTrackDown)
                            onMoveTrackDown(trackIndex);
                    }
                    else if (selectedId == 17)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->setMute(!tracks[trackIndex]->isMuted());
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId == 18)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->setSolo(!tracks[trackIndex]->isSolo());
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId == 19)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->setArm(!tracks[trackIndex]->isArmed());
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId == 20)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->setInputMonitoring(!tracks[trackIndex]->isInputMonitoringEnabled());
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId >= 21 && selectedId <= 25)
                    {
                        if (trackIndex >= 0)
                        {
                            static constexpr float sendPresets[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
                            tracks[trackIndex]->setSendLevel(sendPresets[selectedId - 21]);
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId == 26 || selectedId == 27 || selectedId == 28)
                    {
                        if (trackIndex >= 0)
                        {
                            const auto mode = (selectedId == 26) ? Track::SendTapMode::PreFader
                                                                 : (selectedId == 27 ? Track::SendTapMode::PostFader
                                                                                     : Track::SendTapMode::PostPan);
                            tracks[trackIndex]->setSendTapMode(mode);
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId >= 60 && selectedId < 60 + Track::maxSendBuses)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->setSendTargetBus(selectedId - 60);
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId == 80)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->routeOutputToMaster();
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId >= 81 && selectedId < 81 + Track::maxSendBuses)
                    {
                        if (trackIndex >= 0)
                        {
                            tracks[trackIndex]->routeOutputToBus(selectedId - 81);
                            if (onTrackStateChanged)
                                onTrackStateChanged(trackIndex);
                        }
                    }
                    else if (selectedId == 52)
                    {
                        if (trackIndex >= 0 && onOpenChannelRack)
                            onOpenChannelRack(trackIndex);
                    }
                    else if (selectedId == 53)
                    {
                        if (trackIndex >= 0 && onOpenInspector)
                            onOpenInspector(trackIndex);
                    }
                }
            );
        }

        TransportEngine& transport;
        std::vector<Clip>& clips;
        const juce::OwnedArray<Track>& tracks;
        juce::OwnedArray<TrackHeader> headers;
        float scrollX = 0.0f;
        float scrollY = 0.0f;
        float pixelsPerBeat = 80.0f;
        float trackHeight = 124.0f;
        double gridStepBeats = 0.25;
        bool dragActive = false;
        bool dragMoved = false;
        bool dragDuplicate = false;
        bool scrubPlayheadDrag = false;
        bool laneReorderPending = false;
        bool laneReorderActive = false;
        bool timelinePanPending = false;
        bool timelinePanActive = false;
        bool autoFollowPlayhead = true;
        int selectedTrackIndex = 0;
        DragMode dragMode = DragMode::none;
        int selectedClipIndex = -1;
        int draggedClipIndex = -1;
        int draggedOriginalTrackIndex = -1;
        int dragPreviewTrackIndex = -1;
        double draggedOriginalStartBeat = 0.0;
        double draggedOriginalLengthBeats = 0.0;
        double dragAnchorBeat = 0.0;
        double dragPreviewStartBeat = 0.0;
        double dragPreviewLengthBeats = 0.0;
        bool trackReorderDragging = false;
        int reorderSourceTrack = -1;
        int reorderTargetTrack = -1;
        int laneReorderSourceTrack = -1;
        juce::Point<float> laneGestureStart;
        juce::Point<float> timelinePanStartPos;
        float timelinePanStartScrollX = 0.0f;
        float timelinePanStartScrollY = 0.0f;
        bool userSizedHeaderWidth = false;
        bool headerResizeDragging = false;
        int headerResizeStartX = 0;
        int headerResizeStartWidth = 280;
        int headerWidth = 320;
        juce::ScrollBar horizontalScrollBar { false };
        juce::ScrollBar verticalScrollBar { true };
    };
}
