#include <string.h>
#include <WiFi.h>

#include "udp-appender.hpp"

namespace esp32m
{

    UDPAppender::UDPAppender(const char *ipaddr, uint16_t port) : _fd(-1)
    {
        memset(&_addr, 0, sizeof(_addr));
        _addr.sin_family = AF_INET;
        _addr.sin_port = htons(port);
        inet_aton(ipaddr, &_addr.sin_addr.s_addr);
    }

    UDPAppender::~UDPAppender()
    {
        if (_fd >= 0)
        {
            shutdown(_fd, 2);
            close(_fd);
            _fd = -1;
        }
    }

    bool UDPAppender::append(const char *message)
    {
        if (!WiFi.isConnected())
            return false;
        static char eol = '\n';
        if (_fd < 0)
        {
            struct timeval send_timeout = {1, 0};
            _fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (_fd >= 0)
                setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));
            else
                return false;
        }
        auto len = strlen(message);
        while (len)
        {
            auto result = sendto(_fd, message, len, 0, (struct sockaddr *)&_addr, sizeof(_addr));
            if (result < 0)
                return false;
            len -= result;
            message += result;
        }
        return sendto(_fd, &eol, sizeof(eol), 0, (struct sockaddr *)&_addr, sizeof(_addr)) == sizeof(eol);
    }

} // namespace esp32m