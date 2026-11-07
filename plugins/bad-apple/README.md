# bad-apple

The screen becomes flip-dot tiles that play a video over the desktop, Bad Apple!! being the one it is made for. Keys: `Super+A`.

```lua
-- config.lua
shady.plugins.load("bad-apple")
```

No video ships with it: make 1-bit frames and a WAV from your own copy with
`./bad-apple.sh <video>` (it needs ffmpeg). `SHADY_BAD_APPLE_PLAYER` names another
audio player than pw-play, paplay or aplay.
