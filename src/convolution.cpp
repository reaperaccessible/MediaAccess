#include "convolution.h"
#include "bass.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Singleton — owned by FreeConvolutionReverb at shutdown. Lazy-created on
// first GetConvolutionReverb() call so an unused effect costs nothing.
static ConvolutionReverb* g_convolutionReverb = nullptr;

ConvolutionReverb* GetConvolutionReverb() {
    if (!g_convolutionReverb) {
        g_convolutionReverb = new ConvolutionReverb();
    }
    return g_convolutionReverb;
}

// Construct (if needed) and immediately Init() the processor with the given
// output sample rate. Called by the DSP path when the effect is enabled and
// no IR has been loaded yet — Init() allocates the FFT scratch buffers.
void InitConvolutionReverb(int sampleRate) {
    if (!g_convolutionReverb) {
        g_convolutionReverb = new ConvolutionReverb();
    }
    g_convolutionReverb->Init(sampleRate);
}

void FreeConvolutionReverb() {
    delete g_convolutionReverb;
    g_convolutionReverb = nullptr;
}

ConvolutionReverb::ConvolutionReverb()
    : m_initialized(false)
    , m_irLoaded(false)
    , m_sampleRate(44100)
    , m_irSampleRate(44100)
    , m_irChannels(2)
    , m_irSamples(0)
    , m_fftSize(2048)
    , m_blockSize(1024)
    , m_numPartitions(0)
    , m_inputPos(0)
    , m_fdlPos(0)
    , m_mix(50.0f)
    , m_gain(0.0f)
{
}

ConvolutionReverb::~ConvolutionReverb() {
}

// Load an impulse response from any format BASS supports (WAV, FLAC, MP3, OGG, ...).
// Decodes the entire file into RAM, deinterleaves to L/R, then runs an FFT
// on each blockSize-sized partition so the realtime Process() path only has
// to do per-block convolutions in the frequency domain.
//
// Side effects: stores the IR spectrum in m_irSpectrumL/R, sets m_irLoaded,
// and clears m_initialized to force Init() to re-allocate scratch buffers
// against the new partition count. Returns false on decode failure, on
// missing/zero length, or if the IR exceeds MAX_IR_BYTES (100 MB).
//
// Thread safety: not thread-safe; caller must ensure no Process() is in
// flight (in practice this is called from the UI thread before the DSP is
// hooked up).
bool ConvolutionReverb::LoadIR(const wchar_t* path) {
    HSTREAM stream = BASS_StreamCreateFile(FALSE, path, 0, 0,
        BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | BASS_UNICODE);
    if (!stream) {
        return false;
    }

    // Get stream info
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(stream, &info)) {
        BASS_StreamFree(stream);
        return false;
    }

    const int channels = info.chans;
    const int sampleRate = info.freq;

    // Get total length in bytes and calculate samples
    const QWORD length = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
    if (length == (QWORD)-1 || length == 0) {
        BASS_StreamFree(stream);
        return false;
    }

    // Reject IR files larger than 100 MB to prevent OOM
    static constexpr QWORD MAX_IR_BYTES = 100ULL * 1024 * 1024;
    if (length > MAX_IR_BYTES) {
        BASS_StreamFree(stream);
        return false;
    }

    int numSamples = (int)(length / (sizeof(float) * channels));

    // Read all audio data
    std::vector<float> interleavedData(numSamples * channels);
    DWORD bytesRead = BASS_ChannelGetData(stream, interleavedData.data(), (DWORD)length);
    BASS_StreamFree(stream);

    if (bytesRead == (DWORD)-1) {
        return false;
    }

    // Deinterleave to L/R channels
    std::vector<float> irDataL(numSamples);
    std::vector<float> irDataR(numSamples);

    for (int i = 0; i < numSamples; i++) {
        irDataL[i] = interleavedData[i * channels];
        if (channels >= 2) {
            irDataR[i] = interleavedData[i * channels + 1];
        } else {
            irDataR[i] = irDataL[i];  // Mono: duplicate to both channels
        }
    }

    // Store IR data
    m_irPath = path;
    m_irSampleRate = sampleRate;
    m_irChannels = channels;
    m_irSamples = numSamples;

    // Partitioned-convolution layout: chop the IR into m_blockSize-sample
    // partitions, FFT each to 2*m_blockSize complex bins (zero-padded to
    // avoid circular convolution wrap, classic overlap-save setup). Per-block
    // realtime cost then becomes M complex multiplies and one IFFT, instead
    // of an FFT over the full IR length every block.
    m_blockSize = 1024;
    m_fftSize = m_blockSize * 2;
    m_numPartitions = (numSamples + m_blockSize - 1) / m_blockSize;

    irDataL.resize(m_numPartitions * m_blockSize, 0.0f);
    irDataR.resize(m_numPartitions * m_blockSize, 0.0f);

    m_irSpectrumL.resize(m_numPartitions);
    m_irSpectrumR.resize(m_numPartitions);
    std::vector<std::complex<float>> fftBuf(m_fftSize);

    for (int p = 0; p < m_numPartitions; p++) {
        m_irSpectrumL[p].resize(m_fftSize);
        m_irSpectrumR[p].resize(m_fftSize);

        // Left channel
        for (int i = 0; i < m_fftSize; i++) {
            if (i < m_blockSize) {
                fftBuf[i] = std::complex<float>(irDataL[p * m_blockSize + i], 0.0f);
            } else {
                fftBuf[i] = std::complex<float>(0.0f, 0.0f);  // Zero-pad
            }
        }
        FFT(fftBuf.data(), m_fftSize, false);
        m_irSpectrumL[p] = fftBuf;

        // Right channel
        for (int i = 0; i < m_fftSize; i++) {
            if (i < m_blockSize) {
                fftBuf[i] = std::complex<float>(irDataR[p * m_blockSize + i], 0.0f);
            } else {
                fftBuf[i] = std::complex<float>(0.0f, 0.0f);
            }
        }
        FFT(fftBuf.data(), m_fftSize, false);
        m_irSpectrumR[p] = fftBuf;
    }

    m_irLoaded = true;
    m_initialized = false;  // Force re-initialization with new IR
    return true;
}

