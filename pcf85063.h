#pragma once
#include <Arduino.h>
#include <time.h>

bool pcf85063_init();
bool pcf85063_is_available();
bool pcf85063_read_time(struct tm *ti);
bool pcf85063_set_time(const struct tm *ti);
