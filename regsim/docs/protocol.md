# regsim Wire Protocol Specification

Version 1 — IEEE 802.3 CRC-32 · TCP (or UDP) · Network byte order

---

## 1. Packet Structure

Every packet consists of a fixed 12-byte header followed by 0 to 256 bytes
of payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Magic (0xC0DE)       |  Version (1)  |     Type      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Sequence            |         Payload Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          CRC-32                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Payload (0..256 bytes)                     |
|                          ...                                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

---

## 2. Header Field Table

| Offset | Size | Field          | Description                                                  |
|--------|------|----------------|--------------------------------------------------------------|
| 0      | 2 B  | Magic          | Fixed value `0xC0DE`. Used for framing / resync.             |
| 2      | 1 B  | Version        | Protocol version. Currently `1`.                             |
| 3      | 1 B  | Type           | Message type (see §3).                                       |
| 4      | 2 B  | Sequence       | Monotonically increasing request ID. Response echoes it.     |
| 6      | 2 B  | Payload Length | Number of payload bytes that follow. 0–256.                  |
| 8      | 4 B  | CRC-32         | IEEE 802.3 reflected CRC over header (CRC field zeroed) + payload. |

All multi-byte fields are in **network byte order** (big-endian).

---

## 3. Message Types

| Value | Name        | Direction       | Payload     |
|-------|-------------|-----------------|-------------|
| 0x01  | READ_REG    | Client → Server | `addr[2]`   |
| 0x02  | WRITE_REG   | Client → Server | `addr[2] + value[4]` |
| 0x03  | READ_RESP   | Server → Client | `addr[2] + value[4]` |
| 0x04  | WRITE_ACK   | Server → Client | *(none)*    |
| 0x05  | ERROR       | Server → Client | `code[1] + bad_seq[2]` |
| 0x06  | PING        | Client → Server | *(none)*    |
| 0x07  | PONG        | Server → Client | *(none)*    |
| 0x08  | RESET       | Client → Server | *(none)*    |
| 0x09  | RESET_ACK   | Server → Client | *(none)*    |

### 3.1 Payload Layouts

**READ_REG (0x01)**
```
+-------+-------+
| addr[1] (MSB) |
+-------+-------+
| addr[0] (LSB) |
+-------+-------+
```
2 bytes total — 16-bit register address in network byte order.

**WRITE_REG (0x02)**
```
+--------+--------+--------+--------+--------+--------+
| addr high | addr low | value[3] | value[2] | value[1] | value[0] |
+--------+--------+--------+--------+--------+--------+
```
6 bytes total — addr (2 B, BE) followed by 32-bit value (4 B, BE).

**READ_RESP (0x03)** — identical layout to WRITE_REG.

**ERROR (0x05)**
```
+----------+----------+----------+
| err_code | seq_high | seq_low  |
+----------+----------+----------+
```
3 bytes total — error code (1 B) + sequence number of the offending request (2 B, BE).

---

## 4. Error Codes

| Code | Name                  | Meaning                                        |
|------|-----------------------|------------------------------------------------|
| 0x01 | BAD_MAGIC             | Magic bytes did not match `0xC0DE`.            |
| 0x02 | BAD_VERSION           | Version field is not `1`.                      |
| 0x03 | BAD_CRC               | CRC-32 mismatch.                               |
| 0x04 | UNKNOWN_TYPE          | Message type is not in the defined set.        |
| 0x05 | UNKNOWN_REGISTER      | Address does not map to any register.          |
| 0x06 | READ_ONLY_VIOLATION   | Client attempted to write a read-only register.|
| 0x07 | INVALID_LENGTH        | Payload length is wrong for the message type.  |
| 0x08 | INTERNAL              | Server-side internal error.                    |

---

## 5. Message Flows

### 5.1 Register Read

```
Client                          Server
  |                               |
  |---READ_REG(seq=N, addr=A)---->|
  |                               | (register lookup)
  |<--READ_RESP(seq=N, addr=A,    |
  |             value=V)----------|
```

### 5.2 Register Write

```
Client                          Server
  |                               |
  |---WRITE_REG(seq=N,----------->|
  |     addr=A, value=V)          |
  |                               | (register write)
  |<--WRITE_ACK(seq=N)------------|
```

### 5.3 Error Response

```
Client                          Server
  |                               |
  |---READ_REG(seq=N, addr=0xFFFF)|
  |                               | (unknown register)
  |<--ERROR(seq=M, code=0x05,     |
  |         bad_seq=N)------------|
```
Note: the ERROR packet uses a fresh server-side sequence number (M), and
`bad_seq` echoes the sequence of the offending request.

### 5.4 Ping/Pong

```
Client                          Server
  |                               |
  |---PING(seq=N)---------------->|
  |<--PONG(seq=N)-----------------|
```

### 5.5 Reset

```
Client                          Server
  |                               |
  |---RESET(seq=N)--------------->|
  |                               | (all registers → reset values)
  |<--RESET_ACK(seq=N)------------|
```

---

## 6. CRC-32 Computation

The CRC is IEEE 802.3 (Ethernet), reflected polynomial `0xEDB88320`.

Algorithm:
1. Copy the 12-byte header into a temporary buffer.
2. Zero the 4 CRC bytes (offset 8–11) in the copy.
3. Compute CRC-32 over (zeroed header || payload).
4. Write the result into the CRC field in network byte order.

Verification mirrors encoding: zero the CRC field, recompute, compare.

**Test vector:** `CRC-32("123456789") = 0xCBF43926`

---

## 7. Connection State Diagram

```
             TCP connect
                 |
                 v
         +---------------+
         |     IDLE      |  ← receives requests, sends responses
         +---------------+
          |           |
   RESET  |           |  idle timeout / EOF / unrecoverable error
          v           v
         +---------------+    +---------------+
         |    RESETTING  |    |    CLOSED     |
         +---------------+    +---------------+
                 |
         RESET_ACK sent
                 |
                 v
         +---------------+
         |     IDLE      |
         +---------------+
```

States:
- **IDLE** — normal operation; client sends requests, server responds.
- **RESETTING** — server is applying RESET to the register file; client
  waits for RESET_ACK before issuing new requests.
- **CLOSED** — TCP connection is torn down. The server closes only on
  unrecoverable protocol violations (e.g. a stream that cannot be
  resynchronised). A single bad packet results in an ERROR response and
  the connection remains open.

---

## 8. Framing and TCP Considerations

TCP is a byte-stream protocol with no inherent message boundaries.
Framing is provided by:

1. **Magic bytes** (`0xC0DE`) — the parser scans for this sentinel when
   out of sync.
2. **Fixed header size** — always 12 bytes; the payload length is decoded
   from the header to find the full packet boundary.
3. **CRC-32** — detects corruption and distinguishes valid packets from
   noise or partial data.

The streaming parser accepts arbitrary chunk sizes (including 1 byte at a
time). On a CRC failure it discards the bad packet and re-scans the
remaining bytes for the next magic word.

---

## 9. UDP Mode

When the server is started with `--udp`, each datagram is one complete
packet. There is no streaming parser — the server decodes each datagram
independently. Retransmission and ordering are the client's responsibility.
The same header format and CRC rules apply.

---

## 10. Endianness Summary

| Field          | Byte order |
|----------------|-----------|
| Magic          | Big-endian |
| Version        | N/A (1 byte) |
| Type           | N/A (1 byte) |
| Sequence       | Big-endian |
| Payload Length | Big-endian |
| CRC-32         | Big-endian |
| addr (payload) | Big-endian |
| value (payload)| Big-endian |
| err_code       | N/A (1 byte) |
| bad_seq        | Big-endian |
