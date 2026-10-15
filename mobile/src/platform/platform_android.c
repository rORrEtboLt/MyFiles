/* Android: JNI bridge to MyFilesActivity.kt (ML Kit document scanner, text recognition, QR code
 * scanner, share intent, HttpURLConnection). Java classes are looked up once on the SDL main thread
 * and cached as global refs, because FindClass on the sync thread would use the system class loader
 * and not find app classes. */
#include <jni.h>

#include "platform.h"

static Uint32 g_scan_event, g_label_event;
static jclass g_activity_cls, g_http_result_cls;
static jmethodID g_init, g_scan_document, g_scan_label, g_share_file, g_http;
static jfieldID g_http_status, g_http_body;

void platform_init(SDL_Window *window, const char *tmp_dir, Uint32 scan_event, Uint32 label_event)
{
    (void)window;
    g_scan_event = scan_event;
    g_label_event = label_event;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jclass cls = (*env)->FindClass(env, "com/example/myfiles/MyFilesActivity");
    jclass res = (*env)->FindClass(env, "com/example/myfiles/HttpResult");
    g_activity_cls = (*env)->NewGlobalRef(env, cls);
    g_http_result_cls = (*env)->NewGlobalRef(env, res);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, res);
    g_init = (*env)->GetStaticMethodID(env, g_activity_cls, "init", "(Ljava/lang/String;)V");
    g_scan_document = (*env)->GetStaticMethodID(env, g_activity_cls, "scanDocument", "()V");
    g_scan_label = (*env)->GetStaticMethodID(env, g_activity_cls, "scanLabel", "()V");
    g_share_file = (*env)->GetStaticMethodID(env, g_activity_cls, "shareFile", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_http = (*env)->GetStaticMethodID(env, g_activity_cls, "http",
                                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Lcom/example/myfiles/HttpResult;");
    g_http_status = (*env)->GetFieldID(env, g_http_result_cls, "status", "I");
    g_http_body = (*env)->GetFieldID(env, g_http_result_cls, "body", "[B");

    jstring tmp = (*env)->NewStringUTF(env, tmp_dir);
    (*env)->CallStaticVoidMethod(env, g_activity_cls, g_init, tmp);
    (*env)->DeleteLocalRef(env, tmp);
}

const char *platform_font_dir(void) { return "fonts/"; } /* APK assets, read through SDL_IOFromFile */

void platform_scan_document(void)
{
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    (*env)->CallStaticVoidMethod(env, g_activity_cls, g_scan_document);
}

void platform_scan_label(void)
{
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    (*env)->CallStaticVoidMethod(env, g_activity_cls, g_scan_label);
}

void platform_share_file(const char *path, const char *mime)
{
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jstring p = (*env)->NewStringUTF(env, path), m = (*env)->NewStringUTF(env, mime);
    (*env)->CallStaticVoidMethod(env, g_activity_cls, g_share_file, p, m);
    (*env)->DeleteLocalRef(env, p);
    (*env)->DeleteLocalRef(env, m);
}

static jstring jstr_or_null(JNIEnv *env, const char *s) { return s ? (*env)->NewStringUTF(env, s) : NULL; }

static char *dup_jstring(JNIEnv *env, jstring s)
{
    if (!s) return NULL;
    const char *utf = (*env)->GetStringUTFChars(env, s, NULL);
    char *out = SDL_strdup(utf ? utf : "");
    if (utf) (*env)->ReleaseStringUTFChars(env, s, utf);
    return out;
}

bool platform_http(const HttpRequest *req, HttpResponse *res)
{
    SDL_zerop(res);
    JNIEnv *env = SDL_GetAndroidJNIEnv(); /* attaches the sync thread on first use */
    (*env)->PushLocalFrame(env, 16);
    jstring method = jstr_or_null(env, req->method), url = jstr_or_null(env, req->url);
    jstring bearer = jstr_or_null(env, req->bearer), ctype = jstr_or_null(env, req->content_type);
    jbyteArray body = NULL;
    if (req->body) {
        body = (*env)->NewByteArray(env, (jsize)req->body_len);
        (*env)->SetByteArrayRegion(env, body, 0, (jsize)req->body_len, req->body);
    }
    jobject result = (*env)->CallStaticObjectMethod(env, g_activity_cls, g_http, method, url, bearer, ctype, body);
    bool ok = false;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else if (result) {
        res->status = (*env)->GetIntField(env, result, g_http_status);
        jbyteArray data = (*env)->GetObjectField(env, result, g_http_body);
        jsize len = data ? (*env)->GetArrayLength(env, data) : 0;
        res->body = SDL_malloc((size_t)len + 1);
        if (len) (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)res->body);
        res->body[len] = '\0';
        res->len = (size_t)len;
        ok = true;
    }
    (*env)->PopLocalFrame(env, NULL);
    return ok;
}

/* ---- callbacks from Kotlin (any thread) ------------------------------------------------------- */

/* Declared as top-level `external fun`s in Native.kt (@file:JvmName("MyFilesNative")). */
JNIEXPORT void JNICALL Java_com_example_myfiles_MyFilesNative_nativeScanDone(
    JNIEnv *env, jclass cls, jobjectArray paths, jintArray widths, jintArray heights, jobjectArray texts, jboolean cancelled, jstring error)
{
    (void)cls;
    int n = paths ? (*env)->GetArrayLength(env, paths) : 0;
    ScanResult *r = scan_result_new(n);
    r->cancelled = cancelled;
    if (error) {
        char *e = dup_jstring(env, error);
        SDL_strlcpy(r->error, e, sizeof r->error);
        SDL_free(e);
    }
    jint *w = n ? (*env)->GetIntArrayElements(env, widths, NULL) : NULL;
    jint *h = n ? (*env)->GetIntArrayElements(env, heights, NULL) : NULL;
    for (int i = 0; i < n; i++) {
        jstring p = (*env)->GetObjectArrayElement(env, paths, i);
        jstring t = (*env)->GetObjectArrayElement(env, texts, i);
        char *path = dup_jstring(env, p);
        SDL_strlcpy(r->pages[i].path, path, sizeof r->pages[i].path);
        SDL_free(path);
        r->pages[i].ocr_text = t ? dup_jstring(env, t) : SDL_strdup("");
        r->pages[i].width = w[i];
        r->pages[i].height = h[i];
        (*env)->DeleteLocalRef(env, p);
        if (t) (*env)->DeleteLocalRef(env, t);
    }
    if (w) (*env)->ReleaseIntArrayElements(env, widths, w, JNI_ABORT);
    if (h) (*env)->ReleaseIntArrayElements(env, heights, h, JNI_ABORT);
    platform_post(g_scan_event, r);
}

JNIEXPORT void JNICALL Java_com_example_myfiles_MyFilesNative_nativeLabelDone(
    JNIEnv *env, jclass cls, jstring text, jboolean from_qr, jboolean cancelled, jstring error)
{
    (void)cls;
    LabelResult *r = label_result_new();
    r->text = dup_jstring(env, text);
    r->from_qr = from_qr;
    r->cancelled = cancelled;
    if (error) {
        char *e = dup_jstring(env, error);
        SDL_strlcpy(r->error, e, sizeof r->error);
        SDL_free(e);
    }
    platform_post(g_label_event, r);
}
