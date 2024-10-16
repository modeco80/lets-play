use anyhow::Context;
use cudarc::{
	driver::{
		sys::{CUdeviceptr, CUmemorytype},
		CudaDevice, CudaSlice, DevicePtr, LaunchAsync,
	},
	nvrtc::CompileOptions,
};
use letsplay_gpu::egl_helpers::DeviceContext;
use std::{
	sync::{Arc, Mutex},
	time::Duration,
};
use tokio::sync::mpsc::{self, error::TryRecvError};

use crate::h264_encoder::H264Encoder;
use crate::{cuda_gl::safe::GraphicsResource, ffmpeg};

use super::{EncodeThreadInput, EncodeThreadOutput};

struct EncoderStateHW {
	encoder: Option<H264Encoder>,
	frame: ffmpeg::frame::Video,
	packet: ffmpeg::Packet,
}

impl EncoderStateHW {
	fn new() -> Self {
		Self {
			encoder: None,
			frame: ffmpeg::frame::Video::empty(),
			packet: ffmpeg::Packet::empty(),
		}
	}

	fn init(&mut self, device: &Arc<CudaDevice>, size: crate::types::Size) -> anyhow::Result<()> {
		self.encoder = Some(H264Encoder::new_nvenc_hwframe(
			&device,
			size.clone(),
			60,
			2 * (1024 * 1024),
		)?);

		// replace packet
		self.packet = ffmpeg::Packet::empty();
		self.frame = self.encoder.as_mut().unwrap().create_frame()?;

		Ok(())
	}

	#[inline]
	fn frame(&mut self) -> &mut ffmpeg::frame::Video {
		&mut self.frame
	}

	fn send_frame(&mut self, pts: u64, force_keyframe: bool) -> Option<ffmpeg::Packet> {
		let frame = &mut self.frame;
		let encoder = self.encoder.as_mut().unwrap();

		// set frame metadata
		unsafe {
			if force_keyframe {
				(*frame.as_mut_ptr()).pict_type = ffmpeg::sys::AVPictureType::AV_PICTURE_TYPE_I;
				(*frame.as_mut_ptr()).flags = ffmpeg::sys::AV_FRAME_FLAG_KEY;
				(*frame.as_mut_ptr()).key_frame = 1;
			} else {
				(*frame.as_mut_ptr()).pict_type = ffmpeg::sys::AVPictureType::AV_PICTURE_TYPE_NONE;
				(*frame.as_mut_ptr()).flags = 0i32;
				(*frame.as_mut_ptr()).key_frame = 0;
			}

			(*frame.as_mut_ptr()).pts = pts as i64;
		}

		encoder.send_frame(&*frame);
		encoder
			.receive_packet(&mut self.packet)
			.expect("Failed to recieve packet");

		unsafe {
			if !self.packet.is_empty() {
				return Some(self.packet.clone());
			}
		}

		return None;
	}
}

/// Source for the kernel used to flip OpenGL framebuffers right-side up.
// FIXME: Optimize this kernel to spit out multiple pixels per block.
const OPENGL_FLIP_KERNEL_SRC: &str = "
extern \"C\" __global__ void flip_opengl(
    const unsigned* pSrc,
    unsigned* pDest,
    int width, 
    int height
) {
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x < width && y < height) {
        unsigned reversed_y = (height - 1) - y;
        ((unsigned*)pDest)[y * width + x] = ((unsigned*)pSrc)[reversed_y * width + x];
    }
}";

