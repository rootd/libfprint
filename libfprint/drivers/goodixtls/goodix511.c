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
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,
  ACTIVATE_NUM_STATES,
};

static void activate_run_state(FpiSsm *ssm, FpDevice *dev) {
  G_DEBUG_HERE();

  switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP:
      // Nop seems to clear the previous command buffer. But we are unable to do
      // so.
      goodix_receive_data(ssm);
      // DON'T ADD A BREAK HERE!
    case ACTIVATE_NOP:
      goodix_cmd_nop(ssm);
      break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_cmd_enable_chip(ssm, TRUE);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_cmd_firmware_version(ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_cmd_preset_psk_read_r(ssm, 0xbb020003, 0);
      break;

    case ACTIVATE_SET_MCU_IDLE:
      goodix_cmd_mcu_switch_to_idle_mode(ssm, 20);
      break;

    case ACTIVATE_SET_MCU_CONFIG:
      goodix_cmd_upload_config_mcu(ssm, device_config, sizeof(device_config),
                                   NULL);
      break;

    case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
      goodix_cmd_set_powerdown_scan_frequency(ssm, 100);
      break;
  }
}

static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  FpImageDevice *image_dev = FP_IMAGE_DEVICE(dev);

  G_DEBUG_HERE();

  fpi_image_device_activate_complete(image_dev, error);

  if (!error) goodix_tls(dev);
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

static void dev_init(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  G_DEBUG_HERE();

  if (goodix_dev_init(dev, &error)) {
    fpi_image_device_open_complete(img_dev, error);
    return;
  }

  fpi_image_device_open_complete(img_dev, NULL);
}

static void dev_deinit(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  G_DEBUG_HERE();

  if (goodix_dev_deinit(dev, &error)) {
    fpi_image_device_close_complete(img_dev, error);
    return;
  }

  fpi_image_device_close_complete(img_dev, NULL);
}

static void dev_activate(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);

  G_DEBUG_HERE();

  fpi_ssm_start(fpi_ssm_new(dev, activate_run_state, ACTIVATE_NUM_STATES),
                activate_complete);
}

static void dev_change_state(FpImageDevice *img_dev,
                             FpiImageDeviceState state) {
  G_DEBUG_HERE();
}

static void dev_deactivate(FpImageDevice *img_dev) {
  G_DEBUG_HERE();

  fpi_image_device_deactivate_complete(img_dev, NULL);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls511_init(FpiDeviceGoodixTls511 *self) {}

static void fpi_device_goodixtls511_class_init(
    FpiDeviceGoodixTls511Class *class) {
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  G_DEBUG_HERE();

  gx_class->interface = GOODIX_INTERFACE;
  gx_class->ep_in = GOODIX_EP_IN;
  gx_class->ep_out = GOODIX_EP_OUT;
  gx_class->firmware_version = (gchar *)GOODIX_FIRMWARE_VERSION_SUPPORTED;

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
