#include <malloc.h>
#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp32-hal.h>

#include "logging.hpp"

namespace esp32m
{

    LogLevel Logging::_level = LogLevel::Debug;
    LogAppender *_appenders = nullptr;
    SemaphoreHandle_t _loggingLock = xSemaphoreCreateMutex();

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
        BufferedAppender(LogAppender &appender, size_t bufsize, bool autoRelease)
            : _appender(appender), _autoRelease(autoRelease)
        {
            _lock = xSemaphoreCreateMutex();
            _handle = xRingbufferCreate(bufsize, RINGBUF_TYPE_NOSPLIT);
        }
        ~BufferedAppender()
        {
            release();
        }

    protected:
        bool append(const char *message)
        {
            if (!_handle)
                return _appender.append(message);
            size_t size;
            bool ok = true;
            char *item;
            while (_appender.append(nullptr))
            {
                xSemaphoreTake(_lock, portMAX_DELAY);
                item = (char *)xRingbufferReceive(_handle, &size, 0);
                xSemaphoreGive(_lock);
                if (!item)
                    break;
                ok &= _appender.append(item);
                vRingbufferReturnItem(_handle, item);
                xSemaphoreGive(_lock);
            }
            if (!_appender.append(message))
                for (;;)
                {
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    if (xRingbufferSend(_handle, message, strlen(message) + 1, 0))
                    {
                        xSemaphoreGive(_lock);
                        break;
                    }
                    item = (char *)xRingbufferReceive(_handle, &size, 0);
                    xSemaphoreGive(_lock);
                    if (!item)
                        break;
                    ok &= _appender.append(item);
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    vRingbufferReturnItem(_handle, item);
                    xSemaphoreGive(_lock);
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
        LogQueue(size_t bufsize)
            : _bufsize(bufsize)
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
        bool enqueue(const char *message)
        {
            xSemaphoreTake(_lock, portMAX_DELAY);
            auto result = xRingbufferSend(_buf, message, strlen(message) + 1, 10);
            xSemaphoreGive(_lock);
            return result;
        }

    private:
        size_t _bufsize;
        SemaphoreHandle_t _lock;
        RingbufHandle_t _buf;
        TaskHandle_t _task = nullptr;
        friend class Logging;
        void run()
        {
            esp_task_wdt_add(nullptr);
            for (;;)
            {
                esp_task_wdt_reset();
                size_t size;
                char *item = (char *)xRingbufferReceive(_buf, &size, 100);
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
            }
        }
    };

    void Logger::log(LogLevel level, const char *msg)
    {
        static const char *levels = "??EWIDV";
        if (!msg)
            return;
        auto msglen = strlen(msg);
        if (!msglen)
            return;
        auto effectiveLevel = _level;
        if (effectiveLevel == LogLevel::Default)
            effectiveLevel = Logging::level();
        if (level > effectiveLevel)
            return;
        if (!_appenders)
        {
            ets_printf("%s\n", msg);
            return;
        }
        auto name = _loggable.logName();
        char l = level >= 0 && level < 6 ? levels[level] : '?';
        char *buf = nullptr;
        struct tm timeinfo;

        time_t now;
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2016 - 1900))
        {
            char strftime_buf[32];
            strftime(strftime_buf, sizeof(strftime_buf), "%F %T", &timeinfo);
            buf = (char *)malloc(strlen(strftime_buf) + 1 /*dot*/ + 4 /*millis*/ + 1 /*space*/ + 1 /*level*/ + 1 /*space*/ + strlen(name) + 2 /*spaces*/ + msglen + 1 /*zero*/);
            sprintf(buf, "%s.%04d %c %s  %s", strftime_buf, (int)(millis() % 1000), l, name, msg);
        }
        else
        {
            auto uptimeUs = esp_timer_get_time();
            long m = uptimeUs / 1000;
            int millis = m % 1000;
            m /= 1000;
            int seconds = m % 60;
            m /= 60;
            int minutes = m % 60;
            m /= 60;
            int hours = m % 24;
            int days = m / 24;
            buf = (char *)malloc(6 /*days*/ + 1 /*colon*/ + 2 /*hours*/ + 1 /*colon*/ + 2 /*minutes*/ + 1 /*colon*/ + 2 /*seconds*/ + 1 /*colon*/ + 4 /*millis*/ + 1 /*space*/ + 1 /*level*/ + 1 /*space*/ + strlen(name) + 2 /*spaces*/ + msglen + 1 /*zero*/);
            sprintf(buf, "%d:%02d:%02d:%02d.%04d %c %s  %s", days, hours, minutes, seconds, millis, l, name, msg);
        }
        if (!buf)
            return;
        LogQueue *queue = logQueue;
        if (queue)
            queue->enqueue(buf);
        else
        {
            LogAppender *appender = _appenders;
            while (appender)
            {
                appender->append(buf);
                appender = appender->_next;
            }
        }
        free(buf);
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

    void Logging::addBufferedAppender(LogAppender *a, int bufsize, bool autoRelease)
    {
        if (a)
            addAppender(new BufferedAppender(*a, bufsize, autoRelease));
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

    void Logging::useQueue(int size)
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
            new LogQueue(size);
        }
        else if (q)
            delete q;
    }

    Logger &Logging::system()
    {
        static SimpleLoggable loggable("system");
        return loggable.logger();
    }

    LogLevel detectLevel(const char **mptr)
    {
        auto msg = *mptr;
        LogLevel l = LogLevel::None;
        if (msg && strlen(msg) > 4 && msg[0] == '[' && msg[2] == ']')
            switch (msg[1])
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
            }
        if (l)
            (*mptr) += 3;
        else
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
        static int esp32hook(const char *str, va_list arg)
        {
            auto h = _esp32Hook;
            if (!h || h->_recursion)
                return 0;
            h->_recursion++;
            const char *mptr = str;
            auto level = detectLevel(&mptr);
            Logging::system().logf(level, mptr, arg);
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
            ets_install_putc1(ets_write_char_uart);
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
                if (c != '\n')
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