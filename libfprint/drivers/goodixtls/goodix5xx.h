// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <ashenglandelbro@protonmail.com>

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

#include "drivers_api.h"
#include "goodix.h"

#define FPI_TYPE_DEVICE_GOODIXTLS5XX (fpi_device_goodixtls5xx_get_type ())

G_DECLARE_DERIVABLE_TYPE (FpiDeviceGoodixTls5xx, fpi_device_goodixtls5xx, FPI, DEVICE_GOODIXTLS5XX, FpiDeviceGoodixTls);


typedef guint16 GoodixTls5xxPix;


typedef struct
{
  guint16        data_len;
  void (*free_fn)(void *);
  const guint8 * data;
} GoodixTls5xxMcuConfig;

typedef struct
{
  guint16         width;
  guint16         height;
  unsigned char * data;
} GoodixTls5xxImage;

typedef FpImage *(*GoodixTls5xxProcessFrameFn)(guint8 * pix);
typedef GoodixTls5xxMcuConfig (*GoodixTls5xxGetMcuFn)(void);
typedef void (*GoodixTls5xxResetStateFn)(FpDevice *);

struct _FpiDeviceGoodixTls5xxClass
{
  FpiDeviceGoodixTlsClass    parent;

  GoodixTls5xxGetMcuFn       get_mcu_cfg;
  GoodixTls5xxProcessFrameFn process_frame;
  GoodixTls5xxResetStateFn   reset_state;

  guint16                    scan_width;
  guint16                    scan_height;

  const char               * firmware_version; ///< only needed if goodixtls5xx_check_firmware_version() is used

  /// only needed if goodixtls5xx_check_preset_psk_read() is used
  int            psk_flags;
  guint16        psk_len;
  const guint8 * psk;

  int            reset_number; ///< only needed if goodixtls5xx_check_reset() is used
};

void goodixtls5xx_check_reset (FpDevice *dev,
                               gboolean  success,
                               guint16   number,
                               gpointer  user_data,
                               GError   *error);

void goodixtls5xx_check_firmware_version (FpDevice *dev,
                                          gchar    *firmware,
                                          gpointer  user_data,
                                          GError   *error);


void goodixtls5xx_check_preset_psk_read (FpDevice *dev,
                                         gboolean  success,
                                         guint32   flags,
                                         guint8   *psk,
                                         guint16   length,
                                         gpointer  user_data,
                                         GError   *error);

void goodixtls5xx_check_idle (FpDevice *dev,
                              gpointer  user_data,
                              GError   *err);

void goodixtls5xx_check_config_upload (FpDevice *dev,
                                       gboolean  success,
                                       gpointer  user_data,
                                       GError   *error);

void goodixtls5xx_check_powerdown_scan_freq (FpDevice *dev,
                                             gboolean  success,
                                             gpointer  user_data,
                                             GError   *error);

void goodixtls5xx_check_none (FpDevice *dev,
                              gpointer  user_data,
                              GError   *error);

void goodixtls5xx_check_none_cmd (FpDevice *dev,
                                  guint8   *data,
                                  guint16   len,
                                  gpointer  ssm,
                                  GError   *err);

void goodixtls5xx_scan_start (FpiDeviceGoodixTls5xx * dev);

void goodixtls5xx_decode_frame (GoodixTls5xxPix * frame,
                                guint32           frame_size,
                                const guint8     *raw_frame);


void goodixtls5xx_init_tls (FpDevice * dev);

gboolean goodixtls5xx_save_image_to_pgm (FpImage    *img,
                                         const char *path);

/**
 * @brief Squashes the 12 bit pixels of a raw frame into the 4 bit pixels used
 * by libfprint.
 * @details Borrowed from the elan driver. We reduce frames to
 * within the max and min.
 *
 * @param frame
 * @param squashed
 */
void goodixtls5xx_squash_frame_linear (GoodixTls5xxPix *frame,
                                       guint8          *squashed,
                                       guint16          frame_size);