bool ConvolutionReverb::Init(int sampleRate) {
    m_sampleRate = sampleRate;

    // Allocate buffers
    m_inputBufferL.resize(m_fftSize, 0.0f);
    m_inputBufferR.resize(m_fftSize, 0.0f);
    m_inputPos = 0;

    m_fftBuffer.resize(m_fftSize);

    // Frequency delay line for partitioned convolution
    if (m_numPartitions > 0) {
        m_fdlL.resize(m_numPartitions);
        m_fdlR.resize(m_numPartitions);
        for (int p = 0; p < m_numPartitions; p++) {
            m_fdlL[p].resize(m_fftSize, std::complex<float>(0.0f, 0.0f));
            m_fdlR[p].resize(m_fftSize, std::complex<float>(0.0f, 0.0f));
        }
    }
    m_fdlPos = 0;

    // Output accumulator (needs to hold overlap from previous block)
    m_outputL.resize(m_fftSize, 0.0f);
    m_outputR.resize(m_fftSize, 0.0f);

    m_initialized = true;
    return true;
}

void ConvolutionReverb::Reset() {
    if (!m_initialized) return;

    std::fill(m_inputBufferL.begin(), m_inputBufferL.end(), 0.0f);
    std::fill(m_inputBufferR.begin(), m_inputBufferR.end(), 0.0f);
    m_inputPos = 0;

    for (auto& fdl : m_fdlL) {
        std::fill(fdl.begin(), fdl.end(), std::complex<float>(0.0f, 0.0f));
    }
    for (auto& fdl : m_fdlR) {
        std::fill(fdl.begin(), fdl.end(), std::complex<float>(0.0f, 0.0f));
    }
    m_fdlPos = 0;

    std::fill(m_outputL.begin(), m_outputL.end(), 0.0f);
    std::fill(m_outputR.begin(), m_outputR.end(), 0.0f);
}

