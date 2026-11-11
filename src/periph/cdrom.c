#include "periph_internal.h"
#include "psx3ds/arena.h"
#include "psx3ds/bus.h"
#include "psx3ds/state.h"

#include <string.h>

enum {
    CD_INT_NONE      = 0,
    CD_INT1_DATARDY  = 1,
    CD_INT2_COMPLETE = 2,
    CD_INT3_ACK      = 3,
    CD_INT4_DATAEND  = 4,
    CD_INT5_ERROR    = 5
};

enum {
    CD_STATE_IDLE          = 0,
    CD_STATE_SEEKL_PENDING = 1,
    CD_STATE_SEEKP_PENDING = 2
};

/* CD-ROM stat byte bits */
#define CD_STAT_ERROR      0x01u
#define CD_STAT_MOTOR_ON   0x02u
#define CD_STAT_SEEK_ERR   0x04u
#define CD_STAT_ID_ERR     0x08u
#define CD_STAT_SHELL_OPEN 0x10u
#define CD_STAT_READ       0x20u
#define CD_STAT_SEEK       0x40u
#define CD_STAT_PLAY       0x80u

static bool cd_disc_present(const PsxCdrom *cd) {
    return cd->img != NULL && cd->img->loaded;
}

static u8 cd_make_stat(const PsxCdrom *cd) {
    if (!cd_disc_present(cd)) {
        return CD_STAT_SHELL_OPEN;
    }
    u8 st = cd->img->motor_on ? CD_STAT_MOTOR_ON : 0x00u;
    if (cd->state == CD_STATE_SEEKL_PENDING || cd->state == CD_STATE_SEEKP_PENDING) {
        st |= CD_STAT_SEEK | CD_STAT_MOTOR_ON;
    } else if (cd->reading) {
        st |= CD_STAT_READ | CD_STAT_MOTOR_ON;
    } else if (cd->playing) {
        st |= CD_STAT_PLAY | CD_STAT_MOTOR_ON;
    }
    return st;
}

static u32 cd_sector_period(const PsxCdrom *cd) {
    return cd->double_speed ? PSX_CD_SECTOR_CYCLES_2X : PSX_CD_SECTOR_CYCLES_1X;
}

static void cd_load_data_fifo(PsxCdrom *cd) {
    bool whole = (cd->mode & 0x20u) != 0u;
    u32  off   = whole ? 12u : ((cd->sector[15] == 0x01u) ? 16u : 24u);
    u32  len   = whole ? 2340u : 2048u;
    memcpy(cd->data_fifo, &cd->sector[off], len);
    cd->data_r     = 0u;
    cd->data_n     = len;
    cd->data_ready = true;
}

static void cd_deliver_int_now(PsxSystem *sys, u8 int_no, const u8 *resp, u8 resp_len) {
    PsxCdrom *cd = &sys->cdrom;
    u8 n = (resp_len > 16u) ? 16u : resp_len;
    if (n > 0u && resp) {
        memcpy(cd->resp_fifo, resp, n);
    }
    cd->resp_r   = 0u;
    cd->resp_w   = n;
    cd->irq_flag = int_no & 0x1Fu;
    cd->status   = cd_make_stat(cd);

    if ((cd->irq_flag & cd->irq_enable) != 0u) {
        irq_raise(sys, IRQ_CDROM);
    }
}

static void cd_clear_pending(PsxCdrom *cd) {
    cd->pending_count = 0u;
    if (cd->img) {
        cd->img->pending_delay[0] = 0u;
        cd->img->pending_delay[1] = 0u;
    }
}

static void cd_queue_int(PsxSystem *sys, u8 int_no, const u8 *resp, u8 resp_len, u32 delay) {
    PsxCdrom *cd = &sys->cdrom;
    if (delay == 0u) {
        delay = PSX_CD_ACK_CYCLES;
    }
    u8 slot = (cd->pending_count < 2u) ? cd->pending_count++ : 1u;
    u8 n    = (resp_len > 16u) ? 16u : resp_len;

    cd->pending_int[slot] = int_no & 0x1Fu;
    cd->pending_n[slot]   = n;
    if (n > 0u && resp) {
        memcpy(cd->pending_resp[slot], resp, n);
    }
    if (cd->img) {
        cd->img->pending_delay[slot] = delay;
    }

    if (slot == 0u && cd->irq_flag == 0u) {
        sched_schedule(sys, EV_CDROM, delay);
    }
}

static void cd_queue_int_1b(PsxSystem *sys, u8 int_no, u8 b0, u32 delay) {
    cd_queue_int(sys, int_no, &b0, 1u, delay);
}

static void cd_queue_err(PsxSystem *sys, u8 stat_bits, u8 err_code) {
    PsxCdrom *cd = &sys->cdrom;
    u8 r[2];
    r[0] = (u8)(cd_make_stat(cd) | stat_bits);
    r[1] = err_code;
    cd_queue_int(sys, CD_INT5_ERROR, r, 2u, PSX_CD_ACK_CYCLES);
}

