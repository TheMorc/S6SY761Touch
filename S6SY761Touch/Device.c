/*++

Copyright (C) Microsoft Corporation, All Rights Reserved.

SPDX-Licence-Identifier: GPL-2.0
Copyright (c) 2017  Samsung Electronics Co., Ltd.
Copyright (c) 2017  Andi Shyti <andi@etezian.org>
					Andi Shyti <andi.shyti@samsung.com>

Copyright (c) 2022 - 2023  Morc - Richard Gr��ik

Module Name:


	S6SY761Touch.c

Abstract:F

	This module contains the implementation of the driver

Environment:

	Windows Driver Framework (WDF)

--*/

#include "Device.h"


NTSTATUS
RequestGetHidXferPacket_ToReadFromDevice(
	_In_  WDFREQUEST        Request,
	_Out_ HID_XFER_PACKET* Packet
)
{
	NTSTATUS                status;
	WDF_REQUEST_PARAMETERS  params;

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET)) {
		status = STATUS_BUFFER_TOO_SMALL;
		KdPrint(("RequestGetHidXferPacket: invalid HID_XFER_PACKET\n"));
		return status;
	}

	RtlCopyMemory(Packet, WdfRequestWdmGetIrp(Request)->UserBuffer, sizeof(HID_XFER_PACKET));
	return STATUS_SUCCESS;
}

NTSTATUS
RequestGetHidXferPacket_ToWriteToDevice(
	_In_  WDFREQUEST        Request,
	_Out_ HID_XFER_PACKET* Packet
)
{
	NTSTATUS                status;
	WDF_REQUEST_PARAMETERS  params;

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET)) {
		status = STATUS_BUFFER_TOO_SMALL;
		KdPrint(("RequestGetHidXferPacket: invalid HID_XFER_PACKET\n"));
		return status;
	}

	RtlCopyMemory(Packet, WdfRequestWdmGetIrp(Request)->UserBuffer, sizeof(HID_XFER_PACKET));
	return STATUS_SUCCESS;
}


#define S6SY761_EVENT_SIZE  8
#define S6SY761_EVENT_COUNT  32

#define S6SY761_MASK_LEFT_EVENTS 0x37
#define S6SY761_MASK_TOUCH_STATE 0xc0
#define S6SY761_MASK_TID 0x3c
#define S6SY761_MASK_EID 0x03
#define S6SY761_MASK_X 0xf0
#define S6SY761_MASK_Y 0x0f

#define S6SY761_TS_NONE 0x00
#define S6SY761_TS_PRESS 0x01
#define S6SY761_TS_MOVE 0x02
#define S6SY761_TS_RELEASE 0x03

/* event id */
#define S6SY761_EVENT_ID_COORDINATE	0x00
#define S6SY761_EVENT_ID_STATUS		0x01

BYTE S6SY761_READ_ALL_EVENTS[1] = { 0x61 }; //read all events
BYTE S6SY761_READ_ONE_EVENT[1] = { 0x60 }; //read single event
BYTE S6SY761_CLEAR_EVENT_STACK[1] = { 0x62 }; //drain all last commands
BYTE S6SY761_SENSE_ON[1] = { 0x10 }; //enable sensing

UINT8 eventbuf[S6SY761_EVENT_SIZE * S6SY761_EVENT_COUNT];

ULONG XRevert = 0;
ULONG YRevert = 0;
ULONG XYExchange = 0;
ULONG XMin = 0;
ULONG XMax = 1440;
ULONG YMin = 0;
ULONG YMax = 3120;

#define DEFINE_GUID2(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        EXTERN_C const GUID DECLSPEC_SELECTANY name \
                = { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }





typedef struct
{
	BYTE  reportId;                                 // Report ID = 0x54 (84) 'T'
													   // Collection: TouchScreen
	BYTE  DIG_TouchScreenContactCountMaximum;       // Usage 0x000D0055: Contact Count Maximum, Value = 0 to 8
} featureReport54_t;

typedef struct __declspec(align(2))
{
	BYTE  DIG_TouchScreenFingerState;               // Usage 0x000D0042: Tip Switch, Value = 0 to 1
	BYTE  DIG_TouchScreenFingerContactIdentifier;   // Usage 0x000D0051: Contact Identifier, Value = 0 to 1
	BYTE GD_TouchScreenFingerXL;                    // Usage 0x00010030: X, Value = 0 to 32767
	BYTE GD_TouchScreenFingerXH;                    // Usage 0x00010030: X, Value = 0 to 32767
	BYTE GD_TouchScreenFingerYL;                    // Usage 0x00010031: Y, Value = 0 to 32767
	BYTE GD_TouchScreenFingerYH;                    // Usage 0x00010031: Y, Value = 0 to 32767
}inputpoint;

typedef struct __declspec(align(2))
{
	BYTE  reportId;                                 // Report ID = 0x54 (84) 'T'
													   // Collection: TouchScreen Finger
	BYTE points[60];

	BYTE  DIG_TouchScreenContactCount;              // Usage 0x000D0054: Contact Count, Value = 0 to 8
} inputReport54_t;

//
// This is the default report descriptor for the virtual Hid device returned
// by the mini driver in response to IOCTL_HID_GET_REPORT_DESCRIPTOR.
//
/*HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
	0x06,0x00, 0xFF,                // USAGE_PAGE (Vender Defined Usage Page)
	0x09,0x01,                      // USAGE (Vendor Usage 0x01)
	0xA1,0x01,                      // COLLECTION (Application)
	0x85,CONTROL_FEATURE_REPORT_ID,    // REPORT_ID (1)
	0x09,0x01,                         // USAGE (Vendor Usage 0x01)
	0x15,0x00,                         // LOGICAL_MINIMUM(0)
	0x26,0xff, 0x00,                   // LOGICAL_MAXIMUM(255)
	0x75,0x08,                         // REPORT_SIZE (0x08)
	0x96,(FEATURE_REPORT_SIZE_CB & 0xff), (FEATURE_REPORT_SIZE_CB >> 8), // REPORT_COUNT
	0xB1,0x00,                         // FEATURE (Data,Ary,Abs)
	0x09,0x01,                         // USAGE (Vendor Usage 0x01)
	0x75,0x08,                         // REPORT_SIZE (0x08)
	0x96,(INPUT_REPORT_SIZE_CB & 0xff), (INPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
	0x81,0x00,                         // INPUT (Data,Ary,Abs)
	0x09,0x01,                         // USAGE (Vendor Usage 0x01)
	0x75,0x08,                         // REPORT_SIZE (0x08)
	0x96,(OUTPUT_REPORT_SIZE_CB & 0xff), (OUTPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
	0x91,0x00,                         // OUTPUT (Data,Ary,Abs)
	0xC0,                           // END_COLLECTION
};*/

HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x04,     //   (LOCAL)USAGE              0x000D0004 Touch Screen(Application Collection)
	0xA1, 0x01,     //   (MAIN)COLLECTION         0x01 Application(Usage = 0x000D0004: Page = Digitizer Device Page, Usage = Touch Screen, Type = Application Collection)
	0x85, 0x54,     //     (GLOBAL)REPORT_ID          0x54 (84) 'T'

	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x14,           //    (GLOBAL)LOGICAL_MINIMUM(0)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //46 47
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //55 56
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
	0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
	0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
	0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
	0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
	0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
	0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
	0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (1080)    //99 100
	0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
	0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x26, 0xCA, 0x08,   // (GLOBAL) LOGICAL_MAXIMUM    0x7FFF (2250)    //108 109
	0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
	0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Logical

	0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
	0x09, 0x54,     //     (LOCAL)USAGE              0x000D0054 Contact Count(Dynamic Value)
	0x75, 0x08,     //     (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
	0x25, 0x0A,     //     (GLOBAL)LOGICAL_MAXIMUM    0x08 (8)
	0x81, 0x02,     //     (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0x09, 0x55,     //     (LOCAL)USAGE              0x000D0055 Contact Count Maximum(Static Value)
	0xB1, 0x02,     //     (MAIN)FEATURE            0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
	0xC0,           // (MAIN)   END_COLLECTION     Application

};

featureReport54_t features = { 0x54,10 };
//
// This is the default HID descriptor returned by the mini driver
// in response to IOCTL_HID_GET_DEVICE_DESCRIPTOR. The size
// of report descriptor is currently the size of G_DefaultReportDescriptor.
//

HID_DESCRIPTOR              G_DefaultHidDescriptor = {
	0x09,   // length of HID descriptor
	0x21,   // descriptor type == HID  0x21
	0x0100, // hid spec release
	0x00,   // country code == Not Specified
	0x01,   // number of HID class descriptors
	{                                       //DescriptorList[0]
		0x22,                               //report descriptor type 0x22
		sizeof(G_DefaultReportDescriptor)   //total length of report descriptor
	}
};

NTSTATUS
DriverEntry(
	_In_  PDRIVER_OBJECT    DriverObject,
	_In_  PUNICODE_STRING   RegistryPath
)
/*++

Routine Description:
	DriverEntry initializes the driver and is the first routine called by the
	system after the driver is loaded. DriverEntry specifies the other entry
	points in the function driver, such as EvtDevice and DriverUnload.

Parameters Description:

	DriverObject - represents the instance of the function driver that is loaded
	into memory. DriverEntry must initialize members of DriverObject before it
	returns to the caller. DriverObject is allocated by the system before the
	driver is loaded, and it is released by the system after the system unloads
	the function driver from memory.

	RegistryPath - represents the driver specific path in the Registry.
	The function driver can use the path to store driver related data between
	reboots. The path does not store hardware instance specific data.

Return Value:

	STATUS_SUCCESS, or another status value for which NT_SUCCESS(status) equals
					TRUE if successful,

	STATUS_UNSUCCESSFUL, or another status for which NT_SUCCESS(status) equals
					FALSE otherwise.

--*/
{
	WDF_DRIVER_CONFIG       config;
	WDF_OBJECT_ATTRIBUTES driverAttributes;
	NTSTATUS                status;

#ifdef _KERNEL_MODE
	//
	// Opt-in to using non-executable pool memory on Windows 8 and later.
	// https://msdn.microsoft.com/en-us/library/windows/hardware/hh920402(v=vs.85).aspx
	//
	ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
#endif

	DbgPrint("fdsfdsa");
	KdPrint(("dfdfsdf"));

	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
	driverAttributes.EvtCleanupCallback = EvtDriverCleanup;

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&driverAttributes,
		&config,
		WDF_NO_HANDLE);
	if (!NT_SUCCESS(status)) {

		goto Exit;
	}
