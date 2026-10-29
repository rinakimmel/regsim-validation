/*
 * regctl.c — Command-line interface for the register simulator client.
 *
 * Usage:
 *   regctl [--host H] [--port P] [--timeout MS] [--retries N] <command>
 *
 * Commands:
 *   read  <addr>          Read a register (addr as 0xNN or decimal)
 *   write <addr> <value>  Write a register
 *   ping                  Send a PING and wait for PONG
 *   reset                 Send a RESET
 *   dump                  Read and print all known registers
 */

#include "client.h"
#include "regfile.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    5555
#define DEFAULT_TIMEOUT 2000
#define DEFAULT_RETRIES 3

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options] <command> [args]\n"
        "\n"
        "Options:\n"
        "  --host HOST       Server hostname or IP (default: 127.0.0.1)\n"
        "  --port PORT       Server port (default: 5555)\n"
        "  --timeout MS      Per-attempt timeout in ms (default: 2000)\n"
        "  --retries N       Number of retries on timeout (default: 3)\n"
        "\n"
        "Commands:\n"
        "  read  <addr>          Read register at addr (hex or decimal)\n"
        "  write <addr> <value>  Write value to register at addr\n"
        "  ping                  Ping the server\n"
        "  reset                 Reset all registers to power-on values\n"
        "  dump                  Read and print all known registers\n"
        "\n"
        "Examples:\n"
        "  %s read 0x0000\n"
        "  %s write 0x000E 0xDEADBEEF\n"
        "  %s --host 192.168.1.10 --port 5556 dump\n",
        argv0, argv0, argv0, argv0);
}

static int parse_uint32(const char *s, uint32_t *out) {
    if (!s || !out) return -1;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (*end || v > 0xFFFFFFFFULL) return -1;
    *out = (uint32_t)v;
    return 0;
}

static const char *access_name(reg_access_t a) {
    switch (a) {
    case ACCESS_RW:  return "RW ";
    case ACCESS_RO:  return "RO ";
    case ACCESS_W1C: return "W1C";
    case ACCESS_WO:  return "WO ";
    default:         return "???";
    }
}

/* -------------------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------------- */

static int cmd_read(client_t *c, const char *addr_s) {
    uint32_t a32;
    if (parse_uint32(addr_s, &a32) < 0 || a32 > 0xFFFF) {
        fprintf(stderr, "error: invalid address: %s\n", addr_s);
        return 1;
    }
    uint16_t addr = (uint16_t)a32;
    uint32_t value;
    client_err_t err = client_read_reg(c, addr, &value);
    if (err != CLIENT_OK) {
        if (err == CLIENT_ERR_SERVER) {
            fprintf(stderr, "error: server error: %s\n",
                    err_code_name(client_last_server_error(c)));
        } else {
            fprintf(stderr, "error: %s\n", client_strerror(err));
        }
        return 1;
    }
    printf("0x%04X = 0x%08" PRIX32 " (%10" PRIu32 ")\n", addr, value, value);
    return 0;
}

static int cmd_write(client_t *c, const char *addr_s, const char *val_s) {
    uint32_t a32, value;
    if (parse_uint32(addr_s, &a32) < 0 || a32 > 0xFFFF) {
        fprintf(stderr, "error: invalid address: %s\n", addr_s);
        return 1;
    }
    if (parse_uint32(val_s, &value) < 0) {
        fprintf(stderr, "error: invalid value: %s\n", val_s);
        return 1;
    }
    uint16_t addr = (uint16_t)a32;
    client_err_t err = client_write_reg(c, addr, value);
    if (err != CLIENT_OK) {
        if (err == CLIENT_ERR_READONLY || err == CLIENT_ERR_SERVER) {
            fprintf(stderr, "error: server error: %s\n",
                    err_code_name(client_last_server_error(c)));
        } else {
            fprintf(stderr, "error: %s\n", client_strerror(err));
        }
        return 1;
    }
    printf("0x%04X <- 0x%08" PRIX32 "  OK\n", addr, value);
    return 0;
}

