# Changelog

All notable changes to this project will be documented in this file.

## [2026-09-05]

### Added
- Added USB Hub support (`CFG_TUH_HUB`), enabling multiple USB HID devices to be connected simultaneously via a USB hub.
- Added support for composite HID devices and multi-device aggregation (merges up to 8 HID interfaces into a single BLE device).

### Fixed
- Fixed an issue where re-pairing failed after the peer deleted bonding information by automatically clearing stale bonding keys and disconnecting upon encryption failure to allow a clean re-pair.
- Other minor fixes and improvements.

## [2026-08-11]

### Added
- Added support for Raspberry Pi Pico W.

### Changed
- Changed the system clock to 240MHz (required for Pico W operation).

## [2026-06-24]

### Fixed
- Fixed an issue where pairing with an iPad sometimes failed.

## [2026-06-21]

### Added
- Initial release.
