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

  gboolean ack;
  gboolean reply;

  GoodixCmdCallback callback;
  gpointer user_data;

  guint8 *data;
  guint32 length;
} FpiDeviceGoodixTlsPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(FpiDeviceGoodixTls, fpi_device_goodixtls,
                                    FP_TYPE_IMAGE_DEVICE);

// TODO remove every GDestroyNotify
// TODO add cmd timeouts

gchar *data_to_str(guint8 *data, guint32 length) {
  gchar *string = g_malloc((length * 2) + 1);

  for (guint32 i = 0; i < length; i++) sprintf(string + i * 2, "%02x", data[i]);

  return string;
}

// ---- GOODIX RECEIVE SECTION START ----

void goodix_receive_done(FpDevice *dev, guint8 *data, guint16 length,
                         GError *error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  GoodixCmdCallback callback = priv->callback;
  gpointer user_data = priv->user_data;

  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;

  if (!error) fp_dbg("Completed command: 0x%02x", priv->cmd);

  if (callback) callback(dev, data, length, user_data, error);
}

void goodix_receive_none(FpDevice *dev, guint8 *data, guint16 length,
                         gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixNoneCallback callback = (GoodixNoneCallback)cb_info->callback;

  callback(dev, cb_info->user_data, error);
}

void goodix_receive_default(FpDevice *dev, guint8 *data, guint16 length,
                            gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixDefaultCallback callback = (GoodixDefaultCallback)cb_info->callback;

  callback(dev, data, length, cb_info->user_data, error);
}

void goodix_receive_success(FpDevice *dev, guint8 *data, guint16 length,
                            gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, cb_info->user_data, error);
    return;
  }

  if (length != sizeof(guint8) * 2) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid success reply length: %d", length);
    callback(dev, FALSE, cb_info->user_data, error);
    return;
  }

  callback(dev, data[0] == 0x00 ? FALSE : TRUE, cb_info->user_data, NULL);
}

void goodix_receive_reset(FpDevice *dev, guint8 *data, guint16 length,
                          gpointer user_data, GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixResetCallback callback = (GoodixResetCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, 0, cb_info->user_data, error);
    return;
  }

  if (length != sizeof(guint8) + sizeof(guint16)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid reset reply length: %d", length);
    callback(dev, FALSE, 0, cb_info->user_data, error);
    return;
  }

  callback(dev, data[0] == 0x00 ? FALSE : TRUE,
           GUINT16_FROM_LE(*(guint16 *)(data + sizeof(guint8))),  // TODO
           cb_info->user_data, NULL);
}

