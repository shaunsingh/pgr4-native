// The port's settings in the game's Options menu (pgr4_settings.cpp).
#pragma once

#include <filesystem>

namespace pgr4::settings {

// At start-up, after the configuration file: the settings the menus saved to `file`, which they
// save to from then on.
void LoadSettings(const std::filesystem::path& file);

}  // namespace pgr4::settings
