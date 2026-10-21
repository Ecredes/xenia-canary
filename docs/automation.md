# Automation channel

Xenia can listen for one automation client and any number of observers on a
loopback TCP port, so a tool can watch and control a run without a window:
on a headless server, in CI, or beside a person playing.

It is **off by default**. With `--automation_port` unset or 0 nothing listens.

## Settings

Both are command line only; neither is saved to the config.

| Option | Default | Meaning |
|---|---|---|
| `--automation_port` | `0` (off) | TCP port to listen on, on 127.0.0.1 only. |
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
| `streams` | The event streams `subscribe` accepts: `telemetry`. |
| `role` | `controller` for the first client while no other has it, otherwise `observer`. |
| `state` | As the `state` op returns it. |

At most 8 connections are open at once.

## Requests

| op | Role | Reply |
|---|---|---|
| `hello` | any | as above |
| `state` | any | `state`: `title_open`, and with a title, `title_id` (8 hex digits) and `title` |
| `subscribe` {`streams`} | any | `streams`: the streams now subscribed. `streams` replaces the subscription; `[]` ends it. |
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
