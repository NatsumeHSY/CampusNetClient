# miniz

- Upstream: https://github.com/richgel999/miniz
- Version: 3.1.2
- Exact upstream commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- Source files: `miniz.c`, `miniz.h`, `miniz_common.h`, `miniz_tdef.c`,
  `miniz_tdef.h`, `miniz_tinfl.c`, `miniz_tinfl.h`, `miniz_zip.c`,
  `miniz_zip.h`
- License: see `LICENSE` (upstream MIT license)
- `miniz_export.h` is a local build shim. Upstream generates this header from
  CMake; this project compiles the pinned sources directly with MSBuild.

The ZIP extractor still enforces CampusNetClient-specific path, package-layout,
file-count, and uncompressed-size restrictions before writing any file.
