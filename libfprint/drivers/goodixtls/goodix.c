/*
 * Goodix 5110 driver for libfprint
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
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
#include "goodix.h"

struct _FpiDeviceGoodixTLS
{
  FpImageDevice parent;

  /* device config */
  //   unsigned short dev_type;
  char *fw_ver;
  char *sensor_psk_hash;
  //   void           (*process_frame) (unsigned short *raw_frame,
  //                                    GSList       ** frames);
  /* end device config */

  /* commands */
  const struct goodix_cmd *cmd;
  int cmd_timeout;
  /* end commands */

  /* state */
  gboolean        active;
  gboolean        deactivating;
  unsigned char  *last_read;
  // unsigned char   calib_atts_left;
  // unsigned char   calib_status;
  unsigned short *background;
  // unsigned char   frame_width;
  // unsigned char   frame_height;
  // unsigned char   raw_frame_height;
  // int             num_frames;
  // GSList         *frames;
  /* end state */
};
G_DEFINE_TYPE (FpiDeviceGoodixTLS, fpi_device_goodixtls, FP_TYPE_IMAGE_DEVICE);

static void
goodix_cmd_done (FpiSsm *ssm)
{
  G_DEBUG_HERE ();
  fpi_ssm_next_state (ssm);
}

static void
goodix_cmd_read (FpiSsm *ssm, FpDevice *dev, int response_len)
{
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiUsbTransfer *transfer;
  GCancellable *cancellable = NULL;
  //int response_len = self->cmd->response_len;

  G_DEBUG_HERE ();

  if (self->cmd->response_len == GOODIX_CMD_SKIP_READ)
    {
      fp_dbg ("skipping read, not expecting anything");
      goodix_cmd_done (ssm);
      return;
    }

  g_clear_pointer (&self->last_read, g_free);

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_fill_bulk (transfer,
                              GOODIX_EP_CMD_IN,
                              response_len);

  fpi_usb_transfer_submit (transfer, self->cmd_timeout, cancellable, goodix_cmd_cb, NULL);
}

static void
goodix_cmd_cb (FpiUsbTransfer *transfer, FpDevice *dev,
             gpointer user_data, GError *error)
{
  FpiSsm *ssm = transfer->ssm;
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);

  G_DEBUG_HERE ();

  if (error)
    {
      /* XXX: In the cancellation case we used to not
       *      mark the SSM as failed?! */
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* XXX: We used to reset the device in error cases! */
  if (transfer->endpoint & FPI_USB_ENDPOINT_IN)
    {
      /* just finished receiving */
      self->last_read = g_memdup (transfer->buffer, transfer->actual_length);
      //fp_dbg("%lu", transfer->actual_length);
      // Some devices send multiple replies, so we need to catch them
      // 0xb0 equels the ACK packet
      // Special case: Reading firmware
      if(self->cmd->cmd == read_fw.cmd)
      {
        if(transfer->actual_length == self->cmd->response_len && self->last_read[4] == 0xb0)
        {
          // We got ACK, now wait for the firmware version
          G_DEBUG_HERE ();
          goodix_cmd_read (ssm, dev, self->cmd->response_len_2);
        }
        else
        {
          // Reading the firmware version
          self->fw_ver = g_memdup (&self->last_read[7], self->cmd->response_len_2);
          G_DEBUG_HERE ();
          goodix_cmd_done (ssm);
        }
      }
      // Special case: Reading PSK hash
      else if(self->cmd->cmd == read_psk.cmd)
      {
        if(transfer->actual_length == self->cmd->response_len && self->last_read[4] == 0xb0)
        {
          // We got ACK, now wait for the PSK
          G_DEBUG_HERE ();
          goodix_cmd_read (ssm, dev, self->cmd->response_len_2);
        }
        else
        {
          /*fp_dbg("%lu", transfer->actual_length);
          int i;
          for (i = 16; i < GOODIX_PSK_LEN+16; i++)
          {
            fp_dbg("%02X", self->last_read[i]);
          }*/

          // Reading the PSK
          self->sensor_psk_hash = g_memdup (&self->last_read[16], GOODIX_PSK_LEN);
          G_DEBUG_HERE ();
          goodix_cmd_done (ssm);
        } 
      }
      // Special case: Setting MCU config
      else if(self->cmd->cmd == mcu_set_config.cmd)
      {
        if(transfer->actual_length == self->cmd->response_len && self->last_read[4] == 0xb0)
        {
          // We got ACK, now wait for the PSK
          G_DEBUG_HERE ();
          goodix_cmd_read (ssm, dev, self->cmd->response_len_2);
        }
        else
        {
          G_DEBUG_HERE ();
          goodix_cmd_done (ssm);
        }
      }
      else if(self->cmd->cmd == set_powerdown_scan_frequency.cmd)
      {
        if(transfer->actual_length == self->cmd->response_len && self->last_read[4] == 0xb0)
        {
          // We got ACK, now wait for the second packet
          G_DEBUG_HERE ();
          goodix_cmd_read (ssm, dev, self->cmd->response_len_2);
        }
        else
        {
          G_DEBUG_HERE ();
          goodix_cmd_done (ssm);
        }
      }
      else
      {
        goodix_cmd_done (ssm);
      }
    }
  else
    {
      /* just finished sending */
      G_DEBUG_HERE ();
      goodix_cmd_read (ssm, dev, self->cmd->response_len);
    }
}

