# Escreen

A simple Wayland screenshot tool with built-in sketching capabilities.

## Features
- **Interactive Sketching**: Draw on your selection before saving.
- **Modular Tools**: brush, blur, arrow and shape tools.
- **Configurable**: Fully themeable UI and automatic saving.

## Getting Started

### Dependencies
Ensure you have the following installed:
- `wayland`, `wayland-protocols`
- `cairo`, `libpng`, `pixman`
- `xkbcommon`
- `gcc`, `g++`, `make`, `pkg-config`

### Compilation
Simply run:
```bash
make -j$(nproc)
```

### Usage
```bash
./escreen [options]
```
- `-c`: Copy to clipboard (default).
- `-f [path]`: Save to file (optional custom path).
- `-d <delay>`: Delay in seconds.
- `-h`: Show help.

## Customization

Escreen automatically generates a default configuration file at `~/.config/escreen/config` on its first run.

### Default Auto-Save Path
By default, auto-saved screenshots are stored in `~/Pictures/escreen/`. You can change this by modifying the `auto_save_path` key in the config file.

### Filename Formatting
The `auto_save_filename_format` setting supports standard `strftime` syntax.
- Default: `escreen_%Y%m%d_%H%M%S`
- Example: `screenshot_%H-%M-%S` results in `screenshot_14-30-05.png`.

### Example Configuration
```ini
# Auto-save behavior
auto_save_enabled=true
auto_save_path=/home/user/Pictures/escreen
auto_save_format=png
auto_save_filename_format=escreen_%Y%m%d_%H%M%S

# Color Scheme (Hex: #RRGGBBAA)
color_accent=#0099FFFF
color_toolbar_bg=#262626F2
color_button_hover=#404040FF
```

### Color Keys
- `color_accent`: Used for selection borders and active tool highlights.
- `color_toolbar_bg`: The background color of the floating toolbar.
- `color_button_hover`: The background of a button when hovered.

## License
MIT
