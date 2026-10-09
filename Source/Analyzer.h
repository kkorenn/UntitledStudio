#pragma once

#include "Theme.h"

// Our own pass-through plugin that taps the audio for the spectrum analyzer / oscilloscope.
// Audio thread only writes into a lock-free FIFO; the UI reads it.
class AnalyzerPlugin : public te::Plugin
{
public:
    AnalyzerPlugin (te::PluginCreationInfo info) : te::Plugin (info) {}
    ~AnalyzerPlugin() override { notifyListenersOfDeletion(); }

    static const char* getPluginName()                  { return "Analyzer"; }
    static inline const char* xmlTypeName = "us_analyzer";

    juce::String getName() const override               { return "Analyzer"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Anlz"; }
    juce::String getSelectableDescription() override    { return "Analyzer"; }
    bool needsConstantBufferSize() override             { return false; }

    void initialise (const te::PluginInitialisationInfo& info) override { rate = info.sampleRate; }
    void deinitialise() override {}

    int getNumOutputChannelsGivenInputs (int numInputs) override { return juce::jmin (numInputs, 2); }
    BusLayout getBusses() const override                        { return BusLayout::singleStereoInOut(); }

    void applyToBuffer (const te::PluginRenderContext& rc) override
    {
        if (rc.destBuffer == nullptr || rc.bufferNumSamples <= 0)
            return;

        // Mono mix into the FIFO (drops samples if the UI isn't reading: fine for a display).
        const auto scope = fifo.write (juce::jmin (rc.bufferNumSamples, fifo.getFreeSpace()));
        const int channels = juce::jmin (2, rc.destBuffer->getNumChannels());
        auto copy = [&] (int start, int size, int offset)
        {
            for (int i = 0; i < size; ++i)
            {
                float sum = 0.0f;
                for (int ch = 0; ch < channels; ++ch)
                    sum += rc.destBuffer->getSample (ch, rc.bufferStartSample + offset + i);
                ring[(size_t) (start + i)] = channels > 0 ? sum / (float) channels : 0.0f;
            }
        };
        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }

    // UI thread: pulls up to `max` new samples.
    int read (float* dest, int max)
    {
        const auto scope = fifo.read (juce::jmin (max, fifo.getNumReady()));
        std::copy_n (ring.begin() + scope.startIndex1, scope.blockSize1, dest);
        std::copy_n (ring.begin() + scope.startIndex2, scope.blockSize2, dest + scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }

    double getSampleRate() const { return rate; }

private:
    static constexpr int capacity = 1 << 15;
    juce::AbstractFifo fifo { capacity };
    std::array<float, capacity> ring {};
    double rate = 44100.0;
};

// Spectrum (log frequency) on top, oscilloscope below.
class AnalyzerView : public juce::Component, private juce::Timer
{
public:
    explicit AnalyzerView (AnalyzerPlugin& p) : plugin (&p) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::lcd);
        auto r = getLocalBounds().reduced (6);
        auto spectrumArea = r.removeFromTop (r.getHeight() * 2 / 3).toFloat();
        r.removeFromTop (6);
        auto scopeArea = r.toFloat();

        // Grid: 100 Hz, 1 kHz, 10 kHz
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.setFont (Theme::font (10.0f));
        for (float f : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = spectrumArea.getX() + freqToX (f) * spectrumArea.getWidth();
            g.drawVerticalLine ((int) x, spectrumArea.getY(), spectrumArea.getBottom());
            g.setColour (Theme::textDim);
            g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f), (int) x + 3, (int) spectrumArea.getY(), 30, 12,
                        juce::Justification::left);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
        }

        juce::Path spectrum;
        const double sr = plugin->getSampleRate();
        for (int px = 0; px < (int) spectrumArea.getWidth(); ++px)
        {
            const float freq = xToFreq ((float) px / spectrumArea.getWidth());
            const int bin = juce::jlimit (1, fftSize / 2 - 1, (int) (freq / sr * fftSize));
            const float db = juce::Decibels::gainToDecibels (smoothed[(size_t) bin], -90.0f);
            const float y = spectrumArea.getBottom() - juce::jmap (db, -90.0f, 0.0f, 0.0f, spectrumArea.getHeight());
            if (px == 0) spectrum.startNewSubPath (spectrumArea.getX(), y);
            else         spectrum.lineTo (spectrumArea.getX() + (float) px, y);
        }
        auto fill = spectrum;
        fill.lineTo (spectrumArea.getRight(), spectrumArea.getBottom());
        fill.lineTo (spectrumArea.getX(), spectrumArea.getBottom());
        fill.closeSubPath();
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillPath (fill);
        g.setColour (Theme::accent);
        g.strokePath (spectrum, juce::PathStrokeType (1.5f));

        // Oscilloscope
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) scopeArea.getCentreY(), scopeArea.getX(), scopeArea.getRight());
        juce::Path wave;
        for (int i = 0; i < scopeSize; ++i)
        {
            const float x = scopeArea.getX() + scopeArea.getWidth() * (float) i / (float) (scopeSize - 1);
            const float y = scopeArea.getCentreY() - juce::jlimit (-1.0f, 1.0f, history[(size_t) ((historyPos + i) % scopeSize)]) * scopeArea.getHeight() * 0.5f;
            if (i == 0) wave.startNewSubPath (x, y);
            else        wave.lineTo (x, y);
        }
        g.setColour (Theme::lcdText);
        g.strokePath (wave, juce::PathStrokeType (1.2f));
    }

private:
    static constexpr int fftOrder = 11, fftSize = 1 << fftOrder, scopeSize = 1024;

    static float freqToX (float f) { return std::log10 (f / 20.0f) / std::log10 (20000.0f / 20.0f); }
    static float xToFreq (float x) { return 20.0f * std::pow (1000.0f, x); }

    void timerCallback() override
    {
        std::array<float, 4096> incoming;
        const int n = plugin->read (incoming.data(), (int) incoming.size());
        for (int i = 0; i < n; ++i)
        {
            fftInput[(size_t) fftPos] = incoming[(size_t) i];
            fftPos = (fftPos + 1) % fftSize;
            history[(size_t) historyPos] = incoming[(size_t) i];
            historyPos = (historyPos + 1) % scopeSize;
        }

        if (! isShowing()) // keep draining the plugin's buffer, skip the FFT + repaint
            return;

        std::array<float, fftSize * 2> data {};
        for (int i = 0; i < fftSize; ++i)
            data[(size_t) i] = fftInput[(size_t) ((fftPos + i) % fftSize)];
        window.multiplyWithWindowingTable (data.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform (data.data());

        for (int i = 0; i < fftSize / 2; ++i)
        {
            const float mag = data[(size_t) i] / (float) fftSize * 4.0f;
            smoothed[(size_t) i] = juce::jmax (mag, smoothed[(size_t) i] * 0.85f);
        }
        repaint();
    }

    juce::ReferenceCountedObjectPtr<AnalyzerPlugin> plugin;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, fftSize> fftInput {};
    std::array<float, fftSize / 2> smoothed {};
    std::array<float, scopeSize> history {};
    int fftPos = 0, historyPos = 0;
};
