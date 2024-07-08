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

#include "CloudXRCommon.h"
#include "CloudXRController.h"

#define LOG_TAG "CloudXRController"
#include "CloudXRLog.h"
#include "CloudXRMatrixHelpers.h"

CloudXRController::CloudXRController(uint64_t devID, bool angularVelInDevSpace)
{
    m_deviceID = devID;
    m_angularVelInDevSpace = angularVelInDevSpace;

    //init input/output controller states
    memset(&m_pose, 0, sizeof(m_pose));
//    m_haptic = 0;
}


void CloudXRController::Update(const cxrControllerTrackingState& state, float timeOffset)
{
    if (m_deviceID != kDeviceIdInvalid)
    {
        UpdatePose(state, timeOffset);
        // without digging, can't really check this flag until
        // AFTER UpdatePose.  Which we might want to review.
        if (!m_pose.deviceIsConnected)
        {
            return;
        }

        // under new input system, they are handled separate from pose, so we're done.
    }
}

void CloudXRController::UpdatePose(const cxrControllerTrackingState& state, float timeOffset)
{
    // pose
    auto &pose = state.pose;
    // TODO!!!! do we need our poses to be timestamped INSIDE the pose struct itself? We're missing real timing here.
    //m_pose.poseTimeOffset = timeOffset; 
    m_pose.deviceIsConnected = pose.deviceIsConnected;
    m_pose.poseIsValid = pose.poseIsValid;

    if (m_pose.poseIsValid)
    {
        cxrVector3 vAngularVelocity = {0, 0, 0};
        if (m_angularVelInDevSpace)
        {
            vAngularVelocity = pose.angularVelocity;
        }
        else
        {
            // The driver interface expects angular velocity in device space. Transform to that, if the angular velocity is in world space.
            cxrVector3 zero = { 0, 0, 0 };
            cxrMatrix34 poseMatrix;
            cxrVecQuatToMatrix(&zero, &pose.rotation, &poseMatrix);
            cxrMatrix34 invert;
            cxrInverseMatrix(&poseMatrix, &invert);
            cxrTransformVector(&invert, &pose.angularVelocity, &vAngularVelocity);
        }

        for (int i = 0; i < 3; i++)
        {
            m_pose.acceleration.v[i] = pose.acceleration.v[i];
            m_pose.angularAcceleration.v[i] = pose.angularAcceleration.v[i];
            m_pose.position.v[i] = pose.position.v[i];
            m_pose.velocity.v[i] = pose.velocity.v[i];
            m_pose.angularVelocity.v[i] = vAngularVelocity.v[i];
        }
    }
}

cxrTrackedDevicePose CloudXRController::GetPose()
{
    return m_pose;
}

// this is a temporary cheat until we move to input types that are
// understood to implicitly be one-sided.  essentially, there are way
// more two-sided inputs, so in essence we assume that as default.
const std::string oneSidedInputs[] =
{
    "/input/trigger/value",
    "/input/grip/value",
    "/input/grip/force"
};

// TODO: we'll eventually want a list of inputs that are RELATIVE vs the default absolute,
// for things like mice or trackpad-as-mouse

cxrError CloudXRController::RegisterController(const cxrControllerDesc& desc)
{
    // these shouldn't be null, but assigning to a std::string should handle that for us.
    m_name = desc.controllerName;
    m_role = desc.role;

    CXR_LOGD("CloudXRController::RegisterController %llu [%s][%s]", desc.id, m_name.c_str(), m_role.c_str());

    // for now, keeping this fairly generic and just look for 'left' or 'right',
    // so just ensure those substrings exist in whatever role naming is used.
    m_hand = cxrHanded_None;
    if (m_role == "cxr://input/hand/left")
        m_hand = cxrHanded_Left;
    else if (m_role == "cxr://input/hand/right")
        m_hand = cxrHanded_Right;

    m_actionMap.RegisterClientInputs(desc.inputCount, desc.inputPaths, desc.inputValueTypes);
    vlog("CloudXRController::RegisterController end");

    return cxrError_Success;
}

void CloudXRController::SetServerInputs(uint32_t count, const char* inputPaths[], const cxrInputValueType inputTypes[])
{
    m_actionMap.RegisterServerInputs(count, inputPaths, inputTypes);
}

void CloudXRController::SetServerActions(uint32_t count, const char* actionPaths[])
{
    m_actionMap.RegisterActions(count, actionPaths);
}

// TODO: in future, we may want AddProfile, to add a bunch, then SetProfile takes an index or name.
void CloudXRController::SetProfile(std::map<std::string, std::string> profile)
{
    m_actionMap.BindProfile(profile);
}


