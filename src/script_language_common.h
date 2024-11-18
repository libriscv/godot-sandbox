#pragma once

#ifdef __ANDROID__
static constexpr bool register_language_icons = false;
#else
static constexpr bool register_language_icons = true;
#endif
