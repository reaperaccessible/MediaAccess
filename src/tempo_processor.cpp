#include "tempo_processor.h"
#include "globals.h"
#include "bass_fx.h"
#include <cmath>
#include <memory>
#include <vector>
#include <mutex>
#include <deque>

// Include Speedy/Sonic
#ifdef USE_SPEEDY
extern "C" {
#include "sonic2.h"
}
#endif

// Include Signalsmith Stretch
#ifdef USE_SIGNALSMITH
#include "signalsmith-stretch.h"
#endif

// Global algorithm preference
static TempoAlgorithm g_algorithm = TempoAlgorithm::SoundTouch;
static std::unique_ptr<TempoProcessor> g_tempoProcessor;

// Algorithm metadata
const char* GetAlgorithmName(TempoAlgorithm algo) {
    switch (algo) {
        case TempoAlgorithm::SoundTouch: return "SoundTouch (BASS_FX)";
        case TempoAlgorithm::Speedy: return "Speedy (Google)";
        case TempoAlgorithm::Signalsmith: return "Signalsmith Stretch";
        default: return "Unknown";
    }
}

const char* GetAlgorithmDescription(TempoAlgorithm algo) {
    switch (algo) {
        case TempoAlgorithm::SoundTouch:
            return "Fast processing, good for speech and general use";
        case TempoAlgorithm::Speedy:
            return "Nonlinear speech speedup, preserves consonants";
        case TempoAlgorithm::Signalsmith:
            return "High quality pitch/time, low latency";
        default:
            return "";
    }
}

// ============================================================================
// SoundTouch (BASS_FX) Implementation
//
// Trade-offs:
//   + Lowest CPU of the three algorithms.
//   + Pull-based: BASS owns the data flow, just wrap the source stream and
//     forget. Tempo/pitch attributes are set live with no glitching.
//   + Works on any source BASS can decode, including live streams.
//   - Quality degrades on music (phasing, slap-back) at extreme tempo.
//   - "Sequence/seek window/overlap" parameters need tuning per content.
//
// Used by: default for new files; mandatory for live/internet streams (the
// other two algorithms use push-based StreamProc callbacks that don't play
// well with network buffering).
// ============================================================================
class SoundTouchProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_fxStream = 0;
    float m_sampleRate = 44100.0f;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier

public:
    ~SoundTouchProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;

        // BASS_FX_FREESOURCE transfers ownership of sourceStream to the new
        // tempo stream — Shutdown() will free both via a single BASS_StreamFree
        // call. BASS_SAMPLE_FLOAT keeps the pipeline in float so downstream
        // BFX effects don't have to round-trip through 16-bit.
        m_fxStream = BASS_FX_TempoCreate(sourceStream, BASS_FX_FREESOURCE | BASS_SAMPLE_FLOAT);
        if (!m_fxStream) {
            return 0;
        }

        // Apply SoundTouch algorithm settings
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_USE_AA_FILTER, g_stAntiAliasFilter ? 1.0f : 0.0f);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_AA_FILTER_LENGTH, static_cast<float>(g_stAAFilterLength));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_USE_QUICKALGO, g_stQuickAlgorithm ? 1.0f : 0.0f);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_SEQUENCE_MS, static_cast<float>(g_stSequenceMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_SEEKWINDOW_MS, static_cast<float>(g_stSeekWindowMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_OVERLAP_MS, static_cast<float>(g_stOverlapMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_PREVENT_CLICK, g_stPreventClick ? 1.0f : 0.0f);

        // Apply current tempo/pitch/rate settings
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO, m_tempo);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_PITCH, m_pitch);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_FREQ, m_sampleRate * m_rate);

        return m_fxStream;
    }

    void Shutdown() override {
        // BASS_FX_FREESOURCE flag means freeing fxStream frees source too
        if (m_fxStream) {
            BASS_StreamFree(m_fxStream);
            m_fxStream = 0;
            m_sourceStream = 0;
        }
    }

    void SetTempo(float tempoPercent) override {
        m_tempo = tempoPercent;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO, m_tempo);
        }
    }

    void SetPitch(float semitones) override {
        m_pitch = semitones;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_PITCH, m_pitch);
        }
    }

    void SetRate(float rate) override {
        m_rate = rate;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_FREQ, m_sampleRate * m_rate);
        }
    }

    float GetTempo() const override { return m_tempo; }
    float GetPitch() const override { return m_pitch; }
    float GetRate() const override { return m_rate; }
    bool IsActive() const override { return m_fxStream != 0; }
    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::SoundTouch; }

    double GetLength() const override {
        if (!m_fxStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_fxStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_fxStream, bytes);
    }

    double GetPosition() const override {
        if (!m_fxStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_fxStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_fxStream, bytes);
    }

    void SetPosition(double seconds) override {
        if (!m_fxStream) return;
        QWORD bytes = BASS_ChannelSeconds2Bytes(m_fxStream, seconds);
        BASS_ChannelSetPosition(m_fxStream, bytes, BASS_POS_BYTE | BASS_POS_FLUSH);
    }

    HSTREAM GetSourceStream() const override {
        return m_sourceStream;
    }
};

