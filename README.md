<p align="center"><img src="assets/logo.png" width="160" alt="Virtual Prism for OpenXR logo"></p>

# Virtual Prism for OpenXR (Double Vision Aid)

A "virtual prism" for VR. If you have double vision (diplopia / strabismus / a paralysed eye muscle) and wear prism glasses in real life but
can't fit prism lenses in your headset, this tool shifts and rotates each eye's image so the two images
line up again - in **any OpenXR game**.

> **Not a medical device.** It does not treat or diagnose anything. Ask your eye doctor about prism
> values, and take breaks if VR is uncomfortable.

<p align="center"><img src="docs/screenshot.png" width="800" alt="Virtual Prism settings window with the live spectator view"></p>

**Download:** get the latest zip from the [Releases page](https://github.com/criso2hd-alt/Virtual-Prism-for-OpenXR-Double-Vision-Aid/releases/latest), unzip it anywhere and run `VirtualPrism.exe`.

## Why this exists
I have been using VR since the Oculus DK2. A few years ago a serious illness left one of my eye muscles
working poorly, and I have had double vision ever since. In real life I wear glasses with prism, but a
headset has no place for them. Custom prism lenses for a VR headset are often impractical, and a pair
can cost as much as the headset itself.

So I built the prism in software. Instead of bending light, Virtual Prism shifts and rotates each eye's
image by the amount you need, and you set it by lining the two images up inside the headset. The first time
I aligned them, my eyes locked on and I saw one clear image again. I'm sharing it, free, in case it helps
other people who love VR and have the same problem.

## How it works
- **API layer** (`XR_APILAYER_NOVENDOR_virtual_prism.dll`): an OpenXR API layer loaded by the OpenXR
  loader into every OpenXR app. It turns each eye's view slightly before the game renders and puts the
  original pose back when the frame is submitted, so the compositor displays that eye's image shifted
  by exactly the correction (horizontal / vertical prism + roll).
- **VirtualPrism.exe**: a settings window (type your prescription, turn the effect on/off, install the
  layer) and an in-headset calibration where you align the two eyes' images with the controllers.

Settings live in `%APPDATA%\VirtualPrismOpenXR\config.ini`. Running games notice changes within a
second, so the on/off checkbox works live (handy when someone else wants to use your headset).

While you calibrate, the desktop window shows a live spectator view of what you see, so a helper can follow along.

## Use
1. Run `VirtualPrism.exe`, click **Install layer** (per-user, no admin needed).
2. Optionally type your prescription (prism diopters + base direction) and click **Save settings**.
3. Make sure your headset's OpenXR runtime is active (Meta Quest Link: Settings > General > "Set Meta
   Quest Link as active OpenXR runtime"), connect the headset, click **Start VR calibration**, put it on.
4. In VR, align the images:

| Control | Action |
|---|---|
| Right stick / Left stick | move the right / left eye image |
| A / Y | rotate clockwise (right / left eye) |
| B / X | rotate counter-clockwise (right / left eye) |
| Stick click | reset that eye |
| Left trigger | both eyes / left only / right only |
| Left grip | next scene |
| Right grip | step size: coarse / medium / fine |
| Menu button | show / hide help |
| Hold right trigger | save and exit |

5. Start any OpenXR game. Restart games that were already running when you installed the layer.

Units: horizontal/vertical values are prism diopters (1 = 1 cm at 1 m, about 0.57 degrees); rotation is in degrees.

## Limitations
- Only OpenXR apps. Games that talk directly to OpenVR/SteamVR bypass the layer (use their OpenXR mode,
  or SteamVR's OpenXR path where available).
- Only projection layers (the game's 3D view) are corrected, not 2D overlay/quad layers some games use for menus.
- It shifts the image (field of view at the edge of one eye is lost); it does not bend light like a real prism.
- Tested design target: Meta Quest 3 over Quest Link (Touch controllers).

## Build
Requires Visual Studio 2022+ with C++ and CMake. The OpenXR SDK is fetched automatically.
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Output is in `build\Release` (`VirtualPrism.exe`, the layer DLL + JSON, `openxr_loader.dll`) - keep them together.

## License
Free to use, and the source is open for you to read and to suggest changes via issues and pull requests.
It is **not** open source in the MIT/GPL sense: please do not re-upload or redistribute it, and do not
change the author credit or the support link. See [LICENSE](LICENSE) and [CONTRIBUTING.md](CONTRIBUTING.md).
Download releases only from this repository.

---
Made by Chris Soares. If this helps you, you can [buy me a coffee](https://buymeacoffee.com/criso2hdj).
