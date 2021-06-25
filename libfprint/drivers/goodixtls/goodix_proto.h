/*
 * Goodix Tls driver for libfprint
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
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

#include "drivers_api.h"
#include <glib.h>

#define GOODIX_MAX_DATA_WRITE 0x40
#define EP_IN_MAX_BUF_SIZE 0x2000

#define GOODIX_FLAGS_MSG_PROTOCOL 0xA0
#define GOODIX_FLAGS_TLS 0xB0

#define GOODIX_CMD_NOP 0x00
#define GOODIX_CMD_MCU_GET_IMAGE 0x20
#define GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN 0x32
#define GOODIX_CMD_MCU_SWITCH_TO_FDT_UP 0x34
#define GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE 0x36
#define GOODIX_CMD_NAV_0 0x50
#define GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE 0x70
#define GOODIX_CMD_WRITE_SENSOR_REGISTER 0x80
#define GOODIX_CMD_READ_SENSOR_REGISTER 0x82
#define GOODIX_CMD_UPLOAD_CONFIG_MCU 0x90
#define GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY 0x94
#define GOODIX_CMD_ENABLE_CHIP 0x96
#define GOODIX_CMD_RESET 0xA2
#define GOODIX_CMD_MCU_ERASE_APP 0xA4
#define GOODIX_CMD_READ_OTP 0xA6
#define GOODIX_CMD_FIRMWARE_VERSION 0xA8
#define GOODIX_CMD_QUERY_MCU_STATE 0xAE
#define GOODIX_CMD_ACK 0xB0
#define GOODIX_CMD_REQUEST_TLS_CONNECTION 0xD0
#define GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED 0xD4
#define GOODIX_CMD_PRESET_PSK_WRITE_R 0xE0
#define GOODIX_CMD_PRESET_PSK_READ_R 0xE4

guint8 goodix_calc_checksum(gpointer data, guint16 data_len);

gsize goodix_encode_pack(gpointer *data, guint8 flags, gpointer payload,
                         guint16 payload_len, GDestroyNotify payload_destroy);

gsize goodix_encode_protocol(gpointer *data, guint8 cmd, gboolean calc_checksum,
                             gpointer payload, guint16 payload_len,
                             GDestroyNotify payload_destroy);

guint16 goodix_decode_pack(guint8 *flags, gpointer *payload,
                           guint16 *payload_len, gpointer data, gsize data_len,
                           GDestroyNotify data_destroy, GError **error);

guint16 goodix_decode_protocol(guint8 *cmd, gboolean *invalid_checksum,
                               gpointer *payload, guint16 *payload_len,
                               gpointer data, gsize data_len,
                               GDestroyNotify data_destroy, GError **error);

gboolean goodix_decode_ack(guint8 *cmd, gpointer data, guint16 data_len,
                           GDestroyNotify data_destroy, GError **error);
