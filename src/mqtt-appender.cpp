#include "mqtt-appender.hpp"

namespace esp32m
{
    bool MQTTAppender::append(const char *message)
    {
        if (!_handle)
            return false;
        if (!message)
            return true;
        return esp_mqtt_client_publish(_handle, _topic, message, strlen(message), 0, false) != 0;
    }
    
} // namespace esp32m