fn encoder_thread_hwframe_main(
	mut rx: mpsc::Receiver<EncodeThreadInput>,
	tx: mpsc::Sender<EncodeThreadOutput>,

	cuda_device: &Arc<CudaDevice>,
	cuda_resource: &Arc<Mutex<GraphicsResource>>,
	gl_context: &Arc<Mutex<DeviceContext>>,
) -> anyhow::Result<()> {
	let mut frame_number = 0u64;
	let mut force_keyframe = false;

	let mut encoder = EncoderStateHW::new();

	// :)
	cuda_device.bind_to_thread()?;

	// Compile the support kernel
	let ptx = cudarc::nvrtc::compile_ptx_with_opts(
		&OPENGL_FLIP_KERNEL_SRC,
		CompileOptions {
			//options: vec!["--gpu-architecture=compute_50".into()],
			..Default::default()
		},
	)
	.with_context(|| "compiling support kernel")?;

	// pop it in
	cuda_device.load_ptx(ptx, "module", &["flip_opengl"])?;

	let mut memcpy = cudarc::driver::sys::CUDA_MEMCPY2D_st::default();

	// setup the things that won't change about the cuda memcpy

	// src
	memcpy.srcXInBytes = 0;
	memcpy.srcY = 0;
	memcpy.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_ARRAY;

	// dest
	memcpy.dstXInBytes = 0;
	memcpy.dstY = 0;
	memcpy.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;

	// Temporary buffer used for opengl flip on the GPU. We copy to this buffer,
	// then copy the flipped version (using the launched support kernel) to the CUDA device memory ffmpeg
	// allocated.
	let mut temp_buffer: CudaSlice<u32> = cuda_device.alloc_zeros::<u32>(48).expect("over");

	loop {
		match rx.blocking_recv() {
			Some(msg) => match msg {
				EncodeThreadInput::Init { size } => {
					frame_number = 0;

					if force_keyframe {
						force_keyframe = false;
					}

					temp_buffer = cuda_device
						.alloc_zeros::<u32>((size.width * size.height) as usize)
						.expect("Could not allocate temporary buffer");

					encoder
						.init(cuda_device, size)
						.expect("Failed to initalize FFmpeg NVENC encoder");
				}

				EncodeThreadInput::ForceKeyframe => {
					force_keyframe = true;
				}

				EncodeThreadInput::SendFrame => {
					// benchmarking
					//use std::time::Instant;
					//let start = Instant::now();

					// copy gl frame *ON THE GPU* to ffmpeg frame
					{
						let gl_ctx = gl_context.lock().expect("Couldn't lock EGL device context");
						let mut gl_resource =
							cuda_resource.lock().expect("Couldn't lock CUDA graphics resource");

						gl_ctx.make_current();

						let mut mapped = gl_resource
							.map()?;

						let array = mapped
							.get_mapped_array()?;

						let frame = encoder.frame();

						// setup the cuMemcpy2D operation to copy to the temporary buffer
						// (we should probably abstract source and provide a way to elide this,
						// and instead feed ffmpeg directly. for now it's *just* used with gl so /shrug)
						{
							memcpy.srcArray = array;

							unsafe {
								let frame_ptr = frame.as_mut_ptr();
								memcpy.dstDevice = temp_buffer.device_ptr().clone();
								memcpy.dstPitch = (*frame_ptr).linesize[0] as usize;
								memcpy.WidthInBytes = ((*frame_ptr).width * 4) as usize;
								memcpy.Height = (*frame_ptr).height as usize;
							}
						}

						// copy to the temporary buffer and synchronize
						unsafe {
							cudarc::driver::sys::lib()
								.cuMemcpy2DAsync_v2(&memcpy, std::ptr::null_mut())
								.result()
								.expect("cuMemcpy2D failed");

							cudarc::driver::sys::lib()
								.cuStreamSynchronize(std::ptr::null_mut())
								.result()?;
						}

						// launch kernel to flip the opengl framebuffer right-side up
						{
							let width = frame.width();
							let height = frame.height();

							let launch_config = cudarc::driver::LaunchConfig {
								grid_dim: (width / 16 + 1, height / 2 + 1, 1),
								block_dim: (16, 2, 1),
								shared_mem_bytes: 0,
							};

							let flip_opengl = cuda_device.get_func("module", "flip_opengl").expect(
								"for some reason we couldn't get the support kernel function",
							);

							unsafe {
								let frame_ptr = frame.as_mut_ptr();

								let mut slice = cuda_device.upgrade_device_ptr::<u32>(
									(*frame_ptr).data[0] as CUdeviceptr,
									(width * height) as usize * 4usize,
								);

								flip_opengl.launch(
									launch_config,
									(&mut temp_buffer, &mut slice, width, height),
								)?;

								// leak so it doesn't free the memory
								// (the device pointer we convert into a slice is owned by ffmpeg, so we shouldn't be the ones
								//  trying to free it!)
								let _ = slice.leak();

								// Synchronize for the final time
								cudarc::driver::sys::lib()
									.cuStreamSynchronize(std::ptr::null_mut())
									.result()?;
							}
						}

						// FIXME: ideally this would work on-drop but it doesn't.
						mapped.unmap().expect("Failed to unmap CUDA graphics resource");
						gl_ctx.release();
					}

					if let Some(pkt) = encoder.send_frame(frame_number as u64, force_keyframe) {
						// A bit less clear than ::empty(), but it's "Safe"
						if let Some(_) = pkt.data() {
							let _ = tx.blocking_send(EncodeThreadOutput::Frame {
								packet: pkt.clone(),
							});
						}

						frame_number += 1;
					}

					if force_keyframe {
						force_keyframe = false;
					}

					//tracing::info!("encoding frame {frame_number} took {:2?}", start.elapsed());
				}
			},

			None => break,
		}
	}

	//std::thread::sleep(Duration::from_millis(1));

	Ok(())
}

pub fn spawn(
	cuda_device: &Arc<CudaDevice>,
	cuda_resource: &Arc<Mutex<GraphicsResource>>,
	gl_context: &Arc<Mutex<DeviceContext>>,
) -> (
	mpsc::Receiver<EncodeThreadOutput>,
	mpsc::Sender<EncodeThreadInput>,
) {
	let (in_tx, in_rx) = mpsc::channel(1);
	let (out_tx, out_rx) = mpsc::channel(1);

	let dev_clone = Arc::clone(cuda_device);
	let rsrc_clone = Arc::clone(cuda_resource);
	let gl_clone = Arc::clone(gl_context);

	std::thread::spawn(move || {
		encoder_thread_hwframe_main(in_rx, out_tx, &dev_clone, &rsrc_clone, &gl_clone)
	});

	(out_rx, in_tx)
}