Exit:
	return status;
}
VOID
EvtDriverCleanup(
	_In_ WDFOBJECT Object
)
{
	UNREFERENCED_PARAMETER(Object);
}
NTSTATUS
EvtDeviceAdd(
	_In_  WDFDRIVER         Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
/*++
Routine Description:

	EvtDeviceAdd is called by the framework in response to AddDevice
	call from the PnP manager. We create and initialize a device object to
	represent a new instance of the device.

Arguments:

	Driver - Handle to a framework driver object created in DriverEntry

	DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDF_OBJECT_ATTRIBUTES   deviceAttributes;
	WDFDEVICE               device;
	PDEVICE_CONTEXT         deviceContext;
	PHID_DEVICE_ATTRIBUTES  hidAttributes;
	UNREFERENCED_PARAMETER(Driver);

	WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

	pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
	pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
	pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
	pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;


	//
	// Mark ourselves as a filter, which also relinquishes power policy ownership
	//
	WdfFdoInitSetFilter(DeviceInit);

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
		&deviceAttributes,
		DEVICE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit,
		&deviceAttributes,
		&device);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	deviceContext = GetDeviceContext(device);
	deviceContext->Device = device;
	deviceContext->DeviceData = 0;

	hidAttributes = &deviceContext->HidDeviceAttributes;
	RtlZeroMemory(hidAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
	hidAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	hidAttributes->VendorID = HIDMINI_VID;
	hidAttributes->ProductID = HIDMINI_PID;
	hidAttributes->VersionNumber = HIDMINI_VERSION;

	status = QueueCreate(device,
		&deviceContext->DefaultQueue);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = ManualQueueCreate(device,
		&deviceContext->ManualQueue);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	//
	// Use default "HID Descriptor" (hardcoded). We will set the
	// wReportLength memeber of HID descriptor when we read the
	// the report descriptor either from registry or the hard-coded
	// one.
	//
	deviceContext->HidDescriptor = G_DefaultHidDescriptor;

	//
	// We need to read read descriptor from registry
	//
	status = ReadDescriptorFromRegistry(device);
	if (!NT_SUCCESS(status)) {
	}

	G_DefaultReportDescriptor[46] = XMax & 0xFF;
	G_DefaultReportDescriptor[47] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55] = YMax & 0xFF;
	G_DefaultReportDescriptor[56] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 1] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 1] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 1] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 1] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 2] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 2] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 2] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 2] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 3] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 3] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 3] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 3] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 4] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 4] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 4] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 4] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 5] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 5] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 5] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 5] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 6] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 6] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 6] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 6] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 7] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 7] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 7] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 7] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 8] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 8] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 8] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 8] = (YMax >> 8) & 0x0F;

	G_DefaultReportDescriptor[46 + 53 * 9] = XMax & 0xFF;
	G_DefaultReportDescriptor[47 + 53 * 9] = (XMax >> 8) & 0x0F;
	G_DefaultReportDescriptor[55 + 53 * 9] = YMax & 0xFF;
	G_DefaultReportDescriptor[56 + 53 * 9] = (YMax >> 8) & 0x0F;

	deviceContext->ReportDescriptor = G_DefaultReportDescriptor;
	status = STATUS_SUCCESS;

	return status;
}

NTSTATUS
PowerSettingCallback(
	_In_ LPCGUID SettingGuid,
	_In_ PVOID Value,
	_In_ ULONG ValueLength,
	_Inout_opt_ PVOID Context
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT devContext = NULL;
	UNREFERENCED_PARAMETER(SettingGuid);
	UNREFERENCED_PARAMETER(ValueLength);
	UNREFERENCED_PARAMETER(Value);

	DbgPrint("PowerSettingsCallback");

	if (Context == NULL)
	{
		DbgPrint(
			"TchPowerSettingCallback: Context is NULL"
		);

		status = STATUS_INVALID_DEVICE_REQUEST;
		goto exit;
	}

	devContext = (PDEVICE_CONTEXT)Context;
	

exit:
	return status;
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

	Routine Description:

	This routine caches the SPB resource connection ID.

	Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesRaw - list of translated hardware resources that
		the PnP manager has assigned to the device
	FxResourcesTranslated - list of raw hardware resources that
		the PnP manager has assigned to the device

	Return Value:

	Status

--*/
{
	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	BOOLEAN fInterruptResourceFound = FALSE;
	ULONG interruptIndex = 0;
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:

			//
			// Look for I2C or SPI resource and save connection ID.
			//

			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;

			if ((Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL) &&
				((Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)))
			{
				if (fSpbResourceFound == FALSE)
				{
					pDevice->PeripheralId.LowPart =
						pDescriptor->u.Connection.IdLowPart;
					pDevice->PeripheralId.HighPart =
						pDescriptor->u.Connection.IdHighPart;

					fSpbResourceFound = TRUE;
				}
			}

			break;

		case CmResourceTypeInterrupt:

			if (fInterruptResourceFound == FALSE)
			{
				fInterruptResourceFound = TRUE;
				interruptIndex = i;
			}
			break;

		default:

			//
			// Ignoring all other resource types.
			//

			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}


	//POWER STUFF
	/*status = PoRegisterPowerSettingCallback(
		NULL,
		&GUID_ACDC_POWER_SOURCE,
		PowerSettingCallback,
		pDevice,
		&pDevice->PoFxPowerSettingCallbackHandle1
	);*/

	if (!NT_SUCCESS(status))
	{
		DbgPrint("Error registering power setting callback (1) - 0x%08lX", status);
	}

	if (NT_SUCCESS(status))
	{
		DbgPrint("Successfully registered power setting callback (1) - 0x%08lX", status);
	}

	/*status = PoRegisterPowerSettingCallback(
		NULL,
		&GUID_CONSOLE_DISPLAY_STATE,
		PowerSettingCallback,
		pDevice,
		&pDevice->PoFxPowerSettingCallbackHandle2
	);*/

	if (!NT_SUCCESS(status))
	{
		DbgPrint("Error registering power setting callback (2) - 0x%08lX", status);

	}

	if (NT_SUCCESS(status))
	{
		DbgPrint("Successfully registered power setting callback (2) - 0x%08lX", status);
	}


	//
	// Create the interrupt if an interrupt
	// resource was found.
	//

	if (NT_SUCCESS(status))
	{
		if (fInterruptResourceFound == TRUE)
		{
			WDF_INTERRUPT_CONFIG interruptConfig;
			WDF_INTERRUPT_CONFIG_INIT(
				&interruptConfig,
				OnInterruptIsr,
				NULL);

			interruptConfig.PassiveHandling = TRUE;
			interruptConfig.InterruptTranslated = WdfCmResourceListGetDescriptor(
				FxResourcesTranslated,
				interruptIndex);
			interruptConfig.InterruptRaw = WdfCmResourceListGetDescriptor(
				FxResourcesRaw,
				interruptIndex);

			status = WdfInterruptCreate(
				pDevice->Device,
				&interruptConfig,
				WDF_NO_OBJECT_ATTRIBUTES,
				&pDevice->Interrupt);

			if (!NT_SUCCESS(status))
			{
			}

			if (NT_SUCCESS(status))
			{
				WdfInterruptDisable(pDevice->Interrupt);
			}
		}
	}

	DbgPrint("interruptIndex %d\n", interruptIndex);

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

	Routine Description:

	Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesTranslated - list of raw hardware resources that
		the PnP manager has assigned to the device

	Return Value:

	Status

--*/
{
	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	if (pDevice->Interrupt != NULL)
	{
		WdfObjectDelete(pDevice->Interrupt);
	}

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

	Routine Description:

	This routine allocates objects needed by the driver.

	Arguments:

	FxDevice - a handle to the framework device object
	FxPreviousState - previous power state

	Return Value:

	Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status;

	//
	// Create the SPB target.
	//

	WDF_OBJECT_ATTRIBUTES targetAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);

	status = WdfIoTargetCreate(
		pDevice->Device,
		&targetAttributes,
		&pDevice->SpbController);

	if (!NT_SUCCESS(status))
	{
	}

	SpbDeviceOpen(pDevice);

	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

	Routine Description:

	This routine destroys objects needed by the driver.

	Arguments:

	FxDevice - a handle to the framework device object
	FxPreviousState - previous power state

	Return Value:

	Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);

	SpbDeviceClose(pDevice);

	if (pDevice->SpbController != WDF_NO_HANDLE)
	{
		WdfObjectDelete(pDevice->SpbController);
		pDevice->SpbController = WDF_NO_HANDLE;
	}

	return STATUS_SUCCESS;
}

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoDeviceControl;


