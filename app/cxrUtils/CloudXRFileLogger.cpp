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

#if defined _WIN32 && !defined _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <string>
#include <cstring>

#define LOG_TAG "CXRFileLogger"
#include "CloudXRFileLogger.h"

#include "CloudXRCommon.h"

using std::string;

#ifdef ANDROID
#include <sys/system_properties.h>
#endif

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)

#if defined(WIN32)
#include <shlobj.h>
#elif defined (__linux__)
#include <sys/stat.h>
#endif

#include <stdarg.h>
#include <errno.h>

// create global singleton here.
FileLogger g_logFile;

FileLogger::FileLogger()
{
}

FileLogger::~FileLogger()
{
    destroy();
}

#ifndef MAX_PATH
#define MAX_PATH    260
#endif


void FileLogger::init(const std::string &appOutputPath, std::string &filenamePrefix)
{
    m_init = true; // when init==false, we end up queueing all messages

    if (m_logLevel == cxrLL_Silence)
    {
        debugOut(cxrLL_Warning, "FileLogger::Init", "Logging set to silent mode, no further log messaging this run.");
        processMsgQueue(); // TBD, this might want to write to stdout still just as a 'heartbeat' thing.
        return; // we're not supposed to log anything else anyway.
    }

    string logDir = getLogDir(appOutputPath);

    // build name of output file, as [path]/[prefix] Log [timestamp suffix].txt
#if defined(WIN32)
    SYSTEMTIME t;
    GetLocalTime(&t);

    char suffix[MAX_PATH];
    sprintf_s(suffix, "%04d-%02d-%02d %02d.%02d.%02d",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
        t.wSecond);
    m_suffix = std::string(suffix);

    char filePath[MAX_PATH];
    sprintf_s(filePath, "%s\\%s Log %s.txt",
        logDir.c_str(), filenamePrefix.c_str(), suffix);

#elif defined (__linux__) || defined (__APPLE__)
    time_t t;
    struct tm* tmptr;
    time(&t);
    tmptr = localtime(&t);

    char suffix[MAX_PATH];
    sprintf(suffix, "%04d-%02d-%02d %02d.%02d.%02d",
        1900 + tmptr->tm_year, 1 + tmptr->tm_mon,
        tmptr->tm_mday, tmptr->tm_hour, tmptr->tm_min, tmptr->tm_sec);
    m_suffix = std::string(suffix);

    char filePath[MAX_PATH];
    sprintf(filePath, "%s/%s Log %s.txt", logDir.c_str(), filenamePrefix.c_str(), suffix);
#endif

    if (!MakePathDirs(logDir))
    {
        // we wont error out here, just report the error and continue.  no diff than it worked prior.
        debugOut(cxrLL_Error, "FileLogger::Init", "Failed to make output path: %s", logDir.c_str());
    }

    m_logFile = fopen(filePath, "wb");
    if (!m_logFile)
    {
        debugOut(cxrLL_Error, "FileLogger::Init", "Err #%s opening log file: %s. File logging disabled but still writing to debug output.", strerror(errno), filePath);
    }

    const char* infoMsg = "File logger for CloudXR SDK " CLOUDXR_VERSION ", built on " __DATE__ " " __TIME__ ".\n";
    reallyLog(cxrLL_Info, "FileLogger::Init", true, infoMsg); // print build info always to both file and stdout as first line.

    // at this point, FileLogger is set up and ready to go.
    // seems a good place for logging platform-specific bits.
#ifdef ANDROID
    char propStr[PROP_VALUE_MAX], propStr2[PROP_VALUE_MAX];
    int proplen;
    proplen = __system_property_get("ro.product.manufacturer", propStr);
    proplen = __system_property_get("ro.product.model", propStr2);
    vlog("Device is %s %s", propStr, propStr2);
    proplen = __system_property_get("ro.build.fingerprint", propStr);
    vlog("OS build fingerprint is %s", propStr);
    proplen = __system_property_get("ro.build.date", propStr);
    vlog("OS build date is %s", propStr);
    proplen = __system_property_get("ro.build.version.sdk", propStr);
    vlog("Android OS sdk version level is %s", propStr);
#endif

    // dump any messages attempted to log prior to init.
    processMsgQueue();
}

