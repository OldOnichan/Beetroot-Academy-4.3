#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr int timeout_ms = 100;
static constexpr uint8_t rtc_address = 0x68;

struct ClockTime {
    int hours;
    int minutes;
    int seconds;
    int day;
    int month;
    int year;
};

static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char font_chars[] = "0123456789:. MTSWFaedhinortu";
static const uint8_t font[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00}, 
    {0x00, 0x60, 0x60, 0x00, 0x00}, 
    {0x00, 0x00, 0x00, 0x00, 0x00}, 
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, 
    {0x01, 0x01, 0x7F, 0x01, 0x01}, 
    {0x46, 0x49, 0x49, 0x49, 0x31}, 
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, 
    {0x7F, 0x09, 0x09, 0x09, 0x01}, 
    {0x20, 0x54, 0x54, 0x54, 0x78}, 
    {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7D, 0x40, 0x00}, 
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x08, 0x04, 0x04, 0x08}, 
    {0x04, 0x3F, 0x44, 0x40, 0x20}, 
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, 
};
static_assert(sizeof(font_chars) - 1 == sizeof(font) / sizeof(font[0]));

static int from_bcd(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0F);
}

static uint8_t to_bcd(int value)
{
    return static_cast<uint8_t>((value / 10) * 16 + value % 10);
}

static i2c_master_dev_handle_t add_device(i2c_master_bus_handle_t bus,
                                          uint8_t address)
{
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 100000;

    i2c_master_dev_handle_t device;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device));
    return device;
}

static bool i2c_ok(esp_err_t result, const char *operation)
{
    if (result == ESP_OK) {
        return true;
    }
    std::printf("%s: %s\n", operation, esp_err_to_name(result));
    return false;
}

static bool init_oled(i2c_master_dev_handle_t oled)
{
    const uint8_t commands[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81,
        0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x21, 0, 127, 0x22, 0, 7,
    };
    if (!i2c_ok(i2c_master_transmit(oled, commands, sizeof(commands), timeout_ms),
                "OLED init")) {
        return false;
    }

    const uint8_t blank_page[129] = {0x40};
    for (int page = 0; page < 8; ++page) {
        if (!i2c_ok(i2c_master_transmit(oled, blank_page, sizeof(blank_page), timeout_ms),
                    "OLED clear")) {
            return false;
        }
    }

    const uint8_t display_on[] = {0x00, 0xAF};
    return i2c_ok(i2c_master_transmit(oled, display_on, sizeof(display_on), timeout_ms),
                  "OLED display on");
}

static int weekday(const ClockTime &time)
{
   
    const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int year = time.year - (time.month < 3);
    return (year + year / 4 - year / 100 + year / 400
            + offsets[time.month - 1] + time.day) % 7;
}

static bool has_calendar_date(const ClockTime &time)
{
    if (time.year < 2000 || time.year > 2099 || time.month < 1 || time.month > 12) {
        return false;
    }

    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month_days = days[time.month - 1] + (time.month == 2 && time.year % 4 == 0);
    return time.day >= 1 && time.day <= month_days
        && !(time.year == 2000 && time.month == 1 && time.day == 1);
}

static bool read_rtc(i2c_master_dev_handle_t rtc, ClockTime &time)
{
    const uint8_t first_register = 0;
    uint8_t data[7];
    if (!i2c_ok(i2c_master_transmit_receive(
                    rtc, &first_register, 1, data, sizeof(data), timeout_ms),
                "RTC read")) {
        return false;
    }

    time.seconds = from_bcd(data[0] & 0x7F);
    time.minutes = from_bcd(data[1] & 0x7F);
    time.hours = from_bcd(data[2] & 0x3F);
    if (data[2] & 0x40) {
        time.hours = from_bcd(data[2] & 0x1F) % 12;
        if (data[2] & 0x20) {
            time.hours += 12;
        }
    }
    time.day = from_bcd(data[4] & 0x3F);
    time.month = from_bcd(data[5] & 0x1F);
    time.year = 2000 + from_bcd(data[6]);

    const bool needs_time = (data[0] & 0x80)
        || time.hours > 23 || time.minutes > 59 || time.seconds > 59;
    const bool needs_date = !has_calendar_date(time);
    if (!needs_time && !needs_date) {
        return true;
    }

    if (needs_date) {
        const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char month[4];
        std::sscanf(__DATE__, "%3s %d %d", month, &time.day, &time.year);
        time.month = static_cast<int>((std::strstr(months, month) - months) / 3) + 1;
    }

    const uint8_t day_of_week = static_cast<uint8_t>(weekday(time) + 1);
    if (needs_time) {
        std::sscanf(__TIME__, "%d:%d:%d", &time.hours, &time.minutes, &time.seconds);
        const uint8_t start[] = {
            0, to_bcd(time.seconds), to_bcd(time.minutes), to_bcd(time.hours),
            day_of_week, to_bcd(time.day), to_bcd(time.month), to_bcd(time.year % 100),
        };
        return i2c_ok(i2c_master_transmit(rtc, start, sizeof(start), timeout_ms), "RTC start");
    }

    const uint8_t calendar[] = {
        3, day_of_week, to_bcd(time.day), to_bcd(time.month), to_bcd(time.year % 100),
    };
    return i2c_ok(i2c_master_transmit(rtc, calendar, sizeof(calendar), timeout_ms), "RTC set date");
}

