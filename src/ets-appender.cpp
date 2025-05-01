#include <string.h>
#include "platform-uart.hpp"
#include "ets-appender.hpp"

namespace esp32m {

    ETSAppender &ETSAppender::instance()
    {
        static ETSAppender i;
        return i;
    }

    bool ETSAppender::append(const char *message)
    {
        if (message)
        {
            auto l = strlen(message);
            for (auto i = 0; i < l; i++)
                platform_write_char_uart(message[i]);
            platform_write_char_uart('\n');
        }

        return true;
    }

}