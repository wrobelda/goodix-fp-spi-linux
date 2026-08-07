// Trace the gatekeeper trusted application, which is what mints the
// hw_auth_token_t the Goodix application insists on. Same transport as the
// fingerprint side -- libQSEEComAPI -- so the same hook shape works.
//
// The interesting direction is the response: a successful verify() returns a
// signed token, and we know its layout well enough to spot it.

function hex(p, n) {
    if (p === null || p.isNull() || n <= 0) return "  <null>";
    try { return hexdump(p, { length: Math.min(n, 512), header: false, ansi: false }); }
    catch (e) { return "  <unreadable: " + e + ">"; }
}

// hw_auth_token_t: u8 version=0, u64 LE challenge, u64 LE user_id,
// u64 LE authenticator_id, u32 BE authenticator_type, u64 BE timestamp,
// u8[32] hmac -- 69 bytes packed.
function findHAT(p, len) {
    for (var off = 0; off + 69 <= len; off++) {
        try {
            if (p.add(off).readU8() !== 0) continue;
            var atype = p.add(off + 25).readU32();          // BE on the wire
            atype = ((atype & 0xff) << 24) | ((atype & 0xff00) << 8) |
                    ((atype >> 8) & 0xff00) | ((atype >>> 24) & 0xff);
            if (atype !== 1 && atype !== 2) continue;
            var hmac = [];
            var nz = 0;
            for (var i = 0; i < 32; i++) {
                var b = p.add(off + 37 + i).readU8();
                hmac.push(('0' + b.toString(16)).slice(-2));
                if (b) nz++;
            }
            if (nz < 16) continue;                          // want a real signature
            var chLo = p.add(off + 1).readU32(), chHi = p.add(off + 5).readU32();
            var uidLo = p.add(off + 9).readU32(), uidHi = p.add(off + 13).readU32();
            send("  *** hw_auth_token_t at +0x" + off.toString(16));
            send("      challenge          = 0x" + chHi.toString(16).padStart(8, '0') +
                 chLo.toString(16).padStart(8, '0'));
            send("      user_id            = 0x" + uidHi.toString(16).padStart(8, '0') +
                 uidLo.toString(16).padStart(8, '0'));
            send("      authenticator_type = " + atype);
            send("      hmac               = " + hmac.join(''));
            return;
        } catch (e) { /* keep scanning */ }
    }
}

var qsee = Process.findModuleByName("libQSEEComAPI.so");
send("libQSEEComAPI.so @ " + (qsee ? qsee.base : "NOT LOADED"));

function resolve(name) {
    if (qsee !== null) {
        var a = qsee.findExportByName(name);
        if (a !== null) return a;
    }
    try { return Module.getGlobalExportByName(name); } catch (e) { return null; }
}

var started = resolve("QSEECom_start_app");
if (started !== null) Interceptor.attach(started, {
    onEnter: function (args) {
        try { send("QSEECom_start_app  name=\"" + args[2].readCString() +
                   "\"  sb_len=" + args[3].toInt32()); } catch (e) {}
    }
});

["QSEECom_send_modified_cmd", "QSEECom_send_cmd"].forEach(function (name) {
    var f = resolve(name);
    if (f === null) { send("no export " + name); return; }
    Interceptor.attach(f, {
        onEnter: function (args) {
            this.name = name;
            this.req = args[1]; this.reqLen = args[2].toInt32();
            this.rsp = args[3]; this.rspLen = args[4].toInt32();
            send("\n>>> " + name + " reqLen=" + this.reqLen + " rspLen=" + this.rspLen);
            send("REQ:\n" + hex(this.req, this.reqLen));
        },
        onLeave: function (ret) {
            send("<<< " + this.name + " ret=" + ret);
            send("RSP:\n" + hex(this.rsp, this.rspLen));
            findHAT(this.rsp, this.rspLen);
        }
    });
    send("hooked " + name);
});
