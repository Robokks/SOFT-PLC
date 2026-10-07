# External I/O in Windows Studio

Studio supports **TCP, UDP, gRPC, Modbus TCP and Modbus RTU serial**. The PLC
can initiate exchanges as a client / Modbus master and listen as a server /
Modbus slave at the same time. Multiple configured links and multiple network
peers are supported. Mapping direction is always relative to the PLC: inputs
are values received by the PLC; outputs are values supplied by the PLC. Windows Portable includes the required Python
wheels; no Visual Studio, CMake, gRPC SDK or pip installation is needed to use
that package. The native scan engine remains a separate small C++ process.

## Connect from Studio

1. Compile and load the program. Leave the CPU in **STOP**.
2. Open **I/O connections**, add a connection, and choose its protocol and role.
3. Enter the device IP and port for a client, the local listen IP and port for
   a server, or the serial port and line settings.
4. Map device channels to declared PLC symbols or addresses such as
   `DB1.Start`, `DB1.DBX0.0`, or `%I0.0`. Mapped addresses must exist in the
   loaded program. Input mappings cannot write Q outputs or system tags.
5. Choose a cycle, whole-exchange timeout and failure policy.
6. Select **Save & connect**. Check for **online**, then select **RUN**.

Configuration changes take effect only when you select Save & connect in
STOP. Loading a program disconnects existing I/O; apply its mappings again.
Changing only `io_links` does not require recompilation. Connections are not
automatically started when Studio opens. Up to eight connections and 128
mappings in each direction per connection are supported. A tag has only one
input owner. Strings are not supported by these I/O mappings.

Live diagnostics show sequence/acknowledgement, exchange and error counts,
round-trip time, time since last good exchange and the latest error.

## Try a device without hardware

Open a command window in the extracted SOFT-PLC folder:

```bat
portable\python\python.exe tools\io_peer.py --protocol tcp --port 15000
```

Launch `Start-Studio.cmd`, compile/load the motor demo, add the default TCP
connection and select Save & connect. The reference peer supplies `start=true`
and prints the received `motor` output. Select RUN, observe the output, then
STOP. Use `--protocol udp --port 15001` or `--protocol grpc --port 50051` to try
the other transports, changing the Studio protocol and port accordingly.

The peer is a small reference device with sequence deduplication and an output
lease watchdog. It defaults to loopback. `--host` can bind a lab interface;
`--inputs` accepts a JSON object of constant input channels. The supplied
`examples/io_demo/project.json` contains disabled examples for all five
protocols. Enable **one** of those mappings at a time because they share tags.

For a source checkout with an existing Python 3.10+ installation:

```sh
python -m pip install --only-binary=:all: -r tools/requirements-io.txt
python tools/io_peer.py --protocol tcp
python tools/plc_studio.py
```

TCP, UDP and Modbus TCP use the standard library. gRPC uses `grpcio` and
`protobuf`; serial uses `pyserial`. Imports are lazy, so the core Studio still
works without these optional packages. Download dependencies only when
preparing a build; the Portable delivery runs offline.

## Multiple clients and servers at the same time

Select **Client / master** or **Server / slave** independently for each link.
The eight-link limit applies to the total. TCP, UDP, gRPC and Modbus TCP servers
accept up to `max_clients` active peers (default 16, configurable 1–32) each.
Use separate listen ports for separate server maps. TCP and gRPC listeners
cannot share a port; UDP uses a separate socket namespace. Use a local adapter
IP, `0.0.0.0` (all IPv4 adapters), or `::` (IPv6) for server binding. A remote
client connects to the machine's actual IP, not to a wildcard address.

A server can have many readers. Concurrent writers may own **different** PLC
input tags. The first successful write leases its tags to that peer. Another
peer cannot write those same tags until the owner expires. Custom protocols
report a failed exchange; Modbus reports exception 06 (server busy). This
prevents conflicting client writes. Different configured links cannot share
an input tag at all. `lease_ms` controls the server's peer watchdog (default
1,000 ms). For custom protocols the smaller of this value and the requested
lease applies. Duplicate sequences do not extend the lease. Modbus renews the
peer lease on successful requests. Modbus RTU has one master on its serial bus.

In a **custom protocol server**, incoming request `outputs` write the PLC's
mapped **inputs** and response `inputs` contain its mapped **outputs**. A peer
can write a subset of channels; `{}` makes it a read-only monitor. The response
also includes `outputs_enabled` to identify the PLC's RUN state. Start each new
session at sequence 1 with outputs disabled, including read-only clients.

In a **Modbus server**, PLC inputs are writable coils/holding registers and
PLC outputs can be read through coils, discrete inputs, holding or input
registers. Readback of writable mappings is supported. Server write functions
are FC05, FC06, FC15 and FC16; unsupported mapped addresses return exception 02.
No unmapped PLC memory is exposed. The server also accepts multiple independent
Modbus TCP sockets with separate writer ownership.

The `examples/io_multi/project.json` project supplies a mixed configuration:

| Connection | Role | Purpose |
|---|---|---|
| TCP 15000 | Client | Read start command from the reference peer |
| TCP 15010 | Server | Receive a stop command; publish motor status |
| gRPC 15011 | Server | Receive a speed command; publish calculated speed |
| Modbus TCP 15020 | Server | Read motor status and speed registers |

All are disabled in the sample until you enable the desired links. These maps
use separate input tags and can be enabled together. After connecting, try a
second terminal with the bundled reference **client**:

```bat
portable\python\python.exe tools\io_client.py --protocol tcp --port 15010
portable\python\python.exe tools\io_client.py --protocol grpc --port 15011 --write command=75
```

Run one command per terminal. The first monitors motor state; the second owns
the `command` input and monitors scaled speed. To command stop through the TCP
server use `--write stop=true`. The reference client's Ctrl+C performs a
best-effort disabled-output exchange before closing. A server applies its
configured failure policy when a writer's lease expires even after a graceful
client exit; STOP the PLC or choose an appropriate policy before ending a
commissioning session.

For a TLS gRPC listener set `tls:true`, `cert_file` and `key_file` to PEM files.
The files remain external to the exported project. A gRPC client uses `ca_file`
when the listener uses a private CA. Mutual TLS is not implemented.

## Scan behavior and failures

Each client polls and each server listens in workers outside the native scan thread. It snapshots outputs,
performs an exchange within a bounded deadline, validates all mapped inputs,
then commits them together between complete native scan dispatches. All
inputs from one exchange are accepted or rejected together. Different links
are asynchronous and do not form one global input snapshot. A program should
not also assign its externally owned input tags.

A native CPU generation counter changes on LOAD, RUN, STOP, RESET, STEP and
FAULT. An old reply cannot write into a newer generation, even when it loads
the same program again. Inputs continue updating in STOP for commissioning.
Each client first exchanges **zero outputs** before enabling outputs in RUN.
Servers supply zero output values whenever their CPU is not running.
STOP/FAULT mask all mapped outputs to zero, including outputs sourced from DBs.

| Failure policy | Behavior after a failed exchange |
|---|---|
| `fault` (default) | Fault the CPU if it is running; Q image becomes zero. RESET is required before RUN. |
| `zero` | Atomically zero that link's mapped inputs and keep the CPU running. |
| `hold` | Keep the last accepted inputs and keep the CPU running. |

A failed link disarms output transmission and reconnects with zero outputs.
Failures include timeout, protocol/CRC errors, missing channels, invalid values,
wrong acknowledgement or a peer reporting not-ready. Recovery requires a good
exchange before RUN outputs resume. The client retry delay is at least 200 ms. Server input leases apply the
same fault/zero/hold policy if a writer stops communicating; an idle server
with no writer does not fault the CPU. A
cycle is a target polling interval, not a real-time guarantee; a slow exchange
can exceed the interval without queuing more exchanges.

STOP cannot retract a frame already in flight. A normal update is bounded by
the active exchange timeout plus the next poll; CPU scheduling can add delay.
Orderly disconnect sends a final best-effort zero exchange. **Configure a
communication watchdog on physical outputs**: when a cable is disconnected,
the host cannot deliver zero values. The reference custom-protocol peer uses
`lease_ms`; standard Modbus equipment needs its own watchdog configuration.
This software is a soft-real-time engineering prototype, not a safety PLC.

## TCP / UDP / gRPC protocol version 1

All three use the same UTF-8 JSON envelope. PLC request:

```json
{"version":1,"session":"ba067f2813484859b98f83e6d11e3a01","seq":1,"outputs_enabled":false,"lease_ms":1000,"outputs":{"motor":false}}
```

Device response:

```json
{"version":1,"session":"ba067f2813484859b98f83e6d11e3a01","ack":1,"ready":true,"inputs":{"start":true}}
```

- `session` is a random 32-character hex ID. It changes when a link reconnects.
- `seq` increases for every exchange. The first exchange in a new session has
  sequence 1 and outputs disabled. Echo the session and sequence as `ack`.
- Apply a request only once. A duplicate request must have identical contents;
  return its cached reply. Reject reordered sequences and retired sessions.
- Set `ready=true` only after accepting the request and capturing the inputs.
- Honor `outputs_enabled=false` by zeroing outputs. Use a monotonic output
  watchdog; after `lease_ms` without a fresh valid request, set outputs to zero.
  Duplicates must not extend the lease. Re-arm with disabled outputs.
- Each mapped channel must appear in `inputs`. BOOL values are JSON booleans;
  integer types require JSON integers within their PLC range; REAL/LREAL must
  be finite numbers. Extra input channels are ignored. Preserve 64-bit integer
  precision if using a language whose default JSON numbers are doubles.

| Transport | Framing |
|---|---|
| TCP | Persistent connection. Four-byte unsigned **big-endian** JSON byte length, then JSON. Maximum 65,536 bytes. Handle split reads. |
| UDP | One JSON datagram, maximum 1,200 bytes each way. Up to three attempts using the same session/sequence within the exchange deadline. Replies from another address or stale sequence are ignored. |
| gRPC | Unary `/softplc.io.v1.IO/Exchange`. Request/response are `google.protobuf.BytesValue` with JSON bytes in `value`. Same 65,536-byte payload limit, per-call deadline. |

