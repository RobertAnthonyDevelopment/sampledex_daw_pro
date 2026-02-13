#pragma once

#include <JuceHeader.h>
#include <optional>
#include <utility>

namespace sampledex
{
    class NormalizeDialog final
    {
    public:
        struct Result
        {
            float targetPeakDb = -1.0f;
            bool removeDc = false;
            bool preserveDynamics = true;
        };

        using Completion = std::function<void(std::optional<Result>)>;

        static void showAsync(double currentPeakDb, Completion completion)
        {
            const double safePeakDb = std::isfinite(currentPeakDb) ? currentPeakDb : -120.0;
            const double suggestedTargetDb = -1.0;
            const double suggestedGainDb = suggestedTargetDb - safePeakDb;

            auto* dialog = new juce::AlertWindow("Normalize Audio Clip",
                                                 "Current peak: " + juce::String(safePeakDb, 2) + " dBFS\n"
                                                 "Estimated gain to -1.0 dBFS: " + juce::String(suggestedGainDb, 2) + " dB\n\n"
                                                 "Choose target and options before apply.",
                                                 juce::AlertWindow::NoIcon);
            dialog->addTextEditor("target_peak_db", juce::String(suggestedTargetDb, 2), "Target peak (dBFS)");
            dialog->addComboBox("preserve_dynamics",
                                juce::StringArray { "Preserve dynamics (gain-only)", "Additional shaping (future)" },
                                "Dynamics");
            dialog->addComboBox("remove_dc",
                                juce::StringArray { "DC offset removal: Off", "DC offset removal: On" },
                                "DC");
            if (auto* preserve = dialog->getComboBoxComponent("preserve_dynamics"))
                preserve->setSelectedId(1, juce::dontSendNotification);
            if (auto* removeDc = dialog->getComboBoxComponent("remove_dc"))
                removeDc->setSelectedId(1, juce::dontSendNotification);
            dialog->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
            dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            dialog->enterModalState(true, juce::ModalCallbackFunction::create(
                [dialog, doneCb = std::move(completion)](int result) mutable
                {
                    std::unique_ptr<juce::AlertWindow> owner(dialog);
                    if (result != 1)
                    {
                        if (doneCb)
                            doneCb(std::nullopt);
                        return;
                    }

                    Result selected;
                    selected.targetPeakDb = static_cast<float>(juce::jlimit(-24.0,
                                                                            0.0,
                                                                            owner->getTextEditorContents("target_peak_db").getDoubleValue()));
                    if (auto* preserve = owner->getComboBoxComponent("preserve_dynamics"))
                        selected.preserveDynamics = preserve->getSelectedId() != 2;
                    if (auto* removeDc = owner->getComboBoxComponent("remove_dc"))
                        selected.removeDc = removeDc->getSelectedId() == 2;

                    if (doneCb)
                        doneCb(selected);
                }));
        }
    };
}
