# Dedicated peripheral transport v1.0

Implementation snapshot: 2026-10-07. This is the **host-side M2 foundation**:
`backend/PeripheralTransport` supplies a loopback TCP listener, incremental codec,
negotiation, correlation, bounds and host watchdog. It has no QEMU chardev hook,
guest virtual-time barrier, electrical solver or native peripheral-driver path.
The existing cached I2C/stderr extension remains a separate runtime interface.
Transport tests are host protocol tests, not firmware or peripheral qualification.

The transport owns envelope formatting, bounds, negotiation, identity/correlation
and response time/length consistency. It does not own the bus engine's legal phase
grammar, per-bit ACK/CS behavior, clock/IRQ/DMA semantics or endpoint reachability.
Those belong to the QEMU peripheral and authoritative net/mux/electrical models.
Formatter acceptance of an opaque ID or phase record is not proof of wiring or a
legal physical transaction.

## Wire framing and identifiers

Each record is four bytes of unsigned big-endian JSON-body length, followed by
exactly that many UTF-8 bytes. There is no newline terminator. Empty/oversized
prefixes, invalid UTF-8/JSON, duplicate members (including escaped aliases), more
than 64 nesting levels, unknown fields and incomplete frames at EOF fail the
connection. Logs and UART bytes never share this stream.

The codec accumulates at most one bounded body plus four prefix bytes. It consumes
fragmented/coalesced input incrementally through a callback, without accumulating
a list of completed messages. The endpoint reads 64 KiB chunks, at most four per
event-loop turn; its socket read buffer is bounded to the local frame limit plus
prefix. Callers of the standalone codec own the provided input chunk and callback
storage; those are not secretly retained by the codec.
Incomplete prefix/body assembly has a monotonic host watchdog, including a complete
prefix with zero body bytes. Progress does not restart that deadline indefinitely;
expiry fails the channel without creating a modeled hardware timeout.

Every envelope has exactly these fields. [bus.schema.json](bus.schema.json) describes
their structural types; the codec/endpoint additionally enforce negotiated and
stateful constraints.

| Field | Representation and rule |
| --- | --- |
| `version` | `{major: 1, minor: 0}` for this implementation. The codec recognizes the numeric minor range 0–65535; the endpoint selects only implemented minor 0 and rejects unsupported selections |
| `kind` | `hello`, `hello_ack`, `request`, `response`, `event`, `reset` |
| `status` | `ok`, `pending`, `nack`, `error`, `timeout`, `unavailable`; allowed values depend on kind |
| `session_id` | Canonical lowercase, nonzero UUID of the simulator session |
| `connection_id` | New canonical lowercase, nonzero UUID chosen by the host for each accepted physical connection |
| `reset_epoch` | Canonical decimal uint64 string; no signs, leading zeros or JSON numbers |
| `topology_generation` | Canonical decimal uint64 string; supplied by the project/engine integration |
| `sequence` | Positive decimal uint64 string, monotonically increasing per direction/connection; gaps allowed, duplicates/lower values discarded |
| `request_id` | Positive decimal uint64 ordinal for requests/responses; empty string for control/events. Request ordinals increase per direction/connection and responses echo them |
| `virtual_time_ns` | Canonical decimal uint64 string, nanoseconds supplied by the caller; never derived from host wall time |
| `data` | Kind-specific, strictly checked object |

The uint64 domain is 0–18446744073709551615. Sequence and request ordinals start at
1. Ordinal high-water marks detect reused/out-of-order peer requests without an
unbounded history set. The active request maps supply response correlation; unknown
or completed ordinals are discarded. Session/connection/generation are checked
before an old ordinal can affect a current request. A new connection may restart
its counters because its connection UUID changes.

Endpoint, controller and net IDs are **opaque graph references** (1–64 ASCII
characters, `[A-Za-z0-9][A-Za-z0-9_.:/-]*`). The transport does not infer pin routing
from a controller name, address, CS number or component type. Endpoint/net lists
are unique and bounded to 64 entries; endpoint lists may be empty for absent-device
transactions, while a bus request must identify at least one resolved net.

## Host-first handshake and bounds

The listener accepts one peer on `127.0.0.1`. It sends `hello`, sequence 1. The peer
returns `hello_ack`, sequence 1, echoing session/connection/generation, selecting
supported minor 0, a subset of offered capabilities and limits no greater than
the offers. Both use status `ok`, an empty request ordinal and this data shape:

All endpoint methods run in the owning Qt thread; other threads use queued calls.
Socket processing and watchdogs use that thread's event loop, without blocking
waits. The endpoint's `Ready` state means channel negotiation, not guest execution.

