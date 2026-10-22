# Automation channel

Xenia can listen for one automation client and any number of observers on a
loopback TCP port, so a tool can watch and control a run without a window:
on a headless server, in CI, or beside a person playing.

It is **off by default**. With `--automation_port` unset or 0 nothing listens.

## Settings

All are command line only; none is saved to the config.

| Option | Default | Meaning |
|---|---|---|
| `--automation_port` | `0` (off) | TCP port to listen on, on 127.0.0.1 only. |
| `--automation_dialog_timeout` | `30` | Seconds the controller has to answer a dialog before its default answers it. |
| `--automation_token` | empty | The token clients must send. When empty, Xenia makes a random one and writes it to `automation.token` in the storage root, readable only by the user where the platform allows. |

Example:

```
xenia_canary --headless --automation_port=9300 path/to/title.iso
```

The channel never listens on other addresses. To reach a server, tunnel:
`ssh -L 9300:127.0.0.1:9300 server`, then connect to 127.0.0.1:9300 locally.

## Framing

One JSON object per line, UTF-8, `\n` terminated (`\r\n` is accepted). A line
over 16 MB drops the client.

- Requests carry `op` and optionally `id` (a string or a number).
- Each request gets exactly one reply. A reply carries `re` (the request's
  `id`, when it had one) and `ok`; when `ok` is false, `error` says why.
- Events carry `type` and no `re`.

## Connecting

The first request must be `hello`:

```
{"op":"hello","id":1,"protocol":1,"token":"..."}
```

Anything else first, an unreadable line, a different `protocol`, or a wrong
token is answered with an error and the connection is closed. A good hello is
answered with:

| Field | Meaning |
|---|---|
| `protocol` | The protocol version, currently 1. |
| `xenia` | The build: `branch`, `commit`, `date`. |
| `capabilities` | The ops this build supports, beyond `hello`. |
| `streams` | The event streams `subscribe` accepts: `telemetry`, `dialogs`, `input`, `audio`. Frames come from `frames_watch` instead. |
| `role` | `controller` for the first client while no other has it, otherwise `observer`. |
| `state` | As the `state` op returns it. |

At most 8 connections are open at once.

## Requests

| op | Role | Reply |
|---|---|---|
| `hello` | any | as above |
| `state` | any | `state`: `title_open`, and with a title, `title_id` (8 hex digits) and `title` |
| `subscribe` {`streams`} | any | `streams`: the streams now subscribed. `streams` replaces the subscription; `[]` ends it. |
| `dialog_answer` {`dialog`, `answer`} | controller | `ok` when the answer fits the open dialog, which then closes; otherwise an error, and it stays open |
| `pad_connect` {`user`, `connected`} | controller | `ok`; plugs pad `user` (0 to 3) in or out |
| `pad_set` {`user`, `pad`} | controller | `ok`; holds `pad` until changed |
| `pad_queue` {`user`, `input`, `frames`, `pad`} | controller | `ok`, or `queue full` (256); holds `pad` for `frames` vblanks (1 to 3600) after what is queued |
| `pad_clear` {`user`} | controller | `ok`; drops the queued states |
| `frame` {`format`, `max_width`, `quality`} | any | the guest output now, as below, or `no frame yet` before the title has drawn |
| `frames_watch` {`fps`, `format`, `max_width`, `quality`} | any | `ok`; then `frame` events at up to `fps` a second (1 to 60), replacing any earlier watch; `fps` 0 ends it |
| `video_start` {`port`, `width`, `height`, `fps`} | any | `ok` with `pix_fmt` and `ns`, when frame 0 is due; raw video to the listener on loopback `port`, as below; `video is already streaming`, or `nothing listens on 127.0.0.1:port` |
| `video_stop` | the client that started it | `ok` with `frames`, `captures` and `dropped`, or `no video streaming` |
| `bench_start` | any | `ok`; starts gathering frame times for this client, restarting any bench it had |
| `bench_stop` {`raw`} | any | the bench's summary, as below, or `no bench running` |
| `pause` | controller | `ok` with `paused`, `vblank` and `swap`; the emulator pauses, as below |
| `resume` | controller | `ok` with `paused`, `vblank` and `swap`; the emulator runs again |
| `advance` {`vblanks`} | controller | when the emulator is paused: runs it for `vblanks` vblanks (1 to 3600), pauses it again, then `ok` with `paused`, `vblank` and `swap`; `not paused` otherwise |
| `stop` | controller | `ok`; then every client gets an `exit` event and the process ends |

An unknown op is answered with an error; the connection stays open.

## Events

| type | When | Fields |
|---|---|---|
| `exit` | the process is about to end at the controller's request | `status`: its exit status |

### Telemetry

With the channel on, telemetry runs even without `--telemetry_file`. A client
subscribed to `telemetry` gets its lines exactly as
[telemetry.md](telemetry.md) describes them, each with the envelope (`type`,
`vblank`, `swap`, `ns`, `state`):

- First the session's `session_start` line, however late the client
  subscribes, then every line written from then on.
- Lines reach clients within about 150 ms of being written.
- A client more than 16 MB behind is disconnected rather than buffered for.

