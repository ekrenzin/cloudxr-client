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
import android.os.Handler
import android.os.Looper
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


    private fun handleXR(intentData: Intent? = intent) {
        val ip = intentData?.data?.getQueryParameter("ip")
        val dvCLI = intentData?.data?.getQueryParameter("dvCLI")
        Log.d(TAG, "cxr ip: $ip")
        if (ip == null) {
            launchBrowser()
        } else {
            handleNativeOptions(ip, dvCLI)
        }
    }

    private fun launchBrowser() {
        Log.d(TAG, "cxr launchBrowser()")

        // Delay logic using Handler
        Handler(Looper.getMainLooper()).postDelayed({
            val newIntent = Intent("com.oculus.vrshell.intent.action.LAUNCH").apply {
                setPackage("com.oculus.vrshell")
                putExtra("uri", "ovrweb://webtask?uri=" + Uri.encode(launchUrl))
                putExtra("intent_data", Uri.parse("systemux://browser"))
            }
            sendBroadcast(newIntent)
        }, 1000)
    }
    private fun handleNativeOptions(ip: String?, dvCLI: String?) {
        Log.d(TAG, "cxr handleNativeOptions()")
        Log.d(TAG, "cxr ip: $ip")
        Log.d(TAG, "cxr dvCLI: $dvCLI")
        if (dvCLI != null) {
            nativeHandleLaunchOptions("$dvCLI -s $ip");
        } else {
            nativeHandleLaunchOptions("-rrr 90 -f 50 -sa -s $ip")
        }
    }

    override fun onResume() {
        super.onResume()
        if (permissionDone && !didResume) doResume()
    }
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        Log.d(TAG, "cxr $this onNewIntent()")
        handleXR(intent)
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

    private fun doResume() {
        didResume = true
        handleXR(intent)
    }
    companion object {
        private const val TAG = "CloudXR"
        private const val PERMISSION_REQUEST_CODE = 1
        private const val launchUrl = "https://desktop.vision/app/#/xr?appid=100&xr=true"
        private var cmdlineOptions: String? = ""
        private var resumeReady = false
        private var permissionDone = false
        private var didResume = false
        private const val REQUEST_CODE = 1001


        init {
            System.loadLibrary("vrapi")
            System.loadLibrary("CloudXRClientOVR")
        }

        @JvmStatic
        external fun nativeHandleLaunchOptions(jcmdline: String?)
    }
}