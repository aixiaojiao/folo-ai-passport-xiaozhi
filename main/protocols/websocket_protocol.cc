#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <freertos/semphr.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                auto* protocol = static_cast<WebsocketProtocol*>(arg);
                auto alive = protocol->alive_;
                if (!*alive || protocol->reconnect_pending_.exchange(true)) {
                    return;
                }
                Application::GetInstance().Schedule([protocol, alive]() {
                    if (!*alive) {
                        return;
                    }
                    protocol->reconnect_pending_ = false;
                    protocol->CheckConnection();
                });
            },
        .arg = this,
        .name = "ws_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer_));
}

WebsocketProtocol::~WebsocketProtocol() {
    {
        std::lock_guard<std::recursive_mutex> lock(*callback_mutex_);
        *alive_ = false;
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_delete(reconnect_timer_);
    // Timer deletion is deferred and does not join an already running callback.
    // A task-dispatched fence keeps `this` alive until earlier timer callbacks finish.
    auto fence_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(fence_done ? ESP_OK : ESP_ERR_NO_MEM);
    esp_timer_handle_t fence_timer;
    esp_timer_create_args_t fence_args = {
        .callback = [](void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); },
        .arg = fence_done,
        .name = "ws_deinit",
    };
    ESP_ERROR_CHECK(esp_timer_create(&fence_args, &fence_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(fence_timer, 1));
    xSemaphoreTake(fence_done, portMAX_DELAY);
    esp_timer_delete(fence_timer);
    vSemaphoreDelete(fence_done);
    ResetTransport();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    if (!esp_timer_is_active(reconnect_timer_)) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(reconnect_timer_, 5000000));
    }
    return Connect(false);
}

void WebsocketProtocol::CheckConnection() {
    if (IsAudioChannelOpened()) {
        return;
    }
    ResetTransport();
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "Restoring standby websocket connection");
        Connect(false);
    }
}

void WebsocketProtocol::ResetTransport() {
    bool was_ready;
    {
        std::lock_guard<std::recursive_mutex> lock(*callback_mutex_);
        was_ready = ready_.exchange(false);
        if (channel_alive_) {
            *channel_alive_ = false;
        }
    }
    // Never destroy TCP while holding the callback lock: Disconnect may join its RX task.
    websocket_.reset();
    if (was_ready && *alive_ && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool WebsocketProtocol::IsTimeout() const {
    const auto now = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    return now - last_received_seconds_.load() > 120;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return ready_.load() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    if (send_goodbye && IsAudioChannelOpened()) {
        // stop finalizes ASR; closing a conversation must cancel it instead.
        SendAbortSpeaking(kAbortReasonNone);
    }
    // The gateway needs this transport to deliver alarms while the device is idle.
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool WebsocketProtocol::OpenAudioChannel() { return Connect(true); }

bool WebsocketProtocol::Connect(bool report_error) {
    if (IsAudioChannelOpened()) {
        return true;
    }
    ResetTransport();
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;
    if (url.empty()) {
        ESP_LOGW(TAG, "Websocket endpoint is not configured");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    channel_alive_ = std::make_shared<std::atomic<bool>>(true);
    auto channel_alive = channel_alive_;
    auto alive = alive_;
    auto callback_mutex = callback_mutex_;
    websocket_->OnData([this, alive, channel_alive, callback_mutex](const char* data, size_t len,
                                                                    bool binary) {
        std::lock_guard<std::recursive_mutex> lock(*callback_mutex);
        if (!*alive || !*channel_alive) {
            return;
        }
        last_received_seconds_ = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    if (len < sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Short v2 audio frame");
                        return;
                    }
                    const auto* bp2 = reinterpret_cast<const BinaryProtocol2*>(data);
                    const auto payload_size = ntohl(bp2->payload_size);
                    if (payload_size > len - sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Invalid v2 audio payload size");
                        return;
                    }
                    auto payload = bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = ntohl(bp2->timestamp),
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)}));
                } else if (version_ == 3) {
                    if (len < sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Short v3 audio frame");
                        return;
                    }
                    const auto* bp3 = reinterpret_cast<const BinaryProtocol3*>(data);
                    const auto payload_size = ntohs(bp3->payload_size);
                    if (payload_size > len - sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Invalid v3 audio payload size");
                        return;
                    }
                    auto payload = bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)}));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)}));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
    });

    websocket_->OnDisconnected([this, alive, channel_alive, callback_mutex]() {
        std::lock_guard<std::recursive_mutex> lock(*callback_mutex);
        if (!*alive || !*channel_alive) {
            return;
        }
        ready_ = false;
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        ResetTransport();
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!websocket_->Send(message)) {
        ResetTransport();
        if (report_error) {
            SetError(Lang::Strings::SERVER_ERROR);
        }
        return false;
    }

    // Wait for server hello
    EventBits_t bits =
        xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(10000));
    bool connected;
    {
        std::lock_guard<std::recursive_mutex> lock(*callback_mutex_);
        connected = (bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT) && websocket_->IsConnected();
        if (connected) {
            ready_ = true;
        }
    }
    if (!connected) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        ResetTransport();
        if (report_error) {
            SetError(Lang::Strings::SERVER_TIMEOUT);
        }
        return false;
    }

    ESP_LOGI(TAG, "Standby websocket ready");
    if (on_connected_) {
        on_connected_();
    }
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported websocket transport");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
