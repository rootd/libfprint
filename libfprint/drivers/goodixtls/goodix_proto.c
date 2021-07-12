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
  struct _pack {
    guint8 flags;
    guint16 length;
    guint8 checksum;
  } __attribute__((__packed__)) pack = {
      .flags = flags,
      .length = GUINT16_TO_LE(payload_len),
      .checksum =
          goodix_calc_checksum(&flags, sizeof(flags)) +
          goodix_calc_checksum((guint8 *)&payload_len, sizeof(payload_len))};
  gsize data_ptr_len = sizeof(pack) + payload_len;

  if (pad_data && data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    data_ptr_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(data_ptr_len);

  memcpy(*data, &pack, sizeof(pack));
  memcpy(*data + sizeof(pack), payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  return data_ptr_len;
}

gsize goodix_encode_protocol(guint8 **data, gboolean pad_data, guint8 cmd,
                             gboolean calc_checksum, guint8 *payload,
                             guint16 payload_len,
                             GDestroyNotify payload_destroy) {
  struct _protocol {
    guint8 cmd;
    guint16 length;
  } __attribute__((__packed__)) protocol = {
      .cmd = cmd, .length = GUINT16_TO_LE(payload_len) + sizeof(guint8)};
  gsize data_ptr_len = sizeof(protocol) + payload_len + sizeof(guint8);

  if (pad_data && data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    data_ptr_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - data_ptr_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(data_ptr_len);

  memcpy(*data, &protocol, sizeof(protocol));
  memcpy(*data + sizeof(protocol), payload, payload_len);
  if (payload_destroy) payload_destroy(payload);

  if (calc_checksum)
    *(*data + sizeof(protocol) + payload_len) =
        0xaa - goodix_calc_checksum(*data, sizeof(protocol) + payload_len);
  else
    *(*data + sizeof(protocol) + payload_len) = 0x88;

  return data_ptr_len;
}

guint16 goodix_decode_pack(guint8 *flags, guint8 **payload,
                           guint16 *payload_len, guint8 *data, gsize data_len,
                           GDestroyNotify data_destroy, GError **error) {
  struct _pack {
    guint8 flags;
    guint16 length;
    guint8 checksum;  // TODO Remove checksum from pack
  } __attribute__((__packed__)) pack;
  guint8 checksum;
  guint16 payload_ptr_len = data_len - sizeof(pack);

  if (data_len < sizeof(pack)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack length: %ld", data_len);
    return 0;
  }

  memcpy(&pack, data, sizeof(pack));
  pack.length = GUINT16_FROM_LE(pack.length);

  if (payload_ptr_len >= pack.length) payload_ptr_len = pack.length;

  checksum = goodix_calc_checksum(data, sizeof(pack) - sizeof(guint8));
  if (checksum != pack.checksum) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message pack checksum: 0x%02x", checksum);
    return 0;
  }

  *payload = g_memdup(data + sizeof(pack), payload_ptr_len);
  if (data_destroy) data_destroy(data);

  *flags = pack.flags;
  *payload_len = pack.length;

  return payload_ptr_len;
}

guint16 goodix_decode_protocol(guint8 *cmd, guint8 **payload,
                               guint16 *payload_len, gboolean calc_checksum,
                               guint8 *data, gsize data_len,
                               GDestroyNotify data_destroy, GError **error) {
  struct _protocol {
    guint8 cmd;
    guint16 length;
  } __attribute__((__packed__)) protocol;
  guint8 checksum;
  guint16 payload_ptr_len = data_len - sizeof(protocol) - sizeof(guint8);

  if (data_len - sizeof(guint8) < sizeof(protocol)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %ld",
                data_len - sizeof(guint8));
    return 0;
  }

  memcpy(&protocol, data, sizeof(protocol));
  protocol.length = GUINT16_FROM_LE(protocol.length) - sizeof(guint8);

  if (payload_ptr_len >= protocol.length) {
    payload_ptr_len = protocol.length;

    if (calc_checksum) {
      checksum =
          0xaa - goodix_calc_checksum(data, sizeof(protocol) + protocol.length);
      if (checksum != *(data + sizeof(protocol) + protocol.length)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Invalid message protocol checksum: 0x%02x", checksum);
        return 0;
      }
    } else if (0x88 != *(data + sizeof(protocol) + protocol.length)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Invalid message protocol checksum: 0x88");
      return 0;
    }
  }

  *payload = g_memdup(data + sizeof(protocol), payload_ptr_len);
  if (data_destroy) data_destroy(data);

  *cmd = protocol.cmd;
  *payload_len = protocol.length;

  return payload_ptr_len;
}

void goodix_decode_ack(guint8 *cmd, gboolean *has_no_config, guint8 *data,
                       guint16 data_len, GDestroyNotify data_destroy,
                       GError **error) {
  struct _ack {
    guint8 cmd;
    guint8 always_true : 1;
    guint8 has_no_config : 1;
    guint8 : 6;
  } __attribute__((__packed__)) ack;

  if (data_len != sizeof(ack)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid ack length: %d", data_len);
    return;
  }

  memcpy(&ack, data, sizeof(ack));
  if (data_destroy) data_destroy(data);

  if (!ack.always_true) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Invalid ack flags");
    return;
  }

  *cmd = ack.cmd;
  *has_no_config = ack.has_no_config;
}