static u8 cd_effective_region(const PsxSystem *sys) {
    switch (sys->cfg.region) {
    case PSX_REGION_NTSC_U:
        return 'A';
    case PSX_REGION_PAL:
        return 'E';
    case PSX_REGION_NTSC_J:
        return 'I';
    case PSX_REGION_AUTO:
    default:
        return (sys->cdrom.region != 0u) ? sys->cdrom.region : 'A';
    }
}

static void cd_exec_command(PsxSystem *sys, u8 cmd) {
    PsxCdrom *cd = &sys->cdrom;
    cd->cmd        = cmd;
    cd->state_tick = sched_now(sys);

    /* New command supersedes any queued completion of a previous command */
    cd_clear_pending(cd);
    if (cmd != 0x02u && cmd != 0x0Du && cmd != 0x0Eu &&
        cmd != 0x0Bu && cmd != 0x0Cu && cmd != 0x01u &&
        cmd != 0x0Fu && cmd != 0x10u && cmd != 0x11u) {
        cd->state = CD_STATE_IDLE;
    }

    u8 params[16];
    u8 pn = cd->param_n;
    if (pn > 0u) {
        memcpy(params, cd->param_fifo, pn);
    }
    cd->param_n = 0u;

    switch (cmd) {
    case 0x00u: /* Sync */
    case 0x01u: /* Getstat */ {
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x02u: /* Setloc(amm, ass, asect) */ {
        if (pn < 3u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x20u);
            break;
        }
        bool ok_m = false, ok_s = false, ok_f = false;
        u8 mm = cd_bcd_to_bin(params[0], &ok_m);
        u8 ss = cd_bcd_to_bin(params[1], &ok_s);
        u8 ff = cd_bcd_to_bin(params[2], &ok_f);
        if (!ok_m || !ok_s || !ok_f || ss >= 60u || ff >= 75u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x10u);
            break;
        }
        cd->setloc_lba     = cd_msf_to_lba(mm, ss, ff);
        cd->setloc_pending = true;
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x03u: /* Play([track_bcd]) */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        if (pn >= 1u && params[0] != 0u) {
            bool ok = false;
            u8 tno = cd_bcd_to_bin(params[0], &ok);
            if (!ok || tno == 0u || tno > cd->img->last_track) {
                cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x10u);
                break;
            }
            for (u32 i = 0u; i < cd->img->track_count; i++) {
                if (cd->img->tracks[i].track_no == tno) {
                    cd->cur_lba = cd->img->tracks[i].start_lba;
                    break;
                }
            }
            cd->setloc_pending = false;
        } else if (cd->setloc_pending) {
            cd->cur_lba        = cd->setloc_lba;
            cd->setloc_pending = false;
        }
        cd->img->motor_on = true;
        cd->reading       = false;
        cd->playing       = true;
        cd->paused        = false;
        cdrom_ra_seek(sys, cd->cur_lba);
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x04u: /* Forward */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        if (cd->cur_lba + 30u < cd->img->total_lba) {
            cd->cur_lba += 30u;
        }
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x05u: /* Backward */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        cd->cur_lba = (cd->cur_lba > 30u) ? (cd->cur_lba - 30u) : 0u;
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x06u: /* ReadN */
    case 0x1Bu: /* ReadS */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        if (cd->setloc_pending) {
            cd->cur_lba        = cd->setloc_lba;
            cd->setloc_pending = false;
            cdrom_ra_seek(sys, cd->cur_lba);
        }
        cd->img->motor_on = true;
        cd->reading       = true;
        cd->playing       = false;
        cd->paused        = false;
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x07u: /* Standby */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        cd->reading       = false;
        cd->playing       = false;
        cd->img->motor_on = true;
        u8 st = cd_make_stat(cd);
        cd_queue_int_1b(sys, CD_INT3_ACK, st, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st, PSX_CD_INIT_CYCLES);
        break;
    }

    case 0x08u: /* Stop */ {
        u8 st_before  = cd_make_stat(cd);
        cd->reading   = false;
        cd->playing   = false;
        cd->paused    = true;
        if (cd->img) {
            cd->img->motor_on = false;
        }
        u8 st_after = cd_make_stat(cd);
        cd_queue_int_1b(sys, CD_INT3_ACK, st_before, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st_after, PSX_CD_INIT_CYCLES);
        break;
    }

    case 0x09u: /* Pause */ {
        bool was_active = cd->reading || cd->playing;
        u8   st_before  = cd_make_stat(cd);
        cd->reading     = false;
        cd->playing     = false;
        cd->paused      = true;
        u8   st_after   = cd_make_stat(cd);
        u32  pdelay     = was_active
                              ? (cd->double_speed ? (PSX_CD_PAUSE_CYCLES_1X >> 1) : PSX_CD_PAUSE_CYCLES_1X)
                              : PSX_CD_ACK_CYCLES;
        cd_queue_int_1b(sys, CD_INT3_ACK, st_before, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st_after, pdelay);
        break;
    }

    case 0x0Au: /* Init */ {
        u8 st_before     = cd_make_stat(cd);
        cd->reading      = false;
        cd->playing      = false;
        cd->paused       = false;
        cd->muted        = false;
        cd->mode         = 0x00u;
        cd->double_speed = false;
        cd->xa_filter_on = false;
        if (cd->img) {
            cd->img->motor_on     = cd->img->loaded;
            cd->img->header_valid = false;
            cd->img->bfrd         = false;
        }
        cd->data_ready = false;
        cd->data_r     = 0u;
        cd->data_n     = 0u;
        spu_xa_reset();
        u8 st_after = cd_make_stat(cd);
        cd_queue_int_1b(sys, CD_INT3_ACK, st_before, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st_after, PSX_CD_INIT_CYCLES);
        break;
    }

    case 0x0Bu: /* Mute */
        cd->muted = true;
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;

    case 0x0Cu: /* Demute */
        cd->muted = false;
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;

    case 0x0Du: /* Setfilter(file, channel) */ {
        if (pn < 2u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x20u);
            break;
        }
        cd->xa_file    = params[0];
        cd->xa_channel = params[1];
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x0Eu: /* Setmode(mode) */ {
        if (pn < 1u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x20u);
            break;
        }
        cd->mode         = params[0];
        cd->double_speed = (cd->mode & 0x80u) != 0u;
        cd->xa_filter_on = (cd->mode & 0x08u) != 0u;
        cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x0Fu: /* Getparam -> (stat, mode, 0, file, channel) */ {
        u8 r[5];
        r[0] = cd_make_stat(cd);
        r[1] = cd->mode;
        r[2] = 0x00u;
        r[3] = cd->xa_file;
        r[4] = cd->xa_channel;
        cd_queue_int(sys, CD_INT3_ACK, r, 5u, PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x10u: /* GetlocL -> 8 bytes (amm, ass, asect, mode, file, channel, sm, ci) */ {
        if (!cd_disc_present(cd) || !cd->img->header_valid) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x80u);
            break;
        }
        cd_queue_int(sys, CD_INT3_ACK, cd->img->last_header, 8u, PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x11u: /* GetlocP -> 8 bytes (track, index, mm, ss, sect, amm, ass, asect) */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        const CdTrack *t = cd_image_find_track(cd->img, cd->cur_lba);
        u8 tno   = t ? t->track_no : 1u;
        u8 index = 1u;
        u32 rel  = 0u;
        if (t) {
            if (cd->cur_lba < t->start_lba) {
                index = 0u;
                rel   = t->start_lba - cd->cur_lba;
            } else {
                index = 1u;
                rel   = cd->cur_lba - t->start_lba;
            }
        }
        u8 rmm = (u8)(rel / (75u * 60u));
        u8 rss = (u8)((rel / 75u) % 60u);
        u8 rff = (u8)(rel % 75u);
        u8 amm = 0u, ass = 0u, aff = 0u;
        cd_lba_to_msf(cd->cur_lba, &amm, &ass, &aff);

        u8 r[8];
        r[0] = cd_bin_to_bcd(tno);
        r[1] = cd_bin_to_bcd(index);
        r[2] = cd_bin_to_bcd(rmm);
        r[3] = cd_bin_to_bcd(rss);
        r[4] = cd_bin_to_bcd(rff);
        r[5] = cd_bin_to_bcd(amm);
        r[6] = cd_bin_to_bcd(ass);
        r[7] = cd_bin_to_bcd(aff);
        cd_queue_int(sys, CD_INT3_ACK, r, 8u, PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x12u: /* SetSession(session) */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        if (pn < 1u || params[0] == 0u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x10u);
            break;
        }
        cd->reading       = false;
        cd->playing       = false;
        cd->img->motor_on = true;
        u8 st = cd_make_stat(cd);
        cd_queue_int_1b(sys, CD_INT3_ACK, st, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st, PSX_CD_INIT_CYCLES);
        break;
    }

    case 0x13u: /* GetTN -> (stat, first_bcd, last_bcd) */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        u8 r[3];
        r[0] = cd_make_stat(cd);
        r[1] = cd_bin_to_bcd(cd->img->first_track);
        r[2] = cd_bin_to_bcd(cd->img->last_track);
        cd_queue_int(sys, CD_INT3_ACK, r, 3u, PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x14u: /* GetTD(track_bcd) -> (stat, mm_bcd, ss_bcd) */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        if (pn < 1u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x20u);
            break;
        }
        bool ok = false;
        u8 tno = cd_bcd_to_bin(params[0], &ok);
        if (!ok || tno > cd->img->last_track) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x10u);
            break;
        }
        u32 lba = cd->img->total_lba;
        if (tno != 0u) {
            bool found = false;
            for (u32 i = 0u; i < cd->img->track_count; i++) {
                if (cd->img->tracks[i].track_no == tno) {
                    lba   = cd->img->tracks[i].start_lba;
                    found = true;
                    break;
                }
            }
            if (!found) {
                cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x10u);
                break;
            }
        }
        u8 mm = 0u, ss = 0u, ff = 0u;
        cd_lba_to_msf(lba, &mm, &ss, &ff);
        (void)ff;
        u8 r[3];
        r[0] = cd_make_stat(cd);
        r[1] = cd_bin_to_bcd(mm);
        r[2] = cd_bin_to_bcd(ss);
        cd_queue_int(sys, CD_INT3_ACK, r, 3u, PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x15u: /* SeekL */
    case 0x16u: /* SeekP */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        u32 prev_lba = cd->cur_lba;
        if (cd->setloc_pending) {
            cd->seek_lba       = cd->setloc_lba;
            cd->setloc_pending = false;
        } else {
            cd->seek_lba = cd->cur_lba;
        }
        cd->reading           = false;
        cd->playing           = false;
        cd->paused            = false;
        cd->img->motor_on     = true;
        cd->img->header_valid = false;
        cd->state             = (cmd == 0x15u) ? CD_STATE_SEEKL_PENDING : CD_STATE_SEEKP_PENDING;
        cdrom_ra_seek(sys, cd->seek_lba);

        u32 dist = (cd->seek_lba > prev_lba) ? (cd->seek_lba - prev_lba) : (prev_lba - cd->seek_lba);
        u32 sdelay = PSX_CD_SEEK_BASE_CYCLES + dist * PSX_CD_SEEK_PER_SECTOR;
        if (sdelay > 0x00100000u) {
            sdelay = 0x00100000u;
        }

        u8 st_seeking = (u8)(CD_STAT_MOTOR_ON | CD_STAT_SEEK);
        u8 st_done    = CD_STAT_MOTOR_ON;
        cd_queue_int_1b(sys, CD_INT3_ACK, st_seeking, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st_done, sdelay);
        break;
    }

    case 0x19u: /* Test(subfunc) */ {
        if (pn < 1u) {
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x20u);
            break;
        }
        switch (params[0]) {
        case 0x20u: {
            /* CD-ROM BIOS version: 97/01/10, C2 */
            static const u8 k_ver[4] = { 0x97u, 0x01u, 0x10u, 0xC2u };
            cd_queue_int(sys, CD_INT3_ACK, k_ver, 4u, PSX_CD_ACK_CYCLES);
            break;
        }
        case 0x22u: {
            static const u8 k_reg[10] = { 'f', 'o', 'r', ' ', 'U', 'S', '/', 'A', 'E', 'P' };
            cd_queue_int(sys, CD_INT3_ACK, k_reg, 10u, PSX_CD_ACK_CYCLES);
            break;
        }
        case 0x04u: {
            cd_queue_int_1b(sys, CD_INT3_ACK, cd_make_stat(cd), PSX_CD_ACK_CYCLES);
            break;
        }
        case 0x05u: {
            /*
             * Read SCEx counters (total_reads, success_count):
             *  - In the Lead-In area (cur_lba < 150) while not playing CD-DA / reading data,
             *    return 0x01, 0x01 for BIOS / early Lead-In checks.
             *  - While playing CD-DA, reading data sectors, or positioned in the data/audio
             *    area (cur_lba >= 150), genuine hardware reads 0 SCEx strings (0x00, 0x00);
             *    returning non-zero here trips 1999+ anti-modchip checks (e.g. Winning
             *    Eleven 4 "強制終了しました。本体が改造されているおそれがあります。").
             */
            u8 scex = (cd->playing || cd->reading || cd->cur_lba >= 150u) ? 0x00u : 0x01u;
            u8 r[3] = { cd_make_stat(cd), scex, scex };
            cd_queue_int(sys, CD_INT3_ACK, r, 3u, PSX_CD_ACK_CYCLES);
            break;
        }
        case 0x60u: {
            u8 zero = 0x00u;
            cd_queue_int(sys, CD_INT3_ACK, &zero, 1u, PSX_CD_ACK_CYCLES);
            break;
        }
        default:
            cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x10u);
            break;
        }
        break;
    }

    case 0x1Au: /* GetID */ {
        if (!cd_disc_present(cd)) {
            static const u8 k_nodisc[8] = { 0x08u, 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
            cd_queue_int_1b(sys, CD_INT3_ACK, CD_STAT_SHELL_OPEN, PSX_CD_ACK_CYCLES);
            cd_queue_int(sys, CD_INT5_ERROR, k_nodisc, 8u, PSX_CD_ACK_CYCLES);
            break;
        }
        cd->img->motor_on = true;
        u8 st = cd_make_stat(cd);
        if (cd->img->tracks[0].mode == CD_TRACK_AUDIO_2352) {
            static const u8 k_audiocd[8] = { 0x0Au, 0x90u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
            cd_queue_int_1b(sys, CD_INT3_ACK, st, PSX_CD_ACK_CYCLES);
            cd_queue_int(sys, CD_INT5_ERROR, k_audiocd, 8u, PSX_CD_ACK_CYCLES);
            break;
        }
        u8 reg_ch = cd_effective_region(sys);
        u8 id[8]  = { st, 0x00u, 0x20u, 0x00u, 'S', 'C', 'E', reg_ch };
        cd_queue_int_1b(sys, CD_INT3_ACK, st, PSX_CD_ACK_CYCLES);
        cd_queue_int(sys, CD_INT2_COMPLETE, id, 8u, PSX_CD_ACK_CYCLES);
        break;
    }

    case 0x1Eu: /* ReadTOC */ {
        if (!cd_disc_present(cd)) {
            cd_queue_err(sys, CD_STAT_ERROR, 0x80u);
            break;
        }
        cd->img->motor_on = true;
        u8 st = cd_make_stat(cd);
        cd_queue_int_1b(sys, CD_INT3_ACK, st, PSX_CD_ACK_CYCLES);
        cd_queue_int_1b(sys, CD_INT2_COMPLETE, st, PSX_CD_INIT_CYCLES);
        break;
    }

    default: {
        static u8 s_unsupported_seen[32];
        if ((s_unsupported_seen[cmd >> 3] & (1u << (cmd & 7u))) == 0u) {
            s_unsupported_seen[cmd >> 3] |= (u8)(1u << (cmd & 7u));
            plat_log("[cdrom] unsupported command 0x%02X (%u params) -> INT5\n", (unsigned)cmd,
                     (unsigned)pn);
        }
        cd_queue_err(sys, CD_STAT_SEEK_ERR, 0x40u);
        break;
    }
    }
}

