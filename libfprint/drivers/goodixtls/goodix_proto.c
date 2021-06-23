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

guint8 goodix_calc_checksum(gpointer data, guint16 data_len) {
  guint8 checksum = 0;

  for (guint16 i = 0; i < data_len; i++) checksum += *((guint8 *)data + i);

  return checksum;
}

gsize goodix_encode_pack(gpointer *data, guint8 flags, gpointer payload,
                         guint16 payload_len, GDestroyNotify payload_destroy) {
  // Only work on little endian machine

  gsize data_ptr_pen =
      ceil((payload_len + 4) / GOODIX_MAX_DATA_WRITE) * GOODIX_MAX_DATA_WRITE;

  *data = g_malloc0(payload_len + 4);  // Use g_malloc?

  *(guint8 *)*data = flags;
  *(guint16 *)((guint8 *)*data + 1) = payload_len;
  *((guint8 *)*data + 3) = goodix_calc_checksum(*data, 3);
  memcpy((guint8 *)*data + 4, payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  return data_ptr_pen;
}

gsize goodix_encode_protocol(gpointer *data, guint8 cmd, gboolean calc_checksum,
                             gpointer payload, guint16 payload_len,
                             GDestroyNotify payload_destroy) {
  // Only work on little endian machine

  gsize payload_ptr_len = payload_len + 4;

  *data = g_malloc(payload_ptr_len);

  *(guint8 *)*data = cmd;
  *(guint16 *)((guint8 *)*data + 1) = payload_len + 1;

  memcpy((guint8 *)*data + 3, payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  if (calc_checksum)
    *((guint8 *)*data + payload_ptr_len - 1) =
        0xaa - goodix_calc_checksum(*data, payload_ptr_len - 1);
  else
    *((guint8 *)*data + payload_ptr_len - 1) = 0x88;

  return payload_ptr_len;
}

guint16 goodix_decode_pack(guint8 *flags, gpointer *payload,
                           guint16 *payload_len, gpointer data, gsize data_len,
                           GDestroyNotify data_destroy, GError **error) {
  // Only work on little endian machine

  guint16 payload_ptr_len = data_len - 4;

  if (data_len < 4) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack length: %d", data_len);
    return 0;
  }

  *flags = *(guint8 *)data;
  *payload_len = *(guint16 *)((guint8 *)data + 1);

  if (*payload_len <= payload_ptr_len) payload_ptr_len = *payload_len;

  if (goodix_calc_checksum(data, 3) != *((guint8 *)data + 3)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Invalid message pack checksum");
    return 0;
  }

  *payload = g_memdup((guint8 *)data + 4, payload_ptr_len);
  if (data_destroy) data_destroy(data);

  return payload_ptr_len;
}

guint16 goodix_decode_protocol(guint8 *cmd, gboolean *invalid_checksum,
                               gpointer *payload, guint16 *payload_len,
                               gpointer data, gsize data_len,
                               GDestroyNotify data_destroy, GError **error) {
  // Only work on little endian machine

  guint16 payload_ptr_len = data_len - 4;

  if (data_len < 4) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %d", data_len);
    return 0;
  }

  *cmd = *(guint8 *)data;
  *payload_len = *(guint16 *)((guint8 *)data + 1) - 1;

  if (*payload_len <= payload_ptr_len) {
    payload_ptr_len = *payload_len;

    *invalid_checksum = 0xaa - goodix_calc_checksum(data, *payload_len + 3) !=
                        *((guint8 *)data + *payload_len + 3);
  }

  *payload = g_memdup((guint8 *)data + 3, payload_ptr_len);
  if (data_destroy) data_destroy(data);

  return payload_ptr_len;
}

gboolean goodix_decode_ack(guint8 *cmd, gpointer data, guint16 data_len,
                           GDestroyNotify data_destroy, GError **error) {
  guint8 status;

  if (data_len != 2) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid ack length: %d", data_len);
    return 0;
  }

  *cmd = *(guint8 *)data;
  status = *((guint8 *)data + 1);
  if (data_destroy) data_destroy(data);

  if (!(status & 0x1)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Invalid ack status");
    return 0;
  }

  return status & 0x2 == 0x2;
}
