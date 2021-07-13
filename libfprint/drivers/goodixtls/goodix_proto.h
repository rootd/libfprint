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

#pragma once

#define GOODIX_EP_IN_MAX_BUF_SIZE (0x2000)
#define GOODIX_EP_OUT_MAX_BUF_SIZE (0x40)

#define GOODIX_FLAGS_MSG_PROTOCOL (0xa0)
#define GOODIX_FLAGS_TLS (0xb0)

#define GOODIX_CMD_NOP (0x00)
#define GOODIX_CMD_MCU_GET_IMAGE (0x20)
#define GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN (0x32)
#define GOODIX_CMD_MCU_SWITCH_TO_FDT_UP (0x34)
#define GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE (0x36)
#define GOODIX_CMD_NAV_0 (0x50)
#define GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE (0x70)
#define GOODIX_CMD_WRITE_SENSOR_REGISTER (0x80)
#define GOODIX_CMD_READ_SENSOR_REGISTER (0x82)
#define GOODIX_CMD_UPLOAD_CONFIG_MCU (0x90)
#define GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY (0x94)
#define GOODIX_CMD_ENABLE_CHIP (0x96)
#define GOODIX_CMD_RESET (0xa2)
#define GOODIX_CMD_READ_OTP (0xa6)
#define GOODIX_CMD_FIRMWARE_VERSION (0xa8)
#define GOODIX_CMD_QUERY_MCU_STATE (0xae)
#define GOODIX_CMD_ACK (0xb0)
#define GOODIX_CMD_REQUEST_TLS_CONNECTION (0xd0)
#define GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED (0xd4)
#define GOODIX_CMD_PRESET_PSK_WRITE_R (0xe0)
#define GOODIX_CMD_PRESET_PSK_READ_R (0xe4)

typedef struct __attribute__((__packed__)) _goodix_pack {
  guint8 flags;
  guint16 length;
} goodix_pack;

typedef struct __attribute__((__packed__)) _goodix_protocol {
  guint8 cmd;
  guint16 length;
} goodix_protocol;

typedef struct __attribute__((__packed__)) _goodix_ack {
  guint8 cmd;
  guint8 always_true : 1;
  guint8 has_no_config : 1;
  guint8 : 6;
} goodix_ack;

typedef struct __attribute__((__packed__)) _goodix_nop {
  guint32 unknown;
} goodix_nop;

typedef struct __attribute__((__packed__)) _goodix_mcu_switch_to_idle_mode {
  guint8 sleep_time;
  guint8 : 8;
} goodix_mcu_switch_to_idle_mode;

typedef struct __attribute__((__packed__)) _goodix_write_sensor_register {
  guint8 multiples;
  guint16 address;
  guint16 value;
} goodix_write_sensor_register;

typedef struct __attribute__((__packed__)) _goodix_read_sensor_register {
  guint8 multiples;
  guint16 address;
  guint8 length;
  guint8 : 8;
} goodix_read_sensor_register;

typedef struct __attribute__((__packed__))
_goodix_set_powerdown_scan_frequency {
  guint16 powerdown_scan_frequency;
} goodix_set_powerdown_scan_frequency;

typedef struct __attribute__((__packed__)) _goodix_enable_chip {
  guint8 enable;
  guint8 : 8;
} goodix_enable_chip;

typedef struct __attribute__((__packed__)) _goodix_reset {
  guint8 reset_sensor : 1;
  guint8 soft_reset_mcu : 1;
  guint8 : 6;
  guint8 sleep_time;
} goodix_reset;

typedef struct __attribute__((__packed__)) _goodix_query_mcu_state {
  guint8 unused_flags;
} goodix_query_mcu_state;

typedef struct __attribute__((__packed__)) _goodix_preset_psk_r {
  guint32 address;
  guint32 length;
} goodix_preset_psk_r;

typedef struct __attribute__((__packed__)) _goodix_default {
  guint8 unused_flags;
  guint8 : 8;
} goodix_default;

typedef struct __attribute__((__packed__)) _goodix_none {
  guint16 : 16;
} goodix_none;

guint8 goodix_calc_checksum(guint8 *data, guint16 data_len);

gsize goodix_encode_pack(guint8 **data, gboolean pad_data, guint8 flags,
                         guint8 *payload, guint16 payload_len,
                         GDestroyNotify payload_destroy);

gsize goodix_encode_protocol(guint8 **data, gboolean pad_data, guint8 cmd,
                             gboolean calc_checksum, guint8 *payload,
                             guint16 payload_len,
                             GDestroyNotify payload_destroy);

guint16 goodix_decode_pack(guint8 *flags, guint8 **payload,
                           guint16 *payload_len, guint8 *data, gsize data_len,
                           GDestroyNotify data_destroy, GError **error);

guint16 goodix_decode_protocol(guint8 *cmd, guint8 **payload,
                               guint16 *payload_len, gboolean calc_checksum,
                               guint8 *data, gsize data_len,
                               GDestroyNotify data_destroy, GError **error);

void goodix_decode_ack(guint8 *cmd, gboolean *has_no_config, guint8 *data,
                       guint16 data_len, GDestroyNotify data_destroy,
                       GError **error);