void goodix_receive_preset_psk_read_r(FpDevice *dev, guint8 *data,
                                      guint16 length, gpointer user_data,
                                      GError *error) {
  guint32 psk_r_len;
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixPresetPskReadRCallback callback =
      (GoodixPresetPskReadRCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  if (length < sizeof(guint8)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read R reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  if (data[0] != 0x00) {
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, NULL);
    return;
  }

  if (length < sizeof(guint8) + sizeof(GoodixPresetPskR)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read R reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  psk_r_len =
      GUINT32_FROM_LE(((GoodixPresetPskR *)(data + sizeof(guint8)))->length);

  if (length < psk_r_len + sizeof(guint8) + sizeof(GoodixPresetPskR)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read R reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  callback(
      dev, TRUE,
      GUINT32_FROM_LE(((GoodixPresetPskR *)(data + sizeof(guint8)))->address),
      data + sizeof(guint8) + sizeof(GoodixPresetPskR), psk_r_len,
      cb_info->user_data, NULL);
}

void goodix_receive_preset_psk_write_r(FpDevice *dev, guint8 *data,
                                       guint16 length, gpointer user_data,
                                       GError *error) {
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, cb_info->user_data, error);
    return;
  }

  if (length < sizeof(guint8)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK write R reply length: %d", length);
    callback(dev, FALSE, cb_info->user_data, error);
    return;
  }

  callback(dev, data[0] == 0x00 ? TRUE : FALSE, cb_info->user_data, NULL);
}

void goodix_receive_firmware_version(FpDevice *dev, guint8 *data,
                                     guint16 length, gpointer user_data,
                                     GError *error) {
  g_autofree gchar *payload = g_malloc(length + sizeof(gchar));
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixFirmwareVersionCallback callback =
      (GoodixFirmwareVersionCallback)cb_info->callback;

  if (error) {
    callback(dev, NULL, cb_info->user_data, error);
    return;
  }

  memcpy(payload, data, length);

  // Some device send the firmware without the null terminator
  payload[length] = 0x00;

  callback(dev, payload, cb_info->user_data, NULL);
}

void goodix_receive_ack(FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;

  if (length != sizeof(GoodixAck)) {
    fp_warn("Invalid ACK length: %d", length);
    return;
  }

  if (!((GoodixAck *)data)->always_true) {
    // Warn about error.
    fp_warn("Invalid ACK flags: 0x%02x", data[sizeof(guint8)]);
    return;
  }

  cmd = ((GoodixAck *)data)->cmd;

  if (((GoodixAck *)data)->has_no_config) fp_warn("MCU has no config");

  if (priv->cmd != cmd) {
    fp_warn("Invalid ACK command: 0x%02x", cmd);
    return;
  }

  if (!priv->ack) {
    fp_warn("Didn't excpect an ACK for command: 0x%02x", priv->cmd);
    return;
  }

  if (!priv->reply) {
    goodix_receive_done(dev, NULL, 0, NULL);
    return;
  }

  priv->ack = FALSE;
}

void goodix_receive_protocol(FpDevice *dev, guint8 *data, guint32 length) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 cmd;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum, valid_null_checksum;  // TODO implement checksum.

  if (!goodix_decode_protocol(data, length, &cmd, &payload, &payload_len,
                              &valid_checksum, &valid_null_checksum))
    // Protocol is not full, we still need data.
    // TODO implement protocol assembling.
    return;

  if (cmd == GOODIX_CMD_ACK) {
    goodix_receive_ack(dev, payload, payload_len, NULL, NULL);
    return;
  }

  if (priv->cmd != cmd) {
    fp_warn("Invalid protocol command: 0x%02x", cmd);
    return;
  }

  if (!priv->reply) {
    fp_warn("Didn't excpect a reply for command: 0x%02x", priv->cmd);
    return;
  }

  if (priv->ack) fp_warn("Didn't got ACK for command: 0x%02x", priv->cmd);

  goodix_receive_done(dev, payload, payload_len, NULL);
}

void goodix_receive_pack(FpDevice *dev, guint8 *data, guint32 length) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 flags;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum;  // TODO implement checksum.

  priv->data = g_realloc(priv->data, priv->length + length);
  memcpy(priv->data + priv->length, data, length);
  priv->length += length;

  if (!goodix_decode_pack(priv->data, priv->length, &flags, &payload,
                          &payload_len, &valid_checksum))
    // Packet is not full, we still need data.
    return;

  switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      goodix_receive_protocol(dev, payload, payload_len);
      break;

    case GOODIX_FLAGS_TLS:
      // TLS message sending it to TLS server.
      // TODO
      break;

    default:
      fp_warn("Unknown flags: 0x%02x", flags);
      break;
  }

  g_clear_pointer(&priv->data, g_free);
  priv->length = 0;
}

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error) {
  if (error) {
    // Warn about error and free it.
    fp_warn("Receive data error: %s", error->message);
    g_error_free(error);

    // Retry receiving data and return.
    goodix_receive_data(dev);
    return;
  }

  goodix_receive_pack(dev, transfer->buffer, transfer->actual_length);

  goodix_receive_data(dev);
}

void goodix_receive_data(FpDevice *dev) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk(transfer, class->ep_in, GOODIX_EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit(transfer, 0, NULL, goodix_receive_data_cb, NULL);
}

// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

