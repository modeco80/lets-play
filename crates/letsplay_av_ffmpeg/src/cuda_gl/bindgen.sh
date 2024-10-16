#!/bin/bash
# Does bindgen for CUDA cudaGL. (needs postprocessing. FIXME)
set -exu

# add types from existing bindings
echo "use cudarc::driver::sys::*; /* Hack :3 */" > ./sys.rs
echo "use gl::types::{GLenum, GLuint};" >> ./sys.rs

bindgen \
  --allowlist-item="^cuGraphicsGL.*" \
  --blocklist-type="(cu.*|CU.*|GL.*)" \
  --default-enum-style=rust \
  --no-doc-comments \
  --with-derive-default \
  --with-derive-eq \
  --with-derive-hash \
  --with-derive-ord \
  --use-core \
  --dynamic-loading Lib \
  gl.h -- -I/opt/cuda/include \
  >> ./sys.rs
