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

#define GOODIX_MAX_PACKET_SIZE 64

#define COMMAND_NOP 0x00
#define COMMAND_MCU_GET_IMAGE 0x20
#define COMMAND_MCU_SWITCH_TO_FDT_DOWN 0x32
#define COMMAND_MCU_SWITCH_TO_FDT_UP 0x34
#define COMMAND_MCU_SWITCH_TO_FDT_MODE 0x36
#define COMMAND_NAV_0 0x50
#define COMMAND_MCU_SWITCH_TO_IDLE_MODE 0x70
#define COMMAND_WRITE_SENSOR_REGISTER 0x80
#define COMMAND_READ_SENSOR_REGISTER 0x82
#define COMMAND_UPLOAD_CONFIG_MCU 0x90
#define COMMAND_SET_POWERDOWN_SCAN_FREQUENCY 0x94
#define COMMAND_ENABLE_CHIP 0x96
#define COMMAND_RESET 0xa2
#define COMMAND_MCU_ERASE_APP 0xa4
#define COMMAND_READ_OTP 0xa6
#define COMMAND_FIRMWARE_VERSION 0xa8
#define COMMAND_QUERY_MCU_STATE 0xae
#define COMMAND_ACK 0xb0
#define COMMAND_REQUEST_TLS_CONNECTION 0xd0
#define COMMAND_TLS_SUCCESSFULLY_ESTABLISHED 0xd4
#define COMMAND_PRESET_PSK_WRITE_R 0xe0
#define COMMAND_PRESET_PSK_READ_R 0xe4
#define COMMAND_WRITE_FIRMWARE 0xf0
#define COMMAND_READ_FIRMWARE 0xf2
#define COMMAND_CHECK_FIRMWARE 0xf4
