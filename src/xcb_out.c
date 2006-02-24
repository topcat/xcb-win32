/* Copyright (C) 2001-2004 Bart Massey and Jamey Sharp.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 * Except as contained in this notice, the names of the authors or their
 * institutions shall not be used in advertising or otherwise to promote the
 * sale, use or other dealings in this Software without prior written
 * authorization from the authors.
 */

/* Stuff that sends stuff to the server. */

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifndef __GNUC__
# if HAVE_ALLOCA_H
#  include <alloca.h>
# else
#  ifdef _AIX
 #pragma alloca
#  endif
# endif
#endif

#include "xcb.h"
#include "xcbext.h"
#include "xcbint.h"
#include "extensions/bigreq.h"

static int force_sequence_wrap(XCBConnection *c)
{
    int ret = 1;
    if((c->out.request - c->in.request_read) > 65530)
    {
        pthread_mutex_unlock(&c->iolock);
        ret = XCBSync(c, 0);
        pthread_mutex_lock(&c->iolock);
    }
    return ret;
}

/* Public interface */

CARD32 XCBGetMaximumRequestLength(XCBConnection *c)
{
    pthread_mutex_lock(&c->out.reqlenlock);
    if(!c->out.maximum_request_length)
    {
        const XCBQueryExtensionRep *ext;
        c->out.maximum_request_length = c->setup->maximum_request_length;
        ext = XCBGetExtensionData(c, &XCBBigRequestsId);
        if(ext && ext->present)
        {
            XCBBigRequestsEnableRep *r = XCBBigRequestsEnableReply(c, XCBBigRequestsEnable(c), 0);
            c->out.maximum_request_length = r->maximum_request_length;
            free(r);
        }
    }
    pthread_mutex_unlock(&c->out.reqlenlock);
    return c->out.maximum_request_length;
}

int XCBSendRequest(XCBConnection *c, unsigned int *request, struct iovec *vector, const XCBProtocolRequest *req)
{
    static const char pad[3];
    int ret;
    int i;
    struct iovec *padded;
    int padlen = 0;
    CARD16 shortlen = 0;
    CARD32 longlen = 0;
    enum workarounds workaround = WORKAROUND_NONE;

    assert(c != 0);
    assert(request != 0);
    assert(vector != 0);
    assert(req->count > 0);

    /* set the major opcode, and the minor opcode for extensions */
    if(req->ext)
    {
        const XCBQueryExtensionRep *extension = XCBGetExtensionData(c, req->ext);
        /* TODO: better error handling here, please! */
        assert(extension && extension->present);
        ((CARD8 *) vector[0].iov_base)[0] = extension->major_opcode;
        ((CARD8 *) vector[0].iov_base)[1] = req->opcode;

        /* do we need to work around the X server bug described in glx.xml? */
        if(strcmp(req->ext->name, "GLX") &&
                ((req->opcode == 17 && ((CARD32 *) vector[0].iov_base)[0] == 0x10004) ||
                 req->opcode == 21))
            workaround = WORKAROUND_GLX_GET_FB_CONFIGS_BUG;
    }
    else
        ((CARD8 *) vector[0].iov_base)[0] = req->opcode;

    /* put together the length field, possibly using BIGREQUESTS */
    for(i = 0; i < req->count; ++i)
        longlen += (vector[i].iov_len + 3) >> 2;

    if(longlen > c->setup->maximum_request_length)
    {
        if(longlen > XCBGetMaximumRequestLength(c))
            return 0; /* server can't take this; maybe need BIGREQUESTS? */
    }
    else
    {
        /* we don't need BIGREQUESTS. */
        shortlen = longlen;
        longlen = 0;
    }

    padded =
#ifdef HAVE_ALLOCA
        alloca
#else
        malloc
#endif
        ((req->count * 2 + 3) * sizeof(struct iovec));
    /* set the length field. */
    ((CARD16 *) vector[0].iov_base)[1] = shortlen;
    if(!shortlen)
    {
        padded[0].iov_base = vector[0].iov_base;
        padded[0].iov_len = sizeof(CARD32);
        vector[0].iov_base = ((char *) vector[0].iov_base) + sizeof(CARD32);
        vector[0].iov_len -= sizeof(CARD32);
        ++longlen;
        padded[1].iov_base = &longlen;
        padded[1].iov_len = sizeof(CARD32);
        padlen = 2;
    }

    for(i = 0; i < req->count; ++i)
    {
        if(!vector[i].iov_len)
            continue;
        padded[padlen].iov_base = vector[i].iov_base;
        padded[padlen++].iov_len = vector[i].iov_len;
        if(!XCB_PAD(vector[i].iov_len))
            continue;
        padded[padlen].iov_base = (caddr_t) pad;
        padded[padlen++].iov_len = XCB_PAD(vector[i].iov_len);
    }

    /* get a sequence number and arrange for delivery. */
    pthread_mutex_lock(&c->iolock);
    if(req->isvoid && !force_sequence_wrap(c))
    {
        pthread_mutex_unlock(&c->iolock);
#ifndef HAVE_ALLOCA
        free(padded);
#endif
        return -1;
    }

    *request = ++c->out.request;

    if(!req->isvoid)
        _xcb_in_expect_reply(c, *request, workaround);

    ret = _xcb_out_write_block(c, padded, padlen);
    pthread_mutex_unlock(&c->iolock);
#ifndef HAVE_ALLOCA
    free(padded);
#endif

    return ret;
}

