#include "squirrel_config.h"
#include <stdlib.h>
#include "uv.h"
//#include "http_parser.h"
#include "internal.h"
#include "squirrel_test.h"
#include "squirrel_http.h"

#define RECV_TIMEOUT 1
#define TEST_PORT 8456

static void startup(void) {
#ifdef _WIN32
  struct WSAData wsa_data;
  int r = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  assert(r == 0);
#endif
}

enum select_mode {
  select_read  = 1,
  select_write = 2,
  select_error = 4
};

bool socket_set_option (uv_os_sock_t sock,
                        int level,
                        int option,
                        void *optval,
                        int optlen) {
  return ( SOCKET_ERROR != setsockopt (sock, level,
                                       option, (char *) optval, optlen) );
}

bool socket_get_option (uv_os_sock_t sock,
                        int level,
                        int option,
                        void *optval,
                        int *optlen) {
  return ( SOCKET_ERROR != getsockopt (sock, level,
                                       option, (char *) optval, optlen) );
}

bool socket_enable (uv_os_sock_t sock, int value) {
  u_long nonblock = 1;
  return ( 0 ==::ioctlsocket (sock,
                              value,
                              &nonblock));
}

bool socket_disable (uv_os_sock_t sock, int value) {
  u_long nonblock = 0;
  return ( 0 == ioctlsocket (sock,
                             value,
                             &nonblock));
}

bool socket_poll(uv_os_sock_t sock, const TIMEVAL& time_val, int mode) {
  fd_set socket_set;
  FD_ZERO( &socket_set );
  FD_SET(sock, &socket_set );

  return ( 1 == ::select( 0, (mode & select_read)?&socket_set:NULL
                          , (mode & select_write)?&socket_set:NULL
                          , (mode & select_error)?&socket_set:NULL
                          , &time_val ) );
}

bool socket_isReadable(uv_os_sock_t sock) {
  TIMEVAL time_val;
  time_val.tv_sec = 0;
  time_val.tv_usec = 0;
  return socket_poll(sock, time_val, select_read );
}

bool socket_isWritable(uv_os_sock_t sock) {
  TIMEVAL time_val;
  time_val.tv_sec = 0;
  time_val.tv_usec = 0;
  return socket_poll(sock, time_val, select_write );
}

void socket_setBlocking(uv_os_sock_t sock, bool val) {
  if( val )
    socket_enable(sock, FIONBIO);
  else
    socket_disable(sock, FIONBIO);
}

bool send_n(uv_os_sock_t sock, const char* buf, size_t length) {
  do {
#pragma warning(disable: 4267)
    int n = ::send(sock, buf, length, 0 );
#pragma warning(default: 4267)
    if( 0 >= n)
      return false;

    length -= n;
    buf += n;
  } while( 0 < length );

  return true;
}

bool recv_n(uv_os_sock_t sock, char* buf, size_t length) {
  do {
#pragma warning(disable: 4267)
    int n = ::recv(sock, buf, length, 0 );
#pragma warning(default: 4267)

    if( 0 >= n)
      return false;

    length -= n;
    buf += n;
  } while( 0 < length );

  return true;
}
//
//bool sendv_n(uv_os_sock_t sock, uv_buf_t* wsaBuf, size_t size) {
//  uv_buf_t* p = wsaBuf;
//
//  do {
//    DWORD numberOfBytesSent =0;
//#pragma warning(disable: 4267)
//    if( SOCKET_ERROR == ::WSASend(sock, p, size, &numberOfBytesSent,0,0 , 0 ) )
//#pragma warning(default: 4267)
//      return false;
//
//    do {
//      if( numberOfBytesSent < p->len) {
//        p->len -= numberOfBytesSent;
//        p->base = p->base + numberOfBytesSent;
//        break;
//      }
//      numberOfBytesSent -= p->len;
//      ++ p;
//      -- size;
//    } while( 0 < numberOfBytesSent );
//  } while( 0 < size );
//
//  return true;
//}
//
//bool recvv_n(uv_os_sock_t sock, uv_buf_t* wsaBuf, size_t size) {
//  uv_buf_t* p = wsaBuf;
//
//  do {
//    DWORD numberOfBytesRecvd =0;
//#pragma warning(disable: 4267)
//    if( SOCKET_ERROR == ::WSARecv(sock, p, size, &numberOfBytesRecvd,0,0 , 0 ) )
//#pragma warning(default: 4267)
//      return false;
//
//    do {
//      if( numberOfBytesRecvd < p->len) {
//        p->len -= numberOfBytesRecvd;
//        p->base = p->base + numberOfBytesRecvd;
//        break;
//      }
//      numberOfBytesRecvd -= p->len;
//      ++ p;
//      -- size;
//    } while( 0 < numberOfBytesRecvd );
//  } while( 0 < size );
//
//  return true;
//}


