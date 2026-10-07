"""Cyclic I/O clients. No device wait occurs inside the native PLC scan thread."""
from __future__ import annotations
import copy
import ipaddress
import json
import math
from pathlib import Path
import secrets
import socket
import struct
import threading
import time
from plc_project import ProjectError, integer

PROTOCOLS = ('tcp', 'udp', 'grpc', 'modbus_tcp', 'modbus_rtu')
FORMATS = {'bool': 'H', 'uint16': 'H', 'int16': 'h', 'uint32': 'I', 'int32': 'i',
           'float32': 'f', 'float64': 'd', 'int64': 'q'}
MAX_FRAME = 65536
RPC_PATH = '/softplc.io.v1.IO/Exchange'


def remaining(deadline):
    value = deadline - time.monotonic()
    if value <= 0:
        raise TimeoutError('I/O exchange deadline exceeded')
    return value


def encode_frame(frame):
    data = json.dumps(frame, separators=(',', ':'), allow_nan=False).encode('utf-8')
    if len(data) > MAX_FRAME:
        raise ProjectError('I/O frame exceeds 64 KiB')
    return data


def decode_frame(data):
    if len(data) > MAX_FRAME:
        raise ProjectError('I/O frame exceeds 64 KiB')
    frame = json.loads(data.decode('utf-8'))
    if not isinstance(frame, dict):
        raise ProjectError('Expected an I/O object')
    return frame


def recv_exact(sock, size, deadline):
    result = bytearray()
    while len(result) < size:
        sock.settimeout(remaining(deadline))
        part = sock.recv(size - len(result))
        if not part:
            raise ConnectionError('Peer closed the connection')
        result.extend(part)
    return bytes(result)


