#include <string.h>
#include <WiFi.h>

#include "udp-appender.hpp"

namespace esp32m
{

UDPAppender::UDPAppender(const char* ipaddr, uint16_t port) : _fd(-1)
{
  memset(&_addr, 0, sizeof(_addr));
  _addr.sin_family = AF_INET;
  _addr.sin_port = htons(port);
  if (ipaddr) {
    inet_aton(ipaddr, &_addr.sin_addr.s_addr);
  }
  _format = port == 514 ? Format::Syslog : Format::Text;
  if (_addr.sin_addr.s_addr == 0)
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event)
    {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        _addr.sin_addr.s_addr = WiFi.gatewayIP();
        break;
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        _addr.sin_addr.s_addr = 0;
        break;
      default:
        break;
    }
  });
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

const uint8_t SyslogSeverity[] = {5, 5, 3, 4, 6, 7, 7};

bool UDPAppender::append(const LogMessage* message)
{
  if (!WiFi.isConnected() || !_addr.sin_addr.s_addr) {
    return false;
  }
  if (!message) {
    return true;
  }
  static char eol = '\n';
  if (_fd < 0)
  {
    struct timeval send_timeout = {1, 0};
    _fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_fd >= 0) {
      setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_timeout, sizeof(send_timeout));
    }
    else {
      return false;
    }
  }
  switch (_format)
  {
    case Format::Text:
    {
      auto formatter = Logging::formatter();
      auto msg = formatter(message);
      if (!msg) {
        return true;
      }
      auto len = strlen(msg);
      auto mptr = msg;
      while (len)
      {
        auto result = sendto(_fd, mptr, len, 0, (struct sockaddr*)&_addr, sizeof(_addr));
        if (result < 0)
        {
          free(msg);
          return false;
        }
        len -= result;
        mptr += result;
      }
      free(msg);
      return sendto(_fd, &eol, sizeof(eol), 0, (struct sockaddr*)&_addr, sizeof(_addr)) == sizeof(eol);
    }
    case Format::Syslog:
      // https://tools.ietf.org/html/rfc5424
      int pri = 3 /*system daemons*/ * 8 + SyslogSeverity[message->level()];
      char strftime_buf[4 /* YEAR */ + 1 /* - */ + 2 /* MONTH */ + 1 /* - */ + 2 /* DAY */ + 1 /* T */ + 2 /* HOUR */ + 1 /* : */ + 2 /* MINUTE */ + 1 /* : */ + 2 /* SECOND */ + 1 /*NULL*/];
      auto stamp = message->stamp();
      struct tm timeinfo;
      auto neg = stamp < 0;
      if (neg) {
        stamp = -stamp;
      }
      time_t now = stamp / 1000;
      gmtime_r(&now, &timeinfo);
      if (!neg) {
        timeinfo.tm_year = 0;
      }
      strftime(strftime_buf, sizeof(strftime_buf), "%FT%T", &timeinfo);
      const char* hostname = WiFi.getHostname();
      const char* name = message->name();
      auto ms = 1 /* < */ + 3 /* PRIVAL */ + 1 /* > */ + 1 /* version */ + 1 /* SP */ + strlen(strftime_buf) + 1 /* . */ + 4 /* MS */ + 1 /* Z */ + 1 /* SP */ + strlen(hostname) + 1 /* SP */ + strlen(name) + 1 /* SP */ + 1 + /* PROCID */ +1 /*SP*/ + 1 + /* MSGID */ +1 /* SP */ + 1 + /* STRUCTURED-DATA */ +1 /* SP */ + message->message_size() + 1 /*NULL*/;
      char* buf = (char*)malloc(ms);
      if (!buf) {
        return true;
      }
      sprintf(buf, "<%d>1 %s.%04dZ %s %s - - - %s", pri, strftime_buf, (int)(stamp % 1000), hostname, name, message->message());
      auto result = sendto(_fd, buf, strlen(buf), 0, (struct sockaddr*)&_addr, sizeof(_addr));
      free(buf);
      return (result >= 0);
  }
  return true;
}

} // namespace esp32m