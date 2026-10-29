/*
 * server.c — Epoll-based TCP/UDP server for the register simulator.
 *
 * Design:
 *  - One listening TCP socket (or one UDP socket in --udp mode).
 *  - epoll_wait() drives the event loop.
 *  - Per-connection state: receive buffer, streaming parser, send queue.
 *  - Idle timeout enforced via timerfd (one timerfd per connection, or a
 *    coarse 1-second sweep timer to check all connections).
 *  - Graceful shutdown via signalfd on SIGINT/SIGTERM.
 *
 * All sockets are O_NONBLOCK.  Partial reads and partial writes are fully
 * handled: the send queue accumulates pending bytes and EPOLLOUT is
 * armed/disarmed as needed.
 */

#define _GNU_SOURCE   /* signalfd, timerfd, epoll */
#include "server.h"
#include "protocol.h"
#include "regfile.h"
#include "log.h"
#include "crc32.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define RECV_BUF_SIZE    (16u * 1024u)    /* per-connection receive buffer */
#define MAX_EVENTS       64
#define SWEEP_INTERVAL_S 5                /* idle-check sweep period */

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */

typedef struct conn conn_t;

struct conn {
    int       fd;
    parser_t *parser;

    /* Receive buffer (circular / linear with compact-on-overflow) */
    uint8_t   recv_buf[RECV_BUF_SIZE];
    size_t    recv_len;

    /* Send queue: a simple byte FIFO */
    uint8_t  *send_buf;
    size_t    send_cap;
    size_t    send_head;   /* offset of first unsent byte */
    size_t    send_len;    /* number of queued bytes */

    /* Current epoll event mask (updated before each parser_feed call) */
    uint32_t  cur_events;

    /* Idle tracking */
    time_t    last_active;

    /* Server back-pointer (used in parser callback) */
    struct server_state *srv;

    conn_t   *next;  /* free-list linkage */
};

/* -------------------------------------------------------------------------
 * Server state
 * ---------------------------------------------------------------------- */

typedef struct server_state {
    int              epfd;
    int              listen_fd;   /* TCP listen fd OR UDP fd */
    int              sig_fd;      /* signalfd */
    int              timer_fd;    /* timerfd for idle sweep */
    int              udp_mode;
    int              idle_timeout;

    regfile_t        regfile;

    /* Connection table (indexed by fd, sparse) */
    conn_t          *conns[1024]; /* fd → conn; 1024 is a safe upper bound */
    int              nconns;

    uint16_t         next_seq;   /* server-side response sequence counter */
} server_state_t;

/* -------------------------------------------------------------------------
 * Send queue helpers
 * ---------------------------------------------------------------------- */

static int sendq_push(conn_t *c, const uint8_t *data, size_t len) {
    /* Grow buffer if needed */
    if (c->send_len + len > c->send_cap) {
        size_t new_cap = c->send_cap;
        if (new_cap == 0) new_cap = 4096;
        while (new_cap < c->send_len + len) new_cap *= 2;
        if (new_cap > SEND_QUEUE_CAP) {
            log_warn("send queue full for fd %d, dropping connection", c->fd);
            return -1;
        }
        /* Compact: move unsent data to front */
        uint8_t *nb = (uint8_t *)malloc(new_cap);
        if (!nb) return -1;
        if (c->send_len > 0) {
            memcpy(nb, c->send_buf + c->send_head, c->send_len);
        }
        free(c->send_buf);
        c->send_buf  = nb;
        c->send_cap  = new_cap;
        c->send_head = 0;
    } else if (c->send_head + c->send_len + len > c->send_cap) {
        /* Not enough room at tail — compact */
        memmove(c->send_buf, c->send_buf + c->send_head, c->send_len);
        c->send_head = 0;
    }
    memcpy(c->send_buf + c->send_head + c->send_len, data, len);
    c->send_len += len;
    return 0;
}