size_t max_recv(uv_os_sock_t sock, char* simple_request, size_t simple_request_len, int timeout) {
  size_t recv_size = 0;
  int n;
  TIMEVAL time_val;

  time_val.tv_sec = timeout;
  time_val.tv_usec = 0;

  while(socket_poll(sock, time_val, select_read )) {
    n = recv(sock, simple_request + recv_size, (int)(simple_request_len - recv_size), 0);
    if(0 >= n) {
      break;
    }
    recv_size += n;
    simple_request[recv_size] =0;
  }
  return recv_size;
}

static uv_os_sock_t create_tcp_socket(void) {
  uv_os_sock_t sock;

  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
#ifdef _WIN32
  assert(sock != INVALID_SOCKET);
#else
  assert(sock >= 0);
#endif

#ifndef _WIN32
  {
    /* Allow reuse of the port. */
    int yes = 1;
    int r = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    ASSERT(r == 0);
  }
#endif

  return sock;
}

#define GET_REQUEST "GET /favicon.ico HTTP/1.1\r\n" \
                  "Host: 0.0.0.0=5000\r\n"          \
                  "User-Agent: Mozilla/5.0 (X11; U; Linux i686; en-US; rv:1.9) Gecko/2008061015 Firefox/3.0\r\n" \
                  "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"                  \
                  "Accept-Language: en-us,en;q=0.5\r\n"                                                          \
                  "Accept-Encoding: gzip,deflate\r\n"                                                            \
                  "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"                                           \
                  "Keep-Alive: 300\r\n"                                                                          \
                  "Connection: keep-alive\r\n"

const char * simple_request= GET_REQUEST "\r\n";

const char * pipeline_requests= GET_REQUEST "\r\n" GET_REQUEST "\r\n";

#define HELLO_WORLD "<html><body>HELLO WORLD!!!</body></html>"

const char *HELLO_WORLD_RESPONSE = "HTTP/1.1 200 OK\r\nabc: abc\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nContent-Length: 40\r\n\r\n<html><body>HELLO WORLD!!!</body></html>";

static cstring_t HTTP_CONTENT_TEXT_HTML = { 9, "text/html"};

typedef struct usr_context_s {
  int                 status;
  void                *data1;
  void                *data2;
} usr_context_t;


void on_write_frist(shttp_connection_t* conn, void* act) {
  usr_context_t *ctx = (usr_context_t*)conn->ctx;
  ctx->data1 = act;
}

void on_write_second(shttp_connection_t* conn, void* act) {
  usr_context_t *ctx = (usr_context_t*)conn->ctx;
  ctx->data2 = act;
}

void on_log(int severity, const char *fmt, va_list ap) {
  vprintf(fmt, ap);
  printf("\r\n");
}

int  on_message_begin (shttp_connection_t* conn) {
  return 0;
}

int  on_body (shttp_connection_t* conn, const char *at, size_t length) {
  return 0;
}

