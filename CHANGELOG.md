# Changelog

All notable changes to neggia are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Releases before 1.3.1 are reconstructed from the GitHub release pages and the
commit history.

## [Unreleased]

## [1.3.1-beta.1] — 2026-09-11

Pre-release for testing. **Not intended for production data.**

### Fixed

- **The plugin no longer aborts while reading a chunked pixel mask.** Sizing a
  chunk assumed a three-dimensional dataset and read past the end of the
  dimension vector for a chunked two-dimensional one, such as a pixel mask
  stored chunked. Depending on what happened to follow in memory, this aborted
  the process.

  This was present in every earlier release. The failure mode is a loud abort,
  never silently incorrect data — no dataset that processed successfully was
  affected by it.

### Changed

- **Large reduction in memory use on big detectors.** The master file is now
  mapped once per process and the pixel mask allocated once, shared across
  worker threads, rather than one of each per worker. On a 16M detector with 16
  workers this reclaims roughly 1 GB per process.
- **Reworked concurrent access.** The global handle singleton was replaced with
  a per-worker pool: each worker owns its own cache and file mapping, and frame
  reads are dispatched by frame number.

### Testing

- Added a frame-read benchmark harness.
- Added ThreadSanitizer and Helgrind jobs to continuous integration.
- Extended the concurrent stress test to cover more file layouts.

## [1.2.0] — 2021-03-26

### Added

- Support for HDF5 superblock version 3.
- Support for Data Layout Message version 4.
- Tools for checking whether an HDF5 file is compatible with the plugin; see
  the README.

### Fixed

- Reading uncompressed chunked data.
- Overflow values were not set to -1 for `uint16` and `uint8` input data.

## [1.1.1] — 2021-03-10

### Changed

- Build system uses CMake features to determine compiler flags and related
  settings.

## [1.1.0] — 2021-03-09

### Added

- Support for Eiger2 HDF5 files.
- Support for HDF5 superblock version 2.

## [1.0.2] — 2021-03-09

### Fixed

- 16-bit image depth data is handled correctly.

## [1.0.1] — 2021-03-09

### Changed

- Exception text now contains `NEGGIA ERROR`, so it is obvious in XDS logs
  where an error came from.

## [1.0.0] — 2021-03-09

Initial release. XDS plugin for reading HDF5 files written by DECTRIS Eiger
detectors.
