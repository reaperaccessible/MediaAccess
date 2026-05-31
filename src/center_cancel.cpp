#include "center_cancel.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Singleton, owned by FreeCenterCancelProcessor() at shutdown.
// Unlike the convolution singleton this one is NOT lazily created — the DSP
// callback (effects.cpp::CenterCancelDSPProc) must call InitCenterCancelProcessor()
// first, so GetCenterCancelProcessor() returns null until init runs.
static CenterCancelProcessor* g_centerCancelProcessor = nullptr;

CenterCancelProcessor* GetCenterCancelProcessor() {
    return g_centerCancelProcessor;
}

void InitCenterCancelProcessor(int sampleRate) {
    if (!g_centerCancelProcessor) {
        g_centerCancelProcessor = new CenterCancelProcessor();
    }
    g_centerCancelProcessor->Init(sampleRate);
}

void FreeCenterCancelProcessor() {
    delete g_centerCancelProcessor;
    g_centerCancelProcessor = nullptr;
}

CenterCancelProcessor::CenterCancelProcessor()
    : m_initialized(false)
    , m_sampleRate(44100)
    , m_fftSize(4096)
    , m_hopSize(1024)
    , m_amount(0.0f)
    , m_inputPos(0)
    , m_outputPos(0)
    , m_outputAvail(0)
{
}

CenterCancelProcessor::~CenterCancelProcessor() {
}

// Allocate FFT scratch buffers and build the Hann analysis/synthesis window.
// Default fftSize=4096 with hopSize = fftSize/4 gives 75% overlap, which is
// the standard sweet spot for Hann-windowed STFT: yields perfect (or near-
// perfect after the scale factor in ProcessFrame()) overlap-add reconstruction
// when the signal is passed through unmodified.
bool CenterCancelProcessor::Init(int sampleRate, int fftSize) {
    m_sampleRate = sampleRate;
    m_fftSize = fftSize;
    m_hopSize = fftSize / 4;  // 75% overlap

    m_inputBufferL.resize(fftSize, 0.0f);
    m_inputBufferR.resize(fftSize, 0.0f);

    // Output ring needs > fftSize because the overlap-add can wrap further
    // forward than a single FFT length when frames pile up at process time.
    int outputBufSize = fftSize * 2;
    m_outputBufferL.resize(outputBufSize, 0.0f);
    m_outputBufferR.resize(outputBufSize, 0.0f);

    m_fftL.resize(fftSize);
    m_fftR.resize(fftSize);

    // Hann window — used both for analysis (pre-FFT) and synthesis (post-IFFT)
    // so the effective per-sample window is Hann^2; the scale factor in
    // ProcessFrame() compensates so unprocessed audio plays back at unity gain.
    m_window.resize(fftSize);
    for (int i = 0; i < fftSize; i++) {
        m_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (fftSize - 1)));
    }

    m_inputPos = 0;
    m_outputPos = 0;
    m_outputAvail = 0;
    m_initialized = true;

    return true;
}

void CenterCancelProcessor::Reset() {
    if (!m_initialized) return;

    std::fill(m_inputBufferL.begin(), m_inputBufferL.end(), 0.0f);
    std::fill(m_inputBufferR.begin(), m_inputBufferR.end(), 0.0f);
    std::fill(m_outputBufferL.begin(), m_outputBufferL.end(), 0.0f);
    std::fill(m_outputBufferR.begin(), m_outputBufferR.end(), 0.0f);

    m_inputPos = 0;
    m_outputPos = 0;
    m_outputAvail = 0;
}

// In-place radix-2 Cooley-Tukey FFT (or IFFT when inverse=true).
// n must be a power of two — here it's always m_fftSize (default 4096).
// Same implementation as convolution.cpp::FFT; kept duplicated for now since
// extracting a header for a 30-line function is overkill.
void CenterCancelProcessor::FFT(std::complex<float>* data, int n, bool inverse) {
    // Bit-reversal permutation so the iterative butterflies write outputs
    // in natural order.
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            std::swap(data[i], data[j]);
        }
        int k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }

    // Cooley-Tukey iterative FFT
    for (int len = 2; len <= n; len *= 2) {
        float angle = (inverse ? 2.0f : -2.0f) * (float)M_PI / len;
        std::complex<float> wn(cosf(angle), sinf(angle));

        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int jj = 0; jj < len / 2; jj++) {
                std::complex<float> u = data[i + jj];
                std::complex<float> t = w * data[i + jj + len / 2];
                data[i + jj] = u + t;
                data[i + jj + len / 2] = u - t;
                w *= wn;
            }
        }
    }

    // Scale for inverse FFT
    if (inverse) {
        for (int i = 0; i < n; i++) {
            data[i] /= (float)n;
        }
    }
}

