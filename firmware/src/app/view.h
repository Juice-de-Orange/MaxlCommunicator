/*
 * Turning a running node into something the screens can draw.
 *
 * ui/ may see hal/ and nothing else -- that is what firmware/layering.toml says
 * and what makes every screen renderable from a struct in a host test. So
 * somebody has to stand between the node and the screens, and app/ is the one
 * layer allowed to see both.
 *
 * Everything time-relative is resolved here, into seconds of age. That is not a
 * convenience: it is what makes docs/test-plan.md gate 4.1 achievable. A screen
 * that could read a clock would render differently every millisecond and refresh
 * for ever; a screen handed "45" refreshes when the 45 becomes 46, and the
 * caller decides how often that is worth asking about.
 */

#ifndef MAXL_APP_VIEW_H
#define MAXL_APP_VIEW_H

#include "app/node.h"
#include "hal/i_gnss.h"
#include "hal/i_sensor.h"
#include "ui/view_model.h"

#include <stdint.h>

namespace app {

/// What the UI needs that the node does not own: the sensor, the battery and
/// the GNSS all belong to the main loop, not to Node.
struct Peripherals {
    hal::SensorSample sensor;
    uint16_t batteryMv = 0;
    bool batteryLow = false;

    bool gnssPowered = false;
    bool gnssTimedOut = false;
    hal::GnssFix fix;
    uint32_t fixTakenAtUnix = 0;
};

/*
 * Fill `out` from the node and the peripherals.
 *
 * `nowUnix` and `uptimeS` come from the caller because Node's clock is not the
 * UI's business. `lastReadCounter` is the journal counter of the newest received
 * message the user has already seen -- everything above it counts as unread, and
 * ui::Action::MarkAllRead is what moves it.
 */
void buildViewModel(const Node &node, const Peripherals &peripherals, uint32_t nowUnix,
                    uint32_t uptimeS, uint32_t lastReadCounter, ui::ViewModel &out);

/// The journal counter of the newest received TEXT message, or 0 if there is
/// none. This is what MarkAllRead stores.
uint32_t newestReceivedCounter(const Node &node);

} // namespace app

#endif // MAXL_APP_VIEW_H
