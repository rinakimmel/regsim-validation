# regsim — Register Device Simulator and Validation Suite

A production-quality portfolio project for a **Chip Validation Software Engineer** role.

`regsim` implements:
- A **TCP server** that simulates a chip's 16-register file with realistic access semantics (RW, RO, W1C, WO, reserved bits, BUSY/DONE simulation, free-running counter, FIFO).
- A **custom binary protocol** over TCP (or UDP) with CRC-32 integrity checking and a streaming framing parser.
- A **blocking client library** (`libclient`) with automatic retry.
- A **CLI tool** (`regctl`) for interactive register access.
- A **unit test suite** for the codec and register model.
- An **integration validation suite** that tests every register, protocol robustness edge case, concurrent load, and round-trip latency with real measured statistics.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│  Client side                         Server side                    │
│                                                                     │
│  ┌──────────┐  encode_packet()  ─────────────►  ┌───────────────┐  │
│  │  regctl  │  ◄─────────────  decode_packet()   │  regsim-      │  │
│  │ (CLI)    │                                    │  server       │  │
│  └──────────┘                                    │               │  │
│       │                                          │  ┌─────────┐  │  │
│  ┌────▼─────┐  TCP (or UDP)                      │  │  epoll  │  │  │
│  │ client.c │  ◄──────────────────────────────►  │  │  loop   │  │  │
│  │ library  │                                    │  └────┬────┘  │  │
│  └──────────┘                                    │       │        │  │
│                                                  │  ┌────▼────┐  │  │
│  ┌──────────┐                                    │  │ parser  │  │  │
│  │ validate │─────────────────────────────────►  │  │ (state  │  │  │
│  │ (suite)  │  50 concurrent TCP connections      │  │  machine│  │  │
│  └──────────┘                                    │  └────┬────┘  │  │
│                                                  │       │        │  │
│                                                  │  ┌────▼────┐  │  │
│                                                  │  │ regfile │  │  │
│                                                  │  │ (model) │  │  │
│                                                  │  └─────────┘  │  │
└─────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────┐
  │  Shared (both sides)                     │
  │  protocol.c — encode/decode/parser       │
  │  crc32.c    — IEEE 802.3 table CRC       │
  │  log.c      — structured logging         │
  └──────────────────────────────────────────┘
```

---

## Project Structure

```
regsim/
├── Makefile
├── README.md
├── docs/
│   └── protocol.md          Wire protocol specification
├── include/
│   ├── protocol.h           Packet types, encode/decode, streaming parser
│   ├── crc32.h              CRC-32 API
│   ├── regfile.h            Register file model API + constants
│   ├── server.h             Server lifecycle API
│   ├── client.h             Client library API
│   ├── log.h                Structured logging
│   └── test_framework.h     Minimal unit test framework
├── src/
│   ├── crc32.c              Table-driven CRC-32
│   ├── protocol.c           Packet codec + streaming parser state machine
│   ├── regfile.c            16-register file simulation
│   ├── log.c                Timestamp + level logging
│   ├── server.c             epoll event loop, connection management
│   ├── server_main.c        Server entry point + argument parsing
│   ├── client.c             Blocking client with poll() + retry
│   └── regctl.c             CLI: read / write / ping / reset / dump
├── tests/
│   ├── test_codec.c         Unit tests: CRC + encode/decode/parser
│   ├── test_regfile.c       Unit tests: all register access semantics
│   └── validate.c           Integration tests against live server
└── scripts/
    └── run_validation.sh    Build + server start + full test run
```

---

## Build

**Requirements:** GCC with C11 support, Linux (WSL is fine), `make`, `nc`
(for the validation script's readiness probe), `pthread`.

```bash
# Release build
make all

# ASan + UBSan build
make asan

# Unit tests only (no server needed)
make test
```

Build output is placed under `build/release/` and `build/asan/`.

---

## Run

### Start the server

```bash
./build/release/regsim-server --port 5555 --log-level info
```

Options:
- `--port PORT`           — TCP/UDP port (default: 5555)
- `--idle-timeout SEC`    — disconnect idle clients after SEC seconds (default: 30)
- `--log-level LEVEL`     — `error | warn | info | debug` (default: info)
- `--udp`                 — serve over UDP instead of TCP

### Use the CLI

```bash
# Read the ID register
./build/release/regctl read 0x0000

# Write to a scratch register
./build/release/regctl write 0x000E 0xDEADBEEF

# Dump all registers
./build/release/regctl dump

# Ping
./build/release/regctl ping

# Reset all registers
./build/release/regctl reset

# Target a remote server
./build/release/regctl --host 192.168.1.10 --port 5556 dump
```

---

## Run the Validation Suite

```bash
# Full build + unit tests + integration validation
bash scripts/run_validation.sh

# With ASan
bash scripts/run_validation.sh --asan

# Different port
bash scripts/run_validation.sh --port 5556
```

The script:
1. Builds everything.
2. Runs unit tests (`test_codec`, `test_regfile`).
3. Starts `regsim-server` in the background.
4. Runs `validate` against it.
5. Stops the server and reports pass/fail.

Exit code is 0 on all tests passing, 1 otherwise.

---

## Run the Sanitizers

```bash
# Build with ASan + UBSan
make asan

# Run unit tests under sanitizers
./build/asan/test_codec
./build/asan/test_regfile

# Run full validation under sanitizers
# (start an asan server in one terminal)
./build/asan/regsim-server --port 5555 &
./build/asan/validate --host 127.0.0.1 --port 5555
kill %1
```

---

## Event Loop Design

`server.c` uses a single-threaded **epoll** loop:

```
epoll_wait()
  ├── signalfd readable  → set running=0, break loop
  ├── timerfd readable   → idle-timeout sweep over all connections
  ├── listen_fd readable → accept4() new clients in a loop
  └── client fd          → EPOLLIN:  recv() → parser_feed() → dispatch → sendq_push()
                           EPOLLOUT: sendq_flush() → disarm EPOLLOUT when drained
```

All sockets are `O_NONBLOCK`. EPOLLOUT is **armed lazily** — only when the
send queue has bytes remaining after a non-blocking flush. This avoids
spurious wakeups on quiet connections.

---

## Streaming Parser Design

`parser_feed()` is a four-state machine:

```
PS_SYNC ──(0xC0)──► PS_SYNC2 ──(0xDE)──► PS_HEADER ──(12 bytes)──► PS_PAYLOAD
  ▲                    |                      |                           |
  │                 (other)               bad magic                  CRC OK → emit
  │                    │                  bad version          ──────────────────►
  └────────────────────┘                  bad plen                    CRC BAD
                                          ─────────────────────────► _resync()
                                                                       (scan for
                                                                        next 0xC0)
```

Key properties:
- Accepts **arbitrary chunk sizes**, including 1 byte at a time.
- Handles **multiple packets per call** — the loop continues after emitting
  a packet.
- On a bad CRC, calls `_parser_resync()` which scans the buffered bytes for
  the next `0xC0` magic start and continues from there, minimising packet
  loss.
- The per-connection receive buffer is separate from the parser's internal
  state; after each `parser_feed()` call the receive buffer is reset to
  empty (the parser has consumed or internally buffered all bytes).

---

## Partial Read / Write Handling

**Partial reads:** `recv()` may return fewer bytes than a full packet.
The parser accumulates bytes across calls via its internal `buf[]` and
`buf_pos` cursor.  No data is ever discarded prematurely.

**Partial writes:** `send()` may write fewer bytes than requested.
Each connection has a **send queue** (a heap-allocated byte FIFO).
`sendq_push()` adds bytes; `sendq_flush()` drains via non-blocking `send()`
in a loop. If the queue is not fully drained, EPOLLOUT is armed so the
remainder is sent when the socket is writeable again.

---

## Why TCP Needs Framing

TCP provides a **reliable byte stream** — it guarantees delivery and order,
but does not preserve message boundaries. A single `send()` of N bytes may
arrive as multiple `recv()` calls, or multiple `send()` calls may be
coalesced by Nagle's algorithm into one `recv()`.

regsim solves this with:
1. **Length prefix** — the 2-byte `payload_len` field in the header tells
   the receiver exactly how many bytes follow.
2. **Magic sentinel** — `0xC0DE` lets the parser resynchronise after
   corruption without dropping the entire connection.
3. **CRC-32** — distinguishes valid packets from partial/corrupted data.

---

## TCP vs UDP Comparison

| Property           | TCP mode                      | UDP mode                          |
|--------------------|-------------------------------|-----------------------------------|
| Reliability        | Guaranteed delivery           | Best-effort; drops possible       |
| Ordering           | In-order delivery             | Out-of-order possible             |
| Framing            | Streaming parser required     | One packet per datagram           |
| Connections        | Per-client state (fd, parser, | Stateless; any packet from any IP |
|                    | send queue, idle timer)       |                                   |
| Latency            | Slightly higher (3-way HS,    | Lower (no connection setup)       |
|                    | TCP_NODELAY helps)            |                                   |
| Retry              | Transparent (TCP retransmits) | Application-level (client.c)      |
| Suitable for       | Long-lived sessions, dump,    | Single-shot probes, broadcasts    |
|                    | validation suite              |                                   |

For the register simulator use case TCP is the better choice: connections
persist across many operations, the send queue handles back-pressure cleanly,
and the streaming parser handles all fragmentation transparently.

---

## Debugging with tcpdump / Wireshark

### Capture

```bash
# On loopback, port 5555
sudo tcpdump -i lo -w regsim.pcap port 5555

# Or without saving, human-readable hex
sudo tcpdump -i lo -X port 5555
```

### Wireshark Display Filter

```
tcp.port == 5555
```

### Reading the Header by Hand

Bytes 0–11 of each TCP segment payload:

```
Offset  Size  Field
  0      2    Magic     → should be C0 DE
  2      1    Version   → 01
  3      1    Type      → 01=READ_REG 02=WRITE_REG 03=READ_RESP ...
  4      2    Sequence  → big-endian uint16
  6      2    PayLen    → big-endian uint16
  8      4    CRC-32    → big-endian uint32
  12+    N    Payload   → N = PayLen bytes
```

### Minimal Wireshark Lua Dissector

Save as `regsim.lua` and load via Wireshark → Analyze → Lua scripts:

```lua
-- regsim.lua — Wireshark dissector for the regsim protocol

local regsim_proto = Proto("regsim", "Register Simulator Protocol")

local f_magic   = ProtoField.uint16("regsim.magic",   "Magic",          base.HEX)
local f_version = ProtoField.uint8 ("regsim.version", "Version",        base.DEC)
local f_type    = ProtoField.uint8 ("regsim.type",    "Type",           base.HEX)
local f_seq     = ProtoField.uint16("regsim.seq",     "Sequence",       base.DEC)
local f_plen    = ProtoField.uint16("regsim.plen",    "Payload Length", base.DEC)
local f_crc     = ProtoField.uint32("regsim.crc",     "CRC-32",         base.HEX)
local f_payload = ProtoField.bytes ("regsim.payload", "Payload")

regsim_proto.fields = { f_magic, f_version, f_type, f_seq,
                         f_plen,  f_crc,     f_payload }

local TYPE_NAMES = {
    [0x01]="READ_REG", [0x02]="WRITE_REG",  [0x03]="READ_RESP",
    [0x04]="WRITE_ACK",[0x05]="ERROR",       [0x06]="PING",
    [0x07]="PONG",     [0x08]="RESET",       [0x09]="RESET_ACK"
}

function regsim_proto.dissector(buffer, pinfo, tree)
    if buffer:len() < 12 then return end
    local magic = buffer(0,2):uint()
    if magic ~= 0xC0DE then return end

    pinfo.cols.protocol = "REGSIM"

    local msg_type = buffer(3,1):uint()
    local type_name = TYPE_NAMES[msg_type] or string.format("0x%02X", msg_type)
    local plen = buffer(6,2):uint()

    local subtree = tree:add(regsim_proto, buffer(), "regsim " .. type_name)
    subtree:add(f_magic,   buffer(0,2))
    subtree:add(f_version, buffer(2,1))
    subtree:add(f_type,    buffer(3,1)):append_text(" (" .. type_name .. ")")
    subtree:add(f_seq,     buffer(4,2))
    subtree:add(f_plen,    buffer(6,2))
    subtree:add(f_crc,     buffer(8,4))
    if plen > 0 and buffer:len() >= 12 + plen then
        subtree:add(f_payload, buffer(12, plen))
    end
    pinfo.cols.info = type_name .. " seq=" .. buffer(4,2):uint()
end

-- Register on TCP port 5555 (change to match your --port)
local tcp_table = DissectorTable.get("tcp.port")
tcp_table:add(5555, regsim_proto)
```

---

## Validation Results

*(Run `bash scripts/run_validation.sh` to fill this table with real values.)*

| Category          | Test                           | Result |
|-------------------|--------------------------------|--------|
| Register          | reset_values                   |        |
| Register          | rw_readback                    |        |
| Register          | ro_rejected                    |        |
| Register          | w1c                            |        |
| Register          | reserved_bits                  |        |
| Register          | walking_ones                   |        |
| Register          | walking_zeros                  |        |
| Register          | ctrl_busy_done                 |        |
| Register          | counter_monotonic              |        |
| Register          | reset_restores_defaults        |        |
| Protocol          | bad_magic                      |        |
| Protocol          | bad_version                    |        |
| Protocol          | bad_crc                        |        |
| Protocol          | truncated                      |        |
| Protocol          | oversized_plen                 |        |
| Protocol          | unknown_type                   |        |
| Protocol          | unknown_register               |        |
| Protocol          | fragmented                     |        |
| Protocol          | back_to_back                   |        |
| Protocol          | abrupt_disconnect              |        |
| Protocol          | concurrent_50x1000             |        |
| Timing            | latency (min/avg/med/p99/max)  |        |
| Timing            | throughput (req/s)             |        |

---

## Design Decisions

**Single-threaded epoll instead of threads:**
One thread per connection does not scale past ~hundreds on Linux; the epoll
approach handles hundreds of idle connections with near-zero overhead.
All register-file state is accessed from a single thread, so no locking is
needed.

**No struct casting for network data:**
Casting a byte buffer directly to a packed struct is undefined behaviour in
C (strict aliasing) and produces wrong results on big-endian hosts or with
unaligned accesses. All fields are serialised and deserialised using explicit
byte reads and writes.

**Lazy CRC table initialisation:**
The 1 KiB lookup table is built once on the first call and reused. A
`crc32_init()` function is provided for explicit pre-initialisation in
multi-threaded contexts.

**W1C (write-1-to-clear) semantics:**
Writing `1` to a W1C bit clears it; writing `0` has no effect. This mirrors
real hardware interrupt status registers and is correctly implemented in both
`regfile.c` (model) and tested end-to-end in `validate.c`.

**Send queue instead of blocking write:**
On a busy server, a non-blocking `send()` may not consume all bytes.
Buffering the remainder and rearming EPOLLOUT prevents blocking the event
loop while waiting for the socket to drain.

---

## Known Limitations

- The register file is global per server instance; concurrent clients share
  it. This is intentional (it models a real shared hardware resource) but
  means register-test results depend on test ordering when running
  concurrently.
- The COUNTER register self-increments on reads, which means its reset value
  cannot be verified after any prior access. The validation suite skips its
  reset-value check and tests monotonicity instead.
- The FIFO model is cosmetic (depth 16, oldest entry overwritten on overflow)
  and is not tested via the network protocol (FIFO_DATA is WO and FIFO_STATUS
  is RO with no read path in the validation suite).
- UDP mode has no connection state, so idle-timeout and send-queue features
  do not apply.

---

## Future Work

- TLS/DTLS transport layer for security.
- Configurable register map loaded from a JSON or YAML file.
- Multi-instance support (multiple independent register files per connection,
  identified by a device ID in the header).
- A Python binding (`ctypes` or `cffi`) for use with pytest.
- Prometheus metrics endpoint (total requests, errors, latency histogram).
- Formal protocol state machine verification with a model checker.