### Dialogs

When the title asks something a player would answer in a dialog, a headless
run asks the controller, if it is subscribed to `dialogs`; otherwise the
default answers at once, as without the channel. Windowed runs show their
dialogs to the person as before.

- A `dialog` event opens it: `dialog` (its id), `kind`, the request's fields,
  `default` (the answer given if the controller doesn't), and `timeout_ms`.
  The guest waits meanwhile, as behind a real dialog, so `state` is
  `system_ui`.
- The controller answers with `dialog_answer`, `answer` being an object as
  below. An answer a real dialog wouldn't allow gets an error and the dialog
  stays open.
- A `dialog_closed` event closes it, with `by`: `client`, `timeout` (the
  default answered), or `policy` (no subscribed controller any more, or the
  run is ending).
- A client subscribing to `dialogs` gets the events of the dialogs already
  open.

| kind | Request fields | answer |
|---|---|---|
| `choice` | `title`, `text`, `buttons` | `{"button": n}`, an index into `buttons`; can't be cancelled |
| `text` | `title`, `description`, `default_text`, `max_length` | `{"text": "..."}`, at most `max_length` bytes, or `{"cancel": true}` |
| `sign_in` | `users_needed`, `last_used_slot`, `signed_in` (profiles in slots) | `{"profiles": [{"slot": 0, "xuid": "E0..."}]}`, slots 0 to 3, at most `users_needed`, or `{"cancel": true}` |
| `passcode` | `title`, `description`, `length` | `{"keys": [...]}`, `length` key values, or `{"cancel": true}` |
| `disc` | `prompt` | `{"path": "..."}`, a disc image on the host, or `{"cancel": true}` |

XUIDs are 16 hex digits, as strings.

### Controller input

With the channel on, the guest also sees the four pads the controller drives,
before any real controller, in windowed and headless runs. A pad counts once
`pad_connect` plugs it in; when the controller disconnects, every pad is
unplugged.

`pad` is an object with the X_INPUT_GAMEPAD fields, each 0 when left out:
`buttons` (0 to 65535, the XINPUT_GAMEPAD button bits), `lt` and `rt` (0 to
255), `lx`, `ly`, `rx` and `ry` (-32768 to 32767).

A queued state starts on the vblank after it is queued, or when the one before
it ends, so each lasts whole vblanks. A client subscribed to `input` gets, for
each queued state, `user`, `input` (its number from `pad_queue`) and
`at_vblank`:

| type | When |
|---|---|
| `pad_begin` | it became visible to the guest |
| `pad_seen` | the guest first read it |
| `pad_end` | it stopped being visible |

### Pause and advance

`pause` is Xenia's own emulator pause, the one its debugger uses, not the
title's pause menu: guest threads are suspended where they are, the GPU and
audio stop, and nothing is saved (it is not a save state). While paused:

- guest time stands still: the guest clock gains nothing, so on `resume` the
  title doesn't see the pause as elapsed time;
- no vblanks are delivered, and queued pad states wait;
- frames, the video stream and telemetry still work: the picture is the last
  one the title drew, and telemetry's state is `paused`.

`advance` runs a paused emulator for an exact number of vblanks, then pauses
it again: the vblank thread holds the next vblank back at the target, so the
guest gets exactly `vblanks` of them, and it may still finish the work of the
last one before the pause lands, a few milliseconds later. If pausing takes
longer than 100 ms (it can wait on work that needs a vblank), the held vblank
is delivered after all; the reply's `vblank` is the count the pause landed at.
Queue pad states first to hold them over the advance. Advancing is exact in
guest vblanks, not deterministic: guest threads are still scheduled by the
host, so the same inputs from the same point need not give the same result.

The pause belongs to the controller: when it disconnects or stops the run,
an advance under way is dropped and the emulator resumes. While an advance is
under way, `pause`, `resume` and another `advance` are refused.

With `clock_no_scaling`, guest time is host time and keeps running while
paused.

### Audio

A client subscribed to `audio` gets the sound the title submits, as `audio`
events: `client` (the guest's audio client, 0 to 7; most titles use one),
`source` (`game`, or `music` below), `rate` (48000), `channels` (2), `frames`,
and `data`, base64 little-endian 16-bit stereo, interleaved. The envelope's `ns`, `vblank` and `swap` are when the
first frame was submitted.

- The guest's 5.1 frames are downmixed: L = FL + 0.707 FC + 0.707 BL and
  R = FR + 0.707 FC + 0.707 BR, clamped; the LFE channel is left out.
- An event holds about 100 ms of contiguous samples. A client that pauses
  more than 50 ms starts a new event, so placing each event at its `ns` keeps
  the gaps; one that stops is sent within 200 ms.
- The tap sees frames as the title submits them, before the host's audio
  driver, so headless runs without a sound device have audio too.
- Music the title plays through the system media player (XMP) comes as
  `client` 8 with `source` `music`, at the playback volume: downmixed the
  same way (mono to both sides) and resampled linearly from the song's rate,
  with `ns` when it plays: the media player queues about a second ahead, so
  each frame is stamped where the one before it ends (or now, when the queue
  ran dry). Mix it with the other clients by `ns`.
- With no subscriber, the tap does nothing.

### Frames

`frame` and `frames_watch` capture the guest output: the image the window
would show, before window scaling and overlays, in windowed and headless runs.
Capture and encoding run on a thread of their own; with no request or watch,
nothing is captured.

- `format`: `jpeg` (the default) or `png`. `quality`: JPEG quality, 1 to 100,
  default 85. `max_width`: scale down to at most this width keeping the aspect
  ratio, averaging pixels; 0 or left out keeps the full size.
- The image is in `data`, base64, with `format`, `width` and `height`. A
  `frame` reply also carries `vblank`, `swap` and `ns`; a `frame` event has
  them in its envelope. They are the guest's counters and the host time when
  the frame was captured, not when it was sent: the image is that swap's, or
  rarely the next one's.
- A watch sends a frame only when the guest has swapped since its last one, on
  a steady `fps` grid. A client with over 1 MB unsent skips frames rather than
  queueing them, so a slow client sees fresh frames, not a growing backlog.
- Frames are for seeing the run and timing its reactions (match a frame's
  `vblank` to a pad event's `at_vblank`). For frame pacing use a bench, or
  telemetry's `frame_stats`: a capture is a GPU readback.

### Video

`video_start` streams the guest output as raw video for an encoder, so a
recording keeps the game's own frame rate without encoding images on the CPU.
Xenia connects to a listener on `127.0.0.1:port` and sends it bare frames,
nothing else: `width` x `height` (even, 16 to 8192) `rgb24`, top row first,
`fps` (1 to 60) a second. One stream runs at a time.

- Each frame is the guest output scaled to fit and centered, with black bars on
  the other axis. Before the title draws, frames are black.
- The rate is constant. A tick the guest hasn't swapped since repeats the last
  frame, so a 30 fps title at `fps` 60 shows each image twice and the stream's
  time stays the guest's. Ticks missed while sending are repeated; over half a
  second behind, they are dropped and counted instead, and the stream shortens.
- Each new image comes with a `video_frame` event to the client that started
  the stream: `n`, the stream frame it starts at, and `capture_ms`, with the
  capture's `vblank`, `swap` and `ns` in the envelope. Frames between two
  events repeat the first one.
- When the listener closes or reads nothing for 10 seconds, the stream ends
  with a `video_end` event (`reason`, `frames`, `captures`, `dropped`). It also
  ends when the client that started it disconnects.

With ffmpeg as the listener, encoding on the GPU through VA-API:

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -f rawvideo -pix_fmt rgb24 \
  -s 1280x720 -framerate 60 -i "tcp://127.0.0.1:5000?listen=1" \
  -vf format=bgr0,hwupload,scale_vaapi=format=nv12 -c:v h264_vaapi out.mp4
```

then `{"op":"video_start","port":5000,"width":1280,"height":720,"fps":60}`.
On Windows, `-c:v h264_amf`, `h264_nvenc` or `h264_qsv` take the frames
directly. A capture costs a few milliseconds of a host thread per new image.

### Benches

A bench summarizes frame pacing between `bench_start` and `bench_stop` the way
a frame time review would. It gathers the `frame_stats` telemetry written in
between (see [telemetry.md](telemetry.md)), whole windows of
`--telemetry_window` vblanks, so bench for several seconds. Each client has its
own bench; no stream subscription is needed.

| Field | Meaning |
|---|---|
| `windows` | `frame_stats` windows gathered |
| `seconds`, `vblank_hz` | host time from the first window's end to the last's, and the guest vblanks per host second over it: 60 at full speed, lower when the emulator can't keep up. Left out with fewer than 2 windows |
| `swaps` | the guest's frame times (swap intervals), summarized as below |
| `stutters` | swap intervals at least twice the median |
| `stutters_with_compiles` | of those, the ones with a shader or pipeline compile during them |
| `compiles` | shader and pipeline compiles during the swap intervals |
| `presents` | the host presents' frame times, summarized as below; only when the run presents (windowed) |
| `present_latency_p50_ms`, `present_latency_p99_ms` | from a guest image's refresh to its host present call's return; only when the run presents |
| `swap_ms`, `swap_compiles`, `present_ms`, `present_latency_ms` | with `raw`, the samples themselves |

A summary has `frames` (intervals), `avg_fps` (frames over their total time),
`low_1_fps` and `low_0_1_fps` (1000 over the mean of the slowest 1% and 0.1%
of intervals, at least one), `p50_ms`, `p95_ms` and `p99_ms` (nearest-rank
percentiles) and `max_ms`; fps and ms to 2 decimals.

Swaps are the title's own frames, what it renders; presents are what Xenia
shows on the host, which drops a guest image when a newer one is ready first.
Input-to-screen latency can be measured from the events: a pad event's
`at_vblank` (`pad_begin`, then `pad_seen` when the guest read it), then the
first watched `frame` whose image changes, by its `vblank`. Presents are timed
at the present call; the display's own latency is not measured.