void CenterCancelProcessor::ApplyWindow(float* data, int n) {
    for (int i = 0; i < n; i++) {
        data[i] *= m_window[i];
    }
}

// Run one STFT frame: window → FFT → spectral Mid/Side processing → IFFT →
// windowed overlap-add into the output ring buffer. Called by ProcessFloat()
// whenever m_inputBufferL/R is full.
void CenterCancelProcessor::ProcessFrame() {
    // Window the time-domain input as the real part of a complex FFT input.
    for (int i = 0; i < m_fftSize; i++) {
        m_fftL[i] = std::complex<float>(m_inputBufferL[i] * m_window[i], 0.0f);
        m_fftR[i] = std::complex<float>(m_inputBufferR[i] * m_window[i], 0.0f);
    }

    FFT(m_fftL.data(), m_fftSize, false);
    FFT(m_fftR.data(), m_fftSize, false);

    // Per-bin processing in frequency-domain Mid/Side. Vocals and other
    // center-panned content concentrate energy in Mid (L+R), while reverb,
    // ambience, and panned instruments dominate Side (L-R). We use that
    // imbalance to decide how much each bin is "centery", then either
    // attenuate Mid (cancel mode) or attenuate Side (extract mode).
    float amount = m_amount;
    bool cancel = amount > 0.0f;
    float strength = fabsf(amount);

    for (int i = 0; i <= m_fftSize / 2; i++) {
        std::complex<float> L = m_fftL[i];
        std::complex<float> R = m_fftR[i];

        // Convert to Mid/Side in frequency domain
        std::complex<float> Mid = (L + R) * 0.5f;
        std::complex<float> Side = (L - R) * 0.5f;

        float magMid = std::abs(Mid);
        float magSide = std::abs(Side);
        float magTotal = magMid + magSide;

        if (magTotal < 1e-10f) continue;  // Skip silent bins

        // Calculate "centerness" based on Mid/Side ratio
        // High Mid relative to Side = center content
        float centerness = magMid / (magMid + magSide + 1e-10f);

        // Also factor in phase correlation for better detection
        float phaseL = std::arg(L);
        float phaseR = std::arg(R);
        float phaseDiff = phaseL - phaseR;
        while (phaseDiff > M_PI) phaseDiff -= 2.0f * (float)M_PI;
        while (phaseDiff < -M_PI) phaseDiff += 2.0f * (float)M_PI;
        float phaseCorrelation = cosf(phaseDiff) * 0.5f + 0.5f;

        // Blend centerness with phase correlation
        centerness = centerness * 0.7f + phaseCorrelation * 0.3f;

        std::complex<float> newL, newR;

        if (cancel) {
            // Cancel center: reduce Mid, keep Side
            float midGain = 1.0f - (centerness * strength);
            midGain = std::max(0.0f, midGain);

            std::complex<float> newMid = Mid * midGain;
            // Reconstruct L/R from modified Mid and original Side
            newL = newMid + Side;
            newR = newMid - Side;
        } else {
            // Extract center: keep Mid, reduce Side
            // Simple approach: reduce side signal proportionally to strength
            // At -100% (strength=1.0): sideGain=0, only Mid remains (mono center)
            // At -50% (strength=0.5): sideGain=0.5, Side is reduced by half
            float sideGain = 1.0f - strength;
            sideGain = std::max(0.0f, sideGain);

            std::complex<float> newSide = Side * sideGain;
            // Reconstruct L/R from original Mid and reduced Side
            newL = Mid + newSide;
            newR = Mid - newSide;
        }

        m_fftL[i] = newL;
        m_fftR[i] = newR;

        // Mirror for negative frequencies (except DC and Nyquist)
        if (i > 0 && i < m_fftSize / 2) {
            m_fftL[m_fftSize - i] = std::conj(m_fftL[i]);
            m_fftR[m_fftSize - i] = std::conj(m_fftR[i]);
        }
    }

    // Inverse FFT
    FFT(m_fftL.data(), m_fftSize, true);
    FFT(m_fftR.data(), m_fftSize, true);

    // Overlap-add: apply synthesis window (so we're effectively Hann^2 across
    // analysis+synthesis, which sums to a flat envelope at 75% overlap) and
    // accumulate into the output ring. The scale factor cancels the gain from
    // 4 overlapping Hann^2 frames so unprocessed audio comes out at unity.
    float scale = 1.0f / (m_fftSize / m_hopSize * 0.5f);

    for (int i = 0; i < m_fftSize; i++) {
        int outIdx = (m_outputPos + i) % (int)m_outputBufferL.size();
        m_outputBufferL[outIdx] += m_fftL[i].real() * m_window[i] * scale;
        m_outputBufferR[outIdx] += m_fftR[i].real() * m_window[i] * scale;
    }

    m_outputAvail += m_hopSize;
}

