#pragma once

#ifdef H2G_FW_VERSION_OVERRIDE
#define H2G_FW_VERSION H2G_FW_VERSION_OVERRIDE
#else
#define H2G_FW_VERSION "0.4.24"
#endif
#define H2G_CONFIG_SCHEMA 8
#ifndef H2G_BUILD_TARGET
#define H2G_BUILD_TARGET "esp32s3-n16r8"
#endif
