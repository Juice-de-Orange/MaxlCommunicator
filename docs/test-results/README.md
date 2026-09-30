# Test results

`docs/test-plan.md`: *"A phase is complete when its gate passes, on real hardware, with the
numbers written down."* This is where the numbers are.

Every result states: date, node (from `firmware/nodes.ini`), firmware version including the
git hash, build environment, measuring equipment, conditions, raw data, result, pass/fail — and
**what the run does not answer.** The last point is the most important one: a report that only
names the half that passed reads like a passed gate.

Naming scheme: `YYYY-MM-DD_<subject>.md`, where `<subject>` is either a gate
(`gate_0-3_flash_cycles`) or a topic block (`phase1_drivers`). A run that touches several
gates gets one report and names all of them.

## Status

| Report | Gates in it |
|---|---|
| `2026-08-30_phase0_toolchain_nodeA.md` | Toolchain, image layout |
| `2026-08-31_gate_0-3_external_flash.md` | 0.3, first version |
| `2026-08-31_gate_0-3_flash_cycles.md` | **0.3 passed**, both halves; D12 |
| `2026-08-31_gate_0-5_rtc_across_reset.md` | 0.5 first half passed, drift measurement running |
| `2026-08-31_gate_0-6_size_budget.md` | **0.6 passed** |
| `2026-08-31_gate_0-7_tickless_idle.md` | **0.7 passed** |
| `2026-08-31_phase0_peripherals.md` | 0.1 partially; D13 |
| `2026-08-31_phase1_drivers.md` | **1.1 passed**; touch polarity, GNSS time against NTP |
| `2026-08-31_phase3_app_layer.md` | 3.1–3.4 in simulation, not claimed |
| `2026-08-31_gate_2-15_crypto_kat.md` | **2.15 passed** — on the device, not just on the host |
| `2026-08-31_phase4_ui.md` | **4.1 passed**; D14 |
| `2026-08-31_phase5_scaffold.md` | no gate — the tool for 0.4 and 5.1–5.4; first run of the radio driver |
| `2026-08-31_phase6_gatt_server.md` | 6.3–6.6 in simulation, not claimed |
| `2026-08-31_gate_6-9_foreground_ux.md` | **6.9 passed** |
| `2026-08-31_phase7_dashboard_gates.md` | **7.1–7.4 passed** |
| `2026-08-31_application_image.md` | no gate — `src/main.cpp` runs; unlocks 6.1 and 6.2 |
| `2026-08-31_deadman_survives_hung_loop.md` | no gate — the prerequisite for unattended flashing |
| `2026-08-31_integration_device_stack.md` | three bugs that no single layer could show |
| `2026-08-31_two_node_app_simulation.md` | two complete nodes over a simulated channel |
| `2026-08-31_gates_3-1_3-2_3-3_storage.md` | **3.1 passed**; 3.2 and 3.3 failed — both causes were in the sketch and have been fixed since the night to 09-01, re-run pending |
| `2026-08-31_gate_2-6_counter_power_cycles.md` | 2.6 failed — the sketch did not measure the frame counter; rebuilt since the night to 09-01, re-run pending |
| `2026-08-31_touch_not_reachable.md` | 1.5 **not reachable** on this device; the consequence is D17 |
| `2026-08-31_send_path_never_transmitted.md` | four bugs in a row on the send path; duty-cycle enforcement takes effect for the first time |
| `2026-08-31_first_two_node_contact.md` | first radio contact; 2.1/2.4 not passed. **Addendum 09-01:** both findings settled from the code — seq instead of counter, and there never was an ACK path |

**Met in simulation is not passed.** 3.1–3.4 and 6.3–6.6 are explicitly not claimed: they
need hardware, and the simulation can only show that the logic is internally consistent.

**And passed on the host is not passed either, when the gate names the device.**
Gate 2.15 requires the crypto known-answer test *"on device at boot in debug builds"*.
`firmware/test/unit/test_crypto.cpp` runs the RFC 3610 vectors thoroughly — on the host.
On 2026-08-31 this was found to be a false claim inherited from earlier versions of the project
notes — and then fixed: `link/self_test.cpp` now runs at boot, and
`2026-08-31_gate_2-15_crypto_kat.md` is the evidence from the device.

## `raw/`

Deliberately gitignored: serial captures can contain provisioned keys, bonding data or
passkeys. Individual files can be added after review with `git add -f`.
The reports above quote from them what matters.