void cdrom_event_cb(PsxSystem *sys, u32 late_cycles) {
    (void)late_cycles;
    PsxCdrom *cd = &sys->cdrom;

    /* Never overwrite an unacknowledged interrupt; re-poll after ack delay */
    if (cd->irq_flag != 0u) {
        sched_schedule(sys, EV_CDROM, PSX_CD_ACK_CYCLES);
        return;
    }

    if (cd->pending_count > 0u) {
        u8 int_no = cd->pending_int[0];
        u8 n      = cd->pending_n[0];
        u8 resp[16];
        if (n > 0u) {
            memcpy(resp, cd->pending_resp[0], n);
        }

        if (cd->pending_count > 1u) {
            cd->pending_int[0] = cd->pending_int[1];
            cd->pending_n[0]   = cd->pending_n[1];
            memcpy(cd->pending_resp[0], cd->pending_resp[1], 16u);
            if (cd->img) {
                cd->img->pending_delay[0] = cd->img->pending_delay[1];
                cd->img->pending_delay[1] = 0u;
            }
        }
        cd->pending_count--;

        /* Apply seek completion latch right before INT2 fires */
        if (int_no == CD_INT2_COMPLETE) {
            if (cd->state == CD_STATE_SEEKL_PENDING) {
                cd->cur_lba = cd->seek_lba;
                cd->state   = CD_STATE_IDLE;
                if (cd_disc_present(cd) && cd->cur_lba < cd->img->total_lba) {
                    if (cdrom_ra_peek_or_read_sector(sys, cd->cur_lba, cd->sector)) {
                        const CdTrack *t = cd_image_find_track(cd->img, cd->cur_lba);
                        if (!t || t->mode != CD_TRACK_AUDIO_2352) {
                            memcpy(cd->img->last_header, &cd->sector[12], 8u);
                            cd->img->header_valid = true;
                        }
                    }
                }
            } else if (cd->state == CD_STATE_SEEKP_PENDING) {
                cd->cur_lba = cd->seek_lba;
                cd->state   = CD_STATE_IDLE;
            }
        }

        cd_deliver_int_now(sys, int_no, resp, n);
        return;
    }

    if (cd->reading) {
        if (!cd_disc_present(cd) || cd->cur_lba >= cd->img->total_lba) {
            cd->reading = false;
            u8 st = cd_make_stat(cd);
            cd_deliver_int_now(sys, CD_INT4_DATAEND, &st, 1u);
            return;
        }

        u32 read_lba = cd->cur_lba;
        if (!cdrom_ra_read_sector(sys, read_lba, cd->sector)) {
            static u32 s_read_errors;
            if (s_read_errors++ < 8u) {
                plat_log("[cdrom] read error at LBA %u (disc has %u sectors)\n",
                         (unsigned)read_lba, (unsigned)cd->img->total_lba);
            }
        }
        cd->cur_lba = read_lba + 1u;
        cd->sectors_read++;

        bool xa_en       = (cd->mode & 0x40u) != 0u;
        bool is_mode2    = (cd->sector[15] == 0x02u);
        u8   submode     = cd->sector[18];
        bool is_rt_audio = is_mode2 && ((submode & 0x44u) == 0x44u);

        if (xa_en && is_rt_audio) {
            bool match = !cd->xa_filter_on ||
                         (cd->sector[16] == cd->xa_file &&
                          (cd->sector[17] & 0x1Fu) == (cd->xa_channel & 0x1Fu));
            if (match && !cd->muted) {
                spu_xa_decode_sector(sys, cd->sector, cd->vol);
            }
            if (match && ((cd->mode & 0x02u) != 0u) && ((submode & 0x80u) != 0u)) {
                cd->reading = false;
                u8 st = cd_make_stat(cd);
                cd_deliver_int_now(sys, CD_INT4_DATAEND, &st, 1u);
                return;
            }
            sched_schedule(sys, EV_CDROM, cd_sector_period(cd));
            return;
        }

        /* Standard data sector delivery -> INT1 */
        memcpy(cd->img->last_header, &cd->sector[12], 8u);
        cd->img->header_valid = true;
        if (cd->img->bfrd) {
            cd_load_data_fifo(cd);
        } else {
            cd->data_ready = false;
            cd->data_r     = 0u;
            cd->data_n     = 0u;
        }

        u8 st = cd_make_stat(cd);
        cd_deliver_int_now(sys, CD_INT1_DATARDY, &st, 1u);
        return;
    }

    if (cd->playing) {
        if (!cd_disc_present(cd) || cd->cur_lba >= cd->img->total_lba) {
            cd->playing = false;
            u8 st = cd_make_stat(cd);
            cd_deliver_int_now(sys, CD_INT4_DATAEND, &st, 1u);
            return;
        }

        u32 play_lba = cd->cur_lba;
        (void)cdrom_ra_read_sector(sys, play_lba, cd->sector);
        if (!cd->muted) {
            spu_cdda_push_sector(sys, cd->sector, cd->vol);
        }
        cd->cur_lba = play_lba + 1u;

        if ((cd->mode & 0x02u) != 0u) {
            const CdTrack *tc = cd_image_find_track(cd->img, play_lba);
            const CdTrack *tn = cd_image_find_track(cd->img, cd->cur_lba);
            if (cd->cur_lba >= cd->img->total_lba ||
                (tc && tn && tn->track_no != tc->track_no)) {
                cd->playing = false;
                u8 st = cd_make_stat(cd);
                cd_deliver_int_now(sys, CD_INT4_DATAEND, &st, 1u);
                return;
            }
        }

        if ((cd->mode & 0x04u) != 0u && ((play_lba & 0x0Fu) == 0u)) {
            const CdTrack *tc = cd_image_find_track(cd->img, play_lba);
            u8 tno = tc ? tc->track_no : 1u;
            u32 rel = (tc && play_lba >= tc->start_lba) ? (play_lba - tc->start_lba) : 0u;
            u8 mm = 0u, ss = 0u, ff = 0u;
            if ((play_lba & 0x10u) != 0u) {
                mm = (u8)(rel / (75u * 60u));
                ss = (u8)((rel / 75u) % 60u) | 0x80u;
                ff = (u8)(rel % 75u);
            } else {
                cd_lba_to_msf(play_lba, &mm, &ss, &ff);
            }
            u8 rep[8];
            rep[0] = cd_make_stat(cd);
            rep[1] = cd_bin_to_bcd(tno);
            rep[2] = 0x01u;
            rep[3] = cd_bin_to_bcd((u8)(mm & 0x7Fu));
            rep[4] = (u8)(cd_bin_to_bcd((u8)(ss & 0x7Fu)) | (ss & 0x80u));
            rep[5] = cd_bin_to_bcd(ff);
            rep[6] = 0x00u;
            rep[7] = 0x00u;
            cd_deliver_int_now(sys, CD_INT1_DATARDY, rep, 8u);
            return;
        }

        sched_schedule(sys, EV_CDROM, PSX_CD_SECTOR_CYCLES_1X);
    }
}

