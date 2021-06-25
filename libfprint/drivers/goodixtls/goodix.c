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

#define FP_COMPONENT "goodixtls"

#include "drivers_api.h"
#include "goodix_proto.h"
#include "goodixtls.h"
#include "goodix.h"

struct _FpiDeviceGoodixTls {
  FpImageDevice parent;

  pthread_t tls_server_thread;
  gint tls_server_sock;
  SSL_CTX *tls_server_ctx;

  guint8 current_cmd;

  gpointer current_data;
  guint16 current_data_len;

  FpiUsbTransfer *read_transfer;
  GCancellable *read_cancellable;

  gboolean active;
};

G_DEFINE_TYPE(FpiDeviceGoodixTls, fpi_device_goodixtls, FP_TYPE_IMAGE_DEVICE);

gchar *data_to_str(gpointer data, gsize data_len) {
  g_autofree gchar *data_str = g_malloc(2 * data_len + 1);

  for (gsize i = 0; i < data_len; i++)
    sprintf((gchar *)data_str + 2 * i, "%02X", *((guint8 *)data + i));

  return g_steal_pointer(&data_str);
}

/* ---- GOODIX SECTION START ---- */

static void goodix_receive_data(FpiSsm *ssm, FpDevice *dev, guint timeout) {
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

  G_DEBUG_HERE();

  transfer->ssm = ssm;
  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk(transfer, GOODIX_EP_IN, EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit(transfer, timeout, NULL, goodix_receive_data_cb,
                          NULL);
}

static void goodix_cmd_done(FpiSsm *ssm, FpDevice *dev, guint8 cmd) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);

  G_DEBUG_HERE();

  if (self->current_cmd != cmd) {
    // Received a command that is not running. This should not happen.
    fp_warn("Command 0x%02X is done but the runnning command is 0x%02X", cmd,
            self->current_cmd);
    return;
  }

  fpi_ssm_next_state(ssm);
}

static void goodix_ack_handle(FpiSsm *ssm, FpDevice *dev, gpointer data,
                              gsize data_len, GDestroyNotify data_destroy) {
  guint8 cmd;
  gboolean need_config;
  GError *error = NULL;

  need_config = goodix_decode_ack(&cmd, data, data_len, data_destroy, &error);

  if (error) goto failed;

  if (need_config) fp_warn("MCU need to be configured");

  switch (cmd) {
    case GOODIX_CMD_NOP:
      fp_warn(
          "Received nop ack, device might be in application production mode");
    case GOODIX_CMD_MCU_GET_IMAGE:
      goto read;
    case GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN:
      goodix_receive_data(ssm, dev, 0);
      return;
    case GOODIX_CMD_MCU_SWITCH_TO_FDT_UP:
      goodix_receive_data(ssm, dev, 0);
      return;
    case GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE:
      goto read;
    case GOODIX_CMD_NAV_0:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_NAV_0);
      goto read;
    case GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE);
      goto read;
    case GOODIX_CMD_WRITE_SENSOR_REGISTER:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_WRITE_SENSOR_REGISTER);
    case GOODIX_CMD_READ_SENSOR_REGISTER:
    case GOODIX_CMD_UPLOAD_CONFIG_MCU:
    case GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY:
      goto read;
    case GOODIX_CMD_ENABLE_CHIP:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_ENABLE_CHIP);
    case GOODIX_CMD_RESET:
      goto read;
    case GOODIX_CMD_MCU_ERASE_APP:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_MCU_ERASE_APP);
    case GOODIX_CMD_READ_OTP:
    case GOODIX_CMD_FIRMWARE_VERSION:
    case GOODIX_CMD_QUERY_MCU_STATE:
    case GOODIX_CMD_ACK:
    case GOODIX_CMD_REQUEST_TLS_CONNECTION:
      goto read;
    case GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED);
    case GOODIX_CMD_PRESET_PSK_WRITE_R:
    case GOODIX_CMD_PRESET_PSK_READ_R:
      goto read;
    default:
      // Unknown command. Raising an error.
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid ack command: 0x%02X", cmd);
      goto failed;
  }

read:
  goodix_receive_data(ssm, dev, GOODIX_TIMEOUT);
  return;

failed:
  fpi_ssm_mark_failed(ssm, error);
  return;
}

