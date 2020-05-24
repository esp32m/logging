#pragma once

#include <FS.h>

#include "logging.hpp"

namespace esp32m
{

    /**
     * Sends output file system (SD, SPIFFS or other)
     */
    class FSAppender : public LogAppender
    {
    public:
        FSAppender(FS &fs, const char *name, uint8_t maxFiles = 1) : _fs(fs), _name(name), _maxFiles(maxFiles), _lock(xSemaphoreCreateRecursiveMutex()) {}
        FSAppender(const FSAppender &) = delete;

    protected:
        virtual bool append(const char *message);
        virtual bool shouldRotate(File &f) { return f.size() > 8192; }

    private:
        FS &_fs;
        File _file;
        const char *_name;
        uint8_t _maxFiles;
        SemaphoreHandle_t _lock;
    };

} // namespace esp32m