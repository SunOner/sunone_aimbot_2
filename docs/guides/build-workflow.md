# Build Workflow for Everyday Changes

Set up a backend or update dependencies:

```powershell
.\BUILDER.bat
# Or choose the backend directly:
.\build_dml.bat
.\build_cuda.bat
```

Rebuild an already configured app target:

```powershell
.\build_no-options.bat -Backend DML
.\build_no-options.bat -Backend CUDA
```

The no-options wrapper reuses the build tree without dependency setup or reconfiguration. Use a full wrapper after dependency or build-system changes. See [build requirements and validation](../build.md).
