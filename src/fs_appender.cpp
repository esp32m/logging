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
                    a.concat('.');
                    a.concat(i);
                }
                if (_fs.exists(a))
                {
                    b = _name;
                    b.concat('.');
                    b.concat(i + 1);
                    if (_fs.exists(b))
                        _fs.remove(b);
                    _fs.rename(a, b);
                }
            }
            _file = _fs.open(_name, "a");
        }
        if (_file)
            result = !message || _file.println(message) > 0;
        xSemaphoreGiveRecursive(_lock);
        return result;
    }

} // namespace esp32m