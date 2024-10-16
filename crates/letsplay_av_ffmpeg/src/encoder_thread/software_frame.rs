// FIXME: This code is fairly rotted and only works for nvenc swframe,
// since the sws code was ripped out long ago.
//
// NVENC swframe is dubiously useful so IMO that can be ripped out and we can
// just have this be the fully software "sad" path.

use anyhow::Context;
use std::{
	sync::{Arc, Mutex},
	time::Duration,
};
use tokio::sync::mpsc::{self, error::TryRecvError};

use crate::ffmpeg;
use crate::h264_encoder::H264Encoder;

use super::{EncodeThreadInput, EncodeThreadOutput};

struct EncoderState {
	encoder: Option<H264Encoder>,
	frame: Arc<Mutex<Option<ffmpeg::frame::Video>>>,
	packet: ffmpeg::Packet,
}

impl EncoderState {
	fn new(frame: Arc<Mutex<Option<ffmpeg::frame::Video>>>) -> Self {
		Self {
			encoder: None,
			frame: frame,
			packet: ffmpeg::Packet::empty(),
		}
	}

	fn init(&mut self, size: crate::types::Size) -> anyhow::Result<()> {
		self.encoder = Some(H264Encoder::new_nvenc_swframe(
			size.clone(),
			60,
			2 * (1024 * 1024),
		)?);

		// replace packet
		self.packet = ffmpeg::Packet::empty();

		Ok(())
	}

	//fn frame(&mut self) -> Arc<Mutex<Option<ffmpeg::frame::Video>>> {
	//    self.frame.clone()
	//}

	fn send_frame(&mut self, pts: u64, force_keyframe: bool) -> Option<ffmpeg::Packet> {
		let mut lk = self.frame.lock().expect("fuck");
		let frame = lk.as_mut().unwrap();
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

fn encoder_thread_main(
	mut rx: mpsc::Receiver<EncodeThreadInput>,
	tx: mpsc::Sender<EncodeThreadOutput>,
	frame: &Arc<Mutex<Option<ffmpeg::frame::Video>>>,
) -> anyhow::Result<()> {
	let mut frame_number = 0u64;
	let mut force_keyframe = false;

	let mut encoder = EncoderState::new(frame.clone());

	loop {
		match rx.try_recv() {
			Ok(msg) => match msg {
				EncodeThreadInput::Init { size } => {
					frame_number = 0;

					if force_keyframe {
						force_keyframe = false;
					}

					encoder.init(size).expect("encoder init failed");
				}

				EncodeThreadInput::ForceKeyframe => {
					force_keyframe = true;
				}

				EncodeThreadInput::SendFrame => {
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
				}
			},

			Err(TryRecvError::Disconnected) => break,
			Err(TryRecvError::Empty) => {
				std::thread::sleep(Duration::from_millis(1));
			}
		}
	}

	Ok(())
}

pub fn spawn(
	frame: &Arc<Mutex<Option<ffmpeg::frame::Video>>>,
) -> (
	mpsc::Receiver<EncodeThreadOutput>,
	mpsc::Sender<EncodeThreadInput>,
) {
	let (in_tx, in_rx) = mpsc::channel(1);
	let (out_tx, out_rx) = mpsc::channel(1);

	let clone = Arc::clone(frame);

	std::thread::spawn(move || encoder_thread_main(in_rx, out_tx, &clone));

	(out_rx, in_tx)
}
