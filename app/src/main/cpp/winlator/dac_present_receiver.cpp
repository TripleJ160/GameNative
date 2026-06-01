/*
 * dac_present_receiver.cpp — Direct Android Compositing present-receiver thread.
 *
 * Built into a STANDALONE libdac.so (with ahb_bridge.c), NOT into GameNative's
 * libvulkan_renderer / libwinlator. This is deliberate: GameNative's committed
 * native C sources are partly decompiled (e.g. gpu_image.c) and cannot be
 * rebuilt cleanly, and its real .so are prebuilt + committed. So instead of
 * rebuilding their libs, libdac.so drives scanout by dlsym-ing the already-
 * exported JNI entrypoints from the prebuilt libvulkan_renderer.so:
 *     Java_com_winlator_renderer_VulkanRenderer_nativeScanoutSetBuffer
 *     Java_com_winlator_renderer_VulkanRenderer_nativeInitScanout
 * Both ignore their JNIEnv and jobject args (they only cast the jlong handle
 * and call VulkanRendererContext methods), so we invoke them with null env/obj.
 * This leaves GameNative's prebuilt libs byte-for-byte untouched.
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
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <atomic>
#include <sys/socket.h>

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

/* dlsym'd JNI entrypoints from the prebuilt libvulkan_renderer.so. Signatures
 * mirror the real exports; env/obj are unused by those functions. */
typedef void (*pfn_scanoutSetBuffer)(void* env, void* obj, jlong handle,
                                     jlong ahbPtr, jint x, jint y, jint w, jint h, jint fenceFd);
typedef void (*pfn_initScanout)(void* env, void* obj, jlong handle);

struct DacReceiver {
    int clientFd = -1;
    int slotCount = 0;
    int width = 0, height = 0;
    jlong rendererHandle = 0;          /* VulkanRendererContext* as jlong */
    void* slots[DAC_MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr}; /* AHardwareBuffer* as void* */
    pfn_scanoutSetBuffer scanoutSetBuffer = nullptr;
    pfn_initScanout      initScanout = nullptr;
    std::atomic<bool> running{false};
    pthread_t thread = 0;
    bool threadStarted = false;
    long frameCount = 0;
};

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

/* Resolve the scanout JNI entrypoints from the already-loaded prebuilt lib. */
static bool resolve_scanout_fns(DacReceiver* st) {
    void* lib = dlopen("libvulkan_renderer.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libvulkan_renderer.so", RTLD_NOW);
    if (!lib) {
        LOGE("resolve: dlopen(libvulkan_renderer.so) failed: %s", dlerror());
        return false;
    }
    st->scanoutSetBuffer = (pfn_scanoutSetBuffer)dlsym(
        lib, "Java_com_winlator_renderer_VulkanRenderer_nativeScanoutSetBuffer");
    st->initScanout = (pfn_initScanout)dlsym(
        lib, "Java_com_winlator_renderer_VulkanRenderer_nativeInitScanout");
    if (!st->scanoutSetBuffer) {
        LOGE("resolve: dlsym nativeScanoutSetBuffer failed: %s", dlerror());
        return false;
    }
    /* initScanout is optional — log but don't fail if absent. */
    if (!st->initScanout) LOGW("resolve: nativeInitScanout not found (continuing)");
    return true;
}

static void* dac_recv_thread(void* arg) {
    auto* st = reinterpret_cast<DacReceiver*>(arg);

    if (st->initScanout) st->initScanout(nullptr, nullptr, st->rendererHandle);

    /* Deferred-by-one-frame release for buffer safety (4-deep pool gives slack). */
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
        if (pmsg.type != MSG_PRESENT) continue;

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

        /* Forward to GameNative's scanout via the dlsym'd JNI entrypoint. It
         * takes ownership of the fence fd (hands it to ASurfaceTransaction_setBuffer);
         * SurfaceFlinger waits on it before scanning out. */
        if (st->scanoutSetBuffer) {
            st->scanoutSetBuffer(nullptr, nullptr, st->rendererHandle,
                                 (jlong)(uintptr_t)st->slots[slot],
                                 0, 0, st->width, st->height, acquireFd);
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
        if (g_dacReceiver->threadStarted) pthread_join(g_dacReceiver->thread, nullptr);
        delete g_dacReceiver;
        g_dacReceiver = nullptr;
    }

    auto* st = new DacReceiver();
    st->clientFd = (int)fd;
    st->rendererHandle = handle;
    st->width = (int)width;
    st->height = (int)height;
    st->slots[0] = reinterpret_cast<void*>(buf0);
    st->slots[1] = reinterpret_cast<void*>(buf1);
    st->slots[2] = reinterpret_cast<void*>(buf2);
    st->slots[3] = reinterpret_cast<void*>(buf3);
    st->slotCount = (buf3 != 0) ? 4 : 3;

    if (!resolve_scanout_fns(st)) {
        LOGE("startPresentReceiver: could not resolve scanout entrypoints; aborting");
        delete st;
        return;
    }

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
    if (st->clientFd >= 0) shutdown(st->clientFd, SHUT_RDWR);
    if (st->threadStarted) pthread_join(st->thread, nullptr);
    LOGI("stopPresentReceiver: stopped");
    delete st;
}
