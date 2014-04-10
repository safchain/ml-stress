/*
 * server.c
 *
 *  Created on: 20 mars 2014
 *      Author: Sylvain Afchain <safchain@gmail.com>
 */
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>

uint64_t total_bytes_read = 0;
uint64_t total_messages_read = 0;

static void set_tcp_no_delay(evutil_socket_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void srv_read_cb(struct bufferevent *bev, void *ctx) {
    struct evbuffer *input = bufferevent_get_input(bev);
    struct evbuffer *output = bufferevent_get_output(bev);
    int len = evbuffer_get_length(input);

    total_bytes_read += len;
    total_messages_read++;

    evbuffer_add_buffer(output, input);
    evbuffer_drain(input, len);
}

static void srv_event_cb(struct bufferevent *bev, short events, void *ctx) {
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_free(bev);
        fprintf(stderr, "Connection closed\n");
    }
}

static void srv_accept_success_cb(struct evconnlistener *listener,
evutil_socket_t fd, struct sockaddr *address, int socklen, void *ctx) {
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev = bufferevent_socket_new(base, fd,
            BEV_OPT_CLOSE_ON_FREE);

    fprintf(stderr, "New connection opened\n");

    set_tcp_no_delay(fd);

    bufferevent_setcb(bev, srv_read_cb, NULL, srv_event_cb, NULL);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

static void srv_accept_error_cb(struct evconnlistener *listener, void *ctx) {
    struct event_base *base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();
    fprintf(stderr, "Got an error %d (%s) on the listener. "
            "Shutting down.\n", err, evutil_socket_error_to_string(err));

    event_base_loopexit(base, NULL);
}

static u_int8_t usage(char *bin) {
    fprintf(stderr, "Usage: %s --port port\n", bin);

    return -1;
}

int main(int argc, char **argv) {
    struct event_base *base;
    struct evconnlistener *listener;
    struct sockaddr_in sin;
    struct option long_options[] = { { "port", 1, 0, 0 },
            { "workers", 1, 0, 0 }, NULL };
    int32_t idx, i;
    u_int32_t workers = 0;
    u_int16_t port = 0;

    char c;

    while (1) {
        c = getopt_long(argc, argv, "", long_options, &idx);
        if (c == -1)
            break;

        switch (c) {
        case 0:
            if (strcmp(long_options[idx].name, "port") == 0) {
                port = atoi(optarg);
            }
            if (strcmp(long_options[idx].name, "workers") == 0) {
                workers = atoi(optarg);
            }
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (!port) {
        return usage(argv[0]);
    }

    base = event_base_new();
    if (!base) {
        puts("Couldn't open event base");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0);
    sin.sin_port = htons(port);

    listener = evconnlistener_new_bind(base, srv_accept_success_cb, NULL,
    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (struct sockaddr*) &sin,
            sizeof(sin));
    if (!listener) {
        perror("Couldn't create listener");
        return 1;
    }
    evconnlistener_set_error_cb(listener, srv_accept_error_cb);

    printf("Server started, waiting for connection...\n");

    if (workers > 2) {
        for (i = 0; i != workers; i++) {
            if (fork() == 0) {
                event_reinit(base);
                event_base_dispatch(base);
            }

        }
    }
    event_reinit(base);
    event_base_dispatch(base);

    return 0;
}
