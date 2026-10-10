#!/usr/bin/env python3
"""Record and render the handset's screen from a SCREEN_CAPTURE build.

See main/screencap.c (the wire format).

    screencap.py record /dev/ttyACM0 take1.scap     # Ctrl+C to stop
    screencap.py render take1.scap take1.mp4 [--scale 3] [--fps 30] [--no-taps]
                        [--start SEC] [--end SEC]
    screencap.py png    take1.scap shot.png [--at SEC] [--scale 1]
    screencap.py info   take1.scap

`record` keeps the RAW byte stream (so a render can be redone with other
options) and asks the handset for a full frame on connect and whenever a packet
arrives damaged. Times in render/png are seconds from the first complete frame.
"""
import struct, sys, time, zlib, argparse, subprocess

W, H = 240, 320
MAGIC = b"SCAP"


# ---------------------------------------------------------------- parsing
def packets(data):
    """Yield (type, payload) for every packet with a good CRC; count the rest."""
    i, bad = 0, 0
    n = len(data)
    while True:
        j = data.find(MAGIC, i)
        if j < 0 or j + 10 > n:
            break
        typ = data[j + 4]
        (ln,) = struct.unpack_from("<I", data, j + 6)
        end = j + 10 + ln + 4
        if typ not in (1, 2) or ln > W * H * 2 + 64 or end > n:
            if end > n and typ in (1, 2) and ln <= W * H * 2 + 64:
                break                       # incomplete tail
            i = j + 1; bad += 1; continue
        body = data[j + 10: j + 10 + ln]
        (crc,) = struct.unpack_from("<I", data, j + 10 + ln)
        if zlib.crc32(body) != crc:
            i = j + 1; bad += 1; continue
        yield typ, body
        i = end
    packets.bad = bad
    packets.consumed = i


def events(data):
    """Regions and touches, in device time order (ms)."""
    out = []
    for typ, b in packets(data):
        if typ == 1:
            seq, t, x1, y1, x2, y2 = struct.unpack_from("<IIHHHH", b)
            out.append(("r", t, (x1, y1, x2, y2), b[16:]))
        else:
            t, x, y, p = struct.unpack_from("<Ihhb", b)
            out.append(("t", t, (x, y), bool(p)))
    return out


# ---------------------------------------------------------------- record
def record(port, path):
    import serial
    p = serial.Serial(port, 115200, timeout=0.2)
    p.dtr = True          # native USB CDC is mute without DTR
    p.rts = False
    p.write(b"F")
    buf = b""
    t0 = time.time(); last = t0; last_f = t0; total = 0; regions = 0; bad_seen = 0
    print(f"recording {port} -> {path}  (Ctrl+C to stop)", flush=True)
    with open(path, "wb") as out:
        try:
            while True:
                c = p.read(65536)
                if c:
                    out.write(c); total += len(c); buf += c
                    evs = 0
                    for typ, _ in packets(buf):
                        regions += typ == 1
                    if packets.bad > 0:
                        bad_seen += packets.bad
                        p.write(b"F")            # resync: resend the whole screen
                    buf = buf[packets.consumed:]
                    if len(buf) > 4 * W * H:
                        buf = buf[-W * H:]
                now = time.time()
                if regions == 0 and now - last_f >= 1:
                    p.write(b"F")      # board may still be booting: keep asking
                    last_f = now
                if now - last >= 2:
                    print(f"  {now - t0:6.1f}s  {total / 1024:8.0f} KB  "
                          f"{regions} regions  {bad_seen} bad", flush=True)
                    last = now
        except KeyboardInterrupt:
            pass
    p.write(b"S")
    print(f"done: {total} bytes, {regions} regions, {bad_seen} damaged packets")


# ---------------------------------------------------------------- render
def frames(evs, fps, start, end):
    """Yield (t_sec, framebuffer, touch) at a steady rate."""
    import numpy as np
    fb = np.zeros((H, W, 3), np.uint8)
    covered = np.zeros((H, W), bool)
    t_first = None
    touch = None            # (x, y, pressed, t_ms of last change)
    k = 0
    step = 1000.0 / fps
    # Find the first moment the whole screen has been painted.
    for e in evs:
        if e[0] == "r":
            x1, y1, x2, y2 = e[2]
            covered[y1:y2 + 1, x1:x2 + 1] = True
            if covered.all():
                t_first = e[1]; break
    if t_first is None:
        sys.exit("no complete frame in the recording")
    t_end_ms = evs[-1][1]
    t = t_first + start * 1000.0
    stop = t_first + end * 1000.0 if end else t_end_ms + 500
    while t <= stop:
        while k < len(evs) and evs[k][1] <= t:
            e = evs[k]; k += 1
            if e[0] == "r":
                x1, y1, x2, y2 = e[2]
                w, h = x2 - x1 + 1, y2 - y1 + 1
                px = np.frombuffer(e[3], ">u2").reshape(h, w)  # LV_COLOR_16_SWAP
                r = ((px >> 11) & 0x1F).astype(np.uint16) * 255 // 31
                g = ((px >> 5) & 0x3F).astype(np.uint16) * 255 // 63
                b = (px & 0x1F).astype(np.uint16) * 255 // 31
                fb[y1:y2 + 1, x1:x2 + 1] = np.dstack([r, g, b]).astype(np.uint8)
            else:
                touch = (*e[2], e[3], e[1])
        yield (t - t_first) / 1000.0, fb, touch, t
        t += step


