"""Wire-level I/O tests plus real native-runtime integration (no physical hardware)."""
import copy
import json
import os
from pathlib import Path
import select
import socket
import socketserver
import struct
import sys
import threading
import time
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from plc_io import (IOManager, JsonClient, ModbusClient, ProjectError, bind_links, crc16,
                    decode_frame, encode_frame, recv_exact, register_bytes, validate_links)
from io_peer import HandshakePeer, Server
from plc_build import compile_project
from plc_project import prepare
from plc_studio import RuntimeProcess, Studio

PROJECT = {'schema': 1, 'name': 'I/O test', 'data_blocks': [{'number': 1, 'fields': [
    {'name': 'Start', 'type': 'BOOL', 'offset': 0, 'initial': False},
    {'name': 'Setpoint', 'type': 'DINT', 'offset': 4, 'initial': 0},
    {'name': 'Temperature', 'type': 'REAL', 'offset': 8, 'initial': 0.0}]}],
    'globals': [{'name': 'MotorOutput', 'type': 'BOOL', 'address': '%Q0.0'}],
    'instances': [], 'blocks': [], 'tasks': [{'ob': 1, 'kind': 'main', 'period_ms': 10,
        'networks': [{'language': 'SCL', 'source': 'MotorOutput := DB1.Start;'}]}]}


def until(predicate, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.01)
    raise AssertionError('I/O condition did not become true')


def values(snapshot):
    return {tag['name']: tag['value'] for tag in snapshot['tags']}


def config(protocol='tcp', port=15000):
    link = {'name': 'Device1', 'enabled': True, 'protocol': protocol, 'host': '127.0.0.1', 'port': port,
            'cycle_ms': 20, 'timeout_ms': 500, 'fail_policy': 'fault',
            'inputs': [{'tag': 'DB1.Start', 'channel': 'start'}, {'tag': 'DB1.Setpoint', 'channel': 'setpoint'},
                       {'tag': 'DB1.Temperature', 'channel': 'temperature'}],
            'outputs': [{'tag': 'MotorOutput', 'channel': 'motor'}]}
    if protocol.startswith('modbus'):
        link.update(unit=1, baudrate=19200, parity='N', stopbits=2, serial_port='COM3',
                    handshake={'request': 100, 'ack': 101, 'ready': 102}, cycle_ms=100, timeout_ms=1500)
        link['inputs'] = [{'tag': 'DB1.Start', 'area': 'discrete', 'address': 0, 'datatype': 'bool'},
                          {'tag': 'DB1.Setpoint', 'area': 'holding', 'address': 10, 'datatype': 'int32'},
                          {'tag': 'DB1.Temperature', 'area': 'input', 'address': 20, 'datatype': 'float32', 'word_order': 'low_high'}]
        link['outputs'] = [{'tag': 'MotorOutput', 'area': 'coil', 'address': 0, 'datatype': 'bool'}]
    return link


class Device:
    """Independent Modbus register fixture, including application request/ack."""
    def __init__(self):
        self.coils = {0: 0}
        self.holding = {10: 0xFFFF, 11: 0xCFC7, 101: 65535, 102: 1}  # -12345
        self.inputs = {20: 0, 21: 0x41AC}  # 21.5, low word first
        self.functions = set()
        self.bad_crc = self.bad_transaction = self.exception = self.no_ack = False

    def pdu(self, request):
        function, address = struct.unpack('>BH', request[:3])
        self.functions.add(function)
        if self.exception:
            return bytes([function | 128, 2])
        if function in (1, 2, 3, 4):
            count = struct.unpack('>H', request[3:5])[0]
            if function <= 2:
                data = bytes([(self.coils.get(address, 0) if function == 1 else 1)])
            else:
                store = self.holding if function == 3 else self.inputs
                data = b''.join(struct.pack('>H', store.get(address + i, 0)) for i in range(count))
            return bytes([function, len(data)]) + data
        if function == 5:
            self.coils[address] = int(request[3:5] == b'\xff\x00')
            return request
        if function == 6:
            self.holding[address] = struct.unpack('>H', request[3:5])[0]
            if address == 100 and not self.no_ack:
                self.holding[101] = self.holding[100]
            return request
        if function == 16:
            count = struct.unpack('>H', request[3:5])[0]
            assert request[5] == count * 2
            for i in range(count):
                self.holding[address + i] = struct.unpack('>H', request[6 + i * 2:8 + i * 2])[0]
            return request[:5]
        raise AssertionError('Unexpected Modbus function')


