# ESP32 logger

Logging library for ESP-32

## Features

* Context-based logging (separate logger may be defined for specific object/task/context)
* Multiple appenders may be used to send log messages to the network, filesystem, UART etc.
* Modular and extensible architecture, custom appenders may be defined easily
* Allows to buffer startup messages until appender's medium is ready to accept them 
* Allows to queue messages and process them in a dedicated thread to minimize impact of slow appenders and ensure thread safety
* Allows to forward ESP32-specific log output to the registered appenders
* Allows to hook log_X output (used in Arduino libs) and forward it to registered appenders

## Usage - simple

```cpp
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
  Logging::addAppender(new FSAppender(SPIFFS, "mylog"));
  // send log messages to standard output (Serial)
  Logging::addAppender(&ETSAppender::instance());
  // send log messages to 192.168.1.1:1234 in the form of UDP packets
  Logging::addAppender(new UDPAppender("192.168.1.1", 1234));
  // redirect standard output to appenders
  Logging::hookUartLogger();
  // now use log_X macros to forward log messages to the registered appenders
  log_i("hello world!");
}

void loop() {
  ...
}
```

## Usage - advanced

```cpp
#include <Arduino.h>

#include <logging.hpp>
#include <ets-appender.hpp>
#include <udp-appender.hpp>

using namespace esp32m;

class C1 : public SimpleLoggable
{
public:
  C1() : SimpleLoggable("c1") {}
  void doWork()
  {
    // will log messages in the context of the "c1" class
    logI("doing work");
  }
};

C1 c1;

void setup()
{
  // send log messages to standard output (Serial)
  Logging::addAppender(&ETSAppender::instance());
  // send log messages to 192.168.1.1:1234 in the form of UDP packets
  Logging::addAppender(new UDPAppender("192.168.1.1", 1234));
  // redirect standard output to appenders
  Logging::hookUartLogger();
}

void loop()
{
  c1.doWork();
  delay(1000);
}
```
