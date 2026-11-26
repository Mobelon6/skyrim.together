#pragma once

struct BSString
{
    BSString()
        : data{nullptr}
        , length{0}
        , capacity{0}
    {
    }
    ~BSString();
    bool Assign(const char* text) noexcept;

    [[nodiscard]] const char* AsAscii() { return data; }

private:
    const char* data;
    uint16_t length;
    uint16_t capacity;
};
