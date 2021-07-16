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
#include <stdio.h>
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
  GCallback callback;
  gpointer user_data;

  guint8 *data;
  guint16 data_len;
} FpiDeviceGoodixTlsPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(FpiDeviceGoodixTls, fpi_device_goodixtls,
                                    FP_TYPE_IMAGE_DEVICE);

gchar *data_to_string(guint8 *data, gsize data_len) {
  gchar *string = g_malloc((data_len * 2) + 1);

  for (gsize i = 0; i < data_len; i++)
    sprintf(string + i * 2, "%02x", *(data + i));

  return string;
}

// ---- GOODIX RECEIVE SECTION START ----

void goodix_receive_done(FpiSsm *ssm, guint8 cmd) {
  fp_dbg("Completed command: 0x%02x", cmd);

  fpi_ssm_next_state(ssm);
}

void goodix_receive_preset_psk_read_r(FpiSsm *ssm, guint8 *data, gsize data_len,
                                      GDestroyNotify data_destroy,
                                      GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint32 pmk_len;
  gchar *pmk = NULL;

  if (data_len < sizeof(guint8) + sizeof(GoodixPresetPskR)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid PSK read reply length: %ld", data_len);
    goto free;
  }

  if (*data != 0x00) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid PSK read reply flags: 0x%02x", *data);
    goto free;
  }

  fp_dbg(
      "PSK address: 0x%08x",
      GUINT32_FROM_LE(((GoodixPresetPskR *)(data + sizeof(guint8)))->address));

  pmk_len =
      GUINT32_FROM_LE(((GoodixPresetPskR *)(data + sizeof(guint8)))->length);

  if (pmk_len > data_len - sizeof(guint8) - sizeof(GoodixPresetPskR)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid PMK length: %d", pmk_len);
    goto free;
  }

  fp_dbg("PMK length: %d", pmk_len);

  pmk =
      data_to_string(data + sizeof(guint8) + sizeof(GoodixPresetPskR), pmk_len);

  fp_dbg("Device PMK hash: 0x%s", pmk);

  g_free(pmk);

  if (priv->callback)
    if (((GoodixPresetPskReadRCallback)priv->callback)(
            data + sizeof(guint8) + sizeof(GoodixPresetPskR), pmk_len, error,
            priv->user_data))
      goto free;

  goodix_receive_done(ssm, GOODIX_CMD_PRESET_PSK_READ_R);

free:
  if (data_destroy) data_destroy(data);
}

void goodix_receive_ack(FpiSsm *ssm, guint8 *data, gsize data_len,
                        GDestroyNotify data_destroy, GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;
  gboolean has_no_config;

  goodix_decode_ack(data, data_len, data_destroy, &cmd, &has_no_config, error);

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

  if (!priv->callback) goodix_receive_done(ssm, cmd);
}

void goodix_receive_firmware_version(FpiSsm *ssm, guint8 *data, gsize data_len,
                                     GDestroyNotify data_destroy,
                                     GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  gchar *payload = g_malloc(data_len + sizeof(gchar));

  memcpy(payload, data, data_len);
  if (data_destroy) data_destroy(data);

  // Some device send to firmware without the null terminator
  *(payload + data_len) = 0x00;

  fp_dbg("Device firmware: \"%s\"", payload);

  if (priv->callback)
    if (((GoodixFirmwareVersionCallback)priv->callback)(payload, error,
                                                        priv->user_data))
      goto free;

  goodix_receive_done(ssm, GOODIX_CMD_FIRMWARE_VERSION);

free:
  g_free(payload);
}

void goodix_receive_protocol(FpiSsm *ssm, guint8 *data, gsize data_len,
                             GDestroyNotify data_destroy, GError **error) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;
  guint8 *payload = NULL;
  guint16 payload_len, payload_ptr_len;

  payload_ptr_len = goodix_decode_protocol(data, data_len, data_destroy, TRUE,
                                           &cmd, &payload, &payload_len, error);

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
    goodix_receive_ack(ssm, payload, payload_ptr_len, g_free, error);
    return;
  }

  if (priv->cmd != cmd) {
    fp_warn("Invalid protocol command: 0x%02x", cmd);
    goto free;
  }

  if (!priv->callback) {
    fp_warn("Didn't excpect a reply for command: 0x%02x", priv->cmd);
    goto free;
  }

  switch (cmd) {
    case GOODIX_CMD_FIRMWARE_VERSION:
      goodix_receive_firmware_version(ssm, payload, payload_ptr_len, g_free,
                                      error);
      return;

    case GOODIX_CMD_PRESET_PSK_READ_R:
      goodix_receive_preset_psk_read_r(ssm, payload, payload_ptr_len, g_free,
                                       error);
      return;

    default:
      // fp_warn("Unknown command: 0x%02x", cmd);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Unknown command: 0x%02x", cmd);
      goto free;
  }