/* Returns 1 if queue drained, 0 if more remains, -1 on error */
static int sendq_flush(conn_t *c) {
    while (c->send_len > 0) {
        ssize_t n = send(c->fd,
                         c->send_buf + c->send_head,
                         c->send_len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        c->send_head += (size_t)n;
        c->send_len  -= (size_t)n;
    }
    c->send_head = 0;
    return 1;
}

/* -------------------------------------------------------------------------
 * Encode and enqueue a response packet
 * ---------------------------------------------------------------------- */

static int conn_send_packet(conn_t *c, packet_t *pkt,
                             int epfd, uint32_t cur_events) {
    uint8_t buf[PROTO_MAX_PACKET];
    int n = encode_packet(pkt, buf, sizeof(buf));
    if (n < 0) {
        log_error("encode_packet failed for fd %d", c->fd);
        return -1;
    }
    if (sendq_push(c, buf, (size_t)n) != 0) return -1;

    /* Try to flush immediately */
    int rc = sendq_flush(c);
    if (rc < 0) return -1;

    /* If there are still bytes queued, arm EPOLLOUT */
    if (c->send_len > 0 && !(cur_events & EPOLLOUT)) {
        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
        ev.data.fd = c->fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Dispatch a decoded request packet
 * ---------------------------------------------------------------------- */

static void handle_packet(const packet_t *pkt, void *userdata) {
    conn_t         *c   = (conn_t *)userdata;
    server_state_t *srv = c->srv;

    log_debug("fd %d rx %s seq=%u", c->fd,
              msg_type_name(pkt->type), pkt->sequence);

    packet_t resp;
    uint16_t seq = pkt->sequence;

    switch ((msg_type_t)pkt->type) {
    case MSG_PING:
        pkt_build_pong(&resp, seq);
        break;

    case MSG_RESET:
        regfile_reset(&srv->regfile);
        pkt_build_reset_ack(&resp, seq);
        log_info("fd %d RESET issued", c->fd);
        break;

    case MSG_READ_REG: {
        if (pkt->payload_len != PLEN_READ_REG) {
            pkt_build_error(&resp, srv->next_seq++, ERR_INVALID_LENGTH, seq);
            break;
        }
        uint16_t addr = pkt->payload.read_reg.addr;
        uint32_t val  = 0;
        int rc = regfile_read(&srv->regfile, addr, &val);
        if (rc == -1) {
            pkt_build_error(&resp, srv->next_seq++, ERR_UNKNOWN_REGISTER, seq);
        } else {
            pkt_build_read_resp(&resp, seq, addr, val);
        }
        break;
    }

    case MSG_WRITE_REG: {
        if (pkt->payload_len != PLEN_WRITE_REG) {
            pkt_build_error(&resp, srv->next_seq++, ERR_INVALID_LENGTH, seq);
            break;
        }
        uint16_t addr = pkt->payload.write_reg.addr;
        uint32_t val  = pkt->payload.write_reg.value;
        int rc = regfile_write(&srv->regfile, addr, val);
        if (rc == -1) {
            pkt_build_error(&resp, srv->next_seq++, ERR_UNKNOWN_REGISTER, seq);
        } else if (rc == -2) {
            pkt_build_error(&resp, srv->next_seq++,
                            ERR_READ_ONLY_VIOLATION, seq);
        } else {
            pkt_build_write_ack(&resp, seq);
        }
        break;
    }

    case MSG_READ_RESP:
    case MSG_WRITE_ACK:
    case MSG_PONG:
    case MSG_RESET_ACK:
        /* Server should not receive these; ignore */
        log_warn("fd %d unexpected msg type %s from client",
                 c->fd, msg_type_name(pkt->type));
        return;

    case MSG_ERROR:
        log_warn("fd %d client sent ERROR packet (ignored)", c->fd);
        return;

    default:
        pkt_build_error(&resp, srv->next_seq++, ERR_UNKNOWN_TYPE, seq);
        break;
    }

    (void)conn_send_packet(c, &resp, srv->epfd, c->cur_events);
}

/* -------------------------------------------------------------------------
 * Parser error callback
 * ---------------------------------------------------------------------- */

static void handle_parse_error(err_code_t code, uint16_t bad_seq,
                                void *userdata) {
    conn_t         *c   = (conn_t *)userdata;
    server_state_t *srv = c->srv;

    log_warn("fd %d parse error: %s (bad_seq=%u)",
             c->fd, err_code_name((uint8_t)code), bad_seq);

    packet_t resp;
    pkt_build_error(&resp, srv->next_seq++, code, bad_seq);
    (void)conn_send_packet(c, &resp, srv->epfd, c->cur_events);
}

/* -------------------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------------- */

static conn_t *conn_new(int fd, server_state_t *srv) {
    conn_t *c = (conn_t *)calloc(1, sizeof(conn_t));
    if (!c) return NULL;
    c->fd          = fd;
    c->parser      = parser_new();
    c->last_active = time(NULL);
    c->srv         = srv;
    if (!c->parser) { free(c); return NULL; }
    return c;
}

static void conn_free(conn_t *c) {
    if (!c) return;
    parser_free(c->parser);
    free(c->send_buf);
    free(c);
}

static void conn_close(server_state_t *srv, int fd) {
    if (fd < 0 || fd >= 1024) return;
    conn_t *c = srv->conns[fd];
    if (!c) return;

    log_info("closing connection fd=%d", fd);
    epoll_ctl(srv->epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    conn_free(c);
    srv->conns[fd] = NULL;
    srv->nconns--;
}

/* -------------------------------------------------------------------------
 * Accept a new TCP connection
 * ---------------------------------------------------------------------- */

static void handle_accept(server_state_t *srv) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept4(srv->listen_fd, (struct sockaddr *)&addr,
                         &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_error("accept4: %s", strerror(errno));
            break;
        }
        if (fd >= 1024 || srv->nconns >= SERVER_MAX_CLIENTS) {
            log_warn("max clients reached, rejecting fd=%d", fd);
            close(fd);
            continue;
        }

        /* TCP_NODELAY for low latency */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        conn_t *c = conn_new(fd, srv);
        if (!c) { close(fd); continue; }

        srv->conns[fd] = c;
        srv->nconns++;

        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.fd = fd;
        if (epoll_ctl(srv->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            log_error("epoll_ctl ADD fd=%d: %s", fd, strerror(errno));
            conn_close(srv, fd);
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        log_info("accepted fd=%d from %s:%u", fd, ip, ntohs(addr.sin_port));
    }
}

/* -------------------------------------------------------------------------
 * Handle data readable on a client fd
 * ---------------------------------------------------------------------- */

static void handle_client_read(server_state_t *srv, int fd) {
    conn_t *c = srv->conns[fd];
    if (!c) return;

    for (;;) {
        size_t space = sizeof(c->recv_buf) - c->recv_len;
        if (space == 0) {
            /* Buffer full — parser is stuck; reset connection */
            log_warn("fd %d recv buffer full, closing", fd);
            conn_close(srv, fd);
            return;
        }
        ssize_t n = recv(fd, c->recv_buf + c->recv_len, space, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_debug("fd %d recv error: %s", fd, strerror(errno));
            conn_close(srv, fd);
            return;
        }
        if (n == 0) {
            log_info("fd %d client disconnected", fd);
            conn_close(srv, fd);
            return;
        }
        c->recv_len     += (size_t)n;
        c->last_active   = time(NULL);

        /* Record current epoll state so parser callbacks can read it */
        c->cur_events = EPOLLIN;

        /* Feed all accumulated bytes to the parser */
        parser_feed(c->parser,
                    c->recv_buf, c->recv_len,
                    handle_packet, handle_parse_error, c);

        /* After parsing, discard consumed bytes.  The parser is stateful
         * and internally buffers partial packets, so we always pass the
         * entire recv_buf from the start.  After each feed call we reset
         * recv_len to 0 — the parser has consumed or buffered everything. */
        c->recv_len = 0;
    }
}

/* -------------------------------------------------------------------------
 * Handle EPOLLOUT (drain send queue)
 * ---------------------------------------------------------------------- */

static void handle_client_write(server_state_t *srv, int fd) {
    conn_t *c = srv->conns[fd];
    if (!c) return;

    int rc = sendq_flush(c);
    if (rc < 0) {
        conn_close(srv, fd);
        return;
    }
    if (rc == 1) {
        /* Queue drained — disarm EPOLLOUT */
        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.fd = fd;
        epoll_ctl(srv->epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}

/* -------------------------------------------------------------------------
 * UDP datagram handling
 * ---------------------------------------------------------------------- */

static void handle_udp(server_state_t *srv) {
    uint8_t buf[PROTO_MAX_PACKET];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);

    for (;;) {
        ssize_t n = recvfrom(srv->listen_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_error("recvfrom: %s", strerror(errno));
            break;
        }

        packet_t req;
        int rc = decode_packet(buf, (size_t)n, &req);

        packet_t resp;
        if (rc == -2) {
            pkt_build_error(&resp, srv->next_seq++, ERR_BAD_MAGIC, 0);
        } else if (rc == -3) {
            pkt_build_error(&resp, srv->next_seq++, ERR_BAD_VERSION,
                            req.sequence);
        } else if (rc == -4) {
            pkt_build_error(&resp, srv->next_seq++, ERR_BAD_CRC,
                            req.sequence);
        } else if (rc != 0) {
            pkt_build_error(&resp, srv->next_seq++, ERR_INVALID_LENGTH,
                            req.sequence);
        } else {
            /* Valid packet — process it via a temporary conn-like context */
            /* For UDP we use a stack-allocated fake conn */
            uint16_t seq = req.sequence;
            switch ((msg_type_t)req.type) {
            case MSG_PING:
                pkt_build_pong(&resp, seq);
                break;
            case MSG_RESET:
                regfile_reset(&srv->regfile);
                pkt_build_reset_ack(&resp, seq);
                break;
            case MSG_READ_REG: {
                uint32_t val = 0;
                int r = regfile_read(&srv->regfile,
                                     req.payload.read_reg.addr, &val);
                if (r < 0) pkt_build_error(&resp, srv->next_seq++,
                                            ERR_UNKNOWN_REGISTER, seq);
                else        pkt_build_read_resp(&resp, seq,
                                                req.payload.read_reg.addr,
                                                val);
                break;
            }
            case MSG_WRITE_REG: {
                int r = regfile_write(&srv->regfile,
                                      req.payload.write_reg.addr,
                                      req.payload.write_reg.value);
                if (r == -1) pkt_build_error(&resp, srv->next_seq++,
                                              ERR_UNKNOWN_REGISTER, seq);
                else if (r == -2) pkt_build_error(&resp, srv->next_seq++,
                                                   ERR_READ_ONLY_VIOLATION,
                                                   seq);
                else              pkt_build_write_ack(&resp, seq);
                break;
            }
            default:
                pkt_build_error(&resp, srv->next_seq++, ERR_UNKNOWN_TYPE, seq);
                break;
            }
        }

        uint8_t out[PROTO_MAX_PACKET];
        int out_n = encode_packet(&resp, out, sizeof(out));
        if (out_n > 0) {
            sendto(srv->listen_fd, out, (size_t)out_n, 0,
                   (struct sockaddr *)&src, slen);
        }
    }
}

/* -------------------------------------------------------------------------
 * Idle-timeout sweep
 * ---------------------------------------------------------------------- */

static void idle_sweep(server_state_t *srv) {
    if (srv->idle_timeout <= 0) return;
    time_t now = time(NULL);
    for (int fd = 0; fd < 1024; fd++) {
        conn_t *c = srv->conns[fd];
        if (!c) continue;
        if (now - c->last_active > srv->idle_timeout) {
            log_info("fd %d idle timeout (%ds)", fd, srv->idle_timeout);
            conn_close(srv, fd);
        }
    }
}

/* -------------------------------------------------------------------------
 * server_run
 * ---------------------------------------------------------------------- */

int server_run(const server_config_t *cfg) {
    crc32_init();
    log_init((log_level_t)cfg->log_level, stderr);

    server_state_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.udp_mode     = cfg->udp_mode;
    srv.idle_timeout = cfg->idle_timeout_sec;
    srv.epfd         = -1;
    srv.listen_fd    = -1;
    srv.sig_fd       = -1;
    srv.timer_fd     = -1;

    regfile_init(&srv.regfile);

    /* ---- Create listening socket ---- */
    int sock_type = cfg->udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    srv.listen_fd = socket(AF_INET, sock_type | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0);
    if (srv.listen_fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(cfg->port);

    if (bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind port %u: %s", cfg->port, strerror(errno));
        close(srv.listen_fd);
        return -1;
    }

    if (!cfg->udp_mode) {
        if (listen(srv.listen_fd, SERVER_BACKLOG) < 0) {
            log_error("listen: %s", strerror(errno));
            close(srv.listen_fd);
            return -1;
        }
    }

    /* ---- epoll fd ---- */
    srv.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epfd < 0) {
        log_error("epoll_create1: %s", strerror(errno));
        close(srv.listen_fd);
        return -1;
    }

    {
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = srv.listen_fd;
        epoll_ctl(srv.epfd, EPOLL_CTL_ADD, srv.listen_fd, &ev);
    }

    /* ---- signalfd for SIGINT/SIGTERM ---- */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    srv.sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (srv.sig_fd < 0) {
        log_error("signalfd: %s", strerror(errno));
        goto shutdown;
    }
    {
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = srv.sig_fd;
        epoll_ctl(srv.epfd, EPOLL_CTL_ADD, srv.sig_fd, &ev);
    }

    /* ---- timerfd for idle sweep ---- */
    srv.timer_fd = timerfd_create(CLOCK_MONOTONIC,
                                  TFD_NONBLOCK | TFD_CLOEXEC);
    if (srv.timer_fd < 0) {
        log_error("timerfd_create: %s", strerror(errno));
        goto shutdown;
    }
    {
        struct itimerspec its;
        its.it_interval.tv_sec  = SWEEP_INTERVAL_S;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec     = SWEEP_INTERVAL_S;
        its.it_value.tv_nsec    = 0;
        timerfd_settime(srv.timer_fd, 0, &its, NULL);

        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = srv.timer_fd;
        epoll_ctl(srv.epfd, EPOLL_CTL_ADD, srv.timer_fd, &ev);
    }

    log_info("regsim server listening on %s port %u (mode=%s)",
             cfg->udp_mode ? "UDP" : "TCP", cfg->port,
             cfg->udp_mode ? "udp" : "tcp");

    /* ---- Event loop ---- */
    struct epoll_event events[MAX_EVENTS];
    int running = 1;

    while (running) {
        int nev = epoll_wait(srv.epfd, events, MAX_EVENTS, -1);
        if (nev < 0) {
            if (errno == EINTR) continue;
            log_error("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nev; i++) {
            int fd = events[i].data.fd;

            /* Signal event */
            if (fd == srv.sig_fd) {
                struct signalfd_siginfo si;
                ssize_t r = read(srv.sig_fd, &si, sizeof(si));
                if (r == (ssize_t)sizeof(si)) {
                    log_info("received signal %u — shutting down", si.ssi_signo);
                }
                running = 0;
                break;
            }

            /* Timer event */
            if (fd == srv.timer_fd) {
                uint64_t exp;
                (void)read(srv.timer_fd, &exp, sizeof(exp));
                idle_sweep(&srv);
                continue;
            }

            /* Listening socket */
            if (fd == srv.listen_fd) {
                if (cfg->udp_mode) {
                    handle_udp(&srv);
                } else {
                    handle_accept(&srv);
                }
                continue;
            }

            /* Client socket */
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                log_debug("fd %d error/hup", fd);
                conn_close(&srv, fd);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                handle_client_read(&srv, fd);
            }
            /* Check conn still alive before handling write */
            if (srv.conns[fd] && (events[i].events & EPOLLOUT)) {
                handle_client_write(&srv, fd);
            }
        }
    }

shutdown:
    log_info("shutting down — closing all connections");

    /* Close all client connections */
    for (int fd = 0; fd < 1024; fd++) {
        if (srv.conns[fd]) conn_close(&srv, fd);
    }

    if (srv.timer_fd >= 0) close(srv.timer_fd);
    if (srv.sig_fd   >= 0) close(srv.sig_fd);
    if (srv.listen_fd >= 0) close(srv.listen_fd);
    if (srv.epfd     >= 0) close(srv.epfd);

    log_info("server stopped");
    return 0;
}
