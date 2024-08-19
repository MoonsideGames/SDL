dxc /T vs_6_0 /E FullscreenVert /Fh D3D12_FullscreenVert.h ..\d3dcommon\D3D_Blit.hlsl /D D3D12=1
dxc /T ps_6_0 /E BlitFrom2D /Fh D3D12_BlitFrom2D.h ..\d3dcommon\D3D_Blit.hlsl /D D3D12=1
dxc /T ps_6_0 /E BlitFrom2DArray /Fh D3D12_BlitFrom2DArray.h ..\d3dcommon\D3D_Blit.hlsl /D D3D12=1
dxc /T ps_6_0 /E BlitFrom3D /Fh D3D12_BlitFrom3D.h ..\d3dcommon\D3D_Blit.hlsl /D D3D12=1
dxc /T ps_6_0 /E BlitFromCube /Fh D3D12_BlitFromCube.h ..\d3dcommon\D3D_Blit.hlsl /D D3D12=1
copy /b D3D12_FullscreenVert.h+D3D12_BlitFrom2D.h+D3D12_BlitFrom2DArray.h+D3D12_BlitFrom3D.h+D3D12_BlitFromCube.h D3D12_Blit.h
del D3D12_FullscreenVert.h D3D12_BlitFrom2D.h D3D12_BlitFrom2DArray.h D3D12_BlitFrom3D.h D3D12_BlitFromCube.h