def validate_links(links):
    if not isinstance(links, list) or len(links) > 8:
        raise ProjectError('io_links must be a list of at most 8 connections')
    names, serial_ports = set(), set()
    for link in links:
        if not isinstance(link, dict):
            raise ProjectError('Each I/O connection must be an object')
        name = link.get('name', '')
        if not isinstance(name, str) or not 1 <= len(name) <= 64 or any(c.isspace() for c in name):
            raise ProjectError('I/O name must contain 1–64 characters without spaces')
        if name in names:
            raise ProjectError('Duplicate I/O connection name: ' + name)
        names.add(name)
        role = link.get('role', 'client')
        if role not in ('client', 'server'):
            raise ProjectError('I/O role must be client or server')
        integer(link.get('max_clients', 16), 1, 32, 'Maximum server clients')
        integer(link.get('lease_ms', 1000), 50, 180000, 'Server peer watchdog (ms)')
        protocol = link.get('protocol')
        if protocol not in PROTOCOLS:
            raise ProjectError('Unknown I/O protocol')
        if not isinstance(link.get('enabled', False), bool):
            raise ProjectError('I/O enabled must be true or false')
        integer(link.get('cycle_ms', 20), 5, 60000, 'I/O cycle (ms)')
        integer(link.get('timeout_ms', 500), 20, 5000, 'I/O whole-exchange timeout (ms)')
        if link.get('fail_policy', 'fault') not in ('fault', 'zero', 'hold'):
            raise ProjectError('I/O failure policy must be fault, zero or hold')
        if protocol != 'modbus_rtu':
            try:
                ipaddress.ip_address(link.get('host', '127.0.0.1'))
            except ValueError:
                raise ProjectError('Use a numeric IPv4/IPv6 device address') from None
            integer(link.get('port', 502 if protocol == 'modbus_tcp' else 50051), 1, 65535, 'Port')
        else:
            port = link.get('serial_port', 'COM1')
            if not isinstance(port, str) or not port or len(port) > 256:
                raise ProjectError('Enter a serial port such as COM3 or /dev/ttyUSB0')
            if '://' in port:
                raise ProjectError('Use a physical serial port, not a serial URL')
            if link.get('enabled') and port.lower() in serial_ports:
                raise ProjectError('Only one active connection may own a serial port')
            if link.get('enabled'):
                serial_ports.add(port.lower())
            integer(link.get('baudrate', 19200), 1200, 1000000, 'Baud rate')
            if link.get('parity', 'E') not in ('N', 'E', 'O') or (type(link.get('stopbits', 1)) is not int or link.get('stopbits', 1) not in (1, 2)):
                raise ProjectError('Serial parity is N/E/O; stop bits are 1 or 2')
        if protocol.startswith('modbus'):
            integer(link.get('unit', 1), 1, 247, 'Modbus unit ID')
        if role == 'server' and link.get('tls'):
            if protocol != 'grpc' or not link.get('cert_file') or not link.get('key_file'):
                raise ProjectError('A TLS gRPC server needs certificate and private key files')
        if not isinstance(link.get('tls', False), bool):
            raise ProjectError('gRPC TLS must be true or false')
        mapped = set()
        for direction in ('inputs', 'outputs'):
            rows = link.get(direction, [])
            if not isinstance(rows, list) or len(rows) > 128:
                raise ProjectError('Each direction supports at most 128 mappings')
            channels = set()
            for row in rows:
                if not isinstance(row, dict) or not isinstance(row.get('tag'), str) or not row['tag']:
                    raise ProjectError('Every mapping needs a PLC tag name or address')
                if protocol.startswith('modbus'):
                    area = row.get('area', 'holding')
                    if area not in ('coil', 'discrete', 'holding', 'input') or (direction == ('outputs' if role == 'client' else 'inputs') and area not in ('coil', 'holding')):
                        raise ProjectError('Writable Modbus mappings must use coils or holding registers')
                    dtype = row.get('datatype', 'uint16')
                    if dtype not in FORMATS or (area in ('coil', 'discrete') and dtype != 'bool'):
                        raise ProjectError('Invalid Modbus datatype (bits require bool)')
                    width = 1 if area in ('coil', 'discrete') else struct.calcsize('>' + FORMATS[dtype]) // 2
                    address = integer(row.get('address', 0), 0, 65536 - width, 'Zero-based Modbus address')
                    if row.get('word_order', 'high_low') not in ('high_low', 'low_high'):
                        raise ProjectError('Word order must be high_low or low_high')
                    for offset in range(width):
                        key = (direction if role == 'client' else 'server', area, address + offset)
                        if key in mapped:
                            raise ProjectError('Overlapping Modbus mappings')
                        mapped.add(key)
                else:
                    channel = row.get('channel', '')
                    if not isinstance(channel, str) or not 1 <= len(channel) <= 64 or channel in channels:
                        raise ProjectError('Channel names must be unique and contain 1–64 characters')
                    channels.add(channel)
        handshake = link.get('handshake')
        if handshake:
            if not protocol.startswith('modbus') or not isinstance(handshake, dict):
                raise ProjectError('Register handshakes are for Modbus only')
            regs = [integer(handshake.get(k), 0, 65535, 'Handshake register ' + k) for k in ('request', 'ack')]
            if handshake.get('ready') is not None:
                regs.append(integer(handshake['ready'], 0, 65535, 'Ready register'))
            if len(set(regs)) != len(regs) or any(area == 'holding' and addr in regs for _, area, addr in mapped):
                raise ProjectError('Handshake registers must be distinct and must not overlap mappings')
    return links


def scalar(value, typ):
    if typ == 'BOOL':
        if type(value) is not bool:
            raise ProjectError('BOOL inputs require true/false')
        return value
    if typ in ('REAL', 'LREAL'):
        if type(value) not in (int, float) or not math.isfinite(value) or (typ == 'REAL' and abs(value) > 3.402823466e38):
            raise ProjectError('Non-finite or out-of-range real input')
        return value
    bounds = {'BYTE': (0, 255), 'INT': (-32768, 32767), 'DINT': (-2147483648, 2147483647),
              'TIME': (-9223372036854775808, 9223372036854775807)}
    if typ not in bounds or type(value) is not int or not bounds[typ][0] <= value <= bounds[typ][1]:
        raise ProjectError('Wrong type or out-of-range integer input')
    return value


