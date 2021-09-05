// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-assembling.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"
#include "glibconfig.h"
#include "gusb/gusb-device.h"
#include <stdio.h>
#define FP_COMPONENT "goodixtls511"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix511.h"

#include <math.h>

#define GOODIX511_WIDTH 88
#define GOODIX511_HEIGHT 80
#define GOODIX511_FRAME_SIZE 80 * 88
// For every 4 pixels there are 6 bytes and there are 8 extra start bytes and 5
// extra end
#define GOODIX511_RAW_FRAME_SIZE 8 + GOODIX511_FRAME_SIZE / 4 * 6 + 5
#define GOODIX511_CAP_FRAMES 3 // Number of frames we capture per swipe

struct _FpiDeviceGoodixTls511 {
  FpiDeviceGoodixTls parent;

  guint8* otp;

  GSList* frames;
};

G_DECLARE_FINAL_TYPE(FpiDeviceGoodixTls511, fpi_device_goodixtls511, FPI,
                     DEVICE_GOODIXTLS511, FpiDeviceGoodixTls);

G_DEFINE_TYPE(FpiDeviceGoodixTls511, fpi_device_goodixtls511,
              FPI_TYPE_DEVICE_GOODIXTLS);

// ---- ACTIVE SECTION START ----

enum activate_states {
    ACTIVATE_READ_AND_NOP,
    ACTIVATE_ENABLE_CHIP,
    ACTIVATE_NOP,
    ACTIVATE_CHECK_FW_VER,
    ACTIVATE_CHECK_PSK,
    ACTIVATE_RESET,
    ACTIVATE_SET_MCU_IDLE,
    ACTIVATE_SET_ODP,
    ACTIVATE_SET_MCU_CONFIG,
    ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,
    ACTIVATE_NUM_STATES,
};

