import os
import subprocess
import sys

def compile_dll():
    loader_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(loader_dir)
    
    print("Searching for compilers...")
    
    # Try MinGW g++ first
    try:
        res = subprocess.run(["g++", "-dumpmachine"], capture_output=True, text=True)
        if res.returncode == 0:
            target = res.stdout.strip()
            print(f"Found GCC/g++ targeting: {target}")
            if "x86_64" not in target and "amd64" not in target:
                print("WARNING: GCC is targeting 32-bit. This DLL must be compiled as 64-bit for FFVII.exe.")
            
            cmd = [
                "g++", "-shared", "-o", "d3d11.dll",
                "d3d11_proxy.cpp", "hooks.cpp",
                "minhook/src/buffer.c", "minhook/src/hook.c", "minhook/src/trampoline.c",
                "minhook/src/hde/hde32.c", "minhook/src/hde/hde64.c",
                "-Iminhook/include",
                "-ld3d11", "-lkernel32", "-luser32", "-ld2d1", "-ldwrite",
                "-static", "-O3"
            ]
            print("Running:", " ".join(cmd))
            compile_res = subprocess.run(cmd, capture_output=True, text=True)
            if compile_res.returncode == 0:
                print("Successfully compiled d3d11.dll using GCC!")
                return True
            else:
                print("Compilation failed with GCC:")
                print(compile_res.stderr)
    except FileNotFoundError:
        pass

    # Try MSVC cl.exe
    try:
        # Check target architecture by running cl without args (it prints to stderr)
        check_res = subprocess.run(["cl"], capture_output=True, text=True)
        compiler_info = check_res.stderr if check_res.stderr else check_res.stdout
        
        is_x64 = "x64" in compiler_info
        if not is_x64:
            print("\n" + "="*80)
            print("CRITICAL WARNING: The current Command Prompt is configured for x86 (32-bit) compilation.")
            print("Final Fantasy VII Steam Edition is a 64-bit game and CANNOT load a 32-bit DLL.")
            print("You MUST use the 'x64 Native Tools Command Prompt for VS' to compile the DLL.")
            print("="*80 + "\n")
            
        print("Found MSVC (cl). Compiling...")
        
        # Compile resource file
        print("Compiling resources (resources.rc)...")
        rc_cmd = ["rc.exe", "resources.rc"]
        rc_res = subprocess.run(rc_cmd, capture_output=True, text=True)
        if rc_res.returncode != 0:
            print("Resource compilation failed:")
            print(rc_res.stdout)
            print(rc_res.stderr)
            return False
            
        cmd = [
            "cl.exe", "/LD", "/O2", "d3d11_proxy.cpp", "hooks.cpp",
            "minhook/src/buffer.c", "minhook/src/hook.c", "minhook/src/trampoline.c",
            "minhook/src/hde/hde32.c", "minhook/src/hde/hde64.c",
            "resources.res",
            "/Iminhook/include",
            "/link", "/out:d3d11.dll", "user32.lib", "kernel32.lib", "gdi32.lib", "d3d11.lib", "d2d1.lib", "dwrite.lib"
        ]
        print("Running:", " ".join(cmd))
        compile_res = subprocess.run(cmd, capture_output=True, text=True)
        if compile_res.returncode == 0:
            print("Successfully compiled d3d11.dll using MSVC!")
            if not is_x64:
                print("\n[NOTE] The compiled DLL is 32-bit and will NOT work with the 64-bit game.")
                print("Please close this prompt, open 'x64 Native Tools Command Prompt for VS', and run compile.py again.")
            return True
        else:
            print("Compilation failed with MSVC:")
            print(compile_res.stdout)
            print(compile_res.stderr)
    except FileNotFoundError:
        print("cl.exe is not in PATH.")

    print("\nError: No C++ compiler (g++ or cl.exe) was found in your PATH.")
    print("Please run this command from the 'x64 Native Tools Command Prompt for VS' (search in Start menu).")
    return False

if __name__ == "__main__":
    compile_dll()
