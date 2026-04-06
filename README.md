# KPipeWire

A set of convenient classes to use PipeWire (https://pipewire.org/) in Qt projects, developed in C++ and mainly targeted for use in QML components.


## Features

At the moment KPipeWire offers two main components:
* **KPipeWire:** connect to and render a PipeWire video stream into your app.
* **KPipeWireRecord:** using FFmpeg, records a PipeWire video stream into a file.

## Usage

Refer to the `tests/` subdirectory for two examples of rendering Plasma video streams; one through the internal Plasma Wayland protocol, and the other using the standard XDG Desktop Portals screencast API.

## Licence

This project is licenced under LGPL v2.1+. You can find all the information under `LICENSES/`.