static u8 cd_pop_data_byte(PsxCdrom *cd) {
    if (!cd->data_ready || cd->data_r >= cd->data_n) {
        return 0u;
    }
    return cd->data_fifo[cd->data_r++];
}

u32 cdrom_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    PsxCdrom *cd = &sys->cdrom;
    u32 reg = addr & 0x03u;

    /* 16-bit and 32-bit reads from 0x1F801802 Data FIFO */
    if (reg == 2u && width > 1u) {
        if (width == 2u) {
            u32 b0 = cd_pop_data_byte(cd);
            u32 b1 = cd_pop_data_byte(cd);
            return b0 | (b1 << 8);
        }
        u32 b0 = cd_pop_data_byte(cd);
        u32 b1 = cd_pop_data_byte(cd);
        u32 b2 = cd_pop_data_byte(cd);
        u32 b3 = cd_pop_data_byte(cd);
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    if (width > 1u) {
        u32 out = 0u;
        for (u32 i = 0u; i < width; i++) {
            out |= (cdrom_mmio_read(sys, addr + i, 1u) & 0xFFu) << (i * 8u);
        }
        return out;
    }

    switch (reg) {
    case 0u: { /* 0x1F801800 Status */
        u8 st = cd->index & 0x03u;
        if (cd->param_n == 0u) {
            st |= (1u << 3); /* PRMEMPT */
        }
        if (cd->param_n < 16u) {
            st |= (1u << 4); /* PRMWRDY */
        }
        if (cd->resp_r < cd->resp_w) {
            st |= (1u << 5); /* RSLRRDY */
        }
        if (cd->data_ready && cd->data_r < cd->data_n) {
            st |= (1u << 6); /* DRQSTS */
        }
        return st;
    }

    case 1u: /* 0x1F801801 Response FIFO (mirrored across indices 0..3) */
        if (cd->resp_r < cd->resp_w) {
            return cd->resp_fifo[cd->resp_r++];
        }
        return 0u;

    case 2u: /* 0x1F801802 Data FIFO */
        return cd_pop_data_byte(cd);

    case 3u: /* 0x1F801803 HINTMSK (idx 0,2) / HINTSTS (idx 1,3) */
        if ((cd->index & 1u) == 0u) {
            return 0xE0u | (cd->irq_enable & 0x1Fu);
        }
        return 0xE0u | (cd->irq_flag & 0x1Fu);

    default:
        return 0u;
    }
}

