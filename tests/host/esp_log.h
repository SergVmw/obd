#pragma once
#include <cstdio>
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (false) std::fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (false) std::fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (false) std::fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
