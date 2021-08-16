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
#include "goodixtls.h"

int sock;
FpiSsm *fpi_ssm;
pthread_t server;
SSL_CTX *ctx;

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

int tls_server_create_socket(int port) {
  int s;
  struct sockaddr_in addr;

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    fp_dbg("Unable to create TLS server socket");
    fpi_ssm_mark_failed(fpi_ssm, fpi_device_error_new_msg(
                                     FP_DEVICE_ERROR_GENERAL,
                                     "Unable to create TLS server socket"));
    return -1;
  }

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fp_dbg("Unable to bind to TLS server socket");
    fpi_ssm_mark_failed(fpi_ssm, fpi_device_error_new_msg(
                                     FP_DEVICE_ERROR_GENERAL,
                                     "Unable to bind to TLS server socket"));
    return -1;
  }

  if (listen(s, 1) < 0) {
    fp_dbg("Unable to listen to TLS server socket");
    fpi_ssm_mark_failed(fpi_ssm, fpi_device_error_new_msg(
                                     FP_DEVICE_ERROR_GENERAL,
                                     "Unable to listen to TLS server socket"));
    return -1;
  }

  return s;
}

SSL_CTX *tls_server_create_ctx(void) {
  const SSL_METHOD *method;

  method = SSLv23_server_method();

  ctx = SSL_CTX_new(method);
  if (!ctx) {
    return NULL;
  }

  return ctx;
}

void tls_server_config_ctx(void) {
  SSL_CTX_set_ecdh_auto(ctx, 1);
  SSL_CTX_set_dh_auto(ctx, 1);
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_cipher_list(ctx, "ALL");

  SSL_CTX_set_psk_server_callback(ctx, tls_server_psk_server_callback);
}

void *tls_server_loop(void *arg) {
  // Handle connections
  while (1) {
    struct sockaddr_in addr;
    guint len = sizeof(addr);
    SSL *ssl;
    const char reply[] = "Hello Goodix\n";

    int client = accept(sock, (struct sockaddr *)&addr, &len);
    if (client < 0) {
      printf("%s\n", strerror(errno));
      fp_dbg("TLS server unable to accept request");
      kill(getpid(), SIGKILL);
      // exit(EXIT_FAILURE);
    }

    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, client);

    if (SSL_accept(ssl) <= 0) {
      ERR_print_errors_fp(stderr);
    } else {
      SSL_write(ssl, reply, strlen(reply));
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(client);

    kill(getpid(), SIGKILL);
  }
}

void tls_server_stop(void) {
  close(sock);
  SSL_CTX_free(ctx);
}

void *tls_server_handshake_loop(void *arg) {
  struct sockaddr_in addr;
  guint len = sizeof(addr);
  SSL *ssl;

  int client = accept(sock, (struct sockaddr *)&addr, &len);
  if (client < 0) {
    printf("%s\n", strerror(errno));
    fp_dbg("TLS server unable to accept socket request");
  } else {
    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, client);

    if (SSL_accept(ssl) <= 0) {
      printf("%s\n", strerror(errno));
      fp_dbg("TLS server unable to accept handshake request");
    }
  }
  return 0;
}

void tls_server_handshake_init(void) {
  int err = pthread_create(&server, NULL, &tls_server_handshake_loop, NULL);
  if (err != 0) {
    fp_dbg("Unable to create TLS server thread");
    fpi_ssm_mark_failed(fpi_ssm, fpi_device_error_new_msg(
                                     FP_DEVICE_ERROR_GENERAL,
                                     "Unable to create TLS server thread"));
  } else {
    fp_dbg("TLS server thread created");
    fpi_ssm_next_state(fpi_ssm);
  }
}

void tls_server_init(FpiSsm *ssm) {
  fpi_ssm = ssm;

  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();

  ctx = tls_server_create_ctx();

  if (ctx == NULL) {
    fp_dbg("Unable to create TLS server context");
    fpi_ssm_mark_failed(
        ssm, fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                      "Unable to create TLS server context"));
    return;
  }

  tls_server_config_ctx();

  sock = tls_server_create_socket(GOODIX_TLS_SERVER_PORT);
  if (sock == -1) {
    fp_dbg("Unable to create TLS server socket");
    fpi_ssm_mark_failed(
        ssm, fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                      "Unable to create TLS server context"));
    return;
  }

  fpi_ssm_next_state(ssm);
}
