/* Stand-in for PPSSPP's ppsspp_config.h: the platform test xBRZ's header makes (Linux builds only). */
#pragma once

#define PPSSPP_PLATFORM(x) (PPSSPP_PLATFORM_##x)
#define PPSSPP_PLATFORM_IOS 0
