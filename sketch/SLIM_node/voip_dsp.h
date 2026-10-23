/*
  voip_dsp.h
  Small signal processing helpers used by voip.cpp (capture task, core 0):
    - DtmfDecoder: software DTMF detector (Goertzel), backs up the MT8870
    - analyzeSpectrum(): FFT of the microphone signal for the noise report

  Everything works on the microphone signal in ADC counts at 8192 samples/sec.
*/
#ifndef voip_dsp_h
#define voip_dsp_h

#include <math.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// DTMF decoder
// ---------------------------------------------------------------------------
class DtmfDecoder {
  public:
    DtmfDecoder(float sampleRate, float minAmplitude) : _minAmp(minAmplitude) {
      static const float freqs[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};
      for (int i = 0; i < 8; i++) _coeff[i] = 2.0f * cosf(2.0f * (float)M_PI * freqs[i] / sampleRate);
      reset();
    }

    void reset() {
      memset(_s1, 0, sizeof(_s1));
      memset(_s2, 0, sizeof(_s2));
      _n = 0;
      _energy = 0;
      _lastBlock = 0;
      _reported = false;
    }

    // feed one sample; returns a digit ('0'-'9','*','#','A'-'D') once per key press, else 0
    char process(float x) {
      for (int i = 0; i < 8; i++) {
        float s0 = x + _coeff[i] * _s1[i] - _s2[i];
        _s2[i] = _s1[i];
        _s1[i] = s0;
      }
      _energy += x * x;
      if (++_n < BLOCK) return 0;

      char d = evaluateBlock();
      memset(_s1, 0, sizeof(_s1));
      memset(_s2, 0, sizeof(_s2));
      _n = 0;
      _energy = 0;

      // a digit counts when seen in 2 blocks in a row (~50 mS), reported once until it stops
      char out = 0;
      if (d && d == _lastBlock && !_reported) {
        out = d;
        _reported = true;
      }
      if (d != _lastBlock) _reported = false;
      _lastBlock = d;
      return out;
    }

  private:
    static const int BLOCK = 205;                  // ~25 mS at 8192 Hz, ~40 Hz resolution
    float _coeff[8], _s1[8], _s2[8];
    float _energy, _minAmp;
    int   _n;
    char  _lastBlock;
    bool  _reported;

    char evaluateBlock() {
      float p[8];
      for (int i = 0; i < 8; i++) p[i] = _s1[i] * _s1[i] + _s2[i] * _s2[i] - _coeff[i] * _s1[i] * _s2[i];
      int r = 0, c = 4;
      for (int i = 1; i < 4; i++) if (p[i] > p[r]) r = i;
      for (int i = 5; i < 8; i++) if (p[i] > p[c]) c = i;
      if (_energy <= 0) return 0;

      // amplitude of each tone in ADC counts
      float ampR = 2.0f * sqrtf(p[r]) / BLOCK;
      float ampC = 2.0f * sqrtf(p[c]) / BLOCK;
      if (ampR < _minAmp || ampC < _minAmp) return 0;

      // the two tones must hold most of the signal energy (dial tone/noise allowed)
      float norm = _energy * BLOCK / 2.0f;
      float fr = p[r] / norm, fc = p[c] / norm;
      if (fr < 0.10f || fc < 0.10f || (fr + fc) < 0.40f) return 0;

      // twist: the two tones within ~8 dB of each other
      float twist = p[c] / p[r];
      if (twist < 0.16f || twist > 6.3f) return 0;

      // no other row/column tone close to the winners
      for (int i = 0; i < 4; i++) if (i != r && p[i] > 0.25f * p[r]) return 0;
      for (int i = 4; i < 8; i++) if (i != c && p[i] > 0.25f * p[c]) return 0;

      static const char keys[4][4] = {{'1', '2', '3', 'A'}, {'4', '5', '6', 'B'},
                                      {'7', '8', '9', 'C'}, {'*', '0', '#', 'D'}};
      return keys[r][c - 4];
    }
};

// ---------------------------------------------------------------------------
// spectrum report of a block of microphone samples (ADC counts)
// ---------------------------------------------------------------------------
#define SPEC_N 1024

struct SpectrumReport {
  float hum;          // RMS below 300 Hz (mains hum, WiFi burst thumps)
  float voice;        // RMS 300 - 3400 Hz (hiss, whistles in the speech band)
  float high;         // RMS above 3400 Hz
  int   nPeaks;
  float peakHz[4];    // strongest tones standing out of the noise
  float peakAmp[4];   // their amplitude in ADC counts
};

static void fftComplex(float* re, float* im, int n) {
  for (int i = 1, j = 0; i < n; i++) {             // bit reversal
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float tr = re[b] * cr - im[b] * ci;
        float ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

// x: SPEC_N samples.  work buffers re/im: SPEC_N floats each
static void analyzeSpectrum(const float* x, float* re, float* im, float fs, SpectrumReport& r) {
  const int n = SPEC_N;
  float mean = 0;
  for (int i = 0; i < n; i++) mean += x[i];
  mean /= n;
  for (int i = 0; i < n; i++) {
    float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (n - 1));   // Hann window
    re[i] = (x[i] - mean) * w;
    im[i] = 0;
  }
  fftComplex(re, im, n);

  const float sumW2 = 3.0f * n / 8.0f;
  const float binHz = fs / n;
  float eHum = 0, eVoice = 0, eHigh = 0;
  for (int k = 1; k < n / 2; k++) {
    float m2 = re[k] * re[k] + im[k] * im[k];
    float f = k * binHz;
    if (f < 300) eHum += m2; else if (f <= 3400) eVoice += m2; else eHigh += m2;
    re[k] = sqrtf(m2) * 4.0f / n;                  // re[] now holds sine amplitude in counts
  }
  r.hum   = sqrtf(2.0f * eHum   / (n * sumW2));
  r.voice = sqrtf(2.0f * eVoice / (n * sumW2));
  r.high  = sqrtf(2.0f * eHigh  / (n * sumW2));

  // typical (median-ish) bin level: average of the lower half of bins
  float floorAmp = 0;
  {
    float sum = 0; int cnt = 0;
    float avg = 0;
    for (int k = 3; k < n / 2; k++) avg += re[k];
    avg /= (n / 2 - 3);
    for (int k = 3; k < n / 2; k++) if (re[k] <= avg) { sum += re[k]; cnt++; }
    floorAmp = cnt ? sum / cnt : avg;
  }

  // strongest local peaks well above the floor
  r.nPeaks = 0;
  for (int k = 3; k < n / 2 - 1; k++) {
    float a = re[k];
    if (a < re[k - 1] || a < re[k + 1] || a < 6.0f * floorAmp || a < 1.0f) continue;
    int pos = r.nPeaks;
    while (pos > 0 && r.peakAmp[pos - 1] < a) pos--;
    if (pos >= 4) continue;
    for (int j = (r.nPeaks < 4 ? r.nPeaks : 3); j > pos; j--) {
      r.peakAmp[j] = r.peakAmp[j - 1];
      r.peakHz[j] = r.peakHz[j - 1];
    }
    r.peakAmp[pos] = a;
    r.peakHz[pos] = k * binHz;
    if (r.nPeaks < 4) r.nPeaks++;
  }
}

#endif
