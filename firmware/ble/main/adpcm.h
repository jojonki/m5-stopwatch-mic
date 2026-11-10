#pragma once
// IMA ADPCM encoder (4 bits/sample). Each packet carries its own
// predictor/index, so packets decode independently.

#include <stddef.h>
#include <stdint.h>

struct AdpcmState {
  int16_t predictor = 0;
  uint8_t index = 0;
};

namespace adpcm_detail {
constexpr int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
constexpr int8_t INDEX_ADJ[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

inline uint8_t encode_sample(AdpcmState &st, int16_t sample) {
  int step = STEP[st.index];
  int diff = sample - st.predictor;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int delta = step >> 3;
  if (diff >= step) { code |= 4; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { code |= 2; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { code |= 1; delta += step; }

  int pred = st.predictor + ((code & 8) ? -delta : delta);
  if (pred > 32767) pred = 32767;
  if (pred < -32768) pred = -32768;
  st.predictor = (int16_t) pred;

  int idx = st.index + INDEX_ADJ[code];
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  st.index = (uint8_t) idx;
  return code;
}
}  // namespace adpcm_detail

// Encodes n samples (n even) into n/2 bytes; low nibble = earlier sample.
inline void adpcm_encode(AdpcmState &st, const int16_t *in, size_t n, uint8_t *out) {
  for (size_t i = 0; i < n; i += 2) {
    uint8_t lo = adpcm_detail::encode_sample(st, in[i]);
    uint8_t hi = adpcm_detail::encode_sample(st, in[i + 1]);
    out[i / 2] = (uint8_t) (lo | (hi << 4));
  }
}
