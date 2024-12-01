#pragma once

#include <FS.h>

#include "logging.hpp"

namespace esp32m
{

    /**
     * Sends output file system (SD, SPIFFS or other)
     */
    class FSAppender : public FormattingAppender
    {
    public:
        FSAppender(FS &fs, const char *name, uint8_t maxFiles = 1, uint32_t maxFileSizeBytes=8192) : _fs(fs), _name(name), _maxFiles(maxFiles), _maxFileSizeBytes(maxFileSizeBytes), _lock(xSemaphoreCreateRecursiveMutex()) {}
        FSAppender(const FSAppender &) = delete;
        virtual bool close();

    protected:
        virtual bool append(const char *message);
        virtual bool shouldRotate(File &f) { return f.size() > _maxFileSizeBytes; }

    private:
        FS &_fs;
        File _file;
        const char *_name;
        uint8_t _maxFiles;
        uint32_t _maxFileSizeBytes;
        SemaphoreHandle_t _lock;

        String newFilename(uint8_t idx); // Max 512 files on disk
    };

} // namespace esp32m