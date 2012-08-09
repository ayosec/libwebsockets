/*
 * libwebsockets-test-server - libwebsockets test implementation
 *
 * Copyright (C) 2010-2011 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <time.h>


#include "../lib/libwebsockets.h"

static int close_testing;

enum demo_protocols {
  /* always first */
  PROTOCOL_HTTP = 0,

  PROTOCOL_NETCAT_MIRROR,

  /* always last */
  DEMO_PROTOCOL_COUNT
};


#define LOCAL_RESOURCE_PATH INSTALL_DATADIR"/libwebsockets-test-server"

/* this protocol server (always the first one) just knows how to do HTTP */

static int callback_http(struct libwebsocket_context *context,
    struct libwebsocket *wsi,
    enum libwebsocket_callback_reasons reason, void *user,
    void *in, size_t len)
{
  char client_name[128];
  char client_ip[128];

  switch (reason) {
    case LWS_CALLBACK_HTTP:
      fprintf(stderr, "serving HTTP URI %s\n", (char *)in);

      if (in && strcmp(in, "/favicon.ico") == 0) {
        if (libwebsockets_serve_http_file(wsi,
              LOCAL_RESOURCE_PATH"/favicon.ico", "image/x-icon"))
          fprintf(stderr, "Failed to send favicon\n");
        break;
      }

      /* send the script... when it runs it'll start websockets */

      if (libwebsockets_serve_http_file(wsi,
            LOCAL_RESOURCE_PATH"/test.html", "text/html"))
        fprintf(stderr, "Failed to send HTTP file\n");
      break;

      /*
       * callback for confirming to continue with client IP appear in
       * protocol 0 callback since no websocket protocol has been agreed
       * yet.  You can just ignore this if you won't filter on client IP
       * since the default uhandled callback return is 0 meaning let the
       * connection continue.
       */

    case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:

      libwebsockets_get_peer_addresses((int)(long)user, client_name,
          sizeof(client_name), client_ip, sizeof(client_ip));

      fprintf(stderr, "Received network connect from %s (%s)\n",
          client_name, client_ip);

      /* if we returned non-zero from here, we kill the connection */
      break;

    default:
      break;
  }

  return 0;
}


/*
 * this is just an example of parsing handshake headers, you don't need this
 * in your code unless you will filter allowing connections by the header
 * content
 */

  static void
dump_handshake_info(struct lws_tokens *lwst)
{
  int n;
  static const char *token_names[WSI_TOKEN_COUNT] = {
    /*[WSI_TOKEN_GET_URI]           =*/ "GET URI",
    /*[WSI_TOKEN_HOST]              =*/ "Host",
    /*[WSI_TOKEN_CONNECTION]        =*/ "Connection",
    /*[WSI_TOKEN_KEY1]              =*/ "key 1",
    /*[WSI_TOKEN_KEY2]              =*/ "key 2",
    /*[WSI_TOKEN_PROTOCOL]          =*/ "Protocol",
    /*[WSI_TOKEN_UPGRADE]           =*/ "Upgrade",
    /*[WSI_TOKEN_ORIGIN]            =*/ "Origin",
    /*[WSI_TOKEN_DRAFT]             =*/ "Draft",
    /*[WSI_TOKEN_CHALLENGE]         =*/ "Challenge",

    /* new for 04 */
    /*[WSI_TOKEN_KEY]               =*/ "Key",
    /*[WSI_TOKEN_VERSION]           =*/ "Version",
    /*[WSI_TOKEN_SWORIGIN]          =*/ "Sworigin",

    /* new for 05 */
    /*[WSI_TOKEN_EXTENSIONS]        =*/ "Extensions",

    /* client receives these */
    /*[WSI_TOKEN_ACCEPT]            =*/ "Accept",
    /*[WSI_TOKEN_NONCE]             =*/ "Nonce",
    /*[WSI_TOKEN_HTTP]              =*/ "Http",
    /*[WSI_TOKEN_MUXURL]    =*/ "MuxURL",
  };

  for (n = 0; n < WSI_TOKEN_COUNT; n++) {
    if (lwst[n].token == NULL)
      continue;

    fprintf(stderr, "    %s = %s\n", token_names[n], lwst[n].token);
  }
}

/* lws-mirror_protocol */

#define MAX_MESSAGE_QUEUE 64

struct per_session_data__netcat_mirror {
  struct libwebsocket *wsi;
  int pid;
  int pipe_read;
  int pipe_write;
};

void _create_subprocess(struct per_session_data__netcat_mirror* pss) {

  int to_read[2], to_write[2];

  pipe2(to_read, O_NONBLOCK);
  pipe2(to_write, O_NONBLOCK);

  pss->pid = fork();

  if(pss->pid == 0) {
    dup2(to_read[0], 0);
    close(to_read[1]);
    close(to_read[0]);

    dup2(to_write[1], 1);
    close(to_write[1]);
    close(to_write[0]);

    execlp("nc", "nc", "-l", "-p", "2000", (char*)NULL);

    perror("execl");
    exit(-1);
  }

  pss->pipe_write = to_read[1];
  pss->pipe_read = to_write[0];

  close(to_read[0]);
  close(to_write[1]);

  fprintf(stderr, "netcat started at port 2000, pid = %d\n", pss->pid);

}