The portable interface is defined in [`tools/io_exchange.proto`](../tools/io_exchange.proto).
Generate stubs in C++, C#, Java, Go or another gRPC language from this file;
Python's implementation uses gRPC's generic unary API and the standard wrapper
message, so it needs no code-generation tools at run time.

gRPC offers server-verified TLS via `tls:true` and optional `ca_file` (PEM path).
Without `ca_file`, gRPC's default roots are used. Device addresses are numeric
IPv4/IPv6, so the server certificate must cover that IP. Mutual TLS is not
implemented. TCP, UDP, Modbus TCP and the reference peer use unencrypted lab
connections; use a controlled device network. The Studio web API remains
loopback-only with its session token.

## Modbus TCP and RTU

The new Studio I/O bridge implements Modbus framing directly, independently
of the older interpreter's libmodbus drivers. There is no libmodbus DLL to
install for Studio. All addresses below are **zero-based protocol offsets**;
for a device manual using holding register 40001, configure address `0`.

| Device area | Read function | Write function used | Mapping type |
|---|---|---|---|
| `coil` | FC01 | FC05 | `bool` |
| `discrete` | FC02 | Read only | `bool` |
| `holding` | FC03 | FC06 for one register, FC16 for multiple | Types below |
| `input` | FC04 | Read only | Types below |

Register types: `bool` (0/1), `uint16`, `int16`, `uint32`, `int32`, `float32`,
`float64`, `int64`. A two-byte register is big-endian; `word_order` is
`high_low` or `low_high` for multiregister values. BOOL maps to BOOL tags,
floating types to REAL/LREAL, and integer types to BYTE/INT/DINT/TIME. Values
must fit both the device encoding and PLC tag; there is no silent truncation.
Each mapping is one Modbus transaction; this first version does not coalesce
adjacent mappings. Increase the whole-exchange timeout for larger/slower maps.

TCP defaults to port 502 and checks MBAP transaction ID, protocol ID, unit,
length, function and acknowledgement. Both transports report device exception
codes. Unit IDs 1–247 are supported; broadcast writes are not used.

RTU client and server roles use an OS serial port (`COM3` on Windows; `/dev/ttyUSB0` on Linux), 8 data
bits, configurable baud/parity/stop bits, a CRC16 and an interframe quiet gap.
Default is 19200, even parity, 1 stop bit; use 2 stop bits with no parity for
standard 11-bit frames. One role and unit/connection owns each serial port in this version. Separate
COM ports may run client and server roles simultaneously. Multiunit scheduling on one port, Modbus ASCII and software RTS
RS-485 direction control are not implemented. Use an RS-485 adapter with
automatic transmit direction, or an RS-232 port with the appropriate wiring.
Hardware adapters and device timing must still be commissioned on the target
Windows machine; CI uses virtual serial transports, not physical equipment.

### Optional device-level register handshake

Ordinary Modbus devices work without `handshake`. Modbus transaction replies
confirm protocol operations, not that the device application consumed a whole
output set. For programmable devices you can add:

```json
"handshake": {"request":100,"ack":101,"ready":102}
```

These are distinct holding-register offsets, separate from mapped data.
In a PLC server, each TCP peer has separate staged writes, ACK and latched
outputs. Writing its request register commits that peer's staged PLC inputs
atomically and captures PLC outputs. Read the ACK, then read the output map.
Only complete mapped multiregister values may be written; partial values are
rejected to avoid tearing a PLC tag. Server handshake staging and output
latches are cleared on CPU generation changes. The
PLC writes output mappings, then writes a new nonzero 16-bit sequence to
`request`. It waits until `ack` equals that sequence and `ready` equals 1,
then reads inputs. `ready` can be omitted. On connection, the next sequence is
chosen from the current acknowledgement, avoiding an old matching ACK; 65535
wraps to 1. Timeout covers the entire group of transactions.

The device application must copy the staged outputs and latch its input
snapshot **before** acknowledging, retain those inputs until the next request,
and obey its own output watchdog. Without this device logic, separate Modbus
reads/writes are not a coherent process-image exchange. Normal Modbus write
responses alone do not supply this additional application handshake.

## Tests and protocol references

Run `python -m unittest discover -s tests/studio -v`. Tests exercise native
compiled modules over actual TCP/UDP/gRPC sockets, Modbus TCP fragmentation,
Modbus RTU bytes/CRC through pySerial, a Linux OS pseudo-terminal, malformed
acks, input ranges, atomic batches, late generations, peer leases and all
failure policies. Windows CI repeats I/O tests from the extracted Portable
package and tests the browser's I/O editor using its bundled Python. Server tests also
exercise simultaneous roles, multiple peers, writer conflicts, transactional
Modbus staging and server input leases.

- [Modbus application protocol V1.1b3](https://www.modbus.org/file/secure/modbusprotocolspecification.pdf)
- [Modbus serial line V1.02](https://www.modbus.org/file/secure/modbusoverserial.pdf)
- [gRPC Python API](https://grpc.github.io/grpc/python/grpc.html)
- [gRPC deadlines](https://grpc.io/docs/guides/deadlines/)
