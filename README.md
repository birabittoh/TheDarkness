# TheDarkness

Static recompilation of **The Darkness** for Windows, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

This project converts the Xbox 360 PowerPC `default.xex` into native x86_64
code at build time, then wraps it with a small host runtime (logging,
overlays, hooks) so the game runs natively and can be modded like a PC port.

**You must own the game.** This project does **not** ship any copyrighted code, data, or assets. You provide your own legally dumped game.

# Get the game on [Goopie](https://goopie.xyz/#/library/thedarkness)!

## Using a prebuilt release

Using Goopie is preferable, as it makes it trivial to manage the game's assets, versions, mods, achievements, leaderboards, etc.

If you still want to go the hard way, do this:

1. Install Python if you don't have it already
2. Extract the release you just downloaded
4. Copy your legally obtained game ISO inside the release directory
5. Run `python scripts/extract_game.py` from the release directory

Finally, run the game executable to play the game.

## Building from scratch (development)

### 0. Install dependencies

#### Linux (Arch/CachyOS)
```bash
paru -S clang20 cmake ninja vulkan-headers
```

#### Windows
```powershell
scoop install llvm cmake ninja
```

### 1. Clone

```bash
git clone https://github.com/birabittoh/TheDarkness
cd TheDarkness
```

### 2. Download the ReXGlue SDK

```bash
python scripts/download-sdk.py --pinned
```

### 3. Provide your game

Place your legally dumped ISO file into the current directory, then extract it into `assets/`:

```bash
python scripts/extract_game.py
```

### 4. Build

Use this script:

```bash
# Vanilla
python scripts/build.py --release

# Title Update
python scripts/build.py --release --tu /path/to/TU_*
```

### 5. Run

```bash
python scripts/run.py
```

This runs the freshly built executable with the correct CLI arguments
(`--game_data_root=assets`, `--gpu_plugin=xenos`).

Any extra arguments are forwarded to the executable, e.g.:

```bash
python scripts/run.py --vulkan_device 1
```

## Options

Options can be persisted by adding them to `thedarkness.toml` next to the game executable, for example:

```toml
vulkan_device = 1 # NVIDIA GPU
user_language = 1 # English
```

### Keyboard & mouse

Keyboard and mouse controls are enabled by default. All bindings are overridable in the **F4** menu or `thedarkness.toml`. For example:

```toml
keybind_a = "F"
keybind_left_trigger = "LControl"
mnk_sensitivity = 10
```

Mouse sensitivity is controlled by `mnk_sensitivity` (default `1.0`).

### GPU selection

If you have multiple GPUs, you can force a specific one:

```bash
python scripts/run.py --vulkan_device 1
```

List available devices by running the game without the flag.

### Logging

The game writes logs into the `logs` directory by default, but you can configure it.

```bash
python scripts/run.py --log_file thedarkness.log --log_level debug
```

## Adding a hook

1. Find the guest address in `default.xex`.
2. Add to `thedarkness_config.toml`:

   ```toml
   [functions]
   0x8XXXXXXX = {name = "MyFunction"}
   ```

3. Implement in `src/thedarkness_hooks.cpp` (create if it doesn't exist, and add it to `CMakeLists.txt`):

   ```cpp
   void MyFunction(PPCContext& ctx, uint8_t* base) {
       // your logic
   }
   ```

4. Re-run codegen and rebuild.

## Adding a midasm hook (inline patch)

```toml
[[midasm_hook]]
address = 0x8XXXXXXX
name = "MyHook"
registers = ["r3"]
return = true
```

Implement in `src/thedarkness_hooks.cpp`:

```cpp
void MyHook(PPCRegister& r3) {
    r3.u32 = 1;
}
```

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)

## License

The host-side source in `src/`, build scripts, and CI config are available
under the MIT License.
