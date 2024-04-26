#include "fs-appender.hpp"

namespace esp32m
{

    bool FSAppender::append(const char *message)
    {
        bool result = false;
        xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
        if (!_file)
            _file = _fs.open(_name, "a");
        if (_file && _maxFiles > 1 && shouldRotate(_file))
        {
            auto rn = strlen(_name) + 1 + 3 + 1;
            String a, b;
            a.reserve(rn);
            b.reserve(rn);
            _file.close();
            for (auto i = _maxFiles - 2; i >= 0; i--)
            {
                a = _name;
                if (i)
                {
                    a = newFilename(i);
                }
                if (_fs.exists(a))
                {
                    b = newFilename(i+1);

                    if (_fs.exists(b))
                        _fs.remove(b);
                    _fs.rename(a, b);
                }
            }
            _file = _fs.open(_name, "a");
        }
        if (_file)
        {
            result = !message || _file.println(message) > 0;
            if (result)
                _file.flush();
        }
        xSemaphoreGiveRecursive(_lock);
        return result;
    }

    String FSAppender::newFilename(uint8_t i) {
        auto rn = strlen(_name) + 1 + 3 + 1; // Reserve space for 3 digits file index : Max 512 (uint8_t) files on disk
        String newName;
        newName.reserve(rn);
        newName = _name;

        int extIdx = newName.lastIndexOf('.');
        if(extIdx >= 0) {
            String ext = newName.substring(extIdx);
            newName = newName.substring(0, extIdx); // Name without file extension
            newName.concat('.');
            newName.concat(i);
            newName.concat(ext);
        }
        else {
            newName.concat('.');
            newName.concat(i);
        }

        return newName;
    }

} // namespace esp32m
