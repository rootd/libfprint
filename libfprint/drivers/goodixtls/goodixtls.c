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
#include <openssl/tls1.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>

#include "drivers_api.h"
#include "fp-device.h"
#include "fpi-device.h"
#include "glibconfig.h"
#include "goodix.h"
#include "goodixtls.h"

static GError* err_from_ssl()
{
    GError* err = malloc(sizeof(GError));
    unsigned long code = ERR_get_error();
    err->code = code;
    const char* msg = ERR_reason_error_string(code);
    err->message = malloc(strlen(msg));
    strcpy(err->message, msg);
    return err;
}

static unsigned int tls_server_psk_server_callback(SSL *ssl,
                                                   const char *identity,
                                                   unsigned char *psk,
                                                   unsigned int max_psk_len) {
  if (sizeof(goodix_511_psk_0) > max_psk_len) {
    fp_dbg("Provided PSK R is too long for OpenSSL");
    return 0;
  }
  fp_dbg("PSK WANTED %d", max_psk_len);
  // I don't know why we must use OPENSSL_hexstr2buf but just copying zeros
  // doesn't work
  const char* buff = "000000000000000000000000000000000000000000000000000000000"
                     "0000000";
  long len = 0;
  unsigned char* key = OPENSSL_hexstr2buf(buff, &len);
  memcpy(psk, key, len);
  OPENSSL_free(key);

  return len;
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

    method = TLS_server_method();

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        return NULL;
    }

    return ctx;
}
static SSL_CTX* tls_cli_create_ctx()
{
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    return ctx;
}

static void tls_server_config_ctx(SSL_CTX* ctx)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);
    SSL_CTX_set_dh_auto(ctx, 1);
    SSL_CTX_set_cipher_list(ctx, "ALL");
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    // SSL_CTX_set_cipher_list(ctx, "ALL");
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
    self->connection_callback(self, &err, self->user_data);
    return FALSE;
}
static void goodix_tls_send_handshake(GoodixTlsServer* self)
{
    const char reply[] = "Hello Goodix\n";
    SSL_write(self->ssl_layer, reply, strlen(reply));
}

void goodix_tls_server_send(GoodixTlsServer* self, guint8* data, guint16 length)
{
    fp_dbg("Sending data to goodix tls server");
    SSL_write(self->ssl_layer, data, length * sizeof(guint8));
    // send(self->client_fd, data, length * sizeof(guint8), 0);
}

int goodix_tls_client_send(GoodixTlsServer* self, guint8* data, guint16 length)
{
    return write(self->client_fd, data, length * sizeof(guint8));
}
int goodix_tls_client_recv(GoodixTlsServer* self, guint8* data, guint16 length) {
    return read(self->client_fd, data, length * sizeof(guint8));
}

int goodix_tls_server_receive(GoodixTlsServer* self, guint8* data,
                              guint16 length, GError** error)
{
    fp_dbg("READ START");
    int retr = SSL_read(self->ssl_layer, data, length * sizeof(guint8));
    if (retr <= 0) {
        g_set_error(error, g_io_channel_error_quark(), retr,
                    ""); // err_from_ssl(retr);
    }
    fp_dbg("READ END");
    return retr;
}

static void tls_config_ssl(SSL* ssl)
{
    SSL_set_min_proto_version(ssl, TLS1_2_VERSION);
    SSL_set_max_proto_version(ssl, TLS1_2_VERSION);
    SSL_set_psk_server_callback(ssl, tls_server_psk_server_callback);
    SSL_set_cipher_list(ssl, "ALL");
}

gboolean goodix_tls_init_cli(GoodixTlsServer* self, GError** error)
{
    SSL_CTX* ctx = tls_cli_create_ctx();
    // tls_server_config_ctx(ctx);
    SSL* ssl_layer = SSL_new(ctx);
    tls_config_ssl(ssl_layer);

    int retr = SSL_set_fd(ssl_layer, self->client_fd);

    if (retr > 0) {
        retr = SSL_connect(ssl_layer);
    }

    if (retr <= 0) {
        //*error = err_from_ssl(retr);
    }
    // SSL_free(ssl_layer);
    // SSL_CTX_free(ctx);

    return retr <= 0 ? 0 : 1;
}

static void* goodix_tls_init_serve(void* me)
{
    GoodixTlsServer* self = me;
    self->ssl_layer = SSL_new(self->ssl_ctx);
    tls_config_ssl(self->ssl_layer);
    SSL_set_fd(self->ssl_layer, self->sock_fd);
    fp_dbg("TLS server waiting to accept...");
    int retr = SSL_accept(self->ssl_layer);
    fp_dbg("TLS server accept done");
    if (retr <= 0) {
        self->connection_callback(self, err_from_ssl(), self->user_data);
    }
    else {
        g_assert(self->connection_callback);
        const char* msg = "HELLO WORLD";
        // SSL_write(self->ssl_layer, msg, strlen(msg));

        /*char buff[1024];
        int qty = SSL_read(self->ssl_layer, buff, sizeof(buff) - 1);
        if (qty <= 0) {
            self->connection_callback(
                self,
                g_error_new(G_FILE_ERROR, SSL_get_error(self->ssl_layer, qty),
                            ""),
                self->user_data);
            return NULL;
        }
        buff[qty] = 0;*/

        // fp_dbg("Finished tls setup server side, got: %s (%d)", buff, qty);
        self->connection_callback(self, NULL, self->user_data);
    }
    return NULL;
}

gboolean goodix_tls_server_deinit(GoodixTlsServer* self, GError** error)
{
    SSL_shutdown(self->ssl_layer);
    SSL_free(self->ssl_layer);

    // SSL_shutdown(self->ssl_layer);
    // SSL_free(self->ssl_layer);

    close(self->client_fd);
    close(self->sock_fd);

    SSL_CTX_free(self->ssl_ctx);

    return TRUE;
}

gboolean goodix_tls_server_init(GoodixTlsServer* self, GError** error)
{
    // g_assert(self->decoded_callback);
    g_assert(self->connection_callback);
    // g_assert(self->send_callback);
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    SSL_library_init();
    self->ssl_ctx = tls_server_create_ctx();
    tls_server_config_ctx(self->ssl_ctx);

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
    pthread_create(&self->serve_thread, 0, goodix_tls_init_serve, self);

    return TRUE;
}
