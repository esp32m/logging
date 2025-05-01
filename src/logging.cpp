#include <malloc.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp32-hal.h>

#include "logging.hpp"
#include "platform-uart.hpp"

namespace esp32m
{

    LogLevel Logging::_level = LogLevel::Debug;
    LogMessageFormatter Logging::_formatter = nullptr;
    LogAppender *_appenders = nullptr;
    SemaphoreHandle_t _loggingLock = xSemaphoreCreateMutex();

    LogMessage *LogMessage::alloc(LogLevel level, int64_t stamp, const char *name, const char *message)
    {
        size_t ml = strlen(message);
        while (ml)
        {
            auto c = message[ml - 1];
            if (c == '\n' || c == '\r')
                ml--;
            else
                break;
        }
        size_t size = sizeof(LogMessage) + ml + 1;
        void *pool = malloc(size);
        return new (pool) LogMessage(size, level, stamp, name, message, ml);
    }

    LogMessage::LogMessage(size_t size, LogLevel level, int64_t stamp, const char *name, const char *message, size_t messageLen)
        : _size(size), _stamp(stamp), _name(name), _level(level)
    {
        strncpy((char *)this->message(), message, messageLen)[messageLen] = '\0';
    }

    Logger &Loggable::logger()
    {
        if (_logger)
            return *_logger;
        xSemaphoreTake(_loggingLock, portMAX_DELAY);
        _logger = std::unique_ptr<Logger>(new Logger(*this));
        xSemaphoreGive(_loggingLock);
        return *_logger;
    }

    class BufferedAppender : public LogAppender
    {
    public:
        BufferedAppender(LogAppender &appender, size_t bufsize, bool autoRelease, uint32_t maxLoopItems)
            : _appender(appender), _autoRelease(autoRelease), _max_loop_item_sent(maxLoopItems)
        {
            _lock = xSemaphoreCreateMutex();
            _handle = xRingbufferCreate(bufsize, RINGBUF_TYPE_NOSPLIT);
            _item_to_be_sent = nullptr;
        }
        ~BufferedAppender()
        {
            release();
        }

    protected:
        bool append(const LogMessage *message)
        {
            if (!_handle) // No Ring Buffer = It has been released by autoRelease option
                return _appender.append(message);

            size_t size;
            bool ok = false;
            bool append_result = false;
            
            // First always add message in ring buffer... always...
            if (message) {
                for (;;) // BE CAREFUL... If this loop takes too much time and useQueue is in use : the WDG of Queue task could never be called...
                {
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    if (xRingbufferSend(_handle, message, message->size(), 0))
                    {
                        // Ok, message added to buffer.
                        xSemaphoreGive(_lock);
                        break;
                    }
                    xSemaphoreGive(_lock);
                    // KO ! : No space left in buffer...
                    
                    if(!_item_to_be_sent) {
                        // Retreive item to be sent from the ring buffer : the older one.
                        xSemaphoreTake(_lock, portMAX_DELAY);
                        _item_to_be_sent = (LogMessage *)xRingbufferReceive(_handle, &size, 0);
                        xSemaphoreGive(_lock);
                    }
                    if (!_item_to_be_sent){
                        // No item in buffer.
                        // Warning : No space left in buffer... message to add to buffer is bigger than the full buffer size !
                        break;
                    }

                    append_result = _appender.append(_item_to_be_sent); // Try to "send" item... last chance before loosing it due to buffer rotation!
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    vRingbufferReturnItem(_handle, _item_to_be_sent); // Here we remove item, even if it has not really been sent ! Free space in buffer...
                    xSemaphoreGive(_lock);
                    _item_to_be_sent = nullptr; // Reset pointer, indicating the item has been removed from Ring buffer
                }
            }
            else {
                // No message... "flush buffer" case, if useQueue autoFlushPeriod is used.
            }

            // Second : try to flush the buffered items (in the FIFO order of course)
            // If not possible, stop and keep the last not sent item in memory (_item_to_be_sent)
            // Limit the loop to _max_loop_item_sent to avoid too long loop in case of useQueue is in use (avoid WDG interrupt/reset)
            uint32_t max_loop_items_counter = _max_loop_item_sent ? _max_loop_item_sent : 0xFFFFFFFF;
            for(; max_loop_items_counter>0; max_loop_items_counter--)
            {
                if(!_item_to_be_sent) {
                    // Retreive item to be sent from the ring buffer
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    _item_to_be_sent = (LogMessage *)xRingbufferReceive(_handle, &size, 0);
                    xSemaphoreGive(_lock);
                    if (!_item_to_be_sent){
                        // No more item in buffer. All items sent.
                        ok = true;
                        break;
                    }
                }
                else {
                    // There is already an item waiting to be sent... (not sent last time)
                }
                append_result = _appender.append(_item_to_be_sent);
                if (!append_result) {
                    // Item not sent ! Keep it in memory for next try... Do not remove it from buffer !
                    // Stop trying to send items for the moment... maybe appender is not ready.
                    break;
                }
                // Ok, item has been sent by appender.
                // Remove it from the buffer.
                xSemaphoreTake(_lock, portMAX_DELAY);
                vRingbufferReturnItem(_handle, _item_to_be_sent);
                xSemaphoreGive(_lock);
                _item_to_be_sent = nullptr; // Reset pointer, indicating the item has been sent.
            }
            if (ok && _autoRelease)
                release();
            return true;
        }
    
