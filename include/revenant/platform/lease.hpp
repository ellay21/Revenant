#pragma once

// The publisher lease: an exclusive open-file-description (OFD) write lock on the whole segment
// file. The kernel owns it: it is released when the last descriptor of the holding open file
// description closes, including when the holder dies by any means. That makes "is the lease
// held?" a definitive liveness test with no PID-reuse, heartbeat or zombie ambiguity.
//
// Caveat: a child forked without exec shares the open file description, so it keeps the lease
// alive after the parent exits.

namespace revenant::platform {

/// Takes the lease if nobody holds it. Narrow contract: `fd` is open read-write.
[[nodiscard]] bool try_acquire_lease(int fd);

/// Waits until the lease is free and takes it: the hot-standby path. When the holder dies the
/// kernel grants the lock to exactly one waiter. Narrow contract: `fd` is open read-write.
void acquire_lease_blocking(int fd);

/// True if any open file description holds the lease. Never acquires anything, so probing can
/// never make a starting publisher fail. Works on a read-only descriptor.
[[nodiscard]] bool lease_is_held(int fd);

}  // namespace revenant::platform
