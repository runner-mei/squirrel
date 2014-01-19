
#include "squirrel_config.h"

#include <assert.h>

#include "internal.h"

#ifdef __cplusplus
extern "C" {
#endif


#define SHTTP_STATUS_OK  200

/*****************************************************************************
 * HTTP Server Callback Functions
 ****************************************************************************/
  
static void _shttp_on_writed(uv_write_t* req, int status);


static void _shttp_on_null_disconnect(uv_handle_t* server_handle) {
  sl_free(server_handle);
}

static void _shttp_on_null_connect(uv_stream_t* server_handle, int status) {
  int          rc;
  uv_handle_t *handle;

  switch (server_handle->type) {
  case UV_TCP:
    handle = (uv_handle_t *)sl_calloc(1, sizeof(uv_tcp_t));
    uv_tcp_init(server_handle->loop, (uv_tcp_t *)handle);
    break;
  case UV_NAMED_PIPE:
    handle = (uv_handle_t *)sl_calloc(1, sizeof(uv_pipe_t));
    uv_pipe_init(server_handle->loop, (uv_pipe_t *)handle, 0);
    break;
  default:
    assert(server_handle->type != UV_TCP);
  }

  rc = uv_accept(server_handle, (uv_stream_t*)handle);
  if(rc) {
    ERR("accept: %s.", uv_strerror(rc));
    goto end;
  }
  uv_close(handle, &_shttp_on_null_disconnect);
end:
  sl_free(handle);
}


#define call_hooks_after_writed(inner_conn)                                \
  for(i = 0; i < inner_conn->outgoing.call_after_writed.length; i ++) {    \
    inner_conn->outgoing.call_after_writed.array[0].cb(&inner_conn->inner, \
      inner_conn->outgoing.call_after_writed.array[0].data);               \
  }                                                                        \
  inner_conn->outgoing.call_after_writed.length = 0

static void _shttp_on_disconnect(uv_handle_t* handle) {
  shttp_connection_internal_t *inner;
  shttp_t            *http;
  inner = (shttp_connection_internal_t *)handle->data;
  http = inner->inner.http;

  TAILQ_REMOVE(&http->connections, inner, next);
  TAILQ_INSERT_TAIL(&http->free_connections, inner, next);
}

static void _shttp_on_head_writed(uv_write_t* req, int status) {
  shttp_connection_internal_t *inner;
  
  inner = (shttp_connection_internal_t*)req->data;
  if (status < 0) {
    ERR("write: %s", uv_strerror(status));
    uv_close((uv_handle_t*) &inner->uv_handle, &_shttp_on_disconnect);
    return;
  }
  inner->outgoing.head_write_buffers.length = 0;
  _shttp_response_send_ready_data(inner,
    &_shttp_on_disconnect,
    &_shttp_on_head_writed,
    &_shttp_on_writed);
}

static void _shttp_on_writed(uv_write_t* req, int status) {
  shttp_connection_internal_t *inner;
  int                         i;
  
  inner = (shttp_connection_internal_t*)req->data;
  if (status < 0) {
    ERR("write: %s", uv_strerror(status));
    uv_close((uv_handle_t*) &inner->uv_handle, &_shttp_on_disconnect);
    return;
  }
  
  inner->outgoing.body_write_buffers.length = 0;
  call_hooks_after_writed(inner);
}

int _shttp_connection_on_message_begin(shttp_connection_internal_t* conn) {
  return conn->callbacks->on_message_begin(&conn->inner);
}

int _shttp_connection_on_body (shttp_connection_internal_t* conn, const char *at, size_t length) {
  return conn->callbacks->on_body(&conn->inner, at, length);
}

int _shttp_connection_on_message_complete (shttp_connection_internal_t* conn) {
  int   res, i;
  spool_init(&conn->pool, ((char*)conn->arena_base) + conn->arena_offset,
    conn->arena_capacity - conn->arena_offset);
  res = conn->callbacks->on_message_complete(&conn->inner);
  if(0 != res) {
    ERR("callback: result of callback is %d.", res);
    call_hooks_after_writed(conn);
    uv_close((uv_handle_t*) &conn->uv_handle, &_shttp_on_disconnect);
    return 0;
  }
  _shttp_response_send_ready_data(conn,
    &_shttp_on_disconnect, 
    &_shttp_on_head_writed, 
    &_shttp_on_writed);
  return 0;
}

static void _shttp_on_connection_alloc(uv_handle_t* req, size_t suggested_size, uv_buf_t* buf) {
  shttp_connection_internal_t *inner;
  size_t                      offset;

  inner = (shttp_connection_internal_t*)req->data;
  offset = inner->arena_offset;

  if(offset == inner->arena_capacity) {
    buf->base = 0;
    buf->len = 0;
    return;
  }
  buf->base = ((char*)inner->arena_base) + offset;
  buf->len = (unsigned long)(inner->arena_capacity - offset);
}

static void _shttp_on_connection_read(uv_stream_t* tcp, ssize_t nread, const uv_buf_t* buf) {
  size_t                      parsed;
  shttp_connection_internal_t *inner;

  inner = (shttp_connection_internal_t*)tcp->data;
  if (nread <= 0) {
    // No Data Read... Handle Error case
    if (0 != nread && UV_EOF != nread) {
      ERR("read: %s", uv_strerror((int)nread));
    }
    uv_close((uv_handle_t*) &inner->uv_handle, &_shttp_on_disconnect);
    return;
  }


  parsed = http_parser_execute(&inner->incomming.parser, &inner->inner.http->parser_settings, buf->base, nread);
  if (nread != parsed) {
    if(HPE_OK != HTTP_PARSER_ERRNO(&inner->incomming.parser)) {
      ERR("parse error:%s", http_errno_name(HTTP_PARSER_ERRNO(&inner->incomming.parser)));
    } else {
      ERR("parse error: parsed is 0.");
    }
    uv_close((uv_handle_t*) &inner->uv_handle, &_shttp_on_disconnect);
    return;
  }
  if (0 == inner->incomming.head_reading) {
    if (0 == inner->incomming.headers_bytes_count) {
      inner->arena_offset += nread;
    } else {
      inner->arena_offset = shttp_align(inner->incomming.headers_bytes_count, shttp_cacheline_size);
    }
  }
  return;
}

void _shttp_on_connect(uv_stream_t* server_handle, int status) {
  int                         rc;
  shttp_listening_t           *listening;
  shttp_connection_internal_t *inner;
  shttp_t                     *http;

  if(-1 == status) {
    ERR("accept: %s", uv_strerror(status));
    return;
  }

  listening = (shttp_listening_t *)server_handle->data;
  http = listening->http;
  if(TAILQ_EMPTY(&http->free_connections)) {
    goto failed;
  }

  inner = TAILQ_LAST(&http->free_connections, shttp_connections_s);
  inner->arena_offset = 0;
  uv_tcp_init(http->uv_loop, &inner->uv_handle);
  inner->uv_handle.data = inner;


  memset(&inner->inner.request, 0, sizeof(shttp_request_t));
  memset(&inner->inner.response, 0, sizeof(shttp_response_t));
  assert(inner->inner.http == http);
  assert(inner->inner.internal == inner);
  assert(inner->inner.pool == &inner->pool);

  assert(((char*)(inner->incomming.headers.array)) ==(((char*)inner) + shttp_mem_align(sizeof(shttp_connection_internal_t)) +
         shttp_mem_align(http->settings.user_ctx_size)));
  assert(inner->incomming.headers.capacity == http->settings.max_headers_count);
  assert(inner->callbacks == &inner->inner.http->settings.callbacks);
  assert(inner->incomming.conn == inner->inner.internal);


  inner->incomming.headers.length = 0;
  inner->incomming.headers_bytes_count = 0;
  inner->incomming.status = shttp_message_begin;
  http_parser_init(&inner->incomming.parser, HTTP_REQUEST);
  inner->incomming.parser.data = &inner->incomming;

  assert(((char*)inner->outgoing.headers.array) == (((char*)inner) + shttp_mem_align(sizeof(shttp_connection_internal_t)) +
         shttp_mem_align(http->settings.user_ctx_size) +
         (sizeof(shttp_kv_t) * http->settings.max_headers_count)));
  assert(inner->outgoing.headers.capacity == http->settings.max_headers_count);
  assert(inner->outgoing.head_write_buffers.capacity == 32);
  assert(inner->outgoing.body_write_buffers.capacity == 32);
  assert(inner->outgoing.call_after_writed.capacity == 32);
  assert(inner->outgoing.content_type.len == 0);
  assert(inner->outgoing.content_type.str == nil);
  
  inner->outgoing.write_req.data = inner;
  inner->outgoing.head_write_buffers.length = 0;
  inner->outgoing.body_write_buffers.length = 0;
  inner->outgoing.call_after_writed.length = 0;
  inner->inner.response.headers.array = inner->outgoing.headers.array;
  inner->inner.response.headers.length = 0;
  inner->inner.response.http_major = inner->inner.request.http_major;
  inner->inner.response.http_minor = inner->inner.request.http_minor;
  inner->inner.response.status_code = SHTTP_STATUS_OK;
  inner->inner.response.chunked = SHTTP_THUNKED_NONE;
  inner->inner.response.close_connection = SHTTP_CLOSE_CONNECTION_NONE;

  rc = uv_accept(server_handle, (uv_stream_t*)&inner->uv_handle);
  if(rc) {
    ERR("accept: %s\n", uv_strerror(rc));
    return ;
  }
  TAILQ_REMOVE(&http->free_connections, inner, next);
  TAILQ_INSERT_TAIL(&http->connections, inner, next);

  rc = uv_read_start((uv_stream_t*)&inner->uv_handle,
                     _shttp_on_connection_alloc,
                     _shttp_on_connection_read);

  if(rc) {
    ERR("accept: %s", uv_strerror(rc));
    uv_close((uv_handle_t*)&inner->uv_handle, &_shttp_on_disconnect);
    return ;
  }

  return ;
failed:
  _shttp_on_null_connect(server_handle, status);
  return ;
}

#ifdef __cplusplus
};
#endif