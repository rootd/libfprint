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
};

typedef gboolean (*GoodixFirmwareVersionCallback)(gchar *firmware,
                                                  GError **error,
                                                  gpointer user_data);

typedef gboolean (*GoodixPresetPskReadRCallback)(guint8 *pmk, guint16 pmk_len,
                                                 GError **error,
                                                 gpointer user_data);

gchar *data_to_string(guint8 *data, guint32 data_len);

// ---- GOODIX RECEIVE SECTION START ----

void goodix_receive_done(FpiSsm *ssm, guint8 cmd);

void goodix_receive_preset_psk_read_r(FpiSsm *ssm, guint8 *data,
                                      guint16 data_len,
                                      GDestroyNotify data_destroy,
                                      GError **error);

void goodix_receive_ack(FpiSsm *ssm, guint8 *data, guint16 data_len,
                        GDestroyNotify data_destroy, GError **error);

void goodix_receive_firmware_version(FpiSsm *ssm, guint8 *data,
                                     guint16 data_len,
                                     GDestroyNotify data_destroy,
                                     GError **error);

void goodix_receive_protocol(FpiSsm *ssm, guint8 *data, guint32 data_len,
                             GDestroyNotify data_destroy, GError **error);

void goodix_receive_pack(FpiSsm *ssm, guint8 *data, guint32 data_len,
                         GDestroyNotify data_destroy, GError **error);

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error);

void goodix_receive_data(FpiSsm *ssm);

// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

void goodix_send_pack(FpiSsm *ssm, guint8 flags, guint8 *payload,
                      guint16 payload_len, GDestroyNotify payload_destroy);

void goodix_send_protocol(FpiSsm *ssm, guint8 cmd, guint8 *payload,
                          guint16 payload_len, GDestroyNotify payload_destroy,
                          gboolean calc_checksum, GCallback callback,
                          gpointer user_data);

void goodix_send_nop(FpiSsm *ssm);

void goodix_send_mcu_get_image(FpiSsm *ssm);

void goodix_send_mcu_switch_to_fdt_down(
    FpiSsm *ssm, guint8 *mode, guint16 mode_len, GDestroyNotify mode_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data);

void goodix_send_mcu_switch_to_fdt_up(
    FpiSsm *ssm, guint8 *mode, guint16 mode_len, GDestroyNotify mode_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data);

void goodix_send_mcu_switch_to_fdt_mode(
    FpiSsm *ssm, guint8 *mode, guint16 mode_len, GDestroyNotify mode_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data);

void goodix_send_nav_0(FpiSsm *ssm,
                       void (*callback)(guint8 *data, guint16 data_len,
                                        gpointer user_data, GError **error),
                       gpointer user_data);

void goodix_send_mcu_switch_to_idle_mode(FpiSsm *ssm, guint8 sleep_time);

void goodix_send_write_sensor_register(FpiSsm *ssm, guint16 address,
                                       guint16 value);

void goodix_send_read_sensor_register(
    FpiSsm *ssm, guint16 address, guint8 length,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data);

void goodix_send_upload_config_mcu(
    FpiSsm *ssm, guint8 *config, guint16 config_len,
    GDestroyNotify config_destroy,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data);

void goodix_send_set_powerdown_scan_frequency(
    FpiSsm *ssm, guint16 powerdown_scan_frequency,
    void (*callback)(guint8 *data, guint16 data_len, gpointer user_data,
                     GError **error),
    gpointer user_data);

void goodix_send_enable_chip(FpiSsm *ssm, gboolean enable);

void goodix_send_reset(FpiSsm *ssm, gboolean reset_sensor, guint8 sleep_time,
                       void (*callback)(guint8 *data, guint16 data_len,
                                        gpointer user_data, GError **error),
                       gpointer user_data);

void goodix_send_firmware_version(FpiSsm *ssm,
                                  GoodixFirmwareVersionCallback callback,
                                  gpointer user_data);

void goodix_send_query_mcu_state(FpiSsm *ssm,
                                 void (*callback)(guint8 *data,
                                                  guint16 data_len,
                                                  gpointer user_data,
                                                  GError **error),
                                 gpointer user_data);

void goodix_send_request_tls_connection(FpiSsm *ssm);

void goodix_send_tls_successfully_established(FpiSsm *ssm);

void goodix_send_preset_psk_write_r(FpiSsm *ssm, guint32 address, guint8 *psk,
                                    guint32 psk_len, GDestroyNotify psk_destroy,
                                    void (*callback)(guint8 *data,
                                                     guint16 data_len,
                                                     gpointer user_data,
                                                     GError **error),
                                    gpointer user_data);

void goodix_send_preset_psk_read_r(FpiSsm *ssm, guint32 address, guint32 length,
                                   GoodixPresetPskReadRCallback callback,
                                   gpointer user_data);

// ---- GOODIX SEND SECTION END ----

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
