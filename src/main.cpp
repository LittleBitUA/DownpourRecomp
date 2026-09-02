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
// 2026-09-03 (issue #29): the exports are NOT harmless without the folder. The
// loader only ignores them when the OS inbox runtime is already >= the exported
// version (Windows 11 24H2+, where 1.1.7 was built and tested). On any older
// Windows the loader insists on D3D12SDKPath\D3D12Core.dll, and when it is
// missing every D3D12CreateDevice fails -> "Failed to get an adapter supporting
// Direct3D 12 with the feature level of at least 11_0" at boot. v1.1.6 shipped
// no exports and the same machines booted fine. The native-render work that
// needed the validation layer is retired, so the exports are opt-in for local
// debugging only: build with -DDPOUR_AGILITY_SDK=1 and put D3D12\D3D12Core.dll
// (+ d3d12SDKLayers.dll) next to the executable. Release builds use the inbox
// runtime, exactly like 1.1.6.
#if defined(DPOUR_AGILITY_SDK) && DPOUR_AGILITY_SDK
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = 614;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif
// === END DPOUR MIGRATION 2026-07-28 ==========================================

REX_DEFINE_APP(downpour, DownpourApp::Create)
