#pragma once
#include <guiddef.h>

inline const GUID CLSID_VCamMediaSource =
    // {B2B674D4-9CF0-461C-BDCE-3D56FBB41356}
{ 0xb2b674d4, 0x9cf0, 0x461c, { 0xbd, 0xce, 0x3d, 0x56, 0xfb, 0xb4, 0x13, 0x56 } };

// PINNAME_VIDEO_CAPTURE {FB6C4281-0353-11d1-905F-0000C0CC16BA}
inline const GUID VCamPinNameVideoCapture =
{ 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };

// Fixed stream ID for the single video stream (MF_DEVICESTREAM_STREAM_ID)
inline const GUID VCAM_VIDEO_STREAM_ID =
// {BF9C8DEF-84C0-4549-BE7C-98B998DD022B}
{ 0xbf9c8def, 0x84c0, 0x4549, { 0xbe, 0x7c, 0x98, 0xb9, 0x98, 0xdd, 0x02, 0x2b } };

// MFT_TRANSFORM_CLSID_Attribute (mftransform.h): the frameserver reads this
// from the source/activator attributes to identify the class behind the device.
inline const GUID kMftTransformClsidAttribute =
// {6821C42B-65A4-4E82-99BC-9A88205ECD C}
{ 0x6821c42b, 0x65a4, 0x4e82, { 0x99, 0xbc, 0x9a, 0x88, 0x20, 0x5e, 0xcd, 0xc } };

// MF_DEVSOURCE_ATTRIBUTE_ENABLE_MS_CAMERA_EFFECTS (NTDDI_WIN10_CO, undocumented)
inline const GUID kMsCameraEffectsAttribute =
// {28A5531A-57DD-4FD5-AAA7-385ABF57D785}
{ 0x28a5531a, 0x57dd, 0x4fd5, { 0xaa, 0xa7, 0x38, 0x5a, 0xbf, 0x57, 0xd7, 0x85 } };

// KSCAMERAPROFILE_Legacy (ksmedia.h): camera profile type GUID
inline const GUID kKsCameraProfileLegacy =
// {B4894D81-62B7-4EEC-8740-80658C4A9D3E}
{ 0xb4894d81, 0x62b7, 0x4eec, { 0x87, 0x40, 0x80, 0x65, 0x8c, 0x4a, 0x9d, 0x3e } };

// KSCAMERAPROFILE_HighFrameRate (ksmedia.h): camera profile type GUID
inline const GUID kKsCameraProfileHighFrameRate =
// {566E6113-8C35-48E7-B89F-D23FDC1219DC}
{ 0x566e6113, 0x8c35, 0x48e7, { 0xb8, 0x9f, 0xd2, 0x3f, 0xdc, 0x12, 0x19, 0xdc } };

// Canonical IInspectable IID (local SDK value differs)
inline const GUID kIID_IInspectable_Canonical =
// {AF86E2E0-589B-42B4-823F-0904203D4E8C}
{ 0xaf86e2e0, 0x589b, 0x42b4, { 0x82, 0x3f, 0x09, 0x04, 0x20, 0x3d, 0x4e, 0x8c } };

// Canonical IMFAttributes IID (local SDK value differs)
inline const GUID kIID_IMFAttributes_Canonical =
// {94EA2B94-A7C1-400B-8A6E-80F10AABFFF8}
{ 0x94ea2b94, 0xa7c1, 0x400b, { 0x8a, 0x6e, 0x80, 0xf1, 0x0a, 0xab, 0xff, 0xf8 } };