int  on_message_complete (shttp_connection_t* conn) {
  usr_context_t *ctx = (usr_context_t*)conn->ctx;
  ctx->status = 0;
  shttp_response_start(conn, 200, HTTP_CONTENT_TEXT_HTML.str, HTTP_CONTENT_TEXT_HTML.len);
  shttp_response_write_header(conn, "abc", 3, "abc", 3);
  shttp_response_write(conn, HELLO_WORLD, strlen(HELLO_WORLD), &on_write_frist, nil);
  shttp_response_end(conn);
  return 0;
}



int  on_message_complete_muti_write(shttp_connection_t* conn) {
  usr_context_t *ctx = (usr_context_t*)conn->ctx;
  ctx->status = 0;

  shttp_response_start(conn, 200, HTTP_CONTENT_TEXT_HTML.str, HTTP_CONTENT_TEXT_HTML.len);
  shttp_response_write_header(conn, "abc", 3, "abc", 3);
  shttp_response_write(conn, HELLO_WORLD, strlen(HELLO_WORLD) -3, on_write_frist,  nil);
  shttp_response_write(conn, HELLO_WORLD + (strlen(HELLO_WORLD) -3), 3, on_write_second, nil);
  shttp_response_end(conn);
  return 0;
}


void start_web(void *arg) {
  shttp_t  *srv = (shttp_t*)arg;
  assert(SHTTP_RES_OK == shttp_run(srv));
}

uv_os_sock_t connect_tcp(const char* remote, int port) {
  struct sockaddr_in     addr;
  uv_os_sock_t           sock;

  sock = create_tcp_socket();
  assert(0 == uv_ip4_addr(remote, port, &addr));
  sock = create_tcp_socket();
  assert(0 == connect(sock, (sockaddr*) &addr, sizeof(struct sockaddr_in)));

  return sock;
}

//void is_valid_response(const char* response, size_t response_len) {
//  http_parser_settings settings;
//  http_parser parser;
//
//  settings.on_url = test_on_url;
//  settings.on_header_field = test_on_header_field;
//
//  http_parser_init(parser, HTTP_REQUEST);
//  parser->data = my_socket;
//}

#define WEB_DECL()                                                 \
  shttp_settings_t settings;                                       \
  shttp_t          *srv;                                           \
  uv_thread_t      tid

#define WEB_INIT()                                                 \
  memset(&settings, 0, sizeof(shttp_settings_t));                  \
  settings.user_ctx_size = sizeof(usr_context_t);                  \
  shttp_set_log_callback(&on_log);                                 \
  settings.callbacks.on_message_begin = &on_message_begin;         \
  settings.callbacks.on_body = &on_body;                           \
  settings.callbacks.on_message_complete = &on_message_complete



#define WEB_START()                                                 \
  ASSERT_NE(nil, (srv = shttp_create(&settings)));                  \
  ASSERT_EQ(SHTTP_RES_OK, shttp_listen_at(srv,                      \
                         "tcp4", "0.0.0.0", TEST_PORT));            \
  uv_thread_create(&tid, &start_web, srv)



#define WEB_STOP()                                                 \
  ASSERT_EQ(SHTTP_RES_OK, shttp_shutdown(srv));                    \
  uv_thread_join(&tid);                                            \
  shttp_free(srv)

TEST(http, create_and_free) {
  WEB_DECL();
  WEB_INIT();
  WEB_START();
  WEB_STOP();
}

TEST(http, simple) {
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;

  WEB_INIT();
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, simple_request, strlen(simple_request)));
  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( s, strlen(HELLO_WORLD_RESPONSE));
  ASSERT_EQ( 0, strcmp(buf, HELLO_WORLD_RESPONSE));
  closesocket(sock);

  WEB_STOP();
}


TEST(http, large_headers) {
  // large_headers will realloc memory while reading headers, and relocate buffers
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;

  WEB_INIT();
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, GET_REQUEST, strlen(GET_REQUEST)));
  for(s =0; s < 200; s ++) {
    send_n(sock, "h234567890: 345678\r\n", 20);
  }
  send_n(sock, "\r\n", 2);

  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( s, strlen(HELLO_WORLD_RESPONSE));
  ASSERT_EQ( 0, strcmp(buf, HELLO_WORLD_RESPONSE));
  closesocket(sock);

  WEB_STOP();
}

