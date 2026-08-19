# The no-Streamline interposer

NVIDIA Streamline (`sl.interposer.dll` + `sl.reflex.dll` and friends) is the
DLSS/Reflex plugin layer some titles route ALL of their D3D12/DXGI creation
through — Cyberpunk 2077's exe imports the interposer statically and calls
`D3D12CreateDevice` through it.  Its plugins install detour hooks by copying
instructions into anonymous trampolines, and on this port the instructions
they copy are TRAP STUBS: sl.reflex ends up calling instruction bytes as a
function pointer (measured: wild pointer `518B4908488D4810` = `push rcx;
mov ecx,[rcx+8]; ...`), from memory no unwinder can walk.  On AMD hardware
every Streamline feature is unsupported anyway.

`sl_interposer_stub.c` is the honest answer: a guest x86-64 DLL that
forwards the D3D/DXGI creators to the real modules and returns failure from
every `sl*` entry point, which the game's own Streamline error handling
turns into "feature unavailable".  Staged into a prefix's guest system
directory, where find_dll_file()'s guest branch finds it BEFORE the game's
own copy — the same mechanism msvcp100's staging note in
ppc64le/steamtool/proton documents.

Build (any host with clang; kernel32 import lib via llvm-dlltool):

    printf 'LIBRARY kernel32.dll\nEXPORTS\n LoadLibraryA\n GetProcAddress\n' > k32.def
    llvm-dlltool -m i386:x86-64 -d k32.def -l libk32.a
    clang -target x86_64-windows-gnu -nostdlibinc -I include -I include/msvcrt \
        -D_UCRT -O1 -fno-builtin -c -o slstub.o ppc64le/streamline/sl_interposer_stub.c
    clang -target x86_64-windows-gnu -fuse-ld=lld -nostdlib -shared \
        -Wl,--entry=DllMain -o sl.interposer.dll slstub.o libk32.a

Stage:

    cp sl.interposer.dll <prefix>/drive_c/windows/sysx8664/

Not yet a proton-tool staging rule; promote it when a second title needs it.
