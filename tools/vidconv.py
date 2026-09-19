#!/usr/bin/env python3
"""vidconv.py -- turn a video (and its cover picture) into a NatOS .nvd file.

WHY THIS RUNS ON THE PC. The board cannot decode H.264: a single 320x240
reference frame is ~115 KB against ~60 KB of spare RAM, and MP3 alone already
costs it 46% of the CPU at 80 MHz (docs/next_moves/11 step 4). It cannot write
files either -- fat.c is read-only. So every expensive step happens here, once,
and the board is left with copying bytes:

    decode, scale, (rotate), cut the frame rate     ffmpeg
    colours -> the panel's RGB565, or 8-bit + a palette of RGB565
    audio   -> 8-bit unsigned mono PCM, the DAC's own format
    cover   -> RGB565 at a small size, ready to blit

THE FILE (.nvd, version 1). All integers little-endian, because the ESP32 is:
RGB565 words can be handed to display_blit() as they sit in memory -- it takes
plain RGB565 (red = 0xF800) and does its own byte swap for the panel.

    sector 0   header (HEADER, then ICON at byte 192; the rest zero)
    sector 1   palette: 256 x u16 RGB565 (PAL8), zeros otherwise
    cover      cover_w x cover_h x u16 RGB565, padded to a sector (optional)
    icon       icon_w x icon_h x u16 RGB565, padded to a sector (optional) --
               the small picture a list of videos shows
    chunks     frame_count of them, each exactly chunk_stride bytes (a
               multiple of 512), starting at first_chunk:
                   0   'NVCK'
                   4   u32 frame index
                   8   u32 audio bytes in this chunk
                  12   u32 reserved (0)
                  16   frame: w*h bytes (PAL8) or w*h*2 (RGB565)
                  16+frame_bytes   audio, then zero padding to the stride

A fixed stride means seeking to frame N is arithmetic, and whole-sector reads
are what the SD driver does fastest. Chunk i carries exactly the audio samples
[A(i), A(i+1)) where A(i) = round(i * rate / fps), so audio cannot drift from
video however long the file is.

Usage:
    python tools/vidconv.py "E:\\video.mkv"                 -> E:\\<title>.nvd
    python tools/vidconv.py IN [-o OUT] [--width 240] [--fps 10]
                            [--format pal8|rgb565] [--rotate]
                            [--audio-rate 22050 | --no-audio]
                            [--cover IMG | --no-cover] [--start S] [--duration S]
    python tools/vidconv.py --set-cover PICTURE FILE.nvd   (new cover + icon)
    python tools/vidconv.py --check FILE.nvd
    python tools/vidconv.py --preview FILE.nvd --frame N --out frame.png
    python tools/vidconv.py --preview FILE.nvd --cover-only --out cover.png
    python tools/vidconv.py --selftest

Needs ffmpeg and ffprobe on PATH. Nothing else outside the standard library.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unicodedata

SECTOR = 512
MAGIC = b"NVID"
CHUNK_MAGIC = b"NVCK"
VERSION = 1
PIX_PAL8, PIX_RGB565 = 1, 2
AUD_NONE, AUD_U8_MONO = 0, 1

# Panel limits (kernel/display.h: 240 x 320, portrait). A landscape video is
# letterboxed across the 240-pixel width unless --rotate turns it sideways.
PANEL_W, PANEL_H = 240, 320

# Header layout. Changing it means bumping VERSION and the board's reader.
HEADER = struct.Struct(
    "<4s"   # 0   magic 'NVID'
    "H"     # 4   version
    "H"     # 6   header sectors before the cover (2: header + palette)
    "HH"    # 8   width, height
    "HH"    # 12  fps numerator, denominator
    "BB"    # 16  pixel format, rotated (0/1)
    "H"     # 18  reserved
    "I"     # 20  frame count
    "I"     # 24  frame bytes (video, per chunk)
    "I"     # 28  audio rate (Hz)
    "B3x"   # 32  audio format
    "I"     # 36  audio bytes max per chunk
    "I"     # 40  chunk stride (multiple of 512)
    "I"     # 44  first chunk offset
    "I"     # 48  cover offset (0 = none)
    "HH"    # 52  cover width, height
    "I"     # 56  duration, ms
    "4x"    # 60  reserved
    "128s"  # 64  title, ASCII, NUL-padded
)
assert HEADER.size == 192

# The list icon, right after HEADER in sector 0 (added 2026-09-19 for the
# video browser; 0 offset = no icon, so files without one still parse). The
# cover is 120 wide for a "now showing" panel; a list row wants something a
# quarter the size, and reading six rows' icons should cost sectors, not a
# cover each.
ICON = struct.Struct(
    "<I"    # 192 icon offset (0 = none)
    "HH"    # 196 icon width, height (RGB565)
)
ICON_AT = HEADER.size


def die(msg):
    print("vidconv: " + msg, file=sys.stderr)
    sys.exit(1)


def need_tools():
    for t in ("ffmpeg", "ffprobe"):
        if not shutil.which(t):
            die("%s is not on PATH" % t)


def round_up(n, to):
    return (n + to - 1) // to * to


# ---- names -------------------------------------------------------------------

def ascii_title(s):
    """What the board can show. Its FAT reader turns anything outside ASCII
    into '?', so fold what has an ASCII form (NFKC turns fullwidth bars and
    letters into plain ones) and drop the rest. A trailing YouTube-style id
    in brackets is noise on a 240-pixel screen."""
    s = unicodedata.normalize("NFKC", s)
    # The closing bracket is optional: the example's name was cut short on the
    # card ("...[LfLmuP.mkv"), and the first version kept "[LfLmuP" in the title.
    s = re.sub(r"\s*\[[A-Za-z0-9_-]{4,}\]?\s*$", "", s)
    s = "".join(c for c in s if 32 <= ord(c) < 127)
    s = re.sub(r"\s+", " ", s).strip(" -|")
    return s or "video"


def safe_filename(title, limit=40):
    """FAT-safe and short: the board's names are 64 bytes (fat.h), and the
    player's list shows ~33 characters."""
    s = re.sub(r'[\\/:*?"<>|]+', " ", title)
    s = re.sub(r"\s+", " ", s).strip(" .")
    if len(s) > limit:
        # At a word boundary: the first version cut "...Chinese Counti".
        cut = s[:limit + 1].rfind(" ")
        s = s[:cut] if cut > limit // 2 else s[:limit]
    return (s.rstrip(" .") or "video")