TEST(http, pipeline_request) {
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;

  WEB_INIT();
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, pipeline_requests, strlen(pipeline_requests)));
  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( 0, strncmp(buf, HELLO_WORLD_RESPONSE, strlen(HELLO_WORLD_RESPONSE)));
  ASSERT_EQ( 0, strncmp(buf + strlen(HELLO_WORLD_RESPONSE), HELLO_WORLD_RESPONSE, strlen(HELLO_WORLD_RESPONSE)));
  closesocket(sock);

  WEB_STOP();
}

TEST(http, pipeline_request_while_two_read) {
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;

  WEB_INIT();
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, pipeline_requests, strlen(pipeline_requests)-12));
  ASSERT_EQ(true, send_n(sock, pipeline_requests + (strlen(pipeline_requests)-12), 12));

  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( 0, strncmp(buf, HELLO_WORLD_RESPONSE, strlen(HELLO_WORLD_RESPONSE)));
  ASSERT_EQ( 0, strncmp(buf + strlen(HELLO_WORLD_RESPONSE), HELLO_WORLD_RESPONSE, strlen(HELLO_WORLD_RESPONSE)));
  closesocket(sock);

  WEB_STOP();
}

TEST(http, reuse_connect) {
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;


  WEB_INIT();
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, simple_request, strlen(simple_request)));
  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( s, strlen(HELLO_WORLD_RESPONSE));
  ASSERT_EQ( 0, strcmp(buf, HELLO_WORLD_RESPONSE));


  ASSERT_EQ(true, send_n(sock, simple_request, strlen(simple_request)));
  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( s, strlen(HELLO_WORLD_RESPONSE));
  ASSERT_EQ( 0, strcmp(buf, HELLO_WORLD_RESPONSE));

  closesocket(sock);


  WEB_STOP();
}

TEST(http, muti_write) {
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;

  WEB_INIT();
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, simple_request, strlen(simple_request)));
  s = max_recv(sock, buf, 2048, RECV_TIMEOUT);
  ASSERT_EQ( s, strlen(HELLO_WORLD_RESPONSE));
  ASSERT_EQ( 0, strcmp(buf, HELLO_WORLD_RESPONSE));
  closesocket(sock);

  WEB_STOP();
}

void on_message_send (shttp_connection_t* conn, void *act) {
  usr_context_t *ctx = (usr_context_t*)act;

  switch(ctx->status) {
  case 0:
    shttp_response_start(conn, 200, HTTP_CONTENT_TEXT_HTML.str, HTTP_CONTENT_TEXT_HTML.len);
    shttp_response_write_header(conn, "abc", 3, "abc", 3);
    shttp_response_write(conn, "abc", 3, &on_message_send, act);
    break;
  case 1:
    shttp_response_write(conn, "abc", 3, &on_message_send, act);
    break;
  case 2:
    shttp_response_end(conn);
    break;
  }
}

int on_message_complete_not_thunked(shttp_connection_t* conn) {
  usr_context_t *ctx = (usr_context_t*)conn->ctx;
  ctx->status = 0;
  on_message_send(conn, ctx);
  return 0;
}


TEST(http, end_not_call_with_not_thunked) {
  WEB_DECL();
  uv_os_sock_t     sock;
  char             buf[2048];
  size_t           s;

  WEB_INIT();
  settings.callbacks.on_message_complete = &on_message_complete_not_thunked;
  WEB_START();

  sock = connect_tcp("127.0.0.1", TEST_PORT);
  ASSERT_EQ(true, send_n(sock, simple_request, strlen(simple_request)));
  s = max_recv(sock, buf, 2048, 2);
  ASSERT_EQ( s, strlen(BODY_NOT_COMPLETE));
  ASSERT_EQ(0, strcmp(buf, BODY_NOT_COMPLETE));
  closesocket(sock);

  WEB_STOP();
}