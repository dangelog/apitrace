/**************************************************************************
 *
 * Copyright 2011 Jose Fonseca
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/


#include "glproc.hpp"


#if !defined(_WIN32)
#include "dlopen.hpp"
#endif


/*
 * Handle to the true OpenGL library.
 * XXX: Not really used yet.
 */
#if defined(_WIN32)
HMODULE _libGlHandle = NULL;
#else
void *_libGlHandle = NULL;
#endif



#if defined(_WIN32)

#error Unsupported

#elif defined(__APPLE__)

#error Unsupported

#else

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

static bool
addressBelongsToApitrace(void *address)
{
    Dl_info info;
    if (dladdr(address, &info)) {
        os::log("apitrace: addressBelongsToApitrace: '%p' belongs to '%s'\n", address, info.dli_fname);
        if (strstr(info.dli_fname, "egltrace.so")) {
            os::log("apitrace: addressBelongsToApitrace: '%p' belongs to apitrace\n", address);
            return true;
        } else {
            os::log("apitrace: addressBelongsToApitrace: '%p' does NOT belong to apitrace\n", address);
            return false;
        }
    } else {
        os::log("apitrace: addressBelongsToApitrace: dladdr failed!\n");
        return false;
    }
}

void *
_eglGetProcAddressWrapper(const char *procName)
{
    os::log("apitrace: _eglGetProcAddressWrapper for '%s'\n", procName);

    if (!procName) {
        return NULL;
    }

    // load libEGL and libGLESv2, just in case
    static void *libEGL = NULL;
    static void *libGLESv2 = NULL;
    if (!libEGL) {
        os::log("apitrace: _eglGetProcAddressWrapper: trying to load libEGL...\n");
        libEGL = _dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY | RTLD_DEEPBIND);
        if (!libEGL) {
            return NULL;
        }
        os::log("apitrace: _eglGetProcAddressWrapper: libEGL loaded\n");

        os::log("apitrace: _eglGetProcAddressWrapper: trying to load libGLESv2...\n");
        libGLESv2 = _dlopen("libGLESv2.so", RTLD_LOCAL | RTLD_LAZY | RTLD_DEEPBIND);
        if (!libGLESv2) {
            return NULL;
        }
        os::log("apitrace: _eglGetProcAddressWrapper: libGLESv2 loaded\n");
    }

    static void *(*eglGetProcAddress_the_real_one)(const char *) = NULL;
    static bool eglGetProcAddress_the_real_one_resolved = false;
    if (!eglGetProcAddress_the_real_one_resolved) {
        os::log("apitrace: _eglGetProcAddressWrapper: trying to resolve the real eglGetProcAddress\n");
        eglGetProcAddress_the_real_one_resolved = true;

        void *eglGetProcAddress_resolved = dlsym(RTLD_NEXT, "eglGetProcAddress");

        if (addressBelongsToApitrace(eglGetProcAddress_resolved)) {
            os::log("apitrace: _eglGetProcAddressWrapper: dlsym returned the wrong eglGetProcAddress :-(\n");
        } else {
            eglGetProcAddress_the_real_one = reinterpret_cast<void *(*)(const char *)>(eglGetProcAddress_resolved);
        }
    }

    void *proc = NULL;

    // if it's there, try the real one first
    if (eglGetProcAddress_the_real_one) {
        os::log("apitrace: _eglGetProcAddressWrapper: trying the real eglGetProcAddress...\n");
        proc = eglGetProcAddress_the_real_one(procName);
        if (proc) {
            if (addressBelongsToApitrace(proc)) {
                os::log("apitrace: _eglGetProcAddressWrapper: the real eglGetProcAddress returned an address (%p) belonging to apitrace\n", proc);
                proc = NULL;
            } else {
                os::log("apitrace: _eglGetProcAddressWrapper: the real eglGetProcAddress returned %p which seems OK\n", proc);
            }
        } else {
            os::log("apitrace: _eglGetProcAddressWrapper: the real eglGetProcAddress returned NULL.\n");
        }
    }

    if (!proc) {
        os::log("apitrace: _eglGetProcAddressWrapper: trying dlsym directly.\n");
        proc = dlsym(RTLD_NEXT, procName);
    }

    os::log("apitrace: _eglGetProcAddressWrapper: resolved '%s' to '%p'\n", procName, proc);

    return proc;
}

/*
 * Lookup a public EGL/GL/GLES symbol
 *
 * The spec states that eglGetProcAddress should only be used for non-core
 * (extensions) entry-points.  Core entry-points should be taken directly from
 * the API specific libraries.
 *
 */