void FileLogger::destroy()
{
    m_loggerMutex.lock();
    if (m_logFile)
    {
        debugOut(cxrLL_Debug, "FileLogger::Destroy", "Closing the CloudXR log.");
        fclose(m_logFile);
        m_logFile = NULL;
    }
    m_preQueue.clear(); // just for sanity.
    m_loggerMutex.unlock();
}

void FileLogger::debugOut(cxrLogLevel ll, const char* tag, const char *fmt, ...)
{
    if (g_logFile.getLogLevel() > ll || g_logFile.getLogLevel() == cxrLL_Silence)
    {
        return;
    }

    char buffer[MAX_LOG_LINE_LEN];
    va_list aptr;

    va_start(aptr, fmt);
    vsnprintf(buffer, MAX_LOG_LINE_LEN, fmt, aptr);
    va_end(aptr);

    // pass false as we do NOT want this written to log file,
    // just to debug console(s).
    g_logFile.reallyLog(ll, tag, false, buffer);
}

void FileLogger::log(cxrLogLevel ll, const char *tag, const char *fmt, ...)
{
    if (m_init && (m_logLevel > ll || m_logLevel == cxrLL_Silence))
    {
        return;
    }

    char buffer[MAX_LOG_LINE_LEN];
    va_list aptr;

    va_start(aptr, fmt);
    vsnprintf(buffer, MAX_LOG_LINE_LEN, fmt, aptr);
    va_end(aptr);

    reallyLog(ll, tag, true, buffer);
}

void FileLogger::logva(cxrLogLevel ll, const char *tag, const char *fmt, va_list aptr)
{
    if (m_init && (m_logLevel > ll || m_logLevel == cxrLL_Silence))
    {
        return;
    }

    char buffer[MAX_LOG_LINE_LEN];
    vsnprintf(buffer, MAX_LOG_LINE_LEN, fmt, aptr);

    reallyLog(ll, tag, true, buffer);
}