NTSTATUS
QueueCreate(
	_In_  WDFDEVICE         Device,
	_Out_ WDFQUEUE* Queue
)
/*++
Routine Description:

	This function creates a default, parallel I/O queue to proces IOCTLs
	from hidclass.sys.

Arguments:

	Device - Handle to a framework device object.

	Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDF_IO_QUEUE_CONFIG     queueConfig;
	WDF_OBJECT_ATTRIBUTES   queueAttributes;
	WDFQUEUE                queue;
	PQUEUE_CONTEXT          queueContext;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
		&queueConfig,
		WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = EvtIoDeviceControl;


	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
		&queueAttributes,
		QUEUE_CONTEXT);

	status = WdfIoQueueCreate(
		Device,
		&queueConfig,
		&queueAttributes,
		&queue);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	queueContext = GetQueueContext(queue);
	queueContext->Queue = queue;
	queueContext->DeviceContext = GetDeviceContext(Device);
	queueContext->OutputReport = 0;

	*Queue = queue;

	return status;
}

VOID
EvtIoDeviceControl(
	_In_  WDFQUEUE          Queue,
	_In_  WDFREQUEST        Request,
	_In_  size_t            OutputBufferLength,
	_In_  size_t            InputBufferLength,
	_In_  ULONG             IoControlCode
)
/*++
Routine Description:

	This event callback function is called when the driver receives an

	(KMDF) IOCTL_HID_Xxx code when handlng IRP_MJ_INTERNAL_DEVICE_CONTROL
	(UMDF) IOCTL_HID_Xxx, IOCTL_UMDF_HID_Xxx when handling IRP_MJ_DEVICE_CONTROL

Arguments:

	Queue - A handle to the queue object that is associated with the I/O request

	Request - A handle to a framework request object.

	OutputBufferLength - The length, in bytes, of the request's output buffer,
			if an output buffer is available.

	InputBufferLength - The length, in bytes, of the request's input buffer, if
			an input buffer is available.

	IoControlCode - The driver or system defined IOCTL associated with the request

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	BOOLEAN                 completeRequest = TRUE;
	WDFDEVICE               device = WdfIoQueueGetDevice(Queue);
	PDEVICE_CONTEXT         deviceContext = NULL;
	PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	deviceContext = GetDeviceContext(device);

	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:   // METHOD_NEITHER
		//
		// Retrieves the device's HID descriptor.
		//
		_Analysis_assume_(deviceContext->HidDescriptor.bLength != 0);
		status = RequestCopyFromBuffer(Request,
			&deviceContext->HidDescriptor,
			deviceContext->HidDescriptor.bLength);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:   // METHOD_NEITHER
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = RequestCopyFromBuffer(Request,
			&queueContext->DeviceContext->HidDeviceAttributes,
			sizeof(HID_DEVICE_ATTRIBUTES));
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:   // METHOD_NEITHER
		//
		//Obtains the report descriptor for the HID device.
		//
		status = RequestCopyFromBuffer(Request,
			deviceContext->ReportDescriptor,
			deviceContext->HidDescriptor.DescriptorList[0].wReportLength);
		break;

	case IOCTL_HID_READ_REPORT:             // METHOD_NEITHER
		//
		// Returns a report from the device into a class driver-supplied
		// buffer.
		//
		status = ReadReport(queueContext, Request, &completeRequest);
		break;

	case IOCTL_HID_WRITE_REPORT:            // METHOD_NEITHER
		//
		// Transmits a class driver-supplied report to the device.
		//
		status = WriteReport(queueContext, Request);
		break;

	case IOCTL_HID_GET_FEATURE:             // METHOD_OUT_DIRECT

		status = GetFeature(queueContext, Request);
		break;

	case IOCTL_HID_SET_FEATURE:             // METHOD_IN_DIRECT

		status = SetFeature(queueContext, Request);
		break;

	case IOCTL_HID_GET_INPUT_REPORT:        // METHOD_OUT_DIRECT

		status = GetInputReport(queueContext, Request);
		break;

	case IOCTL_HID_SET_OUTPUT_REPORT:       // METHOD_IN_DIRECT

		status = SetOutputReport(queueContext, Request);
		break;


	case IOCTL_HID_GET_STRING:                      // METHOD_NEITHER

		status = GetString(Request);
		break;

	case IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT

		status = GetIndexedString(Request);
		break;

	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:  // METHOD_NEITHER
		//
		// This has the USBSS Idle notification callback. If the lower driver
		// can handle it (e.g. USB stack can handle it) then pass it down
		// otherwise complete it here as not inplemented. For a virtual
		// device, idling is not needed.
		//
		// Not implemented. fall through...
		//
	case IOCTL_HID_ACTIVATE_DEVICE:                 // METHOD_NEITHER
	case IOCTL_HID_DEACTIVATE_DEVICE:               // METHOD_NEITHER
	case IOCTL_GET_PHYSICAL_DESCRIPTOR:             // METHOD_OUT_DIRECT
		//
		// We don't do anything for these IOCTLs but some minidrivers might.
		//
		// Not implemented. fall through...
		//
	default:
		status = STATUS_NOT_IMPLEMENTED;
		break;
	}

	//
	// Complete the request. Information value has already been set by request
	// handlers.
	//
	if (completeRequest) {
		WdfRequestComplete(Request, status);
	}
}

NTSTATUS
RequestCopyFromBuffer(
	_In_  WDFREQUEST        Request,
	_In_  PVOID             SourceBuffer,
	_When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
	_In_  size_t            NumBytesToCopyFrom
)
/*++

Routine Description:

	A helper function to copy specified bytes to the request's output memory

Arguments:

	Request - A handle to a framework request object.

	SourceBuffer - The buffer to copy data from.

	NumBytesToCopyFrom - The length, in bytes, of data to be copied.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDFMEMORY               memory;
	size_t                  outputBufferLength;

	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	WdfMemoryGetBuffer(memory, &outputBufferLength);
	if (outputBufferLength < NumBytesToCopyFrom) {
		status = STATUS_INVALID_BUFFER_SIZE;
		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		SourceBuffer,
		NumBytesToCopyFrom);
	if (!NT_SUCCESS(status)) {

		return status;
	}
	WdfRequestSetInformation(Request, NumBytesToCopyFrom);
	return status;
}

NTSTATUS
ReadReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request,
	_Always_(_Out_)
	BOOLEAN* CompleteRequest
)
/*++

Routine Description:

	Handles IOCTL_HID_READ_REPORT for the HID collection. Normally the request
	will be forwarded to a manual queue for further process. In that case, the
	caller should not try to complete the request at this time, as the request
	will later be retrieved back from the manually queue and completed there.
	However, if for some reason the forwarding fails, the caller still need
	to complete the request with proper error code immediately.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to  Request Packet.

	CompleteRequest - A boolean output value, indicating whether the caller
			should complete the request or not

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;

	//
	// forward the request to manual queue
	//
	status = WdfRequestForwardToIoQueue(
		Request,
		QueueContext->DeviceContext->ManualQueue);
	if (!NT_SUCCESS(status)) {
		*CompleteRequest = TRUE;
	}
	else {
		*CompleteRequest = FALSE;
	}

	return status;
}

NTSTATUS
WriteReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_WRITE_REPORT all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to  Request Packet.

Return Value:

	NT status code.

--*/

