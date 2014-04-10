/*
 * drone.c
 *
 *  Created on: 20 mars 2014
 *      Author: Sylvain Afchain <safchain@gmail.com>
 */

#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <assert.h>
#include <inttypes.h>
#include <limits.h>

#include "drone.pb-c.h"

#define MAX_BLOCK_SIZE          10 * 1024 * 1024
#define MAX_SESSIONS            60000
#define ERROR_REPORT_DELAY             5                   // seconds
#define MAX_SEND_TRY            3

#define TYPE_TOTAL_PACKET       1
#define TYPE_INTERVAL_PACKET    2
#define TYPE_ERROR_PACKET       3

struct client {
    struct event_base *base;
    struct bufferevent *bev;

    int cur_size;
    uint8_t *buffer;

    struct job *job;
};

struct session {
    struct job *job;

    struct bufferevent *bev;
    u_int64_t remaining_data;
    u_int64_t send_try;
};

struct job {
    struct client *client;

    struct event_base *base;
    struct event *ev_job_timeout;
    struct event *ev_messages_timer;
    struct event *ev_interval_timer;
    struct event *ev_rampup_timer;
    struct session **sessions;
    char *message;

    Drone__JobRequest *request;
    Drone__JobResult *result;

    u_int32_t cur_interval;
    u_int32_t total_intervals;
    Drone__JobIntervalResult *intervalresults;

    u_int64_t cur_sessions;

    char error_msg[1024];
    struct timeval last_error_msg;
    u_int64_t skipped_error_msg;
};

#pragma pack(1)
struct packet_header {
    u_int32_t size;
    u_int8_t type;
};
#pragma pack(0)

static void drone_start_job(struct client *client, Drone__JobRequest *request);

