"""Configure-only SDK routing tests; fixtures are not real platform binaries."""

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULES = ROOT / "cmake" / "common"


class DependencyDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sei-dependencies-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.sdk = self.root / "sdk with spaces"

    def put(self, name, content=""):
        path = self.sdk / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path.as_posix()

    def configure(self, body, expected_error=None):
        source = self.root / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "project(Discovery NONE)\n"
            "set(CMAKE_FIND_FRAMEWORK NEVER)\n"
            + body
        )
        result = subprocess.run(
            ["cmake", "-S", str(source), "-B", str(self.root / "build")],
            text=True, capture_output=True,
        )
        output = result.stdout + result.stderr
        if expected_error:
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn(expected_error, output)
        else:
            self.assertEqual(result.returncode, 0, output)

    def module(self, name):
        return f'include("{(MODULES / name).as_posix()}")\n'

    def obs_package(self):
        return self.put(
            "lib/cmake/libobs/libobsConfig.cmake",
            'add_library(OBS::libobs INTERFACE IMPORTED)\n'
            'set(libobs_VERSION "fixture")\n',
        )

    def test_obs_sdk_package_precedes_manual_paths(self):
        self.obs_package()
        self.configure(
            f'set(OBS_SDK_PREFIX "{self.sdk.as_posix()}")\n'
            'set(OBS_LIBRARY "/invalid/manual/path")\n'
            'set(OBS_CONFIG_DIR "/invalid/helper/path")\n'
            + self.module("local-libobs.cmake")
            + 'get_target_property(kind OBS::libobs TYPE)\n'
            'if(NOT kind STREQUAL "INTERFACE_LIBRARY")\n'
            '  message(FATAL_ERROR "Package target replaced")\nendif()\n'
        )

    def test_obs_cmake_prefix_path(self):
        self.obs_package()
        self.configure(
            f'set(CMAKE_PREFIX_PATH "{self.sdk.as_posix()}")\n'
            + self.module("local-libobs.cmake")
        )

    def test_obs_package_without_target_is_rejected(self):
        package = self.put("libobsConfig.cmake", 'set(libobs_FOUND TRUE)\n')
        self.configure(
            f'set(libobs_DIR "{pathlib.Path(package).parent.as_posix()}")\n'
            + self.module("local-libobs.cmake"),
            "did not export OBS::libobs",
        )

    def manual_obs(self, windows=False, runtime=True):
        header = self.put("include/obs/obs-module.h")
        library = self.put("lib/obs.lib" if windows else "lib/libobs.so")
        suffix = ".lib" if windows else ".so"
        body = (
            'set(CMAKE_DISABLE_FIND_PACKAGE_libobs TRUE)\n'
            f'set(OBS_INCLUDE_DIR "{pathlib.Path(header).parent.as_posix()}")\n'
            f'set(OBS_LIB_DIR "{self.sdk.as_posix()}/lib")\n'
            'set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "")\n'
            f'set(CMAKE_FIND_LIBRARY_SUFFIXES "{suffix}")\n'
            f'set(WIN32 {"TRUE" if windows else "FALSE"})\n'
        )
        if windows and runtime:
            self.put("bin/64bit/obs.dll")
            body += f'set(OBS_RUNTIME_DIR "{self.sdk.as_posix()}/bin/64bit")\n'
        body += self.module("local-libobs.cmake")
        prop = "IMPORTED_IMPLIB" if windows else "IMPORTED_LOCATION"
        body += (
            f'get_target_property(location OBS::libobs {prop})\n'
            f'if(NOT location STREQUAL "{library}")\n'
            '  message(FATAL_ERROR "Wrong library selected")\nendif()\n'
        )
        if windows and runtime:
            body += (
                'get_target_property(runtime OBS::libobs IMPORTED_LOCATION)\n'
                f'if(NOT runtime STREQUAL "{self.sdk.as_posix()}/bin/64bit/obs.dll")\n'
                '  message(FATAL_ERROR "Wrong DLL selected")\nendif()\n'
            )
        return body

    def test_manual_linux_library_names(self):
        self.configure(self.manual_obs())

    def test_manual_windows_import_library_and_dll(self):
        self.configure(self.manual_obs(windows=True))

    def test_manual_windows_requires_runtime(self):
        self.configure(self.manual_obs(windows=True, runtime=False), "Manual Windows SDK requires")

    def ffmpeg_sdk(self):
        for component, header in (("avcodec", "avcodec.h"), ("avutil", "avutil.h")):
            self.put(f"include/lib{component}/{header}")
            self.put(f"lib/lib{component}.so")
        return (
            'set(CMAKE_FIND_LIBRARY_PREFIXES "lib")\n'
            'set(CMAKE_FIND_LIBRARY_SUFFIXES ".so")\n'
        )

    def test_explicit_ffmpeg_sdk_targets(self):
        self.configure(
            self.ffmpeg_sdk()
            + f'set(FFMPEG_SDK_PREFIX "{self.sdk.as_posix()}")\n'
            + self.module("ffmpeg-dependencies.cmake")
            + 'foreach(component IN ITEMS avcodec avutil)\n'
            '  get_target_property(location SEI::${component} IMPORTED_LOCATION)\n'
            f'  if(NOT location STREQUAL "{self.sdk.as_posix()}/lib/lib${{component}}.so")\n'
            '    message(FATAL_ERROR "Wrong FFmpeg SDK")\n  endif()\nendforeach()\n'
        )

    def test_ffmpeg_prefix_fallback_without_pkg_config(self):
        self.configure(
            self.ffmpeg_sdk()
            + 'set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)\n'
            + f'set(CMAKE_PREFIX_PATH "{self.sdk.as_posix()}")\n'
            + self.module("ffmpeg-dependencies.cmake")
        )

    def test_invalid_explicit_ffmpeg_sdk_does_not_fall_back(self):
        self.configure(
            f'set(FFMPEG_SDK_PREFIX "{self.sdk.as_posix()}/missing")\n'
            + self.module("ffmpeg-dependencies.cmake"),
            "FFmpeg avcodec development headers/library not found",
        )

    def test_dynamic_option_rejected_before_bootstrap(self):
        build_number = ROOT / "cmake" / ".CMakeBuildNumber"
        before = build_number.read_bytes()
        result = subprocess.run(
            ["cmake", "-S", str(ROOT), "-B", str(self.root / "build"),
             "-DSEI_TIMESTAMP_DYNAMIC_FFMPEG=ON"],
            text=True, capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not implemented and is disabled", result.stdout + result.stderr)
        self.assertEqual(build_number.read_bytes(), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
