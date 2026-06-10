/*
 * dac_present_receiver.cpp — Direct Android Compositing present-receiver thread.
 *
 * INTEGRATED into libvulkan_renderer.so (Phase 1): compiled as part of the
 * vulkan_renderer CMake target alongside vulkan_jni.cpp, so it drives scanout
 * by calling VulkanRendererContext methods DIRECTLY (ctx->scanoutSetBuffer /
 * ctx->initScanout) — no dlsym, no null-env JNI trampoline. This supersedes the
 * earlier standalone-libdac + dlsym workaround that existed only because the
 * renderer was a prebuilt blob; it now builds from source (the source matches
 * the prebuilt 1:1), so DAC is a first-class part of the renderer.
 *
 * Flow: Wine's AHB Vulkan layer renders DXVK frames into the shared
 * AHardwareBuffer pool and sends MSG_PRESENT (slot_index + render-complete
 * acquire sync_fd via SCM_RIGHTS) over the AHB Unix socket. This thread reads
 * each frame and forwards the slot's AHB + fence to GameNative's scanout (which
 * hands it to SurfaceFlinger via ASurfaceTransaction_setBuffer), then sends
 * MSG_RELEASE back so Wine can reuse the slot.
 *
 * JNI entrypoints (nativeStartPresentReceiver/Stop) target
 * com.winlator.renderer.VulkanRenderer; handle = VulkanRendererContext*.
 */

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <time.h>
#include <sys/socket.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <android/hardware_buffer.h>
#include "VulkanRendererContext.h"

#define LOG_TAG "DAC_Receiver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MSG_PRESENT 1
#define MSG_RELEASE 2
#define MSG_VSYNC   6   /* Android → Wine: real panel vsync (AChoreographer) */
#define MSG_REALLOC     8  /* Wine → Android: reallocate the AHB pool to a new
                            * resolution/count (dynamic pool — compatibility).
                            * Overloads present_msg: slot_index=width,
                            * dst_x=height, dst_y=count (same wire size). */
#define MSG_REALLOC_ACK 9  /* Android → Wine: pool reallocated; N handles follow.
                            * Overloads release_msg: slot_index=count. */

/* AHB pool format/usage — MUST match AHardwareBufferPool (BGRA_8888 + GPU
 * color-output | sampled | composer-overlay) so the guest imports identically. */
#define DAC_AHB_FORMAT 5            /* HAL_PIXEL_FORMAT_BGRA_8888 */
#define DAC_AHB_USAGE  0xB00ULL     /* 0x200 | 0x100 | 0x800 */

/* Wire layout — MUST match the Wine-side AHB layer (dlls/wineandroid.drv). */
struct present_msg {
    uint8_t  type;
    uint32_t slot_index;
    int32_t  acquire_fd;       /* render-complete sync_fd (also via SCM_RIGHTS) */
    int32_t  dst_x, dst_y, dst_w, dst_h;
    uint64_t present_id;
    uint8_t  bgra_bytes;
};

struct release_msg {
    uint8_t  type;
    uint32_t slot_index;
    int32_t  release_fd;       /* -1 */
    uint8_t  displayed;        /* 1 = shown */
    uint64_t vsync_time_ns;    /* 0 for plain release */
};

#define DAC_MAX_SLOTS 4

struct DacReceiver {
    int clientFd = -1;
    int slotCount = 0;
    int width = 0, height = 0;
    VulkanRendererContext* ctx = nullptr;   /* the renderer (direct C++ calls) */
    void* slots[DAC_MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr}; /* AHardwareBuffer* as void* */
    std::atomic<bool> running{false};
    pthread_t thread = 0;
    bool threadStarted = false;
    long frameCount = 0;
    // Phase-lock vsync source (AChoreographer → MSG_VSYNC), Ludashi parity.
    pthread_t vsyncThread = 0;
    bool vsyncThreadStarted = false;
    ALooper* vsyncLooper = nullptr;
    AChoreographer* vsyncChoreographer = nullptr;
    std::atomic<bool> vsyncPaused{false};   /* gate vsync sends during realloc */
    // Serializes ALL socket sends (vsync, release, and the realloc ACK+handle
    // batch). The realloc batch carries SCM_RIGHTS file descriptors via
    // AHardwareBuffer_sendHandleToUnixSocket; if any other send interleaves
    // between the ACK and a handle, the guest's recvHandleFromUnixSocket
    // desyncs and parses a stray message's bytes as a descriptor. vsyncPaused
    // is the fast-path gate; this mutex closes the TOCTOU window where a vsync
    // send already passed the gate check before realloc began.
    std::mutex sendMtx;
    // Dynamic pool: true once we've reallocated to native-owned AHBs (so we
    // free them on the next realloc / on stop). The INITIAL slots come from the
    // Java AHardwareBufferPool and are owned/freed by Java — we never free those.
    bool slotsNativeOwned = false;