static void set_tcp_no_delay(evutil_socket_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static inline int drone_report_error_ready(struct job *job, struct timeval *now) {
    if ((now->tv_sec - job->last_error_msg.tv_sec) > ERROR_REPORT_DELAY)
        return 1;
    job->skipped_error_msg++;

    return 0;
}

static void drone_report_error(struct job *job, struct timeval *now) {
    Drone__JobErrorMessage error_msg = DRONE__JOB_ERROR_MESSAGE__INIT;
    struct packet_header *ph;
    u_int8_t *data;
    u_int8_t *buffer;
    u_int64_t packet_size;
    u_int64_t data_size;

    if (drone_report_error_ready(job, now)) {
        fprintf(stderr, "%s\n", job->error_msg);

        error_msg.message = job->error_msg;
        data_size = drone__job_error_message__get_packed_size(&error_msg);
        packet_size = data_size + sizeof(struct packet_header);

        buffer = malloc(packet_size);

        ph = (struct packet_header *) buffer;
        data = buffer + sizeof(struct packet_header);

        ph->size = htonl(data_size);
        ph->type = TYPE_ERROR_PACKET;

        drone__job_error_message__pack(&error_msg, data);

        evbuffer_add(bufferevent_get_output(job->client->bev), buffer,
                packet_size);

        free(buffer);

        job->last_error_msg.tv_sec = now->tv_sec;
        job->skipped_error_msg = 0;
    }
}

static void drone_send_messages_cb(evutil_socket_t fd, short what, void *arg) {
    struct job *job = arg;
    struct session *session;
    struct evbuffer *evb;
    struct timeval now;
    float acc = 0;
    size_t len;
    u_int64_t i;

    if (job->client == NULL)
        return;

    for (i = 0; i != job->cur_sessions; i++) {
        session = job->sessions[i];

        fd = bufferevent_getfd(session->bev);
        if (fd > 0) {
            acc += job->request->messages_percent;
            if (acc >= 1) {
                if (session->remaining_data && session->send_try++ > 3) {
                    gettimeofday(&now, NULL);
                    if (drone_report_error_ready(job, &now)) {
                        snprintf(job->error_msg, sizeof(job->error_msg),
                                "Unable to send more packets, "
                                        "the server resources should be "
                                        "increased(%zd message skipped)",
                                job->skipped_error_msg);

                        drone_report_error(job, &now);

                        // try to purge every remaining data
                        evb = bufferevent_get_output(session->bev);
                        len = evbuffer_get_length(evb);
                        evbuffer_drain(evb, len);
                        evb = bufferevent_get_input(session->bev);
                        len = evbuffer_get_length(evb);
                        evbuffer_drain(evb, len);

                        bufferevent_flush(session->bev, EV_READ | EV_WRITE,
                                BEV_FLUSH);
                    }
                } else {
                    evb = bufferevent_get_output(session->bev);
                    evbuffer_add(evb, job->message, job->request->block_size);
                    session->remaining_data += job->request->block_size;
                    session->send_try = 0;
                }
                acc = 0;
            }
        }
    }
}

static void drone_event_cb(struct bufferevent *bev, short events, void *arg) {
    struct session *session = arg;
    struct job *job = session->job;
    struct timeval now;

    if (events & BEV_EVENT_CONNECTED) {
        evutil_socket_t fd = bufferevent_getfd(bev);
        set_tcp_no_delay(fd);

        job->result->total_sessions++;
    } else if (events & BEV_EVENT_ERROR) {
        perror("Not connected");

        gettimeofday(&now, NULL);
        if (drone_report_error_ready(job, &now)) {
            snprintf(job->error_msg, sizeof(job->error_msg),
                    "Connection to the server lost(%zd message skipped)",
                    job->skipped_error_msg);

            drone_report_error(job, &now);
        }

        if (job->result->total_sessions)
            job->result->total_sessions--;

        bufferevent_setfd(bev, -1);
    }
}

static void drone_read_cb(struct bufferevent *bev, void *arg) {
    struct session *session = arg;
    struct job *job = session->job;
    Drone__JobIntervalResult *interval;
    struct evbuffer *input = bufferevent_get_input(bev);
    size_t len = evbuffer_get_length(input);

    evbuffer_drain(input, len);

    session->remaining_data -= len;
    if (session->remaining_data > 0)
        return;
    session->remaining_data = 0;

    interval = &(job->intervalresults[job->cur_interval]);

    interval->total_messages_read++;
    interval->total_bytes_read += job->request->block_size;

    interval->total_sessions = job->result->total_sessions;

    job->result->total_messages_read++;
    job->result->total_bytes_read += len;
}

static void drone_rampup_timer_cb(evutil_socket_t fd, short what, void *arg) {
    struct job *job = arg;
    Drone__JobRequest *request = job->request;
    struct session *session;
    struct bufferevent *bev;
    struct sockaddr_in echo_sin;
    struct timeval timeout;
    u_int64_t sessions;
    u_int64_t i;

    if (job->client == NULL)
        return;

    if ((request->sessions - job->cur_sessions) > request->rampup_sessions)
        sessions = request->rampup_sessions;
    else
        sessions = request->sessions - job->cur_sessions;

    memset(&echo_sin, 0, sizeof(echo_sin));
    echo_sin.sin_family = AF_INET;
    echo_sin.sin_addr.s_addr = htonl(request->ipv4);
    echo_sin.sin_port = htons(request->port);

    for (i = 0; i < sessions; i++) {
        bev = bufferevent_socket_new(job->client->base, -1,
                BEV_OPT_CLOSE_ON_FREE);

        session = malloc(sizeof(struct session));

        bufferevent_setcb(bev, drone_read_cb, NULL, drone_event_cb, session);
        bufferevent_enable(bev, EV_READ | EV_WRITE);

        timeout.tv_sec = request->timeout;
        timeout.tv_usec = 0;
        bufferevent_set_timeouts(bev, NULL, &timeout);

        if (bufferevent_socket_connect(bev, (struct sockaddr *) &echo_sin,
                sizeof(echo_sin)) < 0) {
            bufferevent_free(bev);
            free(session);
            perror("Error during connection");
            return;
        }

        session->job = job;
        session->bev = bev;
        session->remaining_data = 0;

        job->sessions[job->cur_sessions] = session;
        job->cur_sessions++;
    }
}

static void drone_interval_timer_cb(evutil_socket_t fd, short what, void *arg) {
    struct job *job = arg;
    Drone__JobIntervalResult *interval;
    u_int64_t throughtput_kbs;
    struct packet_header *ph;
    u_int8_t *data;
    u_int8_t *buffer;
    u_int64_t packet_size;
    u_int64_t data_size;

    if (job->client == NULL)
        return;

    interval = &(job->intervalresults[job->cur_interval]);

    throughtput_kbs = interval->total_bytes_read
            / (job->request->interval * 1024);

    printf("[%5d] Messages: %8" PRIu64 ", Bytes: %12" PRIu64
    ", Throughtput(KB/sec): %5" PRIu64 ", Connections: %6" PRIu64 "\n",
            job->cur_interval + 1, interval->total_messages_read,
            interval->total_bytes_read, throughtput_kbs,
            interval->total_sessions);

    data_size = drone__job_interval_result__get_packed_size(interval);
    packet_size = data_size + sizeof(struct packet_header);

    buffer = malloc(packet_size);

    ph = (struct packet_header *) buffer;
    data = buffer + sizeof(struct packet_header);

    ph->size = htonl(data_size);
    ph->type = TYPE_INTERVAL_PACKET;

    drone__job_interval_result__pack(interval, data);

    evbuffer_add(bufferevent_get_output(job->client->bev), buffer, packet_size);

    free(buffer);

    if (job->cur_interval < (job->total_intervals - 1))
        job->cur_interval++;
}

static void drone_timeout_cb(evutil_socket_t fd, short what, void *arg) {
    struct job *job = arg;
    struct packet_header *ph;
    u_int8_t *data;
    u_int8_t *buffer;
    u_int64_t packet_size;
    u_int64_t data_size;
    u_int32_t i;

    for (i = 0; i != job->cur_interval; i++)
        job->result->intervalresults[i] = &(job->intervalresults[i]);

    data_size = drone__job_result__get_packed_size(job->result);
    packet_size = data_size + sizeof(struct packet_header);

    buffer = malloc(packet_size);

    ph = (struct packet_header *) buffer;
    data = buffer + sizeof(struct packet_header);

    ph->size = htonl(data_size);
    ph->type = TYPE_TOTAL_PACKET;

    drone__job_result__pack(job->result, data);

    evbuffer_add(bufferevent_get_output(job->client->bev), buffer, packet_size);

    free(buffer);
    free(job->message);
    free(job->intervalresults);
    free(job->result->intervalresults);
    free(job->result);

    drone__job_request__free_unpacked(job->request, NULL);

    if (job->sessions) {
        for (i = 0; i < job->cur_sessions; i++)
            if (job->sessions[i])
                bufferevent_free(job->sessions[i]->bev);
        free(job->sessions);
    }
    event_del(job->ev_messages_timer);
    event_free(job->ev_messages_timer);
    event_del(job->ev_interval_timer);
    event_free(job->ev_interval_timer);
    event_del(job->ev_rampup_timer);
    event_free(job->ev_rampup_timer);
    event_free(job->ev_job_timeout);

    job->client->cur_size = 0;
    job->client->job = NULL;
    free(job);

    printf("Job ended\n");
}

static void drone_srv_read_cb(struct bufferevent *bev, void *ctx) {
    struct client *client = ctx;
    Drone__JobRequest *request;
    u_int32_t size, expected, job_size;

    if (client->cur_size < sizeof(u_int32_t)) {
        expected = sizeof(u_int32_t) - client->cur_size;
        size = bufferevent_read(bev, client->buffer + client->cur_size,
                expected);
        client->cur_size += size;

        if (size < expected)
            return;
    }

    job_size = ntohl(*((u_int32_t *) client->buffer));

    expected = job_size - (client->cur_size - sizeof(u_int32_t));
    size = bufferevent_read(bev, client->buffer + client->cur_size, expected);
    client->cur_size += size;

    if (size < expected)
        return;

    client->bev = bev;

    request = drone__job_request__unpack(NULL, job_size,
            client->buffer + sizeof(u_int32_t));

    drone_start_job(client, request);
}

static void drone_srv_event_cb(struct bufferevent *bev, short events, void *ctx) {
    struct client *client = ctx;

    if (events & BEV_EVENT_ERROR)
        perror("Error from bufferevent");
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_free(bev);

        if (client->job)
            drone_timeout_cb(-1, events, client->job);

        free(client->buffer);
        free(client);
    }
}

