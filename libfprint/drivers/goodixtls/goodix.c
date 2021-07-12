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

#define FP_COMPONENT "goodixtls"

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"

typedef struct {
  pthread_t tls_server_thread;
  gint tls_server_sock;
  SSL_CTX *tls_server_ctx;

  guint8 cmd;
  gboolean reply;

  guint8 *data;
  guint16 data_len;
} FpiDeviceGoodixTlsPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(FpiDeviceGoodixTls, fpi_device_goodixtls,
                                    FP_TYPE_IMAGE_DEVICE);

// ---- GOODIX SECTION START ----

void goodix_receive_data(FpiSsm *ssm) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

  transfer->ssm = ssm;
  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk(transfer, class->ep_in, GOODIX_EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit(transfer, 0, NULL, goodix_receive_data_cb, NULL);
}

void goodix_cmd_done(FpiSsm *ssm, guint8 cmd) {
  fp_dbg("Completed command: 0x%02x", cmd);

  fpi_ssm_next_state(ssm);
}

void goodix_ack_handle(FpiSsm *ssm, guint8 *data, gsize data_len,
                       GDestroyNotify data_destroy, GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;
  gboolean has_no_config;

  goodix_decode_ack(&cmd, &has_no_config, data, data_len, data_destroy, error);

  if (*error) return;

  if (has_no_config) fp_warn("MCU has no config");

  if (cmd == GOODIX_CMD_NOP) {
    fp_warn("Received nop ack");
    return;
  }

  if (priv->cmd != cmd) {
    fp_warn("Invalid ack command: 0x%02x", cmd);
    return;
  }

  if (!priv->reply) goodix_cmd_done(ssm, cmd);
}

void goodix_protocol_handle(FpiSsm *ssm, guint8 *data, gsize data_len,
                            GDestroyNotify data_destroy, GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;
  guint8 *payload = NULL;
  guint16 payload_len, payload_ptr_len;

  payload_ptr_len = goodix_decode_protocol(&cmd, &payload, &payload_len, TRUE,
                                           data, data_len, data_destroy, error);

  if (*error) goto free;

  if (payload_ptr_len < payload_len) {
    // Command is not full but packet is since we checked that before.
    // This means that something when wrong. This should never happen.
    // Raising an error.
    // TODO implement reassembling for messages protocol beacause some devices
    // don't use messages packets.
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %d", payload_ptr_len);
    goto free;
  }

  if (cmd == GOODIX_CMD_ACK) {
    goodix_ack_handle(ssm, payload, payload_ptr_len, NULL, error);
    goto free;
  }

  if (priv->cmd != cmd) {
    fp_warn("Invalid protocol command: 0x%02x", cmd);
    goto free;
  }

  if (!priv->reply) {
    fp_warn("Didn't excpect a reply for command: 0x%02x", priv->cmd);
    goto free;
  }

  switch (cmd) {
    case GOODIX_CMD_FIRMWARE_VERSION:
      // Some device send to firmware without the null terminator
      payload = g_realloc(payload, payload_ptr_len + sizeof(guint8));
      *(payload + payload_ptr_len) = 0x00;

      if (strcmp((gchar *)payload, class->firmware_version)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Invalid device firmware: \"%s\"", payload);
        goto free;
      }

      fp_dbg("Device firmware: \"%s\"", payload);

      goto done;

    case GOODIX_CMD_PRESET_PSK_READ_R:

      if (payload_ptr_len < 9) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Failed to read PMK hash");
        goto free;
      }

      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Failed to read PMK hash");
      goto free;

      goto done;

    default:
      // fp_warn("Unknown command: 0x%02x", cmd);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Unknown command: 0x%02x", cmd);
      goto free;
  }

done:
  goodix_cmd_done(ssm, cmd);

free:
  g_free(payload);
}

