#pragma once

#include <lwip/sockets.h>

#include "logging.hpp"

namespace esp32m
{

    class UDPAppender : public LogAppender
    {
    public:
        UDPAppender(const char *ipaddr, uint16_t port);
        UDPAppender(const UDPAppender &) = delete;
        ~UDPAppender();

    protected:
        virtual bool append(const char *message);

    private:
        struct sockaddr_in _addr;
        int _fd;
    };

} // namespace esp32m