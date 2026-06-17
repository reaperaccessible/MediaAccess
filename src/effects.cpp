#include "effects.h"
#include "globals.h"
#include "accessibility.h"
#include "resource.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/tts_player.h"  // Drive SAPI rate from Rate effect
#include "mediaaccess/daisy_player.h" // v2.45 — drive Edge book rate from Rate effect
#include "mediaaccess/video_engine.h" // v2.15 — apply Volume/Rate to MPV video
#include "bass_fx.h"
#include "tempo_processor.h"
#include "center_cancel.h"
#include "convolution.h"
#ifdef USE_STEAM_AUDIO
#include "spatial_audio.h"
#endif
#include <cstdio>
#include <vector>
#include <cmath>

// Clamp helper
template<typename T> T clamp_val(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Parameter definitions
static const ParamDef g_paramDefs[] = {
    // Stream effects (DSPEffectType cast to -1 means not a DSP effect)
    {ParamId::Volume,      "Volume",       "%",          0.0f,   4.0f,   0.02f, 1.0f,  (DSPEffectType)-1},
    {ParamId::Pitch,       "Pitch",        " semitones", -12.0f, 12.0f,  1.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Tempo,       "Tempo",        "%",          -75.0f, 200.0f, 5.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Rate,        "Rate",         "x",          0.25f,  4.0f,   0.01f, 1.0f,  (DSPEffectType)-1},
    // Freeverb parameters (algorithm 1)
    {ParamId::ReverbMix,   "Reverb Mix",   "%",          0.0f,   100.0f, 5.0f,  30.0f, DSPEffectType::Reverb},
    {ParamId::ReverbRoom,  "Reverb Room",  "%",          0.0f,   100.0f, 5.0f,  50.0f, DSPEffectType::Reverb},
    {ParamId::ReverbDamp,  "Reverb Damp",  "%",          0.0f,   100.0f, 5.0f,  50.0f, DSPEffectType::Reverb},
    // DX8 Reverb parameters (algorithm 2)
    {ParamId::DX8ReverbTime,    "DX8 Reverb Time",   "ms",  1.0f,   3000.0f, 100.0f, 1000.0f, DSPEffectType::Reverb},
    {ParamId::DX8ReverbHFRatio, "DX8 HF Ratio",      "",    0.001f, 0.999f,  0.1f,   0.5f,    DSPEffectType::Reverb},
    {ParamId::DX8ReverbMix,     "DX8 Reverb Mix",    "dB",  -96.0f, 0.0f,    3.0f,   -10.0f,  DSPEffectType::Reverb},
    // I3DL2 Reverb parameters (algorithm 3)
    {ParamId::I3DL2Room,       "I3DL2 Room",        "mB",  -10000.0f, 0.0f,    500.0f, -1000.0f, DSPEffectType::Reverb},
    {ParamId::I3DL2DecayTime,  "I3DL2 Decay",       "s",   0.1f,      20.0f,   0.5f,   1.49f,    DSPEffectType::Reverb},
    {ParamId::I3DL2Diffusion,  "I3DL2 Diffusion",   "%",   0.0f,      100.0f,  5.0f,   100.0f,   DSPEffectType::Reverb},
    {ParamId::I3DL2Density,    "I3DL2 Density",     "%",   0.0f,      100.0f,  5.0f,   100.0f,   DSPEffectType::Reverb},
    // Echo parameters
    {ParamId::EchoDelay,   "Echo Delay",   "ms",         10.0f,  2000.0f, 50.0f, 300.0f, DSPEffectType::Echo},
    {ParamId::EchoFeedback,"Echo Feedback","%",          0.0f,   90.0f,  5.0f,  40.0f, DSPEffectType::Echo},
    {ParamId::EchoMix,     "Echo Mix",     "%",          0.0f,   100.0f, 5.0f,  30.0f, DSPEffectType::Echo},
    // EQ parameters (in dB)
    {ParamId::EQPreamp,    "EQ Preamp",    "dB",         -15.0f, 0.0f,   1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQBass,      "EQ Bass",      "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQMid,       "EQ Mid",       "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQTreble,    "EQ Treble",    "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    // Compressor parameters
    {ParamId::CompThreshold, "Comp Threshold", "dB",     -60.0f, 0.0f,   3.0f,  -20.0f, DSPEffectType::Compressor},
    {ParamId::CompRatio,     "Comp Ratio",     ":1",     1.0f,   20.0f,  1.0f,  4.0f,   DSPEffectType::Compressor},
    {ParamId::CompAttack,    "Comp Attack",    "ms",     0.01f,  500.0f, 10.0f, 20.0f,  DSPEffectType::Compressor},
    {ParamId::CompRelease,   "Comp Release",   "ms",     10.0f,  2000.0f,50.0f, 200.0f, DSPEffectType::Compressor},
    {ParamId::CompGain,      "Comp Gain",      "dB",     -20.0f, 20.0f,  1.0f,  0.0f,   DSPEffectType::Compressor},
    // Stereo width parameter (0% = mono, 100% = normal, 200% = extra wide)
    {ParamId::StereoWidth,   "Stereo Width",   "%",      0.0f,   200.0f, 10.0f, 100.0f, DSPEffectType::StereoWidth},
    // Center cancel parameter (-100% = extract center, 0% = off, +100% = cancel center)
    {ParamId::CenterCancel,  "Center Cancel",  "%",      -100.0f, 100.0f, 10.0f, 0.0f, DSPEffectType::CenterCancel},
    // Convolution reverb parameters
    {ParamId::ConvolutionMix,  "Conv Mix",   "%",      0.0f, 100.0f, 5.0f, 50.0f, DSPEffectType::Convolution},
    {ParamId::ConvolutionGain, "Conv Gain",  "dB",    -20.0f, 20.0f, 1.0f, 0.0f, DSPEffectType::Convolution},
    // 3D audio parameters
    {ParamId::SpatialBlend,      "3D Blend",       "%",    0.0f,   100.0f,  5.0f,  100.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialWidth,      "3D Width",       " deg", 15.0f,  90.0f,   5.0f,  45.0f,  DSPEffectType::SpatialAudio},
    {ParamId::SpatialRotation,   "3D Rotation",    " deg",-180.0f, 180.0f,  5.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialMode,       "3D Mode",        "",     0.0f,   1.0f,    1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialRearCenter, "3D Rear Speaker","",     0.0f,   1.0f,    1.0f,  1.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialX,          "3D Listener X",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialY,          "3D Listener Y",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialZ,          "3D Listener Z",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
};
static const int g_paramDefCount = sizeof(g_paramDefs) / sizeof(g_paramDefs[0]);

// DSP effect handles
static HFX g_hfxReverb = 0;
static HFX g_hfxEcho = 0;
static HFX g_hfxEQPreamp = 0;
static HFX g_hfxEQBass = 0;
static HFX g_hfxEQMid = 0;
static HFX g_hfxEQTreble = 0;
static HFX g_hfxCompressor = 0;
static HDSP g_hdspStereoWidth = 0;  // Custom DSP for stereo width
static HDSP g_hdspCenterCancel = 0; // Custom DSP for center cancel/extract
static HDSP g_hdspConvolution = 0;  // Custom DSP for convolution reverb
static HDSP g_hdspSpatialAudio = 0;  // Custom DSP for 3D audio (Steam Audio)
static HDSP g_hdspVolume = 0;       // Custom DSP for volume (runs LAST, after encoder)

// DSP effect enabled states
static bool g_dspEnabled[(int)DSPEffectType::COUNT] = {false, false, false, false, false, false, false, false};

// Parameter values
static float g_paramValues[(int)ParamId::COUNT];

// Current parameter index for cycling
static int g_currentParamIndex = 0;

// Seed g_paramValues from g_paramDefs[]. Called once at app startup before
// any effect-toggling UI runs.
//
// IMPORTANT ordering: this runs AFTER LoadSettings(), and LoadSettings reads
// the stream-effect globals (g_tempo, g_pitch, g_rate, g_volume) directly.
// We deliberately don't overwrite those four here — they're routed through
// the GetParamValue() switch for stream effects rather than being mirrored
// in g_paramValues[], so the seeding loop only affects DSP params.
bool InitEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        g_paramValues[(int)g_paramDefs[i].id] = g_paramDefs[i].defaultValue;
    }
    return true;
}

void FreeEffects() {
    RemoveDSPEffects();
    FreeCenterCancelProcessor();
#ifdef USE_STEAM_AUDIO
    FreeSpatialAudio();
#endif
}

// Helper to check if a reverb param matches current algorithm
static bool IsReverbParamForCurrentAlgorithm(ParamId id) {
    switch (id) {
        // Freeverb params (algorithm 1)
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
            return g_reverbAlgorithm == 1;
        // DX8 Reverb params (algorithm 2)
        case ParamId::DX8ReverbTime:
        case ParamId::DX8ReverbHFRatio:
        case ParamId::DX8ReverbMix:
            return g_reverbAlgorithm == 2;
        // I3DL2 Reverb params (algorithm 3)
        case ParamId::I3DL2Room:
        case ParamId::I3DL2DecayTime:
        case ParamId::I3DL2Diffusion:
        case ParamId::I3DL2Density:
            return g_reverbAlgorithm == 3;
        default:
            return true;  // Not a reverb param
    }
}

// Build list of available parameters based on enabled stream effects and DSP effects
static std::vector<ParamId> GetAvailableParams() {
    std::vector<ParamId> params;

    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];

        // Check if this is a stream effect parameter
        if ((int)def.dspEffect == -1) {
            // Stream effect - check if enabled in g_effectEnabled
            int effectIdx = (int)def.id;
            if (effectIdx < 4 && g_effectEnabled[effectIdx]) {
                params.push_back(def.id);
            }
        } else if (def.dspEffect == DSPEffectType::Reverb) {
            // Reverb parameter - check algorithm and matching params
            if (g_reverbAlgorithm > 0 && IsReverbParamForCurrentAlgorithm(def.id)) {
                params.push_back(def.id);
            }
        } else {
            // Other DSP effect parameter - check if the DSP effect is enabled
            if (g_dspEnabled[(int)def.dspEffect]) {
                params.push_back(def.id);
            }
        }
    }

    return params;
}

