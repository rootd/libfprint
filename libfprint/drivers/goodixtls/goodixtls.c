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

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>

#include "drivers_api.h"
#include "fp-device.h"
#include "fpi-device.h"
#include "goodixtls.h"

static unsigned int tls_server_psk_server_callback(SSL *ssl,
                                                   const char *identity,
                                                   unsigned char *psk,
                                                   unsigned int max_psk_len) {
  if (sizeof(goodix_511_psk_0) > max_psk_len) {
    fp_dbg("Provided PSK R is too long for OpenSSL");
    return 0;
  }

  psk = (unsigned char *)&goodix_511_psk_0;

  return sizeof(goodix_511_psk_0);
}

static int tls_server_create_socket(int port)
{
    int s;
    struct sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }

    if (bind(s, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        return -1;
    }

    if (listen(s, 1) < 0) {
        return -1;
    }

    return s;
}

static SSL_CTX* tls_server_create_ctx(void)
{
    const SSL_METHOD* method;

    method = SSLv23_server_method();

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        return NULL;
    }

    return ctx;
}

static void tls_server_config_ctx(SSL_CTX* ctx)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);
    SSL_CTX_set_dh_auto(ctx, 1);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(ctx, "ALL");

    SSL_CTX_set_psk_server_callback(ctx, tls_server_psk_server_callback);
}

static gboolean goodix_tls_connect(GoodixTlsServer* self)
{
    struct sockaddr_in addr;
    guint len = sizeof(addr);

    int client_fd = accept(self->sock_fd, (struct sockaddr*) &addr, &len);
    if (client_fd >= 0) {
    }

    GError err;
    err.code = errno;
    err.message = strerror(errno);
    self->connection_callback(self, &err);
    return FALSE;
}
static void goodix_tls_send_handshake(GoodixTlsServer* self)
{
    const char reply[] = "Hello Goodix\n";
    SSL_write(self->ssl_layer, reply, strlen(reply));
}

void goodix_tls_server_send(GoodixTlsServer* self, guint8* data, guint16 length)
{
    SSL_write(self->ssl_layer, data, length * sizeof(guint8));
}
void goodix_tls_server_receive(GoodixTlsServer* self, guint8* data,
                               guint16 length)
{
    SSL_read(self->serve_ssl, data, length * sizeof(guint8));
}

static void* goodix_tls_init_cli(void* me)
{
    GoodixTlsServer* self = me;
    self->ssl_layer = SSL_new(self->ssl_ctx);
    SSL_set_fd(self->ssl_layer, self->client_fd);
    if (SSL_connect(self->ssl_layer) == 0) {
        self->connection_callback(self, NULL);
    }
    else {
        printf("failed to connect: %s\n", strerror(errno));
    }
    return NULL;
}
static void* goodix_tls_init_serve(void* me)
{
    GoodixTlsServer* self = me;
    self->serve_ssl = SSL_new(self->ssl_ctx);
    SSL_set_fd(self->serve_ssl, self->sock_fd);
    if (SSL_accept(self->serve_ssl) != 0) {
        printf("failed to accept: %s\n", strerror(errno));
    }
    return NULL;
}

gboolean goodix_tls_server_deinit(GoodixTlsServer* self, GError** error)
{
    SSL_shutdown(self->ssl_layer);
    SSL_free(self->ssl_layer);

    SSL_shutdown(self->serve_ssl);
    SSL_free(self->serve_ssl);

    close(self->client_fd);
    close(self->sock_fd);

    SSL_CTX_free(self->ssl_ctx);

    return TRUE;
}

gboolean goodix_tls_server_init(GoodixTlsServer* self, guint8* psk,
                                gsize length, GError** error)
{
    // g_assert(self->decoded_callback);
    g_assert(self->connection_callback);
    // g_assert(self->send_callback);
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    self->ssl_ctx = tls_server_create_ctx();

    int socks[2] = {0, 0};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) != 0) {
        g_set_error(error, G_FILE_ERROR, errno,
                    "failed to create socket pair: %s", strerror(errno));
        return FALSE;
    }
    self->sock_fd = socks[0];
    self->client_fd = socks[1];

    if (self->ssl_ctx == NULL) {
        fp_dbg("Unable to create TLS server context\n");
        *error = fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL, "Unable to "
                                                                   "create TLS "
                                                                   "server "
                                                                   "context");
        return FALSE;
    }
    pthread_create(&self->cli_thread, 0, goodix_tls_init_cli, self);
    pthread_create(&self->serve_thread, 0, goodix_tls_init_serve, self);
    return TRUE;
}