void goodix_pack_handle(FpiSsm *ssm, guint8 *data, gsize data_len,
                        GDestroyNotify data_destroy, GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 flags;
  guint8 *payload = NULL;
  guint16 payload_len, payload_ptr_len;

  priv->data = g_realloc(priv->data, priv->data_len + data_len);
  memcpy(priv->data + priv->data_len, data, data_len);
  if (data_destroy) data_destroy(data);
  priv->data_len += data_len;

  payload_ptr_len = goodix_decode_pack(&flags, &payload, &payload_len,
                                       priv->data, priv->data_len, NULL, error);

  if (*error) goto clear;

  if (payload_ptr_len < payload_len)
    // Packet is not full, we still need data. Starting to read again.
    goto free;

  switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      goodix_protocol_handle(ssm, payload, payload_ptr_len, NULL, error);
      goto clear;

    case GOODIX_FLAGS_TLS:
      // TLS message sending it to TLS server.
      // TODO
      goto clear;

    default:
      fp_warn("Unknown flags: 0x%02x", flags);
      goto clear;
  }

clear:
  g_clear_pointer(&priv->data, g_free);
  priv->data_len = 0;

free:
  g_free(payload);
}

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error) {
  if (error) goto failed;

  goodix_pack_handle(transfer->ssm, transfer->buffer, transfer->actual_length,
                     NULL, &error);

  if (error) goto failed;

  goodix_receive_data(transfer->ssm);
  return;

failed:
  fpi_ssm_mark_failed(transfer->ssm, error);
}

void goodix_send_pack(FpiSsm *ssm, guint8 flags, guint8 *payload,
                      guint16 payload_len, GDestroyNotify payload_destroy) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);
  GError *error = NULL;
  guint8 *data;
  gsize data_len;

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  data_len = goodix_encode_pack(&data, TRUE, flags, payload, payload_len,
                                payload_destroy);

  for (gsize i = 0; i < data_len; i += GOODIX_EP_OUT_MAX_BUF_SIZE) {
    fpi_usb_transfer_fill_bulk_full(transfer, class->ep_out, data + i,
                                    GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

    if (!fpi_usb_transfer_submit_sync(transfer, GOODIX_TIMEOUT, &error))
      goto failed;
  }

  goto free;

failed:
  fpi_ssm_mark_failed(ssm, error);

free:
  fpi_usb_transfer_unref(transfer);
  g_free(data);
}

void goodix_send_protocol(FpiSsm *ssm, guint8 cmd, gboolean calc_checksum,
                          gboolean reply, guint8 *payload, guint16 payload_len,
                          GDestroyNotify payload_destroy) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 *data;
  gsize data_len;

  priv->reply = reply;
  priv->cmd = cmd;

  fp_dbg("Running command: 0x%02x", cmd);

  data_len = goodix_encode_protocol(&data, FALSE, cmd, calc_checksum, payload,
                                    payload_len, payload_destroy);

  goodix_send_pack(ssm, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len, g_free);
}

void goodix_cmd_nop(FpiSsm *ssm) {
  struct _payload {
    guint32 unknown;
  } __attribute__((__packed__)) payload = {.unknown = 0x00000000};

  goodix_send_protocol(ssm, GOODIX_CMD_NOP, FALSE, FALSE, (guint8 *)&payload,
                       sizeof(payload), NULL);

  goodix_cmd_done(ssm, GOODIX_CMD_NOP);
}

void goodix_cmd_mcu_get_image(FpiSsm *ssm) {
  struct _payload {
    guint8 unused_flags;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.unused_flags = 0x01};

  goodix_send_protocol(ssm, GOODIX_CMD_MCU_GET_IMAGE, TRUE, FALSE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_mcu_switch_to_fdt_down(FpiSsm *ssm, guint8 *mode,
                                       guint16 mode_len,
                                       GDestroyNotify mode_destroy) {
  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, TRUE, TRUE, mode,
                       mode_len, mode_destroy);
}

void goodix_cmd_mcu_switch_to_fdt_up(FpiSsm *ssm, guint8 *mode,
                                     guint16 mode_len,
                                     GDestroyNotify mode_destroy) {
  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, TRUE, TRUE, mode,
                       mode_len, mode_destroy);
}