int GetAvailableParamCount() {
    return (int)GetAvailableParams().size();
}

// Toggle stream effect (Volume=0, Pitch=1, Tempo=2, Rate=3)
void ToggleStreamEffect(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return;

    g_effectEnabled[effectIndex] = !g_effectEnabled[effectIndex];

    // Bilingual: each effect name and the state word go through Ts() so the
    // announcement matches the active MediaAccess UI language.
    const char* names[] = {"Volume", "Pitch", "Tempo", "Rate"};
    std::string msg = Ts(names[effectIndex]) + " " +
                      Ts(g_effectEnabled[effectIndex] ? "enabled" : "disabled");
    Speak(msg);
}

bool IsStreamEffectEnabled(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return false;
    return g_effectEnabled[effectIndex];
}

// Toggle DSP effect
void ToggleDSPEffect(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    // Reverb uses algorithm selection, not simple toggle
    if (type == DSPEffectType::Reverb) {
        // Cycle through reverb algorithms: Off -> Freeverb -> DX8 -> I3DL2 -> Off
        int newAlgo = (g_reverbAlgorithm + 1) % 4;
        SetReverbAlgorithm(newAlgo);
        const char* algoNames[] = {"Off", "Freeverb", "DX8 Reverb", "I3DL2 Reverb"};
        Speak(Ts("Reverb") + ": " + Ts(algoNames[newAlgo]));
        return;
    }

    bool newState = !g_dspEnabled[(int)type];
    EnableDSPEffect(type, newState);

    const char* names[] = {"Reverb", "Echo", "EQ", "Compressor", "Stereo Width", "Center Cancel", "Convolution", "3D Audio"};
    std::string msg = Ts(names[(int)type]) + " " +
                      Ts(newState ? "enabled" : "disabled");
    Speak(msg);
}

// Helpers to drop an effect/DSP handle from g_fxStream and zero the handle.
// Tolerant of the stream having already gone away (only the BASS call is
// gated, not the handle reset — keeps the slot in a known-clean state).
static inline void RemoveFXHandle(HFX& handle) {
    if (handle) {
        if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, handle);
        handle = 0;
    }
}
static inline void RemoveDSPHandle(HDSP& handle) {
    if (handle) {
        if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, handle);
        handle = 0;
    }
}

// Set the active reverb algorithm and re-apply it to the running stream.
//   0 = Off (removes any existing reverb FX)
//   1 = Freeverb (best general quality)
//   2 = DX8 Reverb (cheap, basic)
//   3 = I3DL2 Reverb (rich, preset-driven)
// The previously applied reverb FX is removed unconditionally so switching
// algorithms is a clean swap, not a layered stack.
void SetReverbAlgorithm(int algorithm) {
    if (algorithm < 0 || algorithm > 3) return;

    // Remove existing reverb effect if any (RemoveFXHandle no-ops if either
    // the stream or the handle is already gone).
    RemoveFXHandle(g_hfxReverb);

    g_reverbAlgorithm = algorithm;

    // Apply new reverb if enabled and stream exists
    if (algorithm > 0 && g_fxStream) {
        ApplyDSPEffects();
    }
}

// Enable or disable a DSP effect and (if a stream is live) wire the change
// into BASS immediately. Reverb is handled by SetReverbAlgorithm instead;
// passing DSPEffectType::Reverb here has no effect on g_reverbAlgorithm.
void EnableDSPEffect(DSPEffectType type, bool enable) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    bool wasEnabled = g_dspEnabled[(int)type];
    g_dspEnabled[(int)type] = enable;

    // If stream exists, apply/remove effect immediately
    if (g_fxStream) {
        if (enable && !wasEnabled) {
            ApplyDSPEffects();
        } else if (!enable && wasEnabled) {
            // Remove just this effect
            switch (type) {
                case DSPEffectType::Reverb:       RemoveFXHandle(g_hfxReverb); break;
                case DSPEffectType::Echo:         RemoveFXHandle(g_hfxEcho); break;
                case DSPEffectType::EQ:
                    RemoveFXHandle(g_hfxEQPreamp);
                    RemoveFXHandle(g_hfxEQBass);
                    RemoveFXHandle(g_hfxEQMid);
                    RemoveFXHandle(g_hfxEQTreble);
                    break;
                case DSPEffectType::Compressor:   RemoveFXHandle(g_hfxCompressor); break;
                case DSPEffectType::StereoWidth:  RemoveDSPHandle(g_hdspStereoWidth); break;
                case DSPEffectType::CenterCancel: RemoveDSPHandle(g_hdspCenterCancel); break;
                case DSPEffectType::Convolution:  RemoveDSPHandle(g_hdspConvolution); break;
                case DSPEffectType::SpatialAudio: RemoveDSPHandle(g_hdspSpatialAudio); break;
                default: break;
            }
        }
    }
}

