#ifndef ESP32M_LOGGING_ETS_APPENDER_UART_FUNC
#include "rom/uart.h"
    const auto platform_write_char_uart = ets_write_char_uart;
#else
    const auto platform_write_char_uart = ESP32M_LOGGING_ETS_APPENDER_UART_FUNC;
#endif