void *
_getPublicProcAddress(const char *procName)
{
    void *proc;

    os::log("apitrace: _getPublicProcAddress '%s'\n", procName);

    if (getenv("OVERRIDE_EGLGETPROCADDRESS") != NULL
            && strcmp(procName, "eglGetProcAddress") == 0)
    {
        os::log("apitrace: _getPublicProcAddress: returning built-in eglGetProcAddress wrapper\n");
        return (void *)&_eglGetProcAddressWrapper;
    }

    /*
     * We rely on dlsym(RTLD_NEXT, ...) whenever we can, because certain gl*
     * symbols are exported by multiple APIs/SOs, and it's not trivial to
     * determine which API/SO we should get the current symbol from.
     */
    proc = dlsym(RTLD_NEXT, procName);
    if (proc) {
        (void)addressBelongsToApitrace(proc);
        return proc;
    }

    /*
     * dlsym(RTLD_NEXT, ...) will fail when the SO containing the symbol was
     * loaded with RTLD_LOCAL.  We try to override RTLD_LOCAL with RTLD_GLOBAL
     * when tracing but this doesn't always work.  So we try to guess and load
     * the shared object directly.
     *
     * See https://github.com/apitrace/apitrace/issues/349#issuecomment-113316990
     */

    if (procName[0] == 'e' && procName[1] == 'g' && procName[2] == 'l') {
        static void *libEGL = NULL;
        if (!libEGL) {
            libEGL = _dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY | RTLD_DEEPBIND);
            if (!libEGL) {
                return NULL;
            }
        }
        proc = dlsym(libEGL, procName);
        (void)addressBelongsToApitrace(proc);
        return proc;
    }

    /*
     * This might happen when:
     *
     * - the application is querying non-extensions functions via
     *   eglGetProcAddress (either because EGL_KHR_get_all_proc_addresses
     *   is advertised, or merely because the EGL implementation supports
     *   it regardless, like Mesa does)
     *
     * - libGLES*.so nor libGL*.so was ever loaded.
     *
     * - we need to resolve entrypoints that application never asked (e.g.,
     *   glGetIntegerv), for internal purposes
     *
     * Therefore, we try to fallback to eglGetProcAddress.
     *
     * See https://github.com/apitrace/apitrace/issues/301#issuecomment-68532248
     */
    if (strcmp(procName, "eglGetProcAddress") != 0) {
        proc = (void *) _eglGetProcAddress(procName);
        if (proc) {
            (void)addressBelongsToApitrace(proc);
            return proc;
        }
    }

    /*
     * TODO: We could futher mitigate against using the wrong SO by:
     * - using RTLD_NOLOAD to ensure we only use an existing SO
     * - the determine the right SO via eglQueryAPI and glGetString(GL_VERSION)
     */

    if (procName[0] == 'g' && procName[1] == 'l') {
        /* TODO: Use GLESv1/GLESv2 on a per-context basis. */

        static void *libGLESv2 = NULL;
        if (!libGLESv2) {
            libGLESv2 = _dlopen("libGLESv2.so", RTLD_LOCAL | RTLD_LAZY | RTLD_DEEPBIND);
        }
        if (libGLESv2) {
            proc = dlsym(libGLESv2, procName);
        }
        if (proc) {
            return proc;
        }

        static void *libGLESv1 = NULL;
        if (!libGLESv1) {
            libGLESv1 = _dlopen("libGLESv1_CM.so", RTLD_LOCAL | RTLD_LAZY | RTLD_DEEPBIND);
        }
        if (libGLESv1) {
            proc = dlsym(libGLESv1, procName);
        }
        if (proc) {
            return proc;
        }
    }

    return NULL;
}

/*
 * Lookup a private EGL/GL/GLES symbol
 *
 * Private symbols should always be available through eglGetProcAddress, and
 * they are guaranteed to work with any context bound (regardless of the API).
 *
 * However, per issue#57, eglGetProcAddress returns garbage on some
 * implementations, and the spec states that implementations may choose to also
 * export extension functions publicly, so we always attempt dlsym before
 * eglGetProcAddress to mitigate that.
 */
void *
_getPrivateProcAddress(const char *procName)
{
    os::log("apitrace: EGL _getPrivateProcAddress '%s'\n", procName);

    void *proc;
    proc = _getPublicProcAddress(procName);
    if (!proc &&
        ((procName[0] == 'e' && procName[1] == 'g' && procName[2] == 'l') ||
         (procName[0] == 'g' && procName[1] == 'l'))) {
        proc = (void *) _eglGetProcAddress(procName);
    }

    return proc;
}

#endif
