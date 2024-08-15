fxc /T vs_5_0 /E FullscreenVert /Fh D3D11_FullscreenVert.h D3D11_Blit.hlsl
fxc /T ps_5_0 /E Blit /Fh D3D11_BlitFrom2D.h D3D11_Blit.hlsl
fxc /T ps_5_0 /E Blit /Fh D3D11_BlitFrom2DArray.h D3D11_Blit.hlsl /D ARRAY=1
fxc /T ps_5_0 /E Blit /Fh D3D11_BlitFromCube.h D3D11_Blit.hlsl /D CUBE=1
