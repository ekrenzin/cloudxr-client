/*
 * Copyright (c) 2019-2023, NVIDIA CORPORATION. All rights reserved.
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

#include "CloudXRCommon.h"
#ifndef LOG_TAG
#define LOG_TAG  "CXRFileLogger"
#endif
#include "CloudXRLog.h"

#include <mutex>
#include <vector>

class FileLogger
{
public:
    FileLogger();
    ~FileLogger();

    void init(const std::string &appOutputPath, std::string &filenamePrefix);
    void destroy();
    void log(cxrLogLevel ll, const char *tag, const char *fmt, ...);
    void logva(cxrLogLevel ll, const char* tag, const char* format, va_list args);
    void reallyLog(cxrLogLevel ll, const char *tag, bool writeToFile, const char *msg);
    void writeBufferToFile(const char* buf);
    void flush();

    void enqueueMsgBuffer(const char *msg);
    void processMsgQueue();

    // this is a special helper that bypasses logfile, and ONLY emits to platform-specific debug output/console.
    // as a static method, it is also 100% safe to call at any time, doesn't require object
    static void debugOut(cxrLogLevel ll, const char *tag, const char *fmt, ...);
 
    std::string &getLogSuffix() { return m_suffix; }

    void setLogLevel(cxrLogLevel ll) { m_logLevel = ll; }
    inline cxrLogLevel getLogLevel() { return m_logLevel; }

    void setPrivacyEnabled(bool privacyEnabled) { m_privacyEnabled = privacyEnabled; }
    inline bool getPrivacyEnabled() { return m_privacyEnabled; }

    void setMaxAgeDays(int32_t days) {
        if (days >= 0)
            m_logMaxAgeDays = days;
    }

    void setMaxSizeKB(int32_t sizeKB) {
        if (sizeKB >= 0)
            m_logMaxSizeKB = sizeKB;
    }
    uint32_t getMaxSizeKB() { return m_logMaxSizeKB; }

    static bool MakePathDirs(const std::string &path); // static as it may be useful outside of logger.

private:
    // managing access to path to logs directory -- making this private until need arises for external access.
    std::string &getLogDir(const std::string outPath);

    cxrLogLevel m_logLevel = cxrLL_Info; // default unless set otherwise
    bool m_privacyEnabled = true; // default is privacy enabled.

    FILE *m_logFile = nullptr;
    bool m_init = false;

    std::mutex m_loggerMutex;
    std::string m_logDir;
    std::string m_suffix;

    std::vector<std::string> m_preQueue;

    // These variables are informative only, code to clean out
    // directories of files is not included in this sample/helper.
    const uint32_t c_defaultMaxAgeDays = 5;
    uint32_t m_logMaxAgeDays = c_defaultMaxAgeDays;

    // DEFAULT max log file size is 5MB, in KB.
    const uint32_t c_defaultMaxSizeKB = 5*1024;
    uint32_t m_logMaxSizeKB = c_defaultMaxSizeKB;
};

extern FileLogger g_logFile;