free:
  g_free(payload);
}

void goodix_receive_pack(FpiSsm *ssm, guint8 *data, gsize data_len,
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

  payload_ptr_len = goodix_decode_pack(priv->data, priv->data_len, NULL, &flags,
                                       &payload, &payload_len, error);

  if (*error) goto clear;

  if (payload_ptr_len < payload_len)
    // Packet is not full, we still need data. Starting to read again.
    goto free;

  switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      goodix_receive_protocol(ssm, payload, payload_ptr_len, NULL, error);
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

  goodix_receive_pack(transfer->ssm, transfer->buffer, transfer->actual_length,
                      NULL, &error);

  if (error) goto failed;

  goodix_receive_data(transfer->ssm);
  return;

failed:
  fpi_ssm_mark_failed(transfer->ssm, error);
}

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

// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

void goodix_send_pack(FpiSsm *ssm, guint8 flags, guint8 *payload,
                      guint16 payload_len, GDestroyNotify payload_destroy) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);
  GError *error = NULL;
  guint8 *data;
  guint32 data_len;

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  data_len = goodix_encode_pack(flags, payload, payload_len, payload_destroy,
                                TRUE, &data);

  // TODO separate in an other function

  for (guint32 i = 0; i < data_len; i += GOODIX_EP_OUT_MAX_BUF_SIZE) {
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

void goodix_send_protocol(FpiSsm *ssm, guint8 cmd, guint8 *payload,
                          guint16 payload_len, GDestroyNotify payload_destroy,
                          gboolean calc_checksum, GCallback callback,
                          gpointer user_data) {
  FpDevice *dev = fpi_ssm_get_device(ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 *data;
  guint32 data_len;

  priv->cmd = cmd;
  priv->callback = callback;
  priv->user_data = user_data;

  data_len = goodix_encode_protocol(cmd, payload, payload_len, payload_destroy,
                                    calc_checksum, FALSE, &data);

  fp_dbg("Running command: 0x%02x", cmd);

  goodix_send_pack(ssm, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len, g_free);
}

void goodix_send_nop(FpiSsm *ssm) {
  GoodixNop payload = {.unknown = 0x00000000};

  goodix_send_protocol(ssm, GOODIX_CMD_NOP, (guint8 *)&payload, sizeof(payload),
                       NULL, FALSE, NULL, NULL);

  goodix_receive_done(ssm, GOODIX_CMD_NOP);
}

void goodix_send_mcu_get_image(FpiSsm *ssm) {
  GoodixDefault payload = {.unused_flags = 0x01};

  goodix_send_protocol(ssm, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_down(
    FpiSsm *ssm, guint8 *mode, guint16 mode_len, GDestroyNotify mode_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data) {
  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, mode_len,
                       mode_destroy, TRUE, G_CALLBACK(callback), user_data);
}

void goodix_send_mcu_switch_to_fdt_up(
    FpiSsm *ssm, guint8 *mode, guint16 mode_len, GDestroyNotify mode_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data) {
  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, mode_len,
                       mode_destroy, TRUE, G_CALLBACK(callback), user_data);
}

void goodix_send_mcu_switch_to_fdt_mode(
    FpiSsm *ssm, guint8 *mode, guint16 mode_len, GDestroyNotify mode_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data) {
  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, mode_len,
                       mode_destroy, TRUE, G_CALLBACK(callback), user_data);
}

void goodix_send_nav_0(FpiSsm *ssm,
                       void (*callback)(guint8 *data, guint16 data_len,
                                        gpointer user_data, GError **error),
                       gpointer user_data) {
  GoodixDefault payload = {.unused_flags = 0x01};

  goodix_send_protocol(ssm, GOODIX_CMD_NAV_0, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, G_CALLBACK(callback),
                       user_data);
}

void goodix_send_mcu_switch_to_idle_mode(FpiSsm *ssm, guint8 sleep_time) {
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};

  goodix_send_protocol(ssm, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, NULL,
                       NULL);
}

void goodix_send_write_sensor_register(FpiSsm *ssm, guint16 address,
                                       guint16 value) {
  // Only support one address and one value

  GoodixWriteSensorRegister payload = {.multiples = FALSE,
                                       .address = GUINT16_TO_LE(address),
                                       .value = GUINT16_TO_LE(value)};

  goodix_send_protocol(ssm, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, NULL,
                       NULL);
}

