// downpour - ReXGlue Recompiled Project

#include "generated/default/downpour_init.h"

#include "downpour_app.h"

// === DPOUR MIGRATION 2026-07-28: DirectX Agility SDK ==========================
//
// Two exports are the whole contract: D3D12.dll looks for them in the EXE and,
// when it finds them, loads the runtime from D3D12SDKPath instead of the one in
// System32. The reference release ships exactly this - UnleashedRecomp-Windows
// carries D3D12/D3D12Core.dll next to its executable.
//
// Why it matters beyond version currency: the same folder can hold
// d3d12SDKLayers.dll, which is the DEBUG LAYER. Without it, validation depends
// on the Graphics Tools optional feature, which on this machine cannot be
// installed at all - Windows Update is disabled by policy and pointed at a WSUS
// server that does not exist, and Features on Demand come from there. Three
// separate faults this session were silent format and state mismatches, exactly
// what validation names on the spot; carrying our own layer means never being
// blind to that class again.
//
// Absent the D3D12 folder these exports are harmless: the loader falls back to
// the system runtime.
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = 614;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
// === END DPOUR MIGRATION 2026-07-28 ==========================================

REX_DEFINE_APP(downpour, DownpourApp::Create)