void cdrom_mmio_write(PsxSystem *sys, u32 addr, u32 val, u32 width) {
    if (width > 1u) {
        for (u32 i = 0u; i < width; i++) {
            cdrom_mmio_write(sys, addr + i, (val >> (i * 8u)) & 0xFFu, 1u);
        }
        return;
    }

    PsxCdrom *cd = &sys->cdrom;
    u32 reg = addr & 0x03u;
    u8  b   = (u8)(val & 0xFFu);

    if (reg == 0u) {
        cd->index = b & 0x03u;
        return;
    }

    switch (cd->index & 0x03u) {
    case 0u:
        if (reg == 1u) {
            cd_exec_command(sys, b);
        } else if (reg == 2u) {
            if (cd->param_n < 16u) {
                cd->param_fifo[cd->param_n++] = b;
            }
        } else if (reg == 3u) {
            bool want_data = (b & 0x80u) != 0u;
            if (cd->img) {
                cd->img->bfrd = want_data;
            }
            if (want_data) {
                if (!cd->data_ready) {
                    cd_load_data_fifo(cd);
                }
            } else {
                cd->data_ready = false;
                cd->data_r     = 0u;
                cd->data_n     = 0u;
            }
        }
        break;

    case 1u:
        if (reg == 2u) {
            cd->irq_enable = b & 0x1Fu;
            if ((cd->irq_flag & cd->irq_enable) != 0u) {
                irq_raise(sys, IRQ_CDROM);
            }
        } else if (reg == 3u) {
            if ((b & 0x40u) != 0u) {
                cd->param_n = 0u;
            }
            u8 prev_flag = cd->irq_flag;
            cd->irq_flag &= (u8)~(b & 0x1Fu);

            if (prev_flag != 0u && cd->irq_flag == 0u) {
                if (cd->pending_count > 0u) {
                    u32 d = (cd->img && cd->img->pending_delay[0] > 0u)
                                ? cd->img->pending_delay[0]
                                : PSX_CD_ACK_CYCLES;
                    sched_schedule(sys, EV_CDROM, d);
                } else if (cd->reading) {
                    sched_schedule(sys, EV_CDROM, cd_sector_period(cd));
                } else if (cd->playing) {
                    sched_schedule(sys, EV_CDROM, PSX_CD_SECTOR_CYCLES_1X);
                }
            }
        }
        break;

    case 2u:
        if (reg == 2u) {
            cd->vol_pending[0] = b; /* Left -> SPU Left */
        } else if (reg == 3u) {
            cd->vol_pending[1] = b; /* Left -> SPU Right */
        }
        break;

    case 3u:
        if (reg == 1u) {
            cd->vol_pending[3] = b; /* Right -> SPU Right */
        } else if (reg == 2u) {
            cd->vol_pending[2] = b; /* Right -> SPU Left */
        } else if (reg == 3u) {
            if ((b & 0x20u) != 0u) {
                memcpy(cd->vol, cd->vol_pending, 4u);
            }
        }
        break;
    }
}