class ModbusFixture:
    def __init__(self, device, rtu=False, pty=False):
        self.device, self.rtu, self.pty = device, rtu, pty
        self.done = threading.Event()
        if pty:
            import tty
            self.master, self.slave = os.openpty()
            tty.setraw(self.master)
            tty.setraw(self.slave)
            self.serial_port = os.ttyname(self.slave)
            self.thread = threading.Thread(target=self.serve_pty, daemon=True)
        else:
            fixture = self
            class Handler(socketserver.BaseRequestHandler):
                def handle(self):
                    try:
                        while not fixture.done.is_set():
                            deadline = time.monotonic() + 4
                            read = lambda n: recv_exact(self.request, n, deadline)
                            data = fixture.response(read)
                            # Fragment every response to exercise accumulation/framing.
                            self.request.sendall(data[:2])
                            self.request.sendall(data[2:])
                    except (OSError, AssertionError):
                        pass
            class Threaded(socketserver.ThreadingTCPServer):
                daemon_threads = True
                allow_reuse_address = True
            self.server = Threaded(('127.0.0.1', 0), Handler)
            self.port = self.server.server_address[1]
            self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .02}, daemon=True)
        self.thread.start()

    def response(self, read):
        if self.rtu:
            header = read(2)
            pdu = header[1:] + read(4)
            if pdu[0] == 16:
                size = read(1)
                pdu += size + read(size[0])
            check = read(2)
            assert check == struct.pack('<H', crc16(header[:1] + pdu))
            body = header[:1] + self.device.pdu(pdu)
            return body + struct.pack('<H', crc16(body) ^ (1 if self.device.bad_crc else 0))
        tx, protocol, size, unit = struct.unpack('>HHHB', read(7))
        assert protocol == 0 and unit == 1
        body = self.device.pdu(read(size - 1))
        return struct.pack('>HHHB', (tx + self.device.bad_transaction) & 65535, 0, len(body) + 1, unit) + body

    def serve_pty(self):
        def read(size):
            data = bytearray()
            end = time.monotonic() + 3
            while len(data) < size and not self.done.is_set():
                if time.monotonic() > end:
                    raise TimeoutError()
                if select.select([self.master], [], [], .05)[0]:
                    data.extend(os.read(self.master, size - len(data)))
            if len(data) != size:
                raise OSError('closed')
            return bytes(data)
        while not self.done.is_set():
            try:
                os.write(self.master, self.response(read))
            except TimeoutError:
                continue
            except OSError:
                break

    def close(self):
        self.done.set()
        if self.pty:
            self.thread.join(timeout=4)
            os.close(self.master)
            os.close(self.slave)
        else:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=2)