gboolean goodix_send_data(FpDevice *dev, guint8 *data, guint32 length,
                          GDestroyNotify free_func, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

  transfer->short_is_error = TRUE;

  for (guint32 i = 0; i < length; i += GOODIX_EP_OUT_MAX_BUF_SIZE) {
    fpi_usb_transfer_fill_bulk_full(transfer, class->ep_out, data + i,
                                    GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

    if (!fpi_usb_transfer_submit_sync(transfer, GOODIX_TIMEOUT, error)) {
      if (free_func) free_func(data);
      fpi_usb_transfer_unref(transfer);
      return FALSE;
    }
  }

  if (free_func) free_func(data);
  fpi_usb_transfer_unref(transfer);
  return TRUE;
}

gboolean goodix_send_pack(FpDevice *dev, guint8 flags, guint8 *payload,
                          guint16 length, GDestroyNotify free_func,
                          GError **error) {
  guint8 *data;
  guint32 data_len;

  goodix_encode_pack(flags, payload, length, TRUE, &data, &data_len);
  if (free_func) free_func(payload);

  return goodix_send_data(dev, data, data_len, g_free, error);
}

void goodix_send_protocol(FpDevice *dev, guint8 cmd, guint8 *payload,
                          guint16 length, GDestroyNotify free_func,
                          gboolean calc_checksum, gboolean reply,
                          GoodixCmdCallback callback, gpointer user_data) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  GError *error = NULL;
  guint8 *data;
  guint32 data_len;

  if (priv->ack || priv->reply) {
    // A command is already running.
    fp_warn("A command is already running: 0x%02x", priv->cmd);
    if (free_func) free_func(payload);
    return;
  }

  fp_dbg("Running command: 0x%02x", cmd);

  priv->cmd = cmd;
  priv->ack = TRUE;
  priv->reply = reply;
  priv->callback = callback;
  priv->user_data = user_data;

  goodix_encode_protocol(cmd, payload, length, calc_checksum, FALSE, &data,
                         &data_len);
  if (free_func) free_func(payload);

  if (!goodix_send_pack(dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len, g_free,
                        &error)) {
    goodix_receive_done(dev, NULL, 0, error);
    return;
  };
}

void goodix_send_nop(FpDevice *dev, GoodixNoneCallback callback,
                     gpointer user_data) {
  GoodixNop payload = {.unknown = 0x00000000};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_NOP, (guint8 *)&payload,
                         sizeof(payload), NULL, FALSE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_NOP, (guint8 *)&payload, sizeof(payload),
                       NULL, FALSE, FALSE, NULL, NULL);
}

void goodix_send_mcu_get_image(FpDevice *dev, GoodixNoneCallback callback,
                               gpointer user_data) {
  GoodixDefault payload = {.unused_flags = 0x01};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, FALSE, NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_down(FpDevice *dev, guint8 *mode,
                                        guint16 length,
                                        GDestroyNotify free_func,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data) {
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                         free_func, TRUE, TRUE, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                       free_func, TRUE, TRUE, NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_up(FpDevice *dev, guint8 *mode,
                                      guint16 length, GDestroyNotify free_func,
                                      GoodixDefaultCallback callback,
                                      gpointer user_data) {
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                         free_func, TRUE, TRUE, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                       free_func, TRUE, TRUE, NULL, NULL);
}

void goodix_send_mcu_switch_to_fdt_mode(FpDevice *dev, guint8 *mode,
                                        guint16 length,
                                        GDestroyNotify free_func,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data) {
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                         free_func, TRUE, TRUE, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                       free_func, TRUE, TRUE, NULL, NULL);
}

void goodix_send_nav_0(FpDevice *dev, GoodixDefaultCallback callback,
                       gpointer user_data) {
  GoodixDefault payload = {.unused_flags = 0x01};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_NAV_0, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_NAV_0, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, TRUE, NULL, NULL);
}

void goodix_send_mcu_switch_to_idle_mode(FpDevice *dev, guint8 sleep_time,
                                         GoodixNoneCallback callback,
                                         gpointer user_data) {
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                       NULL, NULL);
}

void goodix_send_write_sensor_register(FpDevice *dev, guint16 address,
                                       guint16 value,
                                       GoodixNoneCallback callback,
                                       gpointer user_data) {
  // Only support one address and one value

  GoodixWriteSensorRegister payload = {.multiples = FALSE,
                                       .address = GUINT16_TO_LE(address),
                                       .value = GUINT16_TO_LE(value)};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                       NULL, NULL);
}

void goodix_send_read_sensor_register(FpDevice *dev, guint16 address,
                                      guint8 length,
                                      GoodixDefaultCallback callback,
                                      gpointer user_data) {
  // Only support one address

  GoodixReadSensorRegister payload = {
      .multiples = FALSE, .address = GUINT16_TO_LE(address), .length = length};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_READ_SENSOR_REGISTER,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_READ_SENSOR_REGISTER, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, TRUE, NULL, NULL);
}

