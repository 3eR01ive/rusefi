#pragma once

#include <cstddef>

namespace fft {

bool fft(const real_type* data_in, complex_type* data_out, const size_t size);
void fft_freq(real_type* freq, const size_t size, const size_t sampleFreq);
void fft_amp(const complex_type* fft_data, real_type* amplitude, const size_t size);

void rectwin(float * w, unsigned n);
void hann(float * w, unsigned n, bool sflag);
void hamming(float * w, unsigned n, bool sflag);
void blackman(float * w, unsigned n, bool sflag);
void blackmanharris(float * w, unsigned n, bool sflag);

float get_main_freq(float* amplitudes, float* frequencies);

} 

#include "fft.hpp"