    // ── Honest release timing (onComplete-driven) ────────────────────────────
    // Slots presented to scanout but not yet confirmed latched by SurfaceFlinger
    // (FIFO order; scanout transactions complete in submission order). When the
    // onComplete callback fires for the head entry, the PREVIOUS latched slot is
    // definitively off-screen → that's when MSG_RELEASE is sent (displayed=1,
    // which also serves as the guest's display tick). Guarded by g_cbMtx.
    static constexpr int PENDQ_CAP = 8;
    int pendQ[PENDQ_CAP] = {0};
    int pendHead = 0;
    int pendCnt = 0;
    int lastLatched = -1;   /* most recent slot known on-screen */
};

static DacReceiver* g_dacReceiver = nullptr;
/* Protects g_dacReceiver lifetime + the pendQ/lastLatched state against the
 * SurfaceFlinger onComplete callback thread. Lock order: g_cbMtx → sendMtx. */
static std::mutex g_cbMtx;

/* ── Performance metrics (read by the HUD via JNI getters in libdac) ──────────
 * DAC compositor latency EMA: T1 = present_msg arrival (recvmsg return),
 * T2 = scanout submit (ASurfaceTransaction apply). Same 7/8 EMA + 500ms
 * outlier guard as Winlator's VulkanRendererContext. Frametime = interval
 * between present arrivals; jitter = mean abs deviation of frametime.
 * All in microseconds; the Java side divides by 1000 for ms. */
static std::atomic<uint64_t> g_dacLatencyUs{0};
static std::atomic<uint64_t> g_dacFrameTimeUs{0};
static std::atomic<uint64_t> g_dacJitterUs{0};
static std::atomic<uint64_t> g_prevFrameUs{0};   /* last present time (frametime src) */

static inline uint64_t mono_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* 7/8 EMA (alpha = 1/8), matching Winlator's latencyEmaUs. */
static inline uint64_t ema_us(std::atomic<uint64_t>& acc, uint64_t sample) {
    if (sample >= 500000ULL) return acc.load(std::memory_order_relaxed); // >500ms outlier
    uint64_t prev = acc.load(std::memory_order_relaxed);
    uint64_t next = (prev == 0) ? sample : (prev * 7 + sample) / 8;
    acc.store(next, std::memory_order_relaxed);
    return next;
}

/* Bridge for VulkanRendererScanout's onComplete callback to report TRUE
 * compositor latency (arrival → SurfaceFlinger latch). Works for both DAC and
 * native scanout since both go through scanoutSetBuffer. */
extern "C" uint64_t dac_now_us() { return mono_us(); }
extern "C" void dac_record_true_latency(uint64_t latencyUs) { ema_us(g_dacLatencyUs, latencyUs); }

/* Frametime + jitter, measured at the SINGLE common present point
 * (VulkanRendererContext::scanoutSetBuffer) so ALL three pipelines — native,
 * DAC quality, DAC performance — report identical, real per-frame timing.
 * nowUs = scanout-submit timestamp. The jitter EMA feeds back into the
 * frametime EMA so the HUD can show one stable (jitter-absorbed) value. */