// Stereo width DSP callback - uses Mid/Side processing
// Width 0% = mono, 100% = normal stereo, 200% = extra wide
static void CALLBACK StereoWidthDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    // Get channel info to check format
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    // Snapshot parameter once for thread safety (aligned float read is atomic on x86)
    float width = g_paramValues[(int)ParamId::StereoWidth] / 100.0f;

    // Check if float format (BASS_FX tempo streams use float)
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        DWORD frameCount = length / (sizeof(float) * 2);  // 2 channels

        for (DWORD i = 0; i < frameCount; i++) {
            float left = samples[i * 2];
            float right = samples[i * 2 + 1];

            // Convert to Mid/Side
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f;

            // Apply width to side signal
            side *= width;

            // Convert back to Left/Right
            samples[i * 2] = mid + side;
            samples[i * 2 + 1] = mid - side;
        }
    } else {
        // 16-bit format
        short* samples = static_cast<short*>(buffer);
        DWORD frameCount = length / (sizeof(short) * 2);  // 2 channels

        for (DWORD i = 0; i < frameCount; i++) {
            float left = samples[i * 2] / 32768.0f;
            float right = samples[i * 2 + 1] / 32768.0f;

            // Convert to Mid/Side
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f;

            // Apply width to side signal
            side *= width;

            // Convert back to Left/Right and clamp
            float outL = mid + side;
            float outR = mid - side;
            if (outL > 1.0f) outL = 1.0f; else if (outL < -1.0f) outL = -1.0f;
            if (outR > 1.0f) outR = 1.0f; else if (outR < -1.0f) outR = -1.0f;

            samples[i * 2] = static_cast<short>(outL * 32767.0f);
            samples[i * 2 + 1] = static_cast<short>(outR * 32767.0f);
        }
    }
}

// Center cancel/extract DSP callback - FFT-based spectral processing
// -100% = extract center (isolate vocals), 0% = no effect, +100% = cancel center (remove vocals)
static void CALLBACK CenterCancelDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    // Get channel info to check format
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    // Snapshot parameter once for thread safety (aligned float read is atomic on x86)
    float amount = g_paramValues[(int)ParamId::CenterCancel] / 100.0f;

    // Get or initialize the processor
    CenterCancelProcessor* processor = GetCenterCancelProcessor();
    if (!processor) {
        InitCenterCancelProcessor((int)info.freq);
        processor = GetCenterCancelProcessor();
    }
    if (!processor || !processor->IsInitialized()) return;

    // Update the amount
    processor->SetAmount(amount);

    // If amount is 0, processor will passthrough
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        int outputFrames = 0;

        // Process in-place
        std::vector<float> tempOut(frameCount * 2);
        processor->ProcessFloat(samples, frameCount, tempOut.data(), outputFrames);

        // Copy output back (outputFrames should equal frameCount for steady-state)
        for (int i = 0; i < outputFrames * 2; i++) {
            samples[i] = tempOut[i];
        }
    } else {
        // 16-bit format
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);
        int outputFrames = 0;

        std::vector<short> tempOut(frameCount * 2);
        processor->ProcessInt16(samples, frameCount, tempOut.data(), outputFrames);

        for (int i = 0; i < outputFrames * 2; i++) {
            samples[i] = tempOut[i];
        }
    }
}

// Convolution reverb DSP callback
static void CALLBACK ConvolutionDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    ConvolutionReverb* conv = GetConvolutionReverb();
    if (!conv) return;

    // Initialize if IR is loaded but not yet initialized
    if (conv->IsLoaded() && !conv->IsInitialized()) {
        conv->Init((int)info.freq);
    }

    // Snapshot parameters once for thread safety (aligned float read is atomic on x86)
    float convMix = g_paramValues[(int)ParamId::ConvolutionMix];
    float convGain = g_paramValues[(int)ParamId::ConvolutionGain];
    conv->SetMix(convMix);
    conv->SetGain(convGain);

    // Handle both float and 16-bit formats
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        conv->Process(samples, frameCount);
    } else {
        // Convert 16-bit to float, process, convert back
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);

        std::vector<float> floatBuf(frameCount * 2);
        for (int i = 0; i < frameCount * 2; i++) {
            floatBuf[i] = samples[i] / 32768.0f;
        }

        conv->Process(floatBuf.data(), frameCount);

        for (int i = 0; i < frameCount * 2; i++) {
            float val = floatBuf[i];
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
            samples[i] = static_cast<short>(val * 32767.0f);
        }
    }
}

// 3D Audio DSP callback - HRTF binaural rendering via Steam Audio
#ifdef USE_STEAM_AUDIO
static volatile int g_spatialCrashStep = 0;

static void SpatialAudioDSPProcInner(HDSP handle, DWORD channel, void* buffer, DWORD length) {
    g_spatialCrashStep = 1;  // entered callback
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;
    if (info.chans != 2) return;

    g_spatialCrashStep = 2;  // got channel info
    SpatialAudio* spatial = GetSpatialAudio();
    if (!spatial || !spatial->IsInitialized()) return;

    g_spatialCrashStep = 3;  // spatial ready
    // Snapshot parameter once for thread safety (aligned float read is atomic on x86)
    float blend = g_paramValues[(int)ParamId::SpatialBlend] / 100.0f;
    if (blend <= 0.0f) return;

    g_spatialCrashStep = 4;  // about to process
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        g_spatialCrashStep = 5;  // float path, calling Process
        spatial->Process(samples, frameCount, blend);
        g_spatialCrashStep = 6;  // Process returned OK
    } else {
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);
        int totalSamples = frameCount * 2;
        g_spatialCrashStep = 7;  // int16 path, getting conv buffer
        float* floatBuf = spatial->GetConversionBuffer(totalSamples);
        if (!floatBuf) return;
        g_spatialCrashStep = 8;  // converting to float
        for (int i = 0; i < totalSamples; i++)
            floatBuf[i] = samples[i] / 32768.0f;
        g_spatialCrashStep = 9;  // calling Process (int16)
        spatial->Process(floatBuf, frameCount, blend);
        g_spatialCrashStep = 10; // converting back
        for (int i = 0; i < totalSamples; i++) {
            float v = floatBuf[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            samples[i] = static_cast<short>(v * 32767.0f);
        }
        g_spatialCrashStep = 11; // int16 done
    }
}