static void check_none(FpDevice *dev, gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_firmware_version(FpDevice *dev, gchar *firmware,
                                   gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device firmware: \"%s\"", firmware);

  if (strcmp(firmware, GOODIX_511_FIRMWARE_VERSION)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device firmware: \"%s\"", firmware);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_reset(FpDevice *dev, gboolean success, guint16 number,
                        gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to reset device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device reset number: %d", number);

  if (number != GOODIX_511_RESET_NUMBER) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device reset number: %d", number);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_preset_psk_read(FpDevice *dev, gboolean success,
                                  guint32 flags, guint8 *psk, guint16 length,
                                  gpointer user_data, GError *error) {
  g_autofree gchar *psk_str = data_to_str(psk, length);

  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to read PSK from device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device PSK: 0x%s", psk_str);
  fp_dbg("Device PSK flags: 0x%08x", flags);

  if (flags != GOODIX_511_PSK_FLAGS) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK flags: 0x%08x", flags);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (length != sizeof(goodix_511_psk_0)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (memcmp(psk, goodix_511_psk_0, sizeof(goodix_511_psk_0))) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}
static void check_idle(FpDevice* dev, gpointer user_data, GError* err)
{

    if (err) {
        fpi_ssm_mark_failed(user_data, err);
        return;
    }
    fpi_ssm_next_state(user_data);
}
static void check_config_upload(FpDevice* dev, gboolean success,
                                gpointer user_data, GError* error)
{
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
    }
    else if (!success) {
        fpi_ssm_mark_failed(user_data,
                            g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to upload mcu config"));
    }
    else {
        fpi_ssm_next_state(user_data);
    }
}
static void check_powerdown_scan_freq(FpDevice* dev, gboolean success,
                                      gpointer user_data, GError* error)
{
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
    }
    else if (!success) {
        fpi_ssm_mark_failed(user_data,
                            g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to set powerdown freq"));
    }
    else {
        fpi_ssm_next_state(user_data);
    }
}

enum otp_write_states {
    OTP_WRITE_1,
    OTP_WRITE_2,
    OTP_WRITE_3,
    OTP_WRITE_4,

    OTP_WRITE_NUM,
};

static guint16 otp_write_addrs[] = {0x0220, 0x0236, 0x0238, 0x023a};

static void otp_write_run(FpiSsm* ssm, FpDevice* dev)
{
    guint16 data;
    FpiDeviceGoodixTls511* self = FPI_DEVICE_GOODIXTLS511(dev);
    guint8* otp = self->otp;
    switch (fpi_ssm_get_cur_state(ssm)) {
    case OTP_WRITE_1:
        data = otp[46] << 4 | 8;
        break;
    case OTP_WRITE_2:
        data = otp[47];
        break;
    case OTP_WRITE_3:
        data = otp[48];
        break;
    case OTP_WRITE_4:
        data = otp[49];
        break;
    }

    goodix_send_write_sensor_register(
        dev, otp_write_addrs[fpi_ssm_get_cur_state(ssm)], data, check_none,
        ssm);
    if (fpi_ssm_get_cur_state(ssm) == OTP_WRITE_NUM - 1) {
        free(self->otp);
    }
}

static void read_otp_callback(FpDevice* dev, guint8* data, guint16 len,
                              gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    if (len < 64) {
        fpi_ssm_mark_failed(ssm, g_error_new(FP_DEVICE_ERROR,
                                             FP_DEVICE_ERROR_DATA_INVALID,
                                             "OTP is invalid (len: %d)", 64));
        return;
    }
    FpiDeviceGoodixTls511* self = FPI_DEVICE_GOODIXTLS511(dev);
    self->otp = malloc(len);
    memcpy(self->otp, data, len);
    FpiSsm* otp_ssm = fpi_ssm_new(dev, otp_write_run, OTP_WRITE_NUM);
    fpi_ssm_start_subsm(ssm, otp_ssm);
}

static void activate_run_state(FpiSsm* ssm, FpDevice* dev)
{

    switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP:
        // Nop seems to clear the previous command buffer. But we are
        // unable to do so.
        goodix_receive_data(dev);
        goodix_send_nop(dev, check_none, ssm);
        break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip(dev, TRUE, check_none, ssm);
      break;

    case ACTIVATE_NOP:
      goodix_send_nop(dev, check_none, ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_firmware_version(dev, check_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read(dev, GOODIX_511_PSK_FLAGS, 0,
                                  check_preset_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_reset(dev, TRUE, 20, check_reset, ssm);
      break;

    case ACTIVATE_SET_MCU_IDLE:
        goodix_send_mcu_switch_to_idle_mode(dev, 20, check_idle, ssm);
        break;

    case ACTIVATE_SET_ODP:
        goodix_send_read_otp(dev, read_otp_callback, ssm);
        break;
    case ACTIVATE_SET_MCU_CONFIG:
        goodix_send_upload_config_mcu(dev, goodix_511_config,
                                      sizeof(goodix_511_config), NULL,
                                      check_config_upload, ssm);
        break;

    case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
        goodix_send_set_powerdown_scan_frequency(
            dev, 100, check_powerdown_scan_freq, ssm);
        break;
    }
}

static void tls_activation_complete(FpDevice* dev, gpointer user_data,
                                    GError* error)
{
    if (error) {
        fp_err("failed to complete tls activation: %s", error->message);
        return;
    }
    FpImageDevice* image_dev = FP_IMAGE_DEVICE(dev);

    fpi_image_device_activate_complete(image_dev, error);
}

static void activate_complete(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    G_DEBUG_HERE();
    if (!error)
        goodix_tls(dev, tls_activation_complete, NULL);
    else {
        fp_err("failed during activation: %s (code: %d)", error->message,
               error->code);
        fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
    }
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

enum SCAN_STAGES {
    SCAN_STAGE_SWITCH_TO_FDT_MODE,
    SCAN_STAGE_SWITCH_TO_FDT_DOWN,
    SCAN_STAGE_GET_IMG,

    SCAN_STAGE_NUM,
};

static void check_none_cmd(FpDevice* dev, guint8* data, guint16 len,
                           gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    fpi_ssm_next_state(ssm);
}

static unsigned char get_pix(struct fpi_frame_asmbl_ctx* ctx,
                             struct fpi_frame* frame, unsigned int x,
                             unsigned int y)
{
    return frame->data[x + y * ctx->frame_width];
}

// Bitdepth is 12, but we have to fit it in a byte
static unsigned char squash(int v) { return v / 16; }

static void decode_frame(guint8* raw_frame, GSList** frames)
{
    const int frame_size = GOODIX511_FRAME_SIZE * 2;
    struct fpi_frame* frame = g_malloc(frame_size + sizeof(struct fpi_frame));

    guint8* pix = frame->data;
    for (int i = 8; i != GOODIX511_RAW_FRAME_SIZE - 5; i += 6) {
        guint8* chunk = raw_frame + i;
        *pix++ = squash(((chunk[0] & 0xf) << 8) + chunk[1]);
        *pix++ = squash((chunk[3] << 4) + (chunk[0] >> 4));
        *pix++ = squash(((chunk[5] & 0xf) << 8) + chunk[2]);
        *pix++ = squash((chunk[4] << 4) + (chunk[5] >> 4));
    }
    *frames = g_slist_append(*frames, frame);
}

static void scan_on_read_img(FpDevice* dev, guint8* data, guint16 len,
                             gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    FpiDeviceGoodixTls511* self = FPI_DEVICE_GOODIXTLS511(dev);
    if (g_slist_length(self->frames) <= GOODIX511_CAP_FRAMES) {
        self->frames = g_slist_append(self->frames, data);
        fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_MODE);
    }
    else {
        self->frames = g_slist_append(self->frames, data);
        GSList* raw_frames = g_slist_nth(self->frames, 1);

        FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
        struct fpi_frame_asmbl_ctx assembly_ctx;
        assembly_ctx.frame_width = GOODIX511_WIDTH;
        assembly_ctx.frame_height = GOODIX511_HEIGHT;
        assembly_ctx.image_width = GOODIX511_WIDTH;
        assembly_ctx.get_pixel = get_pix;

        GSList* frames = NULL;

        g_slist_foreach(raw_frames, (GFunc) decode_frame, &frames);

        fpi_do_movement_estimation(&assembly_ctx, frames);
        FpImage* img = fpi_assemble_frames(&assembly_ctx, frames);
        img->flags |= FPI_IMAGE_PARTIAL;

        g_slist_free_full(frames, g_free);

        fpi_image_device_image_captured(img_dev, img);
        fpi_image_device_report_finger_status(img_dev, FALSE);

        fpi_ssm_next_state(ssm);
    }
}

static void scan_get_img(FpDevice* dev, FpiSsm* ssm)
{
    goodix_tls_read_image(dev, scan_on_read_img, ssm);
}

const guint8 fdt_switch_state_mode[] = {
    0x0d, 0x01, 0x80, 0xaf, 0x80, 0xbf, 0x80,
    0xa4, 0x80, 0xb8, 0x80, 0xa8, 0x80, 0xb7,
};

const guint8 fdt_switch_state_down[] = {
    0x0c, 0x01, 0x80, 0xaf, 0x80, 0xbf, 0x80,
    0xa4, 0x80, 0xb8, 0x80, 0xa8, 0x80, 0xb7,
};

static void scan_run_state(FpiSsm* ssm, FpDevice* dev)
{
    FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);

    switch (fpi_ssm_get_cur_state(ssm)) {
    case SCAN_STAGE_SWITCH_TO_FDT_MODE:
        goodix_send_mcu_switch_to_fdt_mode(dev, (guint8*) fdt_switch_state_mode,
                                           sizeof(fdt_switch_state_mode), NULL,
                                           check_none_cmd, ssm);
        break;

    case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
        goodix_send_mcu_switch_to_fdt_down(dev, (guint8*) fdt_switch_state_down,
                                           sizeof(fdt_switch_state_down), NULL,
                                           check_none_cmd, ssm);
        break;
    case SCAN_STAGE_GET_IMG:
        fpi_image_device_report_finger_status(img_dev, TRUE);
        scan_get_img(dev, ssm);
        break;
    }
}

static void scan_complete(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    if (error) {
        fp_err("failed to scan: %s (code: %d)", error->message, error->code);
        return;
    }
    fp_dbg("finished scan");
}

static void scan_start(FpiDeviceGoodixTls511* dev)
{
    fpi_ssm_start(fpi_ssm_new(FP_DEVICE(dev), scan_run_state, SCAN_STAGE_NUM),
                  scan_complete);
}

// ---- SCAN SECTION END ----

// ---- DEV SECTION START ----

static void dev_init(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_init(dev, &error)) {
    fpi_image_device_open_complete(img_dev, error);
    return;
  }

  fpi_image_device_open_complete(img_dev, NULL);
}

static void dev_deinit(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_deinit(dev, &error)) {
    fpi_image_device_close_complete(img_dev, error);
    return;
  }

  fpi_image_device_close_complete(img_dev, NULL);
}

static void dev_activate(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);

  fpi_ssm_start(fpi_ssm_new(dev, activate_run_state, ACTIVATE_NUM_STATES),
                activate_complete);
}



static void dev_change_state(FpImageDevice* img_dev, FpiImageDeviceState state)
{
    FpiDeviceGoodixTls511* self = FPI_DEVICE_GOODIXTLS511(img_dev);
    G_DEBUG_HERE();

    if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON) {
        scan_start(self);
    }
}

static void dev_deactivate(FpImageDevice *img_dev) {
  fpi_image_device_deactivate_complete(img_dev, NULL);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls511_init(FpiDeviceGoodixTls511* self)
{
    self->frames = g_slist_alloc();
}

static void fpi_device_goodixtls511_class_init(
    FpiDeviceGoodixTls511Class *class) {
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  gx_class->interface = GOODIX_511_INTERFACE;
  gx_class->ep_in = GOODIX_511_EP_IN;
  gx_class->ep_out = GOODIX_511_EP_OUT;

  dev_class->id = "goodixtls511";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 511";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;

  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  // TODO
  img_dev_class->bz3_threshold = 24;
  img_dev_class->img_width = GOODIX511_WIDTH;
  img_dev_class->img_height = GOODIX511_HEIGHT;

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
  dev_class->features &= ~FP_DEVICE_FEATURE_VERIFY;
}
