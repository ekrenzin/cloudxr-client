/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
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

#ifndef CLIENT_APP_OVR_MAIN_H
#define CLIENT_APP_OVR_MAIN_H

#include <stdlib.h>
#include <unistd.h>
#include <unordered_map>

#include "VrApi.h"
#include "VrApi_Helpers.h"
#include "VrApi_SystemUtils.h"
#include "VrApi_Input.h"

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#include "CloudXRClient.h"
#include "EGLHelper.h"

#include "oboe/Oboe.h"

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class CloudXRClientOVR : public oboe::AudioStreamDataCallback
{
    static constexpr uint32_t SwapChainLen = 3;
    static constexpr uint32_t NumEyes = 2;
    typedef std::unordered_map<GLuint, GLuint> FramebufferMap;

    typedef enum {
        RenderState_Loading = 0,
        RenderState_Running = 1,
        RenderState_Exiting = 2
    } CxrcRenderStates;

public:
    CloudXRClientOVR(struct android_app* app);
    cxrError Initialize();
    cxrError MainLoop();
    cxrError Release();

    void SetWindow(ANativeWindow* win) { mNativeWindow = win; }
    ANativeWindow* GetWindow() { return mNativeWindow; }

    void SetReadyToConnect(bool ready) { mReadyToConnect = ready; }
    void SetDefaultBGColor(const uint32_t col) { mDefaultBGColor = col; }

    void SetPaused(bool p) { mIsPaused = p; }
    void RequestExit();

private:
    void AppResumed();
    void AppPaused();

    bool EnterVRMode();
    void HandleVrModeChanges();
    void HandleVrApiEvents();

    // AudioStreamDataCallback interface
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream,
            void *audioData, int32_t numFrames) override;

    // CloudXR interface callbacks
    void GetTrackingState(cxrVRTrackingState* trackingState);
    void TriggerHaptic(const cxrHapticFeedback*);
    cxrBool RenderAudio(const cxrAudioFrame*);

    cxrError CreateReceiver();
    void TeardownReceiver();
    void UpdateClientState();

    void RecreateSwapchain(uint32_t width, uint32_t height, uint32_t eye);
    bool SetupFramebuffer(GLuint colorTexture, uint32_t eye);

    void DetectControllers();
    void ProcessControllers(float predictedTimeS);

    cxrTrackedDevicePose ConvertPose(const ovrRigidBodyPosef& pose, float rotationX = 0);
    cxrDeviceDesc GetDeviceDesc(float fovX, float fovY);

    void SetHaptic(const cxrHapticFeedback& haptic);

    void SubmitLayers(const ovrLayerHeader2* layers[], int layerCount, ovrFrameFlags flags);
    void RenderLoadScreen();
    void RenderExitScreen();
    void Render();

    void DoTracking(float predictedTimeS);
    cxrError QueryChaperone(cxrDeviceDesc* deviceDesc) const;

    void FillBackground();

protected:
    CxrcRenderStates mRenderState = RenderState_Loading;
    struct android_app *mAndroidApp = NULL;
    ANativeWindow *mNativeWindow = NULL;
    ovrJava mJavaCtx;
    EGLHelper mEglHelper;
    ovrMobile *mOvrSession = NULL; // pointer to oculus VrApi session object.
    uint64_t mFrameCounter = 0;

    bool mRefreshChanged = false;
    float_t mTargetDisplayRefresh = 0;
    const float_t cDefaultDisplayRefresh = 72.0f; // Can change this to hardcode alternate value...

    double mNextDisplayTime = 0;
    ovrRigidBodyPosef mLastHeadPose;

    volatile bool mIsPaused = true; // we start out in paused state
    bool mWasPaused = true; // so we can detect transitions.
    bool mIsFocused = true; // TODO: set based on window state!

    bool mReadyToConnect = false;
    bool mHeadsetOnHead = true;

    bool ControllersFound = false;
    bool IsTouchController = false;

    GLuint Framebuffers[NumEyes];

    ovrMatrix4f TexCoordsFromTanAngles;

    ovrTextureSwapChain* SwapChains[VRAPI_FRAME_LAYER_EYE_MAX] = {};

    uint32_t EyeWidth[VRAPI_FRAME_LAYER_EYE_MAX] = {};
    uint32_t EyeHeight[VRAPI_FRAME_LAYER_EYE_MAX] = {};

    std::shared_ptr<oboe::AudioStream> recordingStream{};
    std::shared_ptr<oboe::AudioStream> playbackStream{};

    cxrVRTrackingState TrackingState = {};
    cxrReceiverHandle Receiver = nullptr;
    cxrClientState mClientState = cxrClientState_ReadyToConnect;
    cxrStateReason mClientStateReason = cxrStateReason_NoError;
    cxrDeviceDesc mDeviceDesc = {};
    cxrConnectionDesc mConnectionDesc = {};
    cxrConnectionStats mStats = {};
    int mFramesUntilStats = 60;

    uint32_t mDefaultBGColor = 0xFF000000; // black to start until we set around OnResume.
    uint32_t mBGColor = mDefaultBGColor;

    struct OvrCxrButtonMapping
    {
        unsigned int ovrId;
        cxrButtonId cxrId;
        char nameStr[32];
    };

    bool setBooleanButton(cxrControllerTrackingState &ctl,
                          const uint64_t &inBitfield, const OvrCxrButtonMapping &mapping);
};

#endif //CLIENT_APP_OVR_MAIN_H
