#include "report.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

namespace report {
namespace {

// One shared buffer: these sketches are single-threaded and print from loop()
// only. Sized for the longest line any sketch produces, with room to spare.
char g_line[192];

void emit(const char *prefix, const char *fmt, va_list args)
{
    vsnprintf(g_line, sizeof(g_line), fmt, args);
    Serial.print(prefix);
    Serial.println(g_line);
}

} // namespace

void begin(uint8_t sketch)
{
    Serial.printf("MAXL-BRINGUP %02u begin\n", static_cast<unsigned>(sketch));
}

void end(uint8_t sketch)
{
    Serial.printf("MAXL-BRINGUP %02u end\n", static_cast<unsigned>(sketch));
}

void info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    emit("INFO   ", fmt, args);
    va_end(args);
}

void value(const char *key, const char *fmt, ...)
{
    Serial.print("RESULT ");
    Serial.print(key);
    Serial.print(" = ");

    va_list args;
    va_start(args, fmt);
    vsnprintf(g_line, sizeof(g_line), fmt, args);
    va_end(args);
    Serial.println(g_line);
}

void verdict(const char *state, const char *why)
{
    Serial.print("VERDICT ");
    Serial.print(state);
    if (why != nullptr && why[0] != '\0') {
        Serial.print(" -- ");
        Serial.print(why);
    }
    Serial.println();
}

} // namespace report
