//! Input devices
pub mod retropad;
pub use retropad::*;

pub mod mouse;
pub use mouse::*;


/// Trait/abstraction for implementing Libretro input devices.
pub trait InputDevice {
	/// Gets the device type. This should never EVER change, and simply return a constant.
	fn device_type(&self) -> u32;

	/// Returns true if the input device is compatible with
	/// the given libretro device ID. 
	/// 
	/// This is needed because Libretro will often look up a device with
	/// either its "base" type (i.e: RetroPad) and then ask for upper buttons
	/// by changing the ID to what it wants to look for (say, Analog RetroPad)
	fn device_type_compatible(&self, id: u32) -> bool;

	/// Gets the state of one button/axis.
	/// is_pressed(id) can simply be expressed as `(get_button(id) != 0)`.
	fn get_button(&self, id: u32) -> i16;

	/// Returns a button mask of all pressed buttons.
	/// Should only be derived by devices that implement
	/// the RetroPad or support it. (effectively, only devices
	/// that superclass the RetroPad do, AFAIK)
	fn button_mask(&self) -> i16 {
		0 as i16
	}

	/// Clears the state of all buttons/axes.
	fn reset(&mut self);

	/// Presses a button/axis.
	fn press_button(&mut self, id: u32, pressure: Option<i16>);

	// FIXME: Add "new" analog support. It can be stubbed out here
	// for devices which don't support it.
}
