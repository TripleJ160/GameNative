/*
 * dac_present_receiver.cpp — Direct Android Compositing present-receiver thread.
 *
 * Ported/adapted from Winlator-Ludashi-Plus (vulkan_jni.cpp recv-thread) for
 * GameNative. Isolated in its own translation unit so GameNative's vulkan_jni.cpp
 * stays untouched. Linked into libvulkan_renderer.
 *
 * Flow: Wine's AHB Vulkan layer renders DXVK frames into the shared
 * AHardwareBuffer pool and sends a MSG_PRESENT (slot_index + render-complete
 * acquire sync_fd via SCM_RIGHTS) over the AHB Unix socket. This thread reads
 * each frame and drives GameNative's existing scanout path
 * (VulkanRendererContext::scanoutSetBuffer), which hands the AHB straight to
 * SurfaceFlinger via ASurfaceTransaction_setBuffer with the acquire fence —
 * bypassing the X11 compositor. A MSG_RELEASE is sent back so Wine can reuse
 * the slot.
 *
 * JNI entry points target com.winlator.renderer.VulkanRenderer (handle =
 * VulkanRendererContext*).
 *
 * NOTE (Milestone 1b / device iteration): scanoutSetBuffer SKIPs unless the
 * scanout SurfaceControl layers are active. The Java-side SC setup + suppression
 * of GameNative's X11 onUpdateWindowContent pull-path (so both don't drive the
 * same transaction) is wired separately. This thread degrades gracefully — if
 * scanout isn't active, scanoutSetBuffer logs SKIPPED and we still release the
 * slot so Wine never blocks.
 */

#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <atomic>
#include <sys/socket.h>

#include "VulkanRendererContext.h"

#define LOG_TAG "DAC_Receiver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MSG_PRESENT 1
#define MSG_RELEASE 2

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
    AHardwareBuffer* slots[DAC_MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr};
    VulkanRendererContext* renderer = nullptr;
    std::atomic<bool> running{false};
    pthread_t thread = 0;
    bool threadStarted = false;
    long frameCount = 0;
};

/* One active receiver per process (one game container at a time). */
static DacReceiver* g_dacReceiver = nullptr;

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

static void* dac_recv_thread(void* arg) {
    auto* st = reinterpret_cast<DacReceiver*>(arg);

    /* Lazily ensure scanout is initialized once real frames start arriving. */
    if (st->renderer) st->renderer->initScanout();

    /* Deferred-by-one-frame release: hold the previously-displayed slot until
     * the next frame arrives, giving SurfaceFlinger a full frame to consume it
     * before Wine re-renders into it. With a 4-deep pool this is ample slack.
     * (A tighter onComplete-driven release is a later refinement.) */
    int prevSlot = -1;

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
        if (pmsg.type != MSG_PRESENT) {
            continue;  /* ignore non-present control messages */
        }

        /* Render-complete acquire fence (SCM_RIGHTS ancillary data). */
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

        /* Drive GameNative's scanout. scanoutSetBuffer takes ownership of the
         * fence fd (passes it to ASurfaceTransaction_setBuffer); SurfaceFlinger
         * waits on it before scanning out. */
        if (st->renderer) {
            st->renderer->scanoutSetBuffer(st->slots[slot], 0, 0, st->width, st->height, acquireFd);
        } else if (acquireFd >= 0) {
            close(acquireFd);
        }

        /* Release the frame shown one cycle ago (deferred-by-one). */
        if (prevSlot >= 0) send_release(st->clientFd, (uint32_t)prevSlot, 1);
        prevSlot = (int)slot;
    }

    /* Release whatever we were still holding so Wine doesn't deadlock on exit. */
    if (prevSlot >= 0) send_release(st->clientFd, (uint32_t)prevSlot, 1);

    LOGI("recv thread: exiting (frames=%ld)", st->frameCount);
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_winlator_renderer_VulkanRenderer_nativeStartPresentReceiver(
        JNIEnv*, jobject, jlong handle, jint fd,
        jlong buf0, jlong buf1, jlong buf2, jlong buf3, jint width, jint height)
{
    auto* r = reinterpret_cast<VulkanRendererContext*>(handle);
    if (!r) { LOGE("startPresentReceiver: null renderer handle"); return; }

    if (g_dacReceiver != nullptr) {
        LOGW("startPresentReceiver: receiver already running, stopping previous");
        g_dacReceiver->running.store(false, std::memory_order_relaxed);
        if (g_dacReceiver->threadStarted) pthread_join(g_dacReceiver->thread, nullptr);
        delete g_dacReceiver;
        g_dacReceiver = nullptr;
    }

    auto* st = new DacReceiver();
    st->clientFd = (int)fd;
    st->renderer = r;
    st->width = (int)width;
    st->height = (int)height;
    st->slots[0] = reinterpret_cast<AHardwareBuffer*>(buf0);
    st->slots[1] = reinterpret_cast<AHardwareBuffer*>(buf1);
    st->slots[2] = reinterpret_cast<AHardwareBuffer*>(buf2);
    st->slots[3] = reinterpret_cast<AHardwareBuffer*>(buf3);
    st->slotCount = (buf3 != 0) ? 4 : 3;
    st->running.store(true, std::memory_order_relaxed);

    if (pthread_create(&st->thread, nullptr, dac_recv_thread, st) != 0) {
        LOGE("startPresentReceiver: pthread_create failed: %s", strerror(errno));
        delete st;
        return;
    }
    st->threadStarted = true;
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
    /* Unblock the recvmsg() so the thread can observe running=false and exit. */
    if (st->clientFd >= 0) shutdown(st->clientFd, SHUT_RDWR);
    if (st->threadStarted) pthread_join(st->thread, nullptr);
    LOGI("stopPresentReceiver: stopped");
    delete st;
}
