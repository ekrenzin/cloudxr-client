/*
 * Copyright (c) 2019-2021, NVIDIA CORPORATION. All rights reserved.
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

#include "main.h"
#include "log.h"

#include <android/window.h>
#include <android/native_window_jni.h>
#include <android_native_app_glue.h>
#include <android/window.h> // for AWINDOW_FLAG_KEEP_SCREEN_ON

#include <linux/prctl.h>
#include <sys/prctl.h>

#include "CloudXRClientOptions.h"
#include "CloudXRMatrixHelpers.h"

#include <android/log.h>
#define TAG "CloudXR"
#ifdef ALOGV
#undef ALOGV
#endif
#ifdef ALOGE
#undef ALOGE
#endif
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)


static struct android_app* GAndroidApp = NULL;
static CloudXR::ClientOptions GOptions;
static std::mutex GJniMutex;
static CloudXRClientOVR *gClientHandle = NULL;

// TODO: these values are heavily dependent on app workload.
static const int CPU_LEVEL = 1;
static const int GPU_LEVEL = 1;


static double GetTimeInSeconds() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1e9 + now.tv_nsec) * 0.000000001;
}

#define CASE(x) \
case x:     \
return #x

const char* ClientStateEnumToString(cxrClientState state)
{
    switch (state)
    {
        CASE(cxrClientState_ReadyToConnect);
        CASE(cxrClientState_ConnectionAttemptInProgress);
        CASE(cxrClientState_ConnectionAttemptFailed);
        CASE(cxrClientState_StreamingSessionInProgress);
        CASE(cxrClientState_Disconnected);
        CASE(cxrClientState_Exiting);
        default:
            return "";
    }
}

const char* StateReasonEnumToString(cxrStateReason reason)
{
    switch (reason)
    {
        CASE(cxrStateReason_HEVCUnsupported);
        CASE(cxrStateReason_VersionMismatch);
        CASE(cxrStateReason_DisabledFeature);
        CASE(cxrStateReason_RTSPCannotConnect);
        CASE(cxrStateReason_HolePunchFailed);
        CASE(cxrStateReason_NetworkError);
        CASE(cxrStateReason_AuthorizationFailed);
        CASE(cxrStateReason_DisconnectedExpected);
        CASE(cxrStateReason_DisconnectedUnexpected);
        default:
            return "";
    }
}
#undef CASE

//==============================================================
//
//==============================================================
void android_handle_cmd(struct android_app* app, int32_t cmd) {
    CloudXRClientOVR* cxrc = (CloudXRClientOVR*)app->userData;
    if (cxrc == nullptr) {
        // TODO: shouldn't hit this case, but if we do we need to likely
        //  log and exit. TBD.
        ALOGE("android_handle_cmd called with null userData");
        return;
    }

    switch (cmd) {
        // TODO: handle gained/lost focus events
        case APP_CMD_START:
            ALOGV("APP_CMD_START");
            break;
        case APP_CMD_RESUME:
            ALOGV("APP_CMD_RESUME");
            cxrc->SetPaused(false);
            break;
        case APP_CMD_PAUSE:
            ALOGV("APP_CMD_PAUSE");
            cxrc->SetPaused(true);
            break;
        case APP_CMD_STOP:
            // TODO - may need to handle this
            ALOGV("APP_CMD_STOP");
            break;
        case APP_CMD_DESTROY:
            // TODO - may need to do more here
            ALOGV("APP_CMD_DESTROY");
            cxrc->SetWindow(nullptr);
            break;
        case APP_CMD_INIT_WINDOW:
            ALOGV("APP_CMD_INIT_WINDOW");
            cxrc->SetWindow(app->window);
            break;
        case APP_CMD_TERM_WINDOW:
            ALOGV("APP_TERM_WINDOW");
            cxrc->SetWindow(nullptr);
            break;
    }
}

//==============================================================
//
//==============================================================
int32_t android_handle_input(struct android_app* app, AInputEvent* event) {
    CloudXRClientOVR* cxrc = (CloudXRClientOVR*)app->userData;
    if (app == nullptr) {
        ALOGE("android_handle_input called with null userData");
        return 0;
    }

    // if needed, handle android keyboard/etc events here.
    // HMD+controller already handled via VrApi calls.

    return 0;
}

namespace
{

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
static inline cxrMatrix34 cxrConvert(const ovrMatrix4f& m)
{
    cxrMatrix34 out{};
    // The matrices are compatible so doing a memcpy() here
    //  noting that we are a [3][4] and ovr uses [4][4]
    memcpy(&out, &m, sizeof(out));
    return out;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
static inline cxrVector3 cxrConvert(const ovrVector3f& v)
{
    return {{v.x, v.y, v.z}};
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
static ovrQuatf cxrToQuaternion(const cxrMatrix34& m)
{
    ovrQuatf q;
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];

    if (trace > 0.f)
    {
        float s = 0.5f / sqrtf(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (m.m[2][1] - m.m[1][2]) * s;
        q.y = (m.m[0][2] - m.m[2][0]) * s;
        q.z = (m.m[1][0] - m.m[0][1]) * s;
    }
    else
    {
        if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
        {
            float s = 2.0f * sqrtf(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]);
            q.w = (m.m[2][1] - m.m[1][2]) / s;
            q.x = 0.25f * s;
            q.y = (m.m[0][1] + m.m[1][0]) / s;
            q.z = (m.m[0][2] + m.m[2][0]) / s;
        }
        else if (m.m[1][1] > m.m[2][2])
        {
            float s = 2.0f * sqrtf(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]);
            q.w = (m.m[0][2] - m.m[2][0]) / s;
            q.x = (m.m[0][1] + m.m[1][0]) / s;
            q.y = 0.25f * s;
            q.z = (m.m[1][2] + m.m[2][1]) / s;
        }
        else
        {
            float s = 2.0f * sqrtf(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]);
            q.w = (m.m[1][0] - m.m[0][1]) / s;
            q.x = (m.m[0][2] + m.m[2][0]) / s;
            q.y = (m.m[1][2] + m.m[2][1]) / s;
            q.z = 0.25f * s;
        }
    }

    return q;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
static ovrVector3f cxrGetTranslation(const cxrMatrix34& m)
{
    return {m.m[0][3], m.m[1][3], m.m[2][3]};
}

}  // namespace


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
CloudXRClientOVR::CloudXRClientOVR(struct android_app* app)
{
    mRenderState = RenderState_Loading;
    mHeadsetOnHead = true; // assume it is until we detect otherwise.

    // clear things we need to clear...
    mNativeWindow = NULL;
    mOvrSession = NULL;
    mFrameCounter = 0;

    // set paused state.
    mIsPaused = mWasPaused = true;

    mAndroidApp = app; // cache and hang onto it.

    mJavaCtx.Vm = app->activity->vm;
    mJavaCtx.Vm->AttachCurrentThread(&mJavaCtx.Env, NULL);
    mJavaCtx.ActivityObject = app->activity->clazz;

    // AttachCurrentThread reset the thread name, set it to something meaningful here.
    prctl(PR_SET_NAME, (long)"CloudXRClientOVR", 0, 0, 0);
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrError CloudXRClientOVR::Initialize()
{
    ALOGV("!!> Initialize START");

    int32_t result = VRAPI_INITIALIZE_SUCCESS;
    ovrInitParms localInitParms = vrapi_DefaultInitParms(&mJavaCtx);
    ALOGV("!!> Initialize VrApi");
    result = vrapi_Initialize(&localInitParms);
    if (result != VRAPI_INITIALIZE_SUCCESS)
    {
        ALOGE("Init - failed to Initialize the VRAPI localInitParms=%p", &localInitParms);
        return cxrError_Module_Load_Failed;
    }

    ALOGV("!!> Initialize EGL");
    if (!mEglHelper.Initialize())
    {
        ALOGE("Init - failed to initialize EglHelper");
        return cxrError_Failed;
    }

    ALOGV("!!> Initialize set pointers");
    mAndroidApp->userData = this;
    mAndroidApp->onAppCmd = android_handle_cmd;
    mAndroidApp->onInputEvent = android_handle_input;

    ALOGV("!!> Initialize END");

    return cxrError_Success;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrError CloudXRClientOVR::Release()
{
    mEglHelper.Release();

    // if we somehow still have session, release it now so we don't block display.
    if (mOvrSession != NULL)
    {
        ALOGV("CALLING vrapi_LeaveVrMode()");
        vrapi_LeaveVrMode(mOvrSession);
        mOvrSession = NULL;
    }
    vrapi_Shutdown();

    mJavaCtx.Vm->DetachCurrentThread();
    mJavaCtx.Vm = NULL;
    mJavaCtx.Env = NULL;
    mJavaCtx.ActivityObject = NULL;

    return cxrError_Success;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::RequestExit()
{
    mClientState = cxrClientState_Exiting;
    mRenderState = RenderState_Exiting;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::UpdateClientState()
{
    if (mClientState==cxrClientState_Exiting)
        return; // early return if we're ALREADY in exiting state.

    if (mConnectionDesc.async)
    {
        // all logging done in the callback, just do 'reactions' here.
        switch (mClientState)
        {
            case cxrClientState_ConnectionAttemptInProgress:
            { // status indication via log
                static int32_t attemptCount = 0;
                if (++attemptCount % 60 == 0) ALOGV("..... waiting for server connection .....");
                break;
            }

            case cxrClientState_StreamingSessionInProgress:
                mRenderState = RenderState_Running;
                break;

            case cxrClientState_ConnectionAttemptFailed:
            case cxrClientState_Disconnected:
                // fall through to below common handling...
                break;

            default:
                break;
        }
    }

    // handle client error state in one place so we don't duplicate this code.
    if (mClientState==cxrClientState_Disconnected ||
        mClientState==cxrClientState_ConnectionAttemptFailed)
    {
        RequestExit();
    }
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrError CloudXRClientOVR::MainLoop()
{
    const double startTime = GetTimeInSeconds();

    while (mAndroidApp->destroyRequested == 0 && mClientState != cxrClientState_Exiting) {
        // Read all pending events.
        for (;;) {
            int events;
            struct android_poll_source* source;
            const int timeoutMilliseconds = (mOvrSession == NULL || !mIsFocused) ? 250 : 1;
            if (ALooper_pollAll(timeoutMilliseconds, NULL, &events, (void**)&source) < 0)
                break;

            // Process this event.
            if (source != NULL)
                source->process(mAndroidApp, source);

        }

        // check and update client state changes from callback
        UpdateClientState();

        // we check state and handle vr enter/leave changes
        HandleVrModeChanges();

        // We must read from the event queue with regular frequency.
        HandleVrApiEvents();

        // if not yet entered vr mode, just continue as not yet set up to render...
        if (mOvrSession == NULL) continue;

        // TODO: is this check now implicitly handled in client state changes?
        //if (Receiver && !cxrIsRunning(Receiver) && mRenderState != RenderState_Exiting)
        //    mRenderState = RenderState_Exiting;

        if (mRenderState == RenderState_Loading)
            RenderLoadScreen();
        else if (mRenderState == RenderState_Exiting)
            RenderExitScreen();
        else
            Render();
    }

    return cxrError_Success;
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrError CloudXRClientOVR::CreateReceiver()
{
    if (Receiver)
        return cxrError_Success;

    if (GOptions.mServerIP.empty())
    {
        ALOGE("No CloudXR server address specified.");
        return cxrError_No_Addr;
    }

    if (mOvrSession == nullptr)
    {
        ALOGE("OVR session is null, cannot continue.");
        return cxrError_Failed; // false..
    }

    if (mDeviceDesc.receiveAudio)
    {
        // Initialize audio playback
        oboe::AudioStreamBuilder playbackStreamBuilder;
        playbackStreamBuilder.setDirection(oboe::Direction::Output);
        playbackStreamBuilder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        playbackStreamBuilder.setSharingMode(oboe::SharingMode::Exclusive);
        playbackStreamBuilder.setFormat(oboe::AudioFormat::I16);
        playbackStreamBuilder.setChannelCount(oboe::ChannelCount::Stereo);
        playbackStreamBuilder.setSampleRate(CXR_AUDIO_SAMPLING_RATE);

        oboe::Result r = playbackStreamBuilder.openStream(playbackStream);
        if (r != oboe::Result::OK) {
            ALOGE("Failed to open playback stream. Error: %s", oboe::convertToText(r));
            return cxrError_Failed;
        }

        int bufferSizeFrames = playbackStream->getFramesPerBurst() * 2;
        r = playbackStream->setBufferSizeInFrames(bufferSizeFrames);
        if (r != oboe::Result::OK) {
            ALOGE("Failed to set playback stream buffer size to: %d. Error: %s",
                    bufferSizeFrames, oboe::convertToText(r));
            return cxrError_Failed;
        }

        r = playbackStream->start();
        if (r != oboe::Result::OK) {
            ALOGE("Failed to start playback stream. Error: %s", oboe::convertToText(r));
            return cxrError_Failed;
        }
    }

    if (mDeviceDesc.sendAudio)
    {
        // Initialize audio recording
        oboe::AudioStreamBuilder recordingStreamBuilder;
        recordingStreamBuilder.setDirection(oboe::Direction::Input);
        recordingStreamBuilder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        recordingStreamBuilder.setSharingMode(oboe::SharingMode::Exclusive);
        recordingStreamBuilder.setFormat(oboe::AudioFormat::I16);
        recordingStreamBuilder.setChannelCount(oboe::ChannelCount::Stereo);
        recordingStreamBuilder.setSampleRate(CXR_AUDIO_SAMPLING_RATE);
        recordingStreamBuilder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        recordingStreamBuilder.setDataCallback(this);

        oboe::Result r = recordingStreamBuilder.openStream(recordingStream);
        if (r != oboe::Result::OK) {
            ALOGE("Failed to open recording stream. Error: %s", oboe::convertToText(r));
            return cxrError_Failed;
        }

        r = recordingStream->start();
        if (r != oboe::Result::OK) {
            ALOGE("Failed to start recording stream. Error: %s", oboe::convertToText(r));
            return cxrError_Failed;
        }
    }

    ALOGV("Trying to create Receiver at %s.", GOptions.mServerIP.c_str());
    cxrGraphicsContext context{cxrGraphicsContext_GLES};
    context.egl.display = eglGetCurrentDisplay();
    context.egl.context = eglGetCurrentContext();

    if(context.egl.context == nullptr)
    {
        ALOGV("Error, null context");
    }

    cxrClientCallbacks clientProxy = { 0 };
    clientProxy.GetTrackingState = [](void* context, cxrVRTrackingState* trackingState)
    {
        return reinterpret_cast<CloudXRClientOVR*>(context)->GetTrackingState(trackingState);
    };
    clientProxy.TriggerHaptic = [](void* context, const cxrHapticFeedback* haptic)
    {
        return reinterpret_cast<CloudXRClientOVR*>(context)->TriggerHaptic(haptic);
    };
    clientProxy.RenderAudio = [](void* context, const cxrAudioFrame *audioFrame)
    {
        return reinterpret_cast<CloudXRClientOVR*>(context)->RenderAudio(audioFrame);
    };

    // the client_lib calls into here when the async connection status changes
    clientProxy.UpdateClientState = [](void* context, cxrClientState state, cxrStateReason reason)
    {
        switch (state)
        {
            case cxrClientState_ConnectionAttemptInProgress:
                ALOGE("Connection attempt in progress..."); // log as error for visibility.
                break;
            case cxrClientState_StreamingSessionInProgress:
                ALOGE("Async connection succeeded."); // log as error for visibility.
                break;
            case cxrClientState_ConnectionAttemptFailed:
                ALOGE("Connection attempt failed. [%i]", reason);
                break;
            case cxrClientState_Disconnected:
                ALOGE("Server disconnected with reason: [%s]", StateReasonEnumToString(reason));
                break;
            default:
                ALOGV("Client state updated: %s, reason: %s", ClientStateEnumToString(state), StateReasonEnumToString(reason));
                break;
        }

        // update the state of the app, don't perform any actions here
        // the client state change will be handled in the render thread (UpdateClientState())
        reinterpret_cast<CloudXRClientOVR*>(context)->mClientState = state;
        reinterpret_cast<CloudXRClientOVR*>(context)->mClientStateReason = reason;
    };

    cxrReceiverDesc desc = { 0 };
    desc.requestedVersion = CLOUDXR_VERSION_DWORD;
    desc.deviceDesc = mDeviceDesc;
    desc.clientCallbacks = clientProxy;
    desc.clientContext = this;
    desc.shareContext = &context;
    desc.numStreams = 2;
    desc.receiverMode = cxrStreamingMode_XR;
    desc.debugFlags = GOptions.mDebugFlags;
    if(mTargetDisplayRefresh > 72.0f)
    {
        desc.debugFlags |= cxrDebugFlags_EnableAImageReaderDecoder;
    }
    desc.logMaxSizeKB = CLOUDXR_LOG_MAX_DEFAULT;
    desc.logMaxAgeDays = CLOUDXR_LOG_MAX_DEFAULT;

    cxrError err = cxrCreateReceiver(&desc, &Receiver);
    if (err != cxrError_Success)
    {
        ALOGE("Failed to create CloudXR receiver. Error %d, %s.", err, cxrErrorString(err));
        return err;
    }

    // else, good to go.
    ALOGV("Receiver created!");

    mConnectionDesc.async = cxrTrue;
    mConnectionDesc.maxVideoBitrateKbps = GOptions.mMaxVideoBitrate;
    mConnectionDesc.clientNetwork = GOptions.mClientNetwork;
    mConnectionDesc.topology = GOptions.mTopology;
    err = cxrConnect(Receiver, GOptions.mServerIP.c_str(), &mConnectionDesc);
    if (!mConnectionDesc.async)
    {
        if (err != cxrError_Success)
        {
            ALOGE("Failed to connect to CloudXR server at %s. Error %d, %s.",
                  GOptions.mServerIP.c_str(), (int)err, cxrErrorString(err));
            TeardownReceiver();
            return err;
        }
        else {
            mClientState = cxrClientState_StreamingSessionInProgress;
            mRenderState = RenderState_Running;
            ALOGV("Receiver created for server: %s", GOptions.mServerIP.c_str());
        }
    }

    return cxrError_Success; //true
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::TeardownReceiver() {
    if (playbackStream)
    {
        playbackStream->close();
    }

    if (recordingStream)
    {
        recordingStream->close();
    }

    if (Receiver) {
        cxrDestroyReceiver(Receiver);
        Receiver = nullptr;
    }
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrError CloudXRClientOVR::QueryChaperone(cxrDeviceDesc* deviceDesc) const
{
    if (mOvrSession == nullptr)
    {
        ALOGE("OVR session is null, cannot continue.");
        return cxrError_Failed; // false..
    }

    // Set arena boundary
    ovrPosef pose = {0};
    ovrVector3f scale = {0};
    ovrResult result;
    // This call fails (returns '1' not '0') when guardian is in stationary mode...
    result = vrapi_GetBoundaryOrientedBoundingBox(mOvrSession, &pose, &scale);
    if (result == ovrSuccess)
    {
        // we fall through to chaperone init/send below.
    }
    else
    {
        ALOGV("Cannot get play bounds, creating default.");
        // should clear pose in case we use in the future.
        // for now, we fill in scale with fake/default values.
        scale.x = scale.z = 1.5f * 0.5f; // use 1.5m for now -- oculus stationary bounds are tight.
    }

    // cxrChaperone chap;
    deviceDesc->chaperone.universe = cxrUniverseOrigin_Standing;
    deviceDesc->chaperone.origin.m[0][0] = deviceDesc->chaperone.origin.m[1][1] = deviceDesc->chaperone.origin.m[2][2] = 1;
    deviceDesc->chaperone.origin.m[0][1] = deviceDesc->chaperone.origin.m[0][2] = deviceDesc->chaperone.origin.m[0][3] = 0;
    deviceDesc->chaperone.origin.m[1][0] = deviceDesc->chaperone.origin.m[1][2] = deviceDesc->chaperone.origin.m[1][3] = 0;
    deviceDesc->chaperone.origin.m[2][0] = deviceDesc->chaperone.origin.m[2][1] = deviceDesc->chaperone.origin.m[2][3] = 0;
    deviceDesc->chaperone.playArea.v[0] = 2.f * scale.x;
    deviceDesc->chaperone.playArea.v[1] = 2.f * scale.z;
    ALOGV("Setting play area to %0.2f x %0.2f", deviceDesc->chaperone.playArea.v[0], deviceDesc->chaperone.playArea.v[1]);

    return cxrError_Success;
}

//-----------------------------------------------------------------------------
// Here we try to determine what controllers are attached, what class of
// device we are on.  First enumerate devices and look for Touch controllers,
// and second check the system device type to detect Quest.
// NOTE: still see some cases where BOTH checks fail.  TODO: debug further...
//-----------------------------------------------------------------------------
void CloudXRClientOVR::DetectControllers()
{
    // determine class of Oculus device
    ControllersFound = false;
    IsTouchController = true;
    uint32_t deviceIndex = 0;
    ovrInputCapabilityHeader capsHeader;
    while (vrapi_EnumerateInputDevices(mOvrSession, deviceIndex, &capsHeader) >= 0)
    {
        ++deviceIndex;
        if (capsHeader.Type == ovrControllerType_TrackedRemote)
        {
            ControllersFound = true;
            ovrInputTrackedRemoteCapabilities remoteCaps;
            remoteCaps.Header = capsHeader;
            vrapi_GetInputDeviceCapabilities(mOvrSession, &remoteCaps.Header);
            if (remoteCaps.ControllerCapabilities & ovrControllerCaps_ModelOculusGo  ||
                remoteCaps.ControllerCapabilities & ovrControllerCaps_ModelGearVR)
            {
                IsTouchController = false;
                break; // one is enough for 'global' decisions.
            }
        }
    }

    if (!ControllersFound)
    {
        ALOGE("No controllers identified!");
    }
    else
    {
        ALOGV("Oculus controller type: %s", IsTouchController ? "Touch/Quest" : "Go/Gear - UNSUPPORTED");
    }

    // we'll also double-check for Touch controllers by checking for Quest device type
    if (mJavaCtx.Vm == nullptr)
    {
        ALOGE("OVR Java context is null, cannot detect oculus device type..");
    }
    else
    {
        ovrDeviceType dtype = (ovrDeviceType) vrapi_GetSystemPropertyInt(&mJavaCtx, VRAPI_SYS_PROP_DEVICE_TYPE);
        if ( (dtype >= VRAPI_DEVICE_TYPE_OCULUSQUEST_START && dtype <= VRAPI_DEVICE_TYPE_OCULUSQUEST_END) ||
             (dtype >= VRAPI_DEVICE_TYPE_OCULUSQUEST2_START && dtype <= VRAPI_DEVICE_TYPE_OCULUSQUEST2_END) )
        {
            ALOGV("Identified that we're on Oculus Quest.");
            IsTouchController = true;
        }
    }
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
bool CloudXRClientOVR::SetupFramebuffer(GLuint colorTexture, uint32_t eye)
{
    if(Framebuffers[eye] == 0)
    {
        GLuint framebuffer;

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, colorTexture, 0);

        GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            ALOGV("Incomplete frame buffer object!");
            return false;
        }

        Framebuffers[eye] = framebuffer;

        ALOGV("Created FBO %d for eye%d texture %d.",
            framebuffer, eye, colorTexture);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Framebuffers[eye]);
    }
    else
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, Framebuffers[eye]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, colorTexture, 0);
    }

    glViewport(0, 0, EyeWidth[eye], EyeHeight[eye]);

    return true;
}

//-----------------------------------------------------------------------------
void CloudXRClientOVR::FillBackground()
{
    float cr = ((mBGColor & 0x00FF0000) >> 16) / 255.0f;
    float cg = ((mBGColor & 0x0000FF00) >> 8) / 255.0f;
    float cb = ((mBGColor & 0x000000FF) >> 255) / 255.0f;
    float ca = ((mBGColor & 0xFF000000) >> 24) / 255.0f;
    glClearColor(cr, cg, cb, ca);
    glClear(GL_COLOR_BUFFER_BIT);
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
bool CloudXRClientOVR::setBooleanButton(cxrControllerTrackingState &ctl,
                             const uint64_t &inBitfield, const OvrCxrButtonMapping &mapping)
{
    const uint64_t prevComps = ctl.booleanComps;
    const uint64_t btnMask = 1ULL << mapping.cxrId;
    bool active = false;

    // TODO: how to handle multiple ovr inputs mapping to the same cxr output!?!
    if (inBitfield & mapping.ovrId)
    {
        ctl.booleanComps |= btnMask;
        active = true;
    }
    else
    {
        ctl.booleanComps &= ~btnMask;
    }

    if (prevComps != ctl.booleanComps)
    {
        // debug logging of state of button changes.
#ifdef INPUT_LOGGING
        ALOGV("#> btn %s [%d] state change %s {%x:%x}", mapping.nameStr, mapping.cxrId,
                active ? "ACTIVE" : "inactive", (uint32_t)prevComps, ctl.booleanComps);
#endif
        return true; // we return true when button goes 'down' first frame
    }

    return false;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::ProcessControllers(float predictedTimeS)
{
    // these are Quest-specific button mappings
    const static OvrCxrButtonMapping QuestLeftRemaps[] =
    {
        { ovrButton_Enter, cxrButton_System, "BtnSystem"},
        { ovrButton_X, cxrButton_X, "BtnX" },
        { ovrButton_Y, cxrButton_Y, "BtnY" },
        { ovrButton_Trigger, cxrButton_Trigger_Click, "BtnTrig" },
    };

    const static OvrCxrButtonMapping QuestRightRemaps[] =
    {
        { ovrButton_A, cxrButton_A, "BtnA" },
        { ovrButton_B, cxrButton_B, "BtnB" },
        { ovrButton_Trigger, cxrButton_Trigger_Click, "BtnTrig" },
    };

    // these are Go/GearVR-specific button mappings
    const static OvrCxrButtonMapping GoGearButtonRemaps[] =
    {
        { ovrButton_Trigger, cxrButton_Trigger_Click, "BtnTrig" },
        // we don't do _Enter here as we do custom/manual handling of touchpad.
    };

    // For 3dof controllers, we will remap 'dpad' side press of touchpad to extra functions
    const static OvrCxrButtonMapping GoGearExtraRemaps[] =
    {
        // Note that Go/Gear generate Enter for touchpad press
        { ovrButton_Enter, cxrButton_Touchpad_Click, "BtnENTER(GoTouch)->Touch"},
        { ovrButton_Up, cxrButton_System, "BtnUP->System" },
        { ovrButton_Down, cxrButton_Grip_Click, "BtnDOWN->Grip" },
#ifdef OVERRIDE_DPAD_LR
        { ovrButton_Right, cxrButton_A, "BtnRIGHT->A" },
        { ovrButton_Left, cxrButton_B, "BtnLEFT->B" },
#endif
};

    // these are mappings referenced BY INDEX for manual remap logic
    const static OvrCxrButtonMapping OCExtraRemaps[] =
    {
        { ovrTouch_IndexTrigger, cxrButton_Trigger_Touch, "TouchTrig" },
        { ovrTouch_Joystick, cxrButton_Joystick_Touch, "TouchJoy" },
        { ovrButton_Joystick, cxrButton_Joystick_Click, "BtnJoy" },
        { ovrTouch_TrackPad, cxrButton_Touchpad_Touch, "TouchTrack" },
        { ovrButton_GripTrigger, cxrButton_Grip_Click, "BtnGrip" },

        // possible future mappings
        //{ ovrTouch_IndexPointing, cxrButton_SteamVR_Trigger_Touch ???, "TouchGrip" },
    };

    const float A2D_PRESSED = 0.4f; // scalar value at which to set digital button 'pressed'

    uint32_t deviceIndex = 0;
    ovrInputCapabilityHeader capsHeader;
    while (vrapi_EnumerateInputDevices(mOvrSession, deviceIndex, &capsHeader) >= 0)
    {
        ++deviceIndex;

        if (capsHeader.Type == ovrControllerType_TrackedRemote)
        {
            ovrInputTrackedRemoteCapabilities remoteCaps;
            remoteCaps.Header = capsHeader;
            vrapi_GetInputDeviceCapabilities(mOvrSession, &remoteCaps.Header);

            ovrInputStateTrackedRemote input;
            input.Header.ControllerType = capsHeader.Type;
            if (vrapi_GetCurrentInputState(
                    mOvrSession, capsHeader.DeviceID, &input.Header) < 0)
            {
                continue;
            }

            // Unless the predicted time is used, tracking state will not
            // be filtered and as a result view will be jumping
            // all over the place.
            ovrTracking tracking;
            if (vrapi_GetInputTrackingState(
                    mOvrSession, capsHeader.DeviceID, predictedTimeS, &tracking) < 0)
            {
                continue;
            }

            auto& controller = TrackingState.controller[
                    (remoteCaps.ControllerCapabilities&ovrControllerCaps_LeftHand)
                            ? cxrController_Left : cxrController_Right];

            // Rotate the orientation of the controller to match the Quest pose with the Touch SteamVR model
            const float QUEST_TO_TOUCH_ROT = 0.45f; // radians
            controller.pose = ConvertPose(tracking.HeadPose, QUEST_TO_TOUCH_ROT);

            controller.pose.deviceIsConnected = cxrTrue;
            controller.pose.trackingResult = cxrTrackingResult_Running_OK;

            // stash current state of booleanComps, to evaluate at end of fn for changes
            // in state this frame.
            const uint64_t priorCompsState = controller.booleanComps;

            // clear changed flags for this pass
            controller.booleanCompsChanged = 0;

            // Handle ALL the button remaps in one quick loop up front, in case further
            // code wants to override any values.
            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_ModelOculusTouch)
            {
                if (remoteCaps.ControllerCapabilities&ovrControllerCaps_LeftHand)
                    for (auto &mapping : QuestLeftRemaps)
                    {
                        if (mapping.cxrId == cxrButton_Num) continue;
                        setBooleanButton(controller, input.Buttons, mapping);
                    }
                else
                    for (auto &mapping : QuestRightRemaps)
                    {
                        if (mapping.cxrId == cxrButton_Num) continue;
                        setBooleanButton(controller, input.Buttons, mapping);
                    }
            }
            else
            {
                for (auto &mapping : GoGearButtonRemaps)
                {
                    if (mapping.cxrId == cxrButton_Num) continue;
                    setBooleanButton(controller, input.Buttons, mapping);
                }
            }

            // Analog trigger gets passed through direct
            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_HasAnalogIndexTrigger)
            { // Quest case essentially.
                controller.scalarComps[cxrAnalog_Trigger] = input.IndexTrigger;
                setBooleanButton(controller, input.Touches, OCExtraRemaps[0]);
            }
            else
            { // 3dof digital trigger, we fake scalar as for 3dofs we emulate vive
                if (controller.booleanComps & (1ULL << cxrButton_Trigger_Click))
                    controller.scalarComps[cxrAnalog_Trigger] = 1.0f;
                else
                    controller.scalarComps[cxrAnalog_Trigger] = 0.0f;
            }

            // Analog grip trigger gets passed through direct, button click handled already.
            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_HasAnalogGripTrigger)
            {
                controller.scalarComps[cxrAnalog_Grip] = input.GripTrigger;
            }
            // else digital button already handled.

            // go/gear 3dof trackpad handling
            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_HasTrackpad)
            {
                // remap touch from ovr coord to -1,1 expected vector, flip Y axis, to match vive
                float x = (2.f*input.TrackpadPosition.x/static_cast<float>(remoteCaps.TrackpadMaxX)) - 1.f;
                float y = 1.f - (2.f*input.TrackpadPosition.y/static_cast<float>(remoteCaps.TrackpadMaxY));
                controller.scalarComps[cxrAnalog_TouchpadX] = x;
                controller.scalarComps[cxrAnalog_TouchpadY] = y;
                // Pass along 'touched' status
                setBooleanButton(controller, input.Touches, OCExtraRemaps[3]);

                if (!GOptions.mBtnRemap)
                {
                    // if button remaps disables, just handle main touchpad-click
                    setBooleanButton(controller, input.Buttons, GoGearExtraRemaps[0]);
                }
                else
                {
                    // For 3dof remotes, we look at touchpad position, and remap side clicks (as Enter)
                    // to 'dpad' events, but leave center area clicks as normal (Enter) press.
                    uint64_t fakemask = input.Buttons;
                    // clear all the button states, will set them again as determined below.
                    fakemask &= ~(ovrButton_Enter & ovrButton_Up & ovrButton_Down &
                                  ovrButton_Right & ovrButton_Left);
                    const float TOUCH_DPAD_CUTOFF = 0.55f; // esp needed for how thumb covers touchpad
                    if (input.Buttons & ovrButton_Enter)
                    {
                        if (x > TOUCH_DPAD_CUTOFF || x < -TOUCH_DPAD_CUTOFF ||
                            y > TOUCH_DPAD_CUTOFF || y < -TOUCH_DPAD_CUTOFF)
                        {
                            if (fabs(y) > fabs(x)) // handle up/down
                            {
                                if (y > 0) fakemask |= ovrButton_Up;
                                else fakemask |= ovrButton_Down;
                            }
                            else // handle right/left
                            {
#ifdef OVERRIDE_DPAD_LR
                                if (x>0) fakemask |= ovrButton_Right;
                                else fakemask |= ovrButton_Left;
#else
                                fakemask |= ovrButton_Enter;
#endif
                            }
                        }
                        else
                            fakemask |= ovrButton_Enter;
                    }

                    // Then we apply GoGearExtraRemaps to remap the above-generated buttons into
                    // specific functionality using remap bindings set at top of fn.
                    for (auto &mapping : GoGearExtraRemaps)
                    {
                        if (setBooleanButton(controller, fakemask, mapping))
                        {
#ifdef DEBUG_LOGGING
                            ALOGV("###> @ %0.2f, %0.2f", x, y);
#endif
                        }
                    }
                }
            }
            else // we assume either trackpad OR joystick right now.
            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_HasJoystick)
            { // pass joystick position, touch, press through as-is.
                controller.scalarComps[cxrAnalog_JoystickX] = input.Joystick.x;
                controller.scalarComps[cxrAnalog_JoystickY] = input.Joystick.y;

                setBooleanButton(controller, input.Touches, OCExtraRemaps[1]);
                setBooleanButton(controller, input.Buttons, OCExtraRemaps[2]);
            }

            // update changed flags based on change in comps, as XOR of prior state and new state.
            controller.booleanCompsChanged = priorCompsState ^ controller.booleanComps;
        }
    }
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrTrackedDevicePose CloudXRClientOVR::ConvertPose(
        const ovrRigidBodyPosef& inPose, float rotationX)
{
    ovrMatrix4f transform = vrapi_GetTransformFromPose(&inPose.Pose);

    if(rotationX)
    {
        const ovrMatrix4f rotation = ovrMatrix4f_CreateRotation( rotationX, 0, 0 );
        transform = ovrMatrix4f_Multiply( &transform, &rotation );
    }

    cxrTrackedDevicePose pose{};
    cxrMatrix34 m = cxrConvert(transform);
    cxrMatrixToVecQuat(&m, &pose.position, &pose.rotation);
    pose.velocity = cxrConvert(inPose.LinearVelocity);
    pose.angularVelocity = cxrConvert(inPose.AngularVelocity);

    pose.poseIsValid = cxrTrue;

    return pose;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::DoTracking(float predictedTimeS)
{
    ProcessControllers(predictedTimeS);

    ovrTracking2_ tracking = vrapi_GetPredictedTracking2(mOvrSession, predictedTimeS);

    TrackingState.hmd.ipd = vrapi_GetInterpupillaryDistance(&tracking);
    // the quest2 ipd sensor reports infinitesimal changes every frame, even when the user has not adjusted the headset IPD
    // so we truncate the value to 5 decimal places (sub-millimeter precision)
    TrackingState.hmd.ipd = truncf(TrackingState.hmd.ipd * 10000.0f) / 10000.0f;
    TrackingState.hmd.flags = 0; // reset dynamic flags every frame
    TrackingState.hmd.flags |= cxrHmdTrackingFlags_HasIPD; // TODO: consider tracking local IPD value and only flag when it actually changes...

    if (mRefreshChanged)
    {
        TrackingState.hmd.displayRefresh = std::fminf(mTargetDisplayRefresh, 90.0f);
        TrackingState.hmd.flags |= cxrHmdTrackingFlags_HasRefresh;
        // TODO: should we have this mutex protected so no race condition on it?
        mRefreshChanged = false;
    }

    mLastHeadPose = tracking.HeadPose;
    TrackingState.hmd.pose = ConvertPose(tracking.HeadPose);
    TrackingState.hmd.pose.poseIsValid = ((tracking.Status & VRAPI_TRACKING_STATUS_ORIENTATION_VALID) > 0) ? cxrTrue : cxrFalse;
    TrackingState.hmd.pose.deviceIsConnected = ((tracking.Status & VRAPI_TRACKING_STATUS_HMD_CONNECTED) > 0) ? cxrTrue : cxrFalse;
    TrackingState.hmd.pose.trackingResult = cxrTrackingResult_Running_OK;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::GetTrackingState(cxrVRTrackingState* trackingState)
{
    // Unless the predicted time is used, tracking state will not be
    // filtered and as a result view will be jumping all over the place.
    const double predictedTimeS = GetTimeInSeconds() + 0.004f;
    // TODO: look into replacing this with mNextDisplayTime so tracking closer to real scanout

    DoTracking(predictedTimeS);
    if (trackingState != nullptr)
        *trackingState = TrackingState;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrDeviceDesc CloudXRClientOVR::GetDeviceDesc(float fovX, float fovY)
{
    cxrDeviceDesc desc = {};
    if (mJavaCtx.Vm == nullptr)
    {
        ALOGE("Java context is null.");
        return desc;
    }

    desc.deliveryType = cxrDeliveryType_Stereo_RGB;

    int texW = vrapi_GetSystemPropertyInt( &mJavaCtx, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH );
    int texH = vrapi_GetSystemPropertyInt( &mJavaCtx, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT );
    int dispW = vrapi_GetSystemPropertyInt( &mJavaCtx, VRAPI_SYS_PROP_DISPLAY_PIXELS_WIDE );
    int dispH = vrapi_GetSystemPropertyInt( &mJavaCtx, VRAPI_SYS_PROP_DISPLAY_PIXELS_HIGH );

    // get the rate the system is running at right now.
    // TODO: should this be a Float property??
    const int currDisplayRefresh = vrapi_GetSystemPropertyInt( &mJavaCtx, VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE );
    ALOGV("System property says current display refresh set to %d", currDisplayRefresh);

    // TODO: atm we ignore current rate, and use a hardcoded default (72hz)
    // TODO: we may want to switch this to assign currDisplayRefresh instead, as that might be 90 at some point...
    mTargetDisplayRefresh = cDefaultDisplayRefresh; // this will be our fallback value.

    if (GOptions.mRequestedRefreshRate <= 0.0f)
    { // leave as default for now
        ALOGV("Override for display refresh not specified, so defaulting to %0.2f", mTargetDisplayRefresh);
        // TODO: we may down the line want to use this case to clamp to something other than mRequested/cDefault.
        //  for example, if no override given, maybe we should try to set to current?
    }
    else
    { // some refresh was requested.
        // query how many different display rates are supported by the system, then get an array of them.
        const int numRates = vrapi_GetSystemPropertyInt(&mJavaCtx,
                                                  VRAPI_SYS_PROP_NUM_SUPPORTED_DISPLAY_REFRESH_RATES);
        std::vector<float> supportedRates(numRates);
        int retNumRates = vrapi_GetSystemPropertyFloatArray(&mJavaCtx,
                                                            VRAPI_SYS_PROP_SUPPORTED_DISPLAY_REFRESH_RATES,
                                                            supportedRates.data(), numRates);

        // if we find more rates available, we look to match requested rate as close as possible.
        // NOTE: this does allow for 120hz mode if enabled on a system and is the requested rate.

        // TODO: Note that CloudXR does NOT currently, actively support 120hz.  You can request it, but no guarantee it works smoothly...

        // TODO: We may want the option and checks in float, as often display rates are not perfect integers.
        //  For example, display mode might might be 59.97, and request is 60, so we will round and match, but setting
        //  might not work.  Might need to stash the floating point value we 'matched' for future reference at minimum.
        if (numRates >= 1) // try to find a matching rate to launch option
        {
            ALOGE("Launch options requested display refreh of %u, checking list of %d available rates",
                  (GOptions.mRequestedRefreshRate), numRates);

            // for debugging, we'll print the list of supportedRates
            ALOGV("Display support rates of:");
            for (auto rate : supportedRates)
            {
                ALOGV("Refresh = %0.2f hz", rate);
            }

            // then we loop through the rate list to find something 'close enough' (less than 1hz from request).
            for (auto rate : supportedRates)
            {
                // try for closest int value for now (within 1hz seems fine), can update to floats all over if needed
                if (abs(rate - (float)(GOptions.mRequestedRefreshRate)) < 1.0f)
                { // found what we wanted -- update variable.
                    mTargetDisplayRefresh = rate;
                    ALOGE("Choosing closest display rate of %0.2f", rate);
                    break;
                }
            }
        }
    }

    if (mOvrSession == nullptr)
    {
        ALOGE("OVR session is null, cannot continue.");
        return {}; // false..
    }

    ALOGV("Setting display rate to %0.2f hz.", mTargetDisplayRefresh);
    ovrResult result = vrapi_SetDisplayRefreshRate(mOvrSession, mTargetDisplayRefresh);
    if (result != ovrSuccess)
    {
        // there are two known cases called out by the api header.
        if (result == ovrError_InvalidParameter) // rate not supported
        {
            ALOGE("Unable to set display rate to 0.2f, unsupported rate.");
        }
        else
        if (result == ovrError_InvalidOperation) // rate can't be set right now -- like low power mode
        {
            ALOGE("Unable to set display rate to 0.2f at this time (may be in low power mode?)");
        }

        // I think the right thing here is to reset the member to default value...
        // TODO: reminder if we change to assigning current refresh, this line will want to match init code.
        mTargetDisplayRefresh = cDefaultDisplayRefresh;
    }
    ALOGV("vrapi HMD Props, texture = %d x %d, display = %d x %d @ %0.2f", texW, texH, dispW, dispH, mTargetDisplayRefresh);

//#define DISP_RES_OCULUS_SUGGESTED
#ifdef DISP_RES_OCULUS_SUGGESTED
    // This is using the suggested texture size for optimal perf per Oculus sdk design
    desc.width = texW;
    desc.height = texH;
#else
    // TODO: This is trying to use display-native per eye w/h instead.
    desc.width = dispW/2;
    desc.height = dispH;
#endif

    desc.maxResFactor = GOptions.mMaxResFactor;

    const int maxWidth = (int)(desc.maxResFactor * (float)desc.width);
    const int maxHeight = (int)(desc.maxResFactor * (float)desc.height);
    ALOGV("HMD size requested as %d x %d, max %d x %d", desc.width, desc.height, maxWidth, maxHeight);

    desc.fps = mTargetDisplayRefresh;

    // Get IPD from OVR API
    const double predictedDisplayTime = vrapi_GetPredictedDisplayTime( mOvrSession, 0 );
    const ovrTracking2 tracking = vrapi_GetPredictedTracking2( mOvrSession, predictedDisplayTime );
    desc.ipd = vrapi_GetInterpupillaryDistance(&tracking);

    desc.predOffset = 0.02f;
    desc.receiveAudio = GOptions.mReceiveAudio;
    desc.sendAudio = GOptions.mSendAudio;
    desc.posePollFreq = 0;
    desc.disablePosePrediction = false;
    desc.angularVelocityInDeviceSpace = false;
    desc.foveatedScaleFactor = (GOptions.mFoveation < 100) ? GOptions.mFoveation : 0;
    // if we have touch controller use Oculus type, else use Vive as more close to 3dof remotes
    desc.ctrlType = IsTouchController ? cxrControllerType_OculusTouch : cxrControllerType_HtcVive;

    const float halfFOVTanX = tanf(VRAPI_PI/360.f * fovX);
    const float halfFOVTanY = tanf(VRAPI_PI/360.f * fovY);

    desc.proj[0][0] = -halfFOVTanX;
    desc.proj[0][1] =  halfFOVTanX;
    desc.proj[0][2] = -halfFOVTanY;
    desc.proj[0][3] =  halfFOVTanY;
    desc.proj[1][0] = -halfFOVTanX;
    desc.proj[1][1] =  halfFOVTanX;
    desc.proj[1][2] = -halfFOVTanY;
    desc.proj[1][3] =  halfFOVTanY;
    QueryChaperone(&desc);
    return desc;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::RecreateSwapchain(uint32_t width, uint32_t height, uint32_t eye)
{
    ALOGV("Recreating swapchain for eye%d: %d x %d (was %d x %d)",
          eye, width, height, EyeWidth[eye], EyeHeight[eye]);

    if (SwapChains[eye])
        vrapi_DestroyTextureSwapChain(SwapChains[eye]);

    SwapChains[eye] = vrapi_CreateTextureSwapChain2(VRAPI_TEXTURE_TYPE_2D,
                                                    VRAPI_TEXTURE_FORMAT_8888_sRGB,
                                                    width, height,
                                                    1, SwapChainLen);
    EyeWidth[eye] = width;
    EyeHeight[eye] = height;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::TriggerHaptic(
       const cxrHapticFeedback* hapticFeedback)
{
    const cxrHapticFeedback& haptic = *hapticFeedback;

    if (haptic.seconds <= 0)
        return;

    uint32_t deviceIndex = 0;
    ovrInputCapabilityHeader capsHeader;
    while (vrapi_EnumerateInputDevices(mOvrSession, deviceIndex, &capsHeader) >= 0)
    {
        ++deviceIndex;

        if (capsHeader.Type == ovrControllerType_TrackedRemote)
        {
            ovrInputTrackedRemoteCapabilities remoteCaps;
            remoteCaps.Header = capsHeader;
            vrapi_GetInputDeviceCapabilities(mOvrSession, &remoteCaps.Header);

            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_LeftHand &&
                    haptic.controllerIdx == cxrController_Right)
            {
                continue;
            }

            if (remoteCaps.ControllerCapabilities&ovrControllerCaps_RightHand &&
                    haptic.controllerIdx == cxrController_Left)
            {
                continue;
            }

            if (0 == (remoteCaps.ControllerCapabilities&
                    ovrControllerCaps_HasBufferedHapticVibration))
            {
                continue;
            }

            ovrHapticBuffer hapticBuffer;
            hapticBuffer.BufferTime = GetTimeInSeconds() + 0.03; // TODO: use mNextDisplayTime?
            hapticBuffer.NumSamples = remoteCaps.HapticSamplesMax;
            hapticBuffer.HapticBuffer =
                    reinterpret_cast<uint8_t*>(alloca(remoteCaps.HapticSamplesMax));
            hapticBuffer.Terminated = true;

            for (uint32_t i = 0; i < hapticBuffer.NumSamples; i++)
            {
                hapticBuffer.HapticBuffer[i] =
                        static_cast<uint8_t>(haptic.amplitude*255.f);
            }

            vrapi_SetHapticVibrationBuffer(mOvrSession, capsHeader.DeviceID, &hapticBuffer);
        }
    }
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
cxrBool CloudXRClientOVR::RenderAudio(const cxrAudioFrame *audioFrame)
{
    if (!playbackStream)
    {
        return cxrFalse;
    }

    const uint32_t timeout = audioFrame->streamSizeBytes / CXR_AUDIO_BYTES_PER_MS;
    const uint32_t numFrames = timeout * CXR_AUDIO_SAMPLING_RATE / 1000;
    playbackStream->write(audioFrame->streamBuffer, numFrames, timeout * oboe::kNanosPerMillisecond);

    return cxrTrue;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::SubmitLayers(const ovrLayerHeader2* layers[], int layerCount, ovrFrameFlags flags)
{
    ovrSubmitFrameDescription2 frameDesc = {0};
    frameDesc.LayerCount = layerCount;
    frameDesc.Layers = layers;
    frameDesc.Flags = flags;
    frameDesc.SwapInterval = 1;
    frameDesc.FrameIndex = mFrameCounter;
    frameDesc.DisplayTime = mNextDisplayTime;
    vrapi_SubmitFrame2(mOvrSession, &frameDesc);
}


//-----------------------------------------------------------------------------
// Here we render the loading 'spinner' while we're starting up.
//-----------------------------------------------------------------------------
void CloudXRClientOVR::RenderLoadScreen()
{
    GetTrackingState(NULL);
    ovrLayerProjection2 blackLayer = vrapi_DefaultLayerBlackProjection2();
    blackLayer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER;
    ovrLayerLoadingIcon2 iconLayer = vrapi_DefaultLayerLoadingIcon2();
    iconLayer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER;
    const ovrLayerHeader2* layers[] = { &blackLayer.Header, &iconLayer.Header };
    SubmitLayers(layers, 2, VRAPI_FRAME_FLAG_FLUSH);

}


//-----------------------------------------------------------------------------
// Clear to black for exit, and tell VrApi we are done submitting frames.
//-----------------------------------------------------------------------------
void CloudXRClientOVR::RenderExitScreen()
{
    GetTrackingState(NULL);
    ovrLayerProjection2 layer = vrapi_DefaultLayerBlackProjection2();
    layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER;
    const ovrLayerHeader2* layers[] = { &layer.Header };
    SubmitLayers(layers, 1,
                 static_cast<ovrFrameFlags>(VRAPI_FRAME_FLAG_FLUSH | VRAPI_FRAME_FLAG_FINAL));
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::Render()
{
    // This is the only place the frame index is incremented, right before
    // calling vrapi_GetPredictedDisplayTime().
    mFrameCounter++;
    mNextDisplayTime = vrapi_GetPredictedDisplayTime(mOvrSession, mFrameCounter);

    ovrLayerProjection2 worldLayer = vrapi_DefaultLayerProjection2();

    worldLayer.HeadPose = mLastHeadPose;
    worldLayer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION;

    // Fetch a CloudXR frame
    cxrFramesLatched framesLatched;
    const uint32_t timeoutMs = 500;
    bool frameValid = false;

    if (Receiver)
    {
        if (mClientState == cxrClientState_StreamingSessionInProgress)
        {
            cxrError frameErr = cxrLatchFrame(Receiver, &framesLatched,
                                            cxrFrameMask_All, timeoutMs);
            frameValid = (frameErr == cxrError_Success);
            if (!frameValid)
            {
                if (frameErr == cxrError_Frame_Not_Ready)
                {
                    ALOGV("LatchFrame failed, frame not ready for %d ms", timeoutMs);
                }
                else
                if (frameErr == cxrError_Receiver_Not_Running)
                {
                    ALOGE("LatchFrame: Receiver no longer running, exiting.");
                    RequestExit();
                }
                else
                {
                    ALOGE("Error in LatchFrame [%0d] = %s", frameErr, cxrErrorString(frameErr));
                }
            }
        }
    }

    for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++)
    {
        // if valid frame and the size has changed, update our buffers to match.
        // TODO: we might want to use a frame subrect if the buffer is BIGGER and we're shrinking,
        //  just to avoid allocation thrash and hiccups due to it.
        cxrVideoFrame &vf = framesLatched.frames[eye];
        if (frameValid && (vf.widthFinal != EyeWidth[eye] || vf.heightFinal != EyeHeight[eye]) )
            RecreateSwapchain(vf.widthFinal, vf.heightFinal, eye);

        const uint32_t swapChainLength = vrapi_GetTextureSwapChainLength(SwapChains[eye]);
        const int swapChainIndex = mFrameCounter % swapChainLength;
        const GLuint colorTexture = vrapi_GetTextureSwapChainHandle(SwapChains[eye], swapChainIndex);

        if (SetupFramebuffer(colorTexture, eye))
        {
            if (frameValid)
            {
                // blit streamed frame into the world layer
                cxrBlitFrame(Receiver, &framesLatched, 1<<eye);
            }
            else
            {
                FillBackground();
            }

            // NOTE: this is where a given app might render UI/overlays
        }

        worldLayer.Textures[eye].ColorSwapChain = SwapChains[eye];
        worldLayer.Textures[eye].SwapChainIndex = swapChainIndex;
        worldLayer.Textures[eye].TexCoordsFromTanAngles = TexCoordsFromTanAngles;
    }

    if (frameValid) // means we had a receiver AND latched frame.
    {
        worldLayer.HeadPose.Pose.Orientation = cxrToQuaternion(framesLatched.poseMatrix);
        worldLayer.HeadPose.Pose.Position = cxrGetTranslation(framesLatched.poseMatrix);

        cxrReleaseFrame(Receiver, &framesLatched);

        // Log connection stats every 3 seconds
        const int STATS_INTERVAL_SEC = 3;
        mFramesUntilStats--;
        if (mFramesUntilStats <= 0 &&
            cxrGetConnectionStats(Receiver, &mStats) == cxrError_Success)
        {
            // Capture the key connection statistics
            char statsString[64] = { 0 };
            snprintf(statsString, 64, "FPS: %6.1f    Bitrate (kbps): %5d    Latency (ms): %3d", mStats.framesPerSecond, mStats.bandwidthUtilizationKbps, mStats.roundTripDelayMs);

            // Turn the connection quality into a visual representation along the lines of a signal strength bar
            char qualityString[64] = { 0 };
            snprintf(qualityString, 64, "Connection quality: [%c%c%c%c%c]",
                     mStats.quality >= cxrConnectionQuality_Bad ? '#' : '_',
                     mStats.quality >= cxrConnectionQuality_Poor ? '#' : '_',
                     mStats.quality >= cxrConnectionQuality_Fair ? '#' : '_',
                     mStats.quality >= cxrConnectionQuality_Good ? '#' : '_',
                     mStats.quality == cxrConnectionQuality_Excellent ? '#' : '_');

            // There could be multiple reasons for low quality however we show only the most impactful to the end user here
            char reasonString[64] = { 0 };
            if (mStats.quality <= cxrConnectionQuality_Fair)
            {
                if (mStats.qualityReasons == cxrConnectionQualityReason_EstimatingQuality)
                {
                    snprintf(reasonString, 64, "Reason: Estimating quality");
                }
                else if (mStats.qualityReasons & cxrConnectionQualityReason_HighLatency)
                {
                    snprintf(reasonString, 64, "Reason: High Latency (ms): %3d", mStats.roundTripDelayMs);
                }
                else if (mStats.qualityReasons & cxrConnectionQualityReason_LowBandwidth)
                {
                    snprintf(reasonString, 64, "Reason: Low Bandwidth (kbps): %5d", mStats.bandwidthAvailableKbps);
                }
                else if (mStats.qualityReasons & cxrConnectionQualityReason_HighPacketLoss)
                {
                    if (mStats.totalPacketsLost == 0)
                    {
                        snprintf(reasonString, 64, "Reason: High Packet Loss (Recoverable)");
                    }
                    else
                    {
                        snprintf(reasonString, 64, "Reason: High Packet Loss (%%): %3.1f", 100.0f * mStats.totalPacketsLost / mStats.totalPacketsReceived);
                    }
                }
            }

            ALOGV("%s    %s    %s", statsString, qualityString, reasonString);
            mFramesUntilStats = (int)mStats.framesPerSecond * STATS_INTERVAL_SEC;
        }
    }

    const ovrLayerHeader2* layers[] = { &worldLayer.Header };
    SubmitLayers(layers, 1, static_cast<ovrFrameFlags>(0));
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::AppResumed()
{
    if (mOvrSession == nullptr)
    {
        ALOGE("OVR session is null, cannot continue.");
        RequestExit();
        return; // false..
    }

    // Force floor level tracking space
    vrapi_SetTrackingSpace(mOvrSession, VRAPI_TRACKING_SPACE_LOCAL_FLOOR);

    // set our color space
    // NOTE: oculus color space guide says to use CV1 and NOT rec709, even if 709 seems correct choice.
    ovrHmdColorDesc colorspace{VRAPI_COLORSPACE_RIFT_CV1};
    vrapi_SetClientColorDesc(mOvrSession, &colorspace);

    float EyeFovDegreesX = vrapi_GetSystemPropertyFloat(&mJavaCtx,
            VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X);
    float EyeFovDegreesY = vrapi_GetSystemPropertyFloat(&mJavaCtx,
            VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_Y);

    ALOGV("Headset suggested FOV: %.1f x %.1f.", EyeFovDegreesX, EyeFovDegreesY);

    const auto projectionMatrix = ovrMatrix4f_CreateProjectionFov(
            EyeFovDegreesX, EyeFovDegreesY, 0.0f, 0.0f, VRAPI_ZNEAR, 0.0f);

    TexCoordsFromTanAngles =
            ovrMatrix4f_TanAngleMatrixFromProjection(&projectionMatrix);

    // get controller state and HMD state up-front now.
    DetectControllers();
    mDeviceDesc = GetDeviceDesc(EyeFovDegreesX, EyeFovDegreesY);

    // create the initial swapchain buffers based on HMD specs.
    for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++)
        RecreateSwapchain(mDeviceDesc.width, mDeviceDesc.height, eye);

    // TODO: move this to a once-per-frame check like wvr sample does in its UpdatePauseLogic fn.
    if (!Receiver && mReadyToConnect &&
        CreateReceiver() != cxrError_Success)
    {
        ALOGE("Failed to create the receiver, exiting...");
        RequestExit();
        return;
    }
    else // precondition or CreateReceiver failed test
    {
        // if connecting async, state management happens in callback.
        // but if sync, we reach this point after successful Connect
        // so update renderstate.
        if (!mConnectionDesc.async)
        {
            mRenderState = RenderState_Running;
        }
    }

    // now match variable state
    mWasPaused = mIsPaused;

}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::AppPaused() {
    // TODO verify whether we need a mutex around resources here to ensure some thread isn't
    //  rendering actively while we're pausing...
    ALOGV("App Paused");

    for (int i = 0; i < NumEyes; ++i)
    {
        glDeleteFramebuffers(1, &Framebuffers[i]);
        Framebuffers[i] = 0;
    }

    if (Receiver)
    {
        TeardownReceiver();
        if (mClientState != cxrClientState_Exiting)
        {
            mClientState = cxrClientState_ReadyToConnect;
            mRenderState = RenderState_Loading;
            ALOGV("Receiver destroyed, client state reset.");
        }
    }

    for (auto& swapChain : SwapChains)
    {
        if (swapChain != nullptr)
            vrapi_DestroyTextureSwapChain(swapChain);
        swapChain = nullptr;
    }

    EyeWidth[0] = EyeWidth[1] = EyeHeight[0] = EyeHeight[1] = 0;
    TrackingState = {};

    // now match variable state
    mWasPaused = mIsPaused;
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
bool CloudXRClientOVR::EnterVRMode()
{
    if (mOvrSession == NULL) {
        ovrModeParms parms = vrapi_DefaultModeParms(&mJavaCtx);

        // Note for future from ovr sdk: don't need to reset FS flag when using a View
        parms.Flags &= ~VRAPI_MODE_FLAG_RESET_WINDOW_FULLSCREEN;
        parms.Flags |= VRAPI_MODE_FLAG_FRONT_BUFFER_SRGB; // bc we have full SRGB pipeline
        parms.Flags |= VRAPI_MODE_FLAG_NATIVE_WINDOW; // bc we're using NativeWindow
        parms.WindowSurface = (size_t)GetWindow(); // the NativeWindow
        parms.Display = (size_t)(mEglHelper.GetDisplay());
        parms.ShareContext = (size_t)(mEglHelper.GetContext());

        ALOGV("        eglGetCurrentSurface( EGL_DRAW ) = %p", eglGetCurrentSurface(EGL_DRAW));
        ALOGV("        vrapi_EnterVrMode()");
        mOvrSession = vrapi_EnterVrMode(&parms);
        ALOGV("        eglGetCurrentSurface( EGL_DRAW ) = %p", eglGetCurrentSurface(EGL_DRAW));

        // If entering VR mode failed then the ANativeWindow was not valid.
        if (mOvrSession == NULL) {
            ALOGE("EnterVrMode failed, assuming invalid ANativeWindow (%p)!", GetWindow());
            return false;
        }

        // Set performance parameters once we have entered VR mode and have a valid ovrMobile.
        if (mOvrSession != NULL) {
            vrapi_SetClockLevels(mOvrSession, CPU_LEVEL, GPU_LEVEL);
            ALOGV("		vrapi_SetClockLevels( %d, %d )", CPU_LEVEL, GPU_LEVEL);

            vrapi_SetPerfThread(mOvrSession, VRAPI_PERF_THREAD_TYPE_MAIN, gettid());
            ALOGV("		vrapi_SetPerfThread( MAIN, %d )", gettid());

//            vrapi_SetPerfThread(mOvrSession, VRAPI_PERF_THREAD_TYPE_RENDERER, gettid());
//            ALOGV("		vrapi_SetPerfThread( RENDERER, %d )", gettid());
        }
    }

    return true;  // we have ovr session.
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CloudXRClientOVR::HandleVrModeChanges()
{
    if (mIsPaused==mWasPaused) // nothing to do then.
        return;

    if (!mIsPaused && mNativeWindow != NULL && mClientState != cxrClientState_Exiting)
    {
        if (!EnterVRMode())
        {
            // might need to notify user as well here.
            ALOGE("Failed to enter VR mode, exiting...");
            RequestExit();
        }
        else
        {
            // then run app-layer resume code.
            AppResumed();
        }
    }
    else
    {
        if (mIsPaused || mClientState == cxrClientState_Exiting)
        {
            if (mOvrSession != NULL)
            {
                ALOGV("CALLING vrapi_LeaveVrMode()");
                vrapi_LeaveVrMode(mOvrSession);
                mOvrSession = NULL;
            }
            // app-layer pause code
            AppPaused();
        }
    }
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
oboe::DataCallbackResult CloudXRClientOVR::onAudioReady(oboe::AudioStream *oboeStream,
        void *audioData, int32_t numFrames)
{
    cxrAudioFrame recordedFrame{};
    recordedFrame.streamBuffer = (int16_t*)audioData;
    recordedFrame.streamSizeBytes = numFrames * CXR_AUDIO_CHANNEL_COUNT * CXR_AUDIO_SAMPLE_SIZE;
    cxrSendAudio(Receiver, &recordedFrame);

    return oboe::DataCallbackResult::Continue;
}

//-----------------------------------------------------------------------------
// TODO: we may need to handle these events for better app lifecycle.
//-----------------------------------------------------------------------------
void CloudXRClientOVR::HandleVrApiEvents()
{
    ovrEventDataBuffer eventDataBuffer = {};

    // Poll for VrApi events
    for (;;) {
        ovrEventHeader* eventHeader = (ovrEventHeader*)(&eventDataBuffer);
        ovrResult res = vrapi_PollEvent(eventHeader);
        if (res != ovrSuccess) {
            break;
        }

        switch (eventHeader->EventType) {
            case VRAPI_EVENT_DATA_LOST:
                ALOGV("vrapi_PollEvent: Received VRAPI_EVENT_DATA_LOST");
                break;
            case VRAPI_EVENT_VISIBILITY_GAINED:
                ALOGV("vrapi_PollEvent: Received VRAPI_EVENT_VISIBILITY_GAINED");
                break;
            case VRAPI_EVENT_VISIBILITY_LOST:
                ALOGV("vrapi_PollEvent: Received VRAPI_EVENT_VISIBILITY_LOST");
                break;
            case VRAPI_EVENT_FOCUS_GAINED:
                // FOCUS_GAINED is sent when the application is in the foreground and has
                // input focus. This may be due to a system overlay relinquishing focus
                // back to the application.
                // TODO: to be implemented...
                ALOGV("vrapi_PollEvent: Received VRAPI_EVENT_FOCUS_GAINED");
                break;
            case VRAPI_EVENT_FOCUS_LOST:
                // FOCUS_LOST is sent when the application is no longer in the foreground and
                // therefore does not have input focus. This may be due to a system overlay taking
                // focus from the application. The application should take appropriate action when
                // this occurs.
                // TODO: to be implemented...
                ALOGV("vrapi_PollEvent: Received VRAPI_EVENT_FOCUS_LOST");
                break;
            case VRAPI_EVENT_DISPLAY_REFRESH_RATE_CHANGE:
            {
                // TODO: consider if we should wrap with a mutex, unclear which thread things occur on, and changed flag wants to be protected.
                ovrEventDisplayRefreshRateChange *rrc = reinterpret_cast<ovrEventDisplayRefreshRateChange*>(eventHeader);
                ALOGV("vrapi_PollEvent: Received VRAPI_EVENT_DISPLAY_REFRESH_RATE_CHANGE");
                ALOGV("Refresh changing from %0.2f to %0.2f", rrc->fromDisplayRefreshRate, rrc->toDisplayRefreshRate);
                // update the member as rate changed under the covers already.
                mTargetDisplayRefresh = rrc->toDisplayRefreshRate;
                // flag to system so next pose update includes this change
                mRefreshChanged = true;

                // get the rate the system thinks it is running at right now.
                // TODO: should this be a Float property??
                const int currDisplayRefresh = vrapi_GetSystemPropertyInt( &mJavaCtx, VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE );
                ALOGV("REFRESH CHANGED! API now returns display refresh as %d", currDisplayRefresh);

                break;
            }
            default:
                ALOGV("vrapi_PollEvent: Unknown event");
                break;
        }
    }

    // now seems as good a time as any to cache state of the hmd
    mHeadsetOnHead = (vrapi_GetSystemStatusInt(&mJavaCtx, VRAPI_SYS_STATUS_MOUNTED) != VRAPI_FALSE);
}
//-----------------------------------------------------------------------------
// we assume here env!=null.
//-----------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL
Java_com_valiventures_cloudxr_ovr_MainActivity_nativeHandleLaunchOptions(JNIEnv* env, jobject act, jstring jcmdline)
{
    std::lock_guard<std::mutex> lock(GJniMutex);

    // acquire any cmdline from java
    std::string cmdline = "";
    if (jcmdline != nullptr) {
        const char *utfstr = env->GetStringUTFChars(jcmdline, 0);
        if (utfstr != nullptr) {
            ALOGV("Commandline received from Java: %s", utfstr);
            cmdline = utfstr;
            env->ReleaseStringUTFChars(jcmdline, utfstr);
        }
    }

    // first, try to read "command line in a text file"
    GOptions.ParseFile("/sdcard/CloudXRLaunchOptions.txt");
    // next, process actual 'commandline' args -- overrides any prior values
    GOptions.ParseString(cmdline);

    // check if we have a server yet (if have no 'input UI', we have no other source)
    if (!GOptions.mServerIP.empty())
    {
        gClientHandle->SetReadyToConnect(true);
    }
    else
    {
        // if we have no server, then we need to wait for input UI to provide one.
        ALOGV("No server specified, waiting for input UI to provide one.");
        return;
    }
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void android_main(struct android_app* app)
{
    cxrError status = cxrError_Success;
    GAndroidApp = app;
    CloudXRClientOVR cxrcOvr(app);
    gClientHandle = &cxrcOvr;

    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

    status = cxrcOvr.Initialize();
    if (status==cxrError_Success)
    {
        status = cxrcOvr.MainLoop();
        // if (status != cxrError_Success)
        // // TODO then report error if haven't already.
    }

    cxrcOvr.Release();
    gClientHandle = NULL;
    GAndroidApp = NULL;

    ANativeActivity_finish(app->activity);
    // just return to native app glue, let it run destroy, activity finish does the rest.

    // TODO: after return, app_destroy doesn't terminate the process.  we need to unload and
    //  reload native bits potentially -- or at least reset in constructors and watch statics.
    exit(1);
}
