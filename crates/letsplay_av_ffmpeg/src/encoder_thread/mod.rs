pub mod hardware_frame;
pub mod software_frame;

pub enum EncodeThreadInput {
    Init { size: crate::types::Size },
    ForceKeyframe,
    SendFrame,
}

#[derive(Clone)]
pub enum EncodeThreadOutput {
    Frame { packet: ffmpeg::Packet },
}
