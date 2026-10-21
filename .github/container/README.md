# xenia-headless image (fork-only)

`Containerfile` packages a Linux build of the fork's `automation/*` branches
for headless automation: Xenia with its automation channel, the libraries it
links against, Mesa's Vulkan drivers, and ffmpeg with VA-API drivers (Intel
and Mesa) to encode the channel's video stream on the GPU.

Fork CI builds it for every `automation/*` push (the `image` job in
`Fork_CI.yml`) from that run's Linux AppImage, and pushes
`ghcr.io/ecredes/xenia-headless:<branch with / as ->` and `:<commit>`. The
package is private: pull with a token that can read packages.

GHCR sets a package's visibility once, when it's first pushed, and the image's
`org.opencontainers.image.source` label links it to this repository, which is
public, so a new package starts out public. Set it to private in the package's
settings (the API can't), and check that an anonymous pull is refused. Later
pushes keep the package's setting.

game-mcp runs it on remote hosts (`image` in a host profile): rootless
`podman run` (or `docker run`) on the host network, as the host user, with
the GPU's render node and the session folder mounted. By hand:

```sh
podman run --rm --network=host --userns=keep-id --device /dev/dri/renderD128 \
  -v "$PWD":"$PWD" -w "$PWD" -v ~/xenia-isos:"$HOME"/xenia-isos:ro \
  ghcr.io/ecredes/xenia-headless:automation-7-media \
  xenia_canary --headless --automation_port=27000 --storage_root="$PWD"/storage \
  "$HOME"/xenia-isos/<TITLEID>/<game>.iso
```

To build it locally, put a `xenia_canary` binary in a folder and run
`podman build -f .github/container/Containerfile <folder>`. The build fails if
a library the binary needs is missing.
