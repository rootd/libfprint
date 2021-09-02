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

#include "fpi-usb-transfer.h"
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
    GoodixTlsServer* tls_hop;

    GSource* timeout;

    guint8 cmd;

    gboolean ack;
    gboolean reply;

    GoodixCmdCallback callback;
    gpointer user_data;

    guint8* data;
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

  if (!(priv->ack || priv->reply)) return;

  if (priv->timeout) g_clear_pointer(&priv->timeout, g_source_destroy);
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

void goodix_receive_preset_psk_read(FpDevice *dev, guint8 *data, guint16 length,
                                    gpointer user_data, GError *error) {
  guint32 psk_len;
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixPresetPskReadCallback callback =
      (GoodixPresetPskReadCallback)cb_info->callback;

  if (error) {
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  if (length < sizeof(guint8)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  if (data[0] != 0x00) {
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, NULL);
    return;
  }

  if (length < sizeof(guint8) + sizeof(GoodixPresetPsk)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  psk_len =
      GUINT32_FROM_LE(((GoodixPresetPsk *)(data + sizeof(guint8)))->length);

  if (length < psk_len + sizeof(guint8) + sizeof(GoodixPresetPsk)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid preset PSK read reply length: %d", length);
    callback(dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
    return;
  }

  callback(dev, TRUE,
           GUINT32_FROM_LE(((GoodixPresetPsk *)(data + sizeof(guint8)))->flags),
           data + sizeof(guint8) + sizeof(GoodixPresetPsk), psk_len,
           cb_info->user_data, NULL);
}

void goodix_receive_preset_psk_write(FpDevice *dev, guint8 *data,
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
                "Invalid preset PSK write reply length: %d", length);
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
  GoodixAck *ack = (GoodixAck *)data;
  guint8 cmd;

  if (length != sizeof(GoodixAck)) {
    fp_warn("Invalid ACK length: %d", length);
    return;
  }

  if (!ack->always_true) {
    // Warn about error.
    fp_warn("Invalid ACK flags: 0x%02x", data[sizeof(guint8)]);
    return;
  }

  cmd = ack->cmd;

  if (ack->has_no_config) fp_warn("MCU has no config");

  if (priv->cmd != cmd) {
    fp_warn("Invalid ACK command: 0x%02x", cmd);
    return;
  }

  if (!priv->ack) {
    fp_warn("Didn't excpect an ACK for command: 0x%02x", priv->cmd);
    return;
  }

  if (!priv->reply) {
      G_DEBUG_HERE();
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
                              &valid_checksum, &valid_null_checksum)) {
      fp_err("Incomplete, size: %d", length);
      // Protocol is not full, we still need data.
      // TODO implement protocol assembling.
      return;
  }

  if (cmd == GOODIX_CMD_ACK) {
      fp_dbg("got ack");
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

static void goodix_receive_tls(FpDevice* dev, guint8* data, guint32 length)
{
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    g_autofree guint8* payload = NULL;
    guint16 payload_len;
    gboolean valid_checksum; // TODO implement checksum.
    guint8 flags;

    if (!goodix_decode_pack(data, length, &flags, &payload, &payload_len,
                            &valid_checksum)) {
        fp_err("Incomplete, size tls: %d", length);
        // Protocol is not full, we still need data.
        // TODO implement protocol assembling.
        return;
    }

    if (!priv->reply) {
        fp_warn("Didn't excpect a reply for command: 0x%02x", priv->cmd);
        return;
    }

    if (priv->ack)
        fp_warn("Didn't got ACK for command: 0x%02x", priv->cmd);

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
                          &payload_len, &valid_checksum)) {
      // Packet is not full, we still need data.
      return;
  }

  switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
        fp_dbg("Got protocol msg");
        goodix_receive_protocol(dev, payload, payload_len);
        break;

    case GOODIX_FLAGS_TLS:
        fp_dbg("Got TLS msg");
        goodix_receive_done(dev, payload, payload_len, NULL);

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

void goodix_receive_timeout_cb(FpDevice *dev, gpointer user_data) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  GError *error = NULL;

  g_set_error(&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
              "Command timed out: 0x%02x", priv->cmd);
  goodix_receive_done(dev, NULL, 0, error);
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
  FpiDeviceGoodixTlsClass* class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);

  for (guint32 i = 0; i < length; i += GOODIX_EP_OUT_MAX_BUF_SIZE) {
      FpiUsbTransfer* transfer = fpi_usb_transfer_new(dev);
      transfer->short_is_error = TRUE;

      fpi_usb_transfer_fill_bulk_full(transfer, class->ep_out, data + i,
                                      GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

      if (!fpi_usb_transfer_submit_sync(transfer, GOODIX_TIMEOUT, error)) {
          if (free_func)
              free_func(data);
          fpi_usb_transfer_unref(transfer);
          return FALSE;
      }
      fpi_usb_transfer_unref(transfer);
  }

  if (free_func)
      free_func(data);
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
                          gboolean calc_checksum, guint timeout_ms,
                          gboolean reply, GoodixCmdCallback callback,
                          gpointer user_data) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  GError *error = NULL;
  guint8 *data;
  guint32 data_len;

  if (priv->ack || priv->reply || priv->timeout) {
    // A command is already running.
    fp_warn("A command is already running: 0x%02x", priv->cmd);
    if (free_func) free_func(payload);
    return;
  }

  fp_dbg("Running command: 0x%02x", cmd);

  if (timeout_ms)
    priv->timeout = fpi_device_add_timeout(
        dev, timeout_ms, goodix_receive_timeout_cb, NULL, NULL);
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
                         sizeof(payload), NULL, FALSE, GOODIX_TIMEOUT, FALSE,
                         goodix_receive_none, cb_info);
    goodix_receive_done(dev, NULL, 0, NULL);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_NOP, (guint8 *)&payload, sizeof(payload),
                       NULL, FALSE, GOODIX_TIMEOUT, FALSE, NULL, NULL);
  goodix_receive_done(dev, NULL, 0, NULL);
}