// ============================================================================
// Speedy (Google) Implementation — push-based stream approach
//
// Trade-offs:
//   + Nonlinear speedup: at high speed (e.g. 2x+), Speedy keeps consonants
//     near-native rate and only compresses vowels/silence, so speech stays
//     intelligible far past where SoundTouch starts to smear.
//   + Designed by Google specifically for accessibility playback.
//   - Heuristic is speech-tuned; sounds worse than SoundTouch on music.
//   - Push-based: we decode source ourselves, push to Sonic, pull processed
//     samples out in a BASS StreamProc. Doesn't fit live URL streams well
//     (LoadURL forces SoundTouch instead).
//   - Doesn't support BASS_FX_FREESOURCE — the source stream is freed
//     manually in Shutdown().
//
// Wraps Sonic2 (libsonic with Speedy extensions enabled via
// sonicEnableNonlinearSpeedup).
// ============================================================================
#ifdef USE_SPEEDY

class SpeedyProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_outputStream = 0;
    sonicStream m_sonicStream = nullptr;
    float m_sampleRate = 44100.0f;
    int m_channels = 2;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier
    bool m_sourceEnded = false;
    bool m_nonlinearEnabled = true;  // Use Speedy's nonlinear speedup

    mutable std::mutex m_mutex;

    // Buffers
    std::vector<float> m_decodeBuffer;
    std::deque<float> m_outputQueue;

    static constexpr size_t DECODE_BLOCK_SIZE = 2048;

    // Convert tempo percentage to speed multiplier
    float TempoToSpeed() const {
        float speed = (100.0f + m_tempo) / 100.0f * m_rate;
        if (speed < 0.1f) speed = 0.1f;
        if (speed > 6.0f) speed = 6.0f;  // Speedy supports up to 6X
        return speed;
    }

    // Convert semitones to pitch multiplier
    float SemitonesToPitch() const {
        return powf(2.0f, m_pitch / 12.0f);
    }

    // Process more audio from source through Speedy
    bool ProcessMoreAudio() {
        if (m_sourceEnded || !m_sonicStream) return false;

        // Decode a block from source
        DWORD bytesNeeded = DECODE_BLOCK_SIZE * m_channels * sizeof(float);
        m_decodeBuffer.resize(DECODE_BLOCK_SIZE * m_channels);

        DWORD bytesRead = BASS_ChannelGetData(m_sourceStream, m_decodeBuffer.data(),
            bytesNeeded | BASS_DATA_FLOAT);

        if (bytesRead == (DWORD)-1 || bytesRead == 0) {
            m_sourceEnded = true;
            sonicFlushStream(m_sonicStream);
            // Read any remaining output
            std::vector<float> tempOut(4096 * m_channels);
            int samplesRead;
            while ((samplesRead = sonicReadFloatFromStream(m_sonicStream, tempOut.data(), 4096)) > 0) {
                for (int i = 0; i < samplesRead * m_channels; i++) {
                    m_outputQueue.push_back(tempOut[i]);
                }
            }
            return false;
        }

        int samplesDecoded = bytesRead / sizeof(float) / m_channels;

        // Write to Speedy
        sonicWriteFloatToStream(m_sonicStream, m_decodeBuffer.data(), samplesDecoded);

        // Read processed output
        std::vector<float> tempOut(4096 * m_channels);
        int samplesRead;
        while ((samplesRead = sonicReadFloatFromStream(m_sonicStream, tempOut.data(), 4096)) > 0) {
            for (int i = 0; i < samplesRead * m_channels; i++) {
                m_outputQueue.push_back(tempOut[i]);
            }
        }

        return true;
    }

    // BASS pull callback: invoked on the audio thread whenever BASS needs
    // more samples for the output stream. We drain m_outputQueue first;
    // when empty, decode another block from source and push it through
    // Sonic. Returns BASS_STREAMPROC_END once the source is exhausted AND
    // our queue is empty, which lets the SYNC_END callback fire.
    static DWORD CALLBACK StreamProc(HSTREAM handle, void* buffer, DWORD length, void* user) {
        SpeedyProcessor* proc = static_cast<SpeedyProcessor*>(user);
        if (!proc) return BASS_STREAMPROC_END;

        std::lock_guard<std::mutex> lock(proc->m_mutex);

        float* outBuf = static_cast<float*>(buffer);
        DWORD samplesNeeded = length / sizeof(float);
        DWORD samplesWritten = 0;

        while (samplesWritten < samplesNeeded) {
            if (!proc->m_outputQueue.empty()) {
                size_t canCopy = std::min((size_t)(samplesNeeded - samplesWritten),
                                         proc->m_outputQueue.size());
                for (size_t i = 0; i < canCopy; i++) {
                    outBuf[samplesWritten++] = proc->m_outputQueue.front();
                    proc->m_outputQueue.pop_front();
                }
            } else if (!proc->m_sourceEnded) {
                if (!proc->ProcessMoreAudio()) {
                    if (proc->m_outputQueue.empty()) break;
                }
            } else {
                break;
            }
        }

        while (samplesWritten < samplesNeeded) {
            outBuf[samplesWritten++] = 0.0f;
        }

        if (proc->m_sourceEnded && proc->m_outputQueue.empty()) {
            return samplesWritten * sizeof(float) | BASS_STREAMPROC_END;
        }

        return samplesWritten * sizeof(float);
    }

