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
build passed with IDF5.5.2. App size2478144 bytes, fits the original OTA partition.
Hardware acceptance is separate and pending at PR creation.

Install only the reviewed application at existing OTA1 offset0x310000. Verify
the connected C3 identity, the current app and a fresh protected-header backup
before flashing; preserve NVS, bootloader, partition table, OTA metadata, OTA0
and the unchanged native wake assets. Previous 2.4.2-footmate.1 application is
the app-only rollback target. Never flash the attached S3 microphone.