// Wrap in SEH to catch delay-load failures or access violations from Steam Audio
static void CALLBACK SpatialAudioDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    DWORD exCode = 0;
    __try {
        SpatialAudioDSPProcInner(handle, channel, buffer, length);
    } __except(exCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        // Steam Audio crashed - disable the effect to prevent repeated crashes
        g_dspEnabled[(int)DSPEffectType::SpatialAudio] = false;
        if (g_hdspSpatialAudio) {
            BASS_ChannelRemoveDSP(channel, g_hdspSpatialAudio);
            g_hdspSpatialAudio = 0;
        }
        int innerStep = 0;
        SpatialAudio* sp = GetSpatialAudio();
        if (sp) innerStep = sp->m_debugStep;
        char msg[256];
        snprintf(msg, sizeof(msg), "3D Audio crashed at step %d dot %d with exception 0x%08X. "
                 "120 is fill, 130 is deinterleave, 150 is surround, 160 is binaural, 170 is done.",
                 g_spatialCrashStep, innerStep, (unsigned int)exCode);
        Speak(msg);
    }
}
#endif

// Volume DSP - runs LAST (very low priority) so encoder captures full volume
// This allows recording at full volume while playback respects g_volume/g_muted
// Only used when legacy volume mode is disabled
static void CALLBACK VolumeDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    (void)handle; (void)channel; (void)user;

    // Snapshot globals once for thread safety (aligned reads are atomic on x86)
    bool legacyVol = g_legacyVolume;
    bool muted = g_muted;
    float vol = g_volume;

    // Skip if using legacy volume (handled by BASS_ATTRIB_VOL instead)
    if (legacyVol) return;

    float volume = muted ? 0.0f : vol;
    if (volume == 1.0f) return; // No processing needed at full volume

    // Apply perceptual volume curve (quadratic) to match BASS_ATTRIB_VOL behavior
    // This makes lower volumes feel more gradual and natural
    float curvedVolume = volume * volume;

    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int sampleCount = length / sizeof(float);
        for (int i = 0; i < sampleCount; i++) {
            samples[i] *= curvedVolume;
        }
    } else {
        short* samples = static_cast<short*>(buffer);
        int sampleCount = length / sizeof(short);
        for (int i = 0; i < sampleCount; i++) {
            samples[i] = static_cast<short>(samples[i] * curvedVolume);
        }
    }
}

bool IsDSPEffectEnabled(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return false;
    // Reverb uses g_reverbAlgorithm instead of g_dspEnabled
    if (type == DSPEffectType::Reverb) {
        return g_reverbAlgorithm > 0;
    }
    return g_dspEnabled[(int)type];
}

// Walk the per-DSP-type enabled flags and attach any missing handles to
// g_fxStream. Idempotent — for each effect, only attaches if the handle is
// currently 0 (so calling this twice doesn't add duplicate FX). Called once
// after stream creation and again whenever an effect is toggled on so the
// new handle gets created. Must be paired with RemoveDSPEffects() before
// freeing g_fxStream or the handles dangle.
void ApplyDSPEffects() {
    if (!g_fxStream) return;

    // Reverb (based on selected algorithm)
    if (g_reverbAlgorithm > 0 && !g_hfxReverb) {
        switch (g_reverbAlgorithm) {
            case 1:  // Freeverb — Schroeder/Moorer-style algorithmic reverb,
                     // dense early reflections + diffuse tail. Best general
                     // choice; sounds natural on music and speech.
                g_hfxReverb = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_FREEVERB, 0);
                if (g_hfxReverb) {
                    BASS_BFX_FREEVERB reverb;
                    reverb.fDryMix = 1.0f - (g_paramValues[(int)ParamId::ReverbMix] / 100.0f);
                    reverb.fWetMix = g_paramValues[(int)ParamId::ReverbMix] / 100.0f * 3.0f;
                    reverb.fRoomSize = g_paramValues[(int)ParamId::ReverbRoom] / 100.0f;
                    reverb.fDamp = g_paramValues[(int)ParamId::ReverbDamp] / 100.0f;
                    reverb.fWidth = 1.0f;
                    reverb.lMode = 0;
                    reverb.lChannel = BASS_BFX_CHANALL;
                    BASS_FXSetParameters(g_hfxReverb, &reverb);
                }
                break;
            case 2:  // DX8 Reverb — DirectX 8 "Waves" simple reverb. Cheap
                     // CPU; controls limited to time/HF-ratio/mix.
                g_hfxReverb = BASS_ChannelSetFX(g_fxStream, BASS_FX_DX8_REVERB, 0);
                if (g_hfxReverb) {
                    BASS_DX8_REVERB reverb;
                    reverb.fInGain = 0.0f;  // No input gain reduction
                    reverb.fReverbMix = g_paramValues[(int)ParamId::DX8ReverbMix];
                    reverb.fReverbTime = g_paramValues[(int)ParamId::DX8ReverbTime];
                    reverb.fHighFreqRTRatio = g_paramValues[(int)ParamId::DX8ReverbHFRatio];
                    BASS_FXSetParameters(g_hfxReverb, &reverb);
                }
                break;
            case 3:  // I3DL2 Reverb — IASIG Interactive 3D Audio Level 2
                     // preset reverb (room/decay/diffusion/density). Richer
                     // than plain DX8 reverb; uses many more params (we set
                     // the seldom-touched ones to sensible defaults below).
                g_hfxReverb = BASS_ChannelSetFX(g_fxStream, BASS_FX_DX8_I3DL2REVERB, 0);
                if (g_hfxReverb) {
                    BASS_DX8_I3DL2REVERB reverb;
                    reverb.lRoom = static_cast<int>(g_paramValues[(int)ParamId::I3DL2Room]);
                    reverb.lRoomHF = 0;
                    reverb.flRoomRolloffFactor = 0.0f;
                    reverb.flDecayTime = g_paramValues[(int)ParamId::I3DL2DecayTime];
                    reverb.flDecayHFRatio = 0.83f;
                    reverb.lReflections = -2602;
                    reverb.flReflectionsDelay = 0.007f;
                    reverb.lReverb = 200;
                    reverb.flReverbDelay = 0.011f;
                    reverb.flDiffusion = g_paramValues[(int)ParamId::I3DL2Diffusion];
                    reverb.flDensity = g_paramValues[(int)ParamId::I3DL2Density];
                    reverb.flHFReference = 5000.0f;
                    BASS_FXSetParameters(g_hfxReverb, &reverb);
                }
                break;
        }
    }

    // Echo — BFX_ECHO4 is the stereo-aware echo with separate delay/feedback
    // per channel. We feed it identical params on both channels for now
    // (lChannel = BASS_BFX_CHANALL) since there's no UI for asymmetric echo.
    if (g_dspEnabled[(int)DSPEffectType::Echo] && !g_hfxEcho) {
        g_hfxEcho = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_ECHO4, 0);
        if (g_hfxEcho) {
            BASS_BFX_ECHO4 echo;
            echo.fDryMix = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
            echo.fWetMix = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
            echo.fFeedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
            echo.fDelay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;  // Convert ms to seconds
            echo.bStereo = TRUE;
            echo.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(g_hfxEcho, &echo);
        }
    }

    // EQ — 3-band (bass/mid/treble) using BFX_PEAKEQ, preceded by a
    // BFX_VOLUME stage acting as preamp. The preamp lets users compensate
    // for the clipping headroom lost when boosting bands at +15 dB. Each
    // band's center frequency is configurable in Options (g_eqBassFreq etc).
    if (g_dspEnabled[(int)DSPEffectType::EQ]) {
        // Preamp: BFX_VOLUME applied first in the chain to attenuate before
        // peak EQ boosts, preventing clipping at the EQ output.
        if (!g_hfxEQPreamp) {
            g_hfxEQPreamp = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_VOLUME, 0);
            if (g_hfxEQPreamp) {
                BASS_BFX_VOLUME vol = {0};
                vol.lChannel = BASS_BFX_CHANALL;
                // Convert dB to linear: linear = 10^(dB/20)
                vol.fVolume = powf(10.0f, g_paramValues[(int)ParamId::EQPreamp] / 20.0f);
                BASS_FXSetParameters(g_hfxEQPreamp, &vol);
            }
        }
        // Bass / Mid / Treble: identical peaking-EQ band setup, just different
        // center frequency + gain parameter.
        auto applyEqBand = [](HFX& handle, float centerFreq, ParamId gainParam) {
            if (handle) return;
            handle = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (!handle) return;
            BASS_BFX_PEAKEQ eq = {0};
            eq.lBand = 0;
            eq.fBandwidth = 2.5f;  // Octaves
            eq.fQ = 0.0f;
            eq.fCenter = centerFreq;
            eq.fGain = g_paramValues[(int)gainParam];
            eq.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(handle, &eq);
        };
        applyEqBand(g_hfxEQBass,   g_eqBassFreq,   ParamId::EQBass);
        applyEqBand(g_hfxEQMid,    g_eqMidFreq,    ParamId::EQMid);
        applyEqBand(g_hfxEQTreble, g_eqTrebleFreq, ParamId::EQTreble);
    }

    // Compressor — BFX_COMPRESSOR2 is the standard threshold/ratio/attack/
    // release dynamic-range compressor with makeup gain. Useful for taming
    // loud sections in audiobooks and podcasts, or evening out music dynamics.
    if (g_dspEnabled[(int)DSPEffectType::Compressor] && !g_hfxCompressor) {
        g_hfxCompressor = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_COMPRESSOR2, 0);
        if (g_hfxCompressor) {
            BASS_BFX_COMPRESSOR2 comp = {0};
            comp.fGain = g_paramValues[(int)ParamId::CompGain];
            comp.fThreshold = g_paramValues[(int)ParamId::CompThreshold];
            comp.fRatio = g_paramValues[(int)ParamId::CompRatio];
            comp.fAttack = g_paramValues[(int)ParamId::CompAttack];
            comp.fRelease = g_paramValues[(int)ParamId::CompRelease];
            comp.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(g_hfxCompressor, &comp);
        }
    }

    // Stereo Width (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::StereoWidth] && !g_hdspStereoWidth) {
        g_hdspStereoWidth = BASS_ChannelSetDSP(g_fxStream, StereoWidthDSPProc, nullptr, 0);
    }

    // Center Cancel/Extract (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::CenterCancel] && !g_hdspCenterCancel) {
        g_hdspCenterCancel = BASS_ChannelSetDSP(g_fxStream, CenterCancelDSPProc, nullptr, 0);
    }

    // Convolution Reverb (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::Convolution] && !g_hdspConvolution) {
        ConvolutionReverb* conv = GetConvolutionReverb();
        if (conv && conv->IsLoaded()) {
            BASS_CHANNELINFO info;
            if (BASS_ChannelGetInfo(g_fxStream, &info)) {
                conv->Init((int)info.freq);
            }
        }
        g_hdspConvolution = BASS_ChannelSetDSP(g_fxStream, ConvolutionDSPProc, nullptr, 0);
    }

    // 3D Audio (Steam Audio HRTF)