public:
    SpeedyProcessor() = default;

    ~SpeedyProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;
        m_sourceEnded = false;
        m_outputQueue.clear();
        m_nonlinearEnabled = g_speedyNonlinear;  // Use global setting

        // Get channel info
        BASS_CHANNELINFO info;
        if (!BASS_ChannelGetInfo(sourceStream, &info)) {
            return 0;
        }
        m_channels = info.chans;

        // Create Speedy/Sonic stream
        m_sonicStream = sonicCreateStream(static_cast<int>(m_sampleRate), m_channels);
        if (!m_sonicStream) {
            return 0;
        }

        // Enable Speedy's nonlinear speedup for speech
        if (m_nonlinearEnabled) {
            sonicEnableNonlinearSpeedup(m_sonicStream, 1.0f);
        }

        // Apply current settings
        UpdateSonicParams();

        // Create output stream
        m_outputStream = BASS_StreamCreate(
            static_cast<DWORD>(m_sampleRate),
            m_channels,
            BASS_SAMPLE_FLOAT,
            StreamProc,
            this
        );

        if (!m_outputStream) {
            sonicDestroyStream(m_sonicStream);
            m_sonicStream = nullptr;
            return 0;
        }

        return m_outputStream;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_outputStream) {
            BASS_StreamFree(m_outputStream);
            m_outputStream = 0;
        }
        if (m_sonicStream) {
            sonicDestroyStream(m_sonicStream);
            m_sonicStream = nullptr;
        }
        m_sourceStream = 0;
        m_outputQueue.clear();
    }

    void SetTempo(float tempoPercent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = tempoPercent;
        UpdateSonicParams();
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        UpdateSonicParams();
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate;
        UpdateSonicParams();
    }

    float GetTempo() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tempo;
    }
    float GetPitch() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pitch;
    }
    float GetRate() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rate;
    }
    bool IsActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sonicStream != nullptr;
    }
    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::Speedy; }

    double GetLength() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    double GetPosition() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    void SetPosition(double seconds) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream || !m_sonicStream) return;

        QWORD bytes = BASS_ChannelSeconds2Bytes(m_sourceStream, seconds);
        BASS_ChannelSetPosition(m_sourceStream, bytes, BASS_POS_BYTE | BASS_POS_FLUSH);

        // Sonic has no documented "flush state" entry point — destroy and
        // recreate is the cleanest way to discard internal frame state
        // (otherwise the first samples after a seek leak audio from before).
        sonicDestroyStream(m_sonicStream);
        m_sonicStream = sonicCreateStream(static_cast<int>(m_sampleRate), m_channels);
        if (m_sonicStream && m_nonlinearEnabled) {
            sonicEnableNonlinearSpeedup(m_sonicStream, 1.0f);
        }
        UpdateSonicParams();
        m_outputQueue.clear();
        m_sourceEnded = false;
    }

    HSTREAM GetSourceStream() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sourceStream;
    }

