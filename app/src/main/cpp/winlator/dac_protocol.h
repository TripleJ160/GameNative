/*
 * dac_protocol.h — Direct Android Compositing wire protocol (single source of
 * truth for BOTH endpoints of the AHB Unix socket).
 *
 * Guest side: dlls/vulkan_layer/ahb_layer.c + dlls/wineandroid.drv/vulkan_ahb.c
 *             (compiled together into libahb_layer.so), via vulkan_ahb.h.
 * Host side:  GameNative app/src/main/cpp/winlator/dac_present_receiver.cpp.
 *
 * The host copy lives at:
 *     GameNative/app/src/main/cpp/winlator/dac_protocol.h
 * and MUST be kept byte-identical with this file (two repos, one protocol).
 *
 * The structs are intentionally NOT packed — both endpoints are aarch64
 * binaries built with the same NDK clang, and the existing shipped layout is
 * the natural-aligned one. The _Static_asserts below pin that layout: if a
 * compiler/ABI ever disagrees, the build fails instead of the socket silently
 * desyncing. Any layout change is a protocol break: bump DAC_PROTOCOL_VERSION
 * and ship layer + receiver together (RUNTIME_VERSION re-extract handles the
 * guest side).
 */
#ifndef __DAC_PROTOCOL_H
#define __DAC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* Bumped on any wire-layout or semantic break. Not yet negotiated at runtime
 * (layer and receiver always ship in the same APK); exists so a future
 * handshake has a number to exchange. */
#define DAC_PROTOCOL_VERSION 1

/* IPC message types for Wine <-> Android app communication */
#define MSG_PRESENT  1
#define MSG_RELEASE  2
#define MSG_BUFFER   3
#define MSG_REQUEST  4
/* MSG_TICK: Android → Wine. Fires from ASurfaceTransaction_setOnCommit
 * (API 31+), once per real panel vsync. Drives vkWaitForPresentKHR pacing
 * in the layer. Distinct from MSG_RELEASE because we need pacing decoupled
 * from slot-freeing: OnCommit fires ~1 vsync earlier than OnComplete, which
 * lets DXVK pipeline frames instead of being locked to apply→display
 * latency. Sent with the same struct layout as release_msg (with type=5
 * and the other fields ignored) so the receive loop can read fixed-size
 * messages. */
#define MSG_TICK     5

/* MSG_VSYNC: Android → Wine. Fires from AChoreographer_postFrameCallback
 * (API 24+), once per real panel vsync — INDEPENDENT of whether we have
 * actually applied a transaction. This is what MSG_TICK was supposed to
 * be but couldn't be (OnCommit only fires when we commit; ticks then
 * follow our own present rate, not the panel rate). MSG_VSYNC is the
 * true panel-rate signal. Used by vkWaitForPresentKHR to phase-lock the
 * layer's release of DXVK's render thread to actual hardware vsync,
 * eliminating the scheduler-jitter "doubled motion" artifact that
 * pure-wall-clock pacing exhibits during fast camera motion. Sent with
 * the same struct layout as release_msg (type=6, other fields ignored). */
#define MSG_VSYNC    6

/* === DYNAMIC POOL REALLOC ===
 * MSG_REALLOC (Wine → Android): request the receiver to reallocate the AHB pool
 * to a new geometry/count so a swapchain whose size differs from the initial
 * pool can still be hooked (instead of falling back to X11 passthrough). Sent as
 * an overloaded present_msg with NO ancillary fd:
 *     type=MSG_REALLOC, slot_index=width, dst_x=height, dst_y=count.
 * MSG_REALLOC_ACK (Android → Wine): the receiver reallocated the pool; N new AHB
 * handles follow contiguously (sent via AHardwareBuffer_sendHandleToUnixSocket).
 * Sent as an overloaded release_msg: type=MSG_REALLOC_ACK, slot_index=N. N=0
 * means the realloc failed and the guest must stay on passthrough. The receiver
 * pauses its vsync sender across the handshake so the ACK + N handles arrive
 * contiguously on the socket with nothing interleaved. */
#define MSG_REALLOC     8
#define MSG_REALLOC_ACK 9

