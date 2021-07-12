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

#pragma once

// 1 seconds USB timeout
#define GOODIX_TIMEOUT (1000)

G_DECLARE_DERIVABLE_TYPE(FpiDeviceGoodixTls, fpi_device_goodixtls, FPI,
                         DEVICE_GOODIXTLS, FpImageDevice)

#define FPI_TYPE_DEVICE_GOODIXTLS (fpi_device_goodixtls_get_type())

struct _FpiDeviceGoodixTlsClass {
  FpImageDeviceClass parent;

  gint interface;
  guint8 ep_in;
  guint8 ep_out;

  const gchar *firmware_version;
  const guint8 *pmk_hash;
  gsize pmk_hash_len;
};

// ---- GOODIX SECTION START ----

void goodix_receive_data(FpiSsm *ssm);

void goodix_cmd_done(FpiSsm *ssm, guint8 cmd);

void goodix_ack_handle(FpiSsm *ssm, guint8 *data, gsize data_len,
                       GDestroyNotify data_destroy, GError **error);

void goodix_cmd_handle(FpiSsm *ssm, guint8 cmd, guint8 *data, gsize data_len,
                       GDestroyNotify data_destroy, GError **error);

void goodix_protocol_handle(FpiSsm *ssm, guint8 *data, gsize data_len,
                            GDestroyNotify data_destroy, GError **error);

void goodix_pack_handle(FpiSsm *ssm, guint8 *data, gsize data_len,
                        GDestroyNotify data_destroy, GError **error);

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error);

void goodix_send_pack(FpiSsm *ssm, guint8 flags, guint8 *payload,
                      guint16 payload_len, GDestroyNotify destroy);

void goodix_send_protocol(FpiSsm *ssm, guint8 cmd, gboolean calc_checksum,
                          gboolean reply, guint8 *payload, guint16 payload_len,
                          GDestroyNotify payload_destroy);

void goodix_cmd_nop(FpiSsm *ssm);

void goodix_cmd_mcu_get_image(FpiSsm *ssm);

void goodix_cmd_mcu_switch_to_fdt_down(FpiSsm *ssm, guint8 *mode,
                                       guint16 mode_len,
                                       GDestroyNotify destroy);

void goodix_cmd_mcu_switch_to_fdt_up(FpiSsm *ssm, guint8 *mode,
                                     guint16 mode_len, GDestroyNotify destroy);

void goodix_cmd_mcu_switch_to_fdt_mode(FpiSsm *ssm, guint8 *mode,
                                       guint16 mode_len,
                                       GDestroyNotify destroy);

void goodix_cmd_nav_0(FpiSsm *ssm);

void goodix_cmd_mcu_switch_to_idle_mode(FpiSsm *ssm, guint8 sleep_time);

void goodix_cmd_write_sensor_register(FpiSsm *ssm, guint16 address,
                                      guint16 value);

void goodix_cmd_read_sensor_register(FpiSsm *ssm, guint16 address,
                                     guint8 length);

void goodix_cmd_upload_config_mcu(FpiSsm *ssm, guint8 *config,
                                  guint16 config_len, GDestroyNotify destroy);

void goodix_cmd_set_powerdown_scan_frequency(FpiSsm *ssm,
                                             guint16 powerdown_scan_frequency);

void goodix_cmd_enable_chip(FpiSsm *ssm, gboolean enable);

void goodix_cmd_reset(FpiSsm *ssm, gboolean reset_sensor,
                      gboolean soft_reset_mcu, guint8 sleep_time);

void goodix_cmd_firmware_version(FpiSsm *ssm);

void goodix_cmd_query_mcu_state(FpiSsm *ssm);

void goodix_cmd_request_tls_connection(FpiSsm *ssm);

void goodix_cmd_tls_successfully_established(FpiSsm *ssm);

void goodix_cmd_preset_psk_write_r(FpiSsm *ssm, guint32 address, guint8 *psk,
                                   guint32 psk_len, GDestroyNotify destroy);

void goodix_cmd_preset_psk_read_r(FpiSsm *ssm, guint32 address, guint32 length);

// ---- GOODIX SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error);

gboolean goodix_dev_deinit(FpDevice *dev, GError **error);

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

void goodix_tls_run_state(FpiSsm *ssm, FpDevice *dev);

void goodix_tls_complete(FpiSsm *ssm, FpDevice *dev, GError *error);

void goodix_tls(FpDevice *dev);

// ---- TLS SECTION END ----
