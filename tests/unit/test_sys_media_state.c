#include "psx3ds/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !PSX3DS_PLATFORM_3DS
#include <unistd.h>
#endif

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));       \
            return 1;                                                               \
        }                                                                           \
    } while (0)

int main(void) {
#if PSX3DS_PLATFORM_3DS
    return 0;
#else
    long pid = (long)getpid();
    char ppm15_path[256];
    char ppm24_path[256];
    char wav_path[256];
    char state_path[256];
    snprintf(ppm15_path, sizeof(ppm15_path), "/tmp/psx3ds_test15_%ld.ppm", pid);
    snprintf(ppm24_path, sizeof(ppm24_path), "/tmp/psx3ds_test24_%ld.ppm", pid);
    snprintf(wav_path,   sizeof(wav_path),   "/tmp/psx3ds_test_%ld.wav",   pid);
    snprintf(state_path, sizeof(state_path), "/tmp/psx3ds_test_%ld.st",    pid);

    /* 1. 15bpp 0BGR555 PPM writer test (4x2 with larger pitch_px = 8) */
    u16 fb15[2 * 8];
    memset(fb15, 0, sizeof(fb15));
    fb15[0] = 0x001Fu; /* Pure Red:   R5=31 -> 255, G=0, B=0 */
    fb15[1] = 0x03E0u; /* Pure Green: R=0, G5=31 -> 255, B=0 */
    fb15[2] = 0x7C00u; /* Pure Blue:  R=0, G=0, B5=31 -> 255 */
    fb15[3] = 0x7FFFu; /* Pure White: 255, 255, 255 */
    fb15[8] = 0x0000u; /* Row 1 col 0: Black */

    CHECK(video_ppm_write_rgb555(ppm15_path, fb15, 4u, 2u, 8u, false) == PSX_OK,
          "video_ppm_write_rgb555 15bpp");
    FILE *fp = fopen(ppm15_path, "rb");
    CHECK(fp != NULL, "open written 15bpp PPM");
    u8 ppm_buf[128];
    size_t ppm_n = fread(ppm_buf, 1u, sizeof(ppm_buf), fp);
    fclose(fp);
    unlink(ppm15_path);

    const char *expect_ppm_hdr = "P6\n4 2\n255\n";
    size_t hdr_len = strlen(expect_ppm_hdr);
    CHECK(ppm_n == hdr_len + 4u * 2u * 3u, "15bpp PPM exact byte length");
    CHECK(memcmp(ppm_buf, expect_ppm_hdr, hdr_len) == 0, "15bpp PPM header");
    const u8 *px = ppm_buf + hdr_len;
    CHECK(px[0] == 255u && px[1] == 0u   && px[2] == 0u,   "pixel 0 red");
    CHECK(px[3] == 0u   && px[4] == 255u && px[5] == 0u,   "pixel 1 green");
    CHECK(px[6] == 0u   && px[7] == 0u   && px[8] == 255u, "pixel 2 blue");
    CHECK(px[9] == 255u && px[10] == 255u && px[11] == 255u, "pixel 3 white");

    /* 2. 24bpp packed RGB888 PPM writer test (pitch_px * 2 byte stride) */
    u16 fb24[2 * 10]; /* row stride = 10 * 2 = 20 bytes (>= 4 * 3 = 12 bytes) */
    memset(fb24, 0, sizeof(fb24));
    u8 *row0 = (u8 *)fb24;
    row0[0] = 12u; row0[1] = 34u; row0[2] = 56u;
    u8 *row1 = (u8 *)fb24 + 20u;
    row1[9] = 90u; row1[10] = 120u; row1[11] = 240u;

    CHECK(video_ppm_write_rgb555(ppm24_path, fb24, 4u, 2u, 10u, true) == PSX_OK,
          "video_ppm_write_rgb555 24bpp");
    fp = fopen(ppm24_path, "rb");
    CHECK(fp != NULL, "open written 24bpp PPM");
    ppm_n = fread(ppm_buf, 1u, sizeof(ppm_buf), fp);
    fclose(fp);
    unlink(ppm24_path);
    CHECK(ppm_n == hdr_len + 24u, "24bpp PPM exact byte length");
    px = ppm_buf + hdr_len;
    CHECK(px[0] == 12u && px[1] == 34u && px[2] == 56u, "24bpp row0 px0");
    CHECK(px[21] == 90u && px[22] == 120u && px[23] == 240u, "24bpp row1 px3");

    /* 3. 44.1 kHz 16-bit Stereo RIFF WAVE writer test */
    s16 stereo[6] = { -32768, 32767, 1000, -1000, 0, 42 };
    CHECK(audio_wav_write(wav_path, stereo, 3u, 44100u) == PSX_OK, "audio_wav_write");
    fp = fopen(wav_path, "rb");
    CHECK(fp != NULL, "open written WAV");
    u8 wav_buf[128];
    size_t wav_n = fread(wav_buf, 1u, sizeof(wav_buf), fp);
    fclose(fp);
    unlink(wav_path);

    CHECK(wav_n == 44u + 12u, "WAV 44B header + 3 stereo frames (12B)");
    CHECK(memcmp(&wav_buf[0], "RIFF", 4u) == 0, "RIFF magic");
    CHECK(memcmp(&wav_buf[8], "WAVE", 4u) == 0, "WAVE magic");
    CHECK(memcmp(&wav_buf[12], "fmt ", 4u) == 0, "fmt  chunk");
    CHECK(memcmp(&wav_buf[36], "data", 4u) == 0, "data chunk");
    u32 riff_sz = 0u, srate = 0u, data_sz = 0u;
    u16 chans = 0u, bps = 0u;
    memcpy(&riff_sz, &wav_buf[4], 4u);
    memcpy(&chans,   &wav_buf[22], 2u);
    memcpy(&srate,   &wav_buf[24], 4u);
    memcpy(&bps,     &wav_buf[34], 2u);
    memcpy(&data_sz, &wav_buf[40], 4u);
    CHECK(riff_sz == 36u + 12u && chans == 2u && srate == 44100u && bps == 16u && data_sz == 12u,
          "WAV header fields exact");
    s16 read_pcm[6];
    memcpy(read_pcm, &wav_buf[44], sizeof(read_pcm));
    CHECK(memcmp(read_pcm, stereo, sizeof(stereo)) == 0, "WAV PCM16 payload exact");

    /* 4. StateIO primitive roundtrip + bounds overflow guard */
    u8 raw_st[64];
    StateIO wst = { .writing = true, .buf = raw_st, .pos = 0u, .cap = sizeof(raw_st), .error = false };
    u8  v8  = 0xA5u;
    u16 v16 = 0xBEEFu;
    u32 v32 = 0x12345678u;
    u64 v64 = 0xFEDCBA9876543210ULL;
    bool vb = true;
    state_u8(&wst, &v8);
    state_u16(&wst, &v16);
    state_u32(&wst, &v32);
    state_u64(&wst, &v64);
    state_bool(&wst, &vb);
    state_skip(&wst, 4u);
    CHECK(!wst.error && wst.pos == 20u, "StateIO write primitives");

    StateIO rst = { .writing = false, .buf = raw_st, .pos = 0u, .cap = wst.pos, .error = false };
    u8 r8 = 0u; u16 r16 = 0u; u32 r32 = 0u; u64 r64 = 0ULL; bool rb = false;
    state_u8(&rst, &r8);
    state_u16(&rst, &r16);
    state_u32(&rst, &r32);
    state_u64(&rst, &r64);
    state_bool(&rst, &rb);
    state_skip(&rst, 4u);
    CHECK(!rst.error && r8 == v8 && r16 == v16 && r32 == v32 && r64 == v64 && rb == vb,
          "StateIO read primitives roundtrip");
    state_u32(&rst, &r32);
    CHECK(rst.error, "StateIO detects read past end of buffer");

    /* 5. Full PsxSystem SaveState roundtrip + CRC32 corruption rejection */
    PsxConfig cfg;
    psx_config_defaults(&cfg, true);
    cfg.small_mem = true;
    cfg.hle_bios = true;

    PsxSystem *sys = (PsxSystem *)calloc(1u, sizeof(*sys));
    CHECK(sys != NULL && psx_system_init(sys, &cfg) == PSX_OK, "init PsxSystem for savestate");

    sys->cpu.pc = 0x80045678u;
    sys->cpu.next_pc = 0x8004567Cu;
    sys->cpu.gpr[4] = 0xCAFEBABEu;
    sys->cpu.gpr[29] = 0x801FFF00u;
    sys->cpu.gte_d[1] = 0x11223344u;
    sys->bus.ram[0x1234] = 0x77u;
    sys->bus.scratch_page[0x10] = 0x33u;
    sys->gpu.vram[512] = 0x55AAu;
    sys->spu.ram[1024] = 0x99u;

    CHECK(psx_state_save_file(sys, state_path) == PSX_OK, "psx_state_save_file");

    /* Mutate live state completely */
    sys->cpu.pc = 0x80000000u;
    sys->cpu.gpr[4] = 0u;
    sys->cpu.gte_d[1] = 0u;
    sys->bus.ram[0x1234] = 0x00u;
    sys->bus.scratch_page[0x10] = 0x00u;
    sys->gpu.vram[512] = 0x0000u;
    sys->spu.ram[1024] = 0x00u;

    /* Flip a byte in the saved file on disk and verify CRC32 rejection! */
    fp = fopen(state_path, "r+b");
    CHECK(fp != NULL && fseek(fp, -8L, SEEK_END) == 0, "seek state file for tamper test");
    u8 orig_byte = 0u;
    CHECK(fread(&orig_byte, 1u, 1u, fp) == 1u, "read byte to tamper");
    u8 bad_byte = (u8)(orig_byte ^ 0x80u);
    CHECK(fseek(fp, -1L, SEEK_CUR) == 0 && fwrite(&bad_byte, 1u, 1u, fp) == 1u, "write tampered byte");
    fclose(fp);

    CHECK(psx_state_load_file(sys, state_path) == PSX_ERR_FORMAT,
          "tampered savestate rejected with PSX_ERR_FORMAT");
    CHECK(sys->cpu.pc == 0x80000000u && sys->bus.ram[0x1234] == 0x00u,
          "failed load leaves live system uncorrupted");

    /* Restore original byte and verify clean load restores every subsystem */
    fp = fopen(state_path, "r+b");
    CHECK(fp != NULL && fseek(fp, -8L, SEEK_END) == 0 && fwrite(&orig_byte, 1u, 1u, fp) == 1u,
          "restore original byte");
    fclose(fp);

    CHECK(psx_state_load_file(sys, state_path) == PSX_OK, "psx_state_load_file clean");
    unlink(state_path);

    CHECK(sys->cpu.pc == 0x80045678u && sys->cpu.next_pc == 0x8004567Cu, "restored PC");
    CHECK(sys->cpu.gpr[4] == 0xCAFEBABEu && sys->cpu.gte_d[1] == 0x11223344u, "restored GPR & GTE");
    CHECK(sys->bus.ram[0x1234] == 0x77u && sys->bus.scratch_page[0x10] == 0x33u,
          "restored RAM & Scratchpad");
    CHECK(sys->gpu.vram[512] == 0x55AAu && sys->spu.ram[1024] == 0x99u,
          "restored VRAM & SPU RAM");

    /* 6. JSON telemetry & multi-engine benchmark formatting */
    u32 jcap = 65536u;
    char *jbuf = (char *)malloc(jcap);
    CHECK(jbuf != NULL, "alloc jbuf");
    u32 jlen = psx_stats_format_json(sys, &sys->stats, "unit_test.exe", 16667ULL, 1u,
                                     jbuf, jcap);
    CHECK(jlen > 20u && strstr(jbuf, "\"emu_fps\"") != NULL &&
          strstr(jbuf, "\"gpu_tex_hit_pct\"") != NULL,
          "psx_stats_format_json valid JSON keys");

    psx_system_shutdown(sys);
    free(sys);

    PsxBenchConfig bcfg = {
        .bios_path = NULL,
        .hle_bios = true,
        .exe_path = NULL,
        .cd_path = NULL,
        .frames = 1u,
        .all_engines = true,
        .all_gpus = false
    };
    CHECK(psx_bench_run_json(&bcfg, jbuf, jcap) == PSX_OK, "psx_bench_run_json");
    CHECK(strstr(jbuf, "\"passes\"") != NULL && strstr(jbuf, "\"interp\"") != NULL,
          "benchmark JSON contains multi-engine passes");
    free(jbuf);

    printf("test_sys_media_state PASS\n");
    return 0;
#endif
}