static int cmd_ping(client_t *c) {
    client_err_t err = client_ping(c);
    if (err != CLIENT_OK) {
        fprintf(stderr, "error: %s\n", client_strerror(err));
        return 1;
    }
    printf("PONG\n");
    return 0;
}

static int cmd_reset(client_t *c) {
    client_err_t err = client_reset(c);
    if (err != CLIENT_OK) {
        fprintf(stderr, "error: %s\n", client_strerror(err));
        return 1;
    }
    printf("RESET_ACK\n");
    return 0;
}

static int cmd_dump(client_t *c) {
    printf("%-16s  %-6s  %-4s  %-10s  %s\n",
           "Name", "Addr", "Acc", "Hex", "Decimal");
    printf("%-16s  %-6s  %-4s  %-10s  %s\n",
           "----------------", "------", "----",
           "----------", "----------");

    for (size_t i = 0; i < REGFILE_NUM_REGS; i++) {
        const reg_desc_t *d = regfile_descriptor_by_index(i);
        if (!d) continue;

        /* WO registers: display as write-only, skip read */
        if (d->access == ACCESS_WO) {
            printf("%-16s  0x%04X  %-4s  %-10s  %s\n",
                   d->name, d->addr, access_name(d->access),
                   "<WO>", "<WO>");
            continue;
        }

        uint32_t value = 0;
        client_err_t err = client_read_reg(c, d->addr, &value);
        if (err != CLIENT_OK) {
            printf("%-16s  0x%04X  %-4s  %-10s  %s\n",
                   d->name, d->addr, access_name(d->access),
                   "<ERR>", client_strerror(err));
        } else {
            printf("%-16s  0x%04X  %-4s  0x%08" PRIX32 "  %" PRIu32 "\n",
                   d->name, d->addr, access_name(d->access),
                   value, value);
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    const char *host    = DEFAULT_HOST;
    int         port    = DEFAULT_PORT;
    int         timeout = DEFAULT_TIMEOUT;
    int         retries = DEFAULT_RETRIES;
    int         cmd_idx = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "error: --host needs arg\n"); return 1; }
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "error: --port needs arg\n"); return 1; }
            port = atoi(argv[++i]);
            if (port < 1 || port > 65535) { fprintf(stderr, "error: bad port\n"); return 1; }
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "error: --timeout needs arg\n"); return 1; }
            timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--retries") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "error: --retries needs arg\n"); return 1; }
            retries = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            /* First non-option is the command */
            cmd_idx = i;
            break;
        }
    }

    if (cmd_idx < 0) {
        usage(argv[0]);
        return 1;
    }

    client_t *c = client_connect(host, (uint16_t)port, timeout, retries);
    if (!c) {
        fprintf(stderr, "error: could not connect to %s:%d\n", host, port);
        return 1;
    }

    const char *cmd = argv[cmd_idx];
    int rc = 0;

    if (strcmp(cmd, "read") == 0) {
        if (cmd_idx + 1 >= argc) {
            fprintf(stderr, "error: read requires <addr>\n");
            rc = 1;
        } else {
            rc = cmd_read(c, argv[cmd_idx + 1]);
        }
    } else if (strcmp(cmd, "write") == 0) {
        if (cmd_idx + 2 >= argc) {
            fprintf(stderr, "error: write requires <addr> <value>\n");
            rc = 1;
        } else {
            rc = cmd_write(c, argv[cmd_idx + 1], argv[cmd_idx + 2]);
        }
    } else if (strcmp(cmd, "ping") == 0) {
        rc = cmd_ping(c);
    } else if (strcmp(cmd, "reset") == 0) {
        rc = cmd_reset(c);
    } else if (strcmp(cmd, "dump") == 0) {
        rc = cmd_dump(c);
    } else {
        fprintf(stderr, "error: unknown command: %s\n", cmd);
        usage(argv[0]);
        rc = 1;
    }

    client_close(c);
    return rc;
}