extern "C" void dac_record_frame(uint64_t nowUs) {
    uint64_t prev = g_prevFrameUs.exchange(nowUs, std::memory_order_relaxed);
    if (prev != 0 && nowUs > prev) {
        uint64_t ft = nowUs - prev;
        uint64_t ftEma = ema_us(g_dacFrameTimeUs, ft);
        uint64_t dev = (ft > ftEma) ? (ft - ftEma) : (ftEma - ft);
        ema_us(g_dacJitterUs, dev);
    }
}

static void send_release(DacReceiver* st, uint32_t slot, uint8_t displayed) {
    if (!st || st->clientFd < 0) return;
    struct release_msg rel{};
    rel.type = (uint8_t)MSG_RELEASE;
    rel.slot_index = slot;
    rel.release_fd = -1;
    rel.displayed = displayed;
    rel.vsync_time_ns = 0;
    std::lock_guard<std::mutex> lk(st->sendMtx);
    send(st->clientFd, &rel, sizeof(rel), MSG_NOSIGNAL);
}

/* Called from the SurfaceFlinger onComplete callback (binder thread) via
 * VulkanRendererScanout: the head pending slot has been latched/presented.
 * The previously-latched slot is now definitively replaced on screen, so THIS
 * is the honest moment to hand it back to the guest. (The old model released
 * the previous slot as soon as the NEXT present *arrived* — before SF had
 * latched it — racing the display; the guest's avoid-last-2-presented ring was
 * the only thing papering over that.) */
extern "C" void dac_on_scanout_complete(void) {
    std::lock_guard<std::mutex> lk(g_cbMtx);
    DacReceiver* st = g_dacReceiver;
    if (!st || st->pendCnt == 0) return;   /* native-scanout mode / stale callback */
    int latched = st->pendQ[st->pendHead];
    st->pendHead = (st->pendHead + 1) % DacReceiver::PENDQ_CAP;
    st->pendCnt--;
    if (st->lastLatched >= 0 && st->lastLatched != latched)
        send_release(st, (uint32_t)st->lastLatched, 1);
    st->lastLatched = latched;
}

/* AChoreographer frame callback — fires once per real panel vsync. Sends one
 * MSG_VSYNC (carrying frameTimeNanos, CLOCK_MONOTONIC) to the Wine layer so it
 * can phase-anchor vkWaitForPresentKHR on the actual vsync, then re-posts itself
 * (Choreographer callbacks are one-shot). Ported 1:1 from Ludashi. */
