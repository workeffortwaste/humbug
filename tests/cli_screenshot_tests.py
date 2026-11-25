import importlib.util
from pathlib import Path
import tempfile
import unittest

from PIL import Image, ImageFont


SCRIPT = Path(__file__).resolve().parents[1] / "src/view/cardputer_zero/tools/generate_cli_screenshots.py"
spec = importlib.util.spec_from_file_location("generate_cli_screenshots", SCRIPT)
screenshots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(screenshots)


class CliScreenshotTests(unittest.TestCase):
    def test_frame_starts_at_last_cursor_home(self):
        # Repeated cursor resets must not leak escape sequences into captured frames.
        output = b"\x1b[H\x1b[Hframe\x1b[J"
        self.assertEqual(screenshots.FRAME.findall(output), [b"frame"])

    def test_box_drawing_joins_and_dim_text(self):
        # Pixel checks catch broken borders and lost dim styling in generated previews.
        frame = ("\x1b[2m┌─┐\x1b[0m\x1b[K\r\n"
                 "\x1b[2m│\x1b[0mA\x1b[2m│\x1b[0m\x1b[K\r\n"
                 "\x1b[2m└─┘\x1b[0m\x1b[K\r\n")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "preview.png"
            screenshots.draw_frame(frame, "DejaVuSansMono.ttf", path)
            with Image.open(path) as image:
                width = round(ImageFont.truetype("DejaVuSansMono.ttf", 18).getlength("M"))
                start = 20 + width // 2
                end = 20 + 2 * width + width // 2
                for x in range(start, end + 1):
                    self.assertEqual(image.getpixel((x, 32)), (127, 135, 151))
                for y in range(32, 81):
                    self.assertEqual(image.getpixel((start, y)), (127, 135, 151))
                self.assertTrue(any(image.getpixel((x, y)) == (226, 230, 239)
                                    for x in range(20 + width, 20 + 2 * width)
                                    for y in range(44, 68)))

    def test_rejects_unknown_terminal_styles(self):
        # Unsupported styling should fail rather than produce a misleading screenshot.
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Unsupported terminal escape"):
                screenshots.draw_frame("\x1b[31mred\x1b[K\n", "DejaVuSansMono.ttf",
                                       Path(directory) / "preview.png")

    def test_narrow_terminal_image_width(self):
        # Narrow terminal captures must preserve their actual column width.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "narrow.png"
            screenshots.draw_frame("│\x1b[K\n", "DejaVuSansMono.ttf", path, 44)
            with Image.open(path) as image:
                width = round(ImageFont.truetype("DejaVuSansMono.ttf", 18).getlength("M"))
                self.assertEqual(image.width, 40 + 44 * width)


if __name__ == "__main__":
    unittest.main()
