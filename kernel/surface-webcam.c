#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>

#define RAW_WIDTH 1928
#define RAW_HEIGHT 1092
#define RAW_STRIDE 1952  // 1952 uint16s per line = 3904 bytes

#define OUT_WIDTH 1920
#define OUT_HEIGHT 1080
#define CROP_X 4
#define CROP_Y 6

#define NUM_BUFFERS 4
#define DEFAULT_GAMMA 2.2
#define BLACK_LEVEL 64
#define MEDIA_BUS_FMT_SGRBG10_1X10 0x300a

#define CID_READERS ((V4L2_CID_USER_BASE | 0xf000) + 4)

static int is_capture_client_active(int out_fd) {
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = CID_READERS;
    if (ioctl(out_fd, VIDIOC_G_CTRL, &c) == 0) return c.value;
    return 1;
}

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

struct buffer {
    void *start;
    size_t length;
};

#define MAX_RAW_VAL (1023 - BLACK_LEVEL)

// 1024-entry lookup tables for 10-bit raw Bayer values
static uint8_t lut_r[1024], lut_g[1024], lut_b[1024];
static double g_wb_r = 2.15, g_wb_g = 1.0, g_wb_b = 1.90;
static double g_exposure_gain = 1.6;

static void rebuild_lut(double wb_r, double wb_g, double wb_b, double gain, double gamma) {
    for (int i = 0; i < 1024; i++) {
        int val = (i > BLACK_LEVEL) ? (i - BLACK_LEVEL) : 0;
        double norm = (double)val / (double)MAX_RAW_VAL;

        double r = norm * gain * wb_r;
        double g = norm * gain * wb_g;
        double b = norm * gain * wb_b;

        if (r > 1.0) r = 1.0; else if (r < 0.0) r = 0.0;
        if (g > 1.0) g = 1.0; else if (g < 0.0) g = 0.0;
        if (b > 1.0) b = 1.0; else if (b < 0.0) b = 0.0;

        lut_r[i] = (uint8_t)(pow(r, 1.0 / gamma) * 255.0 + 0.5);
        lut_g[i] = (uint8_t)(pow(g, 1.0 / gamma) * 255.0 + 0.5);
        lut_b[i] = (uint8_t)(pow(b, 1.0 / gamma) * 255.0 + 0.5);
    }
}

