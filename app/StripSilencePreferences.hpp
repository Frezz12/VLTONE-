#pragma once
#include "StripSilence.hpp"

namespace daw { class EngineController; }
namespace ui::silence {
void restore(daw::EngineController& controller);
void persist(const daw::StripSilenceSettings& settings);
void setAutomatic(daw::EngineController& controller, bool enabled);
}