static void goodix_protocol_handle(FpiSsm *ssm, FpDevice *dev, gpointer data,
                                   gsize data_len,
                                   GDestroyNotify data_destroy) {
  guint8 cmd;
  gboolean invalid_checksum;
  gpointer payload = NULL;
  guint16 payload_len, payload_ptr_len;
  GError *error = NULL;

  payload_ptr_len =
      goodix_decode_protocol(&cmd, &invalid_checksum, &payload, &payload_len,
                             data, data_len, data_destroy, &error);

  if (error) goto failed;

  if (payload_ptr_len < payload_len) {
    // Command is not full but packet is since we checked that before.
    // This means that something when wrong. This should never happen.
    // Raising an error.
    // TODO implement reassembling for messages protocol beacause some device
    // doesn't use essages packets.
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %d bytes / %d bytes",
                payload_ptr_len, payload_len);
    goto failed;
  }

  if (invalid_checksum) {
    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Invalid message protocol checksum");
    goto failed;
  }

  fp_dbg("Received command: cmd=0x%02X, invalid_checksum=%d, payload=%s", cmd,
         invalid_checksum, data_to_str(payload, payload_len));

  switch (cmd) {
    case GOODIX_CMD_ACK:
      // Ack reply. Decoding it.
      goodix_ack_handle(ssm, dev, payload, payload_ptr_len, NULL);
      goto free;

    default:
      // Unknown command. Raising an error.
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid message protocol command: 0x%02X", cmd);
      goto failed;
  }

  goodix_receive_data(ssm, dev, GOODIX_TIMEOUT);
  goto free;

failed:
  fpi_ssm_mark_failed(ssm, error);
  goto free;

free:
  g_free(payload);
}

static void goodix_pack_handle(FpiSsm *ssm, FpDevice *dev, gpointer data,
                               gsize data_len, GDestroyNotify data_destroy) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  guint8 flags;
  gpointer payload = NULL;
  guint16 payload_len, payload_ptr_len;
  GError *error = NULL;

  self->current_data =
      g_realloc(self->current_data, self->current_data_len + data_len);
  memcpy((guint8 *)self->current_data + self->current_data_len, data, data_len);
  if (data_destroy) data_destroy(data);
  self->current_data_len += data_len;

  payload_ptr_len =
      goodix_decode_pack(&flags, &payload, &payload_len, self->current_data,
                         self->current_data_len, NULL, &error);

  if (error) goto failed;

  if (payload_ptr_len < payload_len)
    // Packet is not full, we still need data. Starting to read again.
    goto read;

  fp_dbg("Received pack: flags=0x%02X, payload=%s", flags,
         data_to_str(payload, payload_len));

  switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      // Message protocol. Decoding it.
      goodix_protocol_handle(ssm, dev, payload, payload_ptr_len, NULL);
      goto free;

    case GOODIX_FLAGS_TLS:
      // TLS message sending it to TLS server.
      // TODO
      goto read;

    default:
      // Unknown flags. Raising an error.
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid message pack flags: 0x%02X", flags);
      goto failed;
  }

read:
  goodix_receive_data(ssm, dev, GOODIX_TIMEOUT);
  goto free;

failed:
  fpi_ssm_mark_failed(ssm, error);
  goto free;

free:
  self->current_data_len = 0;
  g_clear_pointer(&self->current_data, g_free);
  g_free(payload);
}