static void vsync_frame_callback(long frameTimeNanos, void* data) {
    auto* st = reinterpret_cast<DacReceiver*>(data);
    if (!st || !st->running.load(std::memory_order_relaxed)) return;
    struct release_msg msg{};
    msg.type = (uint8_t)MSG_VSYNC;
    msg.slot_index = 0;
    msg.release_fd = -1;
    msg.displayed = 0;
    msg.vsync_time_ns = (uint64_t)frameTimeNanos;
    int fd = st->clientFd;
    // Don't inject MSG_VSYNC into the stream while a realloc handshake is in
    // flight — the ACK + handle batch must reach the guest contiguously.
    if (fd >= 0 && !st->vsyncPaused.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(st->sendMtx);
        // Re-check under the lock: realloc may have started between the gate
        // check and acquiring the mutex.
        if (!st->vsyncPaused.load(std::memory_order_acquire))
            (void)send(fd, &msg, sizeof(msg), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    if (st->vsyncChoreographer)
        AChoreographer_postFrameCallback(st->vsyncChoreographer, vsync_frame_callback, data);
}

/* Dedicated Looper thread hosting the AChoreographer callback stream. */
static void* vsync_thread_func(void* arg) {
    auto* st = reinterpret_cast<DacReceiver*>(arg);
    st->vsyncLooper = ALooper_prepare(0);
    if (!st->vsyncLooper) { LOGE("vsync thread: ALooper_prepare failed"); return nullptr; }
    ALooper_acquire(st->vsyncLooper);
    st->vsyncChoreographer = AChoreographer_getInstance();
    if (!st->vsyncChoreographer) {
        LOGE("vsync thread: AChoreographer_getInstance failed");
        ALooper_release(st->vsyncLooper); st->vsyncLooper = nullptr; return nullptr;
    }
    AChoreographer_postFrameCallback(st->vsyncChoreographer, vsync_frame_callback, st);
    LOGI("vsync thread: phase-lock started");
    while (st->running.load(std::memory_order_relaxed)) {
        int rc = ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        if (rc == ALOOPER_POLL_ERROR) { LOGE("vsync thread: pollOnce ERROR"); break; }
    }
    ALooper_release(st->vsyncLooper);
    st->vsyncLooper = nullptr;
    st->vsyncChoreographer = nullptr;
    LOGI("vsync thread exiting");
    return nullptr;
}

/* Dynamic pool: the guest hit a swapchain whose resolution/count the current
 * pool can't serve. Free the old (native-owned) buffers, allocate `count` new
 * AHBs at width×height (BGRA, same usage as the Java pool), and ship them back
 * so the guest can hook instead of falling back to passthrough. The vsync sender
 * is paused so the ACK + handle batch reach the guest contiguously. */
static void realloc_pool(DacReceiver* st, uint32_t width, uint32_t height, uint32_t count) {
    if (count < 1) count = 1;
    if (count > DAC_MAX_SLOTS) count = DAC_MAX_SLOTS;
    LOGI("realloc_pool: request %ux%u count=%u (was %dx%d count=%d)",
         width, height, count, st->width, st->height, st->slotCount);

    st->vsyncPaused.store(true, std::memory_order_release);

    /* Free the previous native-allocated buffers (never the initial Java pool). */
    if (st->slotsNativeOwned) {
        for (int i = 0; i < st->slotCount; i++) {
            if (st->slots[i]) AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(st->slots[i]));
            st->slots[i] = nullptr;
        }
    }

    /* Allocate the new pool. */
    AHardwareBuffer* newbufs[DAC_MAX_SLOTS] = {nullptr};
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < count; i++) {
        AHardwareBuffer_Desc desc = {};
        desc.width  = width;
        desc.height = height;
        desc.layers = 1;
        desc.format = DAC_AHB_FORMAT;
        desc.usage  = DAC_AHB_USAGE;
        if (AHardwareBuffer_allocate(&desc, &newbufs[i]) != 0 || !newbufs[i]) {
            LOGE("realloc_pool: AHardwareBuffer_allocate failed at %u (%ux%u)", i, width, height);
            break;
        }
        allocated++;
    }
    if (allocated < count) {
        /* Partial failure — roll back and tell the guest to keep passthrough. */
        for (uint32_t i = 0; i < allocated; i++) AHardwareBuffer_release(newbufs[i]);
        struct release_msg ack{};
        ack.type = (uint8_t)MSG_REALLOC_ACK;
        ack.slot_index = 0;            /* count=0 → guest stays on passthrough */
        { std::lock_guard<std::mutex> lk(st->sendMtx);
          send(st->clientFd, &ack, sizeof(ack), MSG_NOSIGNAL); }
        st->vsyncPaused.store(false, std::memory_order_release);
        LOGE("realloc_pool: alloc failed; signalled passthrough");
        return;
    }

    /* ACK with the count, then ship the handles contiguously. The whole batch
     * is sent under sendMtx so no vsync/release send can split the ACK from its
     * handles (which would desync the guest's SCM_RIGHTS recv). vsyncPaused is
     * already set, but the mutex also covers the in-flight-vsync TOCTOU. */
    {
        std::lock_guard<std::mutex> lk(st->sendMtx);
        struct release_msg ack{};
        ack.type = (uint8_t)MSG_REALLOC_ACK;
        ack.slot_index = count;
        send(st->clientFd, &ack, sizeof(ack), MSG_NOSIGNAL);
        for (uint32_t i = 0; i < count; i++) {
            if (AHardwareBuffer_sendHandleToUnixSocket(newbufs[i], st->clientFd) != 0)
                LOGE("realloc_pool: sendHandle failed at %u", i);
        }
    }

    /* Adopt the new pool. */
    for (uint32_t i = 0; i < count; i++) st->slots[i] = newbufs[i];
    for (uint32_t i = count; i < DAC_MAX_SLOTS; i++) st->slots[i] = nullptr;
    st->slotCount = (int)count;
    st->width  = (int)width;
    st->height = (int)height;
    st->slotsNativeOwned = true;

    /* Old-pool latch state is meaningless for the new slots (the guest reset
     * all of its slot-free flags on the ACK). Stale onComplete callbacks from
     * old-pool transactions hit the pendCnt==0 guard and no-op. */
    {
        std::lock_guard<std::mutex> lk(g_cbMtx);
        st->pendHead = 0;
        st->pendCnt = 0;
        st->lastLatched = -1;
    }

    g_dacLatencyUs.store(0, std::memory_order_relaxed);
    g_dacFrameTimeUs.store(0, std::memory_order_relaxed);
    g_dacJitterUs.store(0, std::memory_order_relaxed);
    g_prevFrameUs.store(0, std::memory_order_relaxed);

    st->vsyncPaused.store(false, std::memory_order_release);
    LOGI("realloc_pool: now %ux%u count=%u (native-owned)", width, height, count);
}

