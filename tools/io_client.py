"""Reference client for PLC TCP, UDP and gRPC server connections."""
import argparse
import json
import secrets
import time
from plc_io import make_client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol', choices=['tcp', 'udp', 'grpc'], default='tcp')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=15010)
    parser.add_argument('--write', action='append', default=[], metavar='CHANNEL=JSON_VALUE')
    parser.add_argument('--cycle-ms', type=int, default=100)
    parser.add_argument('--tls', action='store_true')
    parser.add_argument('--ca-file')
    args = parser.parse_args()
    if not 5 <= args.cycle_ms <= 1000:
        parser.error('--cycle-ms must be 5–1000')
    outputs = {}
    for item in args.write:
        key, separator, value = item.partition('=')
        if not separator or not key:
            parser.error('--write requires CHANNEL=JSON_VALUE')
        outputs[key] = json.loads(value)
    client = make_client(vars(args))
    session = secrets.token_hex(16)
    sequence = 0
    try:
        while True:
            sequence += 1
            frame = {'version': 1, 'session': session, 'seq': sequence, 'outputs_enabled': sequence > 1,
                     'lease_ms': max(1000, 3 * args.cycle_ms), 'outputs': outputs}
            reply = client.exchange(frame, time.monotonic() + 1)
            if reply.get('session') != session or reply.get('ack') != sequence or reply.get('ready') is not True:
                raise RuntimeError('Server acknowledgement mismatch or CPU fault')
            print(json.dumps(reply), flush=True)
            time.sleep(args.cycle_ms / 1000)
    except KeyboardInterrupt:
        try:
            client.exchange({**frame, 'seq': sequence + 1, 'outputs_enabled': False}, time.monotonic() + 1)
        except Exception:
            pass
    finally:
        client.close()


if __name__ == '__main__':
    main()