// Process stereo float samples (L,R interleaved). Writes processed frames to
// `output` and returns the count in `outputFrames`.
//
// Behaviour by amount:
//   amount == 0     → memcpy passthrough
//   amount  < 0     → time-domain Mid/Side extract (cheap, no STFT latency)
//   amount  > 0     → FFT-based cancel via STFT pipeline (introduces fftSize
//                     latency on the first call — first ~fftSize-hopSize output
//                     samples are silence while the buffer fills)
void CenterCancelProcessor::ProcessFloat(float* input, int inputFrames, float* output, int& outputFrames) {
    if (!m_initialized || m_amount == 0.0f) {
        for (int i = 0; i < inputFrames * 2; i++) {
            output[i] = input[i];
        }
        outputFrames = inputFrames;
        return;
    }

    // Extract-only mode runs as a pure time-domain Mid/Side reduction. The
    // FFT path can also extract, but at -100% it sometimes leaks side energy
    // through bin smearing — the time-domain version produces a cleanly
    // mono'd output at full strength.
    if (m_amount < 0.0f) {
        float strength = -m_amount;  // 0.0 to 1.0
        for (int i = 0; i < inputFrames; i++) {
            float left = input[i * 2];
            float right = input[i * 2 + 1];

            // Center (mono) component
            float center = (left + right) * 0.5f;
            // Side component
            float sideL = left - center;
            float sideR = right - center;

            // Reduce side based on strength
            float sideGain = 1.0f - strength;
            output[i * 2] = center + sideL * sideGain;
            output[i * 2 + 1] = center + sideR * sideGain;
        }
        outputFrames = inputFrames;
        return;
    }

    int outIdx = 0;

    for (int i = 0; i < inputFrames; i++) {
        // Add input to buffer
        m_inputBufferL[m_inputPos] = input[i * 2];
        m_inputBufferR[m_inputPos] = input[i * 2 + 1];
        m_inputPos++;

        // When we have a full FFT frame, process it
        if (m_inputPos >= m_fftSize) {
            ProcessFrame();

            // Shift input buffer by hop size
            for (int j = 0; j < m_fftSize - m_hopSize; j++) {
                m_inputBufferL[j] = m_inputBufferL[j + m_hopSize];
                m_inputBufferR[j] = m_inputBufferR[j + m_hopSize];
            }
            m_inputPos = m_fftSize - m_hopSize;
        }

        // Output available samples
        if (m_outputAvail > 0) {
            int readIdx = m_outputPos % (int)m_outputBufferL.size();
            output[outIdx * 2] = m_outputBufferL[readIdx];
            output[outIdx * 2 + 1] = m_outputBufferR[readIdx];

            // Clear the output buffer position we just read
            m_outputBufferL[readIdx] = 0.0f;
            m_outputBufferR[readIdx] = 0.0f;

            m_outputPos++;
            m_outputAvail--;
            outIdx++;
        } else {
            // No output available yet (startup latency), output silence
            output[outIdx * 2] = 0.0f;
            output[outIdx * 2 + 1] = 0.0f;
            outIdx++;
        }
    }

    outputFrames = outIdx;
}

// 16-bit wrapper around ProcessFloat: scales s16 → float, processes, clamps,
// and scales back. Used when the BASS stream is not float-format.
void CenterCancelProcessor::ProcessInt16(short* input, int inputFrames, short* output, int& outputFrames) {
    std::vector<float> floatIn(inputFrames * 2);
    std::vector<float> floatOut(inputFrames * 2);

    for (int i = 0; i < inputFrames * 2; i++) {
        floatIn[i] = input[i] / 32768.0f;
    }

    ProcessFloat(floatIn.data(), inputFrames, floatOut.data(), outputFrames);

    for (int i = 0; i < outputFrames * 2; i++) {
        float val = floatOut[i];
        if (val > 1.0f) val = 1.0f;
        if (val < -1.0f) val = -1.0f;
        output[i] = (short)(val * 32767.0f);
    }
}
