# Asynchronous Voice Notifications

The `notify` message lets the cloud play a one-way voice notification while a device is idle. It does not open a conversation audio channel and never enables microphone uplink. Starting a conversation still requires an explicit wake action from the user.

## Message format

The cloud sends the following JSON through the device's current protocol control connection. For an
MQTT device, this is the existing MQTT control topic:

```json
{
  "type": "notify",
  "id": "task-20261018-001",
  "sent_at_ms": 1792256400000,
  "ttl_ms": 30000,
  "audio_url": "https://cdn.example.com/audio/task-complete.ogg",
  "subtitles": [
    {
      "start_ms": 0,
      "text": "Your task is complete."
    },
    {
      "start_ms": 1800,
      "text": "The result has been saved."
    }
  ]
}
```

`audio_url` is required. Both `http://` and `https://` URLs are accepted. HTTP is useful for local-network development, while production deployments can enforce HTTPS when generating the URL.

`subtitles` is optional. Each entry contains the media start time in milliseconds and the text to display. The device sorts entries by `start_ms` and updates the display only when playback crosses a new subtitle entry.

The example timestamp is illustrative; send the **current Unix time in milliseconds**, not the
example value. Delivery remains best effort and applies only to online devices.

### Identity, expiry, and compatibility

- `id` is optional for old senders. New senders should supply a unique ID containing 1–64 ASCII
  letters, digits, `-`, `_`, `.`, or `:`. IDs correlate receipts; reuse an ID only for a retry of
  the same logical notification. Invalid IDs are logged and rejected without a receipt.
- `sent_at_ms` and `ttl_ms` must be supplied together, with an ID. Both are integer milliseconds;
  TTL is 1–600,000 ms. Their sum is the absolute expiry, so transit time consumes the TTL.
- Expiring requests require device time synchronized by the existing OTA server-time exchange.
  An unsynchronized clock fails closed as `clock_unsynced`; a sender timestamp more than five
  seconds in the future fails as `future_timestamp`. Expired requests fail as `expired`.
- The accepted lifetime is converted to a monotonic deadline. The HTTP producer checks it and
  each notification PCM frame is gated before codec output. Expiry cancels remaining playback;
  a frame already handed to the codec cannot be retracted. This is not a hard real-time alarm.
- An old request without these fields retains its previous best-effort behavior and produces no
  correlatable receipt. An ID alone enables receipts but has **no expiry guarantee**.
- There is **no notification delivery queue**: the device remains Idle-only. A distinct request
  while busy returns `busy`, without changing or queueing behind the active notification.
- The active ID and the last four terminal receipts are kept in RAM. Retrying one of these IDs
  returns its current/terminal state without replay. The cache does not survive reboot and is
  not permanent exactly-once delivery; the server must also deduplicate IDs and stop retries at
  expiry. A busy rejection is not cached, so a retry can be admitted after the device is idle.
- Metadata is bounded: URL ≤2,048 bytes, ≤32 subtitle entries, ≤4,096 combined subtitle text
  bytes. The existing bounded audio buffers are unchanged.

### Device receipts

The device sends `notify_status` on the **existing control connection**, through shared
`Protocol::SendNotifyStatus`. MQTT uses its existing publish topic; WebSocket uses its current
connected socket. It does not open a UDP/audio channel or enable microphone upload to report a
receipt. WebSocket currently connects only on demand: this feature does **not** create a new
always-on WebSocket notification connection.

```json
{"type":"notify_status","id":"task-20261018-001","state":"received"}
{"type":"notify_status","id":"task-20261018-001","state":"playing"}
{"type":"notify_status","id":"task-20261018-001","state":"completed"}
```

| State | Actual evidence / meaning |
| --- | --- |
| `received` | Validated and admitted by the idle device; download/playback can still fail. |
| `playing` | The first remote notification PCM frame has been handed to the codec. The built-in popup does not count. |
| `completed` | HTTP/Ogg stream completed, at least one notification frame reached codec output, and all decode/playback queues and in-flight work drained without a reported decode failure or expiry. |
| `busy` | Not admitted; `reason` is `not_idle` or `state_transition`. The current notification continues. |
| `failed` | Rejected or stopped; reasons include `expired`, `clock_unsynced`, `future_timestamp`, invalid input, `start_failed`, `audio_failed`, `protocol_error`, and `network_disconnected`. |
| `cancelled` | A wake/user/lifecycle interruption stopped it (`reason: interrupted`). It did not complete. |

`completed` is firmware playback-pipeline evidence, **not proof that a human heard the message**.
The existing codec API does not expose speaker audibility or a hardware playback acknowledgement.
Decode errors cannot become `completed` merely because the queues drained. Receipts have no
independent delivery guarantee: on a broken control connection they may be lost. A retry with the
same ID can recover one of the four cached terminal states after reconnection; a missing receipt
must remain `unknown`, never inferred as completion from a server publish result.

