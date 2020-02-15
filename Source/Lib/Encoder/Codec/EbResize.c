/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */
/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "EbResize.h"
#include "EbReferenceObject.h"

#define DEBUG_SCALING 0
#define DEBUG_REFERENCE_SCALE 0
#define DIVIDE_AND_ROUND(x, y) (((x) + ((y) >> 1)) / (y))

// Filters for factor of 2 downsampling.
static const int16_t av1_down2_symeven_half_filter[] = {56, 12, -3, -1};
static const int16_t av1_down2_symodd_half_filter[]  = {64, 35, 0, -3};

void calculate_scaled_size_helper(uint16_t *dim, uint8_t denom);

void pad_and_decimate_filtered_pic(
        PictureParentControlSet *picture_control_set_ptr_central);

static int get_down2_length(int length, int steps) {
    for (int s = 0; s < steps; ++s) length = (length + 1) >> 1;
    return length;
}

static int get_down2_steps(int in_length, int out_length) {
    int steps = 0;
    int proj_in_length;
    while ((proj_in_length = get_down2_length(in_length, 1)) >= out_length) {
        ++steps;
        in_length = proj_in_length;
        if (in_length == 1) {
            // Special case: we break because any further calls to get_down2_length()
            // with be with length == 1, which return 1, resulting in an infinite
            // loop.
            break;
        }
    }
    return steps;
}