static void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                                   gpointer user_data, GError *error) {
  G_DEBUG_HERE();

  if (error) goto failed;

  goodix_pack_handle(transfer->ssm, dev, transfer->buffer,
                     transfer->actual_length, NULL);

  return;

failed:
  fpi_ssm_mark_failed(transfer->ssm, error);

  // /* XXX: We used to reset the device in error cases! */
  // if (transfer->endpoint & FPI_USB_ENDPOINT_IN) {
  //   /* just finished receiving */
  //   self->last_read = g_memdup(transfer->buffer, transfer->actual_length);
  //   // fp_dbg("%lu", transfer->actual_length);
  //   // Some devices send multiple replies, so we need to catch them
  //   // 0xB0 equels the ACK packet
  //   // Special case: Reading firmware
  //   if (self->cmd->cmd == read_fw.cmd) {
  //     if (transfer->actual_length == self->cmd->response_len &&
  //         self->last_read[4] == 0xB0) {
  //       // We got ACK, now wait for the firmware version
  //       G_DEBUG_HERE();
  //       goodix_receive_data(ssm, dev, self->cmd->response_len_2);
  //     } else {
  //       // Reading the firmware version
  //       self->fw_ver = g_memdup(&self->last_read[7],
  //       self->cmd->response_len_2); G_DEBUG_HERE(); goodix_cmd_done(ssm);
  //     }
  //   }
  //   // Special case: Reading PSK hash
  //   else if (self->cmd->cmd == read_psk.cmd) {
  //     if (transfer->actual_length == self->cmd->response_len &&
  //         self->last_read[4] == 0xB0) {
  //       // We got ACK, now wait for the PSK
  //       G_DEBUG_HERE();
  //       goodix_receive_data(ssm, dev, self->cmd->response_len_2);
  //     } else {
  //       /*fp_dbg("%lu", transfer->actual_length);
  //       gint i;
  //       for (i = 16; i < GOODIX_PSK_LEN+16; i++)
  //       {
  //         fp_dbg("%02X", self->last_read[i]);
  //       }*/

  //       // Reading the PSK
  //       self->sensor_psk_hash = g_memdup(&self->last_read[16],
  //       GOODIX_PSK_LEN); G_DEBUG_HERE(); goodix_cmd_done(ssm);
  //     }
  //   }
  //   // Special case: Setting MCU config
  //   else if (self->cmd->cmd == mcu_set_config.cmd) {
  //     if (transfer->actual_length == self->cmd->response_len &&
  //         self->last_read[4] == 0xB0) {
  //       // We got ACK, now wait for the PSK
  //       G_DEBUG_HERE();
  //       goodix_receive_data(ssm, dev, self->cmd->response_len_2);
  //     } else {
  //       G_DEBUG_HERE();
  //       goodix_cmd_done(ssm);
  //     }
  //   }
  //   // Special case: Setting scan frequency
  //   else if (self->cmd->cmd == set_powerdown_scan_frequency.cmd) {
  //     if (transfer->actual_length == self->cmd->response_len &&
  //         self->last_read[4] == 0xB0) {
  //       // We got ACK, now wait for the second packet
  //       G_DEBUG_HERE();
  //       goodix_receive_data(ssm, dev, self->cmd->response_len_2);
  //     } else {
  //       G_DEBUG_HERE();
  //       goodix_cmd_done(ssm);
  //     }
  //   }
  //   // Special case: Requesting TLS connection
  //   else if (self->cmd->cmd == mcu_request_tls_connection.cmd) {
  //     if (transfer->actual_length == self->cmd->response_len &&
  //         self->last_read[4] == 0xB0) {
  //       // We got ACK, now wait for the second packet
  //       self->cmd_recv_counter = 1;
  //       G_DEBUG_HERE();
  //       goodix_receive_data(ssm, dev, self->cmd->response_len_2);
  //     } else if (self->cmd_recv_counter == 1) {
  //       // Read 56 byte packet
  //       self->cmd_recv_counter = 2;
  //       G_DEBUG_HERE();
  //       self->tls_msg_1 = g_memdup(&self->last_read,
  //       self->cmd->response_len_2); goodix_receive_data(ssm, dev,
  //       self->cmd->response_len_3);
  //     }
  //     /*else if(self->cmd_recv_counter == 2)
  //     {
  //       // Read first 64 byte packet
  //       self->cmd_recv_counter = 3;
  //       G_DEBUG_HERE ();
  //       self->tls_msg_2 = g_memdup (&self->last_read,
  //     self->cmd->response_len_3); goodix_receive_data (ssm, dev,
  //     self->cmd->response_len_4);
  //     }*/
  //     else {
  //       // Read second 64 byte packet
  //       self->cmd_recv_counter = 0;
  //       G_DEBUG_HERE();
  //       self->tls_msg_2 = g_memdup(&self->last_read,
  //       self->cmd->response_len_3);

  //       fp_dbg("\n");

  //       gint i;
  //       for (i = 0; i < self->cmd->response_len_2; i++) {
  //         fp_dbg("%02X", self->tls_msg_1[i]);
  //       }
  //       fp_dbg("\n");
  //       for (i = 0; i < self->cmd->response_len_3; i++) {
  //         fp_dbg("%02X", self->tls_msg_2[i]);
  //       }

  //       fp_dbg("\n");
  //       /*for (i = 0; i < 64; i++)
  //       {
  //         fp_dbg("%02X", self->tls_msg_3[i]);
  //       }*/
  //       fp_dbg("\n");

  //       goodix_cmd_done(ssm);
  //     }
  //   } else {
  //     goodix_cmd_done(ssm);
  //   }
  // } else {
  //   /* just finished sending */
  //   G_DEBUG_HERE();
  //   goodix_receive_data(ssm, dev, self->cmd->response_len);
  // }
}