u32 cdrom_dma_read_words(PsxSystem *sys, u32 *dst, u32 n) {
    if (!sys || !dst || n == 0u) {
        return 0u;
    }
    PsxCdrom *cd = &sys->cdrom;
    for (u32 i = 0u; i < n; i++) {
        u32 b0 = cd_pop_data_byte(cd);
        u32 b1 = cd_pop_data_byte(cd);
        u32 b2 = cd_pop_data_byte(cd);
        u32 b3 = cd_pop_data_byte(cd);
        dst[i] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }
    return n;
}

void cdrom_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCdrom *cd = &sys->cdrom;
    memset(cd, 0, sizeof(*cd));

    cd->img = (CdImage *)arena_alloc(&sys->arena, (u32)sizeof(CdImage), 32u);
    cd_image_init(cd->img);
    if (cd->img) {
        cd->img->small_mem = sys->cfg.small_mem;
    }
    cdrom_ra_init(sys);

    bus_mmio_register(sys, PSX_REG_CDROM_BASE, 0x10u, cdrom_mmio_read, cdrom_mmio_write);
    sched_set_handler(&sys->sched, EV_CDROM, cdrom_event_cb);

    cdrom_reset(sys);
}

void cdrom_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxCdrom *cd  = &sys->cdrom;
    CdImage  *img = cd->img;
    CdReadahead ra_save = cd->ra;

    memset(cd, 0, sizeof(*cd));
    cd->img = img;
    cd->ra  = ra_save;

    /* Default CD-to-SPU unity stereo mixing matrix (0x80 = 1.0x) */
    cd->vol[0]         = 0x80u;
    cd->vol[1]         = 0x00u;
    cd->vol[2]         = 0x00u;
    cd->vol[3]         = 0x80u;
    memcpy(cd->vol_pending, cd->vol, 4u);

    if (img) {
        img->motor_on     = img->loaded;
        img->header_valid = false;
        img->bfrd         = false;
        memset(img->last_header, 0, sizeof(img->last_header));
        img->pending_delay[0] = 0u;
        img->pending_delay[1] = 0u;
        cd->region            = img->region_char ? img->region_char : 'A';
    } else {
        cd->region = 'A';
    }

    cd->status = cd_make_stat(cd);
    cdrom_ra_reset(sys);
    sched_cancel(sys, EV_CDROM);
}

