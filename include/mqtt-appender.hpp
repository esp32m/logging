#pragma once

#include <mqtt_client.h>

#include "logging.hpp"

namespace esp32m
{

  class MQTTAppender : public LogAppender
  {
  public:
    MQTTAppender(const MQTTAppender &) = delete;
    MQTTAppender(const char *topic) : _topic(topic) {}
    void init(esp_mqtt_client_handle_t handle) { _handle = handle; }

  protected:
    virtual bool append(const char *message);

  private:
    const char *_topic;
    esp_mqtt_client_handle_t _handle = nullptr;
  };
  
} // namespace esp32m