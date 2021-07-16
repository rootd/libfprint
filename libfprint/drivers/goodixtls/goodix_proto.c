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

guint32 goodix_encode_pack(guint8 flags, guint8 *payload, guint16 payload_len,
                           GDestroyNotify payload_destroy, gboolean pad_data,
                           guint8 **data) {
  guint32 data_ptr_len = sizeof(GoodixPack) + sizeof(guint8) + payload_len;

  if (pad_data && data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    data_ptr_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(data_ptr_len);

  ((GoodixPack *)*data)->flags = flags;
  ((GoodixPack *)*data)->length = GUINT16_TO_LE(payload_len);
  *(*data + sizeof(GoodixPack)) =
      goodix_calc_checksum(*data, sizeof(GoodixPack));

  memcpy(*data + sizeof(GoodixPack) + sizeof(guint8), payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  return data_ptr_len;
}

guint32 goodix_encode_protocol(guint8 cmd, guint8 *payload, guint16 payload_len,
                               GDestroyNotify payload_destroy,
                               gboolean calc_checksum, gboolean pad_data,
                               guint8 **data) {
  guint32 data_ptr_len = sizeof(GoodixProtocol) + payload_len + sizeof(guint8);

  if (pad_data && data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    data_ptr_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(data_ptr_len);

  ((GoodixProtocol *)*data)->cmd = cmd;
  ((GoodixProtocol *)*data)->length =
      GUINT16_TO_LE(payload_len + sizeof(guint8));

  memcpy(*data + sizeof(GoodixProtocol), payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  if (calc_checksum)
    *(*data + sizeof(GoodixProtocol) + payload_len) =
        0xaa -
        goodix_calc_checksum(*data, sizeof(GoodixProtocol) + payload_len);
  else
    *(*data + sizeof(GoodixProtocol) + payload_len) = 0x88;

  return data_ptr_len;
}

guint16 goodix_decode_pack(guint8 *data, guint32 data_len,
                           GDestroyNotify data_destroy, guint8 *flags,
                           guint8 **payload, guint16 *payload_len,
                           GError **error) {
  guint8 checksum;
  guint16 payload_ptr_len = data_len - sizeof(GoodixPack) - sizeof(guint8);
  guint16 pack_length;

  if (data_len < sizeof(GoodixPack) + sizeof(guint8)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack length: %d", data_len);
    return 0;
  }

  pack_length = GUINT16_FROM_LE(((GoodixPack *)data)->length);

  if (payload_ptr_len >= pack_length) payload_ptr_len = pack_length;

  checksum = goodix_calc_checksum(data, sizeof(GoodixPack));
  if (checksum != *(data + sizeof(GoodixPack))) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack checksum: 0x%02x", checksum);
    return 0;
  }

  *payload =
      g_memdup(data + sizeof(GoodixPack) + sizeof(guint8), payload_ptr_len);
  if (data_destroy) data_destroy(data);

  *flags = ((GoodixPack *)data)->flags;
  *payload_len = pack_length;

  return payload_ptr_len;
}

guint16 goodix_decode_protocol(guint8 *data, guint32 data_len,
                               GDestroyNotify data_destroy,
                               gboolean calc_checksum, guint8 *cmd,
                               guint8 **payload, guint16 *payload_len,
                               GError **error) {
  guint8 checksum;
  guint16 payload_ptr_len = data_len - sizeof(GoodixProtocol) - sizeof(guint8);
  guint16 protocol_length;

  if (data_len < sizeof(GoodixProtocol) + sizeof(guint8)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %d", data_len);
    return 0;
  }

  protocol_length =
      GUINT16_FROM_LE(((GoodixProtocol *)data)->length) - sizeof(guint8);

  if (payload_ptr_len >= protocol_length) {
    payload_ptr_len = protocol_length;

    if (calc_checksum) {
      checksum = 0xaa - goodix_calc_checksum(
                            data, sizeof(GoodixProtocol) + protocol_length);
      if (checksum != *(data + sizeof(GoodixProtocol) + protocol_length)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Invalid message protocol checksum: 0x%02x", checksum);
        return 0;
      }
    } else if (0x88 != *(data + sizeof(GoodixProtocol) + protocol_length)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid message protocol checksum: 0x%02x", 0x88);
      return 0;
    }
  }

  *payload = g_memdup(data + sizeof(GoodixProtocol), payload_ptr_len);
  if (data_destroy) data_destroy(data);

  *cmd = ((GoodixProtocol *)data)->cmd;
  *payload_len = protocol_length;

  return payload_ptr_len;
}

void goodix_decode_ack(guint8 *data, guint16 data_len,
                       GDestroyNotify data_destroy, guint8 *cmd,
                       gboolean *has_no_config, GError **error) {
  if (data_len != sizeof(GoodixAck)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid ack length: %d", data_len);
    return;
  }

  if (!((GoodixAck *)data)->always_true) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid ack flags: 0x%02x", *(data + sizeof(guint8)));
    return;
  }

  *cmd = ((GoodixAck *)data)->cmd;
  *has_no_config = ((GoodixAck *)data)->has_no_config;

  if (data_destroy) data_destroy(data);
}