void goodix_send_mcu_get_image(FpDevice* dev, GoodixNoneCallback callback,
                               gpointer user_data)
{
    GoodixDefault payload = {.unused_flags = 0x01};
    GoodixCallbackInfo* cb_info;

    if (callback) {
        cb_info = malloc(sizeof(GoodixCallbackInfo));

        cb_info->callback = G_CALLBACK(callback);
        cb_info->user_data = user_data;

        goodix_send_protocol(dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8*) &payload,
                             sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                             goodix_receive_none, cb_info);
        return;
    }

    goodix_send_protocol(dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8*) &payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                         NULL, NULL);
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
                         free_func, TRUE, 0, TRUE, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                       free_func, TRUE, 0, TRUE, NULL, NULL);
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
                         free_func, TRUE, 0, TRUE, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                       free_func, TRUE, 0, TRUE, NULL, NULL);
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
                         free_func, TRUE, 0, TRUE, goodix_receive_default,
                         cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                       free_func, TRUE, 0, TRUE, NULL, NULL);
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
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_NAV_0, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
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
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, FALSE, NULL, NULL);
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
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, FALSE, NULL, NULL);
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
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, TRUE, goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_READ_SENSOR_REGISTER, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
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
                         free_func, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_success, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                       free_func, TRUE, GOODIX_TIMEOUT, TRUE, NULL, NULL);
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
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, TRUE, goodix_receive_success, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, TRUE, NULL, NULL);
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
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                         goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE, NULL,
                       NULL);
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
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_reset, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_RESET, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
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
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_firmware_version, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
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
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_default, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

