"""Host regression tests: compile the production player with small hardware fakes.

This verifies notification lifecycle decisions, not I2S, a network transport, or
physical audibility. No ESP-IDF toolchain or network access is required.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

FAKES = {
    "freertos/FreeRTOS.h": r'''
#pragma once
#include <cstdint>
using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskHandle_t = void*;
constexpr int pdPASS = 1;
''',
    "freertos/task.h": r'''
#pragma once
#include "FreeRTOS.h"
inline void (*pending_task)(void*) = nullptr;
inline void* pending_arg = nullptr;
inline bool create_task_ok = true;
inline int xTaskCreate(void (*fn)(void*), const char*, uint32_t, void* arg, unsigned, void** handle) {
    if (!create_task_ok) return 0;
    pending_task = fn; pending_arg = arg; *handle = arg; return pdPASS;
}
inline void vTaskDelete(void*) {}
inline void RunWorker() { auto task = pending_task; pending_task = nullptr; task(pending_arg); }
''',
    "esp_timer.h": r'''
#pragma once
#include <cstdint>
inline int64_t test_now_us = 1000;
inline int64_t esp_timer_get_time() { return test_now_us; }
''',
    "esp_log.h": r'''
#pragma once
inline void TestLog(const char*, const char*, ...) {}
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
''',
    "audio_service.h": r'''
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
struct AudioStreamPacket {
    int sample_rate = 0, frame_duration = 0;
    uint32_t playback_id = 0, media_position_ms = 0;
    std::vector<uint8_t> payload;
};
class AudioService {
public:
    bool accept = true;
    std::vector<std::unique_ptr<AudioStreamPacket>> packets;
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool) {
        if (!accept) return false;
        packets.push_back(std::move(packet)); return true;
    }
};
''',
    "http.h": r'''
#pragma once
#include <string>
struct TestError { std::string ToString() const { return "injected failure"; } };
template <typename T> struct Result {
    T value; bool ok = true;
    explicit operator bool() const { return ok; }
    T operator*() const { return value; }
    TestError error() const { return {}; }
};
inline bool http_ok = true;
class Http {
    bool sent = false;
public:
    void SetTimeout(int) {}
    void SetHeader(const char*, const char*) {}
    Result<int> Open(const char*, const std::string&) { return {1, http_ok}; }
    Result<int> GetStatusCode() { return {200}; }
    Result<int> Read(char* data, size_t) {
        if (sent) return {0}; sent = true; *data = 1; return {1};
    }
    void Close() {}
};
''',
    "board.h": r'''
#pragma once
#include <memory>
#include "http.h"
class Network {
public: std::unique_ptr<Http> CreateHttp(int) { return std::make_unique<Http>(); }
};
class Board {
    Network network;
public:
    static Board& GetInstance() { static Board b; return b; }
    Network* GetNetwork() { return &network; }
};
''',
    "ogg_demuxer.h": r'''
#pragma once
#include <functional>
#include <cstdint>
#include <cstddef>
inline bool ogg_complete = true;
class OggDemuxer {
    std::function<void(const uint8_t*, int, int, size_t)> callback;
public:
    void OnPacket(decltype(callback) cb) { callback = cb; }
    void Process(const uint8_t* data, size_t size) { callback(data, 24000, 60, size); }
    bool Finish() { return ogg_complete; }
    bool HasError() { return false; }
};
''',
}

HARNESS = r'''
#include <cassert>
#include <cstring>
#include "notify/notify_delivery.h"
#include "notify/notify_player.h"
#include "esp_timer.h"
#include "http.h"
#include "ogg_demuxer.h"

struct Session {
    AudioService audio;
    NotifyPlayer player{audio};
    int playing = 0, terminal = 0;
    bool success = false;
    bool Start(int64_t deadline = 0) {
        return player.Start("https://example.test/voice.ogg", {}, 7, deadline,
            [this](uint32_t id) { assert(id == 7); ++playing; }, nullptr,
            [this](uint32_t id, bool ok) { assert(id == 7); ++terminal; success = ok; });
    }
};
int main() {
    constexpr int64_t now = 1792256400000LL;
    assert(IsValidNotifyId("risk:20261018-A_1.0"));
    assert(!IsValidNotifyId(""));
    assert(!IsValidNotifyId(std::string(65, 'x')));
    assert(!IsValidNotifyId("a\"\\b"));
    assert(!ValidateNotifyWindow(now, now - 1000, 30000, true));
    assert(!strcmp(ValidateNotifyWindow(now, now - 30000, 30000, true), "expired"));
    assert(!strcmp(ValidateNotifyWindow(now, now, 30000, false), "clock_unsynced"));
    assert(!strcmp(ValidateNotifyWindow(now, now + 5001, 30000, true), "future_timestamp"));
    assert(!strcmp(ValidateNotifyWindow(now, now, 600001, true), "invalid_expiry"));
    assert(!strcmp(ValidateNotifyWindow(now, now, 0, true), "invalid_expiry"));
    NotifyReceiptCache cache;
    for (int i = 0; i < 5; ++i) cache.Save({std::to_string(i), "completed", ""});
    assert(!cache.Find("0"));
    assert(!cache.Find(""));
    assert(cache.Find("1") && cache.Find("4"));
    assert(!strcmp(cache.Find("4")->state, "completed"));

    { // HTTP admission and queued bytes are NOT playback.
        Session s; assert(s.Start()); assert(s.playing == 0 && s.terminal == 0);
        assert(!s.Start());  // no second worker / notification queue
        RunWorker(); assert(s.audio.packets.size() == 1);
        assert(s.playing == 0 && s.terminal == 0);
        assert(s.player.CanPlay(7)); assert(!s.player.CanPlay(8));
        s.player.OnPlaybackProgress(7, 0);
        s.player.OnPlaybackProgress(7, 60);
        assert(s.playing == 1 && s.terminal == 0);
        s.player.OnPlaybackDrained();
        assert(s.terminal == 1 && s.success);
        s.player.OnPlaybackDrained(); assert(s.terminal == 1);
    }
    { // A drain without a decoded/output frame cannot become completed.
        Session s; assert(s.Start()); RunWorker(); s.player.OnPlaybackDrained();
        assert(s.terminal == 1 && !s.success && s.playing == 0);
    }
    { // A decode error wins over later drain/HTTP completion.
        Session s; assert(s.Start()); RunWorker(); s.player.OnPlaybackError(7);
        s.player.OnPlaybackDrained();
        assert(s.terminal == 1 && !s.success && !s.player.CanPlay(7));
    }
    { // A stale callback cannot cancel the current notification.
        Session s; assert(s.Start()); s.player.OnPlaybackError(6);
        assert(s.terminal == 0 && s.player.CanPlay(7)); s.player.Stop(); RunWorker();
        assert(s.terminal == 0 && !s.player.CanPlay(7));
    }
    { // Cancel while HTTP work is pending: no audio, no false completion.
        Session s; assert(s.Start()); s.player.Stop(); RunWorker();
        assert(s.audio.packets.empty() && s.terminal == 0 && s.playing == 0);
    }
    { // Deadline applies to queued frames, not just message admission.
        test_now_us = 1000; Session s; assert(s.Start(5000)); RunWorker();
        assert(s.player.CanPlay(7)); test_now_us = 5000;
        assert(!s.player.CanPlay(7)); s.player.Stop();
    }
    { // Expiry before download finishes rejects production.
        test_now_us = 1000; Session s; assert(s.Start(5000)); test_now_us = 5000;
        RunWorker(); assert(s.audio.packets.empty() && s.terminal == 1 && !s.success);
    }
    test_now_us = 1000;
    { http_ok = false; Session s; assert(s.Start()); RunWorker();
      assert(s.terminal == 1 && !s.success); http_ok = true; }
    { ogg_complete = false; Session s; assert(s.Start()); RunWorker();
      assert(s.terminal == 1 && !s.success); ogg_complete = true; }
    { Session s; s.audio.accept = false; assert(s.Start()); RunWorker();
      assert(s.terminal == 1 && !s.success); }
    { create_task_ok = false; Session s; assert(!s.Start());
      assert(!s.player.IsBusy()); create_task_ok = true; }
}
'''


class NotifyDeliveryTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "g++ required for production C++ host test")
    def test_production_player_and_delivery_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            for name, content in FAKES.items():
                path = base / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            (base / "test.cc").write_text(HARNESS)
            command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Wno-misleading-indentation", "-I", str(base), "-I", str(ROOT / "main"),
                       str(ROOT / "main/notify/notify_player.cc"), str(base / "test.cc"),
                       "-o", str(base / "notify_test")]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(base / "notify_test")], check=True, timeout=10)

class NotifyProtocolTests(unittest.TestCase):
    def test_shared_receipt_serialization_and_control_send_failure(self):
        import ctypes.util
        import json
        library = ctypes.util.find_library("cjson")
        if not shutil.which("g++") or not library:
            self.skipTest("g++ and host libcjson required for shared Protocol test")
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            # Only public ABI declarations; linked against the real host cJSON library.
            (base / "cJSON.h").write_text(r'''
#pragma once
#include <stddef.h>
extern "C" {
struct cJSON;
cJSON* cJSON_CreateObject(void);
cJSON* cJSON_GetObjectItem(const cJSON*, const char*);
int cJSON_IsObject(const cJSON*);
cJSON* cJSON_AddStringToObject(cJSON*, const char*, const char*);
cJSON* cJSON_AddBoolToObject(cJSON*, const char*, int);
cJSON* cJSON_AddNumberToObject(cJSON*, const char*, double);
int cJSON_AddItemToObject(cJSON*, const char*, cJSON*);
char* cJSON_PrintUnformatted(const cJSON*);
void cJSON_free(void*);
void cJSON_Delete(cJSON*);
}
''')
            (base / "esp_log.h").write_text(FAKES["esp_log.h"])
            (base / "assets.h").write_text(r'''
#pragma once
#include <string>
struct Capability { bool glyph_push = false; std::string bundle, charset; int size = 0, bpp = 0; };
class Assets {
public:
    static Assets& GetInstance() { static Assets a; return a; }
    Capability text_font_capability() const { return {}; }
};
''')
            (base / "protocol_test.cc").write_text(r'''
#include <cassert>
#include <iostream>
#include "protocol.h"
class Control : public Protocol {
public:
    int opened = 0, writes = 0;
    bool connected = true;
    bool Start() override { return true; }
    bool OpenAudioChannel() override { ++opened; return true; }
    void CloseAudioChannel(bool) override {}
    bool IsAudioChannelOpened() const override { return false; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket>) override { assert(false); return false; }
    bool SendText(const std::string& text) override {
        ++writes;
        if (connected) std::cout << text << '\n';
        return connected;
    }
};
int main() {
    Control control;
    assert(control.SendNotifyStatus("test-1", "received"));
    assert(control.SendNotifyStatus("test-1", "playing"));
    assert(control.SendNotifyStatus("test-1", "completed"));
    assert(control.SendNotifyStatus("test-2", "failed", "expired"));
    assert(!control.SendNotifyStatus("", "completed"));
    assert(control.writes == 4 && control.opened == 0);
    control.connected = false;
    assert(!control.SendNotifyStatus("test-3", "received"));
    assert(control.writes == 5 && control.opened == 0);
}
''')
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(base), "-I", str(ROOT / "main/protocols"),
                            str(ROOT / "main/protocols/protocol.cc"), str(base / "protocol_test.cc"),
                            "-l:" + library, "-o", str(base / "protocol_test")],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(base / "protocol_test")], check=True,
                                    capture_output=True, text=True, timeout=10)
            receipts = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual([item["state"] for item in receipts],
                             ["received", "playing", "completed", "failed"])
            self.assertTrue(all(item["type"] == "notify_status" for item in receipts))
            self.assertEqual(receipts[-1]["reason"], "expired")
            self.assertNotIn("reason", receipts[0])


if __name__ == "__main__":
    unittest.main()
