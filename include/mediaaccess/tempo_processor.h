#pragma once
#ifndef MEDIAACCESS_TEMPO_PROCESSOR_H
#define MEDIAACCESS_TEMPO_PROCESSOR_H

#include <windows.h>
#include "bass.h"

// Tempo/pitch algorithm types
enum class TempoAlgorithm {
    SoundTouch,     // BASS_FX (SoundTouch-based) - fast, good for speech
    Speedy,         // Google Speedy - nonlinear speech speedup
    Signalsmith,    // Signalsmith Stretch - high quality pitch/time
    COUNT
};

// Algorithm names for UI
const char* GetAlgorithmName(TempoAlgorithm algo);
const char* GetAlgorithmDescription(TempoAlgorithm algo);

// Abstract tempo processor interface
class TempoProcessor {
public:
    virtual ~TempoProcessor() = default;

    // Initialize the processor for a given source stream
    // Returns the playback stream (may be same as source or a wrapper)
    virtual HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) = 0;

    // Clean up resources
    virtual void Shutdown() = 0;

    // Set tempo change in percentage (-50 to +100)
    virtual void SetTempo(float tempoPercent) = 0;

    // Set pitch change in semitones (-12 to +12)
    virtual void SetPitch(float semitones) = 0;

    // Set rate multiplier (0.5 to 2.0) - affects both tempo and pitch
    virtual void SetRate(float rate) = 0;

    // Get current values
    virtual float GetTempo() const = 0;
    virtual float GetPitch() const = 0;
    virtual float GetRate() const = 0;

    // Check if processor is active
    virtual bool IsActive() const = 0;

    // Get the algorithm type
    virtual TempoAlgorithm GetAlgorithm() const = 0;

    // Position and length (in seconds)
    virtual double GetLength() const = 0;
    virtual double GetPosition() const = 0;
    virtual void SetPosition(double seconds) = 0;

    // Get the source stream
    virtual HSTREAM GetSourceStream() const = 0;

    // v2.50 — A-B loop region in SOURCE seconds. Default no-op so subclasses
    // opt in. enabled=false (or start/end < 0) disarms the loop. The wrap is
    // implemented at the source timebase inside each algorithm so it stays
    // sample-accurate while tempo/pitch change.
    virtual void SetLoopRegion(double startSec, double endSec, bool enabled) {
        (void)startSec; (void)endSec; (void)enabled;
    }

    // Crossfade, phase 1 — local files play through a BASSmix decode mixer
    // placed in front of the processor. The processor's source is then the
    // MIXER (endless, no length, cannot seek), and the song being played is a
    // separate decode channel plugged into it. When a song stream is set, the
    // position, length, seek and A-B loop calls above all refer to THAT song
    // (through the BASS_Mixer_Channel* calls), never to the mixer. 0 (the
    // default) keeps the classic behaviour, where the source IS the media
    // (internet streams).
    void SetSongStream(HSTREAM song) { m_song = song; ClearSourceEnd(); }
    HSTREAM GetSongStream() const { return m_song; }

    // Start a new song in the existing chain at `seconds`: discards everything
    // the processor holds from the previous song. Defaults to SetPosition;
    // Signalsmith also re-primes its latency like a fresh Initialize does.
    virtual void StartSong(double seconds) { SetPosition(seconds); }

    // The mixer never ends (BASS_MIXER_NONSTOP), so the end of a song is first
    // seen at DECODE time, well before it is heard (tempo and playback buffers).
    // MarkSourceEnd() is called from the song's mixtime END sync, on the audio
    // thread, while the processor is pulling the mixer: it must not lock. It
    // records where, in the processor's own output, the song's last sample
    // lies. SourceEndHeard() (UI thread) tells when playback has reached that
    // point, i.e. when the song has really been heard to the end. A seek
    // (SetPosition) clears the mark.
    virtual void MarkSourceEnd() {}
    virtual bool SourceEndHeard() const { return false; }
    bool SourceEndMarked() const { return m_endMarked; }
    void ClearSourceEnd() { m_endMarked = false; }

protected:
    HSTREAM m_song = 0;
    volatile bool m_endMarked = false;
};

// Factory function to create a tempo processor
TempoProcessor* CreateTempoProcessor(TempoAlgorithm algorithm);

// Get/set the global algorithm preference
TempoAlgorithm GetCurrentAlgorithm();
void SetCurrentAlgorithm(TempoAlgorithm algorithm);

// Global tempo processor instance management
void InitTempoProcessor();
void FreeTempoProcessor();
TempoProcessor* GetTempoProcessor();

#endif // MEDIAACCESS_TEMPO_PROCESSOR_H
