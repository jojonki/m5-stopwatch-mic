//! IMA ADPCM decoder matching firmware/ble/main/adpcm.h (low nibble first).

const STEP: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
];
const INDEX_ADJ: [i32; 16] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

/// Decodes `data` (2 samples per byte) starting from `predictor` / `index`,
/// appending normalized f32 samples to `out`.
pub fn decode(data: &[u8], mut predictor: i32, mut index: i32, out: &mut Vec<f32>) {
    for &byte in data {
        for code in [byte & 0x0F, byte >> 4] {
            let code = code as i32;
            let step = STEP[index as usize];
            let mut delta = step >> 3;
            if code & 4 != 0 {
                delta += step;
            }
            if code & 2 != 0 {
                delta += step >> 1;
            }
            if code & 1 != 0 {
                delta += step >> 2;
            }
            predictor += if code & 8 != 0 { -delta } else { delta };
            predictor = predictor.clamp(-32768, 32767);
            index = (index + INDEX_ADJ[code as usize]).clamp(0, 88);
            out.push(predictor as f32 / 32768.0);
        }
    }
}
