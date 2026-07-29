#include "h264_sps.h"
#include <string.h>

/* Bounds-checked bit reader over an already RBSP-unescaped buffer. Any
 * read past the end sets `failed` and returns 0 without advancing further,
 * so callers don't need to guard every single field read -- just check
 * b.failed once at the end. */
typedef struct {
    const uint8_t *data;
    size_t nbits;
    size_t pos;
    int failed;
} br_t;

static uint32_t br_bits(br_t *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (b->failed || b->pos >= b->nbits) { b->failed = 1; return 0; }
        size_t byte = b->pos >> 3;
        int bit = 7 - (int)(b->pos & 7);
        v = (v << 1) | (uint32_t)((b->data[byte] >> bit) & 1);
        b->pos++;
    }
    return v;
}

static uint32_t br_ue(br_t *b) {
    int lz = 0;
    while (!b->failed && br_bits(b, 1) == 0) {
        lz++;
        if (lz > 32) { b->failed = 1; return 0; }
    }
    if (b->failed || lz == 0)
        return 0;
    uint32_t suf = br_bits(b, lz);
    return (((uint32_t)1 << lz) - 1) + suf;
}

static int32_t br_se(br_t *b) {
    uint32_t k = br_ue(b);
    int32_t v = (int32_t)((k + 1) >> 1);
    return (k & 1) ? v : -v;
}

/* Skips one scaling_list() (Table 7.3.2.1.1.1) without needing the values;
 * only the bit position matters for everything that follows in the SPS. */
static void skip_scaling_list(br_t *b, int size) {
    int last_scale = 8, next_scale = 8;
    for (int j = 0; j < size && !b->failed; j++) {
        if (next_scale != 0) {
            int32_t delta = br_se(b);
            next_scale = (last_scale + delta + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

int h264_sps_get_resolution(const uint8_t *nal, size_t len, int *out_width, int *out_height) {
    if (!nal || len < 4)
        return 0;
    if ((nal[0] & 0x1F) != 7)
        return 0; /* not an SPS */

    /* RBSP-unescape (skip the 1-byte NAL header; anti-emulation applies to
     * everything after it): 00 00 03 -> 00 00. */
    uint8_t rbsp[512];
    size_t rbsp_len = 0;
    const uint8_t *p = nal + 1;
    size_t in_len = len - 1;
    for (size_t i = 0; i < in_len && rbsp_len < sizeof(rbsp); ) {
        if (i + 2 < in_len && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3) {
            rbsp[rbsp_len++] = 0;
            rbsp[rbsp_len++] = 0;
            i += 3;
        } else {
            rbsp[rbsp_len++] = p[i];
            i++;
        }
    }

    br_t b = { rbsp, rbsp_len * 8, 0, 0 };

    uint32_t profile_idc = br_bits(&b, 8);
    br_bits(&b, 8);  /* constraint_set flags + reserved */
    br_bits(&b, 8);  /* level_idc */
    br_ue(&b);       /* seq_parameter_set_id */

    uint32_t chroma_format_idc = 1; /* default when not signaled: 4:2:0 */
    int frame_mbs_only_flag = 1;

    switch (profile_idc) {
    case 100: case 110: case 122: case 244: case 44:
    case 83: case 86: case 118: case 128: case 138:
    case 139: case 134: case 135:
        chroma_format_idc = br_ue(&b);
        if (chroma_format_idc == 3)
            br_bits(&b, 1); /* separate_colour_plane_flag */
        br_ue(&b); /* bit_depth_luma_minus8 */
        br_ue(&b); /* bit_depth_chroma_minus8 */
        br_bits(&b, 1); /* qpprime_y_zero_transform_bypass_flag */
        if (br_bits(&b, 1)) { /* seq_scaling_matrix_present_flag */
            int n = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < n && !b.failed; i++)
                if (br_bits(&b, 1)) /* seq_scaling_list_present_flag[i] */
                    skip_scaling_list(&b, i < 6 ? 16 : 64);
        }
        break;
    default:
        break;
    }

    br_ue(&b); /* log2_max_frame_num_minus4 */
    uint32_t poc_type = br_ue(&b);
    if (poc_type == 0) {
        br_ue(&b); /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        br_bits(&b, 1); /* delta_pic_order_always_zero_flag */
        br_se(&b);      /* offset_for_non_ref_pic */
        br_se(&b);      /* offset_for_top_to_bottom_field */
        uint32_t n = br_ue(&b); /* num_ref_frames_in_pic_order_cnt_cycle */
        if (n > 256) return 0; /* sanity guard against a corrupt/desynced parse */
        for (uint32_t i = 0; i < n && !b.failed; i++)
            br_se(&b);
    }
    br_ue(&b);      /* max_num_ref_frames */
    br_bits(&b, 1); /* gaps_in_frame_num_value_allowed_flag */

    uint32_t pic_width_in_mbs_minus1 = br_ue(&b);
    uint32_t pic_height_in_map_units_minus1 = br_ue(&b);
    frame_mbs_only_flag = (int)br_bits(&b, 1);
    if (!frame_mbs_only_flag)
        br_bits(&b, 1); /* mb_adaptive_frame_field_flag */
    br_bits(&b, 1);      /* direct_8x8_inference_flag */

    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (br_bits(&b, 1)) { /* frame_cropping_flag */
        crop_left = br_ue(&b);
        crop_right = br_ue(&b);
        crop_top = br_ue(&b);
        crop_bottom = br_ue(&b);
    }

    if (b.failed)
        return 0;

    int width = (int)((pic_width_in_mbs_minus1 + 1) * 16);
    int height = (int)((2 - frame_mbs_only_flag) * (int)(pic_height_in_map_units_minus1 + 1) * 16);

    int crop_unit_x, crop_unit_y;
    if (chroma_format_idc == 0) {
        crop_unit_x = 1;
        crop_unit_y = 2 - frame_mbs_only_flag;
    } else {
        static const int sub_w[4] = { 1, 2, 2, 1 }; /* index 0 unused (mono handled above) */
        static const int sub_h[4] = { 1, 2, 1, 1 };
        int idx = (int)(chroma_format_idc & 3);
        crop_unit_x = sub_w[idx];
        crop_unit_y = sub_h[idx] * (2 - frame_mbs_only_flag);
    }
    width -= (int)(crop_unit_x * (crop_left + crop_right));
    height -= (int)(crop_unit_y * (crop_top + crop_bottom));

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return 0; /* implausible -- treat as a parse failure, caller keeps its default */

    *out_width = width;
    *out_height = height;
    return 1;
}
