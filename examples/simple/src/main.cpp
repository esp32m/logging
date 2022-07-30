#include <Arduino.h>

#include <SPIFFS.h>
#include <logging.hpp>
#include <fs-appender.hpp>
#include <ets-appender.hpp>
#include <udp-appender.hpp>

using namespace esp32m;

void setup()
{
  SPIFFS.begin(true);
  // send log messages to file "mylog" on the SPIFFS
  Logging::addAppender(new FSAppender(SPIFFS, "/mylog"));
  // send log messages to standard output (Serial)
  Logging::addAppender(&ETSAppender::instance());
  // send log messages to 192.168.1.1:1234 in the form of UDP packets
  Logging::addAppender(new UDPAppender("192.168.1.1", 1234));
  // redirect standard output to appenders
  Logging::hookUartLogger();
  // now use log_X macros to forward log messages to the registered appenders
  log_i("hello world!");
}

void loop()
{
  delay(1000);
  log_i("called from loop");
}