/* Maximum swapchain images / AHB pool slots (both endpoints). */
#define DAC_MAX_SLOTS  4
#define AHB_MAX_IMAGES DAC_MAX_SLOTS

/* IPC message structures.
 *
 * IMPORTANT: layout changes here are a protocol break. Both the layer
 * (libahb_layer.so) and the Android-side receiver MUST be rebuilt and
 * deployed together — the receiver parses fixed-size messages by
 * sizeof(struct) and any size mismatch corrupts subsequent reads. */
struct present_msg {
    uint8_t  type;         /* MSG_PRESENT */
    uint32_t slot_index;
    int32_t  acquire_fd;   /* sent as SCM_RIGHTS ancillary data */
    int32_t  dst_x, dst_y, dst_w, dst_h;
    uint64_t present_id;   /* DXVK's VkPresentIdKHR.pPresentIds[i]; 0 if none */
    uint8_t  bgra_bytes;   /* 0 = AHB memory is RGBA byte order (trojan-blit
                            *     mode — the layer's vkCmdBlitImage normalized
                            *     it).
                            * 1 = AHB memory is BGRA byte order (direct-render
                            *     mode — DXVK wrote BGRA bytes directly because
                            *     Wine's winevulkan thunk caches its own format
                            *     negotiation that bypasses the layer's
                            *     RGBA-only surface format advertisement).
                            * When 1, the receiver must perform an R↔B channel
                            * swap during composition (via vkCmdBlitImage's
                            * format-aware reinterpretation between source
                            * imported as B8G8R8A8 and destination R8G8B8A8). */
};

struct release_msg {
    uint8_t  type;         /* MSG_RELEASE / MSG_TICK / MSG_VSYNC */
    uint32_t slot_index;
    int32_t  release_fd;   /* wire field is vestigial (always -1): the real
                            * release fence travels as SCM_RIGHTS ancillary
                            * data on MSG_RELEASE when SurfaceFlinger provided
                            * one (getPreviousReleaseFenceFd). Receivers that
                            * don't pull the cmsg simply have the kernel close
                            * the fd — degrade-safe. */
    uint8_t  displayed;    /* 1 = onComplete release (frame was shown);
                            * 0 = mailbox-drain release (frame was skipped).
                            * Only displayed=1 advances the layer's
                            * display_count for vkWaitForPresentKHR pacing. */
    uint64_t vsync_time_ns; /* For MSG_VSYNC: AChoreographer frameTimeNanos
                             * (i.e. the actual panel vsync timestamp in
                             * CLOCK_MONOTONIC ns). Ignored for other types. */
};

struct buffer_msg {
    uint8_t  type;         /* MSG_BUFFER */
    uint32_t slot_index;
    /* AHardwareBuffer handle sent via AHardwareBuffer_sendHandleToUnixSocket */
};

struct request_msg {
    uint8_t  type;         /* MSG_REQUEST */
    uint32_t slot_index;
};

/* ── Layout pins (aarch64 natural alignment). A failure here means the
 * compiler produced a different layout than the one on the wire — fix the
 * build, don't ship. ── */
#ifdef __cplusplus
#define DAC_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define DAC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

DAC_STATIC_ASSERT(sizeof(struct present_msg) == 48, "present_msg wire size");
DAC_STATIC_ASSERT(offsetof(struct present_msg, slot_index) == 4,  "present_msg.slot_index");
DAC_STATIC_ASSERT(offsetof(struct present_msg, present_id) == 32, "present_msg.present_id");
DAC_STATIC_ASSERT(offsetof(struct present_msg, bgra_bytes) == 40, "present_msg.bgra_bytes");
DAC_STATIC_ASSERT(sizeof(struct release_msg) == 24, "release_msg wire size");
DAC_STATIC_ASSERT(offsetof(struct release_msg, displayed) == 12,     "release_msg.displayed");
DAC_STATIC_ASSERT(offsetof(struct release_msg, vsync_time_ns) == 16, "release_msg.vsync_time_ns");
DAC_STATIC_ASSERT(sizeof(struct buffer_msg) == 8, "buffer_msg wire size");

#endif /* __DAC_PROTOCOL_H */