// NOTE: the server must mutex lock the event queue BEFORE this call,
// as controller has no access to that mutex, but needs it locked.
void CloudXRController::HandleModernEvents(std::vector<cxrActionEvent> &serverQueue, const cxrControllerEvent* events, uint32_t eventCount)
{
    for(uint32_t i = 0; i < eventCount; ++i)
    {
        const auto& e = events[i];
        // this is the client-side index for a given input path/string.
        uint16_t clientIndex = e.clientInputIndex; // TODO this will update to inputIndex at some point

        if(m_actionMap.m_clientInputDatatypes[clientIndex] != e.inputValue.valueType)
        { // This is a sanity check that should never occur, right?
            CXR_LOGE("Error: m_clientInputTypes mismatch!");
            continue;
        }

        uint32_t ai = m_actionMap.GetActionIndex(clientIndex);
        if (ai == 0) // 0==invalid, no binding for that client input
            continue;

        cxrActionEvent newEvent = {};
        newEvent.actionIndex = ai;
        newEvent.clientEvent = e; // just let C copy the struct over, since we want value passed as-is.
        
        // I think all we need to do is push onto server queue now...
        serverQueue.push_back(newEvent);
    }
}


void cxrControllerInputActionMap::RegisterClientInputs(uint32_t count, const char *paths[], const cxrInputValueType types[])
{
    m_clientInputPaths.clear();
    m_clientInputDatatypes.clear();

    for (uint32_t i = 0; i < count; ++i)
    {
        m_clientInputPaths.insert(std::map<std::string, uint32_t>::value_type(paths[i], i));
        m_clientInputDatatypes.insert(std::map<uint32_t, cxrInputValueType>::value_type(i, types[i]));
    }
}

void cxrControllerInputActionMap::RegisterServerInputs(uint32_t count, const char *inputPaths[], const cxrInputValueType inputTypes[])
{
    m_serverInputPaths.clear();
    m_serverInputDatatypes.clear();

    for (uint32_t i = 0; i < count; ++i)
    {
        m_serverInputPaths.insert(std::map<std::string, uint32_t>::value_type(inputPaths[i], i));
        m_serverInputDatatypes.insert(std::map<uint32_t, cxrInputValueType>::value_type(i, inputTypes[i]));

        // we build client->server index remap here, as it's much lower O() count here then as a sep pass...
        auto clientIt = m_clientInputPaths.find(inputPaths[i]);
        if (clientIt != m_clientInputPaths.end())
        {
            uint32_t clientIndex = clientIt->second;
            // just so we know, this is an advisory check, it is NOT necessarily an error, could be intended...
            if (m_clientInputDatatypes[clientIndex] != inputTypes[i])
            {
               CXR_LOGW("Data types do not match between client and server for %s", inputPaths[i]);
            }
        }
    }
}

// this registers all the available server action names, offset in array becomes index/enum mapped value.
void cxrControllerInputActionMap::RegisterActions(uint32_t count, const char *actionPaths[])
{
    m_serverActionPaths.clear();
    for (uint32_t i = 0; i < count; i++)
    {   // the action INDEX is the array offset, remapped to offset in an enum.
        m_serverActionPaths.insert(std::map<std::string, uint32_t>::value_type(actionPaths[i], i));
    }
}

void cxrControllerInputActionMap::BindProfile(std::map<std::string, std::string> profile)
{
    m_actionProfile.clear(); // we're going to build by hand, in case of any bad bindings.
    m_inputToActionRemap.clear();
    for (auto binding : profile)
    {
        // find the path mappings, both to get indices, and to sanity check this is a usable binding.
        auto clientIt = m_clientInputPaths.find(binding.first);
        if (clientIt == m_clientInputPaths.end())
        {
            CXR_LOGW("BindProfile: client doesn't have input [%s] to bind.", binding.first.c_str());
            continue;
        }

#if 0
        auto serverIt = m_serverInputPaths.find(binding.first);
        if (serverIt == m_serverInputPaths.end())
        {
            CXR_LOGE("BindProfile: server doesn't support input [%s].", binding.first.c_str());
            continue;
        }
#endif

        // get the client index 
        uint32_t clientIndex = clientIt->second;
        // then look up server action index
        auto actIt = m_serverActionPaths.find(binding.second);
        if (actIt == m_serverActionPaths.end())
        {
            CXR_LOGE("BindProfile: server doesn't support action [%s].", binding.second.c_str());
            continue;
        }

        CXR_LOGV("Profile input [%s] maps to [%s]", binding.first.c_str(), binding.second.c_str());

        uint32_t serverActionIndex = actIt->second;
        // then combine client index -> server action enum.  this does the 'heavy lifting' in one go.
        m_inputToActionRemap.insert(std::map<uint32_t, uint32_t>::value_type(clientIndex, serverActionIndex));

        // and we can add to our internal copy of profile with bindings that were okay for this device.
        m_actionProfile.insert(std::map<std::string, std::string>::value_type(binding.first, binding.second));

    }
}

// return action index if in remap table, otherwise return 0 which means no action.
uint32_t cxrControllerInputActionMap::GetActionIndex(uint32_t clientInputIndex)
{
    auto actIt = m_inputToActionRemap.find(clientInputIndex);
    if (actIt != m_inputToActionRemap.end())
    {
        return actIt->second;
    }
    return 0;
}
