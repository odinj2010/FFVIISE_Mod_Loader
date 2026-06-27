import os
import subprocess
import sys

def compile_plugin():
    project_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(project_dir)
    
    print("[INFO] Checking for VS environment (cl.exe)...")
    
    # Check if cl is in PATH
    try:
        check_res = subprocess.run(["cl"], capture_output=True, text=True)
    except FileNotFoundError:
        print("[ERROR] cl.exe is not in PATH. Please run this script from the VS Developer Command Prompt.")
        return False
        
    print("[INFO] Assembling assembly hook helper...")
    asm_cmd = [
        "ml64.exe", "/c", 
        "/Fo", "plugins\\battle_overlay\\hook_helper.obj", 
        "plugins\\battle_overlay\\hook_helper.asm"
    ]
    print("Running:", " ".join(asm_cmd))
    asm_res = subprocess.run(asm_cmd, capture_output=True, text=True)
    if asm_res.returncode != 0:
        print("[ERROR] Assembly failed:")
        print(asm_res.stdout)
        print(asm_res.stderr)
        return False
        
    print("[INFO] Compiling battle_overlay.dll with MSVC...")
    cmd = [
        "cl.exe", "/LD", "/O2", "/EHsc",
        "plugins\\battle_overlay\\dllmain.cpp", 
        "plugins\\battle_overlay\\memory.cpp", 
        "plugins\\battle_overlay\\overlay.cpp",
        "plugins\\battle_overlay\\hook_helper.obj",
        "minhook/src/buffer.c", "minhook/src/hook.c", "minhook/src/trampoline.c",
        "minhook/src/hde/hde32.c", "minhook/src/hde/hde64.c",
        "/Iminhook/include",
        "/Iplugins/battle_overlay",
        "/link", "/out:plugins\\battle_overlay.dll",
        "user32.lib", "kernel32.lib", "gdi32.lib"
    ]
    print("Running:", " ".join(cmd))
    compile_res = subprocess.run(cmd, capture_output=True, text=True)
    if compile_res.returncode == 0:
        print("[SUCCESS] Compilation completed successfully! plugins/battle_overlay.dll is ready.")
        return True
    else:
        print("[ERROR] Compilation failed:")
        print(compile_res.stdout)
        print(compile_res.stderr)
        return False

if __name__ == "__main__":
    compile_plugin()
