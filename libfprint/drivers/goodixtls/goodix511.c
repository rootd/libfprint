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

struct _FpiDeviceGoodixTls511 {
  FpiDeviceGoodixTls parent;

  guint8* otp;
  guint16 otp_len;

  GSList* frames;
};

#define GOODIXTLS_CAP_FRAMES 1

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
    G_DEBUG_HERE();

    if (err) {
        fpi_ssm_mark_failed(user_data, err);
        return;
    }
    fpi_ssm_next_state(user_data);
}
static void check_config_upload(FpDevice* dev, gboolean success,
                                gpointer user_data, GError* error)
{
    G_DEBUG_HERE();
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
    }
    else if (!success) {
        GError* err = malloc(sizeof(GError));
        err->message = "Failed to upload config";
        fpi_ssm_mark_failed(user_data, err);
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
        GError* err = malloc(sizeof(GError));
        err->message = "Failed set powerdown";
        fpi_ssm_mark_failed(user_data, err);
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
    self->otp = data;
    self->otp_len = len;
    FpiSsm* otp_ssm = fpi_ssm_new(dev, otp_write_run, OTP_WRITE_NUM);
    fpi_ssm_start_subsm(ssm, otp_ssm);
}

static void activate_run_state(FpiSsm *ssm, FpDevice *dev) {
  GError *error = NULL;

  switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP:
        /* Uncomment below in case the successfully established bit didn't get
           run and you get a timeout when trying to rerun */
        // goodix_send_tls_successfully_established(dev, NULL, NULL);
        // exit(0);
        //           Nop seems to clear the previous command buffer. But we are
        //           unable to do so.
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
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

enum SCAN_STAGES {
    SCAN_STAGE_QUERY_MCU,
    SCAN_STAGE_SWITCH_TO_FDT_MODE_0,
    SCAN_STAGE_NAV_0,
    SCAN_STAGE_SWITCH_TO_FDT_MODE_1,
    SCAN_STAGE_READ_REG,
    SCAN_STAGE_GET_IMG_0,
    SCAN_STAGE_SWITCH_TO_FDT_MODE_2,
    SCAN_STAGE_SWITCH_TO_FDT_DOWN,
    SCAN_STAGE_GET_IMG_1,

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

static void write_pgm(guint8* data, guint16 len)
{
    g_assert(len != 0);
    int needed = len / 6 * 4;
    int* img = malloc(sizeof(int) * needed);
    int* head = img;
    for (int i = 0; i < len; i += 6) {
        guint8* chunk = data + i;
        *head++ = ((chunk[0] & 0xf) << 8) + chunk[1];
        *head++ = ((chunk[3] << 4) + (chunk[0] >> 4));
        *head++ = ((chunk[5] & 0xf) << 8) + chunk[2];
        *head++ = ((chunk[4] << 4) + (chunk[5] >> 4));
    }
    FILE* out = fopen("./fingerprint.pgm", "w");
    const char buff[] = "P2\n88 80\n4095\n";
    fwrite(buff, sizeof(char), sizeof(buff), out);
    int count = 0;
    for (int* line = img; count < needed; line += 4) {
        fwrite(line, sizeof(int), 4, out);
        fwrite("\n", sizeof(char), 1, out);
        count += 4;
    }
    fclose(out);
    free(img);
}
static unsigned char get_pix(struct fpi_frame_asmbl_ctx* ctx,
                             struct fpi_frame* frame, unsigned int x,
                             unsigned int y)
{
    return frame->data[x + y * ctx->frame_width];
}
static void scan_on_read_img(FpDevice* dev, guint8* data, guint16 len,
                             gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    FpiDeviceGoodixTls511* self = FPI_DEVICE_GOODIXTLS511(dev);
    if (fpi_ssm_get_cur_state(ssm) == SCAN_STAGE_GET_IMG_1) {
        if (g_slist_length(self->frames) > 1) {
            self->frames = g_slist_append(self->frames, data);
            fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_MODE_2);
            return;
        }
        else {
            self->frames = g_slist_append(self->frames, data);

            FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
            struct fpi_frame_asmbl_ctx assembly_ctx;
            assembly_ctx.frame_width = 80;
            assembly_ctx.frame_height = 86;
            assembly_ctx.image_width = 80;
            assembly_ctx.get_pixel = get_pix;

            // write_pgm(data, len);
            FpImage* img = fp_image_new(80, 88);
            img->data = malloc(sizeof(guint8) * 80 * 88);
            memcpy(img->data, data, 80 * 88);
            img->flags |= FPI_IMAGE_PARTIAL;

            fpi_image_device_image_captured(img_dev, img);
            fpi_image_device_report_finger_status(img_dev, FALSE);
        }
    }

    fpi_ssm_next_state(ssm);
}

static void scan_get_img(FpDevice* dev, FpiSsm* ssm)
{
    goodix_tls_read_image(dev, scan_on_read_img, ssm);
}
const guint8 fdt_switch_state_mode_2[] = {
    0x0d, 0x01, 0x80, 0xaf, 0x80, 0xbf, 0x80,
    0xa4, 0x80, 0xb8, 0x80, 0xa8, 0x80, 0xb7,
};
const guint8 fdt_switch_state_mode_0[] = {0x0d, 0x01, 0xae, 0xae, 0xbf,
                                          0xbf, 0xa4, 0xa4, 0xb8, 0xb8,
                                          0xa8, 0xa8, 0xb7, 0xb7};
const guint8 fdt_switch_state_mode_1[] = {0x0d, 0x01, 0x80, 0xaf, 0x80, 0xbf,
                                          0x80, 0xa3, 0x0d, 0x01, 0x80, 0xaf,
                                          0x80, 0xbf, 0x80, 0xa3};

const guint8 fdt_switch_state_down[] = {
    0x0c, 0x01, 0x80, 0xaf, 0x80, 0xbf, 0x80,
    0xa4, 0x80, 0xb8, 0x80, 0xa8, 0x80, 0xb7,
};

const guint8 fdt_switch_state_down_1[] = {0x0d, 0x01, 0x80, 0xaf, 0x80,
                                          0xbf, 0x80, 0xa4, 0x80, 0xb8,
                                          0x80, 0xa8, 0x80, 0xb7};

static void scan_run_state(FpiSsm* ssm, FpDevice* dev)
{
    FpiDeviceGoodixTls511* self = FPI_DEVICE_GOODIXTLS511(dev);
    FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
    switch (fpi_ssm_get_cur_state(ssm)) {
    case SCAN_STAGE_QUERY_MCU:
        goodix_send_query_mcu_state(dev, check_none_cmd, ssm);
        break;

    case SCAN_STAGE_SWITCH_TO_FDT_MODE_0:
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_switch_state_mode_0,
                                           sizeof(fdt_switch_state_mode_0),
                                           NULL, check_none_cmd, ssm);
        break;
    case SCAN_STAGE_NAV_0:
        goodix_send_nav_0(dev, check_none_cmd, ssm);
        break;
    case SCAN_STAGE_SWITCH_TO_FDT_MODE_1:
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_switch_state_mode_1,
                                           sizeof(fdt_switch_state_mode_1),
                                           NULL, check_none_cmd, ssm);
        break;
    case SCAN_STAGE_SWITCH_TO_FDT_MODE_2:
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_switch_state_mode_2,
                                           sizeof(fdt_switch_state_mode_2),
                                           NULL, check_none_cmd, ssm);
        break;

    case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
        goodix_send_mcu_switch_to_fdt_down(dev, fdt_switch_state_down,
                                           sizeof(fdt_switch_state_down), NULL,
                                           check_none_cmd, ssm);
        break;
    case SCAN_STAGE_GET_IMG_1:
        fpi_image_device_report_finger_status(img_dev, TRUE);
    case SCAN_STAGE_GET_IMG_0:
        scan_get_img(dev, ssm);
        break;
    case SCAN_STAGE_READ_REG:
        goodix_send_read_sensor_register(dev, 0x0082, 2, check_none_cmd, ssm);
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
  img_dev_class->img_width = 80;
  img_dev_class->img_height = 88;

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
  dev_class->features &= ~FP_DEVICE_FEATURE_VERIFY;
}