def bind_links(links, snapshot):
    validate_links(links)
    symbols = {}
    for tag in snapshot.get('tags', []):
        symbols[tag['name']] = tag
        if tag.get('address'):
            symbols[tag['address'].lstrip('%').upper()] = tag
    result, owners = [], set()
    for config in links:
        if not config.get('enabled', False):
            continue
        link = copy.deepcopy(config)
        for direction in ('inputs', 'outputs'):
            for row in link.get(direction, []):
                tag = symbols.get(row['tag']) or symbols.get(row['tag'].lstrip('%').upper())
                if not tag or tag['type'] == 'STRING' or tag['name'].startswith(('System.', '__plc_')) or '.__plc_' in tag['name']:
                    raise ProjectError('Unknown or unsupported I/O tag: ' + row['tag'])
                if direction == 'inputs':
                    if tag['area'] == 2:
                        raise ProjectError('An input mapping cannot write a Q output')
                    if tag['id'] in owners:
                        raise ProjectError('Multiple input mappings write ' + tag['name'])
                    owners.add(tag['id'])
                if link['protocol'].startswith('modbus'):
                    dt = row.get('datatype', 'uint16')
                    if (tag['type'] == 'BOOL') != (dt == 'bool') or (tag['type'] in ('REAL', 'LREAL')) != dt.startswith('float'):
                        raise ProjectError('Modbus datatype must match BOOL / integer / real PLC type: ' + row['tag'])
                row['_id'], row['_type'] = tag['id'], tag['type']
        link['_build_id'] = snapshot['build_id']
        result.append(link)
    return result


class JsonClient:
    def __init__(self, link):
        self.link, self.sock = link, None

    def exchange(self, frame, deadline):
        udp = self.link['protocol'] == 'udp'
        if self.sock is None:
            host, port = self.link.get('host', '127.0.0.1'), self.link.get('port', 50051)
            if udp:
                self.sock = socket.socket(socket.AF_INET6 if ':' in host else socket.AF_INET, socket.SOCK_DGRAM)
                self.sock.connect((host, port))
            else:
                self.sock = socket.create_connection((host, port), timeout=remaining(deadline))
        data = encode_frame(frame)
        if udp:
            if len(data) > 1200:
                raise ProjectError('UDP frames must be at most 1200 bytes; use TCP/gRPC for larger maps')
            # Retransmit the SAME sequence. The peer must deduplicate it.
            for attempt in range(3):
                self.sock.settimeout(remaining(deadline) / (3 - attempt))
                self.sock.send(data)
                attempt_end = time.monotonic() + remaining(deadline) / (3 - attempt)
                while True:
                    self.sock.settimeout(remaining(min(deadline, attempt_end)))
                    try:
                        reply = decode_frame(self.sock.recv(1201))
                    except socket.timeout:
                        break
                    if reply.get('session') == frame['session'] and reply.get('ack') == frame['seq']:
                        return reply
            raise TimeoutError('UDP acknowledgement timed out')
        self.sock.settimeout(remaining(deadline))
        self.sock.sendall(struct.pack('>I', len(data)) + data)
        size = struct.unpack('>I', recv_exact(self.sock, 4, deadline))[0]
        if not 1 <= size <= MAX_FRAME:
            raise ProjectError('Invalid TCP frame size')
        return decode_frame(recv_exact(self.sock, size, deadline))

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None


class GrpcClient:
    def __init__(self, link):
        try:
            import grpc
            from google.protobuf.wrappers_pb2 import BytesValue
        except ImportError:
            raise ProjectError('gRPC needs tools/requirements-io.txt; Portable includes it') from None
        self.message = BytesValue
        host = link.get('host', '127.0.0.1')
        target = (f'[{host}]' if ':' in host else host) + ':' + str(link.get('port', 50051))
        options = [('grpc.max_receive_message_length', MAX_FRAME + 32), ('grpc.max_send_message_length', MAX_FRAME + 32)]
        if link.get('tls'):
            roots = Path(link['ca_file']).read_bytes() if link.get('ca_file') else None
            self.channel = grpc.secure_channel(target, grpc.ssl_channel_credentials(root_certificates=roots), options=options)
        else:
            self.channel = grpc.insecure_channel(target, options=options)
        self.call = self.channel.unary_unary(RPC_PATH, request_serializer=BytesValue.SerializeToString,
                                            response_deserializer=BytesValue.FromString)

    def exchange(self, frame, deadline):
        result = self.call(self.message(value=encode_frame(frame)), timeout=remaining(deadline))
        return decode_frame(result.value)

    def close(self):
        self.channel.close()


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def register_bytes(row, value=None, raw=None):
    fmt = '>' + FORMATS[row.get('datatype', 'uint16')]
    if raw is None:
        raw = struct.pack(fmt, value)
    if row.get('word_order', 'high_low') == 'low_high':
        raw = b''.join(raw[i:i + 2] for i in range(len(raw) - 2, -1, -2))
    if value is not None:
        return raw
    value = struct.unpack(fmt, raw)[0]
    if row.get('datatype') == 'bool':
        if value not in (0, 1):
            raise ProjectError('Modbus BOOL register must contain 0 or 1')
        return bool(value)
    return value