    private:
        LogAppender &_appender;
        bool _autoRelease;
        RingbufHandle_t _handle;
        SemaphoreHandle_t _lock;
        LogMessage *_item_to_be_sent;
        uint32_t _max_loop_item_sent;
        void release()
        {
            if (!_handle)
                return;
            xSemaphoreTake(_lock, portMAX_DELAY);
            vRingbufferDelete(_handle);
            xSemaphoreGive(_lock);
            _handle = nullptr;
            vSemaphoreDelete(_lock);
        }
    };

    class LogQueue;
    LogQueue *logQueue = nullptr;

    class LogQueue
    {
    public:
        LogQueue(size_t bufsize, uint32_t autoFlushPeriod)
            : _bufsize(bufsize), _flush_period_ms(autoFlushPeriod)
        {
            _lock = xSemaphoreCreateMutex();
            _buf = xRingbufferCreate(bufsize, RINGBUF_TYPE_NOSPLIT);
            xTaskCreate([](void *self) { ((LogQueue *)self)->run(); }, "esp32m::log-queue", 4096, this, tskIDLE_PRIORITY, &_task);
            logQueue = this;
        }
        ~LogQueue()
        {
            xSemaphoreTake(_lock, portMAX_DELAY);
            vTaskDelete(_task);
            vRingbufferDelete(_buf);
            logQueue = nullptr;
            xSemaphoreGive(_lock);
            vSemaphoreDelete(_lock);
        }
        bool enqueue(const LogMessage *message)
        {
            xSemaphoreTake(_lock, portMAX_DELAY);
            auto result = xRingbufferSend(_buf, message, message->size(), 10);
            xSemaphoreGive(_lock);
            return result;
        }

    private:
        uint32_t _flush_period_ms;
        size_t _bufsize;
        SemaphoreHandle_t _lock;
        RingbufHandle_t _buf;
        TaskHandle_t _task = nullptr;
        friend class Logging;
        void run()
        {
            const TickType_t ticks_timeout = _flush_period_ms ? (TickType_t)(_flush_period_ms/portTICK_PERIOD_MS) : 100;
            esp_task_wdt_add(nullptr);
            for (;;)
            {
                esp_task_wdt_reset();
                size_t size;
                LogMessage *item = (LogMessage *)xRingbufferReceive(_buf, &size, ticks_timeout);
                if (item)
                {
                    LogAppender *appender = _appenders;
                    while (appender)
                    {
                        appender->append(item);
                        appender = appender->_next;
                    }
                    vRingbufferReturnItem(_buf, item);
                }
                else if(_flush_period_ms) {
                    LogAppender *appender = _appenders;
                    while (appender)
                    {
                        //TODO : Loop only on "Buffered" Appenders ?
                        appender->append(nullptr); // nullptr ! Just to "flush" BufferedAppenders
                        appender = appender->_next;
                    }
                }
                else {}
                yield();
            }
        }
    };

