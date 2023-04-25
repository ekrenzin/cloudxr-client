/*
 * Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
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
package com.valiventures.cloudxr.ovr

import android.Manifest
import android.app.NativeActivity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : NativeActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        // do super first, as that sets up some native things.
        super.onCreate(savedInstanceState)

        // check for any data passed to our activity that we want to handle
        cmdlineOptions = intent.getStringExtra("args")

        // check for permission for any 'dangerous' class features.
        // Note that INTERNET is normal and pre-granted, and READ_EXTERNAL is implicitly granted when accepting WRITE.
        if (ContextCompat.checkSelfPermission(
                this, Manifest.permission.WRITE_EXTERNAL_STORAGE
            ) != PackageManager.PERMISSION_GRANTED || ContextCompat.checkSelfPermission(
                this, Manifest.permission.RECORD_AUDIO
            ) != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(
                this, arrayOf(
                    Manifest.permission.WRITE_EXTERNAL_STORAGE, Manifest.permission.RECORD_AUDIO
                ), PERMISSION_REQUEST_CODE
            )
            Log.w(TAG, "Waiting for permissions from user...")
        } else {
            permissionDone = true
        }
    }

    private fun doResume() {
        didResume = true

        // Get the IP value from the intent's data
        val ip = intent.data?.getQueryParameter("ip")
        val dvCLI = intent.data?.getQueryParameter("dvCLI")
        if (ip != null) {
            if (dvCLI != null) {
                nativeHandleLaunchOptions("$dvCLI -s $ip");
            } else {
                nativeHandleLaunchOptions("-rrr 90 -f 50 -sa -s $ip")
            }
        } else {
            // Do something else if there is no intent data
            val newIntent = Intent("com.oculus.vrshell.intent.action.LAUNCH").apply {
                setPackage("com.oculus.vrshell")
                putExtra("uri", "ovrweb://webtask?uri=" + Uri.encode(launchUrl))
                putExtra("intent_data", Uri.parse("systemux://browser"))
            }
            sendBroadcast(newIntent)
            // Close the app after broadcasting the intent
            finish()
        }
    }

    override fun onResume() {
        Log.d(TAG, "$this onResume()")
        super.onResume()
        resumeReady = true
        if (permissionDone && !didResume) doResume()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<String>, grantResults: IntArray
    ) {
        if (requestCode == PERMISSION_REQUEST_CODE && grantResults.isNotEmpty()) {
            if (grantResults[0] != PackageManager.PERMISSION_GRANTED) {
                Log.e(
                    TAG,
                    "Error: external storage permission has not been granted.  It is required to read launch options file or write logs."
                )
            }
            if (grantResults[1] != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "Warning: Record audio permission not granted, cannot use microphone.")
            }
        } else {
            Log.e(
                TAG,
                "Bad return for RequestPermissions: [$requestCode] {$permissions} {$grantResults}"
            )
            // TODO: do we need to exit here?
        }

        // we don't currently treat any of these permissions as required/fatal, so continue on...
        permissionDone = true
        if (!didResume && resumeReady) doResume()
    }

    companion object {
        private const val TAG = "CloudXR"
        private const val PERMISSION_REQUEST_CODE = 1
        private const val launchUrl = "https://desktop.vision/app/#/xr?appid=100&xr=true"
        private var cmdlineOptions: String? = ""
        private var resumeReady = false
        private var permissionDone = false
        private var didResume = false

        init {
            System.loadLibrary("vrapi")
            System.loadLibrary("CloudXRClientOVR")
        }

        @JvmStatic
        external fun nativeHandleLaunchOptions(jcmdline: String?)
    }
}