static struct timeval drone_timeval_from_seconds(float seconds) {
    struct timeval tv;

    if (seconds < 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    } else if (seconds > (double) LONG_MAX) { /* Time too large? */
        tv.tv_sec = LONG_MAX;
        tv.tv_usec = 999999L;
    } else {
        tv.tv_sec = (long) seconds;
        tv.tv_usec = (long) ((seconds - (double) tv.tv_sec) * 1000000.0);
    }

    return tv;
}

static void drone_srv_accept_success_cb(struct evconnlistener *listener,
evutil_socket_t fd, struct sockaddr *address, int socklen, void *ctx) {
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev = bufferevent_socket_new(base, fd,
            BEV_OPT_CLOSE_ON_FREE);
    struct client *client;

    client = calloc(1, sizeof(struct client));
    client->base = base;
    client->buffer = malloc(sizeof(Drone__JobRequest) + sizeof(u_int32_t));

    set_tcp_no_delay(fd);

    bufferevent_setcb(bev, drone_srv_read_cb, NULL, drone_srv_event_cb, client);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

static void drone_srv_accept_error_cb(struct evconnlistener *listener,
        void *ctx) {
    struct event_base *base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();
    fprintf(stderr, "Got an error %d (%s) on the listener. "
            "Shutting down.\n", err, evutil_socket_error_to_string(err));

    event_base_loopexit(base, NULL);
}

void drone_install_messages_timer(struct job* job, struct client* client) {
    struct timeval timeout = drone_timeval_from_seconds(
            job->request->messages_interval);

    job->ev_messages_timer = event_new(client->base, -1, EV_PERSIST,
            drone_send_messages_cb, job);
    event_add(job->ev_messages_timer, &timeout);
}

void drone_install_rampup_timer(struct job* job, struct client* client) {
    struct timeval timeout = drone_timeval_from_seconds(
            job->request->rampup_interval);

    job->ev_rampup_timer = event_new(client->base, -1, EV_PERSIST,
            drone_rampup_timer_cb, job);
    event_add(job->ev_rampup_timer, &timeout);
}

void drone_install_interval_timer(struct job* job, struct client* client) {
    struct timeval timeout = drone_timeval_from_seconds(job->request->interval);

    job->ev_interval_timer = event_new(client->base, -1, EV_PERSIST,
            drone_interval_timer_cb, job);
    event_add(job->ev_interval_timer, &timeout);
}

void drone_install_job_timeout(struct job* job, struct client* client) {
    struct timeval timeout = drone_timeval_from_seconds(job->request->seconds);

    job->ev_job_timeout = evtimer_new(client->base, drone_timeout_cb, job);
    evtimer_add(job->ev_job_timeout, &timeout);
}

static void drone_start_job(struct client *client, Drone__JobRequest *request) {
    struct job *job;
    u_int32_t i;

    if (request->sessions >= MAX_SESSIONS) {
        fprintf(stderr, "Maximum sessions reached");
        return;
    }
    if (request->block_size >= MAX_BLOCK_SIZE) {
        fprintf(stderr, "Maximum block size reached");
        return;
    }

    printf("Job started\n");

    job = calloc(1, sizeof(struct job));
    job->request = request;
    job->client = client;
    client->job = job;

    job->message = malloc(request->block_size);
    for (i = 0; i < request->block_size; i++)
        job->message[i] = i % 128;

    job->result = calloc(1, sizeof(Drone__JobResult));
    drone__job_result__init(job->result);
    job->result->total_bytes_read = 0;
    job->result->total_messages_read = 0;

    job->total_intervals = request->seconds / request->interval;
    job->intervalresults = calloc(job->total_intervals,
            sizeof(Drone__JobIntervalResult));
    for (i = 0; i < job->total_intervals; i++)
        drone__job_interval_result__init(&job->intervalresults[i]);
    job->result->intervalresults = calloc(job->total_intervals,
            sizeof(Drone__JobIntervalResult *));

    job->sessions = calloc(request->sessions, sizeof(struct sessions *));

    gettimeofday(&(job->last_error_msg), NULL);

    drone_install_job_timeout(job, client);
    drone_install_interval_timer(job, client);
    drone_install_rampup_timer(job, client);
    drone_install_messages_timer(job, client);
}

static u_int8_t usage(char *bin) {
    fprintf(stderr, "Usage: %s --port port\n", bin);
    return -1;
}

int main(int argc, char **argv) {
    struct evconnlistener *listener;
    struct event_base *base;
    struct sockaddr_in srv_sin;
    struct option long_options[] = { { "port", 1, 0, 0 } };
    int32_t idx;
    u_int16_t port = 0;
    char c;

    while (1) {
        c = getopt_long(argc, argv, "", long_options, &idx);
        if (c == -1)
            break;

        switch (c) {
        case 0:
            if (strcmp(long_options[idx].name, "port") == 0)
                port = atoi(optarg);
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (!port)
        return usage(argv[0]);

    signal(SIGPIPE, SIG_IGN);

    base = event_base_new();
    if (!base) {
        perror("Couldn't open event base");
        return -1;
    }

    memset(&srv_sin, 0, sizeof(srv_sin));
    srv_sin.sin_family = AF_INET;
    srv_sin.sin_addr.s_addr = htonl(0);
    srv_sin.sin_port = htons(port);

    listener = evconnlistener_new_bind(base, drone_srv_accept_success_cb,
    NULL,
    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (struct sockaddr*) &srv_sin,
            sizeof(srv_sin));
    if (!listener) {
        perror("Couldn't create listener");
        return -1;
    }
    evconnlistener_set_error_cb(listener, drone_srv_accept_error_cb);

    printf("Drone started, waiting for job...\n");

    event_base_dispatch(base);
    event_base_free(base);

    return 0;
}
