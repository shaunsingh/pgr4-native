// The port's settings in the game's Options menu (pgr4_settings.cpp).
#pragma once

#include <filesystem>

namespace pgr4::settings {

// At start-up, after the configuration file `config` loaded: the menus save their settings into
// it from then on. A settings file of 0.3.0 at `legacy` (userdata/pgr4_settings.toml, which was
// read over the configuration file) has its values taken into the configuration file, and goes.
void LoadSettings(const std::filesystem::path& config, const std::filesystem::path& legacy);

}  // namespace pgr4::settings
