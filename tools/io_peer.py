"""Reference TCP/UDP/gRPC I/O device for commissioning; see docs/IO.md."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import copy
import json
import socketserver
import struct
import threading
import time
from plc_io import MAX_FRAME, RPC_PATH, decode_frame, encode_frame, recv_exact


class HandshakePeer:
    def __init__(self, inputs=None):
        self.inputs = inputs or {'start': True}
        self.outputs = {}
        self.lock = threading.Lock()
        self.session = None
        self.retired = []
        self.sequence = 0
        self.request = self.reply = None
        self.deadline = 0
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self.watchdog, daemon=True)
        self.thread.start()

    def zero(self):
        self.outputs = {k: False if type(v) is bool else 0 for k, v in self.outputs.items()}

    def watchdog(self):
        while not self.closed.wait(.01):
            with self.lock:
                if time.monotonic() > self.deadline:
                    self.zero()

    def exchange(self, frame):
        with self.lock:
            session, seq = frame.get('session'), frame.get('seq')
            if frame.get('version') != 1 or not isinstance(session, str) or len(session) != 32 or type(seq) is not int or seq < 1:
                raise ValueError('Invalid protocol version/session/sequence')
            if type(frame.get('outputs_enabled')) is not bool or type(frame.get('lease_ms')) is not int or not 1 <= frame['lease_ms'] <= 180000:
                raise ValueError('Invalid output enable/lease')
            outputs = frame.get('outputs')
            if not isinstance(outputs, dict) or len(outputs) > 128:
                raise ValueError('Invalid output map')
            encode_frame(outputs)  # Reject NaN/infinity before touching outputs.
            if any(type(v) not in (int, float, bool) for v in outputs.values()):
                raise ValueError('Only scalar I/O values are supported')
            if session != self.session:
                if session in self.retired or seq != 1 or frame['outputs_enabled']:
                    raise ValueError('New sessions must start at 1 with outputs disabled')
                if self.session:
                    self.retired = (self.retired + [self.session])[-64:]
                self.zero()
                self.session, self.sequence = session, 0
            if seq == self.sequence:
                if frame != self.request:
                    raise ValueError('Duplicate sequence has changed data')
                if time.monotonic() > self.deadline:
                    raise ValueError('Expired output lease; reconnect with a new session')
                return copy.deepcopy(self.reply)
            if seq != self.sequence + 1:
                raise ValueError('Out-of-order I/O sequence')
            if time.monotonic() > self.deadline and frame['outputs_enabled']:
                self.zero()
                raise ValueError('Expired output lease; re-arm with outputs disabled')
            self.outputs = dict(outputs)
            if not frame['outputs_enabled']:
                self.zero()
            self.sequence, self.request = seq, copy.deepcopy(frame)
            self.deadline = time.monotonic() + frame['lease_ms'] / 1000
            self.reply = {'version': 1, 'session': session, 'ack': seq, 'ready': True, 'inputs': copy.deepcopy(self.inputs)}
            return copy.deepcopy(self.reply)

    def close(self):
        self.closed.set()
        self.thread.join(timeout=1)
        with self.lock:
            self.zero()


class Server:
    """Context manager used by the CLI and integration tests."""
    def __init__(self, protocol, peer, host='127.0.0.1', port=0):
        self.protocol, self.peer = protocol, peer
        if protocol == 'grpc':
            import grpc
            from google.protobuf.wrappers_pb2 import BytesValue
            def exchange(message, context):
                try:
                    return BytesValue(value=encode_frame(peer.exchange(decode_frame(message.value))))
                except (ValueError, KeyError, TypeError) as error:
                    context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(error))
            self.server = grpc.server(ThreadPoolExecutor(max_workers=2), options=[('grpc.max_receive_message_length', MAX_FRAME + 32)])
            self.server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler(RPC_PATH.rsplit('/', 1)[0][1:], {
                'Exchange': grpc.unary_unary_rpc_method_handler(exchange, request_deserializer=BytesValue.FromString, response_serializer=BytesValue.SerializeToString)}),))
            self.port = self.server.add_insecure_port(f'[{host}]:{port}' if ':' in host else f'{host}:{port}')
            if not self.port:
                raise OSError('Cannot bind gRPC peer')
            self.server.start()
        else:
            class TCP(socketserver.BaseRequestHandler):
                def handle(self):
                    try:
                        while True:
                            deadline = time.monotonic() + 3
                            size = struct.unpack('>I', recv_exact(self.request, 4, deadline))[0]
                            if not 1 <= size <= MAX_FRAME:
                                return
                            reply = encode_frame(peer.exchange(decode_frame(recv_exact(self.request, size, deadline))))
                            self.request.sendall(struct.pack('>I', len(reply)) + reply)
                    except (OSError, ValueError, KeyError, TypeError):
                        pass
            class UDP(socketserver.BaseRequestHandler):
                def handle(self):
                    try:
                        data, sock = self.request
                        if len(data) > 1200:
                            return
                        reply = encode_frame(peer.exchange(decode_frame(data)))
                        if len(reply) <= 1200:
                            sock.sendto(reply, self.client_address)
                    except (OSError, ValueError, KeyError, TypeError):
                        pass
            base = socketserver.ThreadingUDPServer if protocol == 'udp' else socketserver.ThreadingTCPServer
            class Threaded(base):
                daemon_threads = True
                allow_reuse_address = True
                max_packet_size = 1201
            self.server = Threaded((host, port), UDP if protocol == 'udp' else TCP)
            self.port = self.server.server_address[1]
            self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .02}, daemon=True)
            self.thread.start()

    def close(self):
        if self.protocol == 'grpc':
            self.server.stop(0).wait(timeout=2)
        else:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=2)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol', choices=['tcp', 'udp', 'grpc'], default='tcp')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int)
    parser.add_argument('--inputs', default='{"start":true}')
    args = parser.parse_args()
    inputs = json.loads(args.inputs)
    if not isinstance(inputs, dict):
        parser.error('--inputs must be a JSON object')
    peer = HandshakePeer(inputs)
    try:
        with Server(args.protocol, peer, args.host, args.port or {'tcp': 15000, 'udp': 15001, 'grpc': 50051}[args.protocol]) as server:
            print(f'{args.protocol} reference device listening on {args.host}:{server.port}', flush=True)
            while True:
                time.sleep(1)
                with peer.lock:
                    print(json.dumps({'outputs': peer.outputs, 'ack': peer.sequence}), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        peer.close()


if __name__ == '__main__':
    main()
