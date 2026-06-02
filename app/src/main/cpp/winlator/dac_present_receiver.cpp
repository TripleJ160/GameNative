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
#include <time.h>
#include <sys/socket.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include "VulkanRendererContext.h"

#define LOG_TAG "DAC_Receiver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MSG_PRESENT 1
#define MSG_RELEASE 2
#define MSG_VSYNC   6   /* Android → Wine: real panel vsync (AChoreographer) */

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
};

static DacReceiver* g_dacReceiver = nullptr;

/* ── Performance metrics (read by the HUD via JNI getters in libdac) ──────────
 * DAC compositor latency EMA: T1 = present_msg arrival (recvmsg return),
 * T2 = scanout submit (ASurfaceTransaction apply). Same 7/8 EMA + 500ms
 * outlier guard as Winlator's VulkanRendererContext. Frametime = interval
 * between present arrivals; jitter = mean abs deviation of frametime.
 * All in microseconds; the Java side divides by 1000 for ms. */
static std::atomic<uint64_t> g_dacLatencyUs{0};
static std::atomic<uint64_t> g_dacFrameTimeUs{0};
static std::atomic<uint64_t> g_dacJitterUs{0};

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

static void send_release(int fd, uint32_t slot, uint8_t displayed) {
    if (fd < 0) return;
    struct release_msg rel{};
    rel.type = (uint8_t)MSG_RELEASE;
    rel.slot_index = slot;
    rel.release_fd = -1;
    rel.displayed = displayed;
    rel.vsync_time_ns = 0;
    send(fd, &rel, sizeof(rel), MSG_NOSIGNAL);
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
    if (fd >= 0) (void)send(fd, &msg, sizeof(msg), MSG_NOSIGNAL | MSG_DONTWAIT);
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

static void* dac_recv_thread(void* arg) {
    auto* st = reinterpret_cast<DacReceiver*>(arg);

    if (st->ctx) st->ctx->initScanout();

    /* Deferred-by-one-frame release for buffer safety (4-deep pool gives slack). */
    int prevSlot = -1;
    uint64_t prevArriveUs = 0;   /* for frametime + jitter */

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
        if ((size_t)ret < sizeof(pmsg)) {
            LOGE("recv thread: short read (%zd < %zu)", ret, sizeof(pmsg));
            continue;
        }
        if (pmsg.type != MSG_PRESENT) continue;

        /* T1 — frame arrival. Also update frametime/jitter EMAs. */
        uint64_t arriveUs = mono_us();
        if (prevArriveUs != 0) {
            uint64_t ft = arriveUs - prevArriveUs;
            uint64_t ftEma = ema_us(g_dacFrameTimeUs, ft);
            uint64_t dev = (ft > ftEma) ? (ft - ftEma) : (ftEma - ft);
            ema_us(g_dacJitterUs, dev);
        }
        prevArriveUs = arriveUs;

        int acquireFd = -1;
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(&acquireFd, CMSG_DATA(cmsg), sizeof(int));
        }

        uint32_t slot = pmsg.slot_index;
        if (slot >= (uint32_t)st->slotCount || st->slots[slot] == nullptr) {
            LOGE("recv thread: invalid slot %u (max=%d)", slot, st->slotCount);
            if (acquireFd >= 0) close(acquireFd);
            continue;
        }

        st->frameCount++;
        if (st->frameCount <= 5 || (st->frameCount % 120 == 0))
            LOGI("recv thread: frame=%ld slot=%u acquireFd=%d", st->frameCount, slot, acquireFd);

        /* Forward to the renderer's scanout via a DIRECT C++ call (integrated —
         * no dlsym). scanoutSetBuffer takes ownership of the fence fd (hands it
         * to ASurfaceTransaction_setBuffer); SurfaceFlinger waits on it. */
        if (st->ctx) {
            st->ctx->scanoutSetBuffer(reinterpret_cast<AHardwareBuffer*>(st->slots[slot]),
                                      0, 0, st->width, st->height, acquireFd);
            /* True latency (arrival → SurfaceFlinger latch) is recorded by the
             * renderer's onComplete callback in scanoutSetBuffer, not here. */
        } else if (acquireFd >= 0) {
            close(acquireFd);
        }

        if (prevSlot >= 0) send_release(st->clientFd, (uint32_t)prevSlot, 1);
        prevSlot = (int)slot;
    }

    if (prevSlot >= 0) send_release(st->clientFd, (uint32_t)prevSlot, 1);
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
        g_dacReceiver->running.store(false, std::memory_order_relaxed);
        if (g_dacReceiver->vsyncThreadStarted) {
            if (g_dacReceiver->vsyncLooper) ALooper_wake(g_dacReceiver->vsyncLooper);
            pthread_join(g_dacReceiver->vsyncThread, nullptr);
        }
        if (g_dacReceiver->threadStarted) pthread_join(g_dacReceiver->thread, nullptr);
        delete g_dacReceiver;
        g_dacReceiver = nullptr;
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
    g_dacReceiver = st;
    LOGI("startPresentReceiver: started (fd=%d, slots=%d, %dx%d)",
         st->clientFd, st->slotCount, st->width, st->height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeStopPresentReceiver(
        JNIEnv*, jobject, jlong handle)
{
    (void)handle;
    if (g_dacReceiver == nullptr) return;
    DacReceiver* st = g_dacReceiver;
    g_dacReceiver = nullptr;

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
