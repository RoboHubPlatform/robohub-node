#!/usr/bin/env python3
"""End-to-end firmware update of a Linux host board (RNODE-UPD-0007, LNX-0009).

The board is managed/tests/e2e/host_node.c: the real robohub-linux platform with
its file slot, the core, the link and the managed layer. This script is its
gateway and its supervisor:

- the gateway, on the other end of a pseudo-terminal: it signs manifests with an
  Ed25519 key of its own (the board is given the public half), sends offers and
  chunks with up to four in flight, drops a chunk, cuts the link for a while and
  resumes, and reads the board's UPDATE_STATUS;
- the supervisor, as systemd's Restart=always would be: it starts the board
  again when it ends.

The image is the host node's own executable, so the image on trial is real code.

  1. update 100: drops, a cut link, resume, verified; the board ends (75), is
     started again, runs the image on trial and confirms it — reports confirmed;
  2. a downgrade (security version 0 after 1) is refused;
  3. update 101: verified; its trial crashes before confirming; the next start
     goes back to update 100's image and reports rolled back;
  4. update 103: verified; its trial cannot become healthy long enough before
     its trial limit, so it is given up and the old image reports rolled back.

    python3 e2e_update.py path/to/robohub-managed-host-node
"""
import hashlib
import os
import pty
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
except ImportError:
    print("skipped: python3 has no `cryptography` to sign manifests with (pip install cryptography)")
    sys.exit(77)

OFFER, CHUNK, STATUS, CANCEL = 0x0501, 0x0502, 0x0503, 0x0504
ACCEPTED, REFUSED, RECEIVING, VERIFIED, CONFIRMED, ROLLED_BACK, FAILED = range(1, 8)
DOWNGRADE = 3
CHUNK_SIZE = 192
UID = bytes([0xC0, 0xFF, 0xEE, 1, 2, 3, 4, 5])

failures = []


def check(ok, what):
    print(("[PASS] " if ok else "[FAIL] ") + what)
    if not ok:
        failures.append(what)


def fletcher16(data):
    a = b = 0
    for x in data:
        a = (a + x) % 255
        b = (b + a) % 255
    return (b << 8) | a


def cobs_encode(data):
    out, block = bytearray(), bytearray()
    for x in data:
        if x == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(x)
            if len(block) == 254:
                out += bytes([255]) + block
                block = bytearray()
    out += bytes([len(block) + 1]) + block
    return bytes(out)


def cobs_decode(data):
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        i += 1
        out += data[i:i + code - 1]
        i += code - 1
        if code != 255 and i < len(data):
            out.append(0)
    return bytes(out)


def frame(msg_id, payload):
    body = struct.pack("<HHH", msg_id, 0, len(payload)) + payload
    body += struct.pack("<H", fletcher16(body))
    return cobs_encode(body) + b"\x00"


