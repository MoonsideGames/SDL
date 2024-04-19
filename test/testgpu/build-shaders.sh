# Rebuilds the shaders needed for the GPU cube test.
# Requires glslangValidator and spirv-cross, which can be obtained from the LunarG Vulkan SDK.

# On Windows, run this via Git Bash.

export MSYS_NO_PATHCONV=1

# SPIR-V
glslangValidator cube.vert -V -o cube.vert.spv --quiet
glslangValidator cube.frag -V -o cube.frag.spv --quiet
xxd -i cube.vert.spv | perl -w -p -e 's/\Aunsigned /const unsigned /;' > cube_vert.h
xxd -i cube.frag.spv | perl -w -p -e 's/\Aunsigned /const unsigned /;' > cube_frag.h
cat cube_vert.h cube_frag.h > testgpu_spirv.h

# Platform-specific compilation
if [ "$OSTYPE" == "darwin"* ]; then

    # MSL
    spirv-cross cube.vert.spv --msl --output cube.vert.metal
    spirv-cross cube.frag.spv --msl --output cube.frag.metal
    # FIXME

elif [[ "$OSTYPE" == "cygwin"* ]] || [[ "$OSTYPE" == "msys"* ]]; then

    # HLSL
    spirv-cross cube.vert.spv --hlsl --shader-model 50 --output cube.vert.hlsl
    spirv-cross cube.frag.spv --hlsl --shader-model 50 --output cube.frag.hlsl

    # FXC
    # Assumes fxc is in the path.
    # If not, you can run `export PATH=$PATH:/c/Program\ Files\ \(x86\)/Windows\ Kits/10/bin/x.x.x.x/x64/`
    fxc cube.vert.hlsl /T vs_5_0 /Fh cube.vert.h
    fxc cube.frag.hlsl /T ps_5_0 /Fh cube.frag.h

    cat cube.vert.h | perl -w -p -e 's/BYTE/unsigned char/;s/main/vert_main/;' > cube_vert.h
    cat cube.frag.h | perl -w -p -e 's/BYTE/unsigned char/;s/main/frag_main/;' > cube_frag.h
    cat cube_vert.h cube_frag.h > testgpu_dxbc.h

fi

# cleanup
rm -f cube.vert.spv cube.frag.spv
rm -f cube.vert.h cube.frag.h
rm -f cube_vert.h cube_frag.h
rm -f cube.vert.hlsl cube.frag.hlsl