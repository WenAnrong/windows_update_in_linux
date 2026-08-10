/*
 * ttydrm.h — DRM/TTY direct-render fake update screen.
 *
 * Renders the fake Windows update screen directly to the physical display via
 * DRM/KMS, bypassing the desktop entirely. Requires root. See ttydrm.c.
 */
#ifndef WINDOWS_UPDATE_IN_LINUX_TTYDRM_H
#define WINDOWS_UPDATE_IN_LINUX_TTYDRM_H

/*
 * Run the DRM/TTY direct-render fake update screen.
 *
 * Flow: switch to an idle VT -> take over DRM master -> draw the update UI
 * with FreeType -> animate progress 0% -> 35% (stuck) -> after timeout_sec
 * restore the original CRTC, drop master, switch back to the desktop VT.
 *
 * NEVER reboots. Returns 0 on a clean release, 2 if not run as root, 1 on error.
 */
int fake_update_ttydrm_run(unsigned int timeout_sec);

#endif /* WINDOWS_UPDATE_IN_LINUX_TTYDRM_H */