static void
goodix_run_cmd (FpiSsm *ssm,
              FpDevice *dev,
              const struct goodix_cmd *cmd,
              int cmd_timeout)
{
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiUsbTransfer *transfer;
  GCancellable *cancellable = NULL;
  
  self->cmd = cmd;

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  
  if(cmd->cmd == mcu_set_config.cmd)
  {
    // We're sending the mcu_set_config command
    fpi_usb_transfer_fill_bulk_full (transfer,
                                   GOODIX_EP_CMD_OUT,
                                   (guint8 *) cmd->cmd_cfg,
                                   GOODIX_CMD_LEN * 5,
                                   NULL);
  }
  else
  {
    // We're sending any other command
    fpi_usb_transfer_fill_bulk_full (transfer,
                                   GOODIX_EP_CMD_OUT,
                                   (guint8 *) cmd->cmd,
                                   GOODIX_CMD_LEN,
                                   NULL);
  }
  
  fpi_usb_transfer_submit (transfer,
                           cmd_timeout,
                           cancellable,
                           goodix_cmd_cb,
                           NULL);
}

/* ------------------------------------------------------------------------------- */

/* ---- ACTIVE SECTION START ---- */

enum activate_states {
  ACTIVATE_NOP1,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP2,
  ACTIVATE_GET_FW_VER,
  ACTIVATE_VERIFY_FW_VER,
  ACTIVATE_READ_PSK,
  ACTIVATE_VERIFY_PSK,
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY1,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY2,
  ACTIVATE_NUM_STATES,
};

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);

  G_DEBUG_HERE ();
  
  //int i;

  switch (fpi_ssm_get_cur_state (ssm))
    {
      // NOP seems to do nothing, but the Windows driver does it in places too
      case ACTIVATE_NOP1:
      case ACTIVATE_NOP2:
        goodix_run_cmd(ssm, dev, &nop, GOODIX_CMD_TIMEOUT);
        break;

      case ACTIVATE_ENABLE_CHIP:
        goodix_run_cmd(ssm, dev, &enable_chip, GOODIX_CMD_TIMEOUT);
        break;

      case ACTIVATE_GET_FW_VER:
        goodix_run_cmd(ssm, dev, &read_fw, GOODIX_CMD_TIMEOUT);
        break;
        
      case ACTIVATE_VERIFY_FW_VER:
        if(strcmp(self->fw_ver, GOODIX_FIRMWARE_VERSION_SUPPORTED) == 0)
        {
          // The firmware version is supported
          fpi_ssm_next_state (ssm);
        }
        else
        {
          // The firmware version is unsupported
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "Unsupported firmware!"));
        }
        break;
        
      case ACTIVATE_READ_PSK:
        goodix_run_cmd(ssm, dev, &read_psk, GOODIX_CMD_TIMEOUT);
        break;
        
      case ACTIVATE_VERIFY_PSK:        
        /*for (i = 0; i < GOODIX_PSK_LEN; i++)
        {
          fp_dbg("%02X", self->sensor_psk_hash[i]);
        }
        
        fp_dbg("-----");
        
        for (i = 0; i < GOODIX_PSK_LEN; i++)
        {
          fp_dbg("%02X", zero_PSK_hash[i]);
        }*/          
        
        // The PSK hash matches the Zero-PSK hash  
        if(memcmp(self->sensor_psk_hash, &zero_PSK_hash, sizeof(&self->sensor_psk_hash)) == 0)
        {
          //fpi_ssm_mark_completed (ssm);
          fpi_ssm_next_state (ssm);
        }
        else
        {
          // The PSK hash doesn't match
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "PSK doesn't match!"));
        }
        break;
        
      case ACTIVATE_SET_MCU_IDLE:
        goodix_run_cmd(ssm, dev, &mcu_set_idle, GOODIX_CMD_TIMEOUT);
        break;
        
      case ACTIVATE_SET_MCU_CONFIG:
        goodix_run_cmd(ssm, dev, &mcu_set_config, GOODIX_CMD_TIMEOUT);
        break;
        
      case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY1:
      case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY2:
        goodix_run_cmd(ssm, dev, &set_powerdown_scan_frequency, GOODIX_CMD_TIMEOUT);
        break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  G_DEBUG_HERE ();

  fpi_image_device_activate_complete (idev, error);

}

