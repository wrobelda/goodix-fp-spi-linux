/*
 * Dump the bytes crossing the client -> trusted app boundary.
 *
 *   gf_ca_invoke_command(handle, uint32 cmd_id, void *buf, uint32 len)
 *
 * logcat gives command ids and buffer sizes; this gives payload contents.
 * Pair it with android-listener-hook.js on qseecomd to get the other channel
 * -- the file service the app drives -- which is where a SAVE actually fails.
 *
 * Usage:  frida -U -n <fingerprint hal process> -l android-gfhook.js
 */

var NAMES = {
    1000: 'DETECT_SENSOR',
    1001: 'INIT',
    1002: 'EXIT',
    1003: 'DOWNLOAD_FW',
    1004: 'DOWNLOAD_CFG',
    1005: 'INIT_FINISHED',
    1006: 'PRE_ENROLL',
    1007: 'ENROLL',
    1008: 'POST_ENROLL',
    1009: 'CANCEL',
    1010: 'AUTHENTICATE',
    1011: 'GET_AUTH_ID',
    1012: 'SAVE',
    1013: 'REMOVE',
    1014: 'SET_ACTIVE_GROUP',
    1015: 'ENUMERATE',
    1016: 'IRQ',
    1017: 'SCREEN_ON',
    1018: 'SCREEN_OFF',
    1019: 'ESD_CHECK',
    1020: 'SET_SAFE_CLASS',
    1021: 'CAMERA_CAPTURE',
    1022: 'ENABLE_FINGERPRINT_MODULE',
    1023: 'ENABLE_FF_FEATURE',
    1024: 'TEST_BAD_POINT',
    1025: 'TEST_SENSOR_FINE',
    1026: 'TEST_SENSOR_FINE_FINISH',
    1027: 'TEST_PIXEL_OPEN',
    1028: 'TEST_PIXEL_OPEN_STEP1',
    1029: 'TEST_PIXEL_OPEN_FINISH',
    1030: 'TEST_PERFORMANCE',
    1031: 'TEST_SPI_PERFORMANCE',
    1032: 'TEST_SPI_TRANSFER',
    1033: 'TEST_PRE_SPI',
    1034: 'TEST_SPI',
    1035: 'TEST_SPI_RW',
    1036: 'TEST_PRE_GET_VERSION',
    1037: 'TEST_GET_VERSION',
    1038: 'TEST_FRR_FAR_INIT',
    1039: 'TEST_FRR_FAR_RECORD_CALIBRATION',
    1040: 'TEST_FRR_FAR_RECORD_ENROLL',
    1041: 'TEST_FRR_FAR_RECORD_AUTHENTICATE',
    1042: 'TEST_FRR_FAR_RECORD_AUTHENTICATE_FINISH',
    1043: 'TEST_FRR_FAR_PLAY_CALIBRATION',
    1044: 'TEST_FRR_FAR_PLAY_ENROLL',
    1045: 'TEST_FRR_FAR_PLAY_AUTHENTICATE',
    1046: 'TEST_FRR_FAR_ENROLL_FINISH',
    1047: 'TEST_FRR_FAR_SAVE_FINGER',
    1048: 'TEST_FRR_FAR_DEL_FINGER',
    1049: 'TEST_FRR_FAR_CANCEL',
    1050: 'TEST_RESET_PIN1',
    1051: 'TEST_RESET_PIN2',
    1052: 'TEST_INTERRUPT_PIN',
    1053: 'TEST_DOWNLOAD_FW',
    1054: 'TEST_DOWNLOAD_CFG',
    1055: 'TEST_DOWNLOAD_FWCFG',
    1056: 'TEST_RESET_FWCFG',
    1057: 'TEST_SENSOR_VALIDITY',
    1058: 'TEST_SET_CONFIG',
    1059: 'TEST_DRIVER_CMD',
    1060: 'TEST_UNTRUSTED_ENROLL',
    1061: 'TEST_UNTRUSTED_AUTHENTICATE',
    1062: 'TEST_DELETE_UNTRUSTED_ENROLLED_FINGER',
    1063: 'TEST_CHECK_FINGER_EVENT',
    1064: 'TEST_BIO_CALIBRATION',
    1065: 'TEST_HBD_CALIBRATION',
    1066: 'TEST_CANCEL',
    1067: 'TEST_REAL_TIME_DATA',
    1068: 'TEST_BMP_DATA',
    1069: 'TEST_READ_CFG',
    1070: 'TEST_READ_FW',
    1071: 'NAVIGATE',
    1072: 'DETECT_NAV_EVENT',
    1073: 'NAVIGATE_COMPLETE',
    1074: 'DUMP_NAV_DATA',
    1075: 'CHECK_FINGER_LONG_PRESS',
    1076: 'FDT_DOWN_TIMEOUT',
    1077: 'START_HBD',
    1078: 'AUTHENTICATE_FIDO',
    1079: 'DUMP_TEMPLATE',
    1080: 'DUMP_DATA',
    1081: 'DUMP_ORIGIN_DATA',
    1082: 'TEST_PRIOR_CANCEL',
    1083: 'TEST_NOISE',
    1084: 'TEST_RAWDATA_SATURATED',
    1085: 'UPDATE_STITCH',
    1086: 'AUTHENTICATE_FINISH',
    1087: 'DUMP_NAV_ENHANCE_DATA',
    1089: 'GET_DEV_INFO',
    1090: 'LOCKOUT',
    1091: 'PAUSE_ENROLL',
    1092: 'TEST_UNTRUSTED_PAUSE_ENROLL',
    1093: 'TEST_FPC_KEY_DETECT',
    1094: 'TEST_BAD_POINT_PRE_GET_BASE',
    1095: 'TEST_MEMORY_CHECK',
    1096: 'TEST_CALIBRATION_PARA_RETEST',
    1097: 'TEST_DATA_NOISE_BASE',
    1098: 'TEST_POLLING_IMAGE',
    1099: 'TEST_FRR_FAR_PREPROCESS_INIT',
    1100: 'TEST_RESET_CLEAR',
    1101: 'TEST_SENSOR_BROKEN',
};

function nm(id) { return NAMES[id] ? (id + " " + NAMES[id]) : String(id); }

function hex(p, n) {
    if (p === null || p.isNull() || n <= 0) return "  <null>";
    try { return hexdump(p, { length: Math.min(n, 1024), header: false, ansi: false }); }
    catch (e) { return "  <unreadable: " + e + ">"; }
}

var ca = Process.findModuleByName("libgf_ca.so");
send("libgf_ca.so @ " + (ca ? ca.base : "not loaded"));

var inv = (ca !== null) ? ca.findExportByName("gf_ca_invoke_command") : null;
if (inv !== null) {
    Interceptor.attach(inv, {
        onEnter: function (args) {
            this.cmd = args[1].toInt32();
            this.buf = args[2];
            this.len = args[3].toInt32();
            send("\n=== CA cmd " + nm(this.cmd) + "  len=" + this.len +
                 "  (handle=" + args[0] + ")");
            send("REQ payload:\n" + hex(this.buf, this.len));
        },
        onLeave: function (ret) {
            send("--- cmd " + nm(this.cmd) + " ret=" + ret);
            send("RSP payload:\n" + hex(this.buf, this.len));
        }
    });
} else {
    send("!! gf_ca_invoke_command not found");
}
