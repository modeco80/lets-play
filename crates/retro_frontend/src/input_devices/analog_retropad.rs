use crate::libretro_sys_new;

use super::{InputDevice, RetroPad};

/// Implementation of the [InputDevice] trait for the
/// Analog RetroPad. Currently, this is mostly a stub which calls
/// into the RetroPad implementation w/out actually implementing
/// any of the analog axes or addl. features.
pub struct AnalogRetroPad {
	pad: RetroPad,
}

impl AnalogRetroPad {
	pub fn new() -> Self {
		Self {
			pad: RetroPad::new(),
		}
	}
}

// Sidenote: I really don't like the fact I have to manually thunk,
// but thankfully there's only like one, maybe 2 levels of subclassing
// in the Libretro input APIs, so it won't grow too awfully..

impl InputDevice for AnalogRetroPad {
	fn device_type(&self) -> u32 {
		libretro_sys_new::DEVICE_ANALOG
	}

	fn device_type_compatible(&self, id: u32) -> bool {
		if self.pad.device_type_compatible(id) {
			// If the RetroPad likes it, then so do we.
			true
		} else {
			// Check for the analog type
			id == self.device_type()
		}
	}

	fn button_mask(&self) -> i16 {
		return self.pad.button_mask();
	}

	fn reset(&mut self) {
		self.pad.reset();
		// FIXME: reset analog axes?
	}

	fn get_button(&self, id: u32) -> i16 {
		// FIXME: Handle analog axes
		return self.pad.get_button(id);
	}

	fn press_button(&mut self, id: u32, pressure: Option<i16>) {
		// FIXME: "press" axes.
		self.pad.press_button(id, pressure);
	}
}