#ifdef USE_STEAM_AUDIO
    if (g_dspEnabled[(int)DSPEffectType::SpatialAudio] && !g_hdspSpatialAudio) {
        bool initOk = false;
        SpatialAudio* spatial = GetSpatialAudio();
        if (spatial) {
            BASS_CHANNELINFO info;
            if (BASS_ChannelGetInfo(g_fxStream, &info)) {
                initOk = spatial->Initialize((int)info.freq);
                if (!initOk) {
                    const wchar_t* err = spatial->GetLastError();
                    if (err && err[0]) {
                        MessageBoxW(GetMessageBoxOwner(), err, T("3D Audio Error"), MB_OK | MB_ICONERROR);
                    }
                    g_dspEnabled[(int)DSPEffectType::SpatialAudio] = false;
                }
            }
        }
        if (initOk) {
            g_hdspSpatialAudio = BASS_ChannelSetDSP(g_fxStream, SpatialAudioDSPProc, nullptr, 0);
        }
    }
#endif

    // Legacy volume mode - apply volume directly to stream attribute
    // This must be done every time a new stream is created
    if (g_legacyVolume) {
        float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
    }

    // Volume DSP — runs LAST in the chain (very low priority) so that the
    // BASS_Encode recording path, which taps in earlier at the default
    // priority of 0, sees full-volume samples. Without this the recorded
    // file would track the user's playback volume (mute = silent recording).
    // In legacy mode, BASS_ATTRIB_VOL is used instead — faster, but it
    // attenuates BEFORE the encoder tap, so recordings inherit the user's
    // volume slider.
    if (!g_legacyVolume && !g_hdspVolume) {
        g_hdspVolume = BASS_ChannelSetDSP(g_fxStream, VolumeDSPProc, nullptr, -2000000000);
    }
}

// Tear down every BFX/DSP handle attached to g_fxStream and zero the
// globals. Safe to call when no stream exists (RemoveFXHandle/RemoveDSPHandle
// no-op when g_fxStream is 0). MUST be called before BASS_StreamFree on
// g_fxStream — leaving handles attached to a freed stream causes undefined
// behaviour in BASS on the next ApplyDSPEffects().
void RemoveDSPEffects() {
    RemoveFXHandle(g_hfxReverb);
    RemoveFXHandle(g_hfxEcho);
    RemoveFXHandle(g_hfxEQPreamp);
    RemoveFXHandle(g_hfxEQBass);
    RemoveFXHandle(g_hfxEQMid);
    RemoveFXHandle(g_hfxEQTreble);
    RemoveFXHandle(g_hfxCompressor);
    RemoveDSPHandle(g_hdspStereoWidth);
    RemoveDSPHandle(g_hdspCenterCancel);
    RemoveDSPHandle(g_hdspConvolution);
    RemoveDSPHandle(g_hdspSpatialAudio);
    RemoveDSPHandle(g_hdspVolume);
}

// Get parameter definition
const ParamDef* GetParamDef(ParamId id) {
    for (int i = 0; i < g_paramDefCount; i++) {
        if (g_paramDefs[i].id == id) return &g_paramDefs[i];
    }
    return nullptr;
}

float GetParamValue(ParamId id) {
    // For stream effects, return the actual global values
    switch (id) {
        case ParamId::Volume: return g_volume;
        case ParamId::Pitch: return g_pitch;
        case ParamId::Tempo: return g_tempo;
        case ParamId::Rate: return g_rate;
        default: break;
    }
    // For DSP effect parameters, use the stored values
    if ((int)id < 0 || (int)id >= (int)ParamId::COUNT) return 0.0f;
    return g_paramValues[(int)id];
}