static void* dac_recv_thread(void* arg) {
    auto* st = reinterpret_cast<DacReceiver*>(arg);

    /* NOTE: scanout is initialized LAZILY on the first MSG_PRESENT (below), NOT
     * here at connect/device-create time. Engaging scanout immediately flips the
     * GameNative X-server into scanout mode while the guest game is still
     * creating/realizing its Win32 window. Some games (e.g. NFS Most Wanted,
     * D3D11) block forever in window creation when that happens — they wait on a
     * show/activate/X round-trip that scanout mode disturbs. Deferring scanout
     * until the game actually presents its first frame keeps the normal X11
     * window path intact during game-window setup, then flips to DAC once frames
     * are flowing. Broforce/Vampire Survivors present almost immediately so they
     * are unaffected. */

    while (st->running.load(std::memory_order_relaxed)) {
        struct present_msg pmsg;
        struct msghdr msg = {};
        struct iovec iov;
        char cmsg_buf[CMSG_SPACE(sizeof(int))];

        iov.iov_base = &pmsg;
        iov.iov_len = sizeof(pmsg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t ret = recvmsg(st->clientFd, &msg, 0);
        if (ret <= 0) {
            if (ret == 0) LOGI("recv thread: Wine disconnected (EOF)");
            else          LOGE("recv thread: recvmsg failed: %s", strerror(errno));
            break;
        }

        /* Grab the SCM_RIGHTS fd from THIS segment first — on a stream socket
         * ancillary data is delivered with the segment's first byte, so it must
         * be captured before any continuation read below. */
        int acquireFd = -1;
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(&acquireFd, CMSG_DATA(cmsg), sizeof(int));
        }

        /* SOCK_STREAM has no message boundaries: a short read here is a SPLIT
         * message, not a lost one. Dropping it (the old `continue`) desyncs the
         * byte stream permanently — every later "message" parses garbage. Read
         * the remainder instead so framing is preserved. */
        if ((size_t)ret < sizeof(pmsg)) {
            size_t got = (size_t)ret;
            LOGW("recv thread: short read (%zu < %zu), completing", got, sizeof(pmsg));
            bool dead = false;
            while (got < sizeof(pmsg)) {
                ssize_t r2 = recv(st->clientFd, (char*)&pmsg + got, sizeof(pmsg) - got, 0);
                if (r2 <= 0) { dead = true; break; }
                got += (size_t)r2;
            }
            if (dead) {
                if (acquireFd >= 0) close(acquireFd);
                LOGE("recv thread: disconnect during split message");
                break;
            }
        }

        if (pmsg.type == MSG_REALLOC) {
            /* Overloaded present_msg: slot_index=width, dst_x=height, dst_y=count. */
            if (acquireFd >= 0) close(acquireFd);
            realloc_pool(st, pmsg.slot_index, (uint32_t)pmsg.dst_x, (uint32_t)pmsg.dst_y);
            continue;
        }
        if (pmsg.type != MSG_PRESENT) {
            if (acquireFd >= 0) close(acquireFd);
            continue;
        }

        /* Frametime/jitter are now measured in scanoutSetBuffer (the common
         * present point for native + DAC), so nothing to do here. */

        uint32_t slot = pmsg.slot_index;
        if (slot >= (uint32_t)st->slotCount || st->slots[slot] == nullptr) {
            LOGE("recv thread: invalid slot %u (max=%d)", slot, st->slotCount);
            if (acquireFd >= 0) close(acquireFd);
            continue;
        }

        st->frameCount++;
        if (st->frameCount <= 5 || (st->frameCount % 120 == 0))
            LOGI("recv thread: frame=%ld slot=%u acquireFd=%d", st->frameCount, slot, acquireFd);

        /* Record this present as pending-latch BEFORE submitting to scanout
         * (its onComplete may fire arbitrarily soon after ST_APPLY).
         *
         * Deadlock-proof fallback: if onComplete callbacks are NOT firing on
         * this device (API < 29 path, or SC callback loss), the queue would
         * fill while the guest's FIFO acquire blocks at image_count-1 frames
         * in flight, and no new presents would ever arrive to drain it. So if
         * the queue reaches slotCount-1, treat the oldest pending as latched
         * (exactly the old arrival-driven behavior, one frame more lag). On
         * devices where onComplete works this threshold is never hit. */
        {
            std::lock_guard<std::mutex> lk(g_cbMtx);
            int threshold = st->slotCount > 2 ? st->slotCount - 1 : 2;
            while (st->pendCnt >= threshold || st->pendCnt >= DacReceiver::PENDQ_CAP - 1) {
                int forced = st->pendQ[st->pendHead];
                st->pendHead = (st->pendHead + 1) % DacReceiver::PENDQ_CAP;
                st->pendCnt--;
                if (st->lastLatched >= 0 && st->lastLatched != forced)
                    send_release(st, (uint32_t)st->lastLatched, 1);
                st->lastLatched = forced;
                if ((st->frameCount % 600) == 0)
                    LOGW("recv thread: onComplete not draining, arrival-fallback release (pend=%d)",
                         st->pendCnt);
            }
            st->pendQ[(st->pendHead + st->pendCnt) % DacReceiver::PENDQ_CAP] = (int)slot;
            st->pendCnt++;
        }

        /* Forward to the renderer's scanout via a DIRECT C++ call (integrated —
         * no dlsym). scanoutSetBuffer takes ownership of the fence fd (hands it
         * to ASurfaceTransaction_setBuffer); SurfaceFlinger waits on it. */
        if (st->ctx) {
            /* Lazy scanout init on the first frame (idempotent — returns early
             * if already active). This is the deferral described at thread top:
             * scanout engages only once the game is actually presenting. */
            st->ctx->initScanout();
            st->ctx->scanoutSetBuffer(reinterpret_cast<AHardwareBuffer*>(st->slots[slot]),
                                      0, 0, st->width, st->height, acquireFd);
            /* True latency (arrival → SurfaceFlinger latch) is recorded by the
             * renderer's onComplete callback in scanoutSetBuffer; the honest
             * MSG_RELEASE for the previous slot fires there too
             * (dac_on_scanout_complete). */
        } else if (acquireFd >= 0) {
            close(acquireFd);
        }
    }

    LOGI("recv thread: exiting (frames=%ld)", st->frameCount);
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeStartPresentReceiver(
        JNIEnv*, jobject, jlong handle, jint fd,
        jlong buf0, jlong buf1, jlong buf2, jlong buf3, jint width, jint height)
{
    if (handle == 0) { LOGE("startPresentReceiver: null renderer handle"); return; }

    if (g_dacReceiver != nullptr) {
        LOGW("startPresentReceiver: receiver already running, stopping previous");
        DacReceiver* old;
        {   /* Detach under g_cbMtx so an in-flight onComplete callback can't
             * touch the receiver while we tear it down. */
            std::lock_guard<std::mutex> lk(g_cbMtx);
            old = g_dacReceiver;
            g_dacReceiver = nullptr;
        }
        old->running.store(false, std::memory_order_relaxed);
        if (old->vsyncThreadStarted) {
            if (old->vsyncLooper) ALooper_wake(old->vsyncLooper);
            pthread_join(old->vsyncThread, nullptr);
        }
        if (old->threadStarted) pthread_join(old->thread, nullptr);
        if (old->slotsNativeOwned) {
            for (int i = 0; i < old->slotCount; i++)
                if (old->slots[i])
                    AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(old->slots[i]));
        }
        delete old;
    }

    auto* st = new DacReceiver();
    st->clientFd = (int)fd;
    st->ctx = reinterpret_cast<VulkanRendererContext*>(handle);
    st->width = (int)width;
    st->height = (int)height;
    st->slots[0] = reinterpret_cast<void*>(buf0);
    st->slots[1] = reinterpret_cast<void*>(buf1);
    st->slots[2] = reinterpret_cast<void*>(buf2);
    st->slots[3] = reinterpret_cast<void*>(buf3);
    st->slotCount = (buf3 != 0) ? 4 : 3;

    /* Reset metrics for the new session. */
    g_dacLatencyUs.store(0, std::memory_order_relaxed);
    g_dacFrameTimeUs.store(0, std::memory_order_relaxed);
    g_dacJitterUs.store(0, std::memory_order_relaxed);
    g_prevFrameUs.store(0, std::memory_order_relaxed);

    st->running.store(true, std::memory_order_relaxed);
    if (pthread_create(&st->thread, nullptr, dac_recv_thread, st) != 0) {
        LOGE("startPresentReceiver: pthread_create failed: %s", strerror(errno));
        delete st;
        return;
    }
    st->threadStarted = true;
    if (pthread_create(&st->vsyncThread, nullptr, vsync_thread_func, st) == 0)
        st->vsyncThreadStarted = true;
    else
        LOGW("startPresentReceiver: vsync thread create failed (pacing degraded)");
    {
        std::lock_guard<std::mutex> lk(g_cbMtx);
        g_dacReceiver = st;
    }
    LOGI("startPresentReceiver: started (fd=%d, slots=%d, %dx%d)",
         st->clientFd, st->slotCount, st->width, st->height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeStopPresentReceiver(
        JNIEnv*, jobject, jlong handle)
{
    (void)handle;
    DacReceiver* st;
    {   /* Detach under g_cbMtx so a late onComplete callback no-ops. */
        std::lock_guard<std::mutex> lk(g_cbMtx);
        st = g_dacReceiver;
        g_dacReceiver = nullptr;
    }
    if (st == nullptr) return;

    st->running.store(false, std::memory_order_relaxed);
    if (st->vsyncThreadStarted) {
        if (st->vsyncLooper) ALooper_wake(st->vsyncLooper);
        pthread_join(st->vsyncThread, nullptr);
    }
    if (st->clientFd >= 0) shutdown(st->clientFd, SHUT_RDWR);
    if (st->threadStarted) pthread_join(st->thread, nullptr);
    /* Clear metrics so the HUD falls back to native-mode timing after DAC stops. */
    g_dacLatencyUs.store(0, std::memory_order_relaxed);
    g_dacFrameTimeUs.store(0, std::memory_order_relaxed);
    g_dacJitterUs.store(0, std::memory_order_relaxed);
    g_prevFrameUs.store(0, std::memory_order_relaxed);
    if (st->slotsNativeOwned) {
        for (int i = 0; i < st->slotCount; i++)
            if (st->slots[i]) AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(st->slots[i]));
    }
    LOGI("stopPresentReceiver: stopped");
    delete st;
}

/* ── HUD metric getters (DAC modes). Return 0 when DAC isn't delivering, so the
 * Java side knows to fall back to native-mode (X11) timing. All in microseconds. */
extern "C" JNIEXPORT jlong JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeGetDacLatencyUs(JNIEnv*, jobject) {
    return (jlong)g_dacLatencyUs.load(std::memory_order_relaxed);
}
extern "C" JNIEXPORT jlong JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeGetDacFrameTimeUs(JNIEnv*, jobject) {
    return (jlong)g_dacFrameTimeUs.load(std::memory_order_relaxed);
}
extern "C" JNIEXPORT jlong JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeGetDacJitterUs(JNIEnv*, jobject) {
    return (jlong)g_dacJitterUs.load(std::memory_order_relaxed);
}
