# Passport standby alarms

This change is based on FoloToy's official `v2.4.2-folo.1` release (commit `d24fce080d86d7cc642f71585f6efde40fb99104`). The stock WebSocket transport opens only during conversations, so the gateway cannot deliver alarms to an idle Passport.

The transport now connects after activation reaches Idle, retains its connection when a conversation closes, and checks every five seconds for reconnection. The gateway's existing incoming heartbeat maintains liveness. Application callbacks share the main task queue so TTS start prepares the decoder before audio frames arrive. An alarm received while idle waits until playback drains before returning to idle, without starting microphone capture. Active button conversations remain available.

Build the official board configuration with ESP-IDF >=5.5.2:

```sh
python scripts/build.py folo/ai-passport-c3 --name folo-ai-passport-c3 --language zh-CN --wake-word disabled
```

Disable wake words for an alarm-only installation so speaker playback cannot wake a new conversation. The button still starts an intentional conversation. Preserve the current device's NVS when installing the resulting firmware, including Wi-Fi credentials, server endpoint and volume settings; never commit those contents.

## Idle backlight

The Passport turns off its backlight after 30 seconds idle with audio playback drained. Announcements and active device states restore the user's saved brightness; returning to idle starts a new 30-second interval. All three buttons wake the display and restart the interval while retaining their existing volume and conversation actions. This policy applies with USB or battery power because the board has no charging-state detection.

Turning the backlight off does not stop the CPU, Wi-Fi or standby WebSocket, and does not clear announcement history. The temporary zero brightness is not saved to NVS. Real-device acceptance must observe the backlight switching off, a new announcement waking it, and another shutdown after playback; serial backlight commands support the check but do not alone prove the physical screen state or battery runtime.

## Announcement screen

The Passport board profile uses the existing multi-message layout. Announcements use the main screen area, wrap across lines, and remain after playback finishes. Once the content exceeds the screen height, the viewport scrolls upward to show the newest text. Emoji overlays are hidden; network, status and battery indicators remain at the top.

The display retains up to 20 messages and 8 KiB of text in memory, removing the oldest entries when either limit is reached. A single message larger than the text budget retains its latest complete UTF-8 characters. History resets when the device reboots. Empty messages and temporary system notifications do not erase announcement history. On boards without PSRAM, the history layout retains server-provided dynamic glyphs across messages within a 64-glyph / 8 KiB bitmap cache; rare glyphs may be evicted when that budget is full.

Screen acceptance must use the real device: send two distinct numbered announcements in the same capture window, include a message longer than the viewport, and check retained text, wrapping, upward scrolling and absence of emoji after returning to idle. Serial events can verify message delivery and layout state, but visual acceptance requires an actual screen image; a reconstructed mock UI is not a substitute.

Real-device acceptance requires: a registered MAC in the gateway; a pushed alarm with received/queued frame counts matching and dropped=0; `Passive TTS playback drained` followed by Idle; no automatic listening after the alarm; continued registration after more than two minutes idle; a real connection interruption followed by reconnection and a successful second alarm. Keep firmware SHA-256, build logs and serial logs together as a repeatable verification artifact.