    int64_t timeOrUptime()
    {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2016 - 1900))
            return -(int64_t)now * 1000 + (millis() % 1000);
        return esp_timer_get_time() / 1000;
    }

    char *format(const LogMessage *msg)
    {
        static const char *levels = "??EWIDV";
        if (!msg)
            return nullptr;
        auto stamp = msg->stamp();
        char *buf = nullptr;
        auto level = msg->level();
        auto name = msg->name();
        char l = level >= 0 && level < 7 ? levels[level] : '?';
        if (stamp < 0)
        {
            stamp = -stamp;
            char strftime_buf[32];
            time_t now = stamp / 1000;
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%F %T", &timeinfo);
            buf = (char *)malloc(strlen(strftime_buf) + 1 /*dot*/ + 4 /*millis*/ + 1 /*space*/ + 1 /*level*/ + 1 /*space*/ + strlen(name) + 2 /*spaces*/ + msg->message_size() + 1 /*zero*/);
            sprintf(buf, "%s.%04d %c %s  %s", strftime_buf, (int)(stamp % 1000), l, name, msg->message());
        }
        else
        {
            int millis = stamp % 1000;
            stamp /= 1000;
            int seconds = stamp % 60;
            stamp /= 60;
            int minutes = stamp % 60;
            stamp /= 60;
            int hours = stamp % 24;
            int days = stamp / 24;
            buf = (char *)malloc(6 /*days*/ + 1 /*colon*/ + 2 /*hours*/ + 1 /*colon*/ + 2 /*minutes*/ + 1 /*colon*/ + 2 /*seconds*/ + 1 /*colon*/ + 4 /*millis*/ + 1 /*space*/ + 1 /*level*/ + 1 /*space*/ + strlen(name) + 2 /*spaces*/ + msg->message_size() + 1 /*zero*/);
            sprintf(buf, "%d:%02d:%02d:%02d.%04d %c %s  %s", days, hours, minutes, seconds, millis, l, name, msg->message());
        }
        return buf;
    }

    FormattingAppender::FormattingAppender(LogMessageFormatter formatter)
    {
        _formatter = formatter == nullptr ? Logging::formatter() : formatter;
    }

    bool FormattingAppender::append(const LogMessage *message)
    {
        auto str = _formatter(message);
        if (!str)
            return true;
        auto result = this->append(str);
        free(str);
        return result;
    }

    bool isEmpty(const char *s)
    {
        if (!s)
            return true;
        auto l = strlen(s);
        if (!l)
            return true;
        for (int i = 0; i < l; i++)
            if (!isspace(s[i]))
                return false;
        return true;
    }

    void Logger::log(LogLevel level, const char *msg)
    {
        if (isEmpty(msg))
            return;
        auto effectiveLevel = _level;
        if (effectiveLevel == LogLevel::Default)
            effectiveLevel = Logging::level();
        if (level > effectiveLevel)
            return;
        auto name = _loggable.logName();
        LogMessage *message = LogMessage::alloc(level, timeOrUptime(), name, msg);
        if (!message)
            return;
        if (!_appenders)
        {
            auto m = Logging::formatter()(message);
            if (m)
            {
                ets_printf("%s\n", m);
                free(m);
            }
        }
        else
        {
            LogQueue *queue = logQueue;
            if (queue)
                queue->enqueue(message);
            else
            {
                LogAppender *appender = _appenders;
                while (appender)
                {
                    appender->append(message);
                    appender = appender->_next;
                }
            }
        }
        free(message);
    }

    void Logger::logf(LogLevel level, const char *format, ...)
    {
        if (!format)
            return;
        va_list arg;
        va_start(arg, format);
        logf(level, format, arg);
        va_end(arg);
    }

    void Logger::logf(LogLevel level, const char *format, va_list arg)
    {
        if (!format)
            return;
        char buf[64];
        char *temp = buf;
        auto len = vsnprintf(NULL, 0, format, arg);
        if (len >= sizeof(buf))
        {
            temp = (char *)malloc(len + 1);
            if (temp == NULL)
                return;
        }
        vsnprintf(temp, len + 1, format, arg);
        log(level, temp);
        if (len >= sizeof(buf))
            free(temp);
    }

    void Logging::addBufferedAppender(LogAppender *a, int bufsize, bool autoRelease, uint32_t maxLoopItems)
    {
        if (a)
            addAppender(new BufferedAppender(*a, bufsize, autoRelease, maxLoopItems));
    }

    void Logging::addAppender(LogAppender *a)
    {
        if (!a)
            return;
        LogAppender *appender = _appenders;
        if (!appender)
            _appenders = a;
        else
        {
            while (appender->_next)
                appender = appender->_next;
            appender->_next = a;
            a->_prev = appender;
        }
    }

    LogMessageFormatter Logging::formatter()
    {
        return _formatter == nullptr ? format : _formatter;
    }

    void Logging::removeAppender(LogAppender *a)
    {
        if (!a)
            return;
        LogAppender *appender = _appenders;
        if (appender == a)
            _appenders = nullptr;
        else
            while (appender)
            {
                if (appender == a)
                {
                    if (a->_prev)
                        a->_prev->_next = a->_next;
                    if (a->_next)
                        a->_next->_prev = a->_prev;
                    break;
                }
                appender = appender->_next;
            }
    }

    void Logging::useQueue(int size, uint32_t autoFlushPeriod)
    {
        auto q = logQueue;
        if (size)
        {
            if (q)
            {
                if (q->_bufsize == size)
                    return;
                delete q;
            }
            new LogQueue(size, autoFlushPeriod);
        }
        else if (q)
            delete q;
    }

    Logger &Logging::system()
    {
        static SimpleLoggable loggable("system");
        return loggable.logger();
    }

    bool charToLevel(char c, LogLevel &l)
    {
        switch (c)
        {
        case 'I':
            l = LogLevel::Info;
            break;
        case 'W':
            l = LogLevel::Warning;
            break;
        case 'D':
            l = LogLevel::Debug;
            break;
        case 'E':
            l = LogLevel::Error;
            break;
        case 'V':
            l = LogLevel::Verbose;
            break;
        default:
            return false;
        }
        return true;
    }

    LogLevel detectLevel(const char **mptr)
    {
        auto msg = *mptr;
        LogLevel l = LogLevel::None;
        if (msg)
        {
            auto len = strlen(msg);
            char lc = '\0';
            int inc = 0;
            if (len > 4 && msg[0] == '[' && msg[2] == ']')
            {
                lc = msg[1];
                inc = 3;
            }
            else if (len > 2 && msg[1] == ' ')
            {
                lc = msg[0];
                inc = 2;
            }
            if (charToLevel(lc, l))
                (*mptr) += inc;
        }
        if (!l)
            l = LogLevel::Debug;
        return l;
    }

    class Esp32Hook;
    Esp32Hook *_esp32Hook = nullptr;

    class Esp32Hook
    {
    public:
        Esp32Hook()
        {
            _esp32Hook = this;
            _prevLogger = esp_log_set_vprintf(esp32hook);
        }
        ~Esp32Hook()
        {
            esp_log_set_vprintf(_prevLogger);
            _esp32Hook = nullptr;
        }

    private:
        vprintf_like_t _prevLogger = nullptr;
        uint8_t _recursion = 0;
        const char *_pendingName = nullptr;
        LogLevel _pendingLevel = LogLevel::None;
        static int esp32hook(const char *str, va_list arg)
        {
            auto h = _esp32Hook;
            if (!h || h->_recursion)
                return 0;
            h->_recursion++;
            if (h->_pendingName)
            {
                SimpleLoggable log(h->_pendingName);
                log.logger().logf(h->_pendingLevel, str, arg);
                h->_pendingLevel = LogLevel::None;
                h->_pendingName = nullptr;
            }
            else if (!strcmp(str, "%c (%d) %s:"))
            {
                charToLevel((char)va_arg(arg, int), h->_pendingLevel);
                va_arg(arg, long);
                h->_pendingName = va_arg(arg, const char *);
            }
            else
            {
                const char *mptr = str;
                auto level = detectLevel(&mptr);
                Logging::system().logf(level, mptr, arg);
            }
            h->_recursion--;
            return strlen(str);
        }
    };

    void Logging::hookEsp32Logger(bool install)
    {
        auto h = _esp32Hook;
        if (install)
        {
            if (h)
                return;
            new Esp32Hook();
        }
        else
        {
            if (!h)
                return;
            delete h;
        }
    }

    class SerialHook;
    SerialHook *serialHook = nullptr;

    class SerialHook
    {
    public:
        SerialHook(size_t bufsize)
        {
            _lock = xSemaphoreCreateMutex();
            _serialBuf = (char *)malloc(_serialBufLen = bufsize);
            serialHook = this;
            ets_install_putc1(hook);
        }
        ~SerialHook()
        {
            ets_install_putc1(platform_write_char_uart);
            xSemaphoreTake(_lock, portMAX_DELAY);
            free(_serialBuf);
            _serialBufLen = 0;
            _serialBufPtr = 0;
            serialHook = nullptr;
            xSemaphoreGive(_lock);
            vSemaphoreDelete(_lock);
        }

    private:
        static void hook(char c)
        {
            auto h = serialHook;
            if (!h || h->_recursion)
                return;
            h->hookImpl(c);
        }
        void hookImpl(char c)
        {
            _recursion++;
            xSemaphoreTake(_lock, portMAX_DELAY);
            auto b = _serialBuf;
            auto bl = _serialBufLen;
            if (b && bl)
            {
                if (c == '\n' || _serialBufPtr >= bl - 1)
                {
                    b[_serialBufPtr] = 0;
                    _serialBufPtr = 0;
                    const char **mptr = (const char **)&b;
                    auto level = detectLevel(mptr);
                    xSemaphoreGive(_lock);
                    Logging::system().log(level, *mptr);
                    xSemaphoreTake(_lock, portMAX_DELAY);
                }
                if (c != '\n' && c != '\r')
                    b[_serialBufPtr++] = c;
            }
            xSemaphoreGive(_lock);
            _recursion--;
        }

        SemaphoreHandle_t _lock;
        char *_serialBuf;
        size_t _serialBufLen;
        int _serialBufPtr = 0;
        uint8_t _recursion = 0;
        friend class Logging;
    };

    void Logging::hookUartLogger(int bufsize)
    {
        auto h = serialHook;
        if (bufsize)
        {
            if (h)
            {
                if (h->_serialBufLen == bufsize)
                    return;
                delete h;
            }
            new SerialHook(bufsize);
        }
        else if (h)
            delete h;
    }

} // namespace esp32m