void goodix_send_read_sensor_register(
    FpiSsm *ssm, guint16 address, guint8 length,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data) {
  // Only support one address

  GoodixReadSensorRegister payload = {
      .multiples = FALSE, .address = GUINT16_TO_LE(address), .length = length};

  goodix_send_protocol(ssm, GOODIX_CMD_READ_SENSOR_REGISTER, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, G_CALLBACK(callback),
                       user_data);
}

void goodix_send_upload_config_mcu(
    FpiSsm *ssm, guint8 *config, guint16 config_len,
    GDestroyNotify config_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data) {
  goodix_send_protocol(ssm, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, config_len,
                       config_destroy, TRUE, G_CALLBACK(callback), user_data);
}

void goodix_send_set_powerdown_scan_frequency(
    FpiSsm *ssm, guint16 powerdown_scan_frequency,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data) {
  GoodixSetPowerdownScanFrequency payload = {
      .powerdown_scan_frequency = GUINT16_TO_LE(powerdown_scan_frequency)};

  goodix_send_protocol(ssm, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       G_CALLBACK(callback), user_data);
}

void goodix_send_enable_chip(FpiSsm *ssm, gboolean enable) {
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};

  goodix_send_protocol(ssm, GOODIX_CMD_ENABLE_CHIP, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, NULL, NULL);
}

void goodix_send_reset(FpiSsm *ssm, gboolean reset_sensor, guint8 sleep_time,
                       void (*callback)(guint8 *data, guint16 data_len,
                                        gpointer user_data, GError **error),
                       gpointer user_data) {
  // Only support reset sensor

  GoodixReset payload = {.soft_reset_mcu = FALSE,
                         .reset_sensor = reset_sensor ? TRUE : FALSE,
                         .sleep_time = sleep_time};

  goodix_send_protocol(ssm, GOODIX_CMD_RESET, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, G_CALLBACK(callback),
                       user_data);
}

void goodix_send_firmware_version(FpiSsm *ssm,
                                  GoodixFirmwareVersionCallback callback,
                                  gpointer user_data) {
  GoodixNone payload = {};

  goodix_send_protocol(ssm, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, G_CALLBACK(callback),
                       user_data);
}

void goodix_send_query_mcu_state(FpiSsm *ssm,
                                 void (*callback)(guint8 *data,
                                                  guint16 data_len,
                                                  gpointer user_data,
                                                  GError **error),
                                 gpointer user_data) {
  GoodixQueryMcuState payload = {.unused_flags = 0x55};

  goodix_send_protocol(ssm, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, G_CALLBACK(callback),
                       user_data);
}

void goodix_send_request_tls_connection(FpiSsm *ssm) {
  GoodixNone payload = {};

  goodix_send_protocol(ssm, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, NULL,
                       NULL);
}

void goodix_send_tls_successfully_established(FpiSsm *ssm) {
  GoodixNone payload = {};

  goodix_send_protocol(ssm, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, NULL,
                       NULL);
}

void goodix_send_preset_psk_write_r(FpiSsm *ssm, guint32 address, guint8 *psk,
                                    guint32 psk_len, GDestroyNotify psk_destroy,
                                    void (*callback)(guint8 *data,
                                                     guint16 data_len,
                                                     gpointer user_data,
                                                     GError **error),
                                    gpointer user_data) {
  // Only support one address, one payload and one length

  guint8 *payload = g_malloc(sizeof(payload) + psk_len);

  ((GoodixPresetPskR *)payload)->address = GUINT32_TO_LE(address);
  ((GoodixPresetPskR *)payload)->length = GUINT32_TO_LE(psk_len);
  memcpy(payload + sizeof(payload), psk, psk_len);
  if (psk_destroy) psk_destroy(psk);

  goodix_send_protocol(ssm, GOODIX_CMD_PRESET_PSK_WRITE_R, payload,
                       sizeof(payload) + psk_len, g_free, TRUE,
                       G_CALLBACK(callback), user_data);
}

void goodix_send_preset_psk_read_r(FpiSsm *ssm, guint32 address, guint32 length,
                                   GoodixPresetPskReadRCallback callback,
                                   gpointer user_data) {
  GoodixPresetPskR payload = {.address = GUINT32_TO_LE(address),
                              .length = GUINT32_TO_LE(length)};

  goodix_send_protocol(ssm, GOODIX_CMD_PRESET_PSK_READ_R, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, G_CALLBACK(callback),
                       user_data);
}

// ---- GOODIX SEND SECTION END ----

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
