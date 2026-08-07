/*
 * Capture the QSEE file-service listener channel on Android.
 *
 * The Goodix command channel (HAL -> trusted app) was captured by hooking
 * gf_ca_invoke_command in libgf_ca.so. This is the other half: the channel the
 * trusted app uses to ask the normal world to do file I/O on its behalf, which
 * is served by qseecomd via libdrmfs.so, in a different process entirely.
 *
 * That is the channel a failing SAVE fails on, so a working SAVE captured here
 * is the reference we lack -- which files get created, with which flags, in
 * what order, and what is written into them.
 *
 * Hook both ends of the transport:
 *
 *   QSEECom_receive_req(handle, buf, len)   the request, filled on return
 *   QSEECom_send_resp(handle, buf, len)     the reply, read on entry
 *
 * Attach to qseecomd (not the fingerprint HAL), enrol a finger, and the whole
 * conversation lands in the log.
 *
 * Usage:  frida -U -n qseecomd -l android-listener-hook.js
 *   or the resident Python driver, as with the other hooks here.
 */

'use strict';

var OPS = {
    0x202: 'open',    0x203: 'openat',  0x204: 'unlinkat', 0x205: 'open_fcntl',
    0x206: 'creat',   0x207: 'read',    0x208: 'write',    0x209: 'close',
    0x20a: 'lseek',   0x20b: 'link',    0x20c: 'unlink',   0x20d: 'opendir',
    0x20e: 'fstat',   0x20f: 'lstat',   0x210: 'stat',     0x211: 'mkdir',
    0x212: 'statfs',  0x213: 'statfs2', 0x214: 'rename',   0x215: 'unhandled',
    0x216: 'fsync',   0x217: 'statfs3', 0x218: 'pathop',   0x219: 'fstat2',
    0x21a: 'readdir', 0x21b: 'closedir', 0x21c: 'geterrno', 0x21d: 'shutdown',
};

function out(s) { send({ type: 'log', msg: s }); }

/* Dump only what is worth reading: the head, and the tail where the write
 * count lives (+20008). Dumping 20 KB per request would drown the log. */
function head(p, n) {
    try { return hexdump(p, { length: n, header: false, ansi: false }); }
    catch (e) { return '<unreadable>'; }
}

function describe(p, tag) {
    var op = p.readU32();
    var name = OPS[op] || ('0x' + op.toString(16));
    var line = tag + ' op=0x' + op.toString(16) + ' (' + name + ')';

    try {
        if (op === 0x202 || op === 0x206 || op === 0x20c || op === 0x20f ||
            op === 0x210 || op === 0x211) {
            /* path-taking: path at +4, open flags at +260 */
            line += ' path="' + p.add(4).readCString() + '"';
            if (op === 0x202)
                line += ' flags=0x' + p.add(260).readU32().toString(16);
        } else {
            /* descriptor-based: fd at +4 */
            line += ' fd=' + p.add(4).readS32();
            if (op === 0x207) line += ' count=' + p.add(8).readU32();
            if (op === 0x208) line += ' count=' + p.add(20008).readU32();
            if (op === 0x20a)
                line += ' off=' + p.add(8).readS32() +
                        ' whence=' + p.add(12).readU32();
        }
    } catch (e) { line += ' <parse failed: ' + e + '>'; }

    return line;
}

var lastReq = null;

function attach() {
    var lib = Process.findModuleByName('libQSEEComAPI.so');
    if (!lib) return false;

    var recv = lib.findExportByName('QSEECom_receive_req');
    var resp = lib.findExportByName('QSEECom_send_resp');
    if (!recv || !resp) {
        out('!! exports not found in libQSEEComAPI.so');
        return false;
    }

    Interceptor.attach(recv, {
        onEnter: function (a) { this.buf = a[1]; this.h = a[0]; },
        onLeave: function (r) {
            if (!this.buf || this.buf.isNull()) return;
            lastReq = this.buf;
            out('');
            out('>>> h=' + this.h + ' ' + describe(this.buf, 'REQ'));
            out(head(this.buf, 288));
            /* the write payload and its count live far out in the buffer */
            var op = this.buf.readU32();
            if (op === 0x208) {
                out('    write data @+8:');
                out(head(this.buf.add(8), 64));
                out('    count @+20008: ' + this.buf.add(20008).readU32());
            }
        },
    });

    Interceptor.attach(resp, {
        onEnter: function (a) {
            var p = a[1], len = a[2].toInt32();
            if (p.isNull()) return;
            out('<<< h=' + a[0] + ' RSP op=0x' + p.readU32().toString(16) +
                ' ret=' + p.add(4).readS32() + ' len=' + len);
            if (len > 8) {
                out('    data @+4:');
                out(head(p.add(4), 64));
                out('    nread @+20004: ' + p.add(20004).readS32());
            }
        },
    });

    out('== hooked QSEECom_receive_req / QSEECom_send_resp ==');
    return true;
}

if (!attach()) {
    /* qseecomd may not have the library mapped yet */
    var loader = Module.getGlobalExportByName('android_dlopen_ext');
    Interceptor.attach(loader, {
        onLeave: function () {
            if (Process.findModuleByName('libQSEEComAPI.so')) {
                if (attach()) Interceptor.detachAll();
            }
        },
    });
    out('== waiting for libQSEEComAPI.so to load ==');
}
