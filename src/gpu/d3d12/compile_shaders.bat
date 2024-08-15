fxc /T vs_5_1 /E FullscreenVert /Fh D3D12_FullscreenVert.h ..\d3d11\D3D_Blit.hlsl /D D3D12=1
fxc /T ps_5_1 /E Blit /Fh D3D12_BlitFrom2D.h ..\d3d11\D3D_Blit.hlsl /D D3D12=1
fxc /T ps_5_1 /E Blit /Fh D3D12_BlitFrom2DArray.h ..\d3d11\D3D_Blit.hlsl /D ARRAY=1 /D D3D12=1
fxc /T ps_5_1 /E Blit /Fh D3D12_BlitFromCube.h ..\d3d11\D3D_Blit.hlsl /D CUBE=1 /D D3D12=1