{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_OUTPUT_REPORT  outputReport;

	status = RequestGetHidXferPacket_ToWriteToDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// Return error for unknown collection
		//
		status = STATUS_INVALID_PARAMETER;
		return status;
	}

	//
	// before touching buffer make sure buffer is big enough.
	//
	reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;

		return status;
	}

	outputReport = (PHIDMINI_OUTPUT_REPORT)packet.reportBuffer;

	//
	// Store the device data in device extension.
	//
	QueueContext->DeviceContext->DeviceData = outputReport->Data;

	//
	// set status and information
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}


HRESULT
GetFeature(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_FEATURE for all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to  Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;

	UNREFERENCED_PARAMETER(QueueContext);
	status = RequestGetHidXferPacket_ToReadFromDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;


		return status;
	}

	//
	// Since output buffer is for write only (no read allowed by UMDF in output
	// buffer), any read from output buffer would be reading garbage), so don't
	// let app embed custom control code in output buffer. The minidriver can
	// support multiple features using separate report ID instead of using
	// custom control code. Since this is targeted at report ID 1, we know it
	// is a request for getting attributes.
	//
	// While KMDF does not enforce the rule (disallow read from output buffer),
	// it is good practice to not do so.
	//

	reportSize = sizeof(features);
	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;


		return status;
	}

	//
	// Since this device has one report ID, hidclass would pass on the report
	// ID in the buffer (it wouldn't if report descriptor did not have any report
	// ID). However, since UMDF allows only writes to an output buffer, we can't
	// "read" the report ID from "output" buffer. There is no need to read the
	// report ID since we get it other way as shown above, however this is
	// something to keep in mind.
	//
	packet.reportBuffer[0] = features.reportId;
	packet.reportBuffer[1] = features.DIG_TouchScreenContactCountMaximum;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}

NTSTATUS
SetFeature(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_SET_FEATURE for all the collection.
	For control collection (custom defined collection) it handles
	the user-defined control codes for sideband communication

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_CONTROL_INFO   controlInfo;
	PHID_DEVICE_ATTRIBUTES  hidAttributes = &QueueContext->DeviceContext->HidDeviceAttributes;

	status = RequestGetHidXferPacket_ToWriteToDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;


		return status;
	}

	//
	// before touching control code make sure buffer is big enough.
	//
	reportSize = sizeof(HIDMINI_CONTROL_INFO);

	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;


		return status;
	}

	controlInfo = (PHIDMINI_CONTROL_INFO)packet.reportBuffer;

	switch (controlInfo->ControlCode)
	{
	case HIDMINI_CONTROL_CODE_SET_ATTRIBUTES:
		//
		// Store the device attributes in device extension
		//
		hidAttributes->ProductID = controlInfo->u.Attributes.ProductID;
		hidAttributes->VendorID = controlInfo->u.Attributes.VendorID;
		hidAttributes->VersionNumber = controlInfo->u.Attributes.VersionNumber;

		//
		// set status and information
		//
		WdfRequestSetInformation(Request, reportSize);
		break;

	case HIDMINI_CONTROL_CODE_DUMMY1:
		status = STATUS_NOT_IMPLEMENTED;

		break;

	case HIDMINI_CONTROL_CODE_DUMMY2:
		status = STATUS_NOT_IMPLEMENTED;

		break;

	default:
		status = STATUS_NOT_IMPLEMENTED;
		break;
	}
	return status;
}

NTSTATUS
GetInputReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_INPUT_REPORT for all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_INPUT_REPORT   reportBuffer;

	status = RequestGetHidXferPacket_ToReadFromDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	reportSize = sizeof(HIDMINI_INPUT_REPORT);
	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;

		return status;
	}

	reportBuffer = (PHIDMINI_INPUT_REPORT)(packet.reportBuffer);

	reportBuffer->ReportId = CONTROL_COLLECTION_REPORT_ID;
	reportBuffer->Data = QueueContext->OutputReport;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}


