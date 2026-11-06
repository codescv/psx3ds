#include "psx3ds/platform.h"

#include <stdio.h>
#include <string.h>

static void wav_put_u16_le(u8 *dst, u16 v) {
    dst[0] = (u8)(v & 0xFFu);
    dst[1] = (u8)((v >> 8) & 0xFFu);
}

static void wav_put_u32_le(u8 *dst, u32 v) {
    dst[0] = (u8)(v & 0xFFu);
    dst[1] = (u8)((v >> 8) & 0xFFu);
    dst[2] = (u8)((v >> 16) & 0xFFu);
    dst[3] = (u8)((v >> 24) & 0xFFu);
}

PsxResult audio_wav_write(const char *path, const s16 *stereo,
                          u32 frames, u32 sample_rate) {
    if (!path || (frames > 0u && !stereo)) {
        return PSX_ERR_BAD_ARG;
    }
    if (sample_rate == 0u) {
        sample_rate = 44100u;
    }
    if (frames > (0xFFFFFFFFu - 36u) / 4u) {
        return PSX_ERR_BAD_ARG;
    }

    u32 data_bytes = frames * 4u;
    u32 riff_size = 36u + data_bytes;
    u32 byte_rate = sample_rate * 4u;

    u8 hdr[44];
    memcpy(&hdr[0], "RIFF", 4u);
    wav_put_u32_le(&hdr[4], riff_size);
    memcpy(&hdr[8], "WAVE", 4u);
    memcpy(&hdr[12], "fmt ", 4u);
    wav_put_u32_le(&hdr[16], 16u);          /* PCM fmt chunk size */
    wav_put_u16_le(&hdr[20], 1u);           /* AudioFormat = 1 (PCM) */
    wav_put_u16_le(&hdr[22], 2u);           /* NumChannels = 2 (Stereo) */
    wav_put_u32_le(&hdr[24], sample_rate);  /* SampleRate */
    wav_put_u32_le(&hdr[28], byte_rate);    /* ByteRate */
    wav_put_u16_le(&hdr[32], 4u);           /* BlockAlign = 4 */
    wav_put_u16_le(&hdr[34], 16u);          /* BitsPerSample = 16 */
    memcpy(&hdr[36], "data", 4u);
    wav_put_u32_le(&hdr[40], data_bytes);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return PSX_ERR_IO;
    }
    if (fwrite(hdr, 1u, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return PSX_ERR_IO;
    }

    /* Host targets (x86_64 T3 and ARM11 LE T2) are little-endian; serialize explicitly per batch */
    u8 pcm_buf[512u * 4u];
    u32 rem = frames;
    const s16 *src = stereo;
    while (rem > 0u) {
        u32 chunk = rem > 512u ? 512u : rem;
        for (u32 i = 0u; i < chunk; i++) {
            wav_put_u16_le(&pcm_buf[i * 4u + 0u], (u16)src[i * 2u + 0u]);
            wav_put_u16_le(&pcm_buf[i * 4u + 2u], (u16)src[i * 2u + 1u]);
        }
        size_t chunk_bytes = (size_t)chunk * 4u;
        if (fwrite(pcm_buf, 1u, chunk_bytes, fp) != chunk_bytes) {
            fclose(fp);
            return PSX_ERR_IO;
        }
        src += (size_t)chunk * 2u;
        rem -= chunk;
    }

    if (fclose(fp) != 0) {
        return PSX_ERR_IO;
    }
    return PSX_OK;
}