static void goodix_send_pack(FpiSsm *ssm, FpDevice *dev, guint8 flags,
                             gpointer payload, guint16 payload_len,
                             GDestroyNotify destroy) {
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);
  GError *error = NULL;
  gpointer data;
  gsize data_len;

  fp_dbg("Sending pack: flags=0x%02X, payload=%s", flags,
         data_to_str(payload, payload_len));

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  data_len = goodix_encode_pack(&data, flags, payload, payload_len, destroy);

  for (gsize i = 0; i < data_len; i += GOODIX_MAX_DATA_WRITE) {
    fpi_usb_transfer_fill_bulk_full(transfer, GOODIX_EP_OUT, (guint8 *)data + i,
                                    GOODIX_MAX_DATA_WRITE, g_free);

    if (!fpi_usb_transfer_submit_sync(transfer, GOODIX_TIMEOUT, &error))
      goto failed;
  }

  goto free;

failed:
  fpi_ssm_mark_failed(ssm, error);
  goto free;

free:
  fpi_usb_transfer_unref(transfer);
}

static void goodix_send_protocol(FpiSsm *ssm, FpDevice *dev, guint8 cmd,
                                 gboolean calc_checksum, gpointer payload,
                                 guint16 payload_len, GDestroyNotify destroy) {
  gpointer data;
  gsize data_len;

  fp_dbg("Sending command: cmd=0x%02X, calc_checksum=%d, payload=%s", cmd,
         calc_checksum, data_to_str(payload, payload_len));

  FPI_DEVICE_GOODIXTLS(dev)->current_cmd = cmd;

  data_len = goodix_encode_protocol(&data, cmd, calc_checksum, payload,
                                    payload_len, destroy);

  goodix_send_pack(ssm, dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len, g_free);
}

static void goodix_cmd_nop(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint32 unknown;
  } __attribute__((packed)) payload = {.unknown = 0};

  fp_dbg("Goodix nop");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_NOP, FALSE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_mcu_get_image(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint8 unused_flags;
    guint8 none;
  } __attribute__((packed)) payload = {.unused_flags = 0x01, .none = 0};

  fp_dbg("Goodix mcu get image");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_GET_IMAGE, TRUE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_mcu_switch_to_fdt_down(FpiSsm *ssm, FpDevice *dev,
                                              gpointer mode, guint16 mode_len,
                                              GDestroyNotify destroy) {
  fp_dbg("Goodix mcu switch to fdt down");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, TRUE, mode,
                       mode_len, destroy);
}

static void goodix_cmd_mcu_switch_to_fdt_up(FpiSsm *ssm, FpDevice *dev,
                                            gpointer mode, guint16 mode_len,
                                            GDestroyNotify destroy) {
  fp_dbg("Goodix mcu switch to fdt up");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, TRUE, mode,
                       mode_len, destroy);
}

static void goodix_cmd_mcu_switch_to_fdt_mode(FpiSsm *ssm, FpDevice *dev,
                                              gpointer mode, guint16 mode_len,
                                              GDestroyNotify destroy) {
  fp_dbg("Goodix mcu switch to fdt mode");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, TRUE, mode,
                       mode_len, destroy);
}

static void goodix_cmd_nav_0(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint8 unused_flags;
    guint8 none;
  } __attribute__((packed)) payload = {.unused_flags = 0x01, .none = 0};

  fp_dbg("Goodix nav 0");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_NAV_0, TRUE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_mcu_switch_to_idle_mode(FpiSsm *ssm, FpDevice *dev,
                                               guint8 sleep_time) {
  struct _payload {
    guint8 sleep_time;
    guint8 none;
  } __attribute__((packed)) payload = {.sleep_time = sleep_time, .none = 0};

  fp_dbg("Goodix mcu switch to idle mode");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE, TRUE,
                       &payload, sizeof(payload), NULL);
}