class ConfigTests(unittest.TestCase):
    def test_io_settings_do_not_change_program_identity(self):
        p = copy.deepcopy(PROJECT)
        before = prepare(p).build_id
        p['io_links'] = [config()]
        self.assertEqual(before, prepare(p).build_id)
        self.assertEqual(Studio.canonical(PROJECT), Studio.canonical(p))

    def test_overlapping_modbus_and_invalid_serial_config_rejected(self):
        link = config('modbus_rtu')
        link['outputs'].append({'tag': 'DB1.Setpoint', 'area': 'holding', 'address': 99, 'datatype': 'int32'})
        with self.assertRaisesRegex(ProjectError, 'overlap'):
            validate_links([link])
        a, b = config('modbus_rtu'), config('modbus_rtu')
        b['name'] = 'Second'
        with self.assertRaisesRegex(ProjectError, 'serial port'):
            validate_links([a, b])
        a['serial_port'] = 'socket://127.0.0.1:1'
        with self.assertRaisesRegex(ProjectError, 'physical'):
            validate_links([a])

    def test_register_order_and_known_crc(self):
        self.assertEqual(crc16(bytes.fromhex('01030000000a')), 0xCDC5)
        row = {'datatype': 'float32', 'word_order': 'low_high'}
        self.assertEqual(register_bytes(row, value=21.5), bytes.fromhex('000041ac'))
        self.assertEqual(register_bytes(row, raw=bytes.fromhex('000041ac')), 21.5)

    def test_peer_duplicate_reorder_and_output_lease(self):
        peer = HandshakePeer({'start': True})
        self.addCleanup(peer.close)
        frame = {'version': 1, 'session': 'a' * 32, 'seq': 1, 'outputs_enabled': False, 'lease_ms': 50, 'outputs': {'motor': True}}
        reply = peer.exchange(frame)
        self.assertFalse(peer.outputs['motor'])
        self.assertEqual(reply, peer.exchange(frame))
        frame['seq'] = 3
        with self.assertRaisesRegex(ValueError, 'order'):
            peer.exchange(frame)
        frame.update(seq=2, outputs_enabled=True)
        peer.exchange(frame)
        self.assertTrue(peer.outputs['motor'])
        until(lambda: not peer.outputs['motor'])
        with self.assertRaisesRegex(ValueError, 'Expired'):
            peer.exchange({**frame, 'seq': 3})

    def test_udp_retries_same_sequence_and_discards_stale_ack(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', 0));sock.settimeout(2)
        self.addCleanup(sock.close)
        seen = []
        def serve():
            first, addr = sock.recvfrom(1200);seen.append(decode_frame(first))
            second, addr = sock.recvfrom(1200);seen.append(decode_frame(second))
            reply = {'version': 1, 'session': seen[1]['session'], 'ack': 0, 'ready': True, 'inputs': {}}
            sock.sendto(encode_frame(reply), addr)
            reply['ack'] = 1
            sock.sendto(encode_frame(reply), addr)
        thread = threading.Thread(target=serve);thread.start()
        client = JsonClient(config('udp', sock.getsockname()[1]));self.addCleanup(client.close)
        frame = {'session': 'b' * 32, 'seq': 1}
        self.assertEqual(client.exchange(frame, time.monotonic() + .6)['ack'], 1)
        thread.join(timeout=2)
        self.assertEqual(seen, [frame, frame])


class RuntimeIOTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiled = compile_project(PROJECT)

    def setUp(self):
        self.runtime = RuntimeProcess();self.runtime.ensure(self.compiled['runtime'])
        self.addCleanup(self.runtime.close)
        self.snapshot = self.runtime.command('LOAD\t' + self.compiled['module'])
        self.io = IOManager(self.runtime);self.addCleanup(self.io.close)

    def start_link(self, link):
        self.io.apply([link], self.runtime.command('STATUS'))
        until(lambda: self.io.status()[0]['exchanges'] > 0, timeout=6)
        self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Start'])
        self.runtime.command('RUN')
        until(lambda: values(self.runtime.command('STATUS'))['MotorOutput'])

    def test_tcp_udp_grpc_exchange_native_tags_and_stop_outputs(self):
        for protocol in ('tcp', 'udp', 'grpc'):
            with self.subTest(protocol=protocol):
                peer = HandshakePeer({'start': True, 'setpoint': -12345, 'temperature': 21.5})
                with Server(protocol, peer) as server:
                    try:
                        self.start_link(config(protocol, server.port))
                        until(lambda: peer.outputs.get('motor') is True)
                        v = values(self.runtime.command('STATUS'))
                        self.assertEqual(v['DB1.Setpoint'], -12345)
                        self.assertEqual(v['DB1.Temperature'], 21.5)
                        self.runtime.command('STOP')
                        until(lambda: peer.outputs.get('motor') is False)
                        self.io.close()
                    finally:
                        peer.close()

    def exercise_modbus(self, rtu=False, pty=False):
        import serial
        device = Device();fixture = ModbusFixture(device, rtu=rtu, pty=pty)
        self.addCleanup(fixture.close)
        link = config('modbus_rtu' if rtu else 'modbus_tcp', getattr(fixture, 'port', 0))
        if pty:
            link['serial_port'] = fixture.serial_port
        real_serial = serial.Serial
        def serial_port(port, **kwargs):
            if pty:
                return real_serial(port, **kwargs)
            return serial.serial_for_url(f'socket://127.0.0.1:{fixture.port}', **kwargs)
        # On Windows CI, actual pySerial reads/writes run through its socket
        # driver; Linux additionally exercises a real OS pseudo-terminal.
        with patch('serial.Serial', serial_port):
            try:
                self.start_link(link)
                until(lambda: device.coils[0] == 1, timeout=6)
                self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Setpoint'], -12345)
                self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Temperature'], 21.5)
                self.assertNotEqual(device.holding[101], 65535)
                self.runtime.command('STOP')
                until(lambda: device.coils[0] == 0, timeout=6)
            finally:
                self.io.close()
        self.assertTrue({2, 3, 4, 5, 6}.issubset(device.functions))

    def test_modbus_tcp_with_register_handshake(self):
        self.exercise_modbus()

    def test_modbus_rtu_with_real_pyserial_and_crc(self):
        self.exercise_modbus(rtu=True)

    @unittest.skipIf(os.name == 'nt', 'PTY requires POSIX; Windows uses pySerial socket driver')
    def test_modbus_rtu_os_serial_port(self):
        self.exercise_modbus(rtu=True, pty=True)

    def test_atomic_batch_rejects_invalid_value_and_stale_generation(self):
        s = self.snapshot
        ids = {t['name']: t['id'] for t in s['tags']}
        def command(epoch, tail):
            return f"IOWRITE\t{s['build_id']}\t{epoch}\t{tail}"
        with self.assertRaisesRegex(ProjectError, 'range'):
            self.runtime.command(command(s['io_epoch'], f"2\t{ids['DB1.Start']}\t1\t{ids['DB1.Setpoint']}\t99999999999"))
        self.assertFalse(values(self.runtime.command('STATUS'))['DB1.Start'])
        self.runtime.command('RESET')
        with self.assertRaisesRegex(ProjectError, 'stale'):
            self.runtime.command(command(s['io_epoch'], '0'))
        s = self.runtime.command('STATUS')
        with self.assertRaisesRegex(ProjectError, 'not writable'):
            self.runtime.command(command(s['io_epoch'], f"1\t{ids['MotorOutput']}\t1"))
        a, b = config(), config();b['name'] = 'Another'
        with self.assertRaisesRegex(ProjectError, 'Multiple'):
            bind_links([a, b], s)

    def test_bad_ack_faults_cpu_without_blocking_scans(self):
        class BadPeer(HandshakePeer):
            fail = False
            def exchange(self, frame):
                if self.fail:
                    time.sleep(.12)
                    return {'version': 1, 'session': frame['session'], 'ack': frame['seq'] - 1, 'ready': True, 'inputs': {}}
                return super().exchange(frame)
        peer = BadPeer({'start': True, 'setpoint': 5, 'temperature': 1.0})
        self.addCleanup(peer.close)
        with Server('tcp', peer) as server:
            self.start_link(config('tcp', server.port))
            until(lambda: peer.outputs.get('motor'))
            before = self.runtime.command('STATUS')['tasks'][0]['cycles']
            peer.fail = True
            until(lambda: self.runtime.command('STATUS')['state'] == 'FAULT')
            final = self.runtime.command('STATUS')
            self.assertGreater(final['tasks'][0]['cycles'], before + 2)
            self.assertFalse(values(final)['MotorOutput'])
            self.assertIn('I/O connection', final['fault'])
            self.assertGreater(self.io.status()[0]['errors'], 0)
            self.io.close()

    def test_invalid_input_is_atomic_with_zero_and_hold_policies(self):
        for policy in ('zero', 'hold'):
            with self.subTest(policy=policy):
                self.runtime.command('RESET')
                peer = HandshakePeer({'start': True, 'setpoint': 10, 'temperature': 1.0})
                with Server('tcp', peer) as server:
                    try:
                        link = config('tcp', server.port);link['fail_policy'] = policy
                        self.start_link(link)
                        with peer.lock:
                            peer.inputs = {'start': False, 'setpoint': 999999999999, 'temperature': 1.0}
                        until(lambda: self.io.status()[0]['errors'] > 0)
                        final = self.runtime.command('STATUS')
                        self.assertEqual(final['state'], 'RUN')
                        self.assertEqual(values(final)['DB1.Setpoint'], 0 if policy == 'zero' else 10)
                        self.assertEqual(values(final)['DB1.Start'], policy == 'hold')
                        self.runtime.command('STOP');self.io.close()
                    finally:
                        peer.close()


class ModbusFailureTests(unittest.TestCase):
    def test_bad_transaction_exception_and_missing_ack(self):
        device = Device();fixture = ModbusFixture(device);self.addCleanup(fixture.close)
        client = ModbusClient(config('modbus_tcp', fixture.port));self.addCleanup(client.close)
        for flag, message in (('bad_transaction', 'transaction'), ('exception', 'exception')):
            setattr(device, flag, True)
            with self.assertRaisesRegex(ProjectError, message):
                client.read('holding', 0, 1, time.monotonic() + .5)
            setattr(device, flag, False);client.close()
        client.write({'area': 'holding', 'address': 30, 'datatype': 'int32'}, -40000, time.monotonic() + .5)
        self.assertEqual(client.read('holding', 30, 2, time.monotonic() + .5), struct.pack('>i', -40000))
        self.assertEqual(client.read('coil', 0, 1, time.monotonic() + .5), b'\x00')
        device.no_ack = True
        with self.assertRaises(TimeoutError):
            client.exchange({'outputs': {'0': False}, 'session': 'x', 'seq': 1}, time.monotonic() + .1)

    def test_corrupt_serial_crc_rejected(self):
        import serial
        device = Device();device.bad_crc = True
        fixture = ModbusFixture(device, rtu=True);self.addCleanup(fixture.close)
        client = ModbusClient(config('modbus_rtu'));self.addCleanup(client.close)
        with patch('serial.Serial', lambda port, **kw: serial.serial_for_url(f'socket://127.0.0.1:{fixture.port}', **kw)):
            with self.assertRaisesRegex(ProjectError, 'CRC'):
                client.read('holding', 0, 1, time.monotonic() + 1)


def unused_port():
    sock = socket.socket();sock.bind(('127.0.0.1', 0));port = sock.getsockname()[1];sock.close()
    return port


class ServerIOTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiled = compile_project(PROJECT)

    def setUp(self):
        self.runtime = RuntimeProcess();self.runtime.ensure(self.compiled['runtime'])
        self.addCleanup(self.runtime.close)
        self.runtime.command('LOAD\t' + self.compiled['module'])
        self.io = IOManager(self.runtime);self.addCleanup(self.io.close)

    def apply(self, links):
        self.io.apply(links, self.runtime.command('STATUS'))
        until(lambda: all(worker.server is not None or worker.link['protocol'] == 'modbus_rtu' for worker in self.io.workers))

    def test_multi_peer_tcp_udp_grpc_servers(self):
        from plc_io import make_client
        for protocol in ('tcp', 'udp', 'grpc'):
            with self.subTest(protocol=protocol):
                link = config(protocol, unused_port());link.update(role='server', lease_ms=3000)
                self.apply([link])
                clients = [make_client(link), make_client(link), make_client(link)]
                try:
                    def send(n, seq, outputs, enabled=False):
                        frame = {'version': 1, 'session': str(n) * 32, 'seq': seq, 'outputs_enabled': enabled, 'lease_ms': 3000, 'outputs': outputs}
                        return clients[n - 1].exchange(frame, time.monotonic() + .7)
                    send(1, 1, {'start': True})
                    send(2, 1, {'setpoint': 90})
                    send(3, 1, {})  # A simultaneous read-only monitor.
                    self.runtime.command('RUN')
                    send(1, 2, {'start': True}, True)
                    send(2, 2, {'setpoint': 90}, True)
                    until(lambda: values(self.runtime.command('STATUS'))['MotorOutput'])
                    reply = send(3, 2, {})
                    self.assertTrue(reply['inputs']['motor'])
                    self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Setpoint'], 90)
                    self.assertEqual(self.io.status()[0]['clients'], 3)
                    # A second writer cannot take a currently leased input.
                    with self.assertRaises(Exception):
                        send(3, 3, {'start': False}, True)
                    self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Start'])
                    self.runtime.command('STOP')
                    self.assertFalse(send(1, 3, {'start': True}, True)['inputs']['motor'])
                finally:
                    for client in clients:
                        client.close()
                    self.io.close()

    def test_servers_and_client_connections_run_simultaneously(self):
        from plc_io import make_client
        peer = HandshakePeer({'setpoint': 17});self.addCleanup(peer.close)
        with Server('tcp', peer) as remote:
            client_link = config('tcp', remote.port);client_link.update(name='Outgoing', inputs=[{'tag': 'DB1.Setpoint', 'channel': 'setpoint'}], outputs=[])
            tcp_link = config('tcp', unused_port());tcp_link.update(name='TCP_listener', role='server', inputs=[{'tag': 'DB1.Start', 'channel': 'start'}])
            grpc_link = config('grpc', unused_port());grpc_link.update(name='GRPC_listener', role='server', inputs=[], outputs=[{'tag': 'DB1.Setpoint', 'channel': 'setpoint'}])
            self.io.apply([client_link, tcp_link, grpc_link], self.runtime.command('STATUS'))
            until(lambda: self.io.status()[0]['exchanges'] > 0)
            external = make_client(tcp_link);monitor = make_client(grpc_link)
            try:
                frame = {'version': 1, 'session': 'a' * 32, 'seq': 1, 'outputs_enabled': False, 'lease_ms': 1000, 'outputs': {'start': False}}
                external.exchange(frame, time.monotonic() + 1)
                self.runtime.command('RUN')
                frame.update(seq=2, outputs_enabled=True, outputs={'start': True})
                external.exchange(frame, time.monotonic() + 1)
                until(lambda: values(self.runtime.command('STATUS'))['MotorOutput'])
                read = {**frame, 'session': 'b' * 32, 'seq': 1, 'outputs_enabled': False, 'outputs': {}}
                self.assertEqual(monitor.exchange(read, time.monotonic() + 1)['inputs']['setpoint'], 17)
                self.assertEqual(len(self.io.status()), 3)
            finally:
                self.runtime.command('STOP');external.close();monitor.close();self.io.close()

    def test_server_lease_loss_zeroes_only_owned_inputs(self):
        from plc_io import make_client
        link = config('tcp', unused_port());link.update(role='server', fail_policy='zero', lease_ms=100)
        self.apply([link]);client = make_client(link)
        self.addCleanup(client.close)
        frame = {'version': 1, 'session': 'c' * 32, 'seq': 1, 'outputs_enabled': False, 'lease_ms': 100, 'outputs': {'start': False}}
        client.exchange(frame, time.monotonic() + 1)
        frame.update(seq=2, outputs_enabled=True, outputs={'start': True})
        client.exchange(frame, time.monotonic() + 1)
        self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Start'])
        until(lambda: not values(self.runtime.command('STATUS'))['DB1.Start'])
        self.assertIn('lease', self.io.status()[0]['last_error'])

    def test_modbus_tcp_server_multi_clients_and_transactional_handshake(self):
        link = config('modbus_tcp', unused_port());link.update(role='server', lease_ms=3000)
        link['inputs'] = [{'tag': 'DB1.Start', 'area': 'coil', 'address': 0, 'datatype': 'bool'},
                          {'tag': 'DB1.Setpoint', 'area': 'holding', 'address': 10, 'datatype': 'int32'}]
        link['outputs'] = [{'tag': 'MotorOutput', 'area': 'discrete', 'address': 0, 'datatype': 'bool'}]
        self.apply([link])
        a = ModbusClient(link);b = ModbusClient(link)
        self.addCleanup(a.close);self.addCleanup(b.close)
        a.write(link['inputs'][0], True, time.monotonic() + 1)
        self.assertFalse(values(self.runtime.command('STATUS'))['DB1.Start'])  # Still staged.
        a.write({'address': 100}, 1, time.monotonic() + 1)
        self.assertEqual(a.read('holding', 101, 1, time.monotonic() + 1), b'\x00\x01')
        self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Start'])
        b.write(link['inputs'][1], -123456, time.monotonic() + 1)
        b.write({'address': 100}, 7, time.monotonic() + 1)
        self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Setpoint'], -123456)
        with self.assertRaisesRegex(ProjectError, 'exception 6'):
            b.write(link['inputs'][0], False, time.monotonic() + 1)
        self.runtime.command('RUN')
        until(lambda: values(self.runtime.command('STATUS'))['MotorOutput'])
        self.assertEqual(a.read('discrete', 0, 1, time.monotonic() + 1), b'\x01')
        # A partial INT32 write fails without changing any input memory.
        with self.assertRaisesRegex(ProjectError, 'exception 3'):
            a.write({'address': 10}, 100, time.monotonic() + 1)
        self.assertEqual(values(self.runtime.command('STATUS'))['DB1.Setpoint'], -123456)
        self.runtime.command('STOP')
        self.assertEqual(a.read('discrete', 0, 1, time.monotonic() + 1), b'\x00')
        self.assertEqual(self.io.status()[0]['clients'], 2)

    @unittest.skipIf(os.name == 'nt', 'Physical serial endpoint emulated by POSIX PTY')
    def test_modbus_rtu_server_os_serial_port(self):
        import tty
        master, slave = os.openpty();tty.setraw(master);tty.setraw(slave)
        link = config('modbus_rtu');link.update(role='server', serial_port=os.ttyname(slave))
        link.pop('handshake')
        link['inputs'] = [{'tag': 'DB1.Start', 'area': 'coil', 'address': 0, 'datatype': 'bool'}]
        link['outputs'] = [{'tag': 'MotorOutput', 'area': 'discrete', 'address': 0, 'datatype': 'bool'}]
        try:
            self.io.apply([link], self.runtime.command('STATUS'))
            time.sleep(.1)
            request = bytes.fromhex('01050000ff00')
            os.write(master, request + struct.pack('<H', crc16(request)))
            data = bytearray();end = time.monotonic() + 2
            while len(data) < 8 and time.monotonic() < end:
                if select.select([master], [], [], .05)[0]:data.extend(os.read(master, 8 - len(data)))
            self.assertEqual(data, request + struct.pack('<H', crc16(request)))
            self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Start'])
        finally:
            self.io.close();os.close(master);os.close(slave)

    def test_modbus_rtu_server_with_pyserial_on_windows_and_linux(self):
        import serial
        listener = socket.socket();listener.bind(('127.0.0.1', 0));listener.listen(1);listener.settimeout(3)
        self.addCleanup(listener.close)
        link = config('modbus_rtu');link.update(role='server')
        link.pop('handshake')
        link['inputs'] = [{'tag': 'DB1.Start', 'area': 'coil', 'address': 0, 'datatype': 'bool'}]
        link['outputs'] = [{'tag': 'MotorOutput', 'area': 'discrete', 'address': 0, 'datatype': 'bool'}]
        with patch('serial.Serial', lambda port, **kw: serial.serial_for_url(f'socket://127.0.0.1:{listener.getsockname()[1]}', **kw)):
            self.io.apply([link], self.runtime.command('STATUS'))
            remote, _ = listener.accept()
            try:
                request = bytes.fromhex('01050000ff00')
                remote.sendall(request + struct.pack('<H', crc16(request)))
                self.assertEqual(recv_exact(remote, 8, time.monotonic() + 2), request + struct.pack('<H', crc16(request)))
                self.assertTrue(values(self.runtime.command('STATUS'))['DB1.Start'])
                self.runtime.command('RUN')
                until(lambda: values(self.runtime.command('STATUS'))['MotorOutput'])
                request = bytes.fromhex('010200000001')
                remote.sendall(request + struct.pack('<H', crc16(request)))
                reply = recv_exact(remote, 6, time.monotonic() + 2)
                self.assertEqual(reply[:4], bytes.fromhex('01020101'))
                self.assertEqual(crc16(reply[:-2]), struct.unpack('<H', reply[-2:])[0])
                request = bytes.fromhex('0107')  # Unsupported function, valid RTU frame.
                remote.sendall(request + struct.pack('<H', crc16(request)))
                reply = recv_exact(remote, 5, time.monotonic() + 2)
                self.assertEqual(reply[:3], bytes.fromhex('018701'))
            finally:
                self.runtime.command('STOP');self.io.close();remote.close()


if __name__ == '__main__':
    unittest.main()