int XCBFlush(XCBConnection *c)
{
    int ret;
    pthread_mutex_lock(&c->iolock);
    ret = _xcb_out_flush(c);
    pthread_mutex_unlock(&c->iolock);
    return ret;
}

/* Private interface */

int _xcb_out_init(_xcb_out *out)
{
    if(pthread_cond_init(&out->cond, 0))
        return 0;
    out->writing = 0;

    out->queue_len = 0;
    out->vec = 0;
    out->vec_len = 0;

    out->request = 0;
    out->request_written = 0;

    if(pthread_mutex_init(&out->reqlenlock, 0))
        return 0;
    out->maximum_request_length = 0;

    return 1;
}

void _xcb_out_destroy(_xcb_out *out)
{
    pthread_cond_destroy(&out->cond);
    pthread_mutex_destroy(&out->reqlenlock);
}

/* precondition: there must be something for us to write. */
int _xcb_out_write(XCBConnection *c)
{
    int n;
    assert(!c->out.queue_len);
    n = writev(c->fd, c->out.vec, c->out.vec_len);
    if(n < 0 && errno == EAGAIN)
        return 1;
    if(n <= 0)
        return 0;

    for(; c->out.vec_len; --c->out.vec_len, ++c->out.vec)
    {
        int cur = c->out.vec->iov_len;
        if(cur > n)
            cur = n;
        c->out.vec->iov_len -= cur;
        c->out.vec->iov_base = (char *) c->out.vec->iov_base + cur;
        n -= cur;
        if(c->out.vec->iov_len)
            break;
    }
    if(!c->out.vec_len)
        c->out.vec = 0;
    assert(n == 0);
    return 1;
}

int _xcb_out_write_block(XCBConnection *c, struct iovec *vector, size_t count)
{
    while(c->out.writing)
        pthread_cond_wait(&c->out.cond, &c->iolock);
    assert(!c->out.vec && !c->out.vec_len);
    while(count && c->out.queue_len + vector[0].iov_len < sizeof(c->out.queue))
    {
        memcpy(c->out.queue + c->out.queue_len, vector[0].iov_base, vector[0].iov_len);
        c->out.queue_len += vector[0].iov_len;
        ++vector, --count;
    }
    if(!count)
        return 1;

    memmove(vector + 1, vector, count++ * sizeof(struct iovec));
    vector[0].iov_base = c->out.queue;
    vector[0].iov_len = c->out.queue_len;
    c->out.queue_len = 0;

    c->out.vec_len = count;
    c->out.vec = vector;
    return _xcb_out_flush(c);
}

int _xcb_out_flush(XCBConnection *c)
{
    int ret = 1;
    struct iovec vec;
    if(c->out.queue_len)
    {
        assert(!c->out.vec && !c->out.vec_len);
        vec.iov_base = c->out.queue;
        vec.iov_len = c->out.queue_len;
        c->out.vec = &vec;
        c->out.vec_len = 1;
        c->out.queue_len = 0;
    }
    while(ret && c->out.vec_len)
        ret = _xcb_conn_wait(c, /*should_write*/ 1, &c->out.cond);
    c->out.request_written = c->out.request;
    pthread_cond_broadcast(&c->out.cond);
    return ret;
}
