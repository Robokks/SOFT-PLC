"""Multi-peer PLC I/O servers; mapping direction is always relative to the PLC."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import copy
import secrets
import socket
import socketserver
import struct
import threading
import time
from plc_io import (MAX_FRAME, RPC_PATH, FORMATS, ProjectError, crc16, decode_frame,
                    encode_frame, recv_exact, register_bytes, scalar)


class ModbusError(Exception):
    def __init__(self, code):
        self.code = code


class ServerWorker:
    def __init__(self, link, runtime):
        self.link, self.runtime = link, runtime
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, name='io-server-' + link['name'], daemon=True)
        self.peers, self.owners, self.retired = {}, {}, []
        self.sockets = set()
        self.server = None
        self.info = {'name': link['name'], 'protocol': link['protocol'], 'role': 'server', 'state': 'listening',
                     'clients': 0, 'exchanges': 0, 'errors': 0, 'last_error': '', 'seq': 0, 'ack': 0,
                     'latency_ms': 0, 'last_good': None}
        self.input_by_id = {row['_id']: row for row in link.get('inputs', [])}
        self.channel_rows = {row.get('channel'): row for row in link.get('inputs', [])}
        self.regions = {}
        for direction in ('inputs', 'outputs'):
            for row in link.get(direction, []):
                area = row.get('area', 'holding')
                width = 1 if area in ('coil', 'discrete') else struct.calcsize('>' + FORMATS[row.get('datatype', 'uint16')]) // 2
                for offset in range(width):
                    self.regions[(area, row.get('address', 0) + offset)] = (direction, row, width)

    def status(self):
        with self.lock:
            result = dict(self.info)
            result['clients'] = len(self.peers)
        good = result.pop('last_good')
        result['age_ms'] = round((time.monotonic() - good) * 1000) if good is not None else None
        return result

    def snapshot(self):
        if self.stop_event.is_set():
            raise ProjectError('I/O server is stopping')
        result = self.runtime.command('STATUS')
        if result.get('build_id') != self.link['_build_id']:
            raise ProjectError('Loaded program changed; apply I/O again')
        return result

    def peer(self, key):
        self.expire()
        if key not in self.peers:
            if len(self.peers) >= self.link.get('max_clients', 16):
                raise ProjectError('Server client limit reached')
            self.peers[key] = {'deadline': time.monotonic() + self.link.get('lease_ms', 1000) / 1000,
                               'seq': 0, 'request': None, 'reply': None, 'stage': {}, 'ack': 0, 'latch': None, 'epoch': None}
        return self.peers[key]

    def commit(self, key, updates, snapshot):
        checked = []
        for ident, value in updates.items():
            row = self.input_by_id[ident]
            checked.append((ident, scalar(value, row['_type'])))
            if ident in self.owners and self.owners[ident] != key:
                raise ProjectError('Input is owned by another client: ' + row['tag'])
        parts = ['IOWRITE', snapshot['build_id'], str(snapshot['io_epoch']), str(len(checked))]
        for ident, value in checked:
            parts.extend((str(ident), str(int(value)) if type(value) is bool else str(value)))
        self.runtime.command('\t'.join(parts))
        for ident, _ in checked:
            self.owners[ident] = key

    def plc_outputs(self, snapshot):
        by_id = {tag['id']: tag['value'] for tag in snapshot.get('tags', [])}
        return {row['_id']: scalar(by_id[row['_id']], row['_type']) if snapshot['state'] == 'RUN'
                else False if row['_type'] == 'BOOL' else 0 for row in self.link.get('outputs', [])}

    def good(self, seq, started):
        self.info.update(state='online', seq=seq, ack=seq, last_good=time.monotonic(), last_error='',
                         latency_ms=round((time.monotonic() - started) * 1000, 2))
        self.info['exchanges'] += 1

    def error(self, error):
        with self.lock:
            self.info.update(state='error', last_error=str(error))
            self.info['errors'] += 1

    def expire(self):
        now = time.monotonic()
        for key, peer in list(self.peers.items()):
            if peer['deadline'] > now:
                continue
            owned = [ident for ident, owner in self.owners.items() if owner == key]
            try:
                if owned and not self.stop_event.is_set():
                    snapshot = self.snapshot()
                    policy = self.link.get('fail_policy', 'fault')
                    if policy == 'fault' and snapshot['state'] == 'RUN':
                        self.runtime.command(f"IOFAULT\t{snapshot['build_id']}\t{snapshot['io_epoch']}\t{self.link['name']}")
                    elif policy == 'zero':
                        self.commit(key, {i: False if self.input_by_id[i]['_type'] == 'BOOL' else 0 for i in owned}, snapshot)
                    self.error('Client input lease expired')
            except ProjectError as error:
                self.error(error)
            for ident in owned:
                self.owners.pop(ident, None)
            del self.peers[key]
            if key.startswith('json:'):
                self.retired = (self.retired + [key])[-128:]

    def exchange(self, frame):
        started = time.monotonic()
        with self.lock:
            session, seq = frame.get('session'), frame.get('seq')
            if frame.get('version') != 1 or not isinstance(session, str) or len(session) != 32 or type(seq) is not int or seq < 1:
                raise ProjectError('Invalid I/O version/session/sequence')
            if type(frame.get('outputs_enabled')) is not bool or type(frame.get('lease_ms')) is not int or not 1 <= frame['lease_ms'] <= 180000:
                raise ProjectError('Invalid output enable or lease')
            outputs = frame.get('outputs')
            if not isinstance(outputs, dict) or len(outputs) > 128:
                raise ProjectError('Invalid channel map')
            self.expire()
            key = 'json:' + session
            if key in self.retired:
                raise ProjectError('Expired session; start a new session with outputs disabled')
            if key not in self.peers and (seq != 1 or frame['outputs_enabled']):
                raise ProjectError('New sessions require sequence 1 with outputs disabled')
            peer = self.peer(key)
            snapshot = self.snapshot()
            if seq == peer['seq']:
                if frame != peer['request']:
                    raise ProjectError('Changed duplicate sequence')
                # Never replay a RUN output snapshot after STOP/RESET.
                if peer['epoch'] != snapshot['io_epoch']:
                    raise ProjectError('CPU generation changed; reconnect')
                return copy.deepcopy(peer['reply'])
            if seq != peer['seq'] + 1:
                raise ProjectError('Out-of-order sequence')
            updates = {}
            for channel, value in outputs.items():
                if channel not in self.channel_rows:
                    raise ProjectError('Unknown writable channel: ' + str(channel))
                row = self.channel_rows[channel]
                value = scalar(value, row['_type'])
                updates[row['_id']] = value if frame['outputs_enabled'] else False if row['_type'] == 'BOOL' else 0
            self.commit(key, updates, snapshot)
            snapshot = self.snapshot()
            output_values = self.plc_outputs(snapshot)
            reply = {'version': 1, 'session': session, 'ack': seq, 'ready': snapshot['state'] != 'FAULT',
                     'outputs_enabled': snapshot['state'] == 'RUN',
                     'inputs': {row['channel']: output_values[row['_id']] for row in self.link.get('outputs', [])}}
            peer.update(seq=seq, request=copy.deepcopy(frame), reply=copy.deepcopy(reply), epoch=snapshot['io_epoch'],
                        deadline=time.monotonic() + min(frame['lease_ms'], self.link.get('lease_ms', 1000)) / 1000)
            self.good(seq, started)
            return reply

    def modbus(self, key, pdu):
        started = time.monotonic()
        function = pdu[0] if pdu else 0
        with self.lock:
            try:
                peer = self.peer(key)
                snapshot = self.snapshot()
                if peer['epoch'] != snapshot['io_epoch']:
                    peer.update(stage={}, latch=None, epoch=snapshot['io_epoch'])
                handshake = self.link.get('handshake', {})
                if function in (1, 2, 3, 4):
                    if len(pdu) != 5:
                        raise ModbusError(3)
                    address, count = struct.unpack('>HH', pdu[1:])
                    if not 1 <= count <= (2000 if function <= 2 else 125) or address + count > 65536:
                        raise ModbusError(3)
                    area = {1: 'coil', 2: 'discrete', 3: 'holding', 4: 'input'}[function]
                    output_values = peer['latch'] if handshake and peer['latch'] is not None else self.plc_outputs(snapshot)
                    values = {t['id']: t['value'] for t in snapshot['tags']}
                    data = []
                    for location in range(address, address + count):
                        if area == 'holding' and location in handshake.values():
                            value = peer['ack'] if location == handshake.get('ack') else int(snapshot['state'] != 'FAULT') if location == handshake.get('ready') else peer['ack']
                        else:
                            region = self.regions.get((area, location))
                            if region is None:
                                raise ModbusError(2)
                            direction, row, width = region
                            value = output_values[row['_id']] if direction == 'outputs' else peer['stage'].get(row['_id'], values[row['_id']])
                            if function > 2:
                                raw = register_bytes(row, value=value)
                                offset = (location - row.get('address', 0)) * 2
                                value = struct.unpack('>H', raw[offset:offset + 2])[0]
                        data.append(value)
                    if function <= 2:
                        raw = bytearray((count + 7) // 8)
                        for i, value in enumerate(data):
                            raw[i // 8] |= int(bool(value)) << (i % 8)
                        raw = bytes(raw)
                    else:
                        raw = b''.join(struct.pack('>H', n) for n in data)
                    reply = bytes([function, len(raw)]) + raw
                elif function in (5, 6, 15, 16):
                    if len(pdu) < 5:
                        raise ModbusError(3)
                    address, value = struct.unpack('>HH', pdu[1:5])
                    area = 'coil' if function in (5, 15) else 'holding'
                    if function in (5, 6):
                        if len(pdu) != 5 or (function == 5 and value not in (0, 0xFF00)):
                            raise ModbusError(3)
                        data = [bool(value)] if function == 5 else [value]
                    else:
                        count = value
                        expected = (count + 7) // 8 if function == 15 else 2 * count
                        if not 1 <= count <= (1968 if function == 15 else 123) or len(pdu) != expected + 6 or pdu[5] != expected:
                            raise ModbusError(3)
                        data = [bool(pdu[6 + i // 8] & (1 << (i % 8))) for i in range(count)] if function == 15 else list(struct.unpack('>' + 'H' * count, pdu[6:]))
                    if address + len(data) > 65536:
                        raise ModbusError(2)
                    if handshake and area == 'holding' and address == handshake['request'] and len(data) == 1:
                        if not data[0]:
                            raise ModbusError(3)
                        if data[0] != peer['ack']:
                            self.commit(key, peer['stage'], snapshot)
                            peer['ack'], peer['stage'] = data[0], {}
                            peer['latch'] = self.plc_outputs(self.snapshot())
                    else:
                        updates, offset = {}, 0
                        while offset < len(data):
                            location = address + offset
                            region = self.regions.get((area, location))
                            if region is None or region[0] != 'inputs':
                                raise ModbusError(2)
                            _, row, width = region
                            if row.get('address', 0) != location or offset + width > len(data):
                                raise ModbusError(3)  # Do not tear a multiregister PLC tag.
                            raw = b''.join(struct.pack('>H', n) for n in data[offset:offset + width])
                            value = data[offset] if area == 'coil' else register_bytes(row, raw=raw)
                            updates[row['_id']] = scalar(value, row['_type'])
                            if row['_id'] in self.owners and self.owners[row['_id']] != key:
                                raise ModbusError(6)
                            offset += width
                        if handshake:
                            peer['stage'].update(updates)
                        else:
                            self.commit(key, updates, snapshot)
                    reply = pdu[:5]
                else:
                    raise ModbusError(1)
                peer['deadline'] = time.monotonic() + self.link.get('lease_ms', 1000) / 1000
                self.good(peer['ack'], started)
                return reply
            except ModbusError as error:
                self.error('Modbus request exception ' + str(error.code))
                return bytes([function | 128, error.code])
            except (ProjectError, ValueError, OverflowError, struct.error) as error:
                self.error(error)
                return bytes([function | 128, 6 if 'owned' in str(error) or 'limit' in str(error) else 3])

    def network_server(self):
        worker = self
        protocol = self.link['protocol']
        class TCP(socketserver.BaseRequestHandler):
            def handle(self):
                key = 'tcp:' + secrets.token_hex(16)
                with worker.lock:
                    if len(worker.sockets) >= worker.link.get('max_clients', 16):
                        return
                    worker.sockets.add(self.request)
                try:
                    while not worker.stop_event.is_set():
                        deadline = time.monotonic() + max(1, worker.link.get('lease_ms', 1000) / 1000)
                        if protocol == 'modbus_tcp':
                            tx, pid, size, unit = struct.unpack('>HHHB', recv_exact(self.request, 7, deadline))
                            if pid or not 2 <= size <= 254:
                                raise ProjectError('Invalid Modbus TCP header')
                            pdu = recv_exact(self.request, size - 1, deadline)
                            if unit != worker.link.get('unit', 1):
                                response = bytes([pdu[0] | 128, 11])
                            else:
                                response = worker.modbus(key, pdu)
                            self.request.sendall(struct.pack('>HHHB', tx, 0, len(response) + 1, unit) + response)
                        else:
                            size = struct.unpack('>I', recv_exact(self.request, 4, deadline))[0]
                            if not 1 <= size <= MAX_FRAME:
                                raise ProjectError('Invalid TCP frame size')
                            response = encode_frame(worker.exchange(decode_frame(recv_exact(self.request, size, deadline))))
                            self.request.sendall(struct.pack('>I', len(response)) + response)
                except (OSError, ValueError) as error:
                    if not worker.stop_event.is_set():
                        worker.error(error)
                finally:
                    with worker.lock:
                        worker.sockets.discard(self.request)
                        if key in worker.peers:
                            worker.peers[key]['deadline'] = 0
                            worker.expire()
        class UDP(socketserver.BaseRequestHandler):
            def handle(self):
                try:
                    data, sock = self.request
                    if len(data) > 1200:
                        raise ProjectError('Oversized UDP frame')
                    response = encode_frame(worker.exchange(decode_frame(data)))
                    if len(response) > 1200:
                        raise ProjectError('UDP reply exceeds 1200 bytes')
                    sock.sendto(response, self.client_address)
                except (OSError, ValueError) as error:
                    worker.error(error)
        base = socketserver.UDPServer if protocol == 'udp' else socketserver.ThreadingTCPServer
        class Listener(base):
            daemon_threads = True
            allow_reuse_address = True
            block_on_close = False
            max_packet_size = 1201
            address_family = socket.AF_INET6 if ':' in worker.link.get('host', '127.0.0.1') else socket.AF_INET
        return Listener((self.link.get('host', '127.0.0.1'), self.link.get('port', 502 if protocol == 'modbus_tcp' else 50051)), UDP if protocol == 'udp' else TCP)

    def grpc_server(self):
        import grpc
        from google.protobuf.wrappers_pb2 import BytesValue
        def exchange(message, context):
            try:
                return BytesValue(value=encode_frame(self.exchange(decode_frame(message.value))))
            except (ValueError, OSError) as error:
                self.error(error)
                context.abort(grpc.StatusCode.FAILED_PRECONDITION, str(error))
        server = grpc.server(ThreadPoolExecutor(max_workers=4), maximum_concurrent_rpcs=self.link.get('max_clients', 16),
                             options=[('grpc.max_receive_message_length', MAX_FRAME + 32), ('grpc.max_send_message_length', MAX_FRAME + 32)])
        server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler(RPC_PATH.rsplit('/', 1)[0][1:], {
            'Exchange': grpc.unary_unary_rpc_method_handler(exchange, request_deserializer=BytesValue.FromString, response_serializer=BytesValue.SerializeToString)}),))
        host = self.link.get('host', '127.0.0.1')
        address = (f'[{host}]' if ':' in host else host) + ':' + str(self.link.get('port', 50051))
        if self.link.get('tls'):
            creds = grpc.ssl_server_credentials([(Path(self.link['key_file']).read_bytes(), Path(self.link['cert_file']).read_bytes())])
            port = server.add_secure_port(address, creds)
        else:
            port = server.add_insecure_port(address)
        if not port:
            raise ProjectError('Cannot bind gRPC listener')
        server.start()
        return server

    def serial_server(self):
        import serial
        link = self.link
        port = serial.Serial(link.get('serial_port', 'COM1'), baudrate=link.get('baudrate', 19200), bytesize=8,
                             parity=link.get('parity', 'E'), stopbits=link.get('stopbits', 1), timeout=.05,
                             write_timeout=link.get('timeout_ms', 500) / 1000)
        data = bytearray()
        last_byte = time.monotonic()
        try:
            while not self.stop_event.is_set():
                chunk = port.read(max(1, min(port.in_waiting, 256)))
                if chunk:
                    data.extend(chunk);last_byte = time.monotonic()
                elif data and time.monotonic() - last_byte > link.get('timeout_ms', 500) / 1000:
                    self.error('Incomplete RTU request');data.clear()
                if len(data) > 256:
                    self.error('Oversized RTU frame');data.clear()
                while len(data) >= 2:
                    function = data[1]
                    if function in (1, 2, 3, 4, 5, 6):
                        size = 8
                    elif function in (15, 16):
                        if len(data) < 7:
                            break
                        size = 9 + data[6]
                    else:
                        # Unknown function lengths are delimited by a quiet interval.
                        if not chunk and len(data) >= 4:
                            packet = bytes(data);data.clear()
                            if packet[0] == link.get('unit', 1) and crc16(packet[:-2]) == struct.unpack('<H', packet[-2:])[0]:
                                response = bytes([packet[0], function | 128, 1])
                                port.write(response + struct.pack('<H', crc16(response)))
                            self.error('Unsupported RTU function')
                        break
                    if size > 256:
                        data.clear();break
                    if len(data) < size:
                        break
                    packet = bytes(data[:size]);del data[:size]
                    if crc16(packet[:-2]) != struct.unpack('<H', packet[-2:])[0]:
                        self.error('Modbus RTU CRC mismatch');data.clear();break
                    if packet[0] != link.get('unit', 1):
                        continue
                    response = bytes([packet[0]]) + self.modbus('serial', packet[1:-2])
                    baud = link.get('baudrate', 19200)
                    bits = 9 + (link.get('parity', 'E') != 'N') + link.get('stopbits', 1)
                    self.stop_event.wait(.00175 if baud > 19200 else 3.5 * bits / baud)
                    port.write(response + struct.pack('<H', crc16(response)))
                with self.lock:
                    self.expire()
        finally:
            port.close()

    def run(self):
        protocol = self.link['protocol']
        listener_thread = None
        try:
            if protocol == 'modbus_rtu':
                self.serial_server()
                return
            self.server = self.grpc_server() if protocol == 'grpc' else self.network_server()
            if protocol != 'grpc':
                listener_thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .02}, daemon=True)
                listener_thread.start()
            while not self.stop_event.wait(.02):
                with self.lock:
                    self.expire()
        except Exception as error:
            self.error(error)
        finally:
            self.stop_event.set()
            if self.server:
                if protocol == 'grpc':
                    self.server.stop(0).wait(timeout=2)
                else:
                    self.server.shutdown()
                    with self.lock:
                        for sock in list(self.sockets):
                            try:
                                sock.shutdown(socket.SHUT_RDWR)
                            except OSError:
                                pass
                    self.server.server_close()
                    if listener_thread:
                        listener_thread.join(timeout=2)
            with self.lock:
                self.peers.clear();self.owners.clear()
                if self.info['state'] != 'error':
                    self.info['state'] = 'disconnected'
