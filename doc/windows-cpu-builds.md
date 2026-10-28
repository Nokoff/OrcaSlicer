# Windows CPU build variants

Choose a build by its executable name:

| Build | Executable | Application DLL |
| --- | --- | --- |
| Regular | `snapmaker-orca.exe` | `Snapmaker_Orca.dll` |
| AVX-512 | `snapmaker-orca-avx512.exe` | `Snapmaker_Orca-avx512.dll` |

The regular build is the default. The AVX-512 build requires a compatible CPU and operating system; it does not automatically fall back to the regular build. The graphics card does not determine which CPU build to choose.

Keep each executable with its matching application DLL and the other required runtime files and resources. The differently named pairs may share a portable directory. Save and close an existing instance before switching builds, so single-instance handling does not send the request to the already running build. Both variants use the same project format and include the painting improvements.

Configure separate build directories with `SLIC3R_MSVC_ARCH` empty for the regular build or `AVX512` for the tuned build. `SLIC3R_MSVC_LTO=ON` additionally enables link-time optimization. Build the `Snapmaker_Orca_app_gui` target in Release configuration to produce both the Windows launcher and its application DLL. An `AVX2` build receives an `-avx2` suffix in the same way.

Windows installer executable references and CPU-specific installer filenames follow the selected variant. These are manual build choices, not automatic CPU detection.
