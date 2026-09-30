#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "protocol.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>
#include <atomic>
#include <memory>
#include <mutex>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> channel_alive_;
    std::shared_ptr<std::recursive_mutex> callback_mutex_ =
        std::make_shared<std::recursive_mutex>();
    std::atomic<bool> ready_ = false;
    std::atomic<bool> reconnect_pending_ = false;
    std::atomic<uint32_t> last_received_seconds_ = 0;

    bool Connect(bool report_error);
    void CheckConnection();
    void ResetTransport();
    bool IsTimeout() const override;
    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
