#include "SleepActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

void SleepActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG("SLP", "SleepActivity entered — clearing screen");
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
