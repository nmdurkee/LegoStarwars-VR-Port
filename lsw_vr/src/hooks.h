#pragma once

struct IDirect3D9;

// Hooks IDirect3D9::CreateDevice so the game's device methods can be hooked
// once the device exists.
void InstallD3D9Hooks(IDirect3D9 *d3d);
