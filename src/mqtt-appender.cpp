#include "mqtt-appender.hpp"

namespace esp32m
{
    bool MQTTAppender::append(const char *message)
    {
        if (!_handle)
            return false;
        if (!message)
            return true;
        auto messageIdOrErrorCode = esp_mqtt_client_publish(_handle, _topic, message, strlen(message), 0, false);
        // Strict positive value indicates a message id. 
        // - when QoS==0, message id is always 0 as per esp-idf's esp_mqtt_client_publish
        // Strict negative value indicates an error. 
        if (messageIdOrErrorCode >= 0) {return true;}
        return false;
    }
    
} // namespace esp32m