NTSTATUS
SetOutputReport(
	_In_  PQUEUE_CONTEXT    QueueContext,
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_SET_OUTPUT_REPORT for all the collection.

Arguments:

	QueueContext - The object context associated with the queue

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	HID_XFER_PACKET         packet;
	ULONG                   reportSize;
	PHIDMINI_OUTPUT_REPORT  reportBuffer;

	status = RequestGetHidXferPacket_ToWriteToDevice(
		Request,
		&packet);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
		//
		// If collection ID is not for control collection then handle
		// this request just as you would for a regular collection.
		//
		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	//
	// before touching buffer make sure buffer is big enough.
	//
	reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

	if (packet.reportBufferLen < reportSize) {
		status = STATUS_INVALID_BUFFER_SIZE;
		return status;
	}

	reportBuffer = (PHIDMINI_OUTPUT_REPORT)packet.reportBuffer;

	QueueContext->OutputReport = reportBuffer->Data;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, reportSize);
	return status;
}


NTSTATUS
GetStringId(
	_In_  WDFREQUEST        Request,
	_Out_ ULONG* StringId,
	_Out_ ULONG* LanguageId
)
/*++

Routine Description:

	Helper routine to decode IOCTL_HID_GET_INDEXED_STRING and IOCTL_HID_GET_STRING.

Arguments:

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	ULONG                   inputValue;

	WDF_REQUEST_PARAMETERS  requestParameters;

	//
	// IOCTL_HID_GET_STRING:                      // METHOD_NEITHER
	// IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT
	//
	// The string id (or string index) is passed in Parameters.DeviceIoControl.
	// Type3InputBuffer. However, Parameters.DeviceIoControl.InputBufferLength
	// was not initialized by hidclass.sys, therefore trying to access the
	// buffer with WdfRequestRetrieveInputMemory will fail
	//
	// Another problem with IOCTL_HID_GET_INDEXED_STRING is that METHOD_OUT_DIRECT
	// expects the input buffer to be Irp->AssociatedIrp.SystemBuffer instead of
	// Type3InputBuffer. That will also fail WdfRequestRetrieveInputMemory.
	//
	// The solution to the above two problems is to get Type3InputBuffer directly
	//
	// Also note that instead of the buffer's content, it is the buffer address
	// that was used to store the string id (or index)
	//

	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(Request, &requestParameters);

	inputValue = PtrToUlong(
		requestParameters.Parameters.DeviceIoControl.Type3InputBuffer);

	status = STATUS_SUCCESS;

	//
	// The least significant two bytes of the INT value contain the string id.
	//
	*StringId = (inputValue & 0x0ffff);

	//
	// The most significant two bytes of the INT value contain the language
	// ID (for example, a value of 1033 indicates English).
	//
	*LanguageId = (inputValue >> 16);
	return status;
}


NTSTATUS
GetIndexedString(
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_INDEXED_STRING

Arguments:

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	ULONG                   languageId, stringIndex;

	status = GetStringId(Request, &stringIndex, &languageId);

	// While we don't use the language id, some minidrivers might.
	//
	UNREFERENCED_PARAMETER(languageId);

	if (NT_SUCCESS(status)) {

		if (stringIndex != VHIDMINI_DEVICE_STRING_INDEX)
		{
			status = STATUS_INVALID_PARAMETER;

			return status;
		}

		status = RequestCopyFromBuffer(Request, VHIDMINI_DEVICE_STRING, sizeof(VHIDMINI_DEVICE_STRING));
	}
	return status;
}


NTSTATUS
GetString(
	_In_  WDFREQUEST        Request
)
/*++

Routine Description:

	Handles IOCTL_HID_GET_STRING.

Arguments:

	Request - Pointer to Request Packet.

Return Value:

	NT status code.

--*/
{
	NTSTATUS                status;
	ULONG                   languageId, stringId;
	size_t                  stringSizeCb;
	PWSTR                   string;

	status = GetStringId(Request, &stringId, &languageId);

	// While we don't use the language id, some minidrivers might.
	//
	UNREFERENCED_PARAMETER(languageId);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	switch (stringId) {
	case HID_STRING_ID_IMANUFACTURER:
		stringSizeCb = sizeof(VHIDMINI_MANUFACTURER_STRING);
		string = VHIDMINI_MANUFACTURER_STRING;
		break;
	case HID_STRING_ID_IPRODUCT:
		stringSizeCb = sizeof(VHIDMINI_PRODUCT_STRING);
		string = VHIDMINI_PRODUCT_STRING;
		break;
	case HID_STRING_ID_ISERIALNUMBER:
		stringSizeCb = sizeof(VHIDMINI_SERIAL_NUMBER_STRING);
		string = VHIDMINI_SERIAL_NUMBER_STRING;
		break;
	default:
		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = RequestCopyFromBuffer(Request, string, stringSizeCb);
	return status;
}


NTSTATUS
ManualQueueCreate(
	_In_  WDFDEVICE         Device,
	_Out_ WDFQUEUE* Queue
)
/*++
Routine Description:

	This function creates a manual I/O queue to receive IOCTL_HID_READ_REPORT
	forwarded from the device's default queue handler.

	It also creates a periodic timer to check the queue and complete any pending
	request with data from the device. Here timer expiring is used to simulate
	a hardware event that new data is ready.

	The workflow is like this:

	- Hidclass.sys sends an ioctl to the miniport to read input report.

	- The request reaches the driver's default queue. As data may not be avaiable
	  yet, the request is forwarded to a second manual queue temporarily.

	- Later when data is ready (as simulated by timer expiring), the driver
	  checks for any pending request in the manual queue, and then completes it.

	- Hidclass gets notified for the read request completion and return data to
	  the caller.

	On the other hand, for IOCTL_HID_WRITE_REPORT request, the driver simply
	sends the request to the hardware (as simulated by storing the data at
	DeviceContext->DeviceData) and completes the request immediately. There is
	no need to use another queue for write operation.

Arguments:

	Device - Handle to a framework device object.

	Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

	NTSTATUS

--*/
{
	NTSTATUS                status;
	WDF_IO_QUEUE_CONFIG     queueConfig;
	WDF_OBJECT_ATTRIBUTES   queueAttributes;
	WDFQUEUE                queue;
	PMANUAL_QUEUE_CONTEXT   queueContext;

	WDF_IO_QUEUE_CONFIG_INIT(
		&queueConfig,
		WdfIoQueueDispatchManual);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
		&queueAttributes,
		MANUAL_QUEUE_CONTEXT);

	status = WdfIoQueueCreate(
		Device,
		&queueConfig,
		&queueAttributes,
		&queue);

	if (!NT_SUCCESS(status)) {


		return status;
	}

	queueContext = GetManualQueueContext(queue);
	queueContext->Queue = queue;
	queueContext->DeviceContext = GetDeviceContext(Device);

	*Queue = queue;
	return status;
}

