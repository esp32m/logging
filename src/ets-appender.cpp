#include <string.h>

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
                ets_write_char_uart(message[i]);
            ets_write_char_uart('\n');
        }

        return true;
    }

}