static void goodix_cmd_write_sensor_register(FpiSsm *ssm, FpDevice *dev,
                                             guint16 address, guint16 value) {
  // Only support one address and one value

  struct _payload {
    guint8 multiples;
    guint16 address;
    guint16 value;
  } __attribute__((packed)) payload = {.multiples = FALSE,
                                       .address = GUINT16_TO_LE(address),
                                       .value = GUINT16_TO_LE(value)};

  fp_dbg("Goodix write sensor register");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_WRITE_SENSOR_REGISTER, TRUE,
                       &payload, sizeof(payload), NULL);
}

static void goodix_cmd_read_sensor_register(FpiSsm *ssm, FpDevice *dev,
                                            guint16 address, guint8 length) {
  // Only support one address

  struct _payload {
    guint8 multiples;
    guint16 address;
    guint8 length;
    guint8 none;
  } __attribute__((packed)) payload = {.multiples = FALSE,
                                       .address = GUINT16_TO_LE(address),
                                       .length = length,
                                       .none = 0};

  fp_dbg("Goodix read sensor register");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_READ_SENSOR_REGISTER, TRUE,
                       &payload, sizeof(payload), NULL);
}

static void goodix_cmd_upload_config_mcu(FpiSsm *ssm, FpDevice *dev,
                                         gpointer config, guint16 config_len,
                                         GDestroyNotify destroy) {
  fp_dbg("Goodix upload config mcu");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, TRUE, config,
                       config_len, destroy);
}

static void goodix_cmd_set_powerdown_scan_frequency(
    FpiSsm *ssm, FpDevice *dev, guint16 powerdown_scan_frequency) {
  struct _payload {
    guint16 powerdown_scan_frequency;
  } __attribute__((packed)) payload = {
      .powerdown_scan_frequency = GUINT16_TO_LE(powerdown_scan_frequency)};

  fp_dbg("Goodix set powerdown scan frequency");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY, TRUE,
                       &payload, sizeof(payload), NULL);
}

static void goodix_cmd_enable_chip(FpiSsm *ssm, FpDevice *dev,
                                   gboolean enable) {
  struct _payload {
    guint8 enable;
    guint8 none;
  } __attribute__((packed))
  payload = {.enable = enable ? TRUE : FALSE, .none = 0};

  fp_dbg("Goodix enable chip");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_ENABLE_CHIP, TRUE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_reset(FpiSsm *ssm, FpDevice *dev, gboolean reset_sensor,
                             gboolean soft_reset_mcu, guint8 sleep_time) {
  struct _payload {
    guint8 reset_flags;
    guint8 sleep_time;
  } __attribute__((packed))
  payload = {.reset_flags = (soft_reset_mcu ? TRUE : FALSE) << 1 |
                            (reset_sensor ? TRUE : FALSE),
             .sleep_time = sleep_time};

  fp_dbg("Goodix reset");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_RESET, TRUE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_firmware_version(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint16 none;
  } __attribute__((packed)) payload = {.none = 0};

  fp_dbg("Goodix firmware version");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_FIRMWARE_VERSION, TRUE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_query_mcu_state(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint8 unused_flags;
  } __attribute__((packed)) payload = {.unused_flags = 0x55};

  fp_dbg("Goodix query mcu state");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_QUERY_MCU_STATE, TRUE, &payload,
                       sizeof(payload), NULL);
}

static void goodix_cmd_request_tls_connection(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint16 none;
  } __attribute__((packed)) payload = {.none = 0};

  fp_dbg("Goodix request tls connection");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_REQUEST_TLS_CONNECTION, TRUE,
                       &payload, sizeof(payload), NULL);
}

static void goodix_cmd_tls_successfully_established(FpiSsm *ssm,
                                                    FpDevice *dev) {
  struct _payload {
    guint16 none;
  } __attribute__((packed)) payload = {.none = 0};

  fp_dbg("Goodix tls successfully established");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED, TRUE,
                       &payload, sizeof(payload), NULL);
}