class Board:
    """The host node, its pty and its supervisor."""

    def __init__(self, binary, work, public_hex):
        self.binary, self.work = binary, work
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(slave)
        self.slave_path = os.ttyname(slave)
        self.env = dict(os.environ)
        self.env.update({
            "RHO_UART0": self.slave_path, "RH_UPDATE_DIR": os.path.join(work, "update"),
            "RH_TEST_UID": UID.hex(), "RH_TEST_UPDATE_KEY": public_hex,
            "RH_TEST_FIRMWARE": "1", "RH_TEST_CONFIRM_MS": "2000",
        })
        self.statuses = []
        self.lock = threading.Lock()
        self.proc = None
        self.log = open(os.path.join(work, "board.log"), "ab")
        self.running = True
        threading.Thread(target=self.reader, daemon=True).start()

    def start(self, **extra):
        env = dict(self.env)
        env.update(extra)
        for key in [k for k, v in extra.items() if v is None]:
            env.pop(key, None)
        self.proc = subprocess.Popen([self.binary], env=env, stdout=self.log, stderr=self.log)

    def wait_exit(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def reader(self):
        buf = bytearray()
        while self.running:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:
                time.sleep(0.05)
                continue
            buf += data
            while b"\x00" in buf:
                raw, _, rest = buf.partition(b"\x00")
                buf = bytearray(rest)
                body = cobs_decode(bytes(raw))
                if len(body) < 8:
                    continue
                msg_id, _tx, length = struct.unpack_from("<HHH", body)
                payload = body[6:6 + length]
                if msg_id == STATUS and len(payload) == 20:
                    _v, uid, state, reason, nxt, version, slot = struct.unpack("<BIIIIHB", payload)
                    with self.lock:
                        self.statuses.append((uid, state, reason, nxt, version, slot))

    def send(self, msg_id, payload):
        os.write(self.master, frame(msg_id, payload))

    def wait_status(self, test, timeout=10.0):
        end = time.time() + timeout
        seen = 0
        while time.time() < end:
            with self.lock:
                fresh = self.statuses[seen:]
                seen = len(self.statuses)
            for s in fresh:
                if test(s):
                    return s
            time.sleep(0.02)
        return None

    def stop(self):
        self.running = False
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def offer_payload(key, update_id, image, security):
    manifest = struct.pack("<BI8sHII32sH", 1, update_id, UID, 2, security, len(image),
                           hashlib.sha256(image).digest(), CHUNK_SIZE)
    signature = key.sign(b"rh-update1" + manifest)
    return b"\x01" + manifest + signature


def send_image(board, update_id, image, drop=None, cut_at=None, key=None, security=1):
    """Send every chunk with up to four in flight; resume where the board says."""
    chunks = (len(image) + CHUNK_SIZE - 1) // CHUNK_SIZE
    acked = 0       # the next index the board expects, as it last said
    sent = 0        # the next index to send
    dropped = set()
    cut_done = False
    last_progress = time.time()
    while True:
        with board.lock:
            mine = [s for s in board.statuses if s[0] == update_id]
        if mine:
            uid, state, reason, nxt, _v, _s = mine[-1]
            if state == VERIFIED:
                return mine[-1]
            if state in (FAILED, REFUSED):
                return mine[-1]
            if nxt > acked or (state in (RECEIVING, ACCEPTED) and nxt < sent and time.time() - last_progress > 0.3):
                if nxt != acked:
                    last_progress = time.time()
                acked = nxt
                if sent < acked or sent - acked > 4 or time.time() - last_progress > 0.3:
                    sent = max(acked, min(sent, acked + 4)) if sent >= acked else acked
        if cut_at is not None and not cut_done and acked >= cut_at:
            # The link is cut: nothing for a while, then the same offer again.
            cut_done = True
            time.sleep(1.5)
            with board.lock:
                board.statuses.clear()
            board.send(OFFER, offer_payload(key, update_id, image, security))
            resumed = board.wait_status(lambda s: s[0] == update_id and s[1] == ACCEPTED)
            check(resumed is not None and resumed[3] >= cut_at, f"after a cut link the board resumes at {resumed and resumed[3]}")
            acked = sent = resumed[3] if resumed else acked
            last_progress = time.time()
            continue
        if sent < chunks and sent < acked + 4:
            index = sent
            sent += 1
            if drop is not None and index == drop and index not in dropped:
                dropped.add(index)  # lost on the way
                continue
            data = image[index * CHUNK_SIZE:(index + 1) * CHUNK_SIZE]
            board.send(CHUNK, struct.pack("<II", update_id, index) + data)
            continue
        # The window is full, or everything was sent: a silence means a loss.
        time.sleep(0.01)
        if time.time() - last_progress > 2.0:
            sent = acked
            last_progress = time.time()
        if time.time() - last_progress > 30:
            return None


def main():
    binary = sys.argv[1]
    work = tempfile.mkdtemp(prefix="rh-e2e-update-")
    key = Ed25519PrivateKey.generate()
    public_hex = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
    image = open(binary, "rb").read()
    board = Board(binary, work, public_hex)
    try:
        board.start()
        time.sleep(0.5)

        # 1. An update, with a lost chunk and a cut link, confirmed.
        board.send(OFFER, offer_payload(key, 100, image, 1))
        s = board.wait_status(lambda s: s[0] == 100)
        check(s is not None and s[1] == ACCEPTED and s[3] == 0, f"update 100 is accepted ({s})")
        chunks = (len(image) + CHUNK_SIZE - 1) // CHUNK_SIZE
        done = send_image(board, 100, image, drop=10, cut_at=chunks // 3, key=key)
        check(done is not None and done[1] == VERIFIED, f"update 100 is verified ({done})")
        code = board.wait_exit(10)
        check(code == 75, f"the board ends to boot the image once ({code})")
        board.start(RH_TEST_FIRMWARE="2")
        s = board.wait_status(lambda s: s[0] == 100 and s[1] in (CONFIRMED, ROLLED_BACK), timeout=15)
        check(s is not None and s[1] == CONFIRMED and s[4] == 2 and s[5] == 1,
              f"its trial confirms it: confirmed, firmware 2, slot 1 ({s})")
        state = open(os.path.join(work, "update", "state")).read()
        check("running=1" in state and "security=1" in state, "the board runs slot 1 from now on, at security version 1")

        # 2. A downgrade is refused.
        board.send(OFFER, offer_payload(key, 102, image, 0))
        s = board.wait_status(lambda s: s[0] == 102)
        check(s is not None and s[1] == REFUSED and s[2] == DOWNGRADE, f"a downgrade is refused ({s})")

        # 3. An update whose trial crashes: rolled back.
        board.send(OFFER, offer_payload(key, 101, image, 1))
        s = board.wait_status(lambda s: s[0] == 101)
        check(s is not None and s[1] == ACCEPTED, f"update 101 is accepted ({s})")
        done = send_image(board, 101, image, key=key)
        check(done is not None and done[1] == VERIFIED, f"update 101 is verified ({done})")
        check(board.wait_exit(10) == 75, "the board ends to boot it once")
        board.start(RH_TEST_FIRMWARE="3", RH_TEST_CRASH="1")
        code = board.wait_exit(10)
        check(code == 1, f"its trial crashes before it is confirmed ({code})")
        board.start(RH_TEST_FIRMWARE="2", RH_TEST_CRASH=None)
        s = board.wait_status(lambda s: s[0] == 101 and s[1] in (CONFIRMED, ROLLED_BACK), timeout=15)
        check(s is not None and s[1] == ROLLED_BACK and s[5] == 1,
              f"the next start goes back to slot 1 and says rolled back ({s})")

        # 4. An update given up at its trial limit (RNODE-UPD-0008, COMM-0014).
        board.send(OFFER, offer_payload(key, 103, image, 1))
        s = board.wait_status(lambda s: s[0] == 103)
        check(s is not None and s[1] == ACCEPTED, f"update 103 is accepted ({s})")
        done = send_image(board, 103, image, key=key)
        check(done is not None and done[1] == VERIFIED, f"update 103 is verified ({done})")
        check(board.wait_exit(10) == 75, "the board ends to boot it once")
        board.start(RH_TEST_FIRMWARE="4", RH_TEST_CONFIRM_MS="600000", RH_TEST_TRIAL_MS="3000")
        code = board.wait_exit(15)
        check(code == 75, f"its trial is given up at the limit, and the board ends to start the old image ({code})")
        board.start(RH_TEST_FIRMWARE="2", RH_TEST_CONFIRM_MS=None, RH_TEST_TRIAL_MS=None)
        s = board.wait_status(lambda s: s[0] == 103 and s[1] in (CONFIRMED, ROLLED_BACK), timeout=15)
        check(s is not None and s[1] == ROLLED_BACK and s[5] == 1,
              f"the old image runs again and says rolled back ({s})")
    finally:
        board.stop()
        if failures:
            print(open(os.path.join(work, "board.log"), "rb").read().decode(errors="replace")[-3000:])
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print("FAILED:", *failures, sep="\n  ")
        return 1
    print("OK: a Linux host board updated over its link, confirmed, refused a downgrade, rolled back a crash and a trial that ran out")
    return 0


if __name__ == "__main__":
    sys.exit(main())
