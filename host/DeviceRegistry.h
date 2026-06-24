#pragma once
//
// DeviceRegistry — the single owner of slot -> device mapping.
//
// Phase 1: a thin, behaviour-preserving wrapper over the vendored
// ps3eye::PS3EYECam::getDevices() pool (one global 8-slot map, its own lock).
// It centralises the enumeration that CaptureController used to do inline, so
// the capture code no longer touches the vendored driver directly.
//
// Phase 4 generalises this into a unified, multi-transport map keyed by USB
// port path (PS3 Eye + EyeToy merged), without changing this interface.
//
#include <memory>
#include "ICameraDevice.h"

namespace framebus { class Writer; }

namespace deviceregistry {

// The DeviceProfile of whatever camera currently occupies `slot` (PS3 Eye or
// EyeToy), or nullptr if the slot is empty (which also answers "is the slot
// occupied?"). Forces a re-enumeration. Used by the host to pick a device-
// appropriate initial mode and to re-advertise capabilities on hot-plug.
const DeviceProfile* ProfileForSlot(int slot);

// App-visible display name for `slot`: the occupying device's profile name
// ("PS3 Eye" / "PS2 EyeToy"), suffixed " #<slot>" only when more than one camera
// of that type is present (so a lone device of a type is unnumbered). Empty
// slots yield "Camera #<slot>". Used for both the virtual-camera friendly name
// and the Settings combo so they agree. Forces a re-enumeration.
void SlotDisplayName(int slot, wchar_t* buf, size_t cap);

// Publish the slot's device capabilities (modes / formats / default) into the
// FrameBus ColdBlock. Called once after bus creation, before the virtual camera
// is registered, so the DLL always has a capability block to build media types
// from (docs/TDD.md §8.3). Phase 1/3: every slot advertises the PS3 Eye; Phase 4
// resolves the profile per-slot from the unified port-path map.
void PublishColdBlock(framebus::Writer& bus, int slot);

// Build the ICameraDevice for the camera in `slot`, or nullptr if empty.
std::unique_ptr<ICameraDevice> Acquire(int slot);

// Force a re-enumeration: drops unplugged devices and repairs replugged slots.
void Rescan();

} // namespace deviceregistry
