/* Host stand-in so face_view.c compiles unchanged in the simulator. */
#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) do { printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
