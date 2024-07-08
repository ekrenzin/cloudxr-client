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
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <map>
#include <vector>

#define kDeviceIdInvalid (uint64_t)(-1)

typedef enum
{
    cxrHanded_None = 0,
    cxrHanded_Left = 1,
    cxrHanded_Right = 2,
} cxrHandedness;

 // these are server actions posted from controllers/devices to server event queue for handling.
typedef struct
{
    uint32_t actionIndex;
    cxrControllerEvent clientEvent;
} cxrActionEvent;

// this is the 'business logic' class that helps convert client input
// over to server actions.  You provide it the various pieces of data,
// and it will build the remap tables, and then can give you what action
// should result from what input. NOTE WE NEED ONE PER INPUT DEVICE.
class cxrControllerInputActionMap
{
    friend class CloudXRController;

public:
    cxrControllerInputActionMap() {};

    // fn needed here to register the server 'full input list'
    void RegisterServerInputs(uint32_t count, const char *inputPaths[], const cxrInputValueType inputTypes[]);

    // this registers all the available server action names to corresponding offset/index.
    void RegisterActions(uint32_t count, const char *actionPaths[]);

    // register the client-sent available inputs, add them to the path->index client remap
    void RegisterClientInputs(uint32_t count, const char *paths[], const cxrInputValueType types[]);
    
    // fn needed here to register the profile for this controller remap
    // given this is pre-constructed remap table, passing in map<inputPath, actionPath>
    // and it will make map<clientInputIndex, actionIndex> from server list, client list, and action list.
    void BindProfile(std::map<std::string, std::string> profile);

    // this does the lookup/translation from client input index all the way to the server action
    // note that index 0 is reserved for kNoActionMapped value.
    uint32_t GetActionIndex(uint32_t inputIndex);

private:
    // there are certainly better ways to do this.
    // But I decided to store ALL the data in this class in maps
    // Load them up one by one, and do resolves to secondary maps when ready.
    // At the end, a given ActionMap could be similar to OpenXR ActionSet

    // server will 'prefill' these maps at init, as they should be statics in the server.
    // they don't change at all at runtime, so server should own canonical form.
    // whether these three turn into pointers or references to server-equiv maps is TBD.
    std::map<std::string, uint32_t> m_serverInputPaths; // server global list of inputpaths -> server index
    std::map<uint32_t, cxrInputValueType> m_serverInputDatatypes; // server global list of input data type per server index (per inputpath)
    std::map<std::string, uint32_t> m_serverActionPaths; // server actionpath -> server actionindex

    // client sends/registers this with server on connection of given controller.
    std::map<std::string, uint32_t> m_clientInputPaths; // client inputpath -> client index
    std::map<uint32_t, cxrInputValueType> m_clientInputDatatypes; // client index -> client sent data type for this input.

    // this is constructed based on server/app profile for a given controller,
    // mapping input strings to action strings.  It is then used to build other remap tables.
    std::map<std::string, std::string> m_actionProfile; // client inputpath -> server actionpath

    // if profile changes, we need to rebuild this.  Or we should have a vector if we have a profile vector,
    // so switching profiles would switch which Remap we use for translations.
    std::map<uint32_t, uint32_t> m_inputToActionRemap; // client inputindex -> server actionindex
};

class CloudXRController
{
public:
    CloudXRController(uint64_t devID, bool angularVelInDevSpace);
    cxrError RegisterController(const cxrControllerDesc& desc);

    void SetServerInputs(uint32_t count, const char* inputPaths[], const cxrInputValueType inputTypes[]);
    void SetServerActions(uint32_t count, const char* actionPaths[]);
    void SetProfile(std::map<std::string, std::string> profile);

    void Update(const cxrControllerTrackingState & state, float timeOffset);
    void UpdatePose(const cxrControllerTrackingState & state, float timeOffset);

    void HandleModernEvents(std::vector<cxrActionEvent> &serverQueue, const cxrControllerEvent* events, uint32_t eventCount);

    cxrTrackedDevicePose GetPose();
    cxrControllerInputActionMap &GetActionMap() { return m_actionMap; };
    uint8_t GetHandedness() { return m_hand; };


protected:
    cxrControllerInputActionMap m_actionMap = {};
    std::mutex s_eventAccessMutex;

    uint64_t m_deviceID = kDeviceIdInvalid;
    uint8_t m_hand = cxrHanded_None;
    bool m_angularVelInDevSpace = false;
    std::string m_role = "Unknown";
    std::string m_name = "Unknown";
    cxrTrackedDevicePose m_pose = {};
};
