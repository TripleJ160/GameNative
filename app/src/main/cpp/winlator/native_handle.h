/*
 * native_handle.h — minimal vendored copy of AOSP's
 * system/core/libcutils/include/cutils/native_handle.h
 *
 * GameNative's gpu_image.c includes "native_handle.h" but no copy is committed
 * to the repo (the prebuilt libwinlator.so was built in an environment that had
 * it on the include path). Vendored here so the native libs can be rebuilt from
 * source (required to land the Direct Android Compositing additions). Only the
 * native_handle_t struct is needed — gpu_image.c reads ->version/->numFds/->data
 * from AHardwareBuffer_getNativeHandle(). The struct layout is a fixed public ABI.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct native_handle {
    int version;        /* sizeof(native_handle_t) */
    int numFds;         /* number of file descriptors at &data[0] */
    int numInts;        /* number of ints at &data[numFds] */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
    int data[0];        /* numFds + numInts ints */
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
} native_handle_t;

typedef const native_handle_t* buffer_handle_t;

#ifdef __cplusplus
}
#endif