void
EvtTimerFunc(
	_In_  WDFTIMER          Timer
)
/*++
Routine Description:

	This periodic timer callback routine checks the device's manual queue and
	completes any pending request with data from the device.

Arguments:

	Timer - Handle to a timer object that was obtained from WdfTimerCreate.

Return Value:

	VOID

--*/
{
	NTSTATUS                status;
	WDFQUEUE                queue;
	PMANUAL_QUEUE_CONTEXT   queueContext;
	WDFREQUEST              request;
	HIDMINI_INPUT_REPORT    readReport;

	queue = (WDFQUEUE)WdfTimerGetParentObject(Timer);
	queueContext = GetManualQueueContext(queue);

	//
	// see if we have a request in manual queue
	//
	status = WdfIoQueueRetrieveNextRequest(
		queueContext->Queue,
		&request);

	if (NT_SUCCESS(status)) {

		readReport.ReportId = CONTROL_FEATURE_REPORT_ID;
		readReport.Data = queueContext->DeviceContext->DeviceData;

		status = RequestCopyFromBuffer(request,
			&readReport,
			sizeof(readReport));

		WdfRequestComplete(request, status);
	}
}
BOOLEAN
OnInterruptIsr(
	_In_  WDFINTERRUPT FxInterrupt,
	_In_  ULONG        MessageID
)
/*++

  Routine Description:

	This routine responds to interrupts generated by the H/W.
	It then waits indefinitely for the user to signal that
	the interrupt has been acknowledged, allowing the ISR to
	return. This ISR is called at PASSIVE_LEVEL.

  Arguments:

	Interrupt - a handle to a framework interrupt object
	MessageID - message number identifying the device's
		hardware interrupt message (if using MSI)

  Return Value:

	TRUE if interrupt recognized.

--*/
{
	//BOOLEAN               fNotificationSent;
	WDFDEVICE               device;
	PDEVICE_CONTEXT         pDevice;
	int                     remain, x, y, reported;
	NTSTATUS                status;
	WDFREQUEST              request;
	inputReport54_t         readReport;
	BYTE                    touchType, touchId;
	UINT8					event_id, * event;
	UNREFERENCED_PARAMETER(MessageID);

	device = WdfInterruptGetDevice(FxInterrupt);
	pDevice = GetDeviceContext(device);

	// get some
	status = SpbDeviceWriteRead(pDevice, S6SY761_READ_ONE_EVENT, eventbuf, 1, S6SY761_EVENT_SIZE);
	if (!NT_SUCCESS(status))
	{
		// TODO dev_err(&sdata->client->dev, "failed to read events\n");
		return TRUE;
	}
	if (!eventbuf[0])
	{
		return TRUE;
	}
	//interrupt single event print
	//DbgPrint("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n", eventbuf[0], eventbuf[1], eventbuf[2], eventbuf[3], eventbuf[4], eventbuf[5], eventbuf[6], eventbuf[7]);

	// get some more
	remain = eventbuf[7] & S6SY761_MASK_LEFT_EVENTS;
	if (remain > S6SY761_EVENT_COUNT - 1)
	{
		return TRUE;
	}
	if (remain)
	{
        //DbgPrint("events remaining: %d\n", remain);
        status = SpbDeviceWriteRead(pDevice, S6SY761_READ_ALL_EVENTS, eventbuf + S6SY761_EVENT_SIZE, 1, remain * S6SY761_EVENT_SIZE);
		if (!NT_SUCCESS(status))
		{
			// TODO dev_err(&sdata->client->dev, "failed to read events\n");
			return TRUE;
		}
	}

	// handle the events now
	reported = 0;
	for (int i = 0; i < remain; ++i)
	{
		event = &eventbuf[i * S6SY761_EVENT_SIZE];
		event_id = event[0] & S6SY761_MASK_EID;
		if (!event[0])
		{
			break;
		}

		switch (event_id)
		{
		case S6SY761_EVENT_ID_COORDINATE:
			if (!(event[0] & S6SY761_MASK_TID))
			{
				break;
			}
            touchId = ((event[0] & S6SY761_MASK_TID) >> 2) - 1;
            touchType = (event[0] & S6SY761_MASK_TOUCH_STATE) >> 6;
            x = (event[1] << 4) | ((event[3] & 0xf0) >> 4);
            y = (event[2] << 4) | (event[3] & 0x0f);

            switch (touchType)
            {
            case S6SY761_TS_NONE:
                break;
            case S6SY761_TS_RELEASE:
                readReport.points[reported * 6 + 0] = 0x06;
				++reported;
                //DbgPrint("release tid: %d\n", touchId);
                break;
            case S6SY761_TS_MOVE:
            case S6SY761_TS_PRESS:
                readReport.points[reported * 6 + 0] = 0x07;
                readReport.points[reported * 6 + 1] = touchId;
                readReport.points[reported * 6 + 2] = x & 0xFF;
                readReport.points[reported * 6 + 3] = (x >> 8) & 0x0F;
                readReport.points[reported * 6 + 4] = y & 0xFF;
                readReport.points[reported * 6 + 5] = (y >> 8) & 0x0F;
				++reported;
                //DbgPrint("move/press tid: %d\tx: %d\ty: %d remain:%d\n", touchId, x, y, remain);
                break;
            }

			break;
		case S6SY761_EVENT_ID_STATUS:
			break;
		default:
			break;
		}
	}

	// XXX contact count 0-8, chip reports 0-32
    readReport.DIG_TouchScreenContactCount = (BYTE)reported + 1;
	status = WdfIoQueueRetrieveNextRequest(
		pDevice->ManualQueue,
		&request);

	if (!NT_SUCCESS(status))
	{
		// TODO dev_err failed to retrieve next request
		return TRUE;
	}

    readReport.reportId = CONTROL_FEATURE_REPORT_ID;

    status = RequestCopyFromBuffer(request,
                                   &readReport,
                                   sizeof(readReport));

    WdfRequestComplete(request, status);

	return TRUE;
}
VOID
SpbDeviceOpen(
	_In_  PDEVICE_CONTEXT  pDevice
)
{
	WDF_IO_TARGET_OPEN_PARAMS  openParams;
	NTSTATUS status;
	DECLARE_UNICODE_STRING_SIZE(DevicePath, RESOURCE_HUB_PATH_SIZE);
	RESOURCE_HUB_CREATE_PATH_FROM_ID(
		&DevicePath,
		pDevice->PeripheralId.LowPart,
		pDevice->PeripheralId.HighPart);

	//
	// Open a handle to the SPB controller.
	//

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
		&openParams,
		&DevicePath,
		(GENERIC_READ | GENERIC_WRITE));

	openParams.ShareAccess = 0;
	openParams.CreateDisposition = FILE_OPEN;
	openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;

	status = WdfIoTargetOpen(
		pDevice->SpbController,
		&openParams);

	if (!NT_SUCCESS(status))
	{
	}

	DbgPrint("sense enable\n");
	SpbDeviceWrite(pDevice, S6SY761_SENSE_ON, 1);

	DbgPrint("reading events\n");
	SpbDeviceWrite(pDevice, S6SY761_READ_ALL_EVENTS, 1);

	DbgPrint("draining last events\n");
	SpbDeviceWriteRead(pDevice, S6SY761_CLEAR_EVENT_STACK, eventbuf, 1, 256);

	DbgPrint("enabling Interrupt\n");
	WdfInterruptEnable(pDevice->Interrupt);

}
VOID
SpbDeviceClose(
	_In_  PDEVICE_CONTEXT  pDevice
)
{
	WdfInterruptDisable(pDevice->Interrupt);

	WdfIoTargetClose(pDevice->SpbController);
}
VOID
SpbDeviceWrite(
	_In_ PDEVICE_CONTEXT pDevice,
	_In_ PVOID pInputBuffer,
	_In_ size_t inputBufferLength
)
{
	WDF_MEMORY_DESCRIPTOR  inMemoryDescriptor;
	ULONG_PTR  bytesWritten = (ULONG_PTR)NULL;
	NTSTATUS status;


	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inMemoryDescriptor,
		pInputBuffer,
		(ULONG)inputBufferLength);

	status = WdfIoTargetSendWriteSynchronously(
		pDevice->SpbController,
		NULL,
		&inMemoryDescriptor,
		NULL,
		NULL,
		&bytesWritten
	);

	if (!NT_SUCCESS(status))
	{
	}
}

