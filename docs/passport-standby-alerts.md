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

## Battery level

The Passport reads the CW2017 fuel gauge on the existing shared I2C0 bus (SDA GPIO10, SCL GPIO7, 7-bit address `0x63`). These assignments and the SOC register format follow FoloToy's [official hardware guide](https://github.com/FoloToy/ai-passport/blob/main/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) and [battery driver](https://github.com/FoloToy/ai-passport/blob/main/components/bsp/src/bsp_battery.c). The [product specification](https://github.com/FoloToy/ai-passport/blob/main/docs/hardware-design/specifications.md) identifies a built-in 520 mAh rechargeable lithium battery; GPIO0 is the button ladder, not a battery ADC input.

Cold boot makes one bounded initialization attempt before the display, audio, or network starts. An already active gauge with its profile update flag set is preserved without mode resets or profile replacement. On the connected device's observed VERSION `0xA0`, a sleeping gauge is awakened using the official restart/active sequence. If its update flag is absent, initialization enters sleep, writes the exact 80-byte UTL 520 mAh profile supplied by the official BSP, verifies every byte, then sets and verifies the update flag while preserving its threshold bits. Mode changes are read back and SOC settling waits at most five seconds. Unverified versions or unknown modes are not modified. A failed initialization does not reset or repeatedly reinitialize the gauge in the running application; a normal reboot permits a new attempt.

Subsequent refreshes only read an active gauge with its profile update flag set and integer SOC within 0–100. This can recover a settling SOC without another reset; it never derives a percentage from voltage. A valid reading describes the gauge's estimate, not independently calibrated accuracy or measured runtime. The observed VERSION is a safety boundary for this connected device, not a vendor-documented universal chip ID.

The top bar shows a numeric percentage beside the existing battery icon. Reads are cached for ten seconds; initialization, unavailable hardware, an inactive or unconfigured gauge, invalid SOC, and I2C errors show `--%` instead of an invented or stale percentage. The board has no verified charging-state interface, so USB connectivity or changing SOC must not be presented as proof of charging or discharging. This feature does not issue charging notifications or infer low-battery audio warnings from an unknown discharge state.

Battery refresh does not wake the backlight or restart the idle timer. Standby WebSocket delivery, announcement playback and history, the fixed status band, buttons, saved brightness, and NVS preservation retain their existing behavior. Acceptance must separately record real gauge responses, displayed values and failure handling; building successfully does not prove SOC accuracy or charging detection.

## Announcement screen

The Passport board profile uses the existing multi-message layout. Announcements use the main screen area, wrap across lines, and remain after playback finishes. Once the content exceeds the screen height, the viewport scrolls upward to show the newest text. Emoji overlays are hidden; network, status and battery indicators remain at the top.

A fixed status band above the announcement history remains visible even with no messages. It displays the current device activity, Wi-Fi connection, alarm connection and monotonic running time. Alarm connectivity requires a live WebSocket with the server hello completed, and becomes unavailable after disconnect, error or receive timeout; Wi-Fi connectivity alone is insufficient. This indicator describes the device's current gateway connection, rather than guaranteeing delivery of every future alert. Running time updates every second without waking the backlight or restarting its idle timeout. Temporary notifications and scrolling history cannot replace this band.

The display retains up to 20 messages and 8 KiB of text in memory, removing the oldest entries when either limit is reached. A single message larger than the text budget retains its latest complete UTF-8 characters. History resets when the device reboots. Empty messages and temporary system notifications do not erase announcement history. On boards without PSRAM, the history layout retains server-provided dynamic glyphs across messages within a 64-glyph / 8 KiB bitmap cache; rare glyphs may be evicted when that budget is full.

Screen acceptance must use the real device: send two distinct numbered announcements in the same capture window, include a message longer than the viewport, and check retained text, wrapping, upward scrolling and absence of emoji after returning to idle. Serial events can verify message delivery and layout state, but visual acceptance requires an actual screen image; a reconstructed mock UI is not a substitute.

Real-device acceptance requires: a registered MAC in the gateway; a pushed alarm with received/queued frame counts matching and dropped=0; `Passive TTS playback drained` followed by Idle; no automatic listening after the alarm; continued registration after more than two minutes idle; a real connection interruption followed by reconnection and a successful second alarm. Keep firmware SHA-256, build logs and serial logs together as a repeatable verification artifact.