// In-place radix-2 Cooley-Tukey FFT (or IFFT when inverse=true).
// n MUST be a power of two — m_fftSize is always 2*m_blockSize (= 2048)
// here so that's guaranteed. Same algorithm is duplicated in
// center_cancel.cpp; if a third caller appears, extract to a shared header.
void ConvolutionReverb::FFT(std::complex<float>* data, int n, bool inverse) {
    // Bit-reversal permutation: rearrange input so the butterfly loop below
    // can write outputs in natural order.
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

    // Iterative butterfly: doubles the transform length each outer iteration
    // until the full n-point DFT is computed.
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

float ConvolutionReverb::GetIRLengthMs() const {
    if (!m_irLoaded || m_irSampleRate == 0) return 0.0f;
    return (float)m_irSamples / m_irSampleRate * 1000.0f;
}

// Process `frames` stereo frames in-place (interleaved L,R,L,R,...).
// Called from the BASS DSP callback on the audio thread for every block.
// Accumulates input into m_inputBuffer; once a full blockSize has arrived,
// runs one FFT/multiply-accumulate/IFFT cycle and overlap-adds the result
// into m_outputL/R, which is drained one sample per input sample.
void ConvolutionReverb::Process(float* buffer, int frames) {
    // Passthrough until both Init() and LoadIR() have completed successfully.
    if (!m_initialized || !m_irLoaded || m_numPartitions == 0) {
        return;
    }

    float wetGain = m_mix / 100.0f;
    float dryGain = 1.0f - wetGain;
    float gainLinear = powf(10.0f, m_gain / 20.0f);

    for (int i = 0; i < frames; i++) {
        float inL = buffer[i * 2];
        float inR = buffer[i * 2 + 1];

        // Store input for processing
        m_inputBufferL[m_inputPos] = inL;
        m_inputBufferR[m_inputPos] = inR;

        // Get wet output from previously processed blocks
        float wetL = m_outputL[m_inputPos] * gainLinear;
        float wetR = m_outputR[m_inputPos] * gainLinear;

        // Mix dry and wet
        buffer[i * 2] = inL * dryGain + wetL * wetGain;
        buffer[i * 2 + 1] = inR * dryGain + wetR * wetGain;

        m_inputPos++;

        // Process when we have a full block
        if (m_inputPos >= m_blockSize) {
            // Left channel: zero-pad and FFT
            for (int j = 0; j < m_blockSize; j++) {
                m_fftBuffer[j] = std::complex<float>(m_inputBufferL[j], 0.0f);
            }
            for (int j = m_blockSize; j < m_fftSize; j++) {
                m_fftBuffer[j] = std::complex<float>(0.0f, 0.0f);
            }
            FFT(m_fftBuffer.data(), m_fftSize, false);
            m_fdlL[m_fdlPos] = m_fftBuffer;

            // Right channel: zero-pad and FFT
            for (int j = 0; j < m_blockSize; j++) {
                m_fftBuffer[j] = std::complex<float>(m_inputBufferR[j], 0.0f);
            }
            for (int j = m_blockSize; j < m_fftSize; j++) {
                m_fftBuffer[j] = std::complex<float>(0.0f, 0.0f);
            }
            FFT(m_fftBuffer.data(), m_fftSize, false);
            m_fdlR[m_fdlPos] = m_fftBuffer;

            // Frequency-domain convolution: Y[k] = sum_p ( X_{n-p}[k] * H_p[k] )
            // where H_p is the p-th IR partition spectrum and X_{n-p} is the
            // FDL entry from p blocks ago. This is the per-block cost of
            // partitioned convolution — O(M*N) complex muls vs O(N*irLen) in
            // the time domain.
            std::vector<std::complex<float>> accumL(m_fftSize, std::complex<float>(0.0f, 0.0f));
            std::vector<std::complex<float>> accumR(m_fftSize, std::complex<float>(0.0f, 0.0f));

            for (int p = 0; p < m_numPartitions; p++) {
                // FDL is a circular ring buffer indexed backwards in time.
                int fdlIdx = (m_fdlPos - p + m_numPartitions) % m_numPartitions;

                for (int k = 0; k < m_fftSize; k++) {
                    accumL[k] += m_fdlL[fdlIdx][k] * m_irSpectrumL[p][k];
                    accumR[k] += m_fdlR[fdlIdx][k] * m_irSpectrumR[p][k];
                }
            }

            // IFFT
            FFT(accumL.data(), m_fftSize, true);
            FFT(accumR.data(), m_fftSize, true);

            // Overlap-add:
            // m_outputL[0..blockSize-1] = pending overlap from previous + first half of current
            // m_outputL[blockSize..2*blockSize-1] = second half of current (pending for next)
            for (int j = 0; j < m_blockSize; j++) {
                m_outputL[j] = m_outputL[j + m_blockSize] + accumL[j].real();
                m_outputR[j] = m_outputR[j + m_blockSize] + accumR[j].real();
            }
            for (int j = 0; j < m_blockSize; j++) {
                m_outputL[j + m_blockSize] = accumL[j + m_blockSize].real();
                m_outputR[j + m_blockSize] = accumR[j + m_blockSize].real();
            }

            // Advance FDL position
            m_fdlPos = (m_fdlPos + 1) % m_numPartitions;

            // Reset input position for next block
            m_inputPos = 0;
        }
    }
}