static void update_aec_awb_subsample(const uint16_t *raw) {
    uint64_t sum_g = 0;
    uint32_t aec_count = 0;

    uint64_t wb_sum_r = 0, wb_sum_g = 0, wb_sum_b = 0;
    uint32_t wb_count = 0;

    // Center-weighted metering (focus on face)
    int y_start = RAW_HEIGHT * 0.15;
    int y_end   = RAW_HEIGHT * 0.85;
    int x_start = RAW_WIDTH * 0.15;
    int x_end   = RAW_WIDTH * 0.85;

    for (int y = y_start; y < y_end; y += 16) {
        const uint16_t *row0 = raw + y * RAW_STRIDE;
        const uint16_t *row1 = raw + (y + 1) * RAW_STRIDE;
        for (int x = x_start; x < x_end; x += 16) {
            int g0 = (row0[x] > BLACK_LEVEL) ? (row0[x] - BLACK_LEVEL) : 0;
            int r0 = (row0[x + 1] > BLACK_LEVEL) ? (row0[x + 1] - BLACK_LEVEL) : 0;
            int b0 = (row1[x] > BLACK_LEVEL) ? (row1[x] - BLACK_LEVEL) : 0;
            int g1 = (row1[x + 1] > BLACK_LEVEL) ? (row1[x + 1] - BLACK_LEVEL) : 0;

            int g_val = (g0 + g1) >> 1;
            sum_g += g_val;
            aec_count++;

            // For AWB: select near-neutral pixels (exclude strongly saturated skin/clothing)
            // On raw Bayer sensor, neutral white/gray has r roughly half of g and b roughly half of g
            int diff_r = abs(r0 * 2 - g_val);
            int diff_b = abs(b0 * 2 - g_val);
            if (g_val > 30 && diff_r < (g_val * 6) / 10 && diff_b < (g_val * 6) / 10) {
                wb_sum_r += r0;
                wb_sum_g += g_val;
                wb_sum_b += b0;
                wb_count++;
            }
        }
    }

    if (aec_count > 0 && sum_g > 0) {
        double avg_g = (double)sum_g / aec_count;

        // Target face linear brightness ~0.24 (which becomes ~133 in sRGB after gamma 2.2)
        double target_gain = (0.24 * (double)MAX_RAW_VAL) / (avg_g > 15.0 ? avg_g : 15.0);
        if (target_gain < 0.6) target_gain = 0.6;
        if (target_gain > 4.5) target_gain = 4.5;
        g_exposure_gain = 0.85 * g_exposure_gain + 0.15 * target_gain;

        if (wb_count > 30 && wb_sum_r > 0 && wb_sum_b > 0) {
            double avg_wb_g = (double)wb_sum_g / wb_count;
            double avg_wb_r = (double)wb_sum_r / wb_count;
            double avg_wb_b = (double)wb_sum_b / wb_count;

            double target_r = avg_wb_g / avg_wb_r;
            double target_b = avg_wb_g / avg_wb_b;

            if (target_r < 1.90) target_r = 1.90;
            if (target_r > 2.45) target_r = 2.45;
            if (target_b < 1.70) target_b = 1.70;
            if (target_b > 2.30) target_b = 2.30;

            g_wb_r = 0.94 * g_wb_r + 0.06 * target_r;
            g_wb_b = 0.94 * g_wb_b + 0.06 * target_b;
            g_wb_g = 1.0;
        }

        rebuild_lut(g_wb_r, g_wb_g, g_wb_b, g_exposure_gain, DEFAULT_GAMMA);
    }
}