void goodix_send_request_tls_connection(FpDevice* dev,
                                        GoodixDefaultCallback callback,
                                        gpointer user_data)
{
    GoodixNone payload = {};
    GoodixCallbackInfo* cb_info;

    if (callback) {
        cb_info = malloc(sizeof(GoodixCallbackInfo));

        cb_info->callback = G_CALLBACK(callback);
        cb_info->user_data = user_data;

        goodix_send_protocol(dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                             (guint8*) &payload, sizeof(payload), NULL, TRUE,
                             GOODIX_TIMEOUT, TRUE, goodix_receive_default,
                             cb_info);
        return;
    }

    goodix_send_protocol(dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                         (guint8*) &payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, TRUE, NULL, NULL);
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
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                       (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                       GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void goodix_send_preset_psk_write(FpDevice *dev, guint32 flags, guint8 *psk,
                                  guint16 length, GDestroyNotify free_func,
                                  GoodixSuccessCallback callback,
                                  gpointer user_data) {
  // Only support one flags, one payload and one length

  guint8 *payload = g_malloc(sizeof(GoodixPresetPsk) + length);
  GoodixPresetPsk *preset_psk = (GoodixPresetPsk *)payload;
  GoodixCallbackInfo *cb_info;

  preset_psk->flags = GUINT32_TO_LE(flags);
  preset_psk->length = GUINT32_TO_LE(length);
  memcpy(payload + sizeof(GoodixPresetPsk), psk, length);
  if (free_func) free_func(psk);

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_WRITE, payload,
                         sizeof(payload) + length, g_free, TRUE, GOODIX_TIMEOUT,
                         TRUE, goodix_receive_preset_psk_write, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_WRITE, payload,
                       sizeof(payload) + length, g_free, TRUE, GOODIX_TIMEOUT,
                       TRUE, NULL, NULL);
}

void goodix_send_preset_psk_read(FpDevice *dev, guint32 flags, guint16 length,
                                 GoodixPresetPskReadCallback callback,
                                 gpointer user_data) {
  GoodixPresetPsk payload = {.flags = GUINT32_TO_LE(flags),
                             .length = GUINT32_TO_LE(length)};
  GoodixCallbackInfo *cb_info;

  if (callback) {
    cb_info = malloc(sizeof(GoodixCallbackInfo));

    cb_info->callback = G_CALLBACK(callback);
    cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *)&payload,
                         sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                         goodix_receive_preset_psk_read, cb_info);
    return;
  }

  goodix_send_protocol(dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *)&payload,
                       sizeof(payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                       NULL);
}

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  priv->timeout = NULL;
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

  if (priv->timeout) g_source_destroy(priv->timeout);
  g_free(priv->data);

  return g_usb_device_release_interface(fpi_device_get_usb_device(dev),
                                        class->interface, 0, error);
}

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

void goodix_read_tls(FpDevice* dev, GoodixTlsCallback callback,
                     gpointer user_data)
{

    fp_dbg("goodix_read_tls()");
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    priv->callback = callback;
    priv->user_data = user_data;
    priv->reply = TRUE;
    goodix_receive_data(FP_DEVICE(self));
}

enum tls_states {
  TLS_SERVER_INIT,
  TLS_SERVER_HANDSHAKE_INIT,
  TLS_NUM_STATES,
};

static void on_goodix_tls_server_ready(GoodixTlsServer* server, GError* err,
                                       gpointer dev)
{
    if (err) {
        fp_err("server ready failed: %s", err->message);
        return;
    }
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    fp_dbg("TLS connection ready");
}
typedef struct _goodix_tls_handshake_state {
    int count;
    void (*callback)(FpDevice*, GError*, struct _goodix_tls_handshake_state*);
    FpDevice* dev;

} goodix_tls_handshake_state;