static void goodix_cmd_preset_psk_write_r(FpiSsm *ssm, FpDevice *dev,
                                          guint32 address, gpointer psk,
                                          guint32 psk_len,
                                          GDestroyNotify destroy) {
  // Only support one address, one payload and one length

  struct _payload {
    guint32 address;
    guint32 length;
  } __attribute__((packed)) payload = {.address = GUINT32_TO_LE(address),
                                       .length = GUINT32_TO_LE(psk_len)};
  gpointer payload_ptr = g_malloc(sizeof(payload) + psk_len);

  fp_dbg("Goodix preset psk write r");

  memcpy(payload_ptr, &payload, sizeof(payload));
  memcpy((guint8 *)payload_ptr + sizeof(payload), psk, psk_len);
  if (destroy) destroy(psk);

  goodix_send_protocol(ssm, dev, GOODIX_CMD_PRESET_PSK_WRITE_R, TRUE,
                       payload_ptr, sizeof(payload) + psk_len, g_free);
}

static void goodix_cmd_preset_psk_read_r(FpiSsm *ssm, FpDevice *dev,
                                         guint32 address, guint32 length) {
  struct _payload {
    guint32 address;
    guint32 length;
  } __attribute__((packed)) payload = {.address = GUINT32_TO_LE(address),
                                       .length = GUINT32_TO_LE(length)};

  fp_dbg("Goodix preset psk read r");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_PRESET_PSK_READ_R, TRUE, &payload,
                       sizeof(payload), NULL);
}

/* ---- GOODIX SECTION END ---- */

/* ------------------------------------------------------------------------- */

/* ---- TLS SECTION START ---- */

enum tls_states {
  TLS_SERVER_INIT,
  TLS_SERVER_HANDSHAKE_INIT,
  // TLS_MCU_REQUEST_CONNECTION,
  TLS_NUM_STATES,
};

static void tls_run_state(FpiSsm *ssm, FpDevice *dev) {
  G_DEBUG_HERE();

  switch (fpi_ssm_get_cur_state(ssm)) {
    case TLS_SERVER_INIT:
      tls_server_init(ssm);
      break;

    case TLS_SERVER_HANDSHAKE_INIT:
      tls_server_handshake_init();
      break;

      // case TLS_MCU_REQUEST_CONNECTION:
  }
}

static void tls_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  G_DEBUG_HERE();

  fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}

static void goodix_tls(FpImageDevice *dev) {
  G_DEBUG_HERE();

  fpi_ssm_start(fpi_ssm_new(FP_DEVICE(dev), tls_run_state, TLS_NUM_STATES),
                tls_complete);
}

/* ---- TLS SECTION END ---- */

/* ------------------------------------------------------------------------- */

/* ---- ACTIVE SECTION START ---- */

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,
  ACTIVATE_NUM_STATES,
};

static void activate_run_state(FpiSsm *ssm, FpDevice *dev) {
  // FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);

  G_DEBUG_HERE();

  // gint i;

  switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP:
      // NOP seems to clear the previous command buffer.
      goodix_receive_data(ssm, dev, GOODIX_TIMEOUT);
      // DON'T ADD A BREAK HERE!
    case ACTIVATE_NOP:
      goodix_cmd_nop(ssm, dev);
      goodix_cmd_done(ssm, dev, GOODIX_CMD_NOP);
      break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_cmd_enable_chip(ssm, dev, TRUE);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_cmd_firmware_version(ssm, dev);
      break;

      // case ACTIVATE_VERIFY_FW_VER:
      //   if (!strcmp(self->fw_ver, GOODIX_FIRMWARE_VERSION_SUPPORTED)) {
      //     // The firmware version is supported
      //     fpi_ssm_next_state(ssm);
      //   } else {
      //     // The firmware version is unsupported
      //     fpi_ssm_mark_failed(ssm,
      //                         fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
      //                                                  "Unsupported
      //                                                  firmware!"));
      //   }
      //   break;

    case ACTIVATE_CHECK_PSK:
      goodix_cmd_preset_psk_read_r(ssm, dev, 0xBB020003, 0);
      break;

      // case ACTIVATE_VERIFY_PSK:
      //   /*for (i = 0; i < GOODIX_PSK_LEN; i++)
      //   {
      //     fp_dbg("%02X", self->sensor_psk_hash[i]);
      //   }

      //   fp_dbg("-----");

      //   for (i = 0; i < GOODIX_PSK_LEN; i++)
      //   {
      //     fp_dbg("%02X", zero_psk_hash[i]);
      //   }*/

      //   // The PSK hash matches the Zero-PSK hash
      //   if (!memcmp(self->sensor_psk_hash, &zero_psk_hash,
      //               sizeof(&self->sensor_psk_hash))) {
      //     // fpi_ssm_mark_completed (ssm);
      //     fpi_ssm_next_state(ssm);
      //   } else {
      //     // The PSK hash doesn't match
      //     fpi_ssm_mark_failed(ssm,
      //                         fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
      //                                                  "PSK doesn't
      //                                                  match!"));
      //   }
      //   break;

    case ACTIVATE_SET_MCU_IDLE:
      goodix_cmd_mcu_switch_to_idle_mode(ssm, dev, 20);
      break;

    case ACTIVATE_SET_MCU_CONFIG:
      goodix_cmd_upload_config_mcu(ssm, dev, device_config,
                                   sizeof(device_config), NULL);
      break;

    case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
      goodix_cmd_set_powerdown_scan_frequency(ssm, dev, 100);
      break;
  }
}