static int
callback_netcat_mirror(
    struct libwebsocket_context *context,
    struct libwebsocket *wsi,
    enum libwebsocket_callback_reasons reason,
    void *user,
    void *in,
    size_t len)
{
  int n;
  struct per_session_data__netcat_mirror *pss = user;

  switch (reason) {

    case LWS_CALLBACK_ESTABLISHED:
      fprintf(stderr, "callback_lws_mirror: LWS_CALLBACK_ESTABLISHED\n");
      pss->wsi = wsi;

      _create_subprocess(pss);
      break;

#if 0
    case LWS_CALLBACK_SERVER_WRITEABLE:
      if (close_testing)
        break;

      break;
#endif

    case LWS_CALLBACK_BROADCAST:
      /* Check for data to read */
      while((n = read(pss->pipe_read, in, len)) > 0) {
        fprintf(stderr, "Read %d chars\n", n);
        libwebsocket_write(wsi, in, n, LWS_WRITE_TEXT);
      }
      break;

    case LWS_CALLBACK_RECEIVE:
      fprintf(stderr, "Write %d chars\n", (int)len);
      write(pss->pipe_write, in, len);
      break;

    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
      dump_handshake_info((struct lws_tokens *)(long)user);
      /* you could return non-zero here and kill the connection */
      break;

    default:
      break;
  }

  return 0;
}


/* list of supported protocols and callbacks */

static struct libwebsocket_protocols protocols[] = {
  /* first protocol must always be HTTP handler */

  {
    "http-only",            /* name */
    callback_http,          /* callback */
    0                       /* per_session_data_size */
  },
  {
    "netcat-protocol",
    callback_netcat_mirror,
    sizeof(struct per_session_data__netcat_mirror)
  },
  {
    NULL, NULL, 0           /* End of list */
  }
};

static struct option options[] = {
  { "help",       no_argument,            NULL, 'h' },
  { "port",       required_argument,      NULL, 'p' },
  { "ssl",        no_argument,            NULL, 's' },
  { "killmask",   no_argument,            NULL, 'k' },
  { "interface",  required_argument,      NULL, 'i' },
  { "closetest",  no_argument,            NULL, 'c' },
  { NULL, 0, 0, 0 }
};

int main(int argc, char **argv)
{
  int n = 0;
  const char *cert_path = LOCAL_RESOURCE_PATH"/libwebsockets-test-server.pem";
  const char *key_path = LOCAL_RESOURCE_PATH"/libwebsockets-test-server.key.pem";
  unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 1024 + LWS_SEND_BUFFER_POST_PADDING];
  int port = 7681;
  int use_ssl = 0;
  struct libwebsocket_context *context;
  int opts = 0;
  char interface_name[128] = "";
  const char *interface = NULL;

  while (n >= 0) {
    n = getopt_long(argc, argv, "ci:khsp:", options, NULL);
    if (n < 0)
      continue;
    switch (n) {
      case 's':
        use_ssl = 1;
        break;
      case 'k':
        opts = LWS_SERVER_OPTION_DEFEAT_CLIENT_MASK;
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'i':
        strncpy(interface_name, optarg, sizeof interface_name);
        interface_name[(sizeof interface_name) - 1] = '\0';
        interface = interface_name;
        break;
      case 'c':
        close_testing = 1;
        fprintf(stderr, " Close testing mode -- closes on "
            "client after 50 dumb increments"
            "and suppresses lws_mirror spam\n");
        break;
      case 'h':
        fprintf(stderr, "Usage: test-server "
            "[--port=<p>] [--ssl]\n");
        exit(1);
    }
  }

  if (!use_ssl)
    cert_path = key_path = NULL;

  context = libwebsocket_create_context(port, interface, protocols,
      libwebsocket_internal_extensions,
      cert_path, key_path, -1, -1, opts);
  if (context == NULL) {
    fprintf(stderr, "libwebsocket init failed\n");
    return -1;
  }

  buf[LWS_SEND_BUFFER_PRE_PADDING] = 'x';

  /*
   * This example shows how to work with the forked websocket service loop
   */

  fprintf(stderr, " Using forked service loop\n");

  /*
   * This forks the websocket service action into a subprocess so we
   * don't have to take care about it.
   */

  n = libwebsockets_fork_service_loop(context);
  if (n < 0) {
    fprintf(stderr, "Unable to fork service loop %d\n", n);
    return 1;
  }

  while (1) {

    usleep(50000);

    libwebsockets_broadcast(&protocols[PROTOCOL_NETCAT_MIRROR],
        &buf[LWS_SEND_BUFFER_PRE_PADDING], LWS_SEND_BUFFER_PRE_PADDING);
  }

  libwebsocket_context_destroy(context);

  return 0;
}