static void down2_symeven(const uint8_t *const input, int length, uint8_t *output) {
    // Actual filter len = 2 * filter_len_half.
    const int16_t *filter          = av1_down2_symeven_half_filter;
    const int      filter_len_half = sizeof(av1_down2_symeven_half_filter) / 2;
    int            i, j;
    uint8_t *      optr = output;
    int            l1   = filter_len_half;
    int            l2   = (length - filter_len_half);
    l1 += (l1 & 1);
    l2 += (l2 & 1);
    if (l1 > l2) {
        // Short input length.
        for (i = 0; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[AOMMAX(i - j, 0)] + input[AOMMIN(i + 1 + j, length - 1)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
    } else {
        // Initial part.
        for (i = 0; i < l1; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[AOMMAX(i - j, 0)] + input[i + 1 + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
        // Middle part.
        for (; i < l2; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[i + 1 + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
        // End part.
        for (; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[AOMMIN(i + 1 + j, length - 1)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
    }
}

static void down2_symodd(const uint8_t *const input, int length, uint8_t *output) {
    // Actual filter len = 2 * filter_len_half - 1.
    const int16_t *filter          = av1_down2_symodd_half_filter;
    const int      filter_len_half = sizeof(av1_down2_symodd_half_filter) / 2;
    int            i, j;
    uint8_t *      optr = output;
    int            l1   = filter_len_half - 1;
    int            l2   = (length - filter_len_half + 1);
    l1 += (l1 & 1);
    l2 += (l2 & 1);
    if (l1 > l2) {
        // Short input length.
        for (i = 0; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[(i - j < 0 ? 0 : i - j)] +
                        input[(i + j >= length ? length - 1 : i + j)]) *
                       filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
    } else {
        // Initial part.
        for (i = 0; i < l1; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[(i - j < 0 ? 0 : i - j)] + input[i + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
        // Middle part.
        for (; i < l2; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[i + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
        // End part.
        for (; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[(i + j >= length ? length - 1 : i + j)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel(sum);
        }
    }
}

static const InterpKernel *choose_interp_filter(int in_length, int out_length) {
    int out_length16 = out_length * 16;
    // TODO: use original filter in libaom
    if (out_length16 >= in_length * 16) return filteredinterp_filters1000;
    if (out_length16 >= in_length * 16)
        return filteredinterp_filters875; // wrong
    else if (out_length16 >= in_length * 13)
        return filteredinterp_filters875;
    else if (out_length16 >= in_length * 11)
        return filteredinterp_filters750;
    else if (out_length16 >= in_length * 9)
        return filteredinterp_filters625;
    else
        return filteredinterp_filters500;
}

static void interpolate_core(const uint8_t *const input, int in_length, uint8_t *output,
                             int out_length, const int16_t *interp_filters, int interp_taps) {
    const int32_t delta =
        (((uint32_t)in_length << RS_SCALE_SUBPEL_BITS) + out_length / 2) / out_length;
    const int32_t offset =
        in_length > out_length
            ? (((int32_t)(in_length - out_length) << (RS_SCALE_SUBPEL_BITS - 1)) + out_length / 2) /
                  out_length
            : -(((int32_t)(out_length - in_length) << (RS_SCALE_SUBPEL_BITS - 1)) +
                out_length / 2) /
                  out_length;
    uint8_t *optr = output;
    int      x, x1, x2, sum, k, int_pel, sub_pel;
    int32_t  y;

    x = 0;
    y = offset + RS_SCALE_EXTRA_OFF;
    while ((y >> RS_SCALE_SUBPEL_BITS) < (interp_taps / 2 - 1)) {
        x++;
        y += delta;
    }
    x1 = x;
    x  = out_length - 1;
    y  = delta * x + offset + RS_SCALE_EXTRA_OFF;
    while ((y >> RS_SCALE_SUBPEL_BITS) + (int32_t)(interp_taps / 2) >= in_length) {
        x--;
        y -= delta;
    }
    x2 = x;
    if (x1 > x2) {
        for (x = 0, y = offset + RS_SCALE_EXTRA_OFF; x < out_length; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k) {
                const int pk = int_pel - interp_taps / 2 + 1 + k;
                sum += filter[k] * input[AOMMAX(AOMMIN(pk, in_length - 1), 0)];
            }
            *optr++ = clip_pixel(ROUND_POWER_OF_TWO(sum, FILTER_BITS));
        }
    } else {
        // Initial part.
        for (x = 0, y = offset + RS_SCALE_EXTRA_OFF; x < x1; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k)
                sum += filter[k] * input[AOMMAX(int_pel - interp_taps / 2 + 1 + k, 0)];
            *optr++ = clip_pixel(ROUND_POWER_OF_TWO(sum, FILTER_BITS));
        }
        // Middle part.
        for (; x <= x2; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k)
                sum += filter[k] * input[int_pel - interp_taps / 2 + 1 + k];
            *optr++ = clip_pixel(ROUND_POWER_OF_TWO(sum, FILTER_BITS));
        }
        // End part.
        for (; x < out_length; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k)
                sum += filter[k] * input[AOMMIN(int_pel - interp_taps / 2 + 1 + k, in_length - 1)];
            *optr++ = clip_pixel(ROUND_POWER_OF_TWO(sum, FILTER_BITS));
        }
    }
}

static void interpolate(const uint8_t *const input, int in_length, uint8_t *output,
                        int out_length) {
    const InterpKernel *interp_filters = choose_interp_filter(in_length, out_length);

    interpolate_core(input, in_length, output, out_length, &interp_filters[0][0], SUBPEL_TAPS);
}

static void resize_multistep(const uint8_t *const input, int length, uint8_t *output, int olength,
                             uint8_t *otmp) {
    if (length == olength) {
        memcpy(output, input, sizeof(output[0]) * length);
        return;
    }
    const int steps = get_down2_steps(length, olength);

    if (steps > 0) {
        uint8_t *out            = NULL;
        int      filteredlength = length;

        assert(otmp != NULL);
        uint8_t *otmp2 = otmp + get_down2_length(length, 1);
        for (int s = 0; s < steps; ++s) {
            const int            proj_filteredlength = get_down2_length(filteredlength, 1);
            const uint8_t *const in                  = (s == 0 ? input : out);
            if (s == steps - 1 && proj_filteredlength == olength)
                out = output;
            else
                out = (s & 1 ? otmp2 : otmp);
            if (filteredlength & 1)
                down2_symodd(in, filteredlength, out);
            else
                down2_symeven(in, filteredlength, out);
            filteredlength = proj_filteredlength;
        }
        if (filteredlength != olength) { interpolate(out, filteredlength, output, olength); }
    } else {
        interpolate(input, length, output, olength);
    }
}

static void fill_arr_to_col(uint8_t *img, int stride, int len, uint8_t *arr) {
    int      i;
    uint8_t *iptr = img;
    uint8_t *aptr = arr;
    for (i = 0; i < len; ++i, iptr += stride) { *iptr = *aptr++; }
}

static void fill_col_to_arr(uint8_t *img, int stride, int len, uint8_t *arr) {
    int      i;
    uint8_t *iptr = img;
    uint8_t *aptr = arr;
    for (i = 0; i < len; ++i, iptr += stride) { *aptr++ = *iptr; }
}

EbErrorType av1_resize_plane(const uint8_t *const input, int height, int width, int in_stride,
                             uint8_t *output, int height2, int width2, int out_stride) {
    int      i;
    uint8_t *intbuf, *tmpbuf, *arrbuf, *arrbuf2;

    assert(width > 0);
    assert(height > 0);
    assert(width2 > 0);
    assert(height2 > 0);

    EB_MALLOC_ARRAY(intbuf, width2 * height);
    EB_MALLOC_ARRAY(tmpbuf, AOMMAX(width, height));
    EB_MALLOC_ARRAY(arrbuf, height);
    EB_MALLOC_ARRAY(arrbuf2, height2);
    if (intbuf == NULL || tmpbuf == NULL || arrbuf == NULL || arrbuf2 == NULL) {
        EB_FREE_ARRAY(intbuf);
        EB_FREE_ARRAY(tmpbuf);
        EB_FREE_ARRAY(arrbuf);
        EB_FREE_ARRAY(arrbuf2);
        return EB_ErrorInsufficientResources;
    }
    for (i = 0; i < height; ++i)
        resize_multistep(input + in_stride * i, width, intbuf + width2 * i, width2, tmpbuf);

    for (i = 0; i < width2; ++i) {
        fill_col_to_arr(intbuf + i, width2, height, arrbuf);
        resize_multistep(arrbuf, height, arrbuf2, height2, tmpbuf);
        fill_arr_to_col(output + i, out_stride, height2, arrbuf2);
    }

    EB_FREE_ARRAY(intbuf);
    EB_FREE_ARRAY(tmpbuf);
    EB_FREE_ARRAY(arrbuf);
    EB_FREE_ARRAY(arrbuf2);

    return EB_ErrorNone;
}

static void highbd_interpolate_core(const uint16_t *const input, int in_length, uint16_t *output,
                                    int out_length, int bd, const int16_t *interp_filters,
                                    int interp_taps) {
    const int32_t delta =
        (((uint32_t)in_length << RS_SCALE_SUBPEL_BITS) + out_length / 2) / out_length;
    const int32_t offset =
        in_length > out_length
            ? (((int32_t)(in_length - out_length) << (RS_SCALE_SUBPEL_BITS - 1)) + out_length / 2) /
                  out_length
            : -(((int32_t)(out_length - in_length) << (RS_SCALE_SUBPEL_BITS - 1)) +
                out_length / 2) /
                  out_length;
    uint16_t *optr = output;
    int       x, x1, x2, sum, k, int_pel, sub_pel;
    int32_t   y;

    x = 0;
    y = offset + RS_SCALE_EXTRA_OFF;
    while ((y >> RS_SCALE_SUBPEL_BITS) < (interp_taps / 2 - 1)) {
        x++;
        y += delta;
    }
    x1 = x;
    x  = out_length - 1;
    y  = delta * x + offset + RS_SCALE_EXTRA_OFF;
    while ((y >> RS_SCALE_SUBPEL_BITS) + (int32_t)(interp_taps / 2) >= in_length) {
        x--;
        y -= delta;
    }
    x2 = x;
    if (x1 > x2) {
        for (x = 0, y = offset + RS_SCALE_EXTRA_OFF; x < out_length; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k) {
                const int pk = int_pel - interp_taps / 2 + 1 + k;
                sum += filter[k] * input[AOMMAX(AOMMIN(pk, in_length - 1), 0)];
            }
            *optr++ = clip_pixel_highbd(ROUND_POWER_OF_TWO(sum, FILTER_BITS), bd);
        }
    } else {
        // Initial part.
        for (x = 0, y = offset + RS_SCALE_EXTRA_OFF; x < x1; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k)
                sum += filter[k] * input[AOMMAX(int_pel - interp_taps / 2 + 1 + k, 0)];
            *optr++ = clip_pixel_highbd(ROUND_POWER_OF_TWO(sum, FILTER_BITS), bd);
        }
        // Middle part.
        for (; x <= x2; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k)
                sum += filter[k] * input[int_pel - interp_taps / 2 + 1 + k];
            *optr++ = clip_pixel_highbd(ROUND_POWER_OF_TWO(sum, FILTER_BITS), bd);
        }
        // End part.
        for (; x < out_length; ++x, y += delta) {
            int_pel               = y >> RS_SCALE_SUBPEL_BITS;
            sub_pel               = (y >> RS_SCALE_EXTRA_BITS) & RS_SUBPEL_MASK;
            const int16_t *filter = &interp_filters[sub_pel * interp_taps];
            sum                   = 0;
            for (k = 0; k < interp_taps; ++k)
                sum += filter[k] * input[AOMMIN(int_pel - interp_taps / 2 + 1 + k, in_length - 1)];
            *optr++ = clip_pixel_highbd(ROUND_POWER_OF_TWO(sum, FILTER_BITS), bd);
        }
    }
}

static void highbd_interpolate(const uint16_t *const input, int in_length, uint16_t *output,
                               int out_length, int bd) {
    const InterpKernel *interp_filters = choose_interp_filter(in_length, out_length);

    highbd_interpolate_core(
        input, in_length, output, out_length, bd, &interp_filters[0][0], SUBPEL_TAPS);
}

static void highbd_down2_symeven(const uint16_t *const input, int length, uint16_t *output,
                                 int bd) {
    // Actual filter len = 2 * filter_len_half.
    static const int16_t *filter          = av1_down2_symeven_half_filter;
    const int             filter_len_half = sizeof(av1_down2_symeven_half_filter) / 2;
    int                   i, j;
    uint16_t *            optr = output;
    int                   l1   = filter_len_half;
    int                   l2   = (length - filter_len_half);
    l1 += (l1 & 1);
    l2 += (l2 & 1);
    if (l1 > l2) {
        // Short input length.
        for (i = 0; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[AOMMAX(0, i - j)] + input[AOMMIN(i + 1 + j, length - 1)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
    } else {
        // Initial part.
        for (i = 0; i < l1; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[AOMMAX(0, i - j)] + input[i + 1 + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
        // Middle part.
        for (; i < l2; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[i + 1 + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
        // End part.
        for (; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1));
            for (j = 0; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[AOMMIN(i + 1 + j, length - 1)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
    }
}

static void highbd_down2_symodd(const uint16_t *const input, int length, uint16_t *output, int bd) {
    // Actual filter len = 2 * filter_len_half - 1.
    static const int16_t *filter          = av1_down2_symodd_half_filter;
    const int             filter_len_half = sizeof(av1_down2_symodd_half_filter) / 2;
    int                   i, j;
    uint16_t *            optr = output;
    int                   l1   = filter_len_half - 1;
    int                   l2   = (length - filter_len_half + 1);
    l1 += (l1 & 1);
    l2 += (l2 & 1);
    if (l1 > l2) {
        // Short input length.
        for (i = 0; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[AOMMAX(i - j, 0)] + input[AOMMIN(i + j, length - 1)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
    } else {
        // Initial part.
        for (i = 0; i < l1; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[AOMMAX(i - j, 0)] + input[i + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
        // Middle part.
        for (; i < l2; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[i + j]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
        // End part.
        for (; i < length; i += 2) {
            int sum = (1 << (FILTER_BITS - 1)) + input[i] * filter[0];
            for (j = 1; j < filter_len_half; ++j) {
                sum += (input[i - j] + input[AOMMIN(i + j, length - 1)]) * filter[j];
            }
            sum >>= FILTER_BITS;
            *optr++ = clip_pixel_highbd(sum, bd);
        }
    }
}

static void highbd_resize_multistep(const uint16_t *const input, int length, uint16_t *output,
                                    int olength, uint16_t *otmp, int bd) {
    if (length == olength) {
        memcpy(output, input, sizeof(output[0]) * length);
        return;
    }
    const int steps = get_down2_steps(length, olength);

    if (steps > 0) {
        uint16_t *out            = NULL;
        int       filteredlength = length;

        assert(otmp != NULL);
        uint16_t *otmp2 = otmp + get_down2_length(length, 1);
        for (int s = 0; s < steps; ++s) {
            const int             proj_filteredlength = get_down2_length(filteredlength, 1);
            const uint16_t *const in                  = (s == 0 ? input : out);
            if (s == steps - 1 && proj_filteredlength == olength)
                out = output;
            else
                out = (s & 1 ? otmp2 : otmp);
            if (filteredlength & 1)
                highbd_down2_symodd(in, filteredlength, out, bd);
            else
                highbd_down2_symeven(in, filteredlength, out, bd);
            filteredlength = proj_filteredlength;
        }
        if (filteredlength != olength) {
            highbd_interpolate(out, filteredlength, output, olength, bd);
        }
    } else {
        highbd_interpolate(input, length, output, olength, bd);
    }
}

static void highbd_fill_col_to_arr(uint16_t *img, int stride, int len, uint16_t *arr) {
    int       i;
    uint16_t *iptr = img;
    uint16_t *aptr = arr;
    for (i = 0; i < len; ++i, iptr += stride) { *aptr++ = *iptr; }
}

static void highbd_fill_arr_to_col(uint16_t *img, int stride, int len, uint16_t *arr) {
    int       i;
    uint16_t *iptr = img;
    uint16_t *aptr = arr;
    for (i = 0; i < len; ++i, iptr += stride) { *iptr = *aptr++; }
}

EbErrorType av1_highbd_resize_plane(const uint16_t *const input, int height, int width,
                                    int in_stride, uint16_t *output, int height2, int width2,
                                    int out_stride, int bd) {
    int       i;
    uint16_t *intbuf;
    uint16_t *tmpbuf;
    uint16_t *arrbuf;
    uint16_t *arrbuf2;

    EB_MALLOC_ARRAY(intbuf, sizeof(uint16_t) * width2 * height);
    EB_MALLOC_ARRAY(tmpbuf, sizeof(uint16_t) * AOMMAX(width, height));
    EB_MALLOC_ARRAY(arrbuf, sizeof(uint16_t) * height);
    EB_MALLOC_ARRAY(arrbuf2, sizeof(uint16_t) * height2);
    if (intbuf == NULL || tmpbuf == NULL || arrbuf == NULL || arrbuf2 == NULL) {
        EB_FREE(intbuf);
        EB_FREE(tmpbuf);
        EB_FREE(arrbuf);
        EB_FREE(arrbuf2);
        return EB_ErrorInsufficientResources;
    }
    for (i = 0; i < height; ++i) {
        highbd_resize_multistep(
            input + in_stride * i, width, intbuf + width2 * i, width2, tmpbuf, bd);
    }
    for (i = 0; i < width2; ++i) {
        highbd_fill_col_to_arr(intbuf + i, width2, height, arrbuf);
        highbd_resize_multistep(arrbuf, height, arrbuf2, height2, tmpbuf, bd);
        highbd_fill_arr_to_col(output + i, out_stride, height2, arrbuf2);
    }

    EB_FREE(intbuf);
    EB_FREE(tmpbuf);
    EB_FREE(arrbuf);
    EB_FREE(arrbuf2);

    return EB_ErrorNone;
}

void pack_highbd_pic(const EbPictureBufferDesc *pic_ptr, uint16_t *buffer_16bit[3], uint32_t ss_x,
                     uint32_t ss_y, EbBool include_padding);

void unpack_highbd_pic(uint16_t *buffer_highbd[3], EbPictureBufferDesc *pic_ptr, uint32_t ss_x,
                       uint32_t ss_y, EbBool include_padding);

void save_YUV_to_file(char *filename, EbByte buffer_y, EbByte buffer_u, EbByte buffer_v,
                      uint16_t width, uint16_t height, uint16_t stride_y, uint16_t stride_u,
                      uint16_t stride_v, uint16_t origin_y, uint16_t origin_x, uint32_t ss_x,
                      uint32_t ss_y);

void save_Y_to_file(char *filename, EbByte buffer_y,
                    uint16_t width, uint16_t height,
                    uint16_t stride_y,
                    uint16_t origin_y, uint16_t origin_x);

void save_YUV_to_file_highbd(char *filename, uint16_t *buffer_y, uint16_t *buffer_u,
                             uint16_t *buffer_v, uint16_t width, uint16_t height, uint16_t stride_y,
                             uint16_t stride_u, uint16_t stride_v, uint16_t origin_y,
                             uint16_t origin_x, uint32_t ss_x, uint32_t ss_y);

EbErrorType av1_resize_and_extend_frame(const EbPictureBufferDesc *src, EbPictureBufferDesc *dst,
                                        int bd, const int num_planes, const uint32_t ss_x,
                                        const uint32_t ss_y) {
    uint16_t *src_buffer_highbd[MAX_MB_PLANE];
    uint16_t *dst_buffer_highbd[MAX_MB_PLANE];

    if (bd > 8) {
        EB_MALLOC_ARRAY(src_buffer_highbd[0], src->luma_size);
        EB_MALLOC_ARRAY(src_buffer_highbd[1], src->chroma_size);
        EB_MALLOC_ARRAY(src_buffer_highbd[2], src->chroma_size);
        EB_MALLOC_ARRAY(dst_buffer_highbd[0], dst->luma_size);
        EB_MALLOC_ARRAY(dst_buffer_highbd[1], dst->chroma_size);
        EB_MALLOC_ARRAY(dst_buffer_highbd[2], dst->chroma_size);
        pack_highbd_pic(src, src_buffer_highbd, ss_x, ss_y, EB_TRUE);
    }

#if DEBUG_SCALING
    if (bd > 8)
        save_YUV_to_file_highbd("unscaled_pic_highbd.yuv",
                                src_buffer_highbd[0],
                                src_buffer_highbd[1],
                                src_buffer_highbd[2],
                                src->width + src->origin_x * 2,
                                src->height + src->origin_y * 2,
                                src->stride_y,
                                src->stride_cb,
                                src->stride_cr,
                                0,
                                0,
                                1,
                                1);
    else
        save_YUV_to_file("unscaled_pic.yuv",
                         src->buffer_y,
                         src->buffer_cb,
                         src->buffer_cr,
                         src->width + src->origin_x * 2,
                         src->height + src->origin_y * 2,
                         src->stride_y,
                         src->stride_cb,
                         src->stride_cr,
                         0,
                         0,
                         1,
                         1);
#endif

    for (int plane = 0; plane <= AOMMIN(num_planes, MAX_MB_PLANE-1); ++plane) {
        if (bd > 8) {
            switch (plane) {
            case 0:
                av1_highbd_resize_plane(
                    src_buffer_highbd[0] + src->origin_y * src->stride_y + src->origin_x,
                    src->height,
                    src->width,
                    src->stride_y,
                    dst_buffer_highbd[0] + dst->origin_y * dst->stride_y + dst->origin_x,
                    dst->height,
                    dst->width,
                    dst->stride_y,
                    bd);
                break;
            case 1:
                av1_highbd_resize_plane(
                    src_buffer_highbd[1] + (src->origin_y >> ss_y) * src->stride_cb +
                        (src->origin_x >> ss_x),
                    src->height >> ss_y,
                    src->width >> ss_x,
                    src->stride_cb,
                    dst_buffer_highbd[1] + (dst->origin_y >> ss_y) * dst->stride_cb +
                        (dst->origin_x >> ss_x),
                    dst->height >> ss_y,
                    dst->width >> ss_x,
                    dst->stride_cb,
                    bd);
                break;
            case 2:
                av1_highbd_resize_plane(
                    src_buffer_highbd[2] + (src->origin_y >> ss_y) * src->stride_cr +
                        (src->origin_x >> ss_x),
                    src->height >> ss_y,
                    src->width >> ss_x,
                    src->stride_cr,
                    dst_buffer_highbd[2] + (dst->origin_y >> ss_y) * dst->stride_cr +
                        (dst->origin_x >> ss_x),
                    dst->height >> ss_y,
                    dst->width >> ss_x,
                    dst->stride_cr,
                    bd);
                break;
            default: break;
            }
        } else {
            switch (plane) {
            case 0:
                av1_resize_plane(src->buffer_y + src->origin_y * src->stride_y + src->origin_x,
                                 src->height,
                                 src->width,
                                 src->stride_y,
                                 dst->buffer_y + dst->origin_y * dst->stride_y + dst->origin_x,
                                 dst->height,
                                 dst->width,
                                 dst->stride_y);
                break;
            case 1:
                av1_resize_plane(src->buffer_cb + (src->origin_y >> ss_y) * src->stride_cb +
                                     (src->origin_x >> ss_x),
                                 src->height >> ss_y,
                                 src->width >> ss_x,
                                 src->stride_cb,
                                 dst->buffer_cb + (dst->origin_y >> ss_y) * dst->stride_cb +
                                     (dst->origin_x >> ss_x),
                                 dst->height >> ss_y,
                                 dst->width >> ss_x,
                                 dst->stride_cb);
                break;
            case 2:
                av1_resize_plane(src->buffer_cr + (src->origin_y >> ss_y) * src->stride_cr +
                                     (src->origin_x >> ss_x),
                                 src->height >> ss_y,
                                 src->width >> ss_x,
                                 src->stride_cr,
                                 dst->buffer_cr + (dst->origin_y >> ss_y) * dst->stride_cr +
                                     (dst->origin_x >> ss_x),
                                 dst->height >> ss_y,
                                 dst->width >> ss_x,
                                 dst->stride_cr);
                break;
            default: break;
            }
        }
    }

#if DEBUG_SCALING
    if (bd > 8)
        save_YUV_to_file_highbd("scaled_pic_highbd.yuv",
                                dst_buffer_highbd[0],
                                dst_buffer_highbd[1],
                                dst_buffer_highbd[2],
                                dst->width + dst->origin_x * 2,
                                dst->height + dst->origin_y * 2,
                                dst->stride_y,
                                dst->stride_cb,
                                dst->stride_cr,
                                0,
                                0,
                                1,
                                1);
    else
        save_YUV_to_file("scaled_pic.yuv",
                         dst->buffer_y,
                         dst->buffer_cb,
                         dst->buffer_cr,
                         dst->width + dst->origin_x * 2,
                         dst->height + dst->origin_y * 2,
                         dst->stride_y,
                         dst->stride_cb,
                         dst->stride_cr,
                         0,
                         0,
                         1,
                         1);
#endif

    if (bd > 8) {
        unpack_highbd_pic(dst_buffer_highbd, dst, ss_x, ss_y, EB_TRUE);

        EB_FREE(src_buffer_highbd[0]);
        EB_FREE(src_buffer_highbd[1]);
        EB_FREE(src_buffer_highbd[2]);
        EB_FREE(dst_buffer_highbd[0]);
        EB_FREE(dst_buffer_highbd[1]);
        EB_FREE(dst_buffer_highbd[2]);
    }

    // TODO: extend frame borders
    // use eb_extend_frame() instead
    // aom_extend_frame_borders(dst, num_planes);

    return EB_ErrorNone;
}

// Generate a random number in the range [0, 32768).
static INLINE unsigned int lcg_rand16(unsigned int *state) {
    *state = (unsigned int)(*state * 1103515245ULL + 12345);
    return *state / 65536 % 32768;
}

// Given the superres configurations and the frame type, determine the denominator and
// encoding resolution
void calc_superres_params(superres_params_type *spr_params, SequenceControlSet *scs_ptr,
                          PictureParentControlSet *pcs_ptr) {
    spr_params->superres_denom = SCALE_NUMERATOR;
    static unsigned int seed = 34567;
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;

    uint8_t superres_mode = scs_ptr->static_config.superres_mode;
    uint8_t cfg_denom     = scs_ptr->static_config.superres_denom;
    uint8_t cfg_kf_denom  = scs_ptr->static_config.superres_kf_denom;
    //uint8_t superres_qthres = scs_ptr->static_config.superres_qthres;

//    // For now, super-resolution can only be enabled for key frames or intra only frames
//    // In addition, it can only be enabled in case allow_intrabc is disabled and
//    // loop restoration is enabled
//    if ((frm_hdr->frame_type != KEY_FRAME &&
//        frm_hdr->frame_type != INTRA_ONLY_FRAME) ||
//        frm_hdr->allow_intrabc ||
//        !scs_ptr->seq_header.enable_restoration) { return; }

    if (frm_hdr->allow_intrabc ||
        !scs_ptr->seq_header.enable_restoration) { return; }

    // remove assertion when rest of the modes are implemented
    assert(superres_mode <= SUPERRES_RANDOM);

    switch (superres_mode) {
    case SUPERRES_NONE: spr_params->superres_denom = SCALE_NUMERATOR; break;
    case SUPERRES_FIXED:
        if (frm_hdr->frame_type == KEY_FRAME)
            spr_params->superres_denom = cfg_kf_denom;
        else
            spr_params->superres_denom = cfg_denom;
        break;
    case SUPERRES_RANDOM: spr_params->superres_denom = (uint8_t)(lcg_rand16(&seed) % 9 + 8); break;
    //SUPERRES_QTHRESH and SUPERRES_AUTO are not yet implemented
    case SUPERRES_QTHRESH: break;
    case SUPERRES_AUTO: break;
    default: break;
    }

    // only encoding width is adjusted
    calculate_scaled_size_helper(&spr_params->encoding_width, spr_params->superres_denom);
}

EbErrorType downscaled_source_buffer_desc_ctor(EbPictureBufferDesc **picture_ptr,
                                               EbPictureBufferDesc * picture_ptr_for_reference,
                                               superres_params_type  spr_params) {
    EbPictureBufferDescInitData initData;

    initData.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    initData.max_width          = spr_params.encoding_width;
    initData.max_height         = spr_params.encoding_height;
    initData.bit_depth          = picture_ptr_for_reference->bit_depth;
    initData.color_format       = picture_ptr_for_reference->color_format;
    initData.split_mode         = EB_TRUE;
    initData.left_padding       = picture_ptr_for_reference->origin_x;
    initData.right_padding      = picture_ptr_for_reference->origin_x;
    initData.top_padding        = picture_ptr_for_reference->origin_y;
    initData.bot_padding        = picture_ptr_for_reference->origin_y;

    EB_NEW(*picture_ptr, eb_picture_buffer_desc_ctor, (EbPtr)&initData);

    return EB_ErrorNone;
}

EbErrorType sb_geom_init_pcs(SequenceControlSet *scs_ptr, PictureParentControlSet *pcs_ptr);

EbErrorType sb_params_init_pcs(SequenceControlSet *scs_ptr, PictureParentControlSet *pcs_ptr);

EbErrorType scale_pcs_params(SequenceControlSet *scs_ptr, PictureParentControlSet *pcs_ptr,
                             superres_params_type spr_params, uint16_t source_width,
                             uint16_t source_height) {
    Av1Common *cm = pcs_ptr->av1_cm;

    // frame sizes
    cm->frm_size.frame_width          = spr_params.encoding_width;
    cm->frm_size.frame_height         = spr_params.encoding_height;
    cm->frm_size.render_width         = source_width;
    cm->frm_size.render_height        = source_height;
    cm->frm_size.superres_denominator = spr_params.superres_denom;

    // align width and height to be a multiple of 8
    uint16_t aligned_width  = (uint16_t)ALIGN_POWER_OF_TWO(spr_params.encoding_width, 3);
    uint16_t aligned_height = (uint16_t)ALIGN_POWER_OF_TWO(spr_params.encoding_height, 3);

    assert((aligned_width == spr_params.encoding_width) &&
           "Downscaled width needs to be a multiple of 8 "
           "(otherwise not yet implemented)");

    // change frame width and height params in pcs
    pcs_ptr->frame_width  = spr_params.encoding_width;
    pcs_ptr->frame_height = spr_params.encoding_height;

    pcs_ptr->aligned_width  = aligned_width;
    pcs_ptr->aligned_height = aligned_height;

    // number of SBs
    const uint16_t picture_sb_width =
        (uint16_t)((aligned_width + scs_ptr->sb_sz - 1) / scs_ptr->sb_sz);
    const uint16_t picture_sb_height =
        (uint16_t)((aligned_height + scs_ptr->sb_sz - 1) / scs_ptr->sb_sz);

    pcs_ptr->picture_sb_width  = picture_sb_width; // TODO: use this instead of re-computing
    pcs_ptr->picture_sb_height = picture_sb_height;

    pcs_ptr->sb_total_count = picture_sb_width * picture_sb_height;

    // mi params
    cm->mi_stride = picture_sb_width * (BLOCK_SIZE_64 / 4);
    cm->mi_cols   = aligned_width >> MI_SIZE_LOG2;
    cm->mi_rows   = aligned_height >> MI_SIZE_LOG2;

    if (cm->frm_size.superres_denominator != SCALE_NUMERATOR) {
        derive_input_resolution(&pcs_ptr->input_resolution,
                                spr_params.encoding_width * spr_params.encoding_height);

        // create new picture level sb_params and sb_geom
        sb_params_init_pcs(scs_ptr, pcs_ptr);

        sb_geom_init_pcs(scs_ptr, pcs_ptr);
    }

    return EB_ErrorNone;
}

EbErrorType allocate_downscaled_reference_pics(EbPictureBufferDesc **downscaled_reference_picture_ptr,
                                               EbPictureBufferDesc **downscaled_reference_picture16bit,
                                               EbPictureBufferDesc *picture_ptr_for_reference,
                                               PictureParentControlSet *pcs_ptr) {

    EbPictureBufferDescInitData ref_pic_buf_desc_init_data;

    // Initialize the various Picture types
    ref_pic_buf_desc_init_data.max_width = pcs_ptr->aligned_width;
    ref_pic_buf_desc_init_data.max_height = pcs_ptr->aligned_height;
    ref_pic_buf_desc_init_data.bit_depth = picture_ptr_for_reference->bit_depth;
    ref_pic_buf_desc_init_data.color_format = picture_ptr_for_reference->color_format;
    ref_pic_buf_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;

    ref_pic_buf_desc_init_data.left_padding = PAD_VALUE;
    ref_pic_buf_desc_init_data.right_padding = PAD_VALUE;
    ref_pic_buf_desc_init_data.top_padding = PAD_VALUE;
    ref_pic_buf_desc_init_data.bot_padding = PAD_VALUE;
    ref_pic_buf_desc_init_data.mfmv = pcs_ptr->scs_ptr->mfmv_enabled;

    if (ref_pic_buf_desc_init_data.bit_depth == EB_10BIT) {
        // Hsan: set split_mode to 0 to construct the packed reference buffer (used @ EP)
        ref_pic_buf_desc_init_data.split_mode = EB_FALSE;
        EB_NEW(*downscaled_reference_picture16bit,
               eb_picture_buffer_desc_ctor,
               (EbPtr)&ref_pic_buf_desc_init_data);

        // Hsan: set split_mode to 1 to construct the unpacked reference buffer (used @ MD)
        ref_pic_buf_desc_init_data.split_mode = EB_TRUE;
        EB_NEW(*downscaled_reference_picture_ptr,
               eb_picture_buffer_desc_ctor,
               (EbPtr)&ref_pic_buf_desc_init_data);
    } else {
        // Hsan: set split_mode to 0 to as 8BIT input
        ref_pic_buf_desc_init_data.split_mode = EB_FALSE;
        EB_NEW(*downscaled_reference_picture_ptr,
               eb_picture_buffer_desc_ctor,
               (EbPtr)&ref_pic_buf_desc_init_data);
    }

    return EB_ErrorNone;
}

EbErrorType allocate_downscaled_source_reference_pics(EbPictureBufferDesc **input_padded_picture_ptr,
                                                      EbPictureBufferDesc **quarter_filtered_picture_ptr,
                                                      EbPictureBufferDesc **quarter_decimated_picture_ptr,
                                                      EbPictureBufferDesc **sixteenth_filtered_picture_ptr,
                                                      EbPictureBufferDesc **sixteenth_decimated_picture_ptr,
                                                      EbPictureBufferDesc *picture_ptr_for_reference,
                                                      superres_params_type spr_params,
                                                      uint8_t down_sampling_method_me_search){

    EbPictureBufferDescInitData initData;

    initData.buffer_enable_mask = PICTURE_BUFFER_DESC_LUMA_MASK;
    initData.max_width          = spr_params.encoding_width;
    initData.max_height         = spr_params.encoding_height;
    initData.bit_depth          = picture_ptr_for_reference->bit_depth;
    initData.color_format       = picture_ptr_for_reference->color_format;
    initData.split_mode         = EB_TRUE;
    initData.left_padding       = picture_ptr_for_reference->origin_x;
    initData.right_padding      = picture_ptr_for_reference->origin_x;
    initData.top_padding        = picture_ptr_for_reference->origin_y;
    initData.bot_padding        = picture_ptr_for_reference->origin_y;

    EB_NEW(*input_padded_picture_ptr, eb_picture_buffer_desc_ctor, (EbPtr)&initData);

    initData.buffer_enable_mask = PICTURE_BUFFER_DESC_LUMA_MASK;
    initData.max_width          = spr_params.encoding_width >> 1;
    initData.max_height         = spr_params.encoding_height >> 1;
    initData.bit_depth          = picture_ptr_for_reference->bit_depth;
    initData.color_format       = picture_ptr_for_reference->color_format;
    initData.split_mode         = EB_TRUE;
    initData.left_padding       = picture_ptr_for_reference->origin_x >> 1;
    initData.right_padding      = picture_ptr_for_reference->origin_x >> 1;
    initData.top_padding        = picture_ptr_for_reference->origin_y >> 1;
    initData.bot_padding        = picture_ptr_for_reference->origin_y >> 1;

    EB_NEW(*quarter_decimated_picture_ptr, eb_picture_buffer_desc_ctor, (EbPtr)&initData);

    if(down_sampling_method_me_search == 1){

        initData.buffer_enable_mask = PICTURE_BUFFER_DESC_LUMA_MASK;
        initData.max_width          = spr_params.encoding_width >> 1;
        initData.max_height         = spr_params.encoding_height >> 1;
        initData.bit_depth          = picture_ptr_for_reference->bit_depth;
        initData.color_format       = picture_ptr_for_reference->color_format;
        initData.split_mode         = EB_TRUE;
        initData.left_padding       = picture_ptr_for_reference->origin_x >> 1;
        initData.right_padding      = picture_ptr_for_reference->origin_x >> 1;
        initData.top_padding        = picture_ptr_for_reference->origin_y >> 1;
        initData.bot_padding        = picture_ptr_for_reference->origin_y >> 1;

        EB_NEW(*quarter_filtered_picture_ptr, eb_picture_buffer_desc_ctor, (EbPtr)&initData);

        initData.buffer_enable_mask = PICTURE_BUFFER_DESC_LUMA_MASK;
        initData.max_width          = spr_params.encoding_width >> 2;
        initData.max_height         = spr_params.encoding_height >> 2;
        initData.bit_depth          = picture_ptr_for_reference->bit_depth;
        initData.color_format       = picture_ptr_for_reference->color_format;
        initData.split_mode         = EB_TRUE;
        initData.left_padding       = picture_ptr_for_reference->origin_x >> 2;
        initData.right_padding      = picture_ptr_for_reference->origin_x >> 2;
        initData.top_padding        = picture_ptr_for_reference->origin_y >> 2;
        initData.bot_padding        = picture_ptr_for_reference->origin_y >> 2;

        EB_NEW(*sixteenth_filtered_picture_ptr, eb_picture_buffer_desc_ctor, (EbPtr)&initData);

    }

    initData.buffer_enable_mask = PICTURE_BUFFER_DESC_LUMA_MASK;
    initData.max_width          = spr_params.encoding_width >> 2;
    initData.max_height         = spr_params.encoding_height >> 2;
    initData.bit_depth          = picture_ptr_for_reference->bit_depth;
    initData.color_format       = picture_ptr_for_reference->color_format;
    initData.split_mode         = EB_TRUE;
    initData.left_padding       = picture_ptr_for_reference->origin_x >> 2;
    initData.right_padding      = picture_ptr_for_reference->origin_x >> 2;
    initData.top_padding        = picture_ptr_for_reference->origin_y >> 2;
    initData.bot_padding        = picture_ptr_for_reference->origin_y >> 2;

    EB_NEW(*sixteenth_decimated_picture_ptr, eb_picture_buffer_desc_ctor, (EbPtr)&initData);

    return EB_ErrorNone;
}

void scale_source_references(SequenceControlSet *scs_ptr,
                             PictureParentControlSet *pcs_ptr,
                             EbPictureBufferDesc *input_picture_ptr){

    EbPaReferenceObject *reference_object;

    uint8_t denom_idx = (uint8_t)(pcs_ptr->superres_denom - 8);
    const int32_t  num_planes = 0; // Y only
    const uint32_t ss_x       = scs_ptr->subsampling_x;
    const uint32_t ss_y       = scs_ptr->subsampling_y;

    uint32_t num_of_list_to_search =
            (pcs_ptr->slice_type == P_SLICE) ? (uint32_t)REF_LIST_0 : (uint32_t)REF_LIST_1;

    for (uint8_t list_index = REF_LIST_0; list_index <= num_of_list_to_search; ++list_index) {
        uint8_t ref_pic_index;

        uint8_t num_of_ref_pic_to_search = (pcs_ptr->slice_type == P_SLICE)
                                           ? pcs_ptr->ref_list0_count
                                           : (list_index == REF_LIST_0) ? pcs_ptr->ref_list0_count
                                                                        : pcs_ptr->ref_list1_count;

        for (ref_pic_index = 0; ref_pic_index < num_of_ref_pic_to_search; ++ref_pic_index) {

            reference_object = (EbPaReferenceObject *) pcs_ptr->ref_pa_pic_ptr_array[list_index][ref_pic_index]
                    ->object_ptr;

            uint64_t ref_picture_number = pcs_ptr->ref_pic_poc_array[list_index][ref_pic_index];
            UNUSED(ref_picture_number);

            EbPictureBufferDesc *ref_pic_ptr = reference_object->input_padded_picture_ptr;

#if DEBUG_REFERENCE_SCALE
            save_Y_to_file("input_reference_pic.yuv",
                           ref_pic_ptr->buffer_y,
                           ref_pic_ptr->width + ref_pic_ptr->origin_x * 2,
                           ref_pic_ptr->height + ref_pic_ptr->origin_y * 2,
                           ref_pic_ptr->stride_y,
                           0,
                           0);
#endif
            
            // if the size of the reference pic is different than the size of the input pic, then scale references
            if (ref_pic_ptr->width != input_picture_ptr->width){

                if (reference_object->downscaled_input_padded_picture_ptr[denom_idx] == NULL){

                    superres_params_type spr_params = {pcs_ptr->aligned_width, // encoding_width
                                                       pcs_ptr->aligned_height, // encoding_height
                                                       scs_ptr->static_config.superres_mode};

                    // Allocate downsampled reference picture buffer descriptors
                    allocate_downscaled_source_reference_pics(&reference_object->downscaled_input_padded_picture_ptr[denom_idx],
                                                       &reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx],
                                                       &reference_object->downscaled_quarter_filtered_picture_ptr[denom_idx],
                                                       &reference_object->downscaled_sixteenth_decimated_picture_ptr[denom_idx],
                                                       &reference_object->downscaled_sixteenth_filtered_picture_ptr[denom_idx],
                                                       ref_pic_ptr,
                                                       spr_params,
                                                       scs_ptr->down_sampling_method_me_search);

                    EbPictureBufferDesc *down_ref_pic_ptr = reference_object->downscaled_input_padded_picture_ptr[denom_idx];

                    // downsample input padded picture buffer
                    av1_resize_and_extend_frame(ref_pic_ptr,
                                                down_ref_pic_ptr,
                                                down_ref_pic_ptr->bit_depth,
                                                num_planes,
                                                ss_x,
                                                ss_y);

                    generate_padding(down_ref_pic_ptr->buffer_y,
                                     down_ref_pic_ptr->stride_y,
                                     down_ref_pic_ptr->width,
                                     down_ref_pic_ptr->height,
                                     down_ref_pic_ptr->origin_x,
                                     down_ref_pic_ptr->origin_y);

#if DEBUG_REFERENCE_SCALE
                    save_Y_to_file("downscaled_reference_pic.yuv",
                                   reference_object->downscaled_input_padded_picture_ptr[denom_idx]->buffer_y,
                                   reference_object->downscaled_input_padded_picture_ptr[denom_idx]->width +
                                   reference_object->downscaled_input_padded_picture_ptr[denom_idx]->origin_x * 2,
                                   reference_object->downscaled_input_padded_picture_ptr[denom_idx]->height +
                                   reference_object->downscaled_input_padded_picture_ptr[denom_idx]->origin_y * 2,
                                   reference_object->downscaled_input_padded_picture_ptr[denom_idx]->stride_y,
                                   0,
                                   0);
#endif
                    downsample_decimation_input_picture(
                            pcs_ptr,
                            down_ref_pic_ptr,
                            reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx],
                            reference_object->downscaled_sixteenth_decimated_picture_ptr[denom_idx]);

                    // 1/4 & 1/16 input picture downsampling through filtering
                    if (scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED) {
                        downsample_filtering_input_picture(
                                pcs_ptr,
                                down_ref_pic_ptr,
                                reference_object->downscaled_quarter_filtered_picture_ptr[denom_idx],
                                reference_object->downscaled_sixteenth_filtered_picture_ptr[denom_idx]);
                    }

#if DEBUG_REFERENCE_SCALE
                    save_Y_to_file("downscaled_quarter_reference_pic.yuv",
                                   reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx]->buffer_y,
                                   reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx]->width +
                                   reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx]->origin_x * 2,
                                   reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx]->height +
                                   reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx]->origin_y * 2,
                                   reference_object->downscaled_quarter_decimated_picture_ptr[denom_idx]->stride_y,
                                   0,
                                   0);
#endif

                }
            }
        }
    }

}

void scale_input_references(PictureParentControlSet *pcs_ptr,
                            superres_params_type superres_params) {

    uint8_t denom_idx = (uint8_t)(superres_params.superres_denom - 8);

    // reference structures (padded pictures + downsampled versions)
    EbPaReferenceObject *src_object = (EbPaReferenceObject *)pcs_ptr->pa_reference_picture_wrapper_ptr->object_ptr;
    EbPictureBufferDesc *padded_pic_ptr = src_object->input_padded_picture_ptr;

    // Allocate downsampled reference picture buffer descriptors
    allocate_downscaled_source_reference_pics(&src_object->downscaled_input_padded_picture_ptr[denom_idx],
                                       &src_object->downscaled_quarter_decimated_picture_ptr[denom_idx],
                                       &src_object->downscaled_quarter_filtered_picture_ptr[denom_idx],
                                       &src_object->downscaled_sixteenth_decimated_picture_ptr[denom_idx],
                                       &src_object->downscaled_sixteenth_filtered_picture_ptr[denom_idx],
                                       padded_pic_ptr,
                                       superres_params,
                                       pcs_ptr->scs_ptr->down_sampling_method_me_search);

    padded_pic_ptr = src_object->downscaled_input_padded_picture_ptr[denom_idx];
    EbPictureBufferDesc *input_picture_ptr = pcs_ptr->enhanced_picture_ptr;

    generate_padding(input_picture_ptr->buffer_y,
                     input_picture_ptr->stride_y,
                     input_picture_ptr->width,
                     input_picture_ptr->height,
                     input_picture_ptr->origin_x,
                     input_picture_ptr->origin_y);

    for (uint32_t row = 0; row < input_picture_ptr->height + 2*input_picture_ptr->origin_y; row++)
        EB_MEMCPY(padded_pic_ptr->buffer_y + row * padded_pic_ptr->stride_y,
                  input_picture_ptr->buffer_y + row * input_picture_ptr->stride_y,
                  sizeof(uint8_t) * input_picture_ptr->stride_y);

    // 1/4 & 1/16 input picture decimation
    downsample_decimation_input_picture(pcs_ptr,
                                        padded_pic_ptr,
                                        src_object->downscaled_quarter_decimated_picture_ptr[denom_idx],
                                        src_object->downscaled_sixteenth_decimated_picture_ptr[denom_idx]);

    // 1/4 & 1/16 input filtered picture
    if (pcs_ptr->scs_ptr->down_sampling_method_me_search == ME_FILTERED_DOWNSAMPLED)
        downsample_filtering_input_picture(pcs_ptr,
                                           padded_pic_ptr,
                                           src_object->downscaled_quarter_filtered_picture_ptr[denom_idx],
                                           src_object->downscaled_sixteenth_filtered_picture_ptr[denom_idx]);
}

// TODO: work in progress
void scale_rec_references(PictureControlSet *pcs_ptr,
                          EbPictureBufferDesc *input_picture_ptr,
                          uint8_t hbd_mode_decision){

    EbReferenceObject *reference_object;

    PictureParentControlSet *ppcs_ptr = pcs_ptr->parent_pcs_ptr;
    SequenceControlSet *scs_ptr = ppcs_ptr->scs_ptr;

    uint8_t denom_idx = (uint8_t)(ppcs_ptr->superres_denom - 8);
    const int32_t num_planes = av1_num_planes(&scs_ptr->seq_header.color_config);
    const uint32_t ss_x = scs_ptr->subsampling_x;
    const uint32_t ss_y = scs_ptr->subsampling_y;

    uint32_t num_of_list_to_search =
            (ppcs_ptr->slice_type == P_SLICE) ? (uint32_t)REF_LIST_0 : (uint32_t)REF_LIST_1;

    for (uint8_t list_index = REF_LIST_0; list_index <= num_of_list_to_search; ++list_index) {
        uint8_t ref_pic_index;

        uint8_t num_of_ref_pic_to_search = (ppcs_ptr->slice_type == P_SLICE)
                                           ? ppcs_ptr->ref_list0_count
                                           : (list_index == REF_LIST_0) ? ppcs_ptr->ref_list0_count
                                                                        : ppcs_ptr->ref_list1_count;

        for (ref_pic_index = 0; ref_pic_index < num_of_ref_pic_to_search; ++ref_pic_index) {

            reference_object = (EbReferenceObject *) pcs_ptr->ref_pic_ptr_array[list_index][ref_pic_index]
                    ->object_ptr;

            uint64_t ref_picture_number = ppcs_ptr->ref_pic_poc_array[list_index][ref_pic_index];
            UNUSED(ref_picture_number);

            EbPictureBufferDesc *ref_pic_ptr = hbd_mode_decision ? reference_object->reference_picture16bit : reference_object->reference_picture;

            // if the size of the reference pic is different than the size of the input pic, then scale references
            if (ref_pic_ptr->width != input_picture_ptr->width) {

                EbPictureBufferDesc *down_ref_pic_ptr = hbd_mode_decision
                                                        ? reference_object->downscaled_reference_picture16bit[denom_idx]
                                                        : reference_object->downscaled_reference_picture[denom_idx];

                if (down_ref_pic_ptr == NULL) {
                    // Allocate downsampled reference picture buffer descriptors
                    allocate_downscaled_reference_pics(&reference_object->downscaled_reference_picture[denom_idx],
                                                       &reference_object->downscaled_reference_picture16bit[denom_idx],
                                                       ref_pic_ptr,
                                                       ppcs_ptr);

                    down_ref_pic_ptr = hbd_mode_decision
                                       ? reference_object->downscaled_reference_picture16bit[denom_idx]
                                       : reference_object->downscaled_reference_picture[denom_idx];

                    // downsample input padded picture buffer
                    // TODO: where is the extend?
                    av1_resize_and_extend_frame(ref_pic_ptr,
                                                down_ref_pic_ptr,
                                                down_ref_pic_ptr->bit_depth,
                                                num_planes,
                                                ss_x,
                                                ss_y);

                    // TODO: is the correct padding?
                    generate_padding(down_ref_pic_ptr->buffer_y,
                                     down_ref_pic_ptr->stride_y,
                                     down_ref_pic_ptr->width,
                                     down_ref_pic_ptr->height,
                                     down_ref_pic_ptr->origin_x,
                                     down_ref_pic_ptr->origin_y);

                    save_Y_to_file("downscaled_reference_pic.yuv",
                                   down_ref_pic_ptr->buffer_y,
                                   down_ref_pic_ptr->width +
                                           down_ref_pic_ptr->origin_x * 2,
                                   down_ref_pic_ptr->height +
                                           down_ref_pic_ptr->origin_y * 2,
                                   down_ref_pic_ptr->stride_y,
                                   0,
                                   0);

                    printf("rescaled reference picture\n");

                }
            }
        }
    }

}

void init_resize_picture(SequenceControlSet *scs_ptr, PictureParentControlSet *pcs_ptr) {
    EbPictureBufferDesc *input_picture_ptr = pcs_ptr->enhanced_picture_ptr;

    superres_params_type spr_params = {input_picture_ptr->width, // encoding_width
                                       input_picture_ptr->height, // encoding_height
                                       scs_ptr->static_config.superres_denom};

    // determine super-resolution parameters - encoding resolution
    // given configs and frame type
    calc_superres_params(&spr_params, scs_ptr, pcs_ptr);

    if (spr_params.superres_denom != SCALE_NUMERATOR) {

        scs_ptr->seq_header.enable_superres = 1; // enable sequence level super-res flag
                                                 // if super-res is ON for any frame

        pcs_ptr->superres_denom = spr_params.superres_denom;

        // Allocate downsampled picture buffer descriptor
        downscaled_source_buffer_desc_ctor(
            &pcs_ptr->enhanced_downscaled_picture_ptr, input_picture_ptr, spr_params);

        const int32_t  num_planes = av1_num_planes(&scs_ptr->seq_header.color_config);
        const uint32_t ss_x       = scs_ptr->subsampling_x;
        const uint32_t ss_y       = scs_ptr->subsampling_y;

        // downsample picture buffer
        av1_resize_and_extend_frame(input_picture_ptr,
                                    pcs_ptr->enhanced_downscaled_picture_ptr,
                                    pcs_ptr->enhanced_downscaled_picture_ptr->bit_depth,
                                    num_planes,
                                    ss_x,
                                    ss_y);

        // use downscaled picture instead of original res for mode decision, encoding loop etc
        // after temporal filtering and motion estimation
        pcs_ptr->enhanced_picture_ptr = pcs_ptr->enhanced_downscaled_picture_ptr;

        pcs_ptr->frame_superres_enabled = EB_TRUE;

        scale_pcs_params(
            scs_ptr, pcs_ptr, spr_params, input_picture_ptr->width, input_picture_ptr->height);

        scale_input_references(pcs_ptr, spr_params);

    }
}