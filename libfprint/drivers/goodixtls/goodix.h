/*
 * Goodix 5110 driver for libfprint
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>
#include <stdint.h>
#include <openssl/ssl.h>

#define GOODIX_VEND_ID 0x27c6

#define GOODIX_CMD_LEN 64
#define GOODIX_EP_CMD_OUT 0x1
#define GOODIX_EP_CMD_IN 0x81

// Needed for commands which don't send an answer back (currently known only NOP)
#define GOODIX_CMD_SKIP_READ -1

// 10 seconds USB read timeout
#define GOODIX_CMD_TIMEOUT 10000

#define GOODIX_FIRMWARE_VERSION_SUPPORTED "GF_ST411SEC_APP_12109"

struct goodix_cmd
{
  uint8_t cmd[64];
  int response_len;
  int response_len_2;
};

static const struct goodix_cmd nop = {
  .cmd = {0xA0, 0x08, 0x00, 0xA8, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA5},
  .response_len = GOODIX_CMD_SKIP_READ,
};

static const struct goodix_cmd enable_chip = {
  .cmd = {0xA0, 0X06, 0x00, 0xA6, 0x96, 0x03, 0x00, 0x01, 0x00, 0x10},
  .response_len = 10,
};

static const struct goodix_cmd read_fw = {
  .cmd = {0xA0, 0x06, 0x00, 0xA6, 0xA8, 0x03, 0x00, 0x00, 0x00, 0xFF},
  .response_len = 10,
  .response_len_2 = 30,
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTLS, fpi_device_goodixtls, FPI, DEVICE_GOODIXTLS,
                      FpImageDevice);

// VID=0 PID=0 is needed for termination
static const FpIdEntry goodix_id_table[] = {
  {.vid = GOODIX_VEND_ID,  .pid = 0x5110, .driver_data = 0},
  {.vid = 0,  .pid = 0,  .driver_data = 0},
};

static void goodix_dev_reset_state (FpiDeviceGoodixTLS *goodixdev);
static void goodix_cmd_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error);