PsxResult cdrom_load(PsxSystem *sys, const char *path) {
    if (!sys || !path) {
        return PSX_ERR_BAD_ARG;
    }
    PsxCdrom *cd = &sys->cdrom;
    if (!cd->img) {
        return PSX_ERR_NOMEM;
    }
    PsxResult res = cd_image_open(cd->img, path);
    if (res != PSX_OK) {
        return res;
    }
    cd->region        = cd->img->region_char ? cd->img->region_char : 'A';
    cd->img->motor_on = true;
    cd->cur_lba       = 0u;
    cd->seek_lba      = 0u;
    cd->status        = cd_make_stat(cd);
    cdrom_ra_reset(sys);
    cdrom_ra_seek(sys, 0u);
    return PSX_OK;
}

bool cdrom_is_chd(const PsxSystem *sys) {
    return sys && sys->cdrom.img && sys->cdrom.img->loaded && (sys->cdrom.img->chd != NULL);
}

void cdrom_chd_cache_stats(const PsxSystem *sys, u64 *out_hits, u64 *out_misses, u32 *out_slots) {
    const CdImage *img = sys ? sys->cdrom.img : NULL;
    cd_chd_get_cache_stats(img, out_hits, out_misses, out_slots);
}

void cdrom_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxCdrom *cd = &sys->cdrom;

    state_u8(st, &cd->index);
    state_u8(st, &cd->status);
    state_u8(st, &cd->mode);
    state_u8(st, &cd->irq_enable);
    state_u8(st, &cd->irq_flag);
    state_bytes(st, cd->param_fifo, 16u);
    state_bytes(st, cd->resp_fifo, 16u);
    state_u8(st, &cd->param_n);
    state_u8(st, &cd->resp_r);
    state_u8(st, &cd->resp_w);
    state_bytes(st, cd->sector, 2352u);
    state_bytes(st, cd->data_fifo, 2352u);
    state_u32(st, &cd->data_r);
    state_u32(st, &cd->data_n);
    state_bool(st, &cd->data_ready);
    state_bytes(st, cd->pending_int, 2u);
    state_bytes(st, cd->pending_resp, 32u);
    state_bytes(st, cd->pending_n, 2u);
    state_u8(st, &cd->pending_count);
    state_u32(st, &cd->seek_lba);
    state_u32(st, &cd->cur_lba);
    state_u32(st, &cd->setloc_lba);
    state_bool(st, &cd->setloc_pending);
    state_bool(st, &cd->reading);
    state_bool(st, &cd->playing);
    state_bool(st, &cd->paused);
    state_bool(st, &cd->muted);
    state_bool(st, &cd->double_speed);
    state_bool(st, &cd->xa_filter_on);
    state_u8(st, &cd->xa_file);
    state_u8(st, &cd->xa_channel);
    state_u8(st, &cd->cmd);
    state_u8(st, &cd->state);
    state_u32(st, &cd->state_tick);
    state_bytes(st, cd->vol, 4u);
    state_bytes(st, cd->vol_pending, 4u);
    state_u8(st, &cd->region);

    if (cd->img) {
        state_bool(st, &cd->img->motor_on);
        state_bool(st, &cd->img->header_valid);
        state_bool(st, &cd->img->bfrd);
        state_bytes(st, cd->img->last_header, 8u);
        state_bytes(st, cd->img->pending_delay, 8u);
    }

    if (!st->writing) {
        cdrom_ra_seek(sys, cd->cur_lba);
    }
}

void cdrom_shutdown(PsxSystem *sys) {
    if (!sys || !sys->cdrom.img) {
        return;
    }
    cd_image_close(sys->cdrom.img);
}
