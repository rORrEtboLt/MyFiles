@file:JvmName("MyFilesNative")

package com.example.myfiles

// Implemented in src/platform/platform_android.c. Top-level functions compile to plain static native
// methods on class MyFilesNative, so each has exactly one JNI symbol. Safe to call from any thread.

external fun nativeScanDone(
    paths: Array<String>?, widths: IntArray?, heights: IntArray?, texts: Array<String>?, cancelled: Boolean, error: String?,
)

external fun nativeLabelDone(text: String?, fromQr: Boolean, cancelled: Boolean, error: String?)