static void on_goodix_tls_read_handshake(FpDevice* dev, guint8* data,
                                         guint16 length, gpointer user_data,
                                         GError* error)
{
    goodix_tls_handshake_state* state = (goodix_tls_handshake_state*) user_data;
    if (error) {
        fp_err("failed to read tls during handshake: %s, code: %d",
               error->message, error->code);
        state->callback(dev, error, state);
        return;
    }
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(state->dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    fp_dbg("handshake loop, got: %s, size: %d", data_to_str(data, length),
           length);

    int sent = goodix_tls_client_send(priv->tls_hop, data, length);
    fp_dbg("sent: %d bytes", sent);

    if (state->count >= 2) {
        fp_dbg("Reading to proxy back");
        guint8 buff[1024];
        int size = goodix_tls_client_recv(priv->tls_hop, buff, sizeof(buff));
        if (size < 0) {
            fp_err("failed to recieve from server: %d", size);

            state->callback(dev, g_error_new(g_io_error_quark(), size, ""),
                            state);
            return;
        }
        fp_dbg("got: %s from the server (size: %d)", data_to_str(buff, size),
               size);
        GError* err = NULL;
        if (!goodix_send_data(dev, buff, size, NULL, &err)) {
            fp_err("failed to send tls data back: %s, code: %d", err->message,
                   err->code);
            state->callback(dev, err, state);
            return;
        }
        state->callback(dev, NULL, state);
    }
    else {
        ++state->count;
        goodix_read_tls(dev, on_goodix_tls_read_handshake, state);
    }
}
static void on_goodix_tls_handshake_done(FpDevice* dev, GError* err,
                                         goodix_tls_handshake_state* state)
{
    fp_dbg("DONE");
    free(state);

    goodix_send_tls_successfully_established(FP_DEVICE(dev), NULL, NULL);
}

static void on_goodix_request_tls_connection(FpDevice* dev, guint8* data,
                                             guint16 length, gpointer user_data,
                                             GError* error)
{
    if (error) {
        fp_err("failed to get tls handshake: %s", error->message);
        goodix_send_tls_successfully_established(FP_DEVICE(dev), NULL, NULL);
        return;
    }
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(user_data);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    GError* err = NULL;

    fp_dbg("len: %d, d: %s", length, data_to_str(data, length));
    goodix_tls_client_send(priv->tls_hop, data, length);
    guint8 buff[1024];
    int size = goodix_tls_client_recv(priv->tls_hop, buff, sizeof(buff));
    if (size < 0) {
        fp_err("failed to read: %d", size);
        goodix_send_tls_successfully_established(FP_DEVICE(dev), NULL, NULL);
        return;
    }
    goodix_send_pack(dev, GOODIX_FLAGS_TLS, buff, size, NULL, &err);

    goodix_tls_handshake_state* hs_st =
        malloc(sizeof(goodix_tls_handshake_state));
    hs_st->callback = on_goodix_tls_handshake_done;
    hs_st->dev = dev;
    hs_st->count = 0;
    goodix_read_tls(dev, on_goodix_tls_read_handshake, hs_st);
}

static void goodix_tls_ready(GoodixTlsServer* server, GError* err, gpointer dev)
{
    if (err) {
        fp_err("failed to init tls server: %s, code: %d", err->message,
               err->code);
        return;
    }
    goodix_send_request_tls_connection(FP_DEVICE(dev),
                                       on_goodix_request_tls_connection, dev);
}

void goodix_tls_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}

void goodix_tls(FpDevice *dev) {
    fp_dbg("Starting up goodix tls server");
    FpiDeviceGoodixTls* self = FPI_DEVICE_GOODIXTLS(dev);
    FpiDeviceGoodixTlsPrivate* priv =
        fpi_device_goodixtls_get_instance_private(self);
    g_assert(priv->tls_hop == NULL);
    priv->tls_hop = malloc(sizeof(GoodixTlsServer));
    GoodixTlsServer* s = priv->tls_hop;
    s->connection_callback = on_goodix_tls_server_ready;
    s->user_data = self;
    GError* err = NULL;
    if (!goodix_tls_server_init(priv->tls_hop, &err)) {
        fp_err("failed to init tls server, error: %s, code: %d", err->message,
               err->code);
        return;
    }

    goodix_tls_ready(s, err, self);
}

// ---- TLS SECTION END ----

static void fpi_device_goodixtls_init(FpiDeviceGoodixTls *self) {}

static void fpi_device_goodixtls_class_init(FpiDeviceGoodixTlsClass *class) {}
