Unreleased
- Remove the Qt dependency: UI, OpenGL preview, QtAwesome and QtWaitingSpinner are gone
- The main executable autoremesher is now a headless CLI and never starts a GUI
- Add a Qt-free CMake build (autoremesher_core + autoremesher) and build.bat
- Add the Go GUI in gui/: a separate process that drives the CLI over JSON lines
- Document the CLI/JSON contract in docs/cli-protocol.md

1.2.0
- Reduce triangles and irregular vertices in quad extraction
- Support touch pad pinch and scroll zooming