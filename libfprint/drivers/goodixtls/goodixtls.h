#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
    
#include "drivers_api.h"

#pragma once

FpiSsm *fpi_ssm;
pthread_t server;
int sock;
SSL_CTX *ctx;

static const char *psk_key = "ba1a86037c1d3c71c3af344955bd69a9a9861d9e911fa24985b677e8dbd72d43";

#define GOODIX_TLS_SERVER_PORT 4433

//#define TLS_PSK_WITH_AES_128_GCM_SHA256  ((const unsigned char *)"\x00\xa8")

SSL_CTX *TLS_server_create_ctx(void);

int TLS_server_create_socket(int port);

void TLS_server_config_ctx(void);

__attribute__((__noreturn__)) void *TLS_server_loop(void* arg);

void TLS_server_stop(void);

void *TLS_server_handshake_loop(void *arg);

void TLS_server_handshake_init(void);

void TLS_server_init(FpiSsm *ssm);