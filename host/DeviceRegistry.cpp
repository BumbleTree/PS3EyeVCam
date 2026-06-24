#include "DeviceRegistry.h"

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

#include "ps3eye.h"
#include "DeviceProfiles.h"
#include "../common/FrameBus.h"
#include "../common/VCamGuids.h"          // kVCamCount
#include "../transports/usb_bulk/Ps3EyeDevice.h"
#include "../transports/usb_iso/EyeToyDevice.h"
#include "../transports/usb_iso/EyeToyUsb.h"

// DeviceRegistry — the single authoritative owner of the slot -> device map
// (docs/TDD.md §6.1). Two enumerators feed it:
//   * the vendored PS3 driver's getDevices() pool (ctx #1), which owns its own
//     stable 0..7 port-path index for PS3 Eyes;
//   * eyetoy::EnumeratePortPaths() (ctx #2) for EyeToys.
//
// PS3 Eyes are AUTHORITATIVE on their getDevices() slot index, so a PS3-only
// system maps byte-for-byte identically to the pre-Phase-4 wrapper (regression
// requirement, TDD §6.1 / §11). EyeToys then fill the slots PS3 does not claim,
// assigned top-down (slot 7 first) and kept stable by port path across rescans,
// so two EyeToys keep distinct, stable virtual-camera slots through replug.

namespace {

// Serializes both enumerations and the persistent EyeToy slot assignment across
// every per-slot capture thread (was CaptureController's g_devicesLock).
SRWLOCK g_lock = SRWLOCK_INIT;

// Persistent EyeToy assignment: g_eyeToySlot[i] is the port path of the EyeToy
// mapped to slot i (empty = none). Reconciled on every rebuild; stable by port
// path. PS3 slots are taken from getDevices() each rebuild, so an EyeToy is
// never left occupying a slot a PS3 Eye has claimed.
std::string g_eyeToySlot[kVCamCount];

// Rebuild the unified map. Caller holds g_lock. Returns the PS3 pool (sparse
// 8-vector indexed by port path) so callers can mint a PS3EYERef by slot index.
const std::vector<ps3eye::PS3EYECam::PS3EYERef>& RebuildLocked()
{
    const auto& ps3 = ps3eye::PS3EYECam::getDevices(true);

    bool ps3Slot[kVCamCount] = {};
    for (int i = 0; i < kVCamCount && i < static_cast<int>(ps3.size()); ++i)
        ps3Slot[i] = (ps3[i] != nullptr);

    // Candidate EyeToys, in deterministic order so assignment is reproducible.
    std::vector<std::string> eyes = eyetoy::EnumeratePortPaths();
    std::sort(eyes.begin(), eyes.end());

    // Phase 1 (keep): an existing EyeToy slot whose port path is still present
    // and whose slot PS3 has not since claimed stays put; consume that path.
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (g_eyeToySlot[i].empty())
            continue;
        auto it = std::find(eyes.begin(), eyes.end(), g_eyeToySlot[i]);
        if (it != eyes.end() && !ps3Slot[i])
            eyes.erase(it);            // stayed plugged here; not a new candidate
        else
            g_eyeToySlot[i].clear();   // unplugged, or PS3 took this slot
    }

    // Phase 2 (assign new): each remaining EyeToy fills the first free slot
    // (no PS3, no EyeToy) counting down from the top.
    for (const auto& pp : eyes)
    {
        for (int i = kVCamCount - 1; i >= 0; --i)
        {
            if (!ps3Slot[i] && g_eyeToySlot[i].empty())
            {
                g_eyeToySlot[i] = pp;
                break;
            }
        }
    }
    return ps3;
}

} // namespace

namespace deviceregistry {

const DeviceProfile* ProfileForSlot(int slot)
{
    if (slot < 0 || slot >= kVCamCount)
        return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked();
    const bool isPs3 = ps3.size() > static_cast<size_t>(slot) && ps3[slot] != nullptr;
    const bool isEye = !g_eyeToySlot[slot].empty();
    ReleaseSRWLockExclusive(&g_lock);
    if (isPs3) return &Ps3EyeProfile();
    if (isEye) return &EyeToyProfile();
    return nullptr;
}

void SlotDisplayName(int slot, wchar_t* buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    if (slot < 0 || slot >= kVCamCount)
    {
        buf[0] = 0;
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked();
    // 1 = PS3 Eye, 2 = EyeToy, 0 = empty.
    auto slotType = [&](int i) -> int {
        if (ps3.size() > static_cast<size_t>(i) && ps3[i] != nullptr) return 1;
        if (!g_eyeToySlot[i].empty()) return 2;
        return 0;
    };
    const int type = slotType(slot);
    int typeCount = 0;
    for (int i = 0; i < kVCamCount; ++i)
        if (slotType(i) == type)
            ++typeCount;
    ReleaseSRWLockExclusive(&g_lock);

    const wchar_t* name = (type == 1) ? Ps3EyeProfile().displayName
                        : (type == 2) ? EyeToyProfile().displayName
                                      : nullptr;
    if (!name)
        swprintf_s(buf, cap, L"Camera #%d", slot);
    else if (typeCount > 1)
        swprintf_s(buf, cap, L"%s #%d", name, slot);  // disambiguate by slot (stable, unique)
    else
        swprintf_s(buf, cap, L"%s", name);            // lone device of its type: no number
}

std::unique_ptr<ICameraDevice> Acquire(int slot)
{
    if (slot < 0 || slot >= kVCamCount)
        return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked();
    ps3eye::PS3EYECam::PS3EYERef eye =
        (ps3.size() > static_cast<size_t>(slot)) ? ps3[slot] : nullptr;
    std::string eyeToyPath = g_eyeToySlot[slot];
    ReleaseSRWLockExclusive(&g_lock);

    // PS3 Eye takes precedence (Phase 1 guarantees the two never share a slot).
    if (eye)
        return std::make_unique<Ps3EyeDevice>(std::move(eye));
    if (!eyeToyPath.empty())
        return std::make_unique<EyeToyDevice>(std::move(eyeToyPath));
    return nullptr;
}

void Rescan()
{
    AcquireSRWLockExclusive(&g_lock);
    RebuildLocked();
    ReleaseSRWLockExclusive(&g_lock);
}

void PublishColdBlock(framebus::Writer& bus, int slot)
{
    // Resolve the slot's device profile (EyeToy or PS3 Eye); an empty slot
    // advertises the PS3 Eye, preserving the pre-Phase-4 default.
    const DeviceProfile* prof = ProfileForSlot(slot);
    const DeviceProfile& p = prof ? *prof : Ps3EyeProfile();

    framebus::ColdMode modes[framebus::kMaxModes];
    uint32_t count = p.modeCount;
    if (count > framebus::kMaxModes)
        count = framebus::kMaxModes;
    for (uint32_t i = 0; i < count; ++i)
    {
        modes[i].width      = p.modes[i].width;
        modes[i].height     = p.modes[i].height;
        modes[i].fps        = p.modes[i].fps;
        modes[i].formatMask = p.formatMask;   // per-device mask (Phase 1 profiles)
    }
    bus.WriteColdBlock(static_cast<uint32_t>(p.transport), p.defaultFormat, modes, count);
}

} // namespace deviceregistry
