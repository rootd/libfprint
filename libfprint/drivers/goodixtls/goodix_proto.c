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

#include "goodix_proto.h"

guint8 goodix_calculate_checksum(gpointer data, guint32 length) {
  guint8 checksum = 0;

  for (guint32 i = 0; i < length; i++) checksum += *((guint8 *)data + i);

  return checksum;
}

guint32 goodix_encode_message_pack(gpointer *payload, guint8 flags,
                                   gpointer data, guint16 length) {
  // Only work on little endian machine

  guint32 payload_length = length + 4;
  gpointer payload_ptr = g_malloc(payload_length);

  *(guint8 *)payload_ptr = flags;
  *((guint16 *)payload_ptr + 1) = length;
  *((guint8 *)payload_ptr + 3) = goodix_calculate_checksum(payload_ptr, 3);
  memcpy((guint8 *)payload_ptr + 4, data, length);

  *payload = payload_ptr;
  return payload_length;
}

guint32 goodix_encode_message_protocol(gpointer *payload, guint8 command,
                                       gpointer data, guint16 length,
                                       gboolean calculate_checksum) {
  // Only work on little endian machine

  guint32 payload_length = length + 4;
  gpointer payload_ptr = g_malloc(payload_length);

  *(guint8 *)payload_ptr = command;
  *((guint16 *)payload_ptr + 1) = length;
  memcpy((guint8 *)payload_ptr + 3, data, length);
  if (calculate_checksum)
    *((guint8 *)payload_ptr + payload_length - 1) =
        0xaa - goodix_calculate_checksum(payload_ptr, payload_length - 1);
  else
    *((guint8 *)payload_ptr + payload_length - 1) = 0x88;

  *payload = payload_ptr;
  return payload_length;
}
