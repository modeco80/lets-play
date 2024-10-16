use std::ptr::null_mut;

use super::check_ret;

use super::ffmpeg;

pub struct CudaDeviceContext {
	buffer: *mut ffmpeg::sys::AVBufferRef,
}

impl CudaDeviceContext {
	fn new(buffer: *mut ffmpeg::sys::AVBufferRef) -> Self {
		Self { buffer }
	}

	// pub fn as_device_mut(&mut self) -> &mut ffmpeg::sys::AVHWDeviceContext {
	// 	unsafe { &mut *((*self.buffer).data as *mut ffmpeg::sys::AVHWDeviceContext) }
	// }

	// pub fn as_device(&self) -> &ffmpeg::sys::AVHWDeviceContext {
	// 	unsafe { &*((*self.buffer).data as *const ffmpeg::sys::AVHWDeviceContext) }
	// }

	pub fn as_raw_mut(&mut self) -> &mut ffmpeg::sys::AVBufferRef {
		unsafe { &mut *self.buffer }
	}

	// pub fn as_raw(&self) -> &ffmpeg::sys::AVBufferRef {
	// 	unsafe { &*self.buffer }
	// }
}

impl Drop for CudaDeviceContext {
	fn drop(&mut self) {
		unsafe {
			if !self.buffer.is_null() {
				ffmpeg::sys::av_buffer_unref(&mut self.buffer);
			}
		}
	}
}

pub struct CudaDeviceContextBuilder {
	buffer: *mut ffmpeg::sys::AVBufferRef,
}

impl CudaDeviceContextBuilder {
	pub fn new() -> anyhow::Result<Self> {
		let buffer = unsafe { ffmpeg::sys::av_hwdevice_ctx_alloc(ffmpeg::sys::AVHWDeviceType::AV_HWDEVICE_TYPE_CUDA) };
		if buffer.is_null() {
			return Err(anyhow::anyhow!("Could not allocate a hwdevice context".to_string()));
		}

		Ok(Self { buffer })
	}

	pub fn build(mut self) -> Result<CudaDeviceContext, ffmpeg::Error> {
		check_ret(unsafe { ffmpeg::sys::av_hwdevice_ctx_init(self.buffer) })?;
		let result = Ok(CudaDeviceContext::new(self.buffer));
		self.buffer = null_mut();

		result
	}

	pub fn set_cuda_context(mut self, context: ffmpeg::sys::CUcontext) -> Self {
		unsafe {
			(*(self.as_device_mut().hwctx as *mut ffmpeg::sys::AVCUDADeviceContext)).cuda_ctx = context;
		}

		self
	}

	pub fn as_device_mut(&mut self) -> &mut ffmpeg::sys::AVHWDeviceContext {
		unsafe { &mut *((*self.buffer).data as *mut ffmpeg::sys::AVHWDeviceContext) }
	}

	// pub fn as_device(&self) -> &ffmpeg::sys::AVHWDeviceContext {
	// 	unsafe { &*((*self.buffer).data as *const ffmpeg::sys::AVHWDeviceContext) }
	// }

	// pub fn as_raw_mut(&mut self) -> &mut ffmpeg::sys::AVBufferRef {
	// 	unsafe { &mut *self.buffer }
	// }

	// pub fn as_raw(&self) -> &ffmpeg::sys::AVBufferRef {
	// 	unsafe { &*self.buffer }
	// }
}
