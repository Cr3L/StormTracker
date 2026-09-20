"""Package the actual LVGL host renders as a contact sheet and animation.

Run tools/test_landscape.ps1 first. Requires Pillow; no firmware dependency.
"""

from pathlib import Path
import subprocess

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "build" / "host"
SCENARIOS = [
    ("day", "Day"), ("dusk", "Twilight"), ("night", "Night"), ("rain", "Rain"),
    ("snow", "Snow"), ("storm", "Thunderstorms"), ("fog", "Fog"), ("cloudy", "Overcast"),
    ("forecast", "Forecast fallback"), ("stale", "Stale weather"),
    ("watch", "Watch"), ("warning", "Warning"),
]


def main():
    sheet = Image.new("RGB", (4 * 268, 3 * 290), "#111a20")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=16)
    for index, (name, title) in enumerate(SCENARIOS):
        x, y = index % 4 * 268 + 14, index // 4 * 290 + 12
        with Image.open(OUTPUT / f"{name}.ppm") as frame:
            sheet.paste(frame, (x, y))
            frame.save(OUTPUT / f"{name}.png")
        draw.text((x + 120, y + 252), title, font=font, fill="#e5edef", anchor="mt")
    sheet.save(OUTPUT / "landscape-preview.png")

    subprocess.run([str(OUTPUT / "preview_landscape.exe"), "rain", "--frames"],
                   cwd=OUTPUT, check=True)
    frames = []
    for path in sorted(OUTPUT.glob("rain-frame-*.ppm")):
        with Image.open(path) as frame:
            frames.append(frame.convert("RGB"))
    frames[0].save(OUTPUT / "landscape-rain.gif", save_all=True,
                   append_images=frames[1:], duration=125, loop=0)
    print(OUTPUT / "landscape-preview.png")
    print(OUTPUT / "landscape-rain.gif")


if __name__ == "__main__":
    main()
