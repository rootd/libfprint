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

typedef FpImage *(*GoodixTls5xxProcessFrameFn)(guint8* pix);
typedef GoodixTls5xxMcuConfig (*GoodixTls5xxGetMcuFn)(void);

struct _FpiDeviceGoodixTls5xxClass
{
  FpiDeviceGoodixTlsClass    parent;

  GoodixTls5xxGetMcuFn       get_mcu_cfg;
  GoodixTls5xxProcessFrameFn process_frame;

  guint16                    scan_width;
  guint16                    scan_height;
};


void goodixtls5xx_scan_start (FpDevice * dev);

void goodixtls5xx_decode_frame (GoodixTls5xxPix * frame,
                                guint32           frame_size,
                                const guint8     *raw_frame);