HRESULT
SpbDeviceWriteRead(
	_In_ PDEVICE_CONTEXT pDevice,
	_In_ PVOID pInputBuffer,
	_In_ PVOID pOutputBuffer,
	_In_ size_t inputBufferLength,
	_In_ size_t outputBufferLength
)
{
	WDF_MEMORY_DESCRIPTOR  inMemoryDescriptor;
	WDF_MEMORY_DESCRIPTOR  outMemoryDescriptor;
	ULONG_PTR  bytesWritten = (ULONG_PTR)NULL;
	ULONG_PTR  bytesRead = (ULONG_PTR)NULL;
	NTSTATUS status;


	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inMemoryDescriptor,
		pInputBuffer,
		(ULONG)inputBufferLength);

	status = WdfIoTargetSendWriteSynchronously(
		pDevice->SpbController,
		NULL,
		&inMemoryDescriptor,
		NULL,
		NULL,
		&bytesWritten
	);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outMemoryDescriptor,
		pOutputBuffer,
		(ULONG)outputBufferLength);

	status = WdfIoTargetSendReadSynchronously(
		pDevice->SpbController,
		NULL,
		&outMemoryDescriptor,
		NULL,
		NULL,
		&bytesRead
	);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS
ReadDescriptorFromRegistry(
	WDFDEVICE Device
)
/*++
Routine Description:
	Read HID report descriptor from registry
Arguments:
	device - pointer to a device object.
Return Value:
	NT status code.
--*/
{
	WDFKEY          hKey = NULL;
	NTSTATUS        status;
	UNICODE_STRING  xRevertName;
	UNICODE_STRING  yRevertName;
	UNICODE_STRING  xYExchangeName;
	UNICODE_STRING  xMinName;
	UNICODE_STRING  xMaxName;
	UNICODE_STRING  yMinName;
	UNICODE_STRING  yMaxName;
	PDEVICE_CONTEXT deviceContext;
	WDF_OBJECT_ATTRIBUTES   attributes;

	deviceContext = GetDeviceContext(Device);

	status = WdfDeviceOpenRegistryKey(Device,
		PLUGPLAY_REGKEY_DEVICE,
		KEY_READ,
		WDF_NO_OBJECT_ATTRIBUTES,
		&hKey);

	if (NT_SUCCESS(status)) {

		RtlInitUnicodeString(&xRevertName, L"XRevert");
		RtlInitUnicodeString(&yRevertName, L"YRevert");
		RtlInitUnicodeString(&xYExchangeName, L"XYExchange");
		RtlInitUnicodeString(&xMinName, L"XMin");
		RtlInitUnicodeString(&xMaxName, L"XMax");
		RtlInitUnicodeString(&yMinName, L"YMin");
		RtlInitUnicodeString(&yMaxName, L"YMax");

		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = Device;

		status = WdfRegistryQueryULong(hKey, &xRevertName, &XRevert);
		status = WdfRegistryQueryULong(hKey, &yRevertName, &YRevert);
		status = WdfRegistryQueryULong(hKey, &xYExchangeName, &XYExchange);
		status = WdfRegistryQueryULong(hKey, &xMinName, &XMin);
		status = WdfRegistryQueryULong(hKey, &xMaxName, &XMax);
		status = WdfRegistryQueryULong(hKey, &yMinName, &YMin);
		status = WdfRegistryQueryULong(hKey, &yMaxName, &YMax);

		WdfRegistryClose(hKey);
	}

	return status;
}

