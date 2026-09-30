# Passport standby alarms

This change is based on FoloToy's official `v2.4.2-folo.1` release (commit `d24fce080d86d7cc642f71585f6efde40fb99104`). The stock WebSocket transport opens only during conversations, so the gateway cannot deliver alarms to an idle Passport.

The transport now connects after activation reaches Idle, retains its connection when a conversation closes, and checks every five seconds for reconnection. The gateway's existing incoming heartbeat maintains liveness. Application callbacks share the main task queue so TTS start prepares the decoder before audio frames arrive. An alarm received while idle waits until playback drains before returning to idle, without starting microphone capture. Active button conversations remain available.

Build the official board configuration with ESP-IDF >=5.5.2:

```sh
python scripts/build.py folo/ai-passport-c3 --name folo-ai-passport-c3 --language zh-CN --wake-word disabled
```

Disable wake words for an alarm-only installation so speaker playback cannot wake a new conversation. The button still starts an intentional conversation. Preserve the current device's NVS when installing the resulting firmware, including Wi-Fi credentials, server endpoint and volume settings; never commit those contents.

Real-device acceptance requires: a registered MAC in the gateway; a pushed alarm with received/queued frame counts matching and dropped=0; `Passive TTS playback drained` followed by Idle; no automatic listening after the alarm; continued registration after more than two minutes idle; a real connection interruption followed by reconnection and a successful second alarm. Keep firmware SHA-256, build logs and serial logs together as a repeatable verification artifact.
