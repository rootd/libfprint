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

#define FP_COMPONENT "goodixtls511"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix511.h"

struct _FpiDeviceGoodixTls511 {
  FpiDeviceGoodixTls parent;
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
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,
  ACTIVATE_NUM_STATES,
};

static gboolean check_firmware(gchar *firmware, GError **error,
                               gpointer user_data) {
  if (strcmp(firmware, FIRMWARE_VERSION)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device firmware: \"%s\"", firmware);
    return TRUE;
  }

  return FALSE;
}

static gboolean check_pmk(guint8 *pmk, guint16 pmk_len, GError **error,
                          gpointer user_data) {
  gchar *pmk_str;
  if (pmk_len != sizeof(pmk_hash)) {
    pmk_str = data_to_string(pmk, pmk_len);

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PMK hash: 0x%s", pmk_str);
    g_free(pmk_str);

    return TRUE;
  }

  if (memcmp(pmk, pmk_hash, sizeof(pmk_hash))) {
    pmk_str = data_to_string(pmk, pmk_len);

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PMK hash: 0x%s", pmk);
    g_free(pmk_str);

    return TRUE;
  }

  return FALSE;
}

static void activate_run_state(FpiSsm *ssm, FpDevice *dev) {
  switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP:
      // Nop seems to clear the previous command buffer. But we are unable to do
      // so.
      goodix_receive_data(ssm);
      goodix_send_nop(ssm);
      break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip(ssm, TRUE);
      break;

    case ACTIVATE_NOP:
      goodix_send_nop(ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_firmware_version(ssm, check_firmware, NULL);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read_r(ssm, PMK_HASH_ADDRESS, 0, check_pmk, NULL);
      break;

    case ACTIVATE_RESET:
      goodix_send_reset(ssm, TRUE, 20, NULL, NULL);
      break;

    case ACTIVATE_SET_MCU_IDLE:
      goodix_send_mcu_switch_to_idle_mode(ssm, 20);
      break;

    case ACTIVATE_SET_MCU_CONFIG:
      goodix_send_upload_config_mcu(ssm, device_config, sizeof(device_config),
                                    NULL, NULL, NULL);
      break;

    case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
      goodix_send_set_powerdown_scan_frequency(ssm, 100, NULL, NULL);
      break;
  }
}

static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  FpImageDevice *image_dev = FP_IMAGE_DEVICE(dev);

  fpi_image_device_activate_complete(image_dev, error);

  if (!error) goodix_tls(dev);
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

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

static void dev_change_state(FpImageDevice *img_dev,
                             FpiImageDeviceState state) {}

static void dev_deactivate(FpImageDevice *img_dev) {
  fpi_image_device_deactivate_complete(img_dev, NULL);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls511_init(FpiDeviceGoodixTls511 *self) {}

static void fpi_device_goodixtls511_class_init(
    FpiDeviceGoodixTls511Class *class) {
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  gx_class->interface = GOODIX_INTERFACE;
  gx_class->ep_in = GOODIX_EP_IN;
  gx_class->ep_out = GOODIX_EP_OUT;

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
