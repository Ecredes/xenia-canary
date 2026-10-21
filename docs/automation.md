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
| `streams` | The event streams `subscribe` accepts: `telemetry`, `dialogs`, `input`. |
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
