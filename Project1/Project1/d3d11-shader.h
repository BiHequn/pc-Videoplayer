#pragma once
#include<d3d11.h>

bool initializeD3D11Shaders(ID3D11Device* device);
bool bindD3D11Shaders(ID3D11DeviceContext* content);
void shutdownD3D11Shaders();