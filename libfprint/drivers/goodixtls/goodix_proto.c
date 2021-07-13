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

#include <gio/gio.h>
#include <glib.h>

#include "goodix_proto.h"

guint8 goodix_calc_checksum(guint8 *data, guint16 data_len) {
  guint8 checksum = 0;

  for (guint16 i = 0; i < data_len; i++) checksum += *(data + i);

  return checksum;
}

gsize goodix_encode_pack(guint8 **data, gboolean pad_data, guint8 flags,
                         guint8 *payload, guint16 payload_len,
                         GDestroyNotify payload_destroy) {
  gsize data_ptr_len = sizeof(goodix_pack) + sizeof(guint8) + payload_len;

  if (pad_data && data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    data_ptr_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(data_ptr_len);

  ((goodix_pack *)*data)->flags = flags;
  ((goodix_pack *)*data)->length = GUINT16_TO_LE(payload_len);
  *(*data + sizeof(goodix_pack)) =
      goodix_calc_checksum(*data, sizeof(goodix_pack));

  memcpy(*data + sizeof(goodix_pack) + sizeof(guint8), payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  return data_ptr_len;
}

gsize goodix_encode_protocol(guint8 **data, gboolean pad_data, guint8 cmd,
                             gboolean calc_checksum, guint8 *payload,
                             guint16 payload_len,
                             GDestroyNotify payload_destroy) {
  gsize data_ptr_len = sizeof(goodix_protocol) + payload_len + sizeof(guint8);

  if (pad_data && data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    data_ptr_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(data_ptr_len);

  ((goodix_protocol *)*data)->cmd = cmd;
  ((goodix_protocol *)*data)->length =
      GUINT16_TO_LE(payload_len + sizeof(guint8));

  memcpy(*data + sizeof(goodix_protocol), payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  if (calc_checksum)
    *(*data + sizeof(goodix_protocol) + payload_len) =
        0xaa -
        goodix_calc_checksum(*data, sizeof(goodix_protocol) + payload_len);
  else
    *(*data + sizeof(goodix_protocol) + payload_len) = 0x88;

  return data_ptr_len;
}

guint16 goodix_decode_pack(guint8 *flags, guint8 **payload,
                           guint16 *payload_len, guint8 *data, gsize data_len,
                           GDestroyNotify data_destroy, GError **error) {
  guint8 checksum;
  guint16 payload_ptr_len = data_len - sizeof(goodix_pack) - sizeof(guint8);
  guint16 pack_length;

  if (data_len < sizeof(goodix_pack) + sizeof(guint8)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack length: %ld", data_len);
    return 0;
  }

  pack_length = GUINT16_FROM_LE(((goodix_pack *)data)->length);

  if (payload_ptr_len >= pack_length) payload_ptr_len = pack_length;

  checksum = goodix_calc_checksum(data, sizeof(goodix_pack));
  if (checksum != *(data + sizeof(goodix_pack))) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack checksum: 0x%02x", checksum);
    return 0;
  }

  *payload =
      g_memdup(data + sizeof(goodix_pack) + sizeof(guint8), payload_ptr_len);
  if (data_destroy) data_destroy(data);

  *flags = ((goodix_pack *)data)->flags;
  *payload_len = pack_length;

  return payload_ptr_len;
}

guint16 goodix_decode_protocol(guint8 *cmd, guint8 **payload,
                               guint16 *payload_len, gboolean calc_checksum,
                               guint8 *data, gsize data_len,
                               GDestroyNotify data_destroy, GError **error) {
  guint8 checksum;
  guint16 payload_ptr_len = data_len - sizeof(goodix_protocol) - sizeof(guint8);
  guint16 protocol_length;

  if (data_len < sizeof(goodix_protocol) + sizeof(guint8)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %ld", data_len);
    return 0;
  }

  protocol_length =
      GUINT16_FROM_LE(((goodix_protocol *)data)->length) - sizeof(guint8);

  if (payload_ptr_len >= protocol_length) {
    payload_ptr_len = protocol_length;

    if (calc_checksum) {
      checksum = 0xaa - goodix_calc_checksum(
                            data, sizeof(goodix_protocol) + protocol_length);
      if (checksum != *(data + sizeof(goodix_protocol) + protocol_length)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Invalid message protocol checksum: 0x%02x", checksum);
        return 0;
      }
    } else if (0x88 != *(data + sizeof(goodix_protocol) + protocol_length)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid message protocol checksum: 0x%02x", 0x88);
      return 0;
    }
  }

  *payload = g_memdup(data + sizeof(goodix_protocol), payload_ptr_len);
  if (data_destroy) data_destroy(data);

  *cmd = ((goodix_protocol *)data)->cmd;
  *payload_len = protocol_length;

  return payload_ptr_len;
}

void goodix_decode_ack(guint8 *cmd, gboolean *has_no_config, guint8 *data,
                       guint16 data_len, GDestroyNotify data_destroy,
                       GError **error) {
  if (data_len != sizeof(goodix_ack)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid ack length: %d", data_len);
    return;
  }

  if (!((goodix_ack *)data)->always_true) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid ack flags: 0x%02x", *(data + sizeof(guint8)));
    return;
  }

  *cmd = ((goodix_ack *)data)->cmd;
  *has_no_config = ((goodix_ack *)data)->has_no_config;

  if (data_destroy) data_destroy(data);
}