const char* GetParamName(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->name : "Unknown";
}

const char* GetParamUnit(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->unit : "";
}

// Write a new value for a parameter, clamp to the param's allowed range,
// and push the change into whatever underlying effect owns it (BASS_ATTRIB
// for stream effects, BASS_FXSetParameters for BFX, custom DSP globals for
// stereo width / center cancel / convolution / spatial).
//
// For Volume the upper bound is dynamic — it depends on g_allowAmplify
// (MAX_VOLUME_NORMAL = 1.0, MAX_VOLUME_AMPLIFY = up to 4.0). Tempo and Rate
// are silently no-op on live streams (handled by caller; this function will
// still update the stored value, but the BASS attribute write is skipped).
void SetParamValue(ParamId id, float value) {
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    float maxVal = def->maxValue;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    }

    value = clamp_val(value, def->minValue, maxVal);
    g_paramValues[(int)id] = value;

    // Apply the change
    switch (id) {
        case ParamId::Volume:
            g_volume = value;
            // v2.15 — during video, push to MPV (no BASS stream then). Mirrors
            // the engine-aware SetVolume() facade so the selected-Volume arrows
            // change the actual video volume, not just g_volume.
            if (g_activeEngine == PlaybackEngine::MPV) {
                ApplyVideoVolume();   // v2.44 — keeps subtitle duck on volume change
            }
            // In legacy mode, apply via BASS_ATTRIB_VOL
            // In normal mode, volume DSP automatically uses updated g_volume
            if (g_legacyVolume && g_fxStream) {
                float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
            }
            break;
        case ParamId::Pitch:
            g_pitch = value;
            if (g_fxStream) {
                TempoProcessor* processor = GetTempoProcessor();
                if (processor && processor->IsActive()) {
                    processor->SetPitch(g_pitch);
                }
            }
            break;
        case ParamId::Tempo:
            g_tempo = value;
            // Skip applying tempo to live streams (not supported)
            if (g_fxStream && !g_isLiveStream) {
                TempoProcessor* processor = GetTempoProcessor();
                if (processor && processor->IsActive()) {
                    processor->SetTempo(g_tempo);
                }
            }
            break;
        case ParamId::Rate:
            g_rate = value;
            // v2.15 — during video, drive the MPV "speed" property (changes
            // speed and pitch together, like BASS_ATTRIB_FREQ). Lets the Rate
            // control speed up / slow down a video.
            if (g_activeEngine == PlaybackEngine::MPV) {
                MPVSetSpeed(static_cast<double>(g_rate));
            }
            // Skip applying rate to live streams (not supported)
            if (g_fxStream && !g_isLiveStream) {
                // Use native BASS frequency attribute (changes speed and pitch together)
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
            }
            // Also drive the SAPI rate so the same control speeds up the
            // voice when reading a text-only DAISY book.
            mediaaccess::TtsSetSpeedMultiplier(g_rate);
            // v2.45 — and the Edge neural voice when a book is read that way
            // (re-synthesizes the current paragraph at the new rate).
            mediaaccess::DaisyOnSpeedChanged(static_cast<double>(g_rate));
            break;
        // Freeverb parameters
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
            if (g_hfxReverb && g_reverbAlgorithm == 1) {
                BASS_BFX_FREEVERB reverb;
                BASS_FXGetParameters(g_hfxReverb, &reverb);
                reverb.fDryMix = 1.0f - (g_paramValues[(int)ParamId::ReverbMix] / 100.0f);
                reverb.fWetMix = g_paramValues[(int)ParamId::ReverbMix] / 100.0f * 3.0f;
                reverb.fRoomSize = g_paramValues[(int)ParamId::ReverbRoom] / 100.0f;
                reverb.fDamp = g_paramValues[(int)ParamId::ReverbDamp] / 100.0f;
                BASS_FXSetParameters(g_hfxReverb, &reverb);
            }
            break;
        // DX8 Reverb parameters
        case ParamId::DX8ReverbTime:
        case ParamId::DX8ReverbHFRatio:
        case ParamId::DX8ReverbMix:
            if (g_hfxReverb && g_reverbAlgorithm == 2) {
                BASS_DX8_REVERB reverb;
                BASS_FXGetParameters(g_hfxReverb, &reverb);
                reverb.fReverbMix = g_paramValues[(int)ParamId::DX8ReverbMix];
                reverb.fReverbTime = g_paramValues[(int)ParamId::DX8ReverbTime];
                reverb.fHighFreqRTRatio = g_paramValues[(int)ParamId::DX8ReverbHFRatio];
                BASS_FXSetParameters(g_hfxReverb, &reverb);
            }
            break;
        // I3DL2 Reverb parameters
        case ParamId::I3DL2Room:
        case ParamId::I3DL2DecayTime:
        case ParamId::I3DL2Diffusion:
        case ParamId::I3DL2Density:
            if (g_hfxReverb && g_reverbAlgorithm == 3) {
                BASS_DX8_I3DL2REVERB reverb;
                BASS_FXGetParameters(g_hfxReverb, &reverb);
                reverb.lRoom = static_cast<int>(g_paramValues[(int)ParamId::I3DL2Room]);
                reverb.flDecayTime = g_paramValues[(int)ParamId::I3DL2DecayTime];
                reverb.flDiffusion = g_paramValues[(int)ParamId::I3DL2Diffusion];
                reverb.flDensity = g_paramValues[(int)ParamId::I3DL2Density];
                BASS_FXSetParameters(g_hfxReverb, &reverb);
            }
            break;
        case ParamId::EchoDelay:
        case ParamId::EchoFeedback:
        case ParamId::EchoMix:
            if (g_hfxEcho) {
                BASS_BFX_ECHO4 echo;
                BASS_FXGetParameters(g_hfxEcho, &echo);
                echo.fDryMix = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
                echo.fWetMix = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
                echo.fFeedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
                echo.fDelay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;
                BASS_FXSetParameters(g_hfxEcho, &echo);
            }
            break;
        case ParamId::EQPreamp:
            if (g_hfxEQPreamp) {
                BASS_BFX_VOLUME vol = {0};
                vol.lChannel = BASS_BFX_CHANALL;
                vol.fVolume = powf(10.0f, value / 20.0f);
                BASS_FXSetParameters(g_hfxEQPreamp, &vol);
            }
            break;
        case ParamId::EQBass:
            if (g_hfxEQBass) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQBass, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQBass, &eq);
            }
            break;
        case ParamId::EQMid:
            if (g_hfxEQMid) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQMid, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQMid, &eq);
            }
            break;
        case ParamId::EQTreble:
            if (g_hfxEQTreble) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQTreble, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQTreble, &eq);
            }
            break;
        case ParamId::CompThreshold:
        case ParamId::CompRatio:
        case ParamId::CompAttack:
        case ParamId::CompRelease:
        case ParamId::CompGain:
            if (g_hfxCompressor) {
                BASS_BFX_COMPRESSOR2 comp = {0};
                BASS_FXGetParameters(g_hfxCompressor, &comp);
                comp.fThreshold = g_paramValues[(int)ParamId::CompThreshold];
                comp.fRatio = g_paramValues[(int)ParamId::CompRatio];
                comp.fAttack = g_paramValues[(int)ParamId::CompAttack];
                comp.fRelease = g_paramValues[(int)ParamId::CompRelease];
                comp.fGain = g_paramValues[(int)ParamId::CompGain];
                BASS_FXSetParameters(g_hfxCompressor, &comp);
            }
            break;
    #ifdef USE_STEAM_AUDIO
        case ParamId::SpatialMode: {
            SpatialAudio* spatial = GetSpatialAudio();
            if (spatial) spatial->SetMode(value >= 0.5f ? SpatialMode::Surround51 : SpatialMode::Binaural);
            break;
        }
        case ParamId::SpatialRearCenter: {
            SpatialAudio* spatial = GetSpatialAudio();
            if (spatial) spatial->SetRearCenter(value >= 0.5f);
            break;
        }
    #endif
        default:
            break;
    }
}

void CycleParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();

    if (params.empty()) {
        Speak(Ts("No parameters available"));
        return;
    }

    // Find current param in available list
    int currentIdx = -1;
    for (int i = 0; i < (int)params.size(); i++) {
        if ((int)params[i] == g_currentParamIndex) {
            currentIdx = i;
            break;
        }
    }

    // If current not found, start at beginning
    if (currentIdx < 0) {
        currentIdx = 0;
    } else {
        currentIdx += direction;
        // Clamp to bounds (no wrapping)
        if (currentIdx < 0) currentIdx = 0;
        if (currentIdx >= (int)params.size()) currentIdx = (int)params.size() - 1;
    }

    g_currentParamIndex = (int)params[currentIdx];
    AnnounceCurrentParam();
}

void AdjustCurrentParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // v2.15 — during video (MPV) only Volume and Rate apply; all other effects
    // are BASS DSP that cannot reach MPV. Refuse clearly and return BEFORE any
    // global is mutated, so an "adjustment" on a video never leaks into the next
    // audio track. Reported by user (Ctrl+1..0 + arrows dead on video).
    if (g_activeEngine == PlaybackEngine::MPV &&
        id != ParamId::Volume && id != ParamId::Rate) {
        Speak(Ts("Not available for video"));
        return;
    }

    // Block tempo and rate adjustments for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak(Ts("Not available for live streams"));
        return;
    }

    // For Volume, respect the allow amplify setting and use g_volumeStep
    float maxVal = def->maxValue;
    float step = def->step;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
        step = g_volumeStep;  // Use configurable volume step
    }

    float currentVal = GetParamValue(id);
    float newVal;

    // Handle Rate with semitone stepping if enabled
    if (id == ParamId::Rate && g_rateStepMode == 1) {
        // Semitone ratio = 2^(1/12) ≈ 1.0594630943592953
        const float semitoneRatio = 1.0594630943592953f;
        if (direction > 0) {
            newVal = currentVal * semitoneRatio;
        } else {
            newVal = currentVal / semitoneRatio;
        }
    } else {
        newVal = currentVal + (direction * step);
    }

    // 3D Rotation, Mode, and Rear Speaker wrap around instead of clamping
    // so the user can cycle through modes or rotate continuously.
    if (id == ParamId::SpatialRotation) {
        // Angular: ±180° meet, so full range = max - min
        float range = def->maxValue - def->minValue;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else if (id == ParamId::SpatialMode || id == ParamId::SpatialRearCenter) {
        // Discrete toggle: add step so past-max wraps to min
        float range = def->maxValue - def->minValue + def->step;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else {
        newVal = clamp_val(newVal, def->minValue, maxVal);
    }

    SetParamValue(id, newVal);
    AnnounceCurrentParam();
}

void ResetCurrentParam() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // v2.15 — video (MPV): only Volume and Rate apply (see AdjustCurrentParam).
    if (g_activeEngine == PlaybackEngine::MPV &&
        id != ParamId::Volume && id != ParamId::Rate) {
        Speak(Ts("Not available for video"));
        return;
    }

    // Block tempo and rate reset for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak(Ts("Not available for live streams"));
        return;
    }

    SetParamValue(id, def->defaultValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMin() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // v2.15 — video (MPV): only Volume and Rate apply (see AdjustCurrentParam).
    if (g_activeEngine == PlaybackEngine::MPV &&
        id != ParamId::Volume && id != ParamId::Rate) {
        Speak(Ts("Not available for video"));
        return;
    }

    // Block tempo and rate min for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak(Ts("Not available for live streams"));
        return;
    }

    SetParamValue(id, def->minValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMax() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // v2.15 — video (MPV): only Volume and Rate apply (see AdjustCurrentParam).
    if (g_activeEngine == PlaybackEngine::MPV &&
        id != ParamId::Volume && id != ParamId::Rate) {
        Speak(Ts("Not available for video"));
        return;
    }

    // Block tempo and rate max for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak(Ts("Not available for live streams"));
        return;
    }

    // Use maxValue, but respect g_allowAmplify for volume
    float maxVal = def->maxValue;
    if (id == ParamId::Volume && !g_allowAmplify && maxVal > 1.0f) {
        maxVal = 1.0f;
    }

    SetParamValue(id, maxVal);
    AnnounceCurrentParam();
}

void AnnounceCurrentParam() {
    if (!g_speechEffect) return;

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    float val = GetParamValue(id);

    // String-valued special cases: route every label and every choice word
    // through Ts() so a French user hears "Mode 3D : Binaural" instead of
    // "3D Mode: Binaural".
    if (id == ParamId::SpatialMode) {
        std::string mode = val >= 0.5f ? Ts("5.1 Surround") : Ts("Binaural");
        Speak(Ts(def->name) + ": " + mode);
        return;
    }
    if (id == ParamId::SpatialRearCenter) {
        std::string state = val >= 0.5f ? Ts("On") : Ts("Off");
        Speak(Ts(def->name) + ": " + state);
        return;
    }

    // Numeric value formatted separately from name/unit so name and unit can
    // be looked up through Ts(). Number format is locale-neutral; we keep
    // the same precision rules as before.
    char numBuf[32];
    if (id == ParamId::Volume) {
        snprintf(numBuf, sizeof(numBuf), "%d", (int)(val * 100 + 0.5f));
    } else if (id == ParamId::Rate) {
        snprintf(numBuf, sizeof(numBuf), "%.2f", val);
    } else if (id == ParamId::Pitch || id == ParamId::EQBass || id == ParamId::EQMid || id == ParamId::EQTreble) {
        snprintf(numBuf, sizeof(numBuf), "%+.0f", val);
    } else {
        snprintf(numBuf, sizeof(numBuf), "%.0f", val);
    }

    std::string msg = Ts(def->name) + " " + numBuf;
    if (def->unit && def->unit[0] != '\0') {
        // Units like "%", "x", ":1", "ms", "Hz", "dB" are universal; the
        // Ts() fallback returns the source string when no translation is
        // registered, so this also works for those. Units that DO translate
        // (" semitones" → " demi-tons", " deg" → " degrés") are registered
        // in translations_player.cpp with their leading space preserved.
        msg += Ts(def->unit);
    }
    Speak(msg);
}

void ResetEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        SetParamValue(g_paramDefs[i].id, g_paramDefs[i].defaultValue);
    }
}

// Legacy compatibility functions
float GetEffectValue(EffectType type) {
    switch (type) {
        case EffectType::Volume: return g_volume;
        case EffectType::Pitch: return g_pitch;
        case EffectType::Tempo: return g_tempo;
        case EffectType::Rate: return g_rate;
        default: return 0.0f;
    }
}

const char* GetEffectName(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "Volume";
        case EffectType::Pitch: return "Pitch";
        case EffectType::Tempo: return "Tempo";
        case EffectType::Rate: return "Rate";
        default: return "Unknown";
    }
}

const char* GetEffectUnit(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "%";
        case EffectType::Pitch: return " semitones";
        case EffectType::Tempo: return "%";
        case EffectType::Rate: return "x";
        default: return "";
    }
}

void SetEffectValue(EffectType type, float value) {
    switch (type) {
        case EffectType::Volume: SetParamValue(ParamId::Volume, value); break;
        case EffectType::Pitch: SetParamValue(ParamId::Pitch, value); break;
        case EffectType::Tempo: SetParamValue(ParamId::Tempo, value); break;
        case EffectType::Rate: SetParamValue(ParamId::Rate, value); break;
        default: break;
    }
}

void CycleEffect(int direction) {
    CycleParam(direction);
}

void AdjustCurrentEffect(int direction) {
    AdjustCurrentParam(direction);
}

// ---------------------------------------------------------------------------
// Effect presets: save/load/delete named sets of all enabled effects + params.
// Stored in MediaAccess.ini under:
//   [Presets]            — Count, Name0, Name1, ... (the preset index)
//   [Preset_<name>]      — one section per preset, holds StreamEnabled*,
//                          Pitch, Tempo, Rate, ReverbAlgorithm, DSPEnabled*,
//                          and Param<id>= values for every DSP param.
// Volume is deliberately excluded from presets — a loud preset shouldn't
// hijack the user's playback volume on apply.
// ---------------------------------------------------------------------------

static std::wstring PresetSectionName(const std::wstring& name) {
    return L"Preset_" + name;
}

static float GetPrivateProfileFloatW_Preset(const wchar_t* section, const wchar_t* key, float defaultVal) {
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(section, key, L"", buf, 64, g_configPath.c_str());
    if (buf[0] == L'\0') return defaultVal;
    return (float)_wtof(buf);
}

std::vector<std::wstring> GetEffectPresetNames() {
    std::vector<std::wstring> names;
    int count = GetPrivateProfileIntW(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        wchar_t buf[128] = {0};
        GetPrivateProfileStringW(L"Presets", key, L"", buf, 128, g_configPath.c_str());
        if (buf[0] != L'\0') names.push_back(buf);
    }
    return names;
}

static void WritePresetNameList(const std::vector<std::wstring>& names) {
    // Clear old name entries first
    int oldCount = GetPrivateProfileIntW(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < oldCount; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        WritePrivateProfileStringW(L"Presets", key, nullptr, g_configPath.c_str());
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", (int)names.size());
    WritePrivateProfileStringW(L"Presets", L"Count", buf, g_configPath.c_str());
    for (size_t i = 0; i < names.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%zu", i);
        WritePrivateProfileStringW(L"Presets", key, names[i].c_str(), g_configPath.c_str());
    }
}

bool SaveEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;

    std::wstring section = PresetSectionName(name);
    wchar_t buf[64];

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        WritePrivateProfileStringW(section.c_str(), key, g_effectEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // Stream effect values (pitch, tempo, rate — volume intentionally excluded)
    swprintf(buf, 64, L"%.4f", g_pitch);
    WritePrivateProfileStringW(section.c_str(), L"Pitch", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_tempo);
    WritePrivateProfileStringW(section.c_str(), L"Tempo", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_rate);
    WritePrivateProfileStringW(section.c_str(), L"Rate", buf, g_configPath.c_str());

    // Reverb algorithm
    swprintf(buf, 64, L"%d", g_reverbAlgorithm);
    WritePrivateProfileStringW(section.c_str(), L"ReverbAlgorithm", buf, g_configPath.c_str());

    // DSP effect enabled flags
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        WritePrivateProfileStringW(section.c_str(), key,
            g_dspEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // All param values (DSP params plus stream effect params)
    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];
        // Skip volume - presets shouldn't hijack playback volume
        if (def.id == ParamId::Volume) continue;
        wchar_t key[64];
        swprintf(key, 64, L"Param%d", (int)def.id);
        swprintf(buf, 64, L"%.6f", g_paramValues[(int)def.id]);
        WritePrivateProfileStringW(section.c_str(), key, buf, g_configPath.c_str());
    }

    // Add to name list if not already present
    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto& n : names) { if (n == name) { found = true; break; } }
    if (!found) {
        names.push_back(name);
        WritePresetNameList(names);
    }
    return true;
}

bool LoadEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);

    // Quick existence check
    wchar_t test[8] = {0};
    GetPrivateProfileStringW(section.c_str(), L"Pitch", L"__MISSING__", test, 8, g_configPath.c_str());
    if (wcscmp(test, L"__MISSING__") == 0) return false;

    // Stream effect values
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(section.c_str(), L"Pitch", L"0", buf, 64, g_configPath.c_str());
    g_pitch = (float)_wtof(buf);
    GetPrivateProfileStringW(section.c_str(), L"Tempo", L"0", buf, 64, g_configPath.c_str());
    g_tempo = (float)_wtof(buf);
    GetPrivateProfileStringW(section.c_str(), L"Rate", L"1", buf, 64, g_configPath.c_str());
    g_rate = (float)_wtof(buf);

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        g_effectEnabled[i] = GetPrivateProfileIntW(section.c_str(), key,
            g_effectEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
    }

    // Reverb algorithm
    int ra = GetPrivateProfileIntW(section.c_str(), L"ReverbAlgorithm", g_reverbAlgorithm, g_configPath.c_str());
    if (ra < 0) ra = 0;
    if (ra > 3) ra = 3;
    SetReverbAlgorithm(ra);

    // DSP effect enabled flags (apply via EnableDSPEffect so handlers hook up properly)
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        if ((DSPEffectType)i == DSPEffectType::Reverb) continue;  // controlled by algorithm
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        bool en = GetPrivateProfileIntW(section.c_str(), key,
            g_dspEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
        EnableDSPEffect((DSPEffectType)i, en);
    }

    // All param values (via SetParamValue so effects update live)
    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];
        if (def.id == ParamId::Volume) continue;
        wchar_t key[64];
        swprintf(key, 64, L"Param%d", (int)def.id);
        float val = GetPrivateProfileFloatW_Preset(section.c_str(), key, g_paramValues[(int)def.id]);
        SetParamValue(def.id, val);
    }

    return true;
}

bool DeleteEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);
    // Wipe the preset's entire section
    WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, g_configPath.c_str());

    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto it = names.begin(); it != names.end(); ) {
        if (*it == name) { it = names.erase(it); found = true; } else { ++it; }
    }
    if (found) WritePresetNameList(names);
    return found;
}