void goodix_cmd_mcu_switch_to_fdt_mode(FpiSsm *ssm, guint8 *mode,
                                       guint16 mode_len,
                                       GDestroyNotify mode_destroy) {
  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, TRUE, TRUE, mode,
                       mode_len, mode_destroy);
}

void goodix_cmd_nav_0(FpiSsm *ssm) {
  struct _payload {
    guint8 unused_flags;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.unused_flags = 0x01};

  goodix_send_protocol(ssm, GOODIX_CMD_NAV_0, TRUE, TRUE, (guint8 *)&payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_mcu_switch_to_idle_mode(FpiSsm *ssm, guint8 sleep_time) {
  struct _payload {
    guint8 sleep_time;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.sleep_time = sleep_time};

  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE, TRUE, FALSE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_write_sensor_register(FpiSsm *ssm, guint16 address,
                                      guint16 value) {
  // Only support one address and one value

  struct _payload {
    guint8 multiples;
    guint16 address;
    guint16 value;
  } __attribute__((__packed__)) payload = {.multiples = FALSE,
                                           .address = GUINT16_TO_LE(address),
                                           .value = GUINT16_TO_LE(value)};

  goodix_send_protocol(ssm, GOODIX_CMD_WRITE_SENSOR_REGISTER, TRUE, FALSE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_read_sensor_register(FpiSsm *ssm, guint16 address,
                                     guint8 length) {
  // Only support one address

  struct _payload {
    guint8 multiples;
    guint16 address;
    guint8 length;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {
      .multiples = FALSE, .address = GUINT16_TO_LE(address), .length = length};

  goodix_send_protocol(ssm, GOODIX_CMD_READ_SENSOR_REGISTER, TRUE, TRUE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_upload_config_mcu(FpiSsm *ssm, guint8 *config,
                                  guint16 config_len,
                                  GDestroyNotify config_destroy) {
  goodix_send_protocol(ssm, GOODIX_CMD_UPLOAD_CONFIG_MCU, TRUE, TRUE, config,
                       config_len, config_destroy);
}

void goodix_cmd_set_powerdown_scan_frequency(FpiSsm *ssm,
                                             guint16 powerdown_scan_frequency) {
  struct _payload {
    guint16 powerdown_scan_frequency;
  } __attribute__((__packed__)) payload = {
      .powerdown_scan_frequency = GUINT16_TO_LE(powerdown_scan_frequency)};

  goodix_send_protocol(ssm, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY, TRUE, TRUE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_enable_chip(FpiSsm *ssm, gboolean enable) {
  struct _payload {
    guint8 enable;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.enable = enable ? TRUE : FALSE};

  goodix_send_protocol(ssm, GOODIX_CMD_ENABLE_CHIP, TRUE, FALSE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_reset(FpiSsm *ssm, gboolean reset_sensor,
                      gboolean soft_reset_mcu, guint8 sleep_time) {
  struct _payload {
    guint8 reset_sensor : 1;
    guint8 soft_reset_mcu : 1;
    guint8 : 6;
    guint8 sleep_time;
  } __attribute__((__packed__))
  payload = {.soft_reset_mcu = soft_reset_mcu ? TRUE : FALSE,
             .reset_sensor = reset_sensor ? TRUE : FALSE,
             .sleep_time = sleep_time};

  goodix_send_protocol(ssm, GOODIX_CMD_RESET, TRUE,
                       soft_reset_mcu ? FALSE : TRUE, (guint8 *)&payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_firmware_version(FpiSsm *ssm) {
  struct _payload {
    guint16 : 16;
  } __attribute__((__packed__)) payload = {};

  goodix_send_protocol(ssm, GOODIX_CMD_FIRMWARE_VERSION, TRUE, TRUE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_query_mcu_state(FpiSsm *ssm) {
  struct _payload {
    guint8 unused_flags;
  } __attribute__((__packed__)) payload = {.unused_flags = 0x55};

  goodix_send_protocol(ssm, GOODIX_CMD_QUERY_MCU_STATE, TRUE, TRUE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_request_tls_connection(FpiSsm *ssm) {
  struct _payload {
    guint16 : 16;
  } __attribute__((__packed__)) payload = {};

  goodix_send_protocol(ssm, GOODIX_CMD_REQUEST_TLS_CONNECTION, TRUE, FALSE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_tls_successfully_established(FpiSsm *ssm) {
  struct _payload {
    guint16 : 16;
  } __attribute__((__packed__)) payload = {};

  goodix_send_protocol(ssm, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED, TRUE,
                       FALSE, (guint8 *)&payload, sizeof(payload), NULL);
}

void goodix_cmd_preset_psk_write_r(FpiSsm *ssm, guint32 address, guint8 *psk,
                                   guint32 psk_len,
                                   GDestroyNotify psk_destroy) {
  // Only support one address, one payload and one length

  struct _payload {
    guint32 address;
    guint32 length;
  } __attribute__((__packed__)) payload = {.address = GUINT32_TO_LE(address),
                                           .length = GUINT32_TO_LE(psk_len)};
  guint8 *payload_ptr = g_malloc(sizeof(payload) + psk_len);

  memcpy(payload_ptr, &payload, sizeof(payload));
  memcpy(payload_ptr + sizeof(payload), psk, psk_len);
  if (psk_destroy) psk_destroy(psk);

  goodix_send_protocol(ssm, GOODIX_CMD_PRESET_PSK_WRITE_R, TRUE, TRUE,
                       payload_ptr, sizeof(payload) + psk_len, g_free);
}

void goodix_cmd_preset_psk_read_r(FpiSsm *ssm, guint32 address,
                                  guint32 length) {
  struct _payload {
    guint32 address;
    guint32 length;
  } __attribute__((__packed__)) payload = {.address = GUINT32_TO_LE(address),
                                           .length = GUINT32_TO_LE(length)};

  goodix_send_protocol(ssm, GOODIX_CMD_PRESET_PSK_READ_R, TRUE, TRUE,
                       (guint8 *)&payload, sizeof(payload), NULL);
}

// ---- GOODIX SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  priv->data = NULL;
  priv->data_len = 0;

  return g_usb_device_claim_interface(fpi_device_get_usb_device(dev),
                                      class->interface, 0, error);
}

gboolean goodix_dev_deinit(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  g_free(priv->data);
  priv->data_len = 0;

  return g_usb_device_release_interface(fpi_device_get_usb_device(dev),
                                        class->interface, 0, error);
}

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

enum tls_states {
  TLS_SERVER_INIT,
  TLS_SERVER_HANDSHAKE_INIT,
  TLS_NUM_STATES,
};

void goodix_tls_run_state(FpiSsm *ssm, FpDevice *dev) {
  switch (fpi_ssm_get_cur_state(ssm)) {
    case TLS_SERVER_INIT:
      tls_server_init(ssm);
      break;

    case TLS_SERVER_HANDSHAKE_INIT:
      tls_server_handshake_init();
      break;
  }
}

void goodix_tls_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}

void goodix_tls(FpDevice *dev) {
  fpi_ssm_start(fpi_ssm_new(dev, goodix_tls_run_state, TLS_NUM_STATES),
                goodix_tls_complete);
}

// ---- TLS SECTION END ----

static void fpi_device_goodixtls_init(FpiDeviceGoodixTls *self) {}

static void fpi_device_goodixtls_class_init(FpiDeviceGoodixTlsClass *class) {}