// Convert raw 10-bit Bayer SGRBG10 -> Rotated 180° YUYV (4:2:2 packed) at native 1080p
// With chroma noise suppression in dark areas
static void process_frame_yuyv_1080p(const uint16_t *raw, uint8_t *yuyv_out) {
    const int out_stride = OUT_WIDTH * 2;

    for (int y = 0; y < OUT_HEIGHT; y += 2) {
        int raw_y0 = y + CROP_Y;
        int raw_y1 = raw_y0 + 1;
        const uint16_t *row0 = raw + raw_y0 * RAW_STRIDE;
        const uint16_t *row1 = raw + raw_y1 * RAW_STRIDE;

        // 180° vertical rotation
        int out_y0 = OUT_HEIGHT - 1 - y;
        int out_y1 = out_y0 - 1;

        uint8_t *out_row0 = yuyv_out + out_y0 * out_stride;
        uint8_t *out_row1 = yuyv_out + out_y1 * out_stride;

        for (int x = 0; x < OUT_WIDTH; x += 2) {
            int raw_x = x + CROP_X;

            uint16_t raw_g0 = row0[raw_x];
            uint16_t raw_r0 = row0[raw_x + 1];
            uint16_t raw_b0 = row1[raw_x];
            uint16_t raw_g1 = row1[raw_x + 1];

            uint8_t g0 = lut_g[raw_g0 & 1023];
            uint8_t r0 = lut_r[raw_r0 & 1023];
            uint8_t b0 = lut_b[raw_b0 & 1023];
            uint8_t g1 = lut_g[raw_g1 & 1023];

            int g_avg = (g0 + g1) >> 1;

            int y_pix0 = (77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8;
            int y_pix1 = (77 * r0 + 150 * g1 + 29 * b0 + 128) >> 8;
            if (y_pix0 < 0) y_pix0 = 0; else if (y_pix0 > 255) y_pix0 = 255;
            if (y_pix1 < 0) y_pix1 = 0; else if (y_pix1 > 255) y_pix1 = 255;

            int u_raw = (-43 * r0 - 85 * g_avg + 128 * b0) >> 8;
            int v_raw = (128 * r0 - 107 * g_avg - 21 * b0) >> 8;

            // Chroma noise suppression: scale saturation down in shadows, moderate boost in highlights
            int avg_y = (y_pix0 + y_pix1) >> 1;
            int sat_mult = 280; // 1.10x saturation
            if (avg_y < 40) {
                sat_mult = (avg_y * 280) / 40; // Suppress chroma noise in shadows
            }

            int u = 128 + ((u_raw * sat_mult) >> 8);
            int v = 128 + ((v_raw * sat_mult) >> 8);
            if (u < 0) u = 0; else if (u > 255) u = 255;
            if (v < 0) v = 0; else if (v > 255) v = 255;

            int out_x = OUT_WIDTH - 2 - x;

            out_row0[out_x * 2 + 0] = (uint8_t)y_pix1;
            out_row0[out_x * 2 + 1] = (uint8_t)u;
            out_row0[out_x * 2 + 2] = (uint8_t)y_pix0;
            out_row0[out_x * 2 + 3] = (uint8_t)v;

            out_row1[out_x * 2 + 0] = (uint8_t)y_pix0;
            out_row1[out_x * 2 + 1] = (uint8_t)u;
            out_row1[out_x * 2 + 2] = (uint8_t)y_pix1;
            out_row1[out_x * 2 + 3] = (uint8_t)v;
        }
    }
}

static int g_cur_target_w = -1;
static int g_cur_target_h = -1;
static int *g_map_x = NULL;
static int *g_map_y = NULL;

static void update_scaling_maps(int target_w, int target_h) {
    if (target_w == g_cur_target_w && target_h == g_cur_target_h) return;

    free(g_map_x);
    free(g_map_y);
    g_map_x = malloc((target_w + 2) * sizeof(int));
    g_map_y = malloc((target_h + 2) * sizeof(int));
    if (!g_map_x || !g_map_y) {
        fprintf(stderr, "Failed to allocate scaling maps for %dx%d\n", target_w, target_h);
        return;
    }
    g_cur_target_w = target_w;
    g_cur_target_h = target_h;

    int crop_w, crop_h, crop_x_off, crop_y_off;
    double aspect = (double)target_w / (double)target_h;
    if (fabs(aspect - (4.0 / 3.0)) < 0.05) {
        // 4:3 target: center-crop 1440x1080 out of 1920x1080 for square pixels & 1:1 round circles
        crop_w = 1440;
        crop_h = 1080;
        crop_x_off = 240;
        crop_y_off = 0;
    } else {
        // 16:9 or full frame
        crop_w = 1920;
        crop_h = 1080;
        crop_x_off = 0;
        crop_y_off = 0;
    }

    for (int y = 0; y < target_h; y++) {
        int unrotated_y = target_h - 1 - y;
        int cy = (unrotated_y * crop_h) / target_h;
        int raw_y = (cy & ~1) + CROP_Y + crop_y_off;
        if (raw_y + 1 >= RAW_HEIGHT) raw_y = RAW_HEIGHT - 2;
        if (raw_y < 0) raw_y = 0;
        g_map_y[y] = raw_y;
    }

    for (int x = 0; x < target_w; x += 2) {
        int unrotated_x = target_w - 1 - x;
        int cx = (unrotated_x * crop_w) / target_w;
        int raw_x = (cx & ~1) + CROP_X + crop_x_off;
        if (raw_x + 1 >= RAW_WIDTH) raw_x = RAW_WIDTH - 2;
        if (raw_x < 0) raw_x = 0;
        g_map_x[x] = raw_x;
    }

    printf("Scaling map updated: target %dx%d, crop %dx%d (+%d,+%d)\n",
           target_w, target_h, crop_w, crop_h, crop_x_off, crop_y_off);
}

// Convert raw 10-bit Bayer SGRBG10 -> Scaled & Rotated 180° YUYV (4:2:2 packed)
static void process_frame_yuyv_scaled(const uint16_t *raw, uint8_t *yuyv_out, int target_w, int target_h) {
    const int out_stride = target_w * 2;

    for (int y = 0; y < target_h; y++) {
        int raw_y = g_map_y[y];
        const uint16_t *row0 = raw + raw_y * RAW_STRIDE;
        const uint16_t *row1 = raw + (raw_y + 1) * RAW_STRIDE;
        uint8_t *out_row = yuyv_out + y * out_stride;

        for (int x = 0; x < target_w; x += 2) {
            int raw_x = g_map_x[x];

            uint16_t raw_g0 = row0[raw_x];
            uint16_t raw_r0 = row0[raw_x + 1];
            uint16_t raw_b0 = row1[raw_x];
            uint16_t raw_g1 = row1[raw_x + 1];

            uint8_t g0 = lut_g[raw_g0 & 1023];
            uint8_t r0 = lut_r[raw_r0 & 1023];
            uint8_t b0 = lut_b[raw_b0 & 1023];
            uint8_t g1 = lut_g[raw_g1 & 1023];

            int g_avg = (g0 + g1) >> 1;

            int y_pix0 = (77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8;
            int y_pix1 = (77 * r0 + 150 * g1 + 29 * b0 + 128) >> 8;
            if (y_pix0 < 0) y_pix0 = 0; else if (y_pix0 > 255) y_pix0 = 255;
            if (y_pix1 < 0) y_pix1 = 0; else if (y_pix1 > 255) y_pix1 = 255;

            int u_raw = (-43 * r0 - 85 * g_avg + 128 * b0) >> 8;
            int v_raw = (128 * r0 - 107 * g_avg - 21 * b0) >> 8;

            int avg_y = (y_pix0 + y_pix1) >> 1;
            int sat_mult = 280;
            if (avg_y < 40) {
                sat_mult = (avg_y * 280) / 40;
            }

            int u = 128 + ((u_raw * sat_mult) >> 8);
            int v = 128 + ((v_raw * sat_mult) >> 8);
            if (u < 0) u = 0; else if (u > 255) u = 255;
            if (v < 0) v = 0; else if (v > 255) v = 255;

            out_row[x * 2 + 0] = (uint8_t)y_pix1;
            out_row[x * 2 + 1] = (uint8_t)u;
            out_row[x * 2 + 2] = (uint8_t)y_pix0;
            out_row[x * 2 + 3] = (uint8_t)v;
        }
    }
}

static int configure_hardware(const char *media_dev) {
    int mfd = open(media_dev, O_RDWR);
    if (mfd >= 0) {
        struct media_entity_desc ent;
        uint32_t csi2_id = 0, isys_id = 0;

        memset(&ent, 0, sizeof(ent));
        ent.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (ioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &ent) == 0) {
            if (strstr(ent.name, "CSI2 0")) csi2_id = ent.id;
            if (strstr(ent.name, "ISYS Capture 0")) isys_id = ent.id;
            ent.id |= MEDIA_ENT_ID_FLAG_NEXT;
        }

        if (csi2_id && isys_id) {
            struct media_link_desc link;
            memset(&link, 0, sizeof(link));
            link.source.entity = csi2_id;
            link.source.index = 1;
            link.source.flags = MEDIA_PAD_FL_SOURCE;
            link.sink.entity = isys_id;
            link.sink.index = 0;
            link.sink.flags = MEDIA_PAD_FL_SINK;
            link.flags = MEDIA_LNK_FL_ENABLED;
            ioctl(mfd, MEDIA_IOC_SETUP_LINK, &link);
            printf("Media link configured: CSI2 pad 1 -> ISYS pad 0\n");
        }
        close(mfd);
    }

    for (int i = 0; i < 32; i++) {
        char sdev[64];
        snprintf(sdev, sizeof(sdev), "/dev/v4l-subdev%d", i);
        int sfd = open(sdev, O_RDWR);
        if (sfd < 0) continue;

        struct v4l2_subdev_format sfmt;
        memset(&sfmt, 0, sizeof(sfmt));
        sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        sfmt.pad = 0;
        if (ioctl(sfd, VIDIOC_SUBDEV_G_FMT, &sfmt) == 0) {
            if (sfmt.format.code == MEDIA_BUS_FMT_SGRBG10_1X10) {
                for (int pad = 0; pad <= 1; pad++) {
                    memset(&sfmt, 0, sizeof(sfmt));
                    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
                    sfmt.pad = pad;
                    sfmt.format.width = RAW_WIDTH;
                    sfmt.format.height = RAW_HEIGHT;
                    sfmt.format.code = MEDIA_BUS_FMT_SGRBG10_1X10;
                    sfmt.format.field = V4L2_FIELD_NONE;
                    ioctl(sfd, VIDIOC_SUBDEV_S_FMT, &sfmt);
                }
                printf("Configured subdev %s pads 0/1 to %dx%d SGRBG10\n", sdev, RAW_WIDTH, RAW_HEIGHT);
            }
        }

        struct v4l2_control ctrl;
        ctrl.id = 0x00980911; // Exposure
        ctrl.value = 1200;
        ioctl(sfd, VIDIOC_S_CTRL, &ctrl);

        ctrl.id = 0x009e0903; // Analogue Gain (clean indoor setting)
        ctrl.value = 48;
        ioctl(sfd, VIDIOC_S_CTRL, &ctrl);

        ctrl.id = 0x009f0905; // Digital Gain (clean 1.0x)
        ctrl.value = 1024;
        ioctl(sfd, VIDIOC_S_CTRL, &ctrl);

        ctrl.id = 0x009f0903; // Test Pattern: Off (0)
        ctrl.value = 0;
        ioctl(sfd, VIDIOC_S_CTRL, &ctrl);

        close(sfd);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    const char *media_dev = (argc > 1) ? argv[1] : "/dev/media0";
    const char *cap_dev   = (argc > 2) ? argv[2] : "/dev/video0";
    const char *out_dev   = (argc > 3) ? argv[3] : "/dev/video42";

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    printf("Starting Clean Surface Webcam Bridge (%s -> %s -> %s)...\n",
           media_dev, cap_dev, out_dev);

    rebuild_lut(g_wb_r, g_wb_g, g_wb_b, g_exposure_gain, DEFAULT_GAMMA);
    configure_hardware(media_dev);

    int out_fd = open(out_dev, O_RDWR);
    if (out_fd < 0) {
        fprintf(stderr, "Cannot open output %s: %s\n", out_dev, strerror(errno));
        return 1;
    }

    struct v4l2_format ofmt;
    memset(&ofmt, 0, sizeof(ofmt));
    ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ofmt.fmt.pix.width = OUT_WIDTH;
    ofmt.fmt.pix.height = OUT_HEIGHT;
    ofmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    ofmt.fmt.pix.field = V4L2_FIELD_NONE;
    ofmt.fmt.pix.bytesperline = OUT_WIDTH * 2;
    ofmt.fmt.pix.sizeimage = OUT_WIDTH * OUT_HEIGHT * 2;
    ofmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    if (ioctl(out_fd, VIDIOC_S_FMT, &ofmt) < 0) {
        fprintf(stderr, "Warning: Cannot set output format on %s: %s\n", out_dev, strerror(errno));
    } else {
        printf("Output %s configured for %dx%d YUYV\n", out_dev, OUT_WIDTH, OUT_HEIGHT);
    }

    int cap_fd = open(cap_dev, O_RDWR);
    if (cap_fd < 0) {
        fprintf(stderr, "Cannot open capture %s: %s\n", cap_dev, strerror(errno));
        close(out_fd);
        return 1;
    }

    struct v4l2_format cfmt;
    memset(&cfmt, 0, sizeof(cfmt));
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cfmt.fmt.pix.width = RAW_WIDTH;
    cfmt.fmt.pix.height = RAW_HEIGHT;
    cfmt.fmt.pix.pixelformat = v4l2_fourcc('B', 'A', '1', '0'); // SGRBG10
    cfmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(cap_fd, VIDIOC_S_FMT, &cfmt) < 0) {
        fprintf(stderr, "Cannot set format on %s: %s\n", cap_dev, strerror(errno));
        close(cap_fd);
        close(out_fd);
        return 1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cap_fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        close(cap_fd);
        close(out_fd);
        return 1;
    }

    struct buffer buffers[NUM_BUFFERS];
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(cap_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
            return 1;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, cap_fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "mmap buffer %d failed\n", i);
            return 1;
        }
        if (ioctl(cap_fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QBUF buffer %d failed: %s\n", i, strerror(errno));
            return 1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int hardware_streaming = 0;
    printf("Streaming started successfully! Delivering frames to %s...\n", out_dev);

    uint8_t *yuyv_frame = malloc(OUT_WIDTH * OUT_HEIGHT * 2);
    if (!yuyv_frame) {
        fprintf(stderr, "Cannot allocate frame buffer\n");
        return 1;
    }

    /* Prime the loopback with one black frame so the writer owns the output
     * stream token; otherwise readers get -EIO on STREAMON while idle. */
    for (size_t i = 0; i < (size_t)OUT_WIDTH * OUT_HEIGHT * 2; i += 2) {
        yuyv_frame[i] = 16;
        yuyv_frame[i + 1] = 128;
    }
    write(out_fd, yuyv_frame, OUT_WIDTH * OUT_HEIGHT * 2);

    uint32_t frame_count = 0;

    while (g_running) {
        int active = is_capture_client_active(out_fd);
        if (!active) {
            if (hardware_streaming) {
                ioctl(cap_fd, VIDIOC_STREAMOFF, &type);
                hardware_streaming = 0;
                printf("Reader gone: sensor stopped (LED off)\n");
            }
            usleep(10000);
            continue;
        }
        if (!hardware_streaming) {
            /* Sensor registers (gain/exposure) are lost on power-down: re-apply */
            configure_hardware(media_dev);
            for (int i = 0; i < NUM_BUFFERS; i++) {
                struct v4l2_buffer qb;
                memset(&qb, 0, sizeof(qb));
                qb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                qb.memory = V4L2_MEMORY_MMAP;
                qb.index = i;
                ioctl(cap_fd, VIDIOC_QBUF, &qb);
            }
            if (ioctl(cap_fd, VIDIOC_STREAMON, &type) < 0) {
                fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
                usleep(200000);
                continue;
            }
            hardware_streaming = 1;
            frame_count = 0;
            printf("Reader active: sensor started (LED on)\n");
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(cap_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            fprintf(stderr, "VIDIOC_DQBUF error: %s\n", strerror(errno));
            break;
        }

        const uint16_t *raw_data = (const uint16_t *)buffers[buf.index].start;

        if ((frame_count % 8) == 0) {
            update_aec_awb_subsample(raw_data);
        }

        struct v4l2_format cur_ofmt;
        memset(&cur_ofmt, 0, sizeof(cur_ofmt));
        cur_ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        int cur_w = OUT_WIDTH;
        int cur_h = OUT_HEIGHT;
        if (ioctl(out_fd, VIDIOC_G_FMT, &cur_ofmt) == 0 &&
            cur_ofmt.fmt.pix.width > 0 && cur_ofmt.fmt.pix.height > 0) {
            cur_w = cur_ofmt.fmt.pix.width;
            cur_h = cur_ofmt.fmt.pix.height;
        }

        if (cur_w == OUT_WIDTH && cur_h == OUT_HEIGHT) {
            process_frame_yuyv_1080p(raw_data, yuyv_frame);
        } else {
            update_scaling_maps(cur_w, cur_h);
            process_frame_yuyv_scaled(raw_data, yuyv_frame, cur_w, cur_h);
        }

        write(out_fd, yuyv_frame, (size_t)cur_w * cur_h * 2);

        if (ioctl(cap_fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QBUF error: %s\n", strerror(errno));
            break;
        }

        frame_count++;
    }

    printf("Streaming stopped. Total frames: %u\n", frame_count);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cap_fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }
    close(cap_fd);
    close(out_fd);
    free(yuyv_frame);
    free(g_map_x);
    free(g_map_y);

    return 0;
}