void goodix_send_upload_config_mcu(FpDevice *dev, guint8 *config,
                                   guint16 length, GDestroyNotify free_func,
                                   GoodixSuccessCallback callback,
                                   gpointer user_data) {
  GoodixCallbackInfo *cb_info;
  ;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                         free_func, TRUE, TRUE, goodix_receive_success,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                       free_func, TRUE, TRUE, NULL, NULL);
}

void goodix_send_set_powerdown_scan_frequency(FpDevice *dev,
                                              guint16 powerdown_scan_frequency,
                                              GoodixSuccessCallback callback,
                                              gpointer user_data) {
  GoodixSetPowerdownScanFrequency payload = {
      .powerdown_scan_frequency = GUINT16_TO_LE(powerdown_scan_frequency)};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_success, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, TRUE,
                       NULL, NULL);
}

void goodix_send_enable_chip(FpDevice *dev, gboolean enable,
                             GoodixNoneCallback callback, gpointer user_data) {
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, FALSE, NULL, NULL);
}

void goodix_send_reset(FpDevice *dev, gboolean reset_sensor, guint8 sleep_time,
                       GoodixResetCallback callback, gpointer user_data) {
  // Only support reset sensor

  GoodixReset payload = {.soft_reset_mcu = FALSE,
                         .reset_sensor = reset_sensor ? TRUE : FALSE,
                         .sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_RESET, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_reset, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_RESET, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, TRUE, NULL, NULL);
}

void goodix_send_firmware_version(FpDevice *dev,
                                  GoodixFirmwareVersionCallback callback,
                                  gpointer user_data) {
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_firmware_version, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, TRUE, NULL, NULL);
}

void goodix_send_query_mcu_state(FpDevice *dev, GoodixDefaultCallback callback,
                                 gpointer user_data) {
  GoodixQueryMcuState payload = {.unused_flags = 0x55};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, TRUE, NULL, NULL);
}

void goodix_send_request_tls_connection(FpDevice *dev,
                                        GoodixNoneCallback callback,
                                        gpointer user_data) {
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                       NULL, NULL);
}

void goodix_send_tls_successfully_established(FpDevice *dev,
                                              GoodixNoneCallback callback,
                                              gpointer user_data) {
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE, FALSE,
                       NULL, NULL);
}

void goodix_send_preset_psk_write_r(FpDevice *dev, guint32 address,
                                    guint8 *psk_r, guint16 length,
                                    GDestroyNotify free_func,
                                    GoodixSuccessCallback callback,
                                    gpointer user_data) {
  // Only support one address, one payload and one length

  guint8 *payload = g_malloc(sizeof(GoodixPresetPskR) + length);
  GoodixCallbackInfo *cb_info;

  ((GoodixPresetPskR *)payload)->address = GUINT32_TO_LE(address);
  ((GoodixPresetPskR *)payload)->length = GUINT32_TO_LE(length);
  memcpy(payload + sizeof(GoodixPresetPskR), psk_r, length);
  if (free_func) free_func(psk_r);

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_WRITE_R, payload,
                         sizeof(payload) + length, g_free, TRUE, TRUE,
                         goodix_receive_preset_psk_write_r, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_WRITE_R, payload,
                       sizeof(payload) + length, g_free, TRUE, TRUE, NULL,
                       NULL);
}

void goodix_send_preset_psk_read_r(FpDevice *dev, guint32 address,
                                   guint16 length,
                                   GoodixPresetPskReadRCallback callback,
                                   gpointer user_data) {
  GoodixPresetPskR payload = {.address = GUINT32_TO_LE(address),
                              .length = GUINT32_TO_LE(length)};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_READ_R, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, TRUE,
                         goodix_receive_preset_psk_read_r, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_READ_R, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, TRUE, NULL, NULL);
}

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
  priv->data = NULL;
  priv->length = 0;

  return g_usb_device_claim_interface(fpi_device_get_usb_device(dev),
                                      class->interface, 0, error);
}

gboolean goodix_dev_deinit(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  g_free(priv->data);

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