class ModbusClient:
    def __init__(self, link):
        self.link, self.sock, self.serial = link, None, None
        self.transaction, self.sequence, self.last_rx = 0, None, 0.0

    def request(self, pdu, deadline):
        unit = self.link.get('unit', 1)
        if self.link['protocol'] == 'modbus_tcp':
            if self.sock is None:
                self.sock = socket.create_connection((self.link.get('host', '127.0.0.1'), self.link.get('port', 502)), timeout=remaining(deadline))
            self.transaction = (self.transaction + 1) & 65535
            self.sock.settimeout(remaining(deadline))
            self.sock.sendall(struct.pack('>HHHB', self.transaction, 0, len(pdu) + 1, unit) + pdu)
            tx, protocol, size, remote_unit = struct.unpack('>HHHB', recv_exact(self.sock, 7, deadline))
            if (tx, protocol, remote_unit) != (self.transaction, 0, unit) or not 2 <= size <= 254:
                raise ProjectError('Invalid Modbus TCP transaction, unit or length')
            reply = recv_exact(self.sock, size - 1, deadline)
        else:
            if self.serial is None:
                try:
                    import serial
                except ImportError:
                    raise ProjectError('Serial RTU needs pyserial; Portable includes it') from None
                self.serial = serial.Serial(self.link.get('serial_port', 'COM1'), baudrate=self.link.get('baudrate', 19200),
                                            bytesize=8, parity=self.link.get('parity', 'E'), stopbits=self.link.get('stopbits', 1),
                                            timeout=remaining(deadline), write_timeout=remaining(deadline))
            baud = self.link.get('baudrate', 19200)
            char_bits = 1 + 8 + (self.link.get('parity', 'E') != 'N') + self.link.get('stopbits', 1)
            gap = 0.00175 if baud > 19200 else 3.5 * char_bits / baud
            delay = max(0, gap - (time.monotonic() - self.last_rx))
            if delay:
                if delay >= remaining(deadline):
                    raise TimeoutError('Serial exchange deadline exceeded')
                time.sleep(delay)
            self.serial.reset_input_buffer()
            body = bytes([unit]) + pdu
            self.serial.write_timeout = remaining(deadline)
            if self.serial.write(body + struct.pack('<H', crc16(body))) != len(body) + 2:
                raise TimeoutError('Incomplete serial write')
            def read(size):
                data = bytearray()
                while len(data) < size:
                    self.serial.timeout = remaining(deadline)
                    part = self.serial.read(size - len(data))
                    if not part:
                        raise TimeoutError('Modbus RTU response timed out')
                    data.extend(part)
                return bytes(data)
            header = read(3)
            if header[0] != unit:
                raise ProjectError('Modbus RTU unit mismatch')
            count = 2 if header[1] & 128 else header[2] + 2 if pdu[0] in (1, 2, 3, 4) else 5
            if count > 253:
                raise ProjectError('Invalid Modbus RTU length')
            packet = header + read(count)
            self.last_rx = time.monotonic()
            if crc16(packet[:-2]) != struct.unpack('<H', packet[-2:])[0]:
                raise ProjectError('Modbus RTU CRC mismatch')
            reply = packet[1:-2]
        if reply[0] == (pdu[0] | 128) and len(reply) == 2:
            raise ProjectError(f'Modbus exception {reply[1]} for function {pdu[0]}')
        if reply[0] != pdu[0]:
            raise ProjectError('Modbus function mismatch')
        return reply

    def read(self, area, address, count, deadline):
        function = {'coil': 1, 'discrete': 2, 'holding': 3, 'input': 4}[area]
        reply = self.request(struct.pack('>BHH', function, address, count), deadline)
        size = (count + 7) // 8 if function <= 2 else 2 * count
        if len(reply) != size + 2 or reply[1] != size:
            raise ProjectError('Modbus read byte count mismatch')
        return reply[2:]

    def write(self, row, value, deadline):
        address = row.get('address', 0)
        if row.get('area', 'holding') == 'coil':
            pdu = struct.pack('>BHH', 5, address, 0xFF00 if value else 0)
        else:
            data = register_bytes(row, value=value)
            pdu = struct.pack('>BH', 6, address) + data if len(data) == 2 else struct.pack('>BHHB', 16, address, len(data) // 2, len(data)) + data
        reply = self.request(pdu, deadline)
        expected = pdu[:5] if pdu[0] == 16 else pdu
        if reply != expected:
            raise ProjectError('Modbus write acknowledgement mismatch')

    def exchange(self, frame, deadline):
        handshake = self.link.get('handshake')
        if handshake and self.sequence is None:
            self.sequence = struct.unpack('>H', self.read('holding', handshake['ack'], 1, deadline))[0]
        for index, row in enumerate(self.link.get('outputs', [])):
            self.write(row, frame['outputs'][str(index)], deadline)
        if handshake:
            self.sequence = self.sequence % 65535 + 1
            self.write({'address': handshake['request'], 'datatype': 'uint16'}, self.sequence, deadline)
            while True:
                ack = struct.unpack('>H', self.read('holding', handshake['ack'], 1, deadline))[0]
                ready = handshake.get('ready')
                is_ready = ready is None or self.read('holding', ready, 1, deadline) == b'\x00\x01'
                if ack == self.sequence and is_ready:
                    break
                time.sleep(min(0.005, remaining(deadline)))
        values = {}
        for index, row in enumerate(self.link.get('inputs', [])):
            area = row.get('area', 'holding')
            width = 1 if area in ('coil', 'discrete') else struct.calcsize('>' + FORMATS[row.get('datatype', 'uint16')]) // 2
            raw = self.read(area, row.get('address', 0), width, deadline)
            values[str(index)] = bool(raw[0] & 1) if area in ('coil', 'discrete') else register_bytes(row, raw=raw)
        return {'version': 1, 'session': frame['session'], 'ack': frame['seq'], 'ready': True, 'inputs': values}

    def close(self):
        for resource in (self.sock, self.serial):
            if resource:
                resource.close()
        self.sock = self.serial = None


def make_client(link):
    if link['protocol'].startswith('modbus'):
        return ModbusClient(link)
    return GrpcClient(link) if link['protocol'] == 'grpc' else JsonClient(link)


class LinkWorker:
    def __init__(self, link, runtime):
        self.link, self.runtime = link, runtime
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.info = {'name': link['name'], 'protocol': link['protocol'], 'role': 'client', 'clients': 1, 'state': 'connecting',
                     'exchanges': 0, 'errors': 0, 'last_error': '', 'seq': 0, 'ack': 0, 'latency_ms': 0, 'last_good': None}
        self.thread = threading.Thread(target=self.run, name='io-' + link['name'], daemon=True)

    def status(self):
        with self.lock:
            result = dict(self.info)
        last = result.pop('last_good')
        result['age_ms'] = round((time.monotonic() - last) * 1000) if last is not None else None
        return result

    def write_inputs(self, snapshot, values):
        fields = []
        for index, row in enumerate(self.link.get('inputs', [])):
            key = str(index) if self.link['protocol'].startswith('modbus') else row['channel']
            if key not in values:
                raise ProjectError('Missing input channel: ' + key)
            value = scalar(values[key], row['_type'])
            fields.extend((str(row['_id']), str(int(value)) if type(value) is bool else str(value)))
        # Native side validates the entire batch before applying any part of it.
        self.runtime.command('\t'.join(['IOWRITE', snapshot['build_id'], str(snapshot['io_epoch']), str(len(fields) // 2)] + fields))

    def frame(self, snapshot, session, seq, armed):
        enabled = snapshot['state'] == 'RUN' and armed
        by_id = {tag['id']: tag['value'] for tag in snapshot.get('tags', [])}
        outputs = {}
        for index, row in enumerate(self.link.get('outputs', [])):
            key = str(index) if self.link['protocol'].startswith('modbus') else row['channel']
            outputs[key] = scalar(by_id[row['_id']], row['_type']) if enabled else (False if row['_type'] == 'BOOL' else 0)
        return {'version': 1, 'session': session, 'seq': seq, 'outputs_enabled': enabled,
                'lease_ms': max(3 * self.link.get('cycle_ms', 20), 2 * self.link.get('timeout_ms', 500)), 'outputs': outputs}

    def run(self):
        client, snapshot, armed, epoch = None, None, False, None
        session, seq = secrets.token_hex(16), 0
        timeout = self.link.get('timeout_ms', 500) / 1000
        while not self.stop_event.is_set():
            started = time.monotonic()
            snapshot = None
            try:
                snapshot = self.runtime.command('STATUS')
                if snapshot.get('build_id') != self.link['_build_id']:
                    raise ProjectError('Loaded program changed; apply I/O mappings again')
                if epoch != snapshot['io_epoch']:
                    armed, epoch = False, snapshot['io_epoch']
                if client is None:
                    client = make_client(self.link)
                seq += 1
                frame = self.frame(snapshot, session, seq, armed)
                reply = client.exchange(frame, time.monotonic() + timeout)
                if reply.get('version') != 1 or reply.get('session') != session or type(reply.get('ack')) is not int or reply['ack'] != seq or reply.get('ready') is not True:
                    raise ProjectError('I/O session/acknowledgement mismatch or peer not ready')
                if not isinstance(reply.get('inputs'), dict):
                    raise ProjectError('Missing I/O input map')
                if self.stop_event.is_set():
                    break
                # Even an output-only link checks the CPU generation here.
                self.write_inputs(snapshot, reply['inputs'])
                armed = True
                with self.lock:
                    self.info.update(state='online', seq=seq, ack=seq, latency_ms=round((time.monotonic() - started) * 1000, 2), last_good=time.monotonic(), last_error='')
                    self.info['exchanges'] += 1
            except Exception as error:
                armed = False
                if client:
                    client.close()
                client, session, seq = None, secrets.token_hex(16), 0
                with self.lock:
                    self.info.update(state='error', last_error=str(error))
                    self.info['errors'] += 1
                if snapshot and snapshot.get('build_id') == self.link['_build_id'] and not self.stop_event.is_set():
                    try:
                        policy = self.link.get('fail_policy', 'fault')
                        if policy == 'fault' and snapshot['state'] == 'RUN':
                            self.runtime.command(f"IOFAULT\t{snapshot['build_id']}\t{snapshot['io_epoch']}\t{self.link['name']}")
                        elif policy == 'zero':
                            zeros = {(str(i) if self.link['protocol'].startswith('modbus') else r['channel']): (False if r['_type'] == 'BOOL' else 0) for i, r in enumerate(self.link.get('inputs', []))}
                            self.write_inputs(snapshot, zeros)
                    except ProjectError:
                        pass  # CPU changed generation or exited during this exchange.
            delay = max(0, self.link.get('cycle_ms', 20) / 1000 - (time.monotonic() - started))
            if client is None:
                delay = max(delay, 0.2)
            self.stop_event.wait(delay)
        # Best effort on orderly disconnect. A disconnected physical device needs
        # its own watchdog; the host cannot guarantee delivery to a lost peer.
        if client and snapshot:
            try:
                client.exchange(self.frame(snapshot, session, seq + 1, False), time.monotonic() + timeout)
            except Exception:
                pass
        if client:
            client.close()
        with self.lock:
            self.info['state'] = 'disconnected'


class IOManager:
    def __init__(self, runtime):
        self.runtime, self.workers = runtime, []

    def apply(self, links, snapshot):
        if snapshot.get('state') != 'STOP':
            raise ProjectError('Load a program and STOP before applying I/O connections')
        bound = bind_links(links, snapshot)
        self.close()
        from plc_io_server import ServerWorker
        self.workers = [(ServerWorker if link.get('role', 'client') == 'server' else LinkWorker)(link, self.runtime) for link in bound]
        for worker in self.workers:
            worker.thread.start()

    def close(self):
        for worker in self.workers:
            worker.stop_event.set()
        for worker in self.workers:
            worker.thread.join(timeout=14)
            if worker.thread.is_alive():
                raise ProjectError('I/O worker is still stopping; retry before reconnecting')
        self.workers = []

    def status(self):
        return [worker.status() for worker in self.workers]
