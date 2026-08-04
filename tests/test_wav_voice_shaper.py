import configparser
import math
import os
import struct
import subprocess
import tempfile
import unittest
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = ROOT / "WavVoiceShaper" / "WavVoiceShaper.ini"
WORLD_ROOT = ROOT / "WavVoiceShaper" / "third_party" / "world"


class RepositoryTests(unittest.TestCase):
    def test_project_and_world_license_boundaries_are_explicit(self) -> None:
        project_license = (ROOT / "LICENSE").read_text(encoding="utf-8")
        notice = (ROOT / "NOTICE.md").read_text(encoding="utf-8")

        self.assertIn("Copyright (c) 2026 Keisuke Katahira", project_license)
        self.assertIn("WavVoiceShaper/third_party/world/", notice)
        self.assertIn("BSD 3-Clause License", notice)

    def test_all_presets_have_the_same_complete_schema(self) -> None:
        parser = configparser.ConfigParser(interpolation=None)
        parser.optionxform = str
        parser.read(CONFIG_PATH, encoding="utf-8-sig")

        preset_names = [
            "標準",
            "明瞭化",
            "緊急放送向け",
            "低め落ち着き",
            "小型スピーカー向け",
            "ラジオ風",
            "カスタム",
            "男性化ボイスチェンジ",
            "女性化ボイスチェンジ",
            "重低音ボイスチェンジ",
            "ロボットボイス",
            "子供ボイス",
            "ロボット自然",
        ]
        sections = [f"Preset.{name}" for name in preset_names]
        self.assertEqual(sections, [name for name in parser.sections() if name.startswith("Preset.")])
        self.assertEqual("標準", parser["General"]["DefaultPreset"])

        expected_keys = set(parser[sections[0]].keys())
        self.assertGreater(len(expected_keys), 25)
        for section in sections:
            with self.subTest(section=section):
                self.assertEqual(expected_keys, set(parser[section].keys()))
                self.assertIn(parser[section]["VoiceChangerEngine"], {"Builtin", "World", "Auto"})

    def test_world_sources_and_license_are_kept_together(self) -> None:
        license_text = (WORLD_ROOT / "LICENSE.txt").read_text(encoding="ascii")
        self.assertIn("Copyright (c) 2010  M. Morise", license_text)
        self.assertIn("Redistribution and use in source and binary forms", license_text)

        cpp_files = sorted((WORLD_ROOT / "src").glob("*.cpp"))
        header_files = sorted((WORLD_ROOT / "src" / "world").glob("*.h"))
        self.assertGreaterEqual(len(cpp_files), 10)
        self.assertGreaterEqual(len(header_files), 10)


class CliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        configured_path = os.environ.get("WVS_EXE", "").strip()
        if not configured_path:
            raise unittest.SkipTest("WVS_EXE is not set; repository-only tests remain available")

        cls.executable = Path(configured_path).resolve()
        if not cls.executable.is_file():
            raise AssertionError(f"WavVoiceShaper executable was not found: {cls.executable}")

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [str(self.executable), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    @staticmethod
    def write_test_wav(path: Path, sample_width: int = 2) -> None:
        sample_rate = 16_000
        frame_count = int(sample_rate * 0.6)
        frames = bytearray()
        for index in range(frame_count):
            active = int(sample_rate * 0.1) <= index < int(sample_rate * 0.5)
            value = math.sin(2.0 * math.pi * 440.0 * index / sample_rate) if active else 0.0
            if sample_width == 2:
                frames.extend(struct.pack("<h", round(value * 9_000)))
            else:
                frames.append(round((value * 0.35 + 0.5) * 255))

        with wave.open(str(path), "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(sample_width)
            wav_file.setframerate(sample_rate)
            wav_file.writeframes(frames)

    def test_help_and_argument_exit_codes(self) -> None:
        self.assertEqual(0, self.run_cli("--help").returncode)
        self.assertEqual(1, self.run_cli("--unknown-option").returncode)

        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            result = self.run_cli(
                "--in",
                str(temporary / "missing.wav"),
                "--out",
                str(temporary / "output.wav"),
            )
            self.assertEqual(2, result.returncode)

    def test_16_bit_pcm_is_processed_from_a_synthetic_wav(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "source.wav"
            output = temporary / "processed.wav"
            self.write_test_wav(source)

            result = self.run_cli(
                "--in",
                str(source),
                "--out",
                str(output),
                "--config",
                str(CONFIG_PATH),
                "--preset",
                "明瞭化",
                "--set",
                "RemoveDcOffset=1",
            )

            self.assertEqual(0, result.returncode, result.stderr.decode(errors="replace"))
            self.assertTrue(output.is_file())
            self.assertNotEqual(source.read_bytes(), output.read_bytes())

            with wave.open(str(source), "rb") as source_wav, wave.open(str(output), "rb") as output_wav:
                self.assertEqual(source_wav.getnchannels(), output_wav.getnchannels())
                self.assertEqual(source_wav.getsampwidth(), output_wav.getsampwidth())
                self.assertEqual(source_wav.getframerate(), output_wav.getframerate())
                self.assertLess(output_wav.getnframes(), source_wav.getnframes())
                self.assertGreater(output_wav.getnframes(), 0)

    def test_unsupported_pcm_width_is_copied_without_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            source = temporary / "source-8bit.wav"
            output = temporary / "copied.wav"
            self.write_test_wav(source, sample_width=1)

            result = self.run_cli("--in", str(source), "--out", str(output))

            self.assertEqual(0, result.returncode, result.stderr.decode(errors="replace"))
            self.assertEqual(source.read_bytes(), output.read_bytes())


if __name__ == "__main__":
    unittest.main()