private:
    void UpdateSonicParams() {
        if (!m_sonicStream) return;

        float speed = TempoToSpeed();
        float pitch = SemitonesToPitch();

        sonicSetSpeed(m_sonicStream, speed);
        // sonicSetPitch is not wrapped by sonic2.h, use internal function
        sonicIntSetPitch(m_sonicStream, pitch);
    }
};

#endif // USE_SPEEDY

// ============================================================================
// Signalsmith Stretch Implementation
//
// Trade-offs:
//   + Highest audio quality of the three on music — phase-coherent STFT
//     stretching with built-in transient detection.
//   + Independent pitch + time control via setTransposeSemitones() and the
//     input/output sample ratio in process().
//   + Low latency mode available (presetCheaper).
//   - Highest CPU; on weak hardware this can underflow the BASS playback
//     buffer at extreme rate.
//   - Push-based like Speedy, same limitations on live streams.
//   - Stretcher state must be reset() on seek and recreated on parameter
//     changes — see SetPosition() for the reset dance.
// ============================================================================
#ifdef USE_SIGNALSMITH

class SignalsmithProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_outputStream = 0;
    signalsmith::stretch::SignalsmithStretch<float> m_stretcher;
    float m_sampleRate = 44100.0f;
    int m_channels = 2;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier
    bool m_sourceEnded = false;

    mutable std::mutex m_mutex;

    // Buffers
    std::vector<float> m_decodeBuffer;
    std::vector<float*> m_inputChannels;
    std::vector<float*> m_outputChannels;
    std::vector<std::vector<float>> m_channelIn;
    std::vector<std::vector<float>> m_channelOut;
    std::deque<float> m_outputQueue;

    static constexpr size_t DECODE_BLOCK_SIZE = 1024;

    double GetSpeedMultiplier() const {
        return (100.0 + m_tempo) / 100.0 * m_rate;
    }

    bool ProcessMoreAudio() {
        if (m_sourceEnded || m_channels == 0) return false;

        DWORD bytesNeeded = DECODE_BLOCK_SIZE * m_channels * sizeof(float);
        m_decodeBuffer.resize(DECODE_BLOCK_SIZE * m_channels);

        DWORD bytesRead = BASS_ChannelGetData(m_sourceStream, m_decodeBuffer.data(),
            bytesNeeded | BASS_DATA_FLOAT);

        if (bytesRead == (DWORD)-1 || bytesRead == 0) {
            m_sourceEnded = true;
            return false;
        }

        size_t samplesDecoded = bytesRead / sizeof(float) / m_channels;

        // Deinterleave input
        for (int ch = 0; ch < m_channels; ch++) {
            m_channelIn[ch].resize(samplesDecoded);
            for (size_t i = 0; i < samplesDecoded; i++) {
                m_channelIn[ch][i] = m_decodeBuffer[i * m_channels + ch];
            }
            m_inputChannels[ch] = m_channelIn[ch].data();
        }

        // Calculate output size based on speed
        double speed = GetSpeedMultiplier();
        if (speed < 0.1) speed = 0.1;
        if (speed > 10.0) speed = 10.0;
        size_t outputSamples = static_cast<size_t>(samplesDecoded / speed + 0.5);
        if (outputSamples < 1) outputSamples = 1;

        // Resize output buffers
        for (int ch = 0; ch < m_channels; ch++) {
            m_channelOut[ch].resize(outputSamples);
            m_outputChannels[ch] = m_channelOut[ch].data();
        }

        // Process through Signalsmith
        m_stretcher.process(m_inputChannels.data(), (int)samplesDecoded,
                           m_outputChannels.data(), (int)outputSamples);

        // Interleave output to queue
        for (size_t i = 0; i < outputSamples; i++) {
            for (int ch = 0; ch < m_channels; ch++) {
                m_outputQueue.push_back(m_channelOut[ch][i]);
            }
        }

        return true;
    }

    static DWORD CALLBACK StreamProc(HSTREAM handle, void* buffer, DWORD length, void* user) {
        SignalsmithProcessor* proc = static_cast<SignalsmithProcessor*>(user);
        if (!proc) return BASS_STREAMPROC_END;

        std::lock_guard<std::mutex> lock(proc->m_mutex);

        float* outBuf = static_cast<float*>(buffer);
        DWORD samplesNeeded = length / sizeof(float);
        DWORD samplesWritten = 0;

        while (samplesWritten < samplesNeeded) {
            if (!proc->m_outputQueue.empty()) {
                size_t canCopy = std::min((size_t)(samplesNeeded - samplesWritten),
                                         proc->m_outputQueue.size());
                for (size_t i = 0; i < canCopy; i++) {
                    outBuf[samplesWritten++] = proc->m_outputQueue.front();
                    proc->m_outputQueue.pop_front();
                }
            } else if (!proc->m_sourceEnded) {
                if (!proc->ProcessMoreAudio()) {
                    if (proc->m_outputQueue.empty()) {
                        break;
                    }
                }
            } else {
                break;
            }
        }

        if (samplesWritten == 0 && proc->m_sourceEnded) {
            return BASS_STREAMPROC_END;
        }

        // Zero remaining buffer if we couldn't fill it
        while (samplesWritten < samplesNeeded) {
            outBuf[samplesWritten++] = 0.0f;
        }

        return samplesWritten * sizeof(float);
    }

