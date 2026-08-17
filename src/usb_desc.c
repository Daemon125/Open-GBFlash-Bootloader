/* usb_desc.c — CH340 emulation descriptors and canned vendor-reply table.
 *
 * Owner: comp:usb.
 *
 * EVERY BYTE IN THIS FILE IS A VERBATIM TRANSCRIPTION from the GBFlash L15
 * application image, and was verified byte-for-byte against that image.
 *
 *   device descriptor   flash 0xB408, 18 bytes
 *   config descriptor   flash 0xB41A, 39 bytes  (wTotalLength 0x0027)
 *   vendor-IN table     flash 0xB441, 26 bytes
 *
 * DO NOT "CORRECT" ANYTHING HERE.  In particular:
 *   - bcdDevice is 0x0304.  A genuine CH340G reports 0x0263.  The author chose
 *     0x0304 and the host stack this device is known to work with accepted it.
 *   - bMaxPacketSize0 is 8, not 64.  The whole EP0 path in usb.c clamps to 8
 *     because of this byte; the two must change together or not at all.
 *   - The 26-byte table is NOT one blob.  It is thirteen independent two-byte
 *     replies handed out in call order to successive bmRequestType==0xC0
 *     requests.  bRequest / wValue / wIndex are never examined by the
 *     application, so the recording preserves only the ORDER, not which host
 *     request produced which pair.  It cannot be "decoded"; reproduce it.
 *   - EP1 (0x81, interrupt IN, 8 B) is declared and never driven.  Keep the
 *     descriptor entry: the ch341 driver walks this exact layout.
 *
 * String descriptors are deliberately absent.  The application carries three
 * at 0xB45B but they have zero cross-references image-wide, every string index
 * in the descriptors above is 0, and GET_DESCRIPTOR type 3 STALLs (0x48F8).
 * Omitting them is 30 bytes saved with no observable difference.
 */

#include <stdint.h>

#include "usb.h"

/* Device descriptor — flash 0xB408.
 * VID 0x1A86 / PID 0x7523, bcdUSB 0x0110, class FF/00/02, bMaxPacketSize0 8,
 * bcdDevice 0x0304, no strings, one configuration. */
const uint8_t bl_usb_desc_device[18] = {
    0x12, 0x01, 0x10, 0x01, 0xFF, 0x00, 0x02, 0x08, 0x86,
    0x1A, 0x23, 0x75, 0x04, 0x03, 0x00, 0x00, 0x00, 0x01,
};

/* Configuration descriptor set — flash 0xB41A.  9 + 9 + 7 + 7 + 7 = 39.
 * One vendor-specific interface (FF/01/02) carrying all three endpoints and no
 * class-specific descriptors of any kind — this is a CH340, not a CDC-ACM
 * device, and the host binds it by VID:PID. */
const uint8_t bl_usb_desc_config[39] = {
    /* configuration: 1 interface, bConfigurationValue 1, bus powered, 480 mA */
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0xF0,
    /* interface 0, alt 0, 3 endpoints, class FF / subclass 01 / protocol 02  */
    0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0x01, 0x02, 0x00,
    /* EP 0x82 bulk IN,  wMaxPacketSize 32, bInterval 0  — device -> host     */
    0x07, 0x05, 0x82, 0x02, 0x20, 0x00, 0x00,
    /* EP 0x02 bulk OUT, wMaxPacketSize 32, bInterval 0  — host -> device     */
    0x07, 0x05, 0x02, 0x02, 0x20, 0x00, 0x00,
    /* EP 0x81 interrupt IN, 8 B, bInterval 1 — DECLARED, NEVER DRIVEN        */
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01,
};

/* CH340 canned vendor-IN replies — flash 0xB441.  Byte order as delivered
 * into ep0buf[0], ep0buf[1]; note the vendor SDK lists these as little-endian
 * halfwords, which reverses each pair.  This is the byte order. */
const uint8_t bl_usb_ch340_vendor_tbl[26] = {
    0x30, 0x00,   /*  1: chip version 0x30, what a real CH340G reports        */
    0xC3, 0x00,   /*  2                                                       */
    0xFF, 0xEC,   /*  3: modem status -> no lines asserted                    */
    0x9F, 0xEC,   /*  4                                                       */
    0xFF, 0xEC,   /*  5                                                       */
    0xDF, 0xEC,   /*  6                                                       */
    0xDF, 0xEC,   /*  7                                                       */
    0xDF, 0xEC,   /*  8                                                       */
    0x9F, 0xEC,   /*  9                                                       */
    0x9F, 0xEC,   /* 10                                                       */
    0x9F, 0xEC,   /* 11                                                       */
    0x9F, 0xEC,   /* 12                                                       */
    0xFF, 0xEC,   /* 13: and every request after the 13th                     */
};

/* Compile-time proof that the three arrays still say what the image says.
 * These are the fields a well-meaning edit is most likely to "fix". */
#if defined(__GNUC__) && !defined(__cplusplus)
__extension__ _Static_assert(sizeof bl_usb_desc_device == 18,
                             "device descriptor must be 18 bytes");
__extension__ _Static_assert(sizeof bl_usb_desc_config == 39,
                             "config descriptor set must be 39 bytes");
__extension__ _Static_assert(sizeof bl_usb_ch340_vendor_tbl == 26,
                             "vendor table must be 26 bytes = 13 replies");
__extension__ _Static_assert(BL_USB_VENDOR_TBL_LAST + 2u
                             == sizeof bl_usb_ch340_vendor_tbl,
                             "cursor must saturate on the last pair");
#endif