void FileLogger::reallyLog(cxrLogLevel ll, const char *tag, bool writeToFile, const char *msg)
{
// leaving this in commit, but commented out, as it's useful for full sanity of logging on android
//#ifdef ANDROID
//    LOGGV("reallyLog: %s", msg);
//#endif

    // Yes, this is checked in the meta calls above.  But we'll sanity check
    // here again as the cost is microscopic, and can call this directly if no varargs.
    // called directly for any reason we want this log level test executed.
    if (m_init && (m_logLevel > ll || m_logLevel == cxrLL_Silence))
    { // only early return if we're initialized.  otherwise we may want to cache.
        return;
    }

    // one of the advantages of the FileLogger tangential to writing of a file
    // is that it builds a structured log line, including a timestamp, log level,
    // tag (module or other), and the actual composited log message.  Even if
    // we don't have a file open, this adds huge benefit to console output.
    char debug[MAX_LOG_LINE_LEN +64]; // add fudge factor for timestamp prefix
    char buffer[MAX_LOG_LINE_LEN];
    char tmptag[MAX_TAG_LEN] = "[?]";

    // SANITY
    // check tag is short/fixed length.
    const size_t taglen = strnlen(tag, MAX_TAG_LEN);
    if (taglen == MAX_TAG_LEN) // was too long, no null seen.
    {   // truncate w/ellipses and null.
        strncpy(tmptag, tag, MAX_TAG_LEN-4);
        tmptag[MAX_TAG_LEN - 4] = '.';
        tmptag[MAX_TAG_LEN - 3] = '.';
        tmptag[MAX_TAG_LEN - 2] = '.';
        tmptag[MAX_TAG_LEN - 1] = 0;
    }
    else if (taglen > 0)
    {
        // no need for safe copy as we're already measuring length.
        strcpy(tmptag, tag);
    }
    // else was zero, we just use tmptag as default initialized...

    // so first stage output buffer composites level+tag+msg.
    snprintf(buffer, MAX_LOG_LINE_LEN, "%c  (%s)  %s", cxrLLToChar(ll), tmptag, msg);

#if defined(WIN32)
    SYSTEMTIME t;
    GetLocalTime(&t);
    // note we add linefeed on the end here, as it's expected these are single line logs.
    sprintf_s(debug, "%02d:%02d:%02d.%03d  %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buffer);
#else // for all other platforms, use posix/C methods -- inc android/linux/ios.
    struct timespec ts;
    struct tm tm;
    char prefix[32];
    // first get raw ns timer
    int retval = clock_gettime(CLOCK_REALTIME, &ts);
    // convert into tm struct, so we can trivially print
    gmtime_r(&ts.tv_sec, &tm);
    // print time format into debug
    strftime(prefix, 32, "%H:%M:%S", &tm);
    // sprintf at END of above time the millisecs...
    snprintf(debug, MAX_LOG_LINE_LEN, "%s.%03ld %s\n", prefix, ts.tv_nsec/(1000L*1000L), buffer);
#endif

#if defined(ANDROID)
    // android raw logcat prints [buffer], as it already composes equiv of debug output.
    // also note the explicit "%s" format string is due to a security warning from compiler
    //  regarding using [bufffer] there raw, due to security of format not being a literal.
    // finally, we force TAG to be 'CXR', as it makes searching/filtering easier, and the
    //  buffer string already has the app-supplied tag embedded.
    int priority = cxrLLToAndroidPriority(ll);
    __android_log_print(priority, "CXR", "%s", buffer);
#else
    // all other platforms do a puts here, using debug string to ensure timestamp.
    // TODO: note on windows, we called ODS above.  That goes to the VS Output panel.
    // this instead goes to the console that comes up for the servers.  If running
    // inside another app, we might NOT want to bother with dup to stdout here on Windows.  
    fputs(debug, stdout); // log to stdout, fputs because linefeeds already added.

#ifdef _WIN32
    // in addition, on windows if debugger around, send to debugger directly.
    if (IsDebuggerPresent())
        OutputDebugStringA(debug);
#endif
#endif

    if (!writeToFile) return; // we're done with above console writes.

    if (!m_init)
        enqueueMsgBuffer(debug);
    else
        writeBufferToFile(debug);
}

void FileLogger::writeBufferToFile(const char* buf)
{
    // shouldn't have to sanity check, but let's do it anyway
    if (buf==nullptr) return;
    if (buf[0]==0) return;
    if (!m_init || !m_logFile) return;

    m_loggerMutex.lock();

    // print message to logfile.
    fputs(buf, m_logFile);
    if (m_logLevel > 0)
    {
        fflush(m_logFile);
    }

    if (m_logMaxSizeKB > 0)
    {
        // we cap logs file to a given KB size, assuming early lines are most important.
        long file_size = ftell(m_logFile);
        if ((file_size > 0) && ((uint32_t)file_size / 1024 >= m_logMaxSizeKB))
        {
            fputs("Reached the log file size limit.", m_logFile);
            fclose(m_logFile);
            m_logFile = NULL;
        }
    }

    m_loggerMutex.unlock();
}

void FileLogger::flush()
{
    m_loggerMutex.lock();
    if (m_logFile)
    {
        fflush(m_logFile);
    }
    m_loggerMutex.unlock();
}

void FileLogger::enqueueMsgBuffer(const char *msg)
{
    if (m_init) return; // queue only meant for pre-init at the moment.

    m_preQueue.push_back(msg); // at this time, takes char* and puts into std::string for convenience.
}

void FileLogger::processMsgQueue()
{
    if (!m_init) return; // only process queue AFTER init.
    if (m_preQueue.capacity()==0) return;

    for (std::string& msg : m_preQueue)
    {
        writeBufferToFile(msg.c_str());
    }

    // release all the data allocations.
    m_preQueue.clear();
}


#ifdef _WIN32
static const char *s_pathDelim = "\\";
#else //if __linux__ || defined(__APPLE__)
static const char *s_pathDelim = "/";
#endif

bool FileLogger::MakePathDirs(const std::string &inpath)
{
    if (inpath.empty()) return false;
    char path[CXR_MAX_PATH]; // TODO, we may need to use different win32 routines for long path support.
    strncpy(path, inpath.c_str(), inpath.length()+1); // +1 as std::string doesn't account for null terminator.

    bool ret = false;
    std::string partialPath;
    char *token = std::strtok(path, s_pathDelim);
    
    while (token) {
#ifdef _WIN32
        partialPath += token;
        partialPath += s_pathDelim;
        // if the file Attr is invalid (likely doesn't exist) OR doesn't flag as a directory, the we try and create.
        // this is both to NOT make unneeded Create calls, and because Create can fail, properly, on some paths
        // (like just C:\ it will fail with alternate error).
        //  TODO: remove or throw loud error if the DIRECTORY attr test fails,
        // because if there is a file with the dir name, we'll fail create.
        DWORD fileAttr = GetFileAttributesA(partialPath.c_str());
        if ((fileAttr == INVALID_FILE_ATTRIBUTES) ||
            (!(fileAttr & FILE_ATTRIBUTE_DIRECTORY))) {
            if (!CreateDirectoryA(partialPath.c_str(), NULL)) {
                DWORD err = GetLastError();
                ret = (err == ERROR_ALREADY_EXISTS);
                if (!ret) CXR_LOGW("Failed to create directory [%s], error = 0x%0x", partialPath.c_str(), err);
            }
            else {
                ret = true;
            }
        }
        else {
            ret = true;
        }
#else //if __linux__ || defined(__APPLE__)
        partialPath += s_pathDelim;
        partialPath += token;
        errno = 0;
        // the mode consts are 777 for folder.  Probably could be 770 or 776 or something.
        if (mkdir(partialPath.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) == -1) {
            ret = (errno == EEXIST);
            if (!ret) vloge("Failed to create directory [%s], error = %d", partialPath.c_str(), errno);
        }
        else {
            ret = true;
        }
#endif
        token = std::strtok(NULL, s_pathDelim);
    }

    // return true if last directory in path already exists or created succesfully
    CXR_LOGI("Log directory %s exist: %s", ret ? "does" : "DOES NOT", partialPath.c_str());
    return ret;
}


std::string &FileLogger::getLogDir(const std::string outPath)
{
    // it's an issue if the app hasn't yet specified an output path.
    if (m_logDir.empty())
    {
        // We normally expect Init to have been called with a valid path.
        // If we have no string, we build an output path matching what
        // 'old' CloudXR releases would do.
        // Note path access may fail on sandboxed OSes, e.g. Android 11.
        if (outPath.empty()) // this may be a subdirectory
        {
            std::string base = "."; // if all else fails, try to use CWD.
#if defined(ANDROID)
            m_logDir = "/sdcard/CloudXR/"; // TODO: something better here?
#elif defined(WIN32)
            char userDir[MAX_PATH + 1] = "";
            // we use the windows API to get the current user local appdata path to store our files.
            HRESULT result = SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, userDir);
            if (!SUCCEEDED(result) && !GetTempPath(sizeof(userDir), userDir))
                strcpy(userDir, "C:\\Temp\\"); // super-fallback case.
            m_logDir = userDir;
            m_logDir += "CloudXR\\";
#elif defined(__linux__)
            m_logDir = std::string(getenv("HOME")) + "/.CloudXR/";
#endif

            // okay, but now make sure output is actually in a subdirectory.
#if defined(WIN32)
            m_logDir += "logs\\";
#else
            m_logDir += "logs/";
#endif
        }
        else
            m_logDir = outPath;

        debugOut(cxrLL_Info, LOG_TAG, "Logs output dir set to: %s", m_logDir.c_str());

        // TODO: might want to verify once per run that the folder passed in,
        // or created above, is valid/accessible.  if we're hitting this block,
        // we've only just set m_logDir, so great time to validate or mkdirtree 
    }

    return m_logDir;
}

#endif
