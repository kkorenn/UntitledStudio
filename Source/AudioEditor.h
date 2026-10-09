#pragma once

#include "Studio.h"
#include "Widgets.h"

// Audio editing on whole buffers. The user's files are never overwritten: "Apply" writes a new
// file into ~/Music/Untitled Studio/Edits and the clip / sampler is pointed at it (undoable).
namespace AudioEdit
{
    using Buffer = juce::AudioBuffer<float>;
    using Range = juce::Range<int>;

    inline Range clip (const Buffer& b, Range r)    { return r.getIntersectionWith ({ 0, b.getNumSamples() }); }

    inline void reverse (Buffer& b, Range r)
    {
        r = clip (b, r);
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.reverse (ch, r.getStart(), r.getLength());
    }

    inline void gain (Buffer& b, Range r, float dB)  { r = clip (b, r); b.applyGain (r.getStart(), r.getLength(), juce::Decibels::decibelsToGain (dB)); }
    inline void silence (Buffer& b, Range r)          { r = clip (b, r); b.clear (r.getStart(), r.getLength()); }
    inline void fadeIn (Buffer& b, Range r)           { r = clip (b, r); b.applyGainRamp (r.getStart(), r.getLength(), 0.0f, 1.0f); }
    inline void fadeOut (Buffer& b, Range r)          { r = clip (b, r); b.applyGainRamp (r.getStart(), r.getLength(), 1.0f, 0.0f); }

    inline void normalise (Buffer& b, Range r, float peakDb = -0.3f)
    {
        r = clip (b, r);
        if (const float peak = b.getMagnitude (r.getStart(), r.getLength()); peak > 0.0f)
            b.applyGain (r.getStart(), r.getLength(), juce::Decibels::decibelsToGain (peakDb) / peak);
    }

    inline Buffer copy (const Buffer& b, Range r)
    {
        r = clip (b, r);
        Buffer out (b.getNumChannels(), r.getLength());
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            out.copyFrom (ch, 0, b, ch, r.getStart(), r.getLength());
        return out;
    }

    // Removes r (cut / delete).
    inline void remove (Buffer& b, Range r)
    {
        r = clip (b, r);
        Buffer out (b.getNumChannels(), b.getNumSamples() - r.getLength());
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            out.copyFrom (ch, 0, b, ch, 0, r.getStart());
            out.copyFrom (ch, r.getStart(), b, ch, r.getEnd(), b.getNumSamples() - r.getEnd());
        }
        b = std::move (out);
    }

    // Keeps only r.
    inline void trim (Buffer& b, Range r)   { b = copy (b, r); }

    inline void insert (Buffer& b, int at, const Buffer& source)
    {
        at = juce::jlimit (0, b.getNumSamples(), at);
        Buffer out (b.getNumChannels(), b.getNumSamples() + source.getNumSamples());
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            out.copyFrom (ch, 0, b, ch, 0, at);
            out.copyFrom (ch, at, source, juce::jmin (ch, source.getNumChannels() - 1), 0, source.getNumSamples());
            out.copyFrom (ch, at + source.getNumSamples(), b, ch, at, b.getNumSamples() - at);
        }
        b = std::move (out);
    }

    inline juce::File getEditsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Untitled Studio").getChildFile ("Edits");
    }

    // "Kick.wav" -> Edits/"Kick (edit 1).wav"; "Kick (edit 1).wav" -> "Kick (edit 2).wav".
    inline juce::File nextEditFile (const juce::File& source, const juce::File& folder = getEditsFolder())
    {
        auto base = source.getFileNameWithoutExtension();
        if (base.endsWith (")") && base.contains (" (edit "))
            base = base.upToLastOccurrenceOf (" (edit ", false, false);
        for (int n = 1;; ++n)
            if (auto f = folder.getChildFile (base + " (edit " + juce::String (n) + ").wav"); ! f.exists())
                return f;
    }

    inline bool writeWav (const Buffer& b, double sampleRate, const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;
        juce::WavAudioFormat wav;
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (b.getNumChannels()).withBitsPerSample (24);
        auto writer = wav.createWriterFor (stream, options);
        return writer != nullptr && writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    inline bool read (juce::AudioFormatManager& formats, const juce::File& file, Buffer& b, double& sampleRate)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
            return false;
        b.setSize ((int) juce::jmin (2u, reader->numChannels), (int) reader->lengthInSamples);
        reader->read (&b, 0, b.getNumSamples(), 0, true, b.getNumChannels() > 1);
        sampleRate = reader->sampleRate;
        return true;
    }
}

