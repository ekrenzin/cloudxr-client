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

#ifndef CLOUDXR_MATRIX_HELPERS_H
#define CLOUDXR_MATRIX_HELPERS_H

#include <math.h>
#include <CloudXRCommon.h>

static inline void cxrMatrixSetIdentity(cxrMatrix34* inout)
{
    memset(inout, 0, sizeof(cxrMatrix34));
    // set identity
    inout->m[0][0] = 1.0f;
    inout->m[1][1] = 1.0f;
    inout->m[2][2] = 1.0f;
}

static inline void cxrMatrixToVecQuat(const cxrMatrix34* in, cxrVector3* outPos, cxrQuaternion* outRot)
{
    if (NULL != outRot)
    {
        cxrQuaternion q;
        q.w = sqrt(fmax(0.0f, 1.0f + in->m[0][0] + in->m[1][1] + in->m[2][2])) / 2.0f;
        q.x = sqrt(fmax(0.0f, 1.0f + in->m[0][0] - in->m[1][1] - in->m[2][2])) / 2.0f;
        q.y = sqrt(fmax(0.0f, 1.0f - in->m[0][0] + in->m[1][1] - in->m[2][2])) / 2.0f;
        q.z = sqrt(fmax(0.0f, 1.0f - in->m[0][0] - in->m[1][1] + in->m[2][2])) / 2.0f;
        q.x = copysign(q.x, in->m[2][1] - in->m[1][2]);
        q.y = copysign(q.y, in->m[0][2] - in->m[2][0]);
        q.z = copysign(q.z, in->m[1][0] - in->m[0][1]);
        *outRot = q;
    }
    // could be useful to allow for just the quaternion returned, not position, so check null.
    if (NULL != outPos)
    {
        for(int i = 0; i < 3; ++i)
        {
            outPos->v[i] = in->m[i][3];
        }
    }
}

static inline void cxrVecQuatToMatrix(const cxrVector3* inPos, const cxrQuaternion* inRot, cxrMatrix34* out)
{
    if (NULL != inRot)
    {
        float wx, wy, wz, xx, yy, yz, xy, xz, zz, x2, y2, z2;
        // calculate coefficients
        x2 = inRot->x + inRot->x; y2 = inRot->y + inRot->y;
        z2 = inRot->z + inRot->z;
        xx = inRot->x * x2; xy = inRot->x * y2; xz = inRot->x * z2;
        yy = inRot->y * y2; yz = inRot->y * z2; zz = inRot->z * z2;
        wx = inRot->w * x2; wy = inRot->w * y2; wz = inRot->w * z2;
        out->m[0][0] = 1.0f - (yy + zz); 
        out->m[0][1] = xy - wz;
        out->m[0][2] = xz + wy; 
        out->m[1][0] = xy + wz; 
        out->m[1][1] = 1.0f - (xx + zz);
        out->m[1][2] = yz - wx; 
        out->m[2][0] = xz - wy; 
        out->m[2][1] = yz + wx;
        out->m[2][2] = 1.0f - (xx + yy); 
    }
    else
    { // clear the out matrix to identity.
        cxrMatrixSetIdentity(out);
    }

    // allow for no position, just rotation
    if (NULL != inPos)
    {
        for(int i = 0; i < 3; ++i)
        {
            out->m[i][3] = inPos->v[i];
        }
    }
    else
    {
        // fill identity which is 0,0,0
        for (int i = 0; i < 3; ++i)
            out->m[i][3] = 0;
    }
}

static inline void cxrInverseMatrix(const cxrMatrix34* in, cxrMatrix34* out)
{
    // inverse.rotation = transpose(matrix.rotation)
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            out->m[j][i] = in->m[i][j];
    }

    // inverse.position = inverse.rotation * -matrix.position
    for (int i = 0; i < 3; i++)
    {
        out->m[i][3] = 0;
        for (int j = 0; j < 3; j++)
            out->m[i][3] += out->m[i][j] * -in->m[j][3];
    }
}

static inline void cxrTransformVector(const cxrMatrix34* inMat, const cxrVector3* inVec, cxrVector3* outVec)
{
    for (int i = 0; i < 3; i++)
    {
        outVec->v[i] = 0.f;
        for (int j = 0; j < 3; j++)
        {
            outVec->v[i] += inVec->v[j] * inMat->m[i][j];
        }
    }
}

#endif //ifndef CLOUDXR_MATRIX_HELPERS_H
