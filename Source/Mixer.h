#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "Track.h"
#include "TimelineModel.h"
#include "Theme.h"

namespace sampledex
{
    class MixerChannel : public juce::Component, private juce::Timer
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
        std::function<void(AutomationTarget, bool)> onAutomationTouch;
        std::function<void(AutomationTarget, AutomationMode)> onSetAutomationMode;
        std::function<AutomationMode(AutomationTarget)> getAutomationMode;

        MixerChannel(Track& t) : track(t)
        {
            trackName.setText(track.getTrackName(), juce::dontSendNotification);
            trackName.setJustificationType(juce::Justification::centred);
            trackName.setColour(juce::Label::textColourId, juce::Colours::white);
            trackName.setFont(juce::Font(juce::FontOptions(15.5f, juce::Font::bold)));
            trackName.setTooltip("Track name. Double-click to rename. Drag strip/header area to reorder.");
            trackName.setInterceptsMouseClicks(false, false);
            pluginName.setJustificationType(juce::Justification::centredLeft);
            pluginName.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.72f));
            pluginName.setFont(juce::Font(juce::FontOptions(13.2f)));
            pluginName.setMinimumHorizontalScale(0.76f);
            pluginName.setText(track.getPluginSummary(), juce::dontSendNotification);
            pluginName.setTooltip("Instrument + insert chain. Left-click name to open plugin UI; right-click for plugin menu.");
            pluginName.setInterceptsMouseClicks(false, false);

            gainValueLabel.setJustificationType(juce::Justification::centred);
            gainValueLabel.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.86f));
            gainValueLabel.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
            gainValueLabel.setInterceptsMouseClicks(false, false);
            gainValueLabel.setText("-inf dB", juce::dontSendNotification);
            
            addMouseListener(this, true); 

            fader.setSliderStyle(juce::Slider::LinearVertical);
            fader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            fader.setRange(0.0, 1.2, 0.01);
            fader.setValue(track.getVolume(), juce::dontSendNotification);
            fader.setDoubleClickReturnValue(true, track.getVolume());
            fader.onValueChange = [this] { track.setVolume((float)fader.getValue()); };
            fader.onDragStart = [this]
            {
                if (onAutomationTouch)
                    onAutomationTouch(AutomationTarget::TrackVolume, true);
            };
            fader.onDragEnd = [this]
            {
                if (onAutomationTouch)
                    onAutomationTouch(AutomationTarget::TrackVolume, false);
            };
            fader.setTooltip("Track volume. Double-click to reset.");

            panKnob.setSliderStyle(juce::Slider::Rotary);
            panKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            panKnob.setRange(-1.0, 1.0, 0.01);
            panKnob.setValue(0.0, juce::dontSendNotification);
            panKnob.setDoubleClickReturnValue(true, 0.0);
            panKnob.onValueChange = [this] { track.setPan((float)panKnob.getValue()); };
            panKnob.onDragStart = [this]
            {
                if (onAutomationTouch)
                    onAutomationTouch(AutomationTarget::TrackPan, true);
            };
            panKnob.onDragEnd = [this]
            {
                if (onAutomationTouch)
                    onAutomationTouch(AutomationTarget::TrackPan, false);
            };
            panKnob.setTooltip("Pan. Double-click to center.");

            sendKnob.setSliderStyle(juce::Slider::Rotary);
            sendKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            sendKnob.setRange(0.0, 1.0, 0.01);
            sendKnob.setValue(0.0, juce::dontSendNotification);
            sendKnob.setDoubleClickReturnValue(true, 0.0);
            sendKnob.onValueChange = [this] { track.setSendLevel((float)sendKnob.getValue()); };
            sendKnob.onDragStart = [this]
            {
                if (onAutomationTouch)
                    onAutomationTouch(AutomationTarget::TrackSend, true);
            };
            sendKnob.onDragEnd = [this]
            {
                if (onAutomationTouch)
                    onAutomationTouch(AutomationTarget::TrackSend, false);
            };
            sendKnob.setTooltip("Aux send. Double-click to reset.");

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
            sendTapBox.setTooltip("Send tap mode.");

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
            outputTargetBox.setTooltip("Track output route.");

            const auto styleRouteBox = [](juce::ComboBox& box)
            {
                box.setColour(juce::ComboBox::backgroundColourId, theme::Colours::panel().brighter(0.08f));
                box.setColour(juce::ComboBox::textColourId, theme::Colours::text());
                box.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.24f));
                box.setColour(juce::ComboBox::arrowColourId, theme::Colours::text().withAlpha(0.86f));
            };
            styleRouteBox(sendTapBox);
            styleRouteBox(sendTargetBox);
            styleRouteBox(outputTargetBox);

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

            addAndMakeVisible(fader);
            addAndMakeVisible(panKnob);
            addAndMakeVisible(sendKnob);
            addAndMakeVisible(sendTapBox);
            addAndMakeVisible(sendTargetBox);
            addAndMakeVisible(outputTargetBox);
            addAndMakeVisible(muteBtn);
            addAndMakeVisible(soloBtn);
            addAndMakeVisible(armBtn);
            addAndMakeVisible(monitorBtn);
            addAndMakeVisible(gainValueLabel);
            
            startTimerHz(12);
        }

        void setSelected(bool isSelected)
        {
            selected = isSelected;
            repaint();
        }

        bool isSelected() const { return selected; }

        float getSendVisualLevel() const { return juce::jlimit(0.0f, 1.0f, track.getSendLevel()); }
        bool isSendPreFader() const { return track.isSendPreFader(); }
        Track::SendTapMode getSendTapMode() const { return track.getSendTapMode(); }
        int getSendTargetBus() const { return track.getSendTargetBus(); }
        juce::Point<float> getSendKnobCenterInParent() const
        {
            return sendKnob.getBounds().toFloat().getCentre() + getPosition().toFloat();
        }
        float getSendLevel() const { return track.getSendLevel(); }
        juce::String getTrackName() const { return track.getTrackName(); }
        void setSendLevelFromUi(float value)
        {
            const float clamped = juce::jlimit(0.0f, 1.0f, value);
            track.setSendLevel(clamped);
            sendKnob.setValue(clamped, juce::dontSendNotification);
            if (onStateChanged) onStateChanged();
        }
        void setSendPreFaderFromUi(bool preFader)
        {
            track.setSendPreFader(preFader);
            if (onStateChanged) onStateChanged();
        }
        void setSendTapModeFromUi(Track::SendTapMode mode)
        {
            track.setSendTapMode(mode);
            if (onStateChanged) onStateChanged();
        }
        void setSendTargetBusFromUi(int busIndex)
        {
            track.setSendTargetBus(busIndex);
            if (onStateChanged) onStateChanged();
        }

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
                const bool dragSurface = (e.eventComponent == this)
                    || (e.eventComponent == &trackName)
                    || isOnReorderHandle(localPos);
                if (dragSurface)
                {
                    dragGesturePending = true;
                    dragGestureActive = false;
                    if (auto* parent = getParentComponent())
                        dragStartInParent = e.getEventRelativeTo(parent).position;
                    else
                        dragStartInParent = e.position;
                }
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
            const auto currentTap = track.getSendTapMode();
            sendModeMenu.addItem(45, "Pre-Fader", true, currentTap == Track::SendTapMode::PreFader);
            sendModeMenu.addItem(46, "Post-Fader", true, currentTap == Track::SendTapMode::PostFader);
            sendModeMenu.addItem(47, "Post-Pan", true, currentTap == Track::SendTapMode::PostPan);
            menu.addSubMenu("Send Tap Point", sendModeMenu);
            juce::PopupMenu sendBusMenu;
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
                sendBusMenu.addItem(48 + bus,
                                    "Bus " + juce::String(bus + 1),
                                    true,
                                    track.getSendTargetBus() == bus);
            menu.addSubMenu("Send Target", sendBusMenu);
            juce::PopupMenu outputTargetMenu;
            const auto outputType = track.getOutputTargetType();
            outputTargetMenu.addItem(67, "Master", true, outputType == Track::OutputTargetType::Master);
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                outputTargetMenu.addItem(68 + bus,
                                         "Bus " + juce::String(bus + 1),
                                         true,
                                         outputType == Track::OutputTargetType::Bus
                                             && track.getOutputTargetBus() == bus);
            }
            menu.addSubMenu("Output Target", outputTargetMenu);
            juce::PopupMenu automationMenu;
            const auto addAutomationModeSubMenu = [this, &automationMenu](AutomationTarget target,
                                                                           const juce::String& targetName,
                                                                           int baseId)
            {
                juce::PopupMenu modeMenu;
                const auto currentMode = getAutomationMode != nullptr
                    ? getAutomationMode(target)
                    : AutomationMode::Read;
                modeMenu.addItem(baseId + 0, "Read", true, currentMode == AutomationMode::Read);
                modeMenu.addItem(baseId + 1, "Touch", true, currentMode == AutomationMode::Touch);
                modeMenu.addItem(baseId + 2, "Latch", true, currentMode == AutomationMode::Latch);
                modeMenu.addItem(baseId + 3, "Write", true, currentMode == AutomationMode::Write);
                automationMenu.addSubMenu(targetName, modeMenu);
            };
            addAutomationModeSubMenu(AutomationTarget::TrackVolume, "Volume", 300);
            addAutomationModeSubMenu(AutomationTarget::TrackPan, "Pan", 310);
            addAutomationModeSubMenu(AutomationTarget::TrackSend, "Send", 320);
            menu.addSubMenu("Automation Mode", automationMenu);
            juce::PopupMenu inputSourceMenu;
            inputSourceMenu.addItem(52,
                                    "Auto (Detect)",
                                    true,
                                    track.getInputSourcePair() < 0);
            for (int pair = 0; pair < 8; ++pair)
            {
                const int left = pair * 2 + 1;
                const int right = left + 1;
                inputSourceMenu.addItem(53 + pair,
                                        "Input " + juce::String(left) + "/" + juce::String(right),
                                        true,
                                        track.getInputSourcePair() == pair);
            }
            menu.addSubMenu("Input Source", inputSourceMenu);
            juce::PopupMenu monitorTapMenu;
            const auto monitorTap = track.getMonitorTapMode();
            monitorTapMenu.addItem(60, "Pre Inserts", true, monitorTap == Track::MonitorTapMode::PreInserts);
            monitorTapMenu.addItem(61, "Post Inserts", true, monitorTap == Track::MonitorTapMode::PostInserts);
            menu.addSubMenu("Monitor Tap", monitorTapMenu);
            juce::PopupMenu monitorGainMenu;
            const float monitorGain = track.getInputMonitorGain();
            monitorGainMenu.addItem(62, "50%", true, std::abs(monitorGain - 0.50f) < 0.03f);
            monitorGainMenu.addItem(63, "68% (Default)", true, std::abs(monitorGain - 0.68f) < 0.03f);
            monitorGainMenu.addItem(64, "85%", true, std::abs(monitorGain - 0.85f) < 0.03f);
            monitorGainMenu.addItem(65, "100%", true, std::abs(monitorGain - 1.00f) < 0.03f);
            monitorGainMenu.addItem(66, "120%", true, std::abs(monitorGain - 1.20f) < 0.03f);
            menu.addSubMenu("Monitor Gain", monitorGainMenu);
            menu.addSeparator();
            menu.addItem(3, "Toggle Mute", true, track.isMuted());
            menu.addItem(4, "Toggle Solo", true, track.isSolo());
            menu.addItem(5, "Toggle Arm", true, track.isArmed());
            menu.addItem(6, "Toggle Input Monitor", true, track.isInputMonitoringEnabled());
            menu.addSeparator();
            menu.addItem(7, "Rename Track...");
            menu.addItem(8, "Duplicate Track");
            menu.addItem(9, "Delete Track");
            menu.addItem(10, "Move Track Left");
            menu.addItem(11, "Move Track Right");
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
                                       sendKnob.setValue(track.getSendLevel(), juce::dontSendNotification);
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
                                   else if (selectedId == 67)
                                   {
                                       track.routeOutputToMaster();
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId >= 68 && selectedId < 68 + Track::maxSendBuses)
                                   {
                                       track.routeOutputToBus(selectedId - 68);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId >= 300 && selectedId <= 333)
                                   {
                                       AutomationTarget target = AutomationTarget::TrackVolume;
                                       int modeOffset = 0;
                                       if (selectedId >= 320)
                                       {
                                           target = AutomationTarget::TrackSend;
                                           modeOffset = selectedId - 320;
                                       }
                                       else if (selectedId >= 310)
                                       {
                                           target = AutomationTarget::TrackPan;
                                           modeOffset = selectedId - 310;
                                       }
                                       else
                                       {
                                           target = AutomationTarget::TrackVolume;
                                           modeOffset = selectedId - 300;
                                       }

                                       const auto mode = static_cast<AutomationMode>(juce::jlimit(0, 3, modeOffset));
                                       if (onSetAutomationMode)
                                           onSetAutomationMode(target, mode);
                                   }
                                   else if (selectedId == 52)
                                   {
                                       track.setInputSourcePair(-1);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId >= 53 && selectedId < 60)
                                   {
                                       track.setInputSourcePair(selectedId - 53);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId == 60 || selectedId == 61)
                                   {
                                       track.setMonitorTapMode(selectedId == 60 ? Track::MonitorTapMode::PreInserts
                                                                                : Track::MonitorTapMode::PostInserts);
                                       if (onStateChanged) onStateChanged();
                                   }
                                   else if (selectedId >= 62 && selectedId <= 66)
                                   {
                                       static constexpr float monitorGains[] { 0.50f, 0.68f, 0.85f, 1.00f, 1.20f };
                                       track.setInputMonitorGain(monitorGains[selectedId - 62]);
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
                onReorderDragMove(parentPos.x);
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            if (dragGestureActive && onReorderDragEnd)
                onReorderDragEnd();
            dragGesturePending = false;
            dragGestureActive = false;
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
                                     || trackName.getBounds().toFloat().contains(localPos)
                                     || isOnReorderHandle(localPos);
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

        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();

            const auto base = theme::Colours::panel();
            g.setGradientFill(juce::ColourGradient(base.brighter(0.06f), 0.0f, r.getY(),
                                                   base.darker(0.26f), 0.0f, r.getBottom(),
                                                   false));
            g.fillRoundedRectangle(r, 7.0f);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawLine(r.getX() + 2.0f, r.getY() + 2.0f, r.getRight() - 2.0f, r.getY() + 2.0f, 1.0f);
            g.setColour(juce::Colours::black.withAlpha(0.30f));
            g.drawLine(r.getX() + 2.0f, r.getBottom() - 2.0f, r.getRight() - 2.0f, r.getBottom() - 2.0f, 1.0f);

            if (selected)
            {
                g.setColour(theme::Colours::accent().withAlpha(0.22f));
                g.fillRoundedRectangle(r.reduced(1.5f), 6.0f);
                g.setColour(theme::Colours::accent().withAlpha(0.95f));
                g.drawRoundedRectangle(r.reduced(1.0f), 6.0f, 2.0f);
            }
            else
            {
                g.setColour(juce::Colours::black.withAlpha(0.42f));
                g.drawRoundedRectangle(r.reduced(1.0f), 6.0f, 1.0f);
            }

            // Track reorder drag hint (horizontal in mixer).
            const auto handleArea = juce::Rectangle<float>(r.getRight() - 16.0f, 6.0f, 10.0f, 12.0f);
            g.setColour(theme::Colours::text().withAlpha(0.42f));
            for (int i = 0; i < 3; ++i)
            {
                const float x = handleArea.getX() + (static_cast<float>(i) * 3.2f);
                g.drawLine(x, handleArea.getY(), x, handleArea.getBottom(), 1.0f);
            }

            const juce::String typeLabel = getChannelTypeShortLabel(track.getChannelType());
            const juce::Rectangle<float> typeBadge(r.getX() + 7.0f, r.getY() + 7.0f, 44.0f, 15.0f);
            g.setColour(theme::Colours::darker().withAlpha(0.74f));
            g.fillRoundedRectangle(typeBadge, 3.0f);
            g.setColour(theme::Colours::accent().withAlpha(0.90f));
            g.drawRoundedRectangle(typeBadge, 3.0f, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.90f));
            g.setFont(9.0f);
            g.drawFittedText(typeLabel, typeBadge.toNearestInt(), juce::Justification::centred, 1);

            if (track.isRenderTaskActive())
            {
                const float progress = juce::jlimit(0.0f, 1.0f, track.getRenderTaskProgress());
                const juce::String label = track.getRenderTaskLabel();
                const juce::Rectangle<float> badge(r.getRight() - 86.0f, r.getY() + 7.0f, 78.0f, 15.0f);
                g.setColour(theme::Colours::panel().withAlpha(0.92f));
                g.fillRoundedRectangle(badge, 3.0f);
                g.setColour(theme::Colours::accent().withAlpha(0.28f));
                g.fillRoundedRectangle(badge.withWidth(badge.getWidth() * progress), 3.0f);
                g.setColour(theme::Colours::accent().withAlpha(0.95f));
                g.drawRoundedRectangle(badge, 3.0f, 1.0f);
                g.setColour(theme::Colours::text().withAlpha(0.96f));
                g.setFont(8.8f);
                g.drawFittedText(label + " " + juce::String(juce::roundToInt(progress * 100.0f)) + "%",
                                 badge.toNearestInt(),
                                 juce::Justification::centred,
                                 1);
            }

            g.setColour(theme::Colours::text().withAlpha(0.62f));
            g.setFont(10.8f);
            g.drawText("Pan", panLabelBounds, juce::Justification::centred);
            g.drawText("Send", sendLabelBounds, juce::Justification::centred);
            g.setFont(9.8f);
            g.drawText("Tap", sendTapLabelBounds, juce::Justification::centred);
            g.drawText("Send Bus", sendBusLabelBounds, juce::Justification::centred);
            g.drawText("Output", outputRouteBounds, juce::Justification::centred);

            if (!meterBarBounds.isEmpty())
            {
                const auto meterRect = meterBarBounds.toFloat();
                g.setColour(juce::Colours::black.withAlpha(0.52f));
                g.fillRoundedRectangle(meterRect, 2.8f);

                const auto toY = [&](float linear)
                {
                    const float safe = juce::jmax(0.000001f, linear);
                    const float dB = juce::jlimit(-60.0f, 6.0f, juce::Decibels::gainToDecibels(safe, -60.0f));
                    const float normal = (dB + 60.0f) / 60.0f;
                    return meterRect.getBottom() - (meterRect.getHeight() * normal);
                };

                const float rmsY = toY(track.getMeterRmsLevel());
                const float peakY = toY(track.getMeterPeakLevel());
                const float holdY = toY(meterHoldDisplay);

                juce::ColourGradient meterGrad(juce::Colour::fromRGB(72, 192, 123),
                                               meterRect.getBottomLeft(),
                                               juce::Colour::fromRGB(250, 88, 66),
                                               meterRect.getTopLeft(),
                                               false);
                meterGrad.addColour(0.70, juce::Colour::fromRGB(246, 210, 92));
                g.setGradientFill(meterGrad);
                g.fillRoundedRectangle(meterRect.withY(peakY).withHeight(juce::jmax(2.0f, meterRect.getBottom() - peakY)), 2.4f);
                g.setColour(juce::Colour::fromRGB(82, 146, 220).withAlpha(0.45f));
                g.fillRoundedRectangle(meterRect.withY(rmsY), 2.2f);

                g.setColour(juce::Colours::white.withAlpha(0.90f));
                g.drawLine(meterRect.getX() + 1.0f, holdY, meterRect.getRight() - 1.0f, holdY, 1.2f);

                if (track.isMeterClipping())
                {
                    g.setColour(juce::Colour::fromRGB(255, 78, 78).withAlpha(0.96f));
                    g.fillEllipse(meterRect.getCentreX() - 4.0f, meterRect.getY() - 10.0f, 8.0f, 8.0f);
                }

                if (meterDiagnosticHoldFrames > 0)
                {
                    g.setColour(juce::Colour::fromRGB(255, 190, 64).withAlpha(0.95f));
                    g.fillRoundedRectangle(meterRect.getX() - 2.0f, meterRect.getY() - 18.0f, 12.0f, 6.0f, 2.0f);
                }

                if (!meterScaleBounds.isEmpty())
                {
                    const std::array<float, 5> marks { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f };
                    g.setFont(8.5f);
                    for (const auto mark : marks)
                    {
                        const float normal = (mark + 60.0f) / 60.0f;
                        const float y = meterRect.getBottom() - (meterRect.getHeight() * normal);
                        g.setColour(juce::Colours::white.withAlpha(mark == 0.0f ? 0.72f : 0.35f));
                        g.drawHorizontalLine(juce::roundToInt(y), meterRect.getX() - 2.0f, meterRect.getX());
                        g.drawText(juce::String(static_cast<int>(mark)),
                                   meterScaleBounds.getX(),
                                   juce::roundToInt(y) - 5,
                                   meterScaleBounds.getWidth(),
                                   10,
                                   juce::Justification::centredRight);
                    }
                }
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(6, 5);
            const int availableHeight = juce::jmax(180, r.getHeight());
            const int topNameHeight = juce::jlimit(22, 34, juce::roundToInt(static_cast<float>(availableHeight) * 0.12f));
            const int pluginTextHeight = juce::jlimit(16, 24, juce::roundToInt(static_cast<float>(availableHeight) * 0.08f));
            const int insertRowHeight = juce::jlimit(22, 34, juce::roundToInt(static_cast<float>(availableHeight) * 0.10f));
            const int gainLabelHeight = juce::jlimit(22, 30, juce::roundToInt(static_cast<float>(availableHeight) * 0.08f));

            int flexibleHeight = r.getHeight() - topNameHeight - pluginTextHeight - insertRowHeight - gainLabelHeight - 6;
            if (flexibleHeight < 110)
                flexibleHeight = 110;

            const int buttonAreaHeight = juce::jlimit(44, 86, flexibleHeight / 3);
            int knobAreaHeight = flexibleHeight - buttonAreaHeight;
            if (knobAreaHeight < 84)
            {
                knobAreaHeight = 84;
            }

            trackName.setBounds(r.removeFromTop(topNameHeight));
            pluginName.setBounds(r.removeFromTop(pluginTextHeight));
            r.removeFromTop(2);

            auto insertRow = r.removeFromBottom(insertRowHeight);
            instrumentButton.setBounds(insertRow.removeFromLeft(juce::jlimit(46, 62, insertRow.getWidth() / 4)).reduced(1));
            const int eqWidth = juce::jmax(34, juce::jmin(56, insertRow.getWidth() / 5));
            const int insertWidth = juce::jmax(24,
                                               (juce::jmax(0, insertRow.getWidth() - eqWidth - 2))
                                               / juce::jmax(1, insertButtonCount));
            for (int i = 0; i < insertButtonCount; ++i)
                insertButtons[static_cast<size_t>(i)].setBounds(insertRow.removeFromLeft(insertWidth).reduced(1));
            eqButton.setBounds(insertRow.removeFromLeft(eqWidth).reduced(1));

            auto btnArea = r.removeFromBottom(buttonAreaHeight);
            auto btnRowTop = btnArea.removeFromTop(juce::jmax(20, buttonAreaHeight / 2));
            auto btnRowBottom = btnArea;
            muteBtn.setBounds(btnRowTop.removeFromLeft(btnRowTop.getWidth() / 2).reduced(1));
            soloBtn.setBounds(btnRowTop.reduced(1));
            armBtn.setBounds(btnRowBottom.removeFromLeft(btnRowBottom.getWidth() / 2).reduced(1));
            monitorBtn.setBounds(btnRowBottom.reduced(1));

            auto knobArea = r.removeFromBottom(knobAreaHeight);
            auto knobLabelRow = knobArea.removeFromTop(14);
            panLabelBounds = knobLabelRow.removeFromLeft(knobLabelRow.getWidth() / 2).reduced(1, 0);
            sendLabelBounds = knobLabelRow.reduced(1, 0);
            auto knobsRow = knobArea.removeFromTop(juce::jlimit(40, 90, knobArea.getHeight() - 62));
            auto panBox = knobsRow.removeFromLeft(knobsRow.getWidth() / 2).reduced(2);
            auto sendBox = knobsRow.reduced(2);
            const int panDiameter = juce::jlimit(32, 74, juce::jmin(panBox.getWidth(), panBox.getHeight()) - 2);
            const int sendDiameter = juce::jlimit(32, 74, juce::jmin(sendBox.getWidth(), sendBox.getHeight()) - 2);
            panKnob.setBounds(panBox.withSizeKeepingCentre(panDiameter, panDiameter));
            sendKnob.setBounds(sendBox.withSizeKeepingCentre(sendDiameter, sendDiameter));

            auto routeLabelRow = knobArea.removeFromTop(12);
            sendTapLabelBounds = routeLabelRow.removeFromLeft(routeLabelRow.getWidth() / 2).reduced(2, 0);
            sendBusLabelBounds = routeLabelRow.reduced(2, 0);
            auto routeRow = knobArea.removeFromTop(22);
            sendTapBox.setBounds(routeRow.removeFromLeft(routeRow.getWidth() / 2).reduced(1));
            sendTargetBox.setBounds(routeRow.reduced(1));
            outputRouteBounds = knobArea.removeFromTop(12).reduced(2, 0);
            outputTargetBox.setBounds(knobArea.removeFromTop(22).reduced(1));

            gainValueLabel.setBounds(r.removeFromBottom(gainLabelHeight));
            r.removeFromBottom(2);

            const int meterWidth = juce::jlimit(20, 42, juce::jmax(20, r.getWidth() / 5));
            const int meterScaleWidth = juce::jlimit(18, 32, juce::jmax(18, r.getWidth() / 6));
            auto meterStrip = r.removeFromRight(meterWidth);
            meterScaleBounds = r.removeFromRight(meterScaleWidth);
            meterBarBounds = meterStrip.reduced(2, 4);
            fader.setBounds(r.reduced(4, 2));
            gainValueLabel.toFront(false);
        }
        
        void timerCallback() override
        {
            fader.setValue(track.getVolume(), juce::dontSendNotification);
            panKnob.setValue(track.getPan(), juce::dontSendNotification);
            sendKnob.setValue(track.getSendLevel(), juce::dontSendNotification);
            muteBtn.setToggleState(track.isMuted(), juce::dontSendNotification);
            soloBtn.setToggleState(track.isSolo(), juce::dontSendNotification);
            armBtn.setToggleState(track.isArmed(), juce::dontSendNotification);
            monitorBtn.setToggleState(track.isInputMonitoringEnabled(), juce::dontSendNotification);
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
                                                          : theme::Colours::accent().withAlpha(0.92f))
                                     : juce::Colours::white.withAlpha(pluginNameHovered ? 0.95f : 0.72f));
            const bool hasInstrument = track.hasPluginInSlot(Track::instrumentSlotIndex);
            const bool hasInstrumentEditor = track.hasInstrumentPlugin();
            instrumentButton.setButtonText(compactInstrumentLabel(track.getPluginNameForSlot(Track::instrumentSlotIndex)));
            instrumentButton.setColour(juce::TextButton::buttonColourId,
                                       hasInstrument ? theme::Colours::accent().withAlpha(0.34f)
                                                     : juce::Colours::black.withAlpha(0.26f));
            instrumentButton.setColour(juce::TextButton::textColourOffId,
                                       hasInstrument ? juce::Colours::white
                                                     : juce::Colours::white.withAlpha(0.68f));
            const auto instrumentName = track.getPluginNameForSlot(Track::instrumentSlotIndex);
            instrumentButton.setTooltip("Instrument: " + (instrumentName.isNotEmpty() ? instrumentName : juce::String("None"))
                                        + (hasInstrumentEditor ? ". Left-click open, right-click load/change."
                                                               : ". Left-click load, right-click load/change."));
            const bool eqEnabled = track.isEqEnabled();
            eqButton.setColour(juce::TextButton::buttonColourId,
                               eqEnabled ? theme::Colours::accent().withAlpha(0.34f)
                                         : juce::Colours::black.withAlpha(0.26f));
            eqButton.setColour(juce::TextButton::textColourOffId,
                               eqEnabled ? juce::Colours::white
                                         : juce::Colours::white.withAlpha(0.68f));
            eqButton.setTooltip("Track EQ: "
                                + juce::String(eqEnabled ? "On" : "Off")
                                + " | L " + juce::String(track.getEqLowGainDb(), 1) + " dB"
                                + " M " + juce::String(track.getEqMidGainDb(), 1) + " dB"
                                + " H " + juce::String(track.getEqHighGainDb(), 1) + " dB");
            for (int i = 0; i < insertButtonCount; ++i)
            {
                const bool loaded = track.hasPluginInSlot(i);
                insertButtons[static_cast<size_t>(i)].setColour(juce::TextButton::buttonColourId,
                                                                loaded ? theme::Colours::accent().withAlpha(0.32f)
                                                                       : juce::Colours::black.withAlpha(0.26f));
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
            sendKnob.setTooltip("Aux send ("
                                + getSendModeLongLabel(track.getSendTapMode())
                                + ", Bus " + juce::String(track.getSendTargetBus() + 1)
                                + ") | Output "
                                + getOutputRouteShortLabel(track.getOutputTargetType(), track.getOutputTargetBus())
                                + ". Double-click to reset.");
            sendTapBox.setSelectedId(track.getSendTapMode() == Track::SendTapMode::PreFader
                                         ? 1
                                         : (track.getSendTapMode() == Track::SendTapMode::PostPan ? 3 : 2),
                                     juce::dontSendNotification);
            sendTargetBox.setSelectedId(track.getSendTargetBus() + 1, juce::dontSendNotification);
            outputTargetBox.setSelectedId(track.getOutputTargetType() == Track::OutputTargetType::Master
                                              ? 1
                                              : (track.getOutputTargetBus() + 2),
                                          juce::dontSendNotification);
            const int inputPair = track.getInputSourcePair();
            const juce::String monitorTap = track.getMonitorTapMode() == Track::MonitorTapMode::PreInserts
                ? "Pre-insert"
                : "Post-insert";
            const juce::String inputLabel = inputPair < 0
                ? juce::String("Auto")
                : (juce::String(inputPair * 2 + 1) + "/" + juce::String(inputPair * 2 + 2));
            monitorBtn.setTooltip("Input monitor: " + inputLabel
                                  + " (" + monitorTap + ", gain " + juce::String(track.getInputMonitorGain(), 2) + ").");

            const float gain = juce::jmax(0.000001f, static_cast<float>(fader.getValue()));
            const float dB = juce::Decibels::gainToDecibels(gain, -60.0f);
            juce::String gainText = dB <= -59.9f ? "-inf dB"
                                                 : juce::String(dB > 0.0f ? "+" : "") + juce::String(dB, 1) + " dB";
            gainValueLabel.setText(gainText, juce::dontSendNotification);

            const float peak = track.getMeterPeakLevel();
            meterHoldDisplay = juce::jmax(peak, meterHoldDisplay * 0.94f);

            const float outputPeak = track.getPostFaderOutputPeak();
            if (outputPeak > 0.02f && peak < 0.0015f)
                meterDiagnosticHoldFrames = 24;
            else
                meterDiagnosticHoldFrames = juce::jmax(0, meterDiagnosticHoldFrames - 1);

            gainValueLabel.setColour(juce::Label::textColourId,
                                     meterDiagnosticHoldFrames > 0
                                         ? juce::Colour::fromRGB(255, 190, 64)
                                         : theme::Colours::text().withAlpha(0.86f));
            repaint();
        }

    private:
        static juce::String getSendModeShortLabel(Track::SendTapMode mode)
        {
            switch (mode)
            {
                case Track::SendTapMode::PreFader: return "Pre";
                case Track::SendTapMode::PostPan: return "Post-Pan";
                case Track::SendTapMode::PostFader:
                default: return "Post";
            }
        }

        static juce::String getSendModeLongLabel(Track::SendTapMode mode)
        {
            switch (mode)
            {
                case Track::SendTapMode::PreFader: return "Pre-fader";
                case Track::SendTapMode::PostPan: return "Post-pan";
                case Track::SendTapMode::PostFader:
                default: return "Post-fader";
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
            if (label.length() > 8)
                label = label.substring(0, 7) + "...";
            return label.toUpperCase();
        }

        static juce::String compactSlotLabel(int slotIndex, const juce::String& rawName)
        {
            auto label = rawName.trim();
            if (label.isEmpty())
                return "I" + juce::String(slotIndex + 1);
            if (label.length() > 8)
                label = label.substring(0, 7) + "...";
            return label;
        }

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

        bool isOnReorderHandle(juce::Point<float> localPos) const
        {
            const auto r = getLocalBounds().toFloat();
            const auto handleArea = juce::Rectangle<float>(r.getRight() - 18.0f, 4.0f, 14.0f, 16.0f);
            return handleArea.contains(localPos);
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
        juce::Slider fader, panKnob, sendKnob;
        juce::ComboBox sendTapBox, sendTargetBox, outputTargetBox;
        juce::Label trackName, pluginName, gainValueLabel;
        juce::TextButton muteBtn, soloBtn, armBtn, monitorBtn;
        juce::String lastTrackName;
        juce::String lastPluginSummary;
        juce::Rectangle<int> meterBarBounds;
        juce::Rectangle<int> meterScaleBounds;
        juce::Rectangle<int> panLabelBounds;
        juce::Rectangle<int> sendLabelBounds;
        juce::Rectangle<int> sendTapLabelBounds;
        juce::Rectangle<int> sendBusLabelBounds;
        juce::Rectangle<int> sendRouteBounds;
        juce::Rectangle<int> outputRouteBounds;
        float meterHoldDisplay = 0.0f;
        int meterDiagnosticHoldFrames = 0;
    };

    class Mixer : public juce::Component
    {
    public:
        std::function<void(int)> onTrackSelected;
        std::function<void(int)> onTrackStateChanged;
        std::function<void(int)> onTrackRenameRequested;
        std::function<void(int)> onTrackDuplicateRequested;
        std::function<void(int)> onTrackDeleteRequested;
        std::function<void(int)> onTrackMoveUpRequested;
        std::function<void(int)> onTrackMoveDownRequested;
        std::function<void(int)> onTrackOpenChannelRackRequested;
        std::function<void(int)> onTrackOpenInspectorRequested;
        std::function<void(int)> onTrackOpenEqRequested;
        std::function<void(int, int)> onReorderTracks;
        std::function<void(int, juce::Component*, int)> onTrackPluginMenuRequested;
        std::function<void(int, int)> onTrackPluginEditorRequested;
        std::function<void(int, AutomationTarget, bool)> onTrackAutomationTouch;
        std::function<void(int, AutomationTarget, AutomationMode)> onTrackSetAutomationMode;
        std::function<AutomationMode(int, AutomationTarget)> getTrackAutomationMode;
        std::function<void(int)> onAuxClicked;
        std::function<void(juce::Component*, int)> onAuxContextMenuRequested;

        void setAuxMeterLevels(const std::array<float, Track::maxSendBuses>& levels)
        {
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
                auxMeterLevelRt[static_cast<size_t>(bus)].store(juce::jlimit(0.0f, 1.0f, levels[static_cast<size_t>(bus)]),
                                                                std::memory_order_relaxed);
        }

        void setAuxEnabled(bool enabled)
        {
            auxEnabledRt.store(enabled, std::memory_order_relaxed);
        }

        void addTrack(Track* track, int index)
        {
            auto* channel = channels.add(new MixerChannel(*track));
            configureChannelCallbacks(channel, index);
            addAndMakeVisible(channel);
            resized();
        }

        void rebuildFromTracks(const juce::OwnedArray<Track>& tracks)
        {
            channels.clear();
            for (int i = 0; i < tracks.size(); ++i)
            {
                auto* channel = channels.add(new MixerChannel(*tracks[i]));
                configureChannelCallbacks(channel, i);
                addAndMakeVisible(channel);
            }
            resized();
        }

        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            const bool horizontalGesture = std::abs(wheel.deltaX) > std::abs(wheel.deltaY);
            const bool wantsScroll = e.mods.isShiftDown() || horizontalGesture;
            if (!wantsScroll)
            {
                userSizedChannels = true;
                const float dominantDelta = std::abs(wheel.deltaY) > 0.0001f ? wheel.deltaY : wheel.deltaX;
                channelWidth = juce::jlimit(280, 500, channelWidth + static_cast<int>(std::round(dominantDelta * 20.0f)));
                resized();
                repaint();
                return;
            }

            const float delta = (std::abs(wheel.deltaX) > 0.0001f ? wheel.deltaX : wheel.deltaY) * 140.0f;
            scrollX = juce::jmax(0.0f, scrollX - delta);
            resized();
            repaint();
        }

        void mouseDoubleClick(const juce::MouseEvent& e) override
        {
            if (getAuxBusHitIndex(e.getPosition()) >= 0)
                return;
            userSizedChannels = false;
            resized();
            repaint();
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const int wireChannelIndex = getSendWireHitIndex(e.position, 10.0f);
            if (wireChannelIndex >= 0 && juce::isPositiveAndBelow(wireChannelIndex, channels.size()))
            {
                selectTrack(wireChannelIndex);
                if (e.mods.isPopupMenu())
                    showSendWireMenu(wireChannelIndex, this);
                return;
            }

            const int auxBus = getAuxBusHitIndex(e.getPosition());
            if (auxBus < 0)
                return;

            if (e.mods.isPopupMenu())
            {
                if (onAuxContextMenuRequested)
                    onAuxContextMenuRequested(this, auxBus);
                return;
            }

            if (onAuxClicked)
                onAuxClicked(auxBus);
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const int auxBus = getAuxBusHitIndex(e.getPosition());
            if (auxBus != hoveredAuxBusIndex)
            {
                hoveredAuxBusIndex = auxBus;
                repaint();
            }

            const int wireChannelIndex = getSendWireHitIndex(e.position, 9.0f);
            if (wireChannelIndex != hoveredWireChannelIndex)
            {
                hoveredWireChannelIndex = wireChannelIndex;
                repaint();
            }

            setMouseCursor((auxBus >= 0 || wireChannelIndex >= 0)
                               ? juce::MouseCursor::PointingHandCursor
                               : juce::MouseCursor::NormalCursor);
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            if (hoveredAuxBusIndex >= 0)
            {
                hoveredAuxBusIndex = -1;
                repaint();
            }

            if (hoveredWireChannelIndex >= 0)
            {
                hoveredWireChannelIndex = -1;
                repaint();
            }

            setMouseCursor(juce::MouseCursor::NormalCursor);
        }

        void paint(juce::Graphics& g) override
        {
            g.setGradientFill(juce::ColourGradient(theme::Colours::darker().brighter(0.04f), 0.0f, 0.0f,
                                                   theme::Colours::darker().darker(0.12f), 0.0f, static_cast<float>(getHeight()),
                                                   false));
            g.fillAll();
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawLine(0.0f, 0.5f, static_cast<float>(getWidth()), 0.5f, 1.0f);
            g.setColour(theme::Colours::text().withAlpha(0.62f));
            g.setFont(11.0f);
            g.drawText("Mixer: wheel resizes strips, Shift+wheel pans, click send wires for routing menus.",
                       8, 2, getWidth() - 16, 14, juce::Justification::centredLeft);
        }

        void paintOverChildren(juce::Graphics& g) override
        {
            bool anyAuxVisible = false;
            for (const auto& bounds : auxStripBounds)
            {
                if (!bounds.isEmpty())
                {
                    anyAuxVisible = true;
                    break;
                }
            }
            if (!anyAuxVisible)
                return;

            const bool auxEnabled = auxEnabledRt.load(std::memory_order_relaxed);
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                const auto stripBounds = auxStripBounds[static_cast<size_t>(bus)];
                if (stripBounds.isEmpty())
                    continue;

                const bool auxHover = hoveredAuxBusIndex == bus;
                const float auxMeter = auxMeterLevelRt[static_cast<size_t>(bus)].load(std::memory_order_relaxed);
                const auto auxNode = getAuxNodeForBus(bus);

                g.setColour(theme::Colours::panel().withAlpha(0.78f));
                g.fillRoundedRectangle(stripBounds.toFloat().reduced(4.0f), 6.0f);
                g.setColour(theme::Colours::text().withAlpha(0.75f));
                g.setFont(10.0f);
                g.drawFittedText("AUX " + juce::String(bus + 1) + "\nRETURN",
                                 stripBounds.reduced(10, 10),
                                 juce::Justification::centredTop,
                                 2);
                g.setColour(theme::Colours::text().withAlpha(auxHover ? 0.95f : 0.65f));
                g.setFont(8.5f);
                g.drawText("L-click: Toggle\nR-click: Options",
                           stripBounds.reduced(8, 48),
                           juce::Justification::centredBottom,
                           true);
                g.setColour(theme::Colours::text().withAlpha(0.62f));
                g.drawText("BUS " + juce::String(bus + 1),
                           stripBounds.getX() + 8,
                           stripBounds.getBottom() - 20,
                           stripBounds.getWidth() - 16,
                           12,
                           juce::Justification::centred);

                g.setColour((auxEnabled ? theme::Colours::accent() : theme::Colours::text()).withAlpha(0.85f));
                g.fillEllipse(auxNode.x - 5.0f, auxNode.y - 5.0f, 10.0f, 10.0f);

                auto meterRect = stripBounds.reduced(16, 36).removeFromBottom(44).toFloat();
                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.fillRoundedRectangle(meterRect, 3.0f);
                const float meterFillWidth = meterRect.getWidth() * juce::jlimit(0.0f, 1.0f, std::sqrt(auxMeter));
                if (meterFillWidth > 0.5f)
                {
                    auto fill = meterRect.withWidth(meterFillWidth);
                    g.setColour((auxEnabled ? theme::Colours::accent() : theme::Colours::text().withAlpha(0.4f)).withAlpha(0.92f));
                    g.fillRoundedRectangle(fill, 3.0f);
                }
                g.setColour(theme::Colours::text().withAlpha(0.65f));
                g.setFont(8.5f);
                const juce::Rectangle<int> meterLabelBounds(
                    juce::roundToInt(meterRect.getX()),
                    juce::roundToInt(meterRect.getY()) - 12,
                    juce::roundToInt(meterRect.getWidth()),
                    11);
                g.drawText(auxEnabled ? "ON" : "OFF",
                           meterLabelBounds,
                           juce::Justification::centred,
                           false);
            }

            for (int channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
            {
                auto* channel = channels[channelIndex];
                if (channel == nullptr)
                    continue;

                const float send = channel->getSendVisualLevel();
                if (send <= 0.001f)
                    continue;

                const int bus = juce::jlimit(0, Track::maxSendBuses - 1, channel->getSendTargetBus());
                const auto auxNode = getAuxNodeForBus(bus);
                const auto start = channel->getSendKnobCenterInParent();
                juce::Path wire;
                wire.startNewSubPath(start);
                const float controlDx = juce::jmax(18.0f, (auxNode.x - start.x) * 0.38f);
                wire.cubicTo(start.x + controlDx, start.y,
                             auxNode.x - controlDx, auxNode.y,
                             auxNode.x, auxNode.y);

                const bool selectedWire = channel->isSelected();
                const bool hoveredWire = channelIndex == hoveredWireChannelIndex;
                const float alpha = juce::jlimit(0.15f, 0.98f,
                                                 0.18f + send * 0.75f
                                                 + (selectedWire ? 0.16f : 0.0f)
                                                 + (hoveredWire ? 0.20f : 0.0f));
                g.setColour(theme::Colours::accent().withAlpha(alpha));
                g.strokePath(wire, juce::PathStrokeType(1.0f + send * 2.2f
                                                        + (selectedWire ? 0.9f : 0.0f)
                                                        + (hoveredWire ? 1.0f : 0.0f)));

                g.setColour(juce::Colours::white.withAlpha(alpha));
                g.fillEllipse(start.x - 2.6f, start.y - 2.6f, 5.2f, 5.2f);

                juce::Path arrow;
                arrow.startNewSubPath(auxNode.x, auxNode.y);
                arrow.lineTo(auxNode.x - 6.5f, auxNode.y - 3.5f);
                arrow.lineTo(auxNode.x - 6.5f, auxNode.y + 3.5f);
                arrow.closeSubPath();
                g.setColour(theme::Colours::accent().withAlpha(alpha));
                g.fillPath(arrow);

                if (send >= 0.05f)
                {
                    const int sendPercent = juce::roundToInt(send * 100.0f);
                    const float labelX = juce::jmin(auxNode.x - 36.0f, start.x + (auxNode.x - start.x) * 0.45f);
                    const juce::Rectangle<float> badge(labelX, start.y - 8.0f, 54.0f, 14.0f);
                    g.setColour(theme::Colours::panel().withAlpha(0.90f));
                    g.fillRoundedRectangle(badge, 3.0f);
                    g.setColour(theme::Colours::accent().withAlpha(0.92f));
                    g.drawRoundedRectangle(badge, 3.0f, (selectedWire || hoveredWire) ? 1.6f : 1.0f);
                    g.setColour(theme::Colours::text().withAlpha(0.94f));
                    g.setFont(9.0f);
                    const juce::Rectangle<int> badgeTextBounds(
                        juce::roundToInt(badge.getX()),
                        juce::roundToInt(badge.getY()),
                        juce::roundToInt(badge.getWidth()),
                        juce::roundToInt(badge.getHeight()));
                    juce::String modeLabel = "Post";
                    switch (channel->getSendTapMode())
                    {
                        case Track::SendTapMode::PreFader: modeLabel = "Pre"; break;
                        case Track::SendTapMode::PostPan: modeLabel = "Pan"; break;
                        case Track::SendTapMode::PostFader: modeLabel = "Post"; break;
                        default: break;
                    }
                    g.drawFittedText("B" + juce::String(bus + 1) + " " + modeLabel + " " + juce::String(sendPercent) + "%",
                                     badgeTextBounds,
                                     juce::Justification::centred,
                                     1);
                }
            }

            if (trackReorderDragging && reorderTargetTrack >= 0)
            {
                const float indicatorX = static_cast<float>(channelStartX
                                                            + reorderTargetTrack * (channelWidth + channelSpacing)
                                                            - static_cast<int>(std::round(scrollX)));
                g.setColour(theme::Colours::accent().withAlpha(0.95f));
                g.fillRect(indicatorX - 1.5f, 18.0f, 3.0f, static_cast<float>(getHeight() - 20));
            }
        }

        void selectTrack(int index)
        {
            for (int i = 0; i < channels.size(); ++i)
                channels[i]->setSelected(i == index);
            if (onTrackSelected) onTrackSelected(index);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            r.removeFromTop(18);
            auto channelArea = r;
            for (auto& bounds : auxStripBounds)
                bounds = {};
            const int desiredAuxWidth = (auxStripWidth * Track::maxSendBuses)
                                      + (auxStripSpacing * juce::jmax(0, Track::maxSendBuses - 1));
            const int totalAuxWidth = juce::jlimit(0,
                                                   juce::jmax(0, channelArea.getWidth() - 24),
                                                   desiredAuxWidth);
            auto auxArea = channelArea.removeFromRight(totalAuxWidth);
            const int stripWidth = Track::maxSendBuses > 0
                ? juce::jmax(24, (auxArea.getWidth() - (auxStripSpacing * juce::jmax(0, Track::maxSendBuses - 1)))
                                    / Track::maxSendBuses)
                : 0;
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                auxStripBounds[static_cast<size_t>(bus)] = auxArea.removeFromLeft(stripWidth);
                if (bus < Track::maxSendBuses - 1)
                    auxArea.removeFromLeft(auxStripSpacing);
            }
            channelStartX = channelArea.getX();

            if (!userSizedChannels && channels.size() > 0)
            {
                const int visibleChannels = juce::jlimit(1, 6, channels.size());
                const int availableWidth = channelArea.getWidth() - ((visibleChannels - 1) * channelSpacing);
                if (availableWidth > 0)
                    channelWidth = juce::jlimit(280, 500, availableWidth / visibleChannels);
            }

            const int totalWidth = juce::jmax(0, channels.size() * (channelWidth + channelSpacing) - channelSpacing);
            const float maxScroll = static_cast<float>(juce::jmax(0, totalWidth - channelArea.getWidth()));
            scrollX = juce::jlimit(0.0f, maxScroll, scrollX);

            int x = channelArea.getX() - static_cast<int>(std::round(scrollX));
            for (auto* c : channels)
            {
                c->setBounds(x, channelArea.getY(), channelWidth, channelArea.getHeight());
                x += channelWidth + channelSpacing;
            }
        }

    private:
        juce::Point<float> getAuxNodeForBus(int busIndex) const
        {
            const int clamped = juce::jlimit(0, Track::maxSendBuses - 1, busIndex);
            const auto bounds = auxStripBounds[static_cast<size_t>(clamped)];
            if (bounds.isEmpty())
                return juce::Point<float>(static_cast<float>(getWidth() - 8),
                                          static_cast<float>(getHeight() * 0.5f));
            return juce::Point<float>(static_cast<float>(bounds.getX() + 12),
                                      static_cast<float>(bounds.getCentreY()));
        }

        int getAuxBusHitIndex(juce::Point<int> localPos) const
        {
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                if (auxStripBounds[static_cast<size_t>(bus)].contains(localPos))
                    return bus;
            }
            return -1;
        }

        int getSendWireHitIndex(juce::Point<float> localPos, float maxDistance) const
        {
            bool hasAux = false;
            for (const auto& bounds : auxStripBounds)
            {
                if (!bounds.isEmpty())
                {
                    hasAux = true;
                    break;
                }
            }
            if (!hasAux)
                return -1;

            int nearestIndex = -1;
            float nearestDistance = maxDistance;
            for (int i = 0; i < channels.size(); ++i)
            {
                auto* channel = channels[i];
                if (channel == nullptr || channel->getSendVisualLevel() <= 0.001f)
                    continue;

                const auto auxNode = getAuxNodeForBus(channel->getSendTargetBus());
                const auto start = channel->getSendKnobCenterInParent();
                const float minX = juce::jmin(start.x, auxNode.x) - 8.0f;
                const float maxX = juce::jmax(start.x, auxNode.x) + 8.0f;
                const float minY = juce::jmin(start.y, auxNode.y) - 16.0f;
                const float maxY = juce::jmax(start.y, auxNode.y) + 16.0f;
                if (localPos.x < minX || localPos.x > maxX || localPos.y < minY || localPos.y > maxY)
                    continue;

                const juce::Line<float> wireLine(start, auxNode);
                juce::Point<float> pointOnLine;
                const float distance = wireLine.getDistanceFromPoint(localPos, pointOnLine);
                if (distance <= nearestDistance)
                {
                    nearestDistance = distance;
                    nearestIndex = i;
                }
            }

            return nearestIndex;
        }

        void showSendWireMenu(int channelIndex, juce::Component* target)
        {
            if (!juce::isPositiveAndBelow(channelIndex, channels.size()))
                return;

            auto* channel = channels[channelIndex];
            if (channel == nullptr)
                return;

            const float currentSend = channel->getSendLevel();
            juce::PopupMenu menu;
            menu.addSectionHeader("Send: " + channel->getTrackName());
            menu.addItem(200, "0%", true, currentSend <= 0.001f);
            menu.addItem(201, "25%", true, std::abs(currentSend - 0.25f) < 0.01f);
            menu.addItem(202, "50%", true, std::abs(currentSend - 0.50f) < 0.01f);
            menu.addItem(203, "75%", true, std::abs(currentSend - 0.75f) < 0.01f);
            menu.addItem(204, "100%", true, std::abs(currentSend - 1.00f) < 0.01f);
            menu.addSeparator();
            const auto currentTap = channel->getSendTapMode();
            menu.addItem(210, "Pre-Fader", true, currentTap == Track::SendTapMode::PreFader);
            menu.addItem(211, "Post-Fader", true, currentTap == Track::SendTapMode::PostFader);
            menu.addItem(212, "Post-Pan", true, currentTap == Track::SendTapMode::PostPan);
            menu.addSeparator();
            for (int bus = 0; bus < Track::maxSendBuses; ++bus)
            {
                menu.addItem(220 + bus,
                             "Send to Bus " + juce::String(bus + 1),
                             true,
                             channel->getSendTargetBus() == bus);
            }
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target != nullptr ? target : this),
                               [this, channelIndex](int selectedId)
                               {
                                   if (!juce::isPositiveAndBelow(channelIndex, channels.size()))
                                       return;

                                   auto* targetChannel = channels[channelIndex];
                                   if (targetChannel == nullptr)
                                       return;

                                   if (selectedId >= 200 && selectedId <= 204)
                                   {
                                       static constexpr float sendValues[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
                                       targetChannel->setSendLevelFromUi(sendValues[selectedId - 200]);
                                       repaint();
                                   }
                                   else if (selectedId == 210 || selectedId == 211 || selectedId == 212)
                                   {
                                       const auto mode = (selectedId == 210) ? Track::SendTapMode::PreFader
                                                                             : (selectedId == 211 ? Track::SendTapMode::PostFader
                                                                                                  : Track::SendTapMode::PostPan);
                                       targetChannel->setSendTapModeFromUi(mode);
                                       repaint();
                                   }
                                   else if (selectedId >= 220 && selectedId < 220 + Track::maxSendBuses)
                                   {
                                       targetChannel->setSendTargetBusFromUi(selectedId - 220);
                                       repaint();
                                   }
                               });
        }

        int getTrackIndexForPositionX(float x) const
        {
            if (channels.isEmpty())
                return -1;

            const float worldX = x - static_cast<float>(channelStartX) + scrollX;
            const float span = static_cast<float>(channelWidth + channelSpacing);
            const int idx = static_cast<int>(std::floor(worldX / span));
            return juce::jlimit(0, channels.size() - 1, idx);
        }

        void configureChannelCallbacks(MixerChannel* channel, int index)
        {
            channel->onSelect = [this, index] { selectTrack(index); };
            channel->onStateChanged = [this, index]
            {
                if (onTrackStateChanged)
                    onTrackStateChanged(index);
            };
            channel->onRequestPluginMenu = [this, index](juce::Component* target, int slotIndex)
            {
                if (onTrackPluginMenuRequested)
                    onTrackPluginMenuRequested(index, target, slotIndex);
            };
            channel->onRequestOpenPluginEditor = [this, index](int slotIndex)
            {
                if (onTrackPluginEditorRequested)
                    onTrackPluginEditorRequested(index, slotIndex);
            };
            channel->onRequestRenameTrack = [this, index]
            {
                if (onTrackRenameRequested)
                    onTrackRenameRequested(index);
            };
            channel->onRequestDuplicateTrack = [this, index]
            {
                if (onTrackDuplicateRequested)
                    onTrackDuplicateRequested(index);
            };
            channel->onRequestDeleteTrack = [this, index]
            {
                if (onTrackDeleteRequested)
                    onTrackDeleteRequested(index);
            };
            channel->onRequestMoveTrackUp = [this, index]
            {
                if (onTrackMoveUpRequested)
                    onTrackMoveUpRequested(index);
            };
            channel->onRequestMoveTrackDown = [this, index]
            {
                if (onTrackMoveDownRequested)
                    onTrackMoveDownRequested(index);
            };
            channel->onRequestOpenChannelRack = [this, index]
            {
                if (onTrackOpenChannelRackRequested)
                    onTrackOpenChannelRackRequested(index);
            };
            channel->onRequestOpenInspector = [this, index]
            {
                if (onTrackOpenInspectorRequested)
                    onTrackOpenInspectorRequested(index);
            };
            channel->onRequestOpenEq = [this, index]
            {
                if (onTrackOpenEqRequested)
                    onTrackOpenEqRequested(index);
            };
            channel->onAutomationTouch = [this, index](AutomationTarget target, bool isTouching)
            {
                if (onTrackAutomationTouch)
                    onTrackAutomationTouch(index, target, isTouching);
            };
            channel->onSetAutomationMode = [this, index](AutomationTarget target, AutomationMode mode)
            {
                if (onTrackSetAutomationMode)
                    onTrackSetAutomationMode(index, target, mode);
            };
            channel->getAutomationMode = [this, index](AutomationTarget target)
            {
                if (getTrackAutomationMode)
                    return getTrackAutomationMode(index, target);
                return AutomationMode::Read;
            };
            channel->onReorderDragBegin = [this, index]
            {
                trackReorderDragging = true;
                reorderSourceTrack = index;
                reorderTargetTrack = index;
                repaint();
            };
            channel->onReorderDragMove = [this, index](float parentX)
            {
                if (!trackReorderDragging || reorderSourceTrack != index)
                    return;
                reorderTargetTrack = getTrackIndexForPositionX(parentX);
                repaint();
            };
            channel->onReorderDragEnd = [this, index]
            {
                if (!trackReorderDragging || reorderSourceTrack != index)
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
        }

        juce::OwnedArray<MixerChannel> channels;
        int channelWidth = 296;
        float scrollX = 0.0f;
        int channelStartX = 0;
        bool userSizedChannels = false;
        int hoveredAuxBusIndex = -1;
        int hoveredWireChannelIndex = -1;
        bool trackReorderDragging = false;
        int reorderSourceTrack = -1;
        int reorderTargetTrack = -1;
        std::array<std::atomic<float>, static_cast<size_t>(Track::maxSendBuses)> auxMeterLevelRt {};
        std::atomic<bool> auxEnabledRt { true };
        std::array<juce::Rectangle<int>, static_cast<size_t>(Track::maxSendBuses)> auxStripBounds {};
        static constexpr int channelSpacing = 8;
        static constexpr int auxStripWidth = 94;
        static constexpr int auxStripSpacing = 7;
    };
}