// Our "Audio editor" window (like FL's sample editor): waveform, drag to select, Ctrl+wheel zoom,
// wheel scroll, double-click selects all. Edits keep an undo history; Apply writes a new file.
class AudioEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void (const juce::File&)> onPreview;  // play a file; an invalid file stops
    std::function<void (const juce::File&)> onApply;    // the edited copy was written

    AudioEditor (juce::AudioFormatManager& formats, const juce::File& f) : file (f)
    {
        setTooltip ("Drag to select. Ctrl+wheel zooms, wheel scrolls, double-click selects all. Tools work on the selection (or the whole sound).");
        const std::pair<const char*, std::function<void()>> tools[] = {
            { "Play",      [this] { play(); } },
            { "Undo",      [this] { step (undoStack, redoStack); } },
            { "Redo",      [this] { step (redoStack, undoStack); } },
            { "Cut",       [this] { copySelection(); edit ([] (auto& b, auto r) { AudioEdit::remove (b, r); }, true, true); } },
            { "Copy",      [this] { copySelection(); } },
            { "Paste",     [this] { paste(); } },
            { "Delete",    [this] { edit ([] (auto& b, auto r) { AudioEdit::remove (b, r); }, true, true); } },
            { "Trim",      [this] { edit ([] (auto& b, auto r) { AudioEdit::trim (b, r); }, true, true); } },
            { "Silence",   [this] { edit ([] (auto& b, auto r) { AudioEdit::silence (b, r); }, true); } },
            { "Fade in",   [this] { edit ([] (auto& b, auto r) { AudioEdit::fadeIn (b, r); }); } },
            { "Fade out",  [this] { edit ([] (auto& b, auto r) { AudioEdit::fadeOut (b, r); }); } },
            { "Normalize", [this] { edit ([] (auto& b, auto r) { AudioEdit::normalise (b, r); }); } },
            { "Reverse",   [this] { edit ([] (auto& b, auto r) { AudioEdit::reverse (b, r); }); } },
            { "+3 dB",     [this] { edit ([] (auto& b, auto r) { AudioEdit::gain (b, r, 3.0f); }); } },
            { "-3 dB",     [this] { edit ([] (auto& b, auto r) { AudioEdit::gain (b, r, -3.0f); }); } },
            { "Apply",     [this] { apply(); } },
        };
        for (auto& [name, fn] : tools)
        {
            auto* b = buttons.add (new juce::TextButton (name));
            b->onClick = fn;
            addAndMakeVisible (b);
        }
        buttons.getLast()->setTooltip ("Save the edit as a new file and use it (the original file is kept)");
        buttons.getLast()->setColour (juce::TextButton::buttonColourId, Theme::accent.darker (0.3f));

        juce::Thread::launch ([safe = juce::Component::SafePointer<AudioEditor> (this), &formats, f]
        {
            auto loaded = std::make_shared<AudioEdit::Buffer>();
            double rate = 44100.0;
            const bool ok = AudioEdit::read (formats, f, *loaded, rate);
            juce::MessageManager::callAsync ([safe, loaded, rate, ok]
            {
                if (safe != nullptr)
                    safe->loaded (ok ? std::move (*loaded) : AudioEdit::Buffer(), rate);
            });
        });

        Widgets::noFocus (*this);
        setSize (860, 360);
    }

    ~AudioEditor() override { if (onPreview && playing) onPreview ({}); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        const auto wave = getWaveArea();
        g.setColour (Theme::lcd);
        g.fillRect (wave);

        const int numChannels = buffer.getNumChannels();
        if (buffer.getNumSamples() == 0)
        {
            g.setColour (Theme::textDim);
            g.drawText (status, wave, juce::Justification::centred);
            return;
        }

        if (! selection.isEmpty())
        {
            g.setColour (Theme::accent.withAlpha (0.25f));
            g.fillRect (juce::Rectangle<int>::leftTopRightBottom (juce::jmax (wave.getX(), sampleToX (selection.getStart())), wave.getY(),
                                                                  juce::jmin (wave.getRight(), sampleToX (selection.getEnd())), wave.getBottom()));
        }

        const float laneHeight = (float) wave.getHeight() / (float) numChannels;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float mid = (float) wave.getY() + laneHeight * ((float) ch + 0.5f);
            g.setColour (Theme::lcdText.withAlpha (0.25f));
            g.drawHorizontalLine ((int) mid, (float) wave.getX(), (float) wave.getRight());
            g.setColour (Theme::lcdText.withAlpha (0.9f));
            for (int x = wave.getX(); x < wave.getRight(); ++x)
            {
                const int s0 = xToSample (x), s1 = juce::jmax (s0 + 1, xToSample (x + 1));
                if (s0 >= buffer.getNumSamples())
                    break;
                const auto range = juce::FloatVectorOperations::findMinAndMax (buffer.getReadPointer (ch, s0), juce::jmin (s1, buffer.getNumSamples()) - s0);
                g.drawVerticalLine (x, mid - range.getEnd() * laneHeight * 0.48f, mid - range.getStart() * laneHeight * 0.48f + 1.0f);
            }
        }

        g.setColour (Theme::accent);
        g.drawVerticalLine (sampleToX (selection.getStart()), (float) wave.getY(), (float) wave.getBottom());

        auto seconds = [this] (int s) { return juce::String ((double) s / sampleRate, 3) + " s"; };
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f));
        g.drawText (file.getFileName() + (dirty ? " *" : "") + "    length " + seconds (buffer.getNumSamples())
                        + (selection.isEmpty() ? "    cursor " + seconds (selection.getStart())
                                               : "    selection " + seconds (selection.getStart()) + " - " + seconds (selection.getEnd())),
                    getLocalBounds().removeFromBottom (22).reduced (8, 0), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        auto row1 = r.removeFromTop (26), row2 = r.removeFromTop (30).withTrimmedTop (4);
        for (int i = 0; i < buttons.size(); ++i)
        {
            auto& row = i < 8 || i == buttons.size() - 1 ? row1 : row2;
            if (i == buttons.size() - 1)
                buttons[i]->setBounds (row1.removeFromRight (80));
            else
                buttons[i]->setBounds (row.removeFromLeft (i == 0 ? 60 : 74).withTrimmedRight (4));
            if (i == 0 || i == 2)
                row1.removeFromLeft (10);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        selecting = getWaveArea().contains (e.getPosition());
        if (! selecting)
            return;
        dragStart = xToSample (e.x);
        selection = { dragStart, dragStart };
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! selecting)
            return;
        const int s = xToSample (e.x);
        selection = { juce::jmin (s, dragStart), juce::jmax (s, dragStart) };
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { selection = { 0, buffer.getNumSamples() }; repaint(); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            const int anchor = xToSample (e.x);
            samplesPerPixel = juce::jlimit (0.05, fitZoom(), samplesPerPixel * (w.deltaY > 0 ? 1.0 / 1.25 : 1.25));
            viewStart = anchor - (e.x - getWaveArea().getX()) * samplesPerPixel;
        }
        else
        {
            const float delta = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
            viewStart -= delta * 200.0 * samplesPerPixel;
        }
        clampView();
        repaint();
    }