# ---- probing -----------------------------------------------------------------

def probe(path):
    # Bytes, decoded as UTF-8 explicitly: text=True decodes with the Windows
    # code page (cp1252), and the example video's tags are Chinese -- ffprobe's
    # JSON is UTF-8 whatever the console is. That failed on the first real file.
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-print_format", "json",
         "-show_streams", "-show_format", path],
        capture_output=True)
    if out.returncode:
        die("ffprobe could not read %s: %s" % (
            path, out.stderr.decode("utf-8", "replace").strip()))
    return json.loads(out.stdout.decode("utf-8", "replace"))


def main_video_stream(info):
    """The real video, not an embedded cover: an attached picture is also a
    'video' stream (the example .mkv carries a 1280x720 JPEG as stream 6)."""
    for s in info["streams"]:
        if s.get("codec_type") == "video" and not s.get("disposition", {}).get("attached_pic"):
            return s
    return None


def attached_pic_stream(info):
    for s in info["streams"]:
        if s.get("codec_type") == "video" and s.get("disposition", {}).get("attached_pic"):
            return s
    return None


def has_audio(info):
    return any(s.get("codec_type") == "audio" for s in info["streams"])


def display_size(vs):
    """Width x height as shown, honouring a non-square sample aspect ratio."""
    w, h = int(vs["width"]), int(vs["height"])
    sar = vs.get("sample_aspect_ratio", "1:1")
    try:
        n, d = (int(x) for x in sar.split(":"))
        if n > 0 and d > 0:
            w = w * n // d
    except ValueError:
        pass
    return w, h


