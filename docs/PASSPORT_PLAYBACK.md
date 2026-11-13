# Passport playback jitter fix

Baseline: 7df5ec4b1e7a44a02e3caa8dab62535d514d3976 (2.4.2-footmate.1).
The previous physical voice trial dropped six compressed TTS packets when the
20-packet decode queue filled. Transport completion alone did not prove smooth
speaker output.

Version 2.4.2-footmate.2 allows 40 compressed packets, with an independent 8192-byte
payload limit. The PCM queue remains two frames and the main loop remains
nonblocking. A 16 kHz question sample encoded to mean117/max148 bytes per packet;
20 additional packets used2725 bytes. This is a bounded jitter allowance, not
a guarantee against long Wi-Fi outages.

Conversation TTS stop now reports queued/dropped totals. The output task warns
when consecutive codec writes in one playback generation finish more than120ms
apart. New/reset playback generations do not produce false inter-turn gap reports.

Failure modes considered before implementation: overflow, RAM pressure, main-loop
blocking, hidden starvation, abort/reset, and accidental writes to identity or
recovery data. Physical acceptance must require zero dropped packets and inspect
gap warnings alongside the independent S3 microphone capture.

Validation: existing eight host checks passed; canonical C3 zh-CN nihaoxiaozhi
build passed with IDF5.5.2. App size2469952 bytes, fits the original OTA partition.
Hardware acceptance is separate and pending at PR creation.

Install only the reviewed application at existing OTA1 offset0x310000. Verify
the connected C3 identity, the current app and a fresh protected-header backup
before flashing; preserve NVS, bootloader, partition table, OTA metadata, OTA0
and the unchanged native wake assets. Previous 2.4.2-footmate.1 application is
the app-only rollback target. Never flash the attached S3 microphone.

## Follow-up diagnosis before implementation
The first footmate.2 physical trial still dropped34/129 packets; output gaps
were360ms,5129ms,6710ms. A second turn stalled before TTS stop. Cloud receipts
showed129 and158 frames sent. The Mac public-path probe also saw1.67s packet
gaps; encrypted origin forwarding saw0.81s gaps, so neither comparison proves
the C3 scheduler is innocent or the public edge is the sole cause.
Distinguish: (1) delayed WebSocket arrival, (2) main-loop callback starvation,
(3) decode/output starvation. Add timestamp-only RX and schedule-gap warnings
before tuning buffers or changing transport. Do not log payloads or tokens.
Failure risks: inter-turn false positives, callback thread races and logging
per frame causing its own starvation. Only gaps over120ms are reported; reset
RX timing on a new TTS start. Application state stays scheduled on main.

Follow-up validation:8 existing host checks and canonical C3 build passed. App2470496 bytes; SHA256 ca96f9456199a9d7f1a86f642928addadb48a9e85dd5c31308e57a90c6ea82b3. Physical diagnosis pending.
