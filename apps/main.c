
#include "squirrel_config.h"

#include <string.h>

#include "squirrel_http.h"
#include "server.h"

#define HELLO_WORLD "<html><body>HELLO WORLD!!!</body></html>"

static cstring_t HTTP_CONTENT_TEXT_HTML = { 9, "text/html"};
static cstring_t HTTP_HELLO_WORLD = { 9, HELLO_WORLD};

typedef struct context_s {
  shttp_data_cb   on_body;
  shttp_cb        on_message;
} context_t;

void on_log(void* ctx, int severity, const char *fmt, va_list ap) {
  vscprintf(fmt, ap);
  printf("\r\n");
}

static inline int on_http_get(shttp_connection_t* conn, context_t *ctx) {

  return 0;
}

int  on_connected (shttp_connection_t* conn) {
  context_t             * ctx;
  ctx = (context_t*)conn->connection_context;
  memset(ctx, 0, sizeof(context_t));
  return 0;
}


int  on_disconnected (shttp_connection_t* conn) {
}


int  on_message_begin (shttp_connection_t* conn) {
  context_t             * ctx;

  ctx = (context_t*)conn->connection_context;
  ctx->on_body = nil;
  ctx->on_message = &shttp_notfound_handler;

  switch(conn->request.method) {
  case SHTTP_REQ_GET:
    return on_http_get(conn, ctx);
  }
  return 0;
}

int  on_body (shttp_connection_t* conn, const char *at, size_t length) {
  context_t             * ctx;

  ctx = (context_t*)conn->connection_context;
  if(nil == ctx->on_body) {
    return 0;
  }
  return ctx->on_body(conn, at, length);
}

int  on_message_complete (shttp_connection_t* conn) {
  context_t             * ctx;

  ctx = (context_t*)conn->connection_context;
  if(nil == ctx->on_message) {
    return 0;
  }
  return ctx->on_message(conn);
}

int main(int argc, char** argv) {
  shttp_settings_t settings;
  shttp_t          *http;
  shttp_res        rc;
  server_t         srv;

  HTTP_CONTENT_TEXT_HTML.len = strlen(HTTP_CONTENT_TEXT_HTML.str);
  HTTP_HELLO_WORLD.len = strlen(HTTP_HELLO_WORLD.str);

  memset(&settings, 0, sizeof(shttp_settings_t));
  settings.server_context = &srv;
  settings.user_ctx_size = sizeof(context_t);
  shttp_set_log_callback(&on_log, nil);

  settings.callbacks.on_connected = &on_connected;
  settings.callbacks.on_disconnected = &on_disconnected;
  settings.callbacks.on_message_begin = &on_message_begin;
  settings.callbacks.on_body = &on_body;
  settings.callbacks.on_message_complete = &on_message_complete;
  http = shttp_create(&settings);
  if(nil == http) {
    return -1;
  }
  rc = shttp_listen_at(http, "tcp4", "0.0.0.0", 8000);
  if(SHTTP_RES_OK != rc) {
    return -1;
  }
  printf("[http] listen at ':8000'\r\n");
  rc = shttp_run(http);
  if(SHTTP_RES_OK != rc) {
    return -1;
  }
  return 0;
}
