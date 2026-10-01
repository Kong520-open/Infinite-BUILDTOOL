# Infinite-BUILDTOOL

An Android building tools project for Xposed/LSPosed, providing in-game structure import, image import, structure projection and printing, and structure export.

See the [project overview](BUILDTOOL项目实现功能总览.md) for details of the current implementation.

The native `tp/` directory provides building tool JNI interfaces, game tick scheduling, world queries, and packet/Python bridges. Export can request movement between capture regions using the server's `/tp` command; this requires the appropriate permissions and is confirmed from the player's actual position.

## Usage Guidelines

This tool is intended solely for building assistance and creative projects. It does not include features designed to attack or harass other players, or maliciously disrupt normal gameplay. Use it with respect for other players' experience and follow the rules of the world or server you are playing on.

All features must be used within the permissions and resource conditions allowed by the game or server. Commands, administrative actions, and teleportation require the appropriate permissions; building by placing blocks requires the necessary materials. Use this tool only within the scope you are authorized to access.

## AI-Assisted Maintenance

This project is developed and maintained almost entirely with AI assistance. Its coding style and implementation quality are still being refined. Constructive feedback, fixes, and personal modifications are welcome. If the current code quality or this approach to maintenance does not meet your expectations, you are free to choose not to use the project.

## License

Project-owned code is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE).
You may use, modify, and redistribute the project for non-commercial purposes only. Commercial use requires separate permission from the copyright holder.
Third-party code and libraries remain subject to their respective licenses; the root license does not replace them.