def draw_tap(img, touch, t_ms, scale):
    """A translucent dot under the finger; fades for 300 ms after release."""
    import numpy as np
    if not touch:
        return img
    x, y, pressed, tc = touch
    a = 0.45 if pressed else 0.45 * max(0.0, 1 - (t_ms - tc) / 300.0)
    if a <= 0:
        return img
    r = 13 * scale
    cx, cy = (x + 0.5) * scale, (y + 0.5) * scale
    hh, ww = img.shape[:2]
    y0, y1 = max(0, int(cy - r)), min(hh, int(cy + r) + 1)
    x0, x1 = max(0, int(cx - r)), min(ww, int(cx + r) + 1)
    yy, xx = np.mgrid[y0:y1, x0:x1]
    m = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
    sub = img[y0:y1, x0:x1].astype(np.float32)
    sub[m] = sub[m] * (1 - a) + np.array([255, 255, 255]) * a
    img[y0:y1, x0:x1] = sub.astype(np.uint8)
    return img


def render(src, dst, scale, fps, taps, start, end):
    import numpy as np
    evs = events(open(src, "rb").read())
    ff = subprocess.Popen(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{W * scale}x{H * scale}", "-r", str(fps), "-i", "-",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20",
         "-preset", "slow", "-movflags", "+faststart", dst],
        stdin=subprocess.PIPE)
    n = 0
    for ts, fb, touch, t_ms in frames(evs, fps, start, end):
        img = fb.repeat(scale, 0).repeat(scale, 1) if scale > 1 else fb.copy()
        if taps:
            img = draw_tap(img, touch, t_ms, scale)
        ff.stdin.write(np.ascontiguousarray(img).tobytes()); n += 1
    ff.stdin.close(); ff.wait()
    print(f"{dst}: {n} frames, {n / fps:.1f}s")


def png(src, dst, at, scale):
    from PIL import Image
    evs = events(open(src, "rb").read())
    for ts, fb, _, _ in frames(evs, 50, at, at):
        img = fb.repeat(scale, 0).repeat(scale, 1)
        Image.fromarray(img).save(dst)
        print(f"{dst} at {ts:.2f}s"); return


def info(src):
    data = open(src, "rb").read()
    evs = events(data)
    rs = [e for e in evs if e[0] == "r"]
    ts = [e for e in evs if e[0] == "t"]
    px = sum(len(e[3]) for e in rs)
    if not rs:
        print("no regions"); return
    dur = (rs[-1][1] - rs[0][1]) / 1000 or 1
    print(f"{len(data)} bytes, {len(rs)} regions ({px / 1024:.0f} KB pixels), "
          f"{len(ts)} touch events, {packets.bad} damaged, "
          f"{dur:.1f}s, {px / dur / 1024:.0f} KB/s pixel throughput")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    a = sp.add_parser("record"); a.add_argument("port"); a.add_argument("out")
    a = sp.add_parser("render"); a.add_argument("src"); a.add_argument("dst")
    a.add_argument("--scale", type=int, default=3); a.add_argument("--fps", type=int, default=30)
    a.add_argument("--no-taps", action="store_true")
    a.add_argument("--start", type=float, default=0); a.add_argument("--end", type=float, default=0)
    a = sp.add_parser("png"); a.add_argument("src"); a.add_argument("dst")
    a.add_argument("--at", type=float, default=0); a.add_argument("--scale", type=int, default=1)
    a = sp.add_parser("info"); a.add_argument("src")
    o = ap.parse_args()
    if o.cmd == "record": record(o.port, o.out)
    elif o.cmd == "render": render(o.src, o.dst, o.scale, o.fps, not o.no_taps, o.start, o.end)
    elif o.cmd == "png": png(o.src, o.dst, o.at, o.scale)
    else: info(o.src)