def fit(src_w, src_h, max_w, max_h):
    """Largest even size inside max_w x max_h keeping the aspect ratio."""
    w = max_w
    h = src_h * w // src_w
    if h > max_h:
        h = max_h
        w = src_w * h // src_h
    return max(2, w // 2 * 2), max(2, h // 2 * 2)


# ---- ffmpeg helpers -------------------------------------------------------------

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode:
        die("ffmpeg failed:\n  " + " ".join(cmd) + "\n" +
            r.stderr.decode("utf-8", "replace")[-2000:])
    return r


def clip_args(a):
    c = []
    if a.start:
        c += ["-ss", str(a.start)]
    if a.duration:
        c += ["-t", str(a.duration)]
    return c


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def image_to_rgb565(src, stream_spec, width):
    """Any picture ffmpeg can read -> (w, h, RGB565 little-endian bytes)."""
    cmd = ["ffmpeg", "-v", "error", "-i", src]
    if stream_spec:
        cmd += ["-map", stream_spec]
    cmd += ["-frames:v", "1", "-vf", "scale=%d:-2:flags=lanczos" % width,
            "-pix_fmt", "rgb565le", "-f", "rawvideo", "-"]
    data = run(cmd).stdout
    # The height is whatever -2 made it; recover it from the byte count.
    if not data or len(data) % (width * 2):
        die("cover conversion returned %d bytes" % len(data))
    return width, len(data) // (width * 2), data


# ---- the front of the file: header, palette, cover, icon ------------------------
#
# One place lays it out, used by both a fresh conversion and --set-cover, so
# the two cannot disagree about where anything is.

def front_layout(cover, icon):
    """(cover_off, icon_off, first_chunk) for these pictures (either may be
    None)."""
    off = 2 * SECTOR
    cover_off = off if cover else 0
    if cover:
        off += round_up(cover[0] * cover[1] * 2, SECTOR)
    icon_off = off if icon else 0
    if icon:
        off += round_up(icon[0] * icon[1] * 2, SECTOR)
    return cover_off, icon_off, off


def front_bytes(hdr, icon_off, icon, palette, cover):
    s0 = hdr + ICON.pack(icon_off, icon[0] if icon else 0, icon[1] if icon else 0)
    b = bytearray(s0 + bytes(SECTOR - len(s0)))
    b += palette + bytes(SECTOR - len(palette))
    for img in (cover, icon):
        if img:
            b += img[2] + bytes(round_up(len(img[2]), SECTOR) - len(img[2]))
    return bytes(b)


# ---- convert -------------------------------------------------------------------

def find_cover(a, info):
    """--cover, else a picture beside the video with the same name (what
    yt-dlp writes: the example has 'X.mkv' and 'X.jpg'), else the picture
    embedded in the video, else nothing."""
    if a.no_cover:
        return None, None
    if a.cover:
        return a.cover, None
    stem = os.path.splitext(a.input)[0]
    for ext in (".jpg", ".jpeg", ".png", ".webp"):
        if os.path.exists(stem + ext):
            return stem + ext, None
    ap = attached_pic_stream(info)
    if ap is not None:
        return a.input, "0:%d" % ap["index"]
    return None, None


def convert(a):
    need_tools()
    info = probe(a.input)
    vs = main_video_stream(info)
    if vs is None:
        die("no video stream in %s" % a.input)

    src_w, src_h = display_size(vs)
    if a.rotate:
        src_w, src_h = src_h, src_w
    max_w = a.width or PANEL_W
    out_w, out_h = fit(src_w, src_h, min(max_w, PANEL_W), PANEL_H)
    fps_num, fps_den = a.fps, 1
    pix = PIX_PAL8 if a.format == "pal8" else PIX_RGB565
    frame_bytes = out_w * out_h * (1 if pix == PIX_PAL8 else 2)

    audio = AUD_U8_MONO if (not a.no_audio and has_audio(info)) else AUD_NONE
    rate = a.audio_rate if audio else 0
    aud_max = (rate * fps_den + fps_num - 1) // fps_num + 1 if audio else 0
    stride = round_up(16 + frame_bytes + aud_max, SECTOR)

    title = ascii_title(a.title or os.path.splitext(os.path.basename(a.input))[0])
    out = a.output or os.path.join(os.path.dirname(os.path.abspath(a.input)),
                                   safe_filename(title) + ".nvd")

    vf = []
    if a.rotate:
        vf.append("transpose=1")
    vf.append("fps=%d/%d" % (fps_num, fps_den))
    vf.append("scale=%d:%d:flags=lanczos" % (out_w, out_h))
    vf = ",".join(vf)

    print("vidconv: %s" % a.input)
    print("  source   %dx%d %s, %.1f s" % (int(vs["width"]), int(vs["height"]),
                                           vs.get("codec_name", "?"),
                                           float(info["format"].get("duration", 0))))
    print("  output   %dx%d %s @ %d fps%s, audio %s" % (
        out_w, out_h, a.format, fps_num, " (rotated)" if a.rotate else "",
        ("%d Hz u8 mono" % rate) if audio else "none"))

    tmp = tempfile.mkdtemp(prefix="vidconv-")
    try:
        # Pass 1 (PAL8): one palette for the whole video, from every frame.
        # A per-frame palette would shimmer; one global palette costs the
        # board a single 512-byte load.
        pal_png = os.path.join(tmp, "pal.png")
        if pix == PIX_PAL8:
            run(["ffmpeg", "-v", "error"] + clip_args(a) + ["-i", a.input,
                 "-map", "0:%d" % vs["index"],
                 "-vf", vf + ",palettegen=max_colors=256:stats_mode=full",
                 "-y", pal_png])

        # Audio, all of it: 8-bit mono is ~22 KB a second -- a ten-minute
        # video is 13 MB, which a PC holds without noticing.
        pcm = b""
        if audio:
            pcm = run(["ffmpeg", "-v", "error"] + clip_args(a) + ["-i", a.input,
                       "-map", "0:a:0", "-ac", "1", "-ar", str(rate),
                       "-f", "u8", "-"]).stdout

        cover_src, cover_map = find_cover(a, info)
        cover = icon = None
        if cover_src:
            cover = image_to_rgb565(cover_src, cover_map, a.cover_width)
            icon = image_to_rgb565(cover_src, cover_map, a.icon_width)

        # Pass 2: frames, streamed, so memory does not grow with length.
        cmd = ["ffmpeg", "-v", "error"] + clip_args(a) + ["-i", a.input]
        if pix == PIX_PAL8:
            cmd += ["-i", pal_png, "-filter_complex",
                    "[0:%d]%s[v];[v][1:v]paletteuse=dither=bayer:bayer_scale=3" % (vs["index"], vf),
                    "-pix_fmt", "pal8"]
        else:
            cmd += ["-map", "0:%d" % vs["index"], "-vf", vf, "-pix_fmt", "rgb565le"]
        cmd += ["-f", "rawvideo", "-"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        # ffmpeg's raw PAL8 is the indices FOLLOWED by a 1,024-byte palette,
        # every frame -- measured, not assumed (2 frames of 16x8 = 2,304 B).
        per_read = frame_bytes + (1024 if pix == PIX_PAL8 else 0)

        cover_off, icon_off, first = front_layout(cover, icon)

        palette = bytes(512)
        frames = 0
        pos = 0                                 # audio samples written so far
        with open(out + ".part", "wb") as f:
            f.write(bytes(first))               # header, palette, cover: later
            while True:
                raw = proc.stdout.read(per_read)
                if len(raw) < per_read:
                    if raw:
                        die("ffmpeg ended mid-frame (%d of %d bytes)" % (len(raw), per_read))
                    break
                vid = raw[:frame_bytes]
                if pix == PIX_PAL8 and frames == 0:
                    ents = struct.unpack("<256I", raw[frame_bytes:])
                    palette = struct.pack("<256H", *(
                        rgb565((e >> 16) & 255, (e >> 8) & 255, e & 255) for e in ents))
                # This chunk's audio: [A(i), A(i+1)), from the running total.
                end = ((frames + 1) * rate * fps_den + fps_num // 2) // fps_num if audio else 0
                snd = pcm[pos:end] if audio else b""
                if audio and len(snd) < end - pos:
                    snd += b"\x80" * (end - pos - len(snd))    # silence, 0x80
                pos = end
                chunk = CHUNK_MAGIC + struct.pack("<III", frames, len(snd), 0) + vid + snd
                f.write(chunk + bytes(stride - len(chunk)))
                frames += 1
            proc.wait()
            if proc.returncode:
                die("ffmpeg (frames) failed: " +
                    proc.stderr.read().decode("utf-8", "replace")[-2000:])
            if frames == 0:
                die("no frames came out")

            hdr = HEADER.pack(
                MAGIC, VERSION, 2, out_w, out_h, fps_num, fps_den, pix,
                1 if a.rotate else 0, 0, frames, frame_bytes, rate, audio,
                aud_max, stride, first, cover_off,
                cover[0] if cover else 0, cover[1] if cover else 0,
                frames * 1000 * fps_den // fps_num,
                title.encode("ascii")[:127])
            f.seek(0)
            f.write(front_bytes(hdr, icon_off, icon, palette, cover))
        os.replace(out + ".part", out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    size = os.path.getsize(out)
    kbs = stride * fps_num / fps_den / 1024
    print("  wrote    %s" % out)
    print("  frames   %d x %d B chunks  (%.1f s), %s" % (
        frames, stride, frames * fps_den / fps_num,
        "cover %dx%d, icon %dx%d" % (cover[0], cover[1], icon[0], icon[1])
        if cover else "no cover"))
    print("  size     %.1f MB" % (size / 1048576))
    print("  the board must read %.0f KB/s to keep up" % kbs)
    # docs/next_moves/11 step 3: SPI3 at 10 MHz read 554 KB/s of the reading
    # task's own CPU time. A file near that is a file the board cannot play.
    if kbs > 400:
        print("  WARNING: that is near or over what the SD bus was measured to "
              "read (554 KB/s of CPU time at 10 MHz). Try --fps lower, "
              "--width smaller, or pal8.")
    return out


# ---- check / preview --------------------------------------------------------------

def read_header(path):
    with open(path, "rb") as f:
        h = f.read(2 * SECTOR)
    if len(h) < 2 * SECTOR or h[:4] != MAGIC:
        die("%s is not a .nvd file" % path)
    v = HEADER.unpack(h[:HEADER.size])
    keys = ("magic version hsec w h fps_num fps_den pix rotated res frames "
            "frame_bytes rate afmt aud_max stride first cover_off cover_w "
            "cover_h duration_ms title").split()
    d = dict(zip(keys, v))
    d["title"] = d["title"].rstrip(b"\0").decode("ascii", "replace")
    d["icon_off"], d["icon_w"], d["icon_h"] = ICON.unpack(h[ICON_AT:ICON_AT + ICON.size])
    d["palette"] = struct.unpack("<256H", h[SECTOR:SECTOR + 512])
    d["raw_header"] = h[:HEADER.size]
    d["raw_palette"] = h[SECTOR:SECTOR + 512]
    return d


def check(path):
    """Everything the board will rely on, checked here first: sizes that must
    agree, every chunk's magic and index, and the audio arithmetic."""
    d = read_header(path)
    bpp = 1 if d["pix"] == PIX_PAL8 else 2
    errs = []
    if d["version"] != VERSION:
        errs.append("version %d, expected %d" % (d["version"], VERSION))
    if d["frame_bytes"] != d["w"] * d["h"] * bpp:
        errs.append("frame_bytes %d != %d x %d x %d" % (d["frame_bytes"], d["w"], d["h"], bpp))
    if d["stride"] % SECTOR or d["first"] % SECTOR:
        errs.append("stride/first not sector aligned")
    if d["stride"] < 16 + d["frame_bytes"] + d["aud_max"]:
        errs.append("stride too small for a frame and its audio")
    # Pictures must sit between the palette and the first chunk, and not
    # overlap: the board seeks straight to them.
    pics = []
    for name, off, w, h in (("cover", d["cover_off"], d["cover_w"], d["cover_h"]),
                            ("icon", d["icon_off"], d["icon_w"], d["icon_h"])):
        if off:
            end = off + w * h * 2
            if off % SECTOR or off < 2 * SECTOR or end > d["first"] or not w or not h:
                errs.append("%s %dx%d at %d does not fit before the chunks" % (name, w, h, off))
            pics.append((off, end, name))
    pics.sort()
    for (o1, e1, n1), (o2, e2, n2) in zip(pics, pics[1:]):
        if e1 > o2:
            errs.append("%s overlaps %s" % (n1, n2))
    size = os.path.getsize(path)
    want = d["first"] + d["frames"] * d["stride"]
    if size != want:
        errs.append("file is %d bytes, header implies %d" % (size, want))
    audio_total = 0
    with open(path, "rb") as f:
        for i in range(d["frames"]):
            f.seek(d["first"] + i * d["stride"])
            ch = f.read(16)
            if ch[:4] != CHUNK_MAGIC:
                errs.append("chunk %d: bad magic" % i)
                break
            idx, ab, _ = struct.unpack("<III", ch[4:])
            if idx != i:
                errs.append("chunk %d: says it is %d" % (i, idx))
                break
            if ab > d["aud_max"]:
                errs.append("chunk %d: %d audio bytes > max %d" % (i, ab, d["aud_max"]))
                break
            audio_total += ab
    if d["rate"]:
        want_a = (d["frames"] * d["rate"] * d["fps_den"] + d["fps_num"] // 2) // d["fps_num"]
        if audio_total != want_a:
            errs.append("audio %d samples, expected %d" % (audio_total, want_a))

    print("%s" % path)
    print("  '%s'" % d["title"])
    print("  %dx%d %s @ %d/%d fps%s, %d frames = %.1f s" % (
        d["w"], d["h"], {1: "pal8", 2: "rgb565"}.get(d["pix"], "?"),
        d["fps_num"], d["fps_den"], " rotated" if d["rotated"] else "",
        d["frames"], d["duration_ms"] / 1000))
    print("  audio %s, %d samples total" % (
        ("%d Hz u8 mono" % d["rate"]) if d["afmt"] else "none", audio_total))
    print("  chunks %d B from offset %d; cover %s; icon %s" % (
        d["stride"], d["first"],
        ("%dx%d at %d" % (d["cover_w"], d["cover_h"], d["cover_off"])) if d["cover_off"] else "none",
        ("%dx%d at %d" % (d["icon_w"], d["icon_h"], d["icon_off"])) if d["icon_off"] else "none"))
    print("  the board must read %.0f KB/s" % (d["stride"] * d["fps_num"] / d["fps_den"] / 1024))
    if errs:
        for e in errs:
            print("  ERROR: " + e)
        return 1
    print("  OK -- every chunk present, in order, sizes and audio agree")
    return 0


def to_png(w, h, rgb, out):
    run(["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", "%dx%d" % (w, h), "-i", "-", "-y", out], input=bytes(rgb))


def rgb24_from565(words):
    out = bytearray()
    for p in words:
        r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
        out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return out


def preview(a):
    """Rebuild a frame the way the board will -- palette lookup or raw RGB565
    -- and write a PNG, so a file can be looked at before it is ever copied."""
    d = read_header(a.preview)
    with open(a.preview, "rb") as f:
        if a.cover_only or a.icon_only:
            k = "icon" if a.icon_only else "cover"
            if not d[k + "_off"]:
                die("no %s in this file" % k)
            f.seek(d[k + "_off"])
            n = d[k + "_w"] * d[k + "_h"]
            words = struct.unpack("<%dH" % n, f.read(2 * n))
            to_png(d[k + "_w"], d[k + "_h"], rgb24_from565(words), a.out)
        else:
            if not 0 <= a.frame < d["frames"]:
                die("frame %d out of range (0..%d)" % (a.frame, d["frames"] - 1))
            f.seek(d["first"] + a.frame * d["stride"] + 16)
            raw = f.read(d["frame_bytes"])
            if d["pix"] == PIX_PAL8:
                words = [d["palette"][i] for i in raw]
            else:
                words = struct.unpack("<%dH" % (d["w"] * d["h"]), raw)
            to_png(d["w"], d["h"], rgb24_from565(words), a.out)
    print("vidconv: wrote %s" % a.out)


def set_cover(picture, path, cover_width, icon_width):
    """Replaces a file's cover and icon from any picture, without the source
    video: the frames are copied across byte for byte, only the front of the
    file is rebuilt. Built so a thumbnail can be changed after the original
    video has been deleted -- which the user's example already has been."""
    need_tools()
    d = read_header(path)
    cover = image_to_rgb565(picture, None, cover_width)
    icon = image_to_rgb565(picture, None, icon_width)
    cover_off, icon_off, first = front_layout(cover, icon)

    # The header with only the fields that moved changed: cover and first.
    h = bytearray(d["raw_header"])
    struct.pack_into("<I", h, 44, first)
    struct.pack_into("<IHH", h, 48, cover_off, cover[0], cover[1])
    front = front_bytes(bytes(h), icon_off, icon, d["raw_palette"], cover)

    tmp = path + ".part"
    with open(path, "rb") as src, open(tmp, "wb") as dst:
        dst.write(front)
        src.seek(d["first"])
        while True:
            block = src.read(1 << 20)
            if not block:
                break
            dst.write(block)
    os.replace(tmp, path)
    print("vidconv: %s -- cover %dx%d, icon %dx%d from %s" % (
        path, cover[0], cover[1], icon[0], icon[1], picture))
    return check(path)


# ---- self-test ---------------------------------------------------------------------

def selftest():
    """Converts a generated clip -- colour bars plus a 1 kHz tone, with an
    embedded cover -- and checks what came out. Covers both pixel formats."""
    need_tools()
    tmp = tempfile.mkdtemp(prefix="vidconv-test-")
    try:
        src = os.path.join(tmp, "test [abcdefghijk].mkv")
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
             "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100",
             "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
             "-shortest", "-y", src])
        fails = 0
        for fmt in ("pal8", "rgb565"):
            ns = argparse.Namespace(
                input=src, output=os.path.join(tmp, "t-%s.nvd" % fmt), width=240,
                fps=10, format=fmt, rotate=False, audio_rate=22050, no_audio=False,
                cover=None, no_cover=True, cover_width=120, icon_width=64,
                start=None, duration=None, title=None)
            out = convert(ns)
            d = read_header(out)
            ok = (d["w"], d["h"], d["frames"]) == (240, 134, 20)
            print("  selftest %s: %dx%d, %d frames -> %s" % (
                fmt, d["w"], d["h"], d["frames"], "ok" if ok else "WRONG (want 240x134, 20)"))
            fails += (not ok) + check(out)

        # A cover beside the video (what yt-dlp writes), then replaced with a
        # different picture: the frames must come through untouched.
        pic1 = os.path.splitext(src)[0] + ".jpg"
        pic2 = os.path.join(tmp, "other.png")
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=red:size=1280x720",
             "-frames:v", "1", "-y", pic1])
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=blue:size=640x360",
             "-frames:v", "1", "-y", pic2])
        ns = argparse.Namespace(
            input=src, output=os.path.join(tmp, "t-cover.nvd"), width=240, fps=10,
            format="pal8", rotate=False, audio_rate=22050, no_audio=False,
            cover=None, no_cover=False, cover_width=120, icon_width=64,
            start=None, duration=None, title=None)
        out = convert(ns)
        d = read_header(out)
        with open(out, "rb") as f:
            f.seek(d["first"])
            frames_before = f.read()
        ok = (d["cover_w"], d["cover_h"], d["icon_w"], d["icon_h"]) == (120, 68, 64, 36)
        fails += (not ok) + check(out)
        fails += set_cover(pic2, out, 120, 64)
        d2 = read_header(out)
        with open(out, "rb") as f:
            f.seek(d2["icon_off"])
            px = struct.unpack("<H", f.read(2))[0]
            f.seek(d2["first"])
            frames_after = f.read()
        blue = (px & 0x1F) > 25 and (px >> 11) < 4          # RGB565: blue high, red low
        same = frames_before == frames_after
        print("  selftest cover: 120x68 + 64x36 %s; after --set-cover icon is blue %s; "
              "frames untouched %s" % ("ok" if ok else "WRONG", "ok" if blue else "WRONG",
                                       "ok" if same else "WRONG"))
        fails += (not blue) + (not same)
        print("selftest:", "PASS" if not fails else "FAIL")
        return 1 if fails else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    # File names here can be Chinese; a cp1252 console must not turn printing
    # one into a crash.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", nargs="?")
    ap.add_argument("-o", "--output")
    ap.add_argument("--width", type=int, default=PANEL_W,
                    help="max output width, default the panel's 240")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--format", choices=("pal8", "rgb565"), default="pal8",
                    help="pal8: 1 byte/pixel + one 256-colour palette (default); "
                         "rgb565: 2 bytes/pixel, exact colour, twice the data")
    ap.add_argument("--rotate", action="store_true",
                    help="turn a landscape video sideways to fill the portrait panel")
    ap.add_argument("--audio-rate", type=int, default=22050,
                    help="Hz; the board's DAC cannot go below ~19,600")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--cover", help="picture to use as the cover")
    ap.add_argument("--no-cover", action="store_true")
    ap.add_argument("--cover-width", type=int, default=120)
    ap.add_argument("--icon-width", type=int, default=64,
                    help="the list thumbnail; 64 gives 64x36 for 16:9")
    ap.add_argument("--set-cover", nargs=2, metavar=("PICTURE", "NVD"),
                    help="replace an existing file's cover and icon")
    ap.add_argument("--icon-only", action="store_true")
    ap.add_argument("--start", type=float, help="seconds into the source")
    ap.add_argument("--duration", type=float, help="seconds to convert")
    ap.add_argument("--title", help="shown on the board; default from the file name")
    ap.add_argument("--check", metavar="NVD")
    ap.add_argument("--preview", metavar="NVD")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--cover-only", action="store_true")
    ap.add_argument("--out", default="preview.png")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if a.set_cover:
        return set_cover(a.set_cover[0], a.set_cover[1], a.cover_width, a.icon_width)
    if a.check:
        return check(a.check)
    if a.preview:
        need_tools()
        preview(a)
        return 0
    if not a.input:
        ap.error("give a video to convert, or --check / --preview / --selftest")
    if a.audio_rate < 19600 and not a.no_audio:
        die("--audio-rate below 19,600 Hz cannot be played by the board's DAC "
            "(kernel/pcm.h, PCM_RATE_MIN)")
    out = convert(a)
    return check(out)


if __name__ == "__main__":
    sys.exit(main())