static bool draw_text(i2c_master_dev_handle_t oled, const char *text, int page, int scale)
{
    const int length = static_cast<int>(std::strlen(text));
    const int width = length * 6 * scale - scale;
    const int left = (128 - width) / 2;
    const uint8_t window[] = {
        0x00, 0x21, static_cast<uint8_t>(left), static_cast<uint8_t>(left + width - 1),
        0x22, static_cast<uint8_t>(page), static_cast<uint8_t>(page + scale - 1),
    };
    uint8_t pixels[1 + 128 * 2] = {0x40};

    for (int i = 0; i < length; ++i) {
        const char *character = std::strchr(font_chars, text[i]);
        if (!character) {
            continue;
        }
        const uint8_t *glyph = font[character - font_chars];
        for (int column = 0; column < 5; ++column) {
            const int x = (i * 6 + column) * scale;
            if (scale == 1) {
                pixels[1 + x] = glyph[column];
                continue;
            }

            uint16_t expanded = 0;
            for (int row = 0; row < 7; ++row) {
                if (glyph[column] & (1 << row)) {
                    expanded |= 0x03 << (row * 2 + 1);
                }
            }
            pixels[1 + x] = pixels[2 + x] = expanded & 0xFF;
            pixels[1 + width + x] = pixels[2 + width + x] = expanded >> 8;
        }
    }

    return i2c_ok(i2c_master_transmit(oled, window, sizeof(window), timeout_ms), "OLED window")
        && i2c_ok(i2c_master_transmit(oled, pixels, 1 + width * scale, timeout_ms), "OLED draw");
}

static bool update_display(i2c_master_dev_handle_t oled, const ClockTime &time)
{
    char clock[12];
    char date[24];
    std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", time.hours, time.minutes, time.seconds);
    std::snprintf(date, sizeof(date), "%s %02d.%02d.%04d",
                  weekdays[weekday(time)], time.day, time.month, time.year);
    return draw_text(oled, clock, 2, 2) && draw_text(oled, date, 5, 1);
}

extern "C" void app_main(void)
{
    i2c_master_bus_config_t config = {};
    config.i2c_port = 0;
    config.sda_io_num = GPIO_NUM_8;
    config.scl_io_num = GPIO_NUM_9;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;

    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
    vTaskDelay(pdMS_TO_TICKS(100));

    const bool rtc_found = i2c_ok(i2c_master_probe(bus, rtc_address, timeout_ms), "RTC 0x68");
    if (rtc_found) {
        std::puts("RTC found at 0x68");
    }

    uint8_t oled_address = 0x3C;
    if (!i2c_ok(i2c_master_probe(bus, oled_address, timeout_ms), "OLED 0x3C")) {
        oled_address = 0x3D;
        if (!i2c_ok(i2c_master_probe(bus, oled_address, timeout_ms), "OLED 0x3D")) {
            return;
        }
    }
    std::printf("OLED found at 0x%02X\n", static_cast<unsigned>(oled_address));
    if (!rtc_found) {
        return;
    }

    auto rtc = add_device(bus, rtc_address);
    auto oled = add_device(bus, oled_address);
    if (!init_oled(oled)) {
        return;
    }

    int last_second = -1;
    while (true) {
        ClockTime time;
        if (!read_rtc(rtc, time)) {
            return;
        }
        if (time.seconds != last_second) {
            if (!update_display(oled, time)) {
                return;
            }
            last_second = time.seconds;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
