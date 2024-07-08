/*
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#ifndef HELPER_CXR_LOG_H
#define HELPER_CXR_LOG_H

#include "CloudXRCommon.h"
#include <stdarg.h> // for vararg support

#if defined(ANDROID)
#include <android/log.h>

// TODO: atm, android values are +2 to ours, this is a quick hack.
// At some point, we can just have a remap array, since I think
// both are zero-based Enums.
extern "C" inline int cxrLLToAndroidPriority(cxrLogLevel ll)
{
    switch (ll)
    {
    case cxrLL_Debug: return ANDROID_LOG_DEBUG;
    case cxrLL_Info: return ANDROID_LOG_INFO;
    case cxrLL_Warning: return ANDROID_LOG_WARN;
    case cxrLL_Error: return ANDROID_LOG_ERROR;
    case cxrLL_Critical: return ANDROID_LOG_FATAL;
    case cxrLL_Silence: return ANDROID_LOG_SILENT;
    default: return ANDROID_LOG_VERBOSE;
    }
}
#endif

#ifndef LOG_TAG
#error "Please #define LOG_TAG in each file including CloudXRLog.h"
#endif

// to make use of the macros below, the including binary, client/server libraries, client/server apps,
// MUST implement this named dispatch function as the primary 'routing' call.  Then, they can implement
// this to log via whatever method is desired.
extern "C" void dispatchLogMsg(cxrLogLevel level, cxrMessageCategory category, void *extra, const char *tag, const char *fmt, ...);

#define CXR_LOGE(format, ...) dispatchLogMsg(cxrLL_Error, cxrMC_Correctness, nullptr, LOG_TAG, format, ## __VA_ARGS__)
#define CXR_LOGW(format, ...) dispatchLogMsg(cxrLL_Warning, cxrMC_Correctness, nullptr, LOG_TAG, format, ## __VA_ARGS__)
#define CXR_LOGI(format, ...) dispatchLogMsg(cxrLL_Info, cxrMC_Correctness, nullptr, LOG_TAG, format, ## __VA_ARGS__)
#define CXR_LOGD(format, ...) dispatchLogMsg(cxrLL_Debug, cxrMC_Correctness, nullptr, LOG_TAG, format, ## __VA_ARGS__)
#define CXR_LOGV(format, ...) dispatchLogMsg(cxrLL_Verbose, cxrMC_Correctness, nullptr, LOG_TAG, format, ## __VA_ARGS__)

// TEMPORARILY keeping older 'vlog' macros, just redirecting to new naming.
// TODO: move these to an internal-only header if in fact they are only used in internal code.
#define vlog        CXR_LOGI
#define vloge       CXR_LOGE
#define vlogex      CXR_LOGV

// TODO: review all usage of this.
// might want an extra param to the dispatch fn that gets passed to the
// logging callback, that says 'just raw stdout, no other logging of this message'.
#define vdprint     CXR_LOGD


// These are inline helper function to attempt to replace certain missing
// functionality with transitioning to the new logging/message callback system.

/// You pass getLogLevel in debugFlags from client or server, and it will look
/// for certain flags in order to return the appropriate default cxrLogLevel.
inline cxrLogLevel getLogLevel(uint32_t debugFlags)
{
    if (debugFlags & cxrDebugFlags_LogQuiet) // quiet takes precedence over other flags.
        return(cxrLL_Silence);
    else if (debugFlags & cxrDebugFlags_LogVerbose)
        return(cxrLL_Verbose);
    // Otherwise, we want a little more information in debug, a bit less in release.
#ifdef _DEBUG
    return(cxrLL_Debug);
#else
    return(cxrLL_Info);
#endif
}

inline cxrBool getLogPrivacyEnabled()
{ // TODO
#if _DEBUG
    return cxrFalse;
#else
    return cxrTrue;
#endif
}

inline uint64_t getLogMaxSize()
{
    return 1024LL * 1024LL * 8; // just hardcoding 8MB for the moment.  TODO.
}

#endif //HELPER_CXR_LOG_H
