#!/usr/bin/env python3
"""
Push a face designed on the website to a device, and watch it land.

The point of the exercise is that **nothing but choices crosses the wire**. The
artwork is already on the device, baked from the same catalogue the website draws
with, so a whole redesign is a few hundred bytes and appears instantly.

    export STEM_MQTT_USER=ESP32-sim-xxxx STEM_MQTT_PASS=<device jwt>
    python3 tools/send_face.py --preset star          # a face, by hand
    python3 tools/send_face.py --file design.json     # whatever the studio saved

WHICH TOPIC
-----------
A device token may *subscribe* to its own directive topic and never publish there —
that is the gateway's privilege, and rightly so. So this publishes to the device's
own **event** topic, which the same token may write, and the firmware listens there
only when built with CONFIG_BUDDY_DESIGN_VIA_EVENT. It exercises the same handler
the gateway's directive reaches.

With service credentials (STEM_MQTT_ADMIN_USER / _PASS) it publishes to the real
directive topic instead, which is the production path.

MQTT in pure stdlib, following ../buddyos-stem-c-firmware/tools/send_directive.py —
no paho, no broker install.
"""
import argparse, json, os, socket, ssl, struct, sys, time, uuid

HOST = os.environ.get("STEM_MQTT_HOST", "mqtt-sg.buddyos.ai")
PORT = int(os.environ.get("STEM_MQTT_PORT", "1883"))
ENV = os.environ.get("STEM_ENV", "staging")

# Faces the device can certainly draw: every part here is in the baked catalogue.
PRESETS = {
    "star":   {"eyes": "star",   "mouth": "grin",     "brows": "raised"},
    "sleepy": {"eyes": "sleepy", "mouth": "snore",    "brows": "sad"},
    "angry":  {"eyes": "angry",  "mouth": "bigfrown", "brows": "angry"},
    "heart":  {"eyes": "heart",  "mouth": "kiss",     "brows": "oneup"},
    "cat":    {"eyes": "happy",  "mouth": "cat",      "brows": "flat"},
    "plain":  {"eyes": "round",  "mouth": "flatsmile", "brows": "flat"},
}
STATES = ["idle", "listening", "thinking", "speaking"]


def _s(x):
    b = x.encode()
    return struct.pack(">H", len(b)) + b


def _rl(n):
    out = b""
    while True:
        d = n % 128
        n //= 128
        out += bytes([d | (0x80 if n else 0)])
        if not n:
            return out


class Mqtt:
    def __init__(self, host, port, cid, user, pw):
        raw = socket.create_connection((host, port), timeout=10)
        # The broker terminates TLS on this port. Verification is the firmware's
        # job with its embedded root; this is a bench tool on a trusted desk.
        ctx = ssl._create_unverified_context()
        self.s = ctx.wrap_socket(raw, server_hostname=host)
        body = _s("MQTT") + bytes([4, 0xC2]) + struct.pack(">H", 30) + _s(cid) + _s(user) + _s(pw)
        self.s.sendall(bytes([0x10]) + _rl(len(body)) + body)
        r = self.s.recv(4)
        if len(r) < 4 or r[3] != 0:
            raise SystemExit("CONNECT refused: %s" % {4: "bad username/password",
                                                      5: "not authorized"}.get(r[3] if len(r) > 3 else -1, r))

    def publish(self, topic, payload, qos=1, retain=False):
        body = _s(topic) + (struct.pack(">H", 1) if qos else b"") + payload.encode()
        self.s.sendall(bytes([0x30 | (qos << 1) | (1 if retain else 0)]) + _rl(len(body)) + body)

    def close(self):
        try:
            self.s.sendall(bytes([0xE0, 0x00]))
        finally:
            self.s.close()


def design_from(args):
    if args.clear:
        return {}
    if args.file:
        extra = json.load(open(args.file))
        # Accept a whole design row, the `extra` column, or a directive.
        return extra.get("extra", extra.get("data", extra))
    face = PRESETS[args.preset]
    return {"faces": {"v": 1, "states": {st: dict(face) for st in STATES}},
            "talk": {"v": 1, "style": args.style, "parts": {}}}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--preset", choices=sorted(PRESETS), help="a face built here")
    g.add_argument("--file", help="a design saved by the studio (JSON)")
    g.add_argument("--clear", action="store_true", help="back to the built-in face")
    ap.add_argument("--style", default="natural", help="how it talks: natural, gentle, big, cheeky")
    ap.add_argument("--serial", default=os.environ.get("STEM_MQTT_USER", ""), help="device serial")
    ap.add_argument("--no-retain", action="store_true", help="do not leave it waiting for an offline kit")
    args = ap.parse_args()

    user = os.environ.get("STEM_MQTT_ADMIN_USER") or os.environ.get("STEM_MQTT_USER")
    pw = os.environ.get("STEM_MQTT_ADMIN_PASS") or os.environ.get("STEM_MQTT_PASS")
    if not (user and pw and args.serial):
        sys.exit("set STEM_MQTT_USER and STEM_MQTT_PASS (the device token), or the admin pair")

    admin = bool(os.environ.get("STEM_MQTT_ADMIN_USER"))
    kind = "directive" if admin else "event"
    topic = f"{ENV}/stem/device/{args.serial}/{kind}"

    directive = {"v": 1, "id": uuid.uuid4().hex, "type": "face.update",
                 "serial": args.serial, "ts": int(time.time()),
                 "data": design_from(args)}
    payload = json.dumps(directive, separators=(",", ":"))

    m = Mqtt(HOST, PORT, f"send-face-{uuid.uuid4().hex[:8]}", user, pw)
    # Retained, as the gateway sends it: a kit is usually switched off while a child
    # is designing for it, and a clean-session device would simply miss the message.
    m.publish(topic, payload, retain=not args.no_retain)
    time.sleep(0.5)
    m.close()
    print(f"sent {len(payload)} B to {topic}")
    idle = directive["data"].get("faces", {}).get("states", {}).get("idle")
    print("  " + (json.dumps(idle) if idle else "cleared — back to the built-in face"))
    if not admin:
        print("  (bench path: the device must be built with CONFIG_BUDDY_DESIGN_VIA_EVENT)")


if __name__ == "__main__":
    main()
