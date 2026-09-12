#pragma once
#include <Arduino.h>

bool shtc3_init();
bool shtc3_read(float *temperature, float *humidity, float offset_c = 3.5f);