```json
{
  "capabilities": ["bus.i2c.v1", "control.reset-generation", "payload.base64"],
  "required_capabilities": ["control.reset-generation", "payload.base64"],
  "limits": {"frame_bytes": 1048576, "bus_payload_bytes": 65536, "in_flight": 64}
}
```

Capability tokens are unique lowercase tokens of 1–64 characters, with at most
32 supported and 32 required entries. Required entries must be selected. The core
reset/base64 capabilities remain mandatory. Defaults additionally offer GPIO,
I2C and SPI request formats; this only declares transport format handling, **not
native hardware or device-model support**. Other bus formats can be configured
before connecting and need their own engine/device qualification.

| Limit | Valid configuration | Default and behavior |
| --- | --- | --- |
| JSON body | 512–1048576 bytes | 1 MiB; prefix rejected before body allocation if larger |
| Decoded bus payload | 1–65536 bytes | 64 KiB; canonical base64 explicitly declared |
| In-flight requests | 1–64 | 64 combined incoming/outgoing requests |
| Outbound queued bytes | 512–4194304 | 4 MiB including complete framed records and socket bytes awaiting write |
| Consecutive discarded records | 1–256 | 256; reaches limit → protocol failure and disconnect |
| Host watchdog | 1–600000 ms | 5000 ms; independent of all virtual deadlines |

Frame, payload and in-flight limits are negotiated downward; queue/discard/watchdog
limits remain local policy. JSON overhead consumes frame capacity, so a reduced
frame cap can reject a payload otherwise under the decoded-byte cap. No field or
payload is silently truncated. Local enqueue saturation rejects the operation and
preserves the live connection; an inbound peer exceeding in-flight bounds fails
the connection. Accepted frames enter a bounded queue and are written in order.

The numeric ranges are necessary, not sufficient: configuration must also fit the
complete handshake and reset control records. Before accepting limits/capabilities,
starting a session/listener or selecting peer limits, the endpoint computes their
maximum control-frame overhead with canonical UUIDs and maximum uint64 counters.
Both body and framed queue capacity must fit. An impossible combination (including
512-byte frame/queue with the default features) is rejected as a configuration
error before opening a listener; adding capabilities is revalidated atomically.
String patterns use full-span/absolute-end checks, so final LF/whitespace cannot
be accepted as part of canonical identifiers, features or decimal counters.

Before negotiation, bus traffic is rejected. An increased offer, unsupported
major/minor, unoffered feature, omitted required feature or malformed handshake
fails explicitly. A handshake without a reply is a host-service stall, not a
firmware bus timeout. Limits/capabilities cannot change on an active connection.

## Request, response and event shapes

Requests have status `pending` and these required data fields:

```json
{
  "bus": "i2c",
  "controller_id": "i2c0",
  "endpoint_ids": ["sensor0"],
  "net_ids": ["net.sda", "net.scl"],
  "phases": [{"kind":"start"}, {"kind":"address","address":64,"direction":"write"},
             {"kind":"write","length":1}, {"kind":"restart"},
             {"kind":"address","address":64,"direction":"read"},
             {"kind":"read","length":3}, {"kind":"stop"}],
  "bit_length": "8",
  "bit_order": "msb-first",
  "mode": null,
  "chip_select": null,
  "read_length": 3,
  "virtual_deadline_ns": "4000",
  "payload_encoding": "base64",
  "payload": "4w=="
}
```

Bus names are `i2c`, `spi`, `gpio`, `uart`, `i2s`, `rmt`, `lcd`, `cam`. A matching
`bus.<name>.v1` feature must have been negotiated. Phases are bounded to 1–64 and
preserve `start/address/write/read/restart/stop`, with declared command/dummy/
edge/sample/frame variants for other buses. Each phase accepts only kind and the
documented optional length/address/direction/bit-length/level/duration fields.
Address is 0–1023, phase byte length is within the negotiated payload cap, and
bit lengths are canonical strings bounded to payload bits plus 4096 framing bits.
SPI requires mode 0–3 and allows CS 0–5 or null for explicit external selection;
non-SPI mode/CS fields are null. Bit order is `msb-first` or `lsb-first`.
An optional `parameters` object holds explicitly versioned model settings; accepting
it does not validate a peripheral's register semantics. Its compact UTF-8 JSON is
bounded to 16 KiB, independently of the frame and decoded-payload limits.

Responses echo the exact bus/controller/endpoint/net context and request ordinal:

```json
{
  "bus": "i2c", "controller_id": "i2c0",
  "endpoint_ids": ["sensor0"], "net_ids": ["net.sda", "net.scl"],
  "phase_statuses": [{"index":1,"status":"ack"}],
  "modeled_latency_ns": "100", "accepted_length": 1,
  "payload_encoding": "base64", "payload": "ESIz", "error_message": ""
}
```

Each phase result references an existing request phase exactly once and uses
`ack/nack/error/timeout`. Accepted TX length cannot exceed request TX bytes; RX
cannot exceed `read_length`. Response virtual time is at or after request time,
and modeled latency equals their difference. An ordinary response completes by
the virtual deadline; an explicit modeled `timeout` completes at or after that
deadline. These are model-supplied outcomes. The host watchdog generates no response.

Host model replies call `sendResponse(originalRequest, ...)` with the complete
immutable `BusEnvelope` received by `requestReceived`. The endpoint checks original
session, connection, epoch, topology, sequence and ordinal against active work,
along with original version/time/data, before deriving a response. An ordinal-only
API is deliberately absent: a delayed model callback must not guess current IDs
or answer a reused ordinal after reconnect/reset.

Events use status `ok/error`, empty request ordinal and exactly
`{event, payload_encoding, payload, details}`. The details object's compact UTF-8
JSON is bounded to 16 KiB. Event/details carry typed model
metadata; subsequent integration must determine whether a timestamp is still
causally admissible in the guest. This endpoint does not own the QEMU clock.

## Reset, reconnect and failures

`resetGeneration` advances reset epoch and/or topology generation monotonically,
cancels pending requests, removes locally queued old frames and queues a host
`reset` record with `{reason}` before notifying observers. New sends are gated
during cancellation notifications; retry after the final `generationChanged`
notification appends behind Reset and remains in the active request map. Already
written TCP bytes cannot be unsent; they
precede the reset record. Old replies are discarded. Applying topology/reset at a
guest quiescent boundary remains a required integration gate.

A reset during handshake disconnects and requires renegotiation; a reconnect gets
a new connection UUID and counters. Disconnect cancels both request maps. An old
reply cannot match a new request even if the session, epoch and ordinal are equal.
Device peers do not issue global reset/topology records or advance host generations.

Duplicate/lower sequences, old generations/connections and unknown/completed reply
ordinals emit `messageDiscarded`. Excessive consecutive discards fail the channel.
Future generations, context mismatches, invalid response lengths/times and invalid
frames emit `protocolError`, cancel bounded pending work and disconnect.

The monotonic host watchdog covers handshake and incoming/outgoing pending work.
Expiry sets transport state `Stalled`, emits `hostServiceStalled` with the original
envelope/direction and cancels/disconnects. It never writes a `timeout` response,
changes virtual timestamps or silently lets a slow host determine firmware behavior.
The future orchestrator must turn this into a paused/error simulation while retaining
control responsiveness. That QEMU pause/barrier integration is **not implemented**.

## QEMU integration and determinism gates still pending

1. Implement a QEMU chardev peer with the same framed protocol and capability
   mapping. Keep UART and logs separate; wire actual bus replies before completion.
2. Bind these opaque IDs to the authoritative project/net/mux model. Controller
   metadata does not bypass a disconnected wire, missing pullup or conflicting CS.
3. Add QEMU virtual-time dependency barriers/time horizons without blocking the Aio
   loop; integrate clock/reset/IRQ/DMA state and host-service cancellation.
4. Scripted/recorded inputs and seeded models require deterministic scheduling and
   repeatable guest results under varied host latency. This endpoint's sequence and
   timestamps alone do not establish that guarantee.
5. Live NAT/TAP/physical inputs are nondeterministic until captured; record every
   guest-affecting input before claiming replay. A socket test is not whole-VM replay.
6. Verify native I2C/SPI, positive/errors/reset and edge/fast-path equivalence before
   advertising device support. Bulk binary/shared-memory streaming is a later
   negotiated extension with independent ownership/backpressure acceptance.

Relevant primary APIs and scheduling references:
[Qt socket buffering](https://doc.qt.io/qt-6/qabstractsocket.html),
[Qt TCP server](https://doc.qt.io/qt-6/qtcpserver.html),
[QEMU character devices](https://www.qemu.org/docs/master/system/invocation.html),
[QEMU instruction-count scheduling](https://www.qemu.org/docs/master/devel/tcg-icount.html),
[QEMU record/replay](https://www.qemu.org/docs/master/system/replay.html).
