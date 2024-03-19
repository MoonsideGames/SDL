#!/usr/bin/env python3

#  Simple DirectMedia Layer
#  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>
#
#  This software is provided 'as-is', without any express or implied
#  warranty.  In no event will the authors be held liable for any damages
#  arising from the use of this software.
#
#  Permission is granted to anyone to use this software for any purpose,
#  including commercial applications, and to alter it and redistribute it
#  freely, subject to the following restrictions:
#
#  1. The origin of this software must not be misrepresented; you must not
#     claim that you wrote the original software. If you use this software
#     in a product, an acknowledgment in the product documentation would be
#     appreciated but is not required.
#  2. Altered source versions must be plainly marked as such, and must not be
#     misrepresented as being the original software.
#  3. This notice may not be removed or altered from any source distribution.

# WHAT IS THIS?
#  SDL_gpu takes in a custom binary format, which is simply various
#  shader input formats globbed together. This script takes high-level
#  shader code and outputs a binary that can be consumed by SDL_gpu.

# Requires Python 3.10 or above
# Requires spirv-cross and glslc, which can be installed from the LunarG Vulkan SDK

import sys, os, glob, subprocess, shutil

def display_help_text():
    print("Usage: refreshc <path-to-glsl-source | directory-with-glsl-source-files>")
    print("Options:")
    print("  --vulkan           Emit shader compatible with the Refresh Vulkan backend")
    print("  --d3d11            Emit shader compatible with the Refresh D3D11 backend")
    print("  --out dir          Write output file(s) to the directory `dir`")
    print("  --preserve-temp    Do not delete the temp directory after compilation. Useful for debugging.")

def cleanup(compile_data):
    if "preserve_temp" not in compile_data:
        shutil.rmtree(os.path.join(os.getcwd(), "temp"))

def write_shader_blob(input_filepath, output_file, backend_type):
    file_bytes = []
    with open(input_filepath, "rb") as f:
        file_bytes = f.read()
    output_file.write(backend_type.to_bytes(1, byteorder='little'))
    output_file.write(len(file_bytes).to_bytes(4, byteorder='little'))
    output_file.write(file_bytes)

def compile_file(filename, compile_data):
    shader_filename = os.path.splitext(os.path.basename(filename))
    shader_name = shader_filename[0]
    shader_type = shader_filename[1]

    if shader_type != ".vert" and shader_type != ".frag" and shader_type != ".comp":
        print("Expected GLSL source file with extension '.vert', '.frag', or '.comp'")
        return 1

    # Create the temp directory, if needed
    temp_dir = os.path.join(os.getcwd(), "temp")
    if not os.path.exists(temp_dir):
        os.makedirs(temp_dir)

    # Compile GLSL to SPIR-V
    spirv_path = os.path.join(temp_dir, shader_name + ".spv")
    result = subprocess.call(["glslc", filename, "-o", spirv_path])
    if result != 0:
        print("Could not compile GLSL code")
        return result

    # Compile SPIR-V to HLSL, if applicable
    if "d3d11" in compile_data:
        hlsl_path = os.path.join(temp_dir, shader_name + ".hlsl")
        result = subprocess.call(["spirv-cross", spirv_path, "--hlsl", "--flip-vert-y", "--shader-model", "50", "--output", hlsl_path])
        if result != 0:
            print("Could not convert SPIR-V to HLSL")
            return result

    # Create the output blob file
    output_filepath = os.path.join(compile_data["output_dir"], shader_name + shader_type + ".refresh")
    with open(output_filepath, "wb") as output_file:
        # Magic
        output_file.write(bytes("RFSH", 'utf-8'))

        # Type
        shader_type_index = 0
        match shader_type:
            case ".vert":
                shader_type_index = 0
            case ".frag":
                shader_type_index = 1
            case ".comp":
                shader_type_index = 2
        output_file.write(shader_type_index.to_bytes(4, byteorder='little'))

        # Actual shader code + metadata
        if "vulkan" in compile_data:
            write_shader_blob(spirv_path, output_file, 0)
        if "d3d11" in compile_data:
            write_shader_blob(hlsl_path, output_file, 1)

    return 0

# Parse arguments

if len(sys.argv) < 2:
    display_help_text()
    exit(1)

compile_data = {}

i = 1
while i < len(sys.argv):
    match sys.argv[i]:
        case "--vulkan":
            compile_data["vulkan"] = True
        case "--d3d11":
            compile_data["d3d11"] = True
        case "--out":
            compile_data["output_dir"] = sys.argv[i + 1]
            i += 1
        case "--preserve-temp":
            compile_data["preserve_temp"] = True
        case _:
            if "input_path" not in compile_data:
                compile_data["input_path"] = sys.argv[i]
            else:
                print("Unknown parameter: " + sys.argv[i])
                exit(1)
    i += 1

# Validation checks

if ("vulkan" not in compile_data) and ("d3d11" not in compile_data):
    print("No platforms selected!")
    exit(1)

if "output_dir" not in compile_data:
    # Assume we want to use the working directory if none is specified
    compile_data["output_dir"] = os.getcwd()

if not os.path.isdir(compile_data["output_dir"]):
    print("Output directory '" + compile_data["output_dir"] + "' does not exist!")
    exit(1)

if "input_path" not in compile_data:
    print("No input file or directory given!")
    exit(1)

# Compile the specified files

input_path = compile_data["input_path"]
if os.path.exists(input_path):
    if os.path.isdir(input_path):
        # Loop over and compile every file in the directory
        filenames = glob.glob(os.path.join(input_path, "*"))
        for filename in filenames:
            print("Compiling file: " + filename)
            result = compile_file(filename, compile_data)
            if result != 0:
                cleanup(compile_data)
                exit(result)
    else:
        result = compile_file(input_path, compile_data)
        if result != 0:
            cleanup(compile_data)
            exit(result)
    cleanup(compile_data)
else:
    print("GLSL source file or directory '" + input_path + "' does not exist!")
    exit(1)
