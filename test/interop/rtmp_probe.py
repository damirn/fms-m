# Raw RTMP probes for behaviour no reference client exercises: a control message
# before connect, and the acknowledgement cadence a peer-announced window sets.
# Simple (pre-FP9) handshake only -- these never reach the crypto path.

import socket, struct, sys, time

def amf_str(s):
    b = s.encode(); return b'\x02' + struct.pack('>H', len(b)) + b
def amf_num(x):
    return b'\x00' + struct.pack('>d', x)
def amf_obj(d):
    out = b'\x03'
    for k, v in d.items():
        kb = k.encode()
        out += struct.pack('>H', len(kb)) + kb
        out += amf_str(v) if isinstance(v, str) else amf_num(v)
    return out + b'\x00\x00\x09'

def chunk(csid, msg_type, stream_id, payload):
    # fmt 0 basic header, 11-byte message header, little-endian stream id
    h = bytes([csid & 0x3F])
    h += b'\x00\x00\x00'                                   # timestamp
    h += struct.pack('>I', len(payload))[1:]               # 3-byte length
    h += bytes([msg_type])
    h += struct.pack('<I', stream_id)
    return h + payload

def handshake(s):
    s.sendall(b'\x03' + b'\x00' * 1536)                    # C0 + simple C1
    buf = b''
    while len(buf) < 1 + 1536 + 1536:
        d = s.recv(4096)
        if not d: raise RuntimeError('eof during handshake')
        buf += d
    s1 = buf[1:1537]
    s.sendall(s1)                                          # C2 echoes S1
    return buf[1 + 1536 + 1536:]

def read_for(s, seconds):
    s.settimeout(0.3)
    end = time.time() + seconds
    out = b''
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            out += d
        except socket.timeout:
            pass
    return out

def main():
    mode, port = sys.argv[1], int(sys.argv[2])
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    extra = handshake(s)

    if mode == 'preconnect-control':
        # WindowAcknowledgementSize before connect: legal per spec.
        s.sendall(chunk(2, 0x05, 0, struct.pack('>I', 2500000)))
        time.sleep(0.2)
        body = amf_str('connect') + amf_num(1.0) + amf_obj({'app': 'media', 'tcUrl': 'rtmp://127.0.0.1/media'})
        s.sendall(chunk(3, 0x14, 0, body))
        got = extra + read_for(s, 3)
        print('RESULT', 'ok' if b'_result' in got else 'no-result', len(got))

    elif mode == 'zero-window':
        s.sendall(chunk(2, 0x05, 0, struct.pack('>I', 0)))
        body = amf_str('connect') + amf_num(1.0) + amf_obj({'app': 'media', 'tcUrl': 'rtmp://127.0.0.1/media'})
        s.sendall(chunk(3, 0x14, 0, body))
        got = extra + read_for(s, 2)
        # The window is only "ignored" if the connect was still answered: a server
        # that dropped the client returns few bytes too.
        print('RESULT', 'ok' if b'_result' in got else 'no-result', len(got))
        # Paced: the acknowledgement is emitted per read, so the sends have to
        # arrive as separate reads for the cadence to be observable at all.
        base = len(got)
        sent = 0
        for _ in range(40):
            try:
                s.sendall(chunk(3, 0x14, 0, body))
                sent += 1
                time.sleep(0.02)
            except OSError:
                break
        try:
            got += read_for(s, 2)
        except OSError:
            pass
        print('SENDS_COMPLETED', sent)
        print('BYTES_AFTER_PACED_SENDS', len(got) - base)

    elif mode == 'window-ack-delta':
        # The absolute byte count is dominated by the 40 connect responses, so it
        # moves whenever the _result object changes. Measure the same workload twice
        # -- once with a window too large to be spent, once with the hostile zero --
        # and compare: only the acknowledgement cadence differs between them.
        def paced(sock, window, first):
            sock.sendall(chunk(2, 0x05, 0, struct.pack('>I', window)))
            body = amf_str('connect') + amf_num(1.0) + amf_obj({'app': 'media', 'tcUrl': 'rtmp://127.0.0.1/media'})
            sock.sendall(chunk(3, 0x14, 0, body))
            got = first + read_for(sock, 2)
            answered = b'_result' in got
            base = len(got)
            sent = 0
            for _ in range(40):
                try:
                    sock.sendall(chunk(3, 0x14, 0, body))
                    sent += 1
                    time.sleep(0.02)
                except OSError:
                    break
            try:
                got += read_for(sock, 2)
            except OSError:
                pass
            return answered, sent, len(got) - base

        big_ok, big_sent, big_bytes = paced(s, 2500000, extra)
        s.close()

        s2 = socket.create_connection(('127.0.0.1', port), timeout=10)
        extra2 = handshake(s2)
        zero_ok, zero_sent, zero_bytes = paced(s2, 0, extra2)
        s2.close()

        print('RESULT', 'ok' if (big_ok and zero_ok) else 'no-result')
        print('SENDS_COMPLETED', min(big_sent, zero_sent))
        print('BASELINE_BYTES', big_bytes)
        print('ZERO_BYTES', zero_bytes)
        print('DELTA', zero_bytes - big_bytes)
        return

    s.close()

main()