static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  FpImageDevice *image_dev = FP_IMAGE_DEVICE(dev);

  G_DEBUG_HERE();

  fpi_image_device_activate_complete(image_dev, error);

  if (!error) goodix_tls(image_dev);
}

/* ---- ACTIVE SECTION END ---- */

/* ------------------------------------------------------------------------- */

/* ---- DEV SECTION START ---- */

static void dev_init(FpImageDevice *dev) {
  GError *error = NULL;

  G_DEBUG_HERE();

  if (!g_usb_device_claim_interface(fpi_device_get_usb_device(FP_DEVICE(dev)),
                                    GOODIX_INTERFACE, 0, &error)) {
    fpi_image_device_open_complete(dev, error);
    return;
  }

  fpi_image_device_open_complete(dev, NULL);
}

static void dev_deinit(FpImageDevice *dev) {
  GError *error = NULL;

  G_DEBUG_HERE();

  if (!g_usb_device_release_interface(fpi_device_get_usb_device(FP_DEVICE(dev)),
                                      GOODIX_INTERFACE, 0, &error)) {
    fpi_image_device_close_complete(dev, error);
    return;
  }

  fpi_image_device_close_complete(dev, NULL);
}

static void dev_activate(FpImageDevice *dev) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);

  G_DEBUG_HERE();

  self->active = TRUE;
  self->current_data = NULL;
  self->current_data_len = 0;

  fpi_ssm_start(
      fpi_ssm_new(FP_DEVICE(dev), activate_run_state, ACTIVATE_NUM_STATES),
      activate_complete);
}

static void dev_change_state(FpImageDevice *dev, FpiImageDeviceState state) {
  G_DEBUG_HERE();

  // if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
  //   goodix_tls(FPI_DEVICE_GOODIX(dev));
}

static void dev_deactivate(FpImageDevice *dev) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);

  G_DEBUG_HERE();

  if (!self->active) {
    g_clear_pointer(&self->current_data, g_free);
    self->current_data_len = 0;

    fpi_image_device_deactivate_complete(dev, NULL);
  } else
    // The device is not yet inactive, flag that we are deactivating (and need
    // to signal back deactivation). Note that any running capture will be
    // cancelled already if needed.
    self->active = FALSE;
  // TODO?
}

/* ---- DEV SECTION END ---- */

/* ------------------------------------------------------------------------- */

/* ---- FPI SECTION START ---- */

static void fpi_device_goodixtls_init(FpiDeviceGoodixTls *self) {}

static void fpi_device_goodixtls_class_init(FpiDeviceGoodixTlsClass *class) {
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS(class);

  G_DEBUG_HERE();

  dev_class->id = "goodixtls";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;

  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  // TODO
  img_class->bz3_threshold = 24;
  img_class->img_width = 80;
  img_class->img_height = 88;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_activate;
  img_class->change_state = dev_change_state;
  img_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
  dev_class->features &= ~FP_DEVICE_FEATURE_VERIFY;
}

/* ---- FPI SECTION END ---- */