static void
goodix_activate (FpImageDevice *dev)
{
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);

  G_DEBUG_HERE ();
  goodix_dev_reset_state (self);

  FpiSsm *ssm =
    fpi_ssm_new (FP_DEVICE (dev), activate_run_state,
                 ACTIVATE_NUM_STATES);

  fpi_ssm_start (ssm, activate_complete);
}

/* ---- ACTIVE SECTION END ---- */

/* ------------------------------------------------------------------------------- */

/* ---- DEV SECTION START ---- */

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;
  // FpiDeviceGoodix *self;

  G_DEBUG_HERE ();

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  // self = FPI_DEVICE_GOODIXTLS (dev);

  fpi_image_device_open_complete (dev, NULL);
}

static void
goodix_dev_reset_state (FpiDeviceGoodixTLS *goodixdev)
{
  G_DEBUG_HERE ();

  goodixdev->cmd = NULL;
  goodixdev->cmd_timeout = GOODIX_CMD_TIMEOUT;

  g_free (goodixdev->last_read);
  goodixdev->last_read = NULL;
}

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);

  G_DEBUG_HERE ();

  goodix_dev_reset_state (self);
  g_free (self->background);
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
dev_activate (FpImageDevice *dev)
{
  G_DEBUG_HERE ();
  goodix_activate (dev);
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  //FpiDeviceGoodix *self = FPI_DEVICE_GOODIX (dev);

  G_DEBUG_HERE ();
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceGoodixTLS *self = FPI_DEVICE_GOODIXTLS (dev);

  G_DEBUG_HERE ();

   if (!self->active)
    /* The device is inactive already, complete the operation immediately. */
     fpi_image_device_deactivate_complete (dev, NULL);
   else
    /* The device is not yet inactive, flag that we are deactivating (and
     * need to signal back deactivation).
     * Note that any running capture will be cancelled already if needed. */
     self->deactivating = TRUE;
}

/* ---- DEV SECTION END ---- */

/* ------------------------------------------------------------------------------- */

/* ---- FPI SECTION START ---- */

static void
fpi_device_goodixtls_init (FpiDeviceGoodixTLS *self)
{
    // nothing to be done here, move along
}

static void
fpi_device_goodixtls_class_init (FpiDeviceGoodixTLSClass *class)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (class);

  dev_class->id = "goodix";
  dev_class->full_name = "Goodix 5110 Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = goodix_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;
  img_class->change_state = dev_change_state;

  // ToDo
  img_class->bz3_threshold = 24;
}

/* ---- FPI SECTION END ---- */
