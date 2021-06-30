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

#pragma once

#define GOODIX_TLS_SERVER_PORT 4433

//#define TLS_PSK_WITH_AES_128_GCM_SHA256  ((const unsigned char *)"\x00\xa8")

static const char *psk_key =
    "ba1a86037c1d3c71c3af344955bd69a9a9861d9e911fa24985b677e8dbd72d43";

SSL_CTX *tls_server_create_ctx(void);

int tls_server_create_socket(int port);

void tls_server_config_ctx(void);

__attribute__((__noreturn__)) void *tls_server_loop(void *arg);

void tls_server_stop(void);

void *tls_server_handshake_loop(void *arg);

void tls_server_handshake_init(void);

void tls_server_init(FpiSsm *ssm);
