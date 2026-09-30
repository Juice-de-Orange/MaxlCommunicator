/*
 * Shared bench configuration for the bring-up sketches.
 *
 * Adaptive SF was wired into app::Node on 2026-08-31 (CLAUDE.md 2.5). A
 * measurement sketch that lets the modulation drift mid-run compares airtimes
 * against a table that assumed SF9 -- so every gate that reports airtime or
 * budget numbers pins the modem to the rendezvous configuration first. The
 * radio gates 2.11-2.14 are the ones that WANT the adaptation, and they get it
 * by not calling this.
 */

#ifndef MAXL_BRINGUP_BENCH_CONFIG_H
#define MAXL_BRINGUP_BENCH_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "app/node.h"

namespace bench {

inline void pinFixedSf9(app::Node &node)
{
    // Config TLVs, docs/bridge-protocol.md section 3: 0x04 sfMode (1 = fixed),
    // 0x05 fixedSf.
    const uint8_t tlvs[] = {0x04, 1, 1, 0x05, 1, 9};
    uint32_t applied = 0;
    uint8_t unapplied[16];
    size_t unappliedCount = 0;
    node.setConfig(1, tlvs, sizeof(tlvs), &applied, unapplied, &unappliedCount);
}

} // namespace bench

#endif // MAXL_BRINGUP_BENCH_CONFIG_H