private:
    void loaded (AudioEdit::Buffer b, double rate)
    {
        buffer = std::move (b);
        sampleRate = rate;
        status = buffer.getNumSamples() > 0 ? juce::String() : "Couldn't read " + file.getFileName();
        samplesPerPixel = fitZoom();
        viewStart = 0.0;
        repaint();
    }

    juce::Rectangle<int> getWaveArea() const { return getLocalBounds().reduced (6).withTrimmedTop (60).withTrimmedBottom (20); }
    double fitZoom() const                   { return juce::jmax (1.0, (double) buffer.getNumSamples() / juce::jmax (1, getWaveArea().getWidth())); }
    int sampleToX (int s) const              { return getWaveArea().getX() + (int) std::round ((s - viewStart) / samplesPerPixel); }
    int xToSample (int x) const              { return juce::jlimit (0, buffer.getNumSamples(), (int) (viewStart + (x - getWaveArea().getX()) * samplesPerPixel)); }

    void clampView()
    {
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, buffer.getNumSamples() - getWaveArea().getWidth() * samplesPerPixel), viewStart);
    }

    // Runs an edit on the selection (or everything if nothing is selected and `needsSelection` is false).
    void edit (std::function<void (AudioEdit::Buffer&, AudioEdit::Range)> fn, bool needsSelection = false, bool changesLength = false)
    {
        if (buffer.getNumSamples() == 0 || (needsSelection && selection.isEmpty()))
            return;
        const auto range = selection.isEmpty() ? AudioEdit::Range (0, buffer.getNumSamples()) : selection;
        pushUndo();
        fn (buffer, range);
        if (changesLength)
            selection = { range.getStart(), range.getStart() };
        selection = selection.getIntersectionWith ({ 0, buffer.getNumSamples() });
        dirty = true;
        clampView();
        repaint();
    }

    void pushUndo()
    {
        // ponytail: whole-buffer snapshots, 20 deep. Fine for samples/loops; diff-based history if long files need it.
        undoStack.push_back (buffer);
        if (undoStack.size() > 20)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
    }

    void step (std::vector<AudioEdit::Buffer>& from, std::vector<AudioEdit::Buffer>& to)
    {
        if (from.empty())
            return;
        to.push_back (std::move (buffer));
        buffer = std::move (from.back());
        from.pop_back();
        selection = selection.getIntersectionWith ({ 0, buffer.getNumSamples() });
        dirty = true;
        clampView();
        repaint();
    }

    static AudioEdit::Buffer& clipboard() { static AudioEdit::Buffer b; return b; }

    void copySelection()
    {
        if (! selection.isEmpty())
            clipboard() = AudioEdit::copy (buffer, selection);
    }

    void paste()
    {
        if (clipboard().getNumSamples() == 0 || buffer.getNumChannels() == 0)
            return;
        pushUndo();
        if (! selection.isEmpty())
            AudioEdit::remove (buffer, selection);
        AudioEdit::insert (buffer, selection.getStart(), clipboard());
        selection = { selection.getStart(), selection.getStart() + clipboard().getNumSamples() };
        dirty = true;
        repaint();
    }

    void play()
    {
        if (! onPreview || buffer.getNumSamples() == 0)
            return;
        if (playing)
        {
            playing = false;
            return onPreview ({});
        }
        const auto range = selection.isEmpty() ? AudioEdit::Range (selection.getStart(), buffer.getNumSamples()) : selection;
        const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Untitled Studio audio editor preview.wav");
        if (AudioEdit::writeWav (AudioEdit::copy (buffer, range), sampleRate, temp))
        {
            playing = true;
            onPreview (temp);
        }
    }

    void apply()
    {
        if (! dirty || buffer.getNumSamples() == 0)
            return;
        // ponytail: writes on the message thread; fine for samples and loops, move to a thread for long recordings.
        const auto out = AudioEdit::nextEditFile (file);
        if (! AudioEdit::writeWav (buffer, sampleRate, out))
            return;
        file = out;
        dirty = false;
        if (onApply) onApply (out);
        repaint();
    }

    juce::File file;
    AudioEdit::Buffer buffer;
    double sampleRate = 44100.0, samplesPerPixel = 1.0, viewStart = 0.0;
    AudioEdit::Range selection;
    int dragStart = 0;
    bool dirty = false, playing = false, selecting = false;
    juce::String status { "Loading..." };
    std::vector<AudioEdit::Buffer> undoStack, redoStack;
    juce::OwnedArray<juce::TextButton> buttons;
};
