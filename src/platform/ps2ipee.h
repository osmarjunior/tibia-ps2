/**
 * Stub header for ps2ipee.h - not present in standard PS2 SDK
 * Used by ps2.c for networking functions (stubbed for POC)
 */

#ifndef __PS2IPEE_H__
#define __PS2IPEE_H__

#ifdef __cplusplus
extern "C" {
#endif

// Stub functions for networking (not used in POC)
static inline int ps2ip_init(void) { return 0; }
static inline void ps2ip_deinit(void) {}

#ifdef __cplusplus
}
#endif

#endif /* __PS2IPEE_H__ */