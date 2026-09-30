#include "band.h"

namespace link {
namespace {

/*
 * The two entries in the table.
 *
 * Transmit power ceilings assume a 0 dBd (roughly 2.15 dBi) antenna and no cable
 * loss, which is what the supplied 868M whip is. ERP is what the regulation
 * limits, so a higher-gain antenna would mean lowering these numbers:
 *
 *   g3  500 mW ERP = 27.0 dBm ERP. The SX1262 tops out at +22 dBm, so the chip
 *       is the binding limit, not the regulation -- which is exactly the point
 *       of choosing g3 (CLAUDE.md 1.3).
 *   g1   25 mW ERP = 14.0 dBm ERP, and here the regulation binds.
 *
 * Airtime budget per hour follows from the duty cycle: 10 % of 3600 s = 360 s,
 * 1 % = 36 s.
 */
constexpr BandPlan kPlans[static_cast<uint8_t>(Band::Count)] = {
    // g3 / P -- primary. Default 869.575 MHz sits clear of 869.525, the LoRaWAN
    // RX2 downlink frequency where every gateway in range transmits at high power.
    {869400000u, 869650000u, 869575000u, 360000u, 22, 10},
    // g1 / M -- fallback.
    {868000000u, 868600000u, 868100000u, 36000u, 14, 1},
};

/// BW125, so a channel occupies 125 kHz centred on the configured frequency.
constexpr uint32_t kChannelHalfWidthHz = 62500u;

} // namespace

const BandPlan &plan(Band band)
{
    const uint8_t index = static_cast<uint8_t>(band);
    if (index >= static_cast<uint8_t>(Band::Count)) {
        return kPlans[static_cast<uint8_t>(Band::G3)];
    }
    return kPlans[index];
}

int8_t clampTxPowerDbm(Band band, int8_t requestedDbm)
{
    const int8_t ceiling = plan(band).maxTxPowerDbm;
    if (requestedDbm > ceiling) {
        return ceiling;
    }
    // The SX1262's low end. Below this the part does not regulate meaningfully.
    constexpr int8_t kFloorDbm = -9;
    if (requestedDbm < kFloorDbm) {
        return kFloorDbm;
    }
    return requestedDbm;
}

bool channelFitsInBand(Band band, uint32_t centreHz)
{
    const BandPlan &p = plan(band);
    if (centreHz < kChannelHalfWidthHz) {
        return false;
    }
    return (centreHz - kChannelHalfWidthHz) >= p.lowerHz &&
           (centreHz + kChannelHalfWidthHz) <= p.upperHz;
}

} // namespace link
