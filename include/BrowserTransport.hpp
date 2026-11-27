// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __EMSCRIPTEN__
#include <cmath>
#include <cstdint>

namespace rack {

// The web standalone delivers both UI and audio callbacks on its main thread.
struct BrowserTransport {
    enum Quantization { Immediate = 0, NextBeat = 1, NextBar = 4 };
    bool enabled = false;
    bool playing = false;
    int bpm = 120;
    int quantizeBeats = NextBar;
    int beatsPerBar = 4;
    int beatType = 4;
    unsigned revision = 0;
    double beat = 0.0;
    uint64_t frame = 0;
    double lastTap = -1.0;
    double tapPeriod = 0.0;

    void setBpm(double value) {
        if (std::isfinite(value))
            bpm = static_cast<int>(std::round(std::fmax(20.0, std::fmin(400.0, value))));
    }

    void tap(double seconds) {
        const double interval = seconds - lastTap;
        if (lastTap >= 0.0 && interval >= 0.15 && interval <= 3.0) {
            tapPeriod = tapPeriod > 0.0 ? (tapPeriod + interval) * 0.5 : interval;
            setBpm(60.0 / tapPeriod);
        } else {
            tapPeriod = 0.0;
        }
        lastTap = seconds;
    }

    int launchGrid() const {
        return quantizeBeats == NextBar ? beatsPerBar : quantizeBeats;
    }

    void setMeter(int beats, int unit) {
        if (beats == beatsPerBar && unit == beatType)
            return;
        beatsPerBar = beats;
        beatType = unit;
        rewind();
    }

    void toggle() {
        if (playing)
            stop();
        else
            play();
    }

    void setEnabled(bool value) {
        enabled = value;
        stop();
    }

    void play() {
        if (!enabled)
            enabled = true;
        if (!playing) {
            playing = true;
            ++revision;
        }
    }

    void stop() {
        playing = false;
        rewind();
    }

    void rewind() {
        beat = 0.0;
        frame = 0;
        ++revision;
    }

    void advance(uint32_t frames, double rate) {
        if (playing) {
            beat += frames * bpm / (60.0 * rate);
            frame += frames;
        }
    }

    template <typename TimePosition>
    void writePosition(TimePosition& position) const {
        constexpr double ticksPerBeat = 1920.0;
        const double normalizedBeat = std::abs(beat - std::round(beat)) < 1e-9
            ? std::round(beat) : beat;
        const auto wholeBeat = static_cast<int64_t>(normalizedBeat);
        position.playing = playing;
        position.frame = frame;
        position.bbt.valid = true;
        position.bbt.bar = wholeBeat / beatsPerBar + 1;
        position.bbt.beat = wholeBeat % beatsPerBar + 1;
        position.bbt.beatsPerBar = beatsPerBar;
        position.bbt.beatType = beatType;
        position.bbt.beatsPerMinute = bpm;
        position.bbt.ticksPerBeat = ticksPerBeat;
        position.bbt.tick = (normalizedBeat - wholeBeat) * ticksPerBeat;
        position.bbt.barStartTick = (wholeBeat / beatsPerBar * beatsPerBar) * ticksPerBeat;
    }
};

inline BrowserTransport& browserTransport() {
    static BrowserTransport transport;
    return transport;
}

} // namespace rack
#endif
