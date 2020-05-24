#pragma once

#include <memory>
#include <esp_log.h>

#define logE(format, ...) this->logger().logf(LogLevel::Error, format, ##__VA_ARGS__)
#define logW(format, ...) this->logger().logf(LogLevel::Warning, format, ##__VA_ARGS__)
#define logI(format, ...) this->logger().logf(LogLevel::Info, format, ##__VA_ARGS__)
#define logD(format, ...) this->logger().logf(LogLevel::Debug, format, ##__VA_ARGS__)
#define logV(format, ...) this->logger().logf(LogLevel::Verbose, format, ##__VA_ARGS__)

#if LOGGING_REDEFINE_LOG_X

#undef log_e
#undef log_w
#undef log_i
#undef log_d
#undef log_v

#define log_e(format, ...) Logging::system().logf(LogLevel::Error, format, ##__VA_ARGS__)
#define log_w(format, ...) Logging::system().logf(LogLevel::Warning, format, ##__VA_ARGS__)
#define log_i(format, ...) Logging::system().logf(LogLevel::Info, format, ##__VA_ARGS__)
#define log_d(format, ...) Logging::system().logf(LogLevel::Debug, format, ##__VA_ARGS__)
#define log_v(format, ...) Logging::system().logf(LogLevel::Verbose, format, ##__VA_ARGS__)

#endif

namespace esp32m
{

  enum LogLevel
  {
    None,
    Default,
    Error,
    Warning,
    Info,
    Debug,
    Verbose
  };

  class Logger;

  /**
   * @brief Base abstract class for classes that support context logging
   */
  class Loggable
  {
  public:
    Logger &logger();

  protected:
    /**
     * @brief This needs to be overriden to return name of the loggable object
     * @return Name of the object for context logging
     */
    virtual const char *logName() const = 0;
    friend class Logger;

  private:
    std::unique_ptr<Logger> _logger;
  };

  /**
   * @brief Base class for loggable objects with the name initialized in the constructor
   */
  class SimpleLoggable : public Loggable
  {
  public:
    SimpleLoggable(const char *name) : _name(name) {}

  protected:
    virtual const char *logName() const { return _name; };

  private:
    const char *_name;
  };

  /**
   * @brief Logger implementation
   * Every loggable object creates and owns an instance of this class on demand
   * All public methods of this class are thread-safe
   */
  class Logger
  {
  public:
    Logger(const Logger &) = delete;
    /**
     * @brief Level of this logger.
     * Log messages with level greater than this one will be dropped
     */
    const LogLevel level() const { return _level; }
    /**
     * @brief Set level for this logger.
     * Log messages with level greater than this one will be dropped
     * @param level New log level
     */
    void setLevel(LogLevel level) { _level = level; }
    /**
     * @brief Send message to the log
     * @param level If greater than this logger's level, the message will be dropped
     * @param msg Message to be recorded
     */
    void log(LogLevel level, const char *msg);
    /**
     * @brief Format and send message to the log
     * @param level If greater than this logger's level, the message will be dropped
     * @param msg Message to be recorded
     * @param arg Arguments
     */
    void logf(LogLevel level, const char *msg, va_list arg);
    /**
     * @brief Format and send message to the log
     * @param level If greater than this logger's level, the message will be dropped
     * @param msg Message to be recorded
     */
    void logf(LogLevel level, const char *format, ...);

  private:
    const Loggable &_loggable;
    LogLevel _level = LogLevel::Default;
    Logger(const Loggable &loggable) : _loggable(loggable) {}
    friend class Loggable;
  };

  /**
   * @brief Base abstract class for log appenders
   * Log messages may be sent to multiple appenders (e.g. UART, filesystem, network etc.)
   */
  class LogAppender
  {
  protected:
    /**
     * @brief Implementations must override to this method to send log message to the corresponding medium
     * @note This method may be called with @p message set to @c nullptr to test appender's ability to record messages at this time. 
     *       In this case, if the appender knows for sure that it will not be able to record messages at this time 
     *       (for example, no connection to the server, filesystem not mounted etc.), it must return @c false. 
     *       Otherwise, if the appender believes that it should be able to record the messages, it should return @c true
     *       This behavior is exploited by buffering algorithms explained in the @c Logging::addBufferedAppender(...)
     * @note This method SHOULD be thread-safe and take as little time as possible to record the message, unless message queue is installed, see @c Logging::useQueue(...)
     * @param message Message to be recorded, may be @c nullptr
     * @return @c true on success, @c false on failure
     */
    virtual bool append(const char *message) = 0;

  private:
    LogAppender *_prev = nullptr;
    LogAppender *_next = nullptr;
    friend class Logger;
    friend class Logging;
    friend class BufferedAppender;
    friend class LogQueue;
  };

  class Logging
  {
  public:
    /** 
     * @brief This is the default logger to be used when @c Loggable instacne is not available
     */
    static Logger &system();
    /**
     * @brief Adds appender to the logging subsystem.
     * All log messages passing the level check will be sent to this appender
     * @param a Appender to be added
     */
    static void addAppender(LogAppender *a);
    /**
     * @brief Adds appender that may need some time to initialize before it can record messages (for example, connect to the network, mount filesystem etc.) 
     * If added with @c Logging::addAppender(), such appender will miss log messages sent before it is ready.
     * This method extablishes buffering layer that saves messages until the appender is ready, and then flushes buffered messages.
     * Additional memory is required to keep messages in the buffer.
     * @param a Appender to be added
     * @param bufsize Size of the circular buffer that keeps the most recent messages. If buffer overflows, it erases older messages until there's a space to record the most recent message.
     * @param autoRelease If @c true, the buffer will be released automatically once the appender is ready to accept messages. 
     *                    May be set to @c false if it is known that the appender may temporarily loose the ability to record messages even after succesful initialization.
     *                    In this case, the buffer memory will never be released.
     */
    static void addBufferedAppender(LogAppender *a, int bufsiza = 1024, bool autoRelease = true);
    /**
     * @brief Removes appender from the logging subsystem.
     * Log messages will no longer be sent to this appender
     * @param a Appender to be removed
     */
    static void removeAppender(LogAppender *a);
    /**
     * @brief Global log level, used in conjunction with the specific @c Logger's level to calculate effective log level.
     */  
    static const LogLevel level() { return _level; }
    /**
     * @brief Set global log level
     */  
    static void setLevel(LogLevel level) { _level = level; }
    /**
     * @brief Defines how the messages are being forwarded to appenders.
     * By default (when this method is not called, or called with @p size = 0), @c Logger::log(...) immediately forwards the message to all registered appenders. 
     * This may not be desirable if an appender takes significant time to record the message, or is not thread-safe.  
     * In the former case, logging may cause unwanted delays for time-critical operations, in the latter case additional synchronization may be required in the appender.
     * To work around these issues, a queue may be installed as an intemediate layer between the loggers and appenders. The messages are then collected in the queue, 
     * and processed sequentially in the dedicated thread, ensuring thread safety and no delay side-effects.
     * @param size Size of the queue. If set to 0, the queue will be removed.
     */
    static void useQueue(int size = 1024);
    /**
     * @brief Hooks ESP32-specific logging mechanism, see @c esp_log_set_vprintf() in the esp-idf docs for details
     * @param install Install or remove the hook to interecept log messages
     */ 
    static void hookEsp32Logger(bool install = true);

    /**
     * @brief Intercepts log messages written to UART via log_X and similar.
     * @param bufsize Size of line buffer. Set to 0 to remove UART hook
     */ 
    static void hookUartLogger(int bufsize = 128);

  private:
    static LogLevel _level;
  };

} // namespace esp32m