public:
    ~SignalsmithProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;
        m_sourceEnded = false;
        m_outputQueue.clear();

        // Get channel info
        BASS_CHANNELINFO info;
        if (!BASS_ChannelGetInfo(sourceStream, &info)) {
            return 0;
        }
        m_channels = info.chans;

        // Configure Signalsmith based on global settings.
        // "Cheaper" preset trades a small quality loss for ~half the CPU; it's
        // what we recommend on weak hardware. "Default" is the high-quality
        // setup with longer FFT windows.
        if (g_ssPreset == 1) {
            m_stretcher.presetCheaper(m_channels, (int)sampleRate);
        } else {
            m_stretcher.presetDefault(m_channels, (int)sampleRate);
        }

        // tonalityLimit (Hz) caps the upper frequency where pitch shifting
        // tries to remain phase-coherent. Above the limit, content is
        // resampled instead. 0 = auto-pick from sample rate. Users with
        // sibilant artifacts on speech sometimes drop this to ~6kHz.
        float tonalityLimit = g_ssTonalityLimit > 0 ? static_cast<float>(g_ssTonalityLimit) / sampleRate : 0.0f;
        m_stretcher.setTransposeSemitones(m_pitch, tonalityLimit);
        m_stretcher.reset();

        // Allocate buffers
        m_channelIn.resize(m_channels);
        m_channelOut.resize(m_channels);
        m_inputChannels.resize(m_channels);
        m_outputChannels.resize(m_channels);

        // Prime: Signalsmith reports an intrinsic input+output latency in
        // samples. We push that many source samples through process() and
        // discard the output, so the very first user-audible sample comes
        // out aligned with t=0 of the source — otherwise the file appears
        // to start a few hundred ms late on the first play.
        int latencySamples = m_stretcher.inputLatency() + m_stretcher.outputLatency();
        if (latencySamples > 0) {
            std::vector<float> primeBuffer(latencySamples * m_channels, 0.0f);
            DWORD bytesRead = BASS_ChannelGetData(sourceStream, primeBuffer.data(),
                latencySamples * m_channels * sizeof(float) | BASS_DATA_FLOAT);

            if (bytesRead > 0 && bytesRead != (DWORD)-1) {
                size_t primeSamples = bytesRead / sizeof(float) / m_channels;

                // Deinterleave and process
                for (int ch = 0; ch < m_channels; ch++) {
                    m_channelIn[ch].resize(primeSamples);
                    for (size_t i = 0; i < primeSamples; i++) {
                        m_channelIn[ch][i] = primeBuffer[i * m_channels + ch];
                    }
                    m_inputChannels[ch] = m_channelIn[ch].data();
                    m_channelOut[ch].resize(primeSamples);
                    m_outputChannels[ch] = m_channelOut[ch].data();
                }

                // Process to prime the stretcher (discard output)
                m_stretcher.process(m_inputChannels.data(), (int)primeSamples,
                                   m_outputChannels.data(), (int)primeSamples);
            }
        }

        // Create output stream (no DECODE flag - this is the playback stream)
        m_outputStream = BASS_StreamCreate(
            (DWORD)sampleRate,
            m_channels,
            BASS_SAMPLE_FLOAT,
            StreamProc,
            this
        );

        if (!m_outputStream) {
            return 0;
        }

        return m_outputStream;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_outputStream) {
            BASS_StreamFree(m_outputStream);
            m_outputStream = 0;
        }
        m_sourceStream = 0;
        m_outputQueue.clear();
        m_channelIn.clear();
        m_channelOut.clear();
        m_inputChannels.clear();
        m_outputChannels.clear();
    }

    void SetTempo(float tempoPercent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = tempoPercent;
        // Signalsmith handles tempo via input/output sample ratio in process()
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        float tonalityLimit = g_ssTonalityLimit > 0 ? static_cast<float>(g_ssTonalityLimit) / m_sampleRate : 0.0f;
        m_stretcher.setTransposeSemitones(semitones, tonalityLimit);
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate;
        // Rate is handled via input/output sample ratio in process()
    }

    float GetTempo() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tempo;
    }

    float GetPitch() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pitch;
    }

    float GetRate() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rate;
    }

    bool IsActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_outputStream != 0;
    }

    TempoAlgorithm GetAlgorithm() const override {
        return TempoAlgorithm::Signalsmith;
    }

    double GetLength() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD len = BASS_ChannelGetLength(m_sourceStream, BASS_POS_BYTE);
        return BASS_ChannelBytes2Seconds(m_sourceStream, len);
    }

    double GetPosition() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD pos = BASS_ChannelGetPosition(m_sourceStream, BASS_POS_BYTE);
        return BASS_ChannelBytes2Seconds(m_sourceStream, pos);
    }

    void SetPosition(double seconds) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return;

        QWORD pos = BASS_ChannelSeconds2Bytes(m_sourceStream, seconds);
        BASS_ChannelSetPosition(m_sourceStream, pos, BASS_POS_BYTE);

        // Reset stretcher and clear output
        m_stretcher.reset();
        m_stretcher.setTransposeSemitones(m_pitch);
        m_outputQueue.clear();
        m_sourceEnded = false;
    }

    HSTREAM GetSourceStream() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sourceStream;
    }
};

#endif // USE_SIGNALSMITH

// ============================================================================
// Factory and Global Management
// ============================================================================

// Allocate a processor of the requested algorithm. If the algorithm is not
// compiled in (USE_SPEEDY / USE_SIGNALSMITH off), silently falls back to
// SoundTouch so callers never get nullptr.
TempoProcessor* CreateTempoProcessor(TempoAlgorithm algorithm) {
    switch (algorithm) {
        case TempoAlgorithm::SoundTouch:
            return new SoundTouchProcessor();
#ifdef USE_SPEEDY
        case TempoAlgorithm::Speedy:
            return new SpeedyProcessor();
#endif
#ifdef USE_SIGNALSMITH
        case TempoAlgorithm::Signalsmith:
            return new SignalsmithProcessor();
#endif
        default:
            // Fall back to SoundTouch if algorithm not available
            return new SoundTouchProcessor();
    }
}

TempoAlgorithm GetCurrentAlgorithm() {
    return g_algorithm;
}

void SetCurrentAlgorithm(TempoAlgorithm algorithm) {
    // Check if the selected algorithm is available
    bool available = false;
    switch (algorithm) {
        case TempoAlgorithm::SoundTouch:
            available = true;
            break;
#ifdef USE_SPEEDY
        case TempoAlgorithm::Speedy:
            available = true;
            break;
#endif
#ifdef USE_SIGNALSMITH
        case TempoAlgorithm::Signalsmith:
            available = true;
            break;
#endif
        default:
            break;
    }

    if (!available) {
        algorithm = TempoAlgorithm::SoundTouch;
    }
    g_algorithm = algorithm;
}

void InitTempoProcessor() {
    if (!g_tempoProcessor) {
        g_tempoProcessor.reset(CreateTempoProcessor(g_algorithm));
    }
}

void FreeTempoProcessor() {
    if (g_tempoProcessor) {
        g_tempoProcessor->Shutdown();
        g_tempoProcessor.reset();
    }
}

TempoProcessor* GetTempoProcessor() {
    if (!g_tempoProcessor) {
        InitTempoProcessor();
    }
    return g_tempoProcessor.get();
}
