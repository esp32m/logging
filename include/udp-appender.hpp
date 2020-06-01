#pragma once

#include <lwip/sockets.h>

#include "logging.hpp"

namespace esp32m
{

    class UDPAppender : public LogAppender
    {
    public:
        enum Format
        {
            Text,
            Syslog
        };
        UDPAppender(const char *ipaddr=nullptr, uint16_t port = 514);
        UDPAppender(const UDPAppender &) = delete;
        ~UDPAppender();
        Format format() { return _format; }
        void setMode(Format format) { _format = format; }

    protected:
        virtual bool append(const LogMessage *message);

    private:
        Format _format;

        struct sockaddr_in _addr;
        int _fd;
    };

} // namespace esp32m