The gateway/server must authenticate the device connection, correlate by **device identity + ID**,
accept duplicate status events idempotently, keep terminal state from being downgraded by a late
`received`/`playing`, and expire unresolved delivery as `unknown`. These gateway changes are
**not included in this firmware repository**. In particular, xz-mqtt and OUYU must explicitly
forward/store `notify_status` before an end-to-end success metric can be claimed.

## Audio response

`audio_url` must return a successful 2xx response containing a single-stream, mono Ogg Opus file. Responses with either `Content-Length` or chunked transfer encoding are supported. Redirects are not followed by the device.

The file is read incrementally. The device does not allocate memory based on the complete response length and does not download the complete file before playback. Notification streaming uses the existing bounded HTTP response queue, a 2 KB Ogg logical-packet buffer, the shared 20-packet Opus decode queue, and the existing two-frame PCM playback queue. The decode queue represents 1.2 seconds at the firmware's normal 60 ms packet duration. These buffers provide TCP backpressure while keeping the feature usable on devices without PSRAM.

On devices that use the standalone LiteAudioEngine WakeNet, WakeNet resources are released while a notification is playing and recreated when the device returns to `Idle`. AFE-based devices keep their existing local wake behavior.

Opus packet duration is read from the Opus TOC byte instead of being supplied in the MQTT message. Integer packet durations from 5 ms through 120 ms that are supported by the firmware decoder are accepted. The current implementation rejects stereo streams, detected Ogg or Opus structural errors, incomplete streams, oversized logical packets, and 2.5 ms packets.

## Device behavior

The device accepts `notify` only while it is in `Idle`. It then performs the following actions:

1. Enters the internal `Notifying` state and switches the board to performance mode.
2. Disables normal voice processing and microphone uplink.
3. Clears previous playback and queues the built-in popup sound.
4. Starts one HTTP GET in a background task.
5. Incrementally demultiplexes Ogg packets and sends them directly to the existing Opus decode queue.
6. Displays subtitles according to the media position of Opus frames handed to codec output.
7. Returns to `Idle` only after the HTTP stream has ended successfully and all queued audio has played.

The popup is queued before the HTTP task starts, so remote audio cannot play before it. HTTP connection setup still overlaps the actual popup playback.

If all playback queues drain after remote audio has started but before the HTTP stream has finished,
the device logs `Notification playback underrun #<count> at <position> ms`. The popup-to-stream
transition and normal end of playback are not reported as underruns.

The device does not open the UDP audio channel, send `start-listening`, or automatically enter `Listening`. AFE-based devices may continue local wake-word detection during playback. Devices without playback echo cancellation retain the existing speaking-mode wake behavior; hardware wake controls can still cancel a notification.

A wake action cancels the HTTP producer, clears queued notification audio, and continues through the normal wake flow. Network loss, HTTP errors, and invalid audio also cancel playback and return the device to `Idle`. A distinct notification received while the device is busy is rejected as `busy` when it has an ID;
legacy messages remain silently best effort.

## xz-mqtt forwarding

`xz-mqtt` forwards the message with its existing Redis RPC method:

```json
{
  "method": "forward",
  "clientId": "device-client-id",
  "params": {
    "type": "notify",
    "audio_url": "https://cdn.example.com/audio/task-complete.ogg",
    "subtitles": [
      {
        "start_ms": 0,
        "text": "Your task is complete."
      }
    ]
  }
}
```

The RPC result contains the boolean returned by the MQTT send operation:

```json
{
  "success": true
}
```

This result means only **gateway accepted/published**, not `received`, `playing`, or `completed`.
It means that `xz-mqtt` wrote the publish message to the current online device connection.
Any controller/UI must label it as accepted, not played or successfully delivered. The sender must
forward `id`, `sent_at_ms`, and `ttl_ms` unchanged in `params`, and consume device receipts separately.
It does not confirm receipt or playback. `xz-mqtt` does not create a UDP session, start the chat bridge, proxy the Ogg file, or retain notifications for offline devices.


## Validation and release gate

Host regression coverage compiles the production notification player with hardware/network
fakes and exercises successful playback, no-output drain, decode failure, HTTP/Ogg failure,
expiry before download and during queued playback, stale callbacks, cancellation, task failure,
ID validation, the TTL window, bounded duplicate receipt retention, and shared Protocol receipt
serialization using host cJSON (when available). It also checks that receipt transmission failure
is returned and does not open an audio channel:

```sh
python3 -m unittest discover -s scripts/tests -p test_notify_delivery.py -v
```

Before shipping firmware, build with ESP-IDF 6.0.1+ (prefer 6.1), then verify on the AI Passport
ESP32-C3 and a representative AFE-enabled board. Exercise MQTT and an already-connected
WebSocket independently: idle success, busy, duplicate active/terminal ID, expired before arrival,
expiry while HTTP is stalled, malformed Ogg/decoder failure, wake cancellation, disconnect,
reconnect + duplicate-status recovery, and ordinary conversation/wake/microphone behavior.
Check logs plus correlated device receipts and actual speaker playback. A successful host test
is not a firmware build, transport integration, physical test, or verification that the gateway
stores the receipt. No gateway integration, firmware flash, or physical acceptance has been
performed by the P0 source change.
