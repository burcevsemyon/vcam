#pragma once

// Live COM object accounting for DllCanUnloadNow (defined in dllmain.cpp):
// increment in every COM object constructor, decrement in its destructor.
void VCamObjectInc();
void